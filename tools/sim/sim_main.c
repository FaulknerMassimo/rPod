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
#include <string.h>

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
static void sim_create_input(rpod_input_t *in)
{
    const char *wheel = getenv("RPOD_WHEEL_SOCK");
    if (wheel != NULL && wheel[0] != '\0') {
        rpod_wheel_input_create(wheel, in);
        return;
    }
    rpod_sim_input_init(in);
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
    char cover_cache[512], backlight_state[512], theme_state[512];
    char alarm_state[512], scores_state[512], lock_state[512];
    char wheel_state[512], lb_token_state[512], control_sock[512];
    char video_dir[512], video_state[512], shutdown_state[512];
    resolve_path(mpd_socket, sizeof(mpd_socket), "RPOD_MPD_SOCKET",
                 ".local/state/rpod-sim/mpd/socket");
    resolve_path(lb_queue, sizeof(lb_queue), "RPOD_LISTENBRAINZ_QUEUE",
                 ".local/state/rpod-sim/listenbrainz_queue.jsonl");
    resolve_path(scrobbler_state, sizeof(scrobbler_state), "RPOD_SCROBBLER_STATE",
                 ".local/state/rpod-sim/scrobbler_state");
    resolve_path(volume_state, sizeof(volume_state), "RPOD_VOLUME_STATE",
                 ".local/state/rpod-sim/volumes");
    resolve_path(backlight_state, sizeof(backlight_state), "RPOD_BACKLIGHT_STATE",
                 ".local/state/rpod-sim/backlight");
    resolve_path(theme_state, sizeof(theme_state), "RPOD_THEME_STATE", ".local/state/rpod-sim/theme");
    resolve_path(alarm_state, sizeof(alarm_state), "RPOD_ALARM_STATE", ".local/state/rpod-sim/alarm");
    resolve_path(scores_state, sizeof(scores_state), "RPOD_SCORES_STATE", ".local/state/rpod-sim/scores");
    resolve_path(lock_state, sizeof(lock_state), "RPOD_LOCK_STATE", ".local/state/rpod-sim/lock");
    resolve_path(wheel_state, sizeof(wheel_state), "RPOD_WHEEL_STATE", ".local/state/rpod-sim/wheel");
    resolve_path(lb_token_state, sizeof(lb_token_state), "RPOD_LISTENBRAINZ_TOKEN_STATE",
                 ".local/state/rpod-sim/listenbrainz_token");
    /* `make web-sim` serves the web UI against this. */
    resolve_path(control_sock, sizeof(control_sock), "RPOD_CONTROL_SOCK",
                 ".local/state/rpod-sim/control.sock");
    resolve_path(vis_fifo, sizeof(vis_fifo), "RPOD_VIS_FIFO",
                 ".local/state/rpod-sim/mpd/visualizer.fifo");
    resolve_path(cover_cache, sizeof(cover_cache), "RPOD_COVER_CACHE", ".cache/rpod-sim/covers");
    /* `make web-sim` uploads into it. */
    resolve_path(video_dir, sizeof(video_dir), "RPOD_VIDEO_DIR", ".local/state/rpod-sim/videos");
    resolve_path(video_state, sizeof(video_state), "RPOD_VIDEO_STATE", ".local/state/rpod-sim/video");
    resolve_path(shutdown_state, sizeof(shutdown_state), "RPOD_SHUTDOWN_STATE", ".local/state/rpod-sim/shutdown");

    rpod_app_config_t cfg = {
        .mpd_socket = mpd_socket,
        .listenbrainz_token = getenv("RPOD_LISTENBRAINZ_TOKEN"),
        .listenbrainz_token_state = lb_token_state,
        .listenbrainz_queue = lb_queue,
        .scrobbler_state = scrobbler_state,
        .volume_state = volume_state,
        .backlight_state = backlight_state,
        .theme_state = theme_state,
        .alarm_state = alarm_state,
        .scores_state = scores_state,
        .lock_state = lock_state,
        .wheel_state = wheel_state,
        .control_sock = control_sock,
        .vis_fifo = vis_fifo,
        .cover_cache_dir = cover_cache,
        .video_dir = video_dir,
        .video_state = video_state,
        /* Off: the sim talks to the desktop's own BlueZ, whose pairing
         * prompts belong to the desktop. RPOD_BT_DEFAULT_AGENT=1 for a
         * private bus (python-dbusmock) where taking them over is fine. */
        .bt_default_agent = getenv("RPOD_BT_DEFAULT_AGENT") != NULL &&
                            strcmp(getenv("RPOD_BT_DEFAULT_AGENT"), "1") == 0,
        /* No battery unless RPOD_BATTERY_FAKE (power/battery.h) fakes one,
         * or RPOD_BATTERY_I2C names a real bus. */
        .battery_i2c = getenv("RPOD_BATTERY_I2C"),
        .shutdown_state = shutdown_state,
        /* Shutting down only says it would: it would be the desktop. */
        .can_power_off = false,
    };

    int rc = rpod_app_run(&sim_board, &cfg);
    if (rc != 0) {
        fprintf(stderr, "rpod-sim: start MPD first with `make mpd-dev`, or set RPOD_MPD_SOCKET.\n");
    }
    return rc;
}
