/*
 * Per-device volume: remembers the volume last used with each Bluetooth
 * audio device (keyed by its address), plus one level for when none is
 * connected ("wired"), and switches MPD's volume between them as devices
 * connect and disconnect -- so AirPods come back at the AirPods' level and a
 * speaker at the speaker's, the way a phone does it.
 *
 * Levels are MPD's own (software mixer) volume, whatever changed it -- the
 * wheel, or another client -- read once a second, and persisted to a small
 * text file so they survive restarts.
 */

#ifndef RPOD_VOLUME_MEMORY_H
#define RPOD_VOLUME_MEMORY_H

#include "audio/mpd_client.h"

/* Starts watching Bluetooth (audio/bluetooth.h, so after rpod_bt_init()) and
 * polling `mpd`, which must outlive it. state_path is where levels persist
 * (parent directory must exist); NULL keeps them in memory only. Call once,
 * after lv_init(). */
void rpod_volume_memory_init(rpod_mpd_t *mpd, const char *state_path);

/* Bumped every time a device switch sets MPD's volume to that device's
 * level. A screen holding its own copy of the volume (Now Playing, mid-turn)
 * should re-read it when this changes rather than step on from a level that
 * belonged to the previous device. 0 until the first switch. */
unsigned rpod_volume_memory_generation(void);

/* Conversation Awareness (audio/airpods.c): lowers MPD's volume to `percent`
 * of the level it had when ducking began, while the wearer is talking; 100
 * puts it back. The lowered level is never remembered as the device's own.
 * Turning the volume while ducked ends it, at whatever level was picked. */
void rpod_volume_memory_duck(unsigned percent);

#endif /* RPOD_VOLUME_MEMORY_H */
