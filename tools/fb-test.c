/*
 * fb-test.c — raw framebuffer colour-fill tool for display bring-up.
 *
 * Two modes, both LVGL-free so they isolate the panel wiring (orientation,
 * offsets, colour order) from any UI bug, per docs/PLAN.md §5.2 and the Phase 1
 * acceptance step in §9:
 *
 *   fb-test [/dev/fbN] [hold_ms]   Solid red, green, blue full-screen fills,
 *                                  looping until interrupted (colour-order check).
 *   fb-test [/dev/fbN] grid        One static frame held until Ctrl-C: black
 *                                  field, 1px white border on all four edges,
 *                                  and a distinct corner block — RED top-left,
 *                                  GREEN top-right, BLUE bottom-left, YELLOW
 *                                  bottom-right. Tells you, in a single look:
 *                                    - offset: any glass strip the driver's
 *                                      write-window doesn't cover shows garbage
 *                                      instead of the black field / white border
 *                                      (the ST7735S 128x128-in-132x162 problem).
 *                                    - orientation/mirroring: which physical
 *                                      corner each colour lands in pins down the
 *                                      exact rotate/flip.
 *                                    - colour order: are the corners actually
 *                                      red/green/blue/yellow.
 *
 * Usage: fb-test [/dev/fb1] [hold_ms | grid]
 */

#include <fcntl.h>
#include <linux/fb.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

struct rgb {
    const char *name;
    uint8_t r, g, b;
};

static const struct rgb COLORS[] = {
    { "red",   0xFF, 0x00, 0x00 },
    { "green", 0x00, 0xFF, 0x00 },
    { "blue",  0x00, 0x00, 0xFF },
};

/* Packs `c` into `px` for the panel's native pixel format. RGB565 is what
 * the ST7789V path uses; 24/32bpp are handled too in case of a different
 * fbdev backend. */
static void pack_pixel(uint8_t *px, int bpp, struct rgb c)
{
    switch (bpp) {
    case 16: {
        uint16_t v = (uint16_t)(((c.r >> 3) << 11) | ((c.g >> 2) << 5) | (c.b >> 3));
        memcpy(px, &v, 2);
        break;
    }
    case 24:
        px[0] = c.b; px[1] = c.g; px[2] = c.r;
        break;
    case 32:
        px[0] = c.b; px[1] = c.g; px[2] = c.r; px[3] = 0xFF;
        break;
    default:
        fprintf(stderr, "fb-test: unsupported bits_per_pixel %d\n", bpp);
        exit(1);
    }
}

/* Panel geometry + pixel format, filled once from the fbdev ioctls. */
struct fbinfo {
    uint8_t *fbp;
    long screensize;
    unsigned xres, yres;
    unsigned line_length;
    int bpp, bytes_per_px;
};

static void put_pixel(const struct fbinfo *fb, unsigned x, unsigned y, struct rgb c)
{
    if (x >= fb->xres || y >= fb->yres) {
        return;
    }
    uint8_t pixel[4];
    pack_pixel(pixel, fb->bpp, c);
    memcpy(fb->fbp + (long)y * fb->line_length + (long)x * fb->bytes_per_px,
           pixel, (size_t)fb->bytes_per_px);
}

static void fill_rect(const struct fbinfo *fb, unsigned x0, unsigned y0,
                      unsigned w, unsigned h, struct rgb c)
{
    for (unsigned y = y0; y < y0 + h; y++) {
        for (unsigned x = x0; x < x0 + w; x++) {
            put_pixel(fb, x, y, c);
        }
    }
}

/* Some fbdev backends (fbtft, DRM fbdev emulation) use deferred IO: mmap'd
 * writes are flushed to the panel by a worker, and msync forces that flush now
 * so the pattern is on-glass before we sleep. Harmless where it's a no-op. */
static void flush_fb(const struct fbinfo *fb)
{
    msync(fb->fbp, (size_t)fb->screensize, MS_SYNC);
}

/* Static orientation/offset probe — see the mode table at the top. */
static void draw_grid(const struct fbinfo *fb)
{
    const struct rgb black  = { "black",  0x00, 0x00, 0x00 };
    const struct rgb white  = { "white",  0xFF, 0xFF, 0xFF };
    const struct rgb red    = { "red",    0xFF, 0x00, 0x00 };
    const struct rgb green  = { "green",  0x00, 0xFF, 0x00 };
    const struct rgb blue   = { "blue",   0x00, 0x00, 0xFF };
    const struct rgb yellow = { "yellow", 0xFF, 0xFF, 0x00 };

    fill_rect(fb, 0, 0, fb->xres, fb->yres, black);

    /* 1px white frame on all four outermost edges. */
    fill_rect(fb, 0, 0, fb->xres, 1, white);
    fill_rect(fb, 0, fb->yres - 1, fb->xres, 1, white);
    fill_rect(fb, 0, 0, 1, fb->yres, white);
    fill_rect(fb, fb->xres - 1, 0, 1, fb->yres, white);

    /* Corner blocks inset 1px so the frame stays visible around them. */
    unsigned s = fb->xres < 32 ? 4 : 16; /* block size, scaled for tiny panels */
    fill_rect(fb, 1, 1, s, s, red);                               /* top-left */
    fill_rect(fb, fb->xres - 1 - s, 1, s, s, green);              /* top-right */
    fill_rect(fb, 1, fb->yres - 1 - s, s, s, blue);              /* bottom-left */
    fill_rect(fb, fb->xres - 1 - s, fb->yres - 1 - s, s, s, yellow); /* bottom-right */

    flush_fb(fb);
    printf("fb-test: grid pattern drawn — frame + RED(TL) GREEN(TR) BLUE(BL) YELLOW(BR). "
           "Ctrl-C to stop.\n");
    fflush(stdout);
}

