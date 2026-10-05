#include "bluetooth.h"

#include "aap.h"
#include "avrcp_volume.h"

#include <systemd/sd-bus.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define POLL_MS            50
#define RECONNECT_MS       5000
/* sd_bus_process() handles one message per call. A busy discovery can queue
 * hundreds of PropertiesChanged; cap the work per tick so the wheel stays
 * responsive, and pick the rest up next tick. */
#define MAX_MSGS_PER_TICK  128
/* Pair() waits on the user's device (and a page timeout for one that's off),
 * well past sd-bus's 25 s default. */
#define PAIR_TIMEOUT_USEC    (60ULL * 1000 * 1000)
#define CONNECT_TIMEOUT_USEC (30ULL * 1000 * 1000)
/* How long after Bluetooth comes up to ask paired headphones to reconnect:
 * at boot WirePlumber registers its A2DP endpoints a few seconds after
 * bluetoothd starts, and a Connect() before that gets no audio profile. */
#define AUTOCONNECT_DELAY_MS 5000

#define BLUEZ          "org.bluez"
#define ADAPTER_IFACE  "org.bluez.Adapter1"
#define DEVICE_IFACE   "org.bluez.Device1"
#define PROPS_IFACE    "org.freedesktop.DBus.Properties"
#define OM_IFACE       "org.freedesktop.DBus.ObjectManager"
#define TRANSPORT_IFACE "org.bluez.MediaTransport1"
#define AGENT_MANAGER_IFACE "org.bluez.AgentManager1"
#define AGENT_IFACE    "org.bluez.Agent1"
#define AGENT_PATH     "/rpod/agent"

typedef struct {
    rpod_bt_device_t pub;
    /* The three signals rpod_bt_device_t.audio is derived from. */
    uint32_t cls;
    bool icon_audio;
    bool uuid_audio;
    bool trusted;           /* may connect in without asking */
    bool autoconnect_tried; /* asked to reconnect this round already */
} device_t;

/* An A2DP stream (MediaTransport1) -- where a headset's own volume shows. */
typedef struct {
    char path[64];
    rpod_avrcp_volume_t volume;
} transport_t;

typedef struct {
    lv_obj_t *owner;
    void (*cb)(void *user);
    void *user;
} watcher_t;

/* send_call() flags. */
enum {
    CALL_FROM_PAIR = 1 << 0, /* the Connect() that finishes our own Pair() */
    CALL_AUTO      = 1 << 1, /* a reconnect nobody asked for: keep failures quiet */
};

/* Context for one async call's reply, freed by its slot's destroy callback. */
typedef struct {
    unsigned gen;       /* g.gen at send time -- see below */
    const char *what;   /* method name, for the log */
    char path[64];      /* device (or transport) the call was about, "" if none */
    unsigned flags;     /* CALL_* */
} call_t;

static struct {
    sd_bus *bus;
    uint32_t retry_at;     /* lv_tick_get() time to retry opening the bus */
    bool warned_no_bus;    /* said so once already -- don't repeat every retry */
    bool default_agent;    /* claim BlueZ's default agent (rpod_bt_init()) */
    bool debug;            /* RPOD_BT_DEBUG: log headset volume changes */
    /* Bumped whenever the mirror is about to be rebuilt from scratch: a
     * reply to a call sent before then refers to state that's gone. */
    unsigned gen;

    bool bluez_up;
    char adapter[64];      /* object path; "" = no adapter */
    bool powered;
    bool discovering;
    bool blocked;          /* PowerState "off-blocked" -- rfkill */
    bool power_pending;
    char adapter_error[64];

    device_t *devs;
    size_t ndev, devcap;
    transport_t *transports;
    size_t ntransport, transportcap;

    /* Reconnecting paired headphones once Bluetooth comes up. */
    bool autoconnect_due;
    uint32_t autoconnect_at;

    /* Headset volume button steps not yet handed to volume_cb. */
    int volume_steps;
    void (*volume_cb)(int steps, void *user);
    void *volume_user;

    bool scan_wanted;      /* a scan screen is open */
    bool discovery_active; /* we've called StartDiscovery and not stopped */
    unsigned pairs_in_flight;

    bool dirty;
    watcher_t *watchers;
    size_t nwatch, watchcap;
} g;

static void mark_dirty(void)
{
    g.dirty = true;
}

/* Copies at most dst_size - 1 bytes of src without splitting a UTF-8
 * sequence -- a half character would render as garbage. */
static void copy_utf8(char *dst, size_t dst_size, const char *src)
{
    size_t n = strlen(src);
    if (n >= dst_size) {
        n = dst_size - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) {
            n--;
        }
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Turns a BlueZ error into something that fits a list row. BlueZ's own
 * messages are terse protocol strings ("br-connection-page-timeout"); the
 * common ones get a plain-English version, anything else passes through. */
static void describe_error(const sd_bus_error *e, char *out, size_t out_size)
{
    static const struct {
        const char *needle;
        const char *text;
    } known[] = {
        { "page-timeout",        "Not responding - is it on?" },
        { "Page Timeout",        "Not responding - is it on?" },
        { "Host is down",        "Not responding - is it on?" },
        { "AuthenticationTimeout", "Not responding - is it on?" },
        { "profile-unavailable", "No audio service (PipeWire?)" },
        { "key-missing",         "Pairing lost - forget, then pair" },
        { "rfkill",              "Blocked by rfkill" },
        { "Authentication",      "Pairing was rejected" },
        { "NoReply",             "Timed out" },
        { "Timeout",             "Timed out" },
    };
    const char *name = e->name != NULL ? e->name : "";
    const char *msg = e->message != NULL ? e->message : "";
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (strstr(msg, known[i].needle) != NULL || strstr(name, known[i].needle) != NULL) {
            copy_utf8(out, out_size, known[i].text);
            return;
        }
    }
    copy_utf8(out, out_size, msg[0] != '\0' ? msg : name);
}

/* --- Mirror ----------------------------------------------------------------- */

static device_t *find_dev(const char *path)
{
    for (size_t i = 0; i < g.ndev; i++) {
        if (strcmp(g.devs[i].pub.path, path) == 0) {
            return &g.devs[i];
        }
    }
    return NULL;
}

