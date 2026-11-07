// VR motion controls, second part: swinging the controller swings the weapon. The weapon already follows the hand
// (it hangs from the Weapon_L / Weapon_R bone under the wrist the arm IK moves, vr_body.cpp); what decides whether it
// hits is the weapon's attack sensor. In first person with the controllers tracked, the sensor is switched on while
// the hand holding it makes a swing or a thrust (BetterVR hooking/weapon.cpp hook_EnableWeaponAttackSensor,
// patch_CTRL_WeaponAttacks.asm), with the damage scaled by the swing's speed; the attack button alone no longer hits.
// The swing analyser is BetterVR's WeaponMotionAnalyser (hooking/weapon.h) with its "normal" sensitivity: a slash is
// a burst of angular acceleration or speed across the grip, a stab a straight push forward along it, and the hitbox
// opens once the blade has turned 36 degrees (or the hand travelled 12 cm, scaled by the arm's length).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>

#include "../hle.h"
#include "hooks.h"
#include "vr.h"

using mem::r32;

extern "C" int botw_vr_view_now(void);  // vr_render.cpp
bool vr_first_person();                 // vr_render.cpp
bool vr_hand_motion(int hand, float grip[16], float velocity[3], float angular[3], float head[3], uint32_t* id);
extern "C" void botw_vr_haptic(int hand, float strength, float seconds);  // ios/src/sense_vision.mm (a Mac: nothing)
bool vr_two_hand();  // vr_gesture.cpp

namespace {
struct V3 {
    float x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float len(V3 a) { return sqrtf(dot(a, a)); }
V3 norm(V3 a) {
    const float l = len(a);
    return l > 0 ? a * (1 / l) : V3{};
}
V3 mix(V3 a, V3 b, float t) { return a + (b - a) * t; }
float lerp(float a, float b, float t) { return a + (b - a) * t; }

// a grip's rotation: its axes as columns of a column-major 4x4
struct Rot {
    V3 col[3];
    V3 to_local(V3 v) const { return {dot(col[0], v), dot(col[1], v), dot(col[2], v)}; }  // R^T v
};

enum class Attack { None, Slash, Stab };

// BetterVR WeaponProfile::ForSensitivity(SWING_NORMAL)
struct Profile {
    float stabSpeedThreshold = 0.05f, stabAccThreshold = 4.0f, stabLinearSteadinessThreshold = cosf((float)M_PI / 4),
          stabAngularSteadinessThreshold = 4.5f, stabTravelDistance = 0.12f, stabAccSmoothingTimeConstant = 0.050f,
          stabVelocityThreshold = 1.0f;
    float slashSpeedThreshold = 1.5f, slashAccThreshold = 20.0f, slashVelocityThreshold = 7.0f,
          slashAccDriftThreshold = 10.0f, slashTravelAngle = (float)M_PI / 5;
    float maxBadDuration = 0.022f;
    int64_t goodSampleGracePeriod = 40000000;  // ns
    float minGoodSwingDuration = 0.040f, minGoodStabDuration = 0.030f, smoothingTimeConstant = 0.020f,
          angularDriftMinVelocity = 0.5f, referenceArmLength = 0.55f;
};

// BetterVR WeaponMotionAnalyser, without its debug sample buffer
class Analyser {
public:
    bool attacking() const { return m_attackActive; }
    bool hitbox() const { return m_hitbox; }
    void set_hitbox(bool on) { m_hitbox = on; }
    float power() const { return m_swingPower; }

    void reset() { *this = Analyser{}; }
    void reset_if_type_changed(uint32_t type) {
        if (type != m_type) reset(), m_type = type;
    }

