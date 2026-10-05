#include "airpods_screens.h"

#include "live_list.h"
#include "audio/aap.h"
#include "audio/airpods.h"
#include "audio/bluetooth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Every screen here is a live list (live_list.h) over audio/airpods.h -- and
 * BlueZ, for the connection rows -- with the AirPods' BlueZ path (and, for a
 * picker, which setting it picks) as its ctx. Row keys carry what a row acts
 * on, in hex ("ctl-28", "mode-3", "opt-32"), so actions need nothing else. */

typedef struct {
    uint8_t value;
    const char *label;
} option_t;

/* A setting picked from a short list. */
typedef struct {
    uint8_t id;
    const char *title;
    const char *note;
    const option_t *options;
    size_t count;
} picker_t;

static const option_t strength_options[] = {
    { 0, "More Noise" },
    { 25, "Slightly More Noise" },
    { 50, "Default" },
    { 75, "Slightly Less Noise" },
    { 100, "Less Noise" },
};
static const option_t press_options[] = { { 0, "Default" }, { 1, "Slower" }, { 2, "Slowest" } };
static const option_t swipe_options[] = { { 1, "Default" }, { 2, "Longer" }, { 3, "Longest" } };

#define OPTIONS(a) a, sizeof(a) / sizeof(a[0])

static const picker_t pickers[] = {
    { RPOD_AAP_CTL_ADAPTIVE_STRENGTH, "Adaptive Audio",
      "How much outside sound Adaptive lets through.", OPTIONS(strength_options) },
    { RPOD_AAP_CTL_PRESS_SPEED, "Press Speed",
      "How quickly you need to press twice or three times.", OPTIONS(press_options) },
    { RPOD_AAP_CTL_HOLD_DURATION, "Press and Hold Duration",
      "How long you need to press and hold.", OPTIONS(press_options) },
    { RPOD_AAP_CTL_SWIPE_SPEED, "Volume Swipe Speed",
      "How long to wait between swipes, so the volume doesn't change by accident.",
      OPTIONS(swipe_options) },
};

/* Noise control modes in iOS's order. */
static const struct {
    uint8_t mode;
    const char *label;
} modes[] = {
    { RPOD_AAP_MODE_ANC, "Noise Cancellation" },
    { RPOD_AAP_MODE_ADAPTIVE, "Adaptive" },
    { RPOD_AAP_MODE_TRANSPARENCY, "Transparency" },
    { RPOD_AAP_MODE_OFF, "Off" },
};

typedef struct {
    char path[64];
    const picker_t *picker; /* picker screens only */
    /* Main screen: the link state it last showed (-1 = no session), so the
     * highlight starts over at the top when that changes. */
    int shown_link;
    bool built;
} ap_ctx_t;

/* --- Shared pieces ------------------------------------------------------------ */

static void ap_list_create(rpod_screen_stack_t *stack, lv_obj_t *screen, rpod_live_fill_fn fill,
                           const ap_ctx_t *src)
{
    ap_ctx_t *c = malloc(sizeof(*c));
    if (c == NULL) {
        return;
    }
    *c = *src;
    rpod_live_list_t *ll = rpod_live_list_create(stack, screen, fill, c, free);
    if (ll != NULL) {
        rpod_airpods_watch(screen, rpod_live_list_changed, ll);
        rpod_bt_watch(screen, rpod_live_list_changed, ll);
    }
}

static void push(rpod_screen_stack_t *stack, rpod_screen_build_fn build, const char *path,
                 const picker_t *picker)
{
    ap_ctx_t *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return;
    }
    snprintf(c->path, sizeof(c->path), "%s", path);
    c->picker = picker;
    rpod_screen_stack_push(stack, build, c, free);
}

/* The control session, if it's for this screen's AirPods. */
static const rpod_airpods_t *session_for(rpod_live_list_t *ll)
{
    const ap_ctx_t *c = rpod_live_list_ctx(ll);
    const rpod_airpods_t *ap = rpod_airpods();
    return ap->link != RPOD_AIRPODS_ABSENT && strcmp(ap->path, c->path) == 0 ? ap : NULL;
}

static bool ready(rpod_live_list_t *ll)
{
    const rpod_airpods_t *ap = session_for(ll);
    return ap != NULL && ap->link == RPOD_AIRPODS_READY;
}

/* What follows the dash in a row's key. */
static unsigned key_value(const rpod_live_row_t *ref)
{
    const char *dash = strchr(ref->key, '-');
    return dash != NULL ? (unsigned)strtoul(dash + 1, NULL, 16) : 0;
}

