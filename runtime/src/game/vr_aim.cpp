// VR first person: Link throws and guards where the player looks, not where the game camera points (BetterVR
// camera.cpp hook_OverrideThrowDirection / hook_OverrideGuardDirection, patch_CTRL_PlayerDirectionFixes.asm). The
// game camera follows the held heading (vr_turn.cpp) but not the head's own turns and tilt, so a throw thrown while
// looking left or up would go straight ahead.
// BetterVR jumps over the game's few instructions that build the direction; here the hook runs after the last of
// them and overwrites what they left in registers and on the stack, which comes to the same.
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "hooks.h"
#include "vr.h"

using mem::r32;
using mem::wf32;

bool vr_view_forward(float out[3]);          // vr_render.cpp
bool vr_hand_velocity_world(int hand, float out[3]);  // vr_render.cpp
bool vr_hand_world(int hand, float pos[3], float forward[3]);  // vr_body.cpp
bool vr_eye_world(float out[3]);             // vr_render.cpp: where the eyes are (the view's base and the head's move)
float vr_turn_last_heading();                // vr_turn.cpp
uint32_t vr_hand_item(int hand);             // vr_hands.cpp
uint32_t vr_hand_bound_age(int hand);        // vr_hands.cpp: stereo frames since the hand's item was last bound
const char* vr_actor_name(uint32_t actor);   // vr_hands.cpp

namespace {
uint32_t bits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

// The last throw, for the flight log (device 2026-11-04: bombs thrown straight ahead flew off to the right, once up
// and left; the direction handed to the game was the view's). The thrown actor is followed for a second after the
// throw: where it went against the direction thrown, and whether the right hand's own movement explains it.
constexpr uint32_t kActorMtx = 0x1f8;  // ActorWiiU::mtx, row-major
uint32_t g_thrown = 0;
char g_thrown_name[32];
float g_thrown_from[3], g_thrown_dir[3], g_thrown_hand_v[3];
float g_thrown_eye[3], g_thrown_look[3];  // the eyes and the view's direction at the throw
bool g_thrown_have_eye = false;
int g_thrown_frames = 0;

// Off by default (BOTW_VR_THROW_LIFT=1, below): the throw leaving from above and in front of the eyes, centred on
// the view, as in the flat game where Link lifts the object over his head (tried 2026-11-04). Thrown from the right hand (device 2026-11-04) it flew parallel to the
// view's line, 0.4-0.7 m to the side of it: the game sets the velocity in the hooked function (0x02c91894 ->
// 0x02951930 -> 0x032c3e90, which only queues it) and the object stays bound to the Weapon_R bone for a few frames
// more, leaving from wherever the hand is then. So from the throw until it leaves, the right arm reaches for the launch
// point instead of the controller (vr_body.cpp reach): the bone, and the object bound to it, are there when it goes,
// and it goes along the view. kLiftFrames at most, in case it is never seen to leave.
constexpr float kLaunchUp = 0.15f, kLaunchAhead = 0.35f;
constexpr int kLiftFrames = 20;
std::mutex g_lift_lock;
bool g_lifting = false;
float g_lift_look[3];  // the view's direction at the throw

// the launch point for the eyes and the view's direction: ahead along the view, up in the world
void launch_point(const float eye[3], const float look[3], float out[3]) {
    for (int i = 0; i < 3; i++) out[i] = eye[i] + look[i] * kLaunchAhead;
    out[1] += kLaunchUp;
}
void set_lifting(bool on, const float look[3] = nullptr) {
    std::lock_guard<std::mutex> l(g_lift_lock);
    g_lifting = on;
    if (look) memcpy(g_lift_look, look, sizeof g_lift_look);
}

void actor_pos(uint32_t actor, float out[3]) {
    for (int i = 0; i < 3; i++) out[i] = mem::rf32(actor + kActorMtx + 16 * i + 12);
}
}  // namespace

// vr_body.cpp, each skeleton pass: where the right hand's object should be while a throw lifts it (the eyes as they
// are now, the view's direction at the throw); false when no throw is lifting it
bool vr_throw_launch_point(float out[3]) {
    float look[3];
    {
        std::lock_guard<std::mutex> l(g_lift_lock);
        if (!g_lifting) return false;
        memcpy(look, g_lift_look, sizeof look);
    }
    float eye[3];
    if (!vr_eye_world(eye)) return false;
    launch_point(eye, look, out);
    return true;
}

