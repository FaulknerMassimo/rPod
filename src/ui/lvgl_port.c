#include "lvgl_port.h"

#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

/*
 * rPod owns the framebuffer flush instead of using LVGL's stock fbdev one.
 * LVGL renders in DIRECT mode into a single full-screen RAM buffer (see
 * LV_LINUX_FBDEV_RENDER_MODE in lv_conf.h); on the last flush of each
 * refresh, rpod_fb_flush_cb() writes the union of that refresh's dirty rows
 * to the framebuffer in one pwrite().
 *
 * Why: LVGL's stock flush wrote each 40-line partial-render chunk as soon as
 * it was rendered, one pwrite() per row. fbtft pushes the framebuffer to the
 * panel from a deferred-I/O timer, which could fire between chunks -- sending
 * a half-drawn frame, then the rest a timer period later: a visible
 * two-stage wipe down the panel on every scroll. One write per refresh hands
 * fbtft complete frames only.
 *
 * Measured on the Pi 3B dev board (fbtft fb_st7789v, kernel 6.18) via the SPI
 * controller's counters in /sys/bus/spi/devices/spi0.0/statistics: fbtft's
 * write() path marks the *entire* display dirty on any write, so every push
 * is a full 153,600-byte frame whatever changed. Writes through an mmap are
 * tracked per 4 KiB page instead, and did push just the touched rows -- but
 * only for a while after boot: after a few process restarts page tracking
 * stopped entirely (no SPI traffic even for a brand-new mapping, until
 * reboot) while pwrite() kept working. So pwrite(), and the SPI clock and
 * deferred-I/O delay in system/config.txt.d/rpod.txt keep full-frame pushes
 * fast (~20 ms each at 62.5 MHz).
 */
typedef struct {
    int fd;               /* our own O_RDWR handle to the fb node */
    uint32_t line_length; /* fb row stride in bytes (FBIOGET_FSCREENINFO) */
    char id[sizeof(((struct fb_fix_screeninfo *)0)->id) + 1]; /* driver name, e.g. "fb_st7789v" */
    int32_t dirty_y1;     /* rows touched since the last push; y1 > y2 = none */
    int32_t dirty_y2;
} rpod_fb_ctx_t;

/* One display on device, so a file-static context is enough (cf. app.c's
 * g_stack). Kept off lv_display_set_user_data() so nothing else can clobber it. */
static rpod_fb_ctx_t g_fb = { .fd = -1, .dirty_y1 = INT32_MAX, .dirty_y2 = -1 };

static void fb_write_rows(const uint8_t *src, uint32_t src_stride, int32_t y1, int32_t rows)
{
    const size_t row_bytes = src_stride < g_fb.line_length ? src_stride : g_fb.line_length;
    const off_t base = (off_t)y1 * g_fb.line_length;

    if (src_stride == g_fb.line_length) {
        /* Full-width rows are one contiguous run in both buffers. */
        if (pwrite(g_fb.fd, src, (size_t)rows * row_bytes, base) < 0) {
            perror("rpod: fb pwrite");
        }
        return;
    }
    for (int32_t r = 0; r < rows; r++) {
        if (pwrite(g_fb.fd, src + (size_t)r * src_stride, row_bytes,
                   base + (off_t)r * g_fb.line_length) < 0) {
            perror("rpod: fb pwrite");
            return;
        }
    }
}

/* DIRECT mode: `px_map` is the whole-screen buffer and `area` is one of this
 * refresh's dirty areas, already rendered in place. Just note its rows until
 * the refresh's last area, then write them all at once. */
static void rpod_fb_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    if (area->y1 < g_fb.dirty_y1) {
        g_fb.dirty_y1 = area->y1;
    }
    if (area->y2 > g_fb.dirty_y2) {
        g_fb.dirty_y2 = area->y2;
    }

    if (lv_display_flush_is_last(disp)) {
        const int32_t hor = lv_display_get_horizontal_resolution(disp);
        const int32_t ver = lv_display_get_vertical_resolution(disp);
        const uint32_t stride = lv_draw_buf_width_to_stride((uint32_t)hor, lv_display_get_color_format(disp));

        int32_t y1 = g_fb.dirty_y1 < 0 ? 0 : g_fb.dirty_y1;
        int32_t y2 = g_fb.dirty_y2 >= ver ? ver - 1 : g_fb.dirty_y2;
        if (y1 <= y2) {
            fb_write_rows(px_map + (size_t)y1 * stride, stride, y1, y2 - y1 + 1);
        }
        g_fb.dirty_y1 = INT32_MAX;
        g_fb.dirty_y2 = -1;
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
     * sets the color format + resolution, and allocates the full-screen
     * DIRECT-mode draw buffer. We then override only the flush, below. */
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
    snprintf(g_fb.id, sizeof(g_fb.id), "%.*s", (int)sizeof(finfo.id), finfo.id);

    lv_display_set_flush_cb(disp, rpod_fb_flush_cb);
    return disp;
}

/* Sleep switches only the backlight, through the backlight device fbtft
 * registers under its driver's name -- never FBIOBLANK. fbtft's blank sends
 * the panel DISPOFF/DISPON from the ioctl while its deferred-I/O worker may
 * still be streaming a frame on the same SPI bus and D/C line, with nothing
 * serialising the two. On the Pi 3B (dmesg, with fbtft's register-write
 * debug on) DISPOFF went out ~20 ms into a frame's RAMWR stream, and after a
 * few sleeps the panel stayed black with its backlight on and frames still
 * flowing. bl_power only toggles the backlight GPIO. It's
 * root's by default; system/udev/99-rpod-panel.rules lets group video (rpod)
 * write it. */
void rpod_lvgl_port_set_power(bool on)
{
    if (g_fb.id[0] == '\0') {
        return;
    }
    char path[96];
    snprintf(path, sizeof(path), "/sys/class/backlight/%s/bl_power", g_fb.id);
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        perror("rpod: backlight bl_power");
        return;
    }
    /* FB_BLANK_UNBLANK (0) is on; FB_BLANK_POWERDOWN (4) is off. */
    const char *v = on ? "0\n" : "4\n";
    if (write(fd, v, 2) != 2) {
        perror("rpod: backlight bl_power write");
    }
    close(fd);
}
