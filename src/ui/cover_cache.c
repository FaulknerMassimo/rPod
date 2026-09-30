#include "cover_cache.h"

#include "audio/mpd_client.h"
#include "cover_art.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* How often the LVGL thread checks for newly finished covers. */
#define POLL_MS 30

#define BUCKETS 512u

typedef enum {
    ST_QUEUED,  /* waiting for the worker */
    ST_WORKING, /* being fetched + decoded right now */
    ST_READY,   /* dsc is valid */
    ST_NONE,    /* resolved to no art -- never retried */
} entry_state_t;

typedef struct entry {
    struct entry *next;  /* hash chain */
    struct entry *qnext; /* queue link, while ST_QUEUED */
    char *key;
    char *uri;           /* representative track to fetch from; immutable */
    int size;
    entry_state_t state;
    /* Queue priority: the most recently wanted entry goes first, so what's
     * on screen now beats whatever a fast scroll already left behind; ties
     * (one screen build asking for many rows in the same millisecond) go
     * first-come-first-served, so a list fills in top to bottom. */
    uint32_t want_ms;
    uint64_t seq;
    lv_image_dsc_t dsc; /* valid once ST_READY; never freed */
} entry_t;

typedef struct {
    lv_obj_t *owner;
    void (*cb)(void *user);
    void *user;
} watcher_t;

/* Everything above the watchers is shared with the worker and guarded by
 * `lock`; the watchers and `notified` are LVGL-thread only. */
static struct {
    bool started;
    char *mpd_socket;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    entry_t *buckets[BUCKETS];
    entry_t *queue;
    uint64_t next_seq;
    unsigned completed;

    unsigned notified;
    watcher_t *watchers;
    size_t watcher_count, watcher_cap;
} g = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
};

static uint32_t hash_str(const char *s)
{
    uint32_t h = 2166136261u; /* FNV-1a */
    for (; *s != '\0'; s++) {
        h = (h ^ (uint8_t)*s) * 16777619u;
    }
    return h;
}

/* Worker: picks the highest-priority queued entry and unlinks it. Caller
 * holds the lock and has checked the queue is non-empty. */
static entry_t *queue_pop_best(void)
{
    entry_t **best = &g.queue;
    for (entry_t **pp = &g.queue; *pp != NULL; pp = &(*pp)->qnext) {
        entry_t *e = *pp, *b = *best;
        int32_t newer = (int32_t)(e->want_ms - b->want_ms);
        if (newer > 0 || (newer == 0 && e->seq < b->seq)) {
            best = pp;
        }
    }
    entry_t *e = *best;
    *best = e->qnext;
    e->qnext = NULL;
    return e;
}

/* Fetches + decodes on the worker's own connection, (re)connecting as
 * needed. A request that fails because the connection died (MPD restarted,
 * or dropped it after its idle timeout) reconnects and retries once; a
 * clean "no art" answer from a live connection doesn't. */
static bool fetch_and_decode(rpod_mpd_t **mpd, const char *uri, int size, rpod_cover_art_t *art)
{
    unsigned char *raw = NULL;
    size_t raw_size = 0;

    for (int attempt = 0; attempt < 2 && raw == NULL; attempt++) {
        if (*mpd == NULL || !rpod_mpd_is_connected(*mpd)) {
            rpod_mpd_disconnect(*mpd);
            *mpd = rpod_mpd_connect(g.mpd_socket);
            if (*mpd == NULL || !rpod_mpd_is_connected(*mpd)) {
                rpod_mpd_disconnect(*mpd);
                *mpd = NULL;
                return false;
            }
        }
        if (!rpod_mpd_get_cover_art(*mpd, uri, &raw, &raw_size) && rpod_mpd_is_connected(*mpd)) {
            return false;
        }
    }
    if (raw == NULL) {
        return false;
    }

    bool ok = rpod_cover_art_decode(raw, raw_size, size, size, art);
    rpod_mpd_free_cover_art(raw);
    return ok;
}

