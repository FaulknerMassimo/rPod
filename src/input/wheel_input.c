#include "wheel_input.h"

#include "../../daemon/wheel_protocol.h"
#include "input/encoder.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define POLL_MS       10
#define RETRY_MS      1000

/* --- scroll acceleration (docs/PLAN.md §8.2) -----------------------------
 *
 * Wheel speed is measured over a sliding window of recent wheel events, using
 * the daemon's own timestamps (when the packet came off the wire, not when
 * this poll got to it). Below V0 every STEP_TICKS positions is one step, for
 * precise slow scrolling; between V0 and V1 the multiplier eases in
 * quadratically up to ACCEL_MAX, so a fast flick covers many rows. */

/* The 4th-gen wheel has 96 positions per turn (docs/clickwheel-protocol.md),
 * so 6 per step is 16 rows a turn at slow speed (4 felt too sensitive on
 * hardware). A slow, deliberate turn measured ~35 positions/s; full
 * acceleration arrives at ~3 turns/s. */
#define DEFAULT_STEP_TICKS  6.0f
#define DEFAULT_ACCEL_V0    100.0f  /* positions/s */
#define DEFAULT_ACCEL_V1    300.0f  /* positions/s */
#define DEFAULT_ACCEL_MAX   6.0f
#define VEL_WINDOW_US       150000u
#define VEL_MIN_SPAN_US     20000u
#define VEL_RING            64

typedef struct {
    float step_ticks;
    float v0;
    float v1;
    float max_gain;

    /* Recent wheel events inside the velocity window: ring of (time, |delta|). */
    struct {
        uint64_t ts;
        int ticks;
    } win[VEL_RING];
    int win_head;   /* index of the oldest entry */
    int win_count;
    int win_sum;    /* sum of ticks over the entries in the ring */

    float acc;      /* gain-scaled positions not yet turned into whole steps */
    int last_dir;
} accel_t;

static float env_float(const char *name, float def)
{
    const char *v = getenv(name);
    if (v == NULL || v[0] == '\0') {
        return def;
    }
    char *end;
    float f = strtof(v, &end);
    return (end != v && f > 0.0f) ? f : def;
}

static void accel_init(accel_t *a)
{
    memset(a, 0, sizeof(*a));
    a->step_ticks = env_float("RPOD_WHEEL_STEP_TICKS", DEFAULT_STEP_TICKS);
    a->v0 = env_float("RPOD_WHEEL_ACCEL_V0", DEFAULT_ACCEL_V0);
    a->v1 = env_float("RPOD_WHEEL_ACCEL_V1", DEFAULT_ACCEL_V1);
    a->max_gain = env_float("RPOD_WHEEL_ACCEL_MAX", DEFAULT_ACCEL_MAX);
    if (a->v1 <= a->v0) {
        a->v1 = a->v0 + 1.0f;
    }
}

/* A new gesture (touch down/up, reversal) starts from rest: no leftover
 * fraction of a step, no speed carried over from before. */
static void accel_reset(accel_t *a)
{
    a->win_head = 0;
    a->win_count = 0;
    a->win_sum = 0;
    a->acc = 0.0f;
    a->last_dir = 0;
}

/* Feeds one wheel delta; returns the whole steps it produced (signed). */
static int accel_feed(accel_t *a, int delta, uint64_t ts)
{
    if (delta == 0) {
        return 0;
    }
    int dir = delta > 0 ? 1 : -1;
    if (a->last_dir != 0 && dir != a->last_dir) {
        accel_reset(a);
    }
    a->last_dir = dir;

    /* Expire events that fell out of the window, then add this one (evicting
     * the oldest if the ring is full -- that only underestimates speed). */
    while (a->win_count > 0 && ts - a->win[a->win_head].ts > VEL_WINDOW_US) {
        a->win_sum -= a->win[a->win_head].ticks;
        a->win_head = (a->win_head + 1) % VEL_RING;
        a->win_count--;
    }
    if (a->win_count == VEL_RING) {
        a->win_sum -= a->win[a->win_head].ticks;
        a->win_head = (a->win_head + 1) % VEL_RING;
        a->win_count--;
    }
    int slot = (a->win_head + a->win_count) % VEL_RING;
    a->win[slot].ts = ts;
    a->win[slot].ticks = abs(delta);
    a->win_count++;
    a->win_sum += abs(delta);

    /* Speed over the span the window actually covers, not the whole window:
     * a flick is often shorter than VEL_WINDOW_US, and dividing by the full
     * window would keep acceleration from engaging until it was nearly over.
     * The oldest event's ticks were travelled before the span began, so they
     * don't count; MIN_SPAN keeps two packets arriving close together from
     * reading as a huge spike. A lone event reads as speed 0. */
    int oldest = a->win_head;
    int newest = (a->win_head + a->win_count - 1) % VEL_RING;
    uint64_t span = a->win[newest].ts - a->win[oldest].ts;
    if (span < VEL_MIN_SPAN_US) {
        span = VEL_MIN_SPAN_US;
    }
    float speed = (float)(a->win_sum - a->win[oldest].ticks) * 1e6f / (float)span;
    float t = (speed - a->v0) / (a->v1 - a->v0);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    float gain = 1.0f + (a->max_gain - 1.0f) * t * t;

    a->acc += (float)delta * gain;
    int steps = (int)(a->acc / a->step_ticks); /* truncates toward zero */
    a->acc -= (float)steps * a->step_ticks;
    return steps;
}

