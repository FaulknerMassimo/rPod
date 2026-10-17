#include "classic.h"

#include "metrics.h"
#include "theme.h"

#if RPOD_IPOD_ASSETS
#include "ipod_fonts.h"
#include "ipod_images.h"
#endif

#define PX RPOD_CLASSIC_PX

/* A menu row's ">" sits 2 iPod pixels in from the row's right edge. */
#define ROW_PAD_RIGHT (2 * PX)

/* --- Fonts ----------------------------------------------------------------- */

#if RPOD_IPOD_ASSETS
/* Copies of the generated fonts, which are const, to hang a fallback on:
 * Chicago has no LVGL symbols (the HUD's speaker, a fallback icon) and Pixo
 * no Latin Extended-A, and Montserrat has both. */
static lv_font_t g_body;
static lv_font_t g_small;

static void init_fonts(void)
{
    if (g_body.dsc != NULL) {
        return;
    }
    g_body = ipod_font_chicago_12_x2;
    g_body.fallback = &lv_font_montserrat_16;
    g_small = ipod_font_pixo_7_bold_x2;
    g_small.fallback = &lv_font_montserrat_14;
}
#endif

const lv_font_t *rpod_classic_font_body(void)
{
#if RPOD_IPOD_ASSETS
    init_fonts();
    return &g_body;
#else
    return &lv_font_montserrat_20;
#endif
}

const lv_font_t *rpod_classic_font_small(void)
{
#if RPOD_IPOD_ASSETS
    init_fonts();
    return &g_small;
#else
    return &lv_font_montserrat_14;
#endif
}

bool rpod_classic_has_assets(void)
{
#if RPOD_IPOD_ASSETS
    return true;
#else
    return false;
#endif
}

/* --- Icons ----------------------------------------------------------------- */

#if RPOD_IPOD_ASSETS
/* The firmware's BMap resources by id (re/ipod4/out/lvgl/ipod_images.h),
 * pixel-doubled A8 masks drawn in the image recolour. */
static const lv_image_dsc_t *const k_bitmaps[RPOD_ICON_COUNT] = {
    [RPOD_ICON_ARROW] = &ipod_img_25ae_x2,
    [RPOD_ICON_PLAY] = &ipod_img_3330_x2,
    [RPOD_ICON_PAUSE] = &ipod_img_332f_x2,
    [RPOD_ICON_BATTERY] = &ipod_img_25c9_x2,
    [RPOD_ICON_REPEAT] = &ipod_img_25a8_x2,
    [RPOD_ICON_REPEAT_ONE] = &ipod_img_25a9_x2,
    [RPOD_ICON_SHUFFLE] = &ipod_img_25ac_x2,
    [RPOD_ICON_VOLUME_LOW] = &ipod_img_25b4_x2,
    [RPOD_ICON_VOLUME_HIGH] = &ipod_img_25b5_x2,
    /* rPod's own (pixel_heart(), below): the iPod had no "loved" mark. */
    [RPOD_ICON_HEART] = NULL,
    /* Pixo's own check mark -- a glyph, not a bitmap (below). */
    [RPOD_ICON_CHECK] = NULL,
    /* The title bar's hold switch icon. */
    [RPOD_ICON_LOCK] = &ipod_img_25a7_x2,
    /* The alarm time view's arrows (re/ipod4/SPEC.md §5.4, card 0x4594). */
    [RPOD_ICON_SPIN_UP] = &ipod_img_25df_x2,
    [RPOD_ICON_SPIN_DOWN] = &ipod_img_25e0_x2,
    [RPOD_ICON_BELL] = &ipod_img_25e3_x2,
    [RPOD_ICON_ALARM] = &ipod_img_36ac_x2,
};

/* The clock's numerals, '0' to '9', then the colon. */
static const lv_image_dsc_t *const k_clock_digits[10] = {
    &ipod_img_25ba_x2, &ipod_img_25bb_x2, &ipod_img_25bc_x2, &ipod_img_25bd_x2, &ipod_img_25be_x2,
    &ipod_img_25bf_x2, &ipod_img_25c0_x2, &ipod_img_25c1_x2, &ipod_img_25c2_x2, &ipod_img_25c3_x2,
};
#endif

