/*
 * Click wheel input: the UI-side client of the rpod-wheel daemon
 * (daemon/rpod-wheel.c, docs/PLAN.md §4.5). Reads its decoded events off the
 * Unix socket and drives the same shared ENCODER indev (encoder.c) and
 * app-level button callbacks (input.h) as the simulator's keyboard stand-in:
 *
 *   rotate  -> encoder steps, through the scroll-acceleration curve (§8.2)
 *   center  -> encoder select (held level, so long-press gestures work)
 *   MENU (top), PLAY/PAUSE (bottom), PREV (left), NEXT (right)
 *           -> the four app-level buttons, on press
 *
 * Unprivileged -- it only needs the socket, so it also runs in the desktop
 * simulator against the Pi's daemon over an SSH-forwarded socket (see
 * tools/sim/sim_main.c).
 *
 * If the daemon isn't up yet (or restarts), this keeps retrying in the
 * background; navigation is just inert until it connects.
 *
 * Acceleration is tunable without a rebuild via environment variables (e.g.
 * /etc/rpod/env on device), read once at startup:
 *   RPOD_WHEEL_STEP_TICKS  wheel positions per list step at slow speed
 *   RPOD_WHEEL_ACCEL_V0    speed (positions/s) where acceleration starts
 *   RPOD_WHEEL_ACCEL_V1    speed (positions/s) where it reaches its max
 *   RPOD_WHEEL_ACCEL_MAX   maximum multiplier
 */

#ifndef RPOD_WHEEL_INPUT_H
#define RPOD_WHEEL_INPUT_H

#include "input/input.h"
#include "lvgl.h"

/* Creates the encoder indev and starts polling `sock_path` (NULL = the
 * daemon's default, RPOD_WHEEL_SOCK_PATH). Returns the indev, for the caller
 * to hand to rpod_screen_stack_create(). */
lv_indev_t *rpod_wheel_input_create(const char *sock_path, const rpod_input_buttons_t *buttons);

#endif /* RPOD_WHEEL_INPUT_H */
