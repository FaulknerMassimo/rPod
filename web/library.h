/*
 * The music library on disk, for the web UI's Music screens: listing,
 * uploading into, and tidying MPD's music directory.
 *
 * Every path the browser sends is relative to the music directory and
 * checked before use (rpod_lib_path_ok()), then walked a component at a
 * time with openat(O_NOFOLLOW), so neither ".." nor a symlink can lead
 * outside it. Names follow exFAT's rules as well as ext4's: the device's
 * music partition is exFAT (docs/PLAN.md §3), whatever the dev board has.
 *
 * The checks are pure logic (tests/test_web.c).
 */

#ifndef RPOD_WEB_LIBRARY_H
#define RPOD_WEB_LIBRARY_H

#include <stdbool.h>
#include <stddef.h>

/* One path component: not empty, ".", "..", or starting with "." (hidden,
 * and where uploads in progress live), no '/', control characters or
 * exFAT's reserved "*:<>?\|, no trailing space or dot, at most 255 bytes,
 * valid UTF-8. */
bool rpod_lib_name_ok(const char *name);

/* A relative path of such components joined by single slashes; "" is the
 * music directory itself, allowed only if `allow_root`. */
bool rpod_lib_path_ok(const char *path, bool allow_root);

/* Whether an upload may be called this: music MPD can play, cover images,
 * cue sheets and lyrics. */
bool rpod_lib_upload_name_ok(const char *name);

/* Opens directory `path` under the music directory `root` (O_DIRECTORY,
 * never through a symlink), creating missing ones on the way if `create`.
 * The fd, or -errno. */
int rpod_lib_open_dir(const char *root, const char *path, bool create);

/* Deletes `path` (not the root), a whole directory tree included. 0 or
 * -errno. */
int rpod_lib_delete(const char *root, const char *path);

/* Renames `path` within its directory to `new_name`, never over something
 * already there. 0 or -errno. */
int rpod_lib_rename(const char *root, const char *path, const char *new_name);

/* Where an upload goes while it arrives: a hidden temp file in the target
 * directory, so MPD never scans half a song, renamed into place by
 * rpod_lib_upload_finish(). The open fd, or -errno; *dir_fd is the
 * directory's, and tmp_name what the temp file is called. */
int rpod_lib_upload_begin(const char *root, const char *dir, int *dir_fd, char *tmp_name, size_t tmp_size);

/* Moves a finished upload into place as `name`, replacing what's there
 * only if `overwrite` (*replaced says whether it did). Closes nothing. 0 or
 * -errno (-EEXIST: it's there and !overwrite). */
int rpod_lib_upload_finish(int dir_fd, const char *tmp_name, const char *name, bool overwrite, bool *replaced);

#endif /* RPOD_WEB_LIBRARY_H */
