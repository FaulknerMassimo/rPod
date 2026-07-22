/*
 * A snapshot of "which songs are in playlists", used to draw the per-row
 * status indicator on the song lists (a heart for a liked song, a checkmark
 * for one that's in some other playlist). Built by scanning every stored
 * playlist's contents into two URI sets -- liked vs. any-other-playlist --
 * so a list row only needs an O(1) lookup, which matters for the virtualized
 * whole-library Songs list. Rebuilt when a list screen regains focus, so it
 * reflects edits made in the Add-to-Playlist picker or on Now Playing.
 */

#ifndef RPOD_PLAYLIST_MEMBERSHIP_H
#define RPOD_PLAYLIST_MEMBERSHIP_H

#include "audio/mpd_client.h"
#include <stdbool.h>

typedef struct rpod_playlist_index rpod_playlist_index_t;

/* Scans all playlists once. Never returns NULL for a live connection (an
 * empty index is valid); returns NULL only on allocation failure. */
rpod_playlist_index_t *rpod_playlist_index_build(rpod_mpd_t *mpd);

/* Re-scans in place, reusing the same handle. */
void rpod_playlist_index_rebuild(rpod_playlist_index_t *idx, rpod_mpd_t *mpd);

void rpod_playlist_index_free(rpod_playlist_index_t *idx);

/* In "Liked Songs". */
bool rpod_playlist_index_is_liked(const rpod_playlist_index_t *idx, const char *uri);
/* In some playlist other than "Liked Songs". */
bool rpod_playlist_index_in_other(const rpod_playlist_index_t *idx, const char *uri);

/* Adds `uri` to "Liked Songs" if absent, removes it if present. Sets
 * *now_liked to the resulting state. Returns false on a connection error. */
bool rpod_playlist_liked_toggle(rpod_mpd_t *mpd, const char *uri, bool *now_liked);

#endif /* RPOD_PLAYLIST_MEMBERSHIP_H */
