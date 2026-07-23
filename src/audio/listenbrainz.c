#include "listenbrainz.h"

#include <curl/curl.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* In-memory ring buffer for "playing_now" pings (always) and "listen"
 * events when no durable queue_path is configured (fallback only). Small
 * and fixed-capacity -- these are the best-effort entries; if it's ever
 * full the newest one is just dropped and logged. */
#define RPOD_LB_QUEUE_CAP 8

/* On-disk queue cap for durable "listen" events -- bounds worst-case disk
 * use for an extended stretch offline (2000 lines at a few hundred bytes
 * each is comfortably under 1 MB) rather than growing without limit.
 * Appending past this drops the oldest queued entry. */
#define RPOD_LB_FILE_QUEUE_CAP 2000

/* How often the worker retries the on-disk queue when nothing new has been
 * signaled -- long enough not to burn radio/battery hammering a dead
 * connection, short enough that a reconnect is noticed promptly. */
#define RPOD_LB_RETRY_INTERVAL_S 60

#define RPOD_LB_SUBMIT_URL "https://api.listenbrainz.org/1/submit-listens"

typedef enum {
    RPOD_LB_NOW_PLAYING,
    RPOD_LB_LISTEN,
} rpod_lb_entry_type_t;

typedef struct {
    rpod_lb_entry_type_t type;
    char artist[256];
    char title[256];
    char album[256];
    unsigned duration_s;
    time_t listened_at;
} rpod_lb_entry_t;

struct rpod_lb {
    bool enabled;
    char token[128];
    char queue_path[512]; /* empty -- persistence disabled */

    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool shutdown;

    rpod_lb_entry_t queue[RPOD_LB_QUEUE_CAP];
    size_t head, count;
};

/* --- JSON body building --------------------------------------------------
 * The payload shape is fixed and narrow (a couple of strings + scalars),
 * so this hand-rolls it rather than pulling in a JSON library. */

static void json_append_raw(char *buf, size_t buf_size, size_t *len, const char *s)
{
    size_t add_len = strlen(s);
    if (*len + add_len > buf_size - 1) {
        add_len = buf_size - 1 - *len;
    }
    memcpy(buf + *len, s, add_len);
    *len += add_len;
    buf[*len] = '\0';
}

/* Artist/track/album come straight from file tags -- escape anything that
 * would break the surrounding JSON string. */
static void json_escape_append(char *buf, size_t buf_size, size_t *len, const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
        char esc_buf[8];
        const char *esc = NULL;
        switch (*p) {
        case '"':  esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\n': esc = "\\n"; break;
        case '\r': esc = "\\r"; break;
        case '\t': esc = "\\t"; break;
        default:
            if (*p < 0x20) {
                snprintf(esc_buf, sizeof(esc_buf), "\\u%04x", *p);
                esc = esc_buf;
            }
            break;
        }
        if (esc != NULL) {
            json_append_raw(buf, buf_size, len, esc);
        } else {
            char single[2] = { (char)*p, '\0' };
            json_append_raw(buf, buf_size, len, single);
        }
    }
}

static void build_payload(char *out, size_t out_size, const rpod_lb_entry_t *e)
{
    size_t len = 0;
    out[0] = '\0';

    json_append_raw(out, out_size, &len, "{\"listen_type\":\"");
    json_append_raw(out, out_size, &len, e->type == RPOD_LB_LISTEN ? "single" : "playing_now");
    json_append_raw(out, out_size, &len, "\",\"payload\":[{");

    if (e->type == RPOD_LB_LISTEN) {
        char ts[32];
        snprintf(ts, sizeof(ts), "\"listened_at\":%lld,", (long long)e->listened_at);
        json_append_raw(out, out_size, &len, ts);
    }

    json_append_raw(out, out_size, &len, "\"track_metadata\":{\"artist_name\":\"");
    json_escape_append(out, out_size, &len, e->artist);
    json_append_raw(out, out_size, &len, "\",\"track_name\":\"");
    json_escape_append(out, out_size, &len, e->title);
    json_append_raw(out, out_size, &len, "\"");

    if (e->album[0] != '\0') {
        json_append_raw(out, out_size, &len, ",\"release_name\":\"");
        json_escape_append(out, out_size, &len, e->album);
        json_append_raw(out, out_size, &len, "\"");
    }

    if (e->duration_s > 0) {
        char dur[48];
        snprintf(dur, sizeof(dur), ",\"additional_info\":{\"duration_ms\":%u}", e->duration_s * 1000u);
        json_append_raw(out, out_size, &len, dur);
    }

    json_append_raw(out, out_size, &len, "}}]}");
}

/* --- HTTP submission ------------------------------------------------------ */

static size_t discard_write(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    (void)ptr;
    (void)userdata;
    return size * nmemb;
}

/* POSTs a pre-built JSON body (see build_payload()) and reports success --
 * true only on a 2xx response. Never touches the on-disk queue; callers
 * decide what to do with a pending entry based on the result. */
