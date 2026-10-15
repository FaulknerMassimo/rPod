#include "big_digits.h"

#include "classic.h"
#include "theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LV_FONT_DECLARE(rpod_font_digits_64)

/* The iPod's colon has no space of its own either side (its digits do):
 * this much on each, in panel pixels. */
#define CLASSIC_COLON_PAD (2 * RPOD_CLASSIC_PX)

/* Montserrat's colon gets the same. */
#define FONT_COLON_PAD 2

typedef struct {
    char text[16];
    lv_color_t color;
} big_digits_t;

/* Whether the iPod's numerals draw them, rather than the font. */
static bool use_bitmaps(void)
{
    return rpod_theme()->classic && rpod_classic_clock_glyph('0') != NULL;
}

static int32_t font_digit_w(void)
{
    int32_t w = 0;
    for (char c = '0'; c <= '9'; c++) {
        int32_t g = (int32_t)lv_font_get_glyph_width(&rpod_font_digits_64, (uint32_t)c, 0);
        if (g > w) {
            w = g;
        }
    }
    return w;
}

static int32_t cell_w(char c)
{
    if (use_bitmaps()) {
        const lv_image_dsc_t *colon = rpod_classic_clock_glyph(':');
        return c == ':' ? (int32_t)colon->header.w + 2 * CLASSIC_COLON_PAD
                        : (int32_t)rpod_classic_clock_glyph('0')->header.w;
    }
    if (c == ':') {
        return (int32_t)lv_font_get_glyph_width(&rpod_font_digits_64, ':', 0) + 2 * FONT_COLON_PAD;
    }
    return font_digit_w();
}

int32_t rpod_big_digits_height(void)
{
    if (use_bitmaps()) {
        return (int32_t)rpod_classic_clock_glyph('0')->header.h;
    }
    return lv_font_get_line_height(&rpod_font_digits_64);
}

int32_t rpod_big_digits_width(const char *text)
{
    int32_t w = 0;
    for (const char *p = text; *p != '\0'; p++) {
        w += cell_w(*p);
    }
    return w;
}

static void paint(lv_obj_t *cell, lv_color_t color)
{
    if (lv_obj_check_type(cell, &lv_image_class)) {
        lv_obj_set_style_image_recolor(cell, color, 0);
        lv_obj_set_style_image_recolor_opa(cell, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_text_color(cell, color, 0);
    }
}

/* One character's cell: its glyph centred in the character's width. */
static lv_obj_t *add_cell(lv_obj_t *obj, char c, lv_color_t color)
{
    int32_t w = cell_w(c);
    int32_t h = rpod_big_digits_height();
    lv_obj_t *cell;
    const lv_image_dsc_t *glyph = use_bitmaps() ? rpod_classic_clock_glyph(c) : NULL;
    if (glyph != NULL) {
        cell = lv_image_create(obj);
        lv_image_set_src(cell, glyph);
        lv_image_set_inner_align(cell, LV_IMAGE_ALIGN_CENTER);
    } else if (!use_bitmaps() && ((c >= '0' && c <= '9') || c == ':')) {
        cell = lv_label_create(obj);
        lv_obj_set_style_text_font(cell, &rpod_font_digits_64, 0);
        lv_obj_set_style_text_align(cell, LV_TEXT_ALIGN_CENTER, 0);
        char s[2] = { c, '\0' };
        lv_label_set_text(cell, s);
    } else {
        cell = lv_obj_create(obj); /* a gap */
        lv_obj_remove_style_all(cell);
    }
    lv_obj_set_size(cell, w, h);
    lv_obj_remove_flag(cell, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    paint(cell, color);
    return cell;
}

static void delete_cb(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

lv_obj_t *rpod_big_digits_create(lv_obj_t *parent)
{
    big_digits_t *bd = calloc(1, sizeof(*bd));
    bd->color = RPOD_COLOR_TEXT;

    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, LV_SIZE_CONTENT, rpod_big_digits_height());
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(obj, bd);
    lv_obj_add_event_cb(obj, delete_cb, LV_EVENT_DELETE, bd);
    return obj;
}

void rpod_big_digits_set_text(lv_obj_t *obj, const char *text)
{
    big_digits_t *bd = lv_obj_get_user_data(obj);
    if (strcmp(bd->text, text) == 0) {
        return; /* the clock asks every second: only a new minute redraws */
    }
    snprintf(bd->text, sizeof(bd->text), "%s", text);
    lv_obj_clean(obj);
    for (const char *p = bd->text; *p != '\0'; p++) {
        add_cell(obj, *p, bd->color);
    }
}

void rpod_big_digits_set_color(lv_obj_t *obj, lv_color_t color)
{
    big_digits_t *bd = lv_obj_get_user_data(obj);
    if (lv_color_eq(bd->color, color)) {
        return;
    }
    bd->color = color;
    uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; i++) {
        paint(lv_obj_get_child(obj, (int32_t)i), color);
    }
}
