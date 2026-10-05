#include "backlight.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

/* Until Settings > Backlight says otherwise. */
#define DEFAULT_TIMEOUT_S 30u

/* Longest timeout a state file is trusted with -- anything past it is
 * corrupt, not a choice the screen offers. */
#define MAX_TIMEOUT_S 3600u

/* How often idle time is checked: how late past the timeout the screen may
 * go dark. */
#define CHECK_MS 250

static struct {
    rpod_input_t *in;
    void (*off)(void *ctx);
    void *ctx;
    unsigned timeout_s;
    char path[PATH_MAX];
} g = {
    .timeout_s = DEFAULT_TIMEOUT_S,
};

static void load(void)
{
    FILE *f = fopen(g.path, "r");
    if (f == NULL) {
        return; /* first run */
    }
    unsigned s;
    if (fscanf(f, "%u", &s) == 1 && s <= MAX_TIMEOUT_S) {
        g.timeout_s = s;
    }
    fclose(f);
}

/* Written to a temp file and renamed over the real one, so a power cut
 * mid-write can't leave it truncated. */
static void save(void)
{
    char tmp[sizeof(g.path) + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g.path);
    FILE *f = fopen(tmp, "w");
    if (f == NULL) {
        fprintf(stderr, "rpod: backlight couldn't write %s: %s\n", tmp, strerror(errno));
        return;
    }
    fprintf(f, "%u\n", g.timeout_s);
    if (fclose(f) != 0 || rename(tmp, g.path) != 0) {
        fprintf(stderr, "rpod: backlight couldn't save %s: %s\n", g.path, strerror(errno));
        remove(tmp);
    }
}

static void check_cb(lv_timer_t *t)
{
    (void)t;
    if (g.timeout_s != RPOD_BACKLIGHT_ALWAYS_ON && !rpod_input_asleep(g.in) &&
        rpod_input_idle_ms(g.in) >= g.timeout_s * 1000u) {
        g.off(g.ctx);
    }
}

void rpod_backlight_init(rpod_input_t *in, void (*off)(void *ctx), void *ctx, const char *state_path)
{
    g.in = in;
    g.off = off;
    g.ctx = ctx;
    if (state_path != NULL && state_path[0] != '\0') {
        snprintf(g.path, sizeof(g.path), "%s", state_path);
        load();
    }
    lv_timer_create(check_cb, CHECK_MS, NULL);
}

unsigned rpod_backlight_timeout_s(void)
{
    return g.timeout_s;
}

void rpod_backlight_set_timeout_s(unsigned seconds)
{
    if (seconds > MAX_TIMEOUT_S || seconds == g.timeout_s) {
        return;
    }
    g.timeout_s = seconds;
    if (g.path[0] != '\0') {
        save();
    }
}
