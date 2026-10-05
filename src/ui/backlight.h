/*
 * The backlight timer (docs/PLAN.md §8.3): after a while without input the
 * screen goes dark -- the app's dim sleep (input/input.h) -- and touching
 * the wheel or pressing anything brings it back. The timeout is Settings >
 * Backlight's, persisted to a one-line file.
 *
 * Off, not faded, and no brightness setting: the panel's backlight is a
 * plain GPIO (fbtft's bl_power), with no levels in between. Dimming needs
 * hardware PWM on GPIO 13 (§1.2), which the Pi 3B's headphone jack and
 * pigpio's DMA pacing both compete for (§4.5).
 */

#ifndef RPOD_BACKLIGHT_H
#define RPOD_BACKLIGHT_H

#include "input/input.h"

/* The timeout that never turns the screen off. */
#define RPOD_BACKLIGHT_ALWAYS_ON 0u

/* Watches `in` for idle time and calls off(ctx) once it's been idle for the
 * timeout -- the caller puts the screen and input into a dim sleep. Not
 * while already asleep. state_path is where the timeout persists (parent
 * directory must exist); NULL keeps it in memory only. Call once, after
 * lv_init(). */
void rpod_backlight_init(rpod_input_t *in, void (*off)(void *ctx), void *ctx, const char *state_path);

/* Seconds without input before the screen goes dark, or
 * RPOD_BACKLIGHT_ALWAYS_ON. */
unsigned rpod_backlight_timeout_s(void);
void rpod_backlight_set_timeout_s(unsigned seconds);

#endif /* RPOD_BACKLIGHT_H */