static device_t *add_dev(const char *path)
{
    if (strlen(path) >= sizeof(g.devs[0].pub.path)) {
        return NULL;
    }
    if (g.ndev == g.devcap) {
        size_t cap = g.devcap ? g.devcap * 2 : 16;
        device_t *grown = realloc(g.devs, cap * sizeof(*grown));
        if (grown == NULL) {
            return NULL;
        }
        g.devs = grown;
        g.devcap = cap;
    }
    device_t *d = &g.devs[g.ndev++];
    memset(d, 0, sizeof(*d));
    snprintf(d->pub.path, sizeof(d->pub.path), "%s", path);
    d->pub.bonded = true; /* until BlueZ says otherwise -- see bluetooth.h */
    /* BlueZ always sends Alias; this only shows if it somehow didn't. */
    snprintf(d->pub.name, sizeof(d->pub.name), "%s", path);
    mark_dirty();
    return d;
}

static void remove_dev(const char *path)
{
    device_t *d = find_dev(path);
    if (d == NULL) {
        return;
    }
    size_t i = (size_t)(d - g.devs);
    memmove(&g.devs[i], &g.devs[i + 1], (g.ndev - i - 1) * sizeof(*g.devs));
    g.ndev--;
    mark_dirty();
}

static transport_t *find_transport(const char *path)
{
    for (size_t i = 0; i < g.ntransport; i++) {
        if (strcmp(g.transports[i].path, path) == 0) {
            return &g.transports[i];
        }
    }
    return NULL;
}

static transport_t *add_transport(const char *path)
{
    if (strlen(path) >= sizeof(g.transports[0].path)) {
        return NULL;
    }
    if (g.ntransport == g.transportcap) {
        size_t cap = g.transportcap ? g.transportcap * 2 : 4;
        transport_t *grown = realloc(g.transports, cap * sizeof(*grown));
        if (grown == NULL) {
            return NULL;
        }
        g.transports = grown;
        g.transportcap = cap;
    }
    transport_t *t = &g.transports[g.ntransport++];
    snprintf(t->path, sizeof(t->path), "%s", path);
    rpod_avrcp_volume_init(&t->volume);
    return t;
}

static void remove_transport(const char *path)
{
    transport_t *t = find_transport(path);
    if (t != NULL) {
        size_t i = (size_t)(t - g.transports);
        memmove(&g.transports[i], &g.transports[i + 1], (g.ntransport - i - 1) * sizeof(*t));
        g.ntransport--;
    }
}

static bool under_adapter(const char *path)
{
    size_t n = strlen(g.adapter);
    return n > 0 && strncmp(path, g.adapter, n) == 0 && path[n] == '/';
}

/* Forgets everything about the adapter and its devices. Calls in flight
 * become stale (see g.gen), so nothing they'd have cleaned up -- an op
 * marker, a pairing count -- is left dangling. */
static void reset_mirror(void)
{
    g.gen++;
    g.adapter[0] = '\0';
    g.powered = g.discovering = g.blocked = g.power_pending = false;
    g.adapter_error[0] = '\0';
    g.ndev = 0;
    g.ntransport = 0;
    g.discovery_active = false;
    g.pairs_in_flight = 0;
    g.autoconnect_due = false;
    mark_dirty();
}

static void update_audio(device_t *d)
{
    /* Major device class 0x04 is Audio/Video (headsets, headphones,
     * speakers, car audio). BlueZ's Icon covers LE devices by appearance
     * too, and an A2DP-sink/headset/hands-free UUID catches anything whose
     * class undersells it. The Audio *service* class bit isn't used: phones
     * set it as well. */
    bool audio = ((d->cls >> 8) & 0x1f) == 0x04 || d->icon_audio || d->uuid_audio;
    if (audio != d->pub.audio) {
        d->pub.audio = audio;
        mark_dirty();
    }
}

static bool is_audio_uuid(const char *uuid)
{
    static const char *const sinks[] = {
        "0000110b-", /* Audio Sink */
        "0000110d-", /* A2DP */
        "00001108-", /* Headset */
        "0000111e-", /* Hands-Free */
    };
    for (size_t i = 0; i < sizeof(sinks) / sizeof(sinks[0]); i++) {
        if (strncasecmp(uuid, sinks[i], strlen(sinks[i])) == 0) {
            return true;
        }
    }
    return false;
}

/* Asks for a reconnect round AUTOCONNECT_DELAY_MS from now: Bluetooth just
 * came up. Every paired device gets one try again. */
static void schedule_autoconnect(void)
{
    g.autoconnect_due = true;
    g.autoconnect_at = lv_tick_get() + AUTOCONNECT_DELAY_MS;
    for (size_t i = 0; i < g.ndev; i++) {
        g.devs[i].autoconnect_tried = false;
    }
}

/* "Massimo's AirPods Pro - Find My": what AirPods with Find My call
 * themselves to anything that isn't an Apple device. */
static void strip_find_my(char *name)
{
    static const char suffix[] = " - Find My";
    size_t n = strlen(name), k = sizeof(suffix) - 1;
    if (n > k && strcmp(name + n - k, suffix) == 0) {
        name[n - k] = '\0';
    }
}

/* --- Property parsing --------------------------------------------------------
 *
 * A prop_fn is handed one a{sv} entry, positioned inside its variant. It
 * reads the value and returns > 0 if it wants the key, or returns 0 without
 * reading anything and the caller skips it. */

typedef int (*prop_fn)(void *obj, const char *key, const char *sig, sd_bus_message *m);

static int set_bool(bool *field, sd_bus_message *m)
{
    int b;
    int r = sd_bus_message_read_basic(m, 'b', &b);
    if (r >= 0 && *field != (b != 0)) {
        *field = b != 0;
        mark_dirty();
    }
    return r < 0 ? r : 1;
}

static int parse_props(sd_bus_message *m, prop_fn fn, void *obj)
{
    int r = sd_bus_message_enter_container(m, 'a', "{sv}");
    if (r < 0) {
        return r;
    }
    while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
        const char *key, *sig;
        if ((r = sd_bus_message_read_basic(m, 's', &key)) < 0 ||
            (r = sd_bus_message_peek_type(m, NULL, &sig)) < 0 ||
            (r = sd_bus_message_enter_container(m, 'v', sig)) < 0) {
            return r;
        }
        r = fn(obj, key, sig, m);
        if (r == 0) {
            r = sd_bus_message_skip(m, sig);
        }
        if (r < 0 ||
            (r = sd_bus_message_exit_container(m)) < 0 ||  /* variant */
            (r = sd_bus_message_exit_container(m)) < 0) {  /* dict entry */
            return r;
        }
    }
    if (r < 0) {
        return r;
    }
    return sd_bus_message_exit_container(m);
}

