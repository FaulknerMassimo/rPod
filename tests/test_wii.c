/*
 * ui/wii_frames.h: which frame of the Wii theme's art shows a play state,
 * repeat, shuffle, a battery and a volume. Host-built: `make test`.
 */

#include "ui/wii_frames.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            failures++;                                                        \
        }                                                                      \
    } while (0)

static void test_play_modes(void)
{
    CHECK(rpod_wii_playmode_frame(RPOD_MPD_STATE_STOP) == 0);
    CHECK(rpod_wii_playmode_frame(RPOD_MPD_STATE_UNKNOWN) == 0);
    CHECK(rpod_wii_playmode_frame(RPOD_MPD_STATE_PLAY) == 1);
    CHECK(rpod_wii_playmode_frame(RPOD_MPD_STATE_PAUSE) == 2);

    rpod_mpd_status_t st;
    memset(&st, 0, sizeof(st));
    CHECK(rpod_wii_repeat_frame(&st) == 0);
    CHECK(rpod_wii_repeat_frame(NULL) == 0);
    st.repeat = true;
    CHECK(rpod_wii_repeat_frame(&st) == 1);
    st.single = true;
    CHECK(rpod_wii_repeat_frame(&st) == 2);
    /* "Single" without repeat is a one-shot, not repeat one. */
    st.repeat = false;
    CHECK(rpod_wii_repeat_frame(&st) == 0);

    CHECK(rpod_wii_shuffle_frame(&st) == 0);
    CHECK(rpod_wii_shuffle_frame(NULL) == 0);
    st.random = true;
    CHECK(rpod_wii_shuffle_frame(&st) == 1);
}

static void test_battery(void)
{
    CHECK(rpod_wii_battery_level_frame(false, 80, false, false) == -1);
    /* Draining: the art runs full (0) to nearly empty (11), every level in
     * range. */
    CHECK(rpod_wii_battery_level_frame(true, 100, false, false) == 0);
    CHECK(rpod_wii_battery_level_frame(true, 0, false, false) == 11);
    for (int l = 0; l <= 100; l++) {
        int f = rpod_wii_battery_level_frame(true, l, false, false);
        CHECK(f >= 0 && f <= 11);
        if (l > 0) {
            CHECK(f <= rpod_wii_battery_level_frame(true, l - 1, false, false));
        }
        int c = rpod_wii_battery_level_frame(true, l, true, true);
        CHECK(c == 12 + f);
    }
    /* On USB power and done charging: the "charged" frame, at any level. */
    CHECK(rpod_wii_battery_level_frame(true, 100, true, false) == 24);
    CHECK(rpod_wii_battery_level_frame(true, 97, true, false) == 24);
    /* Out-of-range levels clamp. */
    CHECK(rpod_wii_battery_level_frame(true, -5, false, false) == 11);
    CHECK(rpod_wii_battery_level_frame(true, 140, false, false) == 0);
}

static void test_volume(void)
{
    CHECK(rpod_wii_volume_frame(-1) == 0);
    CHECK(rpod_wii_volume_frame(0) == 0);
    CHECK(rpod_wii_volume_frame(1) == 0);
    CHECK(rpod_wii_volume_frame(50) == 10);
    CHECK(rpod_wii_volume_frame(100) == 20);
    CHECK(rpod_wii_volume_frame(250) == 20);
    for (int v = 1; v <= 100; v++) {
        CHECK(rpod_wii_volume_frame(v) >= rpod_wii_volume_frame(v - 1));
        CHECK(rpod_wii_volume_frame(v) <= 20);
    }
}

int main(void)
{
    test_play_modes();
    test_battery();
    test_volume();
    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_wii: ok\n");
    return 0;
}
