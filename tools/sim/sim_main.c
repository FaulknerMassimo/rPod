/*
 * Desktop LVGL simulator for rPod UI development.
 * Uses LVGL's built-in SDL window driver — no Pi hardware required.
 * See docs/PLAN.md §5.4.
 */

#include "lvgl.h"
#include <unistd.h>

/* Landscape 320x240 — click-wheel iPods had landscape screens above the
 * wheel despite the device body being portrait overall (docs/PLAN.md §5). */
#define SIM_HOR_RES 320
#define SIM_VER_RES 240

int main(void)
{
    lv_init();

    lv_display_t *disp = lv_sdl_window_create(SIM_HOR_RES, SIM_VER_RES);
    lv_sdl_mouse_create();
    lv_sdl_mousewheel_create();
    lv_sdl_keyboard_create();

    lv_obj_t *scr = lv_display_get_screen_active(disp);

    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, "rPod");
    lv_obj_align(label, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *spinner = lv_spinner_create(scr);
    lv_obj_set_size(spinner, 40, 40);
    lv_obj_align(spinner, LV_ALIGN_CENTER, 0, 40);

    while (1) {
        uint32_t idle_ms = lv_timer_handler();
        usleep(idle_ms * 1000);
    }

    return 0;
}
