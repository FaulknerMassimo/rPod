#include "bluetooth_screens.h"

#include "airpods_screens.h"
#include "live_list.h"
#include "audio/bluetooth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* All three screens are live lists (live_list.h) rebuilt from the BlueZ
 * mirror whenever it changes. Rows are keyed by device path, or a fixed tag
 * like "power". */

/* --- Shared pieces ------------------------------------------------------------ */

static rpod_live_list_t *bt_list_create(rpod_screen_stack_t *stack, lv_obj_t *screen,
                                        rpod_live_fill_fn fill, void *ctx, void (*ctx_free)(void *))
{
    rpod_live_list_t *ll = rpod_live_list_create(stack, screen, fill, ctx, ctx_free);
    if (ll != NULL) {
        rpod_bt_watch(screen, rpod_live_list_changed, ll);
    }
    return ll;
}

/* Shows why there's nothing to list and returns true, unless Bluetooth is
 * on. */
static bool show_unless_on(rpod_live_list_t *ll)
{
    switch (rpod_bt_state()) {
    case RPOD_BT_UNAVAILABLE: rpod_live_list_message(ll, "Bluetooth", "The Bluetooth service isn't running."); return true;
    case RPOD_BT_NO_ADAPTER:  rpod_live_list_message(ll, "Bluetooth", "No Bluetooth adapter found."); return true;
    case RPOD_BT_OFF:         rpod_live_list_message(ll, "Bluetooth", "Bluetooth is off."); return true;
    case RPOD_BT_ON:          break;
    }
    return false;
}

static const char *op_text(const rpod_bt_device_t *d)
{
    switch (d->op) {
    case RPOD_BT_OP_PAIRING:       return "Pairing...";
    case RPOD_BT_OP_CONNECTING:    return "Connecting...";
    case RPOD_BT_OP_DISCONNECTING: return "Disconnecting...";
    case RPOD_BT_OP_NONE:          break;
    }
    return NULL;
}

/* A pairing BlueZ didn't keep the key for (made before rPod had a pairing
 * agent): the device can't reconnect until it's paired again. */
#define UNSAVED_PAIRING "Not saved - forget, then pair again"

/* The row's second line: the last failure, else an unsaved pairing. */
static const char *subtitle_text(const rpod_bt_device_t *d)
{
    if (d->error[0] != '\0') {
        return d->error;
    }
    return d->paired && !d->bonded ? UNSAVED_PAIRING : "";
}

/* "Connected" / "Not Connected", or what's in progress. */
static const char *status_text(const rpod_bt_device_t *d)
{
    const char *op = op_text(d);
    if (op != NULL) {
        return op;
    }
    return d->connected ? "Connected" : "Not Connected";
}

/* The devices passing `keep`, sorted for display: named before bare
 * addresses, then alphabetically. Pointers are into the mirror, so only
 * good for the rest of this fill. Caller frees the array. */
static int device_cmp(const void *a, const void *b)
{
    const rpod_bt_device_t *x = *(const rpod_bt_device_t *const *)a;
    const rpod_bt_device_t *y = *(const rpod_bt_device_t *const *)b;
    if (x->named != y->named) {
        return x->named ? -1 : 1;
    }
    return strcasecmp(x->name, y->name);
}

static const rpod_bt_device_t **collect_devices(rpod_live_list_t *ll,
                                                bool (*keep)(rpod_live_list_t *, const rpod_bt_device_t *),
                                                size_t *count)
{
    size_t total = rpod_bt_device_count();
    const rpod_bt_device_t **out = malloc((total ? total : 1) * sizeof(*out));
    size_t n = 0;
    for (size_t i = 0; out != NULL && i < total; i++) {
        const rpod_bt_device_t *d = rpod_bt_device_at(i);
        if (keep(ll, d)) {
            out[n++] = d;
        }
    }
    if (out != NULL) {
        qsort(out, n, sizeof(*out), device_cmp);
    }
    *count = n;
    return out;
}

/* --- Device screen: Connect/Disconnect, Forget -------------------------------- */

/* The device screen's ctx is the device's path. */

