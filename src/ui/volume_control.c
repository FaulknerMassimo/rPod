#include "volume_control.h"

#include "audio/airpods.h"
#include "audio/bluetooth.h"
#include "audio/volume_memory.h"
#include "ui/hud.h"

#include "lvgl.h"

/* Volume change per step (louder = clockwise). A wheel step is 6 of its 96
 * positions (input/wheel_input.c), so a slow full turn moves the volume
 * ~64%, and the wheel's acceleration covers the whole range in a quick
 * flick. */
#define VOLUME_STEP 4

static struct {
    rpod_mpd_t *mpd;
    /* The level being steered toward (-1: no mixer to set). While the HUD's
     * volume face is up it's authoritative over what MPD reads back, so a
     * read landing between a step and its push can't snap the level back
     * mid-turn. */
    int volume;
    bool push_pending;
    unsigned gen; /* rpod_volume_memory_generation() `volume` follows */
} g = { .volume = -1 };

static rpod_hud_icon_t icon(void)
{
    return rpod_airpods()->link != RPOD_AIRPODS_ABSENT ? RPOD_HUD_ICON_AIRPODS
                                                        : RPOD_HUD_ICON_SPEAKER;
}

/* Sends the target to MPD. Deferred via lv_async_call() rather than run per
 * step: a fast flick arrives as one encoder read carrying several steps (one
 * LV_EVENT_KEY each), and this collapses them into a single MPD round
 * trip. */
static void push_cb(void *user)
{
    (void)user;
    g.push_pending = false;
    if (g.volume >= 0) {
        rpod_mpd_set_volume(g.mpd, (unsigned)g.volume);
    }
}

/* Takes up MPD's current level. A Bluetooth device switch restores that
 * device's own level under us (audio/volume_memory.h): a mid-turn target
 * must be dropped then, or stepping on from it would carry the previous
 * device's level -- maybe a speaker's -- into the new one. */
static void reread(void)
{
    if (g.push_pending) {
        lv_async_call_cancel(push_cb, NULL);
        g.push_pending = false;
    }
    int v;
    g.volume = rpod_mpd_get_volume(g.mpd, &v) ? v : -1;
}

void rpod_volume_control_step(int dir)
{
    if (g.mpd == NULL) {
        return;
    }
    unsigned gen = rpod_volume_memory_generation();
    /* The start of a turn picks up wherever the volume is now: another
     * client, or Conversation Awareness, may have moved it since. */
    if (gen != g.gen || (!rpod_hud_volume_shown() && !g.push_pending)) {
        g.gen = gen;
        reread();
    }

    int target = g.volume + dir * VOLUME_STEP;
    target = target < 0 ? 0 : target > 100 ? 100 : target;
    if (g.volume < 0 || target == g.volume) {
        /* Past either end, or nothing to set: just rubber-band. */
        rpod_hud_volume(g.volume, icon());
        rpod_hud_volume_bump(dir);
        return;
    }
    g.volume = target;
    rpod_hud_volume(target, icon());
    if (!g.push_pending) {
        g.push_pending = true;
        lv_async_call(push_cb, NULL);
    }
}

static void volume_buttons_cb(int steps, void *user)
{
    (void)user;
    for (; steps > 0; steps--) {
        rpod_volume_control_step(+1);
    }
    for (; steps < 0; steps++) {
        rpod_volume_control_step(-1);
    }
}

void rpod_volume_control_init(rpod_mpd_t *mpd)
{
    g.mpd = mpd;
    g.gen = rpod_volume_memory_generation();
    rpod_bt_on_volume_buttons(volume_buttons_cb, NULL);
}
