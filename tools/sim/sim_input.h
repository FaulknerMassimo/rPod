/*
 * Keyboard stand-in for the click wheel / joystick, for UI development
 * without hardware (docs/PLAN.md §5.4's simulator loop). See sim_input.c for
 * the key mapping and why it's built the way it is.
 */

#ifndef RPOD_SIM_INPUT_H
#define RPOD_SIM_INPUT_H

#include "input/input.h"
#include "lvgl.h"

/* Sets up keyboard input: Left/Right arrows = wheel rotate, Enter =
 * center/select (both via the shared ENCODER indev, src/input/encoder.c, so
 * screens don't need to know the input isn't real hardware), and M/Space/N/P
 * = the four app-level buttons (Menu/Play-Pause/Next/Prev, docs/PLAN.md
 * §8.2), dispatched straight to `buttons`.
 *
 * Returns the encoder indev, for the caller to hand to
 * rpod_screen_stack_create(). */
lv_indev_t *rpod_sim_input_init(const rpod_input_buttons_t *buttons);

#endif /* RPOD_SIM_INPUT_H */
