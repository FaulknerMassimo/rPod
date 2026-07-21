#include "cover_art.h"

/* JPEG is decoded via TJpgDec (vendored under third_party/lvgl/src/libs/tjpgd/),
 * called directly rather than through LVGL's own lv_tjpgd.c decoder-plugin
 * wrapper: that wrapper needs LV_USE_FS_MEMFS to decode from memory, only
 * recognizes an exact 10-byte JFIF APP0 signature (missing plain
 * baseline/Exif JPEGs), and decodes straight into an LV_MEM_SIZE-backed
 * buffer at full resolution. Driving TJpgDec ourselves keeps every
 * allocation a plain malloc outside LVGL's arena, and lets the output
 * callback point-sample straight down to the target thumbnail size as MCU
 * blocks stream in -- the full-resolution image is never materialized.
 *
 * PNG is *not* decoded via this project's vendored lodepng.c -- that copy
 * is an LVGL fork whose decode path unconditionally allocates its output
 * through lv_draw_buf_create_ex() (i.e. lv_malloc(), the LV_MEM_SIZE
 * arena) at ARGB8888, with no hook to redirect it elsewhere. Testing
 * against real ripped FLAC files turned up 1400x1400 embedded PNG covers
 * (more common than JPEG, in fact) -- decoding one through that path would
 * need ~28 MB of concurrent LV_MEM_SIZE headroom, unreasonable to reserve
 * on a 512 MB device. Instead, decode_png() below is a small decoder of
 * our own (chunk parsing + PNG unfiltering) against the system's zlib for
 * the actual DEFLATE inflate -- plain malloc throughout, freed right after
 * the downsample pass, same as the JPEG path. */
#include "src/libs/tjpgd/tjpgd.h"

#include <zlib.h>

#include <stdlib.h>
#include <string.h>

/* Maps source coordinate `s` to a destination coordinate in [0, dst_n), or
 * returns false if `s` falls outside the centered `crop_size`-wide crop
 * window starting at `crop0` -- shared by both decoders' downsample loops
 * to get iOS-style "aspect fill" (center-crop then scale) instead of a
 * squash-to-fit stretch. */
static bool crop_map(int s, int crop0, int crop_size, int dst_n, int *d)
{
    if (s < crop0 || s >= crop0 + crop_size) {
        return false;
    }
    *d = ((s - crop0) * dst_n) / crop_size;
    if (*d >= dst_n) {
        *d = dst_n - 1;
    }
    return true;
}

/* --- JPEG, via TJpgDec ----------------------------------------------- */

/* TJpgDec is baseline-only (SOF0) by design -- a deliberate tradeoff for
 * its tiny footprint. A *progressive* JPEG (SOF2, common output from photo
 * editors/converters) fails jd_prepare() with JDR_FMT1/JDR_FMT3 before
 * width/height are even known, confirmed against a real progressive cover
 * fetched from this project's own test library. That falls back to the
 * placeholder tile like any other undecodable art -- not a crash, just a
 * gap versus a full libjpeg. Revisit (e.g. libjpeg-turbo, also vendored
 * under third_party/lvgl/src/libs/) if progressive covers turn out to be
 * common in practice.
 *
 * Comfortably above TJpgDec's typical ~3-6 KB requirement (input buffer +
 * Huffman/quant tables + one MCU's worth of pixel/work buffers) -- see
 * jd_prepare()'s alloc_pool() calls in tjpgd.c. A stack buffer so a failed
 * decode can never leak it. */
#define JPEG_POOL_SIZE (16u * 1024u)

typedef struct {
    /* Input: read-only view over the caller's buffer. */
    const unsigned char *data;
    size_t size;
    size_t pos;

    /* Output: target thumbnail plus the centered square source region
     * being mapped into it. */
    uint16_t *dst;
    int dst_w, dst_h;
    int crop_x0, crop_y0, crop_size;
} jpeg_ctx_t;

