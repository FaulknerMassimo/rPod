/*
 * AirPods pictures, drawn procedurally (the bundled fonts have no such
 * glyph, and a stored bitmap wouldn't scale): a small one-colour glyph for
 * the status bar and the HUD (ui/hud.h), and shaded illustrations of the
 * buds and the charging case for the battery card
 * (ui/airpods_notify.h). Each size is drawn once, 4x4 supersampled, and
 * kept for the life of the process, so the returned descriptors never move
 * -- an lv_image only keeps a pointer to its source.
 */

#ifndef RPOD_AIRPODS_ART_H
#define RPOD_AIRPODS_ART_H

#include "lvgl.h"

/* A pair of AirPods Pro as an A8 mask `h` px tall (and about 1.3x as wide),
 * for lv_image with image recolor -- rpod_airpods_glyph_create() sets that
 * up. NULL if out of memory. */
const lv_image_dsc_t *rpod_airpods_glyph(int h);

/* An lv_image of rpod_airpods_glyph(h) in `color`. */
lv_obj_t *rpod_airpods_glyph_create(lv_obj_t *parent, int h, lv_color_t color);

/* White, shaded ARGB8888 illustrations `h` px tall: the pair of buds, and
 * the case with its lid shut. NULL if out of memory. */
const lv_image_dsc_t *rpod_airpods_art_buds(int h);
const lv_image_dsc_t *rpod_airpods_art_case(int h);

#endif /* RPOD_AIRPODS_ART_H */
