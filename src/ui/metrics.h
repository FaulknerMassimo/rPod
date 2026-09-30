/*
 * UI metrics for the 320x240 landscape panel (the 2" ST7789V, docs/PLAN.md
 * §5): screen size, status-bar height, the typographic scale, and list-row
 * geometry. Screens read rpod_metrics() rather than hardcoding these, so the
 * layout's shared numbers live in one place (src/ui/metrics.c).
 */

#ifndef RPOD_METRICS_H
#define RPOD_METRICS_H

#include "lvgl.h"

typedef struct {
    int32_t screen_w;
    int32_t screen_h;
    int32_t header_h; /* persistent top status bar height (ui/status_bar.c) */

    /* Typographic scale. */
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
    int32_t list_margin; /* inset of the list card from the screen edges */
} rpod_metrics_t;

const rpod_metrics_t *rpod_metrics(void);

#endif /* RPOD_METRICS_H */
