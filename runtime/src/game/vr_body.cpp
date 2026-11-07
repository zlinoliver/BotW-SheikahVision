// VR first person: the player's own head is not drawn (it sits around the eyes). The game reads each bone's local
// matrix through a model unit's virtual getBoneLocalMatrix (the vtable +0x64 call loaded at 0x03820c3c; r3 the model
// unit, r4 the matrix out, r5 the scale out, r6 the bone index, r12 the bone key with its name at +4, and the caller's
// r31 the ksys::phys::ModelBoneAccessor, its gsys model at +0x10). In first person the call goes through a wrapper that
// runs it and then scales the head's bones to zero for the player's model and its head armour (hair, helmets).
// BetterVR hooks the same call (patch_CTRL_Bones.asm custom_gsys_ModelUnit_getBoneLocalMatrix, skeleton.cpp
// hook_ModifyBoneMatrix). The face does not follow its bones; BetterVR makes its materials (3..7) transparent with
// gsys::ModelUnit::setXluAlpha from this callback, but done here that blacked out the whole scene's lighting (tried
// 2026-10-31), so the face is left to the first-person near plane (vr_render.cpp).
//
// The same callback hands over the player's animated head: the local matrices of Skl_Root, Spine_1, Spine_2, Neck and
// Head (the chain in BetterVR skeleton.cpp SKELETON_DATA) multiply into the head in the model's space, and the point
// between the eyeballs (Eyeball_L / R under Face_Root, at 0.07017, 0.12036, +-0.04815 in the head's frame) is where
// first person puts the eyes (vr_player_eyes, used by vr_render.cpp): in the head whatever the pose (climbing,
// swimming, riding), where a fixed height above the feet is not.
//
// Motion controls, first part: with the PS VR2 Sense controllers tracked, the arms reach for them (BetterVR
// skeleton.cpp hook_ModifyBoneMatrix: a two-bone IK from the shoulder, the wrist turned to the controller with a fixed
// correction per hand, and the weapon bone landing on the grip). BetterVR swaps the whole first-person body for a
// still bind pose turned with the headset; here the body keeps its animation (the eyes come from it) and only the arm
// chains (Arm_1 -> Arm_2 -> Wrist, under the animated Clavicle) are solved. The controller is placed relative to the
// eyes of the previous skeleton pass, turned to the view's heading, so hands and eyes stay as they are in the room.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <string_view>

#include "hooks.h"
#include "vr.h"

using mem::r32;

extern "C" int botw_vr_view_now(void);  // view mode and no event running (vr_render.cpp)
bool vr_first_person();  // vr_render.cpp
bool vr_hand_in_base(int hand, float out[12]);  // vr_render.cpp
bool vr_hand_aim_local(int hand, float out[3]);  // vr_render.cpp
bool vr_view_base_world(float out[3]);          // vr_render.cpp: where the first-person view stands
bool vr_head_offset_world(float out[3]);        // vr_render.cpp: the head's level offset from it
bool vr_two_hand();                             // vr_gesture.cpp
bool vr_bow_trigger();                          // vr_gesture.cpp: the right trigger (ZR) held
const char* vr_actor_name(uint32_t actor);      // vr_hands.cpp
uint32_t vr_hand_item(int hand);                // vr_hands.cpp
uint32_t vr_player_bow();                       // vr_weapon.cpp
void vr_bow_flight_update(uint32_t bow);        // vr_bow.cpp
float vr_head_pitch_deg();                      // vr_render.cpp
float vr_turn_last_heading();                   // vr_turn.cpp
bool vr_throw_launch_point(float out[3]);       // vr_aim.cpp: where a throw leaving the right hand goes from

bool vr_head_in_room(float out[3], uint32_t* recentres);  // vr_render.cpp
bool vr_eye_world(float out[3]);                          // vr_render.cpp

