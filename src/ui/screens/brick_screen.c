#include "brick_screen.h"

#include "games/brick.h"
#include "games/scores.h"
#include "input/input.h"
#include "ui/classic.h"
#include "ui/metrics.h"
#include "ui/theme.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCORE_KEY "brick"

/* The field: the same size in every theme, along the bottom of the panel,
 * with the level, balls and score in whatever the status bar leaves above
 * it. */
#define FIELD_H 180

/* Pixels the paddle moves for each position the wheel turns: 96 a turn,
 * so a turn sweeps it across the field and some. */
#define PADDLE_STEP 4

#define FRAME_MS 30

/* Glass's wall: iOS's system colours, top row down. */
static const uint32_t k_glass_rows[RPOD_BRICK_ROWS] = { 0xff453a, 0xff9f0a, 0xffd60a, 0x30d158, 0x0a84ff };

typedef struct {
    rpod_brick_t b;
    bool paused;
    bool scored;   /* a finished game's score is in */
    bool new_best;
} brick_game_t;

typedef struct {
    brick_game_t *game;
    rpod_screen_stack_t *stack;
    lv_obj_t *screen;
    lv_obj_t *field;
    lv_obj_t *bricks[RPOD_BRICK_ROWS][RPOD_BRICK_COLS];
    lv_obj_t *paddle;
    lv_obj_t *ball;
    lv_obj_t *level_label;
    lv_obj_t *balls_label;
    lv_obj_t *score_label;
    lv_obj_t *msg_box;
    lv_obj_t *msg;
    lv_timer_t *timer;
    uint32_t last_ms;
} brick_view_t;

static lv_obj_t *block(lv_obj_t *parent, lv_color_t color, int32_t radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_color(o, color, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static void place(lv_obj_t *o, float x, float y, float w, float h)
{
    lv_obj_set_pos(o, (int32_t)lroundf(x), (int32_t)lroundf(y));
    lv_obj_set_size(o, (int32_t)lroundf(w), (int32_t)lroundf(h));
}

/* Only on a change: hiding an object invalidates it, hidden already or not,
 * and the game syncs every frame. */
static void show(lv_obj_t *o, bool shown)
{
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) != shown) {
        return;
    }
    if (shown) {
        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

/* The wall, the paddle, the ball and the numbers, as the game has them. */
static void sync(brick_view_t *v)
{
    const rpod_brick_t *b = &v->game->b;
    for (int r = 0; r < RPOD_BRICK_ROWS; r++) {
        for (int c = 0; c < RPOD_BRICK_COLS; c++) {
            show(v->bricks[r][c], b->brick[r][c]);
        }
    }
    lv_obj_set_pos(v->paddle, (int32_t)lroundf(b->paddle_x), (int32_t)lroundf(b->paddle_y));
    lv_obj_set_pos(v->ball, (int32_t)lroundf(b->ball_x - b->ball_r), (int32_t)lroundf(b->ball_y - b->ball_r));
    show(v->ball, b->state != RPOD_BRICK_OVER);

    char buf[64];
    snprintf(buf, sizeof(buf), "Level %d", b->level);
    set_text(v->level_label, buf);
    snprintf(buf, sizeof(buf), "%u", b->score);
    set_text(v->score_label, buf);
    buf[0] = '\0';
    for (int i = 0; i < b->balls && i < 8; i++) {
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), i > 0 ? " \xE2\x80\xA2" : "\xE2\x80\xA2");
    }
    set_text(v->balls_label, buf);

    /* What Center does next, while the ball isn't in play. */
    const char *msg = NULL;
    char over[96];
    if (b->state == RPOD_BRICK_OVER) {
        if (v->game->new_best) {
            snprintf(over, sizeof(over), "Game Over\nNew best: %u", b->score);
        } else {
            snprintf(over, sizeof(over), "Game Over\nBest: %u", rpod_score_best(SCORE_KEY));
        }
        msg = over;
    } else if (v->game->paused) {
        msg = "Paused";
    } else if (b->state == RPOD_BRICK_SERVE) {
        msg = b->level > 1 && b->bricks_left == RPOD_BRICK_ROWS * RPOD_BRICK_COLS ? "Next Level" : "Press Center";
    }
    if (msg != NULL) {
        set_text(v->msg, msg);
    }
    show(v->msg_box, msg != NULL);
}

