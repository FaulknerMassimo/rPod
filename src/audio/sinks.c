#include "sinks.h"

#include "mpd_client.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PW_TIMEOUT_S 3
#define DUMP_MAX (2u << 20)
#define WATCH_TICK_MS 500
#define WATCH_REFRESH_TICKS 6
#define SPEAKER_CHECK_MS 5000
#define OWNTONE_API "http://127.0.0.1:3689/api"
#define BT_OUTPUT_NAME RPOD_SINKS_BT_OUTPUT
#define FEED_OUTPUT_NAME RPOD_SINKS_FEED_OUTPUT

static struct {
    pthread_mutex_t lock;
    char dir[128];       /* RPOD_PIPEWIRE_DIR, "" without one */
    char mpd_socket[256];
    bool fake;
    rpod_sink_t sinks[RPOD_SINK_MAX];
    size_t count;
    unsigned version;
    bool running;        /* the worker is alive */
    bool want_refresh;
    bool want_select;
    rpod_sink_t selection; /* valid while want_select */
} g = { .lock = PTHREAD_MUTEX_INITIALIZER };

static const rpod_sink_t k_fake[] = {
    { 11, "", "bluez_output.FAKE.1", "AirPods Pro", RPOD_SINK_BLUETOOTH, true },
    { 0, "101", "Kitchen", "Kitchen", RPOD_SINK_AIRPLAY, false },
    { 0, "102", "Living Room", "Living Room", RPOD_SINK_AIRPLAY, false },
};

/* The directory ends up in a shell command: plain path characters only. */
static bool dir_ok(const char *d)
{
    if (d[0] != '/') {
        return false;
    }
    for (; *d != '\0'; d++) {
        if (!isalnum((unsigned char)*d) && strchr("/_.-", *d) == NULL) {
            return false;
        }
    }
    return true;
}

static void *follow(void *arg);

/* While a speaker plays, asks again now and then: one that drops out (gone
 * from the network, or wanting a PIN) leaves OwnTone with none selected, and
 * the refresh then gives the sound back to the headset. */
static void speaker_check_cb(lv_timer_t *t)
{
    (void)t;
    if (rpod_sinks_airplay_active()) {
        rpod_sinks_refresh();
    }
}

void rpod_sinks_init(const char *mpd_socket)
{
    const char *dir = getenv("RPOD_PIPEWIRE_DIR");
    if (dir != NULL && dir_ok(dir) && strlen(dir) < sizeof(g.dir)) {
        snprintf(g.dir, sizeof(g.dir), "%s", dir);
    }
    if (mpd_socket != NULL) {
        snprintf(g.mpd_socket, sizeof(g.mpd_socket), "%s", mpd_socket);
    }
    g.fake = getenv("RPOD_SINKS_FAKE") != NULL;
    if (g.fake) {
        memcpy(g.sinks, k_fake, sizeof(k_fake));
        g.count = sizeof(k_fake) / sizeof(k_fake[0]);
        g.version++;
    }
    if (rpod_sinks_available()) {
        lv_timer_create(speaker_check_cb, SPEAKER_CHECK_MS, NULL);
    }
    pthread_t t;
    if (!g.fake && rpod_sinks_available() && g.mpd_socket[0] != '\0' &&
        pthread_create(&t, NULL, follow, NULL) == 0) {
        pthread_detach(t);
    }
    rpod_sinks_refresh();
}

bool rpod_sinks_available(void)
{
    return g.fake || g.dir[0] != '\0';
}

