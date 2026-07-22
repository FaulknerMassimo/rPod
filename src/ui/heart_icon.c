#include "heart_icon.h"

#include "ui/theme.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* --- Procedural heart masks --------------------------------------------
 *
 * The heart shape is the classic implicit curve (x^2 + y^2 - 1)^3 - x^2*y^3,
 * negative inside. Each mask pixel is 4x4 supersampled for a smooth edge and
 * stored as A8 (alpha only): the fill mask is the solid interior, the outline
 * mask is a thin stroke found by subtracting an eroded copy of the fill from
 * itself. Both are tinted per-widget by the image recolor style, so the same
 * grey mask can render as a dim outline or a red fill.
 *
 * The shape is mapped a little smaller than the pixel box (SX/SY < the curve's
 * extent would need) so the tips have headroom to grow past 1.0x during the
 * "pop" without being clipped by the container. */
#define HEART_SS   4        /* supersample factor per axis */
#define HEART_SX   1.34f    /* horizontal half-extent mapped to the box edge */
#define HEART_SY   1.28f    /* vertical half-extent */
#define HEART_OY   0.14f    /* vertical centre offset of the shape in the box */

typedef struct {
    int size;
    lv_image_dsc_t fill;
    lv_image_dsc_t outline;
} heart_masks_t;

/* Process-lifetime cache of a handful of distinct heart sizes. A *fixed* array
 * (not a growable one): an lv_image keeps only a pointer to its source dsc, so
 * the dsc structs must never move once handed out -- a realloc here would
 * dangle the source of every heart already created (the same hazard the
 * vcover_t cache in music_screens.c is built to dodge). */
#define HEART_MASK_CACHE_MAX 16
static heart_masks_t g_masks[HEART_MASK_CACHE_MAX];
static size_t g_masks_count = 0;

static float heart_inside(float x, float y)
{
    float a = x * x + y * y - 1.0f;
    return a * a * a - x * x * y * y * y; /* < 0 inside */
}

/* Filled-interior coverage in [0,1] for every pixel, supersampled. */
static float *heart_coverage(int size)
{
    float *cov = malloc((size_t)size * size * sizeof(*cov));
    if (cov == NULL) {
        return NULL;
    }
    const float step = 1.0f / (HEART_SS * size);
    const float sub0 = 0.5f / (HEART_SS * size);
    for (int py = 0; py < size; py++) {
        for (int px = 0; px < size; px++) {
            int hit = 0;
            for (int sy = 0; sy < HEART_SS; sy++) {
                for (int sx = 0; sx < HEART_SS; sx++) {
                    float fx = (float)px / size + sub0 + sx * step;
                    float fy = (float)py / size + sub0 + sy * step;
                    float nx = fx * 2.0f - 1.0f;
                    float ny = fy * 2.0f - 1.0f;
                    float x = nx * HEART_SX;
                    float y = HEART_OY - ny * HEART_SY; /* image y is top-down */
                    if (heart_inside(x, y) < 0.0f) {
                        hit++;
                    }
                }
            }
            cov[py * size + px] = (float)hit / (HEART_SS * HEART_SS);
        }
    }
    return cov;
}

static void fill_a8_dsc(lv_image_dsc_t *dsc, int size, uint8_t *buf)
{
    memset(dsc, 0, sizeof(*dsc));
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_A8;
    dsc->header.w = (uint16_t)size;
    dsc->header.h = (uint16_t)size;
    dsc->header.stride = (uint16_t)size;
    dsc->data_size = (uint32_t)(size * size);
    dsc->data = buf;
}

/* Builds both masks for `size` and appends them to the cache. */
static const heart_masks_t *heart_masks_get(int size)
{
    for (size_t i = 0; i < g_masks_count; i++) {
        if (g_masks[i].size == size) {
            return &g_masks[i];
        }
    }
    if (g_masks_count == HEART_MASK_CACHE_MAX) {
        return NULL;
    }

    float *cov = heart_coverage(size);
    if (cov == NULL) {
        return NULL;
    }

    uint8_t *fill = malloc((size_t)size * size);
    uint8_t *outline = malloc((size_t)size * size);
    if (fill == NULL || outline == NULL) {
        free(cov);
        free(fill);
        free(outline);
        return NULL;
    }

    int r = size / 13;
    if (r < 1) {
        r = 1;
    }
    for (int py = 0; py < size; py++) {
        for (int px = 0; px < size; px++) {
            float c = cov[py * size + px];
            fill[py * size + px] = (uint8_t)(c * 255.0f + 0.5f);

            /* Erode: the minimum coverage over a (2r+1) square neighbourhood,
             * treating off-image samples as empty. The stroke is what the
             * fill has that the eroded copy has lost -- a band ~r px wide
             * hugging the inside of the boundary, plus the outer AA edge. */
            float mn = c;
            for (int dy = -r; dy <= r && mn > 0.0f; dy++) {
                for (int dx = -r; dx <= r; dx++) {
                    int nx = px + dx, ny = py + dy;
                    float s = (nx < 0 || ny < 0 || nx >= size || ny >= size)
                                  ? 0.0f
                                  : cov[ny * size + nx];
                    if (s < mn) {
                        mn = s;
                    }
                }
            }
            float stroke = c - mn;
            if (stroke < 0.0f) {
                stroke = 0.0f;
            }
            outline[py * size + px] = (uint8_t)(stroke * 255.0f + 0.5f);
        }
    }
    free(cov);

    heart_masks_t *m = &g_masks[g_masks_count++];
    m->size = size;
    fill_a8_dsc(&m->fill, size, fill);
    fill_a8_dsc(&m->outline, size, outline);
    return m;
}