// vr_render.cpp, once a game frame: the thrown actor's way, 6, 15, 30 and 60 frames after the throw
void vr_throw_track_frame() {
    // Where the right hand's bound object is against the hand the arm reaches for and the eyes (device 2026-11-04:
    // bombs left from 1.2-1.8 m right of the view's line, the hand 0.2 m right of Link): every 2 s while something is
    // held, the object's place from the hand (should be a few cm) and the eyes, and the eyes from Link's position
    {
        const uint32_t item = vr_hand_item(1), player = vr::player_actor();
        static int n, logged;
        if (item && player && vr_hand_bound_age(1) <= 1 && ++n % 90 == 0 && logged < 40) {
            float ip[3], hand[3], fwd[3], eye[3] = {0, 0, 0}, link[3];
            actor_pos(item, ip);
            actor_pos(player, link);
            const bool h = vr_hand_world(1, hand, fwd), e = vr_eye_world(eye);
            if (h) {
                logged++;
                LOG("vr: held %s at (%.2f %.2f %.2f) from the right hand, (%.2f %.2f %.2f) from the eyes%s; eyes (%.2f %.2f %.2f) from Link",
                    vr_actor_name(item) ? vr_actor_name(item) : "?", ip[0] - hand[0], ip[1] - hand[1], ip[2] - hand[2],
                    ip[0] - eye[0], ip[1] - eye[1], ip[2] - eye[2], e ? "" : " (no eyes)", eye[0] - link[0], eye[1] - link[1],
                    eye[2] - link[2]);
            }
        }
    }
    if (!g_thrown) {
        if (g_lifting) set_lifting(false);  // (read unlocked: only this thread and the hook, both the game's, set it)
        return;
    }
    const int n = ++g_thrown_frames;
    if (n > 60) {
        g_thrown = 0;
        return;
    }
    float p[3];
    actor_pos(g_thrown, p);
    // the lift: until the object is no longer bound to the right hand. Each frame: how far it is from the launch
    // point (should come to ~0 within a frame or two), and where it was when it left
    {
        bool lifting;
        {
            std::lock_guard<std::mutex> l(g_lift_lock);
            lifting = g_lifting;
        }
        if (lifting) {
            const uint32_t age = vr_hand_bound_age(1);
            const bool bound = vr_hand_item(1) == g_thrown && age <= 2;  // bound in the last frame or two
            float at[3] = {0, 0, 0};
            const bool have = vr_throw_launch_point(at);
            const float o[3] = {p[0] - at[0], p[1] - at[1], p[2] - at[2]};
            const float off = sqrtf(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
            static int logged;
            if (logged < 120)
                logged++, LOG("vr: throw lift frame %d: %s (bound %u frames ago), %.2f m from the launch point%s", n,
                              bound ? "still in the hand" : "left the hand", age, off, have ? "" : " (no eyes)");
            if (!bound || n >= kLiftFrames) {
                set_lifting(false);
                LOG("vr: thrown %s %s after %d frames, %.2f m from the launch point (%.2f %.2f %.2f)", g_thrown_name,
                    bound ? "still held, lift given up" : "left the hand", n, off, o[0], o[1], o[2]);
            }
        }
    }
    // frames 1-3: where it is from the right hand the arm reaches for (it should leave from there; device 2026-11-04:
    // 1 m off) and from Link, to find whose place the game lets it go from
    if (n <= 3) {
        float hand[3], fwd[3], link[3];
        actor_pos(vr::player_actor(), link);
        static int logged;
        if (vr_hand_world(1, hand, fwd) && logged < 60)
            logged++, LOG("vr: thrown %s: %d frames on, (%.2f %.2f %.2f) from the right hand, (%.2f %.2f %.2f) from Link, bound %u frames ago",
                          g_thrown_name, n, p[0] - hand[0], p[1] - hand[1], p[2] - hand[2], p[0] - link[0], p[1] - link[1],
                          p[2] - link[2], vr_hand_bound_age(1));
    }
    // where it is against the line the player looked along (from the eyes): what the player sees as "went right"
    if (g_thrown_have_eye && (n <= 3 || n == 6 || n == 15 || n == 30 || n == 60)) {
        const float* l = g_thrown_look;
        float e[3], lr[3] = {-l[2], 0, l[0]};
        for (int i = 0; i < 3; i++) e[i] = p[i] - g_thrown_eye[i];
        const float lrl = sqrtf(lr[0] * lr[0] + lr[2] * lr[2]);
        if (lrl > 1e-4f) lr[0] /= lrl, lr[2] /= lrl;
        const float lu[3] = {lr[1] * l[2] - lr[2] * l[1], lr[2] * l[0] - lr[0] * l[2], lr[0] * l[1] - lr[1] * l[0]};
        static int logged;
        if (logged < 140)
            logged++, LOG("vr: thrown %s: %d frames on, %.2f m along the look, %.2f m right of it, %.2f m above it", g_thrown_name,
                          n, e[0] * l[0] + e[1] * l[1] + e[2] * l[2], e[0] * lr[0] + e[2] * lr[2],
                          e[0] * lu[0] + e[1] * lu[1] + e[2] * lu[2]);
    }
    if (n != 6 && n != 15 && n != 30 && n != 60) return;
    float v[3];
    for (int i = 0; i < 3; i++) v[i] = p[i] - g_thrown_from[i];
    const float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!std::isfinite(len) || len > 500) {  // gone (a bomb blown up, the actor freed)
        LOG("vr: thrown %s: %d frames on, gone", g_thrown_name, n);
        g_thrown = 0;
        return;
    }
    // the way it went in the throw's frame: along the direction thrown, to its right (level) and above it
    const float* d = g_thrown_dir;
    float right[3] = {-d[2], 0, d[0]};
    const float rl = sqrtf(right[0] * right[0] + right[2] * right[2]);
    if (rl > 1e-4f) right[0] /= rl, right[2] /= rl;
    const float up[3] = {right[1] * d[2] - right[2] * d[1], right[2] * d[0] - right[0] * d[2], right[0] * d[1] - right[1] * d[0]};
    auto along = [&](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    const float fwd = along(v, d), side = along(v, right), rise = along(v, up);
    const float off = len > 0.01f ? atan2f(sqrtf(side * side + rise * rise), fwd) * 180 / (float)M_PI : 0;
    const float* h = g_thrown_hand_v;
    LOG("vr: thrown %s: %d frames on, %.2f m away, %.0f deg off the throw (%.2f ahead, %.2f right, %.2f up); the hand "
        "moved (%.1f ahead, %.1f right, %.1f up) m/s at the throw",
        g_thrown_name, n, len, off, fwd, side, rise, along(h, d), along(h, right), along(h, up));
}

// Throwing: the game takes the thrower's forward (z column of its matrix at +0x1568: x at 0x1570, y at 0x1580, z at
// 0x1590) and leaves f10 = x, f12 = z, f13 = y*y, f0 = x*x + y*y, with x / y stored at 0x14 / 0x18 (r1), for the
// normalisation that follows. fmadds f0, f10, f10, f13 is the last of it.
BOTW_HOOK(0x02c91740) {
    float d[3];
    if (c->r[31] != vr::player_actor() || !vr_view_forward(d)) return;  // the player's own throws only
    float look[3], eye[3] = {0, 0, 0}, from[3] = {0, 0, 0};
    memcpy(look, d, sizeof look);
    const uint32_t thrown = vr_hand_item(1);
    const bool have_eye = vr_eye_world(eye);
    float launch[3] = {0, 0, 0};
    // As BetterVR (camera.cpp hook_OverrideThrowDirection): only the direction is the view's, the object leaves from
    // the hand, a little to the side of the view's line (user 2026-11-04). BOTW_VR_THROW_LIFT=1: the lift above.
    static const bool lift = getenv("BOTW_VR_THROW_LIFT") != nullptr;
    const bool lifted = lift && have_eye && thrown;
    if (lifted) {
        // the object goes to the launch point (the right arm reaches for it until it leaves) and along the view from
        // there: the line it flies is the view's, kLaunchUp above it, near and far
        actor_pos(thrown, from);
        launch_point(eye, look, launch);
        set_lifting(true, look);
    }
    // The object does not leave from the eyes but from wherever it is (device 2026-11-04: the bomb's actor stood
    // 1.2-1.8 m to the right of the view's line at the throw and left from 2.0-2.4 m right of it, far from the hand
    // the arm reaches for, which was 0.2 m right of Link; thrown along the view it flew parallel to it, 2 m to the
    // side, "the bomb lands 2 m to the right"). BOTW_VR_THROW_AIM_M (metres, off by default) aims from where the object is to the
    // point of the view's line that far ahead: it crosses the line there, about where a thrown bomb lands,
    // and the side error shrinks as it flies instead of staying. Not when lifted (it leaves from the launch point on
    // the line).
    static const float aim_m = [] {
        const char* e = getenv("BOTW_VR_THROW_AIM_M");
        return e ? (float)atof(e) : 0.0f;  // off: the view's direction only, as BetterVR (user 2026-11-04)
    }();
    float look_d[3];
    memcpy(look_d, d, sizeof look_d);
    if (!lifted && have_eye && thrown && aim_m > 0) {
        float at[3], to[3];
        actor_pos(thrown, at);
        for (int i = 0; i < 3; i++) to[i] = eye[i] + look[i] * aim_m - at[i];
        const float l = sqrtf(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
        if (l > 1.0f && l < 100.0f) {
            for (int i = 0; i < 3; i++) d[i] = to[i] / l;
            static int logged;
            if (logged < 20)
                logged++, LOG("vr: throw aimed from the object (%.2f %.2f %.2f) of the eyes at the view's line %.0f m out: "
                              "(%.2f %.2f %.2f) for the view's (%.2f %.2f %.2f)", at[0] - eye[0], at[1] - eye[1], at[2] - eye[2],
                              aim_m, d[0], d[1], d[2], look_d[0], look_d[1], look_d[2]);
        }
    }
    c->f[10] = d[0], c->f[12] = d[2], c->f[13] = d[1] * d[1], c->f[0] = d[0] * d[0] + d[1] * d[1];
    wf32(c->r[1] + 0x14, d[0]);
    wf32(c->r[1] + 0x18, d[1]);
    // what else could have set the way: the held heading (the game camera's), where Link's actor faces (he turns to
    // the camera for a throw), the direction the game's own code had (the z column at +0x1568), the right hand's
    // place from Link and its movement (the held object leaves the hand a few frames on)
    static int logged;
    const uint32_t player = vr::player_actor(), item = vr_hand_item(1);
    const uint32_t thrower = c->r[31];
    float hand[3] = {0, 0, 0}, fwd[3], hv[3] = {0, 0, 0}, link[3] = {0, 0, 0};
    const bool tracked = vr_hand_world(1, hand, fwd), moving = vr_hand_velocity_world(1, hv);
    actor_pos(player, link);
    if (logged < 20) {
        const float a = vr_turn_last_heading();
        logged++;
        LOG("vr: throwing where the player looks (%.2f %.2f %.2f): held heading (%.2f 0 %.2f), Link faces "
            "(%.2f %.2f %.2f), the game's own way (%.2f %.2f %.2f)%s, right hand %s(%.2f %.2f %.2f) from Link "
            "moving %s(%.1f %.1f %.1f) m/s, holding %s",
            d[0], d[1], d[2], -sinf(a), -cosf(a), mem::rf32(player + kActorMtx + 8), mem::rf32(player + kActorMtx + 24),
            mem::rf32(player + kActorMtx + 40), mem::rf32(thrower + 0x1570), mem::rf32(thrower + 0x1580),
            mem::rf32(thrower + 0x1590), thrower == player ? "" : " (r31 is not the player actor)",
            tracked ? "" : "untracked ", hand[0] - link[0], hand[1] - link[1], hand[2] - link[2],
            moving ? "" : "unknown ", hv[0], hv[1], hv[2], vr_actor_name(item) ? vr_actor_name(item) : "nothing");
        if (lifted)
            LOG("vr: throw lifted from the hand at (%.2f %.2f %.2f) of the eyes to the launch point (%.2f %.2f %.2f) of "
                "them, along the view", from[0] - eye[0], from[1] - eye[1], from[2] - eye[2], launch[0] - eye[0],
                launch[1] - eye[1], launch[2] - eye[2]);
        else
            LOG("vr: throw not lifted (%s): it leaves from the hand", have_eye ? "nothing in the right hand" : "no eyes");
    }
    g_thrown = item;
    g_thrown_frames = 0;
    if (item) {
        const char* name = vr_actor_name(item);
        strncpy(g_thrown_name, name ? name : "?", sizeof g_thrown_name - 1);
        g_thrown_name[sizeof g_thrown_name - 1] = 0;
        if (lifted) memcpy(g_thrown_from, launch, sizeof g_thrown_from);
        else actor_pos(item, g_thrown_from);
        memcpy(g_thrown_dir, d, sizeof g_thrown_dir);
        memcpy(g_thrown_hand_v, hv, sizeof g_thrown_hand_v);
        memcpy(g_thrown_eye, eye, sizeof g_thrown_eye);
        memcpy(g_thrown_look, look, sizeof g_thrown_look);
        g_thrown_have_eye = have_eye;
    }
}

// Guarding with the shield: the game copies the guarding actor's forward (a matrix's z column) to 0xe0..0xe8 (r1),
// also as bits in r12 (x) and r0 (y) and as floats in f12 / f6 / f8. stfs f8, 0xe8(r1) is the last of it; the actor
// is at 0x24 (r1). The view's heading only, level.
BOTW_HOOK(0x02c16ec8) {
    float d[3];
    if (r32(c->r[1] + 0x24) != vr::player_actor() || !vr_view_forward(d)) return;
    const float l = sqrtf(d[0] * d[0] + d[2] * d[2]);
    if (l < 1e-4f) return;
    const float x = d[0] / l, z = d[2] / l;
    c->r[12] = bits(x), c->r[0] = bits(0.0f);
    c->f[12] = x, c->f[6] = 0.0, c->f[8] = z;
    wf32(c->r[1] + 0xe0, x), wf32(c->r[1] + 0xe4, 0.0f), wf32(c->r[1] + 0xe8, z);
    static bool logged;
    if (!logged) logged = true, LOG("vr: guarding where the player looks (%.2f 0 %.2f)", x, z);
}
