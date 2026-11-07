// VR roomscale: walking in the room walks Link (BetterVR hooking/entity_controller.cpp and the ApplyRoomscale loop in
// patch_CTRL_EntityController.asm; map in docs/bettervr-roomscale-map.md). Each frame, as the player's
// controller is handed the velocity it is to move with (0x0344cecc: the vector at sp+8 for 0x0344ac6c), the
// head's level movement in the room since the last frame is turned into the world by the view's heading and checked in
// steps of at most 6.25 cm (8 a frame): each step sweeps the player's own capsule (the game's shape cast, the same
// query objects as BetterVR builds on its stack), the first hit stops (a wall). What is free is walked as velocity:
// added to the vector the controller is handed (m/s; the stick's walking, about 5.5 when running), as if the stick
// walked that way too, and taken back off its stored velocity (+0x1E4) after setLinearVelocity (0x0344dd04), so the
// game's physics moves the body over the frame. Added to +0x1E4 alone it did nothing (Mac, 2026-11-02: standing, the
// controller holds the body still whatever that velocity says). BetterVR, and
// this file until 2026-11-02, set the body there instead (setTransform), which broke off jumps and falls: on the
// headset Link fell at about 40 % speed and once stayed in the air. The distance the body has walked
// ("applied", in the room) is taken off the head's level offset where the eyes and the controllers are placed
// (vr_render.cpp), so the view stays where the head is; what a wall holds back stays as an offset of the head from the
// body, and moving back towards the body only takes that up again.
// The ground is the game's: slopes and steps are its physics. Off outside first person and during events, while
// climbing, swimming, gliding, riding or ragdolling (the head then stays level over the body), and on a recentre.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "../../../ios/src/vr_bridge.h"
#include "hooks.h"
#include "vr.h"

using mem::r32;
using mem::rf32;
using mem::w32;
using mem::wf32;

extern "C" int botw_vr_view_now(void);  // vr_render.cpp
bool vr_first_person();                 // vr_render.cpp
float vr_turn_last_heading();           // vr_turn.cpp
bool vr_head_in_room(float out[3], uint32_t* recentres);  // vr_render.cpp: the head in the recentred frame
float fps_delta();                                         // fps.cpp: the game frame in 1/30 s units