/* --- Widget ------------------------------------------------------------- */

typedef struct {
    lv_obj_t *outline;
    lv_obj_t *fill;
    int size;
    bool liked;
} heart_t;

static void heart_delete_cb(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

static void heart_scale_cb(void *img, int32_t v)
{
    lv_image_set_scale(img, (uint32_t)v);
}

static void heart_opa_cb(void *img, int32_t v)
{
    lv_obj_set_style_opa(img, (lv_opa_t)v, 0);
}

/* End of an unlike fade: hide the fill and reset it to its resting transform
 * so the next like starts clean. */
static void heart_unlike_ready_cb(lv_anim_t *a)
{
    lv_obj_t *img = a->var;
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(img, LV_OPA_COVER, 0);
    lv_image_set_scale(img, 256);
}

static lv_obj_t *make_mask_image(lv_obj_t *parent, const lv_image_dsc_t *src, int size, lv_color_t color)
{
    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, src);
    lv_obj_set_size(img, size, size);
    lv_obj_center(img);
    /* A8 masks are drawn in the recolor colour (see the SW image renderer);
     * recolor_opa must be non-zero for the style to be read at all. */
    lv_obj_set_style_image_recolor(img, color, 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    /* Transform (the pop) pivots about the centre, not the top-left default. */
    lv_image_set_pivot(img, size / 2, size / 2);
    return img;
}

lv_obj_t *rpod_heart_create(lv_obj_t *parent, int size)
{
    const heart_masks_t *masks = heart_masks_get(size);

    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, size, size);
    lv_obj_remove_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    heart_t *h = calloc(1, sizeof(*h));
    h->size = size;
    h->liked = false;

    if (masks != NULL) {
        h->outline = make_mask_image(cont, &masks->outline, size, RPOD_COLOR_DIM_TEXT);
        h->fill = make_mask_image(cont, &masks->fill, size, RPOD_COLOR_LIKE);
        lv_obj_add_flag(h->fill, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_set_user_data(cont, h);
    lv_obj_add_event_cb(cont, heart_delete_cb, LV_EVENT_DELETE, h);
    return cont;
}

void rpod_heart_set_liked(lv_obj_t *heart, bool liked, bool animate)
{
    heart_t *h = lv_obj_get_user_data(heart);
    if (h == NULL || h->fill == NULL) {
        return;
    }
    h->liked = liked;

    /* A state change interrupts any in-flight animation on the fill. */
    lv_anim_delete(h->fill, heart_scale_cb);
    lv_anim_delete(h->fill, heart_opa_cb);

    if (liked) {
        lv_obj_add_flag(h->outline, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(h->fill, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(h->fill, LV_OPA_COVER, 0);
        if (animate) {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, h->fill);
            lv_anim_set_exec_cb(&a, heart_scale_cb);
            lv_anim_set_values(&a, 90, 256);
            lv_anim_set_duration(&a, 300);
            lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
            lv_anim_start(&a);
        } else {
            lv_image_set_scale(h->fill, 256);
        }
    } else {
        lv_obj_remove_flag(h->outline, LV_OBJ_FLAG_HIDDEN);
        if (animate) {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, h->fill);
            lv_anim_set_exec_cb(&a, heart_opa_cb);
            lv_anim_set_values(&a, 255, 0);
            lv_anim_set_duration(&a, 150);
            lv_anim_set_ready_cb(&a, heart_unlike_ready_cb);
            lv_anim_start(&a);
        } else {
            lv_obj_add_flag(h->fill, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_opa(h->fill, LV_OPA_COVER, 0);
            lv_image_set_scale(h->fill, 256);
        }
    }
}

bool rpod_heart_get_liked(lv_obj_t *heart)
{
    heart_t *h = lv_obj_get_user_data(heart);
    return h != NULL && h->liked;
}
