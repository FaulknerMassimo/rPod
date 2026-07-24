/*
 * On-device board registry. rpod_board_select() maps the RPOD_BOARD
 * environment variable to one of these, defaulting to the classic 2" build.
 * Each board pairs a display backend (fbdev, via ui/lvgl_port.c) with an
 * input backend. See docs/PLAN.md §5's multi-board subsection.
 *
 * Not compiled into the simulator (which builds its own SDL board inline in
 * tools/sim/sim_main.c) -- this file pulls in fbdev + libgpiod.
 */

#include "platform/board.h"

#include "input/encoder.h"
#include "input/gpio_buttons.h"
#include "ui/lvgl_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Both current panels are driven through fbtft's /dev/fb1 (docs/PLAN.md §5.2).
 * RPOD_FB overrides the node for an unusual setup. */
static lv_display_t *create_display_fbdev(void)
{
    const char *fb = getenv("RPOD_FB");
    return rpod_lvgl_port_init(fb != NULL ? fb : "/dev/fb1");
}

/* Waveshare 1.44" LCD HAT: 5-way joystick + KEY1/2/3, all active-low with
 * pull-ups (docs/PLAN.md multi-board notes). BCM offsets -- verify against the
 * board on first bring-up. */
static const rpod_gpio_pinmap_t hat144_pins = {
    .chip = "/dev/gpiochip0",
    .up = 6,
    .down = 19,
    .left = 5,
    .right = 26,
    .center = 13,
    .key1 = 21,
    .key2 = 20,
    .key3 = 16,
};

static lv_indev_t *create_input_hat144(const rpod_input_buttons_t *buttons)
{
    return rpod_gpio_input_init(&hat144_pins, buttons);
}

/* The classic build's input is the click wheel over daemon/rpod-wheel.c's
 * socket. That UI-side indev isn't written yet (and the wheel hardware is
 * currently dead -- see CLAUDE.md), so this returns a bare encoder: the
 * display still comes up, navigation is simply inert until a wheel indev
 * exists. Use RPOD_BOARD=waveshare-144 for the working HAT. */
static lv_indev_t *create_input_wheel(const rpod_input_buttons_t *buttons)
{
    (void)buttons;
    fprintf(stderr, "rpod: classic click-wheel input is not wired on-device yet; "
                    "navigation will be inert. Set RPOD_BOARD=waveshare-144 for the LCD HAT.\n");
    return rpod_encoder_create();
}

static const rpod_board_t k_boards[] = {
    {
        .id = "classic",
        .name = "2\" ST7789V 320x240 + click wheel",
        .form = RPOD_FORM_LANDSCAPE,
        .create_display = create_display_fbdev,
        .create_input = create_input_wheel,
    },
    {
        .id = "waveshare-144",
        .name = "Waveshare 1.44\" LCD HAT (ST7735S 128x128 + joystick)",
        .form = RPOD_FORM_SQUARE,
        .create_display = create_display_fbdev,
        .create_input = create_input_hat144,
    },
};

static bool id_in(const char *id, const char *const *names)
{
    for (size_t i = 0; names[i] != NULL; i++) {
        if (strcmp(id, names[i]) == 0) {
            return true;
        }
    }
    return false;
}

const rpod_board_t *rpod_board_select(void)
{
    const char *id = getenv("RPOD_BOARD");
    if (id != NULL && id[0] != '\0') {
        static const char *const square_ids[] = { "waveshare-144", "waveshare144", "hat144",
                                                  "lcdhat", "1in44", "square", NULL };
        static const char *const classic_ids[] = { "classic", "landscape", NULL };
        if (id_in(id, square_ids)) {
            return &k_boards[1];
        }
        if (!id_in(id, classic_ids)) {
            fprintf(stderr, "rpod: unknown RPOD_BOARD='%s', using 'classic'\n", id);
        }
    }
    return &k_boards[0];
}
