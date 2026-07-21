/*
 * Minimal shared look-and-feel: a handful of colours and a header-bar
 * builder used consistently by every screen. Not a full style framework —
 * docs/PLAN.md §8's two-pane layout and iconography are a later pass.
 */

#ifndef RPOD_THEME_H
#define RPOD_THEME_H

#include "lvgl.h"

#define RPOD_COLOR_BG        lv_color_hex(0x000000)
#define RPOD_COLOR_TEXT      lv_color_hex(0xe8e8e8)
#define RPOD_COLOR_HEADER_BG lv_color_hex(0x1c1c1e)
#define RPOD_COLOR_ACCENT    lv_color_hex(0x0a84ff)
#define RPOD_COLOR_DIM_TEXT  lv_color_hex(0x8e8e93)

#define RPOD_HEADER_HEIGHT 28

/* Landscape 320x240 logical framebuffer, on-device and in the simulator
 * alike (docs/PLAN.md §5). */
#define RPOD_SCREEN_WIDTH  320
#define RPOD_SCREEN_HEIGHT 240

/* Applies the background/text colours to a freshly created screen object. */
void rpod_theme_style_screen(lv_obj_t *screen);

/* Creates a full-width title bar docked to the top of `screen`. Returns the
 * header container (rarely needed by callers beyond layout purposes). */
lv_obj_t *rpod_theme_create_header(lv_obj_t *screen, const char *title);

#endif /* RPOD_THEME_H */
