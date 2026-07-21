/*
 * Music submenu (Playlists/Artists/Albums/Songs/Genres, docs/PLAN.md §8.1)
 * and the MPD-backed drill-down screens beneath it.
 */

#ifndef RPOD_MUSIC_SCREENS_H
#define RPOD_MUSIC_SCREENS_H

#include "screen_stack.h"

/* build_fn for rpod_screen_stack_push(). `ctx` is the shared rpod_mpd_t*
 * connection -- not owned here, so pass NULL as ctx_free when pushing. */
void rpod_music_menu_build(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx);

#endif /* RPOD_MUSIC_SCREENS_H */
