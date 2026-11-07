// VR rendering patches: a camera and a projection per eye, and each eye's 3D image handed to the renderer.
//
// Two modes (BOTW_VR_MODE):
//   screen (default): agl::lyr::Layer::getRenderCamera (0x03ae4aa0) returns, for the game's 3D layer, a copy of its
//     sead::LookAtCamera moved half the eye distance along the camera's right axis: parallel eyes with the game's own
//     projection, for a stereo screen (the Mac's side-by-side window, the headset's screen in the immersive space).
//   view: the eyes are the headset's. First person (default; BOTW_VR_FIRST_PERSON=0 for third): the head sits at the
//     player's position, BOTW_VR_EYE_HEIGHT above the feet (1.55 m) and BOTW_VR_EYE_FORWARD ahead (0.1 m, out of the
//     head model), facing the game camera's heading (the right stick still turns); third person: at the game's camera.
//     Then the head's motion since the space opened (recentred on the first pose, yaw only) and the eye's place in
//     the head (BetterVR camera.cpp ResolveGameplayAnchorPosition / ResolveCameraBaseYaw / BuildCameraPoseFromBase); the projection
//     (agl::lyr::Layer::getRenderProjection, 0x03ae4aec, and the light pre-pass's, 0x030c1008) is the eye's own
//     asymmetric frustum. Poses come from the compositor (ios/src/BotWImmersive.swift -> botw_vr_set_pose); on a Mac
//     a fixed pose with Vision Pro-like frustums stands in (BOTW_VR_FAKE_YAW=<degrees> sways it).
// A port of BetterVR's patch_RND_StereoRendering_CameraAndProjection.asm and hooks GetRenderCamera,
// GetRenderProjection, ModifyLightPrePassProjectionMatrix (src/hooking/camera.cpp, src/utils/render_utils.h).
//
// Eye image (and HUD layer, below): where the frame's HDR image is composed, before the HUD is drawn over it (0x039b3044, the epilogue of
// the function that composes it; r26 = its agl::RenderBuffer), the colour buffer is sent to the renderer through the
// scan-buffer copy with target VR_EYE_TARGET | eye | pose tag << 16 (BetterVR patch_RND_Find3DFrameBuffer.asm marks
// the same place with a clear the Vulkan layer recognises). Cemu's LatteRenderTarget.cpp takes it from there.
#include <array>
#include <cstdio>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <condition_variable>
#include <mutex>

#include "../../../ios/src/vr_bridge.h"
#include "../input/input.h"
#include "hooks.h"
#include "vr.h"
extern "C" void botw_vr_crop(int eye, float* out);  // below: BOTW_VR_FOV per edge

using mem::r32;
using mem::rf32;
using mem::wf32;

namespace {
// sead::LookAtCamera: Matrix34f view matrix (row-major: rows = camera x, y, z axes in world, column 3 = translation),
// vtable, pos, at, up
constexpr uint32_t kCamSize = 0x58, kCamPos = 0x34, kCamAt = 0x40, kCamUp = 0x4c;
constexpr uint32_t kDefaultCamera = 0x105978c4;  // sDefaultCameraMatrix
constexpr uint32_t kEpsilon = 0x10323118;        // the game's epsilon constant
// sead::PerspectiveProjection (BetterVR modifiedCopy_seadPerspectiveProjection): dirty bytes, Matrix44f (row-major),
// device Matrix44f, posture, device z scale / offset, vtable, near, far, fovy, its sin / cos / tan, aspect, offset
constexpr uint32_t kProjSize = 0xb8, kProjMtx = 0x04, kProjDeviceMtx = 0x44, kProjZScale = 0x88, kProjZOffset = 0x8c,
                   kProjVtable = 0x90, kProjNear = 0x94, kProjFar = 0x98, kProjFovy = 0x9c, kProjAspect = 0xac,
                   kProjOffset = 0xb0;
constexpr uint32_t kPerspectiveVtable = 0x1027b54c;

uint32_t g_eye_camera[2], g_eye_projection[2];  // guest copies, one per eye
std::atomic<bool> g_gliding{false};  // first_person_base: the glider open (the near plane follows it)

bool view_mode() {
    static const bool on = [] {
        const char* e = getenv("BOTW_VR_MODE");
        bool v = vr::enabled() && e && strcmp(e, "view") == 0;
        if (vr::enabled()) LOG("vr: mode %s", v ? "view (the headset's eye poses and frustums)" : "screen (parallel eyes)");
        return v;
    }();
    return on;
}

// Events (cutscenes, talks, chests...): the game's camera directs them, so view mode steps back to the stereo
// screen for their duration (parallel eyes, the game's projection, no head motion on the camera, Link's head drawn):
// a camera swinging through a scene is easier to watch on a screen than as the view itself. EventMgr is asked once a
// frame (botw_vr_new_frame), as BetterVR's vr_updateSettings does; BetterVR's per-event table of which events keep
// first person is not in its repository, so for now every event goes to the screen.
std::atomic<bool> g_event{false};
std::atomic<bool> g_event_on_screen{false};  // an event (not a menu) on the screen in front
bool view_now() { return view_mode() && !g_event.load(std::memory_order_relaxed); }

float eye_distance() {  // game units are metres
    static const float d = [] {
        const char* e = getenv("BOTW_VR_IPD");
        float v = e ? (float)atof(e) : 0.064f;
        LOG("vr: eye distance %.3f m", v);
        return v;
    }();
    return d;
}

// ---- 4x4 matrices, column-major as simd_float4x4 ---------------------------------------------------------------
struct M4 {
    float m[16];  // m[col * 4 + row]
    float& at(int r, int c) { return m[c * 4 + r]; }
    float at(int r, int c) const { return m[c * 4 + r]; }
};
M4 identity() {
    M4 x{};
    x.m[0] = x.m[5] = x.m[10] = x.m[15] = 1;
    return x;
}
M4 mul(const M4& a, const M4& b) {
    M4 x{};
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            for (int k = 0; k < 4; k++) x.at(r, c) += a.at(r, k) * b.at(k, c);
    return x;
}
M4 rigid_inverse(const M4& a) {  // rotation + translation
    M4 x = identity();
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) x.at(r, c) = a.at(c, r);
    for (int r = 0; r < 3; r++) x.at(r, 3) = -(x.at(r, 0) * a.at(0, 3) + x.at(r, 1) * a.at(1, 3) + x.at(r, 2) * a.at(2, 3));
    return x;
}
M4 from_floats(const float* f) {
    M4 x;
    memcpy(x.m, f, sizeof x.m);
    return x;
}
// the head's place with only its heading kept (turning about +y), for recentring
M4 yaw_only(const M4& head) {
    float fx = -head.at(0, 2), fz = -head.at(2, 2), len = sqrtf(fx * fx + fz * fz);
    if (len < 1e-4f) fx = 0, fz = -1, len = 1;
    fx /= len, fz /= len;
    M4 x = identity();
    // columns: right = (-fz, 0, fx), up = y, back = -forward
    x.at(0, 0) = -fz, x.at(2, 0) = fx;
    x.at(0, 2) = -fx, x.at(2, 2) = -fz;
    for (int r = 0; r < 3; r++) x.at(r, 3) = head.at(r, 3);
    return x;
}

// ---- poses ------------------------------------------------------------------------------------------------------
std::mutex g_pose_lock;
std::condition_variable g_pose_arrived;  // a new pose from the compositor (one per headset frame)
std::chrono::steady_clock::time_point g_pose_time;  // when the newest pose arrived
float g_pose_interval = 1.0f / 90;  // the headset loop's frame time, smoothed (seconds; under g_pose_lock)
BotWVRPose g_pose;
bool g_have_pose = false;
std::atomic<bool> g_have_center{false};  // cleared by a recentre (the menu)
M4 g_center_inverse;  // inverse of the recentred head
uint32_t g_frame_pose_id;  // the pose of the frame being drawn (tagged onto its eye images)

float env_float(const char* name, float dflt);
BotWVRPose fake_pose() {  // a Mac: the head still (or swaying), Vision Pro-like eyes
    BotWVRPose p{};
    M4 head = identity();
    static const float sway = [] {
        const char* e = getenv("BOTW_VR_FAKE_YAW");
        return e ? (float)atof(e) : 0.0f;
    }();
    if (sway != 0) {
        // BOTW_VR_FAKE_YAW_FIXED: straight ahead for the first 20 s (the recentre), then turned by that angle and held
        static const bool fixed = getenv("BOTW_VR_FAKE_YAW_FIXED") != nullptr;
        static const auto start = std::chrono::steady_clock::now();
        double t = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        double since = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        static const float at = env_float("BOTW_VR_FAKE_YAW_AT", 20);  // seconds; later for a save that loads slowly
        float a = sway * (float)M_PI / 180 * (fixed ? (since > at ? 1.0f : 0.0f) : (float)sin(t * 2 * M_PI / 8));
        head.at(0, 0) = cosf(a), head.at(2, 0) = -sinf(a), head.at(0, 2) = sinf(a), head.at(2, 2) = cosf(a);
    }
    // BOTW_VR_FAKE_PITCH=<degrees>: 20 s after the start the head looks that far down (body / eye height tests)
    static const float pitch = env_float("BOTW_VR_FAKE_PITCH", 0);
    if (pitch != 0) {
        static const auto start = std::chrono::steady_clock::now();
        if (std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count() > 20) {
            const float b = -pitch * (float)M_PI / 180;  // about +x: down
            M4 r = identity();
            r.at(1, 1) = cosf(b), r.at(2, 1) = sinf(b), r.at(1, 2) = -sinf(b), r.at(2, 2) = cosf(b);
            head = mul(head, r);
        }
    }
    // BOTW_VR_FAKE_WALK=<metres>: 25 s after the start the head walks that far straight ahead at 0.5 m/s, stays there
    // 5 s, and walks back (roomscale tests)
    static const float walk = env_float("BOTW_VR_FAKE_WALK", 0);
    if (walk != 0) {
        static const auto start = std::chrono::steady_clock::now();
        const float since = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count() - 25;
        const float go = fabsf(walk) / 0.5f, out = since < 0 ? 0 : since < go ? since * 0.5f : since < go + 5 ? fabsf(walk)
                         : since < 2 * go + 5 ? fabsf(walk) - (since - go - 5) * 0.5f : 0;
        head.at(2, 3) = -out * (walk < 0 ? -1 : 1);  // -z is ahead
    }
    memcpy(p.origin_from_device, head.m, sizeof head.m);
    for (int e = 0; e < 2; e++) {
        M4 eye = identity();
        eye.at(0, 3) = (e ? 0.5f : -0.5f) * eye_distance();
        memcpy(p.device_from_view[e], eye.m, sizeof eye.m);
        // about 55 degrees to the outside, 45 to the nose, 45 up, 50 down
        float crop[4];
        botw_vr_crop(e, crop);
        p.tangents[e][0] = (e ? -1.0f : -1.43f) * crop[0];
        p.tangents[e][1] = (e ? 1.43f : 1.0f) * crop[1];
        p.tangents[e][2] = 1.0f * crop[2];
        p.tangents[e][3] = -1.19f * crop[3];
    }
    // BOTW_VR_FAKE_HANDS: controllers held in front of the chest (grip -z forward, +y up, as ARKit's), with =swing
    // the right one swinging down and back up every second (a sword stroke, fast enough to count as one), =left / =right only that one,
    // =jab the left one jabbing forward every 2 s;
    // BOTW_VR_FAKE_HAND_Y / _Z move them (metres from the eyes, default -0.25 / -0.4)
    static const char* hands = getenv("BOTW_VR_FAKE_HANDS");
    if (hands) {
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        for (int h = 0; h < 2; h++) {
            M4 g = identity();
            float pitch = 0;
            if (h == 1 && strcmp(hands, "swing") == 0) {  // +-70 degrees once a second, about the grip's x axis
                pitch = (float)(1.2 * sin(t * 2 * M_PI));
                p.angular_velocity[h][0] = (float)(1.2 * 2 * M_PI * cos(t * 2 * M_PI));
            }
            g.at(1, 1) = cosf(pitch), g.at(2, 1) = sinf(pitch), g.at(1, 2) = -sinf(pitch), g.at(2, 2) = cosf(pitch);
            static const float fy = env_float("BOTW_VR_FAKE_HAND_Y", -0.25f), fz = env_float("BOTW_VR_FAKE_HAND_Z", -0.4f);
            g.at(0, 3) = h ? 0.2f : -0.2f, g.at(1, 3) = fy, g.at(2, 3) = fz;
            if (strcmp(hands, "draw") == 0) {  // =draw: right hand at the bow, pulled back 30 cm and back every 6 s
                const float k = (float)fmod(t, 6.0) < 3 ? (float)fmod(t, 6.0) / 3 : 2 - (float)fmod(t, 6.0) / 3;
                g.at(0, 3) = h ? -0.05f + 0.1f * k : -0.15f, g.at(1, 3) = -0.2f, g.at(2, 3) = h ? -0.4f + 0.3f * k : -0.45f;
            }
            if (h == 0 && strcmp(hands, "jab") == 0)  // =jab: the left hand jabs forward for 0.1 s every 2 s (a parry)
                p.velocity[h][2] = fmod(t, 2.0) < 0.1 ? -3.0f : 0.0f;
            memcpy(p.origin_from_grip[h], g.m, sizeof g.m);
            p.hand_tracked[h] = strcmp(hands, h ? "left" : "right") == 0 ? 0 : 2;  // =left / =right: that hand only
        }
    }
    static uint32_t id;
    p.id = ++id;
    return p;
}

// The frame's pose: taken once at the start of each frame (vr_frame.cpp -> botw_vr_new_frame), so every camera,
// projection and visibility check of the frame uses the same one without locking. Two slots: a reader that still holds
// the previous frame's index reads a slot that is not being written.
BotWVRPose g_frame_pose[2];
std::atomic<uint32_t> g_frame_seq{0};  // the slot in use is g_frame_seq & 1

const BotWVRPose& current_pose() { return g_frame_pose[g_frame_seq.load(std::memory_order_acquire) & 1]; }

float env_float(const char* name, float dflt);
bool first_person();

