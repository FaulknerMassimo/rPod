/*
 * The on-device board: the 2" ST7789V panel (320x240 landscape, via fbtft's
 * /dev/fb1 -- docs/PLAN.md §5.2) plus the click wheel.
 *
 * Not compiled into the simulator (which builds its own SDL board inline in
 * tools/sim/sim_main.c) -- this file pulls in fbdev.
 */

#include "platform/board.h"

#include "input/encoder.h"
#include "ui/lvgl_port.h"

#include <stdio.h>
#include <stdlib.h>

/* RPOD_FB overrides the framebuffer node for an unusual setup. */
static lv_display_t *create_display_fbdev(void)
{
    const char *fb = getenv("RPOD_FB");
    return rpod_lvgl_port_init(fb != NULL ? fb : "/dev/fb1");
}

/* Input is the click wheel over daemon/rpod-wheel.c's socket. That UI-side
 * indev isn't written yet (and the wheel hardware is currently dead -- see
 * CLAUDE.md), so this returns a bare encoder: the display still comes up,
 * navigation is simply inert until a wheel indev exists. */
static lv_indev_t *create_input_wheel(const rpod_input_buttons_t *buttons)
{
    (void)buttons;
    fprintf(stderr, "rpod: click-wheel input is not wired on-device yet; navigation will be inert.\n");
    return rpod_encoder_create();
}

static const rpod_board_t k_board = {
    .name = "2\" ST7789V 320x240 + click wheel",
    .create_display = create_display_fbdev,
    .create_input = create_input_wheel,
};

const rpod_board_t *rpod_device_board(void)
{
    return &k_board;
}
