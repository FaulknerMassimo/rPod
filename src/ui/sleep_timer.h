/*
 * Settings > Sleep Timer (docs/PLAN.md §8.1): pauses and puts rPod to sleep
 * after a set time, as holding Play/Pause does. One-shot, and not kept
 * across restarts -- like the iPod's. Sleeping by hand cancels it.
 */

#ifndef RPOD_SLEEP_TIMER_H
#define RPOD_SLEEP_TIMER_H

#include <stdint.h>

/* fire(ctx) is the sleep itself (the app's). Call once, after lv_init(). */
void rpod_sleep_timer_init(void (*fire)(void *ctx), void *ctx);

/* Starts the timer `minutes` from now, replacing any running one; 0 turns
 * it off. */
void rpod_sleep_timer_set(unsigned minutes);

/* What the running timer was set to, in minutes; 0 when it's off. */
unsigned rpod_sleep_timer_minutes(void);

/* Time left on the running timer; 0 when it's off. */
uint32_t rpod_sleep_timer_remaining_ms(void);

#endif /* RPOD_SLEEP_TIMER_H */