/* --- socket client --------------------------------------------------------- */

typedef struct {
    lv_indev_t *indev;
    rpod_input_buttons_t buttons;
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];

    int fd;                 /* -1 while disconnected */
    uint32_t retry_at;      /* lv_tick_get() time of the next connect attempt */
    bool logged_waiting;

    /* SOCK_STREAM can split an event across reads; carry partial bytes over. */
    uint8_t rx[sizeof(struct rpod_wheel_event) * 32];
    size_t rx_len;

    bool center_held;
    accel_t accel;
} wheel_input_t;

static void try_connect(wheel_input_t *w)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, w->path, sizeof(addr.sun_path));

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        if (!w->logged_waiting) {
            fprintf(stderr, "rpod: waiting for the click wheel daemon at %s\n", w->path);
            w->logged_waiting = true;
        }
        return;
    }
    fprintf(stderr, "rpod: click wheel connected (%s)\n", w->path);
    w->fd = fd;
    w->rx_len = 0;
    w->logged_waiting = false;
}

/* Drops the connection and lets go of anything held, so a daemon restart
 * mid-press can't leave the select button stuck down -- without that
 * synthetic release counting as a click. */
static void disconnect(wheel_input_t *w)
{
    fprintf(stderr, "rpod: click wheel daemon went away; reconnecting\n");
    close(w->fd);
    w->fd = -1;
    w->retry_at = lv_tick_get() + RETRY_MS;
    if (w->center_held) {
        w->center_held = false;
        lv_indev_wait_release(w->indev);
    }
    accel_reset(&w->accel);
}

static void fire(void (*handler)(void *), void *ctx)
{
    if (handler != NULL) {
        handler(ctx);
    }
}

/* Returns the encoder steps this event produced. */
static int handle_event(wheel_input_t *w, const struct rpod_wheel_event *ev)
{
    switch (ev->type) {
    case RPOD_WHEEL_EVENT_WHEEL:
        return accel_feed(&w->accel, ev->value, ev->timestamp_us);

    case RPOD_WHEEL_EVENT_TOUCH:
        accel_reset(&w->accel);
        return 0;

    case RPOD_WHEEL_EVENT_BUTTON:
        if (ev->code == RPOD_WHEEL_BTN_CENTER) {
            /* Fed per edge, not once per poll: a tap whose press and release
             * land in the same poll must still latch as a press. */
            w->center_held = ev->value != 0;
            rpod_encoder_feed(w->indev, 0, w->center_held);
            return 0;
        }
        if (ev->value == 0) {
            return 0; /* the app-level buttons act on press */
        }
        switch (ev->code) {
        case RPOD_WHEEL_BTN_UP:    fire(w->buttons.on_menu, w->buttons.ctx); break;
        case RPOD_WHEEL_BTN_DOWN:  fire(w->buttons.on_play_pause, w->buttons.ctx); break;
        case RPOD_WHEEL_BTN_LEFT:  fire(w->buttons.on_prev, w->buttons.ctx); break;
        case RPOD_WHEEL_BTN_RIGHT: fire(w->buttons.on_next, w->buttons.ctx); break;
        default: break;
        }
        return 0;

    default:
        return 0;
    }
}

static void poll_cb(lv_timer_t *timer)
{
    wheel_input_t *w = lv_timer_get_user_data(timer);

    if (w->fd < 0) {
        if ((int32_t)(lv_tick_get() - w->retry_at) < 0) {
            return;
        }
        try_connect(w);
        if (w->fd < 0) {
            w->retry_at = lv_tick_get() + RETRY_MS;
            return;
        }
    }

    int steps = 0;
    bool got_any = false;
    for (;;) {
        ssize_t n = recv(w->fd, w->rx + w->rx_len, sizeof(w->rx) - w->rx_len, MSG_DONTWAIT);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            disconnect(w);
            break;
        }
        w->rx_len += (size_t)n;

        size_t off = 0;
        while (w->rx_len - off >= sizeof(struct rpod_wheel_event)) {
            struct rpod_wheel_event ev;
            memcpy(&ev, w->rx + off, sizeof(ev));
            off += sizeof(ev);
            steps += handle_event(w, &ev);
            got_any = true;
        }
        memmove(w->rx, w->rx + off, w->rx_len - off);
        w->rx_len -= off;
    }

    rpod_encoder_feed(w->indev, steps, w->center_held);

    /* Hand it to LVGL now rather than waiting up to a whole read period for
     * the indev's own timer -- a button should feel instant. */
    if (got_any) {
        lv_indev_read(w->indev);
    }
}

lv_indev_t *rpod_wheel_input_create(const char *sock_path, const rpod_input_buttons_t *buttons)
{
    wheel_input_t *w = calloc(1, sizeof(*w));
    w->indev = rpod_encoder_create();
    w->buttons = *buttons;
    snprintf(w->path, sizeof(w->path), "%s", sock_path != NULL ? sock_path : RPOD_WHEEL_SOCK_PATH);
    w->fd = -1;
    w->retry_at = lv_tick_get();
    accel_init(&w->accel);

    fprintf(stderr, "rpod: wheel accel: %.1f positions/step, x%.1f max between %.0f and %.0f positions/s\n",
            (double)w->accel.step_ticks, (double)w->accel.max_gain, (double)w->accel.v0,
            (double)w->accel.v1);

    lv_timer_create(poll_cb, POLL_MS, w);
    return w->indev;
}