/* TJpgDec stream input: buf == NULL means "skip ndata bytes without
 * reading them" (used when a segment TJpgDec doesn't care about is
 * skipped) -- both cases just advance ctx->pos. */
static size_t jpeg_input(JDEC *jd, uint8_t *buf, size_t ndata)
{
    jpeg_ctx_t *ctx = jd->device;
    size_t remain = ctx->size - ctx->pos;
    if (ndata > remain) {
        ndata = remain;
    }
    if (buf != NULL && ndata > 0) {
        memcpy(buf, ctx->data + ctx->pos, ndata);
    }
    ctx->pos += ndata;
    return ndata;
}

/* TJpgDec output: called once per decoded MCU block with a rectangle of
 * pixels (in full source-image coordinates; JD_USE_SCALE is off, so these
 * are never pre-scaled) as BGR888 triplets -- see the RGB-build loop in
 * tjpgd.c's jd_mcu_output(), which writes B, then G, then R despite the
 * "RGB888" naming. */
static int jpeg_output(JDEC *jd, void *bitmap, JRECT *rect)
{
    jpeg_ctx_t *ctx = jd->device;
    const uint8_t *pix = bitmap;
    int rw = rect->right - rect->left + 1;
    int rh = rect->bottom - rect->top + 1;

    for (int y = 0; y < rh; y++) {
        int dy;
        if (!crop_map(rect->top + y, ctx->crop_y0, ctx->crop_size, ctx->dst_h, &dy)) {
            continue;
        }
        const uint8_t *row = pix + (size_t)y * (size_t)rw * 3u;
        uint16_t *drow = ctx->dst + (size_t)dy * (size_t)ctx->dst_w;

        for (int x = 0; x < rw; x++) {
            int dx;
            if (!crop_map(rect->left + x, ctx->crop_x0, ctx->crop_size, ctx->dst_w, &dx)) {
                continue;
            }
            uint8_t b = row[(size_t)x * 3u + 0u];
            uint8_t g = row[(size_t)x * 3u + 1u];
            uint8_t r = row[(size_t)x * 3u + 2u];
            drow[dx] = (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
        }
    }
    return 1;
}

static bool decode_jpeg(const unsigned char *data, size_t size, int out_w, int out_h, uint16_t *dst)
{
    jpeg_ctx_t ctx = {
        .data = data,
        .size = size,
        .pos = 0,
        .dst = dst,
        .dst_w = out_w,
        .dst_h = out_h,
    };

    uint8_t pool[JPEG_POOL_SIZE];
    JDEC jd;
    if (jd_prepare(&jd, jpeg_input, pool, sizeof(pool), &ctx) != JDR_OK) {
        return false;
    }
    if (jd.width == 0 || jd.height == 0) {
        return false;
    }

    ctx.crop_size = (jd.width < jd.height) ? jd.width : jd.height;
    ctx.crop_x0 = (jd.width - ctx.crop_size) / 2;
    ctx.crop_y0 = (jd.height - ctx.crop_size) / 2;

    return jd_decomp(&jd, jpeg_output, 0) == JDR_OK;
}

/* --- PNG, own decoder + system zlib ----------------------------------- */

/* Only what real-world embedded cover art actually uses: 8-bit depth,
 * non-interlaced, and one of grayscale/truecolor/grayscale+alpha/
 * truecolor+alpha (not palette). Anything else fails cleanly and the
 * caller falls back to a placeholder -- no worse than a track that simply
 * has no art. */
typedef struct {
    uint32_t width, height;
    int channels;
} png_header_t;

static uint32_t read_u32be(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static bool parse_ihdr(const unsigned char *data, size_t size, png_header_t *hdr)
{
    if (size < 8u + 8u + 13u + 4u || read_u32be(data + 8) != 13 || memcmp(data + 12, "IHDR", 4) != 0) {
        return false;
    }
    const unsigned char *p = data + 16;
    hdr->width = read_u32be(p);
    hdr->height = read_u32be(p + 4);
    uint8_t bit_depth = p[8];
    uint8_t color_type = p[9];
    uint8_t compression = p[10];
    uint8_t filter_method = p[11];
    uint8_t interlace = p[12];

    if (bit_depth != 8 || compression != 0 || filter_method != 0 || interlace != 0) {
        return false;
    }
    if (hdr->width == 0 || hdr->height == 0 || hdr->width > 4096 || hdr->height > 4096) {
        return false;
    }
    switch (color_type) {
        case 0: hdr->channels = 1; break; /* grayscale */
        case 2: hdr->channels = 3; break; /* truecolor */
        case 4: hdr->channels = 2; break; /* grayscale + alpha */
        case 6: hdr->channels = 4; break; /* truecolor + alpha */
        default: return false;            /* palette (3) or unknown */
    }
    return true;
}

/* Concatenates every IDAT chunk's payload (PNG allows the compressed
 * stream to be split across several) into one malloc'd buffer. */
static bool collect_idat(const unsigned char *data, size_t size, unsigned char **out, size_t *out_size)
{
    unsigned char *idat = NULL;
    size_t idat_len = 0, idat_cap = 0;
    size_t pos = 8; /* past the PNG signature */

    while (pos + 12 <= size) {
        uint32_t chunk_len = read_u32be(data + pos);
        const unsigned char *type = data + pos + 4;
        const unsigned char *chunk_data = data + pos + 8;
        if (pos + 12 + (size_t)chunk_len > size) {
            break; /* truncated/corrupt -- stop with whatever IDAT we already have */
        }

        if (memcmp(type, "IDAT", 4) == 0) {
            if (idat_len + chunk_len > idat_cap) {
                size_t new_cap = (idat_cap == 0) ? (64u * 1024u) : (idat_cap * 2u);
                while (new_cap < idat_len + chunk_len) {
                    new_cap *= 2u;
                }
                unsigned char *grown = realloc(idat, new_cap);
                if (grown == NULL) {
                    free(idat);
                    return false;
                }
                idat = grown;
                idat_cap = new_cap;
            }
            memcpy(idat + idat_len, chunk_data, chunk_len);
            idat_len += chunk_len;
        } else if (memcmp(type, "IEND", 4) == 0) {
            break;
        }

        pos += 12 + (size_t)chunk_len; /* length + type + data + crc */
    }

    if (idat_len == 0) {
        free(idat);
        return false;
    }
    *out = idat;
    *out_size = idat_len;
    return true;
}

static uint8_t paeth_predictor(int a, int b, int c)
{
    int p = a + b - c;
    int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    if (pa <= pb && pa <= pc) {
        return (uint8_t)a;
    }
    return (pb <= pc) ? (uint8_t)b : (uint8_t)c;
}

/* Reverses PNG's per-scanline filtering (spec section 9) in place: each
 * row was compressed as [filter type byte][filtered bytes], predicting
 * each byte from already-decoded neighbors (left/above/above-left). */
static void unfilter(uint8_t *raw, const uint8_t *filtered, uint32_t width, uint32_t height, int bpp)
{
    size_t row_bytes = (size_t)width * (size_t)bpp;
    const uint8_t *prev = NULL;

    for (uint32_t y = 0; y < height; y++) {
        const unsigned char *frow = filtered + (size_t)y * (row_bytes + 1);
        uint8_t filter_type = frow[0];
        const unsigned char *src = frow + 1;
        uint8_t *out = raw + (size_t)y * row_bytes;

        for (size_t x = 0; x < row_bytes; x++) {
            int a = (x >= (size_t)bpp) ? out[x - (size_t)bpp] : 0;
            int b = (prev != NULL) ? prev[x] : 0;
            int c = (prev != NULL && x >= (size_t)bpp) ? prev[x - (size_t)bpp] : 0;
            int v = src[x];
            switch (filter_type) {
                case 1: v += a; break;
                case 2: v += b; break;
                case 3: v += (a + b) / 2; break;
                case 4: v += paeth_predictor(a, b, c); break;
                default: break; /* 0 = None */
            }
            out[x] = (uint8_t)v;
        }
        prev = out;
    }
}

static bool decode_png(const unsigned char *data, size_t size, int out_w, int out_h, uint16_t *dst)
{
    png_header_t hdr;
    if (!parse_ihdr(data, size, &hdr)) {
        return false;
    }

    unsigned char *idat = NULL;
    size_t idat_size = 0;
    if (!collect_idat(data, size, &idat, &idat_size)) {
        return false;
    }

    size_t row_bytes = (size_t)hdr.width * (size_t)hdr.channels;
    uLongf filtered_size = (uLongf)((row_bytes + 1) * hdr.height);
    uint8_t *filtered = malloc(filtered_size);
    bool ok = filtered != NULL &&
              uncompress(filtered, &filtered_size, idat, (uLong)idat_size) == Z_OK &&
              filtered_size == (uLongf)((row_bytes + 1) * hdr.height);
    free(idat);
    if (!ok) {
        free(filtered);
        return false;
    }

    uint8_t *raw = malloc(row_bytes * hdr.height);
    if (raw == NULL) {
        free(filtered);
        return false;
    }
    unfilter(raw, filtered, hdr.width, hdr.height, hdr.channels);
    free(filtered);

    int crop_size = (int)((hdr.width < hdr.height) ? hdr.width : hdr.height);
    int crop_x0 = ((int)hdr.width - crop_size) / 2;
    int crop_y0 = ((int)hdr.height - crop_size) / 2;
    int channels = hdr.channels;

    for (uint32_t sy = 0; sy < hdr.height; sy++) {
        int dy;
        if (!crop_map((int)sy, crop_y0, crop_size, out_h, &dy)) {
            continue;
        }
        const uint8_t *row = raw + (size_t)sy * row_bytes;
        uint16_t *drow = dst + (size_t)dy * (size_t)out_w;

        for (uint32_t sx = 0; sx < hdr.width; sx++) {
            int dx;
            if (!crop_map((int)sx, crop_x0, crop_size, out_w, &dx)) {
                continue;
            }
            const uint8_t *px = row + (size_t)sx * (size_t)channels;
            uint8_t r, g, b;
            if (channels == 1 || channels == 2) { /* grayscale (+ alpha, ignored) */
                r = g = b = px[0];
            } else { /* truecolor (+ alpha, ignored) */
                r = px[0];
                g = px[1];
                b = px[2];
            }
            drow[dx] = (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
        }
    }

    free(raw);
    return true;
}

/* --- Dispatch ------------------------------------------------------ */

static const unsigned char PNG_SIGNATURE[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };

bool rpod_cover_art_decode(const unsigned char *data, size_t size, int out_w, int out_h,
                           rpod_cover_art_t *out)
{
    out->pixels = NULL;
    out->w = 0;
    out->h = 0;

    uint16_t *dst = malloc((size_t)out_w * (size_t)out_h * sizeof(uint16_t));
    if (dst == NULL) {
        return false;
    }

    bool ok;
    if (size >= sizeof(PNG_SIGNATURE) && memcmp(data, PNG_SIGNATURE, sizeof(PNG_SIGNATURE)) == 0) {
        ok = decode_png(data, size, out_w, out_h, dst);
    } else if (size >= 2 && data[0] == 0xFF && data[1] == 0xD8) {
        ok = decode_jpeg(data, size, out_w, out_h, dst);
    } else {
        ok = false; /* unsupported format (e.g. GIF/BMP folder art) -- not fatal, just no art */
    }

    if (!ok) {
        free(dst);
        return false;
    }

    out->pixels = dst;
    out->w = out_w;
    out->h = out_h;
    return true;
}

void rpod_cover_art_free(rpod_cover_art_t *art)
{
    free(art->pixels);
    art->pixels = NULL;
    art->w = 0;
    art->h = 0;
}
