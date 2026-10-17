#include "sheet.h"

#include "ui/classic.h"
#include "ui/metrics.h"
#include "ui/theme.h"

/* Dim scrim opacity for the area outside the sheet -- low enough that the
 * screen underneath (Now Playing's art, a list mid-scroll, whatever was up
 * when the hold fired) stays recognisable, per rpod_screen_stack_open_overlay
 * leaving it loaded and rendering on every refresh; high enough to read as
 * "this is a modal" rather than a stray transparent box. */
#define PICKER_SCRIM_OPA 110 /* ~43% */

/* The sheet itself needs to read clearly as a modal card, not blend into
 * whatever's showing through the scrim behind it -- unlike the other
 * rpod_theme_style_glass_panel() users (list_screen.c, now_playing.c), which
 * sit directly on a flat black screen or are meant to show the blurred art
 * through them, this sheet floats over the *live* previous screen (art,
 * a mid-scroll list, ...) dimmed only by PICKER_SCRIM_OPA above, so the
 * shared RPOD_GLASS_FILL_OPA default (~59%) left it looking washed out /
 * see-through. Overridden here, after rpod_theme_style_glass_panel(), rather
 * than raising the shared constant, since that would also thin out every
 * other glass surface in the app. */
#define PICKER_SHEET_OPA LV_OPA_90 /* ~90% -- opaque enough to read as a solid card */

/* A genuine floating popup, not another full page: `screen` here is the
 * overlay's root (see rpod_screen_stack_open_overlay) -- dimmed but
 * translucent, so the actual screen behind the hold gesture keeps showing
 * through it, with a single "liquid glass" sheet floating on top of that,
 * inset from every edge (including the top, below the status bar). Header
 * text and the playlist list are both children of that one sheet -- the
 * list itself stays plain/transparent rather than being its own nested
 * glass panel, so there's one card, not glass-on-glass. */
lv_obj_t *rpod_sheet_create(lv_obj_t *screen, const char *title_text, const char *subtitle)
{

    const rpod_metrics_t *m = rpod_metrics();

    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, PICKER_SCRIM_OPA, 0);

    int side_margin = 28;
    int top_margin = m->header_h + 16;
    int bottom_margin = 16;
    int pad = 14;

    int sheet_w = m->screen_w - 2 * side_margin;
    int sheet_h = rpod_metrics_bottom(m) - top_margin - bottom_margin;

    lv_obj_t *sheet = lv_obj_create(screen);
    lv_obj_remove_style_all(sheet);
    lv_obj_set_size(sheet, sheet_w, sheet_h);
    lv_obj_align(sheet, LV_ALIGN_TOP_MID, 0, top_margin);
    rpod_theme_style_glass_panel(sheet, 16);
    if (!rpod_theme()->classic) {
        lv_obj_set_style_bg_opa(sheet, PICKER_SHEET_OPA, 0);
    }
    lv_obj_set_style_clip_corner(sheet, true, 0);
    lv_obj_clear_flag(sheet, LV_OBJ_FLAG_SCROLLABLE);

    /* --- Header: what we're adding, and to where. --- */
    lv_obj_t *title = lv_label_create(sheet);
    lv_label_set_text(title, title_text);
    lv_obj_set_style_text_font(title, m->font_body, 0);
    lv_obj_set_style_text_color(title, RPOD_COLOR_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, pad, pad);

    int title_h = lv_font_get_line_height(m->font_body);
    if (subtitle != NULL) {
        lv_obj_t *song = lv_label_create(sheet);
        lv_label_set_text(song, subtitle);
        lv_obj_set_style_text_font(song, m->font_small, 0);
        lv_obj_set_style_text_color(song, RPOD_COLOR_DIM_TEXT, 0);
        lv_label_set_long_mode(song, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(song, sheet_w - 2 * pad);
        lv_obj_align(song, LV_ALIGN_TOP_LEFT, pad, pad + title_h + 2);
    }

    /* --- Playlist list, filling the rest of the sheet: full sheet width so
     * the sheet's own rounded corners (clip_corner above) round off the
     * list's last row to match, iOS grouped-table style. The active theme is
     * light-mode (lv_conf.h's LV_THEME_DEFAULT_DARK 0) and lv_list's default
     * "card" styling includes a light bg/border/radius that would otherwise
     * show through as a stray pale outline around the rows, so those are
     * overridden below -- explicitly, not via remove_style_all, which would
     * also wipe the LV_FLEX_FLOW_COLUMN layout lv_list_create() applies
     * internally to stack rows vertically, leaving every row at the
     * container's default (0,0) and overlapping (see list_screen.c's list,
     * which uses this same targeted-override pattern). --- */
    int list_top = pad + title_h + (subtitle != NULL ? lv_font_get_line_height(m->font_small) + 2 : 0) + pad;
    lv_obj_t *list = lv_list_create(sheet);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_size(list, sheet_w, sheet_h - list_top);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, list_top);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    return list;
}

