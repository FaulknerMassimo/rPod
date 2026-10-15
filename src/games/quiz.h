/*
 * Music Quiz (Extras > Games), the iPod's: a few seconds of a song from your
 * library play, and you pick its title from RPOD_QUIZ_CHOICES before the
 * round runs out -- the quicker, the more it scores. This is the picking
 * and the scoring; ui/screens/music_quiz.c plays and shows it. Pure C: no
 * LVGL, no MPD.
 */

#ifndef RPOD_QUIZ_H
#define RPOD_QUIZ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RPOD_QUIZ_CHOICES  5
#define RPOD_QUIZ_ROUNDS   10
#define RPOD_QUIZ_ROUND_MS 10000

/* What a round needs of a song: its title and length. The caller's. */
typedef struct {
    const char *title;
    unsigned duration_s;
} rpod_quiz_song_t;

typedef struct {
    size_t answer;                      /* the song that plays */
    size_t choice[RPOD_QUIZ_CHOICES];   /* titles to pick from, in the order shown */
    int correct;                        /* which of them is the answer */
    unsigned start_ms;                  /* where in the answer it starts playing */
} rpod_quiz_round_t;

/* The quiz's random numbers: xorshift32, from a nonzero seed. */
uint32_t rpod_quiz_rand(uint32_t *state);

/* Picks a round from `songs`: a song long enough to play from somewhere in
 * its middle for a whole round, not one of the `used` answers before it
 * (where it can avoid them), and RPOD_QUIZ_CHOICES - 1 others whose titles
 * differ from it and from each other, ignoring case. False if the library
 * hasn't enough different titles to make one. */
bool rpod_quiz_pick(const rpod_quiz_song_t *songs, size_t count, const size_t *used, size_t used_count,
                    uint32_t *rng, rpod_quiz_round_t *out);

/* What a right answer scores with `left_ms` of the round to spare: 100 for
 * an instant one, down to 10. */
unsigned rpod_quiz_points(unsigned left_ms);

#endif /* RPOD_QUIZ_H */