static int adapter_prop(void *obj, const char *key, const char *sig, sd_bus_message *m)
{
    (void)obj;
    if (strcmp(sig, "b") == 0) {
        if (strcmp(key, "Powered") == 0) {
            bool was = g.powered;
            int r = set_bool(&g.powered, m);
            if (!g.powered) {
                /* bluetoothd drops every client's discovery session when
                 * the adapter powers off -- ours has to be asked for again. */
                g.discovery_active = false;
            } else if (!was && g.bluez_up) {
                schedule_autoconnect();
            }
            return r;
        }
        if (strcmp(key, "Discovering") == 0) {
            return set_bool(&g.discovering, m);
        }
    } else if (strcmp(sig, "s") == 0 && strcmp(key, "PowerState") == 0) {
        /* BlueZ 5.65+: "off-blocked" means rfkill, which Powered=true can't
         * override -- worth saying so before the user tries. */
        const char *s;
        int r = sd_bus_message_read_basic(m, 's', &s);
        if (r < 0) {
            return r;
        }
        bool blocked = strcmp(s, "off-blocked") == 0;
        if (blocked != g.blocked) {
            g.blocked = blocked;
            mark_dirty();
        }
        return 1;
    }
    return 0;
}

static int device_prop(void *obj, const char *key, const char *sig, sd_bus_message *m)
{
    device_t *d = obj;
    int r;
    if (strcmp(sig, "b") == 0) {
        if (strcmp(key, "Paired") == 0) {
            return set_bool(&d->pub.paired, m);
        }
        if (strcmp(key, "Bonded") == 0) {
            return set_bool(&d->pub.bonded, m);
        }
        if (strcmp(key, "Trusted") == 0) {
            return set_bool(&d->trusted, m);
        }
        if (strcmp(key, "Connected") == 0) {
            r = set_bool(&d->pub.connected, m);
            if (d->pub.connected) {
                /* Connected after all (e.g. it reconnected by itself): an
                 * earlier failure is no longer worth showing. */
                d->pub.error[0] = '\0';
            }
            return r;
        }
    } else if (strcmp(sig, "s") == 0) {
        const char *s;
        if (strcmp(key, "Alias") == 0) {
            if ((r = sd_bus_message_read_basic(m, 's', &s)) < 0) {
                return r;
            }
            char name[sizeof(d->pub.name)];
            copy_utf8(name, sizeof(name), s);
            strip_find_my(name);
            if (strcmp(name, d->pub.name) != 0) {
                memcpy(d->pub.name, name, sizeof(name));
                mark_dirty();
            }
            return 1;
        }
        if (strcmp(key, "Name") == 0) {
            /* Alias carries the text; Name's presence says it's real. */
            if ((r = sd_bus_message_read_basic(m, 's', &s)) < 0) {
                return r;
            }
            if (!d->pub.named) {
                d->pub.named = true;
                mark_dirty();
            }
            return 1;
        }
        if (strcmp(key, "Icon") == 0) {
            if ((r = sd_bus_message_read_basic(m, 's', &s)) < 0) {
                return r;
            }
            d->icon_audio = strncmp(s, "audio-", 6) == 0;
            return 1;
        }
        if (strcmp(key, "Address") == 0) {
            if ((r = sd_bus_message_read_basic(m, 's', &s)) < 0) {
                return r;
            }
            snprintf(d->pub.address, sizeof(d->pub.address), "%s", s);
            return 1;
        }
    } else if (strcmp(sig, "u") == 0 && strcmp(key, "Class") == 0) {
        return (r = sd_bus_message_read_basic(m, 'u', &d->cls)) < 0 ? r : 1;
    } else if (strcmp(sig, "as") == 0 && strcmp(key, "UUIDs") == 0) {
        if ((r = sd_bus_message_enter_container(m, 'a', "s")) < 0) {
            return r;
        }
        bool audio = false, aap = false;
        const char *uuid;
        while ((r = sd_bus_message_read_basic(m, 's', &uuid)) > 0) {
            audio = audio || is_audio_uuid(uuid);
            aap = aap || strcasecmp(uuid, RPOD_AAP_UUID) == 0;
        }
        if (r < 0 || (r = sd_bus_message_exit_container(m)) < 0) {
            return r;
        }
        d->uuid_audio = audio;
        if (aap != d->pub.aap) {
            d->pub.aap = aap;
            mark_dirty();
        }
        return 1;
    }
    return 0;
}

static sd_bus_message *new_call(const char *path, const char *iface, const char *method);
static bool send_call(sd_bus_message *m, sd_bus_message_handler_t cb, const char *what,
                      const char *path, unsigned flags, uint64_t timeout_usec);
static const sd_bus_error *reply_error(sd_bus_message *reply, const call_t *c);
static void autoconnect_next(void);

/* One PropertiesChanged's (or GetManagedObjects') worth of a transport. */
typedef struct {
    int volume; /* -1: not in this update */
    int active; /* -1: not in this update; else whether State is "active" */
} transport_update_t;

static int transport_prop(void *obj, const char *key, const char *sig, sd_bus_message *m)
{
    transport_update_t *u = obj;
    int r;
    if (strcmp(sig, "q") == 0 && strcmp(key, "Volume") == 0) {
        uint16_t v;
        if ((r = sd_bus_message_read_basic(m, 'q', &v)) < 0) {
            return r;
        }
        u->volume = v;
        return 1;
    }
    if (strcmp(sig, "s") == 0 && strcmp(key, "State") == 0) {
        const char *st;
        if ((r = sd_bus_message_read_basic(m, 's', &st)) < 0) {
            return r;
        }
        u->active = strcmp(st, "active") == 0;
        return 1;
    }
    return 0;
}

static int on_repin(sd_bus_message *reply, void *userdata, sd_bus_error *ret_error)
{
    (void)ret_error;
    call_t *c = userdata;
    if (reply_error(reply, c) != NULL && c->gen == g.gen) {
        transport_t *t = find_transport(c->path);
        if (t != NULL) {
            fprintf(stderr, "rpod: bluetooth: can't pin %s's volume; its buttons won't reach MPD\n",
                    c->path);
            rpod_avrcp_volume_repin_failed(&t->volume);
        }
    }
    return 0;
}

