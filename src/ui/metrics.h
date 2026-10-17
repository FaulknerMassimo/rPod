/*
 * UI metrics for the 320x240 landscape panel (the 2" ST7789V, docs/PLAN.md
 * §5): screen size, status-bar height, the typographic scale, and list-row
 * geometry. Screens read rpod_metrics() rather than hardcoding these, so the
 * layout's shared numbers live in one place (src/ui/metrics.c). They follow
 * the active theme (ui/theme.h): the classic themes lay the iPod 4G's own
 * 160x128 screen out at 2x.
 */

#ifndef RPOD_METRICS_H
#define RPOD_METRICS_H

#include "lvgl.h"

typedef struct {
    int32_t screen_w;
    int32_t screen_h;
    int32_t header_h; /* persistent top status bar height (ui/status_bar.c) */
    /* ...and what the bar takes from the bottom, when it's at the bottom
     * (the Wii's, ui/wii.h): screens lay out in screen_h - header_h -
     * footer_h. 0 in the other themes. */
    int32_t footer_h;

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
    int32_t row_h;       /* fixed row height, or 0 to fit the row's content */
} rpod_metrics_t;

/* The active theme's. */
const rpod_metrics_t *rpod_metrics(void);

/* Where a screen's content ends: the bottom of the panel, or of the Wii's bar
 * when it's up. */
static inline int32_t rpod_metrics_bottom(const rpod_metrics_t *m)
{
    return m->screen_h - m->footer_h;
}

/* How far below the middle of the screen the middle of the area between the
 * bars is, for aligning something LV_ALIGN_CENTER. */
static inline int32_t rpod_metrics_mid_dy(const rpod_metrics_t *m)
{
    return (m->header_h - m->footer_h) / 2;
}

#endif /* RPOD_METRICS_H */
