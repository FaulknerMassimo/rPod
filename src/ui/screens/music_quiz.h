/*
 * Extras > Games > Music Quiz: the game (games/quiz.h) played from your own
 * library. Each round's song plays from somewhere in its middle, added to
 * the end of the queue and played by itself -- the rest of the queue is
 * left where it was, and when the quiz ends what was playing picks up
 * where it left off, playing or paused as it was. Meanwhile the scrobbler
 * is suspended (audio/scrobbler.h) and the status bar keeps quiet about the
 * song's title.
 */

#ifndef RPOD_MUSIC_QUIZ_H
#define RPOD_MUSIC_QUIZ_H

#include "screen_stack.h"
#include "audio/mpd_client.h"

/* Loads the library's titles and pushes a new quiz on `mpd`, which must
 * outlive it. */
void rpod_music_quiz_push(rpod_screen_stack_t *stack, rpod_mpd_t *mpd);

#endif /* RPOD_MUSIC_QUIZ_H */
