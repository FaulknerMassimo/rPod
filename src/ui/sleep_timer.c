#include "sleep_timer.h"

#include "lvgl.h"

#include <stddef.h>

static struct {
    void (*fire)(void *ctx);
    void *ctx;
    lv_timer_t *timer; /* NULL when off */
    unsigned minutes;
    uint32_t deadline_ms;
} g;

static void fire_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    g.timer = NULL;
    g.minutes = 0;
    g.fire(g.ctx);
}

void rpod_sleep_timer_init(void (*fire)(void *ctx), void *ctx)
{
    g.fire = fire;
    g.ctx = ctx;
}

void rpod_sleep_timer_set(unsigned minutes)
{
    if (g.timer != NULL) {
        lv_timer_delete(g.timer);
        g.timer = NULL;
    }
    g.minutes = minutes;
    if (minutes == 0 || g.fire == NULL) {
        g.minutes = 0;
        return;
    }
    uint32_t ms = minutes * 60u * 1000u;
    g.deadline_ms = lv_tick_get() + ms;
    g.timer = lv_timer_create(fire_cb, ms, NULL);
}

unsigned rpod_sleep_timer_minutes(void)
{
    return g.minutes;
}

uint32_t rpod_sleep_timer_remaining_ms(void)
{
    if (g.timer == NULL) {
        return 0;
    }
    int32_t left = (int32_t)(g.deadline_ms - lv_tick_get());
    return left > 0 ? (uint32_t)left : 0;
}