const lv_image_dsc_t *rpod_classic_clock_glyph(char c)
{
#if RPOD_IPOD_ASSETS
    if (c >= '0' && c <= '9') {
        return k_clock_digits[c - '0'];
    }
    if (c == ':') {
        return &ipod_img_25b9_x2;
    }
#else
    (void)c;
#endif
    return NULL;
}

/* Marks drawn here in the iPod's manner -- hard-edged, on its pixel grid --
 * where its firmware has none to borrow: A8 like its bitmaps, built
 * pixel-doubled from `rows` ('X' set) on first use. */
#define PIXEL_ART(name, w, h, ...)                                                     \
    static const lv_image_dsc_t *name(void)                                            \
    {                                                                                  \
        static const char *const rows[h] = { __VA_ARGS__ };                            \
        static uint8_t px[(w) * PX * (h) * PX];                                        \
        static lv_image_dsc_t dsc;                                                     \
        if (dsc.data == NULL) {                                                        \
            build_pixel_art(&dsc, px, rows, (w), (h));                                 \
        }                                                                              \
        return &dsc;                                                                   \
    }

static void build_pixel_art(lv_image_dsc_t *dsc, uint8_t *px, const char *const *rows, int w, int h)
{
    for (int y = 0; y < h * PX; y++) {
        for (int x = 0; x < w * PX; x++) {
            px[y * w * PX + x] = rows[y / PX][x / PX] == 'X' ? 0xff : 0x00;
        }
    }
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_A8;
    dsc->header.w = (uint32_t)(w * PX);
    dsc->header.h = (uint32_t)(h * PX);
    dsc->header.stride = (uint32_t)(w * PX);
    dsc->data_size = (uint32_t)(w * PX * h * PX);
    dsc->data = px;
}

/* The "loved" mark, 7x6: the iPod had none. */
PIXEL_ART(pixel_heart, 7, 6,
    ".XX.XX.",
    "XXXXXXX",
    "XXXXXXX",
    ".XXXXX.",
    "..XXX..",
    "...X...")

#if !RPOD_IPOD_ASSETS
/* A padlock the size of the iPod's hold icon, 8x10, for builds without its
 * bitmaps -- the lock has no symbol to fall back on. */
PIXEL_ART(pixel_lock, 8, 10,
    "..XXXX..",
    ".X....X.",
    ".X....X.",
    ".X....X.",
    "XXXXXXXX",
    "XXXXXXXX",
    "XXX..XXX",
    "XXX..XXX",
    "XXXXXXXX",
    "XXXXXXXX")
#endif

/* Stand-ins without the iPod's bitmaps. */
static const char *const k_symbols[RPOD_ICON_COUNT] = {
    [RPOD_ICON_ARROW] = LV_SYMBOL_RIGHT,
    [RPOD_ICON_PLAY] = LV_SYMBOL_PLAY,
    [RPOD_ICON_PAUSE] = LV_SYMBOL_PAUSE,
    [RPOD_ICON_BATTERY] = LV_SYMBOL_BATTERY_EMPTY,
    [RPOD_ICON_REPEAT] = LV_SYMBOL_LOOP,
    [RPOD_ICON_REPEAT_ONE] = LV_SYMBOL_LOOP,
    [RPOD_ICON_SHUFFLE] = LV_SYMBOL_SHUFFLE,
    [RPOD_ICON_VOLUME_LOW] = LV_SYMBOL_VOLUME_MID,
    [RPOD_ICON_VOLUME_HIGH] = LV_SYMBOL_VOLUME_MAX,
    [RPOD_ICON_HEART] = NULL,
    [RPOD_ICON_CHECK] = LV_SYMBOL_OK,
    [RPOD_ICON_LOCK] = NULL,
    [RPOD_ICON_SPIN_UP] = LV_SYMBOL_UP,
    [RPOD_ICON_SPIN_DOWN] = LV_SYMBOL_DOWN,
    [RPOD_ICON_BELL] = LV_SYMBOL_BELL,
    [RPOD_ICON_ALARM] = LV_SYMBOL_BELL,
};

