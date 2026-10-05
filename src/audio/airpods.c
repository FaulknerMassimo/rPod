#include "airpods.h"

#include "aap.h"
#include "bluetooth.h"
#include "volume_memory.h"

#include <endian.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define POLL_MS            100
/* Lets BlueZ finish bringing up A2DP/AVRCP before another channel opens. */
#define CONNECT_DELAY_MS   1500
#define CONNECT_TIMEOUT_MS 10000
/* Gap between the setup packets -- handshake, features, notifications. */
#define SETUP_STEP_MS      300
#define RETRY_MIN_MS       2000
#define RETRY_MAX_MS       30000
#define TX_QUEUE           16
/* A send that keeps failing (ENOTCONN while BlueZ is still finishing the
 * channel, EAGAIN) this long drops the link. */
#define TX_STALL_MS        1000
/* How often to look for the user taking over playback while an ear-detection
 * pause is pending. */
#define STATE_POLL_MS      1000
/* system/mpd/mpd.conf's output for Bluetooth audio. */
#define BT_OUTPUT_NAME     "Bluetooth"
/* rpod_airpods_t.path while talking to RPOD_AIRPODS_SOCK. */
#define FAKE_PATH          "/rpod/fake_airpods"

/* The kernel's L2CAP socket address (<bluetooth/l2cap.h>, libbluetooth-dev),
 * restated rather than pulling in BlueZ's library headers for one struct. */
#define BTPROTO_L2CAP 0
#define BDADDR_BREDR  0x00
struct l2cap_addr {
    sa_family_t family;
    uint16_t psm;      /* little-endian */
    uint8_t bdaddr[6]; /* little-endian: FF first for AA:BB:CC:DD:EE:FF */
    uint16_t cid;
    uint8_t bdaddr_type;
};

typedef enum {
    LINK_IDLE,       /* no AAP device connected */
    LINK_WAIT,       /* one is; connecting at next_ms */
    LINK_CONNECTING, /* connect() in progress */
    LINK_SETUP,      /* channel open; sending the setup packets */
    LINK_READY,
} link_t;

typedef struct {
    lv_obj_t *owner;
    void (*cb)(void *user);
    void *user;
} watcher_t;

static struct {
    rpod_mpd_t *mpd;
    char sock_path[108]; /* RPOD_AIRPODS_SOCK; "" = BlueZ */
    bool debug;

    link_t link;
    int fd;
    char address[18];
    uint32_t next_ms;     /* LINK_WAIT: when to connect; LINK_SETUP: next step */
    uint32_t deadline_ms; /* LINK_CONNECTING: when to give up */
    uint32_t retry_ms;
    int setup_step;

    uint8_t txq[TX_QUEUE][RPOD_AAP_MAX_PACKET];
    size_t txlen[TX_QUEUE];
    size_t txhead, txcount;
    bool tx_stalled;
    uint32_t tx_stalled_since;

    rpod_airpods_t pub;
    uint8_t ctl[256];
    bool ctl_known[256];

    int in_ear;          /* buds in ear at the last report; -1 = none yet */
    bool paused_by_ear;
    int resume_at;       /* buds in ear that resume what ear detection paused */
    uint32_t state_poll_ms;
    bool ducked;         /* Conversation Awareness has the volume lowered */

    bool dirty;
    watcher_t *watchers;
    size_t nwatch, watchcap;
} g = { .fd = -1 };

static void mark_dirty(void)
{
    g.dirty = true;
}

static void log_packet(const char *dir, const uint8_t *pkt, size_t len)
{
    char hex[3 * 64 + 4];
    size_t n = 0;
    for (size_t i = 0; i < len && i < 64; i++) {
        n += (size_t)snprintf(hex + n, sizeof(hex) - n, "%02x ", pkt[i]);
    }
    if (len > 64) {
        snprintf(hex + n, sizeof(hex) - n, "...");
    }
    fprintf(stderr, "rpod: airpods: %s %s\n", dir, hex);
}

static void unduck(void)
{
    if (g.ducked) {
        rpod_volume_memory_duck(100);
        g.ducked = false;
    }
}

/* Forgets everything the AirPods have told us. */
static void reset_session(void)
{
    const rpod_airpods_battery_t none = { .level = -1 };
    g.pub.left = g.pub.right = g.pub.charging_case = g.pub.headset = none;
    g.pub.ear[0] = g.pub.ear[1] = -1;
    g.pub.model[0] = g.pub.serial[0] = g.pub.firmware[0] = '\0';
    memset(g.ctl_known, 0, sizeof(g.ctl_known));
    g.txcount = 0;
    g.tx_stalled = false;
    g.in_ear = -1;
    g.paused_by_ear = false;
    unduck();
    mark_dirty();
}

