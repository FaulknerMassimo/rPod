#include "mpd_client.h"

#include <mpd/client.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct rpod_mpd {
    struct mpd_connection *conn;
};

/* Grows *arr (elem_size elements) by one slot, doubling capacity as needed.
 * Returns the (possibly moved) array, or NULL on allocation failure — the
 * caller's existing *arr is left untouched by realloc() in that case, but
 * every call site here treats NULL as fatal for the whole query and frees
 * what it already had. */
static void *array_grow(void *arr, size_t count, size_t *cap, size_t elem_size)
{
    if (count < *cap) {
        return arr;
    }
    size_t new_cap = (*cap == 0) ? 16 : (*cap * 2);
    void *new_arr = realloc(arr, new_cap * elem_size);
    if (new_arr == NULL) {
        return NULL;
    }
    *cap = new_cap;
    return new_arr;
}

static void copy_tag(char *dst, size_t dst_size, const struct mpd_song *song, enum mpd_tag_type type)
{
    const char *v = mpd_song_get_tag(song, type, 0);
    snprintf(dst, dst_size, "%s", v != NULL ? v : "");
}

/* Some tags (artist, in particular -- a featured-artist track tags each
 * name as its own "Artist:" line rather than one delimited string) can
 * repeat on a single song; mpd_song_get_tag()'s index parameter walks
 * those repeats. copy_tag() above only ever reads index 0, silently
 * dropping every artist past the first -- confirmed against a real
 * multi-artist track in testing. This joins all of them with ", ". */
static void copy_multi_tag(char *dst, size_t dst_size, const struct mpd_song *song, enum mpd_tag_type type)
{
    dst[0] = '\0';
    size_t len = 0;
    for (unsigned i = 0; len < dst_size; i++) {
        const char *v = mpd_song_get_tag(song, type, i);
        if (v == NULL) {
            break;
        }
        /* snprintf() returns how many bytes it *would* take, not how many
         * fit -- if that's >= the room left, the buffer is already full
         * and truncated (but still NUL-terminated); stop before `len`
         * overruns dst_size and underflows the next iteration's room. */
        int n = snprintf(dst + len, dst_size - len, "%s%s", (i > 0) ? ", " : "", v);
        if (n < 0 || (size_t)n >= dst_size - len) {
            break;
        }
        len += (size_t)n;
    }
}

/* libmpdclient latches an error on the connection after any failed
 * command; until it's cleared, every subsequent command silently fails
 * too. Every failure path in this file must clear it before returning, or
 * one bad/unsupported query (e.g. stored playlists disabled server-side)
 * permanently wedges the connection for the rest of the session -- caught
 * by testing against a real MPD instance. */
static bool fail(rpod_mpd_t *mpd)
{
    mpd_connection_clear_error(mpd->conn);
    return false;
}

static void copy_song(rpod_mpd_song_t *dst, const struct mpd_song *song)
{
    memset(dst, 0, sizeof(*dst));
    copy_tag(dst->title, sizeof(dst->title), song, MPD_TAG_TITLE);
    copy_multi_tag(dst->artist, sizeof(dst->artist), song, MPD_TAG_ARTIST);
    copy_tag(dst->album, sizeof(dst->album), song, MPD_TAG_ALBUM);
    const char *uri = mpd_song_get_uri(song);
    snprintf(dst->uri, sizeof(dst->uri), "%s", uri != NULL ? uri : "");
    dst->duration_s = mpd_song_get_duration(song);
}

rpod_mpd_t *rpod_mpd_connect(const char *socket_path)
{
    rpod_mpd_t *mpd = calloc(1, sizeof(*mpd));
    if (mpd == NULL) {
        return NULL;
    }
    mpd->conn = mpd_connection_new(socket_path, 0, 0);
    if (mpd->conn == NULL) {
        free(mpd);
        return NULL;
    }
    return mpd;
}

bool rpod_mpd_is_connected(const rpod_mpd_t *mpd)
{
    return mpd != NULL && mpd->conn != NULL &&
           mpd_connection_get_error(mpd->conn) == MPD_ERROR_SUCCESS;
}

void rpod_mpd_disconnect(rpod_mpd_t *mpd)
{
    if (mpd == NULL) {
        return;
    }
    if (mpd->conn != NULL) {
        mpd_connection_free(mpd->conn);
    }
    free(mpd);
}

