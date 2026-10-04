#include "volume_memory.h"

#include "audio/bluetooth.h"

#include "lvgl.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VM_POLL_MS     1000
#define VM_MAX_DEVICES 32
#define VM_WIRED_KEY   "wired"

/* At startup MPD's volume belongs to whichever device was in use when it
 * last saved state, and BlueZ's device list arrives asynchronously. Wait
 * this long for it before concluding there's no bluetoothd at all and
 * treating the output as wired. */
#define VM_BT_GRACE_MS 5000

/* A device seen for the first time keeps the current level, but no louder
 * than this -- new headphones shouldn't inherit a speaker's level. */
#define VM_NEW_DEVICE_MAX 50

typedef struct {
    char key[24]; /* "AA:BB:CC:DD:EE:FF", or VM_WIRED_KEY */
    int volume;
} vm_entry_t;

static struct {
    rpod_mpd_t *mpd;
    char path[512]; /* empty: in-memory only */

    /* Most recently used first; the last one is dropped when full. */
    vm_entry_t entries[VM_MAX_DEVICES];
    size_t count;

    char active[24]; /* key whose level MPD's volume is now; "" until known */
    bool dirty;      /* entries differ from what's on disk */
    uint32_t started_ms;
    unsigned generation;
} g;

static vm_entry_t *find(const char *key)
{
    for (size_t i = 0; i < g.count; i++) {
        if (strcmp(g.entries[i].key, key) == 0) {
            return &g.entries[i];
        }
    }
    return NULL;
}

/* Records `volume` for `key` and moves it to the front (most recent). */
static void remember(const char *key, int volume)
{
    vm_entry_t e = { .volume = volume };
    snprintf(e.key, sizeof(e.key), "%s", key);

    size_t at = g.count;
    for (size_t i = 0; i < g.count; i++) {
        if (strcmp(g.entries[i].key, key) == 0) {
            at = i;
            break;
        }
    }
    if (at == g.count) {
        at = g.count < VM_MAX_DEVICES ? g.count++ : VM_MAX_DEVICES - 1;
    }
    memmove(&g.entries[1], &g.entries[0], at * sizeof(g.entries[0]));
    g.entries[0] = e;
    g.dirty = true;
}

static void load(void)
{
    if (g.path[0] == '\0') {
        return;
    }
    FILE *f = fopen(g.path, "r");
    if (f == NULL) {
        return; /* first run */
    }
    char key[sizeof(g.entries[0].key)];
    int volume;
    while (g.count < VM_MAX_DEVICES && fscanf(f, "%23s %d", key, &volume) == 2) {
        if (volume >= 0 && volume <= 100 && find(key) == NULL) {
            vm_entry_t *e = &g.entries[g.count++];
            snprintf(e->key, sizeof(e->key), "%s", key);
            e->volume = volume;
        }
    }
    fclose(f);
}

/* Written to a temp file and renamed over the real one, so a power cut
 * mid-write can't leave it truncated. */
static void save(void)
{
    if (!g.dirty || g.path[0] == '\0') {
        g.dirty = false;
        return;
    }
    char tmp[sizeof(g.path) + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g.path);
    FILE *f = fopen(tmp, "w");
    if (f == NULL) {
        fprintf(stderr, "rpod: volume memory couldn't write %s: %s\n", tmp, strerror(errno));
        g.dirty = false; /* don't retry (and log) every second */
        return;
    }
    for (size_t i = 0; i < g.count; i++) {
        fprintf(f, "%s %d\n", g.entries[i].key, g.entries[i].volume);
    }
    if (fclose(f) != 0 || rename(tmp, g.path) != 0) {
        fprintf(stderr, "rpod: volume memory couldn't save %s: %s\n", g.path, strerror(errno));
        remove(tmp);
    }
    g.dirty = false;
}

/* BlueZ device path ".../dev_AA_BB_CC_DD_EE_FF" -> "AA:BB:CC:DD:EE:FF". The
 * address, not the path, so a different adapter (hci1) keeps the levels. */
