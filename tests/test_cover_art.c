/*
 * Cover art decoding (src/ui/cover_art.c): JPEG -- baseline, progressive,
 * grayscale, and big enough to be scaled in the DCT -- and PNG, truecolor
 * and palette at several bit depths. Test images are made here, with
 * libjpeg and zlib, as two-colour halves whose colours must survive into
 * the tile.
 */

#include "ui/cover_art.h"

#include <stdio.h> /* before jpeglib.h */

#include <jpeglib.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static int failures;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);   \
            failures++;                                                                \
        }                                                                              \
    } while (0)

#define TILE 40

typedef struct {
    uint8_t r, g, b;
} rgb_t;

static const rgb_t RED = { 220, 30, 40 };
static const rgb_t BLUE = { 20, 60, 200 };
static const rgb_t GREY = { 128, 128, 128 };
static const rgb_t GREEN = { 40, 180, 60 };

/* The palette of the PNGs below, and a stripes pattern's colours. */
static const rgb_t PALETTE[4] = { RED, BLUE, GREY, GREEN };

/* Source pixel colour: `left` on the left half, `right` on the right. */
static rgb_t halves(int x, int w, rgb_t left, rgb_t right)
{
    return x < w / 2 ? left : right;
}

/* The tile's pixel is within `tol` of `want`, per channel. */
static int close_to(const rpod_cover_art_t *art, int x, int y, rgb_t want, int tol)
{
    uint16_t p = art->pixels[y * art->w + x];
    int r = ((p >> 11) & 0x1F) << 3, g = ((p >> 5) & 0x3F) << 2, b = (p & 0x1F) << 3;
    return abs(r - want.r) <= tol && abs(g - want.g) <= tol && abs(b - want.b) <= tol;
}

/* Decodes and checks a tile of left/right halves, away from the seam. */
static void expect_halves(const unsigned char *data, size_t size, rgb_t left, rgb_t right, int tol,
                          int line)
{
    rpod_cover_art_t art;
    if (!rpod_cover_art_decode(data, size, TILE, TILE, &art)) {
        fprintf(stderr, "%s:%d: decode failed\n", __FILE__, line);
        failures++;
        return;
    }
    int bad = 0;
    for (int y = 0; y < TILE; y++) {
        bad += !close_to(&art, 4, y, left, tol);
        bad += !close_to(&art, TILE - 5, y, right, tol);
    }
    if (bad != 0) {
        fprintf(stderr, "%s:%d: %d pixels off colour\n", __FILE__, line, bad);
        failures++;
    }
    rpod_cover_art_free(&art);
}

/* --- JPEG -------------------------------------------------------------- */

static unsigned char *make_jpeg(int w, int h, J_COLOR_SPACE space, bool progressive,
                                unsigned long *size)
{
    struct jpeg_compress_struct c;
    struct jpeg_error_mgr err;
    c.err = jpeg_std_error(&err);
    jpeg_create_compress(&c);
    unsigned char *out = NULL;
    *size = 0;
    jpeg_mem_dest(&c, &out, size);
    c.image_width = (JDIMENSION)w;
    c.image_height = (JDIMENSION)h;
    c.in_color_space = space;
    c.input_components = space == JCS_GRAYSCALE ? 1 : space == JCS_CMYK ? 4 : 3;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, 95, TRUE);
    if (progressive) {
        jpeg_simple_progression(&c);
    }
    jpeg_start_compress(&c, TRUE);
    unsigned char *row = malloc((size_t)w * (size_t)c.input_components);
    while (c.next_scanline < c.image_height) {
        for (int x = 0; x < w; x++) {
            rgb_t px = halves(x, w, RED, BLUE);
            unsigned char *p = row + (size_t)x * (size_t)c.input_components;
            if (space == JCS_GRAYSCALE) {
                p[0] = GREY.r;
            } else if (space == JCS_CMYK) {
                p[0] = p[1] = p[2] = p[3] = 0;
            } else {
                p[0] = px.r;
                p[1] = px.g;
                p[2] = px.b;
            }
        }
        JSAMPROW rows[1] = { row };
        jpeg_write_scanlines(&c, rows, 1);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    free(row);
    return out;
}