/* Feeds a transport update to its volume logic (audio/avrcp_volume.h), and
 * does what that says: queue steps for volume_cb, set the headset back. */
static void apply_transport_update(transport_t *t, const transport_update_t *u)
{
    uint32_t now = lv_tick_get();
    if (u->active >= 0) {
        rpod_avrcp_volume_state(&t->volume, u->active != 0, now);
    }
    if (u->volume < 0) {
        return;
    }
    int repin;
    int steps = rpod_avrcp_volume_changed(&t->volume, u->volume, now, &repin);
    if (g.debug) {
        fprintf(stderr, "rpod: bluetooth: %s volume %d -> %+d steps%s\n", t->path, u->volume, steps,
                repin >= 0 ? ", repinning" : "");
    }
    g.volume_steps += steps;
    if (repin < 0) {
        return;
    }
    sd_bus_message *m = new_call(t->path, PROPS_IFACE, "Set");
    if (m != NULL &&
        sd_bus_message_append(m, "ssv", TRANSPORT_IFACE, "Volume", "q", (uint16_t)repin) < 0) {
        sd_bus_message_unref(m);
        m = NULL;
    }
    send_call(m, on_repin, "Set Volume", t->path, 0, 0);
}

static void parse_transport(sd_bus_message *m, const char *path, int *r)
{
    transport_update_t u = { .volume = -1, .active = -1 };
    transport_t *t = find_transport(path);
    if (t == NULL) {
        t = add_transport(path);
    }
    *r = parse_props(m, transport_prop, &u);
    if (*r >= 0 && t != NULL) {
        apply_transport_update(t, &u);
    }
}

/* Reads one object's a{sa{sv}} (interface -> properties), applying the
 * adapter and/or device interfaces as asked and skipping everything else.
 * A device is only taken if it belongs to the current adapter; an adapter
 * only if there's no current one yet (the first one found wins). */
static int parse_object(sd_bus_message *m, const char *path, bool adapters, bool devices)
{
    int r = sd_bus_message_enter_container(m, 'a', "{sa{sv}}");
    if (r < 0) {
        return r;
    }
    while ((r = sd_bus_message_enter_container(m, 'e', "sa{sv}")) > 0) {
        const char *iface;
        if ((r = sd_bus_message_read_basic(m, 's', &iface)) < 0) {
            return r;
        }
        device_t *d = NULL;
        if (adapters && strcmp(iface, ADAPTER_IFACE) == 0 &&
            (g.adapter[0] == '\0' || strcmp(g.adapter, path) == 0) &&
            strlen(path) < sizeof(g.adapter)) {
            if (g.adapter[0] == '\0') {
                snprintf(g.adapter, sizeof(g.adapter), "%s", path);
                mark_dirty();
            }
            r = parse_props(m, adapter_prop, NULL);
        } else if (devices && strcmp(iface, DEVICE_IFACE) == 0 && under_adapter(path) &&
                   ((d = find_dev(path)) != NULL || (d = add_dev(path)) != NULL)) {
            r = parse_props(m, device_prop, d);
            update_audio(d);
        } else if (devices && strcmp(iface, TRANSPORT_IFACE) == 0 && under_adapter(path)) {
            parse_transport(m, path, &r);
        } else {
            r = sd_bus_message_skip(m, "a{sv}");
        }
        if (r < 0 || (r = sd_bus_message_exit_container(m)) < 0) {
            return r;
        }
    }
    if (r < 0) {
        return r;
    }
    return sd_bus_message_exit_container(m);
}

/* GetManagedObjects' a{oa{sa{sv}}}, adapters or devices only. */
static int parse_objects(sd_bus_message *m, bool adapters, bool devices)
{
    int r = sd_bus_message_enter_container(m, 'a', "{oa{sa{sv}}}");
    if (r < 0) {
        return r;
    }
    while ((r = sd_bus_message_enter_container(m, 'e', "oa{sa{sv}}")) > 0) {
        const char *path;
        if ((r = sd_bus_message_read_basic(m, 'o', &path)) < 0 ||
            (r = parse_object(m, path, adapters, devices)) < 0 ||
            (r = sd_bus_message_exit_container(m)) < 0) {
            return r;
        }
    }
    if (r < 0) {
        return r;
    }
    return sd_bus_message_exit_container(m);
}

/* --- Calls ------------------------------------------------------------------- */

static sd_bus_message *new_call(const char *path, const char *iface, const char *method)
{
    sd_bus_message *m = NULL;
    if (g.bus == NULL || sd_bus_message_new_method_call(g.bus, &m, BLUEZ, path, iface, method) < 0) {
        return NULL;
    }
    /* Never D-Bus-activate bluetoothd from here: whether it runs is the
     * system's call. If it starts later, NameOwnerChanged picks it up. */
    sd_bus_message_set_auto_start(m, 0);
    return m;
}

/* Sends `m` (consuming it) with `cb` as the reply handler. */
static bool send_call(sd_bus_message *m, sd_bus_message_handler_t cb, const char *what,
                      const char *path, unsigned flags, uint64_t timeout_usec)
{
    if (m == NULL) {
        return false;
    }
    call_t *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        sd_bus_message_unref(m);
        return false;
    }
    c->gen = g.gen;
    c->what = what;
    c->flags = flags;
    if (path != NULL) {
        snprintf(c->path, sizeof(c->path), "%s", path);
    }
    sd_bus_slot *slot = NULL;
    int r = sd_bus_call_async(g.bus, &slot, m, cb, c, timeout_usec);
    sd_bus_message_unref(m);
    if (r < 0) {
        fprintf(stderr, "rpod: bluetooth: %s: %s\n", what, strerror(-r));
        free(c);
        return false;
    }
    /* Floating: the bus owns the slot, which frees `c` once the reply (or
     * the bus) is gone. */
    sd_bus_slot_set_destroy_callback(slot, free);
    sd_bus_slot_set_floating(slot, 1);
    sd_bus_slot_unref(slot);
    return true;
}

/* Returns the reply's error, logging it, or NULL on success. */
static const sd_bus_error *reply_error(sd_bus_message *reply, const call_t *c)
{
    if (!sd_bus_message_is_method_error(reply, NULL)) {
        return NULL;
    }
    const sd_bus_error *e = sd_bus_message_get_error(reply);
    fprintf(stderr, "rpod: bluetooth: %s%s%s: %s (%s)\n", c->what,
            c->path[0] != '\0' ? " " : "", c->path,
            e->message != NULL ? e->message : "", e->name != NULL ? e->name : "");
    return e;
}

