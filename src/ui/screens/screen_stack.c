#include "screen_stack.h"

#include "ui/cover_cache.h"
#include "ui/metrics.h"
#include "ui/status_bar.h"
#include "ui/theme.h"

#include <stdio.h>
#include <stdlib.h>

#define RPOD_SCREEN_STACK_MAX 12
#define RPOD_TITLE_MAX        64
#define RPOD_DEFAULT_TITLE    "rPod"

/* How long a push may wait for the new screen's covers to come off disk
 * (ui/cover_cache.h) before showing it: normally a few milliseconds, and
 * this bounds it when the SD card is slow (a big copy onto it, say). */
#define RPOD_COVER_SETTLE_MS 40

typedef struct {
    lv_obj_t *screen;
    lv_group_t *group;
    rpod_screen_build_fn build; /* kept to rebuild it (rpod_screen_stack_rebuild) */
    void *ctx;
    void (*ctx_free)(void *ctx);
    char title[RPOD_TITLE_MAX];
} rpod_screen_frame_t;

struct rpod_screen_stack {
    lv_indev_t *indev;
    rpod_screen_frame_t frames[RPOD_SCREEN_STACK_MAX];
    size_t depth;
    bool locked; /* rpod_screen_stack_set_locked() */

    /* At most one open overlay (see rpod_screen_stack_open_overlay) --
     * overlay_root is NULL when none is open. */
    lv_obj_t *overlay_root;
    lv_group_t *overlay_group;

    /* Titles: the one the next push takes, and -- while a build fn runs --
     * the one the screen it's building gets. */
    char next_title[RPOD_TITLE_MAX];
    char build_title[RPOD_TITLE_MAX];
    bool building;
};

rpod_screen_stack_t *rpod_screen_stack_create(lv_indev_t *indev)
{
    rpod_screen_stack_t *stack = calloc(1, sizeof(*stack));
    if (stack != NULL) {
        stack->indev = indev;
    }
    return stack;
}

/* A screen's (or overlay's) input group. LVGL groups wrap around by
 * default; an iPod list stops at its ends instead -- and with scroll
 * acceleration, a flick that overshoots the top would otherwise land at the
 * bottom of the list. */
static lv_group_t *new_group(void)
{
    lv_group_t *group = lv_group_create();
    lv_group_set_wrap(group, false);
    lv_group_set_default(group);
    return group;
}

/* The top screen's title to the status bar, which shows it in the classic
 * themes. */
static void show_title(const rpod_screen_stack_t *stack)
{
    const char *title = stack->depth > 0 ? stack->frames[stack->depth - 1].title : "";
    rpod_status_bar_set_screen_title(title[0] != '\0' ? title : RPOD_DEFAULT_TITLE);
}

/* A new screen built by `build`, titled `title` unless the build names it
 * itself -- which `title` (RPOD_TITLE_MAX) comes back holding. The group it
 * fills must be the default already. */
static lv_obj_t *build_screen(rpod_screen_stack_t *stack, rpod_screen_build_fn build, void *ctx,
                              char *title)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    rpod_theme_style_screen(screen);

    snprintf(stack->build_title, sizeof(stack->build_title), "%s", title);
    stack->building = true;
    build(stack, screen, ctx);
    stack->building = false;
    snprintf(title, RPOD_TITLE_MAX, "%s", stack->build_title);
    return screen;
}

void rpod_screen_stack_push(rpod_screen_stack_t *stack, rpod_screen_build_fn build, void *ctx,
                             void (*ctx_free)(void *ctx))
{
    if (stack->depth >= RPOD_SCREEN_STACK_MAX) {
        stack->next_title[0] = '\0';
        if (ctx_free != NULL) {
            ctx_free(ctx);
        }
        return;
    }

    rpod_screen_frame_t *frame = &stack->frames[stack->depth];
    snprintf(frame->title, sizeof(frame->title), "%s", stack->next_title);
    stack->next_title[0] = '\0';

    lv_group_t *group = new_group();
    lv_obj_t *screen = build_screen(stack, build, ctx, frame->title);
    rpod_cover_cache_settle(RPOD_COVER_SETTLE_MS);

    lv_indev_set_group(stack->indev, group);
    lv_screen_load(screen);

    frame->screen = screen;
    frame->group = group;
    frame->build = build;
    frame->ctx = ctx;
    frame->ctx_free = ctx_free;
    stack->depth++;
    show_title(stack);
}

void rpod_screen_stack_pop(rpod_screen_stack_t *stack)
{
    if (stack->depth <= 1 || stack->locked) {
        return;
    }

    rpod_screen_frame_t *top = &stack->frames[stack->depth - 1];
    rpod_screen_frame_t *prev = &stack->frames[stack->depth - 2];
    /* A row that popped (Forget This Device) doesn't name a later push --
     * and one that pops and pushes the same screen back to refresh it
     * names that screen itself. */
    stack->next_title[0] = '\0';

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
    show_title(stack);
}

