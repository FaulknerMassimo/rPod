/*
 * Shared ENCODER indev state machine. rPod navigates with an LVGL encoder
 * (rotate = move focus, press = select), and getting the read callback right
 * was fiddly -- see the long comment in encoder.c and the CLAUDE.md note on
 * lv_sdl_keyboard's uninitialised-key bug. That logic lives here once; each
 * backend (the simulator's keyboard poll, the on-device joystick/wheel GPIO
 * poll) only has to translate its raw inputs into rotation steps + a
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

/* A backend calls this from its own input-poll timer. `dir` requests a single
 * rotation step this poll: <0 = previous (LV_KEY_LEFT), >0 = next
 * (LV_KEY_RIGHT), 0 = no step. A step is queued only if no prior step is
 * still being delivered, so steps never overlap; the backend is expected to
 * pass a non-zero `dir` once per physical detent/key-down edge, not every
 * poll it's held. `enter_held` is the current level of the select/centre
 * button -- passed every call so a real press-and-hold reaches LVGL as a
 * sustained press (LV_EVENT_LONG_PRESSED can then fire for the hold gestures). */
void rpod_encoder_feed(lv_indev_t *indev, int dir, bool enter_held);

#endif /* RPOD_ENCODER_H */
