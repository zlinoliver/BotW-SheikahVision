// VR motion controls: body slots (BetterVR hooking/controls.cpp, isHandOver*Slot and the grab handling in its input
// injection). Reaching over a shoulder or to the waist with either hand and squeezing the grip takes something out.
// A hand is over a shoulder when it is behind the head and within 35 cm of it, at the waist when it is behind the
// body (5 cm of slack) and more than 45 cm below the head; left / right by the side of the head it is on. While a hand
// is over a slot its grip is the slot's and does not do its usual job (L / Y on the Sense controllers). A button
// goes to the game for a few frames, long enough to count as a press. First person only, with the controllers tracked.
// The slots and quick menus (user 2026-11-05, both layouts; the classic layout is the default):
//   Right shoulder, grip tap (let go within 0.4 s): the melee weapon out (Y, at once) or put away (B, on the let-go).
//     Grip held 0.4 s: the weapon quick menu (d-pad right; with the bow in the left hand the arrows, d-pad left), as
//     BetterVR / commit 2a0531f. A buzz when it opens; the d-pad is held while the grip is, the right stick picks,
//     letting the grip go confirms. With the bow out the tap's Y waits for the let-go too, so a hold opens the arrows.
//   Left shoulder, grip tap: the bow (ZR; at once, so a hold then opens the bows' menu, d-pad right, as BetterVR's forced
//     bow equip). With a melee weapon in the right hand the hold is the shields' menu (d-pad left) and the tap's ZR waits
//     for the let-go (ZR at once would swap the weapon for the bow).
//   A menu stays open while the grip is held, wherever the hand goes and whether or not it is tracked: the grip is a
//   button and stays reliable, and closing early would confirm a pick the player did not make. No menu with Magnesis
//   in the right hand (BetterVR). The rune menu is a long press of Triangle (sense_controller.h).
//   Left waist: nothing (taken back by the user 2026-11-05; the runes are Triangle).
//   Right waist, grip held 0.6 s: that hand drops what it holds (vr_hands.cpp does it; BetterVR's long press there).
// Throwing: with a melee weapon in the right hand and that hand raised by the head (throw_ready: right of the head,
// up to 15 cm ahead of the eyes and 20 cm below them; the right shoulder slot was too narrow), the right trigger squeezed
// (a fresh squeeze there) holds R (the game's wind-up) until the trigger is let go, which throws (BetterVR's weapon
// throw); the trigger is taken, so it sends no ZR. A throwable object in the right hand (a rock, a pot, a bomb:
// ActorWiiU::flags2 +0x364 bit 0x4000) is thrown with the trigger anywhere.
// The magnet glove (Magnesis in the right hand): a grip held anywhere but a slot moves the object with that hand, from
// where the grip went down: up / down is the right stick's y, forward / back pulses the d-pad up / down, the further
// the faster (BetterVR's magnesisVerticalAmount / magnesisForwardAmount: 1.5 cm dead zone, full at 7.5 cm).
// Two-handed grip: with a two-handed sword or spear in the right hand and the left hand empty, the left grip squeezed with
// the left hand on the weapon (within 12 cm of the blade's line, 5 cm or more from the right hand) holds it with both
// hands until the grip is let go: the arms then aim the blade along the line through both hands (vr_body.cpp) and
// it hits harder (vr_weapon.cpp), after BetterVR's two-hand grip.
// Also (user 2026-11-03, from a PC VR mod's controls card):
//   Right grip with the right stick pulled down: the right hand drops what it holds.
//   Whistle: both hands at the mouth (within 20 cm of the head, ahead of it) with both grips squeezed: d-pad down.
//   The rune: with the Sheikah Slate in the left hand (Item_Conductor) the right trigger is A (use the rune).
// The shield (BetterVR): the left hand raised to chest height with a shield and a melee weapon out guards (ZL); a sharp
// push of it while guarding parries (A); the left trigger guards with the lock-on as before.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cmath>

#include "../../../ios/src/vr_bridge.h"
#include "../input/input.h"
#include "hooks.h"
#include "vr.h"
#include "vr_gesture.h"

