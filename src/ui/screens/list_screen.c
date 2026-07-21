#include "list_screen.h"

#include "ui/theme.h"

#include <stdlib.h>
#include <string.h>

/* iOS Settings/Music-style row metrics: card-inset list, dim separators
 * between rows (not after the last one), dim subtitle/accessory text, and
 * an optional trailing chevron for rows that push another screen. */
#define ROW_PAD_X  14
#define ROW_PAD_Y  8
#define ROW_GAP    8

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

/* Dim subtitle/accessory text reads poorly against the row's own blue
 * LV_STATE_FOCUSED highlight -- but a row's secondary labels are separate
 * lv_obj_ts that never themselves enter LV_STATE_FOCUSED (only `btn`,
 * the object actually in the input group, does), so a style entry keyed
 * to LV_STATE_FOCUSED on *those* labels would simply never match. Brighten
 * them by hand instead, in step with the row's own focus/defocus events. */
typedef struct {
    lv_obj_t *subtitle; /* NULL if this row has none */
    lv_obj_t *accessory;
} row_dim_labels_t;

static void apply_row_focus_dim(lv_obj_t *btn, row_dim_labels_t *labels)
{
    lv_color_t color = lv_obj_has_state(btn, LV_STATE_FOCUSED) ? RPOD_COLOR_TEXT : RPOD_COLOR_DIM_TEXT;
    if (labels->subtitle != NULL) {
        lv_obj_set_style_text_color(labels->subtitle, color, 0);
    }
    if (labels->accessory != NULL) {
        lv_obj_set_style_text_color(labels->accessory, color, 0);
    }
}

static void row_focus_dim_cb(lv_event_t *e)
{
    apply_row_focus_dim(lv_event_get_target(e), lv_event_get_user_data(e));
}

/* A freshly built row that lands as the group's first (auto-focused) row
 * never gets a LV_EVENT_FOCUSED of its own -- lv_group_add_obj() focuses
 * the first object it's given directly, without sending the event -- so
 * without this, the very first row on a freshly pushed screen kept its
 * dim, low-contrast subtitle/accessory colors despite sitting on the blue
 * highlight. Also swaps LVGL's animated scroll-on-focus for an instant
 * snap: the click wheel expects each click to move the highlight
 * immediately, not ease into view like a touch-scroll list. */
static void row_focus_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    apply_row_focus_dim(btn, lv_event_get_user_data(e));
    lv_obj_scroll_to_view_recursive(btn, LV_ANIM_OFF);
}