namespace {
// guest functions (docs/bettervr-roomscale-map.md section 2)
constexpr uint32_t kCtrlGetPosition = 0x0344c0b8, kSphereCtor = 0x034d1b60, kSphereDtor = 0x034d1dd0,
                   kCopyLayers = 0x034d09d4, kSetStartAndEnd = 0x034d0170, kCapsuleCtor = 0x034d1614,
                   kCapsuleDtor = 0x034d16a0, kNullPosition = 0x10549dd0, kEmptyString = 0x10549fec;
// the sweep starts kCenterY above the body's position (its feet: the shape swept is the body's own capsule, so a
// higher start lifts it over low walls such as railings)
constexpr float kStep = 0.0625f, kCenterY = 0.15f, kSweepRadius = 0.35f, kLookAhead = 0.35f;
constexpr int kMaxSteps = 8;
constexpr float kMaxSpeed = 4.0f;  // m/s the room may add (a brisk walk is 1.5)
constexpr uint32_t kOwnPose = (1u << 7) | (1u << 10) | (1u << 15) | (1u << 19) | (1u << 20);  // PlayerMoveBitFlags

// game thread (the right pass's controller update) writes, the render threads read the applied offset
std::mutex g_lock;
float g_applied[2];  // x, z in the recentred room frame
bool g_primed = false;
// the velocity the room added this frame (taken back off +0x1E4 after setLinearVelocity); game thread only
uint32_t g_added_ctrl = 0;
float g_added[2];
float g_prev[2];
uint32_t g_recentres = ~0u;

void prime(const float head[3]) {
    std::lock_guard<std::mutex> l(g_lock);
    g_prev[0] = g_applied[0] = head[0], g_prev[1] = g_applied[1] = head[2];
    g_primed = true;
}

// Guest scratch below the stack: one sweep from 'from' to 'to'; true when the player's capsule hits something
struct Scratch {
    uint32_t base;
    uint32_t at(uint32_t off) const { return base + off; }
};
constexpr uint32_t kArg9 = 0x08, kOwnLayer = 0x20, kGroundHit = 0x24, kFrom = 0x30, kTo = 0x40, kPos = 0x50,
                   kSphere = 0x100, kCapsule = 0x160, kScratchSize = 0x200;

bool sweep(PPCContext* c, const Scratch& s, uint32_t ctrl, uint32_t body, const float from[3], const float to[3]) {
    for (int i = 0; i < 3; i++) wf32(s.at(kFrom) + 4 * i, from[i]), wf32(s.at(kTo) + 4 * i, to[i]);
    w32(s.at(kOwnLayer), 3), w32(s.at(kGroundHit), 15), w32(s.at(kArg9), 0);
    c->f[1] = kSweepRadius;
    guest_call(c, kSphereCtor, {s.at(kSphere), s.at(kOwnLayer), s.at(kGroundHit), 0, 0x80, 0, kNullPosition, kEmptyString});
    guest_call(c, kCopyLayers, {s.at(kSphere), ctrl + 0x208});
    guest_call(c, kSetStartAndEnd, {s.at(kSphere), s.at(kFrom), s.at(kTo)});
    bool hit = false;
    if (const uint32_t data = r32(s.at(kSphere) + 4)) {
        guest_call(c, kCapsuleCtor, {s.at(kCapsule), body, data, 0});
        guest_call(c, kCopyLayers, {s.at(kCapsule), ctrl + 0x208});
        guest_call(c, kSetStartAndEnd, {s.at(kCapsule), s.at(kFrom), s.at(kTo)});
        const uint32_t cast = r32(r32(s.at(kCapsule) + 0x3c) + 0x1c);  // ShapeCastWithInfo's cast (0x034d171c)
        hit = guest_call(c, cast, {s.at(kCapsule), 1}) != 0;
        guest_call(c, kCapsuleDtor, {s.at(kCapsule), 2});
    }
    guest_call(c, kSphereDtor, {s.at(kSphere), 2});
    return hit;
}

void roomscale(PPCContext* c) {
    const uint32_t player = vr::player_actor(), ctrl = c->r[31];
    if (!player || !r32(player + 0x3a0) || ctrl != r32(r32(player + 0x3a0) + 0x50)) return;  // the player's only
    // BOTW_VR_LOG_Y=1: the player's position every frame (tests: how a jump falls, how far the room walks him)
    static const bool log_y = getenv("BOTW_VR_LOG_Y") != nullptr;
    if (log_y)
        LOG("vr: player y %.3f (x %.3f z %.3f)", rf32(player + 0x1f8 + 0x1c), rf32(player + 0x1f8 + 0x0c),
            rf32(player + 0x1f8 + 0x2c));
    // off unless BOTW_VR_ROOMSCALE=1 (2026-11-02): on the headset jumps rose and fell at about 40 % speed and once
    // Link stayed in the air, and this is the one VR patch that moves his physics body; off until that is settled
    static const bool off = [] {
        const char* e = getenv("BOTW_VR_ROOMSCALE");
        return !(e && !strcmp(e, "1"));
    }();
    if (off) return;
    float head[3];
    uint32_t recentres;
    // BOTW_VR_FAKE_WALK=<metres> (Mac tests, no headset): the head walks that far ahead and back every 8 s
    static const float fake_walk = [] {
        const char* e = getenv("BOTW_VR_FAKE_WALK");
        return e ? (float)atof(e) : 0.0f;
    }();
    if (fake_walk > 0) {
        static const auto t0 = std::chrono::steady_clock::now();
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        head[0] = 0, head[1] = 1.6f, head[2] = -fake_walk * 0.5f * (1 - (float)cos(t * M_PI / 4)), recentres = 0;
    } else if (!vr_head_in_room(head, &recentres)) {
        return;
    }
    const bool drive = (fake_walk > 0 || (botw_vr_view_now() && vr_first_person())) && !(r32(player + 0x8dc) & kOwnPose);
    if (!drive || !g_primed || recentres != g_recentres) {
        g_recentres = recentres;
        prime(head);
        return;
    }
    // in the air too: only the level velocity is added, the game keeps its own rise and fall
    float d[2] = {head[0] - g_prev[0], head[2] - g_prev[1]};
    const float len2 = d[0] * d[0] + d[1] * d[1];
    // under 5 mm the head's drift and tracking noise build up instead of nudging the body every frame
    if (len2 < 0.005f * 0.005f) return;
    g_prev[0] = head[0], g_prev[1] = head[2];
    if (len2 > 0.25f) {  // more than 0.5 m in a frame: a jump of the tracking, not a step
        prime(head);
        return;
    }
    // moving back towards the body only takes up what a wall held back (the residual), it does not move the body
    float applied[2];
    {
        std::lock_guard<std::mutex> l(g_lock);
        applied[0] = g_applied[0], applied[1] = g_applied[1];
    }
    const float r[2] = {head[0] - applied[0] - d[0], head[2] - applied[1] - d[1]};  // the residual before this move
    const float rl = sqrtf(r[0] * r[0] + r[1] * r[1]);
    if (rl > 1e-3f) {
        const float back = -(d[0] * r[0] + d[1] * r[1]) / rl;  // how far this move goes back along the residual
        if (back > 0) {
            const float take = std::fmin(back, rl);
            d[0] += r[0] / rl * take, d[1] += r[1] / rl * take;
        }
    }
    if (d[0] * d[0] + d[1] * d[1] < 1e-6f) return;
    // no faster than kMaxSpeed over the game frame: the rest stays as an offset of the head, taken up as it walks on
    const float dt = fps_delta() / 30.0f;
    if (const float dl = sqrtf(d[0] * d[0] + d[1] * d[1]); dl > kMaxSpeed * dt) {
        d[0] *= kMaxSpeed * dt / dl, d[1] *= kMaxSpeed * dt / dl;
    }
    // room -> world: the view's heading (a turn about +y, as the eyes are placed)
    const float a = vr_turn_last_heading(), ca = cosf(a), sa = sinf(a);
    const float w[2] = {ca * d[0] + sa * d[1], -sa * d[0] + ca * d[1]};
    const float wl = sqrtf(w[0] * w[0] + w[1] * w[1]);
    const int steps = std::min(kMaxSteps, std::max(1, (int)ceilf(wl / kStep)));
    const uint32_t body = (r32(ctrl + 0xe8) & 0x10000) ? r32(ctrl + 0x230) : r32(ctrl + 4);
    if (!body) return;
    // the guest scratch, below the stack (restored before returning); guest_call leaves r3, r4, f1 changed
    const uint32_t sp = c->r[1];
    const Scratch s{(sp - kScratchSize) & ~0xfu};
    c->r[1] = s.base;
    guest_call(c, kCtrlGetPosition, {ctrl, s.at(kPos)});
    float pos[3] = {rf32(s.at(kPos)), rf32(s.at(kPos) + 4), rf32(s.at(kPos) + 8)};
    const float start[3] = {pos[0], pos[1], pos[2]};
    int done = 0;
    for (; done < steps; done++) {
        // the step plus some look-ahead: the body stops short of a wall instead of being set into it (a sweep that
        // starts inside the geometry no longer reports it, and the game's physics would push the body back out)
        const float f[3] = {pos[0], pos[1] + kCenterY, pos[2]};
        const float ahead = 1 + kLookAhead / std::fmax(wl / steps, 1e-4f);
        const float t[3] = {f[0] + w[0] / steps * ahead, f[1], f[2] + w[1] / steps * ahead};
        if (sweep(c, s, ctrl, body, f, t)) break;  // a wall
        pos[0] += w[0] / steps, pos[2] += w[1] / steps;
    }
    if (done > 0) {
        // the free part as velocity over the game frame, on top of the stick's (sp+8: x, y, z)
        g_added[0] = (pos[0] - start[0]) / dt, g_added[1] = (pos[2] - start[2]) / dt;
        g_added_ctrl = ctrl;
        wf32(sp + 8, rf32(sp + 8) + g_added[0]);
        wf32(sp + 16, rf32(sp + 16) + g_added[1]);
        std::lock_guard<std::mutex> l(g_lock);
        g_applied[0] += d[0] * done / steps, g_applied[1] += d[1] * done / steps;
    }
    c->r[1] = sp;
    static int blocked_logs;
    if (done < steps && blocked_logs < 5) blocked_logs++, LOG("vr: roomscale step blocked (a wall) at (%.2f %.2f)", pos[0], pos[2]);
    static int logged;
    if (logged < 3 && done > 0)
        logged++, LOG("vr: roomscale walks the body %.3f m (%d of %d steps%s), from (%.2f %.2f) to (%.2f %.2f)",
                      wl * done / steps, done, steps, done < steps ? ", a wall" : "", start[0], start[2], pos[0], pos[2]);
}
}  // namespace

