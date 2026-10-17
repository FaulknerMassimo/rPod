#include "music_quiz.h"

#include "list_screen.h"
#include "audio/scrobbler.h"
#include "games/quiz.h"
#include "games/scores.h"
#include "ui/classic.h"
#include "ui/metrics.h"
#include "ui/status_bar.h"
#include "ui/theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SCORE_KEY "quiz"

/* How long a round's answer shows before the next one starts. */
#define REVEAL_MS 1800

#define TICK_MS 50

/* Glass: the header over the choices, and the choices' padding -- tighter
 * than a list's usual, so all five fit under it without scrolling. */
#define GLASS_HEADER_H 30
#define GLASS_ROW_PAD_Y 5
#define GLASS_BAR_H 4

typedef enum {
    Q_ASK,    /* the song's playing: pick a title */
    Q_REVEAL, /* the answer's showing */
    Q_DONE,   /* the score, and Play Again */
    Q_NONE,   /* not enough of a library to quiz on */
} phase_t;

typedef struct {
    rpod_mpd_t *mpd;

    /* The library, as much of it as the quiz needs. */
    rpod_quiz_song_t *songs;
    char **uris;
    size_t count;
    uint32_t rng;

    /* What was playing before: put back when the quiz is done with it. */
    rpod_mpd_spot_t spot;
    bool borrowed;
    int extra_id; /* the round's song on the queue, -1 with none */

    phase_t phase;
    uint32_t phase_ms; /* when it began */
    int round;
    rpod_quiz_round_t cur;
    size_t used[RPOD_QUIZ_ROUNDS];
    int picked;        /* the choice taken, -1 if time ran out */
    unsigned points;   /* what this round scored */
    unsigned score;
    int right;
    bool new_best;
} quiz_t;

typedef struct quiz_view quiz_view_t;

typedef struct {
    quiz_view_t *v;
    int index;
} choice_ref_t;

struct quiz_view {
    quiz_t *q;
    rpod_screen_stack_t *stack;
    lv_obj_t *screen;
    lv_group_t *group;
    lv_timer_t *timer;
    uint32_t last_ms;
    lv_obj_t *bar;      /* the countdown: a classic slider, or glass's fill */
    int32_t bar_w;
    lv_obj_t *rows[RPOD_QUIZ_CHOICES];
    choice_ref_t refs[RPOD_QUIZ_CHOICES];
};

/* --- Playback ----------------------------------------------------------------- */

static void borrow(quiz_t *q)
{
    if (q->borrowed) {
        return;
    }
    if (!rpod_mpd_get_spot(q->mpd, &q->spot)) {
        q->spot.song_id = -1;
        q->spot.state = RPOD_MPD_STATE_STOP;
    }
    q->borrowed = true;
    q->extra_id = -1;
    rpod_scrobbler_suspend(true);
}

/* Back where it was -- unless something else has taken over playback since
 * (Next, say, or the alarm), which then stands. */
static void give_back(quiz_t *q)
{
    if (!q->borrowed) {
        return;
    }
    rpod_mpd_spot_t now;
    if (q->extra_id >= 0 && rpod_mpd_get_spot(q->mpd, &now) && now.song_id == q->extra_id) {
        rpod_mpd_restore_spot(q->mpd, &q->spot);
    }
    if (q->extra_id >= 0) {
        rpod_mpd_delete_id(q->mpd, q->extra_id);
        q->extra_id = -1;
    }
    rpod_scrobbler_suspend(false);
    q->borrowed = false;
}

/* --- The game ----------------------------------------------------------------- */

static void finish(quiz_t *q)
{
    q->phase = Q_DONE;
    q->phase_ms = lv_tick_get();
    give_back(q);
    q->new_best = rpod_score_submit(SCORE_KEY, q->score);
}

static void start_round(quiz_t *q)
{
    if (!rpod_quiz_pick(q->songs, q->count, q->used, (size_t)q->round, &q->rng, &q->cur)) {
        q->phase = Q_NONE;
        give_back(q);
        return;
    }
    q->used[q->round] = q->cur.answer;
    int old = q->extra_id;
    int id = -1;
    if (!rpod_mpd_play_extra(q->mpd, q->uris[q->cur.answer], q->cur.start_ms, &id)) {
        fprintf(stderr, "rpod: music quiz couldn't play %s\n", q->uris[q->cur.answer]);
    }
    q->extra_id = id;
    if (old >= 0 && old != id) {
        rpod_mpd_delete_id(q->mpd, old);
    }
    q->phase = Q_ASK;
    q->phase_ms = lv_tick_get();
    q->picked = -1;
    q->points = 0;
}

