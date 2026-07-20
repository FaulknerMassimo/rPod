/*
 * rpod-wheel — click wheel decoder daemon.
 *
 * Runs as a privileged root process (pigpio needs /dev/mem). Decodes the
 * 4th-gen click wheel's synchronous serial stream per docs/PLAN.md §4 and
 * publishes normalised events over a Unix domain socket for the
 * (unprivileged) UI process to consume.
 *
 * Packet framing follows docs/PLAN.md §4.2 exactly, and is the same
 * algorithm as tools/wheel-sniff.c: sample DATA on CLOCK's rising edge; 32
 * consecutive 1 bits means idle; recording starts on the first 0 bit after
 * idle; 32 bits later, parse and reset. Both CLOCK and DATA are sampled
 * from a single CLOCK-edge callback rather than two independent alert
 * callbacks (as the reference implementation does) so there's no race
 * between when DATA actually transitions and when it gets sampled.
 */

#include "wheel_bits.h"
#include "wheel_protocol.h"

#include <errno.h>
#include <pigpio.h>
#include <pthread.h>
#include <signal.h>
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

/* Called from the pigpio alert-callback thread. Non-blocking sends — a slow
 * or dead client gets dropped rather than stalling the decoder. */
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
    if (g_haptic_wave_id != -1 && gpioWaveTxBusy() == 0) {
        gpioWaveTxSend(g_haptic_wave_id, PI_WAVE_MODE_ONE_SHOT);
    }
}

/* --- hold switch -------------------------------------------------------- */

static int hold_engaged(void)
{
    /* Active-low: assumes the switch pulls GPIO 16 to GND when hold is on,
     * with the internal pull-up holding it high otherwise. Verify against
     * real hardware and flip this if the wiring says otherwise. */
    return gpioRead(RPOD_WHEEL_HOLD_PIN) == 0;
}

/* --- event emission ------------------------------------------------------ */

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static void emit_button(uint8_t code, int pressed, uint32_t position, uint64_t ts)
{
    struct rpod_wheel_event ev = {
        .type = RPOD_WHEEL_EVENT_BUTTON,
        .code = code,
        .value = (int8_t)(pressed ? 1 : 0),
        ._pad = 0,
        .position = position,
        .timestamp_us = ts,
    };
    clients_broadcast(&ev);
}

static void emit_wheel(int8_t delta, uint32_t position, uint64_t ts)
{
    struct rpod_wheel_event ev = {
        .type = RPOD_WHEEL_EVENT_WHEEL,
        .code = 0,
        .value = delta,
        ._pad = 0,
        .position = position,
        .timestamp_us = ts,
    };
    clients_broadcast(&ev);
}

static void emit_touch(int touched, uint32_t position, uint64_t ts)
{
    struct rpod_wheel_event ev = {
        .type = RPOD_WHEEL_EVENT_TOUCH,
        .code = 0,
        .value = (int8_t)(touched ? 1 : 0),
        ._pad = 0,
        .position = position,
        .timestamp_us = ts,
    };
    clients_broadcast(&ev);
}

/* Wrap-corrected delta on the 0-255 position ring — docs/PLAN.md §4.2 (a
 * jump from 250 to 5 is +11, not -245). */
static int8_t wheel_delta(uint8_t curr, uint8_t prev)
{
    int d = (int)curr - (int)prev;
    if (d > 128) {
        d -= 256;
    } else if (d < -128) {
        d += 256;
    }
    return (int8_t)d;
}

/* --- packet -> event diffing --------------------------------------------- */

static int g_have_last_packet = 0;
static uint32_t g_last_packet = 0;

static void handle_packet(uint32_t packet, uint64_t ts)
{
    if (!g_have_last_packet) {
        /* Nothing to diff the first packet against — establish a baseline
         * rather than emitting spurious press/touch events for whatever
         * state the wheel happened to be in at daemon start. */
        g_last_packet = packet;
        g_have_last_packet = 1;
        return;
    }

    if (hold_engaged()) {
        g_last_packet = packet;
        return; /* keep decoding, suppress emission, per docs/PLAN.md §4.5 */
    }

    static const struct {
        uint8_t code;
        int bit;
    } buttons[] = {
        { RPOD_WHEEL_BTN_CENTER, RPOD_WHEEL_BIT_CENTER },
        { RPOD_WHEEL_BTN_LEFT,   RPOD_WHEEL_BIT_LEFT   },
        { RPOD_WHEEL_BTN_RIGHT,  RPOD_WHEEL_BIT_RIGHT  },
        { RPOD_WHEEL_BTN_UP,     RPOD_WHEEL_BIT_UP     },
        { RPOD_WHEEL_BTN_DOWN,   RPOD_WHEEL_BIT_DOWN   },
    };

    uint32_t position = (packet >> RPOD_WHEEL_POS_SHIFT) & RPOD_WHEEL_POS_MASK;

    for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
        int was = (g_last_packet >> buttons[i].bit) & 1;
        int is  = (packet >> buttons[i].bit) & 1;
        if (was != is) {
            emit_button(buttons[i].code, is, position, ts);
        }
    }

    int was_touched = (g_last_packet >> RPOD_WHEEL_BIT_TOUCH) & 1;
    int is_touched   = (packet >> RPOD_WHEEL_BIT_TOUCH) & 1;
    if (was_touched != is_touched) {
        emit_touch(is_touched, position, ts);
    }

    uint8_t last_position = (uint8_t)((g_last_packet >> RPOD_WHEEL_POS_SHIFT) & RPOD_WHEEL_POS_MASK);
    if (is_touched && (uint8_t)position != last_position) {
        int8_t delta = wheel_delta((uint8_t)position, last_position);
        emit_wheel(delta, position, ts);

        static int step = 0;
        if (++step >= g_haptic_divisor) {
            step = 0;
            haptics_fire();
        }
    }

    g_last_packet = packet;
}

/* --- packet framing (docs/PLAN.md §4.2, mirrors tools/wheel-sniff.c) ---- */

static uint32_t g_packet = 0;
static int g_bit_index = 0;
static int g_recording = 0;
static int g_ones_run = 0;

static void on_bit(int bit, uint64_t ts)
{
    if (bit) {
        g_ones_run++;
        if (g_ones_run >= 32) {
            g_recording = 0;
            g_bit_index = 0;
            g_packet = 0;
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
        g_recording = 0;
        g_bit_index = 0;
        g_packet = 0;
    }
}

static void clock_edge_cb(int gpio, int level, uint32_t tick)
{
    (void)gpio;
    (void)tick;
    if (level != 1) {
        return; /* sample DATA on the rising edge of CLOCK only */
    }
    int bit = gpioRead(RPOD_WHEEL_DATA_PIN);
    on_bit(bit, now_us());
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
    } else {
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

    /* Default 5us sample rate drops edges on this protocol — docs/PLAN.md
     * §4.5. Must be set before gpioInitialise(). */
    if (gpioCfgClock(1, 1, 0) != 0) {
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

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);

    gpioSetAlertFunc(RPOD_WHEEL_CLOCK_PIN, clock_edge_cb);

    fprintf(stderr, "rpod-wheel: listening on %s, decoding CLOCK=GPIO%d DATA=GPIO%d\n",
            RPOD_WHEEL_SOCK_PATH, RPOD_WHEEL_CLOCK_PIN, RPOD_WHEEL_DATA_PIN);

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
        }
    }

    gpioSetAlertFunc(RPOD_WHEEL_CLOCK_PIN, NULL);
    gpioTerminate();
    close(listen_fd);
    unlink(RPOD_WHEEL_SOCK_PATH);
    return 0;
}