static int on_reply_log(sd_bus_message *reply, void *userdata, sd_bus_error *ret_error)
{
    (void)ret_error;
    reply_error(reply, userdata);
    return 0;
}

static bool send_simple(const char *path, const char *iface, const char *method)
{
    return send_call(new_call(path, iface, method), on_reply_log, method, NULL, 0, 0);
}

static void update_discovery(void);

static int on_start_discovery(sd_bus_message *reply, void *userdata, sd_bus_error *ret_error)
{
    (void)ret_error;
    call_t *c = userdata;
    const sd_bus_error *e = reply_error(reply, c);
    /* InProgress means our session is already running -- that's fine. A
     * real failure leaves it off until something else retriggers it (power
     * cycle, reopening the scan screen) rather than retrying in a loop. */
    if (e != NULL && c->gen == g.gen && !sd_bus_error_has_name(e, "org.bluez.Error.InProgress")) {
        g.discovery_active = false;
    }
    return 0;
}

/* Starts or stops our discovery session to match what's wanted. Called
 * after anything that feeds into it changes. */
static void update_discovery(void)
{
    bool want = g.scan_wanted && g.adapter[0] != '\0' && g.powered && g.pairs_in_flight == 0;
    if (want && !g.discovery_active) {
        sd_bus_message *m = new_call(g.adapter, ADAPTER_IFACE, "SetDiscoveryFilter");
        if (m != NULL && sd_bus_message_append(m, "a{sv}", 1, "Transport", "s", "bredr") < 0) {
            sd_bus_message_unref(m);
            m = NULL;
        }
        /* bluetoothd handles one client's calls in order, so the filter is
         * in place before discovery starts without waiting for its reply. */
        send_call(m, on_reply_log, "SetDiscoveryFilter", NULL, 0, 0);
        g.discovery_active = send_call(new_call(g.adapter, ADAPTER_IFACE, "StartDiscovery"),
                                       on_start_discovery, "StartDiscovery", NULL, 0, 0);
    } else if (!want && g.discovery_active) {
        if (g.adapter[0] != '\0' && g.powered) {
            send_simple(g.adapter, ADAPTER_IFACE, "StopDiscovery");
        }
        g.discovery_active = false;
    }
}

/* --- Pairing agent -----------------------------------------------------------
 *
 * NoInputNoOutput, like the headphones it pairs with: there's no screen to
 * show a code on or keyboard to type one into, so Just Works is all it does
 * (the kernel accepts that without asking us), and code requests are
 * refused. What does reach it are confirmations BlueZ wants for a device
 * that isn't trusted, which it grants only while the user is pairing
 * something -- the search screen is open, or a Pair() is running. */

static bool pairing_wanted(void)
{
    return g.scan_wanted || g.pairs_in_flight > 0;
}

static int agent_ok(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
    (void)userdata;
    (void)ret_error;
    return sd_bus_reply_method_return(m, "");
}

static int agent_refuse(sd_bus_message *m, const char *why)
{
    fprintf(stderr, "rpod: bluetooth: agent: refused %s (%s)\n", sd_bus_message_get_member(m), why);
    return sd_bus_reply_method_errorf(m, "org.bluez.Error.Rejected", "%s", why);
}

static int agent_no_code(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
    (void)userdata;
    (void)ret_error;
    return agent_refuse(m, "rPod can't show or enter a code");
}

/* RequestConfirmation (o, u) and RequestAuthorization (o): pairing. */
static int agent_confirm(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
    if (!pairing_wanted()) {
        return agent_refuse(m, "not pairing");
    }
    return agent_ok(m, userdata, ret_error);
}

/* AuthorizeService (o, s): an untrusted device connecting a profile. */
static int agent_authorize_service(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
    const char *path;
    if (sd_bus_message_read_basic(m, 'o', &path) < 0) {
        return agent_refuse(m, "bad request");
    }
    const device_t *d = find_dev(path);
    if (d == NULL || !d->pub.paired || !(d->trusted || pairing_wanted())) {
        return agent_refuse(m, "unknown device");
    }
    return agent_ok(m, userdata, ret_error);
}

/* Unprivileged: the caller is bluetoothd, root but without CAP_SYS_ADMIN,
 * which sd-bus would otherwise want. BlueZ's D-Bus policy is what lets only
 * root send Agent1 calls. */
static const sd_bus_vtable agent_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Release", "", "", agent_ok, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestPinCode", "o", "s", agent_no_code, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("DisplayPinCode", "os", "", agent_no_code, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestPasskey", "o", "u", agent_no_code, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("DisplayPasskey", "ouq", "", agent_ok, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestConfirmation", "ou", "", agent_confirm, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestAuthorization", "o", "", agent_confirm, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("AuthorizeService", "os", "", agent_authorize_service, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Cancel", "", "", agent_ok, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
};

static int on_register_agent(sd_bus_message *reply, void *userdata, sd_bus_error *ret_error)
{
    (void)ret_error;
    call_t *c = userdata;
    /* AlreadyExists: still registered from before on this connection. */
    if (!sd_bus_message_is_method_error(reply, "org.bluez.Error.AlreadyExists") &&
        reply_error(reply, c) != NULL) {
        return 0;
    }
    if (!g.default_agent) {
        return 0;
    }
    sd_bus_message *m = new_call("/org/bluez", AGENT_MANAGER_IFACE, "RequestDefaultAgent");
    if (m != NULL && sd_bus_message_append(m, "o", AGENT_PATH) < 0) {
        sd_bus_message_unref(m);
        m = NULL;
    }
    send_call(m, on_reply_log, "RequestDefaultAgent", NULL, 0, 0);
    return 0;
}

static void register_agent(void)
{
    sd_bus_message *m = new_call("/org/bluez", AGENT_MANAGER_IFACE, "RegisterAgent");
    if (m != NULL && sd_bus_message_append(m, "os", AGENT_PATH, "NoInputNoOutput") < 0) {
        sd_bus_message_unref(m);
        m = NULL;
    }
    send_call(m, on_register_agent, "RegisterAgent", NULL, 0, 0);
}