static void on_toggle_connection(rpod_screen_stack_t *stack, void *item_ctx)
{
    (void)stack;
    rpod_live_row_t *ref = item_ctx;
    const rpod_bt_device_t *d = rpod_bt_find(rpod_live_list_ctx(ref->ll));
    if (d == NULL) {
        return;
    }
    if (d->connected) {
        rpod_bt_disconnect(d->path);
    } else {
        rpod_bt_connect(d->path);
    }
}

static void on_forget(rpod_screen_stack_t *stack, void *item_ctx)
{
    rpod_live_row_t *ref = item_ctx;
    rpod_bt_forget(rpod_live_list_ctx(ref->ll));
    /* Pop deletes this screen and with it `ref` -- nothing after this. */
    rpod_screen_stack_pop(stack);
}

static void fill_device(rpod_live_list_t *ll)
{
    const rpod_bt_device_t *d = rpod_bt_find(rpod_live_list_ctx(ll));
    if (show_unless_on(ll)) {
        return;
    }
    if (d == NULL) {
        rpod_live_list_message(ll, "Bluetooth", "This device is no longer available.");
        return;
    }

    rpod_live_list_header(ll, d->name, d->error[0] != '\0' ? d->error : status_text(d));
    if (d->paired && !d->bonded) {
        rpod_live_list_header(ll, NULL,
                              "This pairing wasn't saved, so it won't reconnect by itself. "
                              "Forget it, then pair it again.");
    }

    rpod_list_item_t *it = rpod_live_list_add(ll, "connection", on_toggle_connection);
    if (it != NULL) {
        snprintf(it->text, sizeof(it->text), "%s", d->connected ? "Disconnect" : "Connect");
        const char *op = op_text(d);
        snprintf(it->accessory, sizeof(it->accessory), "%s", op != NULL ? op : "");
    }
    it = rpod_live_list_add(ll, "forget", on_forget);
    if (it != NULL) {
        snprintf(it->text, sizeof(it->text), "Forget This Device");
    }
}

static void build_device_screen(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    char *path = strdup((const char *)ctx);
    if (path != NULL) {
        bt_list_create(stack, screen, fill_device, path, free);
    }
}

/* AirPods (anything speaking AAP) get their own settings page instead. */
static void push_device_screen(rpod_screen_stack_t *stack, const rpod_bt_device_t *d)
{
    if (d->aap) {
        rpod_airpods_screen_push(stack, d->path);
        return;
    }
    char *ctx = strdup(d->path);
    if (ctx != NULL) {
        rpod_screen_stack_push(stack, build_device_screen, ctx, free);
    }
}

/* --- Search screen ------------------------------------------------------------ */

/* Devices paired from this screen, which stay listed so their progress
 * shows. */
typedef struct {
    char (*picked)[64];
    size_t npicked;
} scan_ctx_t;

static void scan_ctx_free(void *ctx)
{
    scan_ctx_t *s = ctx;
    rpod_bt_scan_stop();
    free(s->picked);
    free(s);
}

static bool was_picked(rpod_live_list_t *ll, const char *path)
{
    scan_ctx_t *s = rpod_live_list_ctx(ll);
    for (size_t i = 0; i < s->npicked; i++) {
        if (strcmp(s->picked[i], path) == 0) {
            return true;
        }
    }
    return false;
}

/* Unpaired audio devices -- phones, laptops and TVs have nothing to offer
 * an iPod -- plus whatever was paired from this screen, so its progress
 * stays visible instead of the row vanishing the moment pairing succeeds. */
static bool keep_for_scan(rpod_live_list_t *ll, const rpod_bt_device_t *d)
{
    return d->audio && (!d->paired || was_picked(ll, d->path));
}

static void on_scan_device(rpod_screen_stack_t *stack, void *item_ctx)
{
    rpod_live_row_t *ref = item_ctx;
    scan_ctx_t *s = rpod_live_list_ctx(ref->ll);
    const rpod_bt_device_t *d = rpod_bt_find(ref->key);
    if (d == NULL) {
        return;
    }
    if (d->paired) {
        push_device_screen(stack, d);
        return;
    }
    if (!was_picked(ref->ll, d->path)) {
        char (*grown)[64] = realloc(s->picked, (s->npicked + 1) * sizeof(*grown));
        if (grown == NULL) {
            return;
        }
        s->picked = grown;
        snprintf(s->picked[s->npicked++], sizeof(*grown), "%s", d->path);
    }
    rpod_bt_pair(d->path);
}

