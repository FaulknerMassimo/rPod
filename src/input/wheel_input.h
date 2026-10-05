/*
 * Click wheel input: the UI-side client of the rpod-wheel daemon
 * (daemon/rpod-wheel.c, docs/PLAN.md §4.5). Reads its decoded events off the
 * Unix socket and feeds them into the same rpod_input_t (input.h) as the
 * simulator's keyboard stand-in:
 *
 *   rotate  -> encoder steps, through the scroll-acceleration curve (§8.2);
 *              fast enough in a long alphabetical list, letters instead
 *              (the alphabet scrub -- see wheel_input.c)
 *   center, MENU (top), PLAY/PAUSE (bottom), PREV (left), NEXT (right)
 *           -> button presses and releases (taps and holds: gestures.h)
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
 *   RPOD_WHEEL_SCRUB_V     speed (positions/s) that switches a long
 *                          alphabetical list to letters (default: V1)
 *   RPOD_WHEEL_SCRUB_TICKS wheel positions per letter once it has
 */

#ifndef RPOD_WHEEL_INPUT_H
#define RPOD_WHEEL_INPUT_H

#include "input/input.h"
#include "lvgl.h"

/* Starts polling `sock_path` (NULL = the daemon's default,
 * RPOD_WHEEL_SOCK_PATH) and feeding what it reads into `in`. */
void rpod_wheel_input_create(const char *sock_path, rpod_input_t *in);

#endif /* RPOD_WHEEL_INPUT_H */
