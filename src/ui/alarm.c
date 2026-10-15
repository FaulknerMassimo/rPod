#include "alarm.h"

#include "audio/volume_memory.h"
#include "lvgl.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

/* How often the time is checked -- how late an alarm may go off. */
#define CHECK_MS 1000

/* The fade-in: from this much of the volume, up by STEP every second (~30
 * s to the full level). It's a duck (audio/volume_memory.h) stepped back
 * up, so the quiet start is never remembered as the device's volume, and
 * turning the volume during it ends it there. */
#define FADE_START_PCT 10u
#define FADE_STEP_PCT  3u

#define DEFAULT_HOUR   7

static struct {
    rpod_mpd_t *mpd;
    void (*ring)(void *ctx);
    void *ctx;
    char path[PATH_MAX];

    bool enabled;
    int hour, minute;
    char sound[256];

    bool ringing;
    time_t snooze_at;   /* 0: no snooze waiting */
    long last_minute;   /* the minute it last went off, so once a day */
    bool fading;
    unsigned fade_pct;
} g = {
    .hour = DEFAULT_HOUR,
    .last_minute = -1,
};

static void load(void)
{
    FILE *f = fopen(g.path, "r");
    if (f == NULL) {
        return; /* never set */
    }
    char onoff[4];
    int h, m;
    if (fscanf(f, "%3s %d:%d\n", onoff, &h, &m) == 3 && h >= 0 && h < 24 && m >= 0 && m < 60) {
        g.enabled = strcmp(onoff, "on") == 0;
        g.hour = h;
        g.minute = m;
        char line[sizeof(g.sound)];
        if (fgets(line, sizeof(line), f) != NULL) {
            line[strcspn(line, "\n")] = '\0';
            snprintf(g.sound, sizeof(g.sound), "%s", line);
        }
    }
    fclose(f);
}

/* Written to a temp file and renamed over the real one, so a power cut
 * mid-write can't leave it truncated. */
static void save(void)
{
    if (g.path[0] == '\0') {
        return;
    }
    char tmp[sizeof(g.path) + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g.path);
    FILE *f = fopen(tmp, "w");
    if (f == NULL) {
        fprintf(stderr, "rpod: alarm couldn't write %s: %s\n", tmp, strerror(errno));
        return;
    }
    fprintf(f, "%s %02d:%02d\n%s\n", g.enabled ? "on" : "off", g.hour, g.minute, g.sound);
    if (fclose(f) != 0 || rename(tmp, g.path) != 0) {
        fprintf(stderr, "rpod: alarm couldn't save %s: %s\n", g.path, strerror(errno));
        remove(tmp);
    }
}

/* The fade's over: the volume back to its real level, unless it's been
 * turned since -- then that level stands. */
static void end_fade(void)
{
    if (g.fading && rpod_volume_memory_ducked()) {
        rpod_volume_memory_duck(100);
    }
    g.fading = false;
}

static void fade_cb(lv_timer_t *t)
{
    (void)t;
    if (!g.fading) {
        return;
    }
    if (!rpod_volume_memory_ducked()) {
        g.fading = false; /* turned by hand, or a device switch: that level stands */
        return;
    }
    g.fade_pct += FADE_STEP_PCT;
    if (g.fade_pct >= 100) {
        end_fade();
    } else {
        rpod_volume_memory_duck(g.fade_pct);
    }
}

/* Music, quietly, then the app's ring(). A snooze picks up the music it
 * paused; on time, it starts its sound afresh. */
static void go_off(bool from_snooze)
{
    fprintf(stderr, "rpod: alarm going off\n");
    g.ringing = true;
    g.fade_pct = FADE_START_PCT;
    rpod_volume_memory_duck(g.fade_pct);
    g.fading = rpod_volume_memory_ducked();

    bool playing = false;
    if (from_snooze) {
        playing = rpod_mpd_set_paused(g.mpd, false);
    } else if (g.sound[0] != '\0') {
        playing = rpod_mpd_play_playlist(g.mpd, g.sound, true);
    }
    if (!playing) {
        /* No playlist by that name any more: the library will do. */
        rpod_mpd_shuffle_all(g.mpd);
    }
    if (g.ring != NULL) {
        g.ring(g.ctx);
    }
}

static void check_cb(lv_timer_t *t)
{
    (void)t;
    time_t now = time(NULL);
    if (g.snooze_at != 0 && now >= g.snooze_at) {
        g.snooze_at = 0;
        go_off(true);
        return;
    }
    if (!g.enabled || g.ringing) {
        return;
    }
    struct tm tm;
    localtime_r(&now, &tm);
    long minute = (long)(now / 60);
    if (tm.tm_hour == g.hour && tm.tm_min == g.minute && minute != g.last_minute) {
        g.last_minute = minute;
        g.snooze_at = 0;
        go_off(false);
    }
}

void rpod_alarm_init(rpod_mpd_t *mpd, const char *state_path, void (*ring)(void *ctx), void *ctx)
{
    g.mpd = mpd;
    g.ring = ring;
    g.ctx = ctx;
    if (state_path != NULL && state_path[0] != '\0') {
        snprintf(g.path, sizeof(g.path), "%s", state_path);
        load();
    }
    /* Not straight away, if rPod starts during its minute: that's a restart,
     * not the time arriving. */
    time_t now = time(NULL);
    g.last_minute = (long)(now / 60);
    lv_timer_create(check_cb, CHECK_MS, NULL);
    lv_timer_create(fade_cb, 1000, NULL);
}

bool rpod_alarm_enabled(void)
{
    return g.enabled;
}

void rpod_alarm_set_enabled(bool on)
{
    if (!on) {
        g.snooze_at = 0;
    }
    if (on == g.enabled) {
        return;
    }
    g.enabled = on;
    save();
}

void rpod_alarm_time(int *hour, int *minute)
{
    *hour = g.hour;
    *minute = g.minute;
}

void rpod_alarm_set_time(int hour, int minute)
{
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        return;
    }
    g.hour = hour;
    g.minute = minute;
    /* A time set for this very minute still goes off. */
    g.last_minute = -1;
    save();
}

const char *rpod_alarm_sound(void)
{
    return g.sound;
}

void rpod_alarm_set_sound(const char *playlist)
{
    snprintf(g.sound, sizeof(g.sound), "%s", playlist != NULL ? playlist : "");
    save();
}

bool rpod_alarm_ringing(void)
{
    return g.ringing;
}

void rpod_alarm_snooze(void)
{
    g.ringing = false;
    rpod_mpd_set_paused(g.mpd, true);
    end_fade();
    g.snooze_at = time(NULL) + RPOD_ALARM_SNOOZE_MIN * 60;
}

void rpod_alarm_stop(void)
{
    g.ringing = false;
    g.snooze_at = 0;
    rpod_mpd_set_paused(g.mpd, true);
    end_fade();
}

void rpod_alarm_dismiss(void)
{
    g.ringing = false; /* the fade carries on up to full */
}

time_t rpod_alarm_snoozed_until(void)
{
    return g.snooze_at;
}
