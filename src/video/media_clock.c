#include "media_clock.h"

/* How much of a small difference each answer takes up: a quarter, so a
 * chunk's worth of jitter moves a frame by a few ms rather than 20. */
#define EASE_DIV 4

void rpod_media_clock_reset(rpod_media_clock_t *c)
{
    *c = (rpod_media_clock_t){ 0 };
}

int64_t rpod_media_clock_now(const rpod_media_clock_t *c, int64_t wall_ms)
{
    if (!c->valid) {
        return 0;
    }
    if (!c->playing || wall_ms <= c->wall_ms) {
        return c->media_ms;
    }
    return c->media_ms + (wall_ms - c->wall_ms);
}

bool rpod_media_clock_observe(rpod_media_clock_t *c, int64_t media_ms, bool playing, int64_t wall_ms)
{
    int64_t predicted = rpod_media_clock_now(c, wall_ms);
    int64_t err = media_ms - predicted;
    bool far = err > RPOD_MEDIA_CLOCK_SNAP_MS || err < -RPOD_MEDIA_CLOCK_SNAP_MS;
    /* Starting or stopping lands exactly where MPD says: the eased value
     * would otherwise freeze a little off it for as long as it's paused. */
    bool snap = !c->valid || far || playing != c->playing;
    c->valid = true;
    c->playing = playing;
    c->wall_ms = wall_ms;
    c->media_ms = snap ? media_ms : predicted + err / EASE_DIV;
    return far;
}