static rpod_list_item_t *add_row(rpod_live_list_t *ll, const char *prefix, unsigned value,
                                 void (*on_select)(rpod_screen_stack_t *, void *))
{
    char key[16];
    snprintf(key, sizeof(key), "%s-%x", prefix, value);
    return rpod_live_list_add(ll, key, on_select);
}

void rpod_airpods_battery_text(char *out, size_t out_size)
{
    const rpod_airpods_t *ap = rpod_airpods();
    const struct {
        const char *label;
        const rpod_airpods_battery_t *b;
    } parts[] = {
        { "Battery", &ap->headset },
        { "L", &ap->left },
        { "R", &ap->right },
        { "Case", &ap->charging_case },
    };
    size_t n = 0;
    out[0] = '\0';
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]) && n < out_size; i++) {
        if (parts[i].b->level < 0) {
            continue;
        }
        n += (size_t)snprintf(out + n, out_size - n, "%s%s %d%%%s", n > 0 ? "   " : "",
                              parts[i].label, parts[i].b->level,
                              parts[i].b->charging ? LV_SYMBOL_CHARGE : "");
    }
}

/* --- Settings rows --------------------------------------------------------------- */

static void on_toggle(rpod_screen_stack_t *stack, void *item_ctx)
{
    (void)stack;
    uint8_t id = (uint8_t)key_value(item_ctx);
    uint8_t v;
    if (rpod_airpods_get(id, &v)) {
        rpod_airpods_set(id, v == RPOD_AAP_ON ? RPOD_AAP_OFF : RPOD_AAP_ON);
    }
}

/* An On/Off row, if these AirPods have the setting. */
static void add_toggle(rpod_live_list_t *ll, uint8_t id, const char *text, const char *subtitle)
{
    uint8_t v;
    if (!rpod_airpods_get(id, &v)) {
        return;
    }
    rpod_list_item_t *it = add_row(ll, "ctl", id, on_toggle);
    if (it != NULL) {
        snprintf(it->text, sizeof(it->text), "%s", text);
        snprintf(it->subtitle, sizeof(it->subtitle), "%s", subtitle != NULL ? subtitle : "");
        snprintf(it->accessory, sizeof(it->accessory), "%s", v == RPOD_AAP_ON ? "On" : "Off");
    }
}

static const picker_t *find_picker(uint8_t id)
{
    for (size_t i = 0; i < sizeof(pickers) / sizeof(pickers[0]); i++) {
        if (pickers[i].id == id) {
            return &pickers[i];
        }
    }
    return NULL;
}

static void build_picker(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx);

static void on_open_picker(rpod_screen_stack_t *stack, void *item_ctx)
{
    rpod_live_row_t *ref = item_ctx;
    const ap_ctx_t *c = rpod_live_list_ctx(ref->ll);
    const picker_t *p = find_picker((uint8_t)key_value(ref));
    if (p != NULL) {
        push(stack, build_picker, c->path, p);
    }
}

/* A row showing the setting's current choice, opening its picker. */
static void add_picker_row(rpod_live_list_t *ll, uint8_t id)
{
    const picker_t *p = find_picker(id);
    uint8_t v;
    if (p == NULL || !rpod_airpods_get(id, &v)) {
        return;
    }
    rpod_list_item_t *it = add_row(ll, "pick", id, on_open_picker);
    if (it == NULL) {
        return;
    }
    snprintf(it->text, sizeof(it->text), "%s", p->title);
    snprintf(it->accessory, sizeof(it->accessory), "%u", v);
    for (size_t i = 0; i < p->count; i++) {
        if (p->options[i].value == v) {
            snprintf(it->accessory, sizeof(it->accessory), "%s", p->options[i].label);
        }
    }
    it->chevron = true;
}

static bool known(uint8_t id)
{
    uint8_t v;
    return rpod_airpods_get(id, &v);
}

/* Adaptive is the newer models' mode; they report its strength setting. */
static bool mode_offered(uint8_t mode, uint8_t current)
{
    uint8_t v;
    switch (mode) {
    case RPOD_AAP_MODE_ADAPTIVE:
        return current == mode || known(RPOD_AAP_CTL_ADAPTIVE_STRENGTH) ||
               (rpod_airpods_get(RPOD_AAP_CTL_HOLD_CYCLE, &v) && (v & RPOD_AAP_MODE_BIT(mode)));
    case RPOD_AAP_MODE_OFF:
        /* AirPods Pro 2 hide Off unless Off Listening Mode is on. */
        return current == mode || !rpod_airpods_get(RPOD_AAP_CTL_ALLOW_OFF, &v) || v == RPOD_AAP_ON;
    default:
        return true;
    }
}

