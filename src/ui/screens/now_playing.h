/*
 * Now Playing screen. §8.3: progress updates at 1 Hz, not per-frame.
 */

#ifndef RPOD_NOW_PLAYING_H
#define RPOD_NOW_PLAYING_H

#include "screen_stack.h"

/* build_fn for rpod_screen_stack_push(). `ctx` is the shared rpod_mpd_t*
 * connection -- this screen doesn't own it, so pass NULL as ctx_free. */
void rpod_now_playing_build(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx);

#endif /* RPOD_NOW_PLAYING_H */
