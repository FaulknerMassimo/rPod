#include "cover_cache.h"

#include "audio/embedded_art.h"
#include "audio/mpd_client.h"
#include "cover_art.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* How often the LVGL thread checks for newly finished covers. */
#define POLL_MS 30

#define BUCKETS 512u

/* Room for a cache key: a size, then the album key ("a" + artist + album,
 * or "u" + uri), within rpod_mpd_song_t's field sizes. */
#define KEY_MAX 1100

/* The disk tier's one tile per album: the largest any screen shows (Now
 * Playing's art), so every smaller size scales down from it. A bigger
 * request skips the disk tier and decodes straight from the source. */
#define MASTER_SIZE 136

/* Raw cover bytes beyond this are refused rather than allocated (the same
 * cap rpod_mpd_get_cover_art() applies). */
#define MAX_COVER_BYTES (6u * 1024u * 1024u)

/* The prewarming worker runs niced, below the UI thread and MPD's decoder,
 * so a whole-library pass never costs a dropped frame or a skip. */
#define PREWARM_NICE 10

/* How often an idle prewarming worker asks MPD whether the library changed
 * (one "stats" round trip; a rescan only when it did). */
#define RESCAN_MS 30000

/* On-disk master tile: a header, the album key (to catch the odd hash
 * collision), then w * h RGB565 pixels. The version is part of the file
 * name: bump it whenever the format or the decoder's output changes, and
 * old files are simply never looked at again. */
#define DISK_MAGIC   "RPCV"
#define DISK_VERSION 1

typedef struct {
    char magic[4];
    uint16_t w, h;
    uint32_t key_len;
} disk_header_t;

typedef enum {
    ST_QUEUED,  /* waiting for the loader or a decoder */
    ST_WORKING, /* being loaded or decoded right now */
    ST_READY,   /* dsc is valid */
    ST_NONE,    /* resolved to no art -- never retried */
} entry_state_t;

typedef struct entry {
    struct entry *next;  /* hash chain */
    struct entry *qnext; /* queue link, while ST_QUEUED */
    char *key;           /* "<size>\x1f<album key>" */
    const char *album_key; /* points into key: the disk tier's key */
    char *uri;           /* representative track to fetch from; immutable */
    int size;
    entry_state_t state;
    /* The loader has looked for it on disk (or there's no disk tier to look
     * in): only these are a decoder's business. */
    bool disk_checked;
    /* Queue priority: the most recently wanted entry goes first, so what's
     * on screen now beats whatever a fast scroll already left behind; ties
     * (one screen build asking for many rows in the same millisecond) go
     * first-come-first-served, so a list fills in top to bottom. */
    uint32_t want_ms;
    uint64_t seq;
    lv_image_dsc_t dsc; /* valid once ST_READY; never freed */
} entry_t;

/* A set of strings: album keys, for "known to have no art" and the
 * library scan's dedup. */
typedef struct strnode {
    struct strnode *next;
    char s[];
} strnode_t;

typedef struct {
    strnode_t *buckets[BUCKETS];
} strset_t;

/* An album the library scan found with no master tile on disk yet. */
typedef struct {
    char *album_key;
    char *uri;
} prewarm_t;

typedef struct {
    lv_obj_t *owner;
    void (*cb)(void *user);
    void *user;
} watcher_t;

/* Everything above the watchers is shared with the workers and guarded by
 * `lock`; the watchers and `notified` are LVGL-thread only. */
static struct {
    bool started;
    char *mpd_socket;
    char *cache_dir;      /* NULL: memory only */
    char *music_dir;      /* MPD's, for reading FLACs directly; NULL: ask MPD */
    bool music_dir_known; /* asked MPD already (music_dir may still be NULL) */
    pthread_mutex_t lock;
    pthread_cond_t cond;  /* broadcast on any change a waiter might care about */
    entry_t *buckets[BUCKETS];
    entry_t *queue;
    uint64_t next_seq;
    unsigned completed;
    unsigned disk_pending; /* entries the loader hasn't looked up yet */

    /* Whole-library prewarm (the prewarming decoder only). */
    prewarm_t *prewarm;
    size_t prewarm_count, prewarm_next;
    bool scanned; /* scanned_db_update is valid */
    unsigned long scanned_db_update;
    uint64_t next_scan_ms;
    strset_t noart; /* album keys that turned out to have no art */

    unsigned notified;
    watcher_t *watchers;
    size_t watcher_count, watcher_cap;
} g = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

