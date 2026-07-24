#include "lvgl_port.h"

#include <stdio.h>
#include <stdlib.h>

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

    /* Pixel byte order. The validated on-device path (Waveshare 1.44" HAT via
     * panel-mipi-dbi, docs/PLAN.md §5.1) delivers LVGL's native RGB565 to the
     * glass correctly, so no swap by default -- confirmed against the real panel
     * with tools/fb-test's fill mode (pure R/G/B read back true). A panel that
     * latches 16-bit pixels in the opposite byte order (some fbtft SPI panels
     * do, turning blue 0x001F into yellow-green 0x1F00 while black/white are
     * unaffected) can opt in with RPOD_FB_SWAP=1 -- no rebuild required.
     * (Legacy: /etc/rpod/env may still carry RPOD_FB_NO_SWAP=1 from before this
     * default flipped; it's now a harmless no-op.) */
    if (getenv("RPOD_FB_SWAP") != NULL) {
        lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
    }

    return disp;
}
