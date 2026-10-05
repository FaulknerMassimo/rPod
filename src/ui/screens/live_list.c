#include "live_list.h"

#include "ui/metrics.h"
#include "ui/theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One row of the current build. `ref` is its own allocation, so the pointer
 * handed to the list as item_ctx stays put while more rows are added. */
typedef struct {
    rpod_live_row_t *ref;
    lv_obj_t *btn; /* NULL until its batch is added to the list */
} row_t;

struct rpod_live_list {
    rpod_screen_stack_t *stack;
    lv_obj_t *screen;
    lv_group_t *group;
    rpod_live_fill_fn fill;
    void *ctx;
    void (*ctx_free)(void *ctx);
    bool stale; /* changed while another screen was on top */
    bool pin_top; /* rpod_live_list_reset_focus() and the wheel hasn't moved since */

    lv_obj_t *list; /* NULL for a message-only screen */
    row_t *rows;
    size_t nrows, rowcap;

    /* Rows added since the last header/section, not yet in `list`: they go
     * in as one batch so separators fall between them and not after. */
    rpod_list_item_t *batch;
    size_t nbatch, batchcap;
};

static void ensure_list(rpod_live_list_t *ll)
{
    if (ll->list == NULL) {
        ll->list = rpod_list_screen_create(ll->screen);
    }
}

static void flush_batch(rpod_live_list_t *ll)
{
    if (ll->nbatch == 0) {
        return;
    }
    rpod_list_screen_populate(ll->stack, ll->list, ll->batch, ll->nbatch);
    /* The batch's buttons are now the list's last children, in order. */
    uint32_t count = lv_obj_get_child_count(ll->list);
    size_t first = ll->nrows - ll->nbatch;
    for (size_t i = 0; i < ll->nbatch; i++) {
        ll->rows[first + i].btn = lv_obj_get_child(ll->list, (int32_t)(count - ll->nbatch + i));
    }
    ll->nbatch = 0;
}

static void free_rows(rpod_live_list_t *ll)
{
    for (size_t i = 0; i < ll->nrows; i++) {
        free(ll->rows[i].ref);
    }
    ll->nrows = 0;
    ll->nbatch = 0;
}

static void refresh(rpod_live_list_t *ll)
{
    /* Which row has the highlight now, by key and by position. */
    char focus_key[sizeof(((rpod_live_row_t *)0)->key)] = "";
    size_t focus_idx = 0;
    lv_obj_t *focused = lv_group_get_focused(ll->group);
    for (size_t i = 0; focused != NULL && i < ll->nrows; i++) {
        if (ll->rows[i].btn == focused) {
            focus_idx = i;
            memcpy(focus_key, ll->rows[i].ref->key, sizeof(focus_key));
            break;
        }
    }
    if (focus_idx != 0) {
        ll->pin_top = false; /* moved off the top */
    }

    /* This may run while another screen (or an overlay) is on top, whose
     * group is the default one -- new rows must join this screen's. */
    lv_group_t *prev_default = lv_group_get_default();
    lv_group_set_default(ll->group);

    lv_obj_clean(ll->screen);
    free_rows(ll);
    ll->list = NULL;
    ll->fill(ll);
    flush_batch(ll);

    lv_group_set_default(prev_default);
    ll->stale = false;

    /* Same row if it's still there, else whatever took its place. Left
     * alone, the group has focused the first row. */
    if (ll->nrows == 0 || focus_key[0] == '\0' || ll->pin_top) {
        return;
    }
    size_t target = focus_idx < ll->nrows ? focus_idx : ll->nrows - 1;
    for (size_t i = 0; i < ll->nrows; i++) {
        if (strcmp(ll->rows[i].ref->key, focus_key) == 0) {
            target = i;
            break;
        }
    }
    if (ll->rows[target].btn != NULL) {
        lv_group_focus_obj(ll->rows[target].btn);
    }
}

/* Only the visible screen rebuilds now; one underneath catches up when it's
 * shown again (loaded_cb). */
void rpod_live_list_changed(void *user)
{
    rpod_live_list_t *ll = user;
    if (lv_screen_active() == ll->screen) {
        refresh(ll);
    } else {
        ll->stale = true;
    }
}

static void loaded_cb(lv_event_t *e)
{
    rpod_live_list_t *ll = lv_event_get_user_data(e);
    if (ll->stale) {
        refresh(ll);
    }
}

static void delete_cb(lv_event_t *e)
{
    rpod_live_list_t *ll = lv_event_get_user_data(e);
    if (ll->ctx_free != NULL) {
        ll->ctx_free(ll->ctx);
    }
    free_rows(ll);
    free(ll->rows);
    free(ll->batch);
    free(ll);
}

rpod_live_list_t *rpod_live_list_create(rpod_screen_stack_t *stack, lv_obj_t *screen,
                                        rpod_live_fill_fn fill, void *ctx,
                                        void (*ctx_free)(void *ctx))
{
    rpod_live_list_t *ll = calloc(1, sizeof(*ll));
    if (ll == NULL) {
        if (ctx_free != NULL) {
            ctx_free(ctx);
        }
        return NULL;
    }
    ll->stack = stack;
    ll->screen = screen;
    ll->group = lv_group_get_default(); /* the stack made it this screen's */
    ll->fill = fill;
    ll->ctx = ctx;
    ll->ctx_free = ctx_free;
    lv_obj_add_event_cb(screen, delete_cb, LV_EVENT_DELETE, ll);
    lv_obj_add_event_cb(screen, loaded_cb, LV_EVENT_SCREEN_LOADED, ll);
    refresh(ll);
    return ll;
}

