/*
 * The "Add to Playlist" sheet, pushed by a press-and-hold on the selected
 * song (Now Playing or a song list). Lists "Liked Songs" first, then every
 * stored playlist, each with a membership indicator; selecting a row toggles
 * the song's membership (adding it, or de-selecting it if already there).
 * Backing out (Menu) returns to the caller, which refreshes its own liked /
 * playlist indicators when it regains focus.
 */

#ifndef RPOD_PLAYLIST_PICKER_H
#define RPOD_PLAYLIST_PICKER_H

#include "screen_stack.h"

typedef struct rpod_mpd rpod_mpd_t;

/* `uri` is the song to add/remove; `title` is shown in the sheet header for
 * context. Both are copied. */
void rpod_playlist_picker_push(rpod_screen_stack_t *stack, rpod_mpd_t *mpd,
                                const char *uri, const char *title);

#endif /* RPOD_PLAYLIST_PICKER_H */