static void new_game(quiz_t *q)
{
    q->round = 0;
    q->score = 0;
    q->right = 0;
    q->new_best = false;
    borrow(q);
    start_round(q);
}

static unsigned time_left(const quiz_t *q)
{
    uint32_t el = lv_tick_elaps(q->phase_ms);
    return el >= RPOD_QUIZ_ROUND_MS ? 0 : RPOD_QUIZ_ROUND_MS - el;
}

static void answer(quiz_t *q, int picked)
{
    q->picked = picked;
    q->points = picked == q->cur.correct ? rpod_quiz_points(time_left(q)) : 0;
    if (q->points > 0) {
        q->score += q->points;
        q->right++;
    }
    q->phase = Q_REVEAL;
    q->phase_ms = lv_tick_get();
}

static void next_round(quiz_t *q)
{
    q->round++;
    if (q->round >= RPOD_QUIZ_ROUNDS) {
        finish(q);
    } else {
        start_round(q);
    }
}

/* --- On screen ---------------------------------------------------------------- */

static void render(quiz_view_t *v);

static void on_choice(rpod_screen_stack_t *stack, void *item_ctx)
{
    (void)stack;
    choice_ref_t *ref = item_ctx;
    quiz_view_t *v = ref->v;
    if (v->q->phase == Q_ASK) {
        answer(v->q, ref->index);
        render(v); /* rebuilds this row: nothing of it after */
    }
}

static void on_play_again(rpod_screen_stack_t *stack, void *item_ctx)
{
    (void)stack;
    quiz_view_t *v = item_ctx;
    new_game(v->q);
    render(v);
}

static void show_countdown(quiz_view_t *v)
{
    if (v->bar == NULL) {
        return;
    }
    unsigned left = v->q->phase == Q_ASK ? time_left(v->q) : 0;
    if (rpod_theme()->classic) {
        rpod_classic_slider_set(v->bar, (int32_t)left, RPOD_QUIZ_ROUND_MS);
    } else {
        lv_obj_set_width(v->bar, (int32_t)((int64_t)v->bar_w * left / RPOD_QUIZ_ROUND_MS));
    }
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    return l;
}

