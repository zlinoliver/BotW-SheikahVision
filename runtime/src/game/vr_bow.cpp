// VR motion controls, the bow: the arrow leaves the bow hand toward where the player aims with the controllers
// instead of toward the game camera's centre (BetterVR hooking/bow.cpp; map of its hooks in docs/bettervr-bow-map.md).
// Drawing and loosing stay the game's: the right trigger is ZR, held to draw, released to shoot. When the shot is
// built (AI__Arrow::handleStateShoot, 0x02705644) the arrow's starting transform and its child action's parameters
// are changed just before they are handed on:
//  - setTransform (bl at 0x02706270): the arrow placed at the launch point, pointing along the aim;
//  - changeChild (homing 0x0270640c, sky shot 0x027064d8, normal 0x02706518): in the InlineParamPack at r1+0x158,
//    FirstSpeed turned onto the aim (its length kept) and TargetPos / HomingTargetPos put 100 m along it.
// Only arrows the pack marks IsShootByPlayer, in first person with the left controller tracked. The aim is where the
// bow in the left hand points (its -x, as BetterVR), wherever the right hand is (user 2026-11-03: no judging how far
// the drawing hand is from the string; the left hand alone steers the line). The launch point is the bow's attackPos when the game has one (+0x770, valid flag
// +0x7a0), else the left grip. BetterVR takes the direction from the bow model's -x axis; here it comes from the
// controllers themselves, which does not depend on how the bow model sits in the hand.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <cstring>

#include "hooks.h"
#include "vr.h"

using mem::r32;
using mem::rf32;
using mem::wf32;

extern "C" int botw_vr_view_now(void);                   // vr_render.cpp
bool vr_first_person();                                  // vr_render.cpp
bool vr_hand_world(int hand, float pos[3], float fwd[3]);  // vr_body.cpp
bool vr_bow_dir_world(float out[3]);                      // vr_body.cpp
uint32_t vr_player_bow();                                // vr_weapon.cpp
const char* vr_actor_name(uint32_t actor);              // vr_hands.cpp
uint32_t vr_hand_item(int hand);                         // vr_hands.cpp

namespace {
constexpr uint32_t kPackOffset = 0x158, kPackCount = 0x680, kEntrySize = 0x34, kEntryKey = 0x30, kEntryVec = 0x10;
constexpr float kTargetDistance = 100.0f;

// the pack's entry for a key, 0 when absent
uint32_t entry(uint32_t pack, const char* key) {
    const uint32_t n = r32(pack + kPackCount);
    for (uint32_t i = 0; i < n && i < 32; i++) {
        const uint32_t e = pack + i * kEntrySize;
        const char* k = mem::str(r32(e + kEntryKey));
        if (k && strcmp(k, key) == 0) return e;
    }
    return 0;
}
bool shot_by_player(uint32_t pack) {
    const uint32_t e = entry(pack, "IsShootByPlayer");
    return e && mem::r8(e) != 0;
}

// The launch point and aim (unit) in the world
bool launch(float origin[3], float dir[3]) {
    if (!botw_vr_view_now() || !vr_first_person()) return false;
    float left[3], d[3];
    if (!vr_hand_world(0, left, d)) return false;
    // where the bow in the left hand shoots, its -x (BetterVR TryGetBowEntityWorldForward): the way the nocked arrow
    // points. The controller's aim space pointed up for a hand holding it as a bow (device 2026-11-04)
    if (const uint32_t held = vr_hand_item(0)) {
        const uint32_t m = held + 0x1f8;
        const float b[3] = {-rf32(m), -rf32(m + 16), -rf32(m + 32)};
        if (b[0] * b[0] + b[1] * b[1] + b[2] * b[2] > 0.25f) d[0] = b[0], d[1] = b[1], d[2] = b[2];
    }
    // riding: the bow model follows the riding animation, so the direction the line shows (vr_body.cpp, from the
    // controllers) is the one shot (device 2026-11-15: every arrow from the saddle went straight ahead)
    if (const uint32_t player = vr::player_actor(); player && (r32(player + 0x8dc) & (1u << 20))) {
        float w[3];
        if (vr_bow_dir_world(w)) d[0] = w[0], d[1] = w[1], d[2] = w[2];
    }
    const float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(len > 1e-4f)) return false;
    for (int i = 0; i < 3; i++) dir[i] = d[i] / len;
    const uint32_t bow = vr_player_bow();
    const bool at_bow = bow && mem::r8(bow + 0x7a0) != 0;
    for (int i = 0; i < 3; i++) origin[i] = at_bow ? rf32(bow + 0x770 + 4 * i) : left[i];
    return true;
}

