/*
 * wheel-sniff — raw 4th-gen iPod click wheel packet logger.
 *
 * No parsing, no bit-position assumptions. Prints each completed 32-bit
 * packet as binary plus timing so the real bit map can be derived by
 * diffing packets against known button presses / wheel motion.
 *
 * Run this FIRST, on hardware, before writing anything that parses packets.
 * Do not carry over the field-position constants from the
 * dupontgu/retro-ipod-spotify-client reference — they were derived against a
 * buggy bit-packing function (setBit() called with a starting index of 0,
 * causing `1 << (k - 1)` to evaluate `1 << -1` for the first bit) and only
 * make sense paired with that bug. This tool packs bits correctly
 * (`1u << bit_index`, no off-by-one), so its output will not match those
 * constants — that's expected. See docs/PLAN.md §4.3.
 *
 * Sampling: pigpio's alert callbacks are delivered from a buffer of DMA
 * samples roughly a millisecond after the fact, so calling gpioRead(DATA)
 * from a CLOCK alert reads DATA's level *now*, not at the clock edge. This
 * uses gpioSetGetSamplesFunc() instead: each sample carries the whole GPIO
 * bank's level at one instant, so DATA is read from the same sample as the
 * CLOCK rising edge.
 *
 * Wiring (docs/PLAN.md §1.2): CLOCK -> GPIO 23, DATA -> GPIO 25, both with
 * pull-ups (the Pi's internal pull-ups are sufficient).
 *
 * Usage:
 *   wheel-sniff              log packets to stdout until Ctrl-C
 *   wheel-sniff --guided     walk through each button/gesture in turn,
 *                            prompting on stderr and writing a "# phase"
 *                            marker to stdout before each one:
 *                              sudo ./wheel-sniff --guided > wheel-sniff.log
 *
 * Only one pigpio process can run at a time — stop rpod-wheel first.
 */

#include <pigpio.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLOCK_PIN 23
#define DATA_PIN  25

/* Decoder state — touched only from pigpio's sample-callback thread. */
static uint32_t packet = 0;
static int bit_index = 0;
static int recording = 0;
static int ones_run = 0;
static int prev_clock = 1;
static uint32_t last_rise_tick = 0;
static uint32_t last_fall_tick = 0;
static uint32_t packet_start_tick = 0;
static uint32_t prev_packet_end_tick = 0;
static int have_prev_packet = 0;

/* Stats — written by the callback thread, read by main. */
static atomic_ulong n_edges;
static atomic_ulong n_packets;
static atomic_uint min_high_us = UINT32_MAX;
static atomic_uint min_low_us = UINT32_MAX;

static volatile sig_atomic_t running = 1;

static void atomic_min(atomic_uint *v, unsigned x)
{
    unsigned cur = atomic_load(v);
    while (x < cur && !atomic_compare_exchange_weak(v, &cur, x)) {
    }
}

static void print_packet(uint32_t p, uint32_t start, uint32_t end)
{
    char bits[33];
    for (int i = 0; i < 32; i++) {
        /* Print MSB (bit 31) first, left to right, matching the field
         * position table's bit-N convention in docs/PLAN.md §4.2. The first
         * bit to arrive is therefore the rightmost character. */
        bits[i] = ((p >> (31 - i)) & 1) ? '1' : '0';
    }
    bits[32] = '\0';

    /* gap = quiet time since the previous packet's last edge; dur = first
     * to last clock edge of this packet (31 bit periods). */
    char gap[16] = "-";
    if (have_prev_packet) {
        snprintf(gap, sizeof(gap), "%u", start - prev_packet_end_tick);
    }
    printf("%10u us  %s  0x%08X  gap=%s dur=%u\n", start, bits, p, gap, end - start);
    fflush(stdout);

    prev_packet_end_tick = end;
    have_prev_packet = 1;
    atomic_fetch_add(&n_packets, 1);
}

/* Packet framing, docs/PLAN.md §4.2: 32 consecutive 1 bits is idle;
 * recording starts on the first 0 bit after idle; 32 bits later, emit. */
static void on_bit(int bit, uint32_t tick)
{
    if (bit) {
        ones_run++;
        if (ones_run >= 32) {
            /* Idle / inter-packet gap. Reset unconditionally, even if we
             * thought we were mid-packet — this recovers from desync. */
            recording = 0;
            bit_index = 0;
            packet = 0;
            ones_run = 32;
            return;
        }
    } else {
        ones_run = 0;
    }

    if (!recording) {
        if (bit) {
            return; /* still idle, waiting for the first 0 */
        }
        recording = 1;
        bit_index = 0;
        packet = 0;
        packet_start_tick = tick;
    }

    if (bit) {
        packet |= (1u << bit_index);
    }
    bit_index++;

    if (bit_index == 32) {
        print_packet(packet, packet_start_tick, tick);
        recording = 0;
        bit_index = 0;
        packet = 0;
    }
}

static void samples_cb(const gpioSample_t *samples, int count)
{
    for (int i = 0; i < count; i++) {
        uint32_t level = samples[i].level;
        uint32_t tick = samples[i].tick;
        int clock = (level >> CLOCK_PIN) & 1;

        if (clock && !prev_clock) {
            /* Rising edge: DATA from this same sample. */
            atomic_fetch_add(&n_edges, 1);
            if (last_fall_tick != 0) {
                atomic_min(&min_low_us, tick - last_fall_tick);
            }
            last_rise_tick = tick;
            on_bit((level >> DATA_PIN) & 1, tick);
        } else if (!clock && prev_clock && last_rise_tick != 0) {
            atomic_min(&min_high_us, tick - last_rise_tick);
            last_fall_tick = tick;
        }
        prev_clock = clock;
    }
}