static bool http_post(const char *token, const char *payload)
{
    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        fprintf(stderr, "rpod: listenbrainz curl_easy_init failed\n");
        return false;
    }

    char auth_header[160];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Token %s", token);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, RPOD_LB_SUBMIT_URL);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    /* Required for a reliable timeout when called off the main thread --
     * libcurl's default timeout mechanism uses signals, which are only
     * safe to deliver to a process's main thread. */
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_write);

    bool ok = false;
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "rpod: listenbrainz submit failed: %s\n", curl_easy_strerror(res));
    } else {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (status < 200 || status >= 300) {
            fprintf(stderr, "rpod: listenbrainz submit rejected: HTTP %ld\n", status);
        } else {
            ok = true;
        }
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ok;
}

/* --- On-disk queue for durable "listen" events ---------------------------
 * The queue file's lines *are* the exact POST bodies (build_payload()'s
 * output) -- no separate serialize/parse format, the worker just reads a
 * line and posts it verbatim. */

static void free_lines(char **lines, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        free(lines[i]);
    }
    free(lines);
}

/* Always succeeds in the "no file yet" case (*out_count = 0) -- a missing
 * queue file is the normal state until the first listen is queued, not an
 * error. */
static void read_all_lines(const char *path, char ***out_lines, size_t *out_count)
{
    *out_lines = NULL;
    *out_count = 0;

    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return;
    }

    char **lines = NULL;
    size_t count = 0;
    size_t cap = 0;
    char *line = NULL;
    size_t line_cap = 0;
    ssize_t n;
    while ((n = getline(&line, &line_cap, f)) >= 0) {
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = '\0';
        }
        if (n == 0) {
            continue; /* skip stray blank lines */
        }
        if (count == cap) {
            size_t new_cap = cap == 0 ? 64 : cap * 2;
            char **grown = realloc(lines, new_cap * sizeof(*lines));
            if (grown == NULL) {
                break;
            }
            lines = grown;
            cap = new_cap;
        }
        lines[count++] = strdup(line);
    }
    free(line);
    fclose(f);

    *out_lines = lines;
    *out_count = count;
}

static void write_all_lines(const char *path, char **lines, size_t count)
{
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        fprintf(stderr, "rpod: listenbrainz couldn't write queue file %s: %s\n",
                path, strerror(errno));
        return;
    }
    for (size_t i = 0; i < count; i++) {
        fprintf(f, "%s\n", lines[i]);
    }
    fclose(f);
}

/* Appends payload as a new line, dropping the oldest queued line(s) if that
 * would push the file past RPOD_LB_FILE_QUEUE_CAP. lb->mutex held
 * throughout -- this is pure file I/O, no network call to hold it across. */
static void append_capped(rpod_lb_t *lb, const char *payload)
{
    pthread_mutex_lock(&lb->mutex);

    char **lines = NULL;
    size_t count = 0;
    read_all_lines(lb->queue_path, &lines, &count);

    char **grown = realloc(lines, (count + 1) * sizeof(*grown));
    if (grown == NULL) {
        fprintf(stderr, "rpod: listenbrainz queue append failed (oom)\n");
        free_lines(lines, count);
        pthread_mutex_unlock(&lb->mutex);
        return;
    }
    lines = grown;
    lines[count] = strdup(payload);
    count++;

    size_t start = count > RPOD_LB_FILE_QUEUE_CAP ? count - RPOD_LB_FILE_QUEUE_CAP : 0;
    write_all_lines(lb->queue_path, lines + start, count - start);
    free_lines(lines, count);

    pthread_mutex_unlock(&lb->mutex);
}

/* Attempts the on-disk queue oldest-first, stopping at the first failure
 * (assume the network's still down rather than hammering it), then removes
 * exactly the entries that were sent. Re-reads the file for the removal
 * step rather than trusting the earlier snapshot, since append_capped()
 * may have appended more entries (always at the tail) while the network
 * calls below were in flight -- dropping by count from the front stays
 * correct regardless. */
static void flush_listen_queue(rpod_lb_t *lb)
{
    if (lb->queue_path[0] == '\0') {
        return;
    }

    pthread_mutex_lock(&lb->mutex);
    char **lines = NULL;
    size_t count = 0;
    read_all_lines(lb->queue_path, &lines, &count);
    pthread_mutex_unlock(&lb->mutex);

    if (count == 0) {
        free_lines(lines, count);
        return;
    }

    size_t sent = 0;
    for (; sent < count; sent++) {
        if (!http_post(lb->token, lines[sent])) {
            break;
        }
    }
    free_lines(lines, count);

    if (sent == 0) {
        return;
    }

    pthread_mutex_lock(&lb->mutex);
    char **cur_lines = NULL;
    size_t cur_count = 0;
    read_all_lines(lb->queue_path, &cur_lines, &cur_count);
    size_t remove = sent < cur_count ? sent : cur_count;
    write_all_lines(lb->queue_path, cur_lines + remove, cur_count - remove);
    pthread_mutex_unlock(&lb->mutex);
    free_lines(cur_lines, cur_count);
}