/* Out of sight -- the screen dark, a card over it, another screen up. */
static bool covered(const brick_view_t *v)
{
    return rpod_screen_stack_overlay(v->stack) != NULL || lv_screen_active() != v->screen ||
           !lv_display_is_invalidation_enabled(lv_obj_get_display(v->screen));
}

static void frame_cb(lv_timer_t *t)
{
    brick_view_t *v = lv_timer_get_user_data(t);
    brick_game_t *g = v->game;
    uint32_t dt = lv_tick_elaps(v->last_ms);
    v->last_ms = lv_tick_get();

    bool hidden = covered(v);
    rpod_input_set_fine(!hidden);
    if (hidden) {
        if (g->b.state == RPOD_BRICK_PLAY && !g->paused) {
            g->paused = true;
            sync(v);
        }
        return;
    }
    if (g->paused || g->b.state != RPOD_BRICK_PLAY) {
        return;
    }
    unsigned ev = rpod_brick_step(&g->b, dt);
    if ((ev & RPOD_BRICK_EV_OVER) && !g->scored) {
        g->scored = true;
        g->new_best = rpod_score_submit(SCORE_KEY, g->b.score);
    }
    sync(v);
}

/* Center: serve, pause, play on -- or, once it's over, a new game. */
static void center(brick_view_t *v)
{
    brick_game_t *g = v->game;
    if (g->b.state == RPOD_BRICK_OVER) {
        rpod_brick_new_game(&g->b, g->b.w, g->b.h);
        g->scored = false;
        g->new_best = false;
    } else if (g->paused) {
        g->paused = false;
    } else if (g->b.state == RPOD_BRICK_SERVE) {
        rpod_brick_serve(&g->b);
    } else {
        g->paused = true;
    }
    v->last_ms = lv_tick_get(); /* no catching up on time spent paused */
    sync(v);
}

static void field_event_cb(lv_event_t *e)
{
    brick_view_t *v = lv_event_get_user_data(e);
    if (lv_event_get_code(e) == LV_EVENT_KEY) {
        uint32_t k = lv_event_get_key(e);
        int dir = (k == LV_KEY_RIGHT || k == LV_KEY_UP) ? 1 : (k == LV_KEY_LEFT || k == LV_KEY_DOWN) ? -1 : 0;
        if (dir != 0 && !v->game->paused) {
            rpod_brick_move_paddle(&v->game->b, (float)(dir * PADDLE_STEP));
            sync(v);
        }
    } else {
        center(v);
    }
}

static void view_delete_cb(lv_event_t *e)
{
    brick_view_t *v = lv_event_get_user_data(e);
    rpod_input_set_fine(false);
    lv_timer_delete(v->timer);
    free(v);
}

static lv_obj_t *hud_label(lv_obj_t *parent, lv_align_t align, int32_t x, int32_t y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, "");
    lv_obj_set_style_text_font(l, rpod_metrics()->font_small, 0);
    lv_obj_set_style_text_color(l, RPOD_COLOR_TEXT, 0);
    lv_obj_align(l, align, x, y);
    return l;
}