bool rpod_mpd_get_status(rpod_mpd_t *mpd, rpod_mpd_status_t *out)
{
    memset(out, 0, sizeof(*out));

    struct mpd_status *status = mpd_run_status(mpd->conn);
    if (status == NULL) {
        return fail(mpd);
    }

    switch (mpd_status_get_state(status)) {
        case MPD_STATE_PLAY:  out->state = RPOD_MPD_STATE_PLAY;  break;
        case MPD_STATE_PAUSE: out->state = RPOD_MPD_STATE_PAUSE; break;
        case MPD_STATE_STOP:  out->state = RPOD_MPD_STATE_STOP;  break;
        default:               out->state = RPOD_MPD_STATE_UNKNOWN; break;
    }
    out->elapsed_s = mpd_status_get_elapsed_time(status);
    out->duration_s = mpd_status_get_total_time(status);
    out->queue_len = mpd_status_get_queue_length(status);
    mpd_status_free(status);

    struct mpd_song *song = mpd_run_current_song(mpd->conn);
    if (song != NULL) {
        copy_tag(out->title, sizeof(out->title), song, MPD_TAG_TITLE);
        copy_multi_tag(out->artist, sizeof(out->artist), song, MPD_TAG_ARTIST);
        copy_tag(out->album, sizeof(out->album), song, MPD_TAG_ALBUM);
        const char *uri = mpd_song_get_uri(song);
        snprintf(out->uri, sizeof(out->uri), "%s", uri != NULL ? uri : "");
        if (out->duration_s == 0) {
            out->duration_s = mpd_song_get_duration(song);
        }
        mpd_song_free(song);
    } else {
        mpd_connection_clear_error(mpd->conn);
    }
    return true;
}

bool rpod_mpd_get_status_settled(rpod_mpd_t *mpd, rpod_mpd_status_t *out)
{
    if (!rpod_mpd_get_status(mpd, out)) {
        return false;
    }
    /* Queue played out to its end: MPD (repeat off) stops and drops the
     * current song, so `out` carries the empty "unknown" track. Re-cue to the
     * first song paused and report that settled status instead, so the caller
     * never renders the transient stopped state. (cue_first_paused() is
     * defined lower down but declared in the header included above.) */
    if (out->state == RPOD_MPD_STATE_STOP && out->queue_len > 0) {
        if (!rpod_mpd_cue_first_paused(mpd)) {
            return false;
        }
        return rpod_mpd_get_status(mpd, out);
    }
    return true;
}

/* Shared implementation for the name-only tag browse queries (artists,
 * albums, genres, playlists-via-a-different-path, artists-in-genre). */
static bool list_tag(rpod_mpd_t *mpd, enum mpd_tag_type type, enum mpd_tag_type filter_type,
                      const char *filter_value, rpod_mpd_item_t **out, size_t *out_count)
{
    *out = NULL;
    *out_count = 0;

    if (!mpd_search_db_tags(mpd->conn, type)) {
        return fail(mpd);
    }
    if (filter_value != NULL &&
        !mpd_search_add_tag_constraint(mpd->conn, MPD_OPERATOR_DEFAULT, filter_type, filter_value)) {
        mpd_search_cancel(mpd->conn);
        return fail(mpd);
    }
    if (!mpd_search_commit(mpd->conn)) {
        return fail(mpd);
    }

    rpod_mpd_item_t *items = NULL;
    size_t count = 0, cap = 0;
    struct mpd_pair *pair;
    while ((pair = mpd_recv_pair_tag(mpd->conn, type)) != NULL) {
        if (pair->value[0] == '\0') {
            mpd_return_pair(mpd->conn, pair);
            continue;
        }
        rpod_mpd_item_t *grown = array_grow(items, count, &cap, sizeof(*items));
        if (grown == NULL) {
            mpd_return_pair(mpd->conn, pair);
            free(items);
            mpd_response_finish(mpd->conn);
            return fail(mpd);
        }
        items = grown;
        snprintf(items[count].name, sizeof(items[count].name), "%s", pair->value);
        count++;
        mpd_return_pair(mpd->conn, pair);
    }

    if (!mpd_response_finish(mpd->conn)) {
        free(items);
        return fail(mpd);
    }

    *out = items;
    *out_count = count;
    return true;
}

bool rpod_mpd_list_artists(rpod_mpd_t *mpd, rpod_mpd_item_t **out, size_t *out_count)
{
    return list_tag(mpd, MPD_TAG_ARTIST, MPD_TAG_ARTIST, NULL, out, out_count);
}

