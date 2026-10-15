/*
 * Look-and-feel: the colours every screen draws with, chosen at runtime from
 * a small set of themes (Settings > Theme), plus the surface-styling helpers
 * screens share.
 *
 *   - Glass: rPod's own dark "liquid glass" look.
 *   - Classic / Classic Dark: the 4th-generation iPod's UI, recovered from
 *     its firmware (re/ipod4/SPEC.md) -- a title bar, Chicago type, inverted
 *     selection and the iPod's scrollbar and sliders (ui/classic.h), in its
 *     own black-on-white or turned around to white-on-black.
 *
 * Geometry and type scale follow the theme too, via rpod_metrics()
 * (ui/metrics.h); `classic` is what screens branch on where a theme changes
 * layout rather than just colour.
 */

#ifndef RPOD_THEME_H
#define RPOD_THEME_H

#include "lvgl.h"

#include <stdbool.h>

typedef enum {
    RPOD_THEME_GLASS = 0,
    RPOD_THEME_CLASSIC,
    RPOD_THEME_CLASSIC_DARK,
    RPOD_THEME_COUNT,
} rpod_theme_id_t;

typedef struct {
    const char *key;  /* what the state file stores */
    const char *name; /* what Settings > Theme lists */
    bool classic;     /* the iPod's layout (title bar, inverted rows...) */

    lv_color_t bg;
    lv_color_t text;
    lv_color_t dim_text;
    lv_color_t header_bg;
    /* The selection highlight, and text drawn on it. */
    lv_color_t accent;
    lv_color_t sel_text;
    lv_color_t separator;
    /* "Loved" accent -- the fill of a liked song's heart. */
    lv_color_t like;
    /* Floating surfaces (rpod_theme_style_glass_panel()): translucent glass,
     * or the classic themes' opaque bordered box. */
    lv_color_t glass_fill;
    lv_opa_t glass_fill_opa;
    lv_color_t glass_edge;
    lv_opa_t glass_edge_opa;
    /* An on-screen keyboard's keycap, unfocused. */
    lv_color_t key_fill;
    lv_opa_t key_fill_opa;
    /* What fills a slider (the classic progress and volume bars). */
    lv_color_t fill;
} rpod_theme_t;

/* The active theme. */
const rpod_theme_t *rpod_theme(void);

/* The colours, by their old fixed names -- now whatever the active theme
 * says, so the screens that read them follow a theme change on their next
 * build. */
#define RPOD_COLOR_BG          (rpod_theme()->bg)
#define RPOD_COLOR_TEXT        (rpod_theme()->text)
#define RPOD_COLOR_HEADER_BG   (rpod_theme()->header_bg)
#define RPOD_COLOR_ACCENT      (rpod_theme()->accent)
#define RPOD_COLOR_SEL_TEXT    (rpod_theme()->sel_text)
#define RPOD_COLOR_DIM_TEXT    (rpod_theme()->dim_text)
#define RPOD_COLOR_SEPARATOR   (rpod_theme()->separator)
#define RPOD_COLOR_LIKE        (rpod_theme()->like)
#define RPOD_COLOR_GLASS_FILL  (rpod_theme()->glass_fill)
#define RPOD_GLASS_FILL_OPA    (rpod_theme()->glass_fill_opa)
#define RPOD_COLOR_GLASS_EDGE  (rpod_theme()->glass_edge)
#define RPOD_GLASS_EDGE_OPA    (rpod_theme()->glass_edge_opa)
#define RPOD_COLOR_KEY_FILL    (rpod_theme()->key_fill)
#define RPOD_KEY_FILL_OPA      (rpod_theme()->key_fill_opa)

/* Loads the persisted choice from state_path (NULL: Glass, not persisted).
 * Call once, after lv_init() and before building any UI. */
void rpod_theme_init(const char *state_path);

rpod_theme_id_t rpod_theme_id(void);
const char *rpod_theme_name(rpod_theme_id_t id);

/* Switches theme and persists it. The UI is rebuilt in its new look by the
 * on_change callback, which runs from the next timer cycle -- never inside
 * the event that picked the theme, whose widgets the rebuild deletes. */
void rpod_theme_set(rpod_theme_id_t id);

/* Who rebuilds the UI after rpod_theme_set() (the app). One at a time. */
void rpod_theme_on_change(void (*cb)(void *ctx), void *ctx);

/* Applies the background/text colours (and, for the classic themes, their
 * body font) to a freshly created screen object. */
void rpod_theme_style_screen(lv_obj_t *screen);

/* Glass styling for a top-docked, full-width bar (no radius; a hairline
 * catches light along the bottom edge instead, where the bar meets
 * content). Used by the persistent status bar (ui/status_bar.c). */
void rpod_theme_style_glass_bar(lv_obj_t *bar);

/* Glass styling for a floating rounded panel/card (list container, info
 * card, etc): translucent fill, a hairline highlight along the top edge,
 * soft drop shadow for lift. The classic themes draw a square, opaque box
 * with a solid 2px border instead, and ignore `radius`. */
void rpod_theme_style_glass_panel(lv_obj_t *panel, int32_t radius);

#endif /* RPOD_THEME_H */