// ---- projection ---------------------------------------------------------------------------------------------------
// The eyes' projection as last set: near, far and the device matrix's depth scale / offset, for turning the depth
// buffer back into distances (the visionOS compositor's per-pixel depth, latte_glue.mm)
std::atomic<float> g_depth_range[4] = {0.1f, 25000.0f, 1.0f, 0.0f};
// The eye's frustum into a sead::PerspectiveProjection, its near / far kept (BetterVR ApplyProjectionFov,
// RenderUtils::CalculateFOVAndOffset / CalculateProjectionMatrix)
void apply_frustum(uint32_t proj, const float tan_lrtb[4]) {
    const float l = tan_lrtb[0], r = tan_lrtb[1], t = tan_lrtb[2], b = tan_lrtb[3];
    const float w = r - l, h = t - b;
    if (!(w > 0) || !(h > 0)) return;
    // first person: the near plane pushed out a little (BOTW_VR_FP_NEAR, 0.12 m) so the face around the eyes (between
    // the eyeballs, vr_body.cpp) is clipped away; a climbed wall, about 0.2 m from the face, must stay in front of it
    static const float fp_near = env_float("BOTW_VR_FP_NEAR", 0.12f);
    // gliding: 0.2 m, the face kept out with the eyes in the head (first_person_base); the glider's bar and the hands
    // are farther
    const float near_now = g_gliding.load(std::memory_order_relaxed) ? std::max(fp_near, 0.2f) : fp_near;
    if (first_person() && vr::player_actor() && rf32(proj + kProjNear) < near_now) wf32(proj + kProjNear, near_now);
    const float fovy = 2 * atanf(h * 0.5f), n = rf32(proj + kProjNear), f = rf32(proj + kProjFar);
    g_depth_range[0] = n, g_depth_range[1] = f;
    g_depth_range[2] = rf32(proj + kProjZScale), g_depth_range[3] = rf32(proj + kProjZOffset);
    wf32(proj + kProjFovy, fovy);
    wf32(proj + kProjFovy + 4, sinf(fovy / 2));
    wf32(proj + kProjFovy + 8, cosf(fovy / 2));
    wf32(proj + kProjFovy + 12, tanf(fovy / 2));
    wf32(proj + kProjAspect, w / h);
    wf32(proj + kProjOffset, (l + r) / (2 * w));
    wf32(proj + kProjOffset + 4, (t + b) / (2 * h));
    float m[4][4] = {{2 / w, 0, (r + l) / w, 0},
                     {0, 2 / h, (t + b) / h, 0},
                     {0, 0, -(f + n) / (f - n), -2 * f * n / (f - n)},
                     {0, 0, -1, 0}};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) wf32(proj + kProjMtx + 4 * (i * 4 + j), m[i][j]);
    // the device matrix folds in the depth range
    const float zs = rf32(proj + kProjZScale), zo = rf32(proj + kProjZOffset);
    m[2][0] *= zs;
    m[2][1] *= zs;
    m[2][2] = (m[2][2] + m[3][2] * zo) * zs;
    m[2][3] = m[2][3] * zs + m[3][3] * zo;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) wf32(proj + kProjDeviceMtx + 4 * (i * 4 + j), m[i][j]);
    mem::w8(proj + 0, 0);  // dirty
    mem::w8(proj + 1, 0);  // device dirty
}

// ---- camera -------------------------------------------------------------------------------------------------------
void eye_camera_screen(uint32_t cam, uint32_t out, int eye) {
    // move along the camera's right axis (row 0 of the view matrix): pos and at shift by o, the view translation
    // of row 0 by -|o| (rows 1 and 2 are perpendicular to it)
    const float half = (eye == 0 ? -0.5f : 0.5f) * eye_distance();
    float right[3] = {rf32(cam + 0), rf32(cam + 4), rf32(cam + 8)};
    for (int i = 0; i < 3; i++) {
        wf32(out + kCamPos + 4 * i, rf32(cam + kCamPos + 4 * i) + right[i] * half);
        wf32(out + kCamAt + 4 * i, rf32(cam + kCamAt + 4 * i) + right[i] * half);
    }
    wf32(out + 12, rf32(cam + 12) - half);
}

float env_float(const char* name, float dflt) {
    const char* e = getenv(name);
    return e ? (float)atof(e) : dflt;
}
bool first_person() {
    static const bool on = [] {
        const char* e = getenv("BOTW_VR_FIRST_PERSON");
        bool v = !(e && strcmp(e, "0") == 0);
        LOG("vr: %s person", v ? "first" : "third");
        return v;
    }();
    return on;
}
constexpr uint32_t kActorMtx = 0x1f8;  // ActorWiiU::mtx, a row-major Matrix34f

}  // namespace
bool vr_first_person() { return first_person(); }  // vr_body.cpp
namespace {

}  // namespace
bool vr_player_eyes(uint32_t player_actor, float out[3]);  // vr_body.cpp
float vr_turn_heading(float game_rad, uint32_t seq);       // vr_turn.cpp
float fps_delta();                                         // fps.cpp
namespace {

// First person: the head's base at the player's eyes (the skeleton's, else a fixed height), turned to the held
// heading (vr_turn.cpp; the game camera's own outside first person)
// where the first-person view stands in the world (the base below, without the head's motion), for the hands
std::mutex g_view_base_lock;
float g_view_base[3];
bool g_have_view_base = false;

bool first_person_base(const M4& world_from_cam, M4& base) {
    const uint32_t player = vr::player_actor();
    if (!player || !first_person()) return false;
    static const float height = env_float("BOTW_VR_EYE_HEIGHT", 1.55f), forward = env_float("BOTW_VR_EYE_FORWARD", 0.1f);
    // the camera's heading (a turn of a about +y looks along (-sin a, 0, -cos a)) -> the held one
    const float a = vr_turn_heading(atan2f(world_from_cam.at(0, 2), world_from_cam.at(2, 2)),
                                    g_frame_seq.load(std::memory_order_relaxed));
    base = identity();
    base.at(0, 0) = cosf(a), base.at(2, 0) = -sinf(a);
    base.at(0, 2) = sinf(a), base.at(2, 2) = cosf(a);
    for (int r = 0; r < 3; r++) base.at(r, 3) = world_from_cam.at(r, 3);
    float eyes[3];
    if (vr_player_eyes(player, eyes)) {  // the eyes of the animated skeleton (vr_body.cpp): their height only
        // Across, the body's own position: the body turns after the head (vr_body.cpp), so the skeleton's eyes swing
        // round with every head turn, a camera movement at the game's frame rate the headset knows nothing of and
        // cannot reproject; the picture stepped as the head turned (device, 2026-11-01: "the picture refreshes bit by bit as I turn my head").
        // The head's own movement comes from the headset pose on top of this. So the eyes are taken as far in front of
        // the body as the skeleton has them, but straight ahead along the held heading (the body's position alone put
        // the eyes behind Link's face, its inside in view: Mac, 2026-11-01); a turn of the head then swings them round
        // with the headset's own movement
        const float px = rf32(player + kActorMtx + 12), py = rf32(player + kActorMtx + 28), pz = rf32(player + kActorMtx + 44);
        // Steadied, as BetterVR's camera.cpp (Link's position plus a fixed height, lowered by the game's own flags):
        // the camera rode the skeleton's head through every animation (sway, everything big, device 2026-11-05), then a
        // height learnt from the skeleton sank and stayed low after standing near things (2026-11-06; Fable's review:
        // the first sample, 1.59 m, was not his usual 1.36-1.42 m stance, and a "25 cm low for 0.8 s" rule kept
        // tripping). Now the eyes stand BOTW_VR_STAND_EYES (1.59 m) above his feet and follow the skeleton only while
        // he crouches, swims, glides, rides or is ragdolled (PlayerBase::moveBitFlags +0x8dc: 4, 0x400, 1<<15, 1<<20, 1<<19), eased
        // 0.15 s. How far ahead of the body the eyes sit is eased over a second
        static const float stand = env_float("BOTW_VR_STAND_EYES", 1.59f);
        static float offset = -1, ahead_s = -1;
        static double last_t = 0;
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        const float dt = last_t > 0 ? (float)std::min(now - last_t, 0.1) : 0;
        last_t = now;
        const float rel = eyes[1] - py;
        const uint32_t flags = mem::r32(player + 0x8dc);
        const bool riding = (flags & (1u << 20)) != 0;
        const float ahead_now = std::min(hypotf(eyes[0] - px, eyes[2] - pz), riding ? 0.6f : 0.3f);
        if (offset < 0) offset = stand, ahead_s = ahead_now;
        // gliding (1 << 15) too: Link hangs below the glider, his head well under the standing height, and looking down
        // showed his face from above (device 2026-11-11; the face does not follow the hidden head bones, only the near
        // plane keeps it out of view, which needs the eyes in the head)
        const bool follow = (flags & (4u | 0x400u | (1u << 15) | (1u << 19) | (1u << 20))) != 0;
        {
            static bool followed = false;
            if (follow != followed) LOG("vr: eyes %s (skeleton %.2f m, flags %08x)", follow ? "follow the body" : "at standing height", rel, flags);
            followed = follow;
        }
        // riding: Link leans down and forward at a gallop, and the eyes, quick to come down but a second behind going
        // forward, sank onto his back and the shield slung on it (device 2026-11-09). The height is held at the highest
        // the settled rider's eyes reach (BetterVR keeps a fixed saddle height), after the mounting animation, slowly
        // letting go, and the eyes go forward with him
        static float ride_peak = -1, ride_time = 0;
        float eye_target = follow ? rel : stand;
        if (riding) {
            ride_time += dt;
            if (ride_time > 1.2f) {
                if (ride_peak < 0) ride_peak = rel, LOG("vr: riding: eyes held from %.2f m above the rider", rel);
                ride_peak = rel > ride_peak ? rel : ride_peak + (rel - ride_peak) * std::min(dt / 30.0f, 1.0f);
                eye_target = ride_peak;
            }
        } else {
            ride_time = 0, ride_peak = -1;
        }
        offset += (eye_target - offset) * std::min(dt / 0.15f, 1.0f);
        ahead_s += (ahead_now - ahead_s) * std::min(dt / (riding ? 0.25f : 1.0f), 1.0f);
        const float ahead = ahead_s;
        // gliding: the eyes go into the head itself, across too, and without the lift below; there his head tips back to
        // look ahead, his face turns up, and from 0.2 m above it the face showed beneath the view (device 2026-11-11, twice)
        const bool gliding = (flags & (1u << 15)) != 0;
        static float glide = 0;  // 0 the usual place, 1 in the head, eased 0.3 s
        glide += ((gliding ? 1.0f : 0.0f) - glide) * std::min(dt / 0.3f, 1.0f);
        g_gliding.store(gliding, std::memory_order_relaxed);
        // riding: ahead along Link's own facing (the horse's), not the held heading. Riding, the left stick turns the
        // horse wherever the head looks, so the two part (76 to 106 degrees in a device log, 2026-11-13), and up to
        // 0.6 m ahead along the heading put the eyes beside the saddle, the horse's body on the left ("my body is
        // shifted to the right"). Eased 0.3 s, so mounting and getting off do not jump
        static float ride_dir = 0, ahead_x = 0, ahead_z = 0;
        ride_dir += ((riding ? 1.0f : 0.0f) - ride_dir) * std::min(dt / 0.3f, 1.0f);
        {
            float fx = -base.at(0, 2), fz = -base.at(2, 2);  // the held heading's forward
            const float lx = rf32(player + kActorMtx + 8), lz = rf32(player + kActorMtx + 40);  // the model's +z
            const float ll = hypotf(lx, lz);
            if (ride_dir > 0.001f && ll > 1e-4f && std::isfinite(ll)) {
                fx += (lx / ll - fx) * ride_dir, fz += (lz / ll - fz) * ride_dir;
                const float fl = hypotf(fx, fz);
                if (fl > 1e-4f) fx /= fl, fz /= fl;
            }
            ahead_x = fx * ahead, ahead_z = fz * ahead;
        }
        base.at(0, 3) = px + ahead_x + (eyes[0] - (px + ahead_x)) * glide;
        // BOTW_VR_HEIGHT (the launcher's view height, metres, default 0.2) on top of Link's eyes: at his own 1.59 m the
        // grass stood as high as him, "like standing in a wheat field" (device 2026-11-02); BetterVR's PlayerHeightOffset
        static const float lift = env_float("BOTW_VR_HEIGHT", 0.2f);
        base.at(1, 3) = py + offset + lift * (1 - glide);
        base.at(2, 3) = pz + ahead_z + (eyes[2] - (pz + ahead_z)) * glide;
        std::lock_guard<std::mutex> l(g_view_base_lock);
        for (int r = 0; r < 3; r++) g_view_base[r] = base.at(r, 3);
        g_have_view_base = true;
        return true;
    }
    const float fx = -base.at(0, 2), fz = -base.at(2, 2);  // the heading
    base.at(0, 3) = rf32(player + kActorMtx + 12) + fx * forward;
    base.at(1, 3) = rf32(player + kActorMtx + 28) + height;
    base.at(2, 3) = rf32(player + kActorMtx + 44) + fz * forward;
    return true;
}

}  // namespace
void vr_roomscale_applied(float out[2]);  // vr_roomscale.cpp
namespace {
std::atomic<uint32_t> g_recentres{0};

// First person: the distance the body has walked for the head (roomscale) taken off a placement in the recentred frame
// (the head's or a controller's), level only
void take_off_walked(M4& m) {
    if (!first_person()) return;
    float a[2];
    vr_roomscale_applied(a);
    m.at(0, 3) -= a[0], m.at(2, 3) -= a[1];
}

// The eye's placement in the world: the game camera (or the player's eyes), the head's motion since the recentre,
// the eye in the head
M4 eye_world(uint32_t cam, int eye, const BotWVRPose& pose) {
    // the game camera's world placement: its view matrix's rows are its axes in the world
    M4 world_from_cam = identity();
    float t[3];
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++) world_from_cam.at(col, row) = rf32(cam + 4 * (row * 4 + col));
        t[row] = rf32(cam + 4 * (row * 4 + 3));
    }
    for (int r = 0; r < 3; r++)
        world_from_cam.at(r, 3) = -(world_from_cam.at(r, 0) * t[0] + world_from_cam.at(r, 1) * t[1] +
                                    world_from_cam.at(r, 2) * t[2]);
    M4 base = world_from_cam;
    first_person_base(world_from_cam, base);
    M4 head = mul(g_center_inverse, from_floats(pose.origin_from_device));
    take_off_walked(head);
    return mul(mul(base, head), from_floats(pose.device_from_view[eye]));
}

