#include "battery.h"

#include "gauge.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>

/* How often the PiSugar is read: the level moves over minutes, but USB
 * power going in or out should show within a few seconds. */
#define SAMPLE_MS 2000

/* With no PiSugar answering, how often to look again. */
#define RETRY_MS 30000

/* How often the LVGL side picks up what the thread read. Watchers hear only
 * of what they show changing: the level, USB power, running out. */
#define DELIVER_MS 500

typedef struct {
    lv_obj_t *owner;
    void (*cb)(void *user);
    void *user;
} watcher_t;

static struct {
    char dev[64];

    /* Written by the reader thread, under the lock. */
    pthread_mutex_t lock;
    rpod_battery_t shared;
    unsigned seq;

    /* The LVGL thread's copy. */
    rpod_battery_t pub;
    unsigned pub_seq;
    watcher_t *watchers;
    size_t nwatch, watchcap;
} g = { .lock = PTHREAD_MUTEX_INITIALIZER };

/* --- The reader thread ------------------------------------------------------------ */

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void sleep_ms(unsigned ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000 };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

/* One register, by SMBus "read byte data"; -1 if the chip didn't answer. */
static int read_reg(int fd, uint8_t reg)
{
    union i2c_smbus_data data;
    struct i2c_smbus_ioctl_data args = {
        .read_write = I2C_SMBUS_READ,
        .command = reg,
        .size = I2C_SMBUS_BYTE_DATA,
        .data = &data,
    };
    return ioctl(fd, I2C_SMBUS, &args) < 0 ? -1 : data.byte;
}

/* Whether anything the UI shows differs: the voltage alone isn't news. */
static bool same_shown(const rpod_battery_t *a, const rpod_battery_t *b)
{
    return a->present == b->present && a->level == b->level && a->external == b->external &&
           a->charging == b->charging && a->critical == b->critical;
}

static void publish(const rpod_battery_t *b)
{
    pthread_mutex_lock(&g.lock);
    if (!same_shown(&g.shared, b)) {
        g.seq++;
    }
    g.shared = *b;
    pthread_mutex_unlock(&g.lock);
}

static void *reader(void *arg)
{
    (void)arg;
    static const uint8_t regs[3] = { RPOD_IP5209_VBAT_LO, RPOD_IP5209_VBAT_HI, RPOD_IP5209_USB };
    rpod_gauge_t gauge;
    rpod_gauge_init(&gauge);
    int fd = -1;
    bool found = false, said_missing = false;
    uint32_t last_ms = 0;

    for (;;) {
        if (fd < 0) {
            fd = open(g.dev, O_RDWR | O_CLOEXEC);
            if (fd >= 0 && ioctl(fd, I2C_SLAVE, RPOD_IP5209_ADDR) < 0) {
                close(fd);
                fd = -1;
            }
        }
        int r[3] = { -1, -1, -1 };
        bool ok = fd >= 0;
        for (size_t i = 0; ok && i < 3; i++) {
            r[i] = read_reg(fd, regs[i]);
            ok = r[i] >= 0;
        }

        if (!ok) {
            if (!said_missing) {
                fprintf(stderr, "rpod: battery: no PiSugar on %s (%s)\n", g.dev, strerror(errno));
                said_missing = true;
            }
            if (fd >= 0) {
                close(fd);
                fd = -1;
            }
            found = false;
            rpod_gauge_init(&gauge);
            publish(&(rpod_battery_t){ .level = -1 });
            sleep_ms(RETRY_MS);
            continue;
        }

        uint32_t t = now_ms();
        int mv = rpod_ip5209_voltage_mv((uint8_t)r[0], (uint8_t)r[1]);
        bool usb = r[2] != 0;
        rpod_gauge_feed(&gauge, mv, usb, found ? t - last_ms : 0);
        last_ms = t;
        if (!found) {
            int lo = read_reg(fd, RPOD_IP5209_LOAD_LO), hi = read_reg(fd, RPOD_IP5209_LOAD_HI);
            fprintf(stderr, "rpod: battery: PiSugar on %s: %d mV, %s, the Pi drawing %d mA\n", g.dev, mv,
                    usb ? "on USB power" : "on battery",
                    lo >= 0 && hi >= 0 ? -rpod_ip5209_current_ma((uint8_t)lo, (uint8_t)hi) : 0);
            found = true;
            said_missing = false;
        }
        publish(&(rpod_battery_t){
            .present = true,
            .level = gauge.level,
            .external = gauge.external,
            .charging = gauge.charging,
            .critical = gauge.critical,
            .mv = gauge.mv,
        });
        sleep_ms(SAMPLE_MS);
    }
    return NULL;
}

