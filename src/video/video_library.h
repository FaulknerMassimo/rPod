/*
 * The videos folder (docs/PLAN.md §8.7): what's in it, for the device's
 * Videos menu and the web UI's. Kept apart from the music directory, where
 * MPD would take every video for a song; MPD still plays a video's sound
 * from here, given its absolute path, since rPod and rpod-web talk to it
 * over its local socket.
 *
 * Each video is a file in the folder itself -- rpod-convert's .mp4s, or
 * anything else FFmpeg reads, copied in by hand -- with its poster, if
 * rpod-convert made one, at .posters/<name without .mp4>.jpg. Hidden files
 * (uploads and conversions still under way) aren't videos yet.
 */

#ifndef RPOD_VIDEO_LIBRARY_H
#define RPOD_VIDEO_LIBRARY_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/* Where posters live, inside the videos folder. */
#define RPOD_VIDEO_POSTER_DIR ".posters"

typedef struct {
    char name[256];  /* the file */
    char title[256]; /* what it's called: the name, less its extension */
    long long size;
    time_t mtime;
    unsigned duration_ms; /* from its MP4 header; 0 if it has none */
    int width, height;    /* likewise */
} rpod_video_entry_t;

/* Whether `name` is a kind of video this lists. */
bool rpod_video_name_ok(const char *name);

/* The videos in `dir`, in the iPod's A-Z order of their titles
 * (ui/alpha_sort.h). *out is malloc'd (NULL with none): free() it. False
 * if the folder can't be read -- which a folder not made yet is, too. */
bool rpod_video_list(const char *dir, rpod_video_entry_t **out, size_t *count);

/* The poster's path for video `name` in `dir` (whether there is one or not). */
void rpod_video_poster_path(const char *dir, const char *name, char *out, size_t size);

#endif /* RPOD_VIDEO_LIBRARY_H */