/* --- Picker screen --------------------------------------------------------------- */

static void on_pick(rpod_screen_stack_t *stack, void *item_ctx)
{
    (void)stack;
    rpod_live_row_t *ref = item_ctx;
    const ap_ctx_t *c = rpod_live_list_ctx(ref->ll);
    rpod_airpods_set(c->picker->id, (uint8_t)key_value(ref));
}

static void fill_picker(rpod_live_list_t *ll)
{
    const picker_t *p = ((const ap_ctx_t *)rpod_live_list_ctx(ll))->picker;
    uint8_t v;
    if (!ready(ll) || !rpod_airpods_get(p->id, &v)) {
        rpod_live_list_message(ll, p->title, "AirPods not connected.");
        return;
    }
    rpod_live_list_header(ll, p->title, p->note);
    for (size_t i = 0; i < p->count; i++) {
        rpod_list_item_t *it = add_row(ll, "opt", p->options[i].value, on_pick);
        if (it != NULL) {
            snprintf(it->text, sizeof(it->text), "%s", p->options[i].label);
            it->status = p->options[i].value == v ? RPOD_ROW_STATUS_CHECK : RPOD_ROW_STATUS_NONE;
        }
    }
}

static void build_picker(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    ap_list_create(stack, screen, fill_picker, ctx);
}

/* --- Press and Hold screen ------------------------------------------------------- */

static void on_cycle(rpod_screen_stack_t *stack, void *item_ctx)
{
    (void)stack;
    uint8_t cycle;
    if (!rpod_airpods_get(RPOD_AAP_CTL_HOLD_CYCLE, &cycle)) {
        return;
    }
    uint8_t next = cycle ^ (uint8_t)RPOD_AAP_MODE_BIT(key_value(item_ctx));
    /* Cycling needs something to cycle between. */
    if (__builtin_popcount(next) >= 2) {
        rpod_airpods_set(RPOD_AAP_CTL_HOLD_CYCLE, next);
    }
}

static void fill_hold(rpod_live_list_t *ll)
{
    uint8_t cycle, current = 0;
    if (!ready(ll) || !rpod_airpods_get(RPOD_AAP_CTL_HOLD_CYCLE, &cycle)) {
        rpod_live_list_message(ll, "Press and Hold", "AirPods not connected.");
        return;
    }
    rpod_airpods_get(RPOD_AAP_CTL_LISTENING_MODE, &current);

    rpod_live_list_header(ll, "Press and Hold",
                          "Pressing and holding the stem cycles between these. Choose at least two.");
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        if (!mode_offered(modes[i].mode, current)) {
            continue;
        }
        rpod_list_item_t *it = add_row(ll, "mode", modes[i].mode, on_cycle);
        if (it != NULL) {
            snprintf(it->text, sizeof(it->text), "%s", modes[i].label);
            it->status = (cycle & RPOD_AAP_MODE_BIT(modes[i].mode)) ? RPOD_ROW_STATUS_CHECK
                                                                     : RPOD_ROW_STATUS_NONE;
        }
    }
    if (known(RPOD_AAP_CTL_ALLOW_OFF)) {
        rpod_live_list_section(ll, NULL);
        add_toggle(ll, RPOD_AAP_CTL_ALLOW_OFF, "Off Listening Mode", "Offers Off in Noise Control");
    }
}

static void build_hold(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    ap_list_create(stack, screen, fill_hold, ctx);
}

/* --- Accessibility screen ------------------------------------------------------- */

static const uint8_t accessibility_ids[] = {
    RPOD_AAP_CTL_PRESS_SPEED, RPOD_AAP_CTL_HOLD_DURATION, RPOD_AAP_CTL_ONE_BUD_ANC,
    RPOD_AAP_CTL_VOLUME_SWIPE, RPOD_AAP_CTL_SWIPE_SPEED,
};

static bool any_accessibility(void)
{
    for (size_t i = 0; i < sizeof(accessibility_ids); i++) {
        if (known(accessibility_ids[i])) {
            return true;
        }
    }
    return false;
}

