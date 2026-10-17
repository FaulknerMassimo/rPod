#include "output_screens.h"

#include "sheet.h"
#include "audio/sinks.h"
#include "ui/metrics.h"
#include "ui/theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct out_state out_state_t;

typedef struct {
    out_state_t *st;
    rpod_sink_t sink;
} out_row_t;

struct out_state {
    rpod_screen_stack_t *stack;
    lv_obj_t *list;
    lv_obj_t *message;
    out_row_t rows[RPOD_SINK_MAX];
    size_t count;
};

static void row_click_cb(lv_event_t *e)
{
    out_row_t *row = lv_event_get_user_data(e);
    rpod_screen_stack_t *stack = row->st->stack; /* closing frees `row` */
    rpod_sinks_select(&row->sink);
    rpod_screen_stack_close_overlay(stack);
}

/* Fills the sheet from the current sinks, keeping the highlight on the same
 * row index (the rows are rebuilt when a speaker turns up or goes). */
static void out_fill(out_state_t *st)
{
    int focused = -1;
    uint32_t n_children = lv_obj_get_child_count(st->list);
    for (uint32_t i = 0; i < n_children; i++) {
        if (lv_obj_has_state(lv_obj_get_child(st->list, (int32_t)i), LV_STATE_FOCUSED)) {
            focused = (int)i;
        }
    }
    lv_obj_clean(st->list);
    if (st->message != NULL) {
        lv_obj_delete(st->message);
        st->message = NULL;
    }

    rpod_sink_t sinks[RPOD_SINK_MAX];
    size_t n = rpod_sinks_list(sinks, RPOD_SINK_MAX);
    st->count = n;
    if (n == 0) {
        st->message = lv_label_create(lv_obj_get_parent(st->list));
        lv_label_set_text(st->message, rpod_sinks_available() ? "No headphones or AirPlay speakers found."
                                                               : "No outputs to choose from.");
        lv_obj_set_style_text_color(st->message, RPOD_COLOR_DIM_TEXT, 0);
        lv_obj_set_width(st->message, lv_obj_get_width(st->list) - 28);
        lv_label_set_long_mode(st->message, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_align_to(st->message, st->list, LV_ALIGN_TOP_LEFT, 14, 4);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        st->rows[i].st = st;
        st->rows[i].sink = sinks[i];
        lv_obj_t *btn = rpod_sheet_add_row(st->list, sinks[i].label,
                                           sinks[i].kind == RPOD_SINK_AIRPLAY ? "AirPlay" : "Bluetooth",
                                           sinks[i].current, i == n - 1);
        lv_obj_add_event_cb(btn, row_click_cb, LV_EVENT_CLICKED, &st->rows[i]);
        if ((int)i == focused || (focused < 0 && sinks[i].current)) {
            lv_group_focus_obj(btn);
        }
    }
}

static void out_changed_cb(void *user)
{
    out_fill(user);
}

static void out_cleanup_cb(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

void rpod_output_screen_build(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    (void)ctx;
    out_state_t *st = calloc(1, sizeof(*st));
    if (st == NULL) {
        return;
    }
    st->stack = stack;
    st->list = rpod_sheet_create(screen, "Play On", NULL);
    lv_obj_add_event_cb(screen, out_cleanup_cb, LV_EVENT_DELETE, st);
    out_fill(st);
    rpod_sinks_watch(screen, out_changed_cb, st);
    rpod_sinks_refresh();
}
