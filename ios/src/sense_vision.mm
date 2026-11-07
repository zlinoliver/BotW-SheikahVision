// PlayStation VR2 Sense controllers on Apple Vision Pro, read straight from GameController (SDL sees the merged pair as a
// gamepad whose only button is Options). Ported from MacNativeGames' runtime/gamepad.m (visionOS branch): each hand is
// read from every object the system publishes for it, its own spatial controller and the merged pair "PlayStation VR2
// Sense Controllers (L/R)", whose full input is only in GCController.input; sense_controller.h maps the pair onto one
// gamepad. Called once per host frame from apply_input (runtime/src/host/window.mm), on the main thread.
#include <TargetConditionals.h>
#if TARGET_OS_VISION
#import <CoreHaptics/CoreHaptics.h>
#import <GameController/GameController.h>

#include <math.h>
#include <string.h>
#include <time.h>

#include <utility>

#include "input/input.h"
#include "log.h"
#include "sense_controller.h"

extern "C" void botw_vr_haptic(int hand, float strength, float seconds);  // below

#include "game/vr_gesture.h"  // the VR gestures (runtime/src/game/vr_gesture.cpp)

namespace {
SenseChord g_chord;
// Haptics: each hand's own spatial controller (seen in the last sense_vision_apply) and its engine; main thread only
GCController* g_hand_controller[2];
CHHapticEngine* g_engine[2];
id<CHHapticPatternPlayer> g_rumble_player[2];

CHHapticEngine* engine(int hand) {
    GCController* c = g_hand_controller[hand];
    if (!c || !c.haptics) return nil;
    if (!g_engine[hand]) {
        g_engine[hand] = [c.haptics createEngineWithLocality:GCHapticsLocalityDefault];
        NSError* err = nil;
        if (![g_engine[hand] startAndReturnError:&err]) {
            LOG("sense: haptics engine (%s hand) failed: %s", hand ? "right" : "left", err.localizedDescription.UTF8String);
            g_engine[hand] = nil;
        }
        g_engine[hand].resetHandler = ^{ [g_engine[hand] startAndReturnError:nil]; };
    }
    return g_engine[hand];
}

// A continuous buzz of the given strength (0..1) and length on one hand; returns the player (to stop it early)
id<CHHapticPatternPlayer> buzz(int hand, float strength, double seconds) {
    CHHapticEngine* e = engine(hand);
    if (!e) return nil;
    CHHapticEvent* ev = [[CHHapticEvent alloc]
        initWithEventType:CHHapticEventTypeHapticContinuous
               parameters:@[ [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity
                                                                            value:strength],
                             [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness
                                                                            value:0.4f] ]
             relativeTime:0
                 duration:seconds];
    NSError* err = nil;
    CHHapticPattern* pattern = [[CHHapticPattern alloc] initWithEvents:@[ ev ] parameters:@[] error:&err];
    id<CHHapticPatternPlayer> player = pattern ? [e createPlayerWithPattern:pattern error:&err] : nil;
    [player startAtTime:CHHapticTimeImmediate error:&err];
    return player;
}

bool is_spatial(GCController* c) {
    return !c.extendedGamepad && [c.productCategory isEqualToString:GCProductCategorySpatialController];
}
// "PlayStation VR2 Sense Controllers (L/R)"; not just "Sense": a PS5 controller is "DualSense Wireless Controller"
bool is_merged(GCController* c) {
    NSString* name = c.vendorName ?: @"";
    return c.extendedGamepad && ([name rangeOfString:@"VR2" options:NSCaseInsensitiveSearch].location != NSNotFound ||
                                 [name rangeOfString:@"Sense Controller" options:NSCaseInsensitiveSearch].location !=
                                     NSNotFound);
}

bool live_pressed(GCControllerLiveInput* in, NSArray<NSString*>* names) {
    for (NSString* n in names)
        if (in.buttons[(GCInputButtonName)n].pressedInput.isPressed) return true;
    return false;
}
void live_stick(GCControllerLiveInput* in, NSString* name, float* x, float* y) {
    GCPoint2 v = in.dpads[(GCInputDirectionPadName)name].xyAxes.value;
    *x = v.x, *y = v.y;
}
bool profile_pressed(NSDictionary<NSString*, GCControllerButtonInput*>* b, NSArray<NSString*>* names) {
    for (NSString* n in names)
        if (b[n].isPressed) return true;
    return false;
}

// Diagnostics (device, 2026-11-01: the left controller's buttons never arrived): whenever the pressed elements of a
// spatial controller change, the names the system reports for them, from its profile and its live input
void log_pressed(GCController* c) {
    NSMutableArray<NSString*>* down = [NSMutableArray array];
    [c.physicalInputProfile.buttons enumerateKeysAndObjectsUsingBlock:^(NSString* k, GCControllerButtonInput* b, BOOL*) {
        if (b.isPressed) [down addObject:[NSString stringWithFormat:@"p:%@ %.2f", k, b.value]];
        else if (b.isTouched) [down addObject:[NSString stringWithFormat:@"t:%@", k]];  // the thumb resting on it
    }];
    if (GCControllerLiveInput* in = c.input)
        for (id<GCButtonElement> b in in.buttons)
            if (b.pressedInput.isPressed) [down addObject:[@"l:" stringByAppendingString:b.localizedName ?: @"?"]];
    NSString* now = [down componentsJoinedByString:@", "];
    static NSMutableDictionary<NSString*, NSString*>* last;
    if (!last) last = [NSMutableDictionary dictionary];
    NSString* key = c.vendorName ?: @"?";
    if ([now isEqualToString:last[key] ?: @""]) return;
    last[key] = now;
    LOG("sense: %s pressed [%s] (profile has %lu buttons)", key.UTF8String, now.UTF8String,
        (unsigned long)c.physicalInputProfile.buttons.count);
}

// One hand's own spatial controller: its physical input profile and its live input together.
SenseHalf read_spatial(GCController* c) {
    log_pressed(c);
    GCPhysicalInputProfile* p = c.physicalInputProfile;
    NSDictionary<NSString*, GCControllerButtonInput*>* b = p.buttons;
    GCControllerDirectionPad* stick =
        p.dpads[GCInputThumbstick] ?: p.dpads[GCInputLeftThumbstick] ?: p.dpads[GCInputRightThumbstick];
    SenseHalf h = {};
    h.side = sense_side(b[GCInputButtonA] != nil, b[GCInputButtonB] != nil, b[GCInputButtonX] != nil,
                        b[GCInputButtonY] != nil, c.vendorName.UTF8String);
    h.x = stick.xAxis.value, h.y = stick.yAxis.value;
    h.stick = profile_pressed(b, @[ GCInputThumbstickButton, GCInputLeftThumbstickButton, GCInputRightThumbstickButton ]);
    h.trigger = profile_pressed(b, @[ GCInputTrigger, GCInputLeftTrigger, GCInputRightTrigger ]);
    h.grip = profile_pressed(b, @[ GCInputGripButton, GCInputLeftShoulder, GCInputRightShoulder ]);
    h.a = b[GCInputButtonA].isPressed, h.b = b[GCInputButtonB].isPressed;
    h.x_button = b[GCInputButtonX].isPressed, h.y_button = b[GCInputButtonY].isPressed;
    h.system = profile_pressed(b, @[ GCInputButtonMenu, GCInputButtonOptions, GCInputButtonShare ]);
    if (GCControllerLiveInput* in = c.input) {
        float x = 0, y = 0;
        live_stick(in, GCInputThumbstick, &x, &y);
        if (fabsf(x) > fabsf(h.x)) h.x = x;
        if (fabsf(y) > fabsf(h.y)) h.y = y;
        h.stick |= live_pressed(in, @[ GCInputThumbstickButton ]);
        h.trigger |= live_pressed(in, @[ GCInputTrigger ]);
        h.grip |= live_pressed(in, @[ GCInputGripButton ]);
        h.a |= live_pressed(in, @[ GCInputButtonA ]), h.b |= live_pressed(in, @[ GCInputButtonB ]);
        h.x_button |= live_pressed(in, @[ GCInputButtonX ]), h.y_button |= live_pressed(in, @[ GCInputButtonY ]);
        h.system |= live_pressed(in, @[ GCInputButtonMenu, GCInputButtonOptions, GCInputButtonShare ]);
    }
    // the left half's A / B are its Square / Triangle (log_pressed: "p:Button A, l:Square Button")
    if (h.side == SENSE_LEFT) h.x_button |= h.a, h.y_button |= h.b, h.a = h.b = 0;
    return h;
}

// The merged pair, split back into its halves.
void read_merged(GCController* c, SenseHalf* l, SenseHalf* r) {
    GCControllerLiveInput* in = c.input;
    *l = SenseHalf{.side = SENSE_LEFT}, *r = SenseHalf{.side = SENSE_RIGHT};
    if (!in) return;
    live_stick(in, GCInputLeftThumbstick, &l->x, &l->y);
    live_stick(in, GCInputRightThumbstick, &r->x, &r->y);
    l->stick = live_pressed(in, @[ GCInputLeftThumbstickButton ]);
    r->stick = live_pressed(in, @[ GCInputRightThumbstickButton ]);
    l->trigger = live_pressed(in, @[ GCInputLeftTrigger ]);
    r->trigger = live_pressed(in, @[ GCInputRightTrigger ]);
    l->grip = live_pressed(in, @[ GCInputLeftShoulder, GCInputLeftBumper, @"Left Grip", @"Left Grip Button" ]);
    r->grip = live_pressed(in, @[ GCInputRightShoulder, GCInputRightBumper, @"Right Grip", @"Right Grip Button" ]);
    r->a = live_pressed(in, @[ GCInputButtonA ]), r->b = live_pressed(in, @[ GCInputButtonB ]);
    l->x_button = live_pressed(in, @[ GCInputButtonX ]), l->y_button = live_pressed(in, @[ GCInputButtonY ]);
    l->system = live_pressed(in, @[ GCInputButtonOptions, GCInputButtonShare ]);  // Create
    r->system = live_pressed(in, @[ GCInputButtonMenu ]);                          // Options
}

// sense_controller.h's physical bits -> Wii U GamePad buttons (positions as on a Nintendo pad: east = A, south = B)
const std::pair<uint64_t, uint32_t> kMap[] = {
    {SENSE_SOUTH, input::BTN_B},     {SENSE_EAST, input::BTN_A},      {SENSE_WEST, input::BTN_Y},
    {SENSE_NORTH, input::BTN_X},     {SENSE_L3, input::BTN_STICK_L},  {SENSE_R3, input::BTN_STICK_R},
    {SENSE_L1, input::BTN_L},        {SENSE_R1, input::BTN_R},        {SENSE_L2, input::BTN_ZL},
    {SENSE_R2, input::BTN_ZR},       {SENSE_MENU, input::BTN_PLUS},   {SENSE_VIEW, input::BTN_MINUS},
    {SENSE_LEFT_DIR, input::BTN_LEFT}, {SENSE_UP, input::BTN_UP},     {SENSE_RIGHT_DIR, input::BTN_RIGHT},
    {SENSE_DOWN, input::BTN_DOWN},
};

// The controllers the system publishes, logged when the list changes: what a device test needs to see.
void log_controllers() {
    static NSString* last;
    NSMutableArray<NSString*>* rows = [NSMutableArray array];
    for (GCController* c in GCController.controllers)
        [rows addObject:[NSString stringWithFormat:@"%@ (%@%@)", c.vendorName ?: @"?", c.productCategory ?: @"?",
                                                   is_spatial(c) ? @", spatial" : is_merged(c) ? @", merged pair" : @""]];
    NSString* now = [rows componentsJoinedByString:@"; "];
    if ([now isEqualToString:last ?: @""]) return;
    last = now;
    LOG("sense: controllers: %s", now.length ? now.UTF8String : "none");
}

void log_state(uint32_t buttons, const float s[4]) {
    static uint32_t last_buttons = ~0u;
    static int last_q = 0, budget = 300;
    int q = (int)lroundf(s[0] * 2) * 1000 + (int)lroundf(s[1] * 2) * 100 + (int)lroundf(s[2] * 2) * 10 +
            (int)lroundf(s[3] * 2);
    if ((buttons == last_buttons && q == last_q) || budget <= 0) return;
    last_buttons = buttons, last_q = q, budget--;
    LOG("sense: to game buttons %05x left (%.2f, %.2f) right (%.2f, %.2f)", buttons, s[0], s[1], s[2], s[3]);
}
}  // namespace

