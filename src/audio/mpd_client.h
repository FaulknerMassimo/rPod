/*
 * Thin wrapper over libmpdclient (docs/PLAN.md §6.1). Used by the UI's
 * Music screens for browsing/playback and by Settings > Audio Output for
 * switching outputs (§6.2). Shared between the simulator and the on-device
 * build — nothing here is sim-specific.
 */

#ifndef RPOD_MPD_CLIENT_H
#define RPOD_MPD_CLIENT_H

#include <stdbool.h>
#include <stddef.h>

typedef struct rpod_mpd rpod_mpd_t;

typedef enum {
    RPOD_MPD_STATE_UNKNOWN = 0,
    RPOD_MPD_STATE_STOP,
    RPOD_MPD_STATE_PLAY,
    RPOD_MPD_STATE_PAUSE,
} rpod_mpd_state_t;

typedef struct {
    rpod_mpd_state_t state;
    char title[256];
    char artist[256];
    char album[256];
    char uri[512];
    unsigned elapsed_s;
    unsigned duration_s;
    /* How many songs are on the play queue right now. Lets a caller tell a
     * genuinely empty queue apart from one that has simply played out to its
     * end and stopped (state == STOP with queue_len > 0) -- see
     * rpod_mpd_cue_first_paused(). */
    unsigned queue_len;
} rpod_mpd_status_t;

/* A generic name-only browse row: artist, album, genre, or playlist name. */
typedef struct {
    char name[256];
} rpod_mpd_item_t;

typedef struct {
    char title[256];
    char artist[256];
    char album[256];
    char uri[512];
    unsigned duration_s;
} rpod_mpd_song_t;

typedef struct {
    unsigned id;
    char name[128];
    bool enabled;
} rpod_mpd_output_t;

/* Connects synchronously. socket_path is passed straight to
 * mpd_connection_new() as the host argument — libmpdclient treats a path
 * starting with '/' as a Unix socket. Returns NULL on allocation failure;
 * check rpod_mpd_connect()'s result with rpod_mpd_is_connected() to
 * distinguish a live connection from one that failed to reach the server. */
rpod_mpd_t *rpod_mpd_connect(const char *socket_path);
bool rpod_mpd_is_connected(const rpod_mpd_t *mpd);
void rpod_mpd_disconnect(rpod_mpd_t *mpd);

bool rpod_mpd_get_status(rpod_mpd_t *mpd, rpod_mpd_status_t *out);

/* rpod_mpd_get_status(), but if it finds playback has stopped at the end of a
 * non-empty queue it re-cues to the first song paused (rpod_mpd_cue_first_paused())
 * and returns *that* settled status instead of the transient stopped one. UI
 * pollers should use this rather than rpod_mpd_get_status() so none of them
 * ever renders the momentary "unknown song / unknown artist" that MPD's
 * dropped current-song leaves behind at end-of-queue -- every poll that sees
 * the stop fixes it in the same tick, whichever fires first. */
bool rpod_mpd_get_status_settled(rpod_mpd_t *mpd, rpod_mpd_status_t *out);

/* Browse queries. On success, *out is a malloc'd array of *out_count
 * entries (NULL/0 if the result is empty) — free with the matching
 * rpod_mpd_free_*(). artist_or_null / album_or_null / genre pass NULL (or,
 * for genre, are simply omitted) to mean "no filter". */
bool rpod_mpd_list_artists(rpod_mpd_t *mpd, rpod_mpd_item_t **out, size_t *out_count);
bool rpod_mpd_list_albums(rpod_mpd_t *mpd, const char *artist_or_null, rpod_mpd_item_t **out, size_t *out_count);
bool rpod_mpd_list_genres(rpod_mpd_t *mpd, rpod_mpd_item_t **out, size_t *out_count);
bool rpod_mpd_list_artists_in_genre(rpod_mpd_t *mpd, const char *genre, rpod_mpd_item_t **out, size_t *out_count);
bool rpod_mpd_list_playlists(rpod_mpd_t *mpd, rpod_mpd_item_t **out, size_t *out_count);

/* When album_or_null is given, artist_or_null is ignored -- a track's own
 * Artist tag can differ from the artist used to browse down to this album
 * (features, collaborations, compilation albums where each track credits a
 * different performer under one shared AlbumArtist), so ANDing both
 * constraints together would silently drop real tracks that belong to the
 * album but aren't credited to that specific artist. The album name alone
 * already identifies the right set of songs; artist_or_null only narrows
 * the results when no album is given. */
bool rpod_mpd_list_songs(rpod_mpd_t *mpd, const char *artist_or_null, const char *album_or_null,
                          rpod_mpd_song_t **out, size_t *out_count);
