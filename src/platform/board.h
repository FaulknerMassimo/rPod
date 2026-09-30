/*
 * Board / platform abstraction: the seam between the shared app
 * (src/app.c) and whatever it runs on. A board bundles a display backend
 * and an input backend -- fbdev + the click wheel on device
 * (src/platform/board_device.c), an SDL window + the keyboard in the
 * desktop simulator (tools/sim/sim_main.c) -- so the same screen graph
 * drives both.
 *
 * See docs/PLAN.md §5 (Display) and §5.4 (UI framework / simulator).
 */

#ifndef RPOD_BOARD_H
#define RPOD_BOARD_H

#include "input/input.h"
#include "lvgl.h"

typedef struct rpod_board {
    const char *name; /* human-readable, for logs */

    /* Creates and returns the LVGL display for this board (fbdev on device,
     * an SDL window in the simulator). NULL on failure. Called once, after
     * lv_init(). */
    lv_display_t *(*create_display)(void);

    /* Creates the ENCODER indev and wires the four app-level buttons to
     * `buttons`. Returns the indev to hand to the screen stack. Called once,
     * after create_display() and after the MPD client the buttons act on
     * exists (so `buttons->ctx` is valid). */
    lv_indev_t *(*create_input)(const rpod_input_buttons_t *buttons);
} rpod_board_t;

/* The on-device board (src/platform/board_device.c). The simulator builds
 * its own board inline instead. Never returns NULL. */
const rpod_board_t *rpod_device_board(void);

#endif /* RPOD_BOARD_H */
