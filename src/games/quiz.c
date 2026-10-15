#include "quiz.h"

#include <strings.h>

/* A song plays from somewhere between these fractions of the way through
 * -- past an intro, short of the fade -- and has to have a whole round left
 * there, plus a little, to be asked about at all. */
#define START_MIN_PCT 20
#define START_MAX_PCT 60
#define TAIL_MS       2000
#define MIN_SONG_MS   (RPOD_QUIZ_ROUND_MS + TAIL_MS + 10000)

/* Random tries before falling back to a scan for what's needed: a library
 * of mostly short songs, or mostly one title, still gets a round if it can
 * make one. */
#define TRIES 64

uint32_t rpod_quiz_rand(uint32_t *state)
{
    uint32_t x = *state != 0 ? *state : 0x9e3779b9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static size_t below(uint32_t *rng, size_t n)
{
    return (size_t)(rpod_quiz_rand(rng) % (uint32_t)n);
}

static bool has_title(const rpod_quiz_song_t *s)
{
    return s->title != NULL && s->title[0] != '\0';
}

static bool long_enough(const rpod_quiz_song_t *s)
{
    return has_title(s) && s->duration_s * 1000u >= MIN_SONG_MS;
}

static bool was_used(size_t i, const size_t *used, size_t used_count)
{
    for (size_t k = 0; k < used_count; k++) {
        if (used[k] == i) {
            return true;
        }
    }
    return false;
}

/* An answer: a random long-enough song, one not asked before if there is
 * one. */
static bool pick_answer(const rpod_quiz_song_t *songs, size_t count, const size_t *used, size_t used_count,
                        uint32_t *rng, size_t *out)
{
    for (int pass = 0; pass < 2; pass++) {
        bool allow_used = pass == 1;
        for (int t = 0; t < TRIES; t++) {
            size_t i = below(rng, count);
            if (long_enough(&songs[i]) && (allow_used || !was_used(i, used, used_count))) {
                *out = i;
                return true;
            }
        }
        size_t from = below(rng, count);
        for (size_t k = 0; k < count; k++) {
            size_t i = (from + k) % count;
            if (long_enough(&songs[i]) && (allow_used || !was_used(i, used, used_count))) {
                *out = i;
                return true;
            }
        }
    }
    return false;
}

/* Whether song `i`'s title is unlike every one picked so far. */
static bool new_title(const rpod_quiz_song_t *songs, size_t i, const size_t *picked, size_t n)
{
    if (!has_title(&songs[i])) {
        return false;
    }
    for (size_t k = 0; k < n; k++) {
        if (strcasecmp(songs[i].title, songs[picked[k]].title) == 0) {
            return false;
        }
    }
    return true;
}

bool rpod_quiz_pick(const rpod_quiz_song_t *songs, size_t count, const size_t *used, size_t used_count,
                    uint32_t *rng, rpod_quiz_round_t *out)
{
    if (count < RPOD_QUIZ_CHOICES) {
        return false;
    }
    size_t picked[RPOD_QUIZ_CHOICES];
    if (!pick_answer(songs, count, used, used_count, rng, &picked[0])) {
        return false;
    }
    size_t n = 1;
    for (int t = 0; t < TRIES && n < RPOD_QUIZ_CHOICES; t++) {
        size_t i = below(rng, count);
        if (new_title(songs, i, picked, n)) {
            picked[n++] = i;
        }
    }
    size_t from = below(rng, count);
    for (size_t k = 0; k < count && n < RPOD_QUIZ_CHOICES; k++) {
        size_t i = (from + k) % count;
        if (new_title(songs, i, picked, n)) {
            picked[n++] = i;
        }
    }
    if (n < RPOD_QUIZ_CHOICES) {
        return false;
    }

    /* The answer goes in at a random place among the others. */
    out->answer = picked[0];
    out->correct = (int)below(rng, RPOD_QUIZ_CHOICES);
    for (int c = 0, k = 1; c < RPOD_QUIZ_CHOICES; c++) {
        out->choice[c] = c == out->correct ? picked[0] : picked[k++];
    }

    unsigned dur_ms = songs[out->answer].duration_s * 1000u;
    unsigned pct = START_MIN_PCT + (unsigned)below(rng, START_MAX_PCT - START_MIN_PCT + 1);
    unsigned start = (unsigned)((unsigned long long)dur_ms * pct / 100u);
    unsigned latest = dur_ms - RPOD_QUIZ_ROUND_MS - TAIL_MS;
    out->start_ms = start < latest ? start : latest;
    return true;
}

unsigned rpod_quiz_points(unsigned left_ms)
{
    if (left_ms >= RPOD_QUIZ_ROUND_MS) {
        return 100;
    }
    unsigned p = (left_ms * 100u + RPOD_QUIZ_ROUND_MS - 1) / RPOD_QUIZ_ROUND_MS;
    return p < 10 ? 10 : p;
}