size_t rpod_screen_stack_depth(const rpod_screen_stack_t *stack)
{
    return stack->depth;
}

void rpod_screen_stack_set_locked(rpod_screen_stack_t *stack, bool locked)
{
    stack->locked = locked;
}

bool rpod_screen_stack_locked(const rpod_screen_stack_t *stack)
{
    return stack->locked;
}

void rpod_screen_stack_open_overlay(rpod_screen_stack_t *stack, rpod_screen_build_fn build, void *ctx)
{
    if (stack->overlay_root != NULL) {
        return;
    }

    lv_group_t *group = new_group();

    const rpod_metrics_t *m = rpod_metrics();
    lv_obj_t *root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, m->screen_w, m->screen_h);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    /* What a classic screen's labels inherit from the screen (see
     * rpod_theme_style_screen()); the top layer has no theme of its own. */
    if (rpod_theme()->classic) {
        lv_obj_set_style_text_color(root, RPOD_COLOR_TEXT, 0);
        lv_obj_set_style_text_font(root, m->font_body, 0);
    }

    build(stack, root, ctx);

    lv_indev_set_group(stack->indev, group);

    stack->overlay_root = root;
    stack->overlay_group = group;
}

bool rpod_screen_stack_close_overlay(rpod_screen_stack_t *stack)
{
    if (stack->overlay_root == NULL) {
        return false;
    }

    rpod_screen_frame_t *cur = &stack->frames[stack->depth - 1];
    lv_indev_set_group(stack->indev, cur->group);
    lv_group_set_default(cur->group);

    lv_obj_delete(stack->overlay_root);
    lv_group_delete(stack->overlay_group);
    stack->overlay_root = NULL;
    stack->overlay_group = NULL;
    return true;
}

lv_obj_t *rpod_screen_stack_overlay(const rpod_screen_stack_t *stack)
{
    return stack->overlay_root;
}

void rpod_screen_stack_next_title(rpod_screen_stack_t *stack, const char *title)
{
    snprintf(stack->next_title, sizeof(stack->next_title), "%s", title != NULL ? title : "");
}

void rpod_screen_stack_set_title(rpod_screen_stack_t *stack, const char *title)
{
    if (stack->building) {
        snprintf(stack->build_title, sizeof(stack->build_title), "%s", title);
    } else if (stack->depth > 0) {
        rpod_screen_frame_t *top = &stack->frames[stack->depth - 1];
        snprintf(top->title, sizeof(top->title), "%s", title);
        show_title(stack);
    }
}

const char *rpod_screen_stack_title(const rpod_screen_stack_t *stack)
{
    if (stack->building) {
        return stack->build_title;
    }
    return stack->depth > 0 ? stack->frames[stack->depth - 1].title : "";
}

/* Where `group`'s focus is, by position, or -1. */
static int32_t focused_index(lv_group_t *group)
{
    lv_obj_t *focused = lv_group_get_focused(group);
    uint32_t n = lv_group_get_obj_count(group);
    for (uint32_t i = 0; focused != NULL && i < n; i++) {
        if (lv_group_get_obj_by_index(group, i) == focused) {
            return (int32_t)i;
        }
    }
    return -1;
}

void rpod_screen_stack_rebuild(rpod_screen_stack_t *stack)
{
    rpod_screen_stack_close_overlay(stack);
    if (stack->depth == 0) {
        return;
    }

    /* Something has to be loaded while the old screens go: a blank one in
     * the new theme's colours, gone again before it's ever drawn. */
    lv_obj_t *blank = lv_obj_create(NULL);
    rpod_theme_style_screen(blank);
    lv_screen_load(blank);

    for (size_t i = 0; i < stack->depth; i++) {
        rpod_screen_frame_t *f = &stack->frames[i];
        int32_t focus = focused_index(f->group);

        /* The old screen goes first: its delete handlers (Now Playing's,
         * say) undo state the new build is about to set up again. */
        lv_obj_delete(f->screen);
        lv_group_remove_all_objs(f->group);
        lv_group_set_editing(f->group, false);
        lv_group_set_default(f->group);
        f->screen = build_screen(stack, f->build, f->ctx, f->title);

        if (focus > 0 && (uint32_t)focus < lv_group_get_obj_count(f->group)) {
            lv_group_focus_obj(lv_group_get_obj_by_index(f->group, (uint32_t)focus));
        }
    }

    rpod_screen_frame_t *top = &stack->frames[stack->depth - 1];
    lv_indev_set_group(stack->indev, top->group);
    lv_group_set_default(top->group);
    lv_screen_load(top->screen);
    lv_obj_delete(blank);
    show_title(stack);
}