static void build_brick_screen(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    const rpod_metrics_t *m = rpod_metrics();
    bool classic = rpod_theme()->classic;
    brick_view_t *v = calloc(1, sizeof(*v));
    v->game = ctx;
    v->stack = stack;
    v->screen = screen;
    rpod_screen_stack_set_title(stack, "Brick");

    /* The numbers, centred in the band between the bar and the field. */
    int32_t hud_y = m->header_h + (m->screen_h - FIELD_H - m->header_h) / 2 -
                    lv_font_get_line_height(m->font_small) / 2;
    v->level_label = hud_label(screen, LV_ALIGN_TOP_LEFT, 10, hud_y);
    v->balls_label = hud_label(screen, LV_ALIGN_TOP_MID, 0, hud_y);
    v->score_label = hud_label(screen, LV_ALIGN_TOP_RIGHT, -10, hud_y);
    lv_obj_set_style_text_align(v->score_label, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *field = lv_obj_create(screen);
    lv_obj_remove_style_all(field);
    lv_obj_set_size(field, m->screen_w, FIELD_H);
    lv_obj_align(field, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_remove_flag(field, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    v->field = field;

    brick_game_t *g = v->game;
    if (g->b.w == 0.0f) {
        rpod_brick_new_game(&g->b, (float)m->screen_w, FIELD_H);
    }
    for (int r = 0; r < RPOD_BRICK_ROWS; r++) {
        lv_color_t color = classic ? (r % 2 == 0 ? RPOD_COLOR_TEXT : rpod_theme()->fill)
                                   : lv_color_hex(k_glass_rows[r]);
        for (int c = 0; c < RPOD_BRICK_COLS; c++) {
            rpod_brick_rect_t cell = rpod_brick_cell(&g->b, r, c);
            v->bricks[r][c] = block(field, color, classic ? 0 : 3);
            place(v->bricks[r][c], cell.x, cell.y, cell.w, cell.h);
        }
    }
    v->paddle = block(field, RPOD_COLOR_TEXT, classic ? 0 : 3);
    lv_obj_set_size(v->paddle, (int32_t)g->b.paddle_w, (int32_t)g->b.paddle_h);
    v->ball = block(field, RPOD_COLOR_TEXT, classic ? 0 : LV_RADIUS_CIRCLE);
    lv_obj_set_size(v->ball, (int32_t)(2 * g->b.ball_r), (int32_t)(2 * g->b.ball_r));

    /* Over the field, between the wall and the paddle. */
    v->msg_box = lv_obj_create(field);
    lv_obj_remove_style_all(v->msg_box);
    rpod_theme_style_glass_panel(v->msg_box, 12);
    lv_obj_set_size(v->msg_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(v->msg_box, 16, 0);
    lv_obj_set_style_pad_ver(v->msg_box, classic ? 2 * RPOD_CLASSIC_PX : 8, 0);
    lv_obj_remove_flag(v->msg_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(v->msg_box, LV_ALIGN_CENTER, 0, 30);
    v->msg = lv_label_create(v->msg_box);
    lv_label_set_text(v->msg, "");
    lv_obj_set_style_text_font(v->msg, m->font_body, 0);
    lv_obj_set_style_text_color(v->msg, RPOD_COLOR_TEXT, 0);
    lv_obj_set_style_text_align(v->msg, LV_TEXT_ALIGN_CENTER, 0);

    /* The field takes the wheel: in edit mode, so turning arrives as keys
     * rather than moving focus, and Center as a click (as Now Playing's
     * proxy does). */
    lv_obj_add_event_cb(field, field_event_cb, LV_EVENT_KEY, v);
    lv_obj_add_event_cb(field, field_event_cb, LV_EVENT_CLICKED, v);
    lv_obj_add_event_cb(field, view_delete_cb, LV_EVENT_DELETE, v);
    lv_group_t *group = lv_group_get_default();
    if (group != NULL) {
        lv_group_add_obj(group, field);
        lv_group_focus_obj(field);
        lv_group_set_editing(group, true);
    }

    sync(v);
    v->last_ms = lv_tick_get();
    v->timer = lv_timer_create(frame_cb, FRAME_MS, v);
    rpod_input_set_fine(true);
}

void rpod_brick_screen_push(rpod_screen_stack_t *stack)
{
    brick_game_t *g = calloc(1, sizeof(*g));
    if (g != NULL) {
        rpod_screen_stack_push(stack, build_brick_screen, g, free);
    }
}
