#include "brick.h"

#include <math.h>
#include <string.h>

/* The wall: bricks this tall with this much between them, its top row this
 * far below the top of the field -- room for a ball that breaks through to
 * rattle around up there. */
#define BRICK_H    10.0f
#define BRICK_GAP  2.0f
#define WALL_TOP   22.0f

#define PADDLE_W   52.0f
#define PADDLE_H   6.0f
#define PADDLE_LIFT 10.0f /* its bottom edge above the field's */
#define BALL_R     3.0f

/* Pixels a second on level 1; each level after is LEVEL_STEP faster, up to
 * MAX_FACTOR times, and within a level every brick broken adds BRICK_STEP. */
#define BASE_SPEED 150.0f
#define LEVEL_STEP 0.12f
#define MAX_FACTOR 2.0f
#define BRICK_STEP 0.005f

/* Off the paddle the ball leaves at up to this angle from straight up,
 * depending on where it lands -- the paddle's edges send it widest. A serve
 * goes no straighter than SERVE_MIN, toward the middle of the field. */
#define MAX_ANGLE  (60.0f * 3.14159265f / 180.0f)
#define SERVE_MAX  (35.0f * 3.14159265f / 180.0f)
#define SERVE_MIN  (12.0f * 3.14159265f / 180.0f)

/* A frame later than this (the screen was busy) is taken as this long, so
 * the ball doesn't jump. */
#define MAX_DT     0.1f

#define TOTAL_BRICKS (RPOD_BRICK_ROWS * RPOD_BRICK_COLS)

float rpod_brick_speed(const rpod_brick_t *b)
{
    float level = 1.0f + LEVEL_STEP * (float)(b->level - 1);
    if (level > MAX_FACTOR) {
        level = MAX_FACTOR;
    }
    float wall = 1.0f + BRICK_STEP * (float)(TOTAL_BRICKS - b->bricks_left);
    return BASE_SPEED * level * wall;
}

rpod_brick_rect_t rpod_brick_cell(const rpod_brick_t *b, int row, int col)
{
    float w = (b->w - BRICK_GAP * (RPOD_BRICK_COLS + 1)) / RPOD_BRICK_COLS;
    return (rpod_brick_rect_t){
        .x = BRICK_GAP + (float)col * (w + BRICK_GAP),
        .y = WALL_TOP + (float)row * (BRICK_H + BRICK_GAP),
        .w = w,
        .h = BRICK_H,
    };
}

static void build_wall(rpod_brick_t *b)
{
    for (int r = 0; r < RPOD_BRICK_ROWS; r++) {
        for (int c = 0; c < RPOD_BRICK_COLS; c++) {
            b->brick[r][c] = true;
        }
    }
    b->bricks_left = TOTAL_BRICKS;
}

/* The ball back on the paddle, waiting to be served. */
static void to_serve(rpod_brick_t *b)
{
    b->state = RPOD_BRICK_SERVE;
    b->vx = 0.0f;
    b->vy = 0.0f;
    b->ball_x = b->paddle_x + b->paddle_w / 2.0f;
    b->ball_y = b->paddle_y - b->ball_r;
}

void rpod_brick_new_game(rpod_brick_t *b, float w, float h)
{
    memset(b, 0, sizeof(*b));
    b->w = w;
    b->h = h;
    b->paddle_w = PADDLE_W;
    b->paddle_h = PADDLE_H;
    b->paddle_x = (w - PADDLE_W) / 2.0f;
    b->paddle_y = h - PADDLE_LIFT - PADDLE_H;
    b->ball_r = BALL_R;
    b->level = 1;
    b->balls = RPOD_BRICK_BALLS;
    build_wall(b);
    to_serve(b);
}

void rpod_brick_move_paddle(rpod_brick_t *b, float dx)
{
    if (b->state == RPOD_BRICK_OVER) {
        return;
    }
    b->paddle_x += dx;
    if (b->paddle_x < 0.0f) {
        b->paddle_x = 0.0f;
    } else if (b->paddle_x > b->w - b->paddle_w) {
        b->paddle_x = b->w - b->paddle_w;
    }
    if (b->state == RPOD_BRICK_SERVE) {
        b->ball_x = b->paddle_x + b->paddle_w / 2.0f;
    }
}

/* Sends the ball off at `angle` from straight up (positive: to the right). */
static void launch_at(rpod_brick_t *b, float angle)
{
    float s = rpod_brick_speed(b);
    b->vx = s * sinf(angle);
    b->vy = -s * cosf(angle);
}

void rpod_brick_serve(rpod_brick_t *b)
{
    if (b->state != RPOD_BRICK_SERVE) {
        return;
    }
    float off = (b->w / 2.0f - b->ball_x) / (b->w / 2.0f); /* toward the middle */
    float angle = off * SERVE_MAX;
    if (fabsf(angle) < SERVE_MIN) {
        angle = (angle < 0.0f || (angle == 0.0f && b->level % 2 == 0)) ? -SERVE_MIN : SERVE_MIN;
    }
    launch_at(b, angle);
    b->state = RPOD_BRICK_PLAY;
}

