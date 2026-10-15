#include "control_client.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

/* rPod answers within a poll period (50 ms); allow for a busy LVGL thread. */
#define TIMEOUT_S 3

/* Larger than any reply (ui/control.c's REPLY_MAX). */
#define REPLY_MAX 8192

char *rpod_ctl_request(const char *sock_path, const char *line)
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    if (sock_path == NULL || strlen(sock_path) >= sizeof(addr.sun_path) || strchr(line, '\n') != NULL) {
        return NULL;
    }
    memcpy(addr.sun_path, sock_path, strlen(sock_path) + 1);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return NULL;
    }
    struct timeval tv = { .tv_sec = TIMEOUT_S };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return NULL;
    }

    size_t len = strlen(line);
    char *req = malloc(len + 2);
    if (req == NULL) {
        close(fd);
        return NULL;
    }
    memcpy(req, line, len);
    req[len] = '\n';
    bool sent = send(fd, req, len + 1, MSG_NOSIGNAL) == (ssize_t)(len + 1);
    free(req);
    if (!sent) {
        close(fd);
        return NULL;
    }

    char *reply = malloc(REPLY_MAX);
    if (reply == NULL) {
        close(fd);
        return NULL;
    }
    size_t got = 0;
    for (;;) {
        ssize_t n = recv(fd, reply + got, REPLY_MAX - 1 - got, 0);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        got += (size_t)n;
        if (got == REPLY_MAX - 1) {
            break;
        }
    }
    close(fd);
    reply[got] = '\0';
    if (got == 0) {
        free(reply);
        return NULL;
    }
    return reply;
}

bool rpod_ctl_ok(const char *reply)
{
    return reply != NULL && strncmp(reply, "ok\n", 3) == 0;
}

bool rpod_ctl_value(const char *reply, const char *key, char *out, size_t out_size)
{
    size_t klen = strlen(key);
    for (const char *line = reply; line != NULL && *line != '\0';) {
        const char *end = strchr(line, '\n');
        size_t len = end != NULL ? (size_t)(end - line) : strlen(line);
        if (len > klen && strncmp(line, key, klen) == 0 && line[klen] == '=') {
            size_t vlen = len - klen - 1;
            if (vlen >= out_size) {
                vlen = out_size - 1;
            }
            memcpy(out, line + klen + 1, vlen);
            out[vlen] = '\0';
            return true;
        }
        line = end != NULL ? end + 1 : NULL;
    }
    return false;
}