static uint32_t hash_str(const char *s)
{
    uint32_t h = 2166136261u; /* FNV-1a */
    for (; *s != '\0'; s++) {
        h = (h ^ (uint8_t)*s) * 16777619u;
    }
    return h;
}

static uint64_t hash_str64(const char *s)
{
    uint64_t h = 14695981039346656037u; /* FNV-1a, 64-bit */
    for (; *s != '\0'; s++) {
        h = (h ^ (uint8_t)*s) * 1099511628211u;
    }
    return h;
}

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* pthread_cond_timedwait() on g.cond (CLOCK_MONOTONIC -- see init). Caller
 * holds the lock. */
static void wait_until(uint64_t deadline_ms)
{
    struct timespec ts = {
        .tv_sec = (time_t)(deadline_ms / 1000u),
        .tv_nsec = (long)(deadline_ms % 1000u) * 1000000L,
    };
    pthread_cond_timedwait(&g.cond, &g.lock, &ts);
}

static bool strset_has(const strset_t *set, const char *s)
{
    for (const strnode_t *n = set->buckets[hash_str(s) % BUCKETS]; n != NULL; n = n->next) {
        if (strcmp(n->s, s) == 0) {
            return true;
        }
    }
    return false;
}

/* False if `s` was already there (or on allocation failure). */
static bool strset_add(strset_t *set, const char *s)
{
    if (strset_has(set, s)) {
        return false;
    }
    size_t len = strlen(s);
    strnode_t *n = malloc(sizeof(*n) + len + 1);
    if (n == NULL) {
        return false;
    }
    memcpy(n->s, s, len + 1);
    uint32_t b = hash_str(s) % BUCKETS;
    n->next = set->buckets[b];
    set->buckets[b] = n;
    return true;
}

static void strset_free(strset_t *set)
{
    for (size_t b = 0; b < BUCKETS; b++) {
        while (set->buckets[b] != NULL) {
            strnode_t *n = set->buckets[b];
            set->buckets[b] = n->next;
            free(n);
        }
    }
}

/* What a cover is cached under, size aside: the album, or for a track with
 * no album tag, the track itself. */
static void make_album_key(char *out, size_t out_size, const char *album_artist, const char *album,
                           const char *uri)
{
    if (album != NULL && album[0] != '\0') {
        snprintf(out, out_size, "a\x1f%s\x1f%s", album_artist != NULL ? album_artist : "", album);
    } else {
        snprintf(out, out_size, "u\x1f%s", uri);
    }
}

/* --- Disk tier ------------------------------------------------------ */

static void disk_path(char *out, size_t out_size, const char *album_key)
{
    snprintf(out, out_size, "%s/%016" PRIx64 ".v%d", g.cache_dir, hash_str64(album_key), DISK_VERSION);
}

static bool disk_has(const char *album_key)
{
    char path[PATH_MAX];
    disk_path(path, sizeof(path), album_key);
    return access(path, F_OK) == 0;
}

static bool disk_load(const char *album_key, rpod_cover_art_t *out)
{
    char path[PATH_MAX];
    disk_path(path, sizeof(path), album_key);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }

    disk_header_t h;
    char key[KEY_MAX];
    size_t key_len = strlen(album_key);
    bool ok = fread(&h, sizeof(h), 1, f) == 1 && memcmp(h.magic, DISK_MAGIC, 4) == 0 &&
              h.w > 0 && h.h > 0 && h.w <= 1024 && h.h <= 1024 && h.key_len == key_len &&
              key_len < sizeof(key) && fread(key, 1, key_len, f) == key_len &&
              memcmp(key, album_key, key_len) == 0;

    size_t n = ok ? (size_t)h.w * h.h : 0;
    uint16_t *px = ok ? malloc(n * sizeof(*px)) : NULL;
    ok = px != NULL && fread(px, sizeof(*px), n, f) == n;
    fclose(f);
    if (!ok) {
        free(px);
        return false;
    }
    out->pixels = px;
    out->w = h.w;
    out->h = h.h;
    return true;
}

/* Written to a per-thread temp file and renamed into place, so a reader
 * never sees half a file -- and a half-written one (power cut) fails
 * disk_load()'s size check and is just decoded again. */