static void close_link(void)
{
    if (g.fd >= 0) {
        close(g.fd);
        g.fd = -1;
    }
}

/* Drops the control channel and tries again later -- the device is still
 * connected (or we'd be LINK_IDLE). */
static void fail(const char *why)
{
    fprintf(stderr, "rpod: airpods: %s: %s; retrying in %us\n",
            g.pub.name[0] != '\0' ? g.pub.name : g.pub.path, why, (unsigned)(g.retry_ms / 1000));
    snprintf(g.pub.error, sizeof(g.pub.error), "%s", why);
    close_link();
    reset_session();
    g.link = LINK_WAIT;
    g.pub.link = RPOD_AIRPODS_CONNECTING;
    g.next_ms = lv_tick_get() + g.retry_ms;
    g.retry_ms = g.retry_ms * 2 > RETRY_MAX_MS ? RETRY_MAX_MS : g.retry_ms * 2;
}

/* --- Sending ----------------------------------------------------------------- */

static void flush_tx(void)
{
    while (g.txcount > 0 && g.fd >= 0) {
        const uint8_t *pkt = g.txq[g.txhead];
        size_t len = g.txlen[g.txhead];
        if (send(g.fd, pkt, len, MSG_NOSIGNAL) < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOTCONN) {
                if (!g.tx_stalled) {
                    g.tx_stalled = true;
                    g.tx_stalled_since = lv_tick_get();
                } else if (lv_tick_elaps(g.tx_stalled_since) > TX_STALL_MS) {
                    fail("Not accepting commands");
                }
                return;
            }
            fail(strerror(errno));
            return;
        }
        if (g.debug) {
            log_packet("tx", pkt, len);
        }
        g.tx_stalled = false;
        g.txhead = (g.txhead + 1) % TX_QUEUE;
        g.txcount--;
    }
}

static void send_packet(const uint8_t *pkt, size_t len)
{
    if (g.fd < 0 || g.txcount == TX_QUEUE) {
        return;
    }
    size_t slot = (g.txhead + g.txcount) % TX_QUEUE;
    memcpy(g.txq[slot], pkt, len);
    g.txlen[slot] = len;
    g.txcount++;
    flush_tx();
}

static void send_control(uint8_t id, uint8_t value)
{
    uint8_t pkt[RPOD_AAP_MAX_PACKET];
    send_packet(pkt, rpod_aap_build_control(pkt, id, value));
}

/* --- What the AirPods report --------------------------------------------------- */

/* Unknown counts as on: the AirPods report every setting they have, so one
 * they don't is one they don't let you turn off. */
static bool setting_on(uint8_t id)
{
    return !g.ctl_known[id] || g.ctl[id] == RPOD_AAP_ON;
}

static void on_control(uint8_t id, uint8_t value)
{
    if (!g.ctl_known[id] || g.ctl[id] != value) {
        g.ctl[id] = value;
        g.ctl_known[id] = true;
        mark_dirty();
    }
    if (id == RPOD_AAP_CTL_CONVERSATION && value != RPOD_AAP_ON) {
        unduck();
    }
}

static void on_battery(const rpod_aap_event_t *ev)
{
    for (size_t i = 0; i < ev->battery.count; i++) {
        rpod_airpods_battery_t *b;
        switch (ev->battery.items[i].component) {
        case RPOD_AAP_BATT_LEFT:   b = &g.pub.left; break;
        case RPOD_AAP_BATT_RIGHT:  b = &g.pub.right; break;
        case RPOD_AAP_BATT_CASE:   b = &g.pub.charging_case; break;
        case RPOD_AAP_BATT_SINGLE: b = &g.pub.headset; break;
        default: continue;
        }
        uint8_t status = ev->battery.items[i].status;
        rpod_airpods_battery_t now = {
            .level = status == RPOD_AAP_BATT_DISCONNECTED ? -1 : ev->battery.items[i].level,
            .charging = status == RPOD_AAP_BATT_CHARGING,
        };
        if (now.level != b->level || now.charging != b->charging) {
            *b = now;
            mark_dirty();
        }
    }
}

/* True unless MPD's Bluetooth output exists and is off -- the music is going
 * to the headphone jack, which a bud coming out shouldn't pause. A config
 * without one (the sim's) counts as Bluetooth. */
