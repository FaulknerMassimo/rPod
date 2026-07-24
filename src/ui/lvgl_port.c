#include "lvgl_port.h"

#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

/*
 * rPod owns the framebuffer flush instead of using LVGL's stock fbdev flush.
 *
 * Why: LVGL's flush writes the framebuffer one row per pwrite() (128 * 2 = 256
 * bytes on this panel). On the Waveshare 1.44" HAT -- a MIPI-DBI command-mode
 * SPI panel driven by kernel 6.18's panel-mipi-dbi on drm_fbdev_dma with
 * deferred I/O -- those tiny per-row writes never reach the glass: the frame
 * lands in the emulated fbdev's shadow buffer but the driver only flushes it to
 * the panel for LARGE writes. Measured on hardware via the SPI master's
 * bytes_tx counter: a single >=16 KB write always presents; 128 separate 256 B
 * row writes present nothing (regardless of content). That left the screen
 * blank on boot and full of residue with LVGL's stock flush.
 *
 * Fix: in FULL render mode (see LV_LINUX_FBDEV_RENDER_MODE in lv_conf.h) the
 * whole 128x128 frame is handed to the flush as one area, so we write it as a
 * single ~32 KB pwrite, which the driver reliably presents. A row-by-row
 * fallback covers any non-full-width area (not hit in FULL mode).
 */
typedef struct {
    int fd;               /* our own O_RDWR handle to the fb node */
    uint32_t line_length; /* fb row stride in bytes (FBIOGET_FSCREENINFO) */
} rpod_fb_ctx_t;

/* One display on device, so a file-static context is enough (cf. app.c's
 * g_stack). Kept off lv_display_set_user_data() so nothing else can clobber it. */
static rpod_fb_ctx_t g_fb = { .fd = -1 };

static void rpod_fb_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    if (g_fb.fd >= 0) {
        const uint32_t bpp = lv_color_format_get_size(lv_display_get_color_format(disp));
        const int32_t w = lv_area_get_width(area);
        const int32_t h = lv_area_get_height(area);
        const uint32_t row_bytes = (uint32_t)w * bpp;
        const off_t base = (off_t)area->y1 * g_fb.line_length + (off_t)area->x1 * bpp;

        if (row_bytes == g_fb.line_length) {
            /* Full-width, so the rows are one contiguous run in both the source
             * buffer and the framebuffer: write them all in ONE pwrite. This is
             * the path FULL render mode always takes, and it's what makes the
             * panel present (see the header comment). */
            if (pwrite(g_fb.fd, px_map, (size_t)row_bytes * h, base) < 0) {
                perror("rpod: fb pwrite");
            }
        } else {
            /* Partial-width area: fb rows aren't contiguous, so fall back to a
             * write per row. Not reached in FULL mode. */
            const uint8_t *src = px_map;
            for (int32_t y = 0; y < h; y++) {
                if (pwrite(g_fb.fd, src, row_bytes, base + (off_t)y * g_fb.line_length) < 0) {
                    perror("rpod: fb pwrite");
                    break;
                }
                src += row_bytes;
            }
        }
    }

    lv_display_flush_ready(disp);
}

lv_display_t *rpod_lvgl_port_init(const char *fb_path)
{
    lv_init();

    lv_display_t *disp = lv_linux_fbdev_create();
    if (disp == NULL) {
        fprintf(stderr, "rpod: lv_linux_fbdev_create failed\n");
        return NULL;
    }

    /* Reuse LVGL's fbdev setup: it opens the node, reads geometry, unblanks,
     * sets the color format + resolution, and (in FULL render mode) allocates
     * full-screen draw buffers. We then override only the flush, below. */
    if (lv_linux_fbdev_set_file(disp, fb_path) != LV_RESULT_OK) {
        fprintf(stderr, "rpod: failed to open framebuffer %s\n", fb_path);
        lv_display_delete(disp);
        return NULL;
    }

    /* Our own fd + row stride for rpod_fb_flush_cb (LVGL doesn't expose its). */
    g_fb.fd = open(fb_path, O_RDWR);
    if (g_fb.fd < 0) {
        perror("rpod: cannot open framebuffer for flush");
        lv_display_delete(disp);
        return NULL;
    }
    struct fb_fix_screeninfo finfo;
    if (ioctl(g_fb.fd, FBIOGET_FSCREENINFO, &finfo) != 0) {
        perror("rpod: FBIOGET_FSCREENINFO");
        close(g_fb.fd);
        g_fb.fd = -1;
        lv_display_delete(disp);
        return NULL;
    }
    g_fb.line_length = finfo.line_length;
    lv_display_set_flush_cb(disp, rpod_fb_flush_cb);

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