static int on_objects(sd_bus_message *reply, void *userdata, sd_bus_error *ret_error)
{
    (void)ret_error;
    call_t *c = userdata;
    if (c->gen != g.gen) {
        return 0;
    }
    reset_mirror();
    if (sd_bus_message_is_method_error(reply, NULL)) {
        /* Usually ServiceUnknown: bluetoothd isn't running. Not worth a log
         * line every time -- the screen says so. */
        g.bluez_up = false;
        return 0;
    }
    g.bluez_up = true;
    /* Two passes: object order in the reply isn't guaranteed, and a device
     * is only kept once its adapter is known. */
    int r = parse_objects(reply, true, false);
    if (r >= 0 && g.adapter[0] != '\0') {
        r = sd_bus_message_rewind(reply, 1);
        if (r >= 0) {
            r = parse_objects(reply, false, true);
        }
    }
    if (r < 0) {
        fprintf(stderr, "rpod: bluetooth: GetManagedObjects: bad reply: %s\n", strerror(-r));
    }
    /* bluetoothd (re)started, or we did: it has no agent of ours yet. */
    register_agent();
    if (g.powered) {
        schedule_autoconnect();
    }
    update_discovery();
    return 0;
}

/* Rebuilds the mirror from scratch. The current one stays up (and keeps
 * taking signals) until the reply replaces it, so the screen doesn't flash
 * "no adapter" in between. */
static void fetch_objects(void)
{
    g.gen++;
    send_call(new_call("/", OM_IFACE, "GetManagedObjects"), on_objects, "GetManagedObjects",
              NULL, 0, 0);
}

/* --- Signals ----------------------------------------------------------------- */

static int on_name_owner_changed(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
    (void)userdata;
    (void)ret_error;
    const char *name, *old_owner, *new_owner;
    if (sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner) < 0 ||
        strcmp(name, BLUEZ) != 0) {
        return 0;
    }
    if (new_owner[0] == '\0') {
        reset_mirror();
        g.bluez_up = false;
    } else {
        fetch_objects();
    }
    return 0;
}

static int on_interfaces_added(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
    (void)userdata;
    (void)ret_error;
    const char *path;
    if (!g.bluez_up || sd_bus_message_read_basic(m, 'o', &path) < 0) {
        return 0;
    }
    parse_object(m, path, true, true);
    update_discovery();
    return 0;
}

static int on_interfaces_removed(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
    (void)userdata;
    (void)ret_error;
    const char *path, *iface;
    if (sd_bus_message_read_basic(m, 'o', &path) < 0 ||
        sd_bus_message_enter_container(m, 'a', "s") < 0) {
        return 0;
    }
    while (sd_bus_message_read_basic(m, 's', &iface) > 0) {
        if (strcmp(iface, DEVICE_IFACE) == 0) {
            remove_dev(path);
        } else if (strcmp(iface, TRANSPORT_IFACE) == 0) {
            remove_transport(path);
        } else if (strcmp(iface, ADAPTER_IFACE) == 0 && strcmp(path, g.adapter) == 0) {
            /* Unplugged (or bluetoothd lost it): fall back to whichever
             * adapter is left, if any. */
            reset_mirror();
            fetch_objects();
            return 0;
        }
    }
    return 0;
}

static int on_properties_changed(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
    (void)userdata;
    (void)ret_error;
    const char *path = sd_bus_message_get_path(m);
    const char *iface;
    if (path == NULL || sd_bus_message_read_basic(m, 's', &iface) < 0) {
        return 0;
    }
    if (strcmp(iface, ADAPTER_IFACE) == 0 && strcmp(path, g.adapter) == 0) {
        parse_props(m, adapter_prop, NULL);
        update_discovery();
    } else if (strcmp(iface, DEVICE_IFACE) == 0) {
        device_t *d = find_dev(path);
        if (d != NULL) {
            parse_props(m, device_prop, d);
            update_audio(d);
        }
    } else if (strcmp(iface, TRANSPORT_IFACE) == 0) {
        transport_t *t = find_transport(path);
        transport_update_t u = { .volume = -1, .active = -1 };
        if (t != NULL && parse_props(m, transport_prop, &u) >= 0) {
            apply_transport_update(t, &u);
        }
    }
    return 0;
}

/* --- Bus lifetime ------------------------------------------------------------ */

static void close_bus(void)
{
    if (g.bus != NULL) {
        /* No flush: the connection is broken or never came up, and the UI
         * thread mustn't wait on it. Frees every pending call's slot (and so
         * its call_t) unanswered. */
        g.bus = sd_bus_close_unref(g.bus);
    }
    reset_mirror();
    g.bluez_up = false;
    g.retry_at = lv_tick_get() + RECONNECT_MS;
}

static bool add_match(const char *rule, sd_bus_message_handler_t cb)
{
    /* Async, so a slow bus daemon can't stall the UI. With no install
     * callback, a failed AddMatch closes the connection -- which the poll
     * loop then notices and retries, so that's the error handling too. */
    int r = sd_bus_add_match_async(g.bus, NULL, rule, cb, NULL, NULL);
    if (r < 0) {
        fprintf(stderr, "rpod: bluetooth: AddMatch: %s\n", strerror(-r));
    }
    return r >= 0;
}

static void open_bus(void)
{
    int r = sd_bus_open_system(&g.bus);
    if (r < 0) {
        if (!g.warned_no_bus) {
            fprintf(stderr, "rpod: bluetooth: no system bus (%s); will keep retrying\n",
                    strerror(-r));
            g.warned_no_bus = true;
        }
        g.bus = NULL;
        close_bus();
        return;
    }
    g.warned_no_bus = false;
    /* Matches go in before the snapshot request: the bus daemon handles
     * them in order, so no change can fall between the two. */
    if (!add_match("type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',"
                   "interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='" BLUEZ "'",
                   on_name_owner_changed) ||
        !add_match("type='signal',sender='" BLUEZ "',interface='" OM_IFACE "',"
                   "member='InterfacesAdded'", on_interfaces_added) ||
        !add_match("type='signal',sender='" BLUEZ "',interface='" OM_IFACE "',"
                   "member='InterfacesRemoved'", on_interfaces_removed) ||
        !add_match("type='signal',sender='" BLUEZ "',interface='" PROPS_IFACE "',"
                   "member='PropertiesChanged',path_namespace='/org/bluez'",
                   on_properties_changed)) {
        close_bus();
        return;
    }
    /* Exported once per connection; registered with BlueZ whenever it has
     * an adapter list for us (on_objects), so a bluetoothd restart gets it
     * again. */
    r = sd_bus_add_object_vtable(g.bus, NULL, AGENT_PATH, AGENT_IFACE, agent_vtable, NULL);
    if (r < 0) {
        fprintf(stderr, "rpod: bluetooth: can't export the pairing agent: %s\n", strerror(-r));
    }
    fetch_objects();
}