static void fill_accessibility(rpod_live_list_t *ll)
{
    if (!ready(ll) || !any_accessibility()) {
        rpod_live_list_message(ll, "Accessibility", "AirPods not connected.");
        return;
    }
    add_picker_row(ll, RPOD_AAP_CTL_PRESS_SPEED);
    add_picker_row(ll, RPOD_AAP_CTL_HOLD_DURATION);
    add_toggle(ll, RPOD_AAP_CTL_ONE_BUD_ANC, "Noise Cancellation", "Even with only one AirPod in");
    add_toggle(ll, RPOD_AAP_CTL_VOLUME_SWIPE, "Volume Control", "Swipe the stem for volume");
    add_picker_row(ll, RPOD_AAP_CTL_SWIPE_SPEED);
}

static void build_accessibility(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    ap_list_create(stack, screen, fill_accessibility, ctx);
}

/* --- About screen --------------------------------------------------------------- */

static void fill_about(rpod_live_list_t *ll)
{
    const rpod_airpods_t *ap = session_for(ll);
    if (ap == NULL || ap->model[0] == '\0') {
        rpod_live_list_message(ll, "About", "AirPods not connected.");
        return;
    }
    rpod_live_list_header(ll, ap->name, NULL);
    const struct {
        const char *label;
        const char *value;
    } rows[] = {
        { "Model Number", ap->model },
        { "Serial Number", ap->serial },
        { "Version", ap->firmware },
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        if (rows[i].value[0] == '\0') {
            continue;
        }
        rpod_list_item_t *it = rpod_live_list_add(ll, rows[i].label, NULL);
        if (it != NULL) {
            /* Versions run long ("61.1868040002000000.2713") -- a second
             * line fits them where an accessory would truncate. */
            snprintf(it->text, sizeof(it->text), "%s", rows[i].label);
            snprintf(it->subtitle, sizeof(it->subtitle), "%s", rows[i].value);
        }
    }
}

static void build_about(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    ap_list_create(stack, screen, fill_about, ctx);
}

/* --- Main screen ------------------------------------------------------------------ */

static void on_mode(rpod_screen_stack_t *stack, void *item_ctx)
{
    (void)stack;
    rpod_airpods_set(RPOD_AAP_CTL_LISTENING_MODE, (uint8_t)key_value(item_ctx));
}

static void on_sub_screen(rpod_screen_stack_t *stack, void *item_ctx)
{
    rpod_live_row_t *ref = item_ctx;
    const ap_ctx_t *c = rpod_live_list_ctx(ref->ll);
    if (strcmp(ref->key, "hold") == 0) {
        push(stack, build_hold, c->path, NULL);
    } else if (strcmp(ref->key, "accessibility") == 0) {
        push(stack, build_accessibility, c->path, NULL);
    } else {
        push(stack, build_about, c->path, NULL);
    }
}

static void on_connection(rpod_screen_stack_t *stack, void *item_ctx)
{
    (void)stack;
    rpod_live_row_t *ref = item_ctx;
    const rpod_bt_device_t *d = rpod_bt_find(((const ap_ctx_t *)rpod_live_list_ctx(ref->ll))->path);
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
    rpod_bt_forget(((const ap_ctx_t *)rpod_live_list_ctx(ref->ll))->path);
    /* Pop deletes this screen and with it `ref` -- nothing after this. */
    rpod_screen_stack_pop(stack);
}

static void add_noise_control(rpod_live_list_t *ll)
{
    uint8_t current;
    if (!rpod_airpods_get(RPOD_AAP_CTL_LISTENING_MODE, &current)) {
        return;
    }
    rpod_live_list_section(ll, "NOISE CONTROL");
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        if (!mode_offered(modes[i].mode, current)) {
            continue;
        }
        rpod_list_item_t *it = add_row(ll, "mode", modes[i].mode, on_mode);
        if (it != NULL) {
            snprintf(it->text, sizeof(it->text), "%s", modes[i].label);
            it->status = modes[i].mode == current ? RPOD_ROW_STATUS_CHECK : RPOD_ROW_STATUS_NONE;
        }
    }
}

