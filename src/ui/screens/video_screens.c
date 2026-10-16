#include "video_screens.h"

#include "list_screen.h"
#include "audio/scrobbler.h"
#include "ui/backlight.h"
#include "ui/classic.h"
#include "ui/cover_art.h"
#include "ui/metrics.h"
#include "ui/seek_control.h"
#include "ui/status_bar.h"
#include "ui/theme.h"
#include "ui/volume_control.h"
#include "video/av_sync.h"
#include "video/media_clock.h"
#include "video/video_decoder.h"
#include "video/video_library.h"

#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* How often the picture checks whether its next frame is due: how late
 * past its time a frame can go up. */
#define FRAME_TICK_MS 5

/* How often MPD's asked where the sound is (video/media_clock.h), and how
 * far behind it the sound is heard (video/av_sync.h). */
#define POLL_MS 200
#define DELAY_EVERY_MS 1000

/* A change of output this big moves the picture straight there. */
#define DELAY_JUMP_MS 100

/* The title and progress bar, after Center or a change: up this long. */
#define OVERLAY_MS 3000

/* Scrubbing: each wheel step (already sped up by a fast turn, input/
 * wheel_input.c), and how long after the last one MPD goes there. */
#define SCRUB_STEP_MS 2000
#define SCRUB_SETTLE_MS 350

/* A scan or a scrub shows keyframes as it goes: at most this often, and
 * once it's moved this far. */
#define PREVIEW_EVERY_MS 120
#define PREVIEW_MOVE_MS 250

/* MPD moving on this close to the end is the video ending. */
#define END_MARGIN_MS 1500

/* MPD moving on this soon after starting it, from the start: it couldn't
 * play the video's sound. */
#define START_GRACE_MS 2000

/* Prev this far in restarts the video, as it does a song (app.c). */
#define PREV_RESTART_MS 2000

/* The list's posters: the biggest file read for one. */
#define POSTER_MAX_BYTES (1024 * 1024)

#define LIST_POLL_MS 60
#define DIR_CHECK_MS 2000

static struct {
    char dir[PATH_MAX];
} g_videos;

static int64_t mono_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void fmt_time(char *out, size_t size, int64_t ms)
{
    if (ms < 0) {
        ms = 0;
    }
    long s = (long)(ms / 1000);
    if (s >= 3600) {
        snprintf(out, size, "%ld:%02ld:%02ld", s / 3600, s / 60 % 60, s % 60);
    } else {
        snprintf(out, size, "%ld:%02ld", s / 60, s % 60);
    }
}

static void video_path(const char *name, char *out, size_t size)
{
    snprintf(out, size, "%s/%s", g_videos.dir, name);
}

/* =============================================================================
 * The player
 * ===========================================================================*/

typedef struct player_view player_view_t;

typedef struct {
    rpod_mpd_t *mpd;
    rpod_screen_stack_t *stack;
    rpod_video_entry_t *list;
    size_t count;
    size_t index;

    /* Playback, borrowed (see video_screens.h). */
    rpod_mpd_spot_t spot;
    bool borrowed;
    int extra_id;
    uint32_t started_at;
    bool taken_over; /* something else has playback now: leave it be */
    bool leaving;    /* over: go at the first chance */
    char error[300]; /* why it isn't playing, shown instead */

    rpod_video_t *video;
    unsigned duration_ms;
    rpod_media_clock_t clock;
    int delay_ms;
    uint32_t delay_at;
    bool have_delay;
    int64_t pos_ms; /* MPD's last answer */
    bool playing;

    bool scrubbing;
    int64_t scrub_ms;
    uint32_t scrub_at;
    bool previewing; /* the last frame tick showed a scan or scrub */
    int64_t preview_ms;
    uint32_t preview_at;

    /* The title and progress bar: up for a while after anything happens
     * (overlay_at), and all the while it's paused or scrub_ready -- Center
     * armed the wheel to scrub, until it's left alone a while. */
    bool overlay;
    uint32_t overlay_at;
    bool scrub_ready;

    player_view_t *view;
} player_t;

struct player_view {
    player_t *p;
    lv_obj_t *screen;
    lv_obj_t *img;
    lv_image_dsc_t dsc;
    bool img_set;
    lv_obj_t *proxy;
    lv_obj_t *panel; /* the title and progress bar */
    lv_obj_t *state_icon;
    lv_obj_t *title;
    lv_obj_t *bar;   /* classic: the iPod's slider; glass: the fill in its track */
    int32_t bar_w;
    lv_obj_t *elapsed;
    lv_obj_t *remaining;
    lv_obj_t *message;
    lv_timer_t *frame_timer;
    lv_timer_t *poll_timer;
};

/* The one playing, for rpod_video_player_skip(). */
static player_t *g_player;