// Adds the Sense pair to s (buttons OR-ed, each stick axis the stronger value); false when no Sense controller is there.
bool sense_vision_apply(input::PadState& s) {
    log_controllers();
    SenseHalf halves[2] = {{.side = SENSE_LEFT}, {.side = SENSE_RIGHT}};
    bool have[2] = {false, false};
    for (GCController* c in GCController.controllers) {
        if (!is_spatial(c)) continue;
        SenseHalf h = read_spatial(c);
        int i = h.side == SENSE_LEFT ? 0 : h.side == SENSE_RIGHT ? 1 : -1;
        if (i >= 0) sense_merge_half(&halves[i], &h), have[i] = true;
        if (i >= 0 && g_hand_controller[i] != c) g_hand_controller[i] = c, g_engine[i] = nil, g_rumble_player[i] = nil;
    }
    uint64_t physical = 0;
    for (GCController* c in GCController.controllers) {
        if (!is_merged(c)) continue;
        SenseHalf l, r;
        read_merged(c, &l, &r);
        sense_merge_half(&halves[0], &l), sense_merge_half(&halves[1], &r);
        have[0] = have[1] = true;
        id<GCDirectionPadElement> dpad = c.input.dpads[GCInputDirectionPad];  // the pair also publishes one
        if (dpad.left.isPressed) physical |= SENSE_LEFT_DIR;
        if (dpad.up.isPressed) physical |= SENSE_UP;
        if (dpad.right.isPressed) physical |= SENSE_RIGHT_DIR;
        if (dpad.down.isPressed) physical |= SENSE_DOWN;
    }
    // VR gestures (runtime/src/game/vr_gesture.cpp): grips and triggers they use (a body slot, a throw, Magnesis) do not
    // do their usual job
    BotWVRGestureIn gin = {{halves[0].grip, halves[1].grip}, {halves[0].trigger, halves[1].trigger},
                           s.buttons | (halves[0].trigger ? input::BTN_ZL : 0u), halves[1].y};
    BotWVRGestureOut gout;
    botw_vr_gestures(&gin, &gout);
    const uint32_t gesture_buttons = gout.buttons;
    for (int i = 0; i < 2; i++) {
        if (gout.take_grip[i]) halves[i].grip = 0;
        if (gout.take_trigger[i]) halves[i].trigger = 0;
    }
    const SenseHalf* left = have[0] ? &halves[0] : nullptr;
    const SenseHalf* right = have[1] ? &halves[1] : nullptr;
    // without the halves the grip / Options state still runs, so a pending short + ends
    if (!left && !right && g_chord.grip == SENSE_IDLE && g_chord.options == SENSE_OPT_IDLE && !g_chord.tap_until_ns)
        return false;
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec, recheck = 0;
    float sticks[4];
    // BetterVR's layout in first-person play, unless the launcher's classic layout is chosen (BOTW_VR_BUTTONS=classic)
    static const bool classic = [] {
        const char* e = getenv("BOTW_VR_BUTTONS");
        return e && strcmp(e, "classic") == 0;
    }();
    const bool vr = !classic && botw_vr_in_play();
    static bool vr_was = false;
    if (vr != vr_was) NSLog(@"[sense] buttons: %s layout", vr ? "VR" : "classic");
    vr_was = vr;
    const bool classic_play = !vr && botw_vr_in_play();  // classic layout in first-person VR play
    physical |= sense_pair(left, right, &g_chord, now, sticks, &recheck,
                           vr ? SENSE_LAYOUT_VR : classic_play ? SENSE_LAYOUT_CLASSIC_PLAY : SENSE_LAYOUT_CLASSIC);
    // the whistle (left stick held) and the scope (right stick pressed) in play, for runtime.log (device 2026-11-05: a
    // blank bar after the whistle, the scope flickering)
    {
        static uint64_t was = 0;
        static int budget = 60;
        const uint64_t now_bits = classic_play ? physical & (SENSE_DOWN | SENSE_R3) : 0;
        if ((now_bits & ~was) && budget > 0)
            budget--, LOG("sense: %s pressed in play", now_bits & ~was & SENSE_DOWN ? "whistle (d-pad down)" : "scope (R3)");
        was = now_bits;
    }
    // the classic Triangle in VR play (sense_controller.h): a tap is L, a hold the rune quick menu (d-pad up)
    {
        static int was = SENSE_OPT_IDLE, budget = 200;
        const int st = g_chord.triangle.state;
        if (classic_play && st != was && budget > 0) {
            if (st == SENSE_OPT_MAP) budget--, LOG("sense: rune menu opened (Triangle held; right stick picks, let go confirms)");
            else if (was == SENSE_OPT_MAP) budget--, LOG("sense: rune menu confirmed (Triangle let go)");
            else if (was == SENSE_OPT_DOWN && st == SENSE_OPT_IDLE) budget--, LOG("sense: Triangle tap -> L (use the rune)");
        }
        was = st;
    }
    if (now >= g_chord.tap_until_ns) g_chord.tap_until_ns = 0;
    uint32_t buttons = 0;
    for (auto& [bit, vpad] : kMap)
        if (physical & bit) buttons |= vpad;
    // VR layout, Square (Y) with the bow out (not drawn): the game would swing the bow; put it away (B) and take the
    // melee weapon out (Y) instead, Y then held on while Square is (a charged attack). User 2026-11-03
    {
        static bool was = false, swapping = false;
        static uint64_t t0 = 0;
        const uint64_t ms = 1000000ull;
        const bool sq = (buttons & input::BTN_Y) != 0;
        if (vr && sq && !was && botw_vr_left_bow()) {
            swapping = true, t0 = now;
            NSLog(@"[sense] Square with the bow out: put it away, take the weapon out");
        }
        was = sq;
        if (swapping) {
            buttons &= ~input::BTN_Y;
            if (now < t0 + 120 * ms) buttons |= input::BTN_B;
            else if (now >= t0 + 200 * ms && (now < t0 + 320 * ms || sq)) buttons |= input::BTN_Y;
            if (now >= t0 + 320 * ms && !sq) swapping = false;
        }
    }
    // classic layout in first-person VR play: the right grip is Y (attack, held: a charged attack; user 2026-11-04), not
    // R, which threw the weapon on a firm squeeze (2026-11-03); throwing is the right trigger over the right shoulder
    // and the grip there is the weapon (quick menu held; vr_gesture.cpp takes it first), as in BetterVR. Menus keep R
    // User 2026-11-05: the right grip and Circle swapped: the right grip is B (back, held: run) and Circle Y (attack,
    // held: a charged attack), so the grips are A (left) and B (right) in play and in the menus alike
    // and back the next day (user 2026-11-05, later): in fights the right grip, squeezed by the way, put the sword away
    // as B; the right grip is Y (attack) again and Circle B (back, held: run)
    if (classic && botw_vr_in_play() && (buttons & input::BTN_R)) buttons = (buttons & ~input::BTN_R) | input::BTN_Y;
    // the pause menu (user 2026-11-05, third try): the left trigger (L2) pages left (L), the right trigger (R2) pages
    // right (R); the left grip confirms (A, as Square), the right grip goes back (B), as in play (user 2026-11-05). The right grip was R there and turned the page right when squeezed
    // never in first-person play: there the pause menu's screen is closed and the map draws flat, so a menu flag seen
    // in play is stale or a HUD screen taken for one (device 2026-11-05: L2 kept taking the Sheikah Slate out)
    // the Camera rune's viewfinder and the scope (flat on the screen) take the same buttons: the left grip captures /
    // confirms (A), the right grip quits / goes back (B) (user 2026-11-13)
    const bool pause_menu = (botw_vr_pause_menu() || botw_vr_flat_camera()) && !botw_vr_in_play();
    {
        static int budget = 40;
        static bool flagged = false;
        const bool f = botw_vr_pause_menu() != 0, play = botw_vr_in_play() != 0;
        if (f != flagged && budget > 0) budget--, LOG("sense: menu flag %d in play %d -> menu buttons %d", f, play, f && !play);
        flagged = f;
    }
    if (pause_menu) {
        const bool lt = buttons & input::BTN_ZL, rt = buttons & input::BTN_ZR;
        const bool lg = left && left->grip, rg = right && right->grip;
        // L and R from the triggers alone: Triangle (L) paged left there too (user 2026-11-05: not wanted)
        buttons &= ~(input::BTN_ZL | input::BTN_ZR | input::BTN_R | input::BTN_L);
        if (lt) buttons |= input::BTN_L;
        if (rt) buttons |= input::BTN_R;
        if (lg) buttons |= input::BTN_A;
        if (rg) buttons |= input::BTN_B;
        static uint32_t was;
        static int budget = 40;
        const uint32_t st = lt | rt << 1 | lg << 2 | rg << 3;
        if (st != was && st && budget > 0)
            budget--, LOG("sense: pause menu:%s%s%s%s", lt ? " L2 -> L" : "", rt ? " R2 -> R" : "", lg ? " left grip -> A" : "",
                            rg ? " right grip -> B" : "");
        was = st;
    }
    // everything else on the screen in front (events' dialogs, the title menu, shops, the stable's lists): the grips
    // confirm and go back as in the pause menu and in play, left A, right B (user 2026-11-16: a dialog in the first
    // event wanted Circle, "use the left grip like everywhere else"). Circle and Cross keep working there too
    if (!pause_menu && !botw_vr_in_play()) {
        const bool lg = left && left->grip, rg = right && right->grip;
        if (lg) buttons = (buttons & ~input::BTN_L) | input::BTN_A;
        if (rg) buttons = (buttons & ~input::BTN_R) | input::BTN_B;
        static bool was;
        static int budget = 20;
        if ((lg || rg) != was && (lg || rg) && budget > 0)
            budget--, LOG("sense: on the screen:%s%s", lg ? " left grip -> A" : "", rg ? " right grip -> B" : "");
        was = lg || rg;
    }
    // a tap stays down at least SENSE_TAP_MS: the game reads the pad once per game frame (20-30 Hz in VR), and a quick
    // press between two reads was lost (device, 2026-11-01: confirm took many presses)
    static uint32_t was_down;
    static uint64_t down_until[32];
    for (int i = 0; i < 32; i++) {
        const uint32_t m = 1u << i;
        if (buttons & m & ~was_down) down_until[i] = now + SENSE_TAP_MS * 1000000ull;  // the press starts
        if (!(buttons & m) && now < down_until[i]) buttons |= m;
    }
    was_down = buttons;
    s.buttons |= buttons | gesture_buttons;
    auto stronger = [](float a, float b) { return fabsf(b) < 0.15f ? a : fabsf(b) > fabsf(a) ? b : a; };  // dead zone as SDL's
    s.lx = stronger(s.lx, sticks[0]), s.ly = stronger(s.ly, sticks[1]);
    s.rx = stronger(s.rx, sticks[2]), s.ry = stronger(s.ry, sticks[3]);
    if (pause_menu) {
        // the right stick turns the category (weapons, bows, food...) in the inventory: log what goes to the game
        static int budget = 60;
        static bool pushed;
        const bool now_pushed = fabsf(s.rx) > 0.5f;
        if (now_pushed != pushed && budget > 0)
            budget--, NSLog(@"[sense] pause menu: right stick x %.2f (sense %.2f), buttons %08x", s.rx, sticks[2], s.buttons);
        pushed = now_pushed;
    }
    if (gout.set_ry) s.ry = gout.ry;  // Magnesis: the hand moves the object up / down
    if (gout.release_left_stick) s.lx = s.ly = 0;  // the top of a ladder (vr_gesture.cpp)
    log_state(buttons, sticks);
    return left || right;
}

// The game's GamePad rumble (window.mm, 60 Hz on / off steps) on both Sense controllers
void sense_vision_rumble(bool on) {
    for (int h = 0; h < 2; h++) {
        if (on && !g_rumble_player[h]) g_rumble_player[h] = buzz(h, 0.6f, 30.0);
        if (!on && g_rumble_player[h]) [g_rumble_player[h] stopAtTime:CHHapticTimeImmediate error:nil], g_rumble_player[h] = nil;
    }
}

// VR events (runtime/src/game: a body-slot grab, a parry, a swing): a short buzz on one hand, from any thread
extern "C" void botw_vr_haptic(int hand, float strength, float seconds) {
    if (hand < 0 || hand > 1) return;
    dispatch_async(dispatch_get_main_queue(), ^{ buzz(hand, strength, seconds); });
}
#endif
