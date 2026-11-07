/* Copied from MacNativeGames/games/totk/runtime/sense_controller.h (device-tested there on visionOS 27, 2026-10-23/24);
   used by ios/src/sense_vision.mm. BotW (Wii U) gives the buttons the same roles as TotK: B run / cancel, X jump,
   Y attack, A action, L the Sheikah Slate runes, R throw, ZL lock-on, ZR bow, R3 scope, L3 crouch, + menu, - map. */
/* PlayStation VR2 Sense controllers on Apple Vision Pro (visionOS 26 and later; docs/visionos-port.md).
   The system usually merges the pair into one MFi controller ("PlayStation VR2 Sense Controllers (L/R)") whose full
   input is only in GCController.input (its extendedGamepad reports Options alone); it may also list each hand as a
   GCProductCategorySpatialController. runtime/gamepad.m reads every source into a SenseHalf per hand (sense_merge_half);
   this file turns the pair into the physical buttons of one gamepad (the bits of controller_physical_names in
   controller_map.h), so controls.txt and its modes apply as for any controller:
   BotW VR maps the pair as the PS VR2 itself (user, 2026-11-01: "the controls should match the PS VR2 controller buttons"), confirm on Cross:
     left:  stick, L3 (crouch), grip -> l1 (L; the d-pad chord below), trigger -> l2 (ZL), Square -> west (Y, attack),
            Triangle -> north (X, jump), Create -> view (-)
     right: stick, stick press -> r3 (the scope), grip -> r1 (R, throw), trigger -> r2 (ZR), Cross -> east (A, confirm /
            action), Circle -> south (B, back / run), Options -> menu (+; held: - the map)
   (TotK's scheme, 2026-10-23, put run, jump and attack all on the right hand.)
   GameController names the face buttons as on an Xbox controller: A = Cross, B = Circle, X = Square, Y = Triangle.
   The system does not hand Create to apps (the merged pair lists 12 elements without it; device log 2026-10-23), so the
   map (-) is a long press of Options (sense_options): let go within SENSE_HOLD_MS it is a short + (the menu, on
   release); held longer it is - for as long as it is held.
   The pair has no d-pad, so the left stick doubles as one while the left grip is held (sense_grip; user, 2026-10-23,
   after two harder schemes: the grip with a 150 ms window, and the clicked stick). The grip is L at once; pushed out of
   the middle while the grip is held, the stick is the d-pad for the rest of that grip press (the stronger axis; L lets
   go, Link does not move), so the right thumb stays free for the right stick (the quick menus pick with it). A grip
   pressed while the stick is already pushed (Link walking) stays L, the stick still moving Link. There is no time
   limit. The left stick click is plain L3 (crouch).
   In first-person VR play the pair takes the VR layout instead (sense_pair's vr; user 2026-11-03, from the layout of
   a PC VR mod's controls card, after BetterVR's proved hard to use), which leaves the rest to gestures (swinging
   attacks, a grip over a shoulder equips, the trigger draws the bow):
     left:  trigger ZL (shield, lock-on); Triangle L (the Sheikah Slate's rune, held as held); Square Y (attack,
            held: a charged attack); stick moves, its press L3 (crouch) on a tap, R3 (the scope) held; grip A (grab /
            interact; with the stick pushed the d-pad, the quick menus, as above); Create - (the map; it reports
            itself as the left half's Options; user 2026-11-03: keep it so)
     right: trigger ZR (the bow); Circle X (jump); Cross + (the inventory) on a tap, - (the map) held; stick turns;
            grip A (grab / interact) on a tap, B (run) held (a slot, Magnesis or dropping take it first: the grip with
            the stick pulled down drops the weapon, vr_gesture.cpp); Options + (the menu; a hold of it never came
            through, the map is a hold of Cross)
   A tap is sent when the button is let go within SENSE_HOLD_MS (for SENSE_TAP_MS), a hold from then on while held
   (sense_tap_hold). Menus, the title and anything on the screen in front keep the layout above, so Cross confirms and
   Circle goes back there.
   The classic layout (the default, sense_pair's classic branch) in first-person VR play (SENSE_LAYOUT_CLASSIC_PLAY;
   user 2026-11-05): Triangle tapped (let go within SENSE_HOLD_MS) is L (use the rune) for SENSE_TAP_MS; held that long
   it is d-pad up for as long as it is held, the rune quick menu (the right stick picks, letting go confirms). The weapon
   / bow / shield / arrow quick menus are a grip held 0.4 s over a shoulder and the melee throw the right trigger over
   the right shoulder (runtime/src/game/vr_gesture.cpp). In that play the left grip is A (grab / interact) and the left
   stick never the d-pad; sense_vision.mm makes the right grip Y (attack; swapped with Circle and back again by the
   user 2026-11-05) and Circle stays B (back, held: run). In the pause menu and the map sense_vision.mm makes L2 / R2 page (L / R), the left grip A,
   the right grip B. Outside play the classic Triangle stays plain L and the left grip with the stick the d-pad.
   Plain C, shared by runtime/gamepad.m and tests/test_sense_controller.c. */