bool vr_room_pose(BotWVRPose& out);  // vr_render.cpp
bool vr_melee_out();                 // vr_weapon.cpp
uint32_t vr_hand_item(int hand);     // vr_hands.cpp
void vr_drop_hand_item(int hand);    // vr_hands.cpp
bool vr_hand_world(int hand, float pos[3], float fwd[3]);  // vr_body.cpp
const char* vr_actor_name(uint32_t actor);
extern "C" void botw_vr_haptic(int hand, float strength, float seconds);  // ios/src/sense_vision.mm
bool vr_first_person();  // vr_render.cpp
bool vr_on_ladder();     // vr_ladder.cpp

namespace {
enum Slot { kNone, kLeftShoulder, kRightShoulder, kLeftWaist, kRightWaist, kMouth };
const char* const kSlotName[] = {"none", "left shoulder", "right shoulder", "left waist", "right waist", "mouth"};

Slot slot_of(const BotWVRPose& p, int hand) {
    if (p.hand_tracked[hand] < 2) return kNone;
    const float* h = p.origin_from_device;  // column-major: x axis 0..2, z axis 8..10, position 12..14
    const float* g = p.origin_from_grip[hand];
    const float d[3] = {g[12] - h[12], g[13] - h[13], g[14] - h[14]};
    float fx = -h[8], fz = -h[10];  // the head's forward, level
    const float fl = sqrtf(fx * fx + fz * fz);
    if (fl < 1e-4f) return kNone;
    fx /= fl, fz /= fl;
    const float ahead = fx * d[0] + fz * d[2];
    const float side = h[0] * d[0] + h[1] * d[1] + h[2] * d[2];
    const bool left = side < 0;
    const float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    // the mouth before the shoulders: a controller held to the mouth sits about level with the headset's own origin,
    // often a little behind it, and was taken for a shoulder (device 2026-11-05: the whistle rarely worked). Near the
    // middle (15 cm either side), below the eyes (up to 35 cm), at most 8 cm behind them
    if (fabsf(side) < 0.15f && d[1] < 0.02f && d[1] > -0.35f && ahead > -0.08f && ahead < 0.30f && dist < 0.35f)
        return kMouth;
    if (ahead < 0 && dist < 0.35f) return left ? kLeftShoulder : kRightShoulder;
    // the waist: behind the body (BetterVR's 5 cm of slack), 45 cm below the head. Only the right one: a left-waist
    // rune slot was tried and taken back by the user (2026-11-04, 2026-11-05 twice); the runes are Triangle
    if (d[1] < -0.45f && !left && ahead - 0.05f < 0) return kRightWaist;
    return kNone;
}

// the melee throw's wind-up (the right trigger): the right hand raised by the head, as for an overhand throw. Wider
// than the shoulder slot (behind the head, 35 cm), which a hand coming up from the front often missed and the trigger
// then drew the bow instead (user 2026-11-05: "hard to trigger, easily mistaken for drawing the bow"): on the right, no more than 15 cm ahead of
// the eyes, no lower than 20 cm below them, within 50 cm
bool throw_ready(const BotWVRPose& p) {
    if (p.hand_tracked[1] < 2) return false;
    const float* h = p.origin_from_device;
    const float* g = p.origin_from_grip[1];
    const float d[3] = {g[12] - h[12], g[13] - h[13], g[14] - h[14]};
    float fx = -h[8], fz = -h[10];
    const float fl = sqrtf(fx * fx + fz * fz);
    if (fl < 1e-4f) return false;
    fx /= fl, fz /= fl;
    const float ahead = fx * d[0] + fz * d[2];
    const float side = h[0] * d[0] + h[1] * d[1] + h[2] * d[2];
    const float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    return side > 0 && ahead < 0.15f && d[1] > -0.20f && dist < 0.5f;
}

double now_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

int g_pulse_frames = 0;  // host input thread only
uint32_t g_pulse = 0;
int g_prev_grip[2];
double g_waist_since[2];  // when a grip went down at the right waist (0: not)
bool g_waist_done[2];
std::atomic<bool> g_two_hand{false};
std::atomic<bool> g_bow_trigger{false};  // the right trigger / ZR, for the bow string (vr_body.cpp)  // read by the bone callback and the weapon update
bool g_throwing = false;  // R held for a throw until the right trigger is let go
std::atomic<bool> g_throw_aiming{false};  // g_throwing in first person, for the headset's render loop
std::atomic<bool> g_magnet_hold{false};  // Magnesis holds an object (the glove is in the right hand)
int g_magnet_hand = -1;   // the hand holding the grip for Magnesis, -1 none
float g_magnet_from[3];   // where that grip went down
int g_magnet_wait = 0;    // polls until the next d-pad pulse
constexpr double kQuickMenuHold = 0.4;
double g_slot_since[2];       // when the grip went down over a shoulder (0: not, or the hold is over)
uint32_t g_slot_tap[2];       // the button an early let-go sends (B, Y or ZR), 0 none (it went at the press)
uint32_t g_slot_menu[2];      // the d-pad direction a 0.4 s hold opens
const char* g_slot_menu_name[2];
uint32_t g_menu_dir[2];       // the d-pad held for an open quick menu, 0 none
int g_prev_rtrigger = 0;
// LOG lines for the headset test, at most this many of each kind
int g_log_budget_menu = 200, g_log_budget_throw = 200;
bool g_dropped[2];            // the grip's drop is done (until it is let go)
bool g_whistled = false;

bool name_starts(uint32_t actor, const char* prefix) {
    const char* n = vr_actor_name(actor);
    return n && strncmp(n, prefix, strlen(prefix)) == 0;
}
bool melee(uint32_t item) {
    const char* n = vr_actor_name(item);
    return n && (strncmp(n, "Weapon_Sword", 12) == 0 || strncmp(n, "Weapon_Lsword", 13) == 0 ||
                 strncmp(n, "Weapon_Spear", 12) == 0);
}
// BetterVR getMagnesisForwardFrameInterval: polls between pulses for a reach of v (0..1)
int pulse_interval(float v) { return v <= 0.25f ? 10 : v < 0.5f ? 5 : v < 0.75f ? 2 : 1; }
float remap(float v) {  // signed, 1.5 cm dead zone, 1 at 7.5 cm
    const float a = std::fmin(std::fmax((fabsf(v) - 0.015f) / (0.075f - 0.015f), 0.0f), 1.0f);
    return v < 0 ? -a : a;
}
}  // namespace

