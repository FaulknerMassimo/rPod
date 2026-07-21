#include "theme.h"

void rpod_theme_style_screen(lv_obj_t *screen)
{
    lv_obj_set_style_bg_color(screen, RPOD_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, RPOD_COLOR_TEXT, 0);
}

void rpod_theme_style_glass_bar(lv_obj_t *bar)
{
    lv_obj_set_style_bg_color(bar, RPOD_COLOR_GLASS_FILL, 0);
    lv_obj_set_style_bg_opa(bar, RPOD_GLASS_FILL_OPA, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, RPOD_COLOR_GLASS_EDGE, 0);
    lv_obj_set_style_border_opa(bar, RPOD_GLASS_EDGE_OPA, 0);
}

void rpod_theme_style_glass_panel(lv_obj_t *panel, int32_t radius)
{
    lv_obj_set_style_bg_color(panel, RPOD_COLOR_GLASS_FILL, 0);
    lv_obj_set_style_bg_opa(panel, RPOD_GLASS_FILL_OPA, 0);
    lv_obj_set_style_radius(panel, radius, 0);
    lv_obj_set_style_border_side(panel, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, RPOD_COLOR_GLASS_EDGE, 0);
    lv_obj_set_style_border_opa(panel, RPOD_GLASS_EDGE_OPA, 0);
    lv_obj_set_style_shadow_width(panel, 10, 0);
    lv_obj_set_style_shadow_ofs_y(panel, 3, 0);
    lv_obj_set_style_shadow_color(panel, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(panel, 70, 0);
}
