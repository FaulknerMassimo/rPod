/*
 * power/gauge.c: the IP5209's registers decoded (against readings taken off
 * the PiSugar 2 on the device), PiSugar's curve, and the level the UI shows
 * through wandering readings, USB power coming and going, and running out.
 * Host-built: `make test`.
 */

#include "power/gauge.h"

#include <stdio.h>
#include <stdlib.h>

static int failures;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            failures++;                                                        \
        }                                                                      \
    } while (0)

#define SAMPLE_MS 2000

/* Feeds the same reading for `ms`, a sample every SAMPLE_MS. */
static void hold(rpod_gauge_t *g, int mv, bool usb, uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += SAMPLE_MS) {
        rpod_gauge_feed(g, mv, usb, SAMPLE_MS);
    }
}

static void test_registers(void)
{
    /* Read off the device: 3.96 V, the Pi drawing 229 mA. */
    CHECK(rpod_ip5209_voltage_mv(0xd1, 0x13) == 3962);
    CHECK(rpod_ip5209_current_ma(0xcd, 0x3e) == -229);
    /* The top two bits of the high byte aren't part of the number. */
    CHECK(rpod_ip5209_voltage_mv(0xd1, 0xd3) == 3962);
    CHECK(rpod_ip5209_current_ma(0xcd, 0xfe) == -229);
    CHECK(rpod_ip5209_current_ma(0x00, 0x00) == 0);
    CHECK(rpod_ip5209_current_ma(0x9e, 0x02) == 500); /* 670 steps */
    CHECK(rpod_ip5209_voltage_mv(0x00, 0x00) == 2600);
}

static void test_curve(void)
{
    CHECK(rpod_gauge_level_for_mv(4200) == 100);
    CHECK(rpod_gauge_level_for_mv(4160) == 100);
    CHECK(rpod_gauge_level_for_mv(4000) == 80);
    CHECK(rpod_gauge_level_for_mv(3960) == 73);
    CHECK(rpod_gauge_level_for_mv(3490) == 3);
    CHECK(rpod_gauge_level_for_mv(3100) == 0);
    CHECK(rpod_gauge_level_for_mv(2900) == 0);
    /* Never back up as the voltage falls. */
    int last = 101;
    for (int mv = 4300; mv >= 3000; mv -= 5) {
        int level = rpod_gauge_level_for_mv(mv);
        CHECK(level <= last);
        last = level;
    }
}

static void test_first_reading(void)
{
    rpod_gauge_t g;
    rpod_gauge_init(&g);
    CHECK(g.level == -1);
    CHECK(rpod_gauge_feed(&g, 3960, false, 0));
    CHECK(g.level == 73);
    CHECK(!g.external && !g.charging && !g.critical);
    CHECK(g.mv == 3960);
}

static void test_discharge_only_steps_down(void)
{
    rpod_gauge_t g;
    rpod_gauge_init(&g);
    rpod_gauge_feed(&g, 3960, false, 0);
    /* Readings wander up a little: the level holds. */
    hold(&g, 3975, false, 60000);
    CHECK(g.level == 73);
    /* One low reading barely moves the average. */
    CHECK(!rpod_gauge_feed(&g, 3900, false, SAMPLE_MS));
    CHECK(g.level == 73);
    /* Really running down. */
    hold(&g, 3900, false, 300000);
    CHECK(g.level == 57);
    /* ...and a first reading taken at a busy moment is put right. */
    rpod_gauge_init(&g);
    rpod_gauge_feed(&g, 3880, false, 0);
    hold(&g, 3960, false, 300000);
    CHECK(g.level == 73);
}

/* The readings around plugging USB in and out on the device: the cell's
 * voltage jumps by the charger's ~50 mV, which isn't charge. */
static void test_usb_power(void)
{
    rpod_gauge_t g;
    rpod_gauge_init(&g);
    rpod_gauge_feed(&g, 3974, false, 0);
    int level = g.level;

    CHECK(rpod_gauge_feed(&g, 4026, true, SAMPLE_MS));
    CHECK(g.external && g.charging);
    hold(&g, 4026, true, 120000);
    CHECK(g.level >= level && g.level <= level + 1); /* not 82, as 4.03 V would read */

    /* Charging, the voltage running ahead: the level climbs a point every
     * 45 s, however far ahead the voltage says it is... */
    rpod_gauge_init(&g);
    rpod_gauge_feed(&g, 3974, false, 0);
    hold(&g, 4120, true, 450000);
    CHECK(g.level == level + 10);
    /* ...and no further than the voltage says. */
    rpod_gauge_init(&g);
    rpod_gauge_feed(&g, 3974, false, 0);
    hold(&g, 4040, true, 3600000);
    CHECK(g.level == 78);

    /* Unplugged: the voltage drops by the same, and so does nothing else. */
    CHECK(rpod_gauge_feed(&g, 3990, false, SAMPLE_MS));
    CHECK(!g.external && !g.charging);
    hold(&g, 3990, false, 120000);
    CHECK(g.level == 78);

    /* Unplugged after the voltage ran ahead: back to what it says as it
     * settles. */
    rpod_gauge_init(&g);
    rpod_gauge_feed(&g, 3974, false, 0);
    hold(&g, 4120, true, 900000);
    hold(&g, 3950, false, 300000);
    CHECK(g.level == 71);
}

static void test_running_out(void)
{
    rpod_gauge_t g;
    rpod_gauge_init(&g);
    rpod_gauge_feed(&g, 3520, false, 0);
    CHECK(g.level == 7 && !g.critical);
    hold(&g, 3450, false, 60000);
    CHECK(g.level <= RPOD_GAUGE_CRITICAL_LEVEL);
    CHECK(g.critical);
    /* Not on the first moment at the critical level... */
    rpod_gauge_init(&g);
    rpod_gauge_feed(&g, 3450, false, 0);
    rpod_gauge_feed(&g, 3450, false, SAMPLE_MS);
    CHECK(!g.critical);
    /* ...but once it's stayed there. */
    hold(&g, 3450, false, 30000);
    CHECK(g.critical);
    /* USB power calls it off. */
    rpod_gauge_feed(&g, 3500, true, SAMPLE_MS);
    CHECK(g.external && !g.critical);
}

int main(void)
{
    test_registers();
    test_curve();
    test_first_reading();
    test_discharge_only_steps_down();
    test_usb_power();
    test_running_out();
    if (failures != 0) {
        fprintf(stderr, "test_gauge: %d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    printf("test_gauge: ok\n");
    return EXIT_SUCCESS;
}
