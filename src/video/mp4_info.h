/*
 * How long an MP4 is and how big its picture, straight from its header
 * boxes -- for the video lists on the device and in the web UI
 * (docs/PLAN.md §8.7), which show every video's length without opening a
 * decoder for each. rpod-convert's files have their index first, so it's a
 * couple of small reads; an MP4 with it at the end costs one more seek.
 *
 * Pure logic over a file descriptor (tests/test_video.c).
 */

#ifndef RPOD_MP4_INFO_H
#define RPOD_MP4_INFO_H

#include <stdbool.h>

typedef struct {
    unsigned duration_ms;
    int width, height; /* the video track's display size; 0 without one */
} rpod_mp4_info_t;

/* Reads `fd` (an MP4, MOV or M4V) from the start. False if it isn't one or
 * has no movie header. */
bool rpod_mp4_info_read(int fd, rpod_mp4_info_t *out);

#endif /* RPOD_MP4_INFO_H */