/* The same direction at the current speed: a broken brick sped it up. */
static void keep_speed(rpod_brick_t *b)
{
    float v = sqrtf(b->vx * b->vx + b->vy * b->vy);
    if (v > 0.0f) {
        float k = rpod_brick_speed(b) / v;
        b->vx *= k;
        b->vy *= k;
    }
}

static bool overlaps(const rpod_brick_t *b, rpod_brick_rect_t r)
{
    return b->ball_x + b->ball_r > r.x && b->ball_x - b->ball_r < r.x + r.w &&
           b->ball_y + b->ball_r > r.y && b->ball_y - b->ball_r < r.y + r.h;
}

/* The first brick the ball is into, knocked out: the ball turns back off
 * whichever face it went in by least. One a step -- steps are short. */
static unsigned hit_bricks(rpod_brick_t *b)
{
    for (int row = 0; row < RPOD_BRICK_ROWS; row++) {
        for (int col = 0; col < RPOD_BRICK_COLS; col++) {
            if (!b->brick[row][col]) {
                continue;
            }
            rpod_brick_rect_t r = rpod_brick_cell(b, row, col);
            if (!overlaps(b, r)) {
                continue;
            }
            float ox = fminf(b->ball_x + b->ball_r - r.x, r.x + r.w - (b->ball_x - b->ball_r));
            float oy = fminf(b->ball_y + b->ball_r - r.y, r.y + r.h - (b->ball_y - b->ball_r));
            if (ox < oy) {
                b->vx = b->ball_x < r.x + r.w / 2.0f ? -fabsf(b->vx) : fabsf(b->vx);
            } else {
                b->vy = b->ball_y < r.y + r.h / 2.0f ? -fabsf(b->vy) : fabsf(b->vy);
            }
            b->brick[row][col] = false;
            b->bricks_left--;
            b->score += (unsigned)((RPOD_BRICK_ROWS - row) * b->level);
            if (b->bricks_left == 0) {
                b->level++;
                build_wall(b);
                to_serve(b);
                return RPOD_BRICK_EV_HIT | RPOD_BRICK_EV_CLEARED;
            }
            keep_speed(b);
            return RPOD_BRICK_EV_HIT;
        }
    }
    return 0;
}

static unsigned substep(rpod_brick_t *b, float dt)
{
    b->ball_x += b->vx * dt;
    b->ball_y += b->vy * dt;

    if (b->ball_x - b->ball_r < 0.0f) {
        b->ball_x = b->ball_r;
        b->vx = fabsf(b->vx);
    } else if (b->ball_x + b->ball_r > b->w) {
        b->ball_x = b->w - b->ball_r;
        b->vx = -fabsf(b->vx);
    }
    if (b->ball_y - b->ball_r < 0.0f) {
        b->ball_y = b->ball_r;
        b->vy = fabsf(b->vy);
    }

    /* Coming down onto the paddle: off it at an angle set by where. */
    if (b->vy > 0.0f && b->ball_y + b->ball_r >= b->paddle_y &&
        b->ball_y - b->ball_r <= b->paddle_y + b->paddle_h &&
        b->ball_x + b->ball_r >= b->paddle_x && b->ball_x - b->ball_r <= b->paddle_x + b->paddle_w) {
        float off = (b->ball_x - (b->paddle_x + b->paddle_w / 2.0f)) / (b->paddle_w / 2.0f + b->ball_r);
        off = fmaxf(-1.0f, fminf(1.0f, off));
        launch_at(b, off * MAX_ANGLE);
        b->ball_y = b->paddle_y - b->ball_r;
        return RPOD_BRICK_EV_PADDLE;
    }

    if (b->ball_y - b->ball_r > b->h) {
        b->balls--;
        if (b->balls <= 0) {
            b->balls = 0;
            b->state = RPOD_BRICK_OVER;
            return RPOD_BRICK_EV_MISS | RPOD_BRICK_EV_OVER;
        }
        to_serve(b);
        return RPOD_BRICK_EV_MISS;
    }

    return hit_bricks(b);
}

unsigned rpod_brick_step(rpod_brick_t *b, unsigned dt_ms)
{
    if (b->state != RPOD_BRICK_PLAY) {
        return 0;
    }
    float dt = (float)dt_ms / 1000.0f;
    if (dt > MAX_DT) {
        dt = MAX_DT;
    }
    /* Half the ball's radius at most per step: never through a brick (10
     * high) or the paddle (6). */
    float dist = sqrtf(b->vx * b->vx + b->vy * b->vy) * dt;
    int n = (int)ceilf(dist / (b->ball_r / 2.0f));
    if (n < 1) {
        n = 1;
    }
    unsigned ev = 0;
    for (int i = 0; i < n && b->state == RPOD_BRICK_PLAY; i++) {
        ev |= substep(b, dt / (float)n);
    }
    return ev;
}
