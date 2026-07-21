/*
 * Decodes cover art fetched via rpod_mpd_get_cover_art() into a small,
 * fixed-size RGB565 thumbnail that now_playing.c hands straight to an
 * lv_image widget. Deliberately not an LVGL image decoder plugin -- see
 * cover_art.c for why.
 */

#ifndef RPOD_COVER_ART_H
#define RPOD_COVER_ART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint16_t *pixels; /* malloc'd, RGB565, w * h, row-major */
    int w;
    int h;
} rpod_cover_art_t;

/* Decodes `data` (raw file bytes, as returned by rpod_mpd_get_cover_art())
 * to an out_w x out_h RGB565 thumbnail, center-cropped to out_w:out_h
 * before scaling (like iOS's "aspect fill", not a squash-to-fit stretch).
 * Supports baseline JPEG and 8-bit non-interlaced PNG -- the two formats
 * actually seen in testing against real ripped FLAC files' embedded cover
 * art (PNG turned out to be the more common of the two). Progressive
 * JPEG, palette PNG, and anything else fail cleanly and the caller should
 * fall back to a placeholder tile. On success, free the result with
 * rpod_cover_art_free(). */
bool rpod_cover_art_decode(const unsigned char *data, size_t size, int out_w, int out_h,
                           rpod_cover_art_t *out);
void rpod_cover_art_free(rpod_cover_art_t *art);

#endif /* RPOD_COVER_ART_H */