static void *worker_main(void *arg)
{
    (void)arg;
    rpod_mpd_t *mpd = NULL;

    for (;;) {
        pthread_mutex_lock(&g.lock);
        if (g.queue == NULL && mpd != NULL) {
            /* Out of work: drop the connection rather than let it idle into
             * MPD's connection_timeout; the next burst reconnects. */
            pthread_mutex_unlock(&g.lock);
            rpod_mpd_disconnect(mpd);
            mpd = NULL;
            pthread_mutex_lock(&g.lock);
        }
        while (g.queue == NULL) {
            pthread_cond_wait(&g.cond, &g.lock);
        }
        entry_t *e = queue_pop_best();
        e->state = ST_WORKING;
        pthread_mutex_unlock(&g.lock);

        rpod_cover_art_t art = { 0 };
        bool ok = fetch_and_decode(&mpd, e->uri, e->size, &art);

        pthread_mutex_lock(&g.lock);
        if (ok) {
            e->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            e->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
            e->dsc.header.flags = 0;
            e->dsc.header.w = (uint16_t)art.w;
            e->dsc.header.h = (uint16_t)art.h;
            e->dsc.header.stride = (uint16_t)(art.w * 2);
            e->dsc.data_size = (uint32_t)(art.w * art.h * 2);
            e->dsc.data = (const uint8_t *)art.pixels;
            e->state = ST_READY;
        } else {
            e->state = ST_NONE;
        }
        g.completed++;
        pthread_mutex_unlock(&g.lock);
    }
    return NULL;
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    pthread_mutex_lock(&g.lock);
    unsigned completed = g.completed;
    pthread_mutex_unlock(&g.lock);
    if (completed == g.notified) {
        return;
    }
    g.notified = completed;
    /* Re-read the count each pass: a callback is free to add a watcher. */
    for (size_t i = 0; i < g.watcher_count; i++) {
        g.watchers[i].cb(g.watchers[i].user);
    }
}

void rpod_cover_cache_init(const char *mpd_socket)
{
    if (g.started) {
        return;
    }
    g.mpd_socket = strdup(mpd_socket);
    pthread_t thread;
    if (g.mpd_socket == NULL || pthread_create(&thread, NULL, worker_main, NULL) != 0) {
        fprintf(stderr, "rpod: cover art worker failed to start; covers disabled\n");
        return;
    }
    pthread_detach(thread);
    lv_timer_create(poll_cb, POLL_MS, NULL);
    g.started = true;
}

const lv_image_dsc_t *rpod_cover_cache_get(const char *artist, const char *album,
                                           const char *uri, int size, bool *pending)
{
    if (pending != NULL) {
        *pending = false;
    }
    if (!g.started || uri == NULL || uri[0] == '\0') {
        return NULL;
    }

    char key[1100];
    if (album != NULL && album[0] != '\0') {
        snprintf(key, sizeof(key), "a%d\x1f%s\x1f%s", size, artist != NULL ? artist : "", album);
    } else {
        snprintf(key, sizeof(key), "u%d\x1f%s", size, uri);
    }
    uint32_t b = hash_str(key) % BUCKETS;

    pthread_mutex_lock(&g.lock);
    entry_t *e = g.buckets[b];
    while (e != NULL && strcmp(e->key, key) != 0) {
        e = e->next;
    }
    const lv_image_dsc_t *result = NULL;
    if (e == NULL) {
        e = calloc(1, sizeof(*e));
        if (e != NULL) {
            e->key = strdup(key);
            e->uri = strdup(uri);
        }
        if (e == NULL || e->key == NULL || e->uri == NULL) {
            if (e != NULL) {
                free(e->key);
                free(e->uri);
                free(e);
            }
            pthread_mutex_unlock(&g.lock);
            return NULL;
        }
        e->size = size;
        e->state = ST_QUEUED;
        e->want_ms = lv_tick_get();
        e->seq = g.next_seq++;
        e->next = g.buckets[b];
        g.buckets[b] = e;
        e->qnext = g.queue;
        g.queue = e;
        pthread_cond_signal(&g.cond);
    } else if (e->state == ST_QUEUED) {
        e->want_ms = lv_tick_get();
    } else if (e->state == ST_READY) {
        result = &e->dsc;
    }
    if (pending != NULL) {
        *pending = (e->state == ST_QUEUED || e->state == ST_WORKING);
    }
    pthread_mutex_unlock(&g.lock);
    return result;
}

static void watch_owner_deleted_cb(lv_event_t *e)
{
    lv_obj_t *owner = lv_event_get_current_target(e);
    size_t out = 0;
    for (size_t i = 0; i < g.watcher_count; i++) {
        if (g.watchers[i].owner != owner) {
            g.watchers[out++] = g.watchers[i];
        }
    }
    g.watcher_count = out;
}

void rpod_cover_cache_watch(lv_obj_t *owner, void (*cb)(void *user), void *user)
{
    if (g.watcher_count == g.watcher_cap) {
        size_t cap = g.watcher_cap ? g.watcher_cap * 2 : 8;
        watcher_t *grown = realloc(g.watchers, cap * sizeof(*grown));
        if (grown == NULL) {
            return;
        }
        g.watchers = grown;
        g.watcher_cap = cap;
    }
    g.watchers[g.watcher_count++] = (watcher_t){ .owner = owner, .cb = cb, .user = user };
    lv_obj_add_event_cb(owner, watch_owner_deleted_cb, LV_EVENT_DELETE, NULL);
}
