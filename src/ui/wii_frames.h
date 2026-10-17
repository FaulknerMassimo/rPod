/*
 * Which frame of the Wii theme's art (ui/wii.h) shows a state: the play
 * state, repeat, shuffle, the battery and the volume. Plain arithmetic on
 * MPD's and the battery's numbers, kept apart from the drawing so `make
 * test` covers it.
 */

#ifndef RPOD_WII_FRAMES_H
#define RPOD_WII_FRAMES_H

#include "audio/mpd_client.h"

#include <stdbool.h>

/* The playmode art's: 0 stop, 1 play, 2 pause. */
static inline int rpod_wii_playmode_frame(rpod_mpd_state_t state)
{
    switch (state) {
    case RPOD_MPD_STATE_PLAY:
        return 1;
    case RPOD_MPD_STATE_PAUSE:
        return 2;
    default:
        return 0;
    }
}

/* The repeat art's: 0 off, 1 all, 2 one. NULL (no MPD) reads as off. */
static inline int rpod_wii_repeat_frame(const rpod_mpd_status_t *status)
{
    if (status == NULL || !status->repeat) {
        return 0;
    }
    return status->single ? 2 : 1;
}

/* The shuffle art's: 0 off, 1 on. */
static inline int rpod_wii_shuffle_frame(const rpod_mpd_status_t *status)
{
    return status != NULL && status->random ? 1 : 0;
}

/* The battery art's 25 frames: 12 draining (full to empty), 12 charging (the
 * same, with a bolt), then "charged"; -1 without a battery. */
static inline int rpod_wii_battery_level_frame(bool present, int level, bool external, bool charging)
{
    if (!present) {
        return -1;
    }
    if (external && !charging) {
        return 24;
    }
    level = level < 0 ? 0 : level > 100 ? 100 : level;
    int drained = 11 - level * 12 / 101;
    return charging ? 12 + drained : drained;
}

/* The volume art's 21 rings, in 5% steps from muted; an unknown volume (< 0)
 * is the muted one. */
static inline int rpod_wii_volume_frame(int volume)
{
    if (volume <= 0) {
        return 0;
    }
    return volume >= 100 ? 20 : (volume * 20 + 50) / 100;
}

#endif /* RPOD_WII_FRAMES_H */
