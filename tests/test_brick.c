/*
 * Brick's rules and physics (src/games/brick.c).
 */

#include "games/brick.h"

#include <stdio.h>

static int failures;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);   \
            failures++;                                                                \
        }                                                                              \
    } while (0)

static rpod_brick_t b;

static unsigned run(unsigned ms)
{
    unsigned ev = 0;
    for (unsigned t = 0; t < ms; t += 30) {
        ev |= rpod_brick_step(&b, 30);
    }
    return ev;
}

static void test_new_game_and_serve(void)
{
    rpod_brick_new_game(&b, 320, 190);
    CHECK(b.state == RPOD_BRICK_SERVE);
    CHECK(b.bricks_left == RPOD_BRICK_ROWS * RPOD_BRICK_COLS);
    CHECK(b.balls == RPOD_BRICK_BALLS && b.level == 1 && b.score == 0);

    /* The ball rides the paddle, which stops at the walls. */
    rpod_brick_move_paddle(&b, -1000);
    CHECK(b.paddle_x == 0.0f);
    CHECK(b.ball_x == b.paddle_w / 2.0f);
    rpod_brick_move_paddle(&b, 1000);
    CHECK(b.paddle_x == b.w - b.paddle_w);
    CHECK(rpod_brick_step(&b, 30) == 0); /* nothing moves until it's served */

    rpod_brick_serve(&b);
    CHECK(b.state == RPOD_BRICK_PLAY);
    CHECK(b.vy < 0.0f && b.vx < 0.0f); /* up, toward the middle */
}

/* Straight up from the middle, it breaks a bottom-row brick and comes back. */
static void test_hit(void)
{
    rpod_brick_new_game(&b, 320, 190);
    rpod_brick_serve(&b);
    rpod_brick_rect_t cell = rpod_brick_cell(&b, 0, 3);
    b.ball_x = cell.x + cell.w / 2.0f;
    b.vx = 0.0f;
    b.vy = -rpod_brick_speed(&b);
    unsigned ev = run(1000);
    CHECK(ev & RPOD_BRICK_EV_HIT);
    CHECK(b.bricks_left == RPOD_BRICK_ROWS * RPOD_BRICK_COLS - 1);
    CHECK(b.score == 1); /* the bottom row's worth least */
    CHECK(b.vy > 0.0f);
}

/* However fast it goes and however long the frame, it can't skip a brick. */
static void test_no_tunnelling(void)
{
    rpod_brick_new_game(&b, 320, 190);
    rpod_brick_serve(&b);
    rpod_brick_rect_t cell = rpod_brick_cell(&b, 0, 3);
    b.ball_x = cell.x + cell.w / 2.0f; /* square on a brick, not between two */
    b.vx = 0.0f;
    b.vy = -5000.0f;
    rpod_brick_step(&b, 100);
    CHECK(b.bricks_left == RPOD_BRICK_ROWS * RPOD_BRICK_COLS - 1);
}

static void test_miss_and_game_over(void)
{
    rpod_brick_new_game(&b, 320, 190);
    for (int i = 0; i < RPOD_BRICK_BALLS; i++) {
        rpod_brick_serve(&b);
        b.ball_x = 5.0f; /* well clear of the paddle, on its way down */
        b.paddle_x = b.w - b.paddle_w;
        b.ball_y = b.paddle_y + b.paddle_h + 5.0f;
        b.vx = 0.0f;
        b.vy = 200.0f;
        unsigned ev = run(500);
        CHECK(ev & RPOD_BRICK_EV_MISS);
        CHECK(b.balls == RPOD_BRICK_BALLS - 1 - i);
        CHECK(b.state == (i + 1 < RPOD_BRICK_BALLS ? RPOD_BRICK_SERVE : RPOD_BRICK_OVER));
        CHECK(!(ev & RPOD_BRICK_EV_OVER) == (i + 1 < RPOD_BRICK_BALLS));
    }
}

/* The last brick sets up the next level, faster. */
static void test_clear(void)
{
    rpod_brick_new_game(&b, 320, 190);
    float speed1 = rpod_brick_speed(&b);
    for (int r = 0; r < RPOD_BRICK_ROWS; r++) {
        for (int c = 0; c < RPOD_BRICK_COLS; c++) {
            b.brick[r][c] = false;
        }
    }
    b.brick[RPOD_BRICK_ROWS - 1][3] = true;
    b.bricks_left = 1;
    rpod_brick_serve(&b);
    rpod_brick_rect_t cell = rpod_brick_cell(&b, RPOD_BRICK_ROWS - 1, 3);
    b.ball_x = cell.x + cell.w / 2.0f;
    b.vx = 0.0f;
    b.vy = -speed1;
    unsigned ev = run(1000);
    CHECK(ev & RPOD_BRICK_EV_CLEARED);
    CHECK(b.level == 2 && b.state == RPOD_BRICK_SERVE);
    CHECK(b.bricks_left == RPOD_BRICK_ROWS * RPOD_BRICK_COLS);
    CHECK(rpod_brick_speed(&b) > speed1);
}

int main(void)
{
    test_new_game_and_serve();
    test_hit();
    test_no_tunnelling();
    test_miss_and_game_over();
    test_clear();
    if (failures != 0) {
        fprintf(stderr, "test_brick: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_brick: ok\n");
    return 0;
}
