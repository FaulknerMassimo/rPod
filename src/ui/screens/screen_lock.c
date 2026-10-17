#include "screen_lock.h"

#include "live_list.h"
#include "ui/classic.h"
#include "ui/metrics.h"
#include "ui/spinner.h"
#include "ui/status_bar.h"
#include "ui/theme.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIGITS 4

/* The classic padlock: the iPod's hold icon, blown up on its pixel grid. */
#define CLASSIC_LOCK_SCALE 3

static struct {
    char code[DIGITS + 1]; /* "" until one's set */
    bool locked;
    char path[PATH_MAX];
} g;

static void load(void)
{
    FILE *f = fopen(g.path, "r");
    if (f == NULL) {
        return; /* never set */
    }
    char code[16], state[16];
    if (fscanf(f, "%15s %15s", code, state) == 2 && strlen(code) == DIGITS &&
        strspn(code, "0123456789") == DIGITS) {
        memcpy(g.code, code, sizeof(g.code)); /* DIGITS and the NUL: checked above */
        g.locked = strcmp(state, "locked") == 0;
    }
    fclose(f);
}

/* Written to a temp file and renamed over the real one, so a power cut
 * mid-write can't leave it truncated. */
static void save(void)
{
    if (g.path[0] == '\0') {
        return;
    }
    char tmp[sizeof(g.path) + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g.path);
    FILE *f = fopen(tmp, "w");
    if (f == NULL) {
        fprintf(stderr, "rpod: screen lock couldn't write %s: %s\n", tmp, strerror(errno));
        return;
    }
    fprintf(f, "%s %s\n", g.code, g.locked ? "locked" : "unlocked");
    if (fclose(f) != 0 || rename(tmp, g.path) != 0) {
        fprintf(stderr, "rpod: screen lock couldn't save %s: %s\n", g.path, strerror(errno));
        remove(tmp);
    }
}

void rpod_screen_lock_init(const char *state_path)
{
    if (state_path != NULL && state_path[0] != '\0') {
        snprintf(g.path, sizeof(g.path), "%s", state_path);
        load();
    }
}

/* --- Entering a combination --------------------------------------------------- */

typedef enum {
    ENTER_NEW,     /* a new one... */
    ENTER_CONFIRM, /* ...and again, to be sure of it */
    ENTER_OLD,     /* the current one, before changing it */
    ENTER_UNLOCK,  /* the lock screen */
} entry_mode_t;

typedef struct {
    rpod_screen_stack_t *stack;
    entry_mode_t mode;
    rpod_spinner_t sp;
    char first[DIGITS + 1]; /* ENTER_NEW's, for ENTER_CONFIRM */
    lv_obj_t *spinner;      /* this build's */
    lv_obj_t *prompt;       /* glass's; NULL in the classic themes */
} entry_t;

/* The screen's title -- short enough for the classic title bar -- and
 * glass's prompt over the digits, which has no title bar to say it. */
static const char *mode_title(entry_mode_t mode)
{
    switch (mode) {
    case ENTER_NEW:
        return "New Code";
    case ENTER_CONFIRM:
        return "Confirm";
    case ENTER_OLD:
        return "Old Code";
    case ENTER_UNLOCK:
    default:
        return "Locked";
    }
}

static const char *mode_prompt(entry_mode_t mode)
{
    switch (mode) {
    case ENTER_NEW:
        return "New Combination";
    case ENTER_CONFIRM:
        return "Confirm Combination";
    case ENTER_OLD:
        return "Current Combination";
    case ENTER_UNLOCK:
    default:
        return "Enter Combination";
    }
}

static void reset_fields(entry_t *en)
{
    en->sp = (rpod_spinner_t){ .count = DIGITS, .mask = true };
    for (int i = 0; i < DIGITS; i++) {
        en->sp.field[i] = (rpod_spinner_field_t){ .min = 0, .max = 9, .value = 0, .digits = 1 };
    }
}