static void *thread_main(void *arg)
{
    rpod_lb_t *lb = arg;

    pthread_mutex_lock(&lb->mutex);
    for (;;) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += RPOD_LB_RETRY_INTERVAL_S;
        pthread_cond_timedwait(&lb->cond, &lb->mutex, &deadline);
        if (lb->shutdown) {
            break;
        }

        /* Drain the in-memory queue: playing_now pings always, plus
         * listen entries too when no durable queue_path is configured. */
        while (lb->count > 0) {
            rpod_lb_entry_t entry = lb->queue[lb->head];
            lb->head = (lb->head + 1) % RPOD_LB_QUEUE_CAP;
            lb->count--;
            pthread_mutex_unlock(&lb->mutex);

            char payload[4096];
            build_payload(payload, sizeof(payload), &entry);
            http_post(lb->token, payload);

            pthread_mutex_lock(&lb->mutex);
        }

        if (lb->queue_path[0] != '\0') {
            pthread_mutex_unlock(&lb->mutex);
            flush_listen_queue(lb);
            pthread_mutex_lock(&lb->mutex);
        }
    }
    pthread_mutex_unlock(&lb->mutex);
    return NULL;
}

static void enqueue(rpod_lb_t *lb, const rpod_lb_entry_t *entry)
{
    pthread_mutex_lock(&lb->mutex);
    if (lb->count == RPOD_LB_QUEUE_CAP) {
        pthread_mutex_unlock(&lb->mutex);
        fprintf(stderr, "rpod: listenbrainz queue full, dropping submission\n");
        return;
    }
    size_t tail = (lb->head + lb->count) % RPOD_LB_QUEUE_CAP;
    lb->queue[tail] = *entry;
    lb->count++;
    pthread_mutex_unlock(&lb->mutex);
    pthread_cond_signal(&lb->cond);
}

rpod_lb_t *rpod_lb_init(const char *token, const char *queue_path)
{
    rpod_lb_t *lb = calloc(1, sizeof(*lb));
    if (lb == NULL) {
        return NULL;
    }
    if (token == NULL || token[0] == '\0') {
        lb->enabled = false;
        return lb;
    }

    snprintf(lb->token, sizeof(lb->token), "%s", token);
    if (queue_path != NULL) {
        snprintf(lb->queue_path, sizeof(lb->queue_path), "%s", queue_path);
    }
    pthread_mutex_init(&lb->mutex, NULL);
    pthread_cond_init(&lb->cond, NULL);
    curl_global_init(CURL_GLOBAL_DEFAULT);

    if (pthread_create(&lb->thread, NULL, thread_main, lb) != 0) {
        fprintf(stderr, "rpod: listenbrainz thread create failed, scrobbling disabled\n");
        curl_global_cleanup();
        pthread_cond_destroy(&lb->cond);
        pthread_mutex_destroy(&lb->mutex);
        lb->enabled = false;
        return lb;
    }
    lb->enabled = true;
    return lb;
}

void rpod_lb_now_playing(rpod_lb_t *lb, const char *artist, const char *title,
                          const char *album, unsigned duration_s)
{
    if (lb == NULL || !lb->enabled) {
        return;
    }
    rpod_lb_entry_t entry = { 0 };
    entry.type = RPOD_LB_NOW_PLAYING;
    snprintf(entry.artist, sizeof(entry.artist), "%s", artist != NULL ? artist : "");
    snprintf(entry.title, sizeof(entry.title), "%s", title != NULL ? title : "");
    snprintf(entry.album, sizeof(entry.album), "%s", album != NULL ? album : "");
    entry.duration_s = duration_s;
    enqueue(lb, &entry);
}

void rpod_lb_submit_listen(rpod_lb_t *lb, const char *artist, const char *title,
                            const char *album, unsigned duration_s, time_t listened_at)
{
    if (lb == NULL || !lb->enabled) {
        return;
    }
    rpod_lb_entry_t entry = { 0 };
    entry.type = RPOD_LB_LISTEN;
    snprintf(entry.artist, sizeof(entry.artist), "%s", artist != NULL ? artist : "");
    snprintf(entry.title, sizeof(entry.title), "%s", title != NULL ? title : "");
    snprintf(entry.album, sizeof(entry.album), "%s", album != NULL ? album : "");
    entry.duration_s = duration_s;
    entry.listened_at = listened_at;

    if (lb->queue_path[0] == '\0') {
        /* No durable queue configured -- fall back to best-effort,
         * in-memory-only, same as playing_now. */
        enqueue(lb, &entry);
        return;
    }

    char payload[4096];
    build_payload(payload, sizeof(payload), &entry);
    append_capped(lb, payload);
    pthread_cond_signal(&lb->cond);
}

void rpod_lb_shutdown(rpod_lb_t *lb)
{
    if (lb == NULL) {
        return;
    }
    if (lb->enabled) {
        pthread_mutex_lock(&lb->mutex);
        lb->shutdown = true;
        pthread_mutex_unlock(&lb->mutex);
        pthread_cond_signal(&lb->cond);
        pthread_join(lb->thread, NULL);
        pthread_cond_destroy(&lb->cond);
        pthread_mutex_destroy(&lb->mutex);
        curl_global_cleanup();
    }
    free(lb);
}
