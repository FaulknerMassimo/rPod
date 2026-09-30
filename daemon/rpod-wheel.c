/*
 * rpod-wheel — click wheel decoder daemon.
 *
 * Runs as a privileged root process (pigpio needs /dev/mem). Decodes the
 * 4th-gen click wheel's synchronous serial stream per docs/PLAN.md §4 and
 * publishes normalised events over a Unix domain socket for the
 * (unprivileged) UI process to consume.
 *
 * Packet framing follows docs/PLAN.md §4.2 and is the same algorithm as
 * tools/wheel-sniff.c: sample DATA on CLOCK's rising edge; 32 consecutive 1
 * bits means idle; recording starts on the first 0 bit after idle; 32 bits
 * later, parse and reset. On top of that, a quiet gap longer than any
 * intra-packet clock gap abandons a half-received packet (so one dropped
 * edge can't shift every later packet by a bit), and packets without the
 * documented preamble are discarded.
 *
 * The wheel only sends a packet when something changes -- nothing repeats
 * while a button is held or a finger rests on the ring -- and every packet
 * carries the full state, so events are just the diff between consecutive
 * packets.
 *
 * DATA is read from the same pigpio sample as the CLOCK rising edge
 * (gpioSetGetSamplesFunc) — never with gpioRead() from a callback: pigpio
 * delivers callbacks from a buffer of DMA samples about a millisecond late,
 * so gpioRead() would return DATA's level now, not at the edge.
 */

#include "wheel_bits.h"
#include "wheel_protocol.h"

#include <errno.h>
#include <pigpio.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define RPOD_WHEEL_MAX_CLIENTS 8
#define RPOD_HAPTIC_CONF_PATH "/etc/rpod/wheel.conf"
#define RPOD_HAPTIC_DIVISOR_DEFAULT 2

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_reload_conf = 0;

/* --- client fan-out ------------------------------------------------- */

static pthread_mutex_t g_clients_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_client_fds[RPOD_WHEEL_MAX_CLIENTS];
static int g_client_count = 0;

static void clients_add(int fd)
{
    pthread_mutex_lock(&g_clients_lock);
    if (g_client_count < RPOD_WHEEL_MAX_CLIENTS) {
        g_client_fds[g_client_count++] = fd;
    } else {
        fprintf(stderr, "rpod-wheel: client table full, dropping connection\n");
        close(fd);
    }
    pthread_mutex_unlock(&g_clients_lock);
}

/* Non-blocking sends — a slow or dead client gets dropped rather than
 * stalling the decoder. */
static void clients_broadcast(const struct rpod_wheel_event *ev)
{
    pthread_mutex_lock(&g_clients_lock);
    for (int i = 0; i < g_client_count; /* conditional increment below */) {
        ssize_t n = send(g_client_fds[i], ev, sizeof(*ev), MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n != (ssize_t)sizeof(*ev)) {
            close(g_client_fds[i]);
            g_client_fds[i] = g_client_fds[--g_client_count];
            continue;
        }
        i++;
    }
    pthread_mutex_unlock(&g_clients_lock);
}

/* --- haptics ---------------------------------------------------------- */
/*
 * The reference fires a haptic pulse every other wheel position — too
 * sensitive otherwise. That divisor interacts with scroll acceleration
 * (docs/PLAN.md §8.2) and needs tuning by feel, so it's a runtime value:
 * re-read from RPOD_HAPTIC_CONF_PATH on SIGHUP rather than baked in.
 */

static volatile int g_haptic_divisor = RPOD_HAPTIC_DIVISOR_DEFAULT;
static int g_haptic_wave_id = -1;

static void haptics_reload_config(void)
{
    FILE *f = fopen(RPOD_HAPTIC_CONF_PATH, "r");
    if (f == NULL) {
        return; /* no config file present — keep current divisor */
    }
    int v;
    if (fscanf(f, "%d", &v) == 1 && v > 0) {
        g_haptic_divisor = v;
        fprintf(stderr, "rpod-wheel: haptic divisor set to %d\n", v);
    }
    fclose(f);
}

static void haptics_init(void)
{
    gpioSetMode(RPOD_WHEEL_HAPTIC_PIN, PI_OUTPUT);

    gpioPulse_t pulse[2];
    pulse[0].gpioOn  = 1u << RPOD_WHEEL_HAPTIC_PIN;
    pulse[0].gpioOff = 0;
    pulse[0].usDelay = 8000;
    pulse[1].gpioOn  = 0;
    pulse[1].gpioOff = 1u << RPOD_WHEEL_HAPTIC_PIN;
    pulse[1].usDelay = 2000;

    gpioWaveAddNew();
    gpioWaveAddGeneric(2, pulse);
    g_haptic_wave_id = gpioWaveCreate();

    haptics_reload_config();
}

