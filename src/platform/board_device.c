/*
 * The on-device board: the 2" ST7789V panel (320x240 landscape, via fbtft's
 * /dev/fb1 -- docs/PLAN.md §5.2) plus the click wheel.
 *
 * Not compiled into the simulator (which builds its own SDL board inline in
 * tools/sim/sim_main.c) -- this file pulls in fbdev.
 */

#include "platform/board.h"

#include "input/wheel_input.h"
#include "ui/lvgl_port.h"

#include <stdio.h>
#include <stdlib.h>

/* RPOD_FB overrides the framebuffer node for an unusual setup. */
static lv_display_t *create_display_fbdev(void)
{
    const char *fb = getenv("RPOD_FB");
    return rpod_lvgl_port_init(fb != NULL ? fb : "/dev/fb1");
}

/* Input is the click wheel over daemon/rpod-wheel.c's socket
 * (src/input/wheel_input.c). RPOD_WHEEL_SOCK overrides the socket path. */
static lv_indev_t *create_input_wheel(const rpod_input_buttons_t *buttons)
{
    return rpod_wheel_input_create(getenv("RPOD_WHEEL_SOCK"), buttons);
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