bool rpod_mpd_list_albums(rpod_mpd_t *mpd, const char *artist_or_null, rpod_mpd_item_t **out, size_t *out_count)
{
    return list_tag(mpd, MPD_TAG_ALBUM, MPD_TAG_ARTIST, artist_or_null, out, out_count);
}

bool rpod_mpd_list_genres(rpod_mpd_t *mpd, rpod_mpd_item_t **out, size_t *out_count)
{
    return list_tag(mpd, MPD_TAG_GENRE, MPD_TAG_GENRE, NULL, out, out_count);
}

bool rpod_mpd_list_artists_in_genre(rpod_mpd_t *mpd, const char *genre, rpod_mpd_item_t **out, size_t *out_count)
{
    return list_tag(mpd, MPD_TAG_ARTIST, MPD_TAG_GENRE, genre, out, out_count);
}

bool rpod_mpd_list_playlists(rpod_mpd_t *mpd, rpod_mpd_item_t **out, size_t *out_count)
{
    *out = NULL;
    *out_count = 0;

    if (!mpd_send_list_playlists(mpd->conn)) {
        return fail(mpd);
    }

    rpod_mpd_item_t *items = NULL;
    size_t count = 0, cap = 0;
    struct mpd_playlist *pl;
    while ((pl = mpd_recv_playlist(mpd->conn)) != NULL) {
        rpod_mpd_item_t *grown = array_grow(items, count, &cap, sizeof(*items));
        if (grown == NULL) {
            mpd_playlist_free(pl);
            free(items);
            mpd_response_finish(mpd->conn);
            return fail(mpd);
        }
        items = grown;
        snprintf(items[count].name, sizeof(items[count].name), "%s", mpd_playlist_get_path(pl));
        count++;
        mpd_playlist_free(pl);
    }

    if (!mpd_response_finish(mpd->conn)) {
        free(items);
        return fail(mpd);
    }

    *out = items;
    *out_count = count;
    return true;
}

/* Shared implementation for song-list queries fed by mpd_recv_song(): both
 * a filtered library search and a named-playlist listing land here. */
static bool recv_songs(rpod_mpd_t *mpd, rpod_mpd_song_t **out, size_t *out_count)
{
    *out = NULL;
    *out_count = 0;

    rpod_mpd_song_t *songs = NULL;
    size_t count = 0, cap = 0;
    struct mpd_song *song;
    while ((song = mpd_recv_song(mpd->conn)) != NULL) {
        rpod_mpd_song_t *grown = array_grow(songs, count, &cap, sizeof(*songs));
        if (grown == NULL) {
            mpd_song_free(song);
            free(songs);
            mpd_response_finish(mpd->conn);
            return fail(mpd);
        }
        songs = grown;
        copy_song(&songs[count], song);
        count++;
        mpd_song_free(song);
    }

    if (!mpd_response_finish(mpd->conn)) {
        free(songs);
        return fail(mpd);
    }

    *out = songs;
    *out_count = count;
    return true;
}

/* MPD's "search"/"find" commands reject a query with zero constraints
 * ("too few arguments for search", confirmed against a real server) --
 * unlike "list <tag>", which is happy to enumerate everything. So the
 * unfiltered "Songs" browse screen (no artist/album picked) can't go
 * through mpd_search_db_songs() at all; it needs the recursive database
 * listing instead. */
static bool list_all_songs(rpod_mpd_t *mpd, rpod_mpd_song_t **out, size_t *out_count)
{
    *out = NULL;
    *out_count = 0;

    if (!mpd_send_list_all_meta(mpd->conn, "")) {
        return fail(mpd);
    }

    rpod_mpd_song_t *songs = NULL;
    size_t count = 0, cap = 0;
    struct mpd_entity *entity;
    while ((entity = mpd_recv_entity(mpd->conn)) != NULL) {
        if (mpd_entity_get_type(entity) == MPD_ENTITY_TYPE_SONG) {
            rpod_mpd_song_t *grown = array_grow(songs, count, &cap, sizeof(*songs));
            if (grown == NULL) {
                mpd_entity_free(entity);
                free(songs);
                mpd_response_finish(mpd->conn);
                return fail(mpd);
            }
            songs = grown;
            copy_song(&songs[count], mpd_entity_get_song(entity));
            count++;
        }
        mpd_entity_free(entity);
    }

    if (!mpd_response_finish(mpd->conn)) {
        free(songs);
        return fail(mpd);
    }

    *out = songs;
    *out_count = count;
    return true;
}