void eye_camera_view(uint32_t cam, uint32_t out, int eye) {
    const BotWVRPose& pose = current_pose();
    g_frame_pose_id = pose.id;
    const M4 world_from_eye = eye_world(cam, eye, pose);
    // back into a LookAtCamera: rows of the view = the eye's axes, pos, at = pos + forward, up
    float pos[3] = {world_from_eye.at(0, 3), world_from_eye.at(1, 3), world_from_eye.at(2, 3)};
    for (int row = 0; row < 3; row++) {
        float trans = 0;
        for (int col = 0; col < 3; col++) {
            float v = world_from_eye.at(col, row);
            wf32(out + 4 * (row * 4 + col), v);
            trans -= v * pos[col];
        }
        wf32(out + 4 * (row * 4 + 3), trans);
    }
    for (int i = 0; i < 3; i++) {
        wf32(out + kCamPos + 4 * i, pos[i]);
        wf32(out + kCamAt + 4 * i, pos[i] - world_from_eye.at(i, 2));
        wf32(out + kCamUp + 4 * i, world_from_eye.at(i, 1));
    }
}
// ---- visibility ---------------------------------------------------------------------------------------------------
bool cull_fix() {  // BOTW_VR_CULL_FIX=0 keeps the game's own checks (A/B comparisons)
    static const bool on = [] {
        const char* e = getenv("BOTW_VR_CULL_FIX");
        return !(e && strcmp(e, "0") == 0);
    }();
    return on;
}
// Whether a sphere lies in either eye's frustum (BetterVR hook_CheckIfCameraCanSeePos): the game culls with its own
// camera's narrow frustum, which would drop what the headset sees beside it. The eyes are pulled back 1 m, as BetterVR
// does, since a third-person game expects to see around the player.
// The eyes' world-to-eye matrices for one camera and frame, kept per thread: the checks run thousands of times a
// frame on several threads, the camera and pose change once a frame
struct EyeFrusta {
    uint32_t seq = ~0u, cam = 0;
    float cam_mtx[12] = {};
    M4 eye_from_world[2];
};
thread_local EyeFrusta t_frusta;

const EyeFrusta& eye_frusta(uint32_t cam, const BotWVRPose& pose) {
    EyeFrusta& f = t_frusta;
    const uint32_t seq = g_frame_seq.load(std::memory_order_acquire);
    float mtx[12];
    for (int i = 0; i < 12; i++) mtx[i] = rf32(cam + 4 * i);
    if (f.seq == seq && f.cam == cam && memcmp(f.cam_mtx, mtx, sizeof mtx) == 0) return f;
    f.seq = seq, f.cam = cam;
    memcpy(f.cam_mtx, mtx, sizeof mtx);
    for (int eye = 0; eye < 2; eye++) {
        M4 world_from_eye = eye_world(cam, eye, pose);
        for (int r = 0; r < 3; r++) world_from_eye.at(r, 3) += world_from_eye.at(r, 2);  // 1 m back along +z
        f.eye_from_world[eye] = rigid_inverse(world_from_eye);
    }
    return f;
}

// widen: the eyes' tangents times this (the actor update check below asks with a margin)
bool eyes_can_see(uint32_t cam, const float center[3], float radius, float near_clip, float far_clip, float widen = 1.0f) {
    const BotWVRPose& pose = current_pose();
    const EyeFrusta& frusta = eye_frusta(cam, pose);
    for (int eye = 0; eye < 2; eye++) {
        const M4& eye_from_world = frusta.eye_from_world[eye];
        float p[3];
        for (int r = 0; r < 3; r++)
            p[r] = eye_from_world.at(r, 0) * center[0] + eye_from_world.at(r, 1) * center[1] +
                   eye_from_world.at(r, 2) * center[2] + eye_from_world.at(r, 3);
        const float t[4] = {pose.tangents[eye][0] * widen, pose.tangents[eye][1] * widen, pose.tangents[eye][2] * widen,
                            pose.tangents[eye][3] * widen};  // left, right, top, bottom
        const float x = p[0], y = p[1], z = p[2];  // looking down -z
        auto outside = [radius](float d, float slope) { return d > radius * sqrtf(1 + slope * slope); };
        if (outside(x + t[1] * z, t[1]) || outside(-x - t[0] * z, t[0]) || outside(y + t[2] * z, t[2]) ||
            outside(-y - t[3] * z, t[3]) || z > -near_clip + radius || -z > far_clip + radius)
            continue;
        return true;
    }
    return false;
}

void check_visibility(PPCContext* c) {
    const float center[3] = {rf32(c->r[4]), rf32(c->r[4] + 4), rf32(c->r[4] + 8)};
    c->r[3] = eyes_can_see(c->r[3], center, (float)c->f[1], (float)c->f[2], (float)c->f[3]) ? 1 : 0;
}
}  // namespace

extern "C" int botw_vr_view_mode(void) { return view_mode(); }
extern "C" int botw_vr_view_now(void) { return view_now(); }

// BOTW_VR_FOV: how much of each eye's view the game draws, per edge (plan A2, quality notes section 18). One number
// for every edge, or four: outer (towards the temple), inner (towards the nose), top, bottom, mirrored between the
// eyes, each 0.5-1 (0.9: that edge's tangent times 0.9). out: left, right, top, bottom for that eye (0 left, 1 right).
// The tangents handed to the game and the view pass's black edge both use it (BotWImmersive.swift), the render
// targets' shape follows it (graphic_pack_shim.cpp), and the Mac's fixed pose too.
extern "C" void botw_vr_crop(int eye, float* out) {
    static const std::array<float, 4> c = [] {  // outer, inner, top, bottom
        std::array<float, 4> v{1, 1, 1, 1};
        const char* e = getenv("BOTW_VR_FOV");
        if (!e || !*e) return v;
        float f[4];
        const int n = sscanf(e, "%f,%f,%f,%f", &f[0], &f[1], &f[2], &f[3]);
        bool ok = n == 1 || n == 4;
        for (int i = 0; ok && i < n; i++) ok = std::isfinite(f[i]);
        if (!ok) {
            LOG("vr: BOTW_VR_FOV \"%s\" not understood (one number, or four: outer,inner,top,bottom), field left whole", e);
            return v;
        }
        for (int i = 0; i < 4; i++) v[i] = std::clamp(n == 4 ? f[i] : f[0], 0.5f, 1.0f);  // device 2026-11-10: the lenses show 50-70 % of the tangents
        LOG("vr: field per edge: outer %.3f, inner %.3f, top %.3f, bottom %.3f (BOTW_VR_FOV)", v[0], v[1], v[2], v[3]);
        return v;
    }();
    out[0] = eye ? c[1] : c[0];  // the left eye's left edge is its outer one
    out[1] = eye ? c[0] : c[1];
    out[2] = c[2];
    out[3] = c[3];
    // the left eye's inner edge drawn whole: the system's recordings and screenshots are made from the left eye and
    // reach past its crop on the nose side, where there was nothing but black, then the stretched, dimmed edge
    // (user 2026-11-15, a band down the right side since the crop came in). Same render targets (both eyes share them;
    // graphic_pack_shim.cpp keeps their shape from the right eye), so the left eye has about 9 % fewer pixels a
    // degree across; the lenses do not show that edge. BOTW_VR_CAPTURE_EDGE=0: cropped as the right eye
    static const bool capture_edge = [] {
        const char* e = getenv("BOTW_VR_CAPTURE_EDGE");
        return !(e && strcmp(e, "0") == 0);
    }();
    if (eye == 0 && capture_edge) out[1] = 1.0f;
}
// LatteRenderTarget.cpp: an event on the screen in front shows the game's finished TV picture to both eyes. The eye
// images are taken before the last steps of the frame: on the headset the right eye came out scattered and the walk out of
// the shrine overexposed (device 2026-11-09). BOTW_VR_EVENT_EYES=1 keeps the two eye images
// The Camera rune's viewfinder and the scope (flat on the screen, g_flat_camera) too: outside the view the frame buffer
// is not cleared for the HUD, so the "HUD layer" is the whole picture again, with whatever alpha the 3D picture left,
// and laid over the eye image (colour + eye * (1 - alpha)) it added the picture to itself: washed out, too bright
// (device 2026-11-13). BOTW_VR_FLAT_CAMERA_EYES=1: the eye images as before
extern std::atomic<bool> g_flat_camera;
extern "C" int botw_vr_event_uses_tv(void) {
    static const bool eyes = getenv("BOTW_VR_EVENT_EYES") != nullptr;
    static const bool flat_eyes = getenv("BOTW_VR_FLAT_CAMERA_EYES") != nullptr;
    if (!view_mode()) return 0;
    if (!eyes && g_event_on_screen.load(std::memory_order_relaxed)) return 1;
    return !flat_eyes && g_flat_camera.load(std::memory_order_relaxed);
}
// BotWImmersive.swift's loading panel: Link is in the world (the title menu's backdrop is drawn in the view too, and
// taken for the game's view it ended the first way in before the world had loaded: device 2026-11-03)
extern "C" int botw_vr_player_ready(void) { return vr::player_actor() ? 1 : 0; }

// window.mm: the left stick walks Link where the head looks, not along the held heading the game's camera keeps (that
// one only turns with the right stick): with the head turned, forward on the stick went off to the side ("pushing the stick forward, Link
// drifts to the left", device 2026-11-02). As BetterVR, the stick is turned by the head's yaw since the recentre
// how far the head looks up (+) or down (-), degrees (vr_body.cpp's bow log)
float vr_head_pitch_deg() {
    const float* m = current_pose().origin_from_device;  // column-major: the z axis is 8..10
    return asinf(std::fmin(std::fmax(-m[9], -1.0f), 1.0f)) * 180 / (float)M_PI;
}

float vr_turn_last_heading();  // vr_turn.cpp
float vr_turn_view_offset();   // vr_turn.cpp
extern std::atomic<bool> g_slate_map;
bool vr_event_running();  // below
extern "C" void botw_vr_move_stick(float* x, float* y) {
    if (!view_now() || !first_person() || !g_have_center || (*x == 0 && *y == 0)) return;
    // an event played in the view (a talk, a shop, the stable's horse list): the stick picks in its menus, and turned by
    // the head, up came out a diagonal the list barely took (device 2026-11-13, "hard to pick a horse")
    if (vr_event_running()) return;
    // the Sheikah Slate's map: the stick moves the map's cursor, as it is. The map is drawn in the view (its screen is
    // not the pause menu's, which goes on the screen in front), so the stick was turned there as for walking, by the
    // head's yaw and the held heading's offset from the game camera: with that camera stuck (item 101) the offset was
    // 180 degrees and up on the stick moved the cursor down (device 2026-11-13)
    if (g_slate_map.load(std::memory_order_relaxed)) return;
    // not while climbing a ladder or a wall (PlayerBase::moveBitFlags +0x8dc: swimming-or-climbing without swimming,
    // BetterVR game_structs.h): there up on the stick is up the ladder, and turned by a head looking 60 degrees aside
    // it climbed at a crawl and drifted sideways (device 2026-11-05, "can't climb the ladder")
    // Not even raw: the game reads the stick in its camera's frame, and that camera keeps the held heading, 54-88
    // degrees off the ladder in the logs, so raw "up" did not climb either (2026-11-06, Fable's review). Turned to
    // Link's own facing instead, up on the stick is towards the ladder or the wall whatever the head and the camera do
    if (const uint32_t player = vr::player_actor()) {
        const uint32_t flags = mem::r32(player + 0x8dc);
        // a ladder (0x10 alone): the stick as it is; vr_ladder.cpp turns up / down into the ladder's own classes
        // (device 2026-11-06: turned, up reached the game as a 45-degree diagonal, class 2, "neither")
        // riding (1 << 20) as on a wall: left is left for the horse wherever the head looks. Turned by the head and the held
        // heading's offset, as on foot, it came out at any angle, and past 90 degrees the left stick turned the horse right
        // (device 2026-11-10)
        const bool riding = (flags & (1u << 20)) != 0;
        // riding: the stick as it is. The game steers the horse from the stick as the horse sees it, and turned to
        // Link's facing (2026-11-10) forward came out sideways by however far the horse had turned from the held
        // heading: forward turned the horse left, back turned it right (device 2026-11-13, turned 15 to 78 degrees)
        if (riding) return;
        if ((flags & 0x10) && !(flags & (0x80 | 0x400))) return;
        if ((flags & 0x10) && !(flags & 0x400)) {
            const float lx = rf32(player + kActorMtx + 8), lz = rf32(player + kActorMtx + 40);  // the model's +z, forward
            const float h = vr_turn_last_heading();
            const float cfx = -sinf(h), cfz = -cosf(h);  // the camera's forward
            const float crx = cosf(h), crz = -sinf(h);   // its right
            // Link's facing, right of the held heading, plus the held heading's offset from the game camera (Fable's
            // review: the climbing camera is one the pivot hooks do not keep on the held heading)
            const float a = atan2f(lx * crx + lz * crz, lx * cfx + lz * cfz) + vr_turn_view_offset();
            const float c = cosf(a), s = sinf(a), sx = *x, sy = *y;
            *x = sx * c + sy * s;
            *y = -sx * s + sy * c;
            static uint32_t logged_seq = 0;  // once per climb (Fable: 20 lines went in 0.1 s)
            const uint32_t seq = g_frame_seq.load(std::memory_order_relaxed);
            if (seq - logged_seq > 60) LOG("vr: climbing: stick turned %.0f deg to Link's facing", a * 180 / (float)M_PI);
            logged_seq = seq;
            return;
        }
    }
    const M4 head = mul(g_center_inverse, from_floats(current_pose().origin_from_device));
    const float fx = -head.at(0, 2), fz = -head.at(2, 2);  // the head's forward, level, in the recentred frame
    if (fx * fx + fz * fz < 1e-6f) return;
    // how far right of straight ahead the head looks, plus how far right of the game camera the held heading is (the
    // game reads the stick in its camera's frame; 0 where the pivot hooks keep that camera on the held heading)
    const float a = atan2f(fx, -fz) + vr_turn_view_offset();
    const float c = cosf(a), s = sinf(a), sx = *x, sy = *y;
    *x = sx * c + sy * s;
    *y = -sx * s + sy * c;
}
// sense_vision.mm: play in the headset's view (no menu, no event on the screen), where the Sense buttons take the play
// layout (BetterVR's, or the classic one's play mapping); elsewhere (the title, menus, the screen) they keep the menu one
// (Cross A, Circle B). Third person too: with first person required it kept the menu layout in play (user 2026-11-11:
// "the controller mapping is strange, not the same as first person"); the motion gestures stay first person only
extern "C" int botw_vr_in_play(void) { return view_now(); }
extern "C" void botw_vr_log(const char* line) { LOG("%s", line); }  // BotWImmersive.swift: lines for runtime.log
// BotWImmersive.swift's loading panel: is the game's title menu up, is its loading screen up
std::atomic<bool> g_title_screen{false}, g_loading_screen{false}, g_pause_menu{false};
std::atomic<bool> g_flat_camera{false};  // the Camera rune or the scope, flat on the screen (botw_vr_new_frame)
bool g_map_screen = false;  // the map is open (game thread)
// the Sheikah Slate's own screen is up (AppHome_00: the map and its apps). AppMap_00 is also up in play (the flag above
// is true all through it), this one only with the real map (device log 2026-11-13: shown as the map opens, hidden
// when it closes or warps)
std::atomic<bool> g_slate_map{false};
extern "C" int botw_vr_slate_map(void) { return g_slate_map.load(std::memory_order_relaxed) ? 1 : 0; }
std::atomic<bool> g_event_running{false};  // any event, also those played in the view (vr_turn.cpp follows its camera)
bool vr_event_running() { return g_event_running.load(std::memory_order_relaxed); }
// the pause menu or the map
// the controllers take the menus' buttons there too (sense_vision.mm; user 2026-11-13: the grip that confirms in
// menus did nothing in the camera)
extern "C" int botw_vr_flat_camera(void) { return g_flat_camera.load(std::memory_order_relaxed) ? 1 : 0; }
extern "C" int botw_vr_pause_menu(void) { return g_pause_menu.load(std::memory_order_relaxed) ? 1 : 0; }
extern "C" int botw_vr_title_screen(void) { return g_title_screen.load(std::memory_order_relaxed) ? 1 : 0; }
extern "C" int botw_vr_loading_screen(void) { return g_loading_screen.load(std::memory_order_relaxed) ? 1 : 0; }
extern "C" void botw_vr_depth_range(float out[4]) {  // latte_glue.mm
    for (int i = 0; i < 4; i++) out[i] = g_depth_range[i].load(std::memory_order_relaxed);
}  // view mode and no event running