void *rpod_live_list_ctx(const rpod_live_list_t *ll)
{
    return ll->ctx;
}

lv_obj_t *rpod_live_list_screen(const rpod_live_list_t *ll)
{
    return ll->screen;
}

void rpod_live_list_message(rpod_live_list_t *ll, const char *title, const char *msg)
{
    const rpod_metrics_t *m = rpod_metrics();
    lv_obj_t *label = lv_label_create(ll->screen);
    lv_label_set_text_fmt(label, "%s\n\n%s", title, msg);
    lv_obj_set_style_text_color(label, RPOD_COLOR_DIM_TEXT, 0);
    lv_obj_set_width(label, m->screen_w - 40);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, m->header_h / 2);
}

static lv_obj_t *add_block(rpod_live_list_t *ll)
{
    const rpod_metrics_t *m = rpod_metrics();
    ensure_list(ll);
    flush_batch(ll);
    lv_obj_t *block = lv_obj_create(ll->list);
    lv_obj_remove_style_all(block);
    lv_obj_set_width(block, LV_PCT(100));
    lv_obj_set_height(block, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(block, m->row_pad_x, 0);
    lv_obj_clear_flag(block, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_side(block, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(block, 1, 0);
    lv_obj_set_style_border_color(block, RPOD_COLOR_SEPARATOR, 0);
    lv_obj_set_style_border_opa(block, LV_OPA_COVER, 0);
    return block;
}

void rpod_live_list_header(rpod_live_list_t *ll, const char *title, const char *note)
{
    const rpod_metrics_t *m = rpod_metrics();
    lv_obj_t *header = add_block(ll);
    lv_obj_set_style_pad_ver(header, 8, 0);
    lv_obj_set_style_pad_row(header, 2, 0);

    if (title != NULL) {
        lv_obj_t *label = lv_label_create(header);
        lv_label_set_text(label, title);
        lv_obj_set_style_text_font(label, m->font_body, 0);
        lv_obj_set_style_text_color(label, RPOD_COLOR_TEXT, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(label, LV_PCT(100));
        lv_obj_set_height(label, lv_font_get_line_height(m->font_body));
    }
    if (note != NULL) {
        lv_obj_t *label = lv_label_create(header);
        lv_label_set_text(label, note);
        lv_obj_set_style_text_font(label, m->font_small, 0);
        lv_obj_set_style_text_color(label, RPOD_COLOR_DIM_TEXT, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(label, LV_PCT(100));
    }
}

void rpod_live_list_section(rpod_live_list_t *ll, const char *caption)
{
    const rpod_metrics_t *m = rpod_metrics();
    lv_obj_t *section = add_block(ll);
    /* Set off by space alone: a rule under it would cut it from its rows. */
    lv_obj_set_style_border_width(section, 0, 0);
    lv_obj_set_style_pad_top(section, 10, 0);
    lv_obj_set_style_pad_bottom(section, 2, 0);

    if (caption != NULL) {
        lv_obj_t *label = lv_label_create(section);
        lv_label_set_text(label, caption);
        lv_obj_set_style_text_font(label, m->font_small, 0);
        lv_obj_set_style_text_color(label, RPOD_COLOR_DIM_TEXT, 0);
    }
}

void rpod_live_list_reset_focus(rpod_live_list_t *ll)
{
    ll->pin_top = true;
}

rpod_list_item_t *rpod_live_list_add(rpod_live_list_t *ll, const char *key,
                                     void (*on_select)(rpod_screen_stack_t *stack, void *item_ctx))
{
    ensure_list(ll);
    if (ll->nrows == ll->rowcap) {
        size_t cap = ll->rowcap ? ll->rowcap * 2 : 8;
        row_t *rows = realloc(ll->rows, cap * sizeof(*rows));
        if (rows == NULL) {
            return NULL;
        }
        ll->rows = rows;
        ll->rowcap = cap;
    }
    if (ll->nbatch == ll->batchcap) {
        size_t cap = ll->batchcap ? ll->batchcap * 2 : 8;
        rpod_list_item_t *batch = realloc(ll->batch, cap * sizeof(*batch));
        if (batch == NULL) {
            return NULL;
        }
        ll->batch = batch;
        ll->batchcap = cap;
    }
    rpod_live_row_t *ref = malloc(sizeof(*ref));
    if (ref == NULL) {
        return NULL;
    }
    ref->ll = ll;
    snprintf(ref->key, sizeof(ref->key), "%s", key);
    ll->rows[ll->nrows++] = (row_t){ .ref = ref };

    rpod_list_item_t *it = &ll->batch[ll->nbatch++];
    memset(it, 0, sizeof(*it));
    it->on_select = on_select;
    it->item_ctx = ref;
    return it;
}