static void borrow(player_t *p)
{
    if (p->borrowed) {
        return;
    }
    if (!rpod_mpd_get_spot(p->mpd, &p->spot)) {
        p->spot.song_id = -1;
        p->spot.state = RPOD_MPD_STATE_STOP;
    }
    p->borrowed = true;
    p->extra_id = -1;
    rpod_scrobbler_suspend(true); /* a video's sound isn't a listen */
}

/* The music back where it was, paused -- unless something's taken over. */
static void give_back(player_t *p)
{
    if (!p->borrowed) {
        return;
    }
    if (!p->taken_over) {
        rpod_mpd_spot_t back = p->spot;
        if (back.state == RPOD_MPD_STATE_PLAY) {
            back.state = RPOD_MPD_STATE_PAUSE;
        }
        rpod_mpd_restore_spot(p->mpd, &back);
    }
    if (p->extra_id >= 0) {
        rpod_mpd_delete_id(p->mpd, p->extra_id);
        p->extra_id = -1;
    }
    rpod_scrobbler_suspend(false);
    p->borrowed = false;
    rpod_status_bar_refresh(); /* the song's title again, not the video's */
}

static const rpod_video_entry_t *current(const player_t *p)
{
    return &p->list[p->index];
}

/* Where the picture should be: following the sound, or a scan or a scrub
 * the sound's not following yet. */
static int64_t picture_ms(const player_t *p, int64_t now, bool *preview)
{
    unsigned pos, dur;
    if (rpod_seek_control_position(&pos, &dur)) {
        *preview = true;
        return pos;
    }
    if (p->scrubbing) {
        *preview = true;
        return p->scrub_ms;
    }
    *preview = false;
    return rpod_media_clock_now(&p->clock, now) - p->delay_ms;
}

static bool visible(const player_view_t *v)
{
    return lv_screen_active() == v->screen && lv_display_is_invalidation_enabled(lv_obj_get_display(v->screen));
}

