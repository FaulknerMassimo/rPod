/*
 * Shared input contract between the UI and whichever backend feeds it.
 *
 * rPod's navigation runs through an LVGL ENCODER indev (rotate = move
 * focus, press = select) built once and handed to the screen stack. The
 * four *app-level* buttons -- Menu/back, Play-Pause, Next, Prev
 * (docs/PLAN.md §8.2) -- are not part of the encoder pair: their taps and
 * holds become the app's actions (gestures.h).
 *
 * The app creates one rpod_input_t with its actions; the board's backend --
 * the desktop simulator's keyboard stand-in (tools/sim/sim_input.c) or the
 * on-device click wheel (wheel_input.c) -- feeds it raw button edges and
 * rotation, so the screens never learn which one is driving them, and taps
 * vs holds, sleep and wake work the same on both.
 */

#ifndef RPOD_INPUT_H
#define RPOD_INPUT_H

#include "input/gestures.h"
#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct rpod_input rpod_input_t;

/* Creates the encoder indev and the gesture state behind it. Needs
 * lv_init() first (it creates an lv_timer). */
rpod_input_t *rpod_input_create(const rpod_input_actions_t *actions);

/* The ENCODER indev, for rpod_screen_stack_create(). */
lv_indev_t *rpod_input_indev(const rpod_input_t *in);

/* --- Backend side ---------------------------------------------------------- */

/* A button went down (pressed) or up. Center included -- it reaches the
 * encoder from here. */
void rpod_input_button(rpod_input_t *in, rpod_button_t btn, bool pressed);

/* Rotation since the last call, in list steps: > 0 next, < 0 previous.
 * Dropped while asleep (and wakes a dim sleep -- gestures.h). */
void rpod_input_rotate(rpod_input_t *in, int steps);

/* A finger landed on the wheel: wakes a dim sleep, like an iPod's backlight
 * coming on at a touch. Backends that can't sense touch skip it. */
void rpod_input_touch(rpod_input_t *in);

/* The alphabet scrub (rpod_input_actions_t's `scrub`): rotation already fed
 * is delivered first, so the jump lands after it. False if the current
 * screen has no letter index, or while asleep. */
bool rpod_input_scrub(rpod_input_t *in, int dir);

/* The backend lost its source (e.g. the wheel daemon went away): lets go of
 * everything held, without a click or a tap. */
void rpod_input_release_all(rpod_input_t *in);

/* --- App side ----------------------------------------------------------------- */

/* From now until the next button press, presses only wake (the actions'
 * `wake`) and rotation is ignored. Anything held is let go of first. A `dim`
 * sleep (the backlight timer's) also wakes on the wheel, and lets the
 * transport buttons act -- see gestures.h. */
void rpod_input_sleep(rpod_input_t *in, bool dim);

bool rpod_input_asleep(const rpod_input_t *in);

/* How long since the last input of any kind -- 0 while a button is held. */
uint32_t rpod_input_idle_ms(const rpod_input_t *in);

#endif /* RPOD_INPUT_H */
