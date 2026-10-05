#include "lvgl_port.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/*
 * rPod owns the whole framebuffer path instead of using LVGL's stock fbdev
 * driver. LVGL renders the 320x240 landscape UI in DIRECT mode into one
 * full-screen RAM buffer; on the last flush of each refresh,
 * rpod_fb_flush_cb() copies what changed into the framebuffer with one
 * pwrite(), then fsync()s it to push the frame to the panel right away.
 *
 * One write per refresh: fbtft pushes the framebuffer from a deferred-I/O
 * timer, and LVGL's stock flush wrote each 40-line chunk as it rendered --
 * the timer could fire between chunks, sending half a frame, then the rest:
 * a two-stage wipe on every scroll. pwrite(), not mmap: re-tested on kernel
 * 6.18 against the SPI controller's counters
 * (/sys/bus/spi/devices/spi0.0/statistics), mmap'd writes pushed, page by
 * page, for a while after boot, then stopped reaching the panel entirely
 * until a reboot, while pwrite() kept working. fbtft's write() path marks
 * the whole display dirty, so every push is a full 153,600-byte frame
 * whatever changed: 25.4 ms, measured -- 50 MHz SPI, the requested 62.5
 * divided down from the 400 MHz core clock (pinned there in config.txt:
 * left free, the core idles at 275 MHz and the SPI clock drops with it).
 *
 * fsync() is fb_deferred_io_fsync(): it runs fbtft's pending push now and
 * returns once it's out. That takes the deferred-I/O delay out of every
 * frame, and since the next pwrite() can't start until the push is done,
 * LVGL can't overwrite the framebuffer while a push is still reading it out
 * (its 33 ms refresh period used to land inside the delay + push, tearing
 * the frame's tail).
 *
 * The landscape rotation happens here, not in the panel, and that's what
 * keeps scrolling from tearing diagonally. The ST7789V refreshes the glass
 * one native (portrait) row at a time. With fbtft's rotate=90, MADCTL maps
 * each landscape row the driver sends onto a native *column*: the push
 * swept the glass perpendicular to the refresh, so every refresh pass that
 * overlapped a push showed new pixels on one side of a slanted line and old
 * ones on the other -- a diagonal tear on every frame that moved anything.
 * With rotate=0 (or 180) the framebuffer is the native 240x320 raster, this
 * rotates each refresh into it, and pushes write the glass in the order it
 * refreshes. Then the two only cross when one catches the other mid-frame,
 * along a straight line, and system/overlays/rpod-panel.dts slows the
 * panel's refresh to about the push's speed so that's rare too. (The
 * Waveshare module doesn't break out the panel's TE line, so there's
 * nothing to sync pushes to.) A landscape framebuffer still works -- copied
 * straight through, tearing as before.
 */

#define RPOD_HOR_RES 320 /* the UI's landscape resolution (docs/PLAN.md §5) */
#define RPOD_VER_RES 240

typedef struct {
    int fd;                /* our O_RDWR handle to the fb node */
    uint32_t line_length;  /* fb row stride in bytes (FBIOGET_FSCREENINFO) */
    uint32_t rotate;       /* fb var.rotate; 0/180 = native portrait, rotated here */
    uint16_t *portrait;    /* rotated copy of the screen (portrait fb only) */
    bool sync;             /* fsync() pushes; cleared if the driver can't */
    char id[sizeof(((struct fb_fix_screeninfo *)0)->id) + 1]; /* driver name, e.g. "fb_st7789v" */
    lv_area_t dirty;       /* union of this refresh's areas; x1 > x2 = none */
} rpod_fb_ctx_t;

/* One display on device, so a file-static context is enough (cf. app.c's
 * g_stack). Kept off lv_display_set_user_data() so nothing else can clobber it. */
static rpod_fb_ctx_t g_fb = {
    .fd = -1,
    .dirty = { .x1 = RPOD_HOR_RES, .y1 = RPOD_VER_RES, .x2 = -1, .y2 = -1 },
};

static void fb_pwrite(const void *src, size_t len, off_t off)
{
    if (pwrite(g_fb.fd, src, len, off) < 0) {
        perror("rpod: fb pwrite");
    }
}

/*
 * Rotates the landscape rectangle `a` of `src` into the portrait copy, then
 * writes the portrait rows it touched -- one per landscape column.
 * Landscape (x, y) lands where fbtft's old rotate=90 put it on the glass:
 * MADCTL MV|MY sent it to native (column y, row 319 - x); unrotated
 * (rotate=0) that's portrait (y, 319 - x), and rotate=180 (MX|MY) mirrors
 * both, giving (239 - y, x). The two are the same picture written in
 * opposite directions, so whichever matches the panel's refresh direction
 * can be picked in config.txt without touching the image.
 */
static void push_rotated(const uint16_t *src, const lv_area_t *a)
{
    const uint32_t pstride = g_fb.line_length / sizeof(uint16_t);
    const bool flip = g_fb.rotate == 180;

    for (int32_t x = a->x1; x <= a->x2; x++) {
        const int32_t py = flip ? x : RPOD_HOR_RES - 1 - x;
        uint16_t *row = g_fb.portrait + (size_t)py * pstride;
        for (int32_t y = a->y1; y <= a->y2; y++) {
            row[flip ? RPOD_VER_RES - 1 - y : y] = src[(size_t)y * RPOD_HOR_RES + x];
        }
    }

    const int32_t py1 = flip ? a->x1 : RPOD_HOR_RES - 1 - a->x2;
    const int32_t rows = a->x2 - a->x1 + 1;
    fb_pwrite(g_fb.portrait + (size_t)py1 * pstride, (size_t)rows * g_fb.line_length,
              (off_t)py1 * g_fb.line_length);
}

