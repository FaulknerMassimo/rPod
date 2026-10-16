/*
 * Videos (docs/PLAN.md §8.7): the MP4 header reader (src/video/mp4_info.c),
 * the clock the picture follows (src/video/media_clock.c) and the videos
 * folder's listing (src/video/video_library.c).
 */

#include "video/media_clock.h"
#include "video/mp4_info.h"
#include "video/video_library.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);   \
            failures++;                                                                \
        }                                                                              \
    } while (0)

/* --- Building MP4 boxes ------------------------------------------------------ */

typedef struct {
    uint8_t d[4096];
    size_t n;
} buf_t;

static void put32(buf_t *b, uint32_t v)
{
    b->d[b->n++] = (uint8_t)(v >> 24);
    b->d[b->n++] = (uint8_t)(v >> 16);
    b->d[b->n++] = (uint8_t)(v >> 8);
    b->d[b->n++] = (uint8_t)v;
}

static void put64(buf_t *b, uint64_t v)
{
    put32(b, (uint32_t)(v >> 32));
    put32(b, (uint32_t)v);
}

static void zeros(buf_t *b, size_t n)
{
    memset(b->d + b->n, 0, n);
    b->n += n;
}

/* Opens a box: its size is filled in by close_box(). */
static size_t open_box(buf_t *b, const char *type)
{
    size_t at = b->n;
    put32(b, 0);
    memcpy(b->d + b->n, type, 4);
    b->n += 4;
    return at;
}

static void close_box(buf_t *b, size_t at)
{
    uint32_t size = (uint32_t)(b->n - at);
    b->d[at] = (uint8_t)(size >> 24);
    b->d[at + 1] = (uint8_t)(size >> 16);
    b->d[at + 2] = (uint8_t)(size >> 8);
    b->d[at + 3] = (uint8_t)size;
}

static void trak(buf_t *b, const char *handler, uint32_t w, uint32_t h)
{
    size_t t = open_box(b, "trak");
    size_t k = open_box(b, "tkhd");
    put32(b, 0x00000003); /* version 0, flags */
    zeros(b, 72);         /* times, track id, duration, layer..., matrix */
    put32(b, w << 16);
    put32(b, h << 16);
    close_box(b, k);
    size_t m = open_box(b, "mdia");
    size_t hd = open_box(b, "hdlr");
    put32(b, 0);
    put32(b, 0);
    memcpy(b->d + b->n, handler, 4);
    b->n += 4;
    zeros(b, 12);
    b->d[b->n++] = 0;
    close_box(b, hd);
    close_box(b, m);
    close_box(b, t);
}

/* An MP4 with its index after the media data, as a camera writes one:
 * ftyp, then mdat (with a 64-bit size if `large`), then moov. */
static void build(buf_t *b, int mvhd_version, uint32_t timescale, uint64_t duration, bool large)
{
    b->n = 0;
    size_t f = open_box(b, "ftyp");
    memcpy(b->d + b->n, "isom", 4);
    b->n += 4;
    put32(b, 512);
    close_box(b, f);

    if (large) {
        put32(b, 1);
        memcpy(b->d + b->n, "mdat", 4);
        b->n += 4;
        put64(b, 16 + 100);
    } else {
        put32(b, 8 + 100);
        memcpy(b->d + b->n, "mdat", 4);
        b->n += 4;
    }
    zeros(b, 100);

    size_t moov = open_box(b, "moov");
    size_t mv = open_box(b, "mvhd");
    if (mvhd_version == 1) {
        put32(b, 0x01000000);
        put64(b, 0);
        put64(b, 0);
        put32(b, timescale);
        put64(b, duration);
    } else {
        put32(b, 0);
        put32(b, 0);
        put32(b, 0);
        put32(b, timescale);
        put32(b, (uint32_t)duration);
    }
    zeros(b, 80);
    close_box(b, mv);
    trak(b, "soun", 0, 0);
    trak(b, "vide", 320, 180);
    close_box(b, moov);
}

static int temp_file(const buf_t *b)
{
    char path[] = "/tmp/rpod-test-mp4-XXXXXX";
    int fd = mkstemp(path);
    unlink(path);
    if (fd >= 0 && write(fd, b->d, b->n) != (ssize_t)b->n) {
        close(fd);
        return -1;
    }
    return fd;
}

static void test_mp4_info(void)
{
    buf_t b;
    rpod_mp4_info_t info;

    build(&b, 0, 1000, 12345, false);
    int fd = temp_file(&b);
    CHECK(fd >= 0 && rpod_mp4_info_read(fd, &info));
    CHECK(info.duration_ms == 12345);
    CHECK(info.width == 320 && info.height == 180); /* the video track, not the sound's */
    close(fd);

    /* Version 1 headers, a 90 kHz clock, and a 64-bit mdat to skip. */
    build(&b, 1, 90000, 90000ull * 3600 * 2 + 45000, true);
    fd = temp_file(&b);
    CHECK(fd >= 0 && rpod_mp4_info_read(fd, &info));
    CHECK(info.duration_ms == 2u * 3600 * 1000 + 500);
    close(fd);

    /* Cut off inside the index: not trusted. */
    build(&b, 0, 1000, 5000, false);
    b.n -= 40;
    fd = temp_file(&b);
    CHECK(fd >= 0 && !rpod_mp4_info_read(fd, &info));
    close(fd);

    /* Not an MP4 at all. */
    b.n = 0;
    memcpy(b.d, "fLaC\0\0\0\x22 plus some more bytes", 30);
    b.n = 30;
    fd = temp_file(&b);
    CHECK(fd >= 0 && !rpod_mp4_info_read(fd, &info));
    close(fd);

    /* A timescale of 0 would divide by it. */
    build(&b, 0, 0, 5000, false);
    fd = temp_file(&b);
    CHECK(fd >= 0 && !rpod_mp4_info_read(fd, &info));
    close(fd);
}