/* A block above the rows, not one of them. */
static lv_obj_t *header_block(lv_obj_t *list, int32_t h)
{
    lv_obj_t *block = lv_obj_create(list);
    lv_obj_remove_style_all(block);
    lv_obj_set_size(block, LV_PCT(100), h);
    lv_obj_remove_flag(block, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return block;
}

/* Round and score along the top, the time left under them -- or, once
 * answered, how it went. */
static void build_round_header(quiz_view_t *v, lv_obj_t *list)
{
    const rpod_metrics_t *m = rpod_metrics();
    const quiz_t *q = v->q;
    bool classic = rpod_theme()->classic;
    int32_t pad = m->row_pad_x;
    lv_obj_t *block = header_block(list, classic ? m->row_h : GLASS_HEADER_H);

    char left[48];
    if (q->phase == Q_REVEAL) {
        if (q->picked == q->cur.correct) {
            snprintf(left, sizeof(left), "Correct! +%u", q->points);
        } else {
            snprintf(left, sizeof(left), "%s", q->picked < 0 ? "Time's up" : "Wrong");
        }
    } else {
        snprintf(left, sizeof(left), "%d of %d", q->round + 1, RPOD_QUIZ_ROUNDS);
    }
    lv_obj_t *l = label(block, m->font_small, RPOD_COLOR_TEXT, left);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, pad, classic ? 0 : 2);
    char score[16];
    snprintf(score, sizeof(score), "%u", q->score);
    lv_obj_t *s = label(block, m->font_small, RPOD_COLOR_TEXT, score);
    lv_obj_align(s, LV_ALIGN_TOP_RIGHT, -pad, classic ? 0 : 2);

    v->bar_w = m->screen_w - 2 * m->list_margin - 2 * pad;
    if (classic) {
        int32_t w = (m->screen_w - 2 * pad) / RPOD_CLASSIC_PX * RPOD_CLASSIC_PX;
        v->bar = rpod_classic_slider_create(block, w, 4 * RPOD_CLASSIC_PX);
        lv_obj_align(v->bar, LV_ALIGN_BOTTOM_LEFT, pad, -RPOD_CLASSIC_PX);
    } else {
        lv_obj_t *track = lv_obj_create(block);
        lv_obj_remove_style_all(track);
        lv_obj_set_size(track, v->bar_w, GLASS_BAR_H);
        lv_obj_set_style_radius(track, GLASS_BAR_H / 2, 0);
        lv_obj_set_style_bg_color(track, RPOD_COLOR_SEPARATOR, 0);
        lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
        lv_obj_remove_flag(track, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(track, LV_ALIGN_BOTTOM_LEFT, pad, -3);
        v->bar = lv_obj_create(track);
        lv_obj_remove_style_all(v->bar);
        lv_obj_set_height(v->bar, GLASS_BAR_H);
        lv_obj_set_style_radius(v->bar, GLASS_BAR_H / 2, 0);
        lv_obj_set_style_bg_color(v->bar, RPOD_COLOR_ACCENT, 0);
        lv_obj_set_style_bg_opa(v->bar, LV_OPA_COVER, 0);
        lv_obj_remove_flag(v->bar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
    show_countdown(v);
}

static void build_choices(quiz_view_t *v, lv_obj_t *list, int focus)
{
    const quiz_t *q = v->q;
    rpod_list_item_t items[RPOD_QUIZ_CHOICES] = { 0 };
    for (int i = 0; i < RPOD_QUIZ_CHOICES; i++) {
        snprintf(items[i].text, sizeof(items[i].text), "%s", q->songs[q->cur.choice[i]].title);
        if (q->phase == Q_REVEAL && i == q->cur.correct) {
            items[i].status = RPOD_ROW_STATUS_CHECK;
        }
        v->refs[i] = (choice_ref_t){ .v = v, .index = i };
        items[i].on_select = on_choice;
        items[i].item_ctx = &v->refs[i];
    }
    rpod_list_screen_populate(v->stack, list, items, RPOD_QUIZ_CHOICES);

    /* The rows are the list's last children, after the header. */
    uint32_t n = lv_obj_get_child_count(list);
    for (int i = 0; i < RPOD_QUIZ_CHOICES; i++) {
        v->rows[i] = lv_obj_get_child(list, (int32_t)(n - RPOD_QUIZ_CHOICES + (uint32_t)i));
        if (!rpod_theme()->classic) {
            lv_obj_set_style_pad_ver(v->rows[i], GLASS_ROW_PAD_Y, 0);
        }
    }
    if (focus >= 0 && focus < RPOD_QUIZ_CHOICES) {
        lv_group_focus_obj(v->rows[focus]);
    }
}

static void build_done(quiz_view_t *v, lv_obj_t *list)
{
    const rpod_metrics_t *m = rpod_metrics();
    const quiz_t *q = v->q;
    lv_obj_t *block = header_block(list, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(block, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(block, 12, 0);
    lv_obj_set_style_pad_row(block, 4, 0);

    /* The iPod's own wording. */
    char buf[64];
    snprintf(buf, sizeof(buf), "Score: %u (%d/%d)", q->score, q->right, RPOD_QUIZ_ROUNDS);
    label(block, m->font_title, RPOD_COLOR_TEXT, buf);
    if (q->new_best) {
        snprintf(buf, sizeof(buf), "New best!");
    } else {
        snprintf(buf, sizeof(buf), "Best: %u", rpod_score_best(SCORE_KEY));
    }
    label(block, m->font_small, RPOD_COLOR_DIM_TEXT, buf);

    rpod_list_item_t again = { .on_select = on_play_again, .item_ctx = v };
    snprintf(again.text, sizeof(again.text), "Play Again");
    rpod_list_screen_populate(v->stack, list, &again, 1);
}

static void render(quiz_view_t *v)
{
    /* Which choice the wheel is on, to keep it there through the reveal. */
    int focus = 0;
    lv_obj_t *focused = lv_group_get_focused(v->group);
    for (int i = 0; i < RPOD_QUIZ_CHOICES; i++) {
        if (v->rows[i] != NULL && v->rows[i] == focused) {
            focus = i;
        }
    }
    if (v->q->phase == Q_ASK) {
        focus = 0; /* a new round */
    }

    /* This may run from the timer under a card (the alarm's), whose group is
     * the default one -- the rows belong in this screen's. */
    lv_group_t *prev = lv_group_get_default();
    lv_group_set_default(v->group);
    lv_obj_clean(v->screen);
    v->bar = NULL;
    for (int i = 0; i < RPOD_QUIZ_CHOICES; i++) {
        v->rows[i] = NULL;
    }

    if (v->q->phase == Q_NONE) {
        const rpod_metrics_t *m = rpod_metrics();
        lv_obj_t *l = label(v->screen, m->font_body, RPOD_COLOR_DIM_TEXT,
                            "Music Quiz needs songs with at least five different titles.");
        lv_obj_set_width(l, m->screen_w - 40);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(l, LV_ALIGN_CENTER, 0, rpod_metrics_mid_dy(m));
    } else {
        lv_obj_t *list = rpod_list_screen_create(v->screen);
        if (v->q->phase == Q_DONE) {
            build_done(v, list);
        } else {
            build_round_header(v, list);
            build_choices(v, list, focus);
        }
    }
    lv_group_set_default(prev);
}

/* Out of sight -- the screen dark, a card over it: the clock stops. */
static bool covered(const quiz_view_t *v)
{
    return rpod_screen_stack_overlay(v->stack) != NULL || lv_screen_active() != v->screen ||
           !lv_display_is_invalidation_enabled(lv_obj_get_display(v->screen));
}

static void tick_cb(lv_timer_t *t)
{
    quiz_view_t *v = lv_timer_get_user_data(t);
    quiz_t *q = v->q;
    uint32_t dt = lv_tick_elaps(v->last_ms);
    v->last_ms = lv_tick_get();
    if (covered(v)) {
        q->phase_ms += dt;
        return;
    }
    if (q->phase == Q_ASK) {
        if (time_left(q) == 0) {
            answer(q, -1);
            render(v);
        } else {
            show_countdown(v);
        }
    } else if (q->phase == Q_REVEAL && lv_tick_elaps(q->phase_ms) >= REVEAL_MS) {
        next_round(q);
        render(v);
    }
}

static void view_delete_cb(lv_event_t *e)
{
    quiz_view_t *v = lv_event_get_user_data(e);
    rpod_status_bar_set_fixed_title(NULL);
    lv_timer_delete(v->timer);
    free(v);
}

static void build_quiz_screen(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    quiz_view_t *v = calloc(1, sizeof(*v));
    v->q = ctx;
    v->stack = stack;
    v->screen = screen;
    v->group = lv_group_get_default();
    rpod_screen_stack_set_title(stack, "Music Quiz");
    /* The bar would otherwise show the song's title: the answer. */
    rpod_status_bar_set_fixed_title("Music Quiz");

    render(v);
    v->last_ms = lv_tick_get();
    v->timer = lv_timer_create(tick_cb, TICK_MS, v);
    lv_obj_add_event_cb(screen, view_delete_cb, LV_EVENT_DELETE, v);
}

static void quiz_free(void *ctx)
{
    quiz_t *q = ctx;
    give_back(q);
    for (size_t i = 0; i < q->count; i++) {
        free((char *)q->songs[i].title);
        free(q->uris[i]);
    }
    free(q->songs);
    free(q->uris);
    free(q);
}

void rpod_music_quiz_push(rpod_screen_stack_t *stack, rpod_mpd_t *mpd)
{
    quiz_t *q = calloc(1, sizeof(*q));
    if (q == NULL) {
        return;
    }
    q->mpd = mpd;
    q->extra_id = -1;
    q->rng = (uint32_t)time(NULL) ^ (lv_tick_get() << 16) ^ 0x5bd1e995u;

    /* Just the titles, lengths and URIs: the full song records are big,
     * and a library can be thousands of them. */
    rpod_mpd_song_t *all = NULL;
    size_t n = 0;
    if (rpod_mpd_list_songs(mpd, NULL, NULL, &all, &n) && n > 0) {
        q->songs = calloc(n, sizeof(*q->songs));
        q->uris = calloc(n, sizeof(*q->uris));
        for (size_t i = 0; q->songs != NULL && q->uris != NULL && i < n; i++) {
            char *title = strdup(all[i].title);
            char *uri = strdup(all[i].uri);
            if (title == NULL || uri == NULL) {
                free(title);
                free(uri);
                break;
            }
            q->songs[q->count] = (rpod_quiz_song_t){ .title = title, .duration_s = all[i].duration_s };
            q->uris[q->count] = uri;
            q->count++;
        }
    }
    rpod_mpd_free_songs(all);

    new_game(q);
    rpod_screen_stack_push(stack, build_quiz_screen, q, quiz_free);
}
