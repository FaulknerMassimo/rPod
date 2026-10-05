#include "seek_control.h"

#include "lvgl.h"

/* Scan speed, in song time per real time: SPEED_START for the first
 * RAMP_AFTER_MS held, then building linearly to SPEED_MAX over RAMP_MS.
 * At 8x a 4-minute song takes 30 s end to end; at 32x, under 8 s. */
#define SPEED_START   8.0f
#define SPEED_MAX     32.0f
#define RAMP_AFTER_MS 2000u
#define RAMP_MS       3000u

/* How often the position moves -- also how often Now Playing redraws it. */
#define TICK_MS 33

/* A forward scan let go this close to the end goes to the next song rather
 * than seeking into the song's last moments. */
#define END_MARGIN_MS 1000u

static struct {
    rpod_mpd_t *mpd;
    lv_timer_t *timer;
    bool active;
    int dir;
    bool was_playing;
    float pos_ms;
    unsigned duration_ms;
    uint32_t start_ms; /* lv_tick_get() when the scan began */
    uint32_t last_ms;  /* ...and when the position last moved */
} s;

static float speed_at(uint32_t held_ms)
{
    if (held_ms <= RAMP_AFTER_MS) {
        return SPEED_START;
    }
    float t = (float)(held_ms - RAMP_AFTER_MS) / (float)RAMP_MS;
    if (t > 1.0f) {
        t = 1.0f;
    }
    return SPEED_START + (SPEED_MAX - SPEED_START) * t;
}

static void tick_cb(lv_timer_t *timer)
{
    (void)timer;
    uint32_t now = lv_tick_get();
    float dt = (float)lv_tick_diff(now, s.last_ms);
    s.last_ms = now;

    s.pos_ms += (float)s.dir * speed_at(lv_tick_diff(now, s.start_ms)) * dt;
    if (s.pos_ms < 0.0f) {
        s.pos_ms = 0.0f;
    }
    if (s.duration_ms > 0 && s.pos_ms > (float)s.duration_ms) {
        s.pos_ms = (float)s.duration_ms;
    }
}

void rpod_seek_control_init(rpod_mpd_t *mpd)
{
    s.mpd = mpd;
    s.timer = lv_timer_create(tick_cb, TICK_MS, NULL);
    lv_timer_pause(s.timer);
}

void rpod_seek_control_start(int dir)
{
    if (s.active || dir == 0) {
        return;
    }
    rpod_mpd_state_t state;
    unsigned elapsed_ms, duration_ms;
    if (!rpod_mpd_get_position(s.mpd, &state, &elapsed_ms, &duration_ms) ||
        (state != RPOD_MPD_STATE_PLAY && state != RPOD_MPD_STATE_PAUSE)) {
        return;
    }
    s.was_playing = state == RPOD_MPD_STATE_PLAY;
    if (s.was_playing) {
        rpod_mpd_set_paused(s.mpd, true);
    }
    s.active = true;
    s.dir = dir > 0 ? 1 : -1;
    s.pos_ms = (float)elapsed_ms;
    s.duration_ms = duration_ms;
    s.start_ms = s.last_ms = lv_tick_get();
    lv_timer_reset(s.timer);
    lv_timer_resume(s.timer);
}

void rpod_seek_control_stop(void)
{
    if (!s.active) {
        return;
    }
    tick_cb(s.timer); /* catch up to the moment it was let go */
    lv_timer_pause(s.timer);
    s.active = false;

    if (s.dir > 0 && s.duration_ms > 0 && s.pos_ms + (float)END_MARGIN_MS >= (float)s.duration_ms) {
        /* Whatever "next" leaves it doing, put it back how it was. */
        rpod_mpd_next(s.mpd);
        rpod_mpd_set_paused(s.mpd, !s.was_playing);
        return;
    }
    rpod_mpd_seek(s.mpd, (unsigned)s.pos_ms);
    if (s.was_playing) {
        rpod_mpd_set_paused(s.mpd, false);
    }
}

bool rpod_seek_control_position(unsigned *pos_ms, unsigned *duration_ms)
{
    if (!s.active) {
        return false;
    }
    *pos_ms = (unsigned)s.pos_ms;
    *duration_ms = s.duration_ms;
    return true;
}
