#include "theme.h"

#include "metrics.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#define HEX(c) LV_COLOR_MAKE(((c) >> 16) & 0xff, ((c) >> 8) & 0xff, (c) & 0xff)

/* Glass: the values these colours had as fixed #defines, unchanged.
 *
 * "Liquid glass" material: a translucent dark fill (so it reads as tinted
 * glass over both flat black screens and the blurred-artwork background on
 * Now Playing) plus a hairline highlight where light would catch a glass
 * edge. No real backdrop blur -- this is pure software rendering on a Pi
 * Zero 2 W with no GPU (see lv_conf.h's LV_USE_DRAW_SW), and PLAN.md #5.3
 * rules out full-frame work per frame. Where there's actually something
 * worth blurring (Now Playing's cover art), that's a one-time blur done on
 * track change, not a live per-frame effect -- see cover_art.c.
 *
 * Classic: the iPod 4G's palette, the four grays its LCD could show
 * (re/ipod4/SPEC.md §2) -- black on white, the selected row inverted, and
 * #555555 filling sliders. Classic Dark is the same palette turned around:
 * white on black, the selection a white bar, sliders filled #AAAAAA. */
static const rpod_theme_t k_themes[RPOD_THEME_COUNT] = {
    [RPOD_THEME_GLASS] = {
        .key = "glass",
        .name = "Glass",
        .classic = false,
        .bg = HEX(0x000000),
        .text = HEX(0xe8e8e8),
        .dim_text = HEX(0x8e8e93),
        .header_bg = HEX(0x1c1c1e),
        .accent = HEX(0x0a84ff),
        .sel_text = HEX(0xe8e8e8),
        .separator = HEX(0x38383a),
        /* Apple Music's pink-red. Distinct from the blue selection accent so
         * a filled heart reads as "liked" on both the dark glass and the
         * accent-blue selection highlight. */
        .like = HEX(0xff375f),
        .glass_fill = HEX(0x2c2c2e),
        .glass_fill_opa = 150, /* ~59% */
        .glass_edge = HEX(0xffffff),
        .glass_edge_opa = 50, /* ~20% -- faint, not a solid line */
        /* Faint white, so a key reads as a raised glass keycap. */
        .key_fill = HEX(0xffffff),
        .key_fill_opa = 20,
        .fill = HEX(0xe8e8e8),
    },
    [RPOD_THEME_CLASSIC] = {
        .key = "classic",
        .name = "Classic",
        .classic = true,
        .bg = HEX(0xffffff),
        .text = HEX(0x000000),
        .dim_text = HEX(0x555555),
        .header_bg = HEX(0xffffff),
        .accent = HEX(0x000000),
        .sel_text = HEX(0xffffff),
        .separator = HEX(0xaaaaaa),
        .like = HEX(0x000000),
        .glass_fill = HEX(0xffffff),
        .glass_fill_opa = LV_OPA_COVER,
        .glass_edge = HEX(0x000000),
        .glass_edge_opa = LV_OPA_COVER,
        .key_fill = HEX(0xffffff),
        .key_fill_opa = LV_OPA_TRANSP,
        .fill = HEX(0x555555),
    },
    [RPOD_THEME_CLASSIC_DARK] = {
        .key = "classic-dark",
        .name = "Classic Dark",
        .classic = true,
        .bg = HEX(0x000000),
        .text = HEX(0xffffff),
        .dim_text = HEX(0xaaaaaa),
        .header_bg = HEX(0x000000),
        .accent = HEX(0xffffff),
        .sel_text = HEX(0x000000),
        .separator = HEX(0x555555),
        .like = HEX(0xffffff),
        .glass_fill = HEX(0x000000),
        .glass_fill_opa = LV_OPA_COVER,
        .glass_edge = HEX(0xffffff),
        .glass_edge_opa = LV_OPA_COVER,
        .key_fill = HEX(0x000000),
        .key_fill_opa = LV_OPA_TRANSP,
        .fill = HEX(0xaaaaaa),
    },
};

/* The classic themes' floating boxes: a solid border, 1 iPod pixel wide at
 * the 2x scale the classic layout draws at. */
#define CLASSIC_BOX_BORDER 2

static struct {
    rpod_theme_id_t id;
    char path[PATH_MAX];
    void (*on_change)(void *ctx);
    void *ctx;
    bool apply_pending;
} g = {
    .id = RPOD_THEME_GLASS,
};

const rpod_theme_t *rpod_theme(void)
{
    return &k_themes[g.id];
}

rpod_theme_id_t rpod_theme_id(void)
{
    return g.id;
}

const char *rpod_theme_name(rpod_theme_id_t id)
{
    return id < RPOD_THEME_COUNT ? k_themes[id].name : "";
}

