/*
 * Extras > Home Assistant (docs/PLAN.md §8.8): the server's devices by kind
 * -- Lights, Switches, Covers, Locks, Climate, Scenes... -- and Center
 * works them. A light with a brightness, and a thermostat, open a page of
 * their own (power, and a number set with the wheel); everything else
 * does its one thing on Center. The state comes from ha/ha.h, refreshed
 * every few seconds while these screens are up.
 */

#ifndef RPOD_HA_SCREENS_H
#define RPOD_HA_SCREENS_H

#include "screen_stack.h"

/* build_fn for rpod_screen_stack_push(); `ctx` is unused (pass NULL). */
void rpod_ha_screen_build(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx);

#endif /* RPOD_HA_SCREENS_H */
