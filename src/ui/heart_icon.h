/*
 * A small "like" heart control used by Now Playing, the song lists, and the
 * Add-to-Playlist picker. The bundled Montserrat fonts carry no heart glyph,
 * so the shape is rendered procedurally into anti-aliased A8 masks (one solid
 * fill, one outline stroke) that are tinted at draw time via image recolor --
 * see heart_icon.c. The masks are generated once per pixel size and cached
 * for the process's life, so creating many small hearts (e.g. one per liked
 * row) is cheap.
 */

#ifndef RPOD_HEART_ICON_H
#define RPOD_HEART_ICON_H

#include "lvgl.h"
#include <stdbool.h>

/* Creates a `size` x `size` heart inside `parent`: a dim outline heart in the
 * empty state, with a colour-filled heart overlaid that is revealed when
 * liked. Returns the container object (a plain, non-interactive lv_obj). */
lv_obj_t *rpod_heart_create(lv_obj_t *parent, int size);

/* Switches between empty (outline) and liked (filled). When `animate`, a like
 * plays a scale-up "pop" and an unlike fades the fill out; otherwise the state
 * is applied instantly (initial state, list rows). */
void rpod_heart_set_liked(lv_obj_t *heart, bool liked, bool animate);

bool rpod_heart_get_liked(lv_obj_t *heart);

#endif /* RPOD_HEART_ICON_H */
