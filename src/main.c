/*
 * rPod on-device entry point.
 *
 * Selects the active board from RPOD_BOARD (src/platform/board_device.c) and
 * hands it to the shared app bootstrap (src/app.c) -- the same one the desktop
 * simulator runs. This is where a real panel + real buttons drive the full UI;
 * for the Waveshare 1.44" LCD HAT, deploy with RPOD_BOARD=waveshare-144.
 *
 * Filesystem defaults match docs/PLAN.md §6/§6.5 and rpod.service's
 * StateDirectory=rpod (/var/lib/rpod). Each is overridable via its
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
    const rpod_board_t *board = rpod_board_select();

    /* Copied into local buffers rather than pointing at getenv() results,
     * which a later setenv() (rpod_app_run seeds RPOD_VIS_FIFO) may invalidate. */
    char mpd_socket[512], lb_queue[512], scrobbler_state[512], vis_fifo[512];
    resolve(mpd_socket, sizeof(mpd_socket), "RPOD_MPD_SOCKET", "/run/mpd/socket");
    resolve(lb_queue, sizeof(lb_queue), "RPOD_LISTENBRAINZ_QUEUE",
            "/var/lib/rpod/listenbrainz_queue.jsonl");
    resolve(scrobbler_state, sizeof(scrobbler_state), "RPOD_SCROBBLER_STATE",
            "/var/lib/rpod/scrobbler_state");
    resolve(vis_fifo, sizeof(vis_fifo), "RPOD_VIS_FIFO", "/run/mpd/visualizer.fifo");

    rpod_app_config_t cfg = {
        .mpd_socket = mpd_socket,
        .listenbrainz_token = getenv("RPOD_LISTENBRAINZ_TOKEN"),
        .listenbrainz_queue = lb_queue,
        .scrobbler_state = scrobbler_state,
        .vis_fifo = vis_fifo,
    };

    fprintf(stderr, "rpod: board '%s' (%s)\n", board->id, board->name);
    return rpod_app_run(board, &cfg);
}
