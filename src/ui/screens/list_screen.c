#include "list_screen.h"

#include "ui/theme.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    rpod_screen_stack_t *stack;
    rpod_list_item_t item;
} row_ctx_t;

static void list_delete_cb(lv_event_t *e)
{
    row_ctx_t *rows = lv_event_get_user_data(e);
    free(rows);
}

static void row_click_cb(lv_event_t *e)
{
    row_ctx_t *row = lv_event_get_user_data(e);
    if (row->item.on_select != NULL) {
        row->item.on_select(row->stack, row->item.item_ctx);
    }
}

void rpod_list_screen_build(rpod_screen_stack_t *stack, lv_obj_t *screen, const char *title,
                             const rpod_list_item_t *items, size_t count)
{
    rpod_theme_create_header(screen, title);

    lv_obj_t *list = lv_list_create(screen);
    lv_obj_set_size(list, LV_PCT(100), RPOD_SCREEN_HEIGHT - RPOD_HEADER_HEIGHT);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);

    if (count == 0) {
        lv_obj_t *btn = lv_list_add_button(list, NULL, "(empty)");
        lv_obj_add_state(btn, LV_STATE_DISABLED);
        return;
    }

    row_ctx_t *rows = malloc(count * sizeof(*rows));
    for (size_t i = 0; i < count; i++) {
        rows[i].stack = stack;
        rows[i].item = items[i];
    }
    lv_obj_add_event_cb(list, list_delete_cb, LV_EVENT_DELETE, rows);

    for (size_t i = 0; i < count; i++) {
        lv_obj_t *btn = lv_list_add_button(list, NULL, rows[i].item.text);
        lv_obj_add_event_cb(btn, row_click_cb, LV_EVENT_CLICKED, &rows[i]);
        /* Default theme's focus indicator (a thin outline) is easy to miss
         * on a 320x240 panel -- make wheel-driven selection obvious. */
        lv_obj_set_style_bg_color(btn, RPOD_COLOR_ACCENT, LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_FOCUSED);
        lv_obj_set_style_text_color(btn, RPOD_COLOR_TEXT, LV_STATE_FOCUSED);
    }
}
