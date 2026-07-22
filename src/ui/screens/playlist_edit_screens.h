/*
 * New Playlist naming (on-screen keyboard, same input model as
 * search_screen.c) and the "add songs to a playlist" picker beneath it.
 */

#ifndef RPOD_PLAYLIST_EDIT_SCREENS_H
#define RPOD_PLAYLIST_EDIT_SCREENS_H

#include "screen_stack.h"

typedef struct rpod_mpd rpod_mpd_t;

/* build_fn for rpod_screen_stack_push(). `ctx` is the shared rpod_mpd_t*
 * connection -- not owned here, so pass NULL as ctx_free when pushing.
 * On confirm, replaces itself (pop + push) with the add-songs picker below,
 * scoped to the newly typed name. */
void rpod_new_playlist_name_build(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx);

/* Pushes a flat Songs browse list in "add to playlist" mode: selecting a
 * row appends that song to `playlist_name` (creating it on the first song,
 * see rpod_mpd_playlist_add_song()) instead of playing it, and stays on the
 * list so several songs can be added in one visit. `playlist_name` is
 * copied -- the caller keeps ownership of its own copy. */
void rpod_push_add_songs_screen(rpod_screen_stack_t *stack, rpod_mpd_t *mpd, const char *playlist_name);

#endif /* RPOD_PLAYLIST_EDIT_SCREENS_H */
