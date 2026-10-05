#include "embedded_art.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* FLAC metadata block types (FLAC format spec, METADATA_BLOCK_HEADER). */
#define FLAC_BLOCK_PICTURE 6
#define FLAC_BLOCK_INVALID 127

/* PICTURE block picture type for "Cover (front)" (the ID3v2 APIC list). */
#define FLAC_PICTURE_FRONT_COVER 3

static bool read_u32be(FILE *f, uint32_t *out)
{
    unsigned char b[4];
    if (fread(b, 1, sizeof(b), f) != sizeof(b)) {
        return false;
    }
    *out = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | (uint32_t)b[3];
    return true;
}

/* Reads a PICTURE block's fields up to its data (the file is positioned at
 * the block's start, `block_len` bytes long), leaving *data_pos and
 * *data_len describing where the image bytes are. False if the fields don't
 * fit the block. */
static bool parse_picture(FILE *f, uint32_t block_len, uint32_t *type, long *data_pos, uint32_t *data_len)
{
    long start = ftell(f);
    uint32_t mime_len, desc_len;
    if (start < 0 || !read_u32be(f, type) || !read_u32be(f, &mime_len) ||
        fseek(f, (long)mime_len, SEEK_CUR) != 0 || !read_u32be(f, &desc_len) ||
        /* description, then width, height, colour depth, palette size */
        fseek(f, (long)desc_len + 16, SEEK_CUR) != 0 || !read_u32be(f, data_len)) {
        return false;
    }
    *data_pos = ftell(f);
    return *data_pos >= start && (uint64_t)(*data_pos - start) + *data_len <= block_len;
}

static bool read_at(FILE *f, long pos, uint32_t len, size_t max_bytes, unsigned char **out, size_t *out_size)
{
    if (len == 0 || len > max_bytes || fseek(f, pos, SEEK_SET) != 0) {
        return false;
    }
    unsigned char *buf = malloc(len);
    if (buf == NULL) {
        return false;
    }
    if (fread(buf, 1, len, f) != len) {
        free(buf);
        return false;
    }
    *out = buf;
    *out_size = len;
    return true;
}

static bool read_picture(FILE *f, size_t max_bytes, unsigned char **out, size_t *out_size)
{
    unsigned char hdr[10];
    if (fread(hdr, 1, 4, f) != 4) {
        return false;
    }
    /* An ID3v2 tag in front of the stream: not part of the FLAC format, but
     * some taggers write one and decoders (MPD included) skip it. Its size
     * is "syncsafe" -- 7 bits a byte -- and excludes the 10-byte header and
     * the optional 10-byte footer. */
    if (memcmp(hdr, "ID3", 3) == 0) {
        if (fread(hdr + 4, 1, 6, f) != 6) {
            return false;
        }
        long size = ((long)(hdr[6] & 0x7F) << 21) | ((long)(hdr[7] & 0x7F) << 14) |
                    ((long)(hdr[8] & 0x7F) << 7) | (long)(hdr[9] & 0x7F);
        long skip = 10 + size + ((hdr[5] & 0x10) ? 10 : 0);
        if (fseek(f, skip, SEEK_SET) != 0 || fread(hdr, 1, 4, f) != 4) {
            return false;
        }
    }
    if (memcmp(hdr, "fLaC", 4) != 0) {
        return false;
    }

    long other_pos = -1; /* the first non-front-cover picture, if that's all there is */
    uint32_t other_len = 0;
    for (;;) {
        unsigned char bh[4];
        if (fread(bh, 1, sizeof(bh), f) != sizeof(bh)) {
            break;
        }
        bool last = (bh[0] & 0x80) != 0;
        int type = bh[0] & 0x7F;
        uint32_t len = ((uint32_t)bh[1] << 16) | ((uint32_t)bh[2] << 8) | (uint32_t)bh[3];
        long next = ftell(f);
        if (type == FLAC_BLOCK_INVALID || next < 0) {
            break;
        }
        next += (long)len;

        uint32_t pic_type, data_len;
        long data_pos;
        if (type == FLAC_BLOCK_PICTURE && parse_picture(f, len, &pic_type, &data_pos, &data_len)) {
            if (pic_type == FLAC_PICTURE_FRONT_COVER) {
                return read_at(f, data_pos, data_len, max_bytes, out, out_size);
            }
            if (other_pos < 0) {
                other_pos = data_pos;
                other_len = data_len;
            }
        }
        if (last || fseek(f, next, SEEK_SET) != 0) {
            break;
        }
    }
    return other_pos >= 0 && read_at(f, other_pos, other_len, max_bytes, out, out_size);
}

bool rpod_embedded_art_read(const char *path, size_t max_bytes, unsigned char **out, size_t *out_size)
{
    *out = NULL;
    *out_size = 0;
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    bool found = read_picture(f, max_bytes, out, out_size);
    fclose(f);
    return found;
}
