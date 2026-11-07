// VR first person: turning (BetterVR hooking/camera.cpp "yaw hold", hooking/controls.cpp snap turn,
// patch_CTRL_CameraControls.asm). In first person the view's heading is held here instead of taken from the game
// camera: the game's CameraChase drifts round behind Link as he walks and swings to his back on the shield button, and
// on a headset any turn the player did not ask for is what makes people sick. The held heading changes only by
//  - the turn the game derives from the right stick (CameraChase 0x02b9c96c, CameraTail 0x02beebcc), in smooth turning;
//  - snap turns: the right stick's x is kept from the game and each push turns by a fixed angle (30 / 45 / 60 degrees).
// The game camera is then put back on the held heading (the camera turned round its target: CameraChase 0x02b97c78,
// CameraTail before 0x02beee48 / 0x02bef014), so Link still walks where the player looks.
// In camera modes these hooks do not reach (bow, scope, paraglider...) and outside first person or during events, the
// heading follows the game camera as before.
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "../input/input.h"
bool vr_event_running();  // vr_render.cpp
#include "hooks.h"
#include "vr.h"

using mem::rf32;
using mem::wf32;

extern "C" int botw_vr_view_now(void);  // vr_render.cpp
bool vr_first_person();                 // vr_render.cpp

namespace {
std::atomic<int> g_turn{1};               // 0 smooth (the game's own stick turning), 1..3 snap 30 / 45 / 60 degrees
std::atomic<int> g_snap{0};               // snap turns asked for by the stick (host input thread), +1 right / -1 left
std::atomic<bool> g_holding{false};       // the heading is held (first person, a camera mode the hooks reach)
std::atomic<bool> g_pivot_seen{false};    // a camera pivot hook ran since the last frame

std::mutex g_lock;  // the state below: the camera hooks and the render camera
bool g_have_held = false;
float g_held = 0;          // degrees about +y, counter-clockwise (a turn to the right lowers it)
float g_game = 0;          // the game camera's heading this frame (degrees)
float g_stick = 0;         // the game's stick turning since the last frame
float g_correction = 0;    // game camera heading - held heading, for the next pivot hook to take away
uint32_t g_last_seq = ~0u;
int g_missed = 0;          // frames in a row without a pivot hook
float g_magnet_pending = 0;  // Magnesis: the game camera's turn the view has not yet jumped by (snap turning)

float wrap_deg(float d) {
    d = fmodf(d + 180.0f, 360.0f);
    return d < 0 ? d + 180.0f : d - 180.0f;
}
int snap_degrees(int mode) { return mode == 1 ? 30 : mode == 2 ? 45 : mode == 3 ? 60 : 0; }

float take_correction() {
    std::lock_guard<std::mutex> l(g_lock);
    const float d = g_correction;
    g_correction = 0;
    return d;
}

void add_stick(float d) {
    if (d == 0 || !std::isfinite(d) || !g_holding.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> l(g_lock);
    g_stick = wrap_deg(g_stick + d);
}
}  // namespace

// The heading the first-person view uses this frame, given the game camera's (radians, the same sense). Called for
// each eye; the held heading moves once per frame (seq: vr_render.cpp's frame counter).
extern "C" int botw_vr_magnet_hold(void);  // vr_gesture.cpp
float vr_turn_heading(float game_rad, uint32_t seq) {
    float game = game_rad * 180.0f / (float)M_PI;
    std::lock_guard<std::mutex> l(g_lock);
    // a game camera that is not a number (item 101) keeps the heading it had: taken as it came, the correction below
    // became NaN and the pivot hooks wrote it into the camera for good
    if (!std::isfinite(game)) game = g_game;
    if (seq != g_last_seq) {
        g_last_seq = seq;
        g_missed = g_pivot_seen.exchange(false, std::memory_order_relaxed) ? 0 : g_missed + 1;
        // and while ZL is held (guarding, locking on): the game's lock-on camera is a mode the pivot hooks do not reach,
        // and followed, it swung the view round (device 2026-11-03: the view behind Link as the shield came up)
        const bool guard = (input::gamepad().buttons & input::BTN_ZL) != 0;
        // Held in all first-person play, as BetterVR (camera.cpp ShouldAdoptGameCameraYaw: only without a base yaw,
        // outside first person, out of the game or in a cutscene). It used to need the pivot hooks seen in the last
        // three frames; after the Sheikah Slate came out the camera stayed in a mode they do not reach and the view
        // followed the game camera for the rest of the session, its lock-on swings included (device 2026-11-06: fights
        // swayed and made the player dizzy). Where the hooks do not put the game camera back on the held heading,
        // the left stick is turned by the difference instead (vr_turn_view_offset, vr_render.cpp), so Link still
        // walks where the player looks
        // and an event of any kind follows the game's camera, as BetterVR in a cutscene (Fable's review: the events
        // played in the view kept the old heading through their cuts)
        (void)guard;
        const bool hold = botw_vr_view_now() && vr_first_person() && vr::player_actor() && !vr_event_running();
        // Magnesis holding an object: the right stick's x goes to the game (botw_vr_turn_stick), whose camera then
        // swings the object round Link; that camera is one the pivot hooks do not keep on the held heading, so the
        // held heading follows its turn here and the view turns with the object (in snap-angle jumps with snap turning)
        // (user 2026-11-17: seated, there was no way to turn with an object held; BetterVR turns with the stick too)
        const float game_turn = std::isfinite(g_game) ? wrap_deg(game - g_game) : 0.0f;
        g_game = game;
        // an event played in the view (a talk, picking something up): its camera pans round the speakers, and
        // followed smoothly it swung the whole view left and right (user 2026-11-17, talking to the old man: dizzy).
        // The view keeps its heading through small pans and jumps at once, as a snap turn, only when the event's
        // camera looks more than BOTW_VR_EVENT_SNAP degrees (60) away; a negative value follows smoothly as before
        static const float event_snap = [] { const char* e = getenv("BOTW_VR_EVENT_SNAP"); return e ? (float)atof(e) : 60.0f; }();
        const bool event_view = botw_vr_view_now() && vr_first_person() && vr::player_actor() && vr_event_running();
        if (event_view && event_snap >= 0 && g_have_held) {
            const float d = wrap_deg(game - g_held);
            if (fabsf(d) > event_snap) {
                g_held = game;
                LOG("vr: event camera %.0f deg away: the view jumps to it (heading %.1f)", d, g_held);
            }
            g_stick = 0;
            g_snap.store(0, std::memory_order_relaxed);
        } else if (!hold || !g_have_held) {
            g_held = game;
            g_have_held = hold;
            g_stick = 0;
            g_snap.store(0, std::memory_order_relaxed);
        } else {
            const int snaps = g_snap.exchange(0, std::memory_order_relaxed);
            float magnet_turn = botw_vr_magnet_hold() && fabsf(game_turn) < 20.0f ? game_turn : 0.0f;
            // with snap turning, the view follows the object in jumps of the snap angle, as the event camera: the
            // object goes round in the view and the view jumps after it (user 2026-11-17: turned smoothly with the
            // object, at the game's frame rate, it was not smooth and made the player dizzy). Smooth turning follows
            // every frame as before
            const int snap = snap_degrees(g_turn.load(std::memory_order_relaxed));
            if (!botw_vr_magnet_hold()) {
                g_magnet_pending = 0;
            } else if (snap) {
                g_magnet_pending += magnet_turn;
                magnet_turn = 0;
                if (fabsf(g_magnet_pending) >= (float)snap) {
                    magnet_turn = g_magnet_pending > 0 ? (float)snap : -(float)snap;
                    g_magnet_pending -= magnet_turn;
                    LOG("vr: Magnesis: the view jumps %+.0f deg after the object", magnet_turn);
                }
            }
            g_held = wrap_deg(g_held + g_stick + magnet_turn - (float)(snaps * snap));
            g_stick = 0;
            if (snaps) LOG("vr: snap turn %+d, heading %.1f (game camera %.1f)", snaps, g_held, game);
        }
        g_correction = hold ? wrap_deg(game - g_held) : 0;
        if (hold != g_holding.load(std::memory_order_relaxed)) {
            LOG("vr: heading %s", hold ? "held (turn with the right stick)" : "follows the game camera");
            g_holding.store(hold, std::memory_order_relaxed);
        }
    }
    return g_held * (float)M_PI / 180.0f;
}

// vr_render.cpp's move stick: how far right of the game camera the held view looks (radians), 0 when not holding
float vr_turn_view_offset() {
    std::lock_guard<std::mutex> l(g_lock);
    if (!g_holding.load(std::memory_order_relaxed)) return 0;
    return wrap_deg(g_game - g_held) * (float)M_PI / 180.0f;
}

// The heading the view used last (radians): the base the controllers are placed on (vr_body.cpp)
float vr_turn_last_heading() {
    std::lock_guard<std::mutex> l(g_lock);
    return g_held * (float)M_PI / 180.0f;
}

// Host input (window.mm), every poll: the right stick's x as the game should see it. With snap turning while the
// heading is held, a push past half way is one turn, and the stick must come back near the middle before the next.
extern "C" int botw_vr_pause_menu(void);  // vr_render.cpp
extern "C" int botw_vr_in_play(void);     // vr_render.cpp
extern "C" int botw_vr_magnet_hold(void); // vr_gesture.cpp
extern "C" int botw_vr_slate_map(void);   // vr_render.cpp
extern "C" float botw_vr_turn_stick(float x) {
    if (!g_holding.load(std::memory_order_relaxed) || g_turn.load(std::memory_order_relaxed) == 0) return x;
    // the Sheikah Slate's map: it is drawn in the view with the heading still held (the pause menu's own screen is
    // closed there), and a push of the stick turned the view behind the map (device 2026-11-13). The stick is the map's
    if (botw_vr_slate_map()) return x;
    // the pause menu: the stick's x is the game's, which turns the inventory's category (weapons, bows, food...) with
    // it; snap turning took it there too (device 2026-11-05: no right stick bit reached the game in the menu)
    if (botw_vr_pause_menu() && !botw_vr_in_play()) return x;  // never in play (see sense_vision.mm)
    static bool latched = false;
    // Magnesis holding an object: the stick moves the object round Link as on a TV, the view stays where it is
    // (user 2026-11-13); latched, so a stick still pushed when the object is let go does not turn the view
    if (botw_vr_magnet_hold()) {
        latched = fabsf(x) >= 0.25f;
        return x;
    }
    if (latched) {
        if (fabsf(x) < 0.25f) latched = false;
    } else if (fabsf(x) > 0.5f) {
        g_snap.fetch_add(x > 0 ? 1 : -1, std::memory_order_relaxed);
        latched = true;
    }
    return 0;
}

extern "C" void botw_vr_set_turn(int mode) {
    // BOTW_VR_TURN (smooth, 30, 45, 60) overrides the menu's choice for a run (tests)
    static const int forced = [] {
        const char* e = getenv("BOTW_VR_TURN");
        if (!e) return -1;
        const int d = atoi(e);
        return d == 30 ? 1 : d == 45 ? 2 : d == 60 ? 3 : 0;
    }();
    g_turn.store(forced >= 0 ? forced : mode, std::memory_order_relaxed);
}

// CameraChase adds the frame's stick turn (at 0x194(r1)) to its heading: lfs f12, 0x194(r1)
BOTW_HOOK(0x02b9c96c) {
    add_stick((float)c->f[12]);
}
// CameraTail adds f28 * f24 to its heading (the fmadds after the next call): lfs f1, 0x98(r31)
BOTW_HOOK(0x02beebcc) {
    add_stick((float)(c->f[28] * c->f[24]));
}

// Item 101 (device 2026-11-13, Mac the same night: the root of the freezes of item 98). The game camera's place became
// NaN and stayed NaN for the rest of the session: every physics query made from it (the three camera queries, rays
// from other callers) then walked Havok's trees leaf by leaf, seconds each, wherever the first-person skips did not
// apply (the map and the pause menu on the screen, events). It starts in CameraChase's "lead" (+0x80, the offset of
// its anchor ahead of Link, eased towards his velocity by at most speed * delta a frame, 0x02b9b4f4..0x02b9b838): the
// speed is sqrt(vx * vx + vz * vz) by frsqrte and one Newton step, taken whenever the sum is above 0. When Link comes
// to rest in water his velocity decays through 1e-21 m a frame, the sum is then a denormal single (1.6e-42 on the
// Mac), the estimate squared overflows, the "speed" is -inf, the step -inf, and the lead (-inf, NaN, -inf); the anchor
// (0x02b97834 adds the lead), the camera and its target follow, and the lead eases from its own NaN ever after. The
// recompiled code keeps denormals in scalar arithmetic (only the paired-single results are flushed, ppc.h); the
// console presumably never sees one here (its FPU's non-IEEE mode flushes such a product to 0, and the test "above
// 0" then fails; not checked on hardware). So here the sum is flushed: below the smallest normal single it is 0.
// fmadds f9, f6, f6, f0 (f9 = vx * vx + vz * vz), compared with 0 next. BOTW_CHASE_LEAD_FIX=0 / BOTW_CHASE_HEAL=0
// (tests): without this / without the net below
static bool env_on(const char* name) {
    const char* e = getenv(name);
    return !(e && strcmp(e, "0") == 0);
}
BOTW_HOOK(0x02b9b684) {
    static const bool on = env_on("BOTW_CHASE_LEAD_FIX");
    if (!on || c->f[9] >= (double)FLT_MIN) return;  // a NaN goes to 0 as well: no speed, no step
    if (c->f[9] != 0) {
        static std::atomic<int> budget{3};
        if (budget.fetch_sub(1, std::memory_order_relaxed) > 0)
            LOG("camera: chase lead: speed squared %g (velocity %g %g) taken as 0", c->f[9], c->f[6], c->f[27]);
    }
    c->f[9] = c->ps1[9] = 0.0;
}

// The net under it, at the end of buildAnchorWorldPos (below): a lead that is not finite is put back to 0 and an
// anchor that is not finite back to its base (+0x74, the place before the lead is added), so one bad frame cannot
// stay. r29 = the camera, r30 = the anchor just built
static void chase_heal(uint32_t self, uint32_t pos) {
    static const bool on = env_on("BOTW_CHASE_HEAL");
    if (!on || !self || !pos) return;
    bool lead = true, place = true, base = true;
    for (int i = 0; i < 3; i++) {
        lead = lead && std::isfinite(rf32(self + 0x80 + 4 * i));
        place = place && std::isfinite(rf32(pos + 4 * i));
        base = base && std::isfinite(rf32(self + 0x74 + 4 * i));
    }
    if (lead && place) return;
    static std::atomic<int> budget{5};
    if (budget.fetch_sub(1, std::memory_order_relaxed) > 0)
        LOG("camera: chase anchor not finite (%f %f %f, lead %f %f %f, base %f %f %f): the lead is put back to 0", rf32(pos),
            rf32(pos + 4), rf32(pos + 8), rf32(self + 0x80), rf32(self + 0x84), rf32(self + 0x88), rf32(self + 0x74),
            rf32(self + 0x78), rf32(self + 0x7c));
    if (!lead)
        for (int i = 0; i < 3; i++) wf32(self + 0x80 + 4 * i, 0.0f);
    if (!place && base)
        for (int i = 0; i < 3; i++) wf32(pos + 4 * i, rf32(self + 0x74 + 4 * i));
}

// CameraChase::buildAnchorWorldPos's last write of the camera's place (stfs f12, 4(r30)): r30 the camera's world
// place, r31 its spherical placement round the target (distance, latitude, heading in degrees). Turned round the
// target by the correction (BetterVR ApplyYawDeltaToComputedCameraPivot).
BOTW_HOOK(0x02b97c78) {
    const uint32_t pos = c->r[30], sph = c->r[31];
    chase_heal(c->r[29], pos);  // in every mode, VR or not
    if (!botw_vr_view_now()) return;
    g_pivot_seen.store(true, std::memory_order_relaxed);
    const float d = take_correction();
    if (d == 0 || !std::isfinite(d) || !pos || !sph) return;
    auto offset = [](float r, float lat, float yaw, float out[3]) {  // camera -> target
        const float la = lat * (float)M_PI / 180, ya = yaw * (float)M_PI / 180, h = r * cosf(la);
        out[0] = h * sinf(ya), out[1] = r * sinf(la), out[2] = h * cosf(ya);
    };
    const float r = rf32(sph), lat = rf32(sph + 4), yaw = rf32(sph + 8);
    if (!(r > 1e-4f) || !std::isfinite(lat) || !std::isfinite(yaw)) return;
    float before[3], after[3];
    offset(r, lat, yaw, before);
    const float turned = wrap_deg(yaw - d);
    offset(r, lat, turned, after);
    float place[3];
    for (int i = 0; i < 3; i++) place[i] = rf32(pos + 4 * i) + before[i] - after[i];
    // nothing but finite numbers into the game's camera
    if (!std::isfinite(turned) || !std::isfinite(place[0]) || !std::isfinite(place[1]) || !std::isfinite(place[2])) return;
    for (int i = 0; i < 3; i++) wf32(pos + 4 * i, place[i]);
    wf32(sph + 8, turned);
}

// CameraTail, just before it converts its spherical placement (bl 0x02e55864 with r4 = the placement): its heading at
// +8 and its own copy at 0x98(r31) turned by the correction (BetterVR ApplyYawDeltaToCameraTailPivot)
void tail_pivot(PPCContext* c) {
    if (!botw_vr_view_now()) return;
    g_pivot_seen.store(true, std::memory_order_relaxed);
    const float d = take_correction();
    if (d == 0 || !std::isfinite(d) || !c->r[4] || !c->r[31]) return;
    const float yaw = rf32(c->r[4] + 8), own = rf32(c->r[31] + 0x98);
    if (std::isfinite(yaw)) wf32(c->r[4] + 8, wrap_deg(yaw - d));
    if (std::isfinite(own)) wf32(c->r[31] + 0x98, wrap_deg(own - d));
}
BOTW_HOOK(0x02beee44) { tail_pivot(c); }  // stfs f1, 16(r1)
BOTW_HOOK(0x02bef010) { tail_pivot(c); }  // stfs f1, 16(r1)

// The shield button swings the camera behind Link (mr r3, r31 -> the recentre request): off while the heading is held
// (BetterVR: 0x02B96E10 = li r3, 0)
BOTW_HOOK(0x02b96e10) {
    if (g_holding.load(std::memory_order_relaxed)) c->r[3] = 0;
}