bool rpod_mpd_list_songs(rpod_mpd_t *mpd, const char *artist_or_null, const char *album_or_null,
                          rpod_mpd_song_t **out, size_t *out_count)
{
    if (artist_or_null == NULL && album_or_null == NULL) {
        return list_all_songs(mpd, out, out_count);
    }

    /* mpd_search_db_songs(), not the add_db_songs()/searchadd variant --
     * the latter queues matches server-side as a side effect instead of
     * just listing them (caught by testing against a real MPD instance:
     * browsing an artist's album was silently enqueuing its songs). */
    if (!mpd_search_db_songs(mpd->conn, true)) {
        return fail(mpd);
    }
    /* See the header comment: an album constraint alone already identifies
     * the right songs, and ANDing an artist constraint on top of it drops
     * tracks credited to a different performer within the same album
     * (confirmed against a real 4-track compilation album where only 1
     * track's own Artist tag matched the artist used to browse there). */
    if (album_or_null == NULL && artist_or_null != NULL &&
        !mpd_search_add_tag_constraint(mpd->conn, MPD_OPERATOR_DEFAULT, MPD_TAG_ARTIST, artist_or_null)) {
        mpd_search_cancel(mpd->conn);
        return fail(mpd);
    }
    if (album_or_null != NULL &&
        !mpd_search_add_tag_constraint(mpd->conn, MPD_OPERATOR_DEFAULT, MPD_TAG_ALBUM, album_or_null)) {
        mpd_search_cancel(mpd->conn);
        return fail(mpd);
    }
    if (!mpd_search_commit(mpd->conn)) {
        return fail(mpd);
    }
    return recv_songs(mpd, out, out_count);
}

bool rpod_mpd_list_playlist_songs(rpod_mpd_t *mpd, const char *playlist_name,
                                   rpod_mpd_song_t **out, size_t *out_count)
{
    if (!mpd_send_list_playlist_meta(mpd->conn, playlist_name)) {
        return fail(mpd);
    }
    return recv_songs(mpd, out, out_count);
}

bool rpod_mpd_playlist_add_song(rpod_mpd_t *mpd, const char *playlist_name, const char *uri)
{
    return mpd_run_playlist_add(mpd->conn, playlist_name, uri) ? true : fail(mpd);
}

bool rpod_mpd_playlist_contains(rpod_mpd_t *mpd, const char *playlist_name, const char *uri, bool *out)
{
    *out = false;

    rpod_mpd_song_t *songs = NULL;
    size_t count = 0;
    /* A missing playlist errors out of the listing (and fail() clears the
     * latch); treat that as simply "not a member" rather than a hard error,
     * so "Liked Songs" reads as empty before its first like instead of
     * wedging the caller. */
    if (!rpod_mpd_list_playlist_songs(mpd, playlist_name, &songs, &count)) {
        return true;
    }
    for (size_t i = 0; i < count; i++) {
        if (strcmp(songs[i].uri, uri) == 0) {
            *out = true;
            break;
        }
    }
    rpod_mpd_free_songs(songs);
    return true;
}

bool rpod_mpd_playlist_remove_song(rpod_mpd_t *mpd, const char *playlist_name, const char *uri)
{
    rpod_mpd_song_t *songs = NULL;
    size_t count = 0;
    /* Nothing to remove (song absent, or playlist doesn't exist) is success. */
    if (!rpod_mpd_list_playlist_songs(mpd, playlist_name, &songs, &count)) {
        return true;
    }

    /* Delete from the highest matching position down: "playlistdelete <pos>"
     * renumbers everything after `pos`, so removing back-to-front keeps the
     * positions we haven't visited yet valid. */
    bool ok = true;
    for (size_t i = count; i-- > 0;) {
        if (strcmp(songs[i].uri, uri) == 0) {
            if (!mpd_run_playlist_delete(mpd->conn, playlist_name, (unsigned)i)) {
                ok = false;
                break;
            }
        }
    }
    rpod_mpd_free_songs(songs);
    return ok ? true : fail(mpd);
}