// vr_gesture.cpp (host input thread): the newest pose in the room, the headset's own when there is one, else the
// frame's (a Mac's fake one); false outside first person
bool vr_room_pose(BotWVRPose& out) {
    if (!view_now() || !first_person()) return false;
    {
        std::lock_guard<std::mutex> l(g_pose_lock);
        if (g_have_pose) {
            out = g_pose;
            return true;
        }
    }
    out = current_pose();
    return true;
}

// vr_roomscale.cpp (game thread): the frame's head in the recentred frame and how many recentres there have been
bool vr_head_in_room(float out[3], uint32_t* recentres) {
    if (!g_have_center) return false;
    const M4 head = mul(g_center_inverse, from_floats(current_pose().origin_from_device));
    for (int i = 0; i < 3; i++) out[i] = head.at(i, 3);
    *recentres = g_recentres.load(std::memory_order_relaxed);
    return true;
}

float vr_turn_last_heading();  // vr_turn.cpp
// vr_body.cpp: the head's level offset from where the body stands, in the world (its move since the recentre less what
// the body walked, turned to the held heading as the eyes are placed), so the body can follow it
bool vr_head_offset_world(float out[3]) {
    if (!view_now() || !first_person() || !g_have_center) return false;
    M4 head = mul(g_center_inverse, from_floats(current_pose().origin_from_device));
    take_off_walked(head);
    const float a = vr_turn_last_heading(), c = cosf(a), s = sinf(a), hx = head.at(0, 3), hz = head.at(2, 3);
    out[0] = c * hx + s * hz, out[1] = 0, out[2] = -s * hx + c * hz;
    return true;
}

// vr_aim.cpp: where the eyes are in the world (the view's base plus the head's move since the recentre, turned to the
// held heading as vr_head_offset_world turns it), false outside first person
bool vr_view_base_world(float out[3]);
bool vr_eye_world(float out[3]) {
    if (!view_now() || !first_person() || !g_have_center || !vr_view_base_world(out)) return false;
    M4 head = mul(g_center_inverse, from_floats(current_pose().origin_from_device));
    take_off_walked(head);
    const float a = vr_turn_last_heading(), c = cosf(a), s = sinf(a), hx = head.at(0, 3), hz = head.at(2, 3);
    out[0] += c * hx + s * hz, out[1] += head.at(1, 3), out[2] += -s * hx + c * hz;
    return true;
}

// vr_aim.cpp: where the first-person view looks in the world (a unit vector): the held heading turned by the head's
// motion since the recentre; false outside first person or without a recentred pose
float vr_turn_last_heading();  // vr_turn.cpp
bool vr_view_forward(float out[3]) {
    if (!view_now() || !first_person() || !g_have_center || !vr::player_actor()) return false;
    const M4 head = mul(g_center_inverse, from_floats(current_pose().origin_from_device));
    const float f[3] = {-head.at(0, 2), -head.at(1, 2), -head.at(2, 2)};  // the head's -z, in the recentred frame
    const float a = vr_turn_last_heading(), c = cosf(a), s = sinf(a);
    out[0] = c * f[0] + s * f[2], out[1] = f[1], out[2] = -s * f[0] + c * f[2];  // turned about +y by the heading
    return true;
}

// vr_aim.cpp: a controller's velocity in the world (m/s): the room's, turned as the view is (the recentre, then the
// held heading); false while it is not tracked in position or before the recentre
bool vr_hand_velocity_world(int hand, float out[3]) {
    const BotWVRPose& p = current_pose();
    if (!g_have_center || p.hand_tracked[hand] < 2) return false;
    const float* v = p.velocity[hand];
    float r[3];
    for (int i = 0; i < 3; i++)
        r[i] = g_center_inverse.at(i, 0) * v[0] + g_center_inverse.at(i, 1) * v[1] + g_center_inverse.at(i, 2) * v[2];
    const float a = vr_turn_last_heading(), c = cosf(a), s = sinf(a);
    out[0] = c * r[0] + s * r[2], out[1] = r[1], out[2] = -s * r[0] + c * r[2];
    return true;
}

// vr_weapon.cpp: a controller's motion in the room (origin space): its grip (column-major 4x4), linear and angular
// velocity, the head's position and the pose's id (one sample per pose); false while it is not tracked in position
bool vr_hand_motion(int hand, float grip[16], float velocity[3], float angular[3], float head[3], uint32_t* id) {
    const BotWVRPose& p = current_pose();
    if (p.hand_tracked[hand] < 2) return false;
    *id = p.id;
    memcpy(grip, p.origin_from_grip[hand], 64);
    memcpy(velocity, p.velocity[hand], 12);
    memcpy(angular, p.angular_velocity[hand], 12);
    for (int r = 0; r < 3; r++) head[r] = p.origin_from_device[12 + r];
    return true;
}

// vr_body.cpp: where the first-person view stands in the world (its base: the body's position, the skeleton's eye
// height, the eyes' distance ahead along the held heading); the hands hang from the same point as the eyes, or they
// sat beside the view whenever the body had turned another way (device, 2026-11-02: "everything is shifted to the left")
bool vr_view_base_world(float out[3]) {
    std::lock_guard<std::mutex> l(g_view_base_lock);
    if (!g_have_view_base) return false;
    for (int r = 0; r < 3; r++) out[r] = g_view_base[r];
    return true;
}

// vr_body.cpp: a controller's grip (0 left, 1 right hand) in the frame the first-person view is built on (the head
// at the recentre: x right, y up, -z ahead), as a row-major 3x4; false while it is not tracked in position
bool vr_hand_in_base(int hand, float out[12]) {
    const BotWVRPose& p = current_pose();
    if (!g_have_center || p.hand_tracked[hand] < 2) return false;
    M4 g = mul(g_center_inverse, from_floats(p.origin_from_grip[hand]));
    take_off_walked(g);
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++) out[r * 4 + c] = g.at(r, c);
    return true;
}

// where a controller points, in its grip's frame (BotWImmersive.swift, from the aim space); false when not known
bool vr_hand_aim_local(int hand, float out[3]) {
    const float* d = current_pose().grip_aim[hand];
    const float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(l > 0.5f)) return false;
    for (int i = 0; i < 3; i++) out[i] = d[i] / l;
    return true;
}

// The in-game menu's "Recenter view": the next frame takes the head's heading as straight ahead again
extern "C" void botw_vr_recenter(void) {
    g_have_center.store(false);
}

#include "vr_events_first_person.inc"
bool first_person_event(const char* name) {
    if (!name) return false;
    return std::binary_search(std::begin(kFirstPersonEvents), std::end(kFirstPersonEvents), name,
                              [](const char* a, const char* b) { return strcmp(a, b) < 0; });
}

void vr_throw_track_frame();  // vr_aim.cpp

