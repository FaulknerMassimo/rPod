#include "metrics.h"

/* Landscape: the original 2" ST7789V build. These values are exactly what
 * the screens hardcoded before the refactor -- do not "tidy" them, the
 * classic board's rendering is defined by their staying identical. */
static const rpod_metrics_t k_landscape = {
    .form           = RPOD_FORM_LANDSCAPE,
    .screen_w       = 320,
    .screen_h       = 240,
    .header_h       = 28,
    .font_title     = &lv_font_montserrat_20,
    .font_body      = &lv_font_montserrat_16,
    .font_small     = &lv_font_montserrat_14,
    .font_np_glyph  = &lv_font_montserrat_24,
    .list_art_size  = 40,
    .row_pad_x      = 14,
    .row_pad_y      = 8,
    .row_gap        = 8,
    .row_heart_size = 18,
    .list_margin    = 8,
};

/* Square: the Waveshare 1.44" LCD HAT (128x128 ST7735S). Smaller type and
 * tighter rows so a legible ~4-5 rows fit; font_body/font_small are the
 * text-only 12/10px Montserrat (no symbol glyphs -- see lv_conf.h). */
static const rpod_metrics_t k_square = {
    .form           = RPOD_FORM_SQUARE,
    .screen_w       = 128,
    .screen_h       = 128,
    .header_h       = 18,
    .font_title     = &lv_font_montserrat_14,
    .font_body      = &lv_font_montserrat_10,
    .font_small     = &lv_font_montserrat_10,
    .font_np_glyph  = &lv_font_montserrat_16,
    .list_art_size  = 20,
    .row_pad_x      = 8,
    .row_pad_y      = 3,
    .row_gap        = 4,
    .row_heart_size = 12,
    .list_margin    = 3,
};

static const rpod_metrics_t *g_metrics = &k_landscape;

void rpod_metrics_init(rpod_form_factor_t form)
{
    g_metrics = (form == RPOD_FORM_SQUARE) ? &k_square : &k_landscape;
}

const rpod_metrics_t *rpod_metrics(void)
{
    return g_metrics;
}
