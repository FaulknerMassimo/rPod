/*
 * Decodes cover art fetched via rpod_mpd_get_cover_art() into a small,
 * fixed-size RGB565 thumbnail that screens hand straight to an lv_image
 * widget (via ui/cover_cache.h). Deliberately not an LVGL image decoder
 * plugin -- see cover_art.c for why.
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
 * before scaling (like iOS's "aspect fill", not a squash-to-fit stretch),
 * each output pixel the average of the source pixels it covers. Supports
 * JPEG (baseline and progressive, RGB or grayscale) and non-interlaced PNG
 * (8-bit, or a palette) -- the formats seen in real ripped FLAC files'
 * embedded cover art (PNG turned out to be the most common). Anything else
 * fails cleanly and the caller should fall back to a placeholder tile. On
 * success, free the result with rpod_cover_art_free(). */
bool rpod_cover_art_decode(const unsigned char *data, size_t size, int out_w, int out_h,
                           rpod_cover_art_t *out);
void rpod_cover_art_free(rpod_cover_art_t *art);

/* Resamples an already-decoded tile to out_w x out_h, cropped and averaged
 * the same way rpod_cover_art_decode() does -- cheap (it never touches the
 * original file), so smaller thumbnails come from one decoded master tile.
 * Free with rpod_cover_art_free(). */
bool rpod_cover_art_scale(const rpod_cover_art_t *src, int out_w, int out_h, rpod_cover_art_t *out);

/* Like rpod_cover_art_scale(), but blurred and darkened -- meant as a
 * full-bleed background behind a Now-Playing-style screen (iOS lock-screen
 * style: legible text and glass panels floating over a soft, dim version of
 * the artwork). This is a *one-time* blur computed here on track change,
 * not a live per-frame effect: LVGL's software renderer has no backdrop
 * blur primitive, there's no GPU on a Pi Zero 2 W, and re-blurring a full
 * frame on every redraw would blow the partial-render budget in
 * docs/PLAN.md #5.3. A few box-blur passes over one small frame costs
 * microseconds and only runs when the song changes. Free with
 * rpod_cover_art_free(). */
bool rpod_cover_art_make_background(const rpod_cover_art_t *src, int out_w, int out_h,
                                    rpod_cover_art_t *out);

#endif /* RPOD_COVER_ART_H */
