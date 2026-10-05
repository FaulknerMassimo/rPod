/*
 * The AirPods' settings page (docs/PLAN.md §6.3, §8.1), iOS-style, on top of
 * audio/airpods.h. Reached from Settings > Bluetooth for any paired device
 * that speaks AAP, and from the top of Settings while one is connected:
 *
 *   <name>                    L 80%  R 78%  Case 45%
 *   NOISE CONTROL             Noise Cancellation / Adaptive / Transparency /
 *                             Off -- a checkmark on the current mode
 *   AUDIO                     Adaptive Audio >, Conversation Awareness,
 *                             Personalized Volume, Automatic Ear Detection
 *   Press and Hold >          which modes the stem cycles; Off Listening Mode
 *   Accessibility >           press speed, hold duration, one-AirPod noise
 *                             cancellation, volume swipe and its speed
 *   About >                   model, serial number, version
 *   Connect/Disconnect, Forget This Device
 *
 * A row only appears for a setting these AirPods report, so a model without
 * noise control simply has no such rows. Every screen rebuilds as the
 * AirPods report changes (a stem press switching modes, a bud charging).
 */

#ifndef RPOD_AIRPODS_SCREENS_H
#define RPOD_AIRPODS_SCREENS_H

#include "screen_stack.h"

#include <stddef.h>

/* Pushes the settings page for the AirPods at BlueZ device `path`. */
void rpod_airpods_screen_push(rpod_screen_stack_t *stack, const char *path);

/* "L 80%  R 78%  Case 45%" for the connected AirPods, with a bolt on
 * whatever's charging; "" while nothing's reported. */
void rpod_airpods_battery_text(char *out, size_t out_size);

#endif /* RPOD_AIRPODS_SCREENS_H */