bool rpod_theme_find(const char *key, rpod_theme_id_t *out)
{
    for (int i = 0; i < RPOD_THEME_COUNT; i++) {
        if (strcmp(key, k_themes[i].key) == 0) {
            *out = (rpod_theme_id_t)i;
            return true;
        }
    }
    return false;
}

static void load(void)
{
    FILE *f = fopen(g.path, "r");
    if (f == NULL) {
        return; /* first run */
    }
    char key[32] = "";
    if (fscanf(f, "%31s", key) == 1) {
        rpod_theme_find(key, &g.id);
    }
    fclose(f);
}

/* Written to a temp file and renamed over the real one, so a power cut
 * mid-write can't leave it truncated (same as ui/backlight.c). */
static void save(void)
{
    char tmp[sizeof(g.path) + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g.path);
    FILE *f = fopen(tmp, "w");
    if (f == NULL) {
        fprintf(stderr, "rpod: theme couldn't write %s: %s\n", tmp, strerror(errno));
        return;
    }
    fprintf(f, "%s\n", k_themes[g.id].key);
    if (fclose(f) != 0 || rename(tmp, g.path) != 0) {
        fprintf(stderr, "rpod: theme couldn't save %s: %s\n", g.path, strerror(errno));
        remove(tmp);
    }
}

void rpod_theme_init(const char *state_path)
{
    if (state_path != NULL && state_path[0] != '\0') {
        snprintf(g.path, sizeof(g.path), "%s", state_path);
        load();
    }
}

static void apply_cb(void *user)
{
    (void)user;
    g.apply_pending = false;
    if (g.on_change != NULL) {
        g.on_change(g.ctx);
    }
}

void rpod_theme_set(rpod_theme_id_t id)
{
    if (id >= RPOD_THEME_COUNT || id == g.id) {
        return;
    }
    g.id = id;
    if (g.path[0] != '\0') {
        save();
    }
    if (!g.apply_pending) {
        g.apply_pending = true;
        lv_async_call(apply_cb, NULL);
    }
}

void rpod_theme_on_change(void (*cb)(void *ctx), void *ctx)
{
    g.on_change = cb;
    g.ctx = ctx;
}

void rpod_theme_style_screen(lv_obj_t *screen)
{
    lv_obj_set_style_bg_color(screen, RPOD_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, RPOD_COLOR_TEXT, 0);
    /* Labels that don't pick a font inherit this one -- in the classic
     * themes, Chicago rather than LVGL's default Montserrat. Glass keeps the
     * default it has always had. */
    if (rpod_theme()->classic) {
        lv_obj_set_style_text_font(screen, rpod_metrics()->font_body, 0);
    }
}

void rpod_theme_style_glass_bar(lv_obj_t *bar)
{
    lv_obj_set_style_bg_color(bar, RPOD_COLOR_GLASS_FILL, 0);
    lv_obj_set_style_bg_opa(bar, RPOD_GLASS_FILL_OPA, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, RPOD_COLOR_GLASS_EDGE, 0);
    lv_obj_set_style_border_opa(bar, RPOD_GLASS_EDGE_OPA, 0);
}

void rpod_theme_style_glass_panel(lv_obj_t *panel, int32_t radius)
{
    if (rpod_theme()->classic) {
        lv_obj_set_style_bg_color(panel, RPOD_COLOR_GLASS_FILL, 0);
        lv_obj_set_style_bg_opa(panel, RPOD_GLASS_FILL_OPA, 0);
        lv_obj_set_style_radius(panel, 0, 0);
        lv_obj_set_style_border_side(panel, LV_BORDER_SIDE_FULL, 0);
        lv_obj_set_style_border_width(panel, CLASSIC_BOX_BORDER, 0);
        lv_obj_set_style_border_color(panel, RPOD_COLOR_GLASS_EDGE, 0);
        lv_obj_set_style_border_opa(panel, RPOD_GLASS_EDGE_OPA, 0);
        lv_obj_set_style_shadow_width(panel, 0, 0);
        return;
    }
    lv_obj_set_style_bg_color(panel, RPOD_COLOR_GLASS_FILL, 0);
    lv_obj_set_style_bg_opa(panel, RPOD_GLASS_FILL_OPA, 0);
    lv_obj_set_style_radius(panel, radius, 0);
    lv_obj_set_style_border_side(panel, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, RPOD_COLOR_GLASS_EDGE, 0);
    lv_obj_set_style_border_opa(panel, RPOD_GLASS_EDGE_OPA, 0);
    lv_obj_set_style_shadow_width(panel, 10, 0);
    lv_obj_set_style_shadow_ofs_y(panel, 3, 0);
    lv_obj_set_style_shadow_color(panel, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(panel, 70, 0);
}