/* --- RPOD_BATTERY_FAKE ------------------------------------------------------------ */

/* "15", "80,charging", "100,charged", "2,critical". */
static bool parse_fake(const char *s, rpod_battery_t *out)
{
    char *end;
    long level = strtol(s, &end, 10);
    if (end == s || level < 0 || level > 100) {
        return false;
    }
    *out = (rpod_battery_t){ .present = true, .level = (int)level, .mv = 3900 };
    if (strcmp(end, ",charging") == 0) {
        out->external = out->charging = true;
    } else if (strcmp(end, ",charged") == 0) {
        out->external = true;
    } else if (strcmp(end, ",critical") == 0) {
        out->critical = true;
    } else if (*end != '\0') {
        return false;
    }
    return true;
}

/* --- LVGL side -------------------------------------------------------------------- */

static void deliver_cb(lv_timer_t *t)
{
    (void)t;
    pthread_mutex_lock(&g.lock);
    bool changed = g.seq != g.pub_seq;
    g.pub = g.shared;
    g.pub_seq = g.seq;
    pthread_mutex_unlock(&g.lock);
    if (changed) {
        for (size_t i = 0; i < g.nwatch; i++) {
            g.watchers[i].cb(g.watchers[i].user);
        }
    }
}

void rpod_battery_init(const char *i2c_dev)
{
    static bool started;
    if (started) {
        return;
    }
    started = true;
    g.pub.level = -1;

    const char *fake = getenv("RPOD_BATTERY_FAKE");
    if (fake != NULL && fake[0] != '\0') {
        if (parse_fake(fake, &g.pub)) {
            fprintf(stderr, "rpod: battery: faking %s (RPOD_BATTERY_FAKE)\n", fake);
        } else {
            fprintf(stderr, "rpod: battery: can't read RPOD_BATTERY_FAKE=%s\n", fake);
        }
        return;
    }
    if (i2c_dev == NULL || i2c_dev[0] == '\0') {
        return;
    }
    snprintf(g.dev, sizeof(g.dev), "%s", i2c_dev);
    g.shared = g.pub;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t th;
    if (pthread_create(&th, &attr, reader, NULL) != 0) {
        fprintf(stderr, "rpod: battery: couldn't start its thread\n");
    } else {
        lv_timer_create(deliver_cb, DELIVER_MS, NULL);
    }
    pthread_attr_destroy(&attr);
}

const rpod_battery_t *rpod_battery(void)
{
    return &g.pub;
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

void rpod_battery_watch(lv_obj_t *owner, void (*cb)(void *user), void *user)
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

bool rpod_power_off(void)
{
    sd_bus *bus = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r = sd_bus_open_system(&bus);
    if (r >= 0) {
        /* interactive = false: no one is there to answer a password prompt. */
        r = sd_bus_call_method(bus, "org.freedesktop.login1", "/org/freedesktop/login1",
                               "org.freedesktop.login1.Manager", "PowerOff", &err, NULL, "b", 0);
    }
    if (r < 0) {
        fprintf(stderr, "rpod: power off: %s\n", err.message != NULL ? err.message : strerror(-r));
    }
    sd_bus_error_free(&err);
    sd_bus_unref(bus);
    return r >= 0;
}
