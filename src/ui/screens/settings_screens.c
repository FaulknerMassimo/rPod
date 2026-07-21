#include "settings_screens.h"

#include "list_screen.h"
#include "audio/mpd_client.h"
#include "ui/theme.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>

static void build_audio_output_screen(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx);

typedef struct {
    rpod_mpd_t *mpd;
    unsigned id;
    bool enabled;
} output_row_t;

typedef struct {
    rpod_mpd_output_t *outputs;
    output_row_t *rows;
} output_fetch_t;

static void output_fetch_cleanup_cb(lv_event_t *e)
{
    output_fetch_t *fetch = lv_event_get_user_data(e);
    rpod_mpd_free_outputs(fetch->outputs);
    free(fetch->rows);
    free(fetch);
}

static void on_output_toggle(rpod_screen_stack_t *stack, void *item_ctx)
{
    output_row_t *row = item_ctx;
    rpod_mpd_t *mpd = row->mpd; /* copy before pop -- pop deletes the
                                  * screen, which frees `row` via
                                  * output_fetch_cleanup_cb, so `row` is
                                  * dangling immediately after */
    if (row->enabled) {
        rpod_mpd_disable_output(mpd, row->id);
    } else {
        rpod_mpd_enable_output(mpd, row->id);
    }
    /* No in-place refresh mechanism on list_screen -- pop and rebuild is
     * simplest and this screen is cheap to rebuild. */
    rpod_screen_stack_pop(stack);
    rpod_screen_stack_push(stack, build_audio_output_screen, mpd, NULL);
}

static void build_audio_output_screen(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    rpod_mpd_t *mpd = ctx;

    rpod_mpd_output_t *outputs = NULL;
    size_t count = 0;
    rpod_mpd_list_outputs(mpd, &outputs, &count);

    output_fetch_t *fetch = malloc(sizeof(*fetch));
    fetch->outputs = outputs;
    fetch->rows = count > 0 ? malloc(count * sizeof(*fetch->rows)) : NULL;
    for (size_t i = 0; i < count; i++) {
        fetch->rows[i].mpd = mpd;
        fetch->rows[i].id = outputs[i].id;
        fetch->rows[i].enabled = outputs[i].enabled;
    }
    lv_obj_add_event_cb(screen, output_fetch_cleanup_cb, LV_EVENT_DELETE, fetch);

    rpod_list_item_t *ui_items = count > 0 ? calloc(count, sizeof(*ui_items)) : NULL;
    for (size_t i = 0; i < count; i++) {
        snprintf(ui_items[i].text, sizeof(ui_items[i].text), "%s", outputs[i].name);
        snprintf(ui_items[i].accessory, sizeof(ui_items[i].accessory), "%s",
                  outputs[i].enabled ? "On" : "Off");
        ui_items[i].on_select = on_output_toggle;
        ui_items[i].item_ctx = &fetch->rows[i];
    }
    rpod_list_screen_build(stack, screen, "Audio Output", ui_items, count);
    free(ui_items);
}

static void build_placeholder_screen(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    (void)stack;
    const char *title = ctx;

    rpod_theme_create_header(screen, title);

    lv_obj_t *label = lv_label_create(screen);
    lv_label_set_text(label, "Not available without hardware.");
    lv_obj_set_style_text_color(label, RPOD_COLOR_DIM_TEXT, 0);
    lv_obj_set_width(label, RPOD_SCREEN_WIDTH - 40);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, RPOD_HEADER_HEIGHT / 2);
}

static void get_local_ip(char *out, size_t out_size)
{
    snprintf(out, out_size, "N/A");

    struct ifaddrs *ifaddr;
    if (getifaddrs(&ifaddr) != 0) {
        return;
    }
    for (struct ifaddrs *ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if (strcmp(ifa->ifa_name, "lo") == 0) {
            continue;
        }
        const struct sockaddr_in *sin = (const struct sockaddr_in *)(void *)ifa->ifa_addr;
        inet_ntop(AF_INET, &sin->sin_addr, out, out_size);
        break;
    }
    freeifaddrs(ifaddr);
}

static void build_about_screen(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    (void)stack;
    (void)ctx;

    rpod_theme_create_header(screen, "About");

    char storage[64] = "N/A";
    struct statvfs vfs;
    if (statvfs("/", &vfs) == 0) {
        double free_gb = (double)(vfs.f_bavail * vfs.f_frsize) / (1024.0 * 1024.0 * 1024.0);
        double total_gb = (double)(vfs.f_blocks * vfs.f_frsize) / (1024.0 * 1024.0 * 1024.0);
        snprintf(storage, sizeof(storage), "%.1f / %.1f GB free", free_gb, total_gb);
    }

    char ip[64];
    get_local_ip(ip, sizeof(ip));

    char body[512];
    snprintf(body, sizeof(body),
              "rPod dev build\n\n"
              "Storage: %s\n"
              "Battery: N/A (no fuel gauge)\n"
              "IP: %s",
              storage, ip);

    lv_obj_t *label = lv_label_create(screen);
    lv_label_set_text(label, body);
    lv_obj_set_width(label, RPOD_SCREEN_WIDTH - 40);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 20, RPOD_HEADER_HEIGHT + 16);
}

static void on_settings_audio_output(rpod_screen_stack_t *stack, void *item_ctx)
{
    rpod_screen_stack_push(stack, build_audio_output_screen, item_ctx, NULL);
}

static void on_settings_placeholder(rpod_screen_stack_t *stack, void *item_ctx)
{
    rpod_screen_stack_push(stack, build_placeholder_screen, item_ctx, NULL);
}

static void on_settings_about(rpod_screen_stack_t *stack, void *item_ctx)
{
    (void)item_ctx;
    rpod_screen_stack_push(stack, build_about_screen, NULL, NULL);
}

void rpod_settings_menu_build(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    (void)stack;
    rpod_mpd_t *mpd = ctx;

    rpod_list_item_t items[] = {
        { .text = "Audio Output", .chevron = true, .on_select = on_settings_audio_output, .item_ctx = mpd },
        { .text = "Bluetooth",    .chevron = true, .on_select = on_settings_placeholder,  .item_ctx = "Bluetooth" },
        { .text = "Backlight",    .chevron = true, .on_select = on_settings_placeholder,  .item_ctx = "Backlight" },
        { .text = "Haptics",      .chevron = true, .on_select = on_settings_placeholder,  .item_ctx = "Haptics" },
        { .text = "Sleep Timer",  .chevron = true, .on_select = on_settings_placeholder,  .item_ctx = "Sleep Timer" },
        { .text = "About",        .chevron = true, .on_select = on_settings_about,        .item_ctx = mpd },
    };
    rpod_list_screen_build(stack, screen, "Settings", items, sizeof(items) / sizeof(items[0]));
}
