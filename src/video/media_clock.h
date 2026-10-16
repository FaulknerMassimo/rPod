/*
 * The clock a video's picture follows (docs/PLAN.md §8.7): MPD's position in
 * the video's sound, which MPD plays. MPD only says where it is when asked,
 * a few times a second and in steps of its ~20 ms chunks, so this runs a
 * clock of its own between the answers -- on time while playing, still
 * while paused -- and eases it toward each answer rather than jumping, so
 * frames come out evenly. A big difference (a seek, a stall, playback
 * starting or stopping) is taken at once instead.
 *
 * Pure logic, clocked by the caller's milliseconds (tests/test_video.c).
 */

#ifndef RPOD_MEDIA_CLOCK_H
#define RPOD_MEDIA_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

/* An answer this far from where the clock had got to is a jump, not drift. */
#define RPOD_MEDIA_CLOCK_SNAP_MS 120

typedef struct {
    bool valid;
    bool playing;
    int64_t media_ms; /* where it was at wall_ms */
    int64_t wall_ms;
} rpod_media_clock_t;

void rpod_media_clock_reset(rpod_media_clock_t *c);

/* MPD said it was `media_ms` into the video at `wall_ms` (the middle of the
 * round trip), playing or not. True if that's a jump -- more than
 * RPOD_MEDIA_CLOCK_SNAP_MS from where the clock had got to (from 0, for
 * the first answer) -- which the picture has to seek to. A pause or play
 * is taken at once too, but isn't a jump. */
bool rpod_media_clock_observe(rpod_media_clock_t *c, int64_t media_ms, bool playing, int64_t wall_ms);

/* Where the video is at `wall_ms`; 0 before the first observation. */
int64_t rpod_media_clock_now(const rpod_media_clock_t *c, int64_t wall_ms);

#endif /* RPOD_MEDIA_CLOCK_H */