static const lv_image_dsc_t *bitmap(rpod_icon_t icon)
{
    if (icon == RPOD_ICON_HEART) {
        return pixel_heart();
    }
#if RPOD_IPOD_ASSETS
    return k_bitmaps[icon];
#else
    return icon == RPOD_ICON_LOCK ? pixel_lock() : NULL;
#endif
}

lv_obj_t *rpod_classic_icon_create(lv_obj_t *parent, rpod_icon_t icon, lv_color_t color)
{
    lv_obj_t *obj;
    const lv_image_dsc_t *bm = bitmap(icon);
    if (bm != NULL) {
        obj = lv_image_create(parent);
        lv_image_set_src(obj, bm);
    } else if (icon == RPOD_ICON_CHECK && rpod_classic_has_assets()) {
        obj = lv_label_create(parent);
        lv_obj_set_style_text_font(obj, rpod_classic_font_small(), 0);
        lv_label_set_text(obj, "\xE2\x9C\x93"); /* U+2713 */
    } else {
        obj = lv_label_create(parent);
        lv_obj_set_style_text_font(obj, &lv_font_montserrat_16, 0);
        lv_label_set_text(obj, k_symbols[icon]);
    }
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    rpod_classic_icon_set_color(obj, color);
    return obj;
}

void rpod_classic_icon_set(lv_obj_t *obj, rpod_icon_t icon)
{
    if (lv_obj_check_type(obj, &lv_image_class)) {
        const lv_image_dsc_t *bm = bitmap(icon);
        if (bm != NULL) {
            lv_image_set_src(obj, bm);
        }
    } else if (lv_obj_check_type(obj, &lv_label_class) && k_symbols[icon] != NULL) {
        lv_label_set_text(obj, k_symbols[icon]);
    }
}

/* --- Battery --------------------------------------------------------------- */

#define BATTERY_LEVELS 16

#if RPOD_IPOD_ASSETS
static const lv_image_dsc_t *const k_battery_levels[BATTERY_LEVELS] = {
    &ipod_img_25c9_x2, &ipod_img_25ca_x2, &ipod_img_25cb_x2, &ipod_img_25cc_x2,
    &ipod_img_25cd_x2, &ipod_img_25ce_x2, &ipod_img_25cf_x2, &ipod_img_25d0_x2,
    &ipod_img_25d1_x2, &ipod_img_25d2_x2, &ipod_img_25d3_x2, &ipod_img_25d4_x2,
    &ipod_img_25d5_x2, &ipod_img_25d6_x2, &ipod_img_25d7_x2, &ipod_img_25d8_x2,
};

/* The charging animation is one strip (0x25C5) of its frames stacked top to
 * bottom; each frame here is a window onto it. */
static const lv_image_dsc_t *charge_frame(int i)
{
    static lv_image_dsc_t frames[RPOD_CLASSIC_CHARGE_FRAMES];
    const lv_image_dsc_t *strip = &ipod_img_25c5_x2;
    lv_image_dsc_t *f = &frames[i];
    if (f->data == NULL) {
        *f = *strip;
        f->header.h = strip->header.h / RPOD_CLASSIC_CHARGE_FRAMES;
        f->data_size = f->header.h * strip->header.stride;
        f->data = strip->data + (size_t)i * f->data_size;
    }
    return f;
}
#endif

