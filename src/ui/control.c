#include "control.h"

#include "alarm.h"
#include "backlight.h"
#include "shutdown.h"
#include "sleep_timer.h"
#include "theme.h"
#include "input/wheel_tuning.h"
#include "power/battery.h"
#include "video/av_sync.h"

#include "lvgl.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/* How often the socket is looked at: how long a request can wait. */
#define POLL_MS 50

/* Requests in flight at once; more wait in the listen backlog. */
#define MAX_CLIENTS 4

/* A request is one short line. */
#define REQ_MAX 512

/* A client that hasn't sent its line by then is dropped. */
#define CLIENT_TIMEOUT_MS 2000

/* `get`'s answer, with room to spare. */
#define REPLY_MAX 4096

/* Longest Sleep Timer the web UI may set: the device's own choices stop at
 * two hours. */
#define MAX_SLEEP_MIN 720u

/* Longest Auto-Off: a day. */
#define MAX_SHUTDOWN_MIN 1440u

typedef struct {
    int fd; /* -1: free */
    char buf[REQ_MAX];
    size_t len;
    uint32_t since;
} client_t;

static struct {
    int listen_fd;
    client_t clients[MAX_CLIENTS];
    rpod_lb_t *lb;
    char lb_token_path[PATH_MAX];
    const char *(*play_video)(const char *name, void *ctx);
    void *play_video_ctx;
} g = {
    .listen_fd = -1,
};

typedef struct {
    char s[REPLY_MAX];
    size_t len;
} reply_t;

static void say(reply_t *r, const char *fmt, ...)
{
    if (r->len >= sizeof(r->s)) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(r->s + r->len, sizeof(r->s) - r->len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        r->len += (size_t)n;
        if (r->len > sizeof(r->s)) {
            r->len = sizeof(r->s);
        }
    }
}

static void do_get(reply_t *r)
{
    say(r, "ok\n");
    say(r, "theme=%s\n", rpod_theme()->key);
    say(r, "backlight=%u\n", rpod_backlight_timeout_s());
    say(r, "sleep=%u\n", rpod_sleep_timer_minutes());
    say(r, "shutdown=%u\n", rpod_shutdown_idle_min());
    say(r, "sleep_remaining_ms=%u\n", (unsigned)rpod_sleep_timer_remaining_ms());

    int h, m;
    rpod_alarm_time(&h, &m);
    say(r, "alarm=%s\n", rpod_alarm_enabled() ? "on" : "off");
    say(r, "alarm_time=%02d:%02d\n", h, m);
    say(r, "alarm_sound=%s\n", rpod_alarm_sound());
    say(r, "alarm_ringing=%d\n", rpod_alarm_ringing() ? 1 : 0);

    size_t count;
    const rpod_wheel_param_t *params = rpod_wheel_tuning_params(&count);
    for (size_t i = 0; i < count; i++) {
        float v, reset;
        rpod_wheel_tuning_get(params[i].key, &v, &reset);
        say(r, "wheel.%s=%g\n", params[i].key, (double)v);
        say(r, "wheel.%s.reset=%g\n", params[i].key, (double)reset);
        say(r, "wheel.%s.min=%g\n", params[i].key, (double)params[i].min);
        say(r, "wheel.%s.max=%g\n", params[i].key, (double)params[i].max);
    }

    say(r, "video_sync=%d\n", rpod_av_sync_trim_ms());
    say(r, "listenbrainz=%s\n", rpod_lb_enabled(g.lb) ? "on" : "off");

    /* Read-only: the web UI's title bar and About show it. */
    const rpod_battery_t *b = rpod_battery();
    if (b->present) {
        say(r, "battery=%d\n", b->level);
        say(r, "battery_state=%s\n", b->charging ? "charging" : b->external ? "charged" : "discharging");
        say(r, "battery_mv=%d\n", b->mv);
    }
}

static bool parse_uint(const char *s, unsigned max, unsigned *out)
{
    char *end;
    errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (end == s || *end != '\0' || errno != 0 || v > max || s[0] == '-') {
        return false;
    }
    *out = (unsigned)v;
    return true;
}