static bool playing_through_bluetooth(void)
{
    rpod_mpd_output_t *outputs = NULL;
    size_t count = 0;
    if (!rpod_mpd_list_outputs(g.mpd, &outputs, &count)) {
        return true;
    }
    bool on = true;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(outputs[i].name, BT_OUTPUT_NAME) == 0) {
            on = outputs[i].enabled;
            break;
        }
    }
    rpod_mpd_free_outputs(outputs);
    return on;
}

/* iPhone-style: a bud coming out pauses, and it going back in resumes --
 * but only what this paused, and only once as many buds are in as when it
 * did. Wearing one bud works the same way with a count of one. */
static void on_ear(uint8_t primary, uint8_t secondary)
{
    if (g.pub.ear[0] != primary || g.pub.ear[1] != secondary) {
        g.pub.ear[0] = primary;
        g.pub.ear[1] = secondary;
        mark_dirty();
    }
    int in = (primary == RPOD_AAP_EAR_IN) + (secondary == RPOD_AAP_EAR_IN);
    int before = g.in_ear;
    g.in_ear = in;
    if (in == 0) {
        /* Taken off mid-conversation: no "back to normal" level may come. */
        unduck();
    }
    if (before < 0 || in == before || !setting_on(RPOD_AAP_CTL_EAR_DETECTION)) {
        return;
    }

    rpod_mpd_state_t state;
    if (in < before) {
        if (!g.paused_by_ear && playing_through_bluetooth() &&
            rpod_mpd_get_state(g.mpd, &state) && state == RPOD_MPD_STATE_PLAY &&
            rpod_mpd_set_paused(g.mpd, true)) {
            g.paused_by_ear = true;
            g.resume_at = before;
            g.state_poll_ms = lv_tick_get();
            fprintf(stderr, "rpod: airpods: paused (%d of %d buds in ear)\n", in, before);
        }
    } else if (g.paused_by_ear && in >= g.resume_at) {
        g.paused_by_ear = false;
        if (rpod_mpd_get_state(g.mpd, &state) && state == RPOD_MPD_STATE_PAUSE) {
            rpod_mpd_set_paused(g.mpd, false);
            fprintf(stderr, "rpod: airpods: resumed (%d buds in ear)\n", in);
        }
    }
}

/* Levels 1-2 duck hard, 3-7 ease back up, 8-9 are normal again. */
static void on_conversation(uint8_t level)
{
    if (!setting_on(RPOD_AAP_CTL_CONVERSATION)) {
        return;
    }
    unsigned percent = level <= 2 ? 20 : level >= 8 ? 100 : 20 + (unsigned)(level - 2) * 80 / 6;
    if (g.debug || percent == 20 || (g.ducked && percent == 100)) {
        fprintf(stderr, "rpod: airpods: conversation level %u -> %u%% volume\n", level, percent);
    }
    rpod_volume_memory_duck(percent);
    g.ducked = percent < 100;
}

static void on_press(uint8_t type)
{
    switch (type) {
    case RPOD_AAP_PRESS_SINGLE: rpod_mpd_toggle_pause(g.mpd); break;
    case RPOD_AAP_PRESS_DOUBLE: rpod_mpd_next(g.mpd); break;
    case RPOD_AAP_PRESS_TRIPLE: rpod_mpd_previous(g.mpd); break;
    default: break;
    }
}

static void handle_packet(const uint8_t *pkt, size_t len)
{
    if (g.debug) {
        log_packet("rx", pkt, len);
    }
    rpod_aap_event_t ev;
    if (!rpod_aap_parse(pkt, len, &ev)) {
        return;
    }
    switch (ev.type) {
    case RPOD_AAP_EV_BATTERY:
        on_battery(&ev);
        break;
    case RPOD_AAP_EV_EAR:
        on_ear(ev.ear.primary, ev.ear.secondary);
        break;
    case RPOD_AAP_EV_CONTROL:
        on_control(ev.control.id, ev.control.value[0]);
        break;
    case RPOD_AAP_EV_CONVERSATION:
        on_conversation(ev.conversation_level);
        break;
    case RPOD_AAP_EV_INFO:
        snprintf(g.pub.model, sizeof(g.pub.model), "%s", ev.info.model);
        snprintf(g.pub.serial, sizeof(g.pub.serial), "%s", ev.info.serial);
        snprintf(g.pub.firmware, sizeof(g.pub.firmware), "%s", ev.info.firmware);
        /* BlueZ's alias (what the user named them) wins when there is one. */
        if (g.pub.name[0] == '\0') {
            snprintf(g.pub.name, sizeof(g.pub.name), "%s", ev.info.name);
        }
        mark_dirty();
        break;
    case RPOD_AAP_EV_PRESS:
        on_press(ev.press.type);
        break;
    case RPOD_AAP_EV_NONE:
        break;
    }
}

