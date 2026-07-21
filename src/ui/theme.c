#include "theme.h"

void rpod_theme_style_screen(lv_obj_t *screen)
{
    lv_obj_set_style_bg_color(screen, RPOD_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, RPOD_COLOR_TEXT, 0);
}

lv_obj_t *rpod_theme_create_header(lv_obj_t *screen, const char *title)
{
    lv_obj_t *header = lv_obj_create(screen);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, LV_PCT(100), RPOD_HEADER_HEIGHT);
    lv_obj_set_style_bg_color(header, RPOD_COLOR_HEADER_BG, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(header);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_color(label, RPOD_COLOR_TEXT, 0);
    lv_obj_center(label);

    return header;
}
