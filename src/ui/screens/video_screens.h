/*
 * Main Menu > Videos (docs/PLAN.md §8.7): the videos folder's list, and the
 * player -- the picture full screen, decoded on its own thread
 * (video/video_decoder.h), and the sound through MPD, whose position the
 * picture follows (video/media_clock.h, video/av_sync.h). Everything MPD
 * does for music works for a video's sound too: the output, the volume,
 * AirPods pausing it, the sleep timer.
 *
 * Playing one borrows playback the way Music Quiz does: the video is added
 * to the end of the queue and played by itself, and leaving it -- Menu, or
 * its end -- puts back the song that was playing, where it was, paused. If
 * something else takes playback over meanwhile (the alarm, the web UI),
 * that stands, and the player goes.
 *
 * On the player the wheel is the volume. Center brings up the title and
 * progress bar with the wheel scrubbing instead, the iPod's way, until it's
 * left alone a few seconds; paused, the bar stays up. Play/Pause pauses;
 * Next goes to the next video in the list, Prev back to the start or to the
 * one before; held, they scan, the picture following. The backlight stays
 * on while it plays.
 */

#ifndef RPOD_VIDEO_SCREENS_H
#define RPOD_VIDEO_SCREENS_H

#include "screen_stack.h"
#include "audio/mpd_client.h"

#include <stdbool.h>

/* Where the videos are (video/video_library.h). Call once, before the menu
 * is opened. */
void rpod_videos_init(const char *video_dir);

/* Main Menu > Videos, on `mpd` (which must outlive it). */
void rpod_videos_push(rpod_screen_stack_t *stack, rpod_mpd_t *mpd);

/* Plays video `name` from the videos folder at once, over whatever's up --
 * the web UI's Play on rPod (ui/control.h). False if it isn't there. */
bool rpod_video_play(rpod_screen_stack_t *stack, rpod_mpd_t *mpd, const char *name);

/* Next (dir > 0) or Prev while a video plays: the next video, or back to
 * the start or the one before (app.c). False with none playing -- the
 * button's the music's then. */
bool rpod_video_player_skip(int dir);

#endif /* RPOD_VIDEO_SCREENS_H */
