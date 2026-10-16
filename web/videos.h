/*
 * The videos folder from the web UI's side (docs/PLAN.md §8.7): an upload
 * arrives whole, as a hidden file in the folder, and then rpod-convert
 * (web/rpod_convert.c) makes the kind of video rPod plays out of it, in the
 * background, one at a time -- under the idle scheduler, CPU and disk, so
 * it only ever has what playback and the UI leave over, and first in line
 * for the OOM killer if a big source runs the Pi out of memory. The
 * original goes once its conversion's done.
 *
 * A conversion waiting or under way is a job: .rpod-source-<n> (the
 * upload) and .rpod-source-<n>.job beside it (what it'll be called). They
 * outlast a restart, so an interrupted job starts over when rpod-web does.
 * Its output and poster are .rpod-convert-<n>.mp4 and
 * .posters/.rpod-poster-<n>.jpg until they're done, then renamed into
 * place: "<name>.mp4", and its poster .posters/<name>.jpg.
 */

#ifndef RPOD_WEB_VIDEOS_H
#define RPOD_WEB_VIDEOS_H

#include "json.h"

#include <stdbool.h>
#include <stddef.h>

/* Starts the conversions' thread for videos folder `dir` (creating it if
 * it can), running `convert_bin`, and takes up any jobs a restart left.
 * Call once. False if the folder isn't there and can't be made. */
bool rpod_videos_start(const char *dir, const char *convert_bin);

/* Whether an upload may be called this: a kind of video FFmpeg reads. */
bool rpod_videos_upload_name_ok(const char *name);

/* The video an upload called `name` becomes: its name, ".mp4" in place of
 * its extension. */
void rpod_videos_target(const char *name, char *out, size_t size);

/* Whether `target` is waiting for or under conversion already. */
bool rpod_videos_pending(const char *target);

/* Hands over a complete upload -- `tmp_name`, a hidden file in the folder
 * -- to become `target`, replacing a video already called that only if
 * `overwrite`. It's renamed to the job's own name first. The job's id, or
 * 0 if it couldn't be queued (and the file's gone). */
unsigned rpod_videos_queue(const char *tmp_name, const char *target, bool overwrite);

/* Stops a job, waiting or under way, and throws its upload away. */
bool rpod_videos_cancel(unsigned id);

/* Forgets the jobs that have finished, failed ones included. */
void rpod_videos_clear_finished(void);

/* The jobs as a JSON array: id, name, state ("waiting", "converting",
 * "done", "failed"), progress (0-1), seconds left (-1 if not known yet),
 * error. */
void rpod_videos_json_jobs(rpod_json_t *j);

#endif /* RPOD_WEB_VIDEOS_H */