static void haptics_fire(void)
{
    if (g_haptic_wave_id >= 0 && gpioWaveTxBusy() == 0) {
        gpioWaveTxSend(g_haptic_wave_id, PI_WAVE_MODE_ONE_SHOT);
    }
}

/* --- hold switch -------------------------------------------------------- */

static int hold_engaged(void)
{
    /* Active-low: assumes the switch pulls GPIO 16 to GND when hold is on,
     * with the internal pull-up holding it high otherwise (so an unwired
     * switch reads as "not held"). Verify against real hardware and flip
     * this if the wiring says otherwise. */
    return gpioRead(RPOD_WHEEL_HOLD_PIN) == 0;
}

/* --- event emission ------------------------------------------------------ */

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static void emit(uint8_t type, uint8_t code, int8_t value, uint32_t position, uint64_t ts)
{
    struct rpod_wheel_event ev = {
        .type = type,
        .code = code,
        .value = value,
        ._pad = 0,
        .position = position,
        .timestamp_us = ts,
    };
    clients_broadcast(&ev);
}

/* Wrap-corrected delta around the position ring — docs/PLAN.md §4.2 (just
 * past the top of the ring back to 0 is a small forward step, not a
 * near-full-turn reverse). */
static int8_t wheel_delta(unsigned curr, unsigned prev)
{
    int d = (int)curr - (int)prev;
    if (d > RPOD_WHEEL_POS_RING / 2) {
        d -= RPOD_WHEEL_POS_RING;
    } else if (d < -RPOD_WHEEL_POS_RING / 2) {
        d += RPOD_WHEEL_POS_RING;
    }
    return (int8_t)d;
}

/* --- decoded state -> events --------------------------------------------- */
/* pigpio's sample-callback thread only (plus the SIGHUP stats read). */

typedef struct {
    uint8_t buttons;   /* bit (1 << rpod_wheel_button) per held button */
    bool touched;
    unsigned position;
} wheel_state_t;

static const struct {
    uint8_t code;
    int bit;
} k_buttons[] = {
    { RPOD_WHEEL_BTN_CENTER, RPOD_WHEEL_BIT_CENTER },
    { RPOD_WHEEL_BTN_LEFT,   RPOD_WHEEL_BIT_LEFT   },
    { RPOD_WHEEL_BTN_RIGHT,  RPOD_WHEEL_BIT_RIGHT  },
    { RPOD_WHEEL_BTN_UP,     RPOD_WHEEL_BIT_UP     },
    { RPOD_WHEEL_BTN_DOWN,   RPOD_WHEEL_BIT_DOWN   },
};
#define N_BUTTONS (sizeof(k_buttons) / sizeof(k_buttons[0]))

static wheel_state_t g_state;          /* as last reported to clients */
static volatile unsigned long g_bad_preamble = 0;

/* Reports every difference between the current state and `next`, then
 * adopts it. `rotate` gates the wheel delta: only while the finger stays on
 * the ring between two packets — a fresh touch lands wherever the finger
 * went down, and that jump isn't rotation. */
static void apply_state(const wheel_state_t *next, bool rotate, uint64_t ts)
{
    for (size_t i = 0; i < N_BUTTONS; i++) {
        uint8_t mask = (uint8_t)(1u << k_buttons[i].code);
        if ((g_state.buttons ^ next->buttons) & mask) {
            emit(RPOD_WHEEL_EVENT_BUTTON, k_buttons[i].code,
                 (next->buttons & mask) ? 1 : 0, next->position, ts);
        }
    }

    if (g_state.touched != next->touched) {
        emit(RPOD_WHEEL_EVENT_TOUCH, 0, next->touched ? 1 : 0, next->position, ts);
    }

    if (rotate && g_state.touched && next->touched && next->position != g_state.position) {
        emit(RPOD_WHEEL_EVENT_WHEEL, 0, wheel_delta(next->position, g_state.position),
             next->position, ts);

        static int step = 0;
        if (++step >= g_haptic_divisor) {
            step = 0;
            haptics_fire();
        }
    }

    g_state = *next;
}

