#include "mp4_info.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Bigger than any real movie header of a video short enough to carry
 * around (a feature film's is ~5 MB); a bound on what's read into memory,
 * for the length of the parse. */
#define MOOV_MAX (32u * 1024u * 1024u)

/* A box can't hold more than this many boxes inside it worth walking. */
#define MAX_BOXES 4096

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t be64(const uint8_t *p)
{
    return (uint64_t)be32(p) << 32 | be32(p + 4);
}

static bool read_at(int fd, void *buf, size_t n, uint64_t off)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = pread(fd, (uint8_t *)buf + got, n - got, (off_t)(off + got));
        if (r <= 0) {
            return false;
        }
        got += (size_t)r;
    }
    return true;
}

/* The box at `off` in [off, end): its type, where its payload starts, and
 * its total size. */
static bool box_at(const uint8_t *buf, size_t len, size_t off, char type[5], size_t *payload, size_t *size)
{
    if (len - off < 8) {
        return false;
    }
    uint64_t sz = be32(buf + off);
    size_t hdr = 8;
    if (sz == 1) {
        if (len - off < 16) {
            return false;
        }
        sz = be64(buf + off + 8);
        hdr = 16;
    } else if (sz == 0) {
        sz = len - off;
    }
    if (sz < hdr || sz > len - off) {
        return false;
    }
    memcpy(type, buf + off + 4, 4);
    type[4] = '\0';
    *payload = off + hdr;
    *size = (size_t)sz;
    return true;
}

/* The first box of `want` directly inside [start, end) of buf. */
static bool find_box(const uint8_t *buf, size_t start, size_t end, const char *want, size_t *payload, size_t *bend)
{
    size_t off = start;
    for (int i = 0; i < MAX_BOXES && off < end; i++) {
        char type[5];
        size_t p, sz;
        if (!box_at(buf, end, off, type, &p, &sz)) {
            return false;
        }
        if (memcmp(type, want, 4) == 0) {
            *payload = p;
            *bend = off + sz;
            return true;
        }
        off += sz;
    }
    return false;
}

/* tkhd's width and height (16.16 fixed point), for a 'vide' track. */
static void video_size(const uint8_t *moov, size_t start, size_t end, rpod_mp4_info_t *out)
{
    size_t off = start;
    for (int i = 0; i < MAX_BOXES && off < end; i++) {
        char type[5];
        size_t p, sz;
        if (!box_at(moov, end, off, type, &p, &sz)) {
            return;
        }
        size_t tend = off + sz;
        off = tend;
        if (strcmp(type, "trak") != 0) {
            continue;
        }
        size_t mdia, mdia_end, hdlr, hdlr_end, tkhd, tkhd_end;
        if (!find_box(moov, p, tend, "mdia", &mdia, &mdia_end) ||
            !find_box(moov, mdia, mdia_end, "hdlr", &hdlr, &hdlr_end) || hdlr_end - hdlr < 12 ||
            memcmp(moov + hdlr + 8, "vide", 4) != 0 || !find_box(moov, p, tend, "tkhd", &tkhd, &tkhd_end)) {
            continue;
        }
        /* Version 0: 4 + 5*4 + 8 + 4 + 36 bytes before width; version 1, 12 more. */
        size_t wh = tkhd + (moov[tkhd] == 1 ? 88 : 76);
        if (wh + 8 <= tkhd_end) {
            out->width = (int)(be32(moov + wh) >> 16);
            out->height = (int)(be32(moov + wh + 4) >> 16);
            return;
        }
    }
}

static bool parse_moov(const uint8_t *moov, size_t len, rpod_mp4_info_t *out)
{
    size_t mvhd, mvhd_end;
    if (!find_box(moov, 0, len, "mvhd", &mvhd, &mvhd_end)) {
        return false;
    }
    uint64_t timescale, duration;
    if (moov[mvhd] == 1) {
        if (mvhd_end - mvhd < 32) {
            return false;
        }
        timescale = be32(moov + mvhd + 20);
        duration = be64(moov + mvhd + 24);
    } else {
        if (mvhd_end - mvhd < 20) {
            return false;
        }
        timescale = be32(moov + mvhd + 12);
        duration = be32(moov + mvhd + 16);
    }
    if (timescale == 0) {
        return false;
    }
    uint64_t ms = duration * 1000u / timescale;
    out->duration_ms = ms > UINT32_MAX ? UINT32_MAX : (unsigned)ms;
    video_size(moov, 0, len, out);
    return true;
}

bool rpod_mp4_info_read(int fd, rpod_mp4_info_t *out)
{
    memset(out, 0, sizeof(*out));
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 16) {
        return false;
    }
    uint64_t file_size = (uint64_t)st.st_size;

    /* The top-level boxes, skipping over the media data to the index. */
    uint64_t off = 0;
    for (int i = 0; i < MAX_BOXES && off + 8 <= file_size; i++) {
        uint8_t hdr[16];
        if (!read_at(fd, hdr, 8, off)) {
            return false;
        }
        uint64_t sz = be32(hdr);
        uint64_t hlen = 8;
        if (sz == 1) {
            if (!read_at(fd, hdr + 8, 8, off + 8)) {
                return false;
            }
            sz = be64(hdr + 8);
            hlen = 16;
        } else if (sz == 0) {
            sz = file_size - off;
        }
        if (sz < hlen || sz > file_size - off) {
            return false;
        }
        if (i == 0 && memcmp(hdr + 4, "ftyp", 4) != 0 && memcmp(hdr + 4, "moov", 4) != 0 &&
            memcmp(hdr + 4, "wide", 4) != 0 && memcmp(hdr + 4, "mdat", 4) != 0) {
            return false; /* not an MP4 at all */
        }
        if (memcmp(hdr + 4, "moov", 4) == 0) {
            uint64_t len = sz - hlen;
            if (len > MOOV_MAX) {
                return false;
            }
            uint8_t *buf = malloc((size_t)len);
            bool ok = buf != NULL && read_at(fd, buf, (size_t)len, off + hlen) && parse_moov(buf, (size_t)len, out);
            free(buf);
            return ok;
        }
        off += sz;
    }
    return false;
}
