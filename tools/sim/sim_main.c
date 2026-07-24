/*
 * Desktop LVGL simulator for rPod UI development.
 * Uses LVGL's built-in SDL window driver -- no Pi hardware required.
 * See docs/PLAN.md §5.4.
 *
 * Talks to a real local MPD instance (`make mpd-dev`) rather than mocking
 * data -- the keyboard stand-in (sim_input.c) drives the same shared app
 * bootstrap (src/app.c) the on-device binary uses. RPOD_BOARD selects the UI
 * form factor + window size: unset (or "classic") = the 320x240 landscape
 * click-wheel build; "hat144" = the 128x128 Waveshare 1.44" LCD HAT.
 */

#include "app.h"
#include "platform/board.h"
#include "sim_input.h"

#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Window size for sim_create_display(). Set from RPOD_BOARD before the board's
 * create_display() runs (rpod_app_run() calls it after lv_init()). */
static int32_t g_sim_w = 320;
static int32_t g_sim_h = 240;

static lv_display_t *sim_create_display(void)
{
    lv_display_t *disp = lv_sdl_window_create(g_sim_w, g_sim_h);
    lv_sdl_mouse_create();
    lv_sdl_mousewheel_create();
    return disp;
}

static lv_indev_t *sim_create_input(const rpod_input_buttons_t *buttons)
{
    return rpod_sim_input_init(buttons);
}

static bool board_is_square(const char *id)
{
    if (id == NULL) {
        return false;
    }
    return strcmp(id, "hat144") == 0 || strcmp(id, "waveshare-144") == 0 ||
           strcmp(id, "lcdhat") == 0 || strcmp(id, "square") == 0;
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
    const char *board_id = getenv("RPOD_BOARD");
    bool square = board_is_square(board_id);
    if (square) {
        g_sim_w = 128;
        g_sim_h = 128;
    }

    rpod_board_t sim_board = {
        .id = square ? "waveshare-144" : "classic",
        .name = square ? "sim: 128x128 square" : "sim: 320x240 landscape",
        .form = square ? RPOD_FORM_SQUARE : RPOD_FORM_LANDSCAPE,
        .create_display = sim_create_display,
        .create_input = sim_create_input,
    };

    char mpd_socket[512], lb_queue[512], scrobbler_state[512], vis_fifo[512];
    resolve_path(mpd_socket, sizeof(mpd_socket), "RPOD_MPD_SOCKET",
                 ".local/state/rpod-sim/mpd/socket");
    resolve_path(lb_queue, sizeof(lb_queue), "RPOD_LISTENBRAINZ_QUEUE",
                 ".local/state/rpod-sim/listenbrainz_queue.jsonl");
    resolve_path(scrobbler_state, sizeof(scrobbler_state), "RPOD_SCROBBLER_STATE",
                 ".local/state/rpod-sim/scrobbler_state");
    resolve_path(vis_fifo, sizeof(vis_fifo), "RPOD_VIS_FIFO",
                 ".local/state/rpod-sim/mpd/visualizer.fifo");

    rpod_app_config_t cfg = {
        .mpd_socket = mpd_socket,
        .listenbrainz_token = getenv("RPOD_LISTENBRAINZ_TOKEN"),
        .listenbrainz_queue = lb_queue,
        .scrobbler_state = scrobbler_state,
        .vis_fifo = vis_fifo,
    };

    int rc = rpod_app_run(&sim_board, &cfg);
    if (rc != 0) {
        fprintf(stderr, "rpod-sim: start MPD first with `make mpd-dev`, or set RPOD_MPD_SOCKET.\n");
    }
    return rc;
}