static void disk_store(const char *album_key, const rpod_cover_art_t *art)
{
    char path[PATH_MAX], tmp[PATH_MAX + 32];
    disk_path(path, sizeof(path), album_key);
    snprintf(tmp, sizeof(tmp), "%s.%ld.tmp", path, (long)syscall(SYS_gettid));
    FILE *f = fopen(tmp, "wb");
    if (f == NULL) {
        return;
    }
    disk_header_t h = { .w = (uint16_t)art->w, .h = (uint16_t)art->h };
    memcpy(h.magic, DISK_MAGIC, 4);
    size_t key_len = strlen(album_key);
    h.key_len = (uint32_t)key_len;
    size_t n = (size_t)art->w * (size_t)art->h;
    bool ok = fwrite(&h, sizeof(h), 1, f) == 1 && fwrite(album_key, 1, key_len, f) == key_len &&
              fwrite(art->pixels, sizeof(*art->pixels), n, f) == n;
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
    }
}

/* mkdir -p. */
static bool make_dirs(const char *dir)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s", dir);
    for (char *p = path + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(path, 0755) != 0 && errno != EEXIST) {
                return false;
            }
            *p = '/';
        }
    }
    return mkdir(path, 0755) == 0 || errno == EEXIST;
}

/* --- Fetch + decode (decoder threads) ------------------------------- */

typedef enum {
    FETCH_OK,
    FETCH_NO_ART, /* a clean answer: there is none, or it doesn't decode */
    FETCH_NO_MPD, /* couldn't ask -- try again another time */
} fetch_result_t;

/* (Re)connects as needed. The first connection also learns where MPD's
 * music lives, for read_raw(). */
static bool ensure_connected(rpod_mpd_t **mpd)
{
    if (*mpd != NULL && rpod_mpd_is_connected(*mpd)) {
        return true;
    }
    rpod_mpd_disconnect(*mpd);
    *mpd = rpod_mpd_connect(g.mpd_socket);
    if (*mpd == NULL || !rpod_mpd_is_connected(*mpd)) {
        rpod_mpd_disconnect(*mpd);
        *mpd = NULL;
        return false;
    }

    pthread_mutex_lock(&g.lock);
    bool known = g.music_dir_known;
    pthread_mutex_unlock(&g.lock);
    if (!known) {
        char dir[PATH_MAX];
        char *music_dir = rpod_mpd_get_music_directory(*mpd, dir, sizeof(dir)) ? strdup(dir) : NULL;
        pthread_mutex_lock(&g.lock);
        if (!g.music_dir_known) {
            g.music_dir = music_dir;
            g.music_dir_known = true;
            music_dir = NULL;
        }
        pthread_mutex_unlock(&g.lock);
        free(music_dir);
    }
    return true;
}

/* The raw cover bytes for `uri`: read straight out of the file when it's a
 * FLAC in MPD's (local) music directory -- see audio/embedded_art.h for why
 * that matters -- otherwise asked of MPD. A request that fails because the
 * connection died (MPD restarted, or dropped it after its idle timeout)
 * reconnects and retries once; a clean "no art" answer from a live
 * connection doesn't. */
static fetch_result_t read_raw(rpod_mpd_t **mpd, const char *uri, unsigned char **raw, size_t *raw_size)
{
    if (!ensure_connected(mpd)) {
        return FETCH_NO_MPD;
    }
    pthread_mutex_lock(&g.lock);
    const char *music_dir = g.music_dir; /* set once, never freed */
    pthread_mutex_unlock(&g.lock);
    if (music_dir != NULL && strstr(uri, "://") == NULL) {
        char path[PATH_MAX];
        if ((size_t)snprintf(path, sizeof(path), "%s/%s", music_dir, uri) < sizeof(path) &&
            rpod_embedded_art_read(path, MAX_COVER_BYTES, raw, raw_size)) {
            return FETCH_OK;
        }
    }

    for (int attempt = 0; attempt < 2; attempt++) {
        if (!ensure_connected(mpd)) {
            return FETCH_NO_MPD;
        }
        if (rpod_mpd_get_cover_art(*mpd, uri, raw, raw_size)) {
            return FETCH_OK;
        }
        if (rpod_mpd_is_connected(*mpd)) {
            return FETCH_NO_ART;
        }
    }
    return FETCH_NO_MPD;
}