static void test_jpeg(void)
{
    unsigned long size;
    unsigned char *jpg;

    jpg = make_jpeg(400, 300, JCS_RGB, false, &size);
    expect_halves(jpg, size, RED, BLUE, 16, __LINE__);
    free(jpg);

    /* What TJpgDec couldn't read. */
    jpg = make_jpeg(400, 300, JCS_RGB, true, &size);
    CHECK(jpg[0] == 0xFF && jpg[1] == 0xD8);
    expect_halves(jpg, size, RED, BLUE, 16, __LINE__);

    /* Cut short: fails, rather than exit() as libjpeg's default would. */
    rpod_cover_art_t art;
    CHECK(!rpod_cover_art_decode(jpg, 200, TILE, TILE, &art));
    CHECK(art.pixels == NULL);
    free(jpg);

    /* 1/8 scale in the DCT, baseline and progressive. */
    jpg = make_jpeg(1600, 1400, JCS_RGB, false, &size);
    expect_halves(jpg, size, RED, BLUE, 16, __LINE__);
    free(jpg);
    jpg = make_jpeg(1600, 1400, JCS_RGB, true, &size);
    expect_halves(jpg, size, RED, BLUE, 16, __LINE__);
    free(jpg);

    jpg = make_jpeg(300, 300, JCS_GRAYSCALE, false, &size);
    expect_halves(jpg, size, GREY, GREY, 8, __LINE__);
    free(jpg);

    /* CMYK isn't supported: a clean failure. */
    jpg = make_jpeg(64, 64, JCS_CMYK, false, &size);
    CHECK(!rpod_cover_art_decode(jpg, size, TILE, TILE, &art));
    free(jpg);
}

/* --- PNG --------------------------------------------------------------- */

typedef struct {
    unsigned char *data;
    size_t size;
} buf_t;

static void put_u32(buf_t *b, uint32_t v)
{
    unsigned char be[4] = { (unsigned char)(v >> 24), (unsigned char)(v >> 16),
                            (unsigned char)(v >> 8), (unsigned char)v };
    b->data = realloc(b->data, b->size + 4);
    memcpy(b->data + b->size, be, 4);
    b->size += 4;
}

static void put_chunk(buf_t *b, const char *type, const unsigned char *payload, size_t len)
{
    put_u32(b, (uint32_t)len);
    b->data = realloc(b->data, b->size + 4 + len);
    memcpy(b->data + b->size, type, 4);
    if (len > 0) {
        memcpy(b->data + b->size + 4, payload, len);
    }
    uLong crc = crc32(0, b->data + b->size, (uInt)(4 + len));
    b->size += 4 + len;
    put_u32(b, (uint32_t)crc);
}

/* A w x h PNG of RED/BLUE left/right halves -- or with `stripes`, of
 * one-pixel columns cycling through as many of PALETTE as `depth` can
 * index, so neighbouring pixels within a byte differ. color_type 2 is 8-bit
 * RGB; 3 is a palette of PALETTE at `depth` bits. Rows alternate between
 * filter types None and Sub, so unfiltering is exercised too. */
static int stripe_colors(int depth)
{
    return depth == 1 ? 2 : 4;
}

static unsigned pixel_index(int x, int w, int depth, bool stripes)
{
    return stripes ? (unsigned)(x % stripe_colors(depth)) : x < w / 2 ? 0u : 1u;
}

