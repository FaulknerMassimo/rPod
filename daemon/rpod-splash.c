/*
 * rpod-splash — the rPod logo on the panel as soon as its driver is up.
 *
 * rpod-splash.service runs this at the very start of boot (docs/PLAN.md §3,
 * "Boot time"), long before rpod itself can: rpod waits for MPD, which waits
 * for half the system. The unit loads the panel's driver first; this waits
 * for fbtft's framebuffer to appear, draws the wordmark into it, pushes it
 * to the glass and exits. The panel then holds the picture until rpod's
 * first frame replaces it -- rpod.service is ordered after this unit, so a
 * late splash can never land on top of the UI.
 *
 * The wordmark is drawn from its geometry rather than an image file. Every
 * stroke in docs/logo/wordmark-on-dark.svg is an axis-aligned bar, a ring
 * or part of one, so a sample is a handful of point-in-shape tests, and 4x4
 * samples a pixel smooth the edges. Dark on Glass and Classic Dark, light
 * on Classic: whichever theme rpod saved last.
 *
 * Usage: rpod-splash [fb driver name] [timeout s]
 *        (defaults: fb_st7789v, 20; RPOD_THEME_STATE as in rpod)
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define HOR_RES 320 /* the UI's landscape screen (docs/PLAN.md §5) */
#define VER_RES 240

typedef struct {
    uint8_t r, g, b;
} rgb_t;

/* docs/logo/wordmark-on-{dark,light}.svg; the dark one's black is Glass's
 * background, the light one's white Classic's. */
static const rgb_t k_dark_bg = { 0x00, 0x00, 0x00 }, k_dark_ink = { 0xe8, 0xe8, 0xe8 };
static const rgb_t k_light_bg = { 0xff, 0xff, 0xff }, k_light_ink = { 0x1c, 0x1c, 0x1e };
static const rgb_t k_accent = { 0x0a, 0x84, 0xff };

/*
 * The wordmark in its SVG's units: viewBox 0 20 218 80, every stroke 12
 * wide with butt ends. Straight strokes are all horizontal or vertical, so
 * each is just the rectangle it covers.
 */
typedef struct {
    float x0, y0, x1, y1;
} bar_t;

static const bar_t k_bars[] = {
    { 0, 44, 12, 100 },    /* r's stem: M6 44V100 */
    { 28, 44, 32, 56 },    /* r's arm, after its shoulder: H32 */
    { 42, 20, 54, 100 },   /* P's stem: M48 20V100 */
    { 48, 20, 60, 32 },    /* P's bowl, top: M48 26H60 */
    { 48, 64, 60, 76 },    /* ...and bottom: H48 */
    { 206, 20, 218, 100 }, /* d's stem: M212 20V100 */
};

/* Rings of radius 22, or the part of one on the kept side of its centre:
 * keep_x -1 keeps x <= cx, +1 keeps x >= cx, 0 both (keep_y alike). Arcs
 * end on a line through the centre, so a butt end is exactly that cut. */
typedef struct {
    float cx, cy;
    int keep_x, keep_y;
} ring_t;

static const ring_t k_rings[] = {
    { 28, 72, -1, -1 }, /* r's shoulder, A22 22 0 0 1 28 50: the top-left quarter */
    { 60, 48, +1, 0 },  /* P's bowl, A22 22 0 0 1 60 70: the right half */
    { 125, 72, 0, 0 },  /* o, the click wheel */
    { 190, 72, 0, 0 },  /* d */
};

#define RING_R 22.0f
#define HALF_STROKE 6.0f
/* o's centre button, in the accent. */
#define DOT_X 125.0f
#define DOT_Y 72.0f
#define DOT_R 8.0f

#define LOGO_X 0.0f /* the viewBox */
#define LOGO_Y 20.0f
#define LOGO_W 218.0f
#define LOGO_H 80.0f
/* Pixels per SVG unit: 218 px wide, about two thirds of the screen. */
#define SCALE 1.0f
#define SUBSAMPLES 4

enum { INK_NONE, INK_STROKE, INK_ACCENT };