namespace {
thread_local uint32_t t_bone_fn;  // the model unit's own getBoneLocalMatrix, for the wrapper called right after

// row-major 3x4 matrices (sead::Matrix34f) as 12 floats
struct M34 {
    float m[12];
};
M34 mul34(const M34& a, const M34& b) {
    M34 x;
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 4; c++) {
            float v = a.m[r * 4 + 0] * b.m[0 * 4 + c] + a.m[r * 4 + 1] * b.m[1 * 4 + c] + a.m[r * 4 + 2] * b.m[2 * 4 + c];
            x.m[r * 4 + c] = v + (c == 3 ? a.m[r * 4 + 3] : 0);
        }
    }
    return x;
}
M34 read34(uint32_t addr) {
    M34 x;
    for (int i = 0; i < 12; i++) x.m[i] = mem::rf32(addr + 4 * i);
    return x;
}
void write34(uint32_t addr, const M34& x) {
    for (int i = 0; i < 12; i++) mem::wf32(addr + 4 * i, x.m[i]);
}
M34 ident34() {
    M34 x{};
    x.m[0] = x.m[5] = x.m[10] = 1;
    return x;
}
M34 inv34(const M34& a) {  // rotation + translation
    M34 x;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) x.m[r * 4 + c] = a.m[c * 4 + r];
    for (int r = 0; r < 3; r++)
        x.m[r * 4 + 3] = -(x.m[r * 4] * a.m[3] + x.m[r * 4 + 1] * a.m[7] + x.m[r * 4 + 2] * a.m[11]);
    return x;
}
M34 rot34(float ax, float ay, float az, float deg) {  // a turn about a unit axis
    const float a = deg * (float)M_PI / 180, c = cosf(a), s = sinf(a), t = 1 - c;
    M34 x{};
    x.m[0] = t * ax * ax + c, x.m[1] = t * ax * ay - s * az, x.m[2] = t * ax * az + s * ay;
    x.m[4] = t * ax * ay + s * az, x.m[5] = t * ay * ay + c, x.m[6] = t * ay * az - s * ax;
    x.m[8] = t * ax * az - s * ay, x.m[9] = t * ay * az + s * ax, x.m[10] = t * az * az + c;
    return x;
}
M34 trans34(float x, float y, float z) {
    M34 m = ident34();
    m.m[3] = x, m.m[7] = y, m.m[11] = z;
    return m;
}
struct V3 {
    float x, y, z;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 norm(V3 a) {
    const float l = sqrtf(dot(a, a));
    return l > 1e-6f ? a * (1 / l) : V3{0, 0, 0};
}
V3 pos34(const M34& a) { return {a.m[3], a.m[7], a.m[11]}; }
V3 apply34(const M34& a, V3 p) {
    return {a.m[0] * p.x + a.m[1] * p.y + a.m[2] * p.z + a.m[3], a.m[4] * p.x + a.m[5] * p.y + a.m[6] * p.z + a.m[7],
            a.m[8] * p.x + a.m[9] * p.y + a.m[10] * p.z + a.m[11]};
}
M34 from_axes(V3 x, V3 y, V3 z, V3 p) {  // columns
    M34 m;
    const V3 col[4] = {x, y, z, p};
    for (int c = 0; c < 4; c++) m.m[c] = col[c].x, m.m[4 + c] = col[c].y, m.m[8 + c] = col[c].z;
    return m;
}

// the player's head chain, collected bone by bone in the callback's order (parents first)
const char* const kChain[] = {"Skl_Root", "Spine_1", "Spine_2", "Neck", "Head"};
constexpr int kChainLen = 5;
thread_local M34 t_chain[kChainLen];
thread_local unsigned t_chain_seen;
std::mutex g_eyes_lock;
float g_eyes_model[3];  // the eyes in the player model's space
bool g_have_eyes;
// BetterVR skeleton.cpp isFaceBone: the head and what hangs from it
bool head_bone(std::string_view b) {
    for (const char* p : {"Eye", "Cheek", "Lip", "Hair", "Ponytail", "Momi", "Teeth_", "Chin"})
        if (b.starts_with(p)) return true;
    // Neck too, as BetterVR: the head and its hair hang from it, and shrunk with it the head stays hidden when an
    // animation leans it forward past the eyes (device 2026-11-02: the hair and ears in view while jumping or drawing a
    // weapon)
    return b == "Nose" || b == "Face_Root" || b == "Head" || b == "Neck";
}

const char* model_name(uint32_t accessor) {
    if (!accessor) return nullptr;
    uint32_t model = r32(accessor + 0x10);
    return model ? mem::str(r32(model + 0x128)) : nullptr;  // gsys model name (sead::SafeString's c_str)
}

void collect_head(const char* bone, uint32_t matrix) {
    int i = 0;
    while (i < kChainLen && strcmp(bone, kChain[i]) != 0) i++;
    if (i == kChainLen) return;
    if (i == 0) t_chain_seen = 0;  // a new pass over the skeleton
    t_chain[i] = read34(matrix);
    t_chain_seen |= 1u << i;
    if (i != kChainLen - 1 || t_chain_seen != (1u << kChainLen) - 1) return;
    M34 head = t_chain[0];
    for (int k = 1; k < kChainLen; k++) head = mul34(head, t_chain[k]);
    const float e[3] = {0.07017f, 0.12036f, 0.0f};  // between the eyeballs, in the head's frame
    float eyes[3];
    for (int r = 0; r < 3; r++) eyes[r] = head.m[r * 4] * e[0] + head.m[r * 4 + 1] * e[1] + head.m[r * 4 + 2] * e[2] + head.m[r * 4 + 3];
    std::lock_guard<std::mutex> l(g_eyes_lock);
    memcpy(g_eyes_model, eyes, sizeof eyes);
    if (!g_have_eyes) LOG("vr: player's eyes from the skeleton, %.2f m above the feet", eyes[1]);
    g_have_eyes = true;
}

// ---- arms ---------------------------------------------------------------------------------------------------------
// Each arm's chain in the model's space, built bone by bone in the callback's order (parents first). Indices: 0
// Skl_Root, 1 Spine_1, 2 Spine_2 (shared), then per hand Clavicle, Arm_1, Arm_2.
thread_local M34 t_spine[3];
thread_local M34 t_arm[2][3];   // Clavicle, Arm_1, Arm_2 in model space (Arm_1 / Arm_2 after the solve)
thread_local M34 t_target[2];   // the wrist's target in model space, for this pass
thread_local bool t_reach[2];   // this pass solves that arm
thread_local M34 t_body;        // the turn (about +y) from the model's +z to where the body faces after reach()
constexpr float kUpper = 0.24f, kFore = 0.27718f;  // Arm_2 and Wrist offsets (BetterVR SKELETON_DATA)

// BetterVR's wrist corrections: from the controller's grip to the game's wrist bone (left: 90 about y, -90 then 30
// about z; right: -90 about z, -180 about y, 270 about x, 30 about z), then back from the weapon bone to the wrist
// (Weapon_L / Weapon_R offsets) so the weapon sits in the hand where the grip is
const M34& grip_to_wrist(int hand) {
    static const M34 m[2] = {
        mul34(mul34(mul34(rot34(0, 1, 0, 90), rot34(0, 0, 1, -90)), rot34(0, 0, 1, 30)), trans34(-0.1069f, -0.00002f, -0.02769f)),
        mul34(mul34(mul34(mul34(rot34(0, 0, 1, -90), rot34(0, 1, 0, -180)), rot34(1, 0, 0, 270)), rot34(0, 0, 1, 30)),
              trans34(0.1069f, 0.00002f, 0.02769f)),
    };
    return m[hand];
}

// The controller's wrist target in the model's space: model <- world (the actor) <- view base (the eyes, turned to the
// view's heading) <- grip
// The view's heading in the model's frame (the turn about +y from the model's +z, where the body faces)
float view_yaw_in_model() {
    M34 actor = read34(vr::player_actor() + 0x1f8);
    actor.m[3] = actor.m[7] = actor.m[11] = 0;
    const M34 r = mul34(inv34(actor), rot34(0, 1, 0, vr_turn_last_heading() * 180 / (float)M_PI));
    return atan2f(-r.m[2], -r.m[10]);  // the view looks along -z: its direction in the model
}

// The view's base (vr_render.cpp) in the model's space, else the skeleton's eyes
bool base_in_model(float eyes[3]) {
    float w[3];
    if (vr_view_base_world(w)) {
        const V3 m = apply34(inv34(read34(vr::player_actor() + 0x1f8)), {w[0], w[1], w[2]});
        eyes[0] = m.x, eyes[1] = m.y, eyes[2] = m.z;
        return true;
    }
    std::lock_guard<std::mutex> l(g_eyes_lock);
    if (!g_have_eyes) return false;
    memcpy(eyes, g_eyes_model, sizeof g_eyes_model);
    return true;
}

bool hand_target(int hand, M34& out) {
    float g[12];
    if (!vr::player_actor() || !vr_hand_in_base(hand, g)) return false;
    float eyes[3];
    if (!base_in_model(eyes)) return false;
    M34 grip;
    memcpy(grip.m, g, sizeof g);
    M34 base = rot34(0, 1, 0, view_yaw_in_model() * 180 / (float)M_PI + 180);  // -z ahead -> the view's heading
    base.m[3] = eyes[0], base.m[7] = eyes[1], base.m[11] = eyes[2];
    out = mul34(mul34(base, grip), grip_to_wrist(hand));
    // BOTW_VR_LOG_HANDS=1: the numbers behind "the game's hands are not where the real ones are" (device 2026-11-08)
    static const bool log_hands = getenv("BOTW_VR_LOG_HANDS") != nullptr;
    static int every[2];
    if (log_hands && ++every[hand] % 120 == 1) {
        float head[3] = {0, 0, 0};
        uint32_t recentres = 0;
        vr_head_in_room(head, &recentres);
        const M34 actor = read34(vr::player_actor() + 0x1f8);
        const V3 wrist = pos34(out), world = pos34(mul34(actor, out));
        float eye[3] = {0, 0, 0};
        const bool have_eye = vr_eye_world(eye);
        LOG("vr hands %s: grip in room %.3f %.3f %.3f, head in room %.3f %.3f %.3f, grip from head %.3f %.3f %.3f; view yaw in model %.1f deg; "
            "eyes in model %.3f %.3f %.3f, wrist in model %.3f %.3f %.3f; wrist from eye (world) %.3f %.3f %.3f (eye %d), heading %.1f deg",
            hand ? "right" : "left", g[3], g[7], g[11], head[0], head[1], head[2], g[3] - head[0], g[7] - head[1], g[11] - head[2],
            view_yaw_in_model() * 180 / (float)M_PI, eyes[0], eyes[1], eyes[2], wrist.x, wrist.y, wrist.z, world.x - eye[0], world.y - eye[1],
            world.z - eye[2], have_eye ? 1 : 0, vr_turn_last_heading() * 180 / (float)M_PI);
    }
    return true;
}

}  // namespace

