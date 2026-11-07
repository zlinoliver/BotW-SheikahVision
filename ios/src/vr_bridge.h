// SheikahVision VR on Apple Vision Pro: what the immersive compositor (ios/src/BotWImmersive.swift) reads from the game's
// renderer (runtime/cemu_shims/latte_glue.mm). Plain C, imported into Swift.
#pragma once
#include <stdint.h>

typedef struct {
    void* eye[2];       // id<MTLTexture>, BGRA8Unorm holding sRGB-encoded colours: the left and right eye pictures
    void* hud;          // id<MTLTexture> or NULL: the HUD layer (premultiplied alpha), drawn over both eyes
    int stereo;         // 0: a frame without eye images (menus, loading): eye[0] == eye[1] = the game's picture
    uint32_t pose_tag;  // view images: 0x100 | the low 8 bits of the BotWVRPose id they were rendered with; 0 for
                        // screen images (screen mode, and events in view mode)
    uint64_t serial;    // the frame set's number; the set is complete once the shared event reaches it
    void* depth[2];     // id<MTLTexture> or NULL, R32Float: each eye's depth buffer as the game wrote it (0..1)
    float depth_range[4];  // the eyes' projection: near, far (m), the depth scale and offset of its device matrix
} BotWVRFrame;

// The headset's pose for the game's cameras (view mode, BOTW_VR_MODE=view), from the compositor each headset frame.
// Matrices are column-major (simd_float4x4), ARKit's world space in metres: right-handed, +y up, -z forward.
typedef struct {
    float origin_from_device[16];   // the head, predicted for the compositor frame's presentation
    float device_from_view[2][16];  // each eye in the head's frame
    float tangents[2][4];           // each eye's frustum: tan of the left, right, top, bottom edge (left / bottom < 0)
    uint32_t id;                    // increases with every pose
    // the PS VR2 Sense controllers (0 left, 1 right hand), predicted for the same time: each one's grip (ARKit's
    // Accessory.LocationName.grip), its velocity and angular velocity (m/s, rad/s, origin space)
    float origin_from_grip[2][16];
    float velocity[2][3];
    float angular_velocity[2][3];
    uint32_t hand_tracked[2];       // 0 not tracked, 1 orientation only, 2 position and orientation
    // where each controller points (ARKit's Accessory.LocationName.aim, its -z) in its grip's frame; zero when not
    // known. The grip's own -z runs along the Sense controller's handle (device 2026-11-04: the bow's line went up)
    float grip_aim[2][3];
} BotWVRPose;

#ifdef __cplusplus
extern "C" {
#endif
int botw_vr_enabled(void);                       // BOTW_VR=1 (runtime/src/game/vr_frame.cpp)
void* botw_vr_shared_event(void);               // id<MTLSharedEvent>, NULL until the renderer made it
int botw_vr_latest_frame(BotWVRFrame* out);     // the newest complete set; 0 when there is none yet
void botw_vr_open_immersive(void);              // BotWImmersive.swift: requests the immersive space (main thread)
int botw_vr_view_mode(void);                     // BOTW_VR_MODE=view: the game renders for the headset's eye poses
void botw_vr_set_pose(const BotWVRPose* pose);  // the compositor's newest pose (runtime/src/game/vr_render.cpp)
#ifdef __cplusplus
}
#endif
