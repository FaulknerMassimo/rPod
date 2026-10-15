/*
 * Brick (Extras > Games), the iPod's breakout: the rules and the physics,
 * in pixels of a field the screen gives it (ui/screens/brick_screen.c draws
 * it). Pure C: no LVGL, so it runs under the unit tests.
 *
 * The wheel slides the paddle, Center serves the ball. A brick is worth
 * more the higher its row, every level is faster than the last, and three
 * balls lost is the game.
 */

#ifndef RPOD_BRICK_H
#define RPOD_BRICK_H

#include <stdbool.h>

#define RPOD_BRICK_ROWS  5
#define RPOD_BRICK_COLS  8
#define RPOD_BRICK_BALLS 3

typedef enum {
    RPOD_BRICK_SERVE, /* the ball sits on the paddle until it's served */
    RPOD_BRICK_PLAY,
    RPOD_BRICK_OVER,  /* no balls left */
} rpod_brick_state_t;

/* What a step did (rpod_brick_step()'s flags). */
enum {
    RPOD_BRICK_EV_HIT = 1 << 0,     /* broke a brick */
    RPOD_BRICK_EV_PADDLE = 1 << 1,  /* came off the paddle */
    RPOD_BRICK_EV_MISS = 1 << 2,    /* a ball got past */
    RPOD_BRICK_EV_CLEARED = 1 << 3, /* the last brick went: the next level's set up */
    RPOD_BRICK_EV_OVER = 1 << 4,    /* that was the last ball */
};

typedef struct {
    float x, y, w, h;
} rpod_brick_rect_t;

typedef struct {
    float w, h; /* the field; (0, 0) is its top left */
    rpod_brick_state_t state;

    float paddle_x; /* left edge */
    float paddle_y; /* top edge */
    float paddle_w, paddle_h;

    float ball_x, ball_y; /* centre */
    float ball_r;
    float vx, vy;         /* pixels per second */

    bool brick[RPOD_BRICK_ROWS][RPOD_BRICK_COLS];
    int bricks_left;

    unsigned score;
    int level; /* from 1 */
    int balls; /* left, the one in play included */
} rpod_brick_t;

/* A new game on a field w x h pixels: level 1, every brick up, serving. */
void rpod_brick_new_game(rpod_brick_t *b, float w, float h);

/* Slides the paddle `dx` pixels, stopping at the walls. While serving the
 * ball rides along. */
void rpod_brick_move_paddle(rpod_brick_t *b, float dx);

/* Serves the ball, if it's waiting on the paddle. */
void rpod_brick_serve(rpod_brick_t *b);

/* Moves the ball on `dt_ms` -- in small enough steps that it can't pass
 * through a brick or the paddle however fast it's going -- and returns what
 * happened (RPOD_BRICK_EV_*). */
unsigned rpod_brick_step(rpod_brick_t *b, unsigned dt_ms);

/* Where brick (row, col) is, whether or not it's still up. Row 0 is the
 * top. */
rpod_brick_rect_t rpod_brick_cell(const rpod_brick_t *b, int row, int col);

/* The ball's speed on the current level, in pixels per second. */
float rpod_brick_speed(const rpod_brick_t *b);

#endif /* RPOD_BRICK_H */
