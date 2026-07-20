/*
 * wheel-sniff — raw 4th-gen iPod click wheel packet logger.
 *
 * No parsing, no bit-position assumptions. Prints each completed 32-bit
 * packet as binary plus a timestamp so the real bit map can be derived by
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
 * Wiring (docs/PLAN.md §1.2): CLOCK -> GPIO 23, DATA -> GPIO 25, both with
 * pull-ups (the Pi's internal pull-ups are sufficient).
 */

#include <pigpio.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#define CLOCK_PIN 23
#define DATA_PIN  25

static uint32_t packet = 0;
static int bit_index = 0;
static int recording = 0;
static int ones_run = 0;
static volatile sig_atomic_t running = 1;

static void print_packet(uint32_t p, uint32_t tick_us)
{
    char bits[33];
    for (int i = 0; i < 32; i++) {
        /* Print MSB (bit 31) first, left to right, matching the field
         * position table's bit-N convention in docs/PLAN.md §4.2. */
        bits[i] = ((p >> (31 - i)) & 1) ? '1' : '0';
    }
    bits[32] = '\0';
    printf("%10u us  %s  (0x%08X)\n", tick_us, bits, p);
    fflush(stdout);
}

static void on_bit(int bit, uint32_t tick_us)
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
    }

    if (bit) {
        packet |= (1u << bit_index);
    }
    bit_index++;

    if (bit_index == 32) {
        print_packet(packet, tick_us);
        recording = 0;
        bit_index = 0;
        packet = 0;
    }
}

static void clock_edge_cb(int gpio, int level, uint32_t tick)
{
    (void)gpio;
    if (level != 1) {
        return; /* sample DATA on the rising edge of CLOCK only */
    }
    int bit = gpioRead(DATA_PIN);
    on_bit(bit, tick);
}

static void on_sigint(int sig)
{
    (void)sig;
    running = 0;
}

int main(void)
{
    /* Default mailbox-based DMA memory allocation fails ("initMboxBlock:
     * init mbox zaps failed") when the vc4-kms-v3d overlay is active — the
     * DRM/KMS driver claims the legacy GPU memory pool pigpio's mailbox
     * path needs, regardless of gpu_mem= in config.txt. PAGEMAP mode
     * sidesteps the mailbox entirely. See docs/PLAN.md §4.5. */
    gpioCfgMemAlloc(PI_MEM_ALLOC_PAGEMAP);

    /* Default 5us sample rate drops edges on this protocol — see
     * docs/PLAN.md §4.5. Must be set before gpioInitialise(). */
    if (gpioCfgClock(1, 1, 0) != 0) {
        fprintf(stderr, "wheel-sniff: gpioCfgClock failed\n");
        return 1;
    }

    if (gpioInitialise() < 0) {
        fprintf(stderr, "wheel-sniff: gpioInitialise failed (are you root?)\n");
        return 1;
    }

    gpioSetMode(CLOCK_PIN, PI_INPUT);
    gpioSetMode(DATA_PIN, PI_INPUT);
    gpioSetPullUpDown(CLOCK_PIN, PI_PUD_UP);
    gpioSetPullUpDown(DATA_PIN, PI_PUD_UP);

    gpioSetAlertFunc(CLOCK_PIN, clock_edge_cb);

    signal(SIGINT, on_sigint);

    fprintf(stderr, "wheel-sniff: listening on CLOCK=GPIO%d DATA=GPIO%d, "
                     "press each button in isolation then rotate slowly. "
                     "Ctrl-C to stop.\n", CLOCK_PIN, DATA_PIN);

    while (running) {
        gpioSleep(PI_TIME_RELATIVE, 1, 0);
    }

    gpioTerminate();
    return 0;
}