#pragma once
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

enum { SENSE_UNKNOWN = 0, SENSE_LEFT = 1, SENSE_RIGHT = 2 };
enum { SENSE_TAP_MS = 100,     /* Options let go early: + held this long */
       SENSE_HOLD_MS = 400 };   /* Options held this long: - (the map) */
#define SENSE_DPAD_PUSH 0.5f   /* how far the left stick goes before it is a d-pad direction */

/* Physical button bits (controller_map.h controller_physical_names). */
enum {
    SENSE_SOUTH = 1 << 0, SENSE_EAST = 1 << 1, SENSE_WEST = 1 << 2, SENSE_NORTH = 1 << 3, SENSE_L3 = 1 << 4,
    SENSE_R3 = 1 << 5, SENSE_L1 = 1 << 6, SENSE_R1 = 1 << 7, SENSE_L2 = 1 << 8, SENSE_R2 = 1 << 9, SENSE_MENU = 1 << 10,
    SENSE_VIEW = 1 << 11, SENSE_LEFT_DIR = 1 << 12, SENSE_UP = 1 << 13, SENSE_RIGHT_DIR = 1 << 14, SENSE_DOWN = 1 << 15
};

typedef struct {
    int side;                          /* SENSE_LEFT / SENSE_RIGHT */
    float x, y;                        /* the thumbstick, -1..1, y up */
    int stick, trigger, grip;          /* stick press, trigger, grip */
    int a, b, x_button, y_button;      /* face buttons by GameController's names */
    int system;                        /* Create (left) or Options (right) */
} SenseHalf;

/* A button that is one thing tapped and another held (sense_tap_hold). */
typedef struct { int state; uint64_t since_ns, tap_until_ns; } SenseTapHold;

/* The left grip (sense_grip), Options (sense_options) and the VR layout's tap / hold buttons, between calls. */
typedef struct {
    int grip, options; uint64_t options_since_ns, tap_until_ns;
    SenseTapHold cross, triangle, lstick, rstick, rgrip;
} SenseChord;
enum { SENSE_IDLE, SENSE_GRIP_L, SENSE_GRIP_DPAD, SENSE_GRIP_WALK };   /* grip */
/* sense_pair's layout: classic (menus, and play with the classic layout outside VR), the VR layout in first-person VR
   play, the classic layout in first-person VR play (Triangle: L tapped, the rune menu held) */
enum { SENSE_LAYOUT_CLASSIC = 0, SENSE_LAYOUT_VR = 1, SENSE_LAYOUT_CLASSIC_PLAY = 2 };
enum { SENSE_OPT_IDLE, SENSE_OPT_DOWN, SENSE_OPT_MAP };                 /* options */

