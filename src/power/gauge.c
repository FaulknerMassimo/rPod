#include "gauge.h"

#include <stddef.h>

/* PiSugar's discharge curve for the PiSugar 2 (pisugar-core/src/ip5209.rs,
 * BATTERY_CURVE): the cell's voltage under a Pi Zero's load, highest first,
 * straight lines between the points. */
static const struct {
    int mv;
    int level;
} k_curve[] = {
    { 4160, 100 }, { 4050, 95 }, { 4000, 80 }, { 3920, 65 }, { 3860, 40 },
    { 3790, 26 },  { 3660, 10 }, { 3520, 7 },  { 3490, 3 },  { 3100, 0 },
};

/* The voltage is averaged over about this long: readings wander by ten
 * millivolts or so from one to the next. */
#define AVG_TAU_MS 30000.0

/* With USB power in, the charger holds the cell's voltage up by about this
 * much, which isn't charge: taken off before the curve. Measured on the
 * device, at the plug: +52 mV going in, -44 mV coming out. */
#define CHARGING_MV 50

/* ...and more as it goes on: the voltage keeps climbing ahead of the
 * charge (10 mV a minute on the device, an hour's worth of charge in ten
 * minutes by the curve). So while charging, the level climbs from where it
 * was at no more than a point this often -- 0 to 100 in about 75 minutes --
 * and never past what the voltage says. */
#define CHARGE_STEP_MS 45000

/* How far the average may move against the way the level is going before
 * the level follows it anyway (say, from a first reading taken at a busy
 * moment). */
#define SLACK_LEVELS 5

/* How long the level must sit at the critical level before it's acted on. */
#define CRITICAL_MS 20000

/* A 14-bit two's-complement number from its low byte and the high byte's
 * low six bits. */
static int s14(uint8_t lo, uint8_t hi)
{
    int v = ((hi & 0x3f) << 8) | lo;
    return (v & 0x2000) ? v - 0x4000 : v;
}

int rpod_ip5209_voltage_mv(uint8_t lo, uint8_t hi)
{
    /* 0.26855 mV a step from 2.6 V, in integer arithmetic. */
    long steps = s14(lo, hi);
    return 2600 + (int)((steps * 26855 + (steps >= 0 ? 50000 : -50000)) / 100000);
}

int rpod_ip5209_current_ma(uint8_t lo, uint8_t hi)
{
    /* 0.745985 mA a step. */
    long steps = s14(lo, hi);
    return (int)((steps * 745985 + (steps >= 0 ? 500000 : -500000)) / 1000000);
}

int rpod_gauge_level_for_mv(int mv)
{
    size_t n = sizeof(k_curve) / sizeof(k_curve[0]);
    if (mv >= k_curve[0].mv) {
        return k_curve[0].level;
    }
    for (size_t i = 1; i < n; i++) {
        if (mv >= k_curve[i].mv) {
            int dv = k_curve[i - 1].mv - k_curve[i].mv;
            int dl = k_curve[i - 1].level - k_curve[i].level;
            return k_curve[i].level + ((mv - k_curve[i].mv) * dl + dv / 2) / dv;
        }
    }
    return 0;
}

void rpod_gauge_init(rpod_gauge_t *g)
{
    *g = (rpod_gauge_t){ .level = -1 };
}

bool rpod_gauge_feed(rpod_gauge_t *g, int mv, bool usb, uint32_t dt_ms)
{
    rpod_gauge_t before = *g;
    bool first = g->level < 0;
    double resting = mv - (usb ? CHARGING_MV : 0);

    g->external = usb;
    g->charging = usb;
    g->mv = mv;
    if (first) {
        g->avg_mv = resting;
    } else {
        g->avg_mv += (resting - g->avg_mv) * (double)dt_ms / (AVG_TAU_MS + (double)dt_ms);
    }

    /* The level only goes down on battery and only up on USB power -- a
     * point at a time, CHARGE_STEP_MS apart -- unless the average has moved
     * well past it the other way. */
    int level = rpod_gauge_level_for_mv((int)(g->avg_mv + 0.5));
    g->rise_ms = g->external ? g->rise_ms + dt_ms : 0;
    if (first) {
        g->level = level;
    } else if (g->external) {
        if (level <= g->level - SLACK_LEVELS) {
            g->level = level;
        } else if (level > g->level) {
            if (g->rise_ms >= CHARGE_STEP_MS) {
                g->level++;
                g->rise_ms -= CHARGE_STEP_MS;
            }
        } else if (g->rise_ms > CHARGE_STEP_MS) {
            g->rise_ms = CHARGE_STEP_MS; /* caught up: no burst when it moves on */
        }
    } else if (level < g->level || level >= g->level + SLACK_LEVELS) {
        g->level = level;
    }

    /* Nearly empty, and staying that way: time to power off. USB power
     * coming back calls it off. */
    if (!g->external && g->level <= RPOD_GAUGE_CRITICAL_LEVEL) {
        g->low_ms += first ? 0 : dt_ms;
        g->critical = g->critical || g->low_ms >= CRITICAL_MS;
    } else {
        g->low_ms = 0;
        g->critical = false;
    }

    return first || g->level != before.level || g->external != before.external ||
           g->charging != before.charging || g->critical != before.critical;
}
