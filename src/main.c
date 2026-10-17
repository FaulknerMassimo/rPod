/*
 * rPod on-device entry point.
 *
 * Hands the device board (src/platform/board_device.c) to the shared app
 * bootstrap (src/app.c) -- the same one the desktop simulator runs.
 *
 * Filesystem defaults match docs/PLAN.md §6/§6.5 and rpod.service's
 * StateDirectory=rpod (/var/lib/rpod) and CacheDirectory=rpod
 * (/var/cache/rpod). Each is overridable via its
 * environment variable (see /etc/rpod/env, rpod.service's EnvironmentFile).
 */

#include "app.h"
#include "platform/board.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void resolve(char *out, size_t out_size, const char *env, const char *def)
{
    const char *v = getenv(env);
    snprintf(out, out_size, "%s", (v != NULL && v[0] != '\0') ? v : def);
}

int main(void)
{
    const rpod_board_t *board = rpod_device_board();

    /* Copied into local buffers rather than pointing at getenv() results,
     * which a later setenv() (rpod_app_run seeds RPOD_VIS_FIFO) may invalidate. */
    char mpd_socket[512], lb_queue[512], scrobbler_state[512], volume_state[512], vis_fifo[512];
    char cover_cache[512], backlight_state[512], theme_state[512];
    char alarm_state[512], scores_state[512], lock_state[512];
    char wheel_state[512], lb_token_state[512], control_sock[512];
    char video_dir[512], video_state[512], battery_i2c[64], shutdown_state[512], ha_state[512];
    resolve(mpd_socket, sizeof(mpd_socket), "RPOD_MPD_SOCKET", "/run/mpd/socket");
    resolve(lb_queue, sizeof(lb_queue), "RPOD_LISTENBRAINZ_QUEUE",
            "/var/lib/rpod/listenbrainz_queue.jsonl");
    resolve(scrobbler_state, sizeof(scrobbler_state), "RPOD_SCROBBLER_STATE",
            "/var/lib/rpod/scrobbler_state");
    resolve(volume_state, sizeof(volume_state), "RPOD_VOLUME_STATE", "/var/lib/rpod/volumes");
    resolve(backlight_state, sizeof(backlight_state), "RPOD_BACKLIGHT_STATE",
            "/var/lib/rpod/backlight");
    resolve(theme_state, sizeof(theme_state), "RPOD_THEME_STATE", "/var/lib/rpod/theme");
    resolve(alarm_state, sizeof(alarm_state), "RPOD_ALARM_STATE", "/var/lib/rpod/alarm");
    resolve(scores_state, sizeof(scores_state), "RPOD_SCORES_STATE", "/var/lib/rpod/scores");
    resolve(lock_state, sizeof(lock_state), "RPOD_LOCK_STATE", "/var/lib/rpod/lock");
    resolve(wheel_state, sizeof(wheel_state), "RPOD_WHEEL_STATE", "/var/lib/rpod/wheel");
    resolve(lb_token_state, sizeof(lb_token_state), "RPOD_LISTENBRAINZ_TOKEN_STATE",
            "/var/lib/rpod/listenbrainz_token");
    /* rpod.service's RuntimeDirectory=rpod-ui. */
    resolve(control_sock, sizeof(control_sock), "RPOD_CONTROL_SOCK", "/run/rpod-ui/control.sock");
    resolve(vis_fifo, sizeof(vis_fifo), "RPOD_VIS_FIFO", "/run/mpd/visualizer.fifo");
    /* rpod.service's CacheDirectory=rpod. */
    resolve(cover_cache, sizeof(cover_cache), "RPOD_COVER_CACHE", "/var/cache/rpod/covers");
    /* Beside the music partition's mount, not in it: MPD would take every
     * video in its music_directory for a song (docs/PLAN.md §8.7). */
    resolve(video_dir, sizeof(video_dir), "RPOD_VIDEO_DIR", "/media/videos");
    resolve(video_state, sizeof(video_state), "RPOD_VIDEO_STATE", "/var/lib/rpod/video");
    /* The PiSugar 2's bus (docs/PLAN.md §7.6): GPIO 2/3, i2c-dev loaded by
     * system/pisugar/. */
    resolve(battery_i2c, sizeof(battery_i2c), "RPOD_BATTERY_I2C", "/dev/i2c-1");
    resolve(shutdown_state, sizeof(shutdown_state), "RPOD_SHUTDOWN_STATE", "/var/lib/rpod/shutdown");
    resolve(ha_state, sizeof(ha_state), "RPOD_HA_STATE", "/var/lib/rpod/homeassistant");

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
        .bt_default_agent = true,
        .battery_i2c = battery_i2c,
        .shutdown_state = shutdown_state,
        .ha_state = ha_state,
        .can_power_off = true,
    };

    fprintf(stderr, "rpod: board: %s\n", board->name);
    return rpod_app_run(board, &cfg);
}
