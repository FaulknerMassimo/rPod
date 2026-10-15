/*
 * Music Quiz's rounds and scoring (src/games/quiz.c), and the games' best
 * scores (src/games/scores.c).
 */

#include "games/quiz.h"
#include "games/scores.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static int failures;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);   \
            failures++;                                                                \
        }                                                                              \
    } while (0)

/* A round's choices are all different titles, the answer among them where
 * it says, and it plays from a point with a whole round left. */
static void check_round(const rpod_quiz_song_t *songs, const rpod_quiz_round_t *r)
{
    CHECK(r->correct >= 0 && r->correct < RPOD_QUIZ_CHOICES);
    CHECK(r->choice[r->correct] == r->answer);
    for (int a = 0; a < RPOD_QUIZ_CHOICES; a++) {
        CHECK(songs[r->choice[a]].title[0] != '\0');
        for (int b = a + 1; b < RPOD_QUIZ_CHOICES; b++) {
            CHECK(strcasecmp(songs[r->choice[a]].title, songs[r->choice[b]].title) != 0);
        }
    }
    CHECK(r->start_ms + RPOD_QUIZ_ROUND_MS <= songs[r->answer].duration_s * 1000u);
}

static void test_pick(void)
{
    /* Duplicate titles (in any case), an untitled song and a jingle too
     * short to play from the middle: none of them may spoil a round. */
    static const rpod_quiz_song_t songs[] = {
        { "Intro", 240 }, { "INTRO", 200 }, { "", 300 }, { "Jingle", 15 },
        { "Alpha", 180 }, { "Bravo", 210 }, { "Charlie", 190 }, { "Delta", 230 },
        { "Echo", 250 }, { "Foxtrot", 205 },
    };
    size_t n = sizeof(songs) / sizeof(songs[0]);
    uint32_t rng = 12345;
    size_t used[RPOD_QUIZ_ROUNDS];
    for (int i = 0; i < 200; i++) {
        rpod_quiz_round_t r;
        CHECK(rpod_quiz_pick(songs, n, used, i % RPOD_QUIZ_ROUNDS, &rng, &r));
        check_round(songs, &r);
        CHECK(r.answer != 2 && r.answer != 3);
        used[i % RPOD_QUIZ_ROUNDS] = r.answer;
    }

    /* Ten rounds in a row never ask the same song twice when there's
     * enough to go round. */
    rng = 99;
    size_t asked[RPOD_QUIZ_ROUNDS];
    for (int i = 0; i < 8; i++) { /* 8 can be asked: all but the untitled and the jingle */
        rpod_quiz_round_t r;
        CHECK(rpod_quiz_pick(songs, n, asked, (size_t)i, &rng, &r));
        for (int k = 0; k < i; k++) {
            CHECK(asked[k] != r.answer);
        }
        asked[i] = r.answer;
    }
}

static void test_too_few(void)
{
    /* Five songs but only four different titles: no round. */
    static const rpod_quiz_song_t few[] = {
        { "A", 200 }, { "B", 200 }, { "C", 200 }, { "D", 200 }, { "a", 200 },
    };
    uint32_t rng = 7;
    rpod_quiz_round_t r;
    CHECK(!rpod_quiz_pick(few, 5, NULL, 0, &rng, &r));
    CHECK(!rpod_quiz_pick(few, 3, NULL, 0, &rng, &r));

    /* Nothing long enough to play. */
    static const rpod_quiz_song_t short_ones[] = {
        { "A", 10 }, { "B", 10 }, { "C", 10 }, { "D", 10 }, { "E", 10 },
    };
    CHECK(!rpod_quiz_pick(short_ones, 5, NULL, 0, &rng, &r));
}

static void test_points(void)
{
    CHECK(rpod_quiz_points(RPOD_QUIZ_ROUND_MS) == 100);
    CHECK(rpod_quiz_points(RPOD_QUIZ_ROUND_MS / 2) == 50);
    CHECK(rpod_quiz_points(1) == 10);
    CHECK(rpod_quiz_points(0) == 10);
}

static void test_scores(void)
{
    char path[] = "/tmp/rpod-test-scoresXXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    close(fd);
    unlink(path); /* first run: no file yet */

    rpod_scores_init(path);
    CHECK(rpod_score_best("brick") == 0);
    CHECK(rpod_score_submit("brick", 120));
    CHECK(!rpod_score_submit("brick", 80));
    CHECK(!rpod_score_submit("brick", 120));
    CHECK(rpod_score_submit("quiz", 640));
    CHECK(rpod_score_best("brick") == 120);

    /* And they're there after a restart. */
    rpod_scores_init(path);
    CHECK(rpod_score_best("brick") == 120);
    CHECK(rpod_score_best("quiz") == 640);
    unlink(path);
}

int main(void)
{
    test_pick();
    test_too_few();
    test_points();
    test_scores();
    if (failures != 0) {
        fprintf(stderr, "test_quiz: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_quiz: ok\n");
    return 0;
}