// BotWImmersive.swift: the bow drawn (a bow in the left hand, the right trigger held), for the aiming line it draws
std::atomic<bool> g_aiming{false};
extern "C" int botw_vr_bow_aiming(void) { return g_aiming.load(std::memory_order_relaxed) ? 1 : 0; }

// vr_bow.cpp: a controller's grip in the world (the same chain as the arms: the actor <- the view base at the eyes <-
// the grip): its position and where it points (-z); false while untracked or before the first skeleton pass
// the grip's world matrix (row-major 3x4), the same chain as the arms
bool vr_hand_world_matrix(int hand, float out[12]) {
    float g[12];
    const uint32_t player = vr::player_actor();
    if (!player || !vr_hand_in_base(hand, g)) return false;
    float eyes[3];
    if (!base_in_model(eyes)) return false;
    M34 grip;
    memcpy(grip.m, g, sizeof g);
    M34 base = rot34(0, 1, 0, view_yaw_in_model() * 180 / (float)M_PI + 180);
    base.m[3] = eyes[0], base.m[7] = eyes[1], base.m[11] = eyes[2];
    const M34 world = mul34(read34(player + 0x1f8), mul34(base, grip));
    memcpy(out, world.m, sizeof world.m);
    return true;
}
bool vr_hand_world(int hand, float pos[3], float forward[3]) {
    float w[12];
    if (!vr_hand_world_matrix(hand, w)) return false;
    // forward: where the controller points (its aim space), else the grip's -z
    float aim[3] = {0, 0, -1};
    vr_hand_aim_local(hand, aim);
    for (int r = 0; r < 3; r++)
        pos[r] = w[r * 4 + 3], forward[r] = w[r * 4 + 0] * aim[0] + w[r * 4 + 1] * aim[1] + w[r * 4 + 2] * aim[2];
    return true;
}

