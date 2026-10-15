#include "metrics.h"

#include "classic.h"
#include "theme.h"

/* Glass: these values are exactly what the screens hardcoded before they
 * moved into this struct -- do not "tidy" them, the panel's rendering is
 * defined by their staying identical. */
static const rpod_metrics_t k_glass = {
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
    .row_h          = 0,
};

/* Classic: the iPod 4G's layout at 2x (re/ipod4/SPEC.md §8) -- its 19px
 * title bar becomes 38, its 6px label inset 12. Rows are 33px rather than a
 * strict 36 so six of them still fit under the title bar: 2x of the iPod's
 * 128 lines is 256, 16 more than the panel has. Fonts come from
 * ui/classic.c, so they're filled in at first use. */
static rpod_metrics_t g_classic = {
    .screen_w       = 320,
    .screen_h       = 240,
    .header_h       = 38,
    .list_art_size  = 40,
    .row_pad_x      = 12,
    .row_pad_y      = 0,
    .row_gap        = 12,
    .row_heart_size = 14,
    .list_margin    = 0,
    .row_h          = 33,
};

const rpod_metrics_t *rpod_metrics(void)
{
    if (!rpod_theme()->classic) {
        return &k_glass;
    }
    if (g_classic.font_body == NULL) {
        g_classic.font_title = rpod_classic_font_body();
        g_classic.font_body = rpod_classic_font_body();
        g_classic.font_small = rpod_classic_font_small();
        g_classic.font_np_glyph = rpod_classic_font_body();
    }
    return &g_classic;
}