/* Which hand: by its name ("Left", "(L)", "Right", "(R)"), else by the face buttons it has (Cross / Circle on the right,
   Square / Triangle on the left). The name goes first: each spatial half calls its two face buttons A and B, Square and
   Triangle as well (device log 2026-11-01: "PlayStation VR2 Sense Controller (L) pressed [p:Button A, l:Square Button]"),
   so by the buttons both halves were the right hand. */
static inline int sense_side(int has_a, int has_b, int has_x, int has_y, const char *name) {
    char lower[128] = {0};
    for (size_t i = 0; name && name[i] && i < sizeof lower - 1; i++) lower[i] = (char)tolower((unsigned char)name[i]);
    if (strstr(lower, "left") || strstr(lower, "(l)")) return SENSE_LEFT;
    if (strstr(lower, "right") || strstr(lower, "(r)")) return SENSE_RIGHT;
    int right = has_a || has_b, left = has_x || has_y;
    if (right != left) return right ? SENSE_RIGHT : SENSE_LEFT;
    return SENSE_UNKNOWN;
}

/* The d-pad direction of the stick: the stronger axis once it is pushed SENSE_DPAD_PUSH far, else 0. */
static inline uint64_t sense_direction(float x, float y) {
    if (fabsf(x) < SENSE_DPAD_PUSH && fabsf(y) < SENSE_DPAD_PUSH) return 0;
    if (fabsf(x) >= fabsf(y)) return x < 0 ? SENSE_LEFT_DIR : SENSE_RIGHT_DIR;
    return y > 0 ? SENSE_UP : SENSE_DOWN;
}

/* The left grip at time now: returns its bits (L or the d-pad); *stick_free says whether the left stick moves the stick. */
static inline uint64_t sense_grip(SenseChord *c, int grip, float x, float y, int *stick_free) {
    *stick_free = 1;
    if (!grip) { c->grip = SENSE_IDLE; return 0; }
    /* the stick already pushed when the grip goes down: L (walking); only a push out of the middle is the d-pad */
    if (c->grip == SENSE_IDLE) c->grip = sense_direction(x, y) ? SENSE_GRIP_WALK : SENSE_GRIP_L;
    if (c->grip == SENSE_GRIP_L && sense_direction(x, y)) c->grip = SENSE_GRIP_DPAD;
    if (c->grip == SENSE_GRIP_DPAD) { *stick_free = 0; return sense_direction(x, y); }
    if (c->grip == SENSE_GRIP_L) *stick_free = 0;   /* held still while it may still become the d-pad */
    return SENSE_L1;
}

/* Options at time now (ns): a short + on release within SENSE_HOLD_MS, else - from then until release. *recheck_ns, when
   not 0, is when this has to be called again without an input event (the hold turning into -, the end of a short +). */
static inline uint64_t sense_options(SenseChord *c, int down, uint64_t now, uint64_t *recheck_ns) {
    const uint64_t ms = 1000000ULL;
    uint64_t bits = 0;
    *recheck_ns = 0;
    if (down) {
        if (c->options == SENSE_OPT_IDLE) { c->options = SENSE_OPT_DOWN; c->options_since_ns = now; }
        if (c->options == SENSE_OPT_DOWN && now >= c->options_since_ns + SENSE_HOLD_MS * ms) c->options = SENSE_OPT_MAP;
        if (c->options == SENSE_OPT_MAP) bits |= SENSE_VIEW;
        else *recheck_ns = c->options_since_ns + SENSE_HOLD_MS * ms;
    } else {
        if (c->options == SENSE_OPT_DOWN) c->tap_until_ns = now + SENSE_TAP_MS * ms;   /* let go early: a short + */
        c->options = SENSE_OPT_IDLE;
    }
    if (now < c->tap_until_ns) { bits |= SENSE_MENU; *recheck_ns = c->tap_until_ns; }
    return bits;
}

/* A tap / hold button at time now (ns): tap for SENSE_TAP_MS once let go within SENSE_HOLD_MS, hold from then on while
   it is held. *recheck_ns as for sense_options (the earliest of the times given). */
