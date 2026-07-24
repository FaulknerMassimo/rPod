/*
 * Board / platform abstraction. rPod supports several hardware targets
 * (and the desktop simulator), selected at runtime -- on device via the
 * RPOD_BOARD environment variable (see src/platform/board_device.c). A
 * board bundles a display backend, an input backend, and a UI form factor
 * so the same app binary drives any of them.
 *
 * See docs/PLAN.md §5 (Display) and the multi-board subsection.
 */

#ifndef RPOD_BOARD_H
#define RPOD_BOARD_H

#include "input/input.h"
#include "lvgl.h"

/* Which UI metrics profile a board renders with (src/ui/metrics.c). The
 * screen graph is one codebase; only sizes/fonts/layout branches differ. */
typedef enum {
    RPOD_FORM_LANDSCAPE, /* 320x240 landscape -- the 2" ST7789V click-wheel build */
    RPOD_FORM_SQUARE,    /* 128x128 square    -- the Waveshare 1.44" LCD HAT */
} rpod_form_factor_t;

typedef struct rpod_board {
    const char *id;   /* RPOD_BOARD value this matches */
    const char *name; /* human-readable, for logs */
    rpod_form_factor_t form;

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

/* Selects the active board. On device this reads RPOD_BOARD and looks it up
 * in the registry (src/platform/board_device.c), defaulting to the classic
 * 320x240 build; the simulator builds its board inline. Never returns NULL. */
const rpod_board_t *rpod_board_select(void);

#endif /* RPOD_BOARD_H */