static entry_t *new_entry(rpod_screen_stack_t *stack, entry_mode_t mode)
{
    entry_t *en = calloc(1, sizeof(*en));
    if (en != NULL) {
        en->stack = stack;
        en->mode = mode;
        reset_fields(en);
    }
    return en;
}

/* Another go: from the first digit, with the screen named for `mode`. A
 * wrong combination shakes first. */
static void again(entry_t *en, entry_mode_t mode, bool wrong)
{
    en->mode = mode;
    reset_fields(en);
    rpod_spinner_refresh(en->spinner);
    if (wrong) {
        rpod_spinner_shake(en->spinner);
    }
    rpod_screen_stack_set_title(en->stack, mode_title(mode));
    if (en->prompt != NULL) {
        lv_label_set_text(en->prompt, mode_prompt(mode));
    }
}

static void unlock(entry_t *en)
{
    g.locked = false;
    save();
    rpod_status_bar_set_locked(false);
    rpod_screen_stack_set_locked(en->stack, false);
    rpod_screen_stack_pop(en->stack); /* frees en */
}

static void entry_done(void *ctx)
{
    entry_t *en = ctx;
    char code[DIGITS + 1];
    for (int i = 0; i < DIGITS; i++) {
        code[i] = (char)('0' + en->sp.field[i].value);
    }
    code[DIGITS] = '\0';

    switch (en->mode) {
    case ENTER_NEW:
        memcpy(en->first, code, sizeof(code));
        again(en, ENTER_CONFIRM, false);
        break;
    case ENTER_CONFIRM:
        if (strcmp(code, en->first) == 0) {
            memcpy(g.code, code, sizeof(code));
            save();
            rpod_screen_stack_pop(en->stack); /* frees en */
        } else {
            again(en, ENTER_NEW, true);
        }
        break;
    case ENTER_OLD:
        again(en, strcmp(code, g.code) == 0 ? ENTER_NEW : ENTER_OLD, strcmp(code, g.code) != 0);
        break;
    case ENTER_UNLOCK:
        if (strcmp(code, g.code) == 0) {
            unlock(en);
        } else {
            again(en, ENTER_UNLOCK, true);
        }
        break;
    }
}

/* A padlock: the iPod's hold icon blown up, or a glass one -- a shackle (a
 * ring, its lower half hidden behind the body) over a rounded body. */