static void key_from_path(const char *path, char *out, size_t out_size)
{
    const char *dev = strrchr(path, '/');
    dev = dev != NULL ? dev + 1 : path;
    if (strncmp(dev, "dev_", 4) == 0) {
        dev += 4;
    }
    snprintf(out, out_size, "%s", dev);
    for (char *p = out; *p != '\0'; p++) {
        if (*p == '_') {
            *p = ':';
        }
    }
}

/* Which device MPD's volume should belong to right now, or false to leave it
 * as is. The device already active wins while it's still connected; a
 * second headset connecting alongside doesn't steal it. */
static bool current_key(char *out, size_t out_size)
{
    if (rpod_bt_state() == RPOD_BT_UNAVAILABLE) {
        /* bluetoothd restarting (or never started): keep whatever's active
         * rather than bounce the volume to wired and back. With nothing
         * active yet, only fall back to wired once BlueZ has had its chance
         * to report a connected headset. */
        if (g.active[0] != '\0' || lv_tick_elaps(g.started_ms) < VM_BT_GRACE_MS) {
            return false;
        }
        snprintf(out, out_size, "%s", VM_WIRED_KEY);
        return true;
    }

    bool found = false;
    for (size_t i = 0; i < rpod_bt_device_count(); i++) {
        const rpod_bt_device_t *d = rpod_bt_device_at(i);
        if (!d->connected || !d->audio) {
            continue;
        }
        char key[sizeof(g.active)];
        key_from_path(d->path, key, sizeof(key));
        if (strcmp(key, g.active) == 0) {
            snprintf(out, out_size, "%s", key);
            return true;
        }
        if (!found) {
            snprintf(out, out_size, "%s", key);
            found = true;
        }
    }
    if (!found) {
        snprintf(out, out_size, "%s", VM_WIRED_KEY);
    }
    return true;
}

/* Hands MPD's volume over from the active device to `key`: the outgoing
 * device keeps the level it was left at, and the incoming one gets its own
 * back. Runs on the BlueZ Connected/disconnected change itself, before A2DP
 * audio is flowing to the new device. */
static void switch_to(const char *key)
{
    int cur = -1;
    rpod_mpd_get_volume(g.mpd, &cur);

    if (g.active[0] != '\0' && cur >= 0) {
        remember(g.active, cur);
    }

    const vm_entry_t *e = find(key);
    bool known = e != NULL;
    int target = known ? e->volume : (cur > VM_NEW_DEVICE_MAX ? VM_NEW_DEVICE_MAX : cur);
    if (target >= 0) {
        if (target != cur && rpod_mpd_set_volume(g.mpd, (unsigned)target)) {
            g.generation++;
        }
        remember(key, target);
    }
    fprintf(stderr, "rpod: volume: %s -> %s at %d%%%s\n", g.active[0] != '\0' ? g.active : "(startup)",
            key, target, known ? "" : " (new)");
    snprintf(g.active, sizeof(g.active), "%s", key);
    save();
}

static void evaluate(void)
{
    char key[sizeof(g.active)];
    if (current_key(key, sizeof(key)) && strcmp(key, g.active) != 0) {
        switch_to(key);
    }
}

static void bt_changed_cb(void *user)
{
    (void)user;
    evaluate();
}

/* Records the active device's level once a second, and writes the file once
 * it has settled (unchanged since the last poll) -- one write per volume
 * change rather than one per second of turning. Also re-evaluates the active
 * device, which covers the startup grace period running out. */
static void poll_cb(lv_timer_t *timer)
{
    (void)timer;
    evaluate();
    if (g.active[0] == '\0') {
        return;
    }
    int v;
    if (!rpod_mpd_get_volume(g.mpd, &v) || v < 0) {
        return;
    }
    const vm_entry_t *e = find(g.active);
    if (e == NULL || e->volume != v) {
        remember(g.active, v);
    } else if (g.dirty) {
        save();
    }
}

void rpod_volume_memory_init(rpod_mpd_t *mpd, const char *state_path)
{
    g.mpd = mpd;
    if (state_path != NULL) {
        snprintf(g.path, sizeof(g.path), "%s", state_path);
    }
    load();
    g.started_ms = lv_tick_get();
    rpod_bt_watch(NULL, bt_changed_cb, NULL);
    lv_timer_create(poll_cb, VM_POLL_MS, NULL);
}

unsigned rpod_volume_memory_generation(void)
{
    return g.generation;
}
