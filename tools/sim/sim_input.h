/*
 * Keyboard stand-in for the click wheel / joystick, for UI development
 * without hardware (docs/PLAN.md §5.4's simulator loop). See sim_input.c for
 * the key mapping and why it's built the way it is.
 */

#ifndef RPOD_SIM_INPUT_H
#define RPOD_SIM_INPUT_H

#include "input/input.h"
#include "lvgl.h"

/* Sets up keyboard input into `in` (src/input/input.h, so screens don't need
 * to know the input isn't real hardware): Left/Right arrows = wheel rotate,
 * Shift+Left/Right = a fast flick's alphabet-scrub letter jump, Enter =
 * center/select, and M/Space/N/P = the four app-level buttons
 * (Menu/Play-Pause/Next/Prev, docs/PLAN.md §8.2) -- held keys are held
 * buttons, so Space held sleeps and N/P held seek. */
void rpod_sim_input_init(rpod_input_t *in);

#endif /* RPOD_SIM_INPUT_H */