static fetch_result_t fetch_and_decode(rpod_mpd_t **mpd, const char *uri, int size, rpod_cover_art_t *art)
{
    unsigned char *raw = NULL;
    size_t raw_size = 0;
    fetch_result_t r = read_raw(mpd, uri, &raw, &raw_size);
    if (r != FETCH_OK) {
        return r;
    }
    bool ok = rpod_cover_art_decode(raw, raw_size, size, size, art);
    free(raw); /* rpod_mpd_free_cover_art() is plain free() too */
    return ok ? FETCH_OK : FETCH_NO_ART;
}

/* --- Queue --------------------------------------------------------- */

/* Picks the highest-priority queued entry with the given disk_checked --
 * not yet looked up on disk (the loader's) or already looked up (the
 * decoders') -- and unlinks it. NULL if there's none. Caller holds the
 * lock. */
static entry_t *queue_pop_best(bool disk_checked)
{
    entry_t **best = NULL;
    for (entry_t **pp = &g.queue; *pp != NULL; pp = &(*pp)->qnext) {
        entry_t *e = *pp;
        if (e->disk_checked != disk_checked) {
            continue;
        }
        if (best == NULL) {
            best = pp;
            continue;
        }
        int32_t newer = (int32_t)(e->want_ms - (*best)->want_ms);
        if (newer > 0 || (newer == 0 && e->seq < (*best)->seq)) {
            best = pp;
        }
    }
    if (best == NULL) {
        return NULL;
    }
    entry_t *e = *best;
    *best = e->qnext;
    e->qnext = NULL;
    e->state = ST_WORKING;
    return e;
}

/* Publishes a finished entry: `art` (whose pixels it takes over), or no
 * art at all if NULL. Caller holds the lock. */
static void entry_finish(entry_t *e, rpod_cover_art_t *art)
{
    if (art != NULL) {
        e->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        e->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        e->dsc.header.flags = 0;
        e->dsc.header.w = (uint16_t)art->w;
        e->dsc.header.h = (uint16_t)art->h;
        e->dsc.header.stride = (uint16_t)(art->w * 2);
        e->dsc.data_size = (uint32_t)(art->w * art->h * 2);
        e->dsc.data = (const uint8_t *)art->pixels;
        e->state = ST_READY;
    } else {
        e->state = ST_NONE;
    }
    g.completed++;
}

/* `master`, scaled to `size` -- or `master` itself, at that size already.
 * Consumes `master` either way. */
static bool from_master(rpod_cover_art_t *master, int size, rpod_cover_art_t *out)
{
    if (master->w == size && master->h == size) {
        *out = *master;
        return true;
    }
    bool ok = rpod_cover_art_scale(master, size, size, out);
    rpod_cover_art_free(master);
    return ok;
}

/* --- Workers -------------------------------------------------------- */

/* The disk tier, on a thread of its own so a lookup -- a few milliseconds
 * at most -- never queues behind a decode. A miss goes back on the queue
 * for the decoders. */
static void *loader_main(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g.lock);
    for (;;) {
        entry_t *e = queue_pop_best(false);
        if (e == NULL) {
            pthread_cond_wait(&g.cond, &g.lock);
            continue;
        }
        pthread_mutex_unlock(&g.lock);

        rpod_cover_art_t master = { 0 }, art = { 0 };
        bool hit = disk_load(e->album_key, &master) && from_master(&master, e->size, &art);

        pthread_mutex_lock(&g.lock);
        e->disk_checked = true;
        g.disk_pending--;
        if (hit) {
            entry_finish(e, &art);
        } else {
            e->state = ST_QUEUED;
            e->qnext = g.queue;
            g.queue = e;
        }
        pthread_cond_broadcast(&g.cond);
    }
    return NULL;
}

static void decode_entry(rpod_mpd_t **mpd, entry_t *e)
{
    rpod_cover_art_t art = { 0 };
    fetch_result_t r;
    if (g.cache_dir != NULL && e->size <= MASTER_SIZE) {
        rpod_cover_art_t master = { 0 };
        r = fetch_and_decode(mpd, e->uri, MASTER_SIZE, &master);
        if (r == FETCH_OK) {
            disk_store(e->album_key, &master);
            if (!from_master(&master, e->size, &art)) {
                r = FETCH_NO_ART;
            }
        }
    } else {
        r = fetch_and_decode(mpd, e->uri, e->size, &art);
    }

    pthread_mutex_lock(&g.lock);
    if (r == FETCH_NO_ART) {
        strset_add(&g.noart, e->album_key);
    }
    entry_finish(e, r == FETCH_OK ? &art : NULL);
    pthread_mutex_unlock(&g.lock);
}

