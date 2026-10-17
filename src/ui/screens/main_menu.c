#include "main_menu.h"

#include "extras_screens.h"
#include "list_screen.h"
#include "music_screens.h"
#include "now_playing.h"
#include "settings_screens.h"
#include "video_screens.h"
#include "audio/mpd_client.h"
#include "ui/wii.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static void on_main_menu_music(rpod_screen_stack_t *stack, void *item_ctx)
{
    rpod_screen_stack_push(stack, rpod_music_menu_build, item_ctx, NULL);
}

static void on_main_menu_videos(rpod_screen_stack_t *stack, void *item_ctx)
{
    rpod_videos_push(stack, item_ctx);
}

static void on_main_menu_now_playing(rpod_screen_stack_t *stack, void *item_ctx)
{
    rpod_screen_stack_push(stack, rpod_now_playing_build, item_ctx, NULL);
}

static void on_main_menu_settings(rpod_screen_stack_t *stack, void *item_ctx)
{
    rpod_screen_stack_push(stack, rpod_settings_menu_build, item_ctx, NULL);
}

static void on_main_menu_extras(rpod_screen_stack_t *stack, void *item_ctx)
{
    rpod_screen_stack_push(stack, rpod_extras_menu_build, item_ctx, NULL);
}

/* --- The Wii's tile menu (ui/wii.h) ------------------------------------------
 *
 * The same items as tiles in a 3-wide grid. The one the wheel is on has the
 * teal frame, and its name is the bar's title, as the Wii's channels' are --
 * the screen's own title, so it's still there on the way back. The art has
 * nine tiles with their Rockbox names baked in, so each item takes the nearest
 * in meaning. */

typedef struct {
    rpod_screen_stack_t *stack;
    rpod_list_item_t item;
} tile_ctx_t;

static void tile_free_cb(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

static void tile_click_cb(lv_event_t *e)
{
    tile_ctx_t *t = lv_event_get_user_data(e);
    if (t->item.on_select == NULL) {
        return;
    }
    /* As a list row does (list_screen.c): the item's name titles whatever it
     * pushes, and it may delete the tile, so nothing of `t` is read after. */
    rpod_screen_stack_t *stack = t->stack;
    rpod_screen_stack_next_title(stack, t->item.text);
    t->item.on_select(stack, t->item.item_ctx);
    rpod_screen_stack_next_title(stack, NULL);
}

static void tile_focus_cb(lv_event_t *e)
{
    lv_obj_t *tile = lv_event_get_target(e);
    tile_ctx_t *t = lv_event_get_user_data(e);
    rpod_wii_tile_set_selected(tile, true);
    /* Its own frame's title, which a rebuild (a theme change) restores focus
     * on while the screen isn't the top one. */
    rpod_screen_stack_set_title_of(t->stack, lv_obj_get_screen(tile), t->item.text);
}

static void tile_defocus_cb(lv_event_t *e)
{
    rpod_wii_tile_set_selected(lv_event_get_target(e), false);
}

static void build_tiles(rpod_screen_stack_t *stack, lv_obj_t *screen, const rpod_list_item_t *items,
                        const rpod_wii_tile_t *kinds, size_t count)
{
    lv_obj_t *grid = lv_obj_create(screen);
    lv_obj_remove_style_all(grid);
    lv_obj_set_pos(grid, RPOD_WII_TILE_X, RPOD_WII_TILE_Y);
    lv_obj_set_size(grid, RPOD_WII_TILE_COLS * RPOD_WII_TILE_W,
                    (int32_t)((count + RPOD_WII_TILE_COLS - 1) / RPOD_WII_TILE_COLS) * RPOD_WII_TILE_H);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_group_t *group = lv_group_get_default();
    for (size_t i = 0; i < count; i++) {
        tile_ctx_t *t = malloc(sizeof(*t));
        t->stack = stack;
        t->item = items[i];

        lv_obj_t *tile = rpod_wii_tile_create(grid, kinds[i]);
        lv_obj_set_pos(tile, (int32_t)(i % RPOD_WII_TILE_COLS) * RPOD_WII_TILE_W,
                       (int32_t)(i / RPOD_WII_TILE_COLS) * RPOD_WII_TILE_H);
        lv_obj_add_event_cb(tile, tile_click_cb, LV_EVENT_CLICKED, t);
        lv_obj_add_event_cb(tile, tile_focus_cb, LV_EVENT_FOCUSED, t);
        lv_obj_add_event_cb(tile, tile_defocus_cb, LV_EVENT_DEFOCUSED, t);
        lv_obj_add_event_cb(tile, tile_free_cb, LV_EVENT_DELETE, t);
        lv_group_add_obj(group, tile);
        if (i == 0) {
            /* The group's first object is focused without an event. */
            rpod_wii_tile_set_selected(tile, true);
            rpod_screen_stack_set_title_of(stack, screen, t->item.text);
        }
    }
}

void rpod_main_menu_build(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    (void)stack;
    rpod_mpd_t *mpd = ctx;

    rpod_mpd_status_t status;
    bool show_now_playing = rpod_mpd_get_status(mpd, &status) && status.title[0] != '\0';

    rpod_list_item_t items[5];
    rpod_wii_tile_t kinds[5]; /* the Wii theme's tile for each */
    size_t count = 0;

    kinds[count] = RPOD_WII_TILE_FILES;
    items[count++] = (rpod_list_item_t){
        .text = "Music", .chevron = true, .on_select = on_main_menu_music, .item_ctx = mpd
    };
    kinds[count] = RPOD_WII_TILE_RECORDING;
    items[count++] = (rpod_list_item_t){
        .text = "Videos", .chevron = true, .on_select = on_main_menu_videos, .item_ctx = mpd
    };
    if (show_now_playing) {
        kinds[count] = RPOD_WII_TILE_NOWPLAYING;
        items[count++] = (rpod_list_item_t){
            .text = "Now Playing", .chevron = true, .on_select = on_main_menu_now_playing, .item_ctx = mpd
        };
    }
    kinds[count] = RPOD_WII_TILE_SETTINGS;
    items[count++] = (rpod_list_item_t){
        .text = "Settings", .chevron = true, .on_select = on_main_menu_settings, .item_ctx = mpd
    };
    kinds[count] = RPOD_WII_TILE_PLUGINS;
    items[count++] = (rpod_list_item_t){
        .text = "Extras", .chevron = true, .on_select = on_main_menu_extras, .item_ctx = mpd
    };

    if (rpod_wii_layout()) {
        build_tiles(stack, screen, items, kinds, count);
        /* The playback panels, as beside any list. */
        rpod_wii_column_create(screen);
        return;
    }
    rpod_list_screen_build(stack, screen, items, count);
}