static void poll_rx(void)
{
    uint8_t buf[1024];
    for (int i = 0; i < 32 && g.fd >= 0; i++) {
        ssize_t n = recv(g.fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n == 0) {
            fail("Control channel closed");
            return;
        }
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                fail(strerror(errno));
            }
            return;
        }
        handle_packet(buf, (size_t)n);
    }
}

/* --- Connection -------------------------------------------------------------- */

static bool parse_address(const char *s, uint8_t out[6])
{
    unsigned b[6];
    if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        out[i] = (uint8_t)b[5 - i];
    }
    return true;
}

static void begin_setup(void)
{
    g.link = LINK_SETUP;
    g.setup_step = 0;
    g.next_ms = lv_tick_get();
    g.pub.error[0] = '\0';
    mark_dirty();
}

static void start_connect(void)
{
    int fd;
    int r;
    if (g.sock_path[0] != '\0') {
        fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        struct sockaddr_un sa = { .sun_family = AF_UNIX };
        snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", g.sock_path);
        r = fd < 0 ? -1 : connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    } else {
        struct l2cap_addr la = {
            .family = AF_BLUETOOTH,
            .psm = htole16(RPOD_AAP_PSM),
            .bdaddr_type = BDADDR_BREDR,
        };
        if (!parse_address(g.address, la.bdaddr)) {
            fail("No Bluetooth address");
            return;
        }
        fd = socket(AF_BLUETOOTH, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, BTPROTO_L2CAP);
        r = fd < 0 ? -1 : connect(fd, (struct sockaddr *)&la, sizeof(la));
    }
    if (r < 0 && errno != EINPROGRESS) {
        const char *why = strerror(errno);
        if (fd >= 0) {
            close(fd);
        }
        fail(why);
        return;
    }
    g.fd = fd;
    if (r == 0) {
        begin_setup();
    } else {
        g.link = LINK_CONNECTING;
        g.deadline_ms = lv_tick_get() + CONNECT_TIMEOUT_MS;
    }
}

static void check_connect(void)
{
    struct pollfd p = { .fd = g.fd, .events = POLLOUT };
    if (poll(&p, 1, 0) > 0) {
        int err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(g.fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
            err = errno;
        }
        if (err != 0) {
            fail(strerror(err));
        } else {
            begin_setup();
        }
    } else if ((int32_t)(lv_tick_get() - g.deadline_ms) >= 0) {
        fail("Not responding");
    }
}

/* Handshake, then feature flags, then the notification request, a beat
 * apart; the AirPods answer the last with their battery, in-ear state and
 * every setting. Stem presses are claimed then too: the AirPods would send
 * them as AVRCP otherwise, which nothing on rPod listens to. */
static void setup_step(void)
{
    uint8_t pkt[RPOD_AAP_MAX_PACKET];
    switch (g.setup_step++) {
    case 0:
        send_packet(pkt, rpod_aap_build_handshake(pkt));
        break;
    case 1:
        send_packet(pkt, rpod_aap_build_features(pkt));
        break;
    default:
        send_packet(pkt, rpod_aap_build_notifications(pkt));
        send_control(RPOD_AAP_CTL_RAW_GESTURES,
                     RPOD_AAP_GESTURE_SINGLE | RPOD_AAP_GESTURE_DOUBLE | RPOD_AAP_GESTURE_TRIPLE);
        if (g.fd >= 0) {
            g.link = LINK_READY;
            g.pub.link = RPOD_AIRPODS_READY;
            g.retry_ms = RETRY_MIN_MS;
            fprintf(stderr, "rpod: airpods: %s: controls up\n", g.pub.name);
            mark_dirty();
        }
        return;
    }
    if (g.link == LINK_SETUP) { /* not if that send failed -- fail() set the retry */
        g.next_ms = lv_tick_get() + SETUP_STEP_MS;
    }
}