bool rpod_mpd_list_playlist_songs(rpod_mpd_t *mpd, const char *playlist_name,
                                   rpod_mpd_song_t **out, size_t *out_count);

/* Appends `uri` to the named stored playlist -- MPD's "playlistadd", which
 * creates the playlist if it doesn't already exist yet. This is the only
 * way to bring a new playlist into existence: MPD has no "create empty
 * playlist" command, so a playlist first appears once its first song has
 * been added this way. */
bool rpod_mpd_playlist_add_song(rpod_mpd_t *mpd, const char *playlist_name, const char *uri);

/* Case-insensitive substring search across all tags (MPD's `search any`).
 * Rejects an empty query (MPD would too). max_results > 0 caps the result
 * server-side via a window, bounding the transfer/allocation for broad
 * queries on a big library; 0 means uncapped. */
bool rpod_mpd_search_songs(rpod_mpd_t *mpd, const char *query, unsigned max_results,
                            rpod_mpd_song_t **out, size_t *out_count);

void rpod_mpd_free_items(rpod_mpd_item_t *items);
void rpod_mpd_free_songs(rpod_mpd_song_t *songs);

/* Fetches raw (still-encoded, e.g. JPEG) cover art bytes for `uri` --
 * MPD's "readpicture" (embedded ID3/FLAC picture tag) first, falling back
 * to "albumart" (a cover.jpg/folder.jpg file beside the track) if that
 * comes back empty. On success, *out is a malloc'd buffer of *out_size
 * bytes -- free with rpod_mpd_free_cover_art(). Returns false if the track
 * has no art at all (common, not an error) or if it exceeds a generous
 * size cap meant to bound a single allocation on a 512 MB device. */
bool rpod_mpd_get_cover_art(rpod_mpd_t *mpd, const char *uri, unsigned char **out, size_t *out_size);
void rpod_mpd_free_cover_art(unsigned char *data);

/* Transport: replaces the queue with the single uri and plays it. */
bool rpod_mpd_play_uri(rpod_mpd_t *mpd, const char *uri);

/* Transport: replaces the queue with all `count` songs (in array order) and
 * starts playing from the first one -- used by an album's header "Play"
 * button. */
bool rpod_mpd_play_songs(rpod_mpd_t *mpd, const rpod_mpd_song_t *songs, size_t count);

/* Like rpod_mpd_play_songs(), but starts playback at `start_index` instead of
 * the first song -- the whole collection is still queued so playback carries
 * on into the following tracks. This is what selecting one song inside an
 * album or playlist does: the tapped song plays now, and the rest of the
 * album/playlist follows instead of the queue ending after a single track.
 * start_index must be < count. */
bool rpod_mpd_play_songs_from(rpod_mpd_t *mpd, const rpod_mpd_song_t *songs, size_t count,
                              size_t start_index);

/* Like rpod_mpd_play_songs(), but shuffles queue order (MPD's "shuffle",
 * applied after the songs are queued) before playing -- the album header's
 * "Shuffle" button. */
bool rpod_mpd_play_songs_shuffled(rpod_mpd_t *mpd, const rpod_mpd_song_t *songs, size_t count);
bool rpod_mpd_toggle_pause(rpod_mpd_t *mpd);
bool rpod_mpd_next(rpod_mpd_t *mpd);
bool rpod_mpd_previous(rpod_mpd_t *mpd);

/* Re-cues the queue to its first song in a *paused* state, and clears its
 * error latch on the way out. Meant to be called after playback has stopped
 * at the end of a non-empty queue: MPD's default end-of-queue behaviour
 * (repeat off) drops the "current song" entirely, so Now Playing has nothing
 * to show and reads as "unknown song / unknown artist". Cueing back to song 0
 * and pausing leaves the first track shown and ready to play again, instead
 * of looping the album/playlist forever. No-op guarding (only calling this
 * when state == STOP and queue_len > 0) is the caller's job. */
bool rpod_mpd_cue_first_paused(rpod_mpd_t *mpd);

/* Outputs (§6.2's DAC/Bluetooth switching). */
bool rpod_mpd_list_outputs(rpod_mpd_t *mpd, rpod_mpd_output_t **out, size_t *out_count);
bool rpod_mpd_enable_output(rpod_mpd_t *mpd, unsigned id);
bool rpod_mpd_disable_output(rpod_mpd_t *mpd, unsigned id);
void rpod_mpd_free_outputs(rpod_mpd_output_t *outputs);

#endif /* RPOD_MPD_CLIENT_H */