static void do_set(reply_t *r, const char *key, const char *value)
{
    unsigned u;

    if (strcmp(key, "theme") == 0) {
        rpod_theme_id_t id;
        if (!rpod_theme_find(value, &id)) {
            say(r, "err unknown theme\n");
            return;
        }
        rpod_theme_set(id); /* rebuilds the UI from the next timer cycle */
        say(r, "ok\n");
        return;
    }
    if (strcmp(key, "backlight") == 0) {
        if (!parse_uint(value, 3600, &u)) {
            say(r, "err backlight is 0-3600 seconds\n");
            return;
        }
        rpod_backlight_set_timeout_s(u);
        say(r, "ok\n");
        return;
    }
    if (strcmp(key, "shutdown") == 0) {
        if (!parse_uint(value, MAX_SHUTDOWN_MIN, &u)) {
            say(r, "err shutdown is 0-%u minutes\n", MAX_SHUTDOWN_MIN);
            return;
        }
        rpod_shutdown_set_idle_min(u);
        say(r, "ok\n");
        return;
    }
    if (strcmp(key, "sleep") == 0) {
        if (!parse_uint(value, MAX_SLEEP_MIN, &u)) {
            say(r, "err sleep is 0-%u minutes\n", MAX_SLEEP_MIN);
            return;
        }
        rpod_sleep_timer_set(u);
        say(r, "ok\n");
        return;
    }
    if (strcmp(key, "alarm") == 0) {
        if (strcmp(value, "on") != 0 && strcmp(value, "off") != 0) {
            say(r, "err alarm is on or off\n");
            return;
        }
        rpod_alarm_set_enabled(strcmp(value, "on") == 0);
        say(r, "ok\n");
        return;
    }
    if (strcmp(key, "alarm_time") == 0) {
        int h, m;
        char tail;
        if (sscanf(value, "%2d:%2d%c", &h, &m, &tail) != 2 || h < 0 || h > 23 || m < 0 || m > 59) {
            say(r, "err alarm_time is HH:MM\n");
            return;
        }
        rpod_alarm_set_time(h, m);
        say(r, "ok\n");
        return;
    }
    if (strcmp(key, "alarm_sound") == 0) {
        rpod_alarm_set_sound(value);
        say(r, "ok\n");
        return;
    }
    if (strncmp(key, "wheel.", 6) == 0) {
        char *end;
        float f = strtof(value, &end);
        if (end == value || *end != '\0' || !rpod_wheel_tuning_set(key + 6, f)) {
            say(r, "err not a value for %s\n", key);
            return;
        }
        say(r, "ok\n");
        return;
    }
    if (strcmp(key, "video_sync") == 0) {
        char *end;
        long ms = strtol(value, &end, 10);
        if (end == value || *end != '\0' || ms < -RPOD_AV_SYNC_TRIM_MAX || ms > RPOD_AV_SYNC_TRIM_MAX) {
            say(r, "err video_sync is %d to %d ms\n", -RPOD_AV_SYNC_TRIM_MAX, RPOD_AV_SYNC_TRIM_MAX);
            return;
        }
        rpod_av_sync_set_trim_ms((int)ms);
        say(r, "ok\n");
        return;
    }
    if (strcmp(key, "listenbrainz_token") == 0) {
        if (!rpod_lb_set_token(g.lb, value)) {
            say(r, "err that isn't a ListenBrainz token\n");
            return;
        }
        if (g.lb_token_path[0] != '\0' && !rpod_lb_save_token(g.lb_token_path, value)) {
            say(r, "err couldn't save the token\n");
            return;
        }
        say(r, "ok\n");
        return;
    }
    say(r, "err unknown setting\n");
}

