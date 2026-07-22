/*
 * Music submenu (Playlists/Artists/Albums/Songs/Genres, docs/PLAN.md §8.1)
 * and the MPD-backed drill-down screens beneath it.
 */

#ifndef RPOD_MUSIC_SCREENS_H
#define RPOD_MUSIC_SCREENS_H

#include "screen_stack.h"

typedef struct rpod_mpd rpod_mpd_t;

/* build_fn for rpod_screen_stack_push(). `ctx` is the shared rpod_mpd_t*
 * connection -- not owned here, so pass NULL as ctx_free when pushing. */
void rpod_music_menu_build(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx);

/* Entry points into the browse drill-down for screens outside this file
 * (Search results): push the album list for an artist, or the song list for
 * an album (optionally scoped to an artist; NULL = album name only). The
 * names are copied -- callers keep ownership. */
void rpod_music_push_artist_albums(rpod_screen_stack_t *stack, rpod_mpd_t *mpd, const char *artist);
void rpod_music_push_album_songs(rpod_screen_stack_t *stack, rpod_mpd_t *mpd,
                                  const char *artist_or_null, const char *album);

#endif /* RPOD_MUSIC_SCREENS_H */
