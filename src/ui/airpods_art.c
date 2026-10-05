#include "airpods_art.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* --- Shapes -------------------------------------------------------------------
 *
 * Signed distance functions (negative inside), in "units": one bud of the
 * pair sits in a 1.0 x PAIR_H box, the case in CASE_W x CASE_H. Distances
 * are what the shading uses to darken toward an edge; coverage comes from
 * supersampling plain inside tests, like heart_icon.c. */

#define SS 4 /* supersample factor per axis */

#define PAIR_W 2.1f /* two bud boxes and a gap */
#define PAIR_H 1.4f
#define RIGHT_BUD_X 1.1f
#define CASE_W 1.34f
#define CASE_H 1.0f

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static float smoothstep(float e0, float e1, float x)
{
    float t = clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/* Approximate for an ellipse, exact for a circle: close enough for edge
 * shading on these near-round ones. */
static float sd_ellipse(float px, float py, float cx, float cy, float rx, float ry)
{
    float dx = (px - cx) / rx, dy = (py - cy) / ry;
    return (sqrtf(dx * dx + dy * dy) - 1.0f) * fminf(rx, ry);
}

static float sd_capsule(float px, float py, float ax, float ay, float bx, float by, float r)
{
    float pax = px - ax, pay = py - ay, bax = bx - ax, bay = by - ay;
    float h = clampf((pax * bax + pay * bay) / (bax * bax + bay * bay), 0.0f, 1.0f);
    float dx = pax - bax * h, dy = pay - bay * h;
    return sqrtf(dx * dx + dy * dy) - r;
}

static float sd_round_rect(float px, float py, float cx, float cy, float hw, float hh, float r)
{
    float qx = fabsf(px - cx) - (hw - r), qy = fabsf(py - cy) - (hh - r);
    float ox = fmaxf(qx, 0.0f), oy = fmaxf(qy, 0.0f);
    return sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0.0f) - r;
}

typedef struct {
    float body; /* head + stem */
    float tip;  /* silicone ear tip */
    float mesh; /* the dark vent on the head */
} bud_t;

/* One AirPods Pro bud facing left -- the right one of the pair, its ear tip
 * pointing in toward the other. `bold` fattens it for the small glyph, where
 * a true-to-life stem would be a single faint pixel. */
static bud_t bud_local(float x, float y, bool bold)
{
    bud_t b;
    float head = bold ? sd_ellipse(x, y, 0.56f, 0.42f, 0.43f, 0.38f)
                      : sd_ellipse(x, y, 0.56f, 0.42f, 0.40f, 0.35f);
    float stem = sd_capsule(x, y, 0.70f, 0.58f, 0.79f, bold ? 1.24f : 1.27f, bold ? 0.16f : 0.13f);
    b.body = fminf(head, stem);
    b.tip = sd_ellipse(x, y, 0.23f, 0.33f, 0.21f, 0.23f);
    b.mesh = sd_ellipse(x, y, 0.64f, 0.31f, 0.10f, 0.06f);
    return b;
}

/* Both buds, the left one mirrored. */
static bud_t pair_at(float x, float y, bool bold)
{
    bud_t l = bud_local(1.0f - x, y, bold);
    bud_t r = bud_local(x - RIGHT_BUD_X, y, bold);
    return (bud_t){ fminf(l.body, r.body), fminf(l.tip, r.tip), fminf(l.mesh, r.mesh) };
}

static float case_at(float x, float y)
{
    return sd_round_rect(x, y, CASE_W / 2, CASE_H / 2, CASE_W / 2 - 0.01f, CASE_H / 2 - 0.01f, 0.40f);
}

/* --- Rendering ------------------------------------------------------------------ */

typedef struct {
    float r, g, b, a; /* straight alpha, 0-1 */
} rgba_t;

/* Colour of one sample, `px` being a pixel's size in units. */
typedef rgba_t (*shade_fn)(float x, float y, float px);

/* White plastic: lit from above, rolling off toward its edges. */
static float plastic(float y, float h, float inside, float px)
{
    float base = 1.0f - 0.20f * powf(clampf(y / h, 0.0f, 1.0f), 1.3f);
    return base * (0.80f + 0.20f * smoothstep(0.0f, 2.5f * px, inside));
}

static rgba_t grey(float v)
{
    return (rgba_t){ v, v, v * 1.01f > 1.0f ? 1.0f : v * 1.01f, 1.0f };
}

static rgba_t shade_buds(float x, float y, float px)
{
    bud_t b = pair_at(x, y, false);
    if (b.body < 0.0f) {
        if (b.mesh < 0.0f) {
            return grey(0.30f);
        }
        return grey(plastic(y, PAIR_H, -b.body, px));
    }
    if (b.tip < 0.0f) {
        float v = 0.88f * (0.78f + 0.22f * smoothstep(0.0f, 2.0f * px, -b.tip));
        /* The crease where the tip meets the head. */
        if (b.body < 1.2f * px) {
            v *= 0.68f;
        }
        return grey(v);
    }
    return (rgba_t){ 0 };
}

static rgba_t shade_case(float x, float y, float px)
{
    float d = case_at(x, y);
    if (d >= 0.0f) {
        return (rgba_t){ 0 };
    }
    if (sd_ellipse(x, y, CASE_W / 2, 0.58f, 0.032f, 0.032f) < 0.0f) {
        return grey(0.45f); /* status light */
    }
    float v = plastic(y, CASE_H, -d, px);
    float seam = fabsf(y - 0.30f);
    if (seam < 0.6f * px) {
        v *= 0.70f; /* where the lid closes */
    } else if (fabsf(y - 0.30f - 1.1f * px) < 0.5f * px) {
        v = fminf(1.0f, v * 1.05f); /* the lip catching light below it */
    }
    return grey(v);
}