static int ink_at(float x, float y)
{
    float dx = x - DOT_X, dy = y - DOT_Y;
    if (dx * dx + dy * dy <= DOT_R * DOT_R) {
        return INK_ACCENT;
    }
    for (size_t i = 0; i < sizeof(k_bars) / sizeof(k_bars[0]); i++) {
        const bar_t *b = &k_bars[i];
        if (x >= b->x0 && x <= b->x1 && y >= b->y0 && y <= b->y1) {
            return INK_STROKE;
        }
    }
    const float r_in = RING_R - HALF_STROKE, r_out = RING_R + HALF_STROKE;
    for (size_t i = 0; i < sizeof(k_rings) / sizeof(k_rings[0]); i++) {
        const ring_t *r = &k_rings[i];
        dx = x - r->cx;
        dy = y - r->cy;
        if (r->keep_x * dx < 0 || r->keep_y * dy < 0) {
            continue;
        }
        const float d2 = dx * dx + dy * dy;
        if (d2 >= r_in * r_in && d2 <= r_out * r_out) {
            return INK_STROKE;
        }
    }
    return INK_NONE;
}

static uint16_t rgb565(unsigned r, unsigned g, unsigned b)
{
    return (uint16_t)(((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 |
                      ((b * 31 + 127) / 255));
}

/* The landscape screen, row by row: the wordmark centred on `bg`. */
static void draw(uint16_t *screen, rgb_t bg, rgb_t ink)
{
    const float ox = (HOR_RES - LOGO_W * SCALE) / 2, oy = (VER_RES - LOGO_H * SCALE) / 2;
    const int n = SUBSAMPLES * SUBSAMPLES;

    const uint16_t plain = rgb565(bg.r, bg.g, bg.b);
    for (int i = 0; i < HOR_RES * VER_RES; i++) {
        screen[i] = plain;
    }
    /* Only the pixels the logo's box touches need sampling: a quarter of
     * the screen, ~40 ms on the Zero 2 W rather than ~150. */
    const int x0 = ox > 0 ? (int)ox : 0, x1 = (int)(ox + LOGO_W * SCALE) + 1;
    const int y0 = oy > 0 ? (int)oy : 0, y1 = (int)(oy + LOGO_H * SCALE) + 1;
    for (int py = y0; py < y1 && py < VER_RES; py++) {
        for (int px = x0; px < x1 && px < HOR_RES; px++) {
            int count[3] = { 0 };
            for (int sy = 0; sy < SUBSAMPLES; sy++) {
                for (int sx = 0; sx < SUBSAMPLES; sx++) {
                    const float x = LOGO_X + (px + (sx + 0.5f) / SUBSAMPLES - ox) / SCALE;
                    const float y = LOGO_Y + (py + (sy + 0.5f) / SUBSAMPLES - oy) / SCALE;
                    count[ink_at(x, y)]++;
                }
            }
            const int nb = count[INK_NONE], ns = count[INK_STROKE], na = count[INK_ACCENT];
            screen[py * HOR_RES + px] =
                rgb565((bg.r * nb + ink.r * ns + k_accent.r * na + n / 2) / n,
                       (bg.g * nb + ink.g * ns + k_accent.g * na + n / 2) / n,
                       (bg.b * nb + ink.b * ns + k_accent.b * na + n / 2) / n);
        }
    }
}

/* Light only for Classic: rpod's theme file holds the theme's key
 * (src/ui/theme.c), and anything else, or no file, is dark. */
static bool theme_is_light(void)
{
    const char *path = getenv("RPOD_THEME_STATE");
    FILE *f = fopen(path != NULL && path[0] != '\0' ? path : "/var/lib/rpod/theme", "r");
    if (f == NULL) {
        return false;
    }
    char key[32] = "";
    const bool light = fgets(key, sizeof(key), f) != NULL && strcmp(key, "classic\n") == 0;
    fclose(f);
    return light;
}

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* The framebuffer whose driver is `name`, as /dev/fbN: N depends on whether
 * vc4 registered one first (system/udev/99-rpod-panel.rules), and udev may
 * not be running yet to make /dev/rpod-panel, but the kernel's devtmpfs
 * makes /dev/fbN on its own. */
static bool find_fb(const char *name, char *dev, size_t dev_size)
{
    DIR *d = opendir("/sys/class/graphics");
    if (d == NULL) {
        return false;
    }
    bool found = false;
    struct dirent *e;
    while (!found && (e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "fb", 2) != 0) {
            continue;
        }
        char path[300], buf[64] = "";
        snprintf(path, sizeof(path), "/sys/class/graphics/%s/name", e->d_name);
        FILE *f = fopen(path, "r");
        if (f == NULL) {
            continue;
        }
        if (fgets(buf, sizeof(buf), f) != NULL) {
            buf[strcspn(buf, "\n")] = '\0';
            if (strcmp(buf, name) == 0) {
                snprintf(dev, dev_size, "/dev/%s", e->d_name);
                found = true;
            }
        }
        fclose(f);
    }
    closedir(d);
    return found;
}

/* Waits up to `timeout` s for the framebuffer, then opens it. */
static int open_fb(const char *name, double timeout, char *dev, size_t dev_size)
{
    const double deadline = now_s() + timeout;
    for (;;) {
        if (find_fb(name, dev, dev_size)) {
            int fd = open(dev, O_RDWR | O_CLOEXEC);
            if (fd >= 0 || errno != ENOENT) {
                return fd;
            }
        }
        if (now_s() > deadline) {
            errno = ETIMEDOUT;
            return -1;
        }
        nanosleep(&(struct timespec){ .tv_nsec = 10 * 1000 * 1000 }, NULL);
    }
}

int main(int argc, char **argv)
{
    const char *name = argc > 1 ? argv[1] : "fb_st7789v";
    const double timeout = argc > 2 ? atof(argv[2]) : 20;
    const double start = now_s();

    /* Drawn while the driver probes: it's ready before the panel is. */
    static uint16_t screen[HOR_RES * VER_RES];
    const bool light = theme_is_light();
    draw(screen, light ? k_light_bg : k_dark_bg, light ? k_light_ink : k_dark_ink);

    char dev[300];
    int fd = open_fb(name, timeout, dev, sizeof(dev));
    if (fd < 0) {
        fprintf(stderr, "rpod-splash: no %s framebuffer: %s\n", name, strerror(errno));
        return 1;
    }

    struct fb_fix_screeninfo finfo;
    struct fb_var_screeninfo vinfo;
    if (ioctl(fd, FBIOGET_FSCREENINFO, &finfo) != 0 || ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) != 0) {
        perror("rpod-splash: framebuffer geometry");
        return 1;
    }
    const bool landscape = vinfo.xres == HOR_RES && vinfo.yres == VER_RES;
    const bool portrait = vinfo.xres == VER_RES && vinfo.yres == HOR_RES &&
                          (vinfo.rotate == 0 || vinfo.rotate == 180);
    if (vinfo.bits_per_pixel != 16 || !(landscape || portrait)) {
        fprintf(stderr, "rpod-splash: %s is %ux%u %ubpp rotate=%u, not the panel\n", dev,
                vinfo.xres, vinfo.yres, vinfo.bits_per_pixel, vinfo.rotate);
        return 1;
    }

    /* Into the framebuffer the way src/ui/lvgl_port.c's push_rotated() puts
     * rpod's frames: on a portrait one, landscape (x, y) is native
     * (y, 319 - x), or (239 - y, x) with rotate=180. */
    const uint32_t stride = finfo.line_length / sizeof(uint16_t);
    uint16_t *fb = calloc(vinfo.yres, finfo.line_length);
    if (fb == NULL) {
        return 1;
    }
    for (int y = 0; y < VER_RES; y++) {
        for (int x = 0; x < HOR_RES; x++) {
            size_t at;
            if (landscape) {
                at = (size_t)y * stride + x;
            } else if (vinfo.rotate == 180) {
                at = (size_t)x * stride + (VER_RES - 1 - y);
            } else {
                at = (size_t)(HOR_RES - 1 - x) * stride + y;
            }
            fb[at] = screen[y * HOR_RES + x];
        }
    }

    /* One pwrite() and an fsync() to push it now, as rpod does: mmap'd
     * writes don't reliably reach this panel (CLAUDE.md, lvgl_port.c). */
    const size_t len = (size_t)vinfo.yres * finfo.line_length;
    if (pwrite(fd, fb, len, 0) != (ssize_t)len) {
        perror("rpod-splash: pwrite");
        return 1;
    }
    if (fsync(fd) != 0 && errno != EINVAL) {
        perror("rpod-splash: fsync");
    }
    close(fd);
    fprintf(stderr, "rpod-splash: logo on %s (%s), %.0f ms after start\n", dev,
            light ? "light" : "dark", (now_s() - start) * 1000);
    return 0;
}
