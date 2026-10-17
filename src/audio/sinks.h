/*
 * Where the sound goes (docs/PLAN.md §6.3b): the Bluetooth headset, or an
 * AirPlay speaker on the network.
 *
 *   Bluetooth  MPD's pipewire output plays into PipeWire's default sink and
 *              WirePlumber moves it when the default changes: `wpctl
 *              set-default`.
 *   AirPlay    MPD's "AirPlay feed" fifo output carries the sound to
 *              OwnTone (system/owntone/), which speaks AirPlay 2 and finds
 *              the speakers. Choosing a speaker selects it in OwnTone, then
 *              switches MPD's Bluetooth output off and the feed on, so
 *              nothing plays twice; choosing the headset does the reverse.
 *              These two MPD outputs are this file's to switch.
 *
 * Every call (`pw-dump`, `wpctl`, OwnTone's HTTP API over curl, MPD) runs on
 * a worker thread with a timeout, never on the LVGL thread; screens read a
 * snapshot and a watcher tells them when it changes.
 *
 * Needs RPOD_PIPEWIRE_DIR (the device's service sets /run/pipewire), the
 * system-wide instance's runtime directory. Without it -- the sim, so it
 * can't change the desktop's own default sink -- there are no sinks, unless
 * RPOD_SINKS_FAKE is set, which makes up a headset and two speakers.
 */

#ifndef RPOD_SINKS_H
#define RPOD_SINKS_H

#include "sinks_parse.h"

#include "lvgl.h"

/* system/mpd/mpd.conf's outputs for the headset and for OwnTone. */
#define RPOD_SINKS_BT_OUTPUT   "Bluetooth"
#define RPOD_SINKS_FEED_OUTPUT "AirPlay feed"

/* How far a speaker plays behind MPD: OwnTone's start_buffer_ms
 * (system/owntone/owntone.conf), plus what it has read ahead of that by
 * the time playback starts. A playing video allows for it
 * (video/av_sync.c), and a pause moves MPD back by it. */
#define RPOD_SINKS_AIRPLAY_DELAY_MS 1600

/* Reads the environment and starts a first refresh. Call once, after
 * lv_init(), with the MPD socket (the worker keeps its own connection). */
void rpod_sinks_init(const char *mpd_socket);

/* False when there's no PipeWire to ask (see above). */
bool rpod_sinks_available(void);

/* Asks PipeWire again, in the background; the snapshot changes when it answers. */
void rpod_sinks_refresh(void);

/* Copies the snapshot: up to `max` sinks. */
size_t rpod_sinks_list(rpod_sink_t *out, size_t max);

/* Plays on `sink`, in the background. */
void rpod_sinks_select(const rpod_sink_t *sink);

/* True while an AirPlay speaker is selected. */
bool rpod_sinks_airplay_active(void);

/* The same, also copying that speaker's OwnTone id (digits; its `ref`, which
 * stays the same across restarts) into `ref`. */
bool rpod_sinks_current_speaker(char *ref, size_t ref_size);

/* Calls cb(user) on the LVGL thread when the snapshot changes, until `owner`
 * is deleted. While watched it also refreshes every few seconds, so a speaker
 * that turns on shows up. */
void rpod_sinks_watch(lv_obj_t *owner, void (*cb)(void *user), void *user);

#endif /* RPOD_SINKS_H */