/* Runs `cmd`, returns what it printed (malloc'd, NUL-terminated) or NULL. */
static char *run(const char *cmd, size_t *len)
{
    FILE *p = popen(cmd, "r");
    if (p == NULL) {
        return NULL;
    }
    size_t cap = 65536, n = 0;
    char *buf = malloc(cap);
    while (buf != NULL) {
        size_t got = fread(buf + n, 1, cap - n - 1, p);
        n += got;
        if (n < cap - 1) {
            break; /* EOF */
        }
        if (cap >= DUMP_MAX) {
            free(buf);
            buf = NULL;
            break;
        }
        char *bigger = realloc(buf, cap * 2);
        if (bigger == NULL) {
            free(buf);
            buf = NULL;
            break;
        }
        buf = bigger;
        cap *= 2;
    }
    pclose(p);
    if (buf != NULL) {
        buf[n] = '\0';
        *len = n;
    }
    return buf;
}

static void publish(const rpod_sink_t *sinks, size_t n)
{
    pthread_mutex_lock(&g.lock);
    if (n != g.count || memcmp(sinks, g.sinks, n * sizeof(*sinks)) != 0) {
        memcpy(g.sinks, sinks, n * sizeof(*sinks));
        g.count = n;
        g.version++;
    }
    pthread_mutex_unlock(&g.lock);
}

static void fake_select(const rpod_sink_t *sel)
{
    rpod_sink_t s[RPOD_SINK_MAX];
    pthread_mutex_lock(&g.lock);
    size_t n = g.count;
    memcpy(s, g.sinks, n * sizeof(*s));
    pthread_mutex_unlock(&g.lock);
    for (size_t i = 0; i < n; i++) {
        s[i].current = s[i].kind == sel->kind && s[i].id == sel->id && strcmp(s[i].ref, sel->ref) == 0;
    }
    publish(s, n);
}

/* OwnTone's API: GET the outputs, or PUT `body` (digits and JSON
 * punctuation only: it goes into a shell command) to `path`. */
static char *owntone_get(size_t *len)
{
    return run("curl -s --max-time 3 " OWNTONE_API "/outputs 2>/dev/null", len);
}

static void owntone_put(const char *path, const char *body)
{
    char cmd[400];
    snprintf(cmd, sizeof(cmd), "curl -s --max-time 5 -X PUT -d '%s' " OWNTONE_API "%s >/dev/null 2>&1", body, path);
    int rc = system(cmd);
    (void)rc;
}

/* Feeds the speakers (OwnTone, through MPD's fifo output) or the headset
 * (MPD's pipewire output), never both. The feed is off while the headset
 * plays, or OwnTone would start on it by itself. */
static void feed_speakers(rpod_mpd_t *mpd, bool speakers)
{
    if (mpd == NULL) {
        return;
    }
    int before = -1, after = -1;
    rpod_mpd_get_volume(mpd, &before);
    /* Off before on, so nothing plays twice on the way. */
    rpod_mpd_set_output_enabled(mpd, speakers ? BT_OUTPUT_NAME : FEED_OUTPUT_NAME, false);
    rpod_mpd_set_output_enabled(mpd, speakers ? FEED_OUTPUT_NAME : BT_OUTPUT_NAME, true);
    /* Each output has its own software mixer, and the one switched on comes
     * back at the level it was left at. Carry the level over instead: then
     * audio/volume_memory.h, which sees the switch, files it under the
     * device it belonged to and gives the new one its own. */
    if (before >= 0 && rpod_mpd_get_volume(mpd, &after) && after != before) {
        rpod_mpd_set_volume(mpd, (unsigned)before);
    }
}

/* Follows MPD's play state while a speaker plays. A pause or a stop reaches
 * OwnTone only as its feed going quiet, and after 1.5 s of that it suspends
 * playback to wait for more, then restarts once its buffer fills again --
 * the slow way back (4 s), and a path that can deadlock OwnTone 29.3 (its
 * input thread calls into the player holding the lock the player waits on;
 * seen here, src/input.c wait_buffer_ready()). So stop it at once instead,
 * and the feed starting again autostarts it afresh. Stopping drops what it
 * had buffered, which MPD counts as played: a pause moves MPD back by that
 * much, so play picks up where the sound stopped. */
