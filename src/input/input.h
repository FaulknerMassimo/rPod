/*
 * Shared input contract between the UI and whichever backend feeds it.
 *
 * rPod's navigation runs through an LVGL ENCODER indev (rotate = move
 * focus, press = select) built once and handed to the screen stack. The
 * four *app-level* buttons -- Menu/back, Play-Pause, Next, Prev
 * (docs/PLAN.md §8.2) -- are not part of the encoder pair; a backend
 * dispatches them straight to these callbacks. Both the desktop simulator
 * (keyboard stand-in, tools/sim/sim_input.c) and the on-device backends
 * (click wheel, or the Waveshare HAT's joystick+keys via
 * src/input/gpio_buttons.c) fill this same struct, so the screens never
 * learn which board is driving them.
 */

#ifndef RPOD_INPUT_H
#define RPOD_INPUT_H

typedef struct {
    void (*on_menu)(void *ctx);        /* back one level; from root, sleep */
    void (*on_play_pause)(void *ctx);
    void (*on_next)(void *ctx);
    void (*on_prev)(void *ctx);
    void *ctx;
} rpod_input_buttons_t;

#endif /* RPOD_INPUT_H */
