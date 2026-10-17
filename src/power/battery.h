/*
 * The battery: a PiSugar 2 under the Pi (docs/PLAN.md §7.6), whose IP5209
 * power chip is read over I2C on a thread of its own every couple of
 * seconds and turned into a level by power/gauge.h. The status bar, About
 * and the web UI show it; the app powers off when it runs out
 * (src/app.c).
 *
 * Without a PiSugar (the simulator, a bare Pi) it's simply absent. For UI
 * work, RPOD_BATTERY_FAKE stands in for one: a level, optionally followed
 * by ",charging", ",charged" or ",critical" -- e.g. RPOD_BATTERY_FAKE=15 or
 * RPOD_BATTERY_FAKE=80,charging.
 */

#ifndef RPOD_BATTERY_H
#define RPOD_BATTERY_H

#include "lvgl.h"

#include <stdbool.h>

typedef struct {
    bool present;  /* a PiSugar is answering */
    int level;     /* 0-100 */
    bool external; /* on USB power: charging, or charged */
    bool charging;
    bool critical; /* nearly empty: time to power off (power/gauge.h) */
    int mv;        /* the cell's voltage, as last read */
} rpod_battery_t;

/* Starts reading the PiSugar on `i2c_dev` (e.g. /dev/i2c-1); NULL or ""
 * leaves the battery absent, unless RPOD_BATTERY_FAKE is set. Call once,
 * after lv_init(). */
void rpod_battery_init(const char *i2c_dev);

/* Current state. Valid until the next lv_timer tick -- copy what you keep. */
const rpod_battery_t *rpod_battery(void);

/* Calls cb(user) on the LVGL thread after anything rpod_battery() shows
 * changes -- not .mv alone, which moves with every reading -- until `owner`
 * is deleted (or, for a NULL owner, for the life of the process). */
void rpod_battery_watch(lv_obj_t *owner, void (*cb)(void *user), void *user);

/* Asks logind to power the device off; the PiSugar then cuts its power
 * once the Pi has halted (system/pisugar/). rpod's service user may by
 * system/pisugar/50-rpod-power.rules. Returns false if logind said no. */
bool rpod_power_off(void);

#endif /* RPOD_BATTERY_H */