/* Landscape framebuffer (fbtft rotating): rows y1..y2 straight across. */
static void push_rows(const uint8_t *src, int32_t y1, int32_t y2)
{
    const size_t stride = RPOD_HOR_RES * sizeof(uint16_t);
    if (stride == g_fb.line_length) {
        fb_pwrite(src + (size_t)y1 * stride, (size_t)(y2 - y1 + 1) * stride,
                  (off_t)y1 * g_fb.line_length);
        return;
    }
    for (int32_t y = y1; y <= y2; y++) {
        fb_pwrite(src + (size_t)y * stride, stride, (off_t)y * g_fb.line_length);
    }
}

/* DIRECT mode: `px_map` is the whole landscape screen and `area` is one of
 * this refresh's dirty areas, already rendered in place. Just note it until
 * the refresh's last area, then push them all at once. */
static void rpod_fb_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    lv_area_t *d = &g_fb.dirty;
    d->x1 = LV_MIN(d->x1, area->x1);
    d->y1 = LV_MIN(d->y1, area->y1);
    d->x2 = LV_MAX(d->x2, area->x2);
    d->y2 = LV_MAX(d->y2, area->y2);

    if (lv_display_flush_is_last(disp)) {
        lv_area_t a = {
            .x1 = LV_MAX(d->x1, 0),
            .y1 = LV_MAX(d->y1, 0),
            .x2 = LV_MIN(d->x2, RPOD_HOR_RES - 1),
            .y2 = LV_MIN(d->y2, RPOD_VER_RES - 1),
        };
        if (a.x1 <= a.x2 && a.y1 <= a.y2) {
            if (g_fb.portrait != NULL) {
                push_rotated((const uint16_t *)px_map, &a);
            } else {
                push_rows(px_map, a.y1, a.y2);
            }
            if (g_fb.sync && fsync(g_fb.fd) != 0) {
                perror("rpod: fb fsync (falling back to fbtft's deferred push)");
                g_fb.sync = false;
            }
        }
        *d = (lv_area_t){ .x1 = RPOD_HOR_RES, .y1 = RPOD_VER_RES, .x2 = -1, .y2 = -1 };
    }

    lv_display_flush_ready(disp);
}

/* LVGL's millisecond clock. lv_linux_fbdev_create() used to install this as
 * a side effect; without it lv_tick_get() stays 0 and no lv_timer ever
 * fires -- the click wheel's socket poll, MPD status, the clock, all of it. */
static uint32_t tick_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u);
}

lv_display_t *rpod_lvgl_port_init(const char *fb_path)
{
    lv_init();
    lv_tick_set_cb(tick_ms);

    g_fb.fd = open(fb_path, O_RDWR | O_CLOEXEC);
    if (g_fb.fd < 0) {
        fprintf(stderr, "rpod: cannot open framebuffer %s: %s\n", fb_path, strerror(errno));
        return NULL;
    }

    struct fb_fix_screeninfo finfo;
    struct fb_var_screeninfo vinfo;
    if (ioctl(g_fb.fd, FBIOGET_FSCREENINFO, &finfo) != 0 ||
        ioctl(g_fb.fd, FBIOGET_VSCREENINFO, &vinfo) != 0) {
        perror("rpod: framebuffer geometry");
        goto fail;
    }
    g_fb.line_length = finfo.line_length;
    g_fb.rotate = vinfo.rotate;
    snprintf(g_fb.id, sizeof(g_fb.id), "%.*s", (int)sizeof(finfo.id), finfo.id);

    const bool landscape = vinfo.xres == RPOD_HOR_RES && vinfo.yres == RPOD_VER_RES;
    const bool portrait = vinfo.xres == RPOD_VER_RES && vinfo.yres == RPOD_HOR_RES;
    if (vinfo.bits_per_pixel != 16 || !(landscape || portrait) ||
        (portrait && vinfo.rotate != 0 && vinfo.rotate != 180)) {
        fprintf(stderr, "rpod: %s is %ux%u %ubpp rotate=%u; want 16bpp, 320x240 or 240x320 "
                "with rotate 0/180\n", fb_path, vinfo.xres, vinfo.yres, vinfo.bits_per_pixel,
                vinfo.rotate);
        goto fail;
    }
    if (portrait) {
        g_fb.portrait = calloc(vinfo.yres, g_fb.line_length);
        if (g_fb.portrait == NULL) {
            goto fail;
        }
    }
    g_fb.sync = true;

    lv_display_t *disp = lv_display_create(RPOD_HOR_RES, RPOD_VER_RES);
    if (disp == NULL) {
        goto fail;
    }
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    const uint32_t buf_size = lv_draw_buf_width_to_stride(RPOD_HOR_RES, LV_COLOR_FORMAT_RGB565) *
                              RPOD_VER_RES;
    /* Off LVGL's own LV_MEM_SIZE heap: it's for widgets, and this is for good. */
    void *buf = aligned_alloc(64, buf_size);
    if (buf == NULL) {
        lv_display_delete(disp);
        goto fail;
    }
    lv_display_set_buffers(disp, buf, NULL, buf_size, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, rpod_fb_flush_cb);
    return disp;

fail:
    free(g_fb.portrait);
    g_fb.portrait = NULL;
    close(g_fb.fd);
    g_fb.fd = -1;
    return NULL;
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
