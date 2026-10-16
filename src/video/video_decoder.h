/*
 * A video's pictures, decoded on a thread of their own (docs/PLAN.md §8.7):
 * FFmpeg's libavformat/libavcodec into a few RGB565 frames ready ahead, for
 * the player screen (ui/screens/video_screens.c) to put up as their time
 * comes. Only the picture: MPD plays the sound, and its position is the
 * clock the frames are timed against (video/media_clock.h), in the same
 * terms -- milliseconds from the start of the sound, the way MPD counts.
 *
 * rpod-convert's videos already fit the screen, so decoding one is just
 * H.264 at 320x180 and a colour conversion: a few ms a frame on one A53.
 * Anything else is scaled down to fit too, at whatever that costs; frames
 * that are already late when they're decoded are dropped unconverted.
 *
 * Everything below but the thread is for the LVGL thread.
 */

#ifndef RPOD_VIDEO_DECODER_H
#define RPOD_VIDEO_DECODER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct rpod_video rpod_video_t;

typedef struct {
    const uint8_t *pixels; /* RGB565, `stride` bytes a row */
    int w, h, stride;
    int64_t pts_ms;
} rpod_video_frame_t;

typedef enum {
    RPOD_VIDEO_OPENING,
    RPOD_VIDEO_READY,
    RPOD_VIDEO_FAILED,
} rpod_video_state_t;

/* Starts decoding `path` from its start, its picture scaled to fit max_w x
 * max_h (aspect kept). Opens on the thread: rpod_video_state() says how
 * that went. NULL only if out of memory. */
rpod_video_t *rpod_video_open(const char *path, int max_w, int max_h);

/* Stops the thread and frees everything, the frames handed out included. */
void rpod_video_close(rpod_video_t *v);

/* Whether it's open yet; once READY, the frames' size and the video's
 * length (0 if unknown). */
rpod_video_state_t rpod_video_state(rpod_video_t *v, int *w, int *h, unsigned *duration_ms);

/* The newest frame whose time has come at `media_ms`, if it's newer than
 * the one last returned -- NULL otherwise, the last one still showing.
 * Older frames still waiting are dropped: they're late. What's returned
 * stays valid until a later call returns another frame, or close. */
const rpod_video_frame_t *rpod_video_frame_at(rpod_video_t *v, int64_t media_ms);

/* The frame frame_at() last returned, or NULL -- for a screen built again
 * (a theme change) to show straight away. */
const rpod_video_frame_t *rpod_video_current(rpod_video_t *v);

/* Moves to `media_ms`: frames from there on follow. A `preview` (a scan)
 * shows the keyframe at or before it at once rather than decoding up to it
 * -- quick, and near enough while the position's gliding past. */
void rpod_video_seek(rpod_video_t *v, int64_t media_ms, bool preview);

/* True once every frame up to the end has been handed out. */
bool rpod_video_ended(rpod_video_t *v);

#endif /* RPOD_VIDEO_DECODER_H */