static void *follow(void *arg)
{
    (void)arg;
    rpod_mpd_t *mpd = NULL;
    rpod_mpd_state_t last = RPOD_MPD_STATE_UNKNOWN;
    for (;;) {
        if (mpd == NULL && (mpd = rpod_mpd_connect(g.mpd_socket)) == NULL) {
            sleep(5);
            continue;
        }
        rpod_mpd_state_t state;
        unsigned elapsed_ms = 0, duration_ms = 0;
        if (!rpod_mpd_get_position(mpd, &state, &elapsed_ms, &duration_ms)) {
            sleep(1); /* it reconnects on the next call */
            continue;
        }
        if (last == RPOD_MPD_STATE_PLAY && state != RPOD_MPD_STATE_PLAY && rpod_sinks_airplay_active()) {
            owntone_put("/player/stop", "");
            if (state == RPOD_MPD_STATE_PAUSE) {
                unsigned back = RPOD_SINKS_AIRPLAY_DELAY_MS;
                rpod_mpd_seek(mpd, elapsed_ms > back ? elapsed_ms - back : 0);
            }
        }
        last = state;
        if (!rpod_mpd_wait_player(mpd)) {
            sleep(1);
        }
    }
    return NULL;
}

/* Plays on `sel`: the one thing that's true afterwards is that exactly one of
 * the headset and the speakers is fed. */
static void apply_selection(rpod_mpd_t *mpd, const rpod_sink_t *sel)
{
    char cmd[320], body[96], path[64];
    if (sel->kind == RPOD_SINK_AIRPLAY) {
        for (const char *c = sel->ref; *c != '\0'; c++) {
            if (!isdigit((unsigned char)*c)) {
                return;
            }
        }
        /* Selected before the feed starts, which is when OwnTone plays. */
        snprintf(body, sizeof(body), "{\"outputs\":[\"%s\"]}", sel->ref);
        owntone_put("/outputs/set", body);
        /* The software mixer in MPD is the volume control; the speaker's own
         * level is a fixed ceiling under it. AirPlay maps 0-100 linearly
         * onto -30..0 dB, and 100 was too loud with the wheel low: 50 is
         * 15 dB down, like the Bluetooth ceiling. */
        snprintf(path, sizeof(path), "/outputs/%s", sel->ref);
        owntone_put(path, "{\"volume\":50}");
        feed_speakers(mpd, true);
    } else {
        feed_speakers(mpd, false);
        owntone_put("/outputs/set", "{\"outputs\":[]}");
        snprintf(cmd, sizeof(cmd), "PIPEWIRE_RUNTIME_DIR=%s timeout %d wpctl set-default %u >/dev/null 2>&1",
                 g.dir, PW_TIMEOUT_S, sel->id);
        int rc = system(cmd);
        (void)rc;
    }
}

static void *worker(void *arg)
{
    (void)arg;
    rpod_mpd_t *mpd = NULL;
    for (;;) {
        pthread_mutex_lock(&g.lock);
        bool select = g.want_select;
        rpod_sink_t sel = g.selection;
        bool refresh = g.want_refresh || select;
        g.want_select = false;
        g.want_refresh = false;
        if (!refresh) {
            g.running = false;
            pthread_mutex_unlock(&g.lock);
            rpod_mpd_disconnect(mpd);
            return NULL;
        }
        pthread_mutex_unlock(&g.lock);

        if (g.fake) {
            if (select) {
                fake_select(&sel);
            }
            continue;
        }
        if (mpd == NULL && g.mpd_socket[0] != '\0') {
            mpd = rpod_mpd_connect(g.mpd_socket);
        }
        if (select) {
            apply_selection(mpd, &sel);
        }

        char cmd[320];
        rpod_sink_t sinks[RPOD_SINK_MAX];
        size_t n = 0;
        snprintf(cmd, sizeof(cmd), "PIPEWIRE_RUNTIME_DIR=%s timeout %d pw-dump 2>/dev/null", g.dir, PW_TIMEOUT_S);
        size_t len = 0;
        char *json = run(cmd, &len);
        if (json != NULL) {
            n = rpod_sinks_parse(json, len, sinks, RPOD_SINK_MAX);
        }
        free(json);
        size_t headset = n;
        json = owntone_get(&len);
        if (json != NULL) {
            n += rpod_sinks_parse_owntone(json, len, sinks + n, RPOD_SINK_MAX - n);
        }
        free(json);

        /* One place at a time: a selected speaker takes the sound from the
         * headset, which is only "current" while none is. This also puts
         * MPD's two outputs right after a restart of either side, or
         * OwnTone's going away. */
        bool speaker = false;
        for (size_t i = headset; i < n; i++) {
            speaker = speaker || sinks[i].current;
        }
        for (size_t i = 0; i < headset && speaker; i++) {
            sinks[i].current = false;
        }
        if (!select) {
            feed_speakers(mpd, speaker);
        }
        publish(sinks, n);
    }
}

