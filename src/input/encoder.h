/*
 * Shared ENCODER indev state machine. rPod navigates with an LVGL encoder
 * (rotate = move focus, press = select), and getting the read callback right
 * was fiddly -- see the long comment in encoder.c and the CLAUDE.md note on
 * lv_sdl_keyboard's uninitialised-key bug. That logic lives here once; each
 * backend (the simulator's keyboard poll, the on-device click wheel client in
 * wheel_input.c) only has to translate its raw inputs into rotation steps + a
 * held/not-held select level and feed them in.
 */

#ifndef RPOD_ENCODER_H
#define RPOD_ENCODER_H

#include "lvgl.h"
#include <stdbool.h>

/* Creates an ENCODER indev with the shared read callback and its backing
 * state. Returns the indev to hand to rpod_screen_stack_create(). The caller
 * owns nothing extra -- the state hangs off the indev's driver data and is
 * reached again through the indev in rpod_encoder_feed(). */
lv_indev_t *rpod_encoder_create(void);

/* A backend calls this from its own input-poll timer. `steps` is rotation
 * since its last call: >0 = next (LV_KEY_RIGHT / focus next), <0 = previous,
 * 0 = none. Steps accumulate until the indev's next read, which delivers them
 * all at once -- a keyboard stand-in passes +-1 per key-down edge, the click
 * wheel however many rows its acceleration curve produced. `enter_held` is
 * the current level of the select/centre button -- passed every call so a
 * real press-and-hold reaches LVGL as a sustained press
 * (LV_EVENT_LONG_PRESSED can then fire for the hold gestures). A press that's
 * already released again by the next read still registers as one click. */
void rpod_encoder_feed(lv_indev_t *indev, int steps, bool enter_held);

#endif /* RPOD_ENCODER_H */