static void handle_packet(uint32_t packet, uint64_t ts)
{
    if ((packet & RPOD_WHEEL_PREAMBLE_MASK) != RPOD_WHEEL_PREAMBLE) {
        g_bad_preamble++;
        return; /* desynced or corrupted — the wheel resends continuously */
    }

    wheel_state_t next = {
        .buttons = 0,
        .touched = ((packet >> RPOD_WHEEL_BIT_TOUCH) & 1) != 0,
        .position = (packet >> RPOD_WHEEL_POS_SHIFT) & RPOD_WHEEL_POS_MASK,
    };
    for (size_t i = 0; i < N_BUTTONS; i++) {
        if ((packet >> k_buttons[i].bit) & 1) {
            next.buttons |= (uint8_t)(1u << k_buttons[i].code);
        }
    }

    if (hold_engaged()) {
        /* Keep decoding, suppress emission (docs/PLAN.md §4.5): to the UI
         * the wheel just sits idle while hold is on. Anything down when it
         * engaged gets its release (only on the first packet -- after that
         * this is a no-op), and anything still down once it's off again
         * gets reported fresh, with no rotation replayed. */
        wheel_state_t idle = { .buttons = 0, .touched = false, .position = next.position };
        apply_state(&idle, false, ts);
    } else {
        apply_state(&next, true, ts);
    }
}

/* --- packet framing (docs/PLAN.md §4.2, mirrors tools/wheel-sniff.c) ---- */
/* Sample-callback thread only. */

static uint32_t g_packet = 0;
static int g_bit_index = 0;
static int g_recording = 0;
static int g_ones_run = 0;
static int g_prev_clock = 1;
static uint32_t g_last_rise_tick = 0;

static void framing_reset(void)
{
    g_recording = 0;
    g_bit_index = 0;
    g_packet = 0;
}

static void on_bit(int bit, uint64_t ts)
{
    if (bit) {
        g_ones_run++;
        if (g_ones_run >= 32) {
            framing_reset();
            g_ones_run = 32;
            return;
        }
    } else {
        g_ones_run = 0;
    }

    if (!g_recording) {
        if (bit) {
            return; /* still idle, waiting for the first 0 */
        }
        g_recording = 1;
        g_bit_index = 0;
        g_packet = 0;
    }

    if (bit) {
        g_packet |= (1u << g_bit_index);
    }
    g_bit_index++;

    if (g_bit_index == 32) {
        handle_packet(g_packet, ts);
        framing_reset();
    }
}

static void samples_cb(const gpioSample_t *samples, int count)
{
    /* Map pigpio's 32-bit microsecond ticks onto CLOCK_MONOTONIC so event
     * timestamps reflect when the packet arrived on the wire, not when this
     * (batched, ~1 ms late) callback got around to it. */
    uint64_t mono_now = now_us();
    uint32_t tick_now = gpioTick();

    for (int i = 0; i < count; i++) {
        uint32_t level = samples[i].level;
        uint32_t tick = samples[i].tick;
        int clock = (level >> RPOD_WHEEL_CLOCK_PIN) & 1;

        if (clock && !g_prev_clock) {
            if (g_recording && tick - g_last_rise_tick > RPOD_WHEEL_RESYNC_GAP_US) {
                framing_reset(); /* stale half-packet from before a gap */
            }
            g_last_rise_tick = tick;
            on_bit((level >> RPOD_WHEEL_DATA_PIN) & 1, mono_now - (uint32_t)(tick_now - tick));
        }
        g_prev_clock = clock;
    }
}

/* --- socket server -------------------------------------------------------- */

static int make_listen_socket(void)
{
    if (mkdir("/run/rpod", 0755) != 0 && errno != EEXIST) {
        perror("rpod-wheel: mkdir /run/rpod");
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("rpod-wheel: socket");
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, RPOD_WHEEL_SOCK_PATH, sizeof(addr.sun_path) - 1);
    unlink(RPOD_WHEEL_SOCK_PATH);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("rpod-wheel: bind");
        close(fd);
        return -1;
    }
    chmod(RPOD_WHEEL_SOCK_PATH, 0666);

    if (listen(fd, 4) != 0) {
        perror("rpod-wheel: listen");
        close(fd);
        return -1;
    }

    return fd;
}

static void on_signal(int sig)
{
    if (sig == SIGHUP) {
        g_reload_conf = 1;
    } else if (sig != SIGCONT) {
        g_running = 0;
    }
}

