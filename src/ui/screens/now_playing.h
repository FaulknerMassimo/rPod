/*
 * Now Playing screen. §8.3: progress updates at 1 Hz, not per-frame.
 */

#ifndef RPOD_NOW_PLAYING_H
#define RPOD_NOW_PLAYING_H

#include "screen_stack.h"

/* build_fn for rpod_screen_stack_push(). `ctx` is the shared rpod_mpd_t*
 * connection -- this screen doesn't own it, so pass NULL as ctx_free. */
void rpod_now_playing_build(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx);

/* Makes a Now Playing screen that's up refresh on the next LVGL tick rather
 * than at its next 1 Hz poll -- for when the song has just been changed
 * (Next/Prev), so the new title and art don't lag up to a second behind.
 * No-op when there's no Now Playing screen. */
void rpod_now_playing_refresh(void);

/* Classic themes: shows `percent` (< 0: no volume to set, an empty bar) on
 * the volume bar that takes the progress bar's place while the wheel turns
 * it, as on the iPod, and restarts its countdown. False -- nothing shown --
 * unless a classic Now Playing is the screen up; the HUD shows it then
 * (ui/volume_control.c). */
bool rpod_now_playing_show_volume(int percent);

/* True while that volume bar is up. */
bool rpod_now_playing_volume_shown(void);

#endif /* RPOD_NOW_PLAYING_H */