/* Lists the library and queues every album with no master tile on disk
 * (and not already known to have no art) for prewarming -- but only when
 * MPD's database has changed since the last scan. */
static void scan_library(rpod_mpd_t **mpd)
{
    /* A connection left over from a long prewarm (direct reads don't touch
     * it) may have idled into MPD's connection_timeout: reconnect once. */
    unsigned long db_update;
    if (!ensure_connected(mpd)) {
        return;
    }
    if (!rpod_mpd_get_db_update_time(*mpd, &db_update) &&
        (rpod_mpd_is_connected(*mpd) || !ensure_connected(mpd) ||
         !rpod_mpd_get_db_update_time(*mpd, &db_update))) {
        return;
    }
    pthread_mutex_lock(&g.lock);
    bool unchanged = g.scanned && g.scanned_db_update == db_update;
    pthread_mutex_unlock(&g.lock);
    if (unchanged) {
        return;
    }

    rpod_mpd_song_t *songs = NULL;
    size_t count = 0;
    strset_t *seen = calloc(1, sizeof(*seen));
    if (seen == NULL || !rpod_mpd_list_songs(*mpd, NULL, NULL, &songs, &count)) {
        free(seen);
        return;
    }

    prewarm_t *items = NULL;
    size_t n = 0, cap = 0;
    for (size_t i = 0; i < count; i++) {
        char key[KEY_MAX];
        make_album_key(key, sizeof(key), songs[i].album_artist, songs[i].album, songs[i].uri);
        if (!strset_add(seen, key)) {
            continue;
        }
        pthread_mutex_lock(&g.lock);
        bool noart = strset_has(&g.noart, key);
        pthread_mutex_unlock(&g.lock);
        if (noart || disk_has(key)) {
            continue;
        }
        if (n == cap) {
            size_t new_cap = cap ? cap * 2 : 64;
            prewarm_t *grown = realloc(items, new_cap * sizeof(*grown));
            if (grown == NULL) {
                break;
            }
            items = grown;
            cap = new_cap;
        }
        items[n].album_key = strdup(key);
        items[n].uri = strdup(songs[i].uri);
        if (items[n].album_key == NULL || items[n].uri == NULL) {
            free(items[n].album_key);
            free(items[n].uri);
            break;
        }
        n++;
    }
    rpod_mpd_free_songs(songs);
    strset_free(seen);
    free(seen);

    pthread_mutex_lock(&g.lock);
    for (size_t i = g.prewarm_next; i < g.prewarm_count; i++) {
        free(g.prewarm[i].album_key);
        free(g.prewarm[i].uri);
    }
    free(g.prewarm);
    g.prewarm = items;
    g.prewarm_count = n;
    g.prewarm_next = 0;
    g.scanned = true;
    g.scanned_db_update = db_update;
    pthread_mutex_unlock(&g.lock);
}

static void prewarm_one(rpod_mpd_t **mpd, prewarm_t *item)
{
    if (!disk_has(item->album_key)) { /* a foreground decode may have got there first */
        rpod_cover_art_t master = { 0 };
        fetch_result_t r = fetch_and_decode(mpd, item->uri, MASTER_SIZE, &master);
        if (r == FETCH_OK) {
            disk_store(item->album_key, &master);
            rpod_cover_art_free(&master);
        } else if (r == FETCH_NO_ART) {
            pthread_mutex_lock(&g.lock);
            strset_add(&g.noart, item->album_key);
            pthread_mutex_unlock(&g.lock);
        }
    }
    free(item->album_key);
    free(item->uri);
}

/* Fetch + decode. Both decoders serve the queue (most urgent first); the
 * one started with `prewarm` set also, when there's nothing on it, scans
 * the library and prewarms the disk tier one album at a time -- checking
 * the queue between albums, so what's on screen still comes first. */