void rpod_classic_battery_set(lv_obj_t *obj, int level, bool charging, bool charged, int frame)
{
    frame = ((frame % RPOD_CLASSIC_CHARGE_FRAMES) + RPOD_CLASSIC_CHARGE_FRAMES) % RPOD_CLASSIC_CHARGE_FRAMES;
    int step = level < 0 ? 0 : (level * (BATTERY_LEVELS - 1) + 50) / 100;
#if RPOD_IPOD_ASSETS
    if (lv_obj_check_type(obj, &lv_image_class)) {
        lv_image_set_src(obj, charging  ? charge_frame(frame)
                              : charged ? &ipod_img_25c8_x2
                                        : k_battery_levels[step]);
        return;
    }
#endif
    if (lv_obj_check_type(obj, &lv_label_class)) {
        /* Charging fills from empty to full over and over, as the iPod's does. */
        static const char *const k_fills[RPOD_CLASSIC_CHARGE_FRAMES] = {
            LV_SYMBOL_BATTERY_EMPTY, LV_SYMBOL_BATTERY_1, LV_SYMBOL_BATTERY_2,
            LV_SYMBOL_BATTERY_3,     LV_SYMBOL_BATTERY_FULL,
        };
        int fill = charging ? frame
                   : charged ? RPOD_CLASSIC_CHARGE_FRAMES - 1
                             : (step * (RPOD_CLASSIC_CHARGE_FRAMES - 1) + (BATTERY_LEVELS - 1) / 2) /
                                   (BATTERY_LEVELS - 1);
        lv_label_set_text_static(obj, k_fills[fill]);
    }
}

