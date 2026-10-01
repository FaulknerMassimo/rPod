#include "bluetooth_screens.h"

#include "list_screen.h"
#include "audio/bluetooth.h"
#include "ui/metrics.h"
#include "ui/theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* --- Live views ----------------------------------------------------------------
 *
 * All three screens are a bt_view_t: a fill function that builds the whole
 * screen from the current BlueZ mirror, rerun (via refresh()) whenever the
 * mirror changes. A rebuild throws the old rows away, so each row carries a
 * key -- a device path, or a fixed tag like "power" -- and refresh() puts the
 * highlight back on the row with the same key afterwards. */

typedef struct bt_view bt_view_t;

/* A row's item_ctx: what it is, for its on_select and for refresh(). */
typedef struct {
    bt_view_t *view;
    char key[64];
} row_ref_t;

struct bt_view {
    rpod_screen_stack_t *stack;
    lv_obj_t *screen;
    lv_group_t *group;
    void (*fill)(bt_view_t *v);
    bool stale; /* changed while another screen was on top */
    bool scan;  /* the search screen: owns the discovery session */

    char path[64];       /* device screen: the device */
    char (*picked)[64];  /* scan screen: devices paired from here, which */
    size_t npicked;      /* stay listed so their progress shows */

    /* Set by fill: the list (NULL for a message-only screen), how many
     * non-row children (headers) precede the rows in it, and each row's
     * ref, in order. */
    lv_obj_t *list;
    size_t row_base;
    row_ref_t *refs;
    size_t nrefs;
};

/* Rows being collected by a fill function. */
typedef struct {
    rpod_list_item_t *items;
    row_ref_t *refs;
    size_t n, cap;
} rows_t;

static rpod_list_item_t *rows_add(rows_t *r, bt_view_t *v, const char *key,
                                  void (*on_select)(rpod_screen_stack_t *, void *))
{
    if (r->n == r->cap) {
        size_t cap = r->cap ? r->cap * 2 : 8;
        rpod_list_item_t *items = realloc(r->items, cap * sizeof(*items));
        if (items == NULL) {
            return NULL;
        }
        r->items = items;
        row_ref_t *refs = realloc(r->refs, cap * sizeof(*refs));
        if (refs == NULL) {
            return NULL;
        }
        r->refs = refs;
        r->cap = cap;
    }
    rpod_list_item_t *it = &r->items[r->n];
    memset(it, 0, sizeof(*it));
    it->on_select = on_select;
    r->refs[r->n].view = v;
    snprintf(r->refs[r->n].key, sizeof(r->refs[r->n].key), "%s", key);
    r->n++;
    return it;
}

/* Hands the collected rows to v->list (built by the caller, headers and
 * all) and keeps their refs on the view. */
static void rows_finish(bt_view_t *v, rows_t *r)
{
    /* item_ctx only now: refs may have moved while rows were added. */
    for (size_t i = 0; i < r->n; i++) {
        r->items[i].item_ctx = &r->refs[i];
    }
    v->row_base = (size_t)lv_obj_get_child_count(v->list);
    if (r->n > 0) {
        rpod_list_screen_populate(v->stack, v->list, r->items, r->n);
    }
    free(r->items);
    v->refs = r->refs;
    v->nrefs = r->n;
}