static void add_audio(rpod_live_list_t *ll)
{
    if (!known(RPOD_AAP_CTL_ADAPTIVE_STRENGTH) && !known(RPOD_AAP_CTL_CONVERSATION) &&
        !known(RPOD_AAP_CTL_PERSONAL_VOLUME) && !known(RPOD_AAP_CTL_EAR_DETECTION)) {
        return;
    }
    rpod_live_list_section(ll, "AUDIO");
    add_picker_row(ll, RPOD_AAP_CTL_ADAPTIVE_STRENGTH);
    add_toggle(ll, RPOD_AAP_CTL_CONVERSATION, "Conversation Awareness", "Lowers the music when you talk");
    add_toggle(ll, RPOD_AAP_CTL_PERSONAL_VOLUME, "Personalized Volume", NULL);
    add_toggle(ll, RPOD_AAP_CTL_EAR_DETECTION, "Automatic Ear Detection", "Pauses when you take one out");
}

static void add_sub_screens(rpod_live_list_t *ll, const rpod_airpods_t *ap)
{
    bool hold = known(RPOD_AAP_CTL_HOLD_CYCLE);
    bool accessibility = any_accessibility();
    bool about = ap->model[0] != '\0';
    if (!hold && !accessibility && !about) {
        return;
    }
    rpod_live_list_section(ll, NULL);
    const struct {
        bool show;
        const char *key;
        const char *text;
    } rows[] = {
        { hold, "hold", "Press and Hold" },
        { accessibility, "accessibility", "Accessibility" },
        { about, "about", "About" },
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        if (!rows[i].show) {
            continue;
        }
        rpod_list_item_t *it = rpod_live_list_add(ll, rows[i].key, on_sub_screen);
        if (it != NULL) {
            snprintf(it->text, sizeof(it->text), "%s", rows[i].text);
            it->chevron = true;
        }
    }
}

static void fill_main(rpod_live_list_t *ll)
{
    ap_ctx_t *c = rpod_live_list_ctx(ll);
    const rpod_bt_device_t *d = rpod_bt_find(c->path); /* NULL with fake AirPods */
    const rpod_airpods_t *ap = session_for(ll);
    if (d == NULL && ap == NULL) {
        rpod_live_list_message(ll, "AirPods", "These AirPods are no longer available.");
        return;
    }

    /* Coming up (or dropping) swaps nearly every row: start at the top
     * rather than wherever the old rows' keys land in the new ones. */
    int link = ap != NULL ? (int)ap->link : -1;
    if (c->built && link != c->shown_link) {
        rpod_live_list_reset_focus(ll);
    }
    c->shown_link = link;
    c->built = true;

    char note[96];
    if (ap == NULL) {
        snprintf(note, sizeof(note), "%s",
                 d->error[0] != '\0'                      ? d->error
                 : d->op == RPOD_BT_OP_CONNECTING          ? "Connecting..."
                 : d->connected                            ? "Connected"
                                                           : "Not Connected");
    } else if (ap->link == RPOD_AIRPODS_CONNECTING) {
        if (ap->error[0] != '\0') {
            snprintf(note, sizeof(note), "Controls unavailable: %s", ap->error);
        } else {
            snprintf(note, sizeof(note), "Connecting to AirPods controls...");
        }
    } else {
        rpod_airpods_battery_text(note, sizeof(note));
        if (note[0] == '\0') {
            snprintf(note, sizeof(note), "Connected");
        }
    }
    const char *name = d != NULL ? d->name : ap->name;
    rpod_live_list_header(ll, name[0] != '\0' ? name : "AirPods", note);

    bool ready_rows = ap != NULL && ap->link == RPOD_AIRPODS_READY;
    if (ready_rows) {
        add_noise_control(ll);
        add_audio(ll);
        add_sub_screens(ll, ap);
    }

    if (d != NULL) {
        if (ready_rows) {
            rpod_live_list_section(ll, NULL);
        }
        rpod_list_item_t *it = rpod_live_list_add(ll, "connection", on_connection);
        if (it != NULL) {
            snprintf(it->text, sizeof(it->text), "%s", d->connected ? "Disconnect" : "Connect");
            snprintf(it->accessory, sizeof(it->accessory), "%s",
                     d->op == RPOD_BT_OP_CONNECTING      ? "Connecting..."
                     : d->op == RPOD_BT_OP_DISCONNECTING ? "Disconnecting..."
                                                         : "");
        }
        it = rpod_live_list_add(ll, "forget", on_forget);
        if (it != NULL) {
            snprintf(it->text, sizeof(it->text), "Forget This Device");
        }
    }
}

static void build_main(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    ap_list_create(stack, screen, fill_main, ctx);
}

void rpod_airpods_screen_push(rpod_screen_stack_t *stack, const char *path)
{
    push(stack, build_main, path, NULL);
}
