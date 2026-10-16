#include "list_screen.h"

#include "ui/alpha_sort.h"
#include "ui/classic.h"
#include "ui/heart_icon.h"
#include "ui/metrics.h"
#include "ui/scrub.h"
#include "ui/theme.h"

#include <stdlib.h>
#include <string.h>

/* iOS Settings/Music-style rows: card-inset list, dim separators between
 * rows (not after the last one), dim subtitle/accessory text, and an
 * optional trailing chevron for rows that push another screen. Row padding,
 * gap, art size and heart size are form-factor dependent -- rpod_metrics().
 *
 * The classic themes build the iPod's rows instead (build_row_classic()):
 * one line of Chicago, a value right-aligned where the row has one, the
 * iPod's arrow, and the whole row inverted while it's selected -- in a
 * full-width list with the iPod's scrollbar beside it. */

/* The classic list sits one iPod pixel below the title bar's rule. */
#define CLASSIC_LIST_GAP RPOD_CLASSIC_PX

/* How much of a classic row its value may take before it's cut short, out
 * of 20 (the label gets the rest). */
#define CLASSIC_VALUE_MAX_20THS 11

typedef struct {
    rpod_screen_stack_t *stack;
    rpod_list_item_t item;
} row_ctx_t;

static void list_delete_cb(lv_event_t *e)
{
    row_ctx_t *rows = lv_event_get_user_data(e);
    free(rows);
}

/* Runs a row's select action, offering it the row's label as the title of
 * any screen it pushes (screen_stack.h). The action may well delete the
 * row -- popping its screen, or rebuilding a live list -- so nothing of
 * `row` is read after it. */
static void run_select(row_ctx_t *row)
{
    if (row->item.on_select == NULL) {
        return;
    }
    rpod_screen_stack_t *stack = row->stack;
    rpod_screen_stack_next_title(stack, row->item.text);
    row->item.on_select(stack, row->item.item_ctx);
    rpod_screen_stack_next_title(stack, NULL);
}

static void row_click_cb(lv_event_t *e)
{
    run_select(lv_event_get_user_data(e));
}

/* --- Rows with a press-and-hold action -----------------------------------
 *
 * The plain select fires on SHORT_CLICKED; a row with a hold action opens it
 * immediately from LONG_PRESSED, while the button is still down. The encoder
 * would otherwise still fire CLICKED on the physical release regardless of
 * hold length (only SHORT_CLICKED is suppressed after a hold -- see
 * indev_encoder_proc) -- and since that release lands *after* the picker's
 * push has already switched the indev to the new screen's group, it would be
 * delivered as a stray click on the picker's own first row. Telling LVGL to
 * wait_release() here makes it swallow that trailing release instead. */
static void row_short_click_cb(lv_event_t *e)
{
    run_select(lv_event_get_user_data(e));
}

static void row_long_pressed_cb(lv_event_t *e)
{
    row_ctx_t *row = lv_event_get_user_data(e);
    lv_indev_wait_release(lv_event_get_indev(e));
    if (row->item.on_long_press != NULL) {
        row->item.on_long_press(row->stack, row->item.item_ctx);
    }
}

/* Dim subtitle/accessory text reads poorly against the row's own blue
 * LV_STATE_FOCUSED highlight -- but a row's secondary labels are separate
 * lv_obj_ts that never themselves enter LV_STATE_FOCUSED (only `btn`,
 * the object actually in the input group, does), so a style entry keyed
 * to LV_STATE_FOCUSED on *those* labels would simply never match. Brighten
 * them by hand instead, in step with the row's own focus/defocus events. */
