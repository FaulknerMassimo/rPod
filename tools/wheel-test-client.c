/*
 * wheel-test-client — connects to the rpod-wheel daemon's Unix socket and
 * prints normalised events, human-readable. For manually checking Phase 2's
 * acceptance criteria (docs/PLAN.md §9): every button reports press and
 * release exactly once, a full slow rotation reports monotonic
 * wraparound-correct deltas, and no packets are dropped over 60s of
 * continuous scrolling (watch the running total and the [N] sequence
 * counter for gaps).
 *
 * Usage: wheel-test-client [socket_path]
 *        wheel-test-client --summary SECONDS [socket_path]
 *
 * --summary prints nothing live, then after SECONDS a tally: presses and
 * releases per button, touches, wheel events, summed rotation, and how many
 * wheel deltas skipped a position (|delta| > 1 -- expected during fast
 * scrolling, which a packet every ~15 ms can't keep up with, but a sign of
 * dropped packets during a slow turn).
 */

#include "../daemon/wheel_protocol.h"

#include <errno.h>
#include <sys/time.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static const char *button_name(uint8_t code)
{
    switch (code) {
    case RPOD_WHEEL_BTN_CENTER: return "center";
    case RPOD_WHEEL_BTN_LEFT:   return "left";
    case RPOD_WHEEL_BTN_RIGHT:  return "right";
    case RPOD_WHEEL_BTN_UP:     return "up";
    case RPOD_WHEEL_BTN_DOWN:   return "down";
    default:                    return "?";
    }
}

int main(int argc, char **argv)
{
    int summary_secs = 0;
    if (argc > 2 && strcmp(argv[1], "--summary") == 0) {
        summary_secs = atoi(argv[2]);
        argv += 2;
        argc -= 2;
    }
    const char *path = argc > 1 ? argv[1] : RPOD_WHEEL_SOCK_PATH;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("connect");
        return 1;
    }

    printf("wheel-test-client: connected to %s\n", path);
    if (summary_secs > 0) {
        printf("wheel-test-client: collecting for %d s...\n", summary_secs);
        fflush(stdout);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    struct rpod_wheel_event ev;
    long long running_delta = 0;
    unsigned long long seq = 0;
    unsigned long presses[5] = { 0 }, releases[5] = { 0 };
    unsigned long touches = 0, wheel_events = 0, skips = 0;
    time_t deadline = time(NULL) + summary_secs;

    while (summary_secs == 0 || time(NULL) < deadline) {
        ssize_t n = recv(fd, &ev, sizeof(ev), MSG_WAITALL);
        if (n < 0 && summary_secs > 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue; /* receive timeout: just re-check the deadline */
        }
        if (n == 0) {
            printf("wheel-test-client: daemon closed the connection\n");
            break;
        }
        if (n != (ssize_t)sizeof(ev)) {
            perror("recv");
            break;
        }
        seq++;

        if (summary_secs > 0) {
            if (ev.type == RPOD_WHEEL_EVENT_BUTTON && ev.code < 5) {
                (ev.value ? presses : releases)[ev.code]++;
            } else if (ev.type == RPOD_WHEEL_EVENT_TOUCH && ev.value) {
                touches++;
            } else if (ev.type == RPOD_WHEEL_EVENT_WHEEL) {
                wheel_events++;
                running_delta += ev.value;
                if (ev.value > 1 || ev.value < -1) {
                    skips++;
                }
            }
            continue;
        }

        switch (ev.type) {
        case RPOD_WHEEL_EVENT_BUTTON:
            printf("[%llu] button %-6s %-7s position=%u\n",
                   seq, button_name(ev.code),
                   ev.value ? "press" : "release", ev.position);
            break;
        case RPOD_WHEEL_EVENT_WHEEL:
            running_delta += ev.value;
            printf("[%llu] wheel  delta=%+4d       position=%-3u running=%lld\n",
                   seq, ev.value, ev.position, running_delta);
            break;
        case RPOD_WHEEL_EVENT_TOUCH:
            printf("[%llu] touch  %-7s position=%u\n",
                   seq, ev.value ? "on" : "off", ev.position);
            break;
        default:
            printf("[%llu] unknown event type=%u\n", seq, ev.type);
            break;
        }
        fflush(stdout);
    }

    if (summary_secs > 0) {
        printf("buttons (press/release):");
        for (uint8_t c = 0; c < 5; c++) {
            printf("  %s %lu/%lu", button_name(c), presses[c], releases[c]);
        }
        printf("\ntouches: %lu\nwheel: %lu events, summed delta %+lld (%.2f turns), %lu skipped a position\n",
               touches, wheel_events, running_delta, running_delta / 96.0, skips);
    }

    close(fd);
    return 0;
}
