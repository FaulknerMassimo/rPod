#include "extras_screens.h"

#include "brick_screen.h"
#include "clock_screens.h"
#include "list_screen.h"
#include "music_quiz.h"
#include "screen_lock.h"

static void on_clock(rpod_screen_stack_t *stack, void *item_ctx)
{
    rpod_screen_stack_push(stack, rpod_clock_screen_build, item_ctx, NULL);
}

static void on_brick(rpod_screen_stack_t *stack, void *item_ctx)
{
    (void)item_ctx;
    rpod_brick_screen_push(stack);
}

static void on_music_quiz(rpod_screen_stack_t *stack, void *item_ctx)
{
    rpod_music_quiz_push(stack, item_ctx);
}

static void build_games_screen(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    rpod_list_item_t items[] = {
        { .text = "Brick",      .chevron = true, .on_select = on_brick,      .item_ctx = ctx },
        { .text = "Music Quiz", .chevron = true, .on_select = on_music_quiz, .item_ctx = ctx },
    };
    rpod_list_screen_build(stack, screen, items, sizeof(items) / sizeof(items[0]));
}

static void on_games(rpod_screen_stack_t *stack, void *item_ctx)
{
    rpod_screen_stack_push(stack, build_games_screen, item_ctx, NULL);
}

static void on_screen_lock(rpod_screen_stack_t *stack, void *item_ctx)
{
    (void)item_ctx;
    rpod_screen_stack_push(stack, rpod_screen_lock_menu_build, NULL, NULL);
}

void rpod_extras_menu_build(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    rpod_list_item_t items[] = {
        { .text = "Clock",       .chevron = true, .on_select = on_clock,       .item_ctx = ctx },
        { .text = "Games",       .chevron = true, .on_select = on_games,       .item_ctx = ctx },
        { .text = "Screen Lock", .chevron = true, .on_select = on_screen_lock, .item_ctx = NULL },
    };
    rpod_list_screen_build(stack, screen, items, sizeof(items) / sizeof(items[0]));
}
