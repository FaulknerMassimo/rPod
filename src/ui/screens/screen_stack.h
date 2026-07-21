/*
 * Push/pop screen navigation, iPod-menu style. Each screen gets its own
 * lv_group_t (LVGL's keypad/encoder demo pattern: make it the default group
 * before building the screen so its focusable widgets auto-join it), and
 * popping reassigns the input device's group back to the previous screen's
 * before deleting the old one. This is what makes "Menu = back" work without
 * any screen needing to know about navigation itself.
 */

#ifndef RPOD_SCREEN_STACK_H
#define RPOD_SCREEN_STACK_H

#include "lvgl.h"
#include <stddef.h>

typedef struct rpod_screen_stack rpod_screen_stack_t;

/* Populates a freshly created, empty `screen`. Widgets created here join
 * whichever lv_group is default at the time -- the stack sets that up
 * before calling this, so builders don't need to touch groups at all. */
typedef void (*rpod_screen_build_fn)(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx);

rpod_screen_stack_t *rpod_screen_stack_create(lv_indev_t *indev);

/* Builds and loads a new screen on top of the stack. `ctx` is passed through
 * to `build` and, if non-NULL, `ctx_free` is called on it when this screen
 * is later popped -- use it for any heap allocation a screen needs to keep
 * alive for its own lifetime (e.g. "which artist am I browsing"). */
void rpod_screen_stack_push(rpod_screen_stack_t *stack, rpod_screen_build_fn build, void *ctx,
                             void (*ctx_free)(void *ctx));

/* Returns to the previous screen, deleting the current one. No-op at depth
 * 1 -- the root screen (Main Menu) never pops. */
void rpod_screen_stack_pop(rpod_screen_stack_t *stack);

size_t rpod_screen_stack_depth(const rpod_screen_stack_t *stack);

#endif /* RPOD_SCREEN_STACK_H */
