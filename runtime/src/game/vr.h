// VR (stereo) state shared by the VR game patches (vr_*.cpp). The approach and the guest addresses follow BetterVR
// (Crementif, MIT; ../BotW-BetterVR), which targets the same U-King.rpx v208; see docs/visionos-vr-feasibility.md.
// Off unless BOTW_VR=1: with VR off every patch runs the game's original code.
#pragma once
#include <atomic>
#include <cstdint>

// GX2CopyColorBufferToScanBuffer target bit marking an eye's 3D image (vr_render.cpp -> runtime/cemu/Cafe/HW/Latte/Core/LatteRenderTarget.cpp);
// the low bit is the eye. The game's own targets are 1 (TV) and 4 (GamePad).
#define VR_EYE_TARGET 0x100u
#define VR_HUD_TARGET 0x200u  // the HUD layer (vr_render.cpp), premultiplied alpha
#define VR_HUD_BEGIN 0x800u   // the HUD is drawn into memory of its own from here (vr_render.cpp hud_buffer)
#define VR_DEPTH_TARGET 0x400u  // an eye's depth buffer (its GX2DepthBuffer through the same copy); the low bit is the eye

namespace vr {
bool enabled();         // stereo rendering on (read once at start)
int eye();              // 0 left, 1 right: the pass of the current frame being drawn
uint64_t stereo_frames();  // frames rendered as a stereo pair (tests / log)
uint32_t player_actor();   // the player's actor (vr_actors.cpp sees it in its jobs), 0 until then
int calc_pass();           // the frame's calc running now: 1 left, 2 right, 0 none (draw, reset, outside the frame)
int job_eye();             // eye(), except inside a routed actor job0_1: the pass that pushed the job (vr_actors.cpp)
}  // namespace vr