static void refresh(bt_view_t *v)
{
    /* Which row has the highlight now, by key and by position. */
    char focus_key[64] = "";
    size_t focus_idx = 0;
    lv_obj_t *focused = lv_group_get_focused(v->group);
    if (focused != NULL && v->list != NULL && lv_obj_get_parent(focused) == v->list) {
        int32_t idx = lv_obj_get_index(focused) - (int32_t)v->row_base;
        if (idx >= 0 && (size_t)idx < v->nrefs) {
            focus_idx = (size_t)idx;
            memcpy(focus_key, v->refs[idx].key, sizeof(focus_key));
        }
    }

    /* This may run while another screen (or an overlay) is on top, whose
     * group is the default one -- new rows must join this screen's. */
    lv_group_t *prev_default = lv_group_get_default();
    lv_group_set_default(v->group);

    lv_obj_clean(v->screen);
    free(v->refs);
    v->refs = NULL;
    v->nrefs = 0;
    v->list = NULL;
    v->row_base = 0;
    v->fill(v);

    lv_group_set_default(prev_default);
    v->stale = false;

    /* Same row if it's still there, else whatever took its place. */
    if (v->list == NULL || v->nrefs == 0 || focus_key[0] == '\0') {
        return;
    }
    size_t target = focus_idx < v->nrefs ? focus_idx : v->nrefs - 1;
    for (size_t i = 0; i < v->nrefs; i++) {
        if (strcmp(v->refs[i].key, focus_key) == 0) {
            target = i;
            break;
        }
    }
    lv_group_focus_obj(lv_obj_get_child(v->list, (int32_t)(v->row_base + target)));
}

/* Mirror changed. Only the visible screen rebuilds now; one underneath
 * catches up when it's shown again (view_loaded_cb). */
static void view_changed_cb(void *user)
{
    bt_view_t *v = user;
    if (lv_screen_active() == v->screen) {
        refresh(v);
    } else {
        v->stale = true;
    }
}

static void view_loaded_cb(lv_event_t *e)
{
    bt_view_t *v = lv_event_get_user_data(e);
    if (v->stale) {
        refresh(v);
    }
}

static void view_delete_cb(lv_event_t *e)
{
    bt_view_t *v = lv_event_get_user_data(e);
    if (v->scan) {
        rpod_bt_scan_stop();
    }
    free(v->refs);
    free(v->picked);
    free(v);
}

static bt_view_t *view_create(rpod_screen_stack_t *stack, lv_obj_t *screen, void (*fill)(bt_view_t *))
{
    bt_view_t *v = calloc(1, sizeof(*v));
    v->stack = stack;
    v->screen = screen;
    v->group = lv_group_get_default(); /* the stack made it this screen's */
    v->fill = fill;
    lv_obj_add_event_cb(screen, view_delete_cb, LV_EVENT_DELETE, v);
    lv_obj_add_event_cb(screen, view_loaded_cb, LV_EVENT_SCREEN_LOADED, v);
    rpod_bt_watch(screen, view_changed_cb, v);
    return v;
}

/* --- Shared pieces ------------------------------------------------------------ */

/* A screen with nothing to select, just why (Bluetooth off, no adapter...). */
static void show_message(bt_view_t *v, const char *msg)
{
    const rpod_metrics_t *m = rpod_metrics();
    lv_obj_t *label = lv_label_create(v->screen);
    lv_label_set_text_fmt(label, "Bluetooth\n\n%s", msg);
    lv_obj_set_style_text_color(label, RPOD_COLOR_DIM_TEXT, 0);
    lv_obj_set_width(label, m->screen_w - 40);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, m->header_h / 2);
}

/* Shows why there's nothing to list and returns true, unless Bluetooth is
 * on. */
static bool show_unless_on(bt_view_t *v)
{
    switch (rpod_bt_state()) {
    case RPOD_BT_UNAVAILABLE: show_message(v, "The Bluetooth service isn't running."); return true;
    case RPOD_BT_NO_ADAPTER:  show_message(v, "No Bluetooth adapter found."); return true;
    case RPOD_BT_OFF:         show_message(v, "Bluetooth is off."); return true;
    case RPOD_BT_ON:          break;
    }
    return false;
}

/* Non-selectable block at the top of a list: a title line and/or a dim
 * wrapped note (either may be NULL). */
