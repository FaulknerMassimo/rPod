/*
 * Reads a FLAC file's embedded cover picture straight off disk, bypassing
 * MPD's "readpicture".
 *
 * MPD serves readpicture on its main thread, re-reading the file's tags for
 * every chunk it sends -- so while it fetches one 1-4 MB cover, every other
 * client waits. Measured on the Pi 3B dev board: the UI's status poll, 0.2
 * ms normally, took 69 ms on average and up to 1.4 s while another
 * connection fetched covers. rPod runs beside MPD and can read the library
 * itself (MPD's "config" gives its music directory to local clients), so
 * for FLAC -- what the library is -- this is one plain read of the picture
 * block that holds no one else up. Anything else still goes through MPD.
 */

#ifndef RPOD_EMBEDDED_ART_H
#define RPOD_EMBEDDED_ART_H

#include <stdbool.h>
#include <stddef.h>

/* Returns the raw (still encoded) bytes of the FLAC file's front-cover
 * picture -- or its first picture of any kind, if none is marked as the
 * front cover -- as a malloc'd buffer the caller frees. False if `path`
 * isn't a readable FLAC file, has no picture, or the picture is larger than
 * `max_bytes`. Handles the ID3v2 tag some taggers put in front of the FLAC
 * stream. */
bool rpod_embedded_art_read(const char *path, size_t max_bytes, unsigned char **out, size_t *out_size);

#endif /* RPOD_EMBEDDED_ART_H */
