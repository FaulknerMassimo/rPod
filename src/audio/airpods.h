/*
 * AirPods extras over AAP (audio/aap.h, docs/PLAN.md §6.3): battery for each
 * bud and the case, noise control, and the AirPods' own settings, for
 * ui/screens/airpods_screens.c -- plus what an iPhone does with them:
 *
 *   - Taking a bud out pauses MPD; putting it back resumes what that paused.
 *     Only while MPD plays to its Bluetooth output, and only with the
 *     AirPods' Automatic Ear Detection on.
 *   - Conversation Awareness lowers the volume while the wearer talks
 *     (audio/volume_memory.h does the ducking).
 *   - Stem presses drive MPD: press for play/pause, double press for next,
 *     triple press for previous. Press-and-hold stays the AirPods' own
 *     (cycling noise control), which they report back like any other change.
 *
 * Whichever connected BlueZ device advertises the AAP service (one at a time)
 * gets an L2CAP channel on PSM 0x1001, opened shortly after it connects and
 * reopened with backoff if it drops while the device stays connected. All on
 * the LVGL thread: a non-blocking socket polled from an lv_timer, like
 * audio/bluetooth.c's bus.
 *
 * In the sim, RPOD_AIRPODS_SOCK=<path> swaps BlueZ for a Unix SEQPACKET
 * socket served by tools/fake-airpods.py. RPOD_AIRPODS_DEBUG=1 logs every
 * packet both ways.
 */

#ifndef RPOD_AIRPODS_H
#define RPOD_AIRPODS_H

#include "audio/mpd_client.h"

#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    RPOD_AIRPODS_ABSENT,     /* no AAP device connected over Bluetooth */
    RPOD_AIRPODS_CONNECTING, /* connected for audio; control channel coming up */
    RPOD_AIRPODS_READY,      /* settings arrive (and can be set) from here on */
} rpod_airpods_link_t;

typedef struct {
    int level;     /* percent, or -1 while it isn't reporting */
    bool charging;
} rpod_airpods_battery_t;

typedef struct {
    rpod_airpods_link_t link;
    char path[64];  /* BlueZ device path; "" while ABSENT */
    char name[96];
    char error[64]; /* why the control channel last dropped; "" once it's up */
    rpod_airpods_battery_t left, right, charging_case;
    rpod_airpods_battery_t headset; /* one-piece headphones (AirPods Max) */
    int ear[2];     /* primary, secondary bud: RPOD_AAP_EAR_*, or -1 unknown */
    char model[16];
    char serial[32];
    char firmware[48];
} rpod_airpods_t;

/* Starts following BlueZ (audio/bluetooth.h, so after rpod_bt_init()) for
 * AAP devices. `mpd` must outlive it. Call once, after lv_init(). */
void rpod_airpods_init(rpod_mpd_t *mpd);

/* Current state. Valid until the next lv_timer tick -- copy what you keep. */
const rpod_airpods_t *rpod_airpods(void);

/* A setting's value (RPOD_AAP_CTL_*), as last reported -- or set. False if
 * these AirPods haven't reported it, which usually means the model doesn't
 * have that setting at all. */
bool rpod_airpods_get(uint8_t id, uint8_t *value);

/* Changes a setting. Takes effect in rpod_airpods_get() straight away; the
 * AirPods confirm it (or correct it) shortly after. No-op unless READY. */
void rpod_airpods_set(uint8_t id, uint8_t value);

/* Calls cb(user) on the LVGL thread after anything in rpod_airpods() or a
 * setting changes, at most once per tick, until `owner` is deleted (or, for
 * a NULL owner, for the life of the process). */
void rpod_airpods_watch(lv_obj_t *owner, void (*cb)(void *user), void *user);

#endif /* RPOD_AIRPODS_H */