/* Graduated ruler border for measuring panel column/row offsets: concentric
 * 1px rings from the outermost edge inward, each a distinct colour, so a viewer
 * can count how many pixels are cut on each edge (ring 0 red flush to the edge
 * = 0px cut; if green is the outermost visible ring on an edge, 2px are cut
 * there, etc.). A magenta centre block confirms the interior is being drawn. */
static void draw_ruler(const struct fbinfo *fb)
{
    const struct rgb black   = { "black",   0x00, 0x00, 0x00 };
    const struct rgb magenta = { "magenta", 0xFF, 0x00, 0xFF };
    /* outermost -> inward */
    const struct rgb rings[] = {
        { "red",   0xFF, 0x00, 0x00 },
        { "yellow",0xFF, 0xFF, 0x00 },
        { "green", 0x00, 0xFF, 0x00 },
        { "cyan",  0x00, 0xFF, 0xFF },
        { "white", 0xFF, 0xFF, 0xFF },
    };
    unsigned n = sizeof(rings) / sizeof(rings[0]);

    fill_rect(fb, 0, 0, fb->xres, fb->yres, black);
    for (unsigned r = 0; r < n; r++) {
        if (2 * r + 1 >= fb->xres || 2 * r + 1 >= fb->yres) {
            break;
        }
        fill_rect(fb, r, r, fb->xres - 2 * r, 1, rings[r]);                 /* top */
        fill_rect(fb, r, fb->yres - 1 - r, fb->xres - 2 * r, 1, rings[r]);  /* bottom */
        fill_rect(fb, r, r, 1, fb->yres - 2 * r, rings[r]);                 /* left */
        fill_rect(fb, fb->xres - 1 - r, r, 1, fb->yres - 2 * r, rings[r]);  /* right */
    }
    fill_rect(fb, fb->xres / 2 - 4, fb->yres / 2 - 4, 8, 8, magenta);       /* centre */

    flush_fb(fb);
    printf("fb-test: ruler drawn — rings outward-to-inward: red,yellow,green,cyan,white; "
           "magenta centre. Ctrl-C to stop.\n");
    fflush(stdout);
}

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "/dev/fb1";
    bool grid_mode = argc > 2 && strcmp(argv[2], "grid") == 0;
    bool ruler_mode = argc > 2 && strcmp(argv[2], "ruler") == 0;
    long hold_ms = (argc > 2 && !grid_mode && !ruler_mode) ? strtol(argv[2], NULL, 10) : 2000;

    int fd = open(dev, O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        perror("ioctl(FBIOGET_VSCREENINFO)");
        return 1;
    }
    if (ioctl(fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        perror("ioctl(FBIOGET_FSCREENINFO)");
        return 1;
    }

    struct fbinfo fb = {
        .xres = vinfo.xres,
        .yres = vinfo.yres,
        .line_length = finfo.line_length,
        .bpp = vinfo.bits_per_pixel,
        .bytes_per_px = vinfo.bits_per_pixel / 8,
        .screensize = (long)finfo.line_length * vinfo.yres,
    };

    printf("fb-test: %s %ux%u @ %dbpp, line_length=%u, mode=%s, Ctrl-C to stop\n",
           dev, fb.xres, fb.yres, fb.bpp, fb.line_length,
           ruler_mode ? "ruler" : grid_mode ? "grid" : "rgb-fills");

    fb.fbp = mmap(NULL, (size_t)fb.screensize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb.fbp == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (grid_mode || ruler_mode) {
        if (ruler_mode) {
            draw_ruler(&fb);
        } else {
            draw_grid(&fb);
        }
        while (!g_stop) {
            struct timespec t = { .tv_sec = 0, .tv_nsec = 100000000L };
            nanosleep(&t, NULL);
        }
    } else {
        struct timespec hold = {
            .tv_sec = hold_ms / 1000,
            .tv_nsec = (hold_ms % 1000) * 1000000L,
        };
        while (!g_stop) {
            for (size_t i = 0; i < sizeof(COLORS) / sizeof(COLORS[0]) && !g_stop; i++) {
                printf("fb-test: %s\n", COLORS[i].name);
                fflush(stdout);
                fill_rect(&fb, 0, 0, fb.xres, fb.yres, COLORS[i]);
                flush_fb(&fb);
                nanosleep(&hold, NULL);
            }
        }
    }

    munmap(fb.fbp, (size_t)fb.screensize);
    close(fd);
    printf("fb-test: stopped\n");
    return 0;
}
