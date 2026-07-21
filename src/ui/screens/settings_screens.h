/*
 * Settings menu (docs/PLAN.md §8.1). Audio Output is real (talks to MPD,
 * §6.2's DAC/Bluetooth output switching); Bluetooth/Backlight/Haptics/Sleep
 * Timer are honest placeholders since they need hardware this dev setup
 * doesn't have. About shows what's genuinely available without hardware
 * (storage, IP) alongside what isn't (battery).
 */

#ifndef RPOD_SETTINGS_SCREENS_H
#define RPOD_SETTINGS_SCREENS_H

#include "screen_stack.h"

/* build_fn for rpod_screen_stack_push(). `ctx` is the shared rpod_mpd_t*
 * connection -- not owned here, so pass NULL as ctx_free when pushing. */
void rpod_settings_menu_build(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx);

#endif /* RPOD_SETTINGS_SCREENS_H */