static buf_t make_png(int w, int h, int color_type, int depth, bool stripes)
{
    buf_t b = { 0 };
    static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    b.data = malloc(8);
    memcpy(b.data, sig, 8);
    b.size = 8;

    unsigned char ihdr[13] = { 0 };
    for (int i = 0; i < 4; i++) {
        ihdr[i] = (unsigned char)(w >> (24 - 8 * i));
        ihdr[4 + i] = (unsigned char)(h >> (24 - 8 * i));
    }
    ihdr[8] = (unsigned char)depth;
    ihdr[9] = (unsigned char)color_type;
    put_chunk(&b, "IHDR", ihdr, sizeof(ihdr));

    int channels = color_type == 2 ? 3 : 1;
    if (color_type == 3) {
        unsigned char plte[12];
        memcpy(plte, PALETTE, sizeof(plte));
        put_chunk(&b, "PLTE", plte, sizeof(plte));
    }

    size_t row_bytes = ((size_t)w * (size_t)depth * (size_t)channels + 7) / 8;
    size_t raw_size = (row_bytes + 1) * (size_t)h;
    unsigned char *raw = calloc(1, raw_size);
    for (int y = 0; y < h; y++) {
        unsigned char *row = raw + (size_t)y * (row_bytes + 1);
        unsigned char *px = row + 1;
        for (int x = 0; x < w; x++) {
            unsigned idx = pixel_index(x, w, depth, stripes);
            if (color_type == 2) {
                px[x * 3] = PALETTE[idx].r;
                px[x * 3 + 1] = PALETTE[idx].g;
                px[x * 3 + 2] = PALETTE[idx].b;
            } else {
                size_t bit = (size_t)x * (size_t)depth;
                px[bit / 8] |= (unsigned char)(idx << (8 - depth - (int)(bit % 8)));
            }
        }
        if (y % 2 == 1) {
            size_t bpp = (size_t)channels; /* whole bytes, at least one */
            row[0] = 1;                    /* Sub */
            for (size_t i = row_bytes; i-- > bpp;) {
                px[i] = (unsigned char)(px[i] - px[i - bpp]);
            }
        }
    }
    uLongf zsize = compressBound((uLong)raw_size);
    unsigned char *z = malloc(zsize);
    compress(z, &zsize, raw, (uLong)raw_size);
    /* Split across two IDATs, as encoders may. */
    put_chunk(&b, "IDAT", z, zsize / 2);
    put_chunk(&b, "IDAT", z + zsize / 2, zsize - zsize / 2);
    put_chunk(&b, "IEND", NULL, 0);
    free(raw);
    free(z);
    return b;
}

/* Stripes at the tile's own size, so each pixel comes through unaveraged. */
static void expect_stripes(int color_type, int depth, int line)
{
    buf_t png = make_png(TILE, TILE, color_type, depth, true);
    rpod_cover_art_t art;
    if (!rpod_cover_art_decode(png.data, png.size, TILE, TILE, &art)) {
        fprintf(stderr, "%s:%d: decode failed\n", __FILE__, line);
        failures++;
        free(png.data);
        return;
    }
    int bad = 0;
    for (int y = 0; y < TILE; y++) {
        for (int x = 0; x < TILE; x++) {
            bad += !close_to(&art, x, y, PALETTE[pixel_index(x, TILE, depth, true)], 8);
        }
    }
    if (bad != 0) {
        fprintf(stderr, "%s:%d: depth %d: %d pixels off colour\n", __FILE__, line, depth, bad);
        failures++;
    }
    rpod_cover_art_free(&art);
    free(png.data);
}

static void test_png(void)
{
    buf_t png = make_png(300, 200, 2, 8, false);
    expect_halves(png.data, png.size, RED, BLUE, 8, __LINE__);
    free(png.data);
    expect_stripes(2, 8, __LINE__);

    static const int depths[] = { 1, 2, 4, 8 };
    for (size_t i = 0; i < sizeof(depths) / sizeof(depths[0]); i++) {
        /* An odd width, so rows end mid-byte. */
        png = make_png(301, 211, 3, depths[i], false);
        expect_halves(png.data, png.size, RED, BLUE, 8, __LINE__);
        free(png.data);
        expect_stripes(3, depths[i], __LINE__);
    }

    /* A palette PNG with no PLTE fails cleanly. */
    png = make_png(64, 64, 3, 8, false);
    size_t plte = 8 + 25; /* after the signature and IHDR */
    CHECK(memcmp(png.data + plte + 4, "PLTE", 4) == 0);
    memcpy(png.data + plte + 4, "zzzz", 4);
    rpod_cover_art_t art;
    CHECK(!rpod_cover_art_decode(png.data, png.size, TILE, TILE, &art));
    free(png.data);
}

int main(void)
{
    test_jpeg();
    test_png();

    rpod_cover_art_t art;
    static const unsigned char junk[] = "GIF89a not a cover";
    CHECK(!rpod_cover_art_decode(junk, sizeof(junk), TILE, TILE, &art));

    if (failures != 0) {
        fprintf(stderr, "test_cover_art: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_cover_art: ok\n");
    return 0;
}