static void handle(const char *line, reply_t *r)
{
    if (strcmp(line, "get") == 0) {
        do_get(r);
        return;
    }
    if (strcmp(line, "active") == 0) {
        rpod_shutdown_activity();
        say(r, "ok\n");
        return;
    }
    if (strcmp(line, "reset wheel") == 0) {
        rpod_wheel_tuning_reset();
        say(r, "ok\n");
        return;
    }
    if (strncmp(line, "play video ", 11) == 0) {
        const char *why = g.play_video != NULL ? g.play_video(line + 11, g.play_video_ctx) : "no videos here";
        if (why != NULL) {
            say(r, "err %s\n", why);
            return;
        }
        say(r, "ok\n");
        return;
    }
    if (strncmp(line, "set ", 4) == 0) {
        char key[64];
        const char *rest = line + 4;
        size_t klen = strcspn(rest, " ");
        if (klen == 0 || klen >= sizeof(key)) {
            say(r, "err bad request\n");
            return;
        }
        memcpy(key, rest, klen);
        key[klen] = '\0';
        const char *value = rest[klen] == ' ' ? rest + klen + 1 : "";
        do_set(r, key, value);
        return;
    }
    say(r, "err bad request\n");
}

static void drop(client_t *c)
{
    close(c->fd);
    c->fd = -1;
}

/* Reads what's there; answers once the line is complete. */
static void serve(client_t *c)
{
    for (;;) {
        ssize_t n = recv(c->fd, c->buf + c->len, sizeof(c->buf) - 1 - c->len, MSG_DONTWAIT);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (lv_tick_elaps(c->since) > CLIENT_TIMEOUT_MS) {
                drop(c);
            }
            return;
        }
        if (n <= 0) {
            drop(c);
            return;
        }
        c->len += (size_t)n;
        c->buf[c->len] = '\0';
        char *nl = strchr(c->buf, '\n');
        if (nl == NULL) {
            if (c->len == sizeof(c->buf) - 1) {
                drop(c); /* too long to be a request */
                return;
            }
            continue;
        }
        *nl = '\0';
        if (nl > c->buf && nl[-1] == '\r') {
            nl[-1] = '\0';
        }
        reply_t r = { .len = 0 };
        handle(c->buf, &r);
        /* Small enough for the socket's buffer: one non-blocking send. */
        if (send(c->fd, r.s, r.len, MSG_DONTWAIT | MSG_NOSIGNAL) < 0) {
            fprintf(stderr, "rpod: control reply failed: %s\n", strerror(errno));
        }
        drop(c);
        return;
    }
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    for (;;) {
        client_t *free_slot = NULL;
        for (int i = 0; i < MAX_CLIENTS && free_slot == NULL; i++) {
            if (g.clients[i].fd < 0) {
                free_slot = &g.clients[i];
            }
        }
        if (free_slot == NULL) {
            break;
        }
        int fd = accept(g.listen_fd, NULL, NULL);
        if (fd < 0) {
            break;
        }
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        free_slot->fd = fd;
        free_slot->len = 0;
        free_slot->since = lv_tick_get();
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g.clients[i].fd >= 0) {
            serve(&g.clients[i]);
        }
    }
}

void rpod_control_init(const char *sock_path, rpod_lb_t *lb, const char *lb_token_path)
{
    g.lb = lb;
    snprintf(g.lb_token_path, sizeof(g.lb_token_path), "%s", lb_token_path != NULL ? lb_token_path : "");
    for (int i = 0; i < MAX_CLIENTS; i++) {
        g.clients[i].fd = -1;
    }
    if (sock_path == NULL || sock_path[0] == '\0') {
        return;
    }

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    if (strlen(sock_path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "rpod: control socket path too long: %s\n", sock_path);
        return;
    }
    memcpy(addr.sun_path, sock_path, strlen(sock_path) + 1);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        fprintf(stderr, "rpod: control socket: %s\n", strerror(errno));
        return;
    }
    /* A socket left behind by a previous run (only a socket: never a file
     * that happens to sit at that path). */
    struct stat st;
    if (lstat(sock_path, &st) == 0 && S_ISSOCK(st.st_mode)) {
        unlink(sock_path);
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || chmod(sock_path, 0660) != 0 ||
        listen(fd, 8) != 0) {
        fprintf(stderr, "rpod: control socket %s: %s\n", sock_path, strerror(errno));
        close(fd);
        return;
    }
    g.listen_fd = fd;
    lv_timer_create(poll_cb, POLL_MS, NULL);
    fprintf(stderr, "rpod: settings control at %s\n", sock_path);
}

void rpod_control_on_play_video(const char *(*play)(const char *name, void *ctx), void *ctx)
{
    g.play_video = play;
    g.play_video_ctx = ctx;
}
