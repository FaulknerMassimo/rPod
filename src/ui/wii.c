#include "wii.h"

#include "theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef RPOD_WII_ASSETS
#include "wii_images.h"
#endif

bool rpod_wii_has_assets(void)
{
#ifdef RPOD_WII_ASSETS
    return true;
#else
    return false;
#endif
}

bool rpod_wii_layout(void)
{
    return rpod_theme()->wii && rpod_wii_has_assets();
}

static bool g_bar_hidden;

void rpod_wii_set_bar_hidden(bool hidden)
{
    g_bar_hidden = hidden;
}

bool rpod_wii_bar_hidden(void)
{
    return g_bar_hidden;
}

int rpod_wii_battery_frame(const rpod_battery_t *battery)
{
    if (battery == NULL) {
        return -1;
    }
    return rpod_wii_battery_level_frame(battery->present, battery->level, battery->external, battery->charging);
}

#ifdef RPOD_WII_ASSETS

/* The bar's contents, in the bar's own coordinates (the wave's crop starts
 * at y = 170 of the theme's 320x240). */
#define BAR_Y        (240 - RPOD_WII_BAR_H)
#define CLOCK_Y      0     /* the digits sit in the dip's white */
#define CLOCK_GAP    2
#define CLOCK_COLOR  0x808080
#define DATE_Y       34
#define DATE_COLOR   0x5a5a5a
#define TITLE_Y      53
#define TITLE_X      70
#define TITLE_W      180
#define TITLE_COLOR  0x707070
#define LOCK_X       276 /* on the right-hand button */
#define LOCK_Y       26

/* The playback panels, relative to the column image (the theme drew these at
 * absolute positions on its backdrop; 233, 5 is where the crop starts). */
#define COLUMN_X     233
#define COLUMN_Y     5
#define COL_BATT_X   (246 - COLUMN_X)
#define COL_BATT_Y   (19 - COLUMN_Y)
#define COL_PCT_Y    (37 - COLUMN_Y)
#define COL_PLAY_X   (250 - COLUMN_X)
#define COL_PLAY_Y   (72 - COLUMN_Y)
#define COL_REPEAT_X (236 - COLUMN_X)
#define COL_REPEAT_Y (128 - COLUMN_Y)
#define COL_SHUFFLE_X (267 - COLUMN_X)

static lv_obj_t *image_at(lv_obj_t *parent, const lv_image_dsc_t *dsc, int32_t x, int32_t y)
{
    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, dsc);
    lv_obj_set_pos(img, x, y);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return img;
}

