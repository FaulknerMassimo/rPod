/*
 * Alphabet scrub (docs/PLAN.md §8.2): a fast flick through a long
 * alphabetical list jumps letter by letter instead of row by row, with the
 * letter shown big in the middle of the screen. The input backend decides
 * when a flick is fast enough (src/input/wheel_input.c); this sends each jump
 * to whichever screen is up and draws the letter.
 *
 * A screen with a letter index handles rpod_scrub_event() on its screen
 * object: rpod_list_screen_enable_scrub() (ui/screens/list_screen.h) for the
 * plain browse lists, and the virtual Songs list in music_screens.c. Lists
 * are put in that order with ui/alpha_sort.h.
 */

#ifndef RPOD_SCRUB_H
#define RPOD_SCRUB_H

#include "lvgl.h"
#include <stdbool.h>

/* A list shorter than this keeps scrolling rows: a fast turn already
 * crosses it, and letters would only skip past what's on screen. */
#define RPOD_SCRUB_MIN_ROWS 25

/* The event's parameter (lv_event_get_param()). */
typedef struct {
    int dir;      /* next letter (+1), previous (-1), or stay put (0) */
    char letter;  /* handler: the letter the selection is now on */
    bool handled; /* handler: set if this screen has a letter index */
} rpod_scrub_param_t;

/* The custom LVGL event code a screen object gets. */
uint32_t rpod_scrub_event(void);

/* Creates the letter overlay, hidden, on LVGL's system layer. After
 * rpod_hud_init(), so it draws above the status bar and HUD. Call once. */
void rpod_scrub_init(void);

/* Sends a jump to the active screen and shows the letter it lands on.
 * False, showing nothing, if that screen has no letter index. */
bool rpod_scrub(int dir);

#endif /* RPOD_SCRUB_H */