static void on_sigint(int sig)
{
    if (sig != SIGCONT) {
        running = 0;
    }
}

/* --- guided mode ------------------------------------------------------- */

static const struct {
    const char *name;
    const char *prompt;
} k_phases[] = {
    { "idle",   "Hands off the wheel entirely for a couple of seconds." },
    { "touch",  "Rest one finger on the wheel ring. Don't press or move it." },
    { "center", "Press and release the CENTER button 3 times (slowly)." },
    { "menu",   "Press and release MENU (top) 3 times." },
    { "play",   "Press and release PLAY/PAUSE (bottom) 3 times." },
    { "prev",   "Press and release PREV (left) 3 times." },
    { "next",   "Press and release NEXT (right) 3 times." },
    { "cw",     "Rotate slowly CLOCKWISE through one full turn, then lift." },
    { "ccw",    "Rotate slowly COUNTER-CLOCKWISE through one full turn, then lift." },
};

static int wait_for_enter(void)
{
    int c;
    while ((c = getchar()) != EOF && c != '\n') {
    }
    return c != EOF && running;
}

static void run_guided(void)
{
    size_t n = sizeof(k_phases) / sizeof(k_phases[0]);
    for (size_t i = 0; i < n && running; i++) {
        unsigned long p0 = atomic_load(&n_packets);
        unsigned long e0 = atomic_load(&n_edges);

        printf("# phase %s\n", k_phases[i].name);
        fflush(stdout);
        fprintf(stderr, "\n[%zu/%zu] %s\n      Press Enter here when done.", i + 1, n,
                k_phases[i].prompt);
        if (!wait_for_enter()) {
            break;
        }
        /* Let the ~1 ms-delayed sample callback catch up before counting. */
        gpioDelay(20000);
        fprintf(stderr, "      -> %lu packets, %lu clock edges\n",
                atomic_load(&n_packets) - p0, atomic_load(&n_edges) - e0);
    }
    printf("# phase end\n");
    fflush(stdout);
}

int main(int argc, char **argv)
{
    int guided = argc > 1 && strcmp(argv[1], "--guided") == 0;

    /* Default mailbox-based DMA memory allocation fails ("initMboxBlock:
     * init mbox zaps failed") when the vc4-kms-v3d overlay is active — the
     * DRM/KMS driver claims the legacy GPU memory pool pigpio's mailbox
     * path needs, regardless of gpu_mem= in config.txt. PAGEMAP mode
     * sidesteps the mailbox entirely. See docs/PLAN.md §4.5. */
    gpioCfgMemAlloc(PI_MEM_ALLOC_PAGEMAP);

    /* pigpio's library otherwise opens a root GPIO-control socket on TCP
     * port 8888 and a /dev/pigpio FIFO. */
    gpioCfgInterfaces(PI_DISABLE_FIFO_IF | PI_DISABLE_SOCK_IF);

    /* Default 5us sample rate drops edges on this protocol — see
     * docs/PLAN.md §4.5. PCM paces the DMA: on the Pi 3B dev board, PWM is
     * the headphone jack MPD plays through. Must be set before
     * gpioInitialise(). */
    if (gpioCfgClock(1, PI_CLOCK_PCM, 0) != 0) {
        fprintf(stderr, "wheel-sniff: gpioCfgClock failed\n");
        return 1;
    }

    if (gpioInitialise() < 0) {
        fprintf(stderr, "wheel-sniff: gpioInitialise failed (are you root? is rpod-wheel running?)\n");
        return 1;
    }

    gpioSetMode(CLOCK_PIN, PI_INPUT);
    gpioSetMode(DATA_PIN, PI_INPUT);
    gpioSetPullUpDown(CLOCK_PIN, PI_PUD_UP);
    gpioSetPullUpDown(DATA_PIN, PI_PUD_UP);

    gpioSetGetSamplesFunc(samples_cb, (1u << CLOCK_PIN) | (1u << DATA_PIN));

    /* Through pigpio, which hooks every signal and treats unknown ones --
     * including the SIGCONT that `timeout` and systemd send -- as fatal. */
    gpioSetSignalFunc(SIGINT, on_sigint);
    gpioSetSignalFunc(SIGTERM, on_sigint);
    gpioSetSignalFunc(SIGCONT, on_sigint);

    fprintf(stderr, "wheel-sniff: listening on CLOCK=GPIO%d DATA=GPIO%d.\n", CLOCK_PIN, DATA_PIN);

    if (guided) {
        run_guided();
    } else {
        fprintf(stderr, "Press each button in isolation, then rotate slowly. Ctrl-C to stop.\n");
        while (running) {
            gpioSleep(PI_TIME_RELATIVE, 0, 200000);
        }
    }

    gpioSetGetSamplesFunc(NULL, 0);

    /* The narrowest CLOCK pulse bounds how coarse the sample period can
     * get (docs/PLAN.md §4.5's 1 us vs 2 us question). To both the log and
     * the terminal. */
    unsigned hi = atomic_load(&min_high_us), lo = atomic_load(&min_low_us);
    char total[160];
    snprintf(total, sizeof(total),
             "# total: %lu packets, %lu clock edges, min CLOCK high %d us, min CLOCK low %d us\n",
             atomic_load(&n_packets), atomic_load(&n_edges),
             hi == UINT32_MAX ? -1 : (int)hi, lo == UINT32_MAX ? -1 : (int)lo);
    fputs(total, stdout);
    fflush(stdout);
    fputs(total, stderr);

    gpioTerminate();
    return 0;
}
