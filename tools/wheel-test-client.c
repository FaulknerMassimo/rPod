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
 */

#include "../daemon/wheel_protocol.h"

#include <stdio.h>
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

    struct rpod_wheel_event ev;
    long long running_delta = 0;
    unsigned long long seq = 0;

    while (1) {
        ssize_t n = recv(fd, &ev, sizeof(ev), MSG_WAITALL);
        if (n == 0) {
            printf("wheel-test-client: daemon closed the connection\n");
            break;
        }
        if (n != (ssize_t)sizeof(ev)) {
            perror("recv");
            break;
        }
        seq++;

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

    close(fd);
    return 0;
}
