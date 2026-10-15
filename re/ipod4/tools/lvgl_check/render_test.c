/* Headless LVGL 9.5 render of one iPod menu row using the generated font + A8 image. */
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include "lvgl.h"
#include "ipod_fonts.h"
#include "ipod_images.h"
#define W 160
#define H 128
static uint8_t drawbuf[W * H * 2];
static uint16_t fb[W * H];
static uint32_t tick(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *px) {
    uint16_t *p = (uint16_t *)px;
    for (int y = a->y1; y <= a->y2; y++) for (int x = a->x1; x <= a->x2; x++) fb[y * W + x] = *p++;
    lv_display_flush_ready(d);
}
static lv_obj_t *label(lv_obj_t *par, const lv_font_t *f, const char *s, int x, int y, lv_color_t c) {
    lv_obj_t *l = lv_label_create(par);
    lv_obj_set_style_text_font(l, f, 0); lv_obj_set_style_text_color(l, c, 0);
    lv_label_set_text(l, s); lv_obj_set_pos(l, x, y); return l;
}
int main(int argc, char **argv) {
    lv_init(); lv_tick_set_cb(tick);
    lv_display_t *d = lv_display_create(W, H);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d, drawbuf, NULL, sizeof drawbuf, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(d, flush);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    /* selected row: y 20..37 black, label at row.top + (18-16)/2, arrow at right-2-12, row.top+5 */
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar); lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, lv_color_black(), 0); lv_obj_set_pos(bar, 0, 20); lv_obj_set_size(bar, W, 18);
    label(scr, &ipod_font_chicago_12, "Music", 6, 21, lv_color_white());
    lv_obj_t *img = lv_image_create(scr); lv_image_set_src(img, &ipod_img_25ae); lv_obj_set_pos(img, W - 2 - 12, 25);
    lv_obj_set_style_image_recolor(img, lv_color_white(), 0); lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    label(scr, &ipod_font_chicago_12, "Extras  \xc3\x84\xc3\xb6 \xe2\x80\xa6 jg", 6, 39, lv_color_black());
    label(scr, &ipod_font_pixo_7_bold, "3 of 12 jgy;", 5, 60, lv_color_black());
    label(scr, &ipod_font_espy_sans_10, "Espy Sans 10 jgy", 5, 75, lv_color_black());
    lv_obj_t *img2 = lv_image_create(scr); lv_image_set_src(img2, &ipod_img_25b5); lv_obj_set_pos(img2, 120, 95);
    lv_obj_set_style_image_recolor(img2, lv_color_black(), 0); lv_obj_set_style_image_recolor_opa(img2, LV_OPA_COVER, 0);
    lv_refr_now(d);
    FILE *f = fopen(argv[1], "wb"); fprintf(f, "P5\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) { uint16_t v = fb[i]; fputc(((v >> 5) & 0x3f) * 255 / 63, f); }
    fclose(f); return 0;
}