    void update(const Rot& rot, V3 position, V3 velocity, V3 angular, V3 head, int64_t now) {
        const float dt = m_prevTime > 0 ? (float)(now - m_prevTime) / 1e9f : 0.0f;
        local_signals(rot, velocity, angular, dt);
        m_angularDrift = angular_drift(angular, dt);
        smoothed(dt);
        if (dt > 0) m_frameDt = m_frameDt == 0 ? dt : lerp(m_frameDt, dt, 1 - expf(-dt / 0.5f));
        const float reach = len(position - head);
        if (reach <= 1.2f) m_armLength = lerp(m_armLength, reach, reach > m_armLength ? 0.05f : 0.002f);
        const float dtAccum = std::min(dt, 0.05f);
        detect(rot, position, now, dtAccum);
        steadiness(now, dtAccum);
        activity(rot, position);
        swing_power();
        m_prevAngular = angular;
        m_prevLocalLin = m_localLin;
        m_prevTime = now;
    }

private:
    void local_signals(const Rot& rot, V3 velocity, V3 angular, float dt) {
        m_localLin = rot.to_local(velocity);
        m_localAng = rot.to_local(angular);
        m_localLinAcc = dt > 0 ? (m_localLin - m_prevLocalLin) * (1 / dt) : V3{};
        const V3 flat = {m_localAng.x, m_localAng.y, 0};  // the twist about the grip's own axis removed
        m_flatAngAcc = dt > 0 ? len(flat - m_prevFlatAng) / dt : 0;
        m_prevFlatAng = flat;
    }
    float angular_drift(V3 angular, float dt) const {
        const float a = len(angular), b = len(m_prevAngular);
        if (a < m_p.angularDriftMinVelocity || b < m_p.angularDriftMinVelocity || dt <= 0) return 0;
        return acosf(std::clamp(dot(norm(angular), norm(m_prevAngular)), -1.0f, 1.0f)) / dt;
    }
    void smoothed(float dt) {
        const float a = dt > 0 ? 1 - expf(-dt / m_p.smoothingTimeConstant) : 0;
        m_smoothLinAcc = mix(m_smoothLinAcc, m_localLinAcc, a);
        m_smoothFlatAngAcc = lerp(m_smoothFlatAngAcc, m_flatAngAcc, a);
        m_smoothDrift = lerp(m_smoothDrift, m_angularDrift, a);
        const float slow = dt > 0 ? 1 - expf(-dt / m_p.stabAccSmoothingTimeConstant) : 0;
        m_smoothLinAccSlow = mix(m_smoothLinAccSlow, m_localLinAcc, slow);
    }
    void detect(const Rot& rot, V3 position, int64_t now, float dt) {
        if (m_locked != Attack::None) return;
        const bool refractory = now < m_refractoryUntil;
        bool stabOk = false;
        if (dot(m_localLin, m_localLin) >= 1e-8f && !refractory) {
            const V3 dir = norm(m_localLin);
            stabOk = fabsf(dir.z) > m_p.stabLinearSteadinessThreshold &&
                     fabsf(m_localAng.x) < m_p.stabAngularSteadinessThreshold &&
                     fabsf(m_localAng.y) < m_p.stabAngularSteadinessThreshold &&
                     (-m_smoothLinAccSlow.z > m_p.stabAccThreshold || -m_localLin.z > m_p.stabVelocityThreshold);
            if (stabOk) {
                if (m_goodStab == 0) m_lockedPosition = position;
                m_goodStab += dt;
                m_lastGoodStab = now;
                if (m_goodStab > m_p.minGoodStabDuration) m_locked = Attack::Stab, m_peak = 0, m_lockTime = now;
            } else if (now - m_lastGoodStab > m_p.goodSampleGracePeriod) {
                m_goodStab = 0;
            }
        }
        if (!stabOk && m_locked != Attack::Stab) {
            const float flat = len({m_localAng.x, m_localAng.y, 0});
            if ((!refractory && m_smoothFlatAngAcc > m_p.slashAccThreshold) || flat > m_p.slashVelocityThreshold) {
                if (m_goodSwing == 0) m_lockedAxis = rot.col[2];
                m_goodSwing += dt;
                m_lastGoodSwing = now;
                if (m_goodSwing > m_p.minGoodSwingDuration) m_locked = Attack::Slash, m_peak = 0, m_lockTime = now;
            } else if (now - m_lastGoodSwing > m_p.goodSampleGracePeriod) {
                m_goodSwing = 0;
            }
        }
    }
    void steadiness(int64_t now, float dt) {
        bool bad = false;
        if (m_locked == Attack::None) return;
        if (m_locked == Attack::Stab) {
            if (dot(m_localLin, m_localLin) < 1e-8f) {
                bad = true;
            } else {
                const V3 dir = norm(m_localLin);
                bad = fabsf(dir.z) < m_p.stabLinearSteadinessThreshold || -m_localLin.z < m_p.stabSpeedThreshold ||
                      len({m_localAng.x, m_localAng.y, 0}) > m_p.stabAngularSteadinessThreshold;
            }
        } else {
            bad = m_smoothDrift > m_p.slashAccDriftThreshold ||
                  len({m_localAng.x, m_localAng.y, 0}) < m_p.slashSpeedThreshold;
        }
        const int64_t grace = std::max<int64_t>(33000000, (int64_t)(2.5f * m_frameDt * 1e9f));
        if (now - m_lockTime < grace) bad = false;
        m_bad = bad ? m_bad + dt : std::max(0.0f, m_bad - dt);
        if (m_bad >= std::max(m_p.maxBadDuration, 2.5f * m_frameDt)) {
            m_bad = 0;
            if (m_attackActive) m_refractoryUntil = now + 250000000;
            if (m_locked == Attack::Slash) m_goodSwing = 0, m_lastGoodSwing = 0;
            else m_goodStab = 0, m_lastGoodStab = 0;
            m_locked = Attack::None;
        }
    }
    void activity(const Rot& rot, V3 position) {
        if (m_locked == Attack::None) {
            m_attackActive = false;
            return;
        }
        if (m_attackActive) return;
        const float armScale = m_armLength / m_p.referenceArmLength;
        if (m_locked == Attack::Stab) {
            if (len(position - m_lockedPosition) > m_p.stabTravelDistance * armScale) m_attackActive = true;
        } else if (acosf(std::clamp(dot(rot.col[2], m_lockedAxis), -1.0f, 1.0f)) > m_p.slashTravelAngle) {
            m_attackActive = true;
        }
    }
    void swing_power() {
        if (!m_attackActive) {
            m_swingPower = 0;
            return;
        }
        if (m_locked == Attack::Stab) {
            m_peak = std::max(m_peak, fabsf(m_localLin.z));
            const float straight = dot(m_localLin, m_localLin) >= 1e-8f ? fabsf(norm(m_localLin).z) : 0;
            const float t = std::clamp((straight - m_p.stabLinearSteadinessThreshold) / (1 - m_p.stabLinearSteadinessThreshold), 0.0f, 1.0f);
            m_swingPower = std::clamp(m_peak / 2.0f, 0.0f, 1.0f) * lerp(0.70f, 1.0f, t);  // 2 m/s: a strong stab
        } else {
            m_peak = std::max(m_peak, len({m_localAng.x, m_localAng.y, 0}));
            m_swingPower = std::clamp(m_peak / 10.0f, 0.0f, 1.0f);  // 10 rad/s: a strong slash
        }
    }