// vr_frame.cpp, at the start of each frame (view mode): the newest pose becomes the frame's
extern "C" void botw_vr_new_frame(PPCContext* c) {
    if (!view_mode()) return;
    vr_throw_track_frame();
    // the event manager's active event (EventMgr::getActiveEventName(sInstance, &event, &entry) -> active)
    if (const uint32_t mgr = r32(0x1046d3ac)) {
        // guest_call leaves the callee's r3, r4 and f1 behind: the caller's (procFrame's framework) must survive
        const uint32_t r3 = c->r[3], r4 = c->r[4];
        const double f1 = c->f[1];
        const uint32_t sp = c->r[1];
        c->r[1] = sp - 0x20;  // two guest stack slots for the names
        mem::w32(c->r[1] + 8, 0), mem::w32(c->r[1] + 12, 0);
        const bool active = guest_call(c, 0x031ca1c0, {mgr, c->r[1] + 8, c->r[1] + 12}) != 0;
        const char* name = mem::str(r32(c->r[1] + 8));
        const char* entry = mem::str(r32(c->r[1] + 12));
        g_event_running.store(active, std::memory_order_relaxed);
        c->r[1] = sp;
        c->r[3] = r3, c->r[4] = r4, c->f[1] = f1;
        // events BetterVR plays in first person (picking an item up, chests, cooking, most talks) stay in the view;
        // the rest go to the screen in front (device 2026-11-02: an item picked up, "GetDemo", put the game on the
        // screen). BOTW_VR_EVENTS_SCREEN=1: every event on the screen, as before
        static const bool all_on_screen = getenv("BOTW_VR_EVENTS_SCREEN") != nullptr;
        // BOTW_VR_CUTSCENES=screen (the launcher's "Story cutscenes: On the screen", user 2026-11-17): every scripted
        // cutscene (DemoNNN) on the screen in front, talks and small events still in the view. The game's camera flies
        // round the scene in many of them, and from Link's head the eyes could go into walls (the Great Plateau Tower)
        static const bool cutscenes_on_screen = [] {
            const char* e = getenv("BOTW_VR_CUTSCENES");
            const bool on = e && strcmp(e, "screen") == 0;
            LOG("vr: story cutscenes %s (BOTW_VR_CUTSCENES)", on ? "on the screen" : "as listed (first person where BetterVR plays them so)");
            return on;
        }();
        // the pause menu (inventory, map, ...) too goes on the screen in front, as BetterVR shows it flat
        // (camera.cpp UseFlatCameraPresentation: ScreenId::PauseMenuInfo_00): it draws Link in its own camera beside
        // the panels, and in the headset's view he stood behind them (device 2026-11-02). The screen manager
        // (0x1047e650) +0x18 holds a pointer per screen, PauseMenuInfo_00 the 0x2f-th, set while the menu is open
        bool menu = false;
        if (const uint32_t screens = r32(0x1047e650)) {
            if (const uint32_t list = r32(screens + 0x18)) {
                menu = r32(list + 0x2f * 4) != 0;
                // for the headset's loading panel (BotWImmersive.swift): the title menu and the loading screen,
                // visible as BetterVR's IsScreenVisible counts it (open, or fading in or out: state +0x96, open
                // priority +0x95)
                auto visible = [&](uint32_t id) {
                    const uint32_t screen = r32(list + id * 4);
                    return screen && (mem::r8(screen + 0x96) != 0 || (int8_t)mem::r8(screen + 0x95) > 0);
                };
                const bool title = visible(0x01), loading_screen = visible(0x52);  // Title_00, LoadingWeapon_00
                if (title != g_title_screen.load(std::memory_order_relaxed)) LOG("vr: title menu %s", title ? "shown" : "gone");
                if (loading_screen != g_loading_screen.load(std::memory_order_relaxed))
                    LOG("vr: loading screen %s", loading_screen ? "shown" : "gone");
                g_title_screen.store(title, std::memory_order_relaxed);
                g_loading_screen.store(loading_screen, std::memory_order_relaxed);
                // the map (AppMap_00, BetterVR game_structs.h ScreenId) takes the pause menu's buttons as well
                // (sense_vision.mm; user 2026-11-05), but only with the Sheikah Slate's pad behind it
                // (SeekPadMenuBG_00): AppMap_00 alone also counted in play once the standard HUD (the minimap)
                // was on, and turned L2 into L, the rune (device 2026-11-05)
                // every screen shown or hidden, for runtime.log (device 2026-11-05: a blank bar after the whistle)
                {
                    static uint64_t shown[2] = {0, 0};
                    static int budget = 300;
                    for (uint32_t id = 0; id < 0x63 && budget > 0; id++) {
                        const bool v = visible(id), was = (shown[id >> 6] >> (id & 63)) & 1;
                        if (v != was) {
                            budget--, LOG("vr: screen 0x%02x %s", id, v ? "shown" : "hidden");
                            shown[id >> 6] ^= 1ull << (id & 63);
                        }
                    }
                }
                const bool app_map = visible(0x2a), pad_bg = visible(0x1e);
                const bool map = app_map && pad_bg;
                static bool app_map_was = false, pad_bg_was = false;
                if (app_map != app_map_was || pad_bg != pad_bg_was)
                    LOG("vr: screens AppMap_00 %d SeekPadMenuBG_00 %d", app_map, pad_bg);
                app_map_was = app_map, pad_bg_was = pad_bg;
                if (map != g_map_screen) LOG("vr: map %s", map ? "open" : "closed");
                g_map_screen = map;
                const bool slate = visible(0x2c);  // AppHome_00
                if (slate != g_slate_map.load(std::memory_order_relaxed))
                    LOG("vr: Sheikah Slate map %s", slate ? "open (the sticks are the map's)" : "closed");
                g_slate_map.store(slate, std::memory_order_relaxed);
            }
        }
        static bool menu_was = false;
        if (menu != menu_was) LOG("vr: pause menu %s", menu ? "open: on the screen" : "closed");
        menu_was = menu;
        g_pause_menu.store(menu || g_map_screen, std::memory_order_relaxed);
        // taking the Sheikah Slate (Demo700_0, Demo701_0): the view was turned by the event's camera and its prompt came
        // out blurred in first person (device 2026-11-09), so these two go on the screen in front
        const bool slate = name && (!strcmp(name, "Demo700_0") || !strcmp(name, "Demo701_0"));
        // the Camera rune's viewfinder and the scope on the screen in front, flat, with the game's own camera (BetterVR
        // IsFlatCameraModeActive: RuneMgr 0x10463798, flags +0x4c bit 4 with rune 5 selected at +0x1d4, bit 5 the
        // scope): in the view the frame buffer is cleared for the HUD once the eye's picture is taken, and the photo,
        // copied from it later, came out black (device 2026-11-12, Fable 5.2). BOTW_VR_FLAT_CAMERA=0: in the view
        bool flat_rune = false;
        if (const uint32_t rm = r32(0x10463798)) {
            const uint32_t f = r32(rm + 0x4c);
            flat_rune = ((f & 0x10) && r32(rm + 0x1d4) == 5) || (f & 0x20);
        }
        static const bool flat_on = env_float("BOTW_VR_FLAT_CAMERA", 1.0f) != 0.0f;
        g_flat_camera.store(flat_on && flat_rune, std::memory_order_relaxed);
        const bool screen = menu || (flat_on && flat_rune) || (active && (all_on_screen || slate || !first_person_event(name) ||
                                                                       (cutscenes_on_screen && name && !strncmp(name, "Demo", 4)) ||
                                                                       // the Sheikah Slate map opened inside an event (the old man at the tower: "See those
                                                                       // blue icons?"): drawn in the view it flickered (user 2026-11-17), on the screen
                                                                       g_slate_map.load(std::memory_order_relaxed)));
        g_event_on_screen.store(active && screen && !menu, std::memory_order_relaxed);
        if (screen != g_event.load(std::memory_order_relaxed)) {
            if (screen) LOG("vr: event %s (%s) begins: on the screen", name ? name : "?", entry ? entry : "?");
            else LOG("vr: event over: back to the view");
            g_event.store(screen, std::memory_order_relaxed);
        }
        static std::string last_event;
        if (active && name && last_event != name) {
            last_event = name;
            if (!screen) LOG("vr: event %s (%s) stays in the view (first person)", name, entry ? entry : "?");
            else LOG("vr: event %s (%s) on the screen", name, entry ? entry : "?");  // one event following another
        }
        if (!active) last_event.clear();
    }
    // BOTW_VR_LOG_FRAME_Y=1 (tests): the player's height every game frame, for timing jumps in game frames
    static const bool log_frame_y = getenv("BOTW_VR_LOG_FRAME_Y") != nullptr;
    if (log_frame_y && vr::player_actor()) {
        static uint32_t n = 0;
        LOG("vr: frame %u y %.3f", n++, rf32(vr::player_actor() + kActorMtx + 28));
    }
    // every jump (device 2026-11-02, twice: "falling... like floating", right after loading normal, slower after walking about):
    // the rise from the last frame on the ground to the top and the fall from the top until the height stops changing,
    // in seconds and game frames, and the game time the frames' deltas add up to (fps.cpp), to tell a slowed game from
    // slow physics
    if (const uint32_t pl = vr::player_actor(); pl && view_now()) {
        static float last_y = 0, ground_y = 0, peak = 0;
        static int state = 0, rise_frames = 0, fall_frames = 0;  // 0 on the ground, 1 rising, 2 falling
        static double game_time = 0;
        static std::chrono::steady_clock::time_point left, top;
        static uint32_t last_pl = 0, start_buttons = 0;
        static float max_dy = 0;
        const float y = rf32(pl + kActorMtx + 28);
        // the first frame with a player (and a new one after a load), or a jump of more than 2 m in a frame (a
        // teleport, a load): the height starts there. The first headset log's "jump 116.92 m: up 2.94 s" at 16.9 s
        // was this: last_y and ground_y started at 0, and the first frame after loading "rose" to the height of
        // the map (device 2026-11-04)
        if (pl != last_pl || fabsf(y - last_y) > 2.0f) {
            if (state != 0) LOG("vr: jump tracking restarted (%s, height %.2f m)", pl != last_pl ? "new player" : "moved", y);
            last_pl = pl, last_y = y, ground_y = y, state = 0;
        }
        const float dy = y - last_y;
        const auto now = std::chrono::steady_clock::now();
        // a jump rises 5 cm or more a game frame from the start; walking up a slope or a step does not (those made
        // "rises" of seconds in the first headset log)
        if (state == 0 && dy > 0.05f) {
            state = 1, rise_frames = 0, fall_frames = 0, peak = y, game_time = 0, left = now, max_dy = 0;
            start_buttons = input::gamepad().buttons;  // the jump's button (B on the GamePad), or none: a slope
        }
        if (state == 1 && dy > max_dy) max_dy = dy;
        if (state != 0) game_time += fps_delta() / 30.0;
        if (state == 1) {
            rise_frames++;
            if (y > peak) peak = y;
            if (dy < -0.005f) state = 2, top = now;
            if (rise_frames > 600) state = 0;  // climbing, gliding
        } else if (state == 2) {
            fall_frames++;
            if (fabsf(dy) < 0.005f || dy > 0.02f || fall_frames > 600) {
                state = 0;
                const double rise = std::chrono::duration<double>(top - left).count();
                const double fall = std::chrono::duration<double>(now - top).count();
                static int logged;
                if (peak - ground_y > 0.3f && logged < 60)
                    logged++, LOG("vr: jump %.2f m: up %.2f s (%d frames), down %.2f m in %.2f s (%d frames), game time %.2f s "
                                  "of %.2f s, delta %.2f, move flags %08x, buttons %08x (at the start %08x), fastest "
                                  "rise %.2f m a frame",
                                  peak - ground_y, rise, rise_frames, peak - y, fall, fall_frames, game_time, rise + fall,
                                  fps_delta(), r32(pl + 0x8dc), input::gamepad().buttons, start_buttons, max_dy);
            }
        }
        if (state == 0) ground_y = y;
        last_y = y;
    }
    BotWVRPose p;
    {
        std::unique_lock<std::mutex> l(g_pose_lock);
        // frame pacing (BOTW_VR_PACE=n, 2 by default, 0 off): a game frame starts every n-th headset frame, so each picture
        // shows for the same number of refreshes. Unpaced at 51-61 fps on the 90 Hz display, pictures alternated
        // between one refresh and two (device 2026-11-02, "vr timing: shown 1x 238 2x 331"): everything that moves
        // advanced unevenly ("a little stuttery"). Tried on 2026-11-01 when the compositor was heavy: its own loop fell to 45 Hz
        // and the game to 22 fps; since foveation and render quality 0.6 its GPU work is 1.4 ms and it has not missed a
        // deadline ("late 0/900"), which the timing line keeps watching
        static const int pace = [] {
            const char* e = getenv("BOTW_VR_PACE");
            return e ? atoi(e) : 2;
        }();
        static uint32_t last_id = 0;
        // the headset's view not running (taken off, the app sent away): the game waits until it is back. Running on,
        // its GPU work, which the system no longer ran, piled up at about 0.5 GB a second until the app was killed
        // (device, 2026-11-01: 1.3 GB to 6.2 GB in 10 s after the view closed)
        if (g_have_pose && std::chrono::steady_clock::now() - g_pose_time > std::chrono::milliseconds(500)) {
            LOG("vr: the headset's view stopped, the game waits for it");
            const auto paused = std::chrono::steady_clock::now();
            while (std::chrono::steady_clock::now() - g_pose_time > std::chrono::milliseconds(500))
                g_pose_arrived.wait_for(l, std::chrono::milliseconds(200));
            LOG("vr: the headset's view is back after %.1f s, the game goes on",
                std::chrono::duration<float>(std::chrono::steady_clock::now() - paused).count());
        }
        // adaptive (device 2026-11-06: after an explosion the headset's own loop dropped from 90 to 45 Hz, picture
        // latency 2 frames of 22 ms, and the game, still waiting for every 2nd frame, locked at 22.5 fps with its GPU
        // half idle): with the loop at 60 Hz or less (frames of 15 ms or more) a game frame starts every frame
        static int effective = pace;
        // with hysteresis (to every frame above 15 ms, back below 12 ms) and a log budget (Fable's review)
        int want = pace <= 1 ? pace : effective == 1 ? (g_pose_interval < 0.012f ? pace : 1)
                                                   : (g_pose_interval > 0.015f ? 1 : pace);
        // BOTW_VR_RECOVER_90=<seconds> (an experiment, off by default): the loop has never come back from 45 Hz in the logs
        // (3 runs). With the loop slow the game keeps its pace (22.5 fps, the GPU half idle) for that long, to see if a
        // lighter load brings 90 Hz back; if not, every frame runs for 30 s before the next try
        {
            static const float recover = env_float("BOTW_VR_RECOVER_90", 0);
            using clock = std::chrono::steady_clock;
            static clock::time_point light_since{}, next_try{};
            static bool light = false;
            const auto now = clock::now();
            if (recover > 0 && pace > 1) {
                if (g_pose_interval < 0.012f) {
                    if (light) LOG("vr: recover: the loop is back at %.0f Hz after %.1f s of lighter load", 1.0f / g_pose_interval,
                                   std::chrono::duration<float>(now - light_since).count());
                    light = false;
                } else if (g_pose_interval > 0.015f && now >= next_try) {
                    if (!light) light = true, light_since = now, LOG("vr: recover: loop at %.0f Hz, lighter load for %.0f s", 1.0f / g_pose_interval, recover);
                    if (std::chrono::duration<float>(now - light_since).count() < recover) want = pace;
                    else light = false, next_try = now + std::chrono::seconds(30), LOG("vr: recover: the loop stayed slow");
                }
            }
        }
        if (want != effective) {
            static int budget = 30;
            if (budget > 0) budget--, LOG("vr: headset loop at %.0f Hz, game frame every %d headset frame(s)", 1.0f / g_pose_interval, want);
            effective = want;
        }
        if (g_have_pose && effective > 1 && last_id)
            g_pose_arrived.wait_for(l, std::chrono::milliseconds(40),
                                    [] { return g_pose.id - last_id >= (uint32_t)effective; });
        p = g_have_pose ? g_pose : fake_pose();
        last_id = p.id;
    }
    if (!g_have_center) {
        g_center_inverse = rigid_inverse(yaw_only(from_floats(p.origin_from_device)));
        g_have_center = true;
        LOG("vr: view recentred on pose %u", p.id);
        g_recentres.fetch_add(1, std::memory_order_relaxed);
    }
    // the head no higher than BOTW_VR_HEAD_UP_M (0.1 m) above where it was at the recentre: recentred sitting (as the
    // app starts) and then standing put the eyes high over Link's head (device, 2026-11-01: "the eyes sit high above
    // Link\'s head"). The recentre's height follows the head up instead, for the eyes and the hands alike; ducking still works
    {
        static const float up_max = env_float("BOTW_VR_HEAD_UP_M", 0.1f);
        const float up = mul(g_center_inverse, from_floats(p.origin_from_device)).at(1, 3);
        if (up > up_max) g_center_inverse.at(1, 3) -= up - up_max;
        // and back down slowly (about 8 s) once the head is lower again: raised only, a lean or a look over something
        // left the whole view low for good, crouching and standing in the game could not undo it (device 2026-11-06,
        // Fable's review). A real duck still lowers the view at once and only eases back after seconds
        static double last = 0;
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        const float dt = last > 0 ? (float)std::min(t - last, 0.1) : 0;
        last = t;
        if (up < 0) g_center_inverse.at(1, 3) -= up * std::min(dt / 8.0f, 1.0f);
    }
    // BOTW_VR_LOG_POS=1: the player's position and the head's offset from the recentre, about once a second (roomscale)
    static const bool log_pos = getenv("BOTW_VR_LOG_POS") != nullptr;
    if (log_pos && vr::player_actor() && g_frame_seq.load(std::memory_order_relaxed) % 40 == 0) {
        const uint32_t pl = vr::player_actor();
        const M4 head = mul(g_center_inverse, from_floats(p.origin_from_device));
        float walked[2];
        vr_roomscale_applied(walked);
        LOG("vr: pos player (%.2f %.2f %.2f) head offset (%.2f %.2f %.2f) walked (%.2f %.2f)", rf32(pl + kActorMtx + 12),
            rf32(pl + kActorMtx + 28), rf32(pl + kActorMtx + 44), head.at(0, 3), head.at(1, 3), head.at(2, 3), walked[0],
            walked[1]);
    }
    const uint32_t next = g_frame_seq.load(std::memory_order_relaxed) + 1;
    g_frame_pose[next & 1] = p;
    g_frame_seq.store(next, std::memory_order_release);
}