bool rpod_mpd_search_songs(rpod_mpd_t *mpd, const char *query, unsigned max_results,
                            rpod_mpd_song_t **out, size_t *out_count)
{
    *out = NULL;
    *out_count = 0;

    /* MPD rejects a constraint-less search outright ("too few arguments"),
     * so an empty query never reaches the server. */
    if (query == NULL || query[0] == '\0') {
        return false;
    }

    /* exact=false selects the "search" command (case-insensitive substring
     * match) rather than "find" (exact). The "any" constraint matches
     * against every tag, so one query covers title, artist, album, etc. */
    if (!mpd_search_db_songs(mpd->conn, false)) {
        return fail(mpd);
    }
    if (!mpd_search_add_any_tag_constraint(mpd->conn, MPD_OPERATOR_DEFAULT, query)) {
        mpd_search_cancel(mpd->conn);
        return fail(mpd);
    }
    if (max_results > 0 && !mpd_search_add_window(mpd->conn, 0, max_results)) {
        mpd_search_cancel(mpd->conn);
        return fail(mpd);
    }
    if (!mpd_search_commit(mpd->conn)) {
        return fail(mpd);
    }
    return recv_songs(mpd, out, out_count);
}

void rpod_mpd_free_items(rpod_mpd_item_t *items)
{
    free(items);
}

void rpod_mpd_free_songs(rpod_mpd_song_t *songs)
{
    free(songs);
}

bool rpod_mpd_play_uri(rpod_mpd_t *mpd, const char *uri)
{
    if (!mpd_run_clear(mpd->conn)) {
        return fail(mpd);
    }
    if (mpd_run_add(mpd->conn, uri) == false) {
        return fail(mpd);
    }
    if (!mpd_run_play(mpd->conn)) {
        return fail(mpd);
    }
    return true;
}

/* Clears the queue and appends all `count` songs in array order. Leaves the
 * connection playing nothing yet -- the caller starts playback. */
static bool queue_songs(rpod_mpd_t *mpd, const rpod_mpd_song_t *songs, size_t count)
{
    if (!mpd_run_clear(mpd->conn)) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        if (mpd_run_add(mpd->conn, songs[i].uri) == false) {
            return false;
        }
    }
    return true;
}

bool rpod_mpd_play_songs(rpod_mpd_t *mpd, const rpod_mpd_song_t *songs, size_t count)
{
    if (!queue_songs(mpd, songs, count)) {
        return fail(mpd);
    }
    if (!mpd_run_play(mpd->conn)) {
        return fail(mpd);
    }
    return true;
}

bool rpod_mpd_play_songs_from(rpod_mpd_t *mpd, const rpod_mpd_song_t *songs, size_t count,
                              size_t start_index)
{
    if (start_index >= count) {
        return false;
    }
    if (!queue_songs(mpd, songs, count)) {
        return fail(mpd);
    }
    /* "play <pos>" starts at that queue position and plays on through the
     * rest of the queue, so the tracks after the tapped one follow it. */
    if (!mpd_run_play_pos(mpd->conn, (unsigned)start_index)) {
        return fail(mpd);
    }
    return true;
}

bool rpod_mpd_play_songs_shuffled(rpod_mpd_t *mpd, const rpod_mpd_song_t *songs, size_t count)
{
    if (!queue_songs(mpd, songs, count)) {
        return fail(mpd);
    }
    /* Shuffles the queue order server-side, then "play" with no position
     * starts at position 0 -- which after the shuffle is a random track,
     * same as rpod_mpd_play_uri()'s plain case starts at the single queued
     * song. */
    if (!mpd_run_shuffle(mpd->conn)) {
        return fail(mpd);
    }
    if (!mpd_run_play(mpd->conn)) {
        return fail(mpd);
    }
    return true;
}

bool rpod_mpd_toggle_pause(rpod_mpd_t *mpd)
{
    return mpd_run_toggle_pause(mpd->conn) ? true : fail(mpd);
}

bool rpod_mpd_next(rpod_mpd_t *mpd)
{
    return mpd_run_next(mpd->conn) ? true : fail(mpd);
}

bool rpod_mpd_previous(rpod_mpd_t *mpd)
{
    return mpd_run_previous(mpd->conn) ? true : fail(mpd);
}

bool rpod_mpd_cue_first_paused(rpod_mpd_t *mpd)
{
    /* "play 0" is the only way to make song 0 the current one -- MPD has no
     * "cue without playing" -- so start it, then immediately pause. Sent
     * back-to-back over the socket, playback is halted before any real audio
     * has been output. */
    if (!mpd_run_play_pos(mpd->conn, 0)) {
        return fail(mpd);
    }
    if (!mpd_run_pause(mpd->conn, true)) {
        return fail(mpd);
    }
    return true;
}

