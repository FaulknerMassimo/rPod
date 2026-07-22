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

/* Browse queries. On success, *out is a malloc'd array of *out_count
 * entries (NULL/0 if the result is empty) — free with the matching
 * rpod_mpd_free_*(). artist_or_null / album_or_null / genre pass NULL (or,
 * for genre, are simply omitted) to mean "no filter". */
bool rpod_mpd_list_artists(rpod_mpd_t *mpd, rpod_mpd_item_t **out, size_t *out_count);
bool rpod_mpd_list_albums(rpod_mpd_t *mpd, const char *artist_or_null, rpod_mpd_item_t **out, size_t *out_count);
bool rpod_mpd_list_genres(rpod_mpd_t *mpd, rpod_mpd_item_t **out, size_t *out_count);
bool rpod_mpd_list_artists_in_genre(rpod_mpd_t *mpd, const char *genre, rpod_mpd_item_t **out, size_t *out_count);
bool rpod_mpd_list_playlists(rpod_mpd_t *mpd, rpod_mpd_item_t **out, size_t *out_count);

bool rpod_mpd_list_songs(rpod_mpd_t *mpd, const char *artist_or_null, const char *album_or_null,
                          rpod_mpd_song_t **out, size_t *out_count);
bool rpod_mpd_list_playlist_songs(rpod_mpd_t *mpd, const char *playlist_name,
                                   rpod_mpd_song_t **out, size_t *out_count);

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
bool rpod_mpd_toggle_pause(rpod_mpd_t *mpd);
bool rpod_mpd_next(rpod_mpd_t *mpd);
bool rpod_mpd_previous(rpod_mpd_t *mpd);

/* Outputs (§6.2's DAC/Bluetooth switching). */
bool rpod_mpd_list_outputs(rpod_mpd_t *mpd, rpod_mpd_output_t **out, size_t *out_count);
bool rpod_mpd_enable_output(rpod_mpd_t *mpd, unsigned id);
bool rpod_mpd_disable_output(rpod_mpd_t *mpd, unsigned id);
void rpod_mpd_free_outputs(rpod_mpd_output_t *outputs);

#endif /* RPOD_MPD_CLIENT_H */