thread_local bool t_have;  // the shot being built has a launch (between setTransform and changeChild)
thread_local float t_origin[3], t_dir[3];

// FirstSpeed onto the aim, the targets along it. Every shot is logged (device 2026-11-03: elemental arrows did not land
// where the arc said; which arrow, which of the three paths, aimed or not)
const char* arrow_name() {
    const char* n = vr_actor_name(vr_hand_item(1));
    return n ? n : "?";
}
void retarget(PPCContext* c, const char* path) {
    const uint32_t pack = c->r[1] + kPackOffset;
    static int logged;
    if (!t_have || !shot_by_player(pack)) {
        if (logged < 60 && shot_by_player(pack))
            logged++, LOG("vr: shot %s (%s path): not aimed (no controller launch)", arrow_name(), path);
        return;
    }
    t_have = false;
    if (const uint32_t e = entry(pack, "FirstSpeed")) {
        const uint32_t v = e + kEntryVec;
        const float x = rf32(v), y = rf32(v + 4), z = rf32(v + 8), speed = sqrtf(x * x + y * y + z * z);
        if (speed > 0.01f)
            for (int i = 0; i < 3; i++) wf32(v + 4 * i, t_dir[i] * speed);
    }
    for (const char* key : {"TargetPos", "HomingTargetPos"})
        if (const uint32_t e = entry(pack, key))
            for (int i = 0; i < 3; i++) wf32(e + kEntryVec + 4 * i, t_origin[i] + t_dir[i] * kTargetDistance);
    if (logged < 60)
        logged++, LOG("vr: shot %s (%s path) aimed with the controllers (%.2f %.2f %.2f)", arrow_name(), path, t_dir[0],
                      t_dir[1], t_dir[2]);
}
}  // namespace

// The arrow's flight for the aiming arc the headset draws (BotWImmersive.swift, after BetterVR bow.cpp
// TryReadLiveBowState / BuildPreviewTrajectory): the drawn bow's own parameters, read on the game thread each frame
// while it is drawn (vr_body.cpp). The bow's actor parameters (+0x39C) -> GParamList (+0x78), its objects (+0x244,
// count +0x240): the bow's (slot 0x44 / 4) FirstSpeed +0x104, Acceleration +0x114, StabilitySpeed +0x124,
// FallAcceleration +0x134, FallStabilitySpeed +0x144, Gravity +0x154; the attack's (slot 0x20 / 4) AtRange +0x5C, times
// the weapon's original z scale (+0x998). How far it is drawn: +0x94C (0 to 1; the speed goes with its square). Speeds
// are metres a 30 fps frame, the gravity's per frame squared once divided by 900, and each frame is scaled by the
// game's frame delta (fps.cpp)
float fps_delta();  // fps.cpp
namespace {
std::mutex g_flight_lock;
float g_flight[8];
bool g_flight_valid = false;
}  // namespace

