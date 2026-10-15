/*
 * The 4th-generation iPod's look, for the classic themes (ui/theme.h): its
 * fonts and bitmaps, and the widgets whose drawing its firmware defines
 * pixel by pixel -- the menu scrollbar and the rounded slider. Everything is
 * laid out at 2x: one iPod pixel is RPOD_CLASSIC_PX panel pixels. Numbers
 * here come from re/ipod4/SPEC.md (§5), recovered from firmware 3.1.1.
 *
 * The fonts and bitmaps are Apple's, extracted from an iPod firmware image
 * by re/ipod4/tools/extract.py -- never committed, and built in only when
 * the Makefile finds them (IPOD_ASSETS, which defines RPOD_IPOD_ASSETS).
 * Without them everything here still works, drawn with Montserrat and
 * LVGL's symbols instead.
 */

#ifndef RPOD_CLASSIC_H
#define RPOD_CLASSIC_H

#include "lvgl.h"

#include <stdbool.h>

/* One iPod pixel, on the panel. */
#define RPOD_CLASSIC_PX 2

/* Chicago 12 (menus, titles, Now Playing) and Pixo 7 bold (Now Playing's
 * "3 of 12"), both pixel-doubled. Glyphs they lack -- LVGL's symbols, and
 * scripts the iPod didn't cover -- fall back to Montserrat. */
const lv_font_t *rpod_classic_font_body(void);
const lv_font_t *rpod_classic_font_small(void);

/* True when the iPod's own fonts and bitmaps are built in. */
bool rpod_classic_has_assets(void);

typedef enum {
    RPOD_ICON_ARROW,       /* a menu row's ">" */
    RPOD_ICON_PLAY,        /* title bar play state */
    RPOD_ICON_PAUSE,
    RPOD_ICON_BATTERY,     /* title bar battery, drawn empty: no fuel gauge yet */
    RPOD_ICON_REPEAT,
    RPOD_ICON_REPEAT_ONE,
    RPOD_ICON_SHUFFLE,
    RPOD_ICON_VOLUME_LOW,  /* either end of the volume bar */
    RPOD_ICON_VOLUME_HIGH,
    RPOD_ICON_HEART,       /* a liked song */
    RPOD_ICON_CHECK,       /* a song in some playlist */
    RPOD_ICON_LOCK,        /* title bar, while the screen is locked */
    RPOD_ICON_SPIN_UP,     /* either side of a number being set */
    RPOD_ICON_SPIN_DOWN,
    RPOD_ICON_BELL,        /* an alarm that's set */
    RPOD_ICON_ALARM,       /* an alarm going off: 32x32 */
    RPOD_ICON_COUNT,
} rpod_icon_t;

/* An icon in `color`: one of the iPod's bitmaps, or its stand-in without
 * them. Not interactive. */
lv_obj_t *rpod_classic_icon_create(lv_obj_t *parent, rpod_icon_t icon, lv_color_t color);

/* The iPod's clock numerals for '0'-'9' (20x30 iPod pixels) and ':' (4x30),
 * as A8 masks at 2x -- NULL for anything else, or without its bitmaps. */
const lv_image_dsc_t *rpod_classic_clock_glyph(char c);

/* Swaps an icon for another of the same size class (play/pause, repeat /
 * repeat one) without rebuilding it. */
void rpod_classic_icon_set(lv_obj_t *obj, rpod_icon_t icon);

/* Recolours an icon, e.g. onto a selected row's inverted colours. */
void rpod_classic_icon_set_color(lv_obj_t *obj, lv_color_t color);

/* Styles a button as a classic menu row: row height, Chicago, transparent
 * until it's the selection, then inverted (theme accent fill, sel_text). */
void rpod_classic_style_row(lv_obj_t *btn);

/* The menu scrollbar: 10 iPod pixels wide, `h` tall, with the iPod's two
 * rules, bottom rule and 5-pixel thumb. Hidden until
 * rpod_classic_scrollbar_set() finds the content taller than the view. */
#define RPOD_CLASSIC_SCROLLBAR_W (10 * RPOD_CLASSIC_PX)
lv_obj_t *rpod_classic_scrollbar_create(lv_obj_t *parent, int32_t h);

/* Shows the thumb for a view `visible` tall onto `content`, scrolled down
 * `offset` (all in the same units). Returns whether the scrollbar shows:
 * false when everything fits, which hides it. */
bool rpod_classic_scrollbar_set(lv_obj_t *sb, int32_t content, int32_t visible, int32_t offset);

/* The iPod's slider (progress and volume): a frame with rounded corners,
 * filled from the left in the theme's slider fill. `w` and `h` are whole
 * iPod pixels (multiples of RPOD_CLASSIC_PX). */
lv_obj_t *rpod_classic_slider_create(lv_obj_t *parent, int32_t w, int32_t h);
void rpod_classic_slider_set(lv_obj_t *slider, int32_t value, int32_t range);

#endif /* RPOD_CLASSIC_H */