static void classic_mark_color_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *mark = lv_event_get_user_data(e);
    lv_color_t c = lv_obj_has_state(btn, LV_STATE_FOCUSED) ? RPOD_COLOR_SEL_TEXT : RPOD_COLOR_TEXT;
    rpod_classic_icon_set_color(mark, c);
}

/* The row comes into view as the wheel reaches it: the list can be taller
 * than the sheet, and the rows don't scroll themselves (no SCROLL_ON_FOCUS,
 * which eases; list_screen.c snaps the same way). */
static void row_focused_cb(lv_event_t *e)
{
    lv_obj_scroll_to_view(lv_event_get_target_obj(e), LV_ANIM_OFF);
}

lv_obj_t *rpod_sheet_add_row(lv_obj_t *list, const char *label_text, const char *tag, bool checked,
                             bool is_last)
{
    const rpod_metrics_t *m = rpod_metrics();
    bool classic = rpod_theme()->classic;
    lv_obj_t *btn = lv_list_add_button(list, NULL, NULL);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_event_cb(btn, row_focused_cb, LV_EVENT_FOCUSED, NULL);
    if (classic) {
        rpod_classic_style_row(btn);
    } else {
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(btn, RPOD_COLOR_ACCENT, LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_FOCUSED);
        lv_obj_set_style_text_color(btn, RPOD_COLOR_TEXT, LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(btn, RPOD_COLOR_SEL_TEXT, LV_STATE_FOCUSED);
        rpod_theme_style_row_focus(btn);
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
    }
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, label_text);
    if (!classic) {
        lv_obj_set_style_text_font(label, m->font_body, 0);
    }
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(label, 1);
    if (classic) {
        lv_obj_set_height(label, lv_font_get_line_height(m->font_body));
    } else {
        lv_obj_set_width(label, LV_SIZE_CONTENT);
    }

    if (tag != NULL) {
        lv_obj_t *t = lv_label_create(btn);
        lv_label_set_text(t, tag);
        lv_obj_set_style_text_font(t, m->font_small, 0);
        lv_obj_set_style_pad_right(t, 6, 0);
    }

    lv_obj_t *mark;
    if (classic) {
        mark = rpod_classic_icon_create(btn, RPOD_ICON_CHECK, RPOD_COLOR_TEXT);
        lv_obj_add_event_cb(btn, classic_mark_color_cb, LV_EVENT_FOCUSED, mark);
        lv_obj_add_event_cb(btn, classic_mark_color_cb, LV_EVENT_DEFOCUSED, mark);
    } else {
        mark = lv_label_create(btn);
        lv_label_set_text(mark, LV_SYMBOL_OK);
    }
    if (!checked) {
        lv_obj_add_flag(mark, LV_OBJ_FLAG_HIDDEN);
    }
    return btn;
}