void rpod_classic_icon_set_color(lv_obj_t *obj, lv_color_t color)
{
    if (lv_obj_check_type(obj, &lv_image_class)) {
        lv_obj_set_style_image_recolor(obj, color, 0);
        lv_obj_set_style_image_recolor_opa(obj, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_text_color(obj, color, 0);
    }
}

/* --- Rows ------------------------------------------------------------------ */

void rpod_classic_style_row(lv_obj_t *btn)
{
    const rpod_metrics_t *m = rpod_metrics();
    lv_obj_set_height(btn, m->row_h);
    lv_obj_set_style_radius(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_outline_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_left(btn, m->row_pad_x, 0);
    lv_obj_set_style_pad_right(btn, ROW_PAD_RIGHT, 0);
    lv_obj_set_style_pad_ver(btn, 0, 0);
    lv_obj_set_style_pad_column(btn, m->row_gap, 0);
    lv_obj_set_style_text_font(btn, m->font_body, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(btn, RPOD_COLOR_TEXT, 0);

    /* Inverted while it's the selection. Set for FOCUS_KEY as well as
     * FOCUSED: the encoder focuses with both, and LVGL's default theme
     * styles FOCUS_KEY, which outranks a FOCUSED-only style. Its PRESSED
     * and FOCUS_KEY styles also grow the row and darken it -- the iPod's
     * rows did neither. */
    static const lv_state_t sel_states[] = { LV_STATE_FOCUSED, LV_STATE_FOCUS_KEY };
    for (size_t i = 0; i < sizeof(sel_states) / sizeof(sel_states[0]); i++) {
        lv_obj_set_style_bg_color(btn, RPOD_COLOR_ACCENT, sel_states[i]);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, sel_states[i]);
        lv_obj_set_style_text_color(btn, RPOD_COLOR_SEL_TEXT, sel_states[i]);
        lv_obj_set_style_transform_width(btn, 0, sel_states[i]);
    }
    lv_obj_set_style_transform_width(btn, 0, LV_STATE_PRESSED);
    lv_obj_set_style_recolor_opa(btn, LV_OPA_TRANSP, LV_STATE_PRESSED);
}

/* --- Pixel rectangles ------------------------------------------------------ */

static lv_obj_t *rect(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, lv_color_t color)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_pos(r, x, y);
    lv_obj_set_size(r, w, h);
    lv_obj_set_style_bg_color(r, color, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

/* --- Scrollbar --------------------------------------------------------------
 *
 * ScrollBar Draw (firmware 0x5A6E0, thumb rect 0x3C49C), in iPod pixels on a
 * bar 10 wide: white ground, rules down x = 1 and x = 9 and along the
 * bottom row, and a black thumb at x = 3..7 that runs from 1 below the top
 * to 2 above the bottom, never shorter than 3. No top rule -- the title
 * bar's sits just above. Children, in order: the three rules, the thumb. */

#define SB_THUMB_CHILD 3

lv_obj_t *rpod_classic_scrollbar_create(lv_obj_t *parent, int32_t h)
{
    lv_color_t fg = RPOD_COLOR_TEXT;
    lv_obj_t *sb = lv_obj_create(parent);
    lv_obj_remove_style_all(sb);
    lv_obj_set_size(sb, RPOD_CLASSIC_SCROLLBAR_W, h);
    lv_obj_set_style_bg_color(sb, RPOD_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(sb, LV_OPA_COVER, 0);
    lv_obj_remove_flag(sb, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    rect(sb, 1 * PX, 0, PX, h, fg);
    rect(sb, 9 * PX, 0, PX, h, fg);
    rect(sb, 1 * PX, h - PX, 9 * PX, PX, fg);
    rect(sb, 3 * PX, 1 * PX, 5 * PX, 3 * PX, fg);

    lv_obj_add_flag(sb, LV_OBJ_FLAG_HIDDEN);
    return sb;
}

bool rpod_classic_scrollbar_set(lv_obj_t *sb, int32_t content, int32_t visible, int32_t offset)
{
    if (content <= visible || visible <= 0) {
        lv_obj_add_flag(sb, LV_OBJ_FLAG_HIDDEN);
        return false;
    }
    lv_obj_remove_flag(sb, LV_OBJ_FLAG_HIDDEN);

    /* In whole iPod pixels, so the thumb stays on the doubled grid. */
    int32_t track = lv_obj_get_style_height(sb, 0) / PX - 3;
    int32_t thumb = (int32_t)((int64_t)visible * track / content);
    thumb = thumb < 3 ? 3 : thumb > track ? track : thumb;
    int32_t room = track - thumb;
    if (offset < 0) {
        offset = 0;
    } else if (offset > content - visible) {
        offset = content - visible;
    }
    int32_t off = (int32_t)((int64_t)offset * room / (content - visible));

    lv_obj_t *t = lv_obj_get_child(sb, SB_THUMB_CHILD);
    lv_obj_set_y(t, (1 + off) * PX);
    lv_obj_set_height(t, thumb * PX);
    return true;
}

/* --- Slider -----------------------------------------------------------------
 *
 * Slider Draw (firmware 0x5AB30): the filled part first, inset 1 inside the
 * frame, then the frame over it -- straight edges stopping 2 short of each
 * corner, with the corner pixel set diagonally inside. Children, in order:
 * the fill, the eight frame pieces. */

#define SLIDER_FILL_CHILD 0

lv_obj_t *rpod_classic_slider_create(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_color_t fg = RPOD_COLOR_TEXT;
    int32_t W = w / PX, H = h / PX;

    lv_obj_t *s = lv_obj_create(parent);
    lv_obj_remove_style_all(s);
    lv_obj_set_size(s, w, h);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *fill = rect(s, PX, PX, 0, (H - 2) * PX, rpod_theme()->fill);
    lv_obj_add_flag(fill, LV_OBJ_FLAG_HIDDEN);

    rect(s, 0, 2 * PX, PX, (H - 4) * PX, fg);            /* left edge */
    rect(s, (W - 1) * PX, 2 * PX, PX, (H - 4) * PX, fg); /* right edge */
    rect(s, 2 * PX, 0, (W - 4) * PX, PX, fg);            /* top edge */
    rect(s, 2 * PX, (H - 1) * PX, (W - 4) * PX, PX, fg); /* bottom edge */
    rect(s, PX, PX, PX, PX, fg);                         /* corners */
    rect(s, (W - 2) * PX, PX, PX, PX, fg);
    rect(s, PX, (H - 2) * PX, PX, PX, fg);
    rect(s, (W - 2) * PX, (H - 2) * PX, PX, PX, fg);
    return s;
}

void rpod_classic_slider_set(lv_obj_t *slider, int32_t value, int32_t range)
{
    int32_t W = lv_obj_get_style_width(slider, 0) / PX;
    if (range <= 0 || value < 0) {
        value = 0;
        range = 1;
    } else if (value > range) {
        value = range;
    }
    int32_t fill_w = (int32_t)((int64_t)(W - 2) * value / range);
    lv_obj_t *fill = lv_obj_get_child(slider, SLIDER_FILL_CHILD);
    if (fill_w <= 0) {
        lv_obj_add_flag(fill, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(fill, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(fill, fill_w * PX);
    }
}