extern "C" void botw_vr_gestures(const BotWVRGestureIn* in, BotWVRGestureOut* out) {
    *out = BotWVRGestureOut{};
    g_bow_trigger.store(in->trigger[1] || (in->buttons & input::BTN_ZR), std::memory_order_relaxed);
    const int* grip = in->grip;
    int* consumed = out->take_grip;
    const uint32_t buttons = in->buttons;
    BotWVRPose p;
    if (g_pulse_frames > 0) g_pulse_frames--;
    const bool on = vr_room_pose(p);
    const uint32_t left_item = on ? vr_hand_item(0) : 0;
    const bool slate = name_starts(left_item, "Item_Conductor");
    // whistle: both hands at the mouth, both grips
    {
        const bool mouth = on && slot_of(p, 0) == kMouth && slot_of(p, 1) == kMouth;
        // both hands there: their grips are the whistle's even before both are squeezed (one squeezed first attacked
        // or ran, device 2026-11-05)
        if (mouth) consumed[0] |= grip[0], consumed[1] |= grip[1];
        if (mouth && grip[0] && grip[1]) {
            consumed[0] = consumed[1] = 1;
            if (!g_whistled) {
                g_whistled = true;
                g_pulse = input::BTN_DOWN, g_pulse_frames = 8;
                botw_vr_haptic(0, 0.6f, 0.2f), botw_vr_haptic(1, 0.6f, 0.2f);
                LOG("vr: whistle (both hands at the mouth)");
            }
        } else if (!grip[0] || !grip[1]) {
            g_whistled = false;
        }
    }
    for (int h = 0; h < 2; h++) {
        Slot slot = on ? slot_of(p, h) : kNone;
        if (slot == kMouth) slot = kNone;
        const bool pressed = grip[h] && !g_prev_grip[h];
        g_prev_grip[h] = grip[h];
        // an open quick menu: its d-pad direction held while the grip is, wherever the hand goes or if tracking is
        // lost; letting the grip go confirms the pick. Before the drop below: the right stick pulled down picks here
        if (g_menu_dir[h]) {
            if (grip[h]) {
                consumed[h] = 1;
                out->buttons |= g_menu_dir[h];
                continue;
            }
            if (g_log_budget_menu > 0)
                g_log_budget_menu--, LOG("vr: quick menu %s confirmed (%s grip let go)", g_slot_menu_name[h], h ? "right" : "left");
            g_menu_dir[h] = 0;
        }
        // the right grip with the right stick pulled down: drop what the hand holds (once a squeeze)
        if (!grip[h]) g_dropped[h] = false;
        if (h == 1 && grip[h] && in->ry < -0.7f && on && slot == kNone && g_slot_since[h] == 0) {
            consumed[h] = 1;
            out->set_ry = 1, out->ry = 0;
            if (!g_dropped[h] && vr_hand_item(h)) {
                g_dropped[h] = true;
                LOG("vr: right hand drops %s (grip and the stick down)", vr_actor_name(vr_hand_item(h)));
                vr_drop_hand_item(h);
                botw_vr_haptic(h, 0.7f, 0.12f);
            }
            continue;
        }
        // a grip down over a shoulder: an early let-go sends the deferred tap, a 0.4 s hold opens the quick menu
        if (g_slot_since[h] > 0) {
            if (!grip[h]) {
                if (g_slot_tap[h]) {
                    g_pulse = g_slot_tap[h], g_pulse_frames = 8;
                    LOG("vr: %s hand tap at a slot -> %s", h ? "right" : "left",
                        g_slot_tap[h] == input::BTN_B ? "B (put away)" : g_slot_tap[h] == input::BTN_Y ? "Y (weapon)"
                        : g_slot_tap[h] == input::BTN_L ? "L (the rune)" : "ZR (bow)");
                }
                g_slot_since[h] = 0, g_slot_tap[h] = 0;
            } else if (now_s() - g_slot_since[h] >= kQuickMenuHold) {
                g_slot_since[h] = 0, g_slot_tap[h] = 0;
                if (g_slot_menu[h] && !name_starts(vr_hand_item(1), "Item_Magnetglove")) {
                    g_menu_dir[h] = g_slot_menu[h];
                    botw_vr_haptic(h, 0.8f, 0.15f);
                    if (g_log_budget_menu > 0)
                        g_log_budget_menu--, LOG("vr: quick menu %s opened (%s grip held; right stick picks, let go confirms)",
                                                 g_slot_menu_name[h], h ? "right" : "left");
                    consumed[h] = 1;
                    out->buttons |= g_menu_dir[h];
                    continue;
                }
            }
        }
        if (slot == kRightWaist) {  // a long squeeze drops what the hand holds
            consumed[h] = 1;
            const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            if (!grip[h]) g_waist_since[h] = 0, g_waist_done[h] = false;
            else if (pressed) g_waist_since[h] = now;
            else if (g_waist_since[h] > 0 && !g_waist_done[h] && now - g_waist_since[h] > 0.6 && vr_hand_item(h)) {
                g_waist_done[h] = true;
                vr_drop_hand_item(h);
                botw_vr_haptic(h, 0.7f, 0.12f);
                LOG("vr: %s hand drops %s", h ? "right" : "left", vr_actor_name(vr_hand_item(h)));
            }
            continue;
        }
        g_waist_since[h] = 0;
        if (slot == kNone && g_slot_since[h] == 0) continue;
        consumed[h] = 1;
        if (!pressed) continue;
        if (slot != kLeftShoulder && slot != kRightShoulder && slot != kLeftWaist) continue;
        g_slot_since[h] = now_s(), g_slot_tap[h] = 0, g_slot_menu[h] = 0;
        uint32_t button = 0;
        bool defer = false;
        if (slot == kLeftWaist) {  // BetterVR's rune slot: a tap takes the Sheikah Slate out (L), a hold the runes' menu
            button = input::BTN_L;
            g_slot_menu[h] = input::BTN_UP;
            g_slot_menu_name[h] = "runes";
            defer = true;  // L at once would take the slate out before the menu
        } else if (slot == kRightShoulder) {
            const bool bow = name_starts(left_item, "Weapon_Bow");
            button = vr_melee_out() ? input::BTN_B : input::BTN_Y;
            g_slot_menu[h] = bow ? input::BTN_LEFT : input::BTN_RIGHT;
            g_slot_menu_name[h] = bow ? "arrows" : "weapons";
            defer = button == input::BTN_B || bow;  // Y at once would put the bow away before the arrows' menu
        } else {
            const bool sword = melee(vr_hand_item(1));
            button = input::BTN_ZR;
            g_slot_menu[h] = sword ? input::BTN_LEFT : input::BTN_RIGHT;
            g_slot_menu_name[h] = sword ? "shields" : "bows";
            defer = sword;  // ZR at once would swap the weapon for the bow before the shields' menu
        }
        botw_vr_haptic(h, 0.5f, 0.08f);
        if (defer) {
            g_slot_tap[h] = button;
            continue;
        }
        g_pulse = button, g_pulse_frames = 8;
        LOG("vr: %s hand grabs at the %s -> %s", h ? "right" : "left", kSlotName[slot],
            button == input::BTN_ZR ? "ZR (bow)" : "Y (weapon)");
    }
    // two-handed grip
    {
        const uint32_t weapon = on ? vr_hand_item(1) : 0;
        const bool two_hander = name_starts(weapon, "Weapon_Lsword") || name_starts(weapon, "Weapon_Spear");
        if (g_two_hand && (!grip[0] || !two_hander)) {
            g_two_hand = false;
            LOG("vr: two-handed grip let go");
        }
        static int prev_left_grip;
        const bool pressed = grip[0] && !prev_left_grip;
        prev_left_grip = grip[0];
        float l[3], r[3], lf[3], rf[3];
        if (!g_two_hand && pressed && !consumed[0] && two_hander && !vr_hand_item(0) && vr_hand_world(0, l, lf) &&
            vr_hand_world(1, r, rf)) {
            // the blade's line: from the right hand along the weapon's z axis (its actor matrix, row-major 3x4)
            float b[3] = {mem::rf32(weapon + 0x1f8 + 8), mem::rf32(weapon + 0x1f8 + 24), mem::rf32(weapon + 0x1f8 + 40)};
            const float bl = sqrtf(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
            const float v[3] = {l[0] - r[0], l[1] - r[1], l[2] - r[2]};
            const float sep = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (bl > 1e-4f && sep > 0.05f) {
                for (float& x : b) x /= bl;
                const float along = v[0] * b[0] + v[1] * b[1] + v[2] * b[2];
                const float off = sqrtf(std::fmax(sep * sep - along * along, 0.0f));
                if (off < 0.12f) {
                    g_two_hand = true;
                    botw_vr_haptic(0, 0.5f, 0.08f);
                    LOG("vr: two-handed grip on %s (%.2f m along the blade)", vr_actor_name(weapon), fabsf(along));
                }
            }
        }
        if (g_two_hand) consumed[0] = 1;
    }
    // the shield (BetterVR controls.cpp "Handle shield" / "Handle Parry gesture"): with a shield in the left hand and a
    // melee weapon in the right, the left hand raised to chest height guards (ZL, the right stick's y held at 0.2 so
    // the guard does not lock the camera on); the left trigger as ever guards with the lock-on. While guarding, a
    // push of the left hand away from the body parries (A).
    // Device 2026-11-05: the hand held at the chest buzzed on and on. The raw speed differenced between polls is noisy
    // enough to pass BetterVR's 35 m/s2 all the time, and the height threshold flickered: now the guard has 6 cm of
    // hysteresis and buzzes once when raised (not when lowered), the speed is smoothed (about 60 ms), and a parry needs
    // the hand moving forward (along the view) at 1.2 m/s or more, gaining 20 m/s2, at most once a second
    {
        static bool guarding = false;
        static float vel[3], prev_pos[3], prev_fwd_speed = 0;
        static double prev_t = 0, last_parry = 0;
        static int parry_frames = 0;
        const bool shield = on && name_starts(left_item, "Weapon_Shield");
        bool raised = guarding;
        if (shield && p.hand_tracked[0] >= 2) {
            const float head_y = p.origin_from_device[13];
            const float k = std::fmin(std::fmax((head_y - 1.60f) / 0.40f, 0.0f), 1.0f);
            const float chest = head_y - (0.40f + 0.20f * k);
            const float hand_y = p.origin_from_grip[0][13];
            raised = guarding ? hand_y > chest - 0.06f : hand_y > chest;
        } else {
            raised = false;
        }
        const bool guard = !in->trigger[0] && shield && melee(on ? vr_hand_item(1) : 0) && raised;
        if (guard != guarding) {
            guarding = guard;
            if (guard) botw_vr_haptic(0, 0.3f, 0.05f);
            LOG("vr: shield %s", guard ? "raised (guard)" : "lowered");
        }
        if (guard) out->buttons |= input::BTN_ZL, out->set_ry = 1, out->ry = 0.2f;
        const double t = now_s();
        const float* g = p.origin_from_grip[0];
        if (on && p.hand_tracked[0] >= 2 && prev_t > 0 && t - prev_t > 1e-3 && t - prev_t < 0.1) {
            const float dt = (float)(t - prev_t);
            const float a = std::fmin(dt / 0.06f, 1.0f);  // exponential smoothing, ~60 ms
            for (int i = 0; i < 3; i++) vel[i] += a * ((g[12 + i] - prev_pos[i]) / dt - vel[i]);
            const float* h = p.origin_from_device;
            float fx = -h[8], fz = -h[10];
            const float fl = sqrtf(fx * fx + fz * fz);
            const float fwd = fl > 1e-4f ? (vel[0] * fx + vel[2] * fz) / fl : 0;
            const float accel = (fwd - prev_fwd_speed) / dt;
            prev_fwd_speed = fwd;
            if (guarding && fwd > 1.2f && accel > 20.0f && t - last_parry > 1.0 && parry_frames == 0) {
                parry_frames = 6, last_parry = t;
                botw_vr_haptic(0, 0.6f, 0.08f);
                LOG("vr: parry (left hand forward %.1f m/s, %.0f m/s2)", fwd, accel);
            }
        } else {
            vel[0] = vel[1] = vel[2] = 0, prev_fwd_speed = 0;
        }
        if (on) prev_pos[0] = g[12], prev_pos[1] = g[13], prev_pos[2] = g[14], prev_t = t;
        if (parry_frames > 0) parry_frames--, out->buttons |= input::BTN_A;
    }
    // ladders: at the top the stick alone does not take Link off: he hops and falls back on the ladder. X gets him off,
    // but only with the stick released (device 2026-11-08: released then X climbed off, X with the stick held up did
    // not, and neither did an automatic X with the stick up, the press reaching the game each time). So with the stick
    // held up and Link falling back (or no rise for 0.35 s): the game sees the stick released, Link settles on the ladder, X, and the stick
    // comes back a moment after. The player only keeps pushing up.
    {
        enum { kIdle, kRelease, kPress, kAfter };
        static int phase = kIdle;
        static double at = 0, next = 0, since = -1;
        static float ref_y = 0;
        const uint32_t player = vr::player_actor();
        const double t = now_s();
        if (phase == kIdle) {
            if (on && player && vr_on_ladder() && input::gamepad().ly > 0.5f) {
                const float y = mem::rf32(player + 0x1f8 + 28);  // ActorWiiU::mtx, row-major: the height
                if (since < 0 || y > ref_y + 0.02f) ref_y = y, since = t;
                // the first fall of the hop at the top starts it (climbing with the stick up never goes down); waiting
                // for 0.35 s without a rise let the game hop twice first (device 2026-11-08, "shakes once or twice")
                const bool fell = y < ref_y - 0.08f;
                if ((fell || t - since > 0.35) && t >= next) {
                    phase = kRelease, at = t;
                    LOG("vr: ladder: stuck with the stick up (height %.2f, %s): releasing it for the game", y, fell ? "fell" : "no rise");
                }
            } else {
                since = -1;
            }
        }
        if (phase != kIdle) {
            out->release_left_stick = 1;
            const uint32_t flags = player ? mem::r32(player + 0x8dc) : 0;
            const bool on_rungs = (flags & 0x10) && !(flags & (0x80 | 0x400));
            const auto finish = [&] { phase = kIdle, next = t + 1.0, since = -1; };
            if (!on || !player) {
                finish();
            } else if (phase == kRelease) {
                if (t - at >= 0.13 && on_rungs) {  // the hop's fall takes about 0.1 s
                    phase = kPress, at = t;
                    LOG("vr: ladder: top: X with the stick released");
                } else if (t - at > 1.0) {
                    LOG("vr: ladder: top: Link did not settle on the ladder (flags %08x)", flags);
                    finish();
                }
            }
            if (phase == kPress) {
                out->buttons |= input::BTN_X;
                if (t - at >= 0.15) phase = kAfter, at = t;  // about as long as the press by hand in the log
            } else if (phase == kAfter && t - at >= 0.2) {
                finish();
            }
        }
    }
    // throwing the melee weapon in the right hand
    const uint32_t right_item = on ? vr_hand_item(1) : 0;
    {
        static uint32_t last;
        if (last && !right_item) LOG("vr: right hand empty (was %s)", vr_actor_name(last) ? vr_actor_name(last) : "?");
        last = right_item;
    }
    const bool rtrigger_pressed = in->trigger[1] && !g_prev_rtrigger;
    g_prev_rtrigger = in->trigger[1];
    if (g_throwing && !in->trigger[1]) {  // let go: the game throws
        g_throwing = false;
        if (g_log_budget_throw > 0) g_log_budget_throw--, LOG("vr: thrown (right trigger let go)");
    }
    const bool throwable = right_item && (mem::r32(right_item + 0x364) & 0x4000);
    {
        static uint32_t logged;
        if (right_item && right_item != logged)
            logged = right_item, LOG("vr: right hand item %s flags2 %08x%s", vr_actor_name(right_item),
                                     mem::r32(right_item + 0x364), throwable ? " (throwable)" : "");
    }
    if (!g_throwing && on && in->trigger[1] && throwable) {
        g_throwing = true;
        botw_vr_haptic(1, 0.4f, 0.1f);
        LOG("vr: throwing %s", vr_actor_name(right_item));
    }
    // a melee weapon: a fresh squeeze over the right shoulder (a trigger already held for ZR keeps being ZR)
    if (!g_throwing && on && rtrigger_pressed && throw_ready(p) && melee(right_item)) {
        g_throwing = true;
        botw_vr_haptic(1, 0.5f, 0.1f);
        if (g_log_budget_throw > 0)
            g_log_budget_throw--, LOG("vr: throw wind-up (R held) with %s, the right hand raised", vr_actor_name(right_item));
    }
    if (g_throwing) out->take_trigger[1] = 1, out->buttons |= input::BTN_R;
    // the headset marks where the throw will go while it is aimed (BotWImmersive.swift): the throw follows the view
    // (vr_aim.cpp), not the flat HUD's crosshair, whose place comes from the game camera the right stick pitches
    if (g_throw_aiming.exchange(g_throwing && on, std::memory_order_relaxed) != (g_throwing && on)) {
        static int logged;
        if (logged < 40)
            logged++, LOG("vr: throw aim %s (the headset marks where the view meets the world; right stick y %.2f)",
                          g_throwing && on ? "shown" : "hidden", in->ry);
    }
    // Magnesis holding an object (the glove is in the right hand only then): the right trigger is not A
    const bool magnet = on && name_starts(right_item, "Item_Magnetglove");
    g_magnet_hold.store(magnet, std::memory_order_relaxed);
    // the rune: the right trigger is A while the Sheikah Slate is in the left hand
    if (!g_throwing && on && slate && in->trigger[1] && !magnet) {
        static bool logged;
        if (!logged) logged = true, LOG("vr: right trigger uses the rune (A)");
        out->take_trigger[1] = 1, out->buttons |= input::BTN_A;
    }
    // Magnesis
    if (magnet) {
        // push / pull is the d-pad's up / down, which the Sense controllers do not have: the right stick's y with the
        // right trigger held (user 2026-11-13: a held object could not be moved with the stick); without the trigger
        // the stick is the game's: y up / down, x round Link (vr_turn.cpp leaves x to the game meanwhile)
        if (in->trigger[1]) {
            out->take_trigger[1] = 1;
            out->set_ry = 1, out->ry = 0;
            if (fabsf(in->ry) > 0.5f) out->buttons |= in->ry > 0 ? input::BTN_UP : input::BTN_DOWN;
            static bool logged;
            if (!logged && fabsf(in->ry) > 0.5f) logged = true, LOG("vr: Magnesis: right trigger + stick %s", in->ry > 0 ? "pushes" : "pulls");
        }
        if (g_magnet_hand >= 0 && !grip[g_magnet_hand]) g_magnet_hand = -1;
        for (int h = 0; h < 2 && g_magnet_hand < 0; h++)
            if (grip[h] && !consumed[h] && p.hand_tracked[h] >= 2) {
                g_magnet_hand = h, g_magnet_wait = 0;
                for (int i = 0; i < 3; i++) g_magnet_from[i] = p.origin_from_grip[h][12 + i];
                botw_vr_haptic(h, 0.5f, 0.08f);
            }
        if (g_magnet_hand >= 0) {
            const int h = g_magnet_hand;
            consumed[h] = 1;
            const float* g = p.origin_from_grip[h];
            const float d[3] = {g[12] - g_magnet_from[0], g[13] - g_magnet_from[1], g[14] - g_magnet_from[2]};
            float fx = -p.origin_from_device[8], fz = -p.origin_from_device[10];
            const float fl = sqrtf(fx * fx + fz * fz);
            if (fl > 1e-4f) fx /= fl, fz /= fl;
            const float forward = remap(fx * d[0] + fz * d[2]), up = remap(d[1]);
            out->set_ry = 1, out->ry = up;
            // the grip held with the right stick pushed forward / back pushes / pulls too, as with the right trigger
            // (user 2026-11-13: tried the right grip + stick, which the hand's height used to take whole)
            if (h == 1 && fabsf(in->ry) > 0.5f) {
                out->buttons |= in->ry > 0 ? input::BTN_UP : input::BTN_DOWN;
                static bool logged;
                if (!logged) logged = true, LOG("vr: Magnesis: grip + stick %s", in->ry > 0 ? "pushes" : "pulls");
            } else if (forward != 0) {
                if (g_magnet_wait <= 0) {
                    out->buttons |= forward > 0 ? input::BTN_UP : input::BTN_DOWN;
                    g_magnet_wait = pulse_interval(fabsf(forward));
                } else {
                    g_magnet_wait--;
                }
            }
        }
    } else {
        g_magnet_hand = -1;
    }
    if (g_pulse_frames > 0) out->buttons |= g_pulse;
}

// vr_turn.cpp: Magnesis holds an object, so the right stick's x is the game's (the object goes round Link), not a turn
extern "C" int botw_vr_magnet_hold(void) { return g_magnet_hold.load(std::memory_order_relaxed) ? 1 : 0; }

extern "C" int botw_vr_left_bow(void) {  // sense_vision.mm: Square swaps the bow for the melee weapon
    return name_starts(vr_hand_item(0), "Weapon_Bow") && !g_bow_trigger.load(std::memory_order_relaxed) ? 1 : 0;
}

bool vr_two_hand() { return g_two_hand.load(std::memory_order_relaxed); }  // vr_body.cpp, vr_weapon.cpp
bool vr_bow_trigger() { return g_bow_trigger.load(std::memory_order_relaxed); }  // vr_body.cpp

// BotWImmersive.swift: a throw being aimed (R held for it), for the mark where the view meets the world
extern "C" int botw_vr_throw_aiming(void) {
    return g_throw_aiming.load(std::memory_order_relaxed) && vr_first_person() ? 1 : 0;
}