    Profile m_p;
    uint32_t m_type = 0;
    V3 m_prevLocalLin, m_prevFlatAng, m_prevAngular;
    int64_t m_prevTime = 0;
    V3 m_smoothLinAcc, m_smoothLinAccSlow;
    float m_smoothFlatAngAcc = 0, m_smoothDrift = 0, m_frameDt = 0;
    V3 m_localLin, m_localAng, m_localLinAcc;
    float m_flatAngAcc = 0, m_angularDrift = 0;
    Attack m_locked = Attack::None;
    bool m_attackActive = false, m_hitbox = false;
    float m_goodSwing = 0, m_goodStab = 0, m_bad = 0;
    int64_t m_lastGoodSwing = 0, m_lastGoodStab = 0, m_lockTime = 0, m_refractoryUntil = 0;
    V3 m_lockedPosition, m_lockedAxis;
    float m_swingPower = 0, m_peak = 0, m_armLength = 0.55f;
};

std::atomic<uint64_t> g_out_frame{0};  // the frame a melee weapon was last seen in the hand
std::atomic<uint32_t> g_bow{0};  // the player's bow, seen at its attack update
Analyser g_analyser[2];          // 0 left hand, 1 right (calc threads: one weapon per hand, so one writer each)
uint32_t g_sampled_pose[2];      // the pose each hand was last analysed with (once per pose)

// Weapon fields (BetterVR include/game_structs.h Weapon): the attack sensor's setup block and the finalized copy
constexpr uint32_t kSensorMode = 0x874, kSensorMultiplier = 0x87c, kSensorShieldBreak = 0x884,
                   kSensorContactLayerSet = 0x89c, kSensorReset = 0x8a0, kWeaponType = 0x938,
                   kFinalMultiplier = 0x94c, kHeldIndex = 0x5f4;

void sensor_off(uint32_t w) {
    mem::w8(w + kSensorReset, 1);
    mem::w32(w + kSensorMode, 1);
    mem::w8(w + kSensorContactLayerSet, 0);
}

// BetterVR ComputeAttackDamageMultiplier: weak motions stay light, committed swings ramp up to full damage
float damage(float power) {
    const float n = std::clamp((power - 0.15f) / 0.85f, 0.0f, 1.0f);
    return 0.20f + 1.15f * n * n * (3 - 2 * n);
}

bool any_hand_tracked() {
    float g[16], v[3], a[3], h[3];
    uint32_t id;
    return vr_hand_motion(0, g, v, a, h, &id) || vr_hand_motion(1, g, v, a, h, &id);
}

// One of the player's weapons, at the start of the game's attack update (r31 the Weapon, r3 its holder)
void weapon_attack(PPCContext* c) {
    const uint32_t w = c->r[31], holder = c->r[3];
    if (!holder || holder != vr::player_actor()) return;
    const uint32_t type = mem::r32(w + kWeaponType);
    if (type == 3) g_bow.store(w, std::memory_order_relaxed);  // the bow (vr_bow.cpp)
    if (type > 2) return;  // small sword, large sword, spear: what can be swung
    // Weapon::isHolding(weapon, holder) through the vtable (+0x4cc): 0 for a weapon in the hand (BetterVR's reading);
    // guest_call leaves r3, r4 and f1 changed, which this function still uses
    const uint32_t r3 = c->r[3], r4 = c->r[4];
    const double f1 = c->f[1];
    const bool in_hand = guest_call(c, r32(r32(w + 0xe8) + 0x4cc), {w, holder}) == 0;
    c->r[3] = r3, c->r[4] = r4, c->f[1] = f1;
    if (in_hand) g_out_frame.store(vr::stereo_frames(), std::memory_order_relaxed);  // vr_gesture.cpp
    const uint32_t held = mem::r32(w + kHeldIndex);
    if (held > 1) return;
    const int hand = held == 0 ? 1 : 0;  // the game's index 0 is the right hand
    Analyser& a = g_analyser[hand];
    float grip[16], vel[3], ang[3], head[3];
    uint32_t id;
    if (!in_hand || !vr_hand_motion(hand, grip, vel, ang, head, &id)) {
        a.reset();
        sensor_off(w);
        return;
    }
    if (id != g_sampled_pose[hand]) {
        g_sampled_pose[hand] = id;
        const Rot rot = {{{grip[0], grip[1], grip[2]}, {grip[4], grip[5], grip[6]}, {grip[8], grip[9], grip[10]}}};
        const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count();
        a.reset_if_type_changed(type);
        a.update(rot, {grip[12], grip[13], grip[14]}, {vel[0], vel[1], vel[2]}, {ang[0], ang[1], ang[2]},
                 {head[0], head[1], head[2]}, now);
    }
    if (a.attacking()) {
        const bool arm = !a.hitbox();  // one hit per swing: re-arming starts a new attack (clears its hit list)
        if (arm) {
            LOG("vr: swing with the %s hand (power %.2f)", hand ? "right" : "left", a.power());
            botw_vr_haptic(hand, 0.3f + 0.6f * a.power(), 0.12f);  // BetterVR rumbles the swinging hand by its power
        }
        a.set_hitbox(true);
        mem::w32(w + kSensorMode, 2);
        mem::w32(w + kSensorShieldBreak, 2);  // damage type 2: also fells trees
        if (arm) mem::w8(w + kSensorReset, 1), mem::w8(w + kSensorContactLayerSet, 0);
        // two-handers: 0.85 swung with one hand, 1.2 held with both (BetterVR's grip multipliers)
        const float m = damage(a.power()) * (type == 0 ? 1.0f : vr_two_hand() ? 1.2f : 0.85f);
        mem::wf32(w + kSensorMultiplier, m);
        mem::wf32(w + kFinalMultiplier, m);
    } else if (a.hitbox()) {  // the swing is over (otherwise the game's own attacks are left as they are)
        a.set_hitbox(false);
        sensor_off(w);
    }
}
}  // namespace

// Weapon's attack update reads the sensor's reset flag (lbz r0, 0x8a0(r31)) right after checking it has a holder:
// the sensor is set up first, then the instruction runs
BOTW_SITE(0x024aa7c4) {
    if (botw_vr_view_now() && vr_first_person() && any_hand_tracked()) weapon_attack(c);
    c->r[0] = mem::r8(c->r[31] + kSensorReset);
}

uint32_t vr_player_bow() { return g_bow.load(std::memory_order_relaxed); }  // vr_bow.cpp
// vr_gesture.cpp: whether a melee weapon is out, in the hand (seen within the last few frames)
bool vr_melee_out() {
    return vr::stereo_frames() - g_out_frame.load(std::memory_order_relaxed) < 8;
}
