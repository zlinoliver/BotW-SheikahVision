// VR first person: ladders (BetterVR hooking/camera.cpp hook_FixLadder / hook_StoreLadderState,
// patch_CTRL_CameraControls.asm "workaround for ladder climbing issue"; plan by Fable 2026-11-06).
// The ladder action (its update at 0x02d07ce8, in the player's job, the right calc pass) asks f_02d69d4c how the
// player's intended move compares with the ladder: 0 towards it (climb), 1 away (slide down), 2 / 3 neither. The move
// is the stick turned by the game camera, which in VR is not the view the player sees, so the class came out 2 / 3 and
// Link hung on the ladder (device 2026-11-05 / 06; walls climb by another path, the stick projected onto the wall).
// Here, while the ladder update ran in the last two frames: the stick up is 0 (climb), down 1 (slide), in the middle
// BetterVR's 4 (nothing; A jumps up). BetterVR itself only forces 4 / 1 (its README: jump up the ladder with A); 0 for
// the stick up is ours and logged so it can be taken back.
#include <atomic>

#include "../input/input.h"
#include "hooks.h"
#include "vr.h"

extern "C" int botw_vr_view_now(void);  // vr_render.cpp
bool vr_first_person();                 // vr_render.cpp
float vr_head_pitch_deg();              // vr_render.cpp

namespace {
std::atomic<uint64_t> g_ladder_frame{~0ull};
bool ladder_vr() { return vr::enabled() && botw_vr_view_now() && vr_first_person(); }
}  // namespace

// vr_gesture.cpp: Link is on a ladder (the ladder update ran in the last two frames)
bool vr_on_ladder() {
    if (!ladder_vr()) return false;
    const uint64_t marked = g_ladder_frame.load(std::memory_order_relaxed);
    return marked != ~0ull && vr::stereo_frames() - marked <= 2;
}

// the ladder action's update (its entry, mflr r0): Link is on a ladder this frame
BOTW_HOOK(0x02d07ce8) {
    if (!ladder_vr()) return;
    const uint64_t now = vr::stereo_frames(), was = g_ladder_frame.exchange(now, std::memory_order_relaxed);
    if (was == ~0ull || now - was > 30) LOG("vr: ladder: on a ladder (frame %llu, pass %d)", (unsigned long long)now, vr::calc_pass());
}

// the move-against-ladder class
BOTW_REPLACE(0x02d69d4c) {
    const uint32_t site = (uint32_t)c->lr;
    orig_0x02d69d4c(c);
    if (!ladder_vr()) return;
    const uint64_t marked = g_ladder_frame.load(std::memory_order_relaxed), now = vr::stereo_frames();
    if (marked == ~0ull || now - marked > 2) return;  // BetterVR's two-frame counter
    // logged only now (2026-11-06): the game's own class was already 0 (towards the ladder) with the stick up, and
    // forcing it changed nothing but the other callers (0x02d14a84 in the wall climbing among them)
    const input::PadState p = input::gamepad();
    static uint32_t last_class = ~0u;
    static int budget = 40;
    if (site == 0x02d07e28 && c->r[3] != last_class && budget > 0)
        budget--, LOG("vr: ladder: class %u (stick %.2f %.2f, head pitch %.0f, frame %llu, pass %d)", (uint32_t)c->r[3],
                      p.lx, p.ly, vr_head_pitch_deg(), (unsigned long long)now, vr::calc_pass());
    // at the ladder update itself: the stick up is towards the ladder (0, climb), down away (1, slide down); the game's
    // own class needs the stick within 45 degrees of a direction worked out from its camera, which in VR is not where
    // the player looks (the log: the stick up came out 2, "neither", and Link only climbed by jumping)
    if (site == 0x02d07e28 && (p.ly > 0.3f || p.ly < -0.3f)) {
        const uint32_t want = p.ly > 0 ? 0 : 1;
        if (c->r[3] != want && budget > 0)
            budget--, LOG("vr: ladder: class %u -> %u (stick %.2f %.2f)", (uint32_t)c->r[3], want, p.lx, p.ly);
        c->r[3] = want;
    }
    if (site == 0x02d07e28) last_class = c->r[3];
}
