/*
 * Desktop LVGL simulator for rPod UI development.
 * Uses LVGL's built-in SDL window driver — no Pi hardware required.
 * See docs/PLAN.md §5.4.
 *
 * Talks to a real local MPD instance (`make mpd-dev`) rather than mocking
 * data — the click wheel and DAC aren't wired up, so this and the keyboard
 * stand-in in sim_input.c are the whole iteration loop for now.
 */

#include "lvgl.h"
#include "sim_input.h"

#include "audio/mpd_client.h"
#include "ui/screens/main_menu.h"
#include "ui/screens/screen_stack.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define SIM_HOR_RES 320
#define SIM_VER_RES 240

static rpod_screen_stack_t *g_stack;

static void resolve_mpd_socket_path(char *out, size_t out_size)
{
    const char *override = getenv("RPOD_MPD_SOCKET");
    if (override != NULL) {
        snprintf(out, out_size, "%s", override);
        return;
    }
    /* Matches tools/sim/mpd-dev.conf.in's default RPOD_MPD_STATE_DIR. */
    const char *home = getenv("HOME");
    snprintf(out, out_size, "%s/.local/state/rpod-sim/mpd/socket", home != NULL ? home : "");
}

static void on_menu(void *ctx)
{
    (void)ctx;
    rpod_screen_stack_pop(g_stack);
}

static void on_play_pause(void *ctx)
{
    rpod_mpd_toggle_pause((rpod_mpd_t *)ctx);
}

static void on_next(void *ctx)
{
    rpod_mpd_next((rpod_mpd_t *)ctx);
}

static void on_prev(void *ctx)
{
    rpod_mpd_previous((rpod_mpd_t *)ctx);
}

int main(void)
{
    char socket_path[512];
    resolve_mpd_socket_path(socket_path, sizeof(socket_path));

    rpod_mpd_t *mpd = rpod_mpd_connect(socket_path);
    if (mpd == NULL || !rpod_mpd_is_connected(mpd)) {
        fprintf(stderr, "rpod-sim: couldn't connect to MPD at %s\n", socket_path);
        fprintf(stderr, "rpod-sim: start it first with `make mpd-dev`, or set RPOD_MPD_SOCKET.\n");
        return 1;
    }

    lv_init();

    lv_sdl_window_create(SIM_HOR_RES, SIM_VER_RES);
    lv_sdl_mouse_create();
    lv_sdl_mousewheel_create();

    rpod_sim_buttons_t buttons = {
        .on_menu = on_menu,
        .on_play_pause = on_play_pause,
        .on_next = on_next,
        .on_prev = on_prev,
        .ctx = mpd,
    };
    lv_indev_t *indev = rpod_sim_input_init(&buttons);

    g_stack = rpod_screen_stack_create(indev);
    rpod_screen_stack_push(g_stack, rpod_main_menu_build, mpd, NULL);

    while (1) {
        uint32_t idle_ms = lv_timer_handler();
        usleep(idle_ms * 1000);
    }

    return 0;
}
