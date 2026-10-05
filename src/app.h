/*
 * Shared application bootstrap. The same screen graph runs on device and in
 * the desktop simulator; only the display and input backends differ, and
 * those are supplied by the active board (src/platform/board.h). Everything
 * else -- MPD client, ListenBrainz scrobbler, status bar, screen stack, the
 * event loop, the app-level button actions and sleep -- lives here so the two
 * entry points (src/main.c on device, tools/sim/sim_main.c) stay tiny.
 */

#ifndef RPOD_APP_H
#define RPOD_APP_H

#include "platform/board.h"

#include <stdbool.h>

/* Filesystem locations the app needs. Each entry point resolves these (with
 * its own environment-variable overrides and defaults -- sim state under
 * $HOME, device state under /run and /var/lib) and passes them in already
 * decided, so rpod_app_run() itself stays free of any sim-vs-device policy.
 * NULL fields fall back to a built-in default where one exists. */
typedef struct {
    const char *mpd_socket;           /* required */
    const char *listenbrainz_token;   /* NULL/"" -> scrobbling is an inert no-op */
    const char *listenbrainz_queue;   /* required */
    const char *scrobbler_state;      /* required */
    const char *volume_state;         /* per-device volumes; NULL -> not persisted */
    const char *vis_fifo;             /* NULL -> status bar's own RPOD_VIS_FIFO default */
    const char *cover_cache_dir;      /* decoded cover art; NULL -> kept in memory only */
    /* Claim BlueZ's default pairing agent (audio/bluetooth.h): the device,
     * where nothing else pairs -- not the sim, on the desktop's BlueZ. */
    bool bt_default_agent;
} rpod_app_config_t;

/* Runs the app on `board` to completion (never returns under normal
 * operation -- it enters the LVGL event loop). Returns non-zero only on a
 * fatal startup failure (no display, or no MPD connection). Assumes LVGL is
 * NOT yet initialised -- it calls lv_init() itself so display/metrics/timers
 * are set up in the right order. */
int rpod_app_run(const rpod_board_t *board, const rpod_app_config_t *cfg);

#endif /* RPOD_APP_H */
