/*
 * Desktop LVGL simulator for rPod UI development.
 * Uses LVGL's built-in SDL window driver -- no Pi hardware required.
 * See docs/PLAN.md §5.4.
 *
 * Talks to a real local MPD instance (`make mpd-dev`) rather than mocking
 * data -- the keyboard stand-in (sim_input.c) drives the same shared app
 * bootstrap (src/app.c) the on-device binary uses.
 */

#include "app.h"
#include "input/wheel_input.h"
#include "platform/board.h"
#include "sim_input.h"

#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>

static lv_display_t *sim_create_display(void)
{
    lv_display_t *disp = lv_sdl_window_create(320, 240);
    lv_sdl_mouse_create();
    lv_sdl_mousewheel_create();
    return disp;
}

/* Keyboard by default. With RPOD_WHEEL_SOCK set, the real click wheel
 * instead -- forward the Pi's daemon socket first, e.g.
 *   ssh -N -L /tmp/rpod-wheel.sock:/run/rpod/wheel.sock rpod@rpod.local
 *   RPOD_WHEEL_SOCK=/tmp/rpod-wheel.sock make sim
 * which makes the sim the fast loop for tuning scroll acceleration too. */
static lv_indev_t *sim_create_input(const rpod_input_buttons_t *buttons)
{
    const char *wheel = getenv("RPOD_WHEEL_SOCK");
    if (wheel != NULL && wheel[0] != '\0') {
        return rpod_wheel_input_create(wheel, buttons);
    }
    return rpod_sim_input_init(buttons);
}

/* Same env-override-with-a-$HOME-default shape the sim has always used. The
 * buffers live on main()'s stack and outlive rpod_app_run() (which never
 * returns), so pointing the config at them is safe. */
static void resolve_path(char *out, size_t out_size, const char *env, const char *home_suffix)
{
    const char *override = getenv(env);
    if (override != NULL) {
        snprintf(out, out_size, "%s", override);
        return;
    }
    const char *home = getenv("HOME");
    snprintf(out, out_size, "%s/%s", home != NULL ? home : "", home_suffix);
}

int main(void)
{
    rpod_board_t sim_board = {
        .name = "sim: 320x240 SDL window",
        .create_display = sim_create_display,
        .create_input = sim_create_input,
    };

    char mpd_socket[512], lb_queue[512], scrobbler_state[512], volume_state[512], vis_fifo[512];
    resolve_path(mpd_socket, sizeof(mpd_socket), "RPOD_MPD_SOCKET",
                 ".local/state/rpod-sim/mpd/socket");
    resolve_path(lb_queue, sizeof(lb_queue), "RPOD_LISTENBRAINZ_QUEUE",
                 ".local/state/rpod-sim/listenbrainz_queue.jsonl");
    resolve_path(scrobbler_state, sizeof(scrobbler_state), "RPOD_SCROBBLER_STATE",
                 ".local/state/rpod-sim/scrobbler_state");
    resolve_path(volume_state, sizeof(volume_state), "RPOD_VOLUME_STATE",
                 ".local/state/rpod-sim/volumes");
    resolve_path(vis_fifo, sizeof(vis_fifo), "RPOD_VIS_FIFO",
                 ".local/state/rpod-sim/mpd/visualizer.fifo");

    rpod_app_config_t cfg = {
        .mpd_socket = mpd_socket,
        .listenbrainz_token = getenv("RPOD_LISTENBRAINZ_TOKEN"),
        .listenbrainz_queue = lb_queue,
        .scrobbler_state = scrobbler_state,
        .volume_state = volume_state,
        .vis_fifo = vis_fifo,
    };

    int rc = rpod_app_run(&sim_board, &cfg);
    if (rc != 0) {
        fprintf(stderr, "rpod-sim: start MPD first with `make mpd-dev`, or set RPOD_MPD_SOCKET.\n");
    }
    return rc;
}
