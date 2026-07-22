#include "screen_stack.h"

#include "ui/theme.h"

#include <stdlib.h>

#define RPOD_SCREEN_STACK_MAX 12

typedef struct {
    lv_obj_t *screen;
    lv_group_t *group;
    void *ctx;
    void (*ctx_free)(void *ctx);
} rpod_screen_frame_t;

struct rpod_screen_stack {
    lv_indev_t *indev;
    rpod_screen_frame_t frames[RPOD_SCREEN_STACK_MAX];
    size_t depth;
};

rpod_screen_stack_t *rpod_screen_stack_create(lv_indev_t *indev)
{
    rpod_screen_stack_t *stack = calloc(1, sizeof(*stack));
    if (stack != NULL) {
        stack->indev = indev;
    }
    return stack;
}

void rpod_screen_stack_push(rpod_screen_stack_t *stack, rpod_screen_build_fn build, void *ctx,
                             void (*ctx_free)(void *ctx))
{
    if (stack->depth >= RPOD_SCREEN_STACK_MAX) {
        if (ctx_free != NULL) {
            ctx_free(ctx);
        }
        return;
    }

    lv_group_t *group = lv_group_create();
    lv_group_set_default(group);

    lv_obj_t *screen = lv_obj_create(NULL);
    rpod_theme_style_screen(screen);

    build(stack, screen, ctx);

    lv_indev_set_group(stack->indev, group);
    lv_screen_load(screen);

    rpod_screen_frame_t *frame = &stack->frames[stack->depth];
    frame->screen = screen;
    frame->group = group;
    frame->ctx = ctx;
    frame->ctx_free = ctx_free;
    stack->depth++;
}

void rpod_screen_stack_pop(rpod_screen_stack_t *stack)
{
    if (stack->depth <= 1) {
        return;
    }

    rpod_screen_frame_t *top = &stack->frames[stack->depth - 1];
    rpod_screen_frame_t *prev = &stack->frames[stack->depth - 2];

    /* Group state goes live *before* the screen load, mirroring push()'s
     * own order -- lv_screen_load() synchronously fires LV_EVENT_SCREEN_LOADED
     * on prev->screen, and a handler reacting to that (e.g. rebuilding a
     * list's rows so a screen refreshes on every return visit, not just its
     * first push) needs the default group to already be `prev->group` so
     * any widgets it creates join it -- not the about-to-be-deleted
     * `top->group` below. */
    lv_indev_set_group(stack->indev, prev->group);
    lv_group_set_default(prev->group);
    lv_screen_load(prev->screen);

    if (top->ctx_free != NULL) {
        top->ctx_free(top->ctx);
    }
    lv_obj_delete(top->screen);
    lv_group_delete(top->group);

    stack->depth--;
}

size_t rpod_screen_stack_depth(const rpod_screen_stack_t *stack)
{
    return stack->depth;
}
