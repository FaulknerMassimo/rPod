#include "main_menu.h"

#include "extras_screens.h"
#include "list_screen.h"
#include "music_screens.h"
#include "now_playing.h"
#include "settings_screens.h"
#include "video_screens.h"
#include "audio/mpd_client.h"

#include <stdbool.h>

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

void rpod_main_menu_build(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    (void)stack;
    rpod_mpd_t *mpd = ctx;

    rpod_mpd_status_t status;
    bool show_now_playing = rpod_mpd_get_status(mpd, &status) && status.title[0] != '\0';

    rpod_list_item_t items[5];
    size_t count = 0;

    items[count++] = (rpod_list_item_t){
        .text = "Music", .chevron = true, .on_select = on_main_menu_music, .item_ctx = mpd
    };
    items[count++] = (rpod_list_item_t){
        .text = "Videos", .chevron = true, .on_select = on_main_menu_videos, .item_ctx = mpd
    };
    if (show_now_playing) {
        items[count++] = (rpod_list_item_t){
            .text = "Now Playing", .chevron = true, .on_select = on_main_menu_now_playing, .item_ctx = mpd
        };
    }
    items[count++] = (rpod_list_item_t){
        .text = "Settings", .chevron = true, .on_select = on_main_menu_settings, .item_ctx = mpd
    };
    items[count++] = (rpod_list_item_t){
        .text = "Extras", .chevron = true, .on_select = on_main_menu_extras, .item_ctx = mpd
    };

    rpod_list_screen_build(stack, screen, items, count);
}