static void add_header(lv_obj_t *list, const char *title, const char *note)
{
    const rpod_metrics_t *m = rpod_metrics();
    lv_obj_t *header = lv_obj_create(list);
    lv_obj_remove_style_all(header);
    lv_obj_set_width(header, LV_PCT(100));
    lv_obj_set_height(header, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(header, m->row_pad_x, 0);
    lv_obj_set_style_pad_ver(header, 8, 0);
    lv_obj_set_style_pad_row(header, 2, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_border_color(header, RPOD_COLOR_SEPARATOR, 0);
    lv_obj_set_style_border_opa(header, LV_OPA_COVER, 0);

    if (title != NULL) {
        lv_obj_t *label = lv_label_create(header);
        lv_label_set_text(label, title);
        lv_obj_set_style_text_font(label, m->font_body, 0);
        lv_obj_set_style_text_color(label, RPOD_COLOR_TEXT, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(label, LV_PCT(100));
        lv_obj_set_height(label, lv_font_get_line_height(m->font_body));
    }
    if (note != NULL) {
        lv_obj_t *label = lv_label_create(header);
        lv_label_set_text(label, note);
        lv_obj_set_style_text_font(label, m->font_small, 0);
        lv_obj_set_style_text_color(label, RPOD_COLOR_DIM_TEXT, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(label, LV_PCT(100));
    }
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

static const rpod_bt_device_t **collect_devices(bt_view_t *v,
                                                bool (*keep)(bt_view_t *, const rpod_bt_device_t *),
                                                size_t *count)
{
    size_t total = rpod_bt_device_count();
    const rpod_bt_device_t **out = malloc((total ? total : 1) * sizeof(*out));
    size_t n = 0;
    for (size_t i = 0; out != NULL && i < total; i++) {
        const rpod_bt_device_t *d = rpod_bt_device_at(i);
        if (keep(v, d)) {
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

static void on_toggle_connection(rpod_screen_stack_t *stack, void *item_ctx)
{
    (void)stack;
    row_ref_t *ref = item_ctx;
    const rpod_bt_device_t *d = rpod_bt_find(ref->view->path);
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
    row_ref_t *ref = item_ctx;
    rpod_bt_forget(ref->view->path);
    /* Pop deletes this screen and with it `ref` -- nothing after this. */
    rpod_screen_stack_pop(stack);
}

static void fill_device(bt_view_t *v)
{
    const rpod_bt_device_t *d = rpod_bt_find(v->path);
    if (show_unless_on(v)) {
        return;
    }
    if (d == NULL) {
        show_message(v, "This device is no longer available.");
        return;
    }

    v->list = rpod_list_screen_create(v->screen);
    add_header(v->list, d->name, d->error[0] != '\0' ? d->error : status_text(d));

    rows_t rows = { 0 };
    rpod_list_item_t *it = rows_add(&rows, v, "connection", on_toggle_connection);
    if (it != NULL) {
        snprintf(it->text, sizeof(it->text), "%s", d->connected ? "Disconnect" : "Connect");
        const char *op = op_text(d);
        snprintf(it->accessory, sizeof(it->accessory), "%s", op != NULL ? op : "");
    }
    it = rows_add(&rows, v, "forget", on_forget);
    if (it != NULL) {
        snprintf(it->text, sizeof(it->text), "Forget This Device");
    }
    rows_finish(v, &rows);
}

static void build_device_screen(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    bt_view_t *v = view_create(stack, screen, fill_device);
    snprintf(v->path, sizeof(v->path), "%s", (const char *)ctx);
    refresh(v);
}

static void push_device_screen(rpod_screen_stack_t *stack, const char *path)
{
    char *ctx = strdup(path);
    if (ctx != NULL) {
        rpod_screen_stack_push(stack, build_device_screen, ctx, free);
    }
}

/* --- Search screen ------------------------------------------------------------ */

static bool was_picked(bt_view_t *v, const char *path)
{
    for (size_t i = 0; i < v->npicked; i++) {
        if (strcmp(v->picked[i], path) == 0) {
            return true;
        }
    }
    return false;
}

/* Unpaired audio devices -- phones, laptops and TVs have nothing to offer
 * an iPod -- plus whatever was paired from this screen, so its progress
 * stays visible instead of the row vanishing the moment pairing succeeds. */
static bool keep_for_scan(bt_view_t *v, const rpod_bt_device_t *d)
{
    return d->audio && (!d->paired || was_picked(v, d->path));
}

static void on_scan_device(rpod_screen_stack_t *stack, void *item_ctx)
{
    row_ref_t *ref = item_ctx;
    bt_view_t *v = ref->view;
    const rpod_bt_device_t *d = rpod_bt_find(ref->key);
    if (d == NULL) {
        return;
    }
    if (d->paired) {
        push_device_screen(stack, d->path);
        return;
    }
    if (!was_picked(v, d->path)) {
        char (*grown)[64] = realloc(v->picked, (v->npicked + 1) * sizeof(*grown));
        if (grown == NULL) {
            return;
        }
        v->picked = grown;
        snprintf(v->picked[v->npicked++], sizeof(*grown), "%s", d->path);
    }
    rpod_bt_pair(d->path);
}

static void fill_scan(bt_view_t *v)
{
    if (show_unless_on(v)) {
        return;
    }

    size_t n = 0;
    const rpod_bt_device_t **devs = collect_devices(v, keep_for_scan, &n);

    v->list = rpod_list_screen_create(v->screen);
    add_header(v->list, rpod_bt_discovering() ? "Searching..." : "Not searching",
               n == 0 ? "Put your headphones or speaker in pairing mode."
                      : "Select a device to pair with it.");

    rows_t rows = { 0 };
    for (size_t i = 0; i < n; i++) {
        const rpod_bt_device_t *d = devs[i];
        rpod_list_item_t *it = rows_add(&rows, v, d->path, on_scan_device);
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
    rows_finish(v, &rows);
}

static void build_scan_screen(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    (void)ctx;
    bt_view_t *v = view_create(stack, screen, fill_scan);
    v->scan = true;
    rpod_bt_scan_start();
    refresh(v);
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
    row_ref_t *ref = item_ctx;
    push_device_screen(stack, ref->key);
}

static void on_search(rpod_screen_stack_t *stack, void *item_ctx)
{
    (void)item_ctx;
    rpod_screen_stack_push(stack, build_scan_screen, NULL, NULL);
}

static bool keep_paired(bt_view_t *v, const rpod_bt_device_t *d)
{
    (void)v;
    return d->paired;
}

static void fill_main(bt_view_t *v)
{
    /* Unlike the other two screens, Off still lists the power row. */
    rpod_bt_state_t state = rpod_bt_state();
    if (state != RPOD_BT_OFF && show_unless_on(v)) {
        return;
    }

    v->list = rpod_list_screen_create(v->screen);
    rows_t rows = { 0 };

    rpod_list_item_t *it = rows_add(&rows, v, "power", on_power);
    if (it != NULL) {
        snprintf(it->text, sizeof(it->text), "Bluetooth");
        snprintf(it->subtitle, sizeof(it->subtitle), "%s", rpod_bt_adapter_error());
        snprintf(it->accessory, sizeof(it->accessory), "%s",
                 rpod_bt_power_pending() ? "..." : state == RPOD_BT_ON ? "On" : "Off");
    }

    if (state == RPOD_BT_ON) {
        size_t n = 0;
        const rpod_bt_device_t **devs = collect_devices(v, keep_paired, &n);
        for (size_t i = 0; i < n; i++) {
            const rpod_bt_device_t *d = devs[i];
            it = rows_add(&rows, v, d->path, on_paired_device);
            if (it == NULL) {
                break;
            }
            snprintf(it->text, sizeof(it->text), "%s", d->name);
            snprintf(it->subtitle, sizeof(it->subtitle), "%s", d->error);
            snprintf(it->accessory, sizeof(it->accessory), "%s", status_text(d));
            it->chevron = true;
        }
        free(devs);

        it = rows_add(&rows, v, "search", on_search);
        if (it != NULL) {
            snprintf(it->text, sizeof(it->text), "Search for Devices");
            it->chevron = true;
        }
    }
    rows_finish(v, &rows);
}

void rpod_bluetooth_screen_build(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    (void)ctx;
    bt_view_t *v = view_create(stack, screen, fill_main);
    refresh(v);
}
