/*
 * Persistent top status bar: time on the left, "rPod" in the centre (or,
 * whenever MPD has a current track -- playing or paused, matching
 * main_menu.c's own "Now Playing" row condition -- that track's title plus
 * a live audio visualizer) in the centre, and a battery indicator on the
 * right -- with an AirPods glyph beside it while they're connected. Lives on LVGL's system layer (see rpod_status_bar_create()) so
 * it's created once and stays put across every rpod_screen_stack_t
 * push/pop, instead of being rebuilt per screen the way the old per-screen
 * header was.
 *
 * In the classic themes (ui/theme.h) it's the iPod's title bar instead: the
 * play state on the left, the current screen's title in the centre, the
 * battery on the right, and a rule along the bottom.
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
 * Pins the glass bar's centre to a plain `title` (copied), with no
 * visualizer, in place of the current track's title; NULL lets it mirror the
 * track again. Now Playing pins "Now Playing" while it's up (the screen
 * itself already shows the track title and a full visualizer, so repeating
 * them up here is redundant), and Music Quiz "Music Quiz" (where the title
 * would give the answer away). The classic title bar shows the screen's own
 * title anyway. No-ops before rpod_status_bar_create() has run.
 */
void rpod_status_bar_set_fixed_title(const char *title);

/* While the screen is locked (ui/screens/screen_lock.h), the classic title
 * bar shows the iPod's lock beside the play state, where the iPod showed its
 * hold switch. */
void rpod_status_bar_set_locked(bool locked);

/* The top screen's title (ui/screens/screen_stack.h keeps it current),
 * which the classic title bar shows. */
void rpod_status_bar_set_screen_title(const char *title);

/* Re-reads the player now rather than at the next 1 s poll -- after a
 * button changed it, so the classic play/pause icon keeps up. */
void rpod_status_bar_refresh(void);

/* Rebuilds the bar in the active theme's look, after a theme change. */
void rpod_status_bar_restyle(void);

#endif /* RPOD_STATUS_BAR_H */
