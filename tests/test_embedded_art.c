/*
 * Reading a FLAC file's embedded cover straight off disk
 * (src/audio/embedded_art.c), against small synthetic files.
 */

#include "audio/embedded_art.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);   \
            failures++;                                                                \
        }                                                                              \
    } while (0)

/* A file being assembled in memory. */
typedef struct {
    unsigned char data[4096];
    size_t len;
} buf_t;

static void put(buf_t *b, const void *p, size_t n)
{
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

static void put_u32(buf_t *b, uint32_t v)
{
    unsigned char x[4] = { (unsigned char)(v >> 24), (unsigned char)(v >> 16), (unsigned char)(v >> 8),
                           (unsigned char)v };
    put(b, x, 4);
}

static void put_block_header(buf_t *b, int type, bool last, uint32_t len)
{
    unsigned char h[4] = { (unsigned char)((last ? 0x80 : 0) | type), (unsigned char)(len >> 16),
                           (unsigned char)(len >> 8), (unsigned char)len };
    put(b, h, 4);
}

static void put_streaminfo(buf_t *b, bool last)
{
    unsigned char info[34] = { 0 };
    put_block_header(b, 0, last, sizeof(info));
    put(b, info, sizeof(info));
}

static void put_padding(buf_t *b, bool last, uint32_t len)
{
    unsigned char zeros[64] = { 0 };
    put_block_header(b, 1, last, len);
    put(b, zeros, len);
}

/* A PICTURE block holding `data` as a picture of type `pic_type`. */
static void put_picture(buf_t *b, bool last, uint32_t pic_type, const char *data)
{
    const char *mime = "image/png", *desc = "cover";
    uint32_t data_len = (uint32_t)strlen(data);
    uint32_t len = 4 + 4 + (uint32_t)strlen(mime) + 4 + (uint32_t)strlen(desc) + 16 + 4 + data_len;
    put_block_header(b, 6, last, len);
    put_u32(b, pic_type);
    put_u32(b, (uint32_t)strlen(mime));
    put(b, mime, strlen(mime));
    put_u32(b, (uint32_t)strlen(desc));
    put(b, desc, strlen(desc));
    put_u32(b, 600); /* width */
    put_u32(b, 600); /* height */
    put_u32(b, 24);  /* depth */
    put_u32(b, 0);   /* colours */
    put_u32(b, data_len);
    put(b, data, data_len);
}

static char path[64];

static void write_file(const buf_t *b)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL || fwrite(b->data, 1, b->len, f) != b->len || fclose(f) != 0) {
        perror(path);
        exit(1);
    }
}

/* Reads `path` back; returns the picture as a string ("" for none). */
static const char *read_back(size_t max_bytes)
{
    static char got[256];
    unsigned char *out = NULL;
    size_t n = 0;
    got[0] = '\0';
    if (rpod_embedded_art_read(path, max_bytes, &out, &n)) {
        snprintf(got, sizeof(got), "%.*s", (int)n, (const char *)out);
        free(out);
    }
    return got;
}

static void test_front_cover_preferred(void)
{
    buf_t b = { .len = 0 };
    put(&b, "fLaC", 4);
    put_streaminfo(&b, false);
    put_picture(&b, false, 4, "back");
    put_padding(&b, false, 16);
    put_picture(&b, true, 3, "front");
    write_file(&b);
    CHECK(strcmp(read_back(1024), "front") == 0);
}

static void test_any_picture_without_front_cover(void)
{
    buf_t b = { .len = 0 };
    put(&b, "fLaC", 4);
    put_streaminfo(&b, false);
    put_picture(&b, false, 0, "other");
    put_picture(&b, true, 4, "back");
    write_file(&b);
    CHECK(strcmp(read_back(1024), "other") == 0);
}

static void test_no_picture(void)
{
    buf_t b = { .len = 0 };
    put(&b, "fLaC", 4);
    put_streaminfo(&b, false);
    put_padding(&b, true, 32);
    write_file(&b);
    CHECK(strcmp(read_back(1024), "") == 0);
}

static void test_id3_prefix(void)
{
    buf_t b = { .len = 0 };
    /* ID3v2.4 header, no footer, 300-byte tag body: syncsafe 0x00 0x00 0x02 0x2C. */
    unsigned char id3[10] = { 'I', 'D', '3', 4, 0, 0, 0, 0, 0x02, 0x2C };
    put(&b, id3, sizeof(id3));
    unsigned char body[300];
    memset(body, 0xAA, sizeof(body));
    put(&b, body, sizeof(body));
    put(&b, "fLaC", 4);
    put_streaminfo(&b, false);
    put_picture(&b, true, 3, "front");
    write_file(&b);
    CHECK(strcmp(read_back(1024), "front") == 0);
}

static void test_rejects(void)
{
    buf_t b = { .len = 0 };
    put(&b, "OggS", 4);
    put_streaminfo(&b, false);
    put_picture(&b, true, 3, "front");
    write_file(&b);
    CHECK(strcmp(read_back(1024), "") == 0); /* not a FLAC */

    b.len = 0;
    put(&b, "fLaC", 4);
    put_streaminfo(&b, false);
    put_picture(&b, true, 3, "front");
    write_file(&b);
    CHECK(strcmp(read_back(4), "") == 0); /* over max_bytes */

    b.len -= 3; /* picture data cut short */
    write_file(&b);
    CHECK(strcmp(read_back(1024), "") == 0);

    b.len = 0;
    put(&b, "fLaC", 4);
    put_picture(&b, true, 3, "front");
    b.data[4 + 4 + 4 + 3] = 0xFF; /* MIME length runs off the block */
    write_file(&b);
    CHECK(strcmp(read_back(1024), "") == 0);

    unlink(path);
    CHECK(strcmp(read_back(1024), "") == 0); /* no file */
}

int main(void)
{
    snprintf(path, sizeof(path), "/tmp/test_embedded_art.%ld.flac", (long)getpid());
    test_front_cover_preferred();
    test_any_picture_without_front_cover();
    test_no_picture();
    test_id3_prefix();
    test_rejects();
    unlink(path);
    if (failures != 0) {
        fprintf(stderr, "test_embedded_art: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_embedded_art: ok\n");
    return 0;
}
