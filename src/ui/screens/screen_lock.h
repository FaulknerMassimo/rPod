/*
 * Extras > Screen Lock (docs/PLAN.md §8.5), the later iPods': a four-digit
 * combination, set with the wheel a digit at a time (ui/spinner.h), shuts
 * everything but the music's own buttons away until it's entered again.
 * Menu can't leave the lock (screen_stack.h), while Play/Pause, Next and
 * Prev work as ever; it holds through sleep, a theme change and a restart.
 * The combination and whether it's locked persist in a small file --
 * deleting it over SSH is the way back in from a forgotten one.
 */

#ifndef RPOD_SCREEN_LOCK_H
#define RPOD_SCREEN_LOCK_H

#include "screen_stack.h"

/* Loads the combination, and whether it's locked, from state_path (parent
 * directory must exist; NULL keeps them in memory only). Call once. */
void rpod_screen_lock_init(const char *state_path);

/* Puts the lock back up if rPod was locked when it stopped. Call after the
 * root screen's pushed. */
void rpod_screen_lock_restore(rpod_screen_stack_t *stack);

/* build_fn for Extras > Screen Lock's menu: Set Combination, or Lock and
 * Change Combination. No ctx. */
void rpod_screen_lock_menu_build(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx);

#endif /* RPOD_SCREEN_LOCK_H */
