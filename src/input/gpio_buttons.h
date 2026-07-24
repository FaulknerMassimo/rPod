/*
 * On-device button/joystick input via libgpiod (character device, no root
 * needed with the `gpio` group -- see system/systemd/rpod.service). Reads a
 * board's buttons directly and drives the shared ENCODER indev
 * (src/input/encoder.c) plus the four app-level button callbacks, so the UI
 * can't tell it apart from the click wheel or the simulator's keyboard.
 *
 * Written for the Waveshare 1.44" LCD HAT (5-way joystick + KEY1/2/3), but
 * the pin map is a parameter so any GPIO-button board can reuse it.
 */

#ifndef RPOD_GPIO_BUTTONS_H
#define RPOD_GPIO_BUTTONS_H

#include "input/input.h"
#include "lvgl.h"

/* BCM GPIO offsets on one gpiochip. All lines are active-low with a pull-up
 * bias (pressed == 0), which is how the HAT's buttons are wired. A field set
 * to -1 is unused. */
typedef struct {
    const char *chip; /* e.g. "/dev/gpiochip0"; NULL -> that default */
    int up;
    int down;
    int left;
    int right;
    int center; /* joystick press = encoder select/enter */
    int key1;
    int key2;
    int key3;
} rpod_gpio_pinmap_t;

/* Requests the lines in `pins`, wires them to the input model, and returns the
 * ENCODER indev to hand to rpod_screen_stack_create(). Never returns NULL: if
 * the GPIO chip can't be opened (e.g. missing permissions) it logs and returns
 * a bare encoder so the display still comes up -- input is simply dead until
 * the environment is fixed. */
lv_indev_t *rpod_gpio_input_init(const rpod_gpio_pinmap_t *pins,
                                 const rpod_input_buttons_t *buttons);

#endif /* RPOD_GPIO_BUTTONS_H */
