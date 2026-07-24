/*
 * Minimal shared look-and-feel: a handful of colours and glass-surface
 * styling helpers used consistently across screens. Not a full style
 * framework — docs/PLAN.md §8's two-pane layout and iconography are a later
 * pass.
 */

#ifndef RPOD_THEME_H
#define RPOD_THEME_H

#include "lvgl.h"

#define RPOD_COLOR_BG        lv_color_hex(0x000000)
#define RPOD_COLOR_TEXT      lv_color_hex(0xe8e8e8)
#define RPOD_COLOR_HEADER_BG lv_color_hex(0x1c1c1e)
#define RPOD_COLOR_ACCENT    lv_color_hex(0x0a84ff)
#define RPOD_COLOR_DIM_TEXT  lv_color_hex(0x8e8e93)
#define RPOD_COLOR_SEPARATOR lv_color_hex(0x38383a)
/* "Loved" accent -- the fill colour of a liked-song heart (Apple Music's
 * pink-red). Distinct from the blue selection accent so a filled heart reads
 * as "liked" on both the dark glass and the accent-blue selection highlight. */
#define RPOD_COLOR_LIKE      lv_color_hex(0xff375f)

/* "Liquid glass" material: a translucent dark fill (so it reads as tinted
 * glass over both flat black screens and the blurred-artwork background on
 * Now Playing) plus a hairline highlight where light would catch a glass
 * edge. No real backdrop blur -- this is pure software rendering on a Pi
 * Zero 2 W with no GPU (see lv_conf.h's LV_USE_DRAW_SW), and PLAN.md #5.3
 * rules out full-frame work per frame. Where there's actually something
 * worth blurring (Now Playing's cover art), that's a one-time blur done on
 * track change, not a live per-frame effect -- see cover_art.c. */
#define RPOD_COLOR_GLASS_FILL lv_color_hex(0x2c2c2e)
#define RPOD_GLASS_FILL_OPA   150 /* ~59% */
#define RPOD_COLOR_GLASS_EDGE lv_color_hex(0xffffff)
#define RPOD_GLASS_EDGE_OPA   50 /* ~20% -- faint, not a solid line */

/* Screen geometry (width/height, status-bar header height) and the
 * typographic scale are no longer compile-time constants -- they come from
 * the active board's form-factor profile at runtime. Read them via
 * rpod_metrics() (ui/metrics.h): rpod_metrics()->screen_w / ->screen_h /
 * ->header_h and ->font_body etc. The landscape profile reproduces the old
 * 320x240 / 28px / montserrat values exactly. */

/* Applies the background/text colours to a freshly created screen object. */
void rpod_theme_style_screen(lv_obj_t *screen);

/* Glass styling for a top-docked, full-width bar (no radius; a hairline
 * catches light along the bottom edge instead, where the bar meets
 * content). Used by the persistent status bar (ui/status_bar.c) -- exposed
 * separately in case a future bottom tab/toolbar wants the same treatment. */
void rpod_theme_style_glass_bar(lv_obj_t *bar);

/* Glass styling for a floating rounded panel/card (list container, info
 * card, etc): translucent fill, a hairline highlight along the top edge,
 * soft drop shadow for lift. */
void rpod_theme_style_glass_panel(lv_obj_t *panel, int32_t radius);

#endif /* RPOD_THEME_H */
