/*
 * Runtime UI metrics. rPod's screens were originally written for one panel
 * (320x240 landscape) with the geometry and fonts baked in as compile-time
 * macros. To support multiple boards from one binary (docs/PLAN.md §5's
 * multi-board subsection), those values now come from a form-factor profile
 * chosen at startup by the active board (src/platform/board.h).
 *
 * A screen reads rpod_metrics() instead of the old RPOD_SCREEN_WIDTH /
 * RPOD_HEADER_HEIGHT / &lv_font_montserrat_16 constants. The landscape
 * profile reproduces the original values exactly, so the classic board
 * renders byte-for-byte as before; the square profile (128x128) shrinks
 * fonts and metrics for the Waveshare 1.44" LCD HAT. Screens whose absolute
 * layout can't just scale (Now Playing, Search) branch on `form`.
 */

#ifndef RPOD_METRICS_H
#define RPOD_METRICS_H

#include "platform/board.h"
#include "lvgl.h"

typedef struct {
    rpod_form_factor_t form;

    int32_t screen_w;
    int32_t screen_h;
    int32_t header_h; /* persistent top status bar height (ui/status_bar.c) */

    /* Typographic scale. font_small omits FontAwesome glyphs in the square
     * profile (see lv_conf.h) -- widgets showing an LV_SYMBOL_* must use
     * font_body or larger there. */
    const lv_font_t *font_title;    /* Now Playing title, prominent text */
    const lv_font_t *font_body;     /* list rows, menu items, search field */
    const lv_font_t *font_small;    /* subtitles, accessory, keys, results, headers */
    const lv_font_t *font_np_glyph; /* big placeholder glyph on the Now Playing art tile */

    /* List-row metrics (ui/screens/list_screen.c). */
    int32_t list_art_size;
    int32_t row_pad_x;
    int32_t row_pad_y;
    int32_t row_gap;
    int32_t row_heart_size;
} rpod_metrics_t;

/* Selects the profile for `form`. Call once at startup, before any screen
 * is built (rpod_app_run() does this right after lv_init()). */
void rpod_metrics_init(rpod_form_factor_t form);

/* The active profile. Defaults to the landscape profile until
 * rpod_metrics_init() runs, so early/static callers are always safe. */
const rpod_metrics_t *rpod_metrics(void);

#endif /* RPOD_METRICS_H */