typedef struct {
    const char *text;   /* the row's primary label (its rpod_list_item_t's) */
    lv_obj_t *subtitle; /* NULL if this row has none */
    lv_obj_t *accessory;
    /* Trailing membership indicator, mutable after build via
     * rpod_list_row_set_status(). status_obj is the heart/check child (NULL
     * when status is NONE). */
    rpod_row_status_t status;
    lv_obj_t *status_obj;
    /* Art column tile (NULL if the row has none) and the image inside it
     * (NULL while it still shows the placeholder glyph) -- so art that
     * arrives after the row is built can fill it in via
     * rpod_list_row_set_thumb(). */
    lv_obj_t *art;
    lv_obj_t *art_img;
    const char *art_glyph; /* the placeholder's (rpod_list_item_t's) */
    /* Classic: the arrow, recoloured with the selection (it's an image, so
     * it can't inherit the row's text colour the way the labels do). */
    lv_obj_t *arrow;
} row_dim_labels_t;

/* Classic: the row's icons in the colour of its text -- inverted while
 * it's the selection. */
static void classic_row_colors(lv_obj_t *btn, row_dim_labels_t *dl)
{
    lv_color_t c = lv_obj_has_state(btn, LV_STATE_FOCUSED) ? RPOD_COLOR_SEL_TEXT : RPOD_COLOR_TEXT;
    if (dl->arrow != NULL) {
        rpod_classic_icon_set_color(dl->arrow, c);
    }
    if (dl->status_obj != NULL) {
        rpod_classic_icon_set_color(dl->status_obj, c);
    }
}

/* Creates, swaps, or removes the trailing heart/check to match `status`. */
static void apply_row_status(lv_obj_t *btn, row_dim_labels_t *dl, rpod_row_status_t status)
{
    if (dl->status == status && dl->status_obj != NULL) {
        return;
    }
    if (dl->status == status && status == RPOD_ROW_STATUS_NONE) {
        return;
    }
    if (dl->status_obj != NULL) {
        lv_obj_delete(dl->status_obj);
        dl->status_obj = NULL;
    }
    dl->status = status;

    if (rpod_theme()->classic) {
        if (status != RPOD_ROW_STATUS_NONE) {
            dl->status_obj = rpod_classic_icon_create(
                btn, status == RPOD_ROW_STATUS_HEART ? RPOD_ICON_HEART : RPOD_ICON_CHECK, RPOD_COLOR_TEXT);
            /* Before the arrow, if the row has one. */
            if (dl->arrow != NULL) {
                lv_obj_move_to_index(dl->status_obj, lv_obj_get_index(dl->arrow));
            }
            classic_row_colors(btn, dl);
        }
        return;
    }

    if (status == RPOD_ROW_STATUS_HEART) {
        dl->status_obj = rpod_heart_create(btn, rpod_metrics()->row_heart_size);
        rpod_heart_set_liked(dl->status_obj, true, false);
    } else if (status == RPOD_ROW_STATUS_CHECK) {
        dl->status_obj = lv_label_create(btn);
        lv_label_set_text(dl->status_obj, LV_SYMBOL_OK);
        lv_obj_set_style_text_color(dl->status_obj, RPOD_COLOR_TEXT, 0);
    }
}

void rpod_list_row_set_status(lv_obj_t *row, rpod_row_status_t status)
{
    row_dim_labels_t *dl = lv_obj_get_user_data(row);
    if (dl != NULL) {
        apply_row_status(row, dl, status);
    }
}

/* Fills the art tile with `thumb`, or with the placeholder glyph if NULL. */
static void fill_art_tile(row_dim_labels_t *dl, const lv_image_dsc_t *thumb)
{
    const rpod_metrics_t *m = rpod_metrics();
    lv_obj_clean(dl->art);
    dl->art_img = NULL;
    if (thumb != NULL) {
        dl->art_img = lv_image_create(dl->art);
        lv_image_set_src(dl->art_img, thumb);
        lv_obj_set_size(dl->art_img, m->list_art_size, m->list_art_size);
        lv_obj_center(dl->art_img);
    } else {
        lv_obj_t *placeholder = lv_label_create(dl->art);
        lv_label_set_text(placeholder, dl->art_glyph != NULL ? dl->art_glyph : LV_SYMBOL_AUDIO);
        lv_obj_set_style_text_color(placeholder, RPOD_COLOR_DIM_TEXT, 0);
        lv_obj_center(placeholder);
    }
}

