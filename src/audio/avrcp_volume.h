/*
 * A Bluetooth headset's own volume buttons, as rPod volume steps
 * (docs/PLAN.md §6.3). AirPods report a stem swipe, and most headphones their
 * volume buttons, by changing their AVRCP absolute volume -- which BlueZ
 * shows as MediaTransport1.Volume (0-127). rPod keeps the headset's own
 * volume pinned (system/wireplumber/: MPD's software mixer is the one volume
 * control), so a change there is turned into steps on MPD's volume and the
 * headset is set back to where it was pinned.
 *
 * Pure logic, no I/O: audio/bluetooth.c feeds it the transport's Volume and
 * State and does what it says. `make test` covers it.
 */

#ifndef RPOD_AVRCP_VOLUME_H
#define RPOD_AVRCP_VOLUME_H

#include <stdbool.h>
#include <stdint.h>

/* AVRCP volume per step: iOS moves AirPods 1/16th of the range a press. */
#define RPOD_AVRCP_VOLUME_STEP 8
/* After streaming starts, PipeWire sets the headset to its pinned level and
 * the headset reports its own -- setup, not buttons. Changes this soon after
 * just move the pin. */
#define RPOD_AVRCP_SETTLE_MS   2500

typedef struct {
    int anchor;          /* the level the headset is pinned at; -1 until known */
    int last;            /* the level it last reported */
    bool active;         /* streaming: only then is a change a button press */
    uint32_t settle_until;
    bool repin_pending;  /* we set it back to `anchor`; its echo is coming */
    bool broken;         /* setting it back failed: leave the headset alone */
} rpod_avrcp_volume_t;

void rpod_avrcp_volume_init(rpod_avrcp_volume_t *v);

/* The transport started (active) or stopped streaming. */
void rpod_avrcp_volume_state(rpod_avrcp_volume_t *v, bool active, uint32_t now_ms);

/* The headset's volume is now `value`. Returns how many steps to move MPD's
 * volume (> 0 louder, 0 for none) and sets *repin to the level to set the
 * headset back to, or -1 to leave it. */
int rpod_avrcp_volume_changed(rpod_avrcp_volume_t *v, int value, uint32_t now_ms, int *repin);

/* Setting the level back failed (BlueZ refused, say): from now on changes
 * pass through untouched rather than being counted twice. */
void rpod_avrcp_volume_repin_failed(rpod_avrcp_volume_t *v);

#endif /* RPOD_AVRCP_VOLUME_H */
