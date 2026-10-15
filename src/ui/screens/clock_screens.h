/*
 * Extras > Clock (docs/PLAN.md §8.5), the iPod's: the time and date, big,
 * over Alarm Clock and Sleep Timer -- and the alarm going off (ui/alarm.h),
 * as a card over whatever's on screen.
 */

#ifndef RPOD_CLOCK_SCREENS_H
#define RPOD_CLOCK_SCREENS_H

#include "screen_stack.h"

/* build_fn for rpod_screen_stack_push(). `ctx` is the shared rpod_mpd_t*
 * connection -- not owned here, so pass NULL as ctx_free when pushing. */
void rpod_clock_screen_build(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx);

/* The alarm's card -- Snooze or Stop -- over whatever's on screen, in place
 * of any other overlay. Menu closes it with the music playing on. */
void rpod_alarm_card_open(rpod_screen_stack_t *stack);

#endif /* RPOD_CLOCK_SCREENS_H */