// vr_render.cpp: the distance the body has walked for the head (x, z in the recentred frame), taken off the head
void vr_roomscale_applied(float out[2]) {
    std::lock_guard<std::mutex> l(g_lock);
    out[0] = g_primed ? g_applied[0] : 0, out[1] = g_primed ? g_applied[1] : 0;
}

// stfs f12, 16(r1): the last of the velocity (sp+8) for 0x0344ac6c right after (r3 the controller, r4 sp+8)
BOTW_SITE(0x0344dcf0) {
    mem::wf32(c->r[1] + 16, (float)c->f[12]);
    if (vr::enabled()) roomscale(c);
    c->r[3] = c->r[31], c->r[4] = c->r[1] + 8;
}

// mr r3, r31 after setLinearVelocity: the room's part taken back off the controller's stored velocity, for whatever
// reads it after (the animation's speed, its next frame)
BOTW_SITE(0x0344dd04) {
    if (g_added_ctrl && g_added_ctrl == c->r[31]) {
        wf32(g_added_ctrl + 0x1e4, rf32(g_added_ctrl + 0x1e4) - g_added[0]);
        wf32(g_added_ctrl + 0x1e4 + 8, rf32(g_added_ctrl + 0x1e4 + 8) - g_added[1]);
        g_added_ctrl = 0;
    }
    c->r[3] = c->r[31];
}
