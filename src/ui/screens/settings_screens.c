#include "settings_screens.h"

#include "airpods_screens.h"
#include "bluetooth_screens.h"
#include "list_screen.h"
#include "live_list.h"
#include "audio/airpods.h"
#include "audio/mpd_client.h"
#include "ui/backlight.h"
#include "ui/metrics.h"
#include "ui/sleep_timer.h"
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
    rpod_list_screen_build(stack, screen, ui_items, count);
    free(ui_items);
}

/* --- Backlight and Sleep Timer ------------------------------------------- */

/* Both are a short list of choices, the current one checked. Row keys are
 * the choice's value, in decimal. */
typedef struct {
    unsigned value;
    const char *label;
} choice_t;

static const choice_t backlight_choices[] = {
    { 10, "10 Seconds" },
    { 20, "20 Seconds" },
    { 30, "30 Seconds" },
    { 60, "1 Minute" },
    { 120, "2 Minutes" },
    { RPOD_BACKLIGHT_ALWAYS_ON, "Always On" },
};

static const choice_t sleep_choices[] = {
    { 0, "Off" },
    { 15, "15 Minutes" },
    { 30, "30 Minutes" },
    { 60, "60 Minutes" },
    { 90, "90 Minutes" },
    { 120, "120 Minutes" },
};

#define CHOICES(a) a, sizeof(a) / sizeof(a[0])

static void add_choices(rpod_live_list_t *ll, const choice_t *choices, size_t count, unsigned current,
                        void (*on_pick)(rpod_screen_stack_t *, void *))
{
    for (size_t i = 0; i < count; i++) {
        char key[16];
        snprintf(key, sizeof(key), "%u", choices[i].value);
        rpod_list_item_t *it = rpod_live_list_add(ll, key, on_pick);
        if (it != NULL) {
            snprintf(it->text, sizeof(it->text), "%s", choices[i].label);
            it->status = choices[i].value == current ? RPOD_ROW_STATUS_CHECK : RPOD_ROW_STATUS_NONE;
        }
    }
}

static unsigned picked(const rpod_live_row_t *ref)
{
    return (unsigned)strtoul(ref->key, NULL, 10);
}

static void on_pick_backlight(rpod_screen_stack_t *stack, void *item_ctx)
{
    (void)stack;
    rpod_live_row_t *ref = item_ctx;
    rpod_backlight_set_timeout_s(picked(ref));
    rpod_live_list_changed(ref->ll);
}

static void fill_backlight(rpod_live_list_t *ll)
{
    rpod_live_list_header(ll, "Backlight", "Turns the screen off after this long without input.");
    add_choices(ll, CHOICES(backlight_choices), rpod_backlight_timeout_s(), on_pick_backlight);
}

static void build_backlight_screen(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    (void)ctx;
    rpod_live_list_create(stack, screen, fill_backlight, NULL, NULL);
}

/* The Sleep Timer screen counts down in its header, rebuilt only when the
 * minutes shown change. */
typedef struct {
    rpod_live_list_t *ll;
    lv_timer_t *timer;
    unsigned shown_min; /* minutes left, rounded up; 0 when off */
} sleep_screen_t;

static unsigned sleep_minutes_left(void)
{
    return (unsigned)((rpod_sleep_timer_remaining_ms() + 59999u) / 60000u);
}

static void on_pick_sleep(rpod_screen_stack_t *stack, void *item_ctx)
{
    (void)stack;
    rpod_live_row_t *ref = item_ctx;
    rpod_sleep_timer_set(picked(ref));
    rpod_live_list_changed(ref->ll);
}

static void fill_sleep(rpod_live_list_t *ll)
{
    sleep_screen_t *ss = rpod_live_list_ctx(ll);
    ss->shown_min = sleep_minutes_left();
    char note[64];
    if (ss->shown_min == 0) {
        snprintf(note, sizeof(note), "Pauses and puts rPod to sleep after this long.");
    } else {
        snprintf(note, sizeof(note), "Sleeping in %u minute%s.", ss->shown_min,
                 ss->shown_min == 1 ? "" : "s");
    }
    rpod_live_list_header(ll, "Sleep Timer", note);
    add_choices(ll, CHOICES(sleep_choices), rpod_sleep_timer_minutes(), on_pick_sleep);
}

static void sleep_tick_cb(lv_timer_t *t)
{
    sleep_screen_t *ss = lv_timer_get_user_data(t);
    if (sleep_minutes_left() != ss->shown_min) {
        rpod_live_list_changed(ss->ll);
    }
}