static inline uint64_t sense_tap_hold(SenseTapHold *t, int down, uint64_t now, uint64_t tap, uint64_t hold,
                                      uint64_t *recheck_ns) {
    const uint64_t ms = 1000000ULL;
    uint64_t bits = 0, when = 0;
    if (down) {
        if (t->state == SENSE_OPT_IDLE) { t->state = SENSE_OPT_DOWN; t->since_ns = now; }
        if (t->state == SENSE_OPT_DOWN && now >= t->since_ns + SENSE_HOLD_MS * ms) t->state = SENSE_OPT_MAP;
        if (t->state == SENSE_OPT_MAP) bits |= hold;
        else when = t->since_ns + SENSE_HOLD_MS * ms;
    } else {
        if (t->state == SENSE_OPT_DOWN) t->tap_until_ns = now + SENSE_TAP_MS * ms;
        t->state = SENSE_OPT_IDLE;
    }
    if (now < t->tap_until_ns) bits |= tap, when = when && when < t->tap_until_ns ? when : t->tap_until_ns;
    else t->tap_until_ns = 0;
    if (when && (!*recheck_ns || when < *recheck_ns)) *recheck_ns = when;
    return bits;
}

/* Two readings of the same hand (its own spatial controller and the merged pair the system may publish as well, device
   test 2026-10-23): a button is down if either says so, each stick axis takes the stronger value. */
static inline void sense_merge_half(SenseHalf *into, const SenseHalf *from) {
    if (fabsf(from->x) > fabsf(into->x)) into->x = from->x;
    if (fabsf(from->y) > fabsf(into->y)) into->y = from->y;
    into->stick |= from->stick; into->trigger |= from->trigger; into->grip |= from->grip;
    into->a |= from->a; into->b |= from->b; into->x_button |= from->x_button; into->y_button |= from->y_button;
    into->system |= from->system;
}

/* The pair as one gamepad: physical button bits, and sticks[4] = left x, left y, right x, right y. Either half may be
   NULL (one controller connected, or one not yet identified). */