/* Starts following `path` (with its address), or stops for "". */
static void set_target(const char *path, const char *address)
{
    if (strcmp(path, g.pub.path) == 0) {
        return;
    }
    close_link();
    reset_session();
    snprintf(g.pub.path, sizeof(g.pub.path), "%s", path);
    snprintf(g.address, sizeof(g.address), "%s", address);
    g.pub.error[0] = '\0';
    g.pub.name[0] = '\0';
    if (path[0] == '\0') {
        g.link = LINK_IDLE;
        g.pub.link = RPOD_AIRPODS_ABSENT;
    } else {
        g.link = LINK_WAIT;
        g.pub.link = RPOD_AIRPODS_CONNECTING;
        g.next_ms = lv_tick_get() + CONNECT_DELAY_MS;
        g.retry_ms = RETRY_MIN_MS;
    }
}

/* The connected AAP device to follow: the current one while it stays
 * connected, else the first one found. */
static void bt_changed_cb(void *user)
{
    (void)user;
    if (g.sock_path[0] != '\0') {
        return;
    }
    const rpod_bt_device_t *pick = NULL;
    for (size_t i = 0; i < rpod_bt_device_count(); i++) {
        const rpod_bt_device_t *d = rpod_bt_device_at(i);
        if (!d->connected || !d->aap || d->address[0] == '\0') {
            continue;
        }
        if (pick == NULL || strcmp(d->path, g.pub.path) == 0) {
            pick = d;
        }
    }
    if (pick == NULL) {
        set_target("", "");
        return;
    }
    set_target(pick->path, pick->address);
    if (strcmp(pick->name, g.pub.name) != 0) {
        snprintf(g.pub.name, sizeof(g.pub.name), "%s", pick->name);
        mark_dirty();
    }
}

/* --- Timer, API ------------------------------------------------------------------ */

static void notify_watchers(void)
{
    g.dirty = false;
    for (size_t i = 0; i < g.nwatch; i++) {
        g.watchers[i].cb(g.watchers[i].user);
    }
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    switch (g.link) {
    case LINK_IDLE:
        break;
    case LINK_WAIT:
        if ((int32_t)(lv_tick_get() - g.next_ms) >= 0) {
            start_connect();
        }
        break;
    case LINK_CONNECTING:
        check_connect();
        break;
    case LINK_SETUP:
    case LINK_READY:
        flush_tx();
        poll_rx();
        if (g.link == LINK_SETUP && (int32_t)(lv_tick_get() - g.next_ms) >= 0) {
            setup_step();
        }
        break;
    }

    /* The user resumed (or stopped) by hand: a bud going back in mustn't
     * then resume anything. */
    rpod_mpd_state_t state;
    if (g.paused_by_ear && lv_tick_elaps(g.state_poll_ms) >= STATE_POLL_MS) {
        g.state_poll_ms = lv_tick_get();
        if (rpod_mpd_get_state(g.mpd, &state) && state != RPOD_MPD_STATE_PAUSE) {
            g.paused_by_ear = false;
        }
    }

    if (g.dirty) {
        notify_watchers();
    }
}

void rpod_airpods_init(rpod_mpd_t *mpd)
{
    static bool started;
    if (started) {
        return;
    }
    started = true;
    g.mpd = mpd;
    reset_session();
    const char *debug = getenv("RPOD_AIRPODS_DEBUG");
    g.debug = debug != NULL && debug[0] != '\0' && strcmp(debug, "0") != 0;

    const char *sock = getenv("RPOD_AIRPODS_SOCK");
    if (sock != NULL && sock[0] != '\0') {
        if (strlen(sock) >= sizeof(g.sock_path)) {
            fprintf(stderr, "rpod: airpods: RPOD_AIRPODS_SOCK is too long for a socket path\n");
        }
        snprintf(g.sock_path, sizeof(g.sock_path), "%s", sock);
        set_target(FAKE_PATH, "");
        fprintf(stderr, "rpod: airpods: using %s instead of Bluetooth\n", g.sock_path);
    } else {
        rpod_bt_watch(NULL, bt_changed_cb, NULL);
    }
    lv_timer_create(poll_cb, POLL_MS, NULL);
}

const rpod_airpods_t *rpod_airpods(void)
{
    return &g.pub;
}

bool rpod_airpods_get(uint8_t id, uint8_t *value)
{
    if (!g.ctl_known[id]) {
        return false;
    }
    *value = g.ctl[id];
    return true;
}

void rpod_airpods_set(uint8_t id, uint8_t value)
{
    if (g.link != LINK_READY) {
        return;
    }
    send_control(id, value);
    if (g.link == LINK_READY) {
        on_control(id, value);
    }
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

void rpod_airpods_watch(lv_obj_t *owner, void (*cb)(void *user), void *user)
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