/* A w x h ARGB8888 image of `shade` over a units_w x units_h scene. */
static uint8_t *render_argb(int w, int h, float units_h, shade_fn shade)
{
    uint8_t *buf = malloc((size_t)w * h * 4);
    if (buf == NULL) {
        return NULL;
    }
    float px = units_h / (float)h;
    for (int py = 0; py < h; py++) {
        for (int pxl = 0; pxl < w; pxl++) {
            float r = 0, g = 0, b = 0, a = 0;
            for (int sy = 0; sy < SS; sy++) {
                for (int sx = 0; sx < SS; sx++) {
                    float ux = ((float)pxl + (sx + 0.5f) / SS) * px;
                    float uy = ((float)py + (sy + 0.5f) / SS) * px;
                    rgba_t c = shade(ux, uy, px);
                    r += c.r * c.a;
                    g += c.g * c.a;
                    b += c.b * c.a;
                    a += c.a;
                }
            }
            uint8_t *o = buf + ((size_t)py * w + pxl) * 4;
            if (a > 0.0f) { /* LVGL's ARGB8888 is straight alpha, stored B, G, R, A */
                o[0] = (uint8_t)(b / a * 255.0f + 0.5f);
                o[1] = (uint8_t)(g / a * 255.0f + 0.5f);
                o[2] = (uint8_t)(r / a * 255.0f + 0.5f);
            } else {
                o[0] = o[1] = o[2] = 0;
            }
            o[3] = (uint8_t)(a / (SS * SS) * 255.0f + 0.5f);
        }
    }
    return buf;
}

/* The glyph: the pair's silhouette, with the ear tips cut free of the heads
 * by a hairline so they read as tips at 14 px rather than one blob. */
static uint8_t *render_glyph(int w, int h)
{
    uint8_t *buf = malloc((size_t)w * h);
    if (buf == NULL) {
        return NULL;
    }
    float px = PAIR_H / (float)h;
    for (int py = 0; py < h; py++) {
        for (int pxl = 0; pxl < w; pxl++) {
            int hit = 0;
            for (int sy = 0; sy < SS; sy++) {
                for (int sx = 0; sx < SS; sx++) {
                    bud_t b = pair_at(((float)pxl + (sx + 0.5f) / SS) * px,
                                      ((float)py + (sy + 0.5f) / SS) * px, true);
                    hit += b.body < 0.0f || (b.tip < 0.0f && b.body > 0.7f * px);
                }
            }
            buf[(size_t)py * w + pxl] = (uint8_t)(hit * 255 / (SS * SS));
        }
    }
    return buf;
}

/* --- Cache ------------------------------------------------------------------------ */

typedef enum { ART_GLYPH, ART_BUDS, ART_CASE } art_kind_t;

/* Fixed, never reallocated: lv_images point at these descriptors. */
#define ART_CACHE_MAX 12
static struct {
    art_kind_t kind;
    int h;
    lv_image_dsc_t dsc;
} g_cache[ART_CACHE_MAX];
static size_t g_cached;

static const lv_image_dsc_t *art_get(art_kind_t kind, int h)
{
    for (size_t i = 0; i < g_cached; i++) {
        if (g_cache[i].kind == kind && g_cache[i].h == h) {
            return &g_cache[i].dsc;
        }
    }
    if (g_cached == ART_CACHE_MAX || h <= 0) {
        return NULL;
    }
    int w = (int)lroundf(h * (kind == ART_CASE ? CASE_W / CASE_H : PAIR_W / PAIR_H));
    uint8_t *data;
    switch (kind) {
    case ART_GLYPH: data = render_glyph(w, h); break;
    case ART_BUDS:  data = render_argb(w, h, PAIR_H, shade_buds); break;
    default:        data = render_argb(w, h, CASE_H, shade_case); break;
    }
    if (data == NULL) {
        return NULL;
    }
    int bpp = kind == ART_GLYPH ? 1 : 4;
    lv_image_dsc_t *dsc = &g_cache[g_cached].dsc;
    memset(dsc, 0, sizeof(*dsc));
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = kind == ART_GLYPH ? LV_COLOR_FORMAT_A8 : LV_COLOR_FORMAT_ARGB8888;
    dsc->header.w = (uint16_t)w;
    dsc->header.h = (uint16_t)h;
    dsc->header.stride = (uint16_t)(w * bpp);
    dsc->data_size = (uint32_t)(w * h * bpp);
    dsc->data = data;
    g_cache[g_cached].kind = kind;
    g_cache[g_cached].h = h;
    g_cached++;
    return dsc;
}

const lv_image_dsc_t *rpod_airpods_glyph(int h)
{
    return art_get(ART_GLYPH, h);
}

const lv_image_dsc_t *rpod_airpods_art_buds(int h)
{
    return art_get(ART_BUDS, h);
}

const lv_image_dsc_t *rpod_airpods_art_case(int h)
{
    return art_get(ART_CASE, h);
}

lv_obj_t *rpod_airpods_glyph_create(lv_obj_t *parent, int h, lv_color_t color)
{
    lv_obj_t *img = lv_image_create(parent);
    const lv_image_dsc_t *dsc = rpod_airpods_glyph(h);
    if (dsc != NULL) {
        lv_image_set_src(img, dsc);
    }
    /* A8 masks are drawn in the recolor colour; recolor_opa must be
     * non-zero for it to be read at all (as in heart_icon.c). */
    lv_obj_set_style_image_recolor(img, color, 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    return img;
}