/* --- The clock ----------------------------------------------------------------- */

static void test_media_clock(void)
{
    rpod_media_clock_t c;
    rpod_media_clock_reset(&c);
    CHECK(rpod_media_clock_now(&c, 5000) == 0);

    /* Starting near the start isn't a jump: the picture's there already. */
    CHECK(!rpod_media_clock_observe(&c, 40, true, 1000));
    CHECK(rpod_media_clock_now(&c, 1000) == 40);
    CHECK(rpod_media_clock_now(&c, 1250) == 290); /* runs on between answers */

    /* An answer a chunk off is eased toward (a quarter of the way), not
     * jumped to: 20 ms ahead of the 240 it had got to. */
    CHECK(!rpod_media_clock_observe(&c, 260, true, 1200));
    CHECK(rpod_media_clock_now(&c, 1200) == 245);

    /* A seek is. */
    CHECK(rpod_media_clock_observe(&c, 60000, true, 1400));
    CHECK(rpod_media_clock_now(&c, 1400) == 60000);
    CHECK(rpod_media_clock_now(&c, 1500) == 60100);

    /* Paused, it stands still where MPD says, even a little off. */
    CHECK(!rpod_media_clock_observe(&c, 60150, false, 1600));
    CHECK(rpod_media_clock_now(&c, 1600) == 60150);
    CHECK(rpod_media_clock_now(&c, 9000) == 60150);

    /* Playing again from there. */
    CHECK(!rpod_media_clock_observe(&c, 60150, true, 9000));
    CHECK(rpod_media_clock_now(&c, 9100) == 60250);

    /* Back to the start is a jump too. */
    CHECK(rpod_media_clock_observe(&c, 0, true, 9200));
    CHECK(rpod_media_clock_now(&c, 9200) == 0);

    /* The first answer far from the start is one. */
    rpod_media_clock_reset(&c);
    CHECK(rpod_media_clock_observe(&c, 30000, true, 100));
}

/* --- The folder ---------------------------------------------------------------- */

static void touch(const char *dir, const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    if (f != NULL) {
        fputs("not really a video", f);
        fclose(f);
    }
}

static void test_library(void)
{
    CHECK(rpod_video_name_ok("Clip.mp4"));
    CHECK(rpod_video_name_ok("Holiday.MOV"));
    CHECK(rpod_video_name_ok("a.mkv"));
    CHECK(!rpod_video_name_ok(".rpod-upload-1-0.part"));
    CHECK(!rpod_video_name_ok(".hidden.mp4"));
    CHECK(!rpod_video_name_ok("song.flac"));
    CHECK(!rpod_video_name_ok("mp4"));
    CHECK(!rpod_video_name_ok(".mp4"));

    char poster[512];
    rpod_video_poster_path("/media/videos", "My Clip.mp4", poster, sizeof(poster));
    CHECK(strcmp(poster, "/media/videos/.posters/My Clip.jpg") == 0);
    rpod_video_poster_path("/v", "a.b.mkv", poster, sizeof(poster));
    CHECK(strcmp(poster, "/v/.posters/a.b.jpg") == 0);

    char dir[] = "/tmp/rpod-test-videos-XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    touch(dir, "b roll.mp4");
    touch(dir, "The Clip.mp4");
    touch(dir, "Archive.mkv");
    touch(dir, ".rpod-upload-7-1.part");
    touch(dir, "notes.txt");
    char sub[600];
    snprintf(sub, sizeof(sub), "%s/folder.mp4", dir);
    mkdir(sub, 0755); /* a directory with a video's name isn't one */

    rpod_video_entry_t *list;
    size_t n;
    CHECK(rpod_video_list(dir, &list, &n));
    CHECK(n == 3);
    if (n == 3) {
        /* The iPod's order: "The Clip" files under C. */
        CHECK(strcmp(list[0].name, "Archive.mkv") == 0);
        CHECK(strcmp(list[1].name, "b roll.mp4") == 0);
        CHECK(strcmp(list[2].name, "The Clip.mp4") == 0);
        CHECK(strcmp(list[2].title, "The Clip") == 0);
        CHECK(list[0].duration_ms == 0); /* not a real MP4 */
        CHECK(list[1].size == (long long)strlen("not really a video"));
    }
    free(list);

    char name[600];
    const char *names[] = { "b roll.mp4", "The Clip.mp4", "Archive.mkv", ".rpod-upload-7-1.part", "notes.txt" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        snprintf(name, sizeof(name), "%s/%s", dir, names[i]);
        unlink(name);
    }
    rmdir(sub);
    rmdir(dir);

    CHECK(!rpod_video_list("/nonexistent/rpod-videos", &list, &n));
    CHECK(list == NULL && n == 0);
}

int main(void)
{
    test_mp4_info();
    test_media_clock();
    test_library();
    if (failures != 0) {
        fprintf(stderr, "test_video: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_video: ok\n");
    return 0;
}
