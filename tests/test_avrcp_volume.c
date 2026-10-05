/*
 * audio/avrcp_volume.c: a headset's volume changes in, MPD steps and re-pins
 * out, through the sequences BlueZ and PipeWire produce. Host-built:
 * `make test`.
 */

#include "audio/avrcp_volume.h"

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

/* A transport that's been streaming long enough to settle, pinned at 95. */
static rpod_avrcp_volume_t settled(uint32_t *now)
{
    rpod_avrcp_volume_t v;
    int repin;
    rpod_avrcp_volume_init(&v);
    *now = 1000;
    CHECK(rpod_avrcp_volume_changed(&v, 127, *now, &repin) == 0); /* headset's own report */
    rpod_avrcp_volume_state(&v, true, *now);
    CHECK(rpod_avrcp_volume_changed(&v, 95, *now + 200, &repin) == 0); /* PipeWire's pin */
    CHECK(repin == -1);
    *now += RPOD_AVRCP_SETTLE_MS + 1000;
    return v;
}

static void test_setup_moves_the_pin(void)
{
    uint32_t now;
    rpod_avrcp_volume_t v = settled(&now);
    CHECK(v.anchor == 95);
    CHECK(!v.repin_pending);
}

static void test_swipe_up_steps_and_repins(void)
{
    uint32_t now;
    int repin;
    rpod_avrcp_volume_t v = settled(&now);
    CHECK(rpod_avrcp_volume_changed(&v, 103, now, &repin) == 1);
    CHECK(repin == 95);
    /* The echo of setting it back isn't a swipe down. */
    CHECK(rpod_avrcp_volume_changed(&v, 95, now + 50, &repin) == 0);
    CHECK(repin == -1);
    CHECK(rpod_avrcp_volume_changed(&v, 87, now + 500, &repin) == -1);
    CHECK(repin == 95);
}

static void test_fast_swipes_before_the_echo(void)
{
    uint32_t now;
    int repin;
    rpod_avrcp_volume_t v = settled(&now);
    CHECK(rpod_avrcp_volume_changed(&v, 103, now, &repin) == 1);
    /* Second swipe, measured from 103 because our set hadn't landed. */
    CHECK(rpod_avrcp_volume_changed(&v, 111, now + 20, &repin) == 1);
    CHECK(repin == 95);
    CHECK(rpod_avrcp_volume_changed(&v, 95, now + 60, &repin) == 0);
    CHECK(!v.repin_pending);
}

static void test_big_jump_is_several_steps(void)
{
    uint32_t now;
    int repin;
    rpod_avrcp_volume_t v = settled(&now);
    CHECK(rpod_avrcp_volume_changed(&v, 95 - 3 * RPOD_AVRCP_VOLUME_STEP, now, &repin) == -3);
    /* A nudge smaller than a step still counts as one. */
    rpod_avrcp_volume_changed(&v, 95, now + 10, &repin);
    CHECK(rpod_avrcp_volume_changed(&v, 97, now + 500, &repin) == 1);
}

static void test_ignored_while_paused_or_settling(void)
{
    uint32_t now;
    int repin;
    rpod_avrcp_volume_t v = settled(&now);
    rpod_avrcp_volume_state(&v, false, now);
    CHECK(rpod_avrcp_volume_changed(&v, 110, now + 10, &repin) == 0);
    CHECK(repin == -1);
    CHECK(v.anchor == 110);
    /* Streaming again: PipeWire re-applying its level is still setup. */
    rpod_avrcp_volume_state(&v, true, now + 100);
    CHECK(rpod_avrcp_volume_changed(&v, 95, now + 300, &repin) == 0);
    CHECK(v.anchor == 95);
    CHECK(rpod_avrcp_volume_changed(&v, 103, now + 100 + RPOD_AVRCP_SETTLE_MS, &repin) == 1);
}

static void test_broken_passes_through(void)
{
    uint32_t now;
    int repin;
    rpod_avrcp_volume_t v = settled(&now);
    CHECK(rpod_avrcp_volume_changed(&v, 103, now, &repin) == 1);
    rpod_avrcp_volume_repin_failed(&v);
    CHECK(rpod_avrcp_volume_changed(&v, 111, now + 10, &repin) == 0);
    CHECK(repin == -1);
}

int main(void)
{
    test_setup_moves_the_pin();
    test_swipe_up_steps_and_repins();
    test_fast_swipes_before_the_echo();
    test_big_jump_is_several_steps();
    test_ignored_while_paused_or_settling();
    test_broken_passes_through();
    if (failures > 0) {
        fprintf(stderr, "test_avrcp_volume: %d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    printf("test_avrcp_volume: all checks passed\n");
    return EXIT_SUCCESS;
}