static void *decoder_main(void *arg)
{
    bool prewarm = arg != NULL;
    if (prewarm) {
        /* Linux's nice is per thread: this lowers only the calling one. */
        setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), PREWARM_NICE);
    }
    rpod_mpd_t *mpd = NULL;

    pthread_mutex_lock(&g.lock);
    for (;;) {
        entry_t *e = queue_pop_best(true);
        if (e != NULL) {
            pthread_mutex_unlock(&g.lock);
            decode_entry(&mpd, e);
            pthread_mutex_lock(&g.lock);
            continue;
        }
        if (prewarm && g.prewarm_next < g.prewarm_count) {
            prewarm_t item = g.prewarm[g.prewarm_next++];
            pthread_mutex_unlock(&g.lock);
            prewarm_one(&mpd, &item);
            pthread_mutex_lock(&g.lock);
            continue;
        }
        if (prewarm && mono_ms() >= g.next_scan_ms) {
            pthread_mutex_unlock(&g.lock);
            scan_library(&mpd);
            pthread_mutex_lock(&g.lock);
            g.next_scan_ms = mono_ms() + RESCAN_MS;
            continue;
        }
        if (mpd != NULL) {
            /* Out of work: drop the connection rather than let it idle into
             * MPD's connection_timeout; the next burst reconnects. */
            pthread_mutex_unlock(&g.lock);
            rpod_mpd_disconnect(mpd);
            mpd = NULL;
            pthread_mutex_lock(&g.lock);
            continue;
        }
        if (prewarm) {
            wait_until(g.next_scan_ms);
        } else {
            pthread_cond_wait(&g.cond, &g.lock);
        }
    }
    return NULL;
}

/* --- LVGL side ------------------------------------------------------ */

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

static bool start_thread(void *(*fn)(void *), void *arg)
{
    pthread_t thread;
    if (pthread_create(&thread, NULL, fn, arg) != 0) {
        return false;
    }
    pthread_detach(thread);
    return true;
}

void rpod_cover_cache_init(const char *mpd_socket, const char *cache_dir)
{
    if (g.started) {
        return;
    }
    g.mpd_socket = strdup(mpd_socket);
    if (g.mpd_socket == NULL) {
        fprintf(stderr, "rpod: cover art workers failed to start; covers disabled\n");
        return;
    }

    /* Timed waits run on the monotonic clock, immune to the wall clock
     * being set (NTP, at boot). */
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&g.cond, &attr);
    pthread_condattr_destroy(&attr);

    if (cache_dir != NULL && cache_dir[0] != '\0') {
        if (make_dirs(cache_dir)) {
            g.cache_dir = strdup(cache_dir);
        } else {
            fprintf(stderr, "rpod: can't create cover cache %s: %s; covers stay in memory\n",
                    cache_dir, strerror(errno));
        }
    }
    if (g.cache_dir != NULL && !start_thread(loader_main, NULL)) {
        free(g.cache_dir);
        g.cache_dir = NULL;
    }

    /* The second decoder prewarms -- pointless with nowhere to keep it. */
    bool ok = start_thread(decoder_main, NULL);
    if (ok && g.cache_dir != NULL) {
        start_thread(decoder_main, (void *)1);
    }
    if (!ok) {
        fprintf(stderr, "rpod: cover art workers failed to start; covers disabled\n");
        return;
    }
    lv_timer_create(poll_cb, POLL_MS, NULL);
    g.started = true;
}

const lv_image_dsc_t *rpod_cover_cache_get(const char *album_artist, const char *album,
                                           const char *uri, int size, bool *pending)
{
    if (pending != NULL) {
        *pending = false;
    }
    if (!g.started || uri == NULL || uri[0] == '\0') {
        return NULL;
    }

    char album_key[KEY_MAX], key[KEY_MAX + 16];
    make_album_key(album_key, sizeof(album_key), album_artist, album, uri);
    int prefix = snprintf(key, sizeof(key), "%d\x1f", size);
    snprintf(key + prefix, sizeof(key) - (size_t)prefix, "%s", album_key);
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
        e->album_key = e->key + prefix;
        e->size = size;
        e->state = ST_QUEUED;
        e->disk_checked = g.cache_dir == NULL || size > MASTER_SIZE;
        if (!e->disk_checked) {
            g.disk_pending++;
        }
        e->want_ms = lv_tick_get();
        e->seq = g.next_seq++;
        e->next = g.buckets[b];
        g.buckets[b] = e;
        e->qnext = g.queue;
        g.queue = e;
        pthread_cond_broadcast(&g.cond);
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

void rpod_cover_cache_settle(uint32_t max_ms)
{
    if (!g.started) {
        return;
    }
    uint64_t deadline = mono_ms() + max_ms;
    pthread_mutex_lock(&g.lock);
    while (g.disk_pending > 0 && mono_ms() < deadline) {
        wait_until(deadline);
    }
    pthread_mutex_unlock(&g.lock);
    poll_cb(NULL);
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
