#include "metrics.h"

/* These values are exactly what the screens hardcoded before they moved into
 * this struct -- do not "tidy" them, the panel's rendering is defined by
 * their staying identical. */
static const rpod_metrics_t k_metrics = {
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

const rpod_metrics_t *rpod_metrics(void)
{
    return &k_metrics;
}