static void notify_watchers(void)
{
    g.dirty = false;
    /* Re-read the count each pass: a callback is free to add a watcher. */
    for (size_t i = 0; i < g.nwatch; i++) {
        g.watchers[i].cb(g.watchers[i].user);
    }
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    if (g.bus == NULL) {
        if ((int32_t)(lv_tick_get() - g.retry_at) >= 0) {
            open_bus();
        }
    } else {
        for (int i = 0; i < MAX_MSGS_PER_TICK; i++) {
            int r = sd_bus_process(g.bus, NULL);
            if (r == 0) {
                break;
            }
            if (r < 0) {
                fprintf(stderr, "rpod: bluetooth: lost the system bus (%s); reconnecting\n",
                        strerror(-r));
                close_bus();
                break;
            }
        }
    }
    if (g.autoconnect_due && (int32_t)(lv_tick_get() - g.autoconnect_at) >= 0) {
        g.autoconnect_due = false;
        autoconnect_next();
    }
    if (g.volume_steps != 0) {
        int steps = g.volume_steps;
        g.volume_steps = 0;
        if (g.volume_cb != NULL) {
            g.volume_cb(steps, g.volume_user);
        }
    }
    if (g.dirty) {
        notify_watchers();
    }
}

void rpod_bt_init(bool default_agent)
{
    static bool started;
    if (started) {
        return;
    }
    started = true;
    g.default_agent = default_agent;
    const char *debug = getenv("RPOD_BT_DEBUG");
    g.debug = debug != NULL && debug[0] != '\0' && strcmp(debug, "0") != 0;
    open_bus();
    lv_timer_create(poll_cb, POLL_MS, NULL);
}

void rpod_bt_on_volume_buttons(void (*cb)(int steps, void *user), void *user)
{
    g.volume_cb = cb;
    g.volume_user = user;
}

/* --- Queries ----------------------------------------------------------------- */

rpod_bt_state_t rpod_bt_state(void)
{
    if (g.bus == NULL || !g.bluez_up) {
        return RPOD_BT_UNAVAILABLE;
    }
    if (g.adapter[0] == '\0') {
        return RPOD_BT_NO_ADAPTER;
    }
    return g.powered ? RPOD_BT_ON : RPOD_BT_OFF;
}

bool rpod_bt_power_pending(void)
{
    return g.power_pending;
}

bool rpod_bt_discovering(void)
{
    return g.discovering;
}

const char *rpod_bt_adapter_error(void)
{
    if (g.adapter_error[0] != '\0') {
        return g.adapter_error;
    }
    return g.blocked && !g.powered ? "Blocked by rfkill" : "";
}

size_t rpod_bt_device_count(void)
{
    return g.ndev;
}

const rpod_bt_device_t *rpod_bt_device_at(size_t i)
{
    return i < g.ndev ? &g.devs[i].pub : NULL;
}

const rpod_bt_device_t *rpod_bt_find(const char *path)
{
    device_t *d = find_dev(path);
    return d != NULL ? &d->pub : NULL;
}

static void watch_owner_deleted_cb(lv_event_t *e)
{
    lv_obj_t *owner = lv_event_get_current_target(e);
    size_t out = 0;
    for (size_t i = 0; i < g.nwatch; i++) {
        if (g.watchers[i].owner != owner) {
            g.watchers[out++] = g.watchers[i];
        }
    }
    g.nwatch = out;
}

void rpod_bt_watch(lv_obj_t *owner, void (*cb)(void *user), void *user)
{
    if (g.nwatch == g.watchcap) {
        size_t cap = g.watchcap ? g.watchcap * 2 : 4;
        watcher_t *grown = realloc(g.watchers, cap * sizeof(*grown));
        if (grown == NULL) {
            return;
        }
        g.watchers = grown;
        g.watchcap = cap;
    }
    g.watchers[g.nwatch++] = (watcher_t){ .owner = owner, .cb = cb, .user = user };
    if (owner != NULL) {
        lv_obj_add_event_cb(owner, watch_owner_deleted_cb, LV_EVENT_DELETE, NULL);
    }
}

/* --- Actions ----------------------------------------------------------------- */

static int on_set_powered(sd_bus_message *reply, void *userdata, sd_bus_error *ret_error)
{
    (void)ret_error;
    call_t *c = userdata;
    const sd_bus_error *e = reply_error(reply, c);
    if (c->gen != g.gen) {
        return 0;
    }
    g.power_pending = false;
    if (e != NULL) {
        describe_error(e, g.adapter_error, sizeof(g.adapter_error));
    }
    mark_dirty();
    return 0;
}

void rpod_bt_set_powered(bool on)
{
    if (g.adapter[0] == '\0' || g.power_pending) {
        return;
    }
    sd_bus_message *m = new_call(g.adapter, PROPS_IFACE, "Set");
    if (m != NULL && sd_bus_message_append(m, "ssv", ADAPTER_IFACE, "Powered", "b", (int)on) < 0) {
        sd_bus_message_unref(m);
        m = NULL;
    }
    if (send_call(m, on_set_powered, "Set Powered", NULL, 0, 0)) {
        g.power_pending = true;
        g.adapter_error[0] = '\0';
        mark_dirty();
    }
}

void rpod_bt_scan_start(void)
{
    g.scan_wanted = true;
    update_discovery();
}

void rpod_bt_scan_stop(void)
{
    g.scan_wanted = false;
    update_discovery();
}

/* Sets (or clears) `path`'s op marker and error. */
static void set_op(const char *path, rpod_bt_op_t op, const sd_bus_error *e)
{
    device_t *d = find_dev(path);
    if (d == NULL) {
        return;
    }
    d->pub.op = op;
    d->pub.error[0] = '\0';
    if (e != NULL) {
        describe_error(e, d->pub.error, sizeof(d->pub.error));
    }
    mark_dirty();
}