/* One binary chunk per round trip (server default binarylimit); looping
 * with the read function below assembles the whole picture. */
#define RPOD_COVER_ART_CHUNK 8192u
/* Generous cap on the whole decoded-from-base64-free raw file: bounds a
 * single allocation without ever tripping on real-world cover art (which is
 * routinely a few hundred KB, rarely more than 1-2 MB). */
#define RPOD_COVER_ART_MAX_BYTES (6u * 1024u * 1024u)

typedef int (*cover_art_read_fn)(struct mpd_connection *, const char *, unsigned, void *, size_t);

static bool read_cover_art(rpod_mpd_t *mpd, cover_art_read_fn read_fn, const char *uri,
                            unsigned char **out, size_t *out_size)
{
    unsigned char *buf = NULL;
    size_t cap = 0, len = 0;
    unsigned offset = 0;
    unsigned char chunk[RPOD_COVER_ART_CHUNK];

    for (;;) {
        int n = read_fn(mpd->conn, uri, offset, chunk, sizeof(chunk));
        if (n < 0) {
            free(buf);
            return fail(mpd);
        }
        if (n == 0) {
            break;
        }
        if (len + (size_t)n > RPOD_COVER_ART_MAX_BYTES) {
            free(buf);
            return false;
        }
        if (len + (size_t)n > cap) {
            size_t new_cap = (cap == 0) ? (RPOD_COVER_ART_CHUNK * 4u) : (cap * 2u);
            unsigned char *grown = realloc(buf, new_cap);
            if (grown == NULL) {
                free(buf);
                return false;
            }
            buf = grown;
            cap = new_cap;
        }
        memcpy(buf + len, chunk, (size_t)n);
        len += (size_t)n;
        offset += (unsigned)n;
        if ((size_t)n < sizeof(chunk)) {
            break; /* short read == last chunk */
        }
    }

    if (len == 0) {
        free(buf);
        return false;
    }
    *out = buf;
    *out_size = len;
    return true;
}

bool rpod_mpd_get_cover_art(rpod_mpd_t *mpd, const char *uri, unsigned char **out, size_t *out_size)
{
    *out = NULL;
    *out_size = 0;
    if (read_cover_art(mpd, mpd_run_readpicture, uri, out, out_size)) {
        return true;
    }
    return read_cover_art(mpd, mpd_run_albumart, uri, out, out_size);
}

void rpod_mpd_free_cover_art(unsigned char *data)
{
    free(data);
}

bool rpod_mpd_list_outputs(rpod_mpd_t *mpd, rpod_mpd_output_t **out, size_t *out_count)
{
    *out = NULL;
    *out_count = 0;

    if (!mpd_send_outputs(mpd->conn)) {
        return fail(mpd);
    }

    rpod_mpd_output_t *outputs = NULL;
    size_t count = 0, cap = 0;
    struct mpd_output *o;
    while ((o = mpd_recv_output(mpd->conn)) != NULL) {
        rpod_mpd_output_t *grown = array_grow(outputs, count, &cap, sizeof(*outputs));
        if (grown == NULL) {
            mpd_output_free(o);
            free(outputs);
            mpd_response_finish(mpd->conn);
            return fail(mpd);
        }
        outputs = grown;
        outputs[count].id = mpd_output_get_id(o);
        outputs[count].enabled = mpd_output_get_enabled(o);
        snprintf(outputs[count].name, sizeof(outputs[count].name), "%s", mpd_output_get_name(o));
        count++;
        mpd_output_free(o);
    }

    if (!mpd_response_finish(mpd->conn)) {
        free(outputs);
        return fail(mpd);
    }

    *out = outputs;
    *out_count = count;
    return true;
}

bool rpod_mpd_enable_output(rpod_mpd_t *mpd, unsigned id)
{
    return mpd_run_enable_output(mpd->conn, id) ? true : fail(mpd);
}

bool rpod_mpd_disable_output(rpod_mpd_t *mpd, unsigned id)
{
    return mpd_run_disable_output(mpd->conn, id) ? true : fail(mpd);
}

void rpod_mpd_free_outputs(rpod_mpd_output_t *outputs)
{
    free(outputs);
}