static void sleep_screen_free(void *ctx)
{
    sleep_screen_t *ss = ctx;
    if (ss->timer != NULL) {
        lv_timer_delete(ss->timer);
    }
    free(ss);
}

static void build_sleep_screen(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    (void)ctx;
    sleep_screen_t *ss = calloc(1, sizeof(*ss));
    if (ss == NULL) {
        return;
    }
    rpod_live_list_t *ll = rpod_live_list_create(stack, screen, fill_sleep, ss, sleep_screen_free);
    if (ll != NULL) { /* else create has freed ss already */
        ss->ll = ll;
        ss->timer = lv_timer_create(sleep_tick_cb, 1000, ss);
    }
}

static void build_placeholder_screen(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    (void)stack;
    const char *title = ctx;

    /* No per-screen header any more (ui/status_bar.h owns the top bar) --
     * this screen's *only* content is the placeholder message, so the
     * setting's name has to live in the body text itself or it's lost
     * entirely (every placeholder would otherwise read identically). */
    char body[128];
    snprintf(body, sizeof(body), "%s\n\nNot available without hardware.", title);

    const rpod_metrics_t *m = rpod_metrics();
    lv_obj_t *label = lv_label_create(screen);
    lv_label_set_text(label, body);
    lv_obj_set_style_text_color(label, RPOD_COLOR_DIM_TEXT, 0);
    lv_obj_set_width(label, m->screen_w - 40);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, m->header_h / 2);
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

    const rpod_metrics_t *m = rpod_metrics();
    lv_obj_t *label = lv_label_create(screen);
    lv_label_set_text(label, body);
    lv_obj_set_width(label, m->screen_w - 40);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 20, m->header_h + 16);
}

static void on_settings_audio_output(rpod_screen_stack_t *stack, void *item_ctx)
{
    rpod_screen_stack_push(stack, build_audio_output_screen, item_ctx, NULL);
}

static void on_settings_bluetooth(rpod_screen_stack_t *stack, void *item_ctx)
{
    (void)item_ctx;
    rpod_screen_stack_push(stack, rpod_bluetooth_screen_build, NULL, NULL);
}

static void on_settings_airpods(rpod_screen_stack_t *stack, void *item_ctx)
{
    (void)item_ctx;
    const rpod_airpods_t *ap = rpod_airpods();
    if (ap->link != RPOD_AIRPODS_ABSENT) {
        rpod_airpods_screen_push(stack, ap->path);
    }
}

static void on_settings_backlight(rpod_screen_stack_t *stack, void *item_ctx)
{
    (void)item_ctx;
    rpod_screen_stack_push(stack, build_backlight_screen, NULL, NULL);
}

static void on_settings_sleep_timer(rpod_screen_stack_t *stack, void *item_ctx)
{
    (void)item_ctx;
    rpod_screen_stack_push(stack, build_sleep_screen, NULL, NULL);
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

    rpod_list_item_t items[7] = { 0 };
    size_t count = 0;

    /* Connected AirPods go on top, the way iOS lists them above everything
     * else in Settings -- one select away from noise control. */
    const rpod_airpods_t *ap = rpod_airpods();
    if (ap->link != RPOD_AIRPODS_ABSENT) {
        rpod_list_item_t *it = &items[count++];
        snprintf(it->text, sizeof(it->text), "%s", ap->name[0] != '\0' ? ap->name : "AirPods");
        rpod_airpods_battery_text(it->subtitle, sizeof(it->subtitle));
        it->chevron = true;
        it->on_select = on_settings_airpods;
    }

    items[count++] = (rpod_list_item_t){ .text = "Audio Output", .chevron = true, .on_select = on_settings_audio_output, .item_ctx = mpd };
    items[count++] = (rpod_list_item_t){ .text = "Bluetooth",    .chevron = true, .on_select = on_settings_bluetooth,    .item_ctx = NULL };
    items[count++] = (rpod_list_item_t){ .text = "Backlight",    .chevron = true, .on_select = on_settings_backlight,    .item_ctx = NULL };
    items[count++] = (rpod_list_item_t){ .text = "Haptics",      .chevron = true, .on_select = on_settings_placeholder,  .item_ctx = "Haptics" };
    items[count++] = (rpod_list_item_t){ .text = "Sleep Timer",  .chevron = true, .on_select = on_settings_sleep_timer, .item_ctx = NULL };
    items[count++] = (rpod_list_item_t){ .text = "About",        .chevron = true, .on_select = on_settings_about,        .item_ctx = mpd };
    rpod_list_screen_build(stack, screen, items, count);
}