bool rpod_list_row_has_thumb(lv_obj_t *row)
{
    row_dim_labels_t *dl = lv_obj_get_user_data(row);
    return dl != NULL && dl->art_img != NULL;
}

void rpod_list_row_set_thumb(lv_obj_t *row, const lv_image_dsc_t *thumb)
{
    row_dim_labels_t *dl = lv_obj_get_user_data(row);
    if (dl != NULL && dl->art != NULL && thumb != NULL) {
        fill_art_tile(dl, thumb);
    }
}

static void apply_row_focus_dim(lv_obj_t *btn, row_dim_labels_t *labels)
{
    if (rpod_theme()->classic) {
        classic_row_colors(btn, labels);
        return;
    }
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

/* Wires up a built row's select / hold / focus handlers -- the same for
 * either theme's rows. */
static void add_row_events(lv_obj_t *btn, row_ctx_t *row, row_dim_labels_t *dim_labels)
{
    /* A row with a hold action splits select (short click) from hold; a plain
     * row keeps the simple click-to-select. */
    if (row->item.on_long_press != NULL) {
        lv_obj_add_event_cb(btn, row_short_click_cb, LV_EVENT_SHORT_CLICKED, row);
        lv_obj_add_event_cb(btn, row_long_pressed_cb, LV_EVENT_LONG_PRESSED, row);
    } else {
        lv_obj_add_event_cb(btn, row_click_cb, LV_EVENT_CLICKED, row);
    }
    lv_obj_add_event_cb(btn, row_focus_cb, LV_EVENT_FOCUSED, dim_labels);
    lv_obj_add_event_cb(btn, row_focus_dim_cb, LV_EVENT_DEFOCUSED, dim_labels);
    lv_obj_add_event_cb(btn, row_dim_labels_free_cb, LV_EVENT_DELETE, dim_labels);
}

/* Builds one iOS-style row: a title (+ optional dim subtitle) column that
 * grows to fill the row, followed by an optional dim accessory label and an
 * optional ">" chevron on the right. */
static lv_obj_t *build_row(lv_obj_t *list, row_ctx_t *row, bool is_last)
{
    const rpod_metrics_t *m = rpod_metrics();
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
    lv_obj_set_style_pad_hor(btn, m->row_pad_x, 0);
    lv_obj_set_style_pad_ver(btn, m->row_pad_y, 0);
    lv_obj_set_style_pad_column(btn, m->row_gap, 0);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Selection highlight: fill the whole row in accent blue, like an iOS
     * table view cell's selected state. */
    lv_obj_set_style_bg_color(btn, RPOD_COLOR_ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(btn, RPOD_COLOR_TEXT, LV_STATE_FOCUSED);

    /* Art column, added before text_col so it lands leftmost in the row's
     * flex order. A fixed opaque tile regardless of focus state -- unlike
     * the row background, it isn't meant to disappear under the accent
     * highlight. */
    row_dim_labels_t *dim_labels = calloc(1, sizeof(*dim_labels));
    dim_labels->text = row->item.text;

    if (row->item.has_art_slot) {
        lv_obj_t *art = lv_obj_create(btn);
        lv_obj_remove_style_all(art);
        lv_obj_set_size(art, m->list_art_size, m->list_art_size);
        lv_obj_set_style_radius(art, 6, 0);
        lv_obj_set_style_bg_color(art, RPOD_COLOR_GLASS_FILL, 0);
        lv_obj_set_style_bg_opa(art, LV_OPA_COVER, 0);
        lv_obj_set_style_clip_corner(art, true, 0);
        lv_obj_clear_flag(art, LV_OBJ_FLAG_SCROLLABLE);
        dim_labels->art = art;
        dim_labels->art_glyph = row->item.art_glyph;
        fill_art_tile(dim_labels, row->item.thumb);
    }

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
    lv_obj_set_style_text_font(title, m->font_body, 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(title, LV_PCT(100));
    lv_obj_set_height(title, lv_font_get_line_height(m->font_body));

    if (row->item.subtitle[0] != '\0') {
        lv_obj_t *subtitle = lv_label_create(text_col);
        lv_label_set_text(subtitle, row->item.subtitle);
        lv_obj_set_style_text_color(subtitle, RPOD_COLOR_DIM_TEXT, 0);
        lv_obj_set_style_text_font(subtitle, m->font_small, 0);
        lv_label_set_long_mode(subtitle, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(subtitle, LV_PCT(100));
        lv_obj_set_height(subtitle, lv_font_get_line_height(m->font_small));
        dim_labels->subtitle = subtitle;
    }

    if (row->item.accessory[0] != '\0') {
        lv_obj_t *accessory = lv_label_create(btn);
        lv_label_set_text(accessory, row->item.accessory);
        lv_obj_set_style_text_color(accessory, RPOD_COLOR_DIM_TEXT, 0);
        dim_labels->accessory = accessory;
    }

    /* Trailing membership indicator (song rows), to the right of the
     * duration accessory. Stored on the button so the song lists can refresh
     * it in place via rpod_list_row_set_status() when they regain focus. */
    apply_row_status(btn, dim_labels, row->item.status);
    lv_obj_set_user_data(btn, dim_labels);

    if (row->item.chevron) {
        lv_obj_t *chevron = lv_label_create(btn);
        lv_label_set_text(chevron, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(chevron, RPOD_COLOR_DIM_TEXT, 0);
    }

    /* Group focus assigns the group's first row directly, without sending
     * it a LV_EVENT_FOCUSED -- sync colors for that initial state here
     * rather than waiting for an event that may never come. */
    apply_row_focus_dim(btn, dim_labels);

    add_row_events(btn, row, dim_labels);
    return btn;
}

/* An iPod menu row (SPEC.md §5.2's row templates): the label from the left
 * inset, then the row's value -- its accessory, else its subtitle --
 * right-aligned the way the iPod's Settings showed "Shuffle   Off", any
 * heart or check, and the arrow. All one line of Chicago: no art, and no
 * second line. Labels inherit the row's text colour, so they invert with
 * it. */
static lv_obj_t *build_row_classic(lv_obj_t *list, row_ctx_t *row)
{
    const rpod_metrics_t *m = rpod_metrics();
    int32_t line_h = lv_font_get_line_height(m->font_body);

    lv_obj_t *btn = lv_list_add_button(list, NULL, NULL);
    /* See build_row(): row_focus_cb snaps instead. */
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    rpod_classic_style_row(btn);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    row_dim_labels_t *dim_labels = calloc(1, sizeof(*dim_labels));
    dim_labels->text = row->item.text;

    /* Grown by the row's flex, which gives it the fixed width DOTS needs. */
    lv_obj_t *title = lv_label_create(btn);
    lv_label_set_text(title, row->item.text);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(title, 1);
    lv_obj_set_height(title, line_h);

    const char *value = row->item.accessory[0] != '\0' ? row->item.accessory : row->item.subtitle;
    if (value[0] != '\0') {
        lv_obj_t *v = lv_label_create(btn);
        lv_label_set_text(v, value);
        lv_obj_set_height(v, line_h);
        lv_point_t size;
        lv_text_get_size(&size, value, m->font_body, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        int32_t max_w = m->screen_w * CLASSIC_VALUE_MAX_20THS / 20;
        if (size.x > max_w) {
            lv_obj_set_width(v, max_w);
            lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_DOTS);
        }
    }

    apply_row_status(btn, dim_labels, row->item.status);

    if (row->item.chevron) {
        dim_labels->arrow = rpod_classic_icon_create(btn, RPOD_ICON_ARROW, RPOD_COLOR_TEXT);
    }
    lv_obj_set_user_data(btn, dim_labels);
    classic_row_colors(btn, dim_labels);
    add_row_events(btn, row, dim_labels);
    return btn;
}

/* --- Classic scrollbar --------------------------------------------------------
 *
 * The classic list's user data is its scrollbar (rpod_classic_scrollbar_*),
 * a sibling inside the wrapper rpod_list_screen_create() makes. It shows
 * while the rows don't all fit, and the list gives up its width to it. */

static void classic_scroll_cb(lv_event_t *e)
{
    lv_obj_t *list = lv_event_get_target(e);
    int32_t visible = lv_obj_get_height(list);
    int32_t top = lv_obj_get_scroll_top(list);
    rpod_classic_scrollbar_set(lv_event_get_user_data(e), visible + top + lv_obj_get_scroll_bottom(list),
                               visible, top);
}

/* Children that aren't rows -- a live list's header or section caption --
 * made a whole number of rows tall, so the rows after them stay on the
 * row grid and scrolling a row at a time never leaves one cut in half. A
 * minimum rather than a height: they still fit their text. */
static void classic_snap_blocks(lv_obj_t *list)
{
    int32_t row_h = rpod_metrics()->row_h;
    uint32_t n = lv_obj_get_child_count(list);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(list, (int32_t)i);
        if (lv_obj_get_user_data(child) != NULL) {
            continue; /* a row (row_dim_labels_t) */
        }
        int32_t h = lv_obj_get_height(child);
        lv_obj_set_style_min_height(child, (h + row_h - 1) / row_h * row_h, 0);
    }
}

static void classic_sync_scrollbar(lv_obj_t *list)
{
    lv_obj_t *sb = lv_obj_get_user_data(list);
    if (sb == NULL) {
        return;
    }
    const rpod_metrics_t *m = rpod_metrics();
    for (int pass = 0; pass < 2; pass++) {
        /* Once to find whether the scrollbar's needed, and again with the
         * list narrowed for it (text may wrap differently). */
        lv_obj_update_layout(list);
        classic_snap_blocks(list);
        lv_obj_update_layout(list);
        int32_t visible = lv_obj_get_height(list);
        int32_t top = lv_obj_get_scroll_top(list);
        bool shown = rpod_classic_scrollbar_set(sb, visible + top + lv_obj_get_scroll_bottom(list), visible, top);
        int32_t w = m->screen_w - (shown ? RPOD_CLASSIC_SCROLLBAR_W : 0);
        if (lv_obj_get_width(list) == w) {
            break;
        }
        lv_obj_set_width(list, w);
    }
}

/* The classic list: full width under the title bar, exactly as many rows
 * tall as fit, in a wrapper that also holds its scrollbar -- so deleting
 * the screen's first child (music_screens.c's playlist list does, to
 * refresh) takes both. */
static lv_obj_t *create_classic(lv_obj_t *screen)
{
    const rpod_metrics_t *m = rpod_metrics();
    int32_t h = m->screen_h - m->header_h;

    lv_obj_t *wrap = lv_obj_create(screen);
    lv_obj_remove_style_all(wrap);
    lv_obj_set_pos(wrap, 0, m->header_h);
    lv_obj_set_size(wrap, m->screen_w, h);
    lv_obj_remove_flag(wrap, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *list = lv_list_create(wrap);
    lv_obj_set_pos(list, 0, CLASSIC_LIST_GAP);
    lv_obj_set_size(list, m->screen_w, ((h - CLASSIC_LIST_GAP) / m->row_h) * m->row_h);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 0, 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *sb = rpod_classic_scrollbar_create(wrap, h);
    lv_obj_set_pos(sb, m->screen_w - RPOD_CLASSIC_SCROLLBAR_W, 0);
    lv_obj_set_user_data(list, sb);
    lv_obj_add_event_cb(list, classic_scroll_cb, LV_EVENT_SCROLL, sb);
    return list;
}

lv_obj_t *rpod_list_screen_create(lv_obj_t *screen)
{
    if (rpod_theme()->classic) {
        return create_classic(screen);
    }
    const rpod_metrics_t *m = rpod_metrics();
    lv_obj_t *list = lv_list_create(screen);
    const int32_t mg = m->list_margin;
    lv_obj_set_size(list, m->screen_w - 2 * mg, m->screen_h - m->header_h - 2 * mg);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, -mg);
    rpod_theme_style_glass_panel(list, 12);
    lv_obj_set_style_clip_corner(list, true, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    /* Wheel-driven, not touch-scrolled -- a scrollbar thumb has nothing to
     * grab and just adds visual noise against the iOS-style card. */
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    return list;
}

void rpod_list_screen_populate(rpod_screen_stack_t *stack, lv_obj_t *list,
                                const rpod_list_item_t *items, size_t count)
{
    bool classic = rpod_theme()->classic;
    if (count == 0) {
        lv_obj_t *btn = lv_list_add_button(list, NULL, "(empty)");
        if (classic) {
            rpod_classic_style_row(btn);
        }
        lv_obj_add_state(btn, LV_STATE_DISABLED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(btn, RPOD_COLOR_DIM_TEXT, 0);
        if (classic) {
            classic_sync_scrollbar(list);
        }
        return;
    }

    row_ctx_t *rows = malloc(count * sizeof(*rows));
    for (size_t i = 0; i < count; i++) {
        rows[i].stack = stack;
        rows[i].item = items[i];
    }
    lv_obj_add_event_cb(list, list_delete_cb, LV_EVENT_DELETE, rows);

    for (size_t i = 0; i < count; i++) {
        if (classic) {
            build_row_classic(list, &rows[i]);
        } else {
            build_row(list, &rows[i], i == count - 1);
        }
    }
    if (classic) {
        classic_sync_scrollbar(list);
    }
}

lv_obj_t *rpod_list_screen_build(rpod_screen_stack_t *stack, lv_obj_t *screen,
                                  const rpod_list_item_t *items, size_t count)
{
    lv_obj_t *list = rpod_list_screen_create(screen);
    rpod_list_screen_populate(stack, list, items, count);
    return list;
}

/* --- Alphabet scrub ---------------------------------------------------------- */

/* The list's rows, in order -- every child built by build_row(), skipping a
 * header or the "(empty)" placeholder. */
typedef struct {
    lv_obj_t **rows;
    size_t count;
} scrub_rows_t;

static const char *scrub_row_name(const void *ctx, size_t i)
{
    const scrub_rows_t *r = ctx;
    const row_dim_labels_t *dl = lv_obj_get_user_data(r->rows[i]);
    return dl->text;
}

static void list_scrub_cb(lv_event_t *e)
{
    lv_obj_t *list = lv_event_get_user_data(e);
    rpod_scrub_param_t *param = lv_event_get_param(e);

    uint32_t n = lv_obj_get_child_count(list);
    if (n < RPOD_SCRUB_MIN_ROWS) {
        return;
    }
    scrub_rows_t r = { .rows = malloc(n * sizeof(*r.rows)), .count = 0 };
    if (r.rows == NULL) {
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(list, (int32_t)i);
        const row_dim_labels_t *dl = lv_obj_get_user_data(child);
        if (dl != NULL && dl->text != NULL) {
            r.rows[r.count++] = child;
        }
    }
    if (r.count < RPOD_SCRUB_MIN_ROWS) {
        free(r.rows);
        return;
    }

    lv_group_t *group = lv_obj_get_group(r.rows[0]);
    lv_obj_t *focused = group != NULL ? lv_group_get_focused(group) : NULL;
    size_t cur = 0;
    for (size_t i = 0; i < r.count; i++) {
        if (r.rows[i] == focused) {
            cur = i;
            break;
        }
    }

    size_t target = rpod_alpha_jump(scrub_row_name, &r, r.count, cur, param->dir);
    if (target != cur) {
        lv_group_focus_obj(r.rows[target]);
        /* Put the letter's first row at the top, rather than wherever
         * focusing it scrolled to, so as much of the letter as fits is on
         * screen. Scrolling is bounded, so the last letters still fill the
         * list. */
        lv_obj_update_layout(list);
        lv_obj_scroll_to_y(list, lv_obj_get_y(r.rows[target]), LV_ANIM_OFF);
    }
    param->letter = rpod_alpha_letter(scrub_row_name(&r, target));
    param->handled = true;
    free(r.rows);
}

void rpod_list_screen_enable_scrub(lv_obj_t *screen, lv_obj_t *list)
{
    lv_obj_add_event_cb(screen, list_scrub_cb, (lv_event_code_t)rpod_scrub_event(), list);
}
