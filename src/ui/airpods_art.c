#include "airpods_art.h"

#include "classic.h"
#include "theme.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* tools/airpods-art.py's output, airpods_art_data.c. */
extern const lv_image_dsc_t rpod_airpods_photo_buds;
extern const lv_image_dsc_t rpod_airpods_photo_case;
extern const lv_image_dsc_t rpod_airpods_glyph_pods;

/* --- Classic --------------------------------------------------------------------
 *
 * Drawn the way the iPod's bitmaps are (re/ipod4/out/lvgl/ipod_images.c):
 * 1-px outlines with the box behind showing through inside them, and its
 * 2-bit greys for detail. '#' is the theme's text colour, '+' and '-' two
 * thirds and a third of it (as A8 coverage, so the same art works in
 * Classic Dark), '.' nothing. */

/* The pair, 13x9 -- the size of the iPod's own title bar icons (repeat,
 * shuffle) -- in the shape of the Glass glyph. */
static const char *const k_glyph[] = {
    ".####...####.",
    "######.######",
    "######.######",
    "######.######",
    ".#####.#####.",
    "....##.##....",
    "....##.##....",
    "....##.##....",
    "....##.##....",
};

/* The same at 11x7, the height of the HUD's Pixo text. */
static const char *const k_glyph_small[] = {
    ".###...###.",
    "#####.#####",
    "#####.#####",
    ".####.####.",
    "...##.##...",
    "...##.##...",
    "...##.##...",
};

/* The left bud of a pair of AirPods Pro in profile, traced from the Glass
 * photo: ear tip (grey) to the right, stem down the left. The right bud is
 * this mirrored, BUD_GAP pixels across. */
static const char *const k_bud[] = {
    "......#####.........",
    "....##.....##.......",
    "...#.........#......",
    "..#...........#.....",
    "..#........##..#....",
    ".#..........##..#...",
    ".#...............#..",
    ".#..............###.",
    "#..............#---#",
    "#..............#---#",
    "#.............#----#",
    "#.............#----#",
    "#.............#---#.",
    "#...##.......#----#.",
    "#...#.##.....#---#..",
    "#...#...#####.###...",
    "#...#...............",
    "#...#...............",
    "#...#...............",
    "#...#...............",
    "#...#...............",
    "#...#...............",
    "#...#...............",
    "#...#...............",
    ".#++#...............",
    "..##................",
};
#define BUD_GAP 3

/* The case from the front, lid shut: the lid's seam and the notch under
 * it, the status light, and a little shade where it curves under. */
static const char *const k_case[] = {
    "....###################....",
    "...#...................#...",
    "..#.....................#..",
    ".#.......................#.",
    ".#.......................#.",
    "#+++++++++++++++++++++++++#",
    "#.........-------.........#",
    "#.........................#",
    "#.........................#",
    "#............#............#",
    "#.........................#",
    "#.........................#",
    "#.........................#",
    "#.........................#",
    "#.........................#",
    "#.........................#",
    ".#.......................#.",
    ".#-.....................-#.",
    "..##-------------------##..",
    "....###################....",
};

#define ROWS(a) ((int)(sizeof(a) / sizeof((a)[0])))

static uint8_t coverage(char c)
{
    switch (c) {
    case '#': return 0xff;
    case '+': return 0xaa;
    case '-': return 0x55;
    default:  return 0x00;
    }
}

/* `rows` as an A8 mask in `dsc`, pixel-doubled, built on first use. With
 * `mirror_gap` >= 0 they're followed by that many blank columns and
 * themselves mirrored. */
static const lv_image_dsc_t *pixel_art(lv_image_dsc_t *dsc, const char *const *rows, int h,
                                       int mirror_gap)
{
    if (dsc->data != NULL) {
        return dsc;
    }
    int w = (int)strlen(rows[0]);
    int art_w = mirror_gap >= 0 ? 2 * w + mirror_gap : w;
    int stride = art_w * RPOD_CLASSIC_PX;
    uint8_t *px = calloc((size_t)stride * h * RPOD_CLASSIC_PX, 1);
    if (px == NULL) {
        return NULL;
    }
    for (int y = 0; y < h * RPOD_CLASSIC_PX; y++) {
        uint8_t *line = px + (size_t)y * stride;
        for (int x = 0; x < w * RPOD_CLASSIC_PX; x++) {
            uint8_t v = coverage(rows[y / RPOD_CLASSIC_PX][x / RPOD_CLASSIC_PX]);
            line[x] = v;
            if (mirror_gap >= 0) {
                line[stride - 1 - x] = v;
            }
        }
    }
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_A8;
    dsc->header.w = (uint16_t)(art_w * RPOD_CLASSIC_PX);
    dsc->header.h = (uint16_t)(h * RPOD_CLASSIC_PX);
    dsc->header.stride = (uint16_t)stride;
    dsc->data_size = (uint32_t)stride * h * RPOD_CLASSIC_PX;
    dsc->data = px;
    return dsc;
}

/* --- Pictures -------------------------------------------------------------------- */

const lv_image_dsc_t *rpod_airpods_glyph(bool small)
{
    static lv_image_dsc_t classic, classic_small;
    if (!rpod_theme()->classic) {
        return &rpod_airpods_glyph_pods;
    }
    return small ? pixel_art(&classic_small, k_glyph_small, ROWS(k_glyph_small), -1)
                 : pixel_art(&classic, k_glyph, ROWS(k_glyph), -1);
}

const lv_image_dsc_t *rpod_airpods_art_buds(void)
{
    static lv_image_dsc_t classic;
    return rpod_theme()->classic ? pixel_art(&classic, k_bud, ROWS(k_bud), BUD_GAP)
                                 : &rpod_airpods_photo_buds;
}

const lv_image_dsc_t *rpod_airpods_art_case(void)
{
    static lv_image_dsc_t classic;
    return rpod_theme()->classic ? pixel_art(&classic, k_case, ROWS(k_case), -1)
                                 : &rpod_airpods_photo_case;
}

lv_obj_t *rpod_airpods_image_create(lv_obj_t *parent, const lv_image_dsc_t *dsc, lv_color_t color)
{
    lv_obj_t *img = lv_image_create(parent);
    if (dsc == NULL) {
        return img;
    }
    lv_image_set_src(img, dsc);
    if (dsc->header.cf == LV_COLOR_FORMAT_A8) {
        /* A8 masks are drawn in the recolor colour; recolor_opa must be
         * non-zero for it to be read at all (as in heart_icon.c). */
        lv_obj_set_style_image_recolor(img, color, 0);
        lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    }
    return img;
}
