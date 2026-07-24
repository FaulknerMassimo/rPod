#include "main_menu.h"

#include "list_screen.h"
#include "music_screens.h"
#include "now_playing.h"
#include "settings_screens.h"
#include "audio/mpd_client.h"
#include "ui/metrics.h"
#include "ui/theme.h"

#include <stdbool.h>

static void on_main_menu_music(rpod_screen_stack_t *stack, void *item_ctx)
{
    rpod_screen_stack_push(stack, rpod_music_menu_build, item_ctx, NULL);
}

static void on_main_menu_now_playing(rpod_screen_stack_t *stack, void *item_ctx)
{
    rpod_screen_stack_push(stack, rpod_now_playing_build, item_ctx, NULL);
}

static void on_main_menu_settings(rpod_screen_stack_t *stack, void *item_ctx)
{
    rpod_screen_stack_push(stack, rpod_settings_menu_build, item_ctx, NULL);
}

static void build_extras_screen(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    (void)stack;
    (void)ctx;

    const rpod_metrics_t *m = rpod_metrics();
    lv_obj_t *label = lv_label_create(screen);
    lv_label_set_text(label, "Reserved for future use (docs/PLAN.md section 11).");
    lv_obj_set_style_text_color(label, RPOD_COLOR_DIM_TEXT, 0);
    lv_obj_set_width(label, m->screen_w - 40);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, m->header_h / 2);
}

static void on_main_menu_extras(rpod_screen_stack_t *stack, void *item_ctx)
{
    (void)item_ctx;
    rpod_screen_stack_push(stack, build_extras_screen, NULL, NULL);
}

void rpod_main_menu_build(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    (void)stack;
    rpod_mpd_t *mpd = ctx;

    rpod_mpd_status_t status;
    bool show_now_playing = rpod_mpd_get_status(mpd, &status) && status.title[0] != '\0';

    rpod_list_item_t items[4];
    size_t count = 0;

    items[count++] = (rpod_list_item_t){
        .text = "Music", .chevron = true, .on_select = on_main_menu_music, .item_ctx = mpd
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