static void show(lv_obj_t *o, bool shown)
{
    if (o != NULL && lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) == shown) {
        if (shown) {
            lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void set_text(lv_obj_t *label, const char *text)
{
    if (label != NULL && strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

/* The title and progress bar, at `at`. */
static void paint_overlay(player_view_t *v, int64_t at)
{
    player_t *p = v->p;
    show(v->panel, p->overlay && p->error[0] == '\0');
    if (!p->overlay || v->panel == NULL) {
        return;
    }
    unsigned dur = p->duration_ms;
    int64_t pos = at < 0 ? 0 : (dur > 0 && at > dur ? dur : at);
    char buf[24];
    fmt_time(buf, sizeof(buf), pos);
    set_text(v->elapsed, buf);
    buf[0] = '-';
    fmt_time(buf + 1, sizeof(buf) - 1, dur > 0 ? (int64_t)dur - pos : 0);
    set_text(v->remaining, buf);
    set_text(v->title, current(p)->title);
    bool paused = !p->playing;
    if (rpod_theme()->classic) {
        rpod_classic_slider_set(v->bar, (int32_t)(pos / 100), dur > 0 ? (int32_t)(dur / 100) : 1);
        rpod_classic_icon_set(v->state_icon, paused ? RPOD_ICON_PAUSE : RPOD_ICON_PLAY);
    } else {
        int32_t w = dur > 0 ? (int32_t)((int64_t)v->bar_w * pos / dur) : 0;
        if (lv_obj_get_width(v->bar) != w) {
            lv_obj_set_width(v->bar, w);
        }
        set_text(v->state_icon, paused ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
}

static void touch_overlay(player_t *p)
{
    p->overlay = true;
    p->overlay_at = lv_tick_get();
}

static void show_frame(player_view_t *v, const rpod_video_frame_t *f)
{
    bool resized = !v->img_set || v->dsc.header.w != (uint32_t)f->w || v->dsc.header.h != (uint32_t)f->h;
    v->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    v->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    v->dsc.header.flags = 0;
    v->dsc.header.w = (uint32_t)f->w;
    v->dsc.header.h = (uint32_t)f->h;
    v->dsc.header.stride = (uint32_t)f->stride;
    v->dsc.data_size = (uint32_t)f->stride * (uint32_t)f->h;
    v->dsc.data = f->pixels;
    if (resized) {
        lv_image_set_src(v->img, &v->dsc);
        lv_obj_center(v->img);
        show(v->img, true);
        v->img_set = true;
    } else {
        lv_obj_invalidate(v->img);
    }
}

static void paint_message(player_view_t *v)
{
    player_t *p = v->p;
    show(v->message, p->error[0] != '\0');
    if (p->error[0] != '\0') {
        set_text(v->message, p->error);
        show(v->img, false);
    }
}

/* Goes back to the list once it can: with this on top, and no card over it. */
static bool leave_if_can(player_view_t *v)
{
    player_t *p = v->p;
    if (lv_screen_active() != v->screen || rpod_screen_stack_overlay(p->stack) != NULL) {
        return false;
    }
    rpod_screen_stack_pop(p->stack); /* deletes v and p */
    return true;
}

/* Starts the list's video `index`, in place of one playing. */
static bool start(player_t *p, size_t index)
{
    p->index = index;
    char path[PATH_MAX + 256];
    video_path(current(p)->name, path, sizeof(path));
    borrow(p);
    int old = p->extra_id;
    int id = -1;
    p->error[0] = '\0';
    if (!rpod_mpd_play_extra(p->mpd, path, 0, &id)) {
        fprintf(stderr, "rpod: MPD won't play %s\n", path);
        snprintf(p->error, sizeof(p->error), "Can't play \xE2\x80\x9C%s\xE2\x80\x9D", current(p)->title);
        id = -1;
    }
    p->extra_id = id;
    if (old >= 0 && old != id) {
        rpod_mpd_delete_id(p->mpd, old);
    }
    rpod_video_close(p->video);
    p->video = id >= 0 ? rpod_video_open(path, rpod_metrics()->screen_w, rpod_metrics()->screen_h) : NULL;
    rpod_media_clock_reset(&p->clock);
    p->started_at = lv_tick_get();
    p->duration_ms = current(p)->duration_ms;
    p->pos_ms = 0;
    p->playing = id >= 0;
    p->scrubbing = false;
    p->scrub_ready = false;
    p->previewing = false;
    p->have_delay = false;
    touch_overlay(p);
    if (p->view != NULL) {
        show(p->view->img, false);
        p->view->img_set = false;
        paint_message(p->view);
        paint_overlay(p->view, 0);
        lv_timer_ready(p->view->poll_timer);
    }
    return id >= 0;
}

static void frame_cb(lv_timer_t *t)
{
    player_view_t *v = lv_timer_get_user_data(t);
    player_t *p = v->p;
    if (p->video == NULL || p->leaving || !visible(v)) {
        return;
    }
    if (p->duration_ms == 0) {
        rpod_video_state(p->video, NULL, NULL, &p->duration_ms);
    }
    bool preview;
    int64_t at = picture_ms(p, mono_ms(), &preview);
    if (preview) {
        if ((!p->previewing || llabs(at - p->preview_ms) >= PREVIEW_MOVE_MS) &&
            lv_tick_elaps(p->preview_at) >= PREVIEW_EVERY_MS) {
            rpod_video_seek(p->video, at, true);
            p->preview_ms = at;
            p->preview_at = lv_tick_get();
        }
        p->previewing = true;
        if (!p->scrubbing) {
            p->pos_ms = at; /* a scan run off the end is the end (poll_cb) */
        }
        touch_overlay(p);
        paint_overlay(v, at);
    } else if (p->previewing) {
        /* The scan's let go, or the scrub's gone there: where's MPD now? */
        p->previewing = false;
        lv_timer_ready(v->poll_timer);
    }

    /* Commit a scrub once the wheel's been still a moment. */
    if (p->scrubbing && lv_tick_elaps(p->scrub_at) >= SCRUB_SETTLE_MS) {
        p->scrubbing = false;
        rpod_mpd_seek(p->mpd, (unsigned)p->scrub_ms);
        rpod_media_clock_observe(&p->clock, p->scrub_ms, p->playing, mono_ms());
        rpod_video_seek(p->video, p->scrub_ms - p->delay_ms, false);
        lv_timer_ready(v->poll_timer);
    }

    const rpod_video_frame_t *f = rpod_video_frame_at(p->video, at);
    if (f != NULL) {
        show_frame(v, f);
        /* Up now, not at the next display refresh: frames at 30 fps
         * against LVGL's 33 ms one would come out unevenly. */
        lv_refr_now(lv_obj_get_display(v->screen));
    }
}

static void poll_cb(lv_timer_t *t)
{
    player_view_t *v = lv_timer_get_user_data(t);
    player_t *p = v->p;
    if (p->leaving) {
        leave_if_can(v);
        return;
    }
    rpod_backlight_hold(p->playing && p->extra_id >= 0 && visible(v));
    if (p->extra_id < 0) {
        return;
    }

    if (!p->have_delay || lv_tick_elaps(p->delay_at) >= DELAY_EVERY_MS) {
        int was = p->delay_ms;
        p->delay_ms = rpod_av_sync_delay_ms(p->mpd);
        p->delay_at = lv_tick_get();
        if (p->have_delay && abs(p->delay_ms - was) >= DELAY_JUMP_MS && p->video != NULL) {
            rpod_video_seek(p->video, rpod_media_clock_now(&p->clock, mono_ms()) - p->delay_ms, false);
        }
        p->have_delay = true;
    }

    int64_t t0 = mono_ms();
    rpod_mpd_spot_t s;
    if (!rpod_mpd_get_spot(p->mpd, &s)) {
        return;
    }
    int64_t t1 = mono_ms();

    if (s.song_id != p->extra_id || s.state == RPOD_MPD_STATE_STOP) {
        bool ended = p->duration_ms == 0 || p->pos_ms + END_MARGIN_MS >= p->duration_ms ||
                     (p->video != NULL && rpod_video_ended(p->video));
        if (!ended && p->pos_ms < 1000 && lv_tick_elaps(p->started_at) < START_GRACE_MS) {
            /* It never got going: MPD couldn't play its sound. */
            fprintf(stderr, "rpod: MPD stopped %s at once\n", current(p)->name);
            give_back(p);
            snprintf(p->error, sizeof(p->error), "Can't play \xE2\x80\x9C%s\xE2\x80\x9D", current(p)->title);
            rpod_video_close(p->video);
            p->video = NULL;
            p->playing = false;
            paint_message(v);
            return;
        }
        if (!ended && s.song_id != p->extra_id) {
            p->taken_over = true; /* Next from the web UI, the alarm... */
        }
        p->leaving = true;
        leave_if_can(v);
        return;
    }

    bool playing = s.state == RPOD_MPD_STATE_PLAY;
    unsigned pos, dur;
    if (!p->scrubbing && !rpod_seek_control_position(&pos, &dur) &&
        rpod_media_clock_observe(&p->clock, s.elapsed_ms, playing, (t0 + t1) / 2) && p->video != NULL) {
        rpod_video_seek(p->video, (int64_t)s.elapsed_ms - p->delay_ms, false);
    }
    p->pos_ms = s.elapsed_ms;
    if (playing != p->playing) {
        p->playing = playing;
        touch_overlay(p); /* paused, it stays up (below) */
    }
    if (!p->scrubbing && !p->previewing && lv_tick_elaps(p->overlay_at) >= OVERLAY_MS) {
        p->scrub_ready = false;
        p->overlay = !p->playing; /* paused, it stays up */
    }
    bool preview;
    paint_overlay(v, picture_ms(p, mono_ms(), &preview));
}

/* The wheel and Center (Menu and the transport buttons are the app's). */
static void proxy_event_cb(lv_event_t *e)
{
    player_view_t *v = lv_event_get_user_data(e);
    player_t *p = v->p;
    if (lv_event_get_code(e) == LV_EVENT_KEY) {
        uint32_t k = lv_event_get_key(e);
        int dir = (k == LV_KEY_RIGHT || k == LV_KEY_UP) ? 1 : (k == LV_KEY_LEFT || k == LV_KEY_DOWN) ? -1 : 0;
        if (dir == 0) {
            return;
        }
        if (!p->scrub_ready || p->extra_id < 0 || p->video == NULL) {
            rpod_volume_control_step(dir);
            return;
        }
        /* After Center, the wheel scrubs. */
        if (!p->scrubbing) {
            bool preview;
            p->scrub_ms = picture_ms(p, mono_ms(), &preview);
            p->scrubbing = true;
        }
        p->scrub_ms += (int64_t)dir * SCRUB_STEP_MS;
        int64_t end = p->duration_ms > 1000 ? (int64_t)p->duration_ms - 1000 : 0;
        p->scrub_ms = p->scrub_ms < 0 ? 0 : p->scrub_ms > end ? end : p->scrub_ms;
        p->scrub_at = lv_tick_get();
        touch_overlay(p);
        paint_overlay(v, p->scrub_ms);
    } else if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        /* Center: the bar, the wheel scrubbing -- or the volume again. */
        if (p->scrub_ready && !p->scrubbing) {
            p->scrub_ready = false;
            p->overlay = !p->playing;
        } else {
            p->scrub_ready = true;
            touch_overlay(p);
        }
        bool preview;
        paint_overlay(v, picture_ms(p, mono_ms(), &preview));
    }
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    return l;
}

/* The title, the play state and the progress bar along the bottom: a glass
 * panel, or the iPod's box with its slider. */
static void build_overlay(player_view_t *v, lv_obj_t *screen)
{
    const rpod_metrics_t *m = rpod_metrics();
    bool classic = rpod_theme()->classic;
    int32_t margin = classic ? 3 * RPOD_CLASSIC_PX : 8;
    int32_t pad = classic ? 3 * RPOD_CLASSIC_PX : 10;
    int32_t panel_w = m->screen_w - 2 * margin;
    int32_t inner_w = panel_w - 2 * pad;

    lv_obj_t *panel = lv_obj_create(screen);
    lv_obj_remove_style_all(panel);
    rpod_theme_style_glass_panel(panel, classic ? 0 : 14);
    lv_obj_set_size(panel, panel_w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(panel, pad, 0);
    lv_obj_set_style_pad_ver(panel, classic ? 2 * RPOD_CLASSIC_PX : 8, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(panel, classic ? 2 * RPOD_CLASSIC_PX : 6, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, -margin);
    v->panel = panel;

    /* The play state and the title. */
    lv_obj_t *top = lv_obj_create(panel);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, inner_w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(top, classic ? 3 * RPOD_CLASSIC_PX : 8, 0);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    if (classic) {
        v->state_icon = rpod_classic_icon_create(top, RPOD_ICON_PLAY, RPOD_COLOR_TEXT);
    } else {
        v->state_icon = label(top, m->font_small, RPOD_COLOR_TEXT, LV_SYMBOL_PLAY);
    }
    v->title = label(top, m->font_body, RPOD_COLOR_TEXT, "");
    lv_obj_set_flex_grow(v->title, 1);
    lv_label_set_long_mode(v->title, LV_LABEL_LONG_MODE_DOTS);

    /* The bar, with the time either side of it. */
    lv_obj_t *row = lv_obj_create(panel);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, inner_w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    v->elapsed = label(row, m->font_small, RPOD_COLOR_TEXT, "0:00");
    int32_t time_w = lv_font_get_line_height(m->font_small) * 3;
    lv_obj_set_width(v->elapsed, time_w);
    int32_t bar_w = inner_w - 2 * time_w;
    if (classic) {
        bar_w = bar_w / RPOD_CLASSIC_PX * RPOD_CLASSIC_PX;
        v->bar = rpod_classic_slider_create(row, bar_w, 5 * RPOD_CLASSIC_PX);
    } else {
        lv_obj_t *track = lv_obj_create(row);
        lv_obj_remove_style_all(track);
        lv_obj_set_size(track, bar_w, 4);
        lv_obj_set_style_radius(track, 2, 0);
        lv_obj_set_style_bg_color(track, RPOD_COLOR_SEPARATOR, 0);
        lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
        lv_obj_remove_flag(track, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        v->bar = lv_obj_create(track);
        lv_obj_remove_style_all(v->bar);
        lv_obj_set_size(v->bar, 0, 4);
        lv_obj_set_style_radius(v->bar, 2, 0);
        lv_obj_set_style_bg_color(v->bar, RPOD_COLOR_ACCENT, 0);
        lv_obj_set_style_bg_opa(v->bar, LV_OPA_COVER, 0);
        lv_obj_remove_flag(v->bar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
    v->bar_w = bar_w;
    v->remaining = label(row, m->font_small, RPOD_COLOR_TEXT, "-0:00");
    lv_obj_set_width(v->remaining, time_w);
    lv_obj_set_style_text_align(v->remaining, LV_TEXT_ALIGN_RIGHT, 0);
}

static void screen_event_cb(lv_event_t *e)
{
    player_view_t *v = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_SCREEN_LOAD_START) {
        rpod_status_bar_set_hidden(true);
    } else if (code == LV_EVENT_SCREEN_UNLOAD_START) {
        rpod_status_bar_set_hidden(false);
        rpod_backlight_hold(false);
    } else if (code == LV_EVENT_DELETE) {
        rpod_status_bar_set_hidden(false);
        lv_timer_delete(v->frame_timer);
        lv_timer_delete(v->poll_timer);
        if (v->p->view == v) {
            v->p->view = NULL;
        }
        free(v);
    }
}

static void build_player(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    player_t *p = ctx;
    const rpod_metrics_t *m = rpod_metrics();
    player_view_t *v = calloc(1, sizeof(*v));
    v->p = p;
    v->screen = screen;
    p->view = v;
    rpod_screen_stack_set_title(stack, current(p)->title);

    /* Black around the picture, whatever the theme. */
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    v->img = lv_image_create(screen);
    show(v->img, false);
    const rpod_video_frame_t *f = p->video != NULL ? rpod_video_current(p->video) : NULL;
    if (f != NULL) {
        show_frame(v, f); /* rebuilt for a theme change: the picture stays up */
    }

    v->message = label(screen, m->font_body, lv_color_white(), "");
    lv_obj_set_width(v->message, m->screen_w - 40);
    lv_label_set_long_mode(v->message, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(v->message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(v->message);
    paint_message(v);

    build_overlay(v, screen);

    /* The encoder drives an offscreen proxy in edit mode, as on Now
     * Playing: turns arrive as keys, Center as a click. */
    v->proxy = lv_obj_create(screen);
    lv_obj_remove_style_all(v->proxy);
    lv_obj_set_size(v->proxy, 1, 1);
    lv_obj_remove_flag(v->proxy, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_event_cb(v->proxy, proxy_event_cb, LV_EVENT_KEY, v);
    lv_obj_add_event_cb(v->proxy, proxy_event_cb, LV_EVENT_CLICKED, v);
    lv_group_t *group = lv_group_get_default();
    if (group != NULL) {
        lv_group_add_obj(group, v->proxy);
        lv_group_focus_obj(v->proxy);
        lv_group_set_editing(group, true);
    }

    v->frame_timer = lv_timer_create(frame_cb, FRAME_TICK_MS, v);
    v->poll_timer = lv_timer_create(poll_cb, POLL_MS, v);
    lv_obj_add_event_cb(screen, screen_event_cb, LV_EVENT_SCREEN_LOAD_START, v);
    lv_obj_add_event_cb(screen, screen_event_cb, LV_EVENT_SCREEN_UNLOAD_START, v);
    lv_obj_add_event_cb(screen, screen_event_cb, LV_EVENT_DELETE, v);
    bool preview;
    paint_overlay(v, picture_ms(p, mono_ms(), &preview));
}

static void player_free(void *ctx)
{
    player_t *p = ctx;
    if (g_player == p) {
        g_player = NULL;
    }
    rpod_backlight_hold(false);
    give_back(p);
    rpod_video_close(p->video);
    free(p->list);
    free(p);
}

/* Plays `list`'s video `index`, taking the list over. */
static void push_player(rpod_screen_stack_t *stack, rpod_mpd_t *mpd, rpod_video_entry_t *list, size_t count,
                        size_t index)
{
    if (g_player != NULL) {
        /* One player at a time: the web UI's Play on rPod over one already
         * playing just changes what it plays. */
        player_t *old = g_player;
        free(old->list);
        old->list = list;
        old->count = count;
        start(old, index);
        return;
    }
    player_t *p = calloc(1, sizeof(*p));
    if (p == NULL) {
        free(list);
        return;
    }
    p->mpd = mpd;
    p->stack = stack;
    p->list = list;
    p->count = count;
    p->extra_id = -1;
    start(p, index);
    g_player = p;
    rpod_screen_stack_push(stack, build_player, p, player_free);
}

bool rpod_video_player_skip(int dir)
{
    player_t *p = g_player;
    if (p == NULL || p->leaving || p->view == NULL) {
        return false;
    }
    if (dir > 0) {
        if (p->index + 1 < p->count) {
            start(p, p->index + 1);
        } else {
            p->leaving = true; /* past the last: back to the list, as at its end */
            lv_timer_ready(p->view->poll_timer);
        }
        return true;
    }
    bool preview;
    int64_t at = picture_ms(p, mono_ms(), &preview);
    if ((at >= PREV_RESTART_MS || p->index == 0) && p->extra_id >= 0) {
        rpod_mpd_seek(p->mpd, 0);
        rpod_media_clock_observe(&p->clock, 0, p->playing, mono_ms());
        if (p->video != NULL) {
            rpod_video_seek(p->video, -p->delay_ms, false);
        }
        touch_overlay(p);
        lv_timer_ready(p->view->poll_timer);
    } else if (p->index > 0) {
        start(p, p->index - 1);
    }
    return true;
}

/* =============================================================================
 * The list
 * ===========================================================================*/

typedef struct {
    rpod_mpd_t *mpd;
    rpod_video_entry_t *list;
    size_t count;
    bool listed; /* the folder could be read */
    time_t dir_mtime;

    /* Posters, decoded on a thread of their own: tiles[i] is ready once
     * done > i (pixels NULL if it has none). */
    pthread_t thread;
    bool thread_on;
    atomic_bool cancel;
    atomic_size_t done;
    int tile_size;
    rpod_cover_art_t *tiles;
    lv_image_dsc_t *dscs;
} videos_t;

typedef struct {
    videos_t *c;
    rpod_screen_stack_t *stack;
    lv_obj_t *screen;
    lv_group_t *group;
    lv_obj_t *list;
    size_t applied; /* posters put up in this view */
    lv_timer_t *timer;
    uint32_t checked_at;
} videos_view_t;

static void *poster_thread(void *arg)
{
    videos_t *c = arg;
    unsigned char *buf = malloc(POSTER_MAX_BYTES);
    for (size_t i = 0; i < c->count && !atomic_load(&c->cancel); i++) {
        char path[PATH_MAX + 256];
        rpod_video_poster_path(g_videos.dir, c->list[i].name, path, sizeof(path));
        FILE *f = buf != NULL ? fopen(path, "rb") : NULL;
        if (f != NULL) {
            size_t n = fread(buf, 1, POSTER_MAX_BYTES, f);
            fclose(f);
            if (n > 0 && n < POSTER_MAX_BYTES &&
                !rpod_cover_art_decode(buf, n, c->tile_size, c->tile_size, &c->tiles[i])) {
                c->tiles[i].pixels = NULL;
            }
        }
        atomic_store(&c->done, i + 1);
    }
    free(buf);
    return NULL;
}

static void stop_posters(videos_t *c)
{
    if (c->thread_on) {
        atomic_store(&c->cancel, true);
        pthread_join(c->thread, NULL);
        c->thread_on = false;
    }
    for (size_t i = 0; c->tiles != NULL && i < c->count; i++) {
        rpod_cover_art_free(&c->tiles[i]);
    }
    free(c->tiles);
    free(c->dscs);
    c->tiles = NULL;
    c->dscs = NULL;
}

/* (Re)reads the folder, and starts on its posters. */
static void load(videos_t *c)
{
    stop_posters(c);
    free(c->list);
    c->list = NULL;
    c->count = 0;
    struct stat st;
    c->dir_mtime = stat(g_videos.dir, &st) == 0 ? st.st_mtime : 0;
    c->listed = rpod_video_list(g_videos.dir, &c->list, &c->count);
    atomic_store(&c->cancel, false);
    atomic_store(&c->done, 0);
    c->tile_size = rpod_metrics()->list_art_size;
    if (c->count == 0 || rpod_theme()->classic) {
        return; /* the classic rows have no art */
    }
    c->tiles = calloc(c->count, sizeof(*c->tiles));
    c->dscs = calloc(c->count, sizeof(*c->dscs));
    if (c->tiles != NULL && c->dscs != NULL && pthread_create(&c->thread, NULL, poster_thread, c) == 0) {
        c->thread_on = true;
    } else {
        atomic_store(&c->done, c->count); /* no posters, then */
    }
}

/* Video i's poster, if the thread's done it (done > i) and it has one. */
static const lv_image_dsc_t *poster(videos_t *c, size_t i, size_t done)
{
    if (c->tiles == NULL || done <= i || c->tiles[i].pixels == NULL) {
        return NULL;
    }
    lv_image_dsc_t *d = &c->dscs[i];
    if (d->data == NULL) {
        const rpod_cover_art_t *a = &c->tiles[i];
        d->header.magic = LV_IMAGE_HEADER_MAGIC;
        d->header.cf = LV_COLOR_FORMAT_RGB565;
        d->header.w = (uint32_t)a->w;
        d->header.h = (uint32_t)a->h;
        d->header.stride = (uint32_t)a->w * 2;
        d->data_size = (uint32_t)(a->w * a->h * 2);
        d->data = (const uint8_t *)a->pixels;
    }
    return d;
}

static void on_video(rpod_screen_stack_t *stack, void *item_ctx);

static void render_list(videos_view_t *v)
{
    videos_t *c = v->c;
    const rpod_metrics_t *m = rpod_metrics();
    lv_group_t *prev = lv_group_get_default();
    lv_group_set_default(v->group);
    lv_obj_clean(v->screen);
    v->list = NULL;
    v->applied = 0;

    if (c->count == 0) {
        lv_obj_t *box = lv_obj_create(v->screen);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, m->screen_w - 40, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(box, 6, 0);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(box, LV_ALIGN_CENTER, 0, m->header_h / 2);
        label(box, m->font_title, RPOD_COLOR_TEXT, "No Videos");
        char host[64] = "rpod";
        gethostname(host, sizeof(host) - 1);
        char msg[160];
        snprintf(msg, sizeof(msg), "Upload them from a browser at %s.local", host);
        lv_obj_t *l = label(box, m->font_small, RPOD_COLOR_DIM_TEXT, msg);
        lv_obj_set_width(l, LV_PCT(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_group_set_default(prev);
        return;
    }

    rpod_list_item_t *items = calloc(c->count, sizeof(*items));
    if (items == NULL) {
        lv_group_set_default(prev);
        return;
    }
    /* The posters done by now go in with their rows; the timer puts up
     * the rest as they come (one finishing meanwhile included). */
    size_t done = atomic_load(&c->done);
    for (size_t i = 0; i < c->count; i++) {
        rpod_list_item_t *it = &items[i];
        snprintf(it->text, sizeof(it->text), "%s", c->list[i].title);
        if (c->list[i].duration_ms > 0) {
            fmt_time(it->accessory, sizeof(it->accessory), c->list[i].duration_ms);
        }
        it->has_art_slot = true;
        it->art_glyph = LV_SYMBOL_VIDEO;
        it->thumb = poster(c, i, done);
        it->on_select = on_video;
        it->item_ctx = (void *)(uintptr_t)i;
    }
    v->list = rpod_list_screen_build(v->stack, v->screen, items, c->count);
    free(items);
    rpod_list_screen_enable_scrub(v->screen, v->list);
    v->applied = done < c->count ? done : c->count;
    lv_group_set_default(prev);
}

/* Which row the wheel's on. */
static int32_t focused_row(const videos_view_t *v)
{
    lv_obj_t *f = lv_group_get_focused(v->group);
    return f != NULL && v->list != NULL && lv_obj_get_parent(f) == v->list ? lv_obj_get_index(f) : -1;
}

/* The folder again, if it's changed -- a conversion finished, a video
 * deleted from the web UI -- keeping the wheel on the same video. */
static void refresh_list(videos_view_t *v, bool force)
{
    struct stat st;
    time_t mtime = stat(g_videos.dir, &st) == 0 ? st.st_mtime : 0;
    if (!force && mtime == v->c->dir_mtime) {
        return;
    }
    int32_t row = focused_row(v);
    char name[256] = "";
    if (row >= 0 && (size_t)row < v->c->count) {
        snprintf(name, sizeof(name), "%s", v->c->list[row].name);
    }
    load(v->c);
    render_list(v);
    for (size_t i = 0; name[0] != '\0' && i < v->c->count && v->list != NULL; i++) {
        if (strcmp(v->c->list[i].name, name) == 0) {
            lv_group_focus_obj(lv_obj_get_child(v->list, (int32_t)i));
        }
    }
}

static void list_timer_cb(lv_timer_t *t)
{
    videos_view_t *v = lv_timer_get_user_data(t);
    videos_t *c = v->c;
    if (lv_screen_active() != v->screen) {
        return;
    }
    if (lv_tick_elaps(v->checked_at) >= DIR_CHECK_MS) {
        v->checked_at = lv_tick_get();
        refresh_list(v, false);
    }
    size_t done = atomic_load(&c->done);
    for (; v->list != NULL && v->applied < done && v->applied < c->count; v->applied++) {
        const lv_image_dsc_t *d = poster(c, v->applied, done);
        if (d != NULL) {
            rpod_list_row_set_thumb(lv_obj_get_child(v->list, (int32_t)v->applied), d);
        }
    }
}

static videos_view_t *g_list_view;

static void on_video(rpod_screen_stack_t *stack, void *item_ctx)
{
    videos_view_t *v = g_list_view;
    size_t i = (size_t)(uintptr_t)item_ctx;
    if (v == NULL || i >= v->c->count) {
        return;
    }
    /* The player keeps its own copy: the list may be re-read under it. */
    rpod_video_entry_t *copy = malloc(v->c->count * sizeof(*copy));
    if (copy == NULL) {
        return;
    }
    memcpy(copy, v->c->list, v->c->count * sizeof(*copy));
    push_player(stack, v->c->mpd, copy, v->c->count, i);
}

static void list_event_cb(lv_event_t *e)
{
    videos_view_t *v = lv_event_get_user_data(e);
    if (lv_event_get_code(e) == LV_EVENT_SCREEN_LOADED) {
        refresh_list(v, false); /* back from a video */
    } else if (lv_event_get_code(e) == LV_EVENT_DELETE) {
        lv_timer_delete(v->timer);
        if (g_list_view == v) {
            g_list_view = NULL;
        }
        free(v);
    }
}

static void build_videos(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx)
{
    videos_t *c = ctx;
    videos_view_t *v = calloc(1, sizeof(*v));
    v->c = c;
    v->stack = stack;
    v->screen = screen;
    v->group = lv_group_get_default();
    v->checked_at = lv_tick_get();
    g_list_view = v;
    rpod_screen_stack_set_title(stack, "Videos");
    if (c->tile_size != rpod_metrics()->list_art_size || (c->tiles == NULL) != rpod_theme()->classic) {
        load(c); /* the theme changed: posters at the new size, or none */
    }
    render_list(v);
    v->timer = lv_timer_create(list_timer_cb, LIST_POLL_MS, v);
    lv_obj_add_event_cb(screen, list_event_cb, LV_EVENT_SCREEN_LOADED, v);
    lv_obj_add_event_cb(screen, list_event_cb, LV_EVENT_DELETE, v);
}

static void videos_free(void *ctx)
{
    videos_t *c = ctx;
    stop_posters(c);
    free(c->list);
    free(c);
}

void rpod_videos_init(const char *video_dir)
{
    /* Absolute: MPD plays a video's sound by its full path. */
    char abs[PATH_MAX];
    if (video_dir != NULL && realpath(video_dir, abs) != NULL) {
        video_dir = abs;
    }
    snprintf(g_videos.dir, sizeof(g_videos.dir), "%s", video_dir != NULL ? video_dir : "");
}

void rpod_videos_push(rpod_screen_stack_t *stack, rpod_mpd_t *mpd)
{
    videos_t *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return;
    }
    c->mpd = mpd;
    load(c);
    rpod_screen_stack_push(stack, build_videos, c, videos_free);
}

bool rpod_video_play(rpod_screen_stack_t *stack, rpod_mpd_t *mpd, const char *name)
{
    rpod_video_entry_t *list = NULL;
    size_t count = 0;
    if (!rpod_video_list(g_videos.dir, &list, &count)) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        if (strcmp(list[i].name, name) == 0) {
            push_player(stack, mpd, list, count, i);
            return true;
        }
    }
    free(list);
    return false;
}
