/*
 * The volume, as the user steers it (docs/PLAN.md §6.3): MPD's software
 * mixer -- the one volume control -- stepped by the wheel on Now Playing and
 * by a Bluetooth headset's own buttons (an AirPods stem swipe; see
 * audio/bluetooth.h), and shown on the HUD (ui/hud.h) either way, with the
 * AirPods' glyph instead of a speaker while they're connected.
 */

#ifndef RPOD_VOLUME_CONTROL_H
#define RPOD_VOLUME_CONTROL_H

#include "audio/mpd_client.h"

/* Starts taking headset volume buttons. `mpd` must outlive it. Call once,
 * after rpod_bt_init() and rpod_hud_init(). */
void rpod_volume_control_init(rpod_mpd_t *mpd);

/* One step louder (dir > 0) or quieter. Shows the HUD straight away; MPD
 * gets the new level once the current batch of steps is in. */
void rpod_volume_control_step(int dir);

#endif /* RPOD_VOLUME_CONTROL_H */
