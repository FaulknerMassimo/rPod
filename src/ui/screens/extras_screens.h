/*
 * Extras (docs/PLAN.md §8.5), the iPod's: Clock (clock_screens.h), Games --
 * Brick (brick_screen.h) and Music Quiz (music_quiz.h) -- and Screen Lock
 * (screen_lock.h).
 */

#ifndef RPOD_EXTRAS_SCREENS_H
#define RPOD_EXTRAS_SCREENS_H

#include "screen_stack.h"

/* build_fn for rpod_screen_stack_push(). `ctx` is the shared rpod_mpd_t*
 * connection -- not owned here, so pass NULL as ctx_free when pushing. */
void rpod_extras_menu_build(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx);

#endif /* RPOD_EXTRAS_SCREENS_H */