static void kick(const rpod_sink_t *select)
{
    if (!rpod_sinks_available()) {
        return;
    }
    pthread_mutex_lock(&g.lock);
    g.want_refresh = true;
    if (select != NULL) {
        g.selection = *select;
        g.want_select = true;
    }
    if (!g.running) {
        pthread_t t;
        if (pthread_create(&t, NULL, worker, NULL) == 0) {
            pthread_detach(t);
            g.running = true;
        }
    }
    pthread_mutex_unlock(&g.lock);
}

void rpod_sinks_refresh(void)
{
    kick(NULL);
}

void rpod_sinks_select(const rpod_sink_t *sink)
{
    kick(sink);
}

size_t rpod_sinks_list(rpod_sink_t *out, size_t max)
{
    pthread_mutex_lock(&g.lock);
    size_t n = g.count < max ? g.count : max;
    memcpy(out, g.sinks, n * sizeof(*out));
    pthread_mutex_unlock(&g.lock);
    return n;
}

bool rpod_sinks_current_speaker(char *ref, size_t ref_size)
{
    bool yes = false;
    pthread_mutex_lock(&g.lock);
    for (size_t i = 0; i < g.count && !yes; i++) {
        if (g.sinks[i].current && g.sinks[i].kind == RPOD_SINK_AIRPLAY) {
            if (ref != NULL) {
                snprintf(ref, ref_size, "%s", g.sinks[i].ref);
            }
            yes = true;
        }
    }
    pthread_mutex_unlock(&g.lock);
    return yes;
}

bool rpod_sinks_airplay_active(void)
{
    return rpod_sinks_current_speaker(NULL, 0);
}

typedef struct {
    void (*cb)(void *user);
    void *user;
    unsigned seen;
    unsigned ticks;
    lv_timer_t *timer;
} watch_t;

static unsigned version(void)
{
    pthread_mutex_lock(&g.lock);
    unsigned v = g.version;
    pthread_mutex_unlock(&g.lock);
    return v;
}

static void watch_tick(lv_timer_t *t)
{
    watch_t *w = lv_timer_get_user_data(t);
    if (++w->ticks % WATCH_REFRESH_TICKS == 0) {
        rpod_sinks_refresh();
    }
    unsigned v = version();
    if (v != w->seen) {
        w->seen = v;
        w->cb(w->user);
    }
}

static void watch_delete(lv_event_t *e)
{
    watch_t *w = lv_event_get_user_data(e);
    lv_timer_delete(w->timer);
    free(w);
}

void rpod_sinks_watch(lv_obj_t *owner, void (*cb)(void *user), void *user)
{
    watch_t *w = calloc(1, sizeof(*w));
    if (w == NULL) {
        return;
    }
    w->cb = cb;
    w->user = user;
    w->seen = version();
    w->timer = lv_timer_create(watch_tick, WATCH_TICK_MS, w);
    lv_obj_add_event_cb(owner, watch_delete, LV_EVENT_DELETE, w);
}