void vr_bow_flight_update(uint32_t bow) {
    float f[8] = {};
    bool ok = false;
    if (bow) {
        const uint32_t list = r32(r32(bow + 0x39c) + 0x78);
        const uint32_t objects = list ? r32(list + 0x244) : 0;
        const uint32_t n = list ? r32(list + 0x240) : 0;
        const uint32_t param = objects && n > 0x44 / 4 ? r32(objects + 0x44) : 0;
        const uint32_t attack = objects && n > 0x20 / 4 ? r32(objects + 0x20) : 0;
        if (param) {
            const float draw = std::clamp(rf32(bow + 0x94c), 0.0f, 1.5f);
            f[0] = std::max(rf32(param + 0x104) * draw * draw, 0.0f);  // launch speed
            f[1] = rf32(param + 0x114), f[2] = rf32(param + 0x124);       // acceleration, steady speed
            f[3] = fabsf(rf32(param + 0x134)), f[4] = rf32(param + 0x144);  // falling: acceleration, steady speed
            f[5] = rf32(param + 0x154) / 900.0f;                           // gravity
            f[6] = attack ? rf32(attack + 0x5c) * rf32(bow + 0x998) : 0;    // range before it falls
            f[7] = fps_delta();
            ok = true;
            for (float v : f) ok = ok && std::isfinite(v);
        }
    }
    static int logged;
    if (ok && logged < 3 && f[0] > 0.5f)
        logged++, LOG("vr: bow flight: speed %.2f accel %.3f steady %.2f, fall accel %.3f steady %.2f, gravity %.4f, "
                      "range %.1f, delta %.2f", f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
    std::lock_guard<std::mutex> l(g_flight_lock);
    if (ok) memcpy(g_flight, f, sizeof f);
    g_flight_valid = ok;
}

// BotWImmersive.swift: the drawn bow's flight (see above), 0 when there is none
extern "C" int botw_vr_bow_flight(float out[8]) {
    std::lock_guard<std::mutex> l(g_flight_lock);
    if (!g_flight_valid) return 0;
    memcpy(out, g_flight, sizeof g_flight);
    return 1;
}

// Before setTransform(rigid body, r1+0xe0, ...): the arrow's 3x4 row-major matrix, position in the last column,
// pointing along -z (BetterVR SetTransformFromLaunchPose: z = -aim, x = up x z with the old y as the up hint)
BOTW_HOOK(0x0270626c) {  // addi r5, r0, 1
    if (t_have) {  // the last aimed shot's launch never reached one of the three paths above
        static int logged;
        if (logged < 30) logged++, LOG("vr: shot %s took another path: its direction was not changed", arrow_name());
    }
    t_have = false;
    const uint32_t pack = c->r[1] + kPackOffset, m = c->r[1] + 0xe0;
    if (!shot_by_player(pack)) {
        static int logged;
        if (logged < 20) logged++, LOG("vr: shot %s not marked as the player's", arrow_name());
        return;
    }
    if (!launch(t_origin, t_dir)) return;
    t_have = true;
    const float z[3] = {-t_dir[0], -t_dir[1], -t_dir[2]};
    float up[3] = {rf32(m + 4), rf32(m + 20), rf32(m + 36)};  // the old y column
    auto cross = [](const float a[3], const float b[3], float o[3]) {
        o[0] = a[1] * b[2] - a[2] * b[1], o[1] = a[2] * b[0] - a[0] * b[2], o[2] = a[0] * b[1] - a[1] * b[0];
    };
    auto unit = [](float v[3]) {
        const float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (l < 1e-6f) return false;
        for (int i = 0; i < 3; i++) v[i] /= l;
        return true;
    };
    float x[3], y[3];
    if (!unit(up) || fabsf(up[0] * z[0] + up[1] * z[1] + up[2] * z[2]) > 0.999f)
        up[0] = 0, up[1] = fabsf(z[1]) < 0.999f ? 1.0f : 0.0f, up[2] = fabsf(z[1]) < 0.999f ? 0.0f : 1.0f;
    cross(up, z, x);
    unit(x);
    cross(z, x, y);
    for (int r = 0; r < 3; r++) {
        wf32(m + 16 * r + 0, x[r]);
        wf32(m + 16 * r + 4, y[r]);
        wf32(m + 16 * r + 8, z[r]);
        wf32(m + 16 * r + 12, t_origin[r]);
    }
}
BOTW_HOOK(0x02706408) { retarget(c, "homing"); }  // homing:   addi r4, r4, ... before bl changeChild
BOTW_HOOK(0x027064d4) { retarget(c, "sky"); }     // sky shot
BOTW_HOOK(0x02706514) { retarget(c, "normal"); }  // normal