static void padlock(lv_obj_t *parent)
{
    if (rpod_theme()->classic) {
        lv_obj_t *img = rpod_classic_icon_create(parent, RPOD_ICON_LOCK, RPOD_COLOR_TEXT);
        if (lv_obj_check_type(img, &lv_image_class)) {
            const lv_image_dsc_t *src = lv_image_get_src(img);
            lv_obj_set_size(img, (int32_t)src->header.w * CLASSIC_LOCK_SCALE,
                            (int32_t)src->header.h * CLASSIC_LOCK_SCALE);
            lv_image_set_inner_align(img, LV_IMAGE_ALIGN_STRETCH);
            lv_image_set_antialias(img, false);
        }
        return;
    }
    lv_obj_t *lock = lv_obj_create(parent);
    lv_obj_remove_style_all(lock);
    lv_obj_set_size(lock, 40, 48);
    lv_obj_remove_flag(lock, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *shackle = lv_obj_create(lock);
    lv_obj_remove_style_all(shackle);
    lv_obj_set_size(shackle, 26, 36);
    lv_obj_set_style_radius(shackle, 13, 0);
    lv_obj_set_style_border_width(shackle, 5, 0);
    lv_obj_set_style_border_color(shackle, RPOD_COLOR_TEXT, 0);
    lv_obj_set_style_border_opa(shackle, LV_OPA_COVER, 0);
    lv_obj_align(shackle, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *body = lv_obj_create(lock);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, 40, 28);
    lv_obj_set_style_radius(body, 6, 0);
    lv_obj_set_style_bg_color(body, RPOD_COLOR_TEXT, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_align(body, LV_ALIGN_BOTTOM_MID, 0, 0);

    lv_obj_t *hole = lv_obj_create(body);
    lv_obj_remove_style_all(hole);
    lv_obj_set_size(hole, 6, 10);
    lv_obj_set_style_radius(hole, 3, 0);
    lv_obj_set_style_bg_color(hole, RPOD_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(hole, LV_OPA_COVER, 0);
    lv_obj_center(hole);
}

static void build_entry_screen(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    entry_t *en = ctx;
    const rpod_metrics_t *m = rpod_metrics();
    bool classic = rpod_theme()->classic;
    rpod_screen_stack_set_title(stack, mode_title(en->mode));

    lv_obj_t *col = lv_obj_create(screen);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, m->screen_w, rpod_metrics_bottom(m) - m->header_h);
    lv_obj_set_pos(col, 0, m->header_h);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, classic ? 3 * RPOD_CLASSIC_PX : 8, 0);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    if (en->mode == ENTER_UNLOCK) {
        padlock(col);
    }
    /* The classic title bar says it already. */
    en->prompt = NULL;
    if (!classic) {
        en->prompt = lv_label_create(col);
        lv_label_set_text(en->prompt, mode_prompt(en->mode));
        lv_obj_set_style_text_font(en->prompt, m->font_small, 0);
        lv_obj_set_style_text_color(en->prompt, RPOD_COLOR_DIM_TEXT, 0);
    }
    en->spinner = rpod_spinner_create(col, &en->sp, entry_done, en);
}

/* --- Locking ------------------------------------------------------------------ */

static void lock(rpod_screen_stack_t *stack)
{
    entry_t *en = new_entry(stack, ENTER_UNLOCK);
    if (en == NULL) {
        return;
    }
    rpod_screen_stack_push(stack, build_entry_screen, en, free);
    rpod_screen_stack_set_locked(stack, true);
    rpod_status_bar_set_locked(true);
}

void rpod_screen_lock_restore(rpod_screen_stack_t *stack)
{
    if (g.locked && g.code[0] != '\0') {
        lock(stack);
    }
}

static void on_lock(rpod_screen_stack_t *stack, void *item_ctx)
{
    (void)item_ctx;
    g.locked = true;
    save();
    lock(stack);
}

static void push_entry(rpod_screen_stack_t *stack, entry_mode_t mode)
{
    entry_t *en = new_entry(stack, mode);
    if (en != NULL) {
        rpod_screen_stack_push(stack, build_entry_screen, en, free);
    }
}

static void on_set(rpod_screen_stack_t *stack, void *item_ctx)
{
    (void)item_ctx;
    push_entry(stack, ENTER_NEW);
}

static void on_change(rpod_screen_stack_t *stack, void *item_ctx)
{
    (void)item_ctx;
    push_entry(stack, ENTER_OLD);
}

static void fill_menu(rpod_live_list_t *ll)
{
    rpod_list_item_t *it;
    if (g.code[0] == '\0') {
        it = rpod_live_list_add(ll, "set", on_set);
        if (it != NULL) {
            snprintf(it->text, sizeof(it->text), "Set Combination");
            it->chevron = true;
        }
        return;
    }
    it = rpod_live_list_add(ll, "lock", on_lock);
    if (it != NULL) {
        snprintf(it->text, sizeof(it->text), "Lock");
    }
    it = rpod_live_list_add(ll, "change", on_change);
    if (it != NULL) {
        snprintf(it->text, sizeof(it->text), "Change Combination");
        it->chevron = true;
    }
}

/* Back from setting one: Lock may be there now. */
static void menu_loaded_cb(lv_event_t *e)
{
    rpod_live_list_changed(lv_event_get_user_data(e));
}

void rpod_screen_lock_menu_build(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    (void)ctx;
    rpod_live_list_t *ll = rpod_live_list_create(stack, screen, fill_menu, NULL, NULL);
    if (ll != NULL) {
        lv_obj_add_event_cb(screen, menu_loaded_cb, LV_EVENT_SCREEN_LOADED, ll);
    }
}