static void finish_pair(void)
{
    if (g.pairs_in_flight > 0) {
        g.pairs_in_flight--;
    }
    update_discovery();
}

static void set_trusted(const char *path)
{
    sd_bus_message *m = new_call(path, PROPS_IFACE, "Set");
    if (m != NULL && sd_bus_message_append(m, "ssv", DEVICE_IFACE, "Trusted", "b", 1) < 0) {
        sd_bus_message_unref(m);
        m = NULL;
    }
    send_call(m, on_reply_log, "Set Trusted", path, 0, 0);
}

static int on_connect(sd_bus_message *reply, void *userdata, sd_bus_error *ret_error)
{
    (void)ret_error;
    call_t *c = userdata;
    const sd_bus_error *e = reply_error(reply, c);
    if (c->gen != g.gen) {
        return 0;
    }
    if (e != NULL && sd_bus_error_has_name(e, "org.bluez.Error.AlreadyConnected")) {
        e = NULL;
    }
    if (c->flags & CALL_AUTO) {
        /* Off, or in a closed case: nobody asked, so nothing to report.
         * Try the next one instead. */
        set_op(c->path, RPOD_BT_OP_NONE, NULL);
        if (e != NULL) {
            autoconnect_next();
        }
        return 0;
    }
    set_op(c->path, RPOD_BT_OP_NONE, e);
    /* Paired some other way (bluetoothctl) without trust: it couldn't
     * reconnect by itself. Now the user's connected it, it can. */
    device_t *d = find_dev(c->path);
    if (e == NULL && d != NULL && d->pub.paired && !d->trusted) {
        set_trusted(c->path);
    }
    if (c->flags & CALL_FROM_PAIR) {
        finish_pair();
    }
    return 0;
}

static bool send_connect(const char *path, unsigned flags)
{
    if (!send_call(new_call(path, DEVICE_IFACE, "Connect"), on_connect, "Connect", path,
                   flags, CONNECT_TIMEOUT_USEC)) {
        return false;
    }
    set_op(path, RPOD_BT_OP_CONNECTING, NULL);
    return true;
}

/* One step of a reconnect round: asks the next paired headphones that could
 * come back by themselves (bonded and trusted) to connect -- AirPods first
 * -- unless some audio device already has. A failure moves on to the next
 * (see on_connect); success, or running out, ends the round. */
static void autoconnect_next(void)
{
    if (rpod_bt_state() != RPOD_BT_ON || g.pairs_in_flight > 0) {
        return;
    }
    for (size_t i = 0; i < g.ndev; i++) {
        if (g.devs[i].pub.audio && g.devs[i].pub.connected) {
            return;
        }
    }
    device_t *pick = NULL;
    for (int pass = 0; pass < 2 && pick == NULL; pass++) {
        for (size_t i = 0; i < g.ndev; i++) {
            device_t *d = &g.devs[i];
            if (d->autoconnect_tried || !d->pub.paired || !d->pub.bonded || !d->trusted ||
                !d->pub.audio || d->pub.connected || d->pub.op != RPOD_BT_OP_NONE ||
                (pass == 0 && !d->pub.aap)) {
                continue;
            }
            pick = d;
            break;
        }
    }
    if (pick == NULL) {
        return;
    }
    pick->autoconnect_tried = true;
    fprintf(stderr, "rpod: bluetooth: reconnecting %s\n", pick->pub.name);
    send_connect(pick->pub.path, CALL_AUTO);
}

static int on_pair(sd_bus_message *reply, void *userdata, sd_bus_error *ret_error)
{
    (void)ret_error;
    call_t *c = userdata;
    const sd_bus_error *e = reply_error(reply, c);
    if (c->gen != g.gen) {
        return 0;
    }
    /* AlreadyExists: paired already (e.g. by bluetoothctl) -- carry on. */
    if (e != NULL && !sd_bus_error_has_name(e, "org.bluez.Error.AlreadyExists")) {
        set_op(c->path, RPOD_BT_OP_NONE, e);
        finish_pair();
        return 0;
    }
    /* Trusted lets the device reconnect by itself later (and spares it an
     * authorization prompt nobody's there to answer). Fire-and-forget: a
     * failure here doesn't stop this connection. */
    set_trusted(c->path);
    if (!send_connect(c->path, CALL_FROM_PAIR)) {
        set_op(c->path, RPOD_BT_OP_NONE, NULL);
        finish_pair();
    }
    return 0;
}

static bool device_idle(const char *path)
{
    device_t *d = find_dev(path);
    return d != NULL && d->pub.op == RPOD_BT_OP_NONE;
}

void rpod_bt_pair(const char *path)
{
    if (!device_idle(path)) {
        return;
    }
    g.pairs_in_flight++;
    update_discovery(); /* pauses it */
    if (send_call(new_call(path, DEVICE_IFACE, "Pair"), on_pair, "Pair", path, 0,
                  PAIR_TIMEOUT_USEC)) {
        set_op(path, RPOD_BT_OP_PAIRING, NULL);
    } else {
        finish_pair();
    }
}

void rpod_bt_connect(const char *path)
{
    if (device_idle(path)) {
        send_connect(path, 0);
    }
}

static int on_disconnect(sd_bus_message *reply, void *userdata, sd_bus_error *ret_error)
{
    (void)ret_error;
    call_t *c = userdata;
    const sd_bus_error *e = reply_error(reply, c);
    if (c->gen == g.gen) {
        set_op(c->path, RPOD_BT_OP_NONE, e);
    }
    return 0;
}

void rpod_bt_disconnect(const char *path)
{
    if (device_idle(path) &&
        send_call(new_call(path, DEVICE_IFACE, "Disconnect"), on_disconnect, "Disconnect", path,
                  0, CONNECT_TIMEOUT_USEC)) {
        set_op(path, RPOD_BT_OP_DISCONNECTING, NULL);
    }
}

void rpod_bt_forget(const char *path)
{
    if (find_dev(path) == NULL) {
        return;
    }
    sd_bus_message *m = new_call(g.adapter, ADAPTER_IFACE, "RemoveDevice");
    if (m != NULL && sd_bus_message_append(m, "o", path) < 0) {
        sd_bus_message_unref(m);
        m = NULL;
    }
    /* The device leaves the mirror when BlueZ says InterfacesRemoved. */
    send_call(m, on_reply_log, "RemoveDevice", path, 0, 0);
}
