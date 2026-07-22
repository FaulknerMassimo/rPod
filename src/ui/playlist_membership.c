#include "playlist_membership.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* --- URI hash set (open addressing, FNV-1a) ----------------------------- */

typedef struct {
    char **slots;   /* strdup'd URIs, NULL = empty */
    size_t cap;     /* power of two */
    size_t count;
} uri_set_t;

static uint64_t fnv1a(const char *s)
{
    uint64_t h = 1469598103934665603ull;
    for (; *s != '\0'; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211ull;
    }
    return h;
}

static void uri_set_init(uri_set_t *s)
{
    s->cap = 64;
    s->count = 0;
    s->slots = calloc(s->cap, sizeof(*s->slots));
}

static void uri_set_free(uri_set_t *s)
{
    if (s->slots != NULL) {
        for (size_t i = 0; i < s->cap; i++) {
            free(s->slots[i]);
        }
        free(s->slots);
    }
    s->slots = NULL;
    s->cap = 0;
    s->count = 0;
}

static void uri_set_clear(uri_set_t *s)
{
    for (size_t i = 0; i < s->cap; i++) {
        free(s->slots[i]);
        s->slots[i] = NULL;
    }
    s->count = 0;
}

static bool uri_set_contains(const uri_set_t *s, const char *uri)
{
    if (s->slots == NULL || s->count == 0) {
        return false;
    }
    size_t mask = s->cap - 1;
    size_t i = (size_t)fnv1a(uri) & mask;
    for (size_t probe = 0; probe < s->cap; probe++) {
        char *slot = s->slots[i];
        if (slot == NULL) {
            return false;
        }
        if (strcmp(slot, uri) == 0) {
            return true;
        }
        i = (i + 1) & mask;
    }
    return false;
}

static void uri_set_insert_raw(uri_set_t *s, char *owned)
{
    size_t mask = s->cap - 1;
    size_t i = (size_t)fnv1a(owned) & mask;
    while (s->slots[i] != NULL) {
        i = (i + 1) & mask;
    }
    s->slots[i] = owned;
    s->count++;
}

static void uri_set_grow(uri_set_t *s)
{
    size_t old_cap = s->cap;
    char **old = s->slots;
    s->cap = old_cap * 2;
    s->count = 0;
    s->slots = calloc(s->cap, sizeof(*s->slots));
    for (size_t i = 0; i < old_cap; i++) {
        if (old[i] != NULL) {
            uri_set_insert_raw(s, old[i]);
        }
    }
    free(old);
}

static void uri_set_add(uri_set_t *s, const char *uri)
{
    if (s->slots == NULL || uri_set_contains(s, uri)) {
        return;
    }
    if ((s->count + 1) * 10 >= s->cap * 7) { /* keep load factor < 0.7 */
        uri_set_grow(s);
    }
    uri_set_insert_raw(s, strdup(uri));
}

/* --- Index -------------------------------------------------------------- */

struct rpod_playlist_index {
    uri_set_t liked;
    uri_set_t other;
};

static void index_scan(rpod_playlist_index_t *idx, rpod_mpd_t *mpd)
{
    rpod_mpd_item_t *playlists = NULL;
    size_t n = 0;
    if (!rpod_mpd_list_playlists(mpd, &playlists, &n)) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        bool is_liked = strcmp(playlists[i].name, RPOD_LIKED_PLAYLIST_NAME) == 0;
        uri_set_t *set = is_liked ? &idx->liked : &idx->other;

        rpod_mpd_song_t *songs = NULL;
        size_t sc = 0;
        if (!rpod_mpd_list_playlist_songs(mpd, playlists[i].name, &songs, &sc)) {
            continue;
        }
        for (size_t j = 0; j < sc; j++) {
            uri_set_add(set, songs[j].uri);
        }
        rpod_mpd_free_songs(songs);
    }
    rpod_mpd_free_items(playlists);
}

rpod_playlist_index_t *rpod_playlist_index_build(rpod_mpd_t *mpd)
{
    rpod_playlist_index_t *idx = calloc(1, sizeof(*idx));
    if (idx == NULL) {
        return NULL;
    }
    uri_set_init(&idx->liked);
    uri_set_init(&idx->other);
    if (idx->liked.slots == NULL || idx->other.slots == NULL) {
        rpod_playlist_index_free(idx);
        return NULL;
    }
    index_scan(idx, mpd);
    return idx;
}

void rpod_playlist_index_rebuild(rpod_playlist_index_t *idx, rpod_mpd_t *mpd)
{
    if (idx == NULL) {
        return;
    }
    uri_set_clear(&idx->liked);
    uri_set_clear(&idx->other);
    index_scan(idx, mpd);
}

void rpod_playlist_index_free(rpod_playlist_index_t *idx)
{
    if (idx == NULL) {
        return;
    }
    uri_set_free(&idx->liked);
    uri_set_free(&idx->other);
    free(idx);
}

bool rpod_playlist_index_is_liked(const rpod_playlist_index_t *idx, const char *uri)
{
    return idx != NULL && uri_set_contains(&idx->liked, uri);
}

bool rpod_playlist_index_in_other(const rpod_playlist_index_t *idx, const char *uri)
{
    return idx != NULL && uri_set_contains(&idx->other, uri);
}

bool rpod_playlist_liked_toggle(rpod_mpd_t *mpd, const char *uri, bool *now_liked)
{
    bool in = false;
    if (!rpod_mpd_playlist_contains(mpd, RPOD_LIKED_PLAYLIST_NAME, uri, &in)) {
        *now_liked = false;
        return false;
    }
    if (in) {
        if (!rpod_mpd_playlist_remove_song(mpd, RPOD_LIKED_PLAYLIST_NAME, uri)) {
            return false;
        }
        *now_liked = false;
    } else {
        if (!rpod_mpd_playlist_add_song(mpd, RPOD_LIKED_PLAYLIST_NAME, uri)) {
            return false;
        }
        *now_liked = true;
    }
    return true;
}
