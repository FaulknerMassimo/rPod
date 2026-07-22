/*
 * Music > Search: wheel-driven on-screen keyboard with debounced
 * search-as-you-type results (Artists / Albums / Songs) from MPD.
 */

#ifndef RPOD_SEARCH_SCREEN_H
#define RPOD_SEARCH_SCREEN_H

#include "screen_stack.h"

/* build_fn for rpod_screen_stack_push(). `ctx` is the shared rpod_mpd_t*
 * connection -- not owned here, so pass NULL as ctx_free when pushing. */
void rpod_search_screen_build(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx);

#endif /* RPOD_SEARCH_SCREEN_H */