namespace {
// BetterVR Skeleton::SolveTwoBoneIK for one arm, at its Arm_1: the upper arm from the shoulder towards the target,
// bent at the elbow in the plane through a pole below, behind and outside, stretched (mostly the upper arm) when the
// target is out of reach. Returns Arm_1's new local matrix and leaves Arm_1 / Arm_2 in t_arm.
M34 solve_arm(int hand, const M34& arm1_local) {
    const M34& clavicle = t_arm[hand][0];
    const V3 root = pos34(mul34(clavicle, arm1_local)), target = pos34(t_target[hand]);
    const float sign = hand == 0 ? 1.0f : -1.0f;
    // the elbow's pole (outside, below, behind) in the body's frame: turned with the body (BetterVR skeleton.cpp
    // rotates poleDir by Skl_Root's rotation). Fixed in the model's frame, as until 2026-11-04, it pointed across
    // the chest whenever the body stood turned from the view (after walking backwards or sideways the body keeps
    // facing that way; the hands' targets were right, the elbows swung inwards: "the arms crossed")
    const V3 pole = apply34(t_body, hand == 0 ? V3{1, -1, -0.5f} : V3{-1, -1, -0.5f});
    float l1 = kUpper, l2 = kFore;
    const V3 dir = target - root;
    float dist = sqrtf(dot(dir, dir));
    const float eps = 0.001f;
    if (dist > l1 + l2 - eps) {
        const float extra = dist - (l1 + l2) + eps;
        l1 += extra * 0.75f, l2 += extra * 0.25f;
    }
    dist = std::fmin(std::fmax(dist, eps), l1 + l2 - eps);
    const float alpha = acosf(std::fmin(std::fmax((l1 * l1 + dist * dist - l2 * l2) / (2 * l1 * dist), -1.0f), 1.0f));
    const V3 d = norm(dir), plane = norm(cross(d, pole)), ortho = norm(cross(plane, d));
    const V3 upper = norm(d * cosf(alpha) + ortho * sinf(alpha));
    const V3 fore = norm(target - (root + upper * l1));
    auto frame = [&](V3 along, V3 p) {  // x along the bone (its sign), z the plane's normal: (x, -y, -z)
        const V3 x = along * sign, z = plane, y = cross(z, x);
        return from_axes(x, y * -1, z * -1, p);
    };
    static bool logged[2];
    if (!logged[hand]) {
        logged[hand] = true;
        LOG("vr: %s arm reaches for its controller (shoulder %.2f %.2f %.2f, hand %.2f %.2f %.2f in the model)",
            hand ? "right" : "left", root.x, root.y, root.z, target.x, target.y, target.z);
    }
    M34 arm1 = frame(upper, root);
    M34 local1 = mul34(inv34(clavicle), arm1);
    local1.m[3] = arm1_local.m[3], local1.m[7] = arm1_local.m[7], local1.m[11] = arm1_local.m[11];
    t_arm[hand][1] = mul34(clavicle, local1);
    t_arm[hand][2] = frame(fore, root + upper * l1);  // Arm_2 in model space (its local is set at its own call)
    return local1;
}

// Two-handed grip (vr_gesture.cpp latches it): the right wrist turned so the blade points along the line from the right
// hand through the left (BetterVR skeleton.cpp: the blade axis measured in the hand, then the minimal turn onto the grip
// axis). The blade's direction in the wrist's frame is measured while the weapon is held with one hand, from the
// weapon actor's matrix (its z axis) and the wrist target of the frame before, smoothed.
V3 g_blade_in_wrist;
bool g_have_blade = false;
uint32_t g_blade_weapon = 0;
M34 g_last_right;  // the right wrist's target of the last pass (model space)
bool g_have_last_right = false;

V3 col(const M34& m, int c) { return {m.m[c], m.m[4 + c], m.m[8 + c]}; }
V3 rotate(const M34& m, V3 v) { return {m.m[0] * v.x + m.m[1] * v.y + m.m[2] * v.z, m.m[4] * v.x + m.m[5] * v.y + m.m[6] * v.z,
                                        m.m[8] * v.x + m.m[9] * v.y + m.m[10] * v.z}; }
V3 unrotate(const M34& m, V3 v) { return {dot(col(m, 0), v), dot(col(m, 1), v), dot(col(m, 2), v)}; }

void two_hand(bool left_tracked) {
    const uint32_t weapon = vr_hand_item(1), player = vr::player_actor();
    if (weapon != g_blade_weapon) g_blade_weapon = weapon, g_have_blade = false;
    if (weapon && player && g_have_last_right && !vr_two_hand()) {
        M34 actor = read34(player + 0x1f8);
        const V3 blade_world = norm(col(read34(weapon + 0x1f8), 2));
        const V3 blade_model = unrotate(actor, blade_world);
        const V3 measured = norm(unrotate(g_last_right, blade_model));
        if (dot(measured, measured) > 0.5f)
            g_blade_in_wrist = g_have_blade ? norm(g_blade_in_wrist * 0.85f + measured * 0.15f) : measured, g_have_blade = true;
    }
    if (vr_two_hand() && left_tracked && g_have_blade) {
        M34& t = t_target[1];
        const V3 axis = norm(pos34(t_target[0]) - pos34(t));
        V3 cur = norm(rotate(t, g_blade_in_wrist));
        const V3 want = dot(cur, axis) >= 0 ? axis : axis * -1.0f;
        const V3 ax = cross(cur, want);
        const float s = sqrtf(dot(ax, ax)), c = dot(cur, want);
        if (s > 1e-5f) {
            const V3 n = ax * (1 / s);
            const M34 turn = rot34(n.x, n.y, n.z, atan2f(s, c) * 180 / (float)M_PI);
            const V3 p = pos34(t);
            t = mul34(turn, t);
            t.m[3] = p.x, t.m[7] = p.y, t.m[11] = p.z;
        }
    }
    g_last_right = t_target[1], g_have_last_right = true;
}

// ---- the bow ----------------------------------------------------------------------------------------------------
// Drawing is the game's (the right trigger is ZR); the right hand stays where its controller is. BetterVR's snap of
// the right hand onto the string went: with the hand on the string the game dropped the shot at every draw (device
// 2026-11-03), and the user found it of no use. The aim is the left hand's (vr_bow.cpp), its line drawn while drawn.
bool g_prev_trigger = false;
std::atomic<float> g_bow_local[3];
std::atomic<bool> g_bow_local_valid{false};
std::atomic<float> g_bow_shot_local[3];  // the last aimed direction, for the shot after the release
std::atomic<double> g_bow_shot_at{0};
// the arrow's launch point (the bow's attackPos, vr_bow.cpp launch) in the left grip's frame, for where the line starts
std::atomic<float> g_bow_origin[3];
std::atomic<bool> g_bow_origin_valid{false};


void bow_string() {
    const char* left = vr_actor_name(vr_hand_item(0));
    const bool bow = left && strncmp(left, "Weapon_Bow", 10) == 0;
    const bool trigger = vr_bow_trigger();
    const bool aiming = bow && trigger && botw_vr_view_now() && vr_first_person();
    g_aiming.store(aiming, std::memory_order_relaxed);
    vr_bow_flight_update(aiming ? vr_hand_item(0) : 0);
    // the drawn bow's -x (where it shoots, BetterVR TryGetBowEntityWorldForward) in the left grip's frame, for the
    // line the headset draws (BotWImmersive.swift turns it with the controller); the shot itself uses it (vr_bow.cpp)
    float w[12];
    if (aiming && vr_hand_world_matrix(0, w)) {
        const uint32_t m = vr_hand_item(0) + 0x1f8;
        const float d[3] = {-mem::rf32(m), -mem::rf32(m + 16), -mem::rf32(m + 32)};
        float local[3];
        for (int i = 0; i < 3; i++) local[i] = w[i] * d[0] + w[4 + i] * d[1] + w[8 + i] * d[2];
        // Riding, the arms are the game's riding animation, not the controllers (vr_body's own pose list): the bow
        // model stayed pointing ahead of the horse whatever the hands did, and every arrow went that way (device
        // 2026-11-15: five shots, bow -x 0.74 0 -0.66 each time). There the direction comes from the controllers:
        // the bow's -x in the left grip as last learnt on foot, else the line from the drawing hand to the bow hand
        const uint32_t player = vr::player_actor();
        const bool riding = player && (mem::r32(player + 0x8dc) & (1u << 20)) != 0;
        static float learnt[3];
        static bool have_learnt = false;
        const float ll = sqrtf(local[0] * local[0] + local[1] * local[1] + local[2] * local[2]);
        if (!riding && ll > 0.5f) {
            for (int i = 0; i < 3; i++) learnt[i] = local[i] / ll;
            have_learnt = true;
        } else if (riding) {
            float wr[12];
            if (have_learnt) {
                for (int i = 0; i < 3; i++) local[i] = learnt[i];
            } else if (vr_hand_world_matrix(1, wr)) {
                const float line[3] = {w[3] - wr[3], w[7] - wr[7], w[11] - wr[11]};  // right grip -> left grip
                for (int i = 0; i < 3; i++) local[i] = w[i] * line[0] + w[4 + i] * line[1] + w[8 + i] * line[2];
            }
            static int logged;
            if (logged < 5)
                logged++, LOG("vr: riding bow: aimed with the controllers (%s)", have_learnt ? "the bow as held on foot"
                                                                                             : "the line between the hands");
        }
        for (int i = 0; i < 3; i++) g_bow_local[i].store(local[i]);
        g_bow_local_valid.store(true);
        // kept past the release for the shot: the game builds the arrow a few frames after the trigger lets go, by
        // which time the line is gone (device 2026-11-15: the line turned with the hands, the arrows still went ahead)
        for (int i = 0; i < 3; i++) g_bow_shot_local[i].store(local[i]);
        g_bow_shot_at.store(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
        // the launch point the shot uses (bow +0x770 while +0x7a0 is set; else the left grip, where the headset's
        // own fallback already starts the line): R^T (p - t) of the grip's world matrix
        const uint32_t held = vr_player_bow();
        bool origin = false;
        if (held && mem::r8(held + 0x7a0) != 0) {
            float p[3], l[3];
            for (int i = 0; i < 3; i++) p[i] = mem::rf32(held + 0x770 + 4 * i) - w[i * 4 + 3];
            for (int i = 0; i < 3; i++) l[i] = w[i] * p[0] + w[4 + i] * p[1] + w[8 + i] * p[2];
            // within an arm's reach of the grip (a stale attackPos from another place is not drawn from)
            if (l[0] * l[0] + l[1] * l[1] + l[2] * l[2] < 0.6f * 0.6f) {
                for (int i = 0; i < 3; i++) g_bow_origin[i].store(l[i]);
                origin = true;
            }
            static int logged;
            if (logged < 5)
                logged++, LOG("vr: bow launch point in the left grip (%.3f %.3f %.3f)%s", l[0], l[1], l[2],
                              origin ? "" : ", too far: the line starts at the grip");
        }
        g_bow_origin_valid.store(origin);
    } else {
        g_bow_local_valid.store(false);
        g_bow_origin_valid.store(false);
    }
    if (!trigger && g_prev_trigger && bow) {  // device 2026-11-03: about one release in three shot nothing
        static int logged;
        float pos[3], aim[3];
        const bool hand = vr_hand_world(0, pos, aim);
        const uint32_t m = vr_hand_item(0) + 0x1f8;  // the bow's ActorWiiU::mtx, row-major: its -x is where it shoots
        if (logged < 60)
            logged++, LOG("vr: bow released: view %d, drawn %.2f, aim %s(%.2f %.2f %.2f), bow -x (%.2f %.2f %.2f), "
                          "head pitch %.0f deg", botw_vr_view_now(), mem::rf32(vr_hand_item(0) + 0x94c),
                          hand ? "" : "untracked ", aim[0], aim[1], aim[2], -mem::rf32(m), -mem::rf32(m + 16),
                          -mem::rf32(m + 32), vr_head_pitch_deg());
    }
    g_prev_trigger = trigger;
}

// The arm bones of the player's model, after the game wrote their animated local matrix
constexpr uint32_t kGliding = 1u << 15;  // PlayerBase::moveBitFlags, the glider open (BetterVR IS_GLIDER_ACTIVE)
// How far the skeleton's root follows the head's level offset, in the model's space (follow_head); false: not at all. Not
// while gliding: the glider hangs from the body, so it slid with the head while the real hands stayed where they were
// ("the paraglider is left of my hands", at some head angles right on them: device 2026-11-08). BetterVR likewise lets go
// of the body when paragliding (entity_controller.cpp shouldDisablePlayerBodyDrive).
bool follow_shift(V3& out) {
    float w[3];
    const uint32_t player = vr::player_actor();
    if (!player || (mem::r32(player + 0x8dc) & kGliding) || !vr_head_offset_world(w)) return false;
    const float len = sqrtf(w[0] * w[0] + w[2] * w[2]);
    if (len < 0.01f) return false;
    if (len > 0.4f) w[0] *= 0.4f / len, w[2] *= 0.4f / len;
    const M34 actor = read34(player + 0x1f8);  // ActorWiiU::mtx, row-major: the model's axes in its columns
    out = {actor.m[0] * w[0] + actor.m[4] * w[1] + actor.m[8] * w[2], actor.m[1] * w[0] + actor.m[5] * w[1] + actor.m[9] * w[2],
           actor.m[2] * w[0] + actor.m[6] * w[1] + actor.m[10] * w[2]};
    return true;
}

void reach(const char* bone, uint32_t matrix) {
    const std::string_view b = bone;
    if (b == "Skl_Root") {
        // not while climbing, swimming, gliding, riding or ragdolling (PlayerBase::moveBitFlags at +0x8dc, BetterVR
        // PlayerMoveBitFlags): there the body and hands are the game's (on a wall the hands hold it; turned from it,
        // or with the hands off it, Link lets go)
        const uint32_t player = vr::player_actor();
        const uint32_t moving = player ? mem::r32(player + 0x8dc) : 0;
        // bit 4 alone is a ladder (Fable's review 2026-11-06: it was missing, the body still turned on ladders)
        // riding with the bow drawn: the arms follow the controllers after all (the body stays the game's, on the
        // saddle). With the riding animation's arms the bow sat big in front of the face and pointed elsewhere than the
        // line and the arrows, which go by the controllers (device 2026-11-15)
        const bool riding = (moving & (1u << 20)) != 0;
        const char* left_item = vr_actor_name(vr_hand_item(0));
        const bool riding_bow = riding && vr_bow_trigger() && left_item && strncmp(left_item, "Weapon_Bow", 10) == 0;
        {
            static bool was;
            if (riding_bow != was) LOG("vr: riding bow drawn: arms %s", riding_bow ? "follow the controllers" : "the game's again");
            was = riding_bow;
        }
        const uint32_t own_bits = (1u << 4) | (1u << 7) | (1u << 10) | (1u << 15) | (1u << 19) | (riding_bow ? 0u : (1u << 20));
        const bool own_pose = (moving & own_bits) != 0;
        for (int h = 0; h < 2; h++) t_reach[h] = !own_pose && hand_target(h, t_target[h]);
        // a throw leaving: the right hand's object (on its Weapon_R bone, where the grip is) at the launch point above
        // and in front of the eyes until it is let go (vr_aim.cpp), the hand turned as the controller is
        float launch[3];
        if (t_reach[1] && vr_throw_launch_point(launch)) {
            const V3 at = apply34(inv34(read34(player + 0x1f8)), {launch[0], launch[1], launch[2]});
            const V3 grip = pos34(mul34(t_target[1], inv34(grip_to_wrist(1))));
            const V3 d = at - grip;
            t_target[1].m[3] += d.x, t_target[1].m[7] += d.y, t_target[1].m[11] += d.z;
            static int logged;
            if (logged < 20)
                logged++, LOG("vr: throw: right hand moved %.2f m to the launch point (%.2f %.2f %.2f in the model)",
                              sqrtf(dot(d, d)), at.x, at.y, at.z);
        }
        // the root is shifted after this (follow_head), and everything under it with it: the targets are already where the
        // hands belong, so they are moved back by the same amount (the hands were off by the head's level offset, in
        // the same direction for both and changing as the head turned: device 2026-11-08)
        {
            V3 shift;
            if (follow_shift(shift))
                for (int h = 0; h < 2; h++)
                    if (t_reach[h]) t_target[h].m[3] -= shift.x, t_target[h].m[7] -= shift.y, t_target[h].m[11] -= shift.z;
        }
        // the view's heading in the model (0 along the model's +z, where the actor faces): the body is turned to it
        // below, and the poles and the crossed check go by it
        const float yaw = t_reach[0] || t_reach[1] ? view_yaw_in_model() : 0.0f;
        t_body = rot34(0, 1, 0, yaw * 180 / (float)M_PI);
        if (t_reach[0] || t_reach[1]) {
            // the actor keeps facing where it last walked, so after a walk backwards or sideways the body stands
            // turned from the view until the next walk (device 2026-11-04: the hands' targets of the "hands crossed"
            // lines were 110 to 125 degrees round in the model, right after walking back and to the right)
            static bool turned;
            static int logged;
            const float deg = yaw * 180 / (float)M_PI;
            if ((fabsf(deg) > 60) != turned && logged < 40)
                logged++, turned = !turned,
                LOG("vr: body %s the view (%.0f deg)", turned ? "turned from" : "back towards", deg);
        }
        if (t_reach[0] && t_reach[1]) {  // device 2026-11-03 / 04: the arms crossed now and then; which way the hands were
            // in the view's frame: its right axis in the model is (-cos yaw, 0, sin yaw) (hand_target's base); the left
            // hand's target right of the right one's crosses the arms. (Until 2026-11-04 this compared the model's x,
            // which only said the body stood turned from the view.)
            const V3 l = pos34(t_target[0]), r = pos34(t_target[1]), right{-cosf(yaw), 0, sinf(yaw)};
            const float lr = dot(l, right), rr = dot(r, right);
            static int crossed, logged;
            if (lr <= rr + 0.05f) crossed = 0;
            else if (crossed++ % 120 == 0 && logged < 40)
                logged++, LOG("vr: hands crossed in the view: left target %.2f m right of the right one's, view %.0f deg "
                              "from the body; left (%.2f %.2f %.2f), right (%.2f %.2f %.2f) in the model",
                              lr - rr, yaw * 180 / (float)M_PI, l.x, l.y, l.z, r.x, r.y, r.z);
        }
        if (t_reach[1]) two_hand(t_reach[0]);
        bow_string();
        M34 root = read34(matrix);
        if ((t_reach[0] || t_reach[1]) && !riding) {  // on the horse the body keeps the saddle's facing
            // the body turned to face where the player looks (BetterVR aligns Skl_Root with the headset's yaw):
            // otherwise an animation that turns Link (an attack, idling) puts his shoulder in front of the eyes
            const float facing = atan2f(root.m[2], root.m[10]);  // the root's +z in the model
            root = mul34(rot34(0, 1, 0, (yaw - facing) * 180 / (float)M_PI), root);
            write34(matrix, root);
        }
        t_spine[0] = root;
        return;
    }
    if (b == "Spine_1") t_spine[1] = mul34(t_spine[0], read34(matrix));
    if (b == "Spine_2") t_spine[2] = mul34(t_spine[1], read34(matrix));
    if (b.size() < 3 || b[b.size() - 2] != '_' || (b.back() != 'L' && b.back() != 'R')) return;
    const int hand = b.back() == 'L' ? 0 : 1;
    const std::string_view name = b.substr(0, b.size() - 2);
    if (!t_reach[hand]) return;
    if (name == "Clavicle") {
        t_arm[hand][0] = mul34(t_spine[2], read34(matrix));
    } else if (name == "Arm_1") {
        write34(matrix, solve_arm(hand, read34(matrix)));
    } else if (name == "Arm_2") {
        write34(matrix, mul34(inv34(t_arm[hand][1]), t_arm[hand][2]));  // the stretched upper arm's length included
    } else if (name == "Wrist") {
        write34(matrix, mul34(inv34(t_arm[hand][2]), t_target[hand]));
    }
}

// The model in the player's head armour slot (a hat, or his hair when he wears none) is a model of its own whose bones
// never come through here, so shrinking them hid nothing (device 2026-11-02: the hair in view drawing a weapon or
// looking down). As BetterVR (skeleton.cpp SubmitHeadMaterialVisibility): its materials are switched off in first
// person and back on otherwise. Player +0x1D94 PlayerArmors, +0x08 armorHead (a BaseProcLink: data, id), the data's id
// +0x3C and actor +0x40; the actor's gsys model +0x330: units (count +0x1C, array +0x24 of holders), each unit's
// vtable +0x74 (0x1034AB98 while live), material count +0x9A (u16), setMaterialVisible the vtable's +0x1B4.
// Called with the skeleton pass's context (a guest call there is what BetterVR's patch makes too)
void head_armour_visible(PPCContext* c, bool visible) {
    // Hidden: each material's own state is noted first (gsys::ModelUnit +0x8C: one visible bit a material, the game's
    // getter at 0x03999228), and shown again it is put back as it was, not every material on: the hair model has
    // materials the game keeps hidden, and switched on they hung a lock of hair across Link's face in the first events
    // (user 2026-11-16). Showing touches only what was hidden here; hiding is repeated every 60 passes (the game may set
    // them again: an armour change, a cutscene), showing is not
    struct Saved { uint32_t unit; uint16_t m; bool on; };
    static std::vector<Saved> saved;
    static uint32_t hidden_actor = 0;
    static bool hidden = false;
    static int age = 0;
    const uint32_t player = vr::player_actor();
    if (!player) return;
    const uint32_t link = player + 0x1d94 + 0x08, data = r32(link);
    const uint32_t actor = data && r32(data + 0x3c) == r32(link + 4) ? r32(data + 0x40) : 0;
    auto bit = [](uint32_t unit, uint32_t m) {
        const uint32_t words = r32(unit + 0x8c);
        return words && ((r32(words + (m >> 5) * 4) >> (m & 31)) & 1) != 0;
    };
    auto units_of = [](uint32_t a, auto&& each) {
        const uint32_t model = a ? r32(a + 0x330) : 0;
        if (!model) return;
        const uint32_t units = r32(model + 0x1c), array = r32(model + 0x24);
        if (!array || units > 256) return;
        for (uint32_t i = 0; i < units; i++) {
            const uint32_t holder = r32(array + i * 4), unit = holder ? r32(holder) : 0;
            if (unit && r32(unit + 0x74) == 0x1034ab98) each(unit);
        }
    };
    int n = 0;
    if (visible) {
        if (!hidden) return;
        hidden = false;
        if (actor == hidden_actor)  // another armour since: the game set up the new one itself
            for (const Saved& v : saved)
                if (r32(v.unit + 0x74) == 0x1034ab98) guest_call(c, r32(r32(v.unit + 0x74) + 0x1b4), {v.unit, v.m, v.on ? 1u : 0u}), n++;
        saved.clear();
    } else {
        if (hidden && actor == hidden_actor && ++age < 60) return;
        age = 0;
        if (!hidden || actor != hidden_actor) {  // newly hidden (or a new armour): note the game's own states
            saved.clear();
            units_of(actor, [&](uint32_t unit) {
                for (uint32_t m = 0; m < mem::r16(unit + 0x9a); m++) saved.push_back({unit, (uint16_t)m, bit(unit, m)});
            });
        }
        hidden = true, hidden_actor = actor;
        units_of(actor, [&](uint32_t unit) {
            const uint32_t set_visible = r32(r32(unit + 0x74) + 0x1b4);
            for (uint32_t m = 0; m < mem::r16(unit + 0x9a); m++, n++) guest_call(c, set_visible, {unit, m, 0u});
        });
    }
    static int logged;
    if (logged < 6) {
        int was_on = 0;
        for (const Saved& v : saved) was_on += v.on;
        logged++, LOG("vr: head armour %s: %d materials %s%s", vr_actor_name(actor) ? vr_actor_name(actor) : "?", n,
                      visible ? "put back as they were" : "hidden", visible ? "" : (" (" + std::to_string(was_on) + " of them shown before)").c_str());
    }
}

// The body under the head: the eyes are placed at the body's and moved with the head's own motion since the recentre,
// so leaning back or to the side took the view behind or beside Link's head, into his hair and onto his shoulders
// (device 2026-11-03, also in water). The skeleton's root follows the head's level offset instead (BetterVR keeps the
// body under the headset the same way), at most 40 cm
void follow_head(uint32_t matrix) {
    V3 shift;
    if (!follow_shift(shift)) return;
    M34 root = read34(matrix);
    root.m[3] += shift.x, root.m[7] += shift.y, root.m[11] += shift.z;
    write34(matrix, root);
}

void bone_wrapper(PPCContext* c) {
    const char* bone = mem::str(r32(c->r[12] + 4));
    const uint32_t scale = c->r[5], matrix = c->r[4];
    const char* model = model_name(c->r[31]);
    rt_call(c, t_bone_fn);
    if (!bone || !model || !scale) return;
    const std::string_view m = model;
    const bool player = m == "GameROMPlayer", head_armour = m.starts_with("Armor_") && m.ends_with("_Head");
    // the skeleton is only changed in first person in the headset's view (not in menus, events on the screen, third
    // person); the head armour's materials follow the same
    const bool active = botw_vr_view_now() && vr_first_person();
    if (player && !strcmp(bone, "Skl_Root")) head_armour_visible(c, !active);
    if (!active) return;
    // BOTW_VR_LOG_BONES=1 (tests): the player models' bone names, once each
    static const bool log_bones = getenv("BOTW_VR_LOG_BONES") != nullptr;
    if (log_bones && (player || head_armour || strstr(model, "Player") || strstr(model, "Link") || strstr(model, "Face") ||
                      !strcmp(bone, "Face_Root") || !strncmp(bone, "Eye", 3))) {
        static std::mutex seen_lock;
        static std::set<std::string> seen;
        std::lock_guard<std::mutex> l(seen_lock);
        if (seen.insert(std::string(m) + "/" + bone).second) LOG("vr: bone %s / %s", model, bone);
    }
    if (player && matrix) {
        reach(bone, matrix);  // first: it turns the body, and the eyes are read from the turned skeleton
        collect_head(bone, matrix);
        if (!strcmp(bone, "Skl_Root")) follow_head(matrix);  // after the eyes are read: they are the view's base
    }
    if (head_armour || (player && head_bone(bone))) {
        mem::wf32(scale, 0), mem::wf32(scale + 4, 0), mem::wf32(scale + 8, 0);
        // the neck's joint moved into the chest: shrunk at the neck, the head left a flat patch of hair in the
        // collar, from vertices weighted between the neck and the chest (device and Mac 2026-11-11, looking down and
        // swimming); pulled to the chest they sit inside the tunic. After collect_head, which has read the neck as it was
        // (one neck length below the chest's joint: at the joint itself a stub of neck still showed in the collar)
        if (player && matrix && !strcmp(bone, "Neck"))
            for (int k : {12, 28, 44}) mem::wf32(matrix + k, -mem::rf32(matrix + k));
    }
    // how often the head is hidden: per pass over the player's skeleton (Skl_Root starts one), was Head among its bones
    if (player) {
        static std::atomic<int> passes{0}, hidden{0};
        static std::atomic<bool> head_this_pass{false};
        if (!strcmp(bone, "Skl_Root")) {
            if (head_this_pass.exchange(false)) hidden++;
            if (++passes == 600) {
                LOG("vr: head hidden in %d of the last 600 skeleton passes", hidden.load());
                passes = 0, hidden = 0;
            }
        } else if (!strcmp(bone, "Head")) {
            head_this_pass = true;
        }
    }
}
}  // namespace

// The player's eyes in the world, from the last skeleton pass and the actor's matrix (false until the first pass)
bool vr_player_eyes(uint32_t player_actor, float out[3]) {
    float eyes[3];
    {
        std::lock_guard<std::mutex> l(g_eyes_lock);
        if (!g_have_eyes) return false;
        memcpy(eyes, g_eyes_model, sizeof eyes);
    }
    const M34 actor = read34(player_actor + 0x1f8);  // ActorWiiU::mtx
    for (int r = 0; r < 3; r++)
        out[r] = actor.m[r * 4] * eyes[0] + actor.m[r * 4 + 1] * eyes[1] + actor.m[r * 4 + 2] * eyes[2] + actor.m[r * 4 + 3];
    return true;
}

// lwz r9, 100(r8): the model unit's getBoneLocalMatrix, called through ctr a few instructions on
BOTW_HOOK(0x03820c3c) {
    if (!vr::enabled()) return;
    static const uint32_t thunk = dispatch::thunk(bone_wrapper, "vr_bone_wrapper");
    t_bone_fn = c->r[9];
    c->r[9] = thunk;
}

// The game fades an actor and its weapons out as the camera comes close (Actor / Weapon / OptionalWeapon
// calculateModelOpacity, the opacity in f1); with the eyes in Link's head that is all of him and what he holds.
// With a controller tracked, first person keeps them opaque so the hands show (BetterVR patch_FirstPersonMode.asm:
// the three functions' blr -> opacity 1); without one Link stays faded out as before, nothing of him in the way.
void keep_opaque(PPCContext* c) {
    float g[12];
    if (botw_vr_view_now() && vr_first_person() && (vr_hand_in_base(0, g) || vr_hand_in_base(1, g))) c->f[1] = 1.0;
}
BOTW_HOOK(0x024a6f1c) { keep_opaque(c); }  // addi r1, r1, 16 before the blr
BOTW_HOOK(0x033afa98) { keep_opaque(c); }  // addi r1, r1, 16 before the blr
BOTW_HOOK(0x037b13a4) { keep_opaque(c); }  // fmuls f1, f13, f0 before the blr

// vr_bow.cpp: the drawn bow's shooting direction in the world, from the left grip and the direction above
bool vr_bow_dir_world(float out[3]) {
    float w[12];
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now - g_bow_shot_at.load() > 1.5 || !vr_hand_world_matrix(0, w)) return false;  // aimed in the last 1.5 s
    const float l[3] = {g_bow_shot_local[0].load(), g_bow_shot_local[1].load(), g_bow_shot_local[2].load()};
    for (int r = 0; r < 3; r++) out[r] = w[r * 4 + 0] * l[0] + w[r * 4 + 1] * l[1] + w[r * 4 + 2] * l[2];
    return true;
}
// BotWImmersive.swift: the drawn bow's shooting direction in the left grip's frame; 0 when not drawn
extern "C" int botw_vr_bow_dir_in_grip(float out[3]) {
    if (!g_bow_local_valid.load()) return 0;
    for (int i = 0; i < 3; i++) out[i] = g_bow_local[i].load();
    return 1;
}
// BotWImmersive.swift: the drawn bow's launch point (where the arrow sits) in the left grip's frame; 0 when unknown
extern "C" int botw_vr_bow_origin_in_grip(float out[3]) {
    if (!g_bow_origin_valid.load()) return 0;
    for (int i = 0; i < 3; i++) out[i] = g_bow_origin[i].load();
    return 1;
}
