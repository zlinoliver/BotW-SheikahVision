// VR first person: the stamina wheel. The game draws it next to Link, at his position projected onto the screen; on
// the HUD panel in front of the player (vr_render.cpp) that puts it somewhere in the middle of the view, unrelated to
// where Link is. As in BetterVR (camera.cpp hook_FixStaminaGaugeScreenPosition, patch_CTRL_FixScreenChecks.asm) it is
// placed under the hearts instead, and the extra stamina icons stay there rather than hanging onto the wheel.
#include "hooks.h"
#include "vr.h"

using mem::wf32;

extern "C" int botw_vr_view_now(void);  // vr_render.cpp
bool vr_first_person();                 // vr_render.cpp

namespace {
thread_local uint32_t t_screen_pos, t_world_offset;  // the call's out pointers (r4, r5)
bool fixed() { return botw_vr_view_now() && vr_first_person(); }
}  // namespace

// li r6, 1 right before the call that projects Link's position for the wheel (bl 0x02fb2468 at 0x02fb2608)
BOTW_HOOK(0x02fb2604) {
    t_screen_pos = c->r[4], t_world_offset = c->r[5];
}
// addi r3, r31, 0x22e8 right after it: the wheel under the hearts, no offset from the player
BOTW_HOOK(0x02fb260c) {
    if (!fixed() || !t_screen_pos || !t_world_offset) return;
    wf32(t_screen_pos, -482.0f), wf32(t_screen_pos + 4, 170.0f);
    wf32(t_world_offset, 0), wf32(t_world_offset + 4, 0), wf32(t_world_offset + 8, 0);
}
// rlwinm. r28, r0, 27, 5, 31: the beq after it skips placing the extra stamina icons next to the wheel; taken here
BOTW_HOOK(0x02fb275c) {
    if (fixed()) c->cr[2] = 1;  // cr0 eq
}
