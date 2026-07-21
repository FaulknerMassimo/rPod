/*
 * Root menu: Music, Now Playing (only when MPD reports a current song),
 * Settings, Extras (docs/PLAN.md §8.1, §11).
 */

#ifndef RPOD_MAIN_MENU_H
#define RPOD_MAIN_MENU_H

#include "screen_stack.h"

/* build_fn for rpod_screen_stack_push() -- push this first, as the root
 * screen. `ctx` is the shared rpod_mpd_t* connection; not owned here, so
 * pass NULL as ctx_free when pushing. */
void rpod_main_menu_build(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx);

#endif /* RPOD_MAIN_MENU_H */
