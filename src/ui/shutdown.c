#include "shutdown.h"

#include "alarm.h"
#include "hud.h"
#include "metrics.h"
#include "theme.h"
#include "power/battery.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

/* Until Settings > Auto-Off says otherwise. */
#define DEFAULT_IDLE_MIN 30u

/* Longest timeout a state file is trusted with -- anything past it is
 * corrupt, not a choice the screen offers. */
#define MAX_IDLE_MIN (24u * 60u)

/* How often it looks: how late past the timeout it may shut down. It asks
 * MPD whether it's playing, so not every tick. */
#define CHECK_MS 15000

/* With a message up: long enough to read it before the panel goes. */
#define MSG_MS 1500

/* The simulator's pretend shutdown: how long it stays up. */
#define PRETEND_MS 2000

static struct {
    rpod_input_t *in;
    rpod_mpd_t *mpd;
    bool for_real;
    unsigned idle_min;
    uint32_t busy_ms; /* lv_tick_get() when something the input doesn't see last happened */
    bool going;
    lv_obj_t *overlay;
    char path[PATH_MAX];
} g = {
    .idle_min = DEFAULT_IDLE_MIN,
};

static void load(void)
{
    FILE *f = fopen(g.path, "r");
    if (f == NULL) {
        return; /* first run */
    }
    unsigned m;
    if (fscanf(f, "%u", &m) == 1 && m <= MAX_IDLE_MIN) {
        g.idle_min = m;
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
        fprintf(stderr, "rpod: shutdown couldn't write %s: %s\n", tmp, strerror(errno));
        return;
    }
    fprintf(f, "%u\n", g.idle_min);
    if (fclose(f) != 0 || rename(tmp, g.path) != 0) {
        fprintf(stderr, "rpod: shutdown couldn't save %s: %s\n", g.path, strerror(errno));
        remove(tmp);
    }
}

/* Back as it was: after the simulator's pretend shutdown, or logind saying
 * no. The idle count starts over. */
static void put_back(void)
{
    g.going = false;
    g.busy_ms = lv_tick_get();
    if (g.overlay != NULL) {
        lv_obj_delete(g.overlay);
        g.overlay = NULL;
    }
}

static void put_back_cb(lv_timer_t *t)
{
    (void)t;
    put_back();
}

static void power_off_cb(lv_timer_t *t)
{
    (void)t;
    if (!g.for_real) {
        fprintf(stderr, "rpod: shutting down: the device would power off now\n");
        lv_timer_t *back = lv_timer_create(put_back_cb, PRETEND_MS, NULL);
        lv_timer_set_repeat_count(back, 1);
        return;
    }
    if (!rpod_power_off()) {
        put_back();
        rpod_hud_message(RPOD_HUD_ICON_POWER, "Couldn't Shut Down");
    }
    /* Else systemd stops rpod in a moment, the overlay up till then. */
}

static void check_cb(lv_timer_t *t)
{
    (void)t;
    if (g.going) {
        return;
    }
    rpod_mpd_state_t state;
    if ((rpod_mpd_get_state(g.mpd, &state) && state == RPOD_MPD_STATE_PLAY) || rpod_battery()->external ||
        rpod_alarm_enabled() || rpod_alarm_ringing() || rpod_alarm_snoozed_until() != 0) {
        g.busy_ms = lv_tick_get();
        return;
    }
    uint32_t limit = g.idle_min * 60000u;
    if (g.idle_min != RPOD_SHUTDOWN_NEVER && rpod_input_idle_ms(g.in) >= limit &&
        lv_tick_elaps(g.busy_ms) >= limit) {
        fprintf(stderr, "rpod: nothing happening for %u minute%s: shutting down\n", g.idle_min,
                g.idle_min == 1 ? "" : "s");
        rpod_shutdown_now(NULL);
    }
}

void rpod_shutdown_init(rpod_input_t *in, rpod_mpd_t *mpd, const char *state_path, bool for_real)
{
    g.in = in;
    g.mpd = mpd;
    g.for_real = for_real;
    g.busy_ms = lv_tick_get();
    if (state_path != NULL && state_path[0] != '\0') {
        snprintf(g.path, sizeof(g.path), "%s", state_path);
        load();
    }
    lv_timer_create(check_cb, CHECK_MS, NULL);
}

unsigned rpod_shutdown_idle_min(void)
{
    return g.idle_min;
}

void rpod_shutdown_set_idle_min(unsigned minutes)
{
    if (minutes > MAX_IDLE_MIN || minutes == g.idle_min) {
        return;
    }
    g.idle_min = minutes;
    if (g.path[0] != '\0') {
        save();
    }
}

void rpod_shutdown_activity(void)
{
    g.busy_ms = lv_tick_get();
}

void rpod_shutdown_now(const char *msg)
{
    if (g.going) {
        return;
    }
    g.going = true;

    /* MPD would keep its place either way; paused, it comes back paused
     * rather than mid-song. */
    rpod_mpd_state_t state;
    if (rpod_mpd_get_state(g.mpd, &state) && state == RPOD_MPD_STATE_PLAY) {
        rpod_mpd_set_paused(g.mpd, true);
    }

    if (msg == NULL) {
        power_off_cb(NULL);
        return;
    }
    rpod_input_wake(g.in);
    g.overlay = lv_obj_create(lv_layer_sys());
    lv_obj_remove_style_all(g.overlay);
    lv_obj_set_size(g.overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(g.overlay, RPOD_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(g.overlay, LV_OPA_COVER, 0);
    lv_obj_t *label = lv_label_create(g.overlay);
    lv_label_set_text(label, msg);
    lv_obj_set_style_text_font(label, rpod_metrics()->font_body, 0);
    lv_obj_set_style_text_color(label, RPOD_COLOR_TEXT, 0);
    lv_obj_center(label);
    lv_timer_t *off = lv_timer_create(power_off_cb, MSG_MS, NULL);
    lv_timer_set_repeat_count(off, 1);
}
