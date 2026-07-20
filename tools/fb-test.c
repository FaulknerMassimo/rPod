/*
 * fb-test.c — raw framebuffer colour-fill tool for display bring-up.
 *
 * Fills the framebuffer with solid red, green, then blue, holding each for
 * a couple of seconds and looping until interrupted. No LVGL involved —
 * this isolates the fbtft/panel wiring (orientation, offsets, colour order)
 * from any UI bugs, per docs/PLAN.md §5.2 and the Phase 1 acceptance step
 * in §9.
 *
 * Usage: fb-test [/dev/fb1] [hold_ms]
 */

#include <fcntl.h>
#include <linux/fb.h>
#include <signal.h>
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

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "/dev/fb1";
    long hold_ms = argc > 2 ? strtol(argv[2], NULL, 10) : 2000;

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

    int bpp = vinfo.bits_per_pixel;
    int bytes_per_px = bpp / 8;
    long screensize = (long)finfo.line_length * vinfo.yres;

    printf("fb-test: %s %ux%u @ %dbpp, line_length=%u, holding %ldms/colour, Ctrl-C to stop\n",
           dev, vinfo.xres, vinfo.yres, bpp, finfo.line_length, hold_ms);

    uint8_t *fbp = mmap(NULL, (size_t)screensize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fbp == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    struct timespec hold = {
        .tv_sec = hold_ms / 1000,
        .tv_nsec = (hold_ms % 1000) * 1000000L,
    };

    while (!g_stop) {
        for (size_t i = 0; i < sizeof(COLORS) / sizeof(COLORS[0]) && !g_stop; i++) {
            uint8_t pixel[4];
            pack_pixel(pixel, bpp, COLORS[i]);

            printf("fb-test: %s\n", COLORS[i].name);
            fflush(stdout);

            for (unsigned y = 0; y < vinfo.yres; y++) {
                uint8_t *row = fbp + (long)y * finfo.line_length;
                for (unsigned x = 0; x < vinfo.xres; x++) {
                    memcpy(row + (long)x * bytes_per_px, pixel, (size_t)bytes_per_px);
                }
            }

            nanosleep(&hold, NULL);
        }
    }

    munmap(fbp, (size_t)screensize);
    close(fd);
    printf("fb-test: stopped\n");
    return 0;
}
