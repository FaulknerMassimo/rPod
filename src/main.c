/*
 * rPod application entry point.
 *
 * Phase 1: LVGL on the fbtft framebuffer, per docs/PLAN.md §9. Same "Hello"
 * label + spinner scene as tools/sim/sim_main.c so the two are directly
 * comparable. Later phases wire in the wheel input socket and MPD client.
 */

#include "ui/lvgl_port.h"

#include <unistd.h>

#define RPOD_FB_PATH "/dev/fb1"

int main(void)
{
    lv_display_t *disp = rpod_lvgl_port_init(RPOD_FB_PATH);
    if (disp == NULL) {
        return 1;
    }

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
