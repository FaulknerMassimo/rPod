/*
 * What an iPhone shows when the AirPods do something (docs/PLAN.md §6.3), on
 * top of audio/airpods.h:
 *
 *   - The battery card: the buds and the case with their charge, sliding up
 *     from the bottom when the AirPods connect -- which, now they reconnect
 *     by themselves, is when the case opens near rPod -- and again when the
 *     case starts reporting while they're connected (a bud going back in).
 *     It goes away after a few seconds, or at any press or turn.
 *   - HUD messages (ui/hud.h): noise control switching mode (a stem press
 *     and hold, or Settings), a bud running low, the AirPods disconnecting.
 */

#ifndef RPOD_AIRPODS_NOTIFY_H
#define RPOD_AIRPODS_NOTIFY_H

#include "ui/screens/screen_stack.h"

/* Starts following the AirPods. The card opens as `stack`'s overlay (or, if
 * another overlay is open, becomes a HUD message instead). Call once, after
 * rpod_airpods_init() and rpod_hud_init(). */
void rpod_airpods_notify_init(rpod_screen_stack_t *stack);

#endif /* RPOD_AIRPODS_NOTIFY_H */
