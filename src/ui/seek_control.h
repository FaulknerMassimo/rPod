/*
 * Scanning through the current song with Next/Prev held (docs/PLAN.md §8.2).
 * Playback pauses for it -- a scan is silent -- and the position glides
 * along at a speed that builds the longer it's held. Letting go seeks there
 * in one step and plays on if it was playing. Forward into the last moments
 * of the song goes on to the next one instead.
 *
 * While a scan is on, MPD still holds the old position, so Now Playing shows
 * rpod_seek_control_position() instead (ui/screens/now_playing.c).
 */

#ifndef RPOD_SEEK_CONTROL_H
#define RPOD_SEEK_CONTROL_H

#include "audio/mpd_client.h"

#include <stdbool.h>

/* `mpd` must outlive it. Call once, after lv_init(). */
void rpod_seek_control_init(rpod_mpd_t *mpd);

/* Starts scanning forward (dir > 0) or back. Does nothing with no song
 * loaded, or while a scan is already on. */
void rpod_seek_control_start(int dir);

/* Let go: seek to where the scan got to and resume. No-op if not scanning. */
void rpod_seek_control_stop(void);

/* True while scanning, with where it's got to and the song's length (0 if
 * unknown). */
bool rpod_seek_control_position(unsigned *pos_ms, unsigned *duration_ms);

#endif /* RPOD_SEEK_CONTROL_H */