// The game's two "can the camera see this sphere" checks (r3 camera, r4 centre, f1 radius, f2 near, f3 far -> r3),
// in view mode answered for the headset's eyes (BetterVR patch_FixVisibilityChecks.asm). The second is only reached
// through its wrapper at 0x0319025c, which loads near / far from the camera (+96 / +100) and jumps into it.
BOTW_REPLACE(0x0318ffa8) {
    if (!view_now() || !cull_fix()) return orig_0x0318ffa8(c);
    check_visibility(c);
}
BOTW_REPLACE(0x0319025c) {
    if (!view_now() || !cull_fix()) return orig_0x0319025c(c);
    c->f[3] = rf32(c->r[3] + 100);
    c->f[2] = rf32(c->r[3] + 96);
    check_visibility(c);
}

// Actors the game freezes as "off screen" (2026-11-15, looked into for the report of a horse stopped mid-gallop and
// Bokoblins hanging in the air; for those see the LOD level below). The actor LOD manager (instance at 0x1047c728, calc
// 0x038571c4, every frame) builds a frustum from the game's own camera (0x03191b70 of the Camera at 0x1046cf88) and
// the 3D layer's render projection (0x03ae4aec, in view mode the eye's, above), keeps it at manager + 3684 (view
// matrix, near / far at + 3908, four side planes at + 3944) and walks the actors (0x038544f0): one with a radius
// (the s16 at actor + 1188: 100, 200 or 1000 for names on lists in the code at 0x03851630 / 0x03851828 / 0x038514ac,
// flags, windmill wings, shrine entrances, sound emitters; 0 for everything else, animals and enemies included)
// whose sphere (position at actor + 516 / 532 / 548) lies outside gets BaseProc flags |= 0x17 at + 92 (0x03854a68),
// which skips every job of it until a later walk finds it inside (0x03854a84). On a television that is "off
// screen". In first person the game camera keeps the held heading while the head looks elsewhere (riding, the two
// were 76 to 106 degrees apart in a device log, 2026-11-13), so what the game called off screen could be in the
// middle of the headset's view (Mac, head turned 90 degrees: 11 of the 17 actors frozen a walk were in the eyes'
// view: flags that stopped waving, shrine entrances).
// In view mode the question is put to the headset's eyes instead (as the visibility checks above; the tangents half
// as wide again and the eyes 1 m back, since the head turns faster than a camera stick and the picture shown is
// rotated on after the pose the game drew with): 0x03854894 has just loaded actor + 1172, a float the walk treats as
// "never frozen" when above 0 (f29), so a visible actor is given 1 there and takes the game's own inside path.
// Actors outside the eyes' view freeze as on a television. BOTW_VR_LOD_FIX=0: the game's frustum as before;
// BOTW_VR_LOD_LOG=1: every 300 walks, how many actors the game's frustum froze and how many of those the eyes saw
namespace {
std::atomic<uint32_t> g_lod_camera{0};
std::atomic<uint32_t> g_lod_walks{0}, g_lod_frozen{0}, g_lod_frozen_seen{0}, g_lod_kept{0};
bool lod_fix() {
    static const bool on = [] {
        const char* e = getenv("BOTW_VR_LOD_FIX");
        const bool v = !(e && strcmp(e, "0") == 0);
        if (vr::enabled()) LOG("vr: actors outside the game camera's view keep updating while the eyes see them: %d (BOTW_VR_LOD_FIX)", v);
        return v;
    }();
    return on;
}
bool lod_log() {
    static const bool on = env_float("BOTW_VR_LOD_LOG", 0) != 0;
    return on;
}
bool lod_eyes_see(uint32_t actor) {
    const uint32_t cam = g_lod_camera.load(std::memory_order_relaxed);
    if (!cam) return true;
    const float center[3] = {rf32(actor + 516), rf32(actor + 532), rf32(actor + 548)};
    const float radius = (float)(int16_t)mem::r16(actor + 1188);
    if (!std::isfinite(center[0]) || !std::isfinite(center[1]) || !std::isfinite(center[2])) return true;
    return eyes_can_see(cam, center, radius, 0.0f, 1.0e9f, 1.5f);
}
}  // namespace
// vr_actors.cpp's actor log: where an actor is in the left eye's picture (across and down, 0-1, of the drawn field)
// and how far to the left of the game camera's heading it lies (degrees); false before the first LOD walk
bool vr_actor_in_view(uint32_t actor, float out[3]) {
    const uint32_t cam = g_lod_camera.load(std::memory_order_relaxed);
    if (!cam || !view_now()) return false;
    const BotWVRPose& pose = current_pose();
    const M4 e = rigid_inverse(eye_world(cam, 0, pose));
    const float w[3] = {rf32(actor + 516), rf32(actor + 532) + 1.0f, rf32(actor + 548)};  // a metre above its feet
    float p[3];
    for (int r = 0; r < 3; r++) p[r] = e.at(r, 0) * w[0] + e.at(r, 1) * w[1] + e.at(r, 2) * w[2] + e.at(r, 3);
    const float* t = pose.tangents[0];  // left, right, top, bottom
    out[0] = p[2] < -0.01f ? (p[0] / -p[2] - t[0]) / (t[1] - t[0]) : -1.0f;
    out[1] = p[2] < -0.01f ? (t[2] - p[1] / -p[2]) / (t[2] - t[3]) : -1.0f;
    float g[3];  // in the game camera's frame: its view matrix rows
    for (int r = 0; r < 3; r++) g[r] = rf32(cam + 16 * r) * w[0] + rf32(cam + 16 * r + 4) * w[1] + rf32(cam + 16 * r + 8) * w[2] + rf32(cam + 16 * r + 12);
    out[2] = atan2f(-g[0], -g[2]) * 180 / (float)M_PI;
    return true;
}
BOTW_HOOK(0x038572e8) { g_lod_camera.store(c->r[30], std::memory_order_relaxed); }  // mr. r30, r3: the game camera  // mr. r30, r3: the game camera
BOTW_HOOK(0x03854894) {  // lfs f0, 1172(r31): r31 the actor
    if (!view_now() || !lod_fix() || c->f[0] > 0.0) return;
    if (!lod_eyes_see(c->r[31])) return;
    c->f[0] = 1.0;
    if (lod_log()) g_lod_kept.fetch_add(1, std::memory_order_relaxed);
}
BOTW_HOOK(0x03854a6c) {  // ori r5, r11, 0x17: the actor (r31) is outside the game's frustum and is frozen
    if (!lod_log() || !view_now()) return;
    const bool seen = lod_eyes_see(c->r[31]);
    g_lod_frozen.fetch_add(1, std::memory_order_relaxed);
    if (!seen) return;
    g_lod_frozen_seen.fetch_add(1, std::memory_order_relaxed);
    // each kind of actor once (60 at most)
    const char* name = mem::str(r32(c->r[31] + 4));
    static std::mutex lock;
    static std::vector<std::string> named;
    std::lock_guard<std::mutex> l(lock);
    if (!name || named.size() >= 60 || std::find(named.begin(), named.end(), name) != named.end()) return;
    named.push_back(name);
    LOG("vr: lod: %s at %.1f %.1f %.1f (radius %d) frozen by the game's frustum in the eyes' view", name,
        rf32(c->r[31] + 516), rf32(c->r[31] + 532), rf32(c->r[31] + 548), (int)(int16_t)mem::r16(c->r[31] + 1188));
}
// The LOD level (2026-11-16: on the headset far Bokoblins still stood frozen until the rider came close, with both
// changes above on and "0 of them in the eyes' view" in the log: the radius test above is not what stops enemies).
// After the walk each LOD state gets its level from 0x038552e0 (r4 = the state, r5 = the actor's position, f1 = its
// radius, f2 = its distance squared): 0 near; else the same frustum of the game camera decides: inside, bit
// 0x20000000 of state + 12 is set and the level is 1 (or 3 / 4 further out); outside, the bit is cleared and the
// level is 2, or 5 when the actor has a physics body and is further than the state's distance (+72). At level 3
// or 5 the walk stops the actor (0x038540c0: actor + 94 |= 0x10, its job4 no longer pushed). So an enemy outside the
// game camera's view and past that distance stands still, which nobody sees on a television. Mac, a Moblin, walking
// away from it: with the game camera turned away and the head looking at it, level 2 up to about 110 m, then level 5,
// + 94 = 0x10, job4 pushed 0 times in 300 frames; with the game camera facing it, level 1 at the same distances.
// At level 2 already the model holds its pose: the user's save (2026-11-16, riding in West Necluda, the game camera
// turned 80 degrees away from a Bokoblin group 62 to 102 m off, the view on them): level 2 and the same pose in
// every capture over six seconds; with this, level 1 and they move.
// In view mode the bit is what the headset's eyes say (the same widened test as above), both where the game set it
// (0x03855494) and where it cleared it (0x038554e8); r29 carries the flags on to the level's branch.
// BOTW_VR_LOD_FIX=0 turns this off as well
namespace {
std::atomic<uint32_t> g_lod_level_seen{0}, g_lod_level_unseen{0};
void lod_level_view(PPCContext* c, bool game_inside) {
    if (!view_now() || !lod_fix()) return;
    const uint32_t cam = g_lod_camera.load(std::memory_order_relaxed), pos = c->r[5];
    if (!cam || !pos) return;
    const float center[3] = {rf32(pos), rf32(pos + 4), rf32(pos + 8)};
    if (!std::isfinite(center[0]) || !std::isfinite(center[1]) || !std::isfinite(center[2])) return;
    const float radius = (float)c->f[1];
    const bool inside = eyes_can_see(cam, center, std::isfinite(radius) && radius > 0 ? radius : 0.0f, 0.0f, 1.0e9f, 1.5f);
    if (inside == game_inside) return;
    const uint32_t state = c->r[31];
    c->r[29] = inside ? c->r[29] | 0x20000000u : c->r[29] & ~0x20000000u;
    mem::w32(state + 12, c->r[29]);
    if (lod_log()) (inside ? g_lod_level_seen : g_lod_level_unseen).fetch_add(1, std::memory_order_relaxed);
}
}  // namespace
BOTW_HOOK(0x03855494) { lod_level_view(c, true); }   // stw r29, 12(r31): inside the game's frustum
BOTW_HOOK(0x038554e8) { lod_level_view(c, false); }  // stw r29, 12(r31): outside it
// The seam down the sky (2026-11-15 / 16: a straight vertical line in the headset's picture, the sky, clouds and far
// cliffs brighter and hazier on one side; at dusk, riding, looking back over a shoulder). Found by leaving out one
// pixel shader at a time at the Mac repro (dusk, head turned 110 degrees): the line goes with pixel shader
// 3df0bb0da1c740d5, one draw a pass, 480 indices of a fixed mesh: the sun's haze, a card the game hangs on its camera
// (model block of 192 bytes: four parameter vectors, then the world matrix twice, no rotation, translation = the
// game camera's position). On a television the camera is inside the card and it covers the picture. In first person
// the eyes are in Link's head, some metres ahead of the game's third-person camera, so looking back they saw the card
// from outside: a bright rectangle about 90 degrees wide behind the player, with straight edges (the "seam" was its
// side; all of the view when looking straight back). Mac, with that draw switched on and off within one run: at 0
// and 60 degrees it changes nothing in the sky, at 110 only the part left of the line.
// In view mode a model block whose matrices sit at the game camera's position is read by the GPU as a copy with the
// translation moved to the head (a ring of our own; the game's data stays): the card is round the viewer again.
// Called where the renderer reads a shader's uniforms out of its blocks (LatteBufferData.cpp, on the GPU thread: the
// game binds these blocks from display lists, so GX2SetVertexUniformBlock never sees them); one compare for any
// other block. BOTW_VR_CAMERA_MODELS=0: the game's matrices
bool vr_eye_world(float out[3]);
extern "C" uint32_t botw_vr_camera_block(uint32_t size, uint32_t data) {
    if (size != 192 || !data || !vr::enabled()) return data;
    static const bool on = [] {
        const bool v = env_float("BOTW_VR_CAMERA_MODELS", 1.0f) != 0.0f;
        LOG("vr: models the game centres on its camera are centred on the eyes: %d (BOTW_VR_CAMERA_MODELS)", v);
        return v;
    }();
    const uint32_t cam = g_lod_camera.load(std::memory_order_relaxed);
    if (!on || !cam || !view_now() || !first_person()) return data;
    float m[2][12];
    for (int k = 0; k < 2; k++) memcpy(m[k], mem::host(data) + 64 + 64 * k, sizeof m[k]);  // host floats, as the GPU reads them
    const float pos[3] = {rf32(cam + kCamPos), rf32(cam + kCamPos + 4), rf32(cam + kCamPos + 8)};
    for (int k = 0; k < 2; k++)  // rows (x y z | translation): both at the camera (1 m: the block is a frame behind, and
        for (int i = 0; i < 3; i++)  // the game hangs these a little below the camera it reports, 0.1 to 0.3 m seen)
            if (!(fabsf(m[k][4 * i + 3] - pos[i]) <= 1.0f)) return data;
    float eye[3];  // the head's place as the frame's camera hook left it (this runs on the GPU thread, a frame later)
    if (!vr_eye_world(eye)) return data;
    for (int i = 0; i < 3; i++)
        if (!std::isfinite(eye[i])) return data;
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < 3; i++) m[k][4 * i + 3] += eye[i] - pos[i];
    static uint32_t ring = 0;
    static std::atomic<unsigned> next{0};
    constexpr unsigned kSlots = 1024;  // the GPU reads the block later; some tens of these a frame
    static std::once_flag once;
    std::call_once(once, [] { ring = mem::sys_alloc(192 * kSlots, 256); });
    const uint32_t copy = ring + 192 * (next.fetch_add(1, std::memory_order_relaxed) % kSlots);
    memcpy(mem::host(copy), mem::host(data), 192);
    for (int k = 0; k < 2; k++) memcpy(mem::host(copy) + 64 + 64 * k, m[k], sizeof m[k]);
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 3)
        LOG("vr: a model at the game camera (%.2f %.2f %.2f) moved to the eyes (%.2f %.2f %.2f)", pos[0], pos[1], pos[2], eye[0], eye[1], eye[2]);
    return copy;
}
BOTW_HOOK(0x038544f0) {  // the walk's first instruction
    if (!lod_log() || !view_now()) return;
    const uint32_t n = g_lod_walks.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n % 300) return;
    LOG("vr: lod: in 300 walks the game's frustum froze %u actor updates, %u of them in the eyes' view; %u kept running for the eyes; "
        "levels: %u times an actor outside the game camera's view counted as seen (the eyes see it), %u times one inside it as unseen",
        g_lod_frozen.exchange(0), g_lod_frozen_seen.exchange(0), g_lod_kept.exchange(0), g_lod_level_seen.exchange(0),
        g_lod_level_unseen.exchange(0));
}

