/*
 * Keyboard stand-in for the click wheel, for UI development without
 * hardware (docs/PLAN.md §5.4's simulator loop). See sim_input.c for the
 * key mapping and why it's built the way it is.
 */

#ifndef RPOD_SIM_INPUT_H
#define RPOD_SIM_INPUT_H

#include "lvgl.h"

typedef struct {
    void (*on_menu)(void *ctx);
    void (*on_play_pause)(void *ctx);
    void (*on_next)(void *ctx);
    void (*on_prev)(void *ctx);
    void *ctx;
} rpod_sim_buttons_t;

/* Sets up keyboard input: Left/Right arrows = wheel rotate, Enter =
 * center/select (both via LVGL's normal ENCODER indev/group machinery, so
 * screens don't need to know the input isn't the real wheel), and
 * M/Space/N/P = the wheel's four physical buttons (Menu/Play-Pause/
 * Next/Prev, docs/PLAN.md §8.2), dispatched straight to `buttons` since
 * those are app-level shortcuts rather than part of the encoder pair.
 * See sim_input.c for why the encoder is a custom read callback rather
 * than the obvious lv_sdl_keyboard_create() shortcut.
 *
 * Returns the encoder indev, for the caller to hand to
 * rpod_screen_stack_create(). */
lv_indev_t *rpod_sim_input_init(const rpod_sim_buttons_t *buttons);

#endif /* RPOD_SIM_INPUT_H */
