/*
 * Button taps vs holds, seek repeats, and sleep/wake (src/input/gestures.c).
 */

#include "input/gestures.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);   \
            failures++;                                                                \
        }                                                                              \
    } while (0)

/* Every action appends one character to the log: M(enu) P(lay/pause) S(leep)
 * N(ext) V (prev) W(ake), > < for a scan starting, 0 for it ending, C/c for the
 * centre level going down/up. */
static char log_buf[256];

static void put(char c)
{
    size_t n = strlen(log_buf);
    if (n + 1 < sizeof(log_buf)) {
        log_buf[n] = c;
        log_buf[n + 1] = '\0';
    }
}

static void a_menu(void *ctx) { (void)ctx; put('M'); }
static void a_play(void *ctx) { (void)ctx; put('P'); }
static void a_sleep(void *ctx) { (void)ctx; put('S'); }
static void a_next(void *ctx) { (void)ctx; put('N'); }
static void a_prev(void *ctx) { (void)ctx; put('V'); }
static void a_wake(void *ctx) { (void)ctx; put('W'); }
static void a_seek(int dir, void *ctx) { (void)ctx; put(dir > 0 ? '>' : dir < 0 ? '<' : '0'); }
static void a_center(bool held, void *ctx) { (void)ctx; put(held ? 'C' : 'c'); }

static rpod_gestures_t g;
static uint32_t now;

static void reset(void)
{
    rpod_input_actions_t actions = {
        .menu = a_menu, .play_pause = a_play, .sleep = a_sleep, .next = a_next,
        .prev = a_prev, .seek = a_seek, .wake = a_wake,
    };
    rpod_gestures_init(&g, &actions, a_center, NULL);
    log_buf[0] = '\0';
    now = 1000;
}

/* Advances time in 50 ms ticks, like input.c's timer. */
static void run(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 50) {
        now += 50;
        if (rpod_gestures_timing(&g)) {
            rpod_gestures_tick(&g, now);
        }
    }
}

static void press(rpod_button_t b) { rpod_gestures_feed(&g, b, true, now); }
static void release(rpod_button_t b) { rpod_gestures_feed(&g, b, false, now); }

static void expect(const char *want, int line)
{
    if (strcmp(log_buf, want) != 0) {
        fprintf(stderr, "%s:%d: log \"%s\", want \"%s\"\n", __FILE__, line, log_buf, want);
        failures++;
    }
}
#define EXPECT(s) expect((s), __LINE__)

static void test_taps(void)
{
    reset();
    press(RPOD_BTN_MENU);
    EXPECT("M");                 /* Menu acts on press */
    release(RPOD_BTN_MENU);
    press(RPOD_BTN_PLAY_PAUSE);
    EXPECT("M");                 /* Play/Pause waits for release */
    run(200);
    release(RPOD_BTN_PLAY_PAUSE);
    press(RPOD_BTN_NEXT);
    run(100);
    release(RPOD_BTN_NEXT);
    press(RPOD_BTN_PREV);
    release(RPOD_BTN_PREV);
    press(RPOD_BTN_CENTER);
    release(RPOD_BTN_CENTER);
    EXPECT("MPNVCc");

    /* Repeated edges are no-ops. */
    reset();
    press(RPOD_BTN_MENU);
    press(RPOD_BTN_MENU);
    release(RPOD_BTN_MENU);
    release(RPOD_BTN_MENU);
    release(RPOD_BTN_PLAY_PAUSE);
    EXPECT("M");
}

static void test_seek(void)
{
    reset();
    press(RPOD_BTN_NEXT);
    run(450);
    EXPECT("");                  /* not held long enough yet */
    run(100);
    EXPECT(">");                 /* 500 ms: the scan starts */
    CHECK(!rpod_gestures_timing(&g)); /* nothing more to time */
    run(1000);
    EXPECT(">");                 /* once -- the app runs the scan */
    release(RPOD_BTN_NEXT);
    EXPECT(">0");                /* ends, and no skip on release */

    reset();
    press(RPOD_BTN_PREV);
    run(600);
    release(RPOD_BTN_PREV);
    EXPECT("<0");
}

static void test_sleep_and_wake(void)
{
    reset();
    press(RPOD_BTN_PLAY_PAUSE);
    run(1450);
    EXPECT("");
    run(100);
    EXPECT("S");                 /* fires while still held */
    CHECK(!rpod_gestures_timing(&g));
    rpod_gestures_sleep(&g);     /* what the app's sleep action does */
    run(500);
    release(RPOD_BTN_PLAY_PAUSE);
    EXPECT("S");                 /* no toggle on the release */

    /* Asleep: rotation isn't this module's, but any press only wakes. */
    press(RPOD_BTN_NEXT);
    run(1000);
    release(RPOD_BTN_NEXT);
    EXPECT("SW");                /* woke; no seek, no skip */
    press(RPOD_BTN_NEXT);
    release(RPOD_BTN_NEXT);
    EXPECT("SWN");               /* awake again */

    /* Centre wakes without reaching the encoder. */
    rpod_gestures_sleep(&g);
    press(RPOD_BTN_CENTER);
    release(RPOD_BTN_CENTER);
    EXPECT("SWNW");

    /* Menu sleeps from the press; its own release mustn't wake. */
    reset();
    press(RPOD_BTN_MENU);
    rpod_gestures_sleep(&g);
    release(RPOD_BTN_MENU);
    EXPECT("M");
    CHECK(g.asleep);
    press(RPOD_BTN_MENU);
    release(RPOD_BTN_MENU);
    EXPECT("MW");

    /* Sleeping mid-seek closes the seek; centre held is reported. */
    reset();
    press(RPOD_BTN_CENTER);
    press(RPOD_BTN_NEXT);
    run(600);
    CHECK(rpod_gestures_sleep(&g));
    release(RPOD_BTN_NEXT);
    release(RPOD_BTN_CENTER);
    EXPECT("C>0");
}

static void test_cancel(void)
{
    reset();
    press(RPOD_BTN_PLAY_PAUSE);
    press(RPOD_BTN_PREV);
    run(600);
    CHECK(!rpod_gestures_cancel(&g));
    EXPECT("<0");
    CHECK(!g.asleep);
    CHECK(!rpod_gestures_timing(&g));
    /* After a reconnect, a fresh press works normally. */
    press(RPOD_BTN_PLAY_PAUSE);
    release(RPOD_BTN_PLAY_PAUSE);
    EXPECT("<0P");
}

static void test_tick_wrap(void)
{
    reset();
    now = UINT32_MAX - 100;
    press(RPOD_BTN_NEXT);
    run(550);
    EXPECT(">");
}

int main(void)
{
    test_taps();
    test_seek();
    test_sleep_and_wake();
    test_cancel();
    test_tick_wrap();
    if (failures != 0) {
        fprintf(stderr, "test_gestures: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_gestures: ok\n");
    return 0;
}
