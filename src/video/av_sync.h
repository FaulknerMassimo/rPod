/*
 * Lining a video's picture up with its sound (docs/PLAN.md §8.7). MPD's
 * position -- what the picture follows -- is the last bit of sound it handed
 * an output, which then holds some before it's heard: ALSA's ring buffer
 * (half a second by MPD's default) on the headphone jack, PipeWire's graph
 * and the Bluetooth link to a headset. So the picture runs that much behind
 * MPD's position.
 *
 * The ALSA delay is read off the kernel while MPD plays
 * (/proc/asound/.../status); the rest are estimates by output type, and on
 * top of them all is the user's own trim, Settings > Video Sync in the web
 * UI (src/ui/control.c), persisted like the backlight's timeout.
 */

#ifndef RPOD_AV_SYNC_H
#define RPOD_AV_SYNC_H

#include "audio/mpd_client.h"

/* The trim's range: + shows the picture later (the sound was late). */
#define RPOD_AV_SYNC_TRIM_MAX 1000

/* Loads the trim from state_path (NULL: in memory only). */
void rpod_av_sync_init(const char *state_path);

int rpod_av_sync_trim_ms(void);
void rpod_av_sync_set_trim_ms(int ms);

/* How far behind MPD's position the sound of its enabled outputs is
 * heard, plus the trim: what the picture lags it by. One MPD round trip. */
int rpod_av_sync_delay_ms(rpod_mpd_t *mpd);

#endif /* RPOD_AV_SYNC_H */