int main(void)
{
    /* Default mailbox-based DMA memory allocation fails ("initMboxBlock:
     * init mbox zaps failed") when the vc4-kms-v3d overlay is active — the
     * DRM/KMS driver claims the legacy GPU memory pool pigpio's mailbox
     * path needs, regardless of gpu_mem= in config.txt. PAGEMAP mode
     * sidesteps the mailbox entirely. See docs/PLAN.md §4.5. */
    gpioCfgMemAlloc(PI_MEM_ALLOC_PAGEMAP);

    /* pigpio's library otherwise opens a root GPIO-control socket on TCP
     * port 8888 (reachable from the network) and a /dev/pigpio FIFO. */
    gpioCfgInterfaces(PI_DISABLE_FIFO_IF | PI_DISABLE_SOCK_IF);

    /* The DMA sampling is paced by the PCM or PWM peripheral, which then
     * can't do its day job: PCM is I²S (the DAC), PWM is the Pi 3B's
     * headphone jack and hardware-PWM backlight dimming. PCM by default --
     * right for the 3B dev board, which plays through the jack.
     * RPOD_WHEEL_PACING=pwm once the I²S DAC is wired (docs/PLAN.md §4.5). */
    const char *pacing = getenv("RPOD_WHEEL_PACING");
    unsigned peripheral = (pacing != NULL && strcmp(pacing, "pwm") == 0) ? PI_CLOCK_PWM : PI_CLOCK_PCM;

    /* Default 5us sample rate drops edges on this protocol — docs/PLAN.md
     * §4.5. 2us, not 1us: the narrowest CLOCK pulse measured on hardware is
     * 7us (docs/clickwheel-protocol.md), so every half-cycle still gets >= 3
     * samples, and 1us cost ~18% of a core even with the wheel idle. Must
     * be set before gpioInitialise(). */
    if (gpioCfgClock(2, peripheral, 0) != 0) {
        fprintf(stderr, "rpod-wheel: gpioCfgClock failed\n");
        return 1;
    }
    if (gpioInitialise() < 0) {
        fprintf(stderr, "rpod-wheel: gpioInitialise failed (are you root?)\n");
        return 1;
    }

    gpioSetMode(RPOD_WHEEL_CLOCK_PIN, PI_INPUT);
    gpioSetMode(RPOD_WHEEL_DATA_PIN, PI_INPUT);
    gpioSetMode(RPOD_WHEEL_HOLD_PIN, PI_INPUT);
    gpioSetPullUpDown(RPOD_WHEEL_CLOCK_PIN, PI_PUD_UP);
    gpioSetPullUpDown(RPOD_WHEEL_DATA_PIN, PI_PUD_UP);
    gpioSetPullUpDown(RPOD_WHEEL_HOLD_PIN, PI_PUD_UP);

    haptics_init();

    int listen_fd = make_listen_socket();
    if (listen_fd < 0) {
        gpioTerminate();
        return 1;
    }

    /* Through pigpio, not signal(): it hooks every signal so that a fatal
     * one stops its DMA before exit. Its fallback treats anything it
     * doesn't recognise as fatal, including the SIGCONT systemd sends right
     * after SIGTERM on stop, so that gets a no-op. */
    gpioSetSignalFunc(SIGINT, on_signal);
    gpioSetSignalFunc(SIGTERM, on_signal);
    gpioSetSignalFunc(SIGHUP, on_signal);
    gpioSetSignalFunc(SIGCONT, on_signal);

    gpioSetGetSamplesFunc(samples_cb, (1u << RPOD_WHEEL_CLOCK_PIN) | (1u << RPOD_WHEEL_DATA_PIN));

    fprintf(stderr, "rpod-wheel: listening on %s, decoding CLOCK=GPIO%d DATA=GPIO%d, %s-paced\n",
            RPOD_WHEEL_SOCK_PATH, RPOD_WHEEL_CLOCK_PIN, RPOD_WHEEL_DATA_PIN,
            peripheral == PI_CLOCK_PWM ? "PWM" : "PCM");

    while (g_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };

        int r = select(listen_fd + 1, &rfds, NULL, NULL, &tv);
        if (r > 0 && FD_ISSET(listen_fd, &rfds)) {
            int fd = accept(listen_fd, NULL, NULL);
            if (fd >= 0) {
                clients_add(fd);
            }
        }

        if (g_reload_conf) {
            g_reload_conf = 0;
            haptics_reload_config();
            fprintf(stderr, "rpod-wheel: %lu packets dropped for a bad preamble so far\n",
                    g_bad_preamble);
        }
    }

    gpioSetGetSamplesFunc(NULL, 0);
    gpioTerminate();
    close(listen_fd);
    unlink(RPOD_WHEEL_SOCK_PATH);
    return 0;
}