static lv_obj_t *plain(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static const lv_image_dsc_t *series_frame(rpod_wii_series_t series, int frame)
{
    const lv_image_dsc_t *const *set;
    int n;
    switch (series) {
    case RPOD_WII_SERIES_VOLUME:   set = wii_img_volume;   n = WII_VOLUME_COUNT;   break;
    case RPOD_WII_SERIES_PLAYMODE: set = wii_img_playmode; n = WII_PLAYMODE_COUNT; break;
    case RPOD_WII_SERIES_REPEAT:   set = wii_img_repeat;   n = WII_REPEAT_COUNT;   break;
    case RPOD_WII_SERIES_SHUFFLE:  set = wii_img_shuffle;  n = WII_SHUFFLE_COUNT;  break;
    case RPOD_WII_SERIES_BATTERY:  set = wii_img_battery;  n = WII_BATTERY_COUNT;  break;
    default:                       set = wii_img_lock;     n = WII_LOCK_COUNT;     break;
    }
    return set[frame < 0 ? 0 : frame >= n ? n - 1 : frame];
}

lv_obj_t *rpod_wii_frame_create(lv_obj_t *parent, rpod_wii_series_t series, int frame)
{
    return image_at(parent, series_frame(series, frame), 0, 0);
}

void rpod_wii_frame_set(lv_obj_t *img, rpod_wii_series_t series, int frame)
{
    const lv_image_dsc_t *dsc = series_frame(series, frame);
    if (lv_image_get_src(img) != dsc) {
        lv_image_set_src(img, dsc);
    }
}

lv_obj_t *rpod_wii_np_bg_create(lv_obj_t *parent)
{
    return image_at(parent, &wii_img_np_bg, 0, 0);
}

lv_obj_t *rpod_wii_mii_create(lv_obj_t *parent, int which)
{
    return image_at(parent, wii_img_mii[which == 0 ? 0 : 1], 0, 0);
}

/* --- The bar ------------------------------------------------------------------ */

static struct {
    lv_obj_t *digits[5]; /* h h : m m */
    lv_obj_t *date;
    lv_obj_t *title;
    lv_obj_t *lock;
} g_bar;

lv_obj_t *rpod_wii_bar_create(lv_obj_t *layer)
{
    lv_obj_t *bar = plain(layer, 0, BAR_Y, 320, RPOD_WII_BAR_H);
    image_at(bar, &wii_img_bar, 0, 0);

    /* The clock: 4 digits and a colon, centred on the dip. */
    static const int kinds[5] = {0, 0, 10, 0, 0};
    int32_t total = 4 * wii_img_digit_0.header.w + wii_img_digit_colon.header.w + 4 * CLOCK_GAP;
    int32_t x = (320 - total) / 2;
    for (int i = 0; i < 5; i++) {
        const lv_image_dsc_t *dsc = wii_img_digits[kinds[i]];
        lv_obj_t *d = image_at(bar, dsc, x, CLOCK_Y);
        lv_obj_set_style_image_recolor(d, lv_color_hex(CLOCK_COLOR), 0);
        lv_obj_set_style_image_recolor_opa(d, LV_OPA_COVER, 0);
        g_bar.digits[i] = d;
        x += dsc->header.w + CLOCK_GAP;
    }

    g_bar.date = lv_label_create(bar);
    lv_obj_set_style_text_font(g_bar.date, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(g_bar.date, lv_color_hex(DATE_COLOR), 0);
    lv_obj_set_style_text_align(g_bar.date, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(g_bar.date, 120, 18);
    lv_obj_set_pos(g_bar.date, 100, DATE_Y);
    lv_label_set_text(g_bar.date, "");

    g_bar.title = lv_label_create(bar);
    lv_obj_set_style_text_font(g_bar.title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(g_bar.title, lv_color_hex(TITLE_COLOR), 0);
    lv_obj_set_style_text_align(g_bar.title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(g_bar.title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(g_bar.title, TITLE_W, 17);
    lv_obj_set_pos(g_bar.title, TITLE_X, TITLE_Y);
    lv_label_set_text(g_bar.title, "");

    g_bar.lock = image_at(bar, wii_img_lock[0], LOCK_X, LOCK_Y);
    lv_obj_add_flag(g_bar.lock, LV_OBJ_FLAG_HIDDEN);
    return bar;
}

void rpod_wii_bar_set_time(const struct tm *tm)
{
    if (g_bar.digits[0] == NULL) {
        return;
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d%02d", tm->tm_hour, tm->tm_min);
    static const int slot[4] = {0, 1, 3, 4};
    for (int i = 0; i < 4; i++) {
        const lv_image_dsc_t *dsc = wii_img_digits[buf[i] - '0'];
        if (lv_image_get_src(g_bar.digits[slot[i]]) != dsc) {
            lv_image_set_src(g_bar.digits[slot[i]], dsc);
        }
    }
    char date[24];
    strftime(date, sizeof(date), "%a %m/%d", tm);
    if (strcmp(lv_label_get_text(g_bar.date), date) != 0) {
        lv_label_set_text(g_bar.date, date);
    }
}

void rpod_wii_bar_set_title(const char *title)
{
    if (g_bar.title != NULL && strcmp(lv_label_get_text(g_bar.title), title) != 0) {
        lv_label_set_text(g_bar.title, title);
    }
}

void rpod_wii_bar_set_locked(bool locked)
{
    if (g_bar.lock == NULL) {
        return;
    }
    if (locked) {
        lv_obj_remove_flag(g_bar.lock, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_bar.lock, LV_OBJ_FLAG_HIDDEN);
    }
}

/* --- The playback panels -------------------------------------------------------- */

#define MAX_COLUMNS 8

typedef struct {
    lv_obj_t *root;
    lv_obj_t *battery;
    lv_obj_t *pct;
    lv_obj_t *play;
    lv_obj_t *repeat;
    lv_obj_t *shuffle;
} column_t;

static column_t *g_columns[MAX_COLUMNS];
static rpod_mpd_status_t g_player;
static bool g_player_known;

static void column_update(column_t *c)
{
    const rpod_mpd_status_t *st = g_player_known ? &g_player : NULL;
    rpod_wii_frame_set(c->play, RPOD_WII_SERIES_PLAYMODE, rpod_wii_playmode_frame(st != NULL ? st->state : 0));
    rpod_wii_frame_set(c->repeat, RPOD_WII_SERIES_REPEAT, rpod_wii_repeat_frame(st));
    rpod_wii_frame_set(c->shuffle, RPOD_WII_SERIES_SHUFFLE, rpod_wii_shuffle_frame(st));

    /* Without a PiSugar the panel stays blank rather than invent a level. */
    const rpod_battery_t *b = rpod_battery();
    int frame = rpod_wii_battery_frame(b);
    if (frame < 0) {
        lv_obj_add_flag(c->battery, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(c->pct, "");
        return;
    }
    lv_obj_remove_flag(c->battery, LV_OBJ_FLAG_HIDDEN);
    rpod_wii_frame_set(c->battery, RPOD_WII_SERIES_BATTERY, frame);
    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", b->level);
    if (strcmp(lv_label_get_text(c->pct), pct) != 0) {
        lv_label_set_text(c->pct, pct);
    }
}

static void column_delete_cb(lv_event_t *e)
{
    column_t *c = lv_event_get_user_data(e);
    for (int i = 0; i < MAX_COLUMNS; i++) {
        if (g_columns[i] == c) {
            g_columns[i] = NULL;
        }
    }
    free(c);
}

lv_obj_t *rpod_wii_column_create(lv_obj_t *screen)
{
    column_t *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return NULL;
    }
    c->root = plain(screen, COLUMN_X, COLUMN_Y, wii_img_column.header.w, wii_img_column.header.h);
    image_at(c->root, &wii_img_column, 0, 0);

    c->battery = image_at(c->root, wii_img_battery[0], COL_BATT_X, COL_BATT_Y);
    c->pct = lv_label_create(c->root);
    lv_obj_set_style_text_font(c->pct, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(c->pct, lv_color_hex(0x707070), 0);
    lv_obj_set_style_text_align(c->pct, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(c->pct, 60, 18);
    lv_obj_set_pos(c->pct, COL_BATT_X - 9, COL_PCT_Y);
    lv_label_set_text(c->pct, "");

    c->play = image_at(c->root, wii_img_playmode[0], COL_PLAY_X, COL_PLAY_Y);
    c->repeat = image_at(c->root, wii_img_repeat[0], COL_REPEAT_X, COL_REPEAT_Y);
    c->shuffle = image_at(c->root, wii_img_shuffle[0], COL_SHUFFLE_X, COL_REPEAT_Y);

    for (int i = 0; i < MAX_COLUMNS; i++) {
        if (g_columns[i] == NULL) {
            g_columns[i] = c;
            break;
        }
    }
    lv_obj_add_event_cb(c->root, column_delete_cb, LV_EVENT_DELETE, c);
    column_update(c);
    return c->root;
}

static void columns_update(void)
{
    for (int i = 0; i < MAX_COLUMNS; i++) {
        if (g_columns[i] != NULL) {
            column_update(g_columns[i]);
        }
    }
}

void rpod_wii_set_player(const rpod_mpd_status_t *status)
{
    g_player_known = status != NULL;
    if (status != NULL) {
        g_player = *status;
    }
    columns_update();
}

void rpod_wii_battery_changed(void)
{
    columns_update();
}

/* --- Tiles -------------------------------------------------------------------- */

lv_obj_t *rpod_wii_tile_create(lv_obj_t *parent, rpod_wii_tile_t tile)
{
    if ((int)tile < 0 || tile >= RPOD_WII_TILE_COUNT) {
        tile = RPOD_WII_TILE_FILES;
    }
    lv_obj_t *t = plain(parent, 0, 0, RPOD_WII_TILE_W, RPOD_WII_TILE_H);
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_user_data(t, (void *)(intptr_t)tile);
    image_at(t, wii_img_tile[tile], 0, 0);
    return t;
}

void rpod_wii_tile_set_selected(lv_obj_t *tile, bool selected)
{
    int idx = (int)(intptr_t)lv_obj_get_user_data(tile);
    lv_obj_t *img = lv_obj_get_child(tile, 0);
    if (img != NULL) {
        lv_image_set_src(img, selected ? wii_img_tile_sel[idx] : wii_img_tile[idx]);
    }
}

#else /* !RPOD_WII_ASSETS */

/* Never called: rpod_wii_layout() is false without the art. */

lv_obj_t *rpod_wii_bar_create(lv_obj_t *layer) { (void)layer; return NULL; }
void rpod_wii_bar_set_time(const struct tm *tm) { (void)tm; }
void rpod_wii_bar_set_title(const char *title) { (void)title; }
void rpod_wii_bar_set_locked(bool locked) { (void)locked; }
lv_obj_t *rpod_wii_column_create(lv_obj_t *screen) { (void)screen; return NULL; }
void rpod_wii_set_player(const rpod_mpd_status_t *status) { (void)status; }
void rpod_wii_battery_changed(void) {}
lv_obj_t *rpod_wii_tile_create(lv_obj_t *parent, rpod_wii_tile_t tile) { (void)parent; (void)tile; return NULL; }
void rpod_wii_tile_set_selected(lv_obj_t *tile, bool selected) { (void)tile; (void)selected; }
lv_obj_t *rpod_wii_np_bg_create(lv_obj_t *parent) { (void)parent; return NULL; }
lv_obj_t *rpod_wii_mii_create(lv_obj_t *parent, int which) { (void)parent; (void)which; return NULL; }
lv_obj_t *rpod_wii_frame_create(lv_obj_t *parent, rpod_wii_series_t series, int frame)
{
    (void)parent; (void)series; (void)frame;
    return NULL;
}
void rpod_wii_frame_set(lv_obj_t *img, rpod_wii_series_t series, int frame)
{
    (void)img; (void)series; (void)frame;
}

#endif
