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
    copy_tag(dst->artist, sizeof(dst->artist), song, MPD_TAG_ARTIST);
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
    mpd_status_free(status);

    struct mpd_song *song = mpd_run_current_song(mpd->conn);
    if (song != NULL) {
        copy_tag(out->title, sizeof(out->title), song, MPD_TAG_TITLE);
        copy_tag(out->artist, sizeof(out->artist), song, MPD_TAG_ARTIST);
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
    if (artist_or_null != NULL &&
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
