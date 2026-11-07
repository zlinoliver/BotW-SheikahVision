// VR gestures (vr_gesture.cpp), called by the host input every poll: ios/src/sense_vision.mm with the Sense
// controllers, runtime/src/host/window.mm with fake ones on a Mac. Plain C.
#pragma once
#include <stdint.h>

typedef struct {
    int grip[2], trigger[2];  // 0 left, 1 right hand
    uint32_t buttons;         // what the controllers press (input::BTN_*)
    float ry;                 // the right stick's y (dropping: the right grip with it pulled down)
} BotWVRGestureIn;

typedef struct {
    uint32_t buttons;                       // to add
    int take_grip[2], take_trigger[2];      // the gestures use these this poll: not their usual buttons
    int set_ry;                             // set the right stick's y to ry
    float ry;
    int release_left_stick;                 // the game sees the left stick at rest this poll (the top of a ladder)
} BotWVRGestureOut;

#ifdef __cplusplus
extern "C" {
#endif
void botw_vr_gestures(const BotWVRGestureIn* in, BotWVRGestureOut* out);
void botw_vr_recenter(void);  // vr_render.cpp: straight ahead is where the head looks next frame
int botw_vr_in_play(void);
int botw_vr_pause_menu(void);  // vr_render.cpp: the pause menu (inventory, map, system) is open
int botw_vr_flat_camera(void);  // vr_render.cpp: the Camera rune or the scope, flat on the screen
int botw_vr_left_bow(void);  // vr_gesture.cpp: the bow in the left hand and not drawn  // vr_render.cpp: first-person play in the view (the Sense buttons' VR layout)
#ifdef __cplusplus
}
#endif