// Far actors' turns (2026-11-15, found while looking for the frozen Bokoblins; on the headset it did not cure them,
// 2026-11-16: that is the LOD level below). An actor nobody asked to
// keep active (0x037a1af8) and further away than its LOD distance is updated one frame in N (N = 60, or the LOD
// state's own period at + 100): the walk gives it BaseProc flags |= 7 (0x03854d74, no job of types 0-2 pushed)
// unless its id % N equals the walk counter % N (manager + 4988, one more at the end of every calc, 0x03857a1c).
// With two calcs a stereo frame the counter is even in every left calc and odd in every right one (Mac: counter 292
// pass 1, 293 pass 2, 294 pass 1 ...) and N is even, so an actor's turn always came in the same pass, and each pass
// runs only part of an actor's jobs (job0_1 in the right pass, the others in the left, vr_actors.cpp): part of its
// update never ran at all, instead of all of it once in 60 frames (Mac, towers 1.8 to 3.9 km away, 600 frames:
// job0_1 pushed 20 times and run 0 times for one, job1_1 never for another; with the counter below every job 10
// times). Such an actor stays as it was, in mid-stride or mid-air, until the player comes within its distance and it
// is updated every frame again. The counter now moves once a stereo frame (the left calc's step is taken back), so
// both calcs of a frame give an actor the same turn. BOTW_VR_LOD_TURN=0: one step a calc as before
BOTW_HOOK(0x03857a1c) {  // addi r9, r9, 1
    static const bool on = [] {
        const char* e = getenv("BOTW_VR_LOD_TURN");
        const bool v = !(e && strcmp(e, "0") == 0);
        if (vr::enabled()) LOG("vr: far actors' update turns counted once a stereo frame: %d (BOTW_VR_LOD_TURN)", v);
        return v;
    }();
    if (vr::enabled() && on && vr::calc_pass() == 1) c->r[9] -= 1;
}

extern "C" void botw_vr_set_pose(const BotWVRPose* pose) {
    {
        std::lock_guard<std::mutex> l(g_pose_lock);
        g_pose = *pose;
        const auto t = std::chrono::steady_clock::now();
        if (g_have_pose) {
            const float d = std::chrono::duration<float>(t - g_pose_time).count();
            if (d > 0.005f && d < 0.1f) g_pose_interval += 0.02f * (d - g_pose_interval);
        }
        g_pose_time = t;
        if (!g_have_pose) LOG("vr: headset poses arriving (first id %u)", pose->id);
        g_have_pose = true;
    }
    g_pose_arrived.notify_all();
}

BOTW_REPLACE(0x03ae4aa0) {
    const uint32_t layer = c->r[3];
    orig_0x03ae4aa0(c);
    if (!vr::enabled()) return;
    const uint32_t cam = c->r[3];
    // only the layer's own perspective camera once it is set up (BetterVR's checks): not the default camera, not a
    // special camera (0x19c), not one at the origin or looking straight along z
    if (cam != r32(layer + 0x48) || cam == kDefaultCamera || rf32(cam + kCamPos) == 0.0f ||
        fabsf(rf32(cam + 0x48)) < rf32(kEpsilon))
        return;
    const int eye = vr::eye();
    // built in a scratch copy and copied over in one go, under a lock: the game's camera copied into the returned
    // buffer and then turned into the eye's in place left a window in which render jobs on other cores read the game's
    // camera (Fable 5.2, 2026-11-11); with the head 60-90 degrees from it the shadow cascades were fitted to the wrong
    // view for a frame and a whole shadow went (device: "the shadow under the stable roof flickers")
    static std::mutex lock[2];
    std::lock_guard<std::mutex> l(lock[eye]);
    static uint32_t scratch[2];
    uint32_t& out = g_eye_camera[eye];
    if (!out) out = mem::sys_alloc(kCamSize, 16);
    if (!scratch[eye]) scratch[eye] = mem::sys_alloc(kCamSize, 16);
    memcpy(mem::host(scratch[eye]), mem::host(cam), kCamSize);
    if (view_now()) eye_camera_view(cam, scratch[eye], eye);
    else eye_camera_screen(cam, scratch[eye], eye);
    memcpy(mem::host(out), mem::host(scratch[eye]), kCamSize);
    c->r[3] = out;
}

// agl::lyr::Layer::getRenderProjection: the layer's perspective projection, as the eye's frustum (view mode)
BOTW_REPLACE(0x03ae4aec) {
    const uint32_t layer = c->r[3];
    orig_0x03ae4aec(c);
    if (!view_now()) return;
    const uint32_t proj = c->r[3];
    // the layer's own perspective projection; BetterVR leaves the one with far 10000 alone
    if (proj != r32(layer + 0x4c) || r32(proj + kProjVtable) != kPerspectiveVtable || rf32(proj + kProjFar) == 10000.0f)
        return;
    static bool logged;
    if (!logged) {
        logged = true;
        LOG("vr: game projection near %.3f far %.1f fovy %.3f aspect %.3f", rf32(proj + kProjNear), rf32(proj + kProjFar),
            rf32(proj + kProjFovy), rf32(proj + kProjAspect));
    }
    const int eye = vr::eye();
    // as the camera above: built aside, copied over in one go (the game's narrower projection never in the buffer)
    static std::mutex lock[2];
    std::lock_guard<std::mutex> l(lock[eye]);
    static uint32_t scratch[2];
    uint32_t& out = g_eye_projection[eye];
    if (!out) out = mem::sys_alloc(kProjSize, 16);
    if (!scratch[eye]) scratch[eye] = mem::sys_alloc(kProjSize, 16);
    memcpy(mem::host(scratch[eye]), mem::host(proj), kProjSize);
    apply_frustum(scratch[eye], current_pose().tangents[eye]);
    memcpy(mem::host(out), mem::host(scratch[eye]), kProjSize);
    c->r[3] = out;
}

// Rune aim: the game aims Magnesis, Stasis, Cryonis and Remote Bombs along its own camera (ActCamera finalCamMtx at
// +0x5c0: position, target), which stays level at the held heading while the headset shows the head's view (that pose
// is applied only when rendering), so looking down at water left the game looking ahead and Cryonis found none
// (device 2026-11-12; Fable 5.2: in VR the game camera stayed at pitch 0 with the head 30 degrees down). As BetterVR
// writes the head's pose into that camera (patch_CTRL_CameraControls.asm updateCameraPositionAndTarget,
// 0x02C054FC / 0x02C05590, hook_UpdateCameraForGameplay), the camera is put at the eyes looking where the head looks,
// at its own distance, while a rune is up (RuneMgr flags bit 4), except the Camera rune (flat, above). Walking keeps
// going where the head looks (Mac). BOTW_VR_RUNE_AIM=0: off
bool vr_eye_world(float out[3]);
bool vr_view_forward(float out[3]);
static void rune_aim_camera(PPCContext* c) {
    static const bool on = env_float("BOTW_VR_RUNE_AIM", 1.0f) != 0.0f;
    if (!on || !vr::enabled() || !view_now() || !first_person()) return;
    const uint32_t rm = r32(0x10463798);
    if (!rm || !(r32(rm + 0x4c) & 0x10) || r32(rm + 0x1d4) == 5) return;
    float eye[3], fwd[3];
    if (!vr_eye_world(eye) || !vr_view_forward(fwd)) return;
    const uint32_t m = c->r[31] + 0x5c0;  // finalCamMtx: position, target
    float d = 0;
    for (int i = 0; i < 3; i++) {
        const float v = rf32(m + 12 + 4 * i) - rf32(m + 4 * i);
        d += v * v;
    }
    d = sqrtf(d);
    if (!(d > 0.01f) || !(d < 1000.0f)) d = 5.0f;
    // nothing but finite numbers into the game's camera (its physics queries take a whole second on anything else)
    for (int i = 0; i < 3; i++)
        if (!std::isfinite(eye[i]) || !std::isfinite(fwd[i])) return;
    static int logged = 0;
    if (logged < 3) {
        logged++;
        LOG("vr: rune aim: game camera put at the eyes %.2f %.2f %.2f looking %.3f %.3f %.3f (%.2f m)", eye[0], eye[1], eye[2],
            fwd[0], fwd[1], fwd[2], d);
    }
    for (int i = 0; i < 3; i++) wf32(m + 4 * i, eye[i]), wf32(m + 12 + 4 * i, eye[i] + fwd[i] * d);
}

// Item 101's net: ActCamera's final camera (+0x5c0: position, target, up, fovy, ..., aspect, near, far; 14 floats),
// just copied from the active camera's (+0x550, "direct") or from the blend between two cameras (+0xeb4), is what the
// rest of the game takes the camera from (the shake, the physics queries, the render camera, the next camera's
// start). A camera that is not finite never gets there: the last finite one stays (and goes back over the source it
// was copied from), logged with what came. In every mode, VR or not; 14 compares a frame
static void final_camera(PPCContext* c, const char* path, uint32_t source) {
    const uint32_t cam = c->r[31], fin = cam + 0x5c0;
    static float good[14];
    static bool have_good = false;
    bool finite = true;
    for (int i = 0; i < 14; i++) finite = finite && std::isfinite(rf32(fin + 4 * i));
    if (finite) {
        for (int i = 0; i < 14; i++) good[i] = rf32(fin + 4 * i);
        have_good = true;
    } else {
        static int budget = 10;
        if (budget > 0) {
            budget--;
            const uint32_t player = vr::player_actor();
            LOG("camera: the game camera (%s) is not finite: at %f %f %f looking at %f %f %f up %f %f %f, fovy %f; player "
                "flags %08x; %s", path, rf32(fin), rf32(fin + 4), rf32(fin + 8), rf32(fin + 12), rf32(fin + 16),
                rf32(fin + 20), rf32(fin + 24), rf32(fin + 28), rf32(fin + 32), rf32(fin + 36),
                player ? r32(player + 0x8dc) : 0, have_good ? "the last finite one is kept" : "none to put back");
        }
        if (have_good)
            for (int i = 0; i < 14; i++) wf32(fin + 4 * i, good[i]), wf32(cam + source + 4 * i, good[i]);
    }
    rune_aim_camera(c);
}
BOTW_HOOK(0x02c054fc) { final_camera(c, "blend", 0xeb4); }   // stfs f0, 0x5CC(r31)
BOTW_HOOK(0x02c05590) { final_camera(c, "direct", 0x550); }  // stfs f13, 0x5C4(r31)

// The camera's two physics queries at its near plane (item 98; device 2026-11-13, twice: the game at 0.4 frames a
// second for as long as it lasted, every sample of the main thread inside one of them, Havok below): ActCamera's last
// step (0x02c07c84) sweeps a sphere the size of the near plane from the camera to where the camera shake puts it
// (0x02c087f4, so the shake does not push the view into a wall), and 0x02c07830 asks which actors that sphere
// touches, to hide those the camera stands in (0x02c079b4). Both serve the game's own picture: in the first-person
// view the picture is drawn from the head's pose, the game camera (behind Link, or at the eyes while a rune is up) is
// not what is seen, and hiding what that camera touches is wrong there anyway. So in the view both answer "nothing"
// without asking, as when nothing is near. BOTW_VR_CAMERA_QUERIES=1: asked as before; a query that then takes over
// 50 ms is logged with what it was asked
static bool skip_camera_queries() {
    static const bool keep = env_float("BOTW_VR_CAMERA_QUERIES", 0.0f) != 0.0f;
    if (keep || !vr::enabled() || !view_now() || !first_person()) return false;
    static bool logged = false;
    if (!logged) logged = true, LOG("vr: the game camera's near-plane physics queries are skipped in the first-person view");
    return true;
}
static void timed_camera_query(PPCContext* c, uint32_t fn, const char* what, uint32_t act_camera, const float* from,
                               const float* to) {
    const uint32_t cam = act_camera + 0x630;  // the camera after the shake: position, target, ..., fovy, aspect, near
    const auto t0 = std::chrono::steady_clock::now();
    rt_call(c, fn);
    const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    static int budget = 20;
    if (ms > 50 && budget > 0) {
        budget--;
        LOG("vr: slow camera query (%s) %.0f ms: from %.2f %.2f %.2f to %.2f %.2f %.2f, camera at %.2f %.2f %.2f looking at "
            "%.2f %.2f %.2f, fovy %.3f aspect %.3f near %.3f",
            what, ms, from[0], from[1], from[2], to[0], to[1], to[2], rf32(cam), rf32(cam + 4), rf32(cam + 8), rf32(cam + 12),
            rf32(cam + 16), rf32(cam + 20), rf32(cam + 36), rf32(cam + 44), rf32(cam + 48));
    }
}
BOTW_SITE(0x02c087f4) {  // bctrl: the sweep (r3 = the query at r1 + 440, r4 = 1); 0: nothing hit
    if (skip_camera_queries()) {
        c->r[3] = 0;
        return;
    }
    const float from[3] = {rf32(c->r[1] + 224), rf32(c->r[1] + 228), rf32(c->r[1] + 232)};
    const float to[3] = {rf32(c->r[1] + 320), rf32(c->r[1] + 324), rf32(c->r[1] + 328)};
    timed_camera_query(c, c->ctr, "shake sweep", c->r[31], from, to);
}
BOTW_SITE(0x02c079b4) {  // bl 0x034b475c: the overlap (r3 = the query at r1 + 172, r4 = 0); 0: nothing touched
    if (skip_camera_queries()) {
        c->r[3] = 0;
        return;
    }
    const float at[3] = {rf32(c->r[1] + 104), rf32(c->r[1] + 108), rf32(c->r[1] + 112)};
    timed_camera_query(c, 0x034b475c, "actors at the camera", c->r[29], at, at);
}