static inline uint64_t sense_pair(const SenseHalf *left, const SenseHalf *right, SenseChord *chord, uint64_t now,
                                  float sticks[4], uint64_t *recheck_ns, int layout) {
    const int vr = layout == SENSE_LAYOUT_VR;
    uint64_t bits = 0;
    sticks[0] = sticks[1] = sticks[2] = sticks[3] = 0;
    int stick_free = 1;
    /* Classic layout in first-person VR play: the left grip is A (grab / interact, as BetterVR) and the stick always
       moves Link; the quick menus are the shoulder grips and Triangle held (user 2026-11-05), so no grip d-pad there.
       Elsewhere (menus, third person) the grip still makes the stick the d-pad. */
    if (layout == SENSE_LAYOUT_CLASSIC_PLAY) {
        chord->grip = SENSE_IDLE;
        if (left && left->grip) bits |= SENSE_EAST;
    } else {
        const uint64_t grip = sense_grip(chord, left && left->grip, left ? left->x : 0, left ? left->y : 0, &stick_free);
        /* VR: the left grip alone is A (grab / interact). Classic: the grip only makes the stick the d-pad, L (the
           rune) is Triangle (user 2026-11-04: a grip squeezed for the quick menus used the rune) */
        bits |= vr && (grip & SENSE_L1) ? SENSE_EAST : (grip & ~(uint64_t)SENSE_L1);
    }
    bits |= sense_options(chord, right && right->system, now, recheck_ns);
    /* SheikahVision (user, 2026-11-01, on the headset): the buttons as PlayStation labels them, Cross confirming:
         right: Cross X (jump), Circle B (back / cancel, held to run), trigger ZR (bow), grip R (menus) / Y (attack,
                held: a charged attack) in first-person VR play (sense_vision.mm), stick press R3 (scope), Options +
                (short) / - (held)
         left:  Square A (confirm / action), Triangle L (the rune; in VR play a tap, held the rune menu), trigger ZL,
                grip held with the stick: the d-pad (the quick menus, above; the grip alone sends nothing), stick
                press L3 (crouch)
         User 2026-11-03 / 04: jump on the right thumb (Cross) so Link can move and jump at once, confirm on Square,
         the rune on Triangle, attack on the right grip; this classic layout is the default in VR as well */
    /* the VR layout's tap / hold buttons run in both layouts, so a press that began in one ends cleanly in the other */
    uint64_t lstick = sense_tap_hold(&chord->lstick, left && left->stick, now, SENSE_L3, SENSE_R3, recheck_ns);
    /* classic VR play, the right stick's press (user 2026-11-05): a tap is the whistle (d-pad down, sent for
       SENSE_TAP_MS only: held, it opened the d-pad's empty shortcut bar, a blank black bar), a hold the scope (R3).
       The whistle on a left-stick hold before it crouched on every short press, and Link stayed crouched */
    uint64_t rstick = sense_tap_hold(&chord->rstick, right && right->stick, now, SENSE_DOWN, SENSE_R3, recheck_ns);
    uint64_t triangle = sense_tap_hold(&chord->triangle, left && left->y_button, now, SENSE_L1, SENSE_UP, recheck_ns);
    uint64_t cross = sense_tap_hold(&chord->cross, right && right->a, now, SENSE_MENU, SENSE_VIEW, recheck_ns);
    uint64_t rgrip = sense_tap_hold(&chord->rgrip, right && right->grip, now, SENSE_EAST, SENSE_SOUTH, recheck_ns);
    if (vr) {
        if (left) {
            if (stick_free) { sticks[0] = left->x; sticks[1] = left->y; }
            if (left->trigger) bits |= SENSE_L2;
            if (left->x_button) bits |= SENSE_WEST;    /* Square: Y (attack) */
            if (left->y_button) bits |= SENSE_L1;      /* Triangle: L (the Sheikah Slate) */
            if (left->system) bits |= SENSE_VIEW;      /* Create: - (the map) */
        }
        if (right) {
            sticks[2] = right->x; sticks[3] = right->y;
            if (right->trigger) bits |= SENSE_R2;
            if (right->b) bits |= SENSE_NORTH;     /* Circle: X (jump) */
        }
        return bits | lstick | cross | rgrip;      /* Cross: + / -, right grip: A / B (run) */
    }
    if (left) {
        if (stick_free) { sticks[0] = left->x; sticks[1] = left->y; }
        if (left->stick) bits |= SENSE_L3;     /* the left stick's press: L3 (crouch) */
        if (left->trigger) bits |= SENSE_L2;
        if (left->x_button) bits |= SENSE_EAST;    /* Square: A (confirm / action) */
        /* Triangle: L (the Sheikah Slate's rune); in VR play a tap is L, a hold d-pad up (the rune quick menu) */
        if (layout == SENSE_LAYOUT_CLASSIC_PLAY) bits |= triangle;
        else if (left->y_button) bits |= SENSE_L1;
        if (left->system) bits |= SENSE_VIEW;      /* Create, should the system ever hand it over */
    }
    if (right) {
        sticks[2] = right->x; sticks[3] = right->y;
        if (layout == SENSE_LAYOUT_CLASSIC_PLAY) bits |= rstick;  /* tap: the whistle, hold: the scope (above) */
        else if (right->stick) bits |= SENSE_R3;    /* stick press: R3 (the scope) */
        if (right->grip) bits |= SENSE_R1;     /* grip: R (throw); Y (attack) in first-person VR play, sense_vision.mm */
        if (right->trigger) bits |= SENSE_R2;
        if (right->a) bits |= SENSE_NORTH;     /* Cross: X (jump) */
        if (right->b) bits |= SENSE_SOUTH;     /* Circle: B (back / cancel, run) */
    }
    return bits;
}