static void fill_scan(rpod_live_list_t *ll)
{
    if (show_unless_on(ll)) {
        return;
    }

    size_t n = 0;
    const rpod_bt_device_t **devs = collect_devices(ll, keep_for_scan, &n);

    rpod_live_list_header(ll, rpod_bt_discovering() ? "Searching..." : "Not searching",
                          n == 0 ? "Put your headphones or speaker in pairing mode."
                                 : "Select a device to pair with it.");

    for (size_t i = 0; i < n; i++) {
        const rpod_bt_device_t *d = devs[i];
        rpod_list_item_t *it = rpod_live_list_add(ll, d->path, on_scan_device);
        if (it == NULL) {
            break;
        }
        snprintf(it->text, sizeof(it->text), "%s", d->name);
        snprintf(it->subtitle, sizeof(it->subtitle), "%s", d->error);
        const char *op = op_text(d);
        snprintf(it->accessory, sizeof(it->accessory), "%s",
                 op != NULL ? op : d->connected ? "Connected" : d->paired ? "Paired" : "");
    }
    free(devs);
}

static void build_scan_screen(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    (void)ctx;
    scan_ctx_t *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return;
    }
    rpod_bt_scan_start();
    bt_list_create(stack, screen, fill_scan, s, scan_ctx_free);
}

/* --- Main Bluetooth screen ----------------------------------------------------- */

static void on_power(rpod_screen_stack_t *stack, void *item_ctx)
{
    (void)stack;
    (void)item_ctx;
    rpod_bt_set_powered(rpod_bt_state() != RPOD_BT_ON);
}

static void on_paired_device(rpod_screen_stack_t *stack, void *item_ctx)
{
    rpod_live_row_t *ref = item_ctx;
    const rpod_bt_device_t *d = rpod_bt_find(ref->key);
    if (d != NULL) {
        push_device_screen(stack, d);
    }
}

static void on_search(rpod_screen_stack_t *stack, void *item_ctx)
{
    (void)item_ctx;
    rpod_screen_stack_push(stack, build_scan_screen, NULL, NULL);
}

static bool keep_paired(rpod_live_list_t *ll, const rpod_bt_device_t *d)
{
    (void)ll;
    return d->paired;
}

static void fill_main(rpod_live_list_t *ll)
{
    /* Unlike the other two screens, Off still lists the power row. */
    rpod_bt_state_t state = rpod_bt_state();
    if (state != RPOD_BT_OFF && show_unless_on(ll)) {
        return;
    }

    rpod_list_item_t *it = rpod_live_list_add(ll, "power", on_power);
    if (it != NULL) {
        snprintf(it->text, sizeof(it->text), "Bluetooth");
        snprintf(it->subtitle, sizeof(it->subtitle), "%s", rpod_bt_adapter_error());
        snprintf(it->accessory, sizeof(it->accessory), "%s",
                 rpod_bt_power_pending() ? "..." : state == RPOD_BT_ON ? "On" : "Off");
    }

    if (state == RPOD_BT_ON) {
        size_t n = 0;
        const rpod_bt_device_t **devs = collect_devices(ll, keep_paired, &n);
        for (size_t i = 0; i < n; i++) {
            const rpod_bt_device_t *d = devs[i];
            it = rpod_live_list_add(ll, d->path, on_paired_device);
            if (it == NULL) {
                break;
            }
            snprintf(it->text, sizeof(it->text), "%s", d->name);
            snprintf(it->subtitle, sizeof(it->subtitle), "%s", subtitle_text(d));
            snprintf(it->accessory, sizeof(it->accessory), "%s", status_text(d));
            it->chevron = true;
        }
        free(devs);

        it = rpod_live_list_add(ll, "search", on_search);
        if (it != NULL) {
            snprintf(it->text, sizeof(it->text), "Search for Devices");
            it->chevron = true;
        }
    }
}

void rpod_bluetooth_screen_build(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    (void)ctx;
    bt_list_create(stack, screen, fill_main, NULL, NULL);
}
