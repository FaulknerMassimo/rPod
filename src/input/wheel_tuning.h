/*
 * The click wheel's feel (docs/PLAN.md §8.2): the scroll-acceleration curve
 * and the alphabet scrub's thresholds, which input/wheel_input.c reads on
 * every wheel event -- so a change takes effect on the next turn, no
 * restart. The web UI tunes them live (Settings > Click Wheel, through
 * ui/control.h), which is what §8.2's "live-tunable over the ACM console"
 * became.
 *
 * Each value comes from, in order: its built-in default, its environment
 * variable (/etc/rpod/env on device), then what was tuned and saved in the
 * state file. Resetting forgets the tuned values and goes back to the first
 * two.
 */

#ifndef RPOD_WHEEL_TUNING_H
#define RPOD_WHEEL_TUNING_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    float step_ticks;  /* wheel positions per list step at slow speed */
    float accel_v0;    /* speed (positions/s) where acceleration starts */
    float accel_v1;    /* speed (positions/s) where it reaches its max */
    float accel_max;   /* maximum multiplier */
    float scrub_v;     /* speed that switches a long A-Z list to letters; 0 = accel_v1 */
    float scrub_ticks; /* wheel positions per letter once it has */
} rpod_wheel_tuning_t;

/* One tunable value, for listing them (ui/control.c): its key, the
 * environment variable it can also come from, its allowed range, and what
 * a reset goes back to. */
typedef struct {
    const char *key;
    const char *env;
    float min, max;
} rpod_wheel_param_t;

/* Loads the values (see above). state_path is where tuned values persist
 * (parent directory must exist); NULL keeps them in memory only. Call
 * once, before the wheel's input starts. */
void rpod_wheel_tuning_init(const char *state_path);

const rpod_wheel_tuning_t *rpod_wheel_tuning(void);

/* scrub_v, with 0 resolved to accel_v1. */
float rpod_wheel_tuning_scrub_v(void);

/* The tunable values, in display order. */
const rpod_wheel_param_t *rpod_wheel_tuning_params(size_t *count);

/* By key: the current value, and what a reset goes back to. */
bool rpod_wheel_tuning_get(const char *key, float *value, float *reset_value);

/* Sets and persists one value. False for an unknown key or a value outside
 * its range. */
bool rpod_wheel_tuning_set(const char *key, float value);

/* Back to the defaults and environment; deletes the state file. */
void rpod_wheel_tuning_reset(void);

#endif /* RPOD_WHEEL_TUNING_H */
