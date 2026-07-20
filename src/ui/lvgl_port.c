#include "lvgl_port.h"

#include <stdio.h>

lv_display_t *rpod_lvgl_port_init(const char *fb_path)
{
    lv_init();

    lv_display_t *disp = lv_linux_fbdev_create();
    if (disp == NULL) {
        fprintf(stderr, "rpod: lv_linux_fbdev_create failed\n");
        return NULL;
    }

    if (lv_linux_fbdev_set_file(disp, fb_path) != LV_RESULT_OK) {
        fprintf(stderr, "rpod: failed to open framebuffer %s\n", fb_path);
        lv_display_delete(disp);
        return NULL;
    }

    return disp;
}