// The third of the camera's physics queries (item 98 again, device 2026-11-13, with the two above already skipped: on
// top of a Cryonis pillar the game at 0.4 frames a second, all three samples of the main thread in this one): the
// chase camera (0x02b9b8a0, its mode byte +483 = 2) tilts with the ground under Link, and asks for the slope along
// its heading once a frame (0x02c10ad8 -> 0x02c10a80 -> 0x02c108d8, kept at +0x74c): 0x02e5e38c takes the heading as
// a direction (sin, 0, cos) and casts rays round Link (0x02e5d40c: up, down, then 0x02e5d244: 2 m along the heading
// and straight down from there, layers ground / ground object / climbable wall). It was the last of those, the ray
// straight down beside the pillar, that took seconds inside one static mesh (hkpWorld::castRay ->
// hkpStaticCompoundShape -> hkpBvCompressedMeshShape, leaf after leaf). The slope only pitches the game's own
// camera; the first-person picture is drawn from the head's pose, so in the view the answer is 0 (level ground, what
// the function answers itself when Link is not found) without asking. The game checks Link's place for NaN before
// these rays (0x037d1604) but not the heading: a heading that is not a number answers 0 in any view, and is logged.
// BOTW_VR_CAMERA_QUERIES=1: asked as before, over 50 ms logged
BOTW_SITE(0x02c10a44) {  // bl 0x02e5e38c: r3 = the direction (3 floats); f1: the slope
    const float dir[3] = {rf32(c->r[3]), rf32(c->r[3] + 4), rf32(c->r[3] + 8)};
    if (!std::isfinite(dir[0]) || !std::isfinite(dir[1]) || !std::isfinite(dir[2])) {
        static std::atomic<int> budget{10};
        if (budget.fetch_sub(1, std::memory_order_relaxed) > 0)
            LOG("vr: camera slope query not asked: its direction is not finite (%f %f %f)", dir[0], dir[1], dir[2]);
        c->f[1] = 0.0;
        return;
    }
    if (skip_camera_queries()) {
        c->f[1] = 0.0;
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    rt_call(c, 0x02e5e38c);
    const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    static std::atomic<int> budget{20};
    if (ms > 50 && budget.fetch_sub(1, std::memory_order_relaxed) > 0)
        LOG("vr: slow camera query (ground slope) %.0f ms: along %.3f %.3f %.3f, answer %.3f", ms, dir[0], dir[1], dir[2],
            (float)c->f[1]);
}

// Every ray the game casts into the physics world goes through 0x034cac70, which calls hkpWorld::castRay
// (0x04095f60: r3 = the world, r4 = the ray, from at +0 and to at +16, r5 = the collector) at three places. Why the
// rays above took seconds was never seen (only where the main thread was): so each call is timed here, and one over
// 50 ms is logged with its two ends and the caller. A ray with an end that is not a finite number cannot hit anything
// and is the one input known to send a tree walk everywhere: it is not cast (the collector stays empty: no hit) and
// logged (2026-11-13). The clock costs two reads a ray. BOTW_RAY_WATCH=0: cast as the game does, nothing timed
static void watched_ray(PPCContext* c, uint32_t site) {
    static const bool on = env_float("BOTW_RAY_WATCH", 1.0f) != 0.0f;
    if (!on) {
        rt_call(c, 0x04095f60);
        return;
    }
    const uint32_t ray = c->r[4];
    float e[6];
    bool finite = true;
    for (int i = 0; i < 3; i++) {
        e[i] = rf32(ray + 4 * i), e[3 + i] = rf32(ray + 16 + 4 * i);
        finite = finite && std::isfinite(e[i]) && std::isfinite(e[3 + i]);
    }
    if (!finite) {
        static std::atomic<int> budget{20};
        if (budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
            // and who asked: the return addresses up the guest stack (the back chain at 0(r1), a function's own
            // return address at 4 of its caller's frame)
            char who[128] = "";
            size_t n = 0;
            uint32_t sp = c->r[1];
            for (int k = 0; k < 10 && sp && n + 10 < sizeof who; k++) {
                const uint32_t up = r32(sp);
                if (up <= sp || up - sp > 0x10000) break;
                n += snprintf(who + n, sizeof who - n, " %08x", r32(up + 4));
                sp = up;
            }
            LOG("physics: ray not cast, an end is not finite: from %f %f %f to %f %f %f (site %08x, callers%s)", e[0], e[1],
                e[2], e[3], e[4], e[5], site, who);
        }
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    rt_call(c, 0x04095f60);
    const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (ms > 50) {
        static std::atomic<int> budget{20};
        if (budget.fetch_sub(1, std::memory_order_relaxed) > 0)
            LOG("physics: slow ray %.0f ms: from %.3f %.3f %.3f to %.3f %.3f %.3f (site %08x)", ms, e[0], e[1], e[2], e[3],
                e[4], e[5], site);
    }
}
BOTW_SITE(0x034cadd4) { watched_ray(c, 0x034cadd4); }  // bl 0x04095f60
BOTW_SITE(0x034cae84) { watched_ray(c, 0x034cae84); }  // bl 0x04095f60
BOTW_SITE(0x034caee4) { watched_ray(c, 0x034caee4); }  // bl 0x04095f60

// The rune target test (Magnesis, Stasis; BetterVR patch_CTRL_FixScreenChecks.asm + camera.cpp
// hook_ModifyPixelUniformBlockData): while a rune is up the game draws, in each pass, a test that samples the picture at
// a screen position and reads the result back (64x4 textures); what lies there becomes the target. The position comes in
// a pixel uniform block of 32 bytes, (0.5, 0.5, ...), already in the GPU's byte order (little endian). An eye's picture
// is not centred on where the eye looks (its field is uneven), so the test fell beside the gaze, on a different side in
// each pass: a target was found beside the crosshair, not under it, and came and went (device 2026-11-13). The position
// becomes where the gaze falls in this pass's picture: across -left / width, down top / height. A copy goes to the GPU
// (a ring of our own; the game's data stays). BOTW_VR_AIM=0: off
// mr r4, r25 just before bl GX2SetPixelUniformBlock at 0x038b2b2c: r4 = size, r5 = data
BOTW_HOOK(0x038b2b24) {
    static const bool on = env_float("BOTW_VR_AIM", 1.0f) != 0.0f;
    static const bool flip = env_float("BOTW_VR_AIM_FLIP", 0.0f) != 0.0f;
    if (!on || !vr::enabled() || !view_now() || c->r[4] != 0x20) return;
    const uint32_t data = c->r[5];
    if (!data || r32(data) != 0x0000003fu || r32(data + 4) != 0x0000003fu) return;  // (0.5, 0.5), little endian
    const float* t = current_pose().tangents[vr::eye()];  // left, right, top, bottom
    const float w = t[1] - t[0], h = t[2] - t[3];
    if (!(w > 0) || !(h > 0)) return;
    const float x = -t[0] / w, y = flip ? -t[3] / h : t[2] / h;
    static uint32_t ring = 0;
    static unsigned next = 0;
    constexpr unsigned kSlots = 256;  // the GPU reads the block later; 256 calls is many frames
    if (!ring) ring = mem::sys_alloc(0x20 * kSlots, 256);
    const uint32_t copy = ring + 0x20 * (next++ % kSlots);
    memcpy(mem::host(copy), mem::host(data), 0x20);
    memcpy(mem::host(copy), &x, 4);  // host floats are little endian
    memcpy(mem::host(copy) + 4, &y, 4);
    c->r[5] = copy;
    static int logged = 0;
    if (logged < 4) logged++, LOG("vr: rune target test at (%.3f, %.3f) of the picture instead of its middle, eye %d", x, y, vr::eye());
}

// sead::Projection::getProjectionMatrix called by the light pre-pass (return address 0x033dbbb8): that projection
// must match the eye's, or the lighting lands beside the geometry (BetterVR custom_sead__Projection__getProjectionMatrix
// + hook_ModifyLightPrePassProjectionMatrix, which change it in place)
BOTW_REPLACE(0x030c1008) {
    const uint32_t proj = c->r[3];
    if (view_now() && proj && c->lr == 0x033dbbb8 && r32(proj + kProjVtable) == kPerspectiveVtable) {
        apply_frustum(proj, current_pose().tangents[vr::eye()]);
    }
    orig_0x030c1008(c);
}

// The HUD layer's own buffer. The frame's 2D render buffer is RGB10A2, 2 bits of alpha (0, 1/3, 2/3, 1): alone in the
// layer laid over the picture by its alpha, the HUD's soft dark backdrops and glows came out as grey steps (item 92).
// In the headset's view the HUD is drawn into memory of our own as R8G8B8A8 instead (same 32 bits a pixel, so the same
// size, pitch and tiling): a marker in the command stream (VR_HUD_BEGIN, after the 3D image is taken) has the
// renderer put that memory in place of the frame buffer's wherever a render target points at it, until the HUD is
// taken (VR_HUD_TARGET; runtime/cemu/Cafe/HW/Latte/Core/LatteRenderTarget.cpp). The game's own GX2ColorBuffers can't
// simply be repointed: its 2D layers restore the render target from saved context states (GX2SetContextState), and
// making the buffer itself RGBA16 in the renderer broke the game's R32F (0x80e) use of the same memory, which then no
// longer shared one texture with it (Mac 2026-11-12). BOTW_VR_HUD_RGBA8=0 keeps the game's buffer
extern "C" std::atomic<uint32_t> g_botw_vr_hud_image;  // LatteRenderTarget.cpp: our memory, read by the renderer
namespace hud_buffer {
uint32_t g_cb = 0;  // a GX2ColorBuffer of our own on that memory, to clear it
bool enabled() {
    static const bool on = [] {
        const char* v = getenv("BOTW_VR_HUD_RGBA8");
        return !(v && !strcmp(v, "0"));
    }();
    return on;
}
// game: the frame buffer's GX2ColorBuffer; true when the HUD goes to our memory, cleared, this frame
bool begin(PPCContext* c, uint32_t game) {
    if (!enabled() || r32(game + 0x14) != 0x19 || r32(game + 0x10) != 1 || r32(game + 0x18) != 0) return false;  // RGB10A2, 1 mip, no AA
    static uint32_t image = 0, image_size = 0;
    const uint32_t size = r32(game + 0x20);
    if (size > image_size) {
        if (image) return false;  // a larger frame buffer than at first: not expected, the game's own then
        image = mem::sys_alloc(size, std::max<uint32_t>(r32(game + 0x38), 0x1000)), image_size = size;
        g_cb = mem::sys_alloc(0x9c, 16);
        LOG("vr: HUD layer drawn into memory of its own, %ux%u RGBA8 at %08x (%u KB)", r32(game + 4), r32(game + 8), image, size / 1024);
    }
    if (!image) return false;
    memcpy(mem::host(g_cb), mem::host(game), 0x9c);
    mem::w32(g_cb + 0x14, 0x1a);  // R8_G8_B8_A8_UNORM
    mem::w32(g_cb + 0x24, image);
    guest_call(c, dispatch::export_thunk("gx2", "GX2InitColorBufferRegs"), {g_cb});
    g_botw_vr_hud_image.store(image, std::memory_order_relaxed);
    c->f[1] = c->f[2] = c->f[3] = c->f[4] = 0.0;
    guest_call(c, dispatch::export_thunk("gx2", "GX2ClearColor"), {g_cb});
    guest_call(c, dispatch::export_thunk("gx2", "GX2CopyColorBufferToScanBuffer"), {game, VR_HUD_BEGIN});
    return true;
}
}  // namespace hud_buffer

BOTW_SITE(0x039b3044) {
    if (vr::enabled()) {
        uint32_t color = r32(c->r[26] + 0x1c);  // agl::RenderBuffer::mColorBuffer[0]
        if (color) {
            // view images carry their pose (low 8 bits of its id) and a view mark (bit 8); screen images neither
            uint32_t tag = view_now() ? (0x100 | (g_frame_pose_id & 0xff)) << 16 : 0;
            guest_call(c, dispatch::export_thunk("gx2", "GX2CopyColorBufferToScanBuffer"),
                       {color + 0xbc, VR_EYE_TARGET | (uint32_t)vr::eye() | tag});  // its GX2ColorBuffer
            // and the scene's depth (agl::RenderBuffer::mDepthTarget, BetterVR patch_RND_Find3DFrameBuffer.asm): the
            // compositor reprojects each pixel by its own distance (a single depth for the whole picture made the
            // world shake as the head turned, near or far)
            if (const uint32_t depth = view_now() ? r32(c->r[26] + 0x3c) : 0)
                guest_call(c, dispatch::export_thunk("gx2", "GX2CopyColorBufferToScanBuffer"),
                           {depth + 0xbc, VR_DEPTH_TARGET | (uint32_t)vr::eye()});  // its GX2DepthBuffer
            // then clear it to transparent: the HUD the game draws next is left alone in it (a layer of its own, with
            // alpha; BetterVR's Vulkan layer clears it the same way after taking the 3D image). Only in the headset's
            // view: with a menu or an event on the screen the game's 2D goes over its picture as it would on a TV, or
            // the inventory's opaque backdrop, alone in the layer laid over the picture, hid Link standing in it but
            // for his legs under the item panel (device and Mac, 2026-11-02)
            if (view_now()) {
                c->f[1] = c->f[2] = c->f[3] = c->f[4] = 0.0;
                guest_call(c, dispatch::export_thunk("gx2", "GX2ClearColor"), {color + 0xbc});
                hud_buffer::begin(c, color + 0xbc);  // the HUD into memory of its own (above)
            }
        }
    }
    for (int k = 18; k < 32; k++) c->r[k] = r32(c->r[1] + 176 + 4 * (k - 18));  // lmw r18, 176(r1)
}

// HUD layer: mr r5, r30 where the UI has been composed into the frame's 2D render buffer (r30 = its agl::RenderBuffer;
// BetterVR patch_RND_Find2DFrameBuffer.asm clear2DColorBuffer). Its colour buffer, holding only the HUD over the
// transparent clear above, goes to the renderer with target VR_HUD_TARGET.
BOTW_HOOK(0x03a147b4) {
    if (!vr::enabled() || !c->r[30]) return;
    uint32_t color = r32(c->r[30] + 0x1c);
    if (!color) return;
    guest_call(c, dispatch::export_thunk("gx2", "GX2CopyColorBufferToScanBuffer"), {color + 0xbc, VR_HUD_TARGET});
}