static void row_dim_labels_free_cb(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

/* Builds one iOS-style row: a title (+ optional dim subtitle) column that
 * grows to fill the row, followed by an optional dim accessory label and an
 * optional ">" chevron on the right. */
static lv_obj_t *build_row(lv_obj_t *list, row_ctx_t *row, bool is_last)
{
    lv_obj_t *btn = lv_list_add_button(list, NULL, NULL);
    /* lv_button's default LV_OBJ_FLAG_SCROLL_ON_FOCUS drives LVGL's own
     * eased scroll-into-view on focus, which reads as laggy for a click
     * wheel where each click should snap the highlight over immediately.
     * row_focus_cb below does the same job with LV_ANIM_OFF instead. */
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    /* Transparent by default -- the list container itself is the glass
     * surface (rpod_theme_style_glass_panel() in rpod_list_screen_build);
     * a row only gets an opaque fill when focused, below. */
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_STATE_DEFAULT);
    /* lv_list_button's default theme (light mode, see lv_conf.h's
     * LV_THEME_DEFAULT_DARK) supplies dark text meant for a light card --
     * invisible against the dark glass panel above unless overridden. Text
     * color is an inheritable style property, so this one line fixes the
     * title label (and any other row child that doesn't set its own
     * color) without touching each label individually. */
    lv_obj_set_style_text_color(btn, RPOD_COLOR_TEXT, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, LV_STATE_DEFAULT);
    if (!is_last) {
        lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, RPOD_COLOR_SEPARATOR, 0);
        lv_obj_set_style_border_opa(btn, LV_OPA_COVER, 0);
    }
    lv_obj_set_style_pad_hor(btn, ROW_PAD_X, 0);
    lv_obj_set_style_pad_ver(btn, ROW_PAD_Y, 0);
    lv_obj_set_style_pad_column(btn, ROW_GAP, 0);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Selection highlight: fill the whole row in accent blue, like an iOS
     * table view cell's selected state. */
    lv_obj_set_style_bg_color(btn, RPOD_COLOR_ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(btn, RPOD_COLOR_TEXT, LV_STATE_FOCUSED);

    lv_obj_t *text_col = lv_obj_create(btn);
    lv_obj_remove_style_all(text_col);
    lv_obj_set_flex_flow(text_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(text_col, 1);
    lv_obj_set_width(text_col, LV_SIZE_CONTENT);
    lv_obj_set_height(text_col, LV_SIZE_CONTENT);
    lv_obj_clear_flag(text_col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_row(text_col, 2, 0);

    /* LONG_MODE_DOTS only truncates a label that has a *fixed* height --
     * left at the default size-content height, it just wraps forever
     * instead. Pin each label to exactly one line's height so a long
     * title/subtitle truncates with "..." rather than wrapping the row
     * across two or three lines. */
    lv_obj_t *title = lv_label_create(text_col);
    lv_label_set_text(title, row->item.text);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(title, LV_PCT(100));
    lv_obj_set_height(title, lv_font_get_line_height(&lv_font_montserrat_16));

    row_dim_labels_t *dim_labels = calloc(1, sizeof(*dim_labels));

    if (row->item.subtitle[0] != '\0') {
        lv_obj_t *subtitle = lv_label_create(text_col);
        lv_label_set_text(subtitle, row->item.subtitle);
        lv_obj_set_style_text_color(subtitle, RPOD_COLOR_DIM_TEXT, 0);
        lv_label_set_long_mode(subtitle, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(subtitle, LV_PCT(100));
        lv_obj_set_height(subtitle, lv_font_get_line_height(LV_FONT_DEFAULT));
        dim_labels->subtitle = subtitle;
    }

    if (row->item.accessory[0] != '\0') {
        lv_obj_t *accessory = lv_label_create(btn);
        lv_label_set_text(accessory, row->item.accessory);
        lv_obj_set_style_text_color(accessory, RPOD_COLOR_DIM_TEXT, 0);
        dim_labels->accessory = accessory;
    }

    if (row->item.chevron) {
        lv_obj_t *chevron = lv_label_create(btn);
        lv_label_set_text(chevron, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(chevron, RPOD_COLOR_DIM_TEXT, 0);
    }

    /* Group focus assigns the group's first row directly, without sending
     * it a LV_EVENT_FOCUSED -- sync colors for that initial state here
     * rather than waiting for an event that may never come. */
    apply_row_focus_dim(btn, dim_labels);

    lv_obj_add_event_cb(btn, row_click_cb, LV_EVENT_CLICKED, row);
    lv_obj_add_event_cb(btn, row_focus_cb, LV_EVENT_FOCUSED, dim_labels);
    lv_obj_add_event_cb(btn, row_focus_dim_cb, LV_EVENT_DEFOCUSED, dim_labels);
    lv_obj_add_event_cb(btn, row_dim_labels_free_cb, LV_EVENT_DELETE, dim_labels);
    return btn;
}

void rpod_list_screen_build(rpod_screen_stack_t *stack, lv_obj_t *screen,
                             const rpod_list_item_t *items, size_t count)
{
    lv_obj_t *list = lv_list_create(screen);
    lv_obj_set_size(list, RPOD_SCREEN_WIDTH - 16, RPOD_SCREEN_HEIGHT - RPOD_HEADER_HEIGHT - 16);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, -8);
    rpod_theme_style_glass_panel(list, 12);
    lv_obj_set_style_clip_corner(list, true, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    /* Wheel-driven, not touch-scrolled -- a scrollbar thumb has nothing to
     * grab and just adds visual noise against the iOS-style card. */
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    if (count == 0) {
        lv_obj_t *btn = lv_list_add_button(list, NULL, "(empty)");
        lv_obj_add_state(btn, LV_STATE_DISABLED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(btn, RPOD_COLOR_DIM_TEXT, 0);
        return;
    }

    row_ctx_t *rows = malloc(count * sizeof(*rows));
    for (size_t i = 0; i < count; i++) {
        rows[i].stack = stack;
        rows[i].item = items[i];
    }
    lv_obj_add_event_cb(list, list_delete_cb, LV_EVENT_DELETE, rows);

    for (size_t i = 0; i < count; i++) {
        build_row(list, &rows[i], i == count - 1);
    }
}
