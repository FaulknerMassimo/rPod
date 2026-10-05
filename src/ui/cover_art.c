#include "cover_art.h"

/* JPEG is decoded via TJpgDec (vendored under third_party/lvgl/src/libs/tjpgd/),
 * called directly rather than through LVGL's own lv_tjpgd.c decoder-plugin
 * wrapper: that wrapper needs LV_USE_FS_MEMFS to decode from memory, only
 * recognizes an exact 10-byte JFIF APP0 signature (missing plain
 * baseline/Exif JPEGs), and decodes straight into an LV_MEM_SIZE-backed
 * buffer at full resolution. Driving TJpgDec ourselves keeps every
 * allocation a plain malloc outside LVGL's arena, and lets the output
 * callback average straight down to the target thumbnail size as MCU
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
 * the actual DEFLATE inflate, streamed a scanline at a time -- so it too
 * never holds more than two rows of the full-size image. */
#include "src/libs/tjpgd/tjpgd.h"

#include <zlib.h>

#include <stdlib.h>
#include <string.h>

static uint16_t rgb565(unsigned r, unsigned g, unsigned b)
{
    return (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

/* --- Area-averaging downsampler ------------------------------------ */

/* Shrinks a source image to an out_w x out_h thumbnail one span of pixels
 * at a time, as a decoder produces them (whole rows for PNG, MCU blocks for
 * JPEG), so the full-resolution image never has to exist in memory. Only
 * the largest centred out_w:out_h window of the source is used (aspect
 * fill), and each output pixel is the plain average of the source pixels
 * it covers -- point-sampling one source pixel per output pixel (what this
 * used to do) aliases badly at the 10-35x reductions real covers need, and
 * fine text and patterns came out as noise.
 *
 * A crop smaller than the output in either axis (an old rip's 100px art at
 * Now Playing's tile size) is averaged to its own size in that axis, then
 * pixel-repeated up to the output size in resampler_finish(). */
typedef struct {
    int out_w, out_h;
    int mid_w, mid_h;  /* accumulated size: out_*, or the crop's size if smaller */
    int *xmap;         /* source x -> accumulator column, or -1 outside the crop */
    int *ymap;         /* source y -> accumulator row, or -1 outside the crop */
    uint32_t *xcount;  /* source columns summed into each accumulator column */
    uint32_t *ycount;  /* source rows summed into each accumulator row */
    uint32_t *acc;     /* mid_w * mid_h * 3 per-channel sums */
} resampler_t;

static void resampler_free(resampler_t *rs)
{
    free(rs->xmap);
    free(rs->ymap);
    free(rs->xcount);
    free(rs->ycount);
    free(rs->acc);
}

/* Maps each source coordinate in [0, src_n) to one of dst_n bins spread
 * evenly over the crop window [crop0, crop0 + crop_n), or -1 outside it.
 * Returns the most sources any one bin got. */
static uint32_t map_axis(int *map, uint32_t *count, int src_n, int crop0, int crop_n, int dst_n)
{
    uint32_t most = 0;
    for (int s = 0; s < src_n; s++) {
        int d = -1;
        if (s >= crop0 && s < crop0 + crop_n) {
            d = (int)(((int64_t)(s - crop0) * dst_n) / crop_n);
            if (++count[d] > most) {
                most = count[d];
            }
        }
        map[s] = d;
    }
    return most;
}

static bool resampler_init(resampler_t *rs, int src_w, int src_h, int out_w, int out_h)
{
    memset(rs, 0, sizeof(*rs));
    if (src_w <= 0 || src_h <= 0 || out_w <= 0 || out_h <= 0) {
        return false;
    }

    int crop_w = src_w;
    int crop_h = (int)(((int64_t)src_w * out_h) / out_w);
    if (crop_h > src_h) {
        crop_h = src_h;
        crop_w = (int)(((int64_t)src_h * out_w) / out_h);
    }
    if (crop_w < 1) {
        crop_w = 1;
    }
    if (crop_h < 1) {
        crop_h = 1;
    }

    rs->out_w = out_w;
    rs->out_h = out_h;
    rs->mid_w = crop_w < out_w ? crop_w : out_w;
    rs->mid_h = crop_h < out_h ? crop_h : out_h;
    rs->xmap = malloc((size_t)src_w * sizeof(*rs->xmap));
    rs->ymap = malloc((size_t)src_h * sizeof(*rs->ymap));
    rs->xcount = calloc((size_t)rs->mid_w, sizeof(*rs->xcount));
    rs->ycount = calloc((size_t)rs->mid_h, sizeof(*rs->ycount));
    rs->acc = calloc((size_t)rs->mid_w * (size_t)rs->mid_h * 3u, sizeof(*rs->acc));
    if (rs->xmap == NULL || rs->ymap == NULL || rs->xcount == NULL || rs->ycount == NULL ||
        rs->acc == NULL) {
        resampler_free(rs);
        return false;
    }

    uint32_t most_x = map_axis(rs->xmap, rs->xcount, src_w, (src_w - crop_w) / 2, crop_w, rs->mid_w);
    uint32_t most_y = map_axis(rs->ymap, rs->ycount, src_h, (src_h - crop_h) / 2, crop_h, rs->mid_h);
    /* A channel sum must fit its uint32_t -- only a pathological source
     * (tens of thousands of pixels into a handful) could overflow one. */
    if ((uint64_t)most_x * most_y * 255u * 2u > UINT32_MAX) {
        resampler_free(rs);
        return false;
    }
    return true;
}

/* Adds `n` source pixels of row `sy`, starting at column `sx`, laid out
 * `bpp` bytes apart with their red/green/blue bytes at offsets r/g/b (all
 * three the same for grayscale; any alpha byte is ignored). Each run of
 * source pixels landing in the same output column is summed in registers
 * and added once -- adding pixel by pixel straight into the accumulator
 * stalled the Pi's in-order cores on every store, a third of the decode. */
static void resampler_add(resampler_t *rs, int sy, int sx, int n, const uint8_t *px, int bpp,
                          int r, int g, int b)
{
    int dy = rs->ymap[sy];
    if (dy < 0) {
        return;
    }
    uint32_t *row = rs->acc + (size_t)dy * (size_t)rs->mid_w * 3u;
    const int *xmap = rs->xmap + sx;
    int i = 0;
    while (i < n) {
        int dx = xmap[i];
        if (dx < 0) {
            i++;
            px += bpp;
            continue;
        }
        uint32_t sr = 0, sg = 0, sb = 0;
        do {
            sr += px[r];
            sg += px[g];
            sb += px[b];
            px += bpp;
            i++;
        } while (i < n && xmap[i] == dx);
        uint32_t *a = row + (size_t)dx * 3u;
        a[0] += sr;
        a[1] += sg;
        a[2] += sb;
    }
}

static void resampler_finish(const resampler_t *rs, uint16_t *dst)
{
    for (int y = 0; y < rs->out_h; y++) {
        int my = (int)(((int64_t)y * rs->mid_h) / rs->out_h);
        for (int x = 0; x < rs->out_w; x++) {
            int mx = (int)(((int64_t)x * rs->mid_w) / rs->out_w);
            const uint32_t *a = rs->acc + ((size_t)my * (size_t)rs->mid_w + (size_t)mx) * 3u;
            uint32_t n = rs->xcount[mx] * rs->ycount[my];
            uint32_t half = n / 2u;
            dst[(size_t)y * (size_t)rs->out_w + (size_t)x] =
                rgb565((a[0] + half) / n, (a[1] + half) / n, (a[2] + half) / n);
        }
    }
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

    resampler_t rs; /* set up once the image size is known */
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
        resampler_add(&ctx->rs, rect->top + y, rect->left, rw, pix + (size_t)y * (size_t)rw * 3u, 3,
                      2, 1, 0);
    }
    return 1;
}

static bool decode_jpeg(const unsigned char *data, size_t size, int out_w, int out_h, uint16_t *dst)
{
    jpeg_ctx_t ctx = {
        .data = data,
        .size = size,
        .pos = 0,
    };

    uint8_t pool[JPEG_POOL_SIZE];
    JDEC jd;
    if (jd_prepare(&jd, jpeg_input, pool, sizeof(pool), &ctx) != JDR_OK) {
        return false;
    }
    if (!resampler_init(&ctx.rs, jd.width, jd.height, out_w, out_h)) {
        return false;
    }

    bool ok = jd_decomp(&jd, jpeg_output, 0) == JDR_OK;
    if (ok) {
        resampler_finish(&ctx.rs, dst);
    }
    resampler_free(&ctx.rs);
    return ok;
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

/* The compressed image data, inflated a scanline at a time straight out of
 * the file's IDAT chunks (PNG allows the zlib stream to be split across
 * several) -- no concatenated copy of the compressed data, and no buffer
 * for the whole inflated image, which for a 1400x1400 cover was ~6 MB
 * twice over (filtered + unfiltered). */
typedef struct {
    const unsigned char *data;
    size_t size;
    size_t pos; /* next chunk to look at */
    z_stream zs;
} png_stream_t;

/* Points the inflater at the next IDAT chunk's payload. False once the
 * chunks run out (IEND, or truncated/corrupt data). */
static bool png_next_idat(png_stream_t *s)
{
    while (s->pos + 12 <= s->size) {
        uint32_t len = read_u32be(s->data + s->pos);
        const unsigned char *type = s->data + s->pos + 4;
        if ((size_t)len > s->size - s->pos - 12) {
            return false;
        }
        const unsigned char *payload = s->data + s->pos + 8;
        s->pos += 12 + (size_t)len; /* length + type + data + crc */

        if (memcmp(type, "IDAT", 4) == 0 && len > 0) {
            s->zs.next_in = (Bytef *)payload;
            s->zs.avail_in = len;
            return true;
        }
        if (memcmp(type, "IEND", 4) == 0) {
            return false;
        }
    }
    return false;
}

/* Inflates exactly `n` bytes into `out`. */
static bool png_read(png_stream_t *s, uint8_t *out, size_t n)
{
    s->zs.next_out = out;
    s->zs.avail_out = (uInt)n;
    while (s->zs.avail_out > 0) {
        if (s->zs.avail_in == 0 && !png_next_idat(s)) {
            return false;
        }
        int r = inflate(&s->zs, Z_NO_FLUSH);
        if (r == Z_STREAM_END) {
            return s->zs.avail_out == 0;
        }
        if (r != Z_OK && r != Z_BUF_ERROR) {
            return false;
        }
    }
    return true;
}

/* Paeth predictor (PNG spec section 9.4) in the form libpng uses: the
 * distances from p = a + b - c, computed without p itself -- and with
 * selects rather than branches, which the Pi's in-order cores mispredict
 * on nearly every byte of a photo. Picks a if pa <= pb && pa <= pc, else b
 * if pb <= pc, else c, same as the spec's ordering of ties. */
static inline int paeth(int a, int b, int c)
{
    int pa = abs(b - c);
    int pb = abs(a - c);
    int pc = abs(a + b - 2 * c);
    bool use_b = pb < pa;
    int best = use_b ? b : a;
    int pbest = use_b ? pb : pa;
    return pc < pbest ? c : best;
}

/* Paeth for `CH`-byte pixels, keeping each channel's left neighbour in a
 * register instead of reading back the byte just written. */
#define PAETH_ROW(CH)                                                       \
    do {                                                                    \
        int left[CH], upleft[CH];                                           \
        for (int k = 0; k < (CH); k++) {                                    \
            left[k] = (uint8_t)(cur[k] + prev[k]); /* paeth(0, b, 0) == b */ \
            upleft[k] = prev[k];                                            \
            cur[k] = (uint8_t)left[k];                                      \
        }                                                                   \
        for (size_t x = (CH); x < n; x += (CH)) {                           \
            for (int k = 0; k < (CH); k++) {                                \
                int up = prev[x + k];                                       \
                left[k] = (uint8_t)(cur[x + k] + paeth(left[k], up, upleft[k])); \
                upleft[k] = up;                                             \
                cur[x + k] = (uint8_t)left[k];                              \
            }                                                               \
        }                                                                   \
    } while (0)

/* Reverses one scanline's filtering (spec section 9) in place: each byte
 * was predicted from already-decoded neighbours -- left (`bpp` bytes back
 * in `cur`), above (`prev`), above-left. One tight loop per filter type
 * rather than a switch per byte; Paeth, by far the most common type in
 * real covers, dominated the old decode time. */
static bool unfilter_row(uint8_t filter, uint8_t *cur, const uint8_t *prev, size_t n, size_t bpp)
{
    size_t i;
    switch (filter) {
        case 0:
            break;
        case 1:
            for (i = bpp; i < n; i++) {
                cur[i] = (uint8_t)(cur[i] + cur[i - bpp]);
            }
            break;
        case 2:
            for (i = 0; i < n; i++) {
                cur[i] = (uint8_t)(cur[i] + prev[i]);
            }
            break;
        case 3:
            for (i = 0; i < bpp; i++) {
                cur[i] = (uint8_t)(cur[i] + (prev[i] >> 1));
            }
            for (; i < n; i++) {
                cur[i] = (uint8_t)(cur[i] + ((cur[i - bpp] + prev[i]) >> 1));
            }
            break;
        case 4:
            switch (bpp) {
                case 1: PAETH_ROW(1); break;
                case 2: PAETH_ROW(2); break;
                case 3: PAETH_ROW(3); break;
                default: PAETH_ROW(4); break;
            }
            break;
        default:
            return false;
    }
    return true;
}

static bool decode_png(const unsigned char *data, size_t size, int out_w, int out_h, uint16_t *dst)
{
    png_header_t hdr;
    if (!parse_ihdr(data, size, &hdr)) {
        return false;
    }

    resampler_t rs;
    if (!resampler_init(&rs, (int)hdr.width, (int)hdr.height, out_w, out_h)) {
        return false;
    }

    /* Each scanline is a filter-type byte followed by the row's bytes; two
     * buffers, so the previous (already unfiltered) row stays readable. */
    size_t bpp = (size_t)hdr.channels;
    size_t row_bytes = (size_t)hdr.width * bpp;
    uint8_t *bufs = calloc(2, row_bytes + 1);
    png_stream_t s = { .data = data, .size = size, .pos = 8 /* past the signature */ };
    if (bufs == NULL || inflateInit(&s.zs) != Z_OK) {
        free(bufs);
        resampler_free(&rs);
        return false;
    }

    uint8_t *cur = bufs, *prev = bufs + row_bytes + 1; /* prev starts as the all-zero row above row 0 */
    int r = 0, g = hdr.channels >= 3 ? 1 : 0, b = hdr.channels >= 3 ? 2 : 0;
    bool ok = true;
    for (uint32_t y = 0; y < hdr.height && ok; y++) {
        ok = png_read(&s, cur, row_bytes + 1) && unfilter_row(cur[0], cur + 1, prev + 1, row_bytes, bpp);
        if (ok) {
            resampler_add(&rs, (int)y, 0, (int)hdr.width, cur + 1, (int)bpp, r, g, b);
            uint8_t *t = prev;
            prev = cur;
            cur = t;
        }
    }

    inflateEnd(&s.zs);
    free(bufs);
    if (ok) {
        resampler_finish(&rs, dst);
    }
    resampler_free(&rs);
    return ok;
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

bool rpod_cover_art_scale(const rpod_cover_art_t *src, int out_w, int out_h, rpod_cover_art_t *out)
{
    out->pixels = NULL;
    out->w = 0;
    out->h = 0;

    resampler_t rs;
    if (!resampler_init(&rs, src->w, src->h, out_w, out_h)) {
        return false;
    }
    uint16_t *dst = malloc((size_t)out_w * (size_t)out_h * sizeof(uint16_t));
    uint8_t *row = malloc((size_t)src->w * 3u);
    if (dst == NULL || row == NULL) {
        free(dst);
        free(row);
        resampler_free(&rs);
        return false;
    }

    /* RGB565 back out to 8 bits a channel (low bits replicated, so white
     * stays white), a row at a time, through the same averaging as a decode. */
    for (int y = 0; y < src->h; y++) {
        const uint16_t *p = src->pixels + (size_t)y * (size_t)src->w;
        for (int x = 0; x < src->w; x++) {
            unsigned r5 = (p[x] >> 11) & 0x1Fu, g6 = (p[x] >> 5) & 0x3Fu, b5 = p[x] & 0x1Fu;
            row[x * 3 + 0] = (uint8_t)((r5 << 3) | (r5 >> 2));
            row[x * 3 + 1] = (uint8_t)((g6 << 2) | (g6 >> 4));
            row[x * 3 + 2] = (uint8_t)((b5 << 3) | (b5 >> 2));
        }
        resampler_add(&rs, y, 0, src->w, row, 3, 0, 1, 2);
    }
    resampler_finish(&rs, dst);
    resampler_free(&rs);
    free(row);

    out->pixels = dst;
    out->w = out_w;
    out->h = out_h;
    return true;
}

/* --- Background blur (Now Playing) ----------------------------------- */

/* One separable box-blur pass (horizontal or vertical) over an RGB565
 * buffer, `src` -> `dst` (must be distinct buffers). Averaging in RGB565's
 * packed component ranges rather than linearising to 8-bit-per-channel
 * first is not gamma-correct, but this is a soft decorative backdrop, not
 * a colour-critical output -- the approximation isn't visible at the blur
 * radii used here. */
static void box_blur_pass(const uint16_t *src, uint16_t *dst, int w, int h, int radius, bool horizontal)
{
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int rsum = 0, gsum = 0, bsum = 0, n = 0;
            for (int k = -radius; k <= radius; k++) {
                int sx = horizontal ? x + k : x;
                int sy = horizontal ? y : y + k;
                if (sx < 0 || sx >= w || sy < 0 || sy >= h) {
                    continue;
                }
                uint16_t p = src[(size_t)sy * (size_t)w + (size_t)sx];
                rsum += (p >> 11) & 0x1Fu;
                gsum += (p >> 5) & 0x3Fu;
                bsum += p & 0x1Fu;
                n++;
            }
            uint16_t r = (uint16_t)(rsum / n);
            uint16_t g = (uint16_t)(gsum / n);
            uint16_t b = (uint16_t)(bsum / n);
            dst[(size_t)y * (size_t)w + (size_t)x] = (uint16_t)((r << 11) | (g << 5) | b);
        }
    }
}

bool rpod_cover_art_make_background(const rpod_cover_art_t *src, int out_w, int out_h,
                                    rpod_cover_art_t *out)
{
    if (!rpod_cover_art_scale(src, out_w, out_h, out)) {
        return false;
    }

    size_t n = (size_t)out_w * (size_t)out_h;
    uint16_t *tmp = malloc(n * sizeof(uint16_t));
    if (tmp == NULL) {
        return true; /* blur is cosmetic -- an unblurred crop still works as a background */
    }

    /* Three passes of a small-radius box blur approximate a Gaussian blur
     * cheaply and look smoother than one wide box-blur pass. */
    for (int i = 0; i < 3; i++) {
        box_blur_pass(out->pixels, tmp, out_w, out_h, 2, true);
        box_blur_pass(tmp, out->pixels, out_w, out_h, 2, false);
    }
    free(tmp);

    /* Darken so foreground text and glass panels stay legible on top. */
    for (size_t i = 0; i < n; i++) {
        uint16_t p = out->pixels[i];
        uint16_t r = (uint16_t)((((p >> 11) & 0x1Fu) * 3u) / 5u);
        uint16_t g = (uint16_t)((((p >> 5) & 0x3Fu) * 3u) / 5u);
        uint16_t b = (uint16_t)(((p & 0x1Fu) * 3u) / 5u);
        out->pixels[i] = (uint16_t)((r << 11) | (g << 5) | b);
    }

    return true;
}
