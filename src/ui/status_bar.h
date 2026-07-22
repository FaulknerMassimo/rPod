/*
 * Persistent top status bar: time on the left, "rPod" in the centre (or,
 * whenever MPD has a current track -- playing or paused, matching
 * main_menu.c's own "Now Playing" row condition -- that track's title plus
 * a live audio visualizer) in the centre, and a battery indicator on the
 * right. Lives on LVGL's system layer (see rpod_status_bar_create()) so
 * it's created once and stays put across every rpod_screen_stack_t
 * push/pop, instead of being rebuilt per screen the way the old per-screen
 * header was.
 */

#ifndef RPOD_STATUS_BAR_H
#define RPOD_STATUS_BAR_H

#include "lvgl.h"
#include "audio/mpd_client.h"
#include "audio/visualizer.h"

typedef struct rpod_status_bar rpod_status_bar_t;

/*
 * Creates the bar on disp's system layer (lv_display_get_layer_sys()) --
 * the same object instance is drawn above every screen the stack pushes, so
 * callers create this once at startup and never touch it again. `mpd` must
 * outlive the bar (the app-lifetime connection, same assumption every
 * screen already makes).
 *
 * Also starts the single background thread reading the MPD visualizer FIFO
 * (audio/visualizer.h) -- see rpod_status_bar_shared_visualizer() below.
 * Call this at most once per process.
 */
rpod_status_bar_t *rpod_status_bar_create(lv_display_t *disp, rpod_mpd_t *mpd);

/*
 * The status bar's own rpod_visualizer_t, for other screens (Now Playing)
 * that want to show a bigger view of the same live spectrum. A FIFO only
 * has one real reader -- two independent rpod_visualizer_start() calls
 * would each get a random split of the same byte stream -- so screens must
 * reuse this handle rather than starting their own. Returns NULL until
 * rpod_status_bar_create() has run.
 */
rpod_visualizer_t *rpod_status_bar_shared_visualizer(void);

/*
 * Tells the bar whether the Now Playing screen is currently on top. While it
 * is, the bar reads a plain "Now Playing" with no visualizer (the screen
 * itself already shows the track title and a full visualizer, so repeating
 * them up here is redundant); otherwise the bar mirrors the current track's
 * title + live visualizer as usual. Now Playing calls this true on build and
 * false on delete. No-ops before rpod_status_bar_create() has run.
 */
void rpod_status_bar_set_now_playing_visible(bool visible);

#endif /* RPOD_STATUS_BAR_H */
