/*
 * rpod-web: the web UI (docs/PLAN.md §8.6) -- music uploads, the library,
 * and settings the click wheel can't reach -- served from the device.
 *
 * Its own process, like the click wheel daemon, so nothing it does can
 * stall or crash the UI: it talks to MPD for the library and playback, to
 * rPod itself over its settings socket (src/ui/control.h) for the settings
 * rPod owns, and writes uploads straight into the music directory. A
 * thread per connection, capped; every response closes its connection.
 *
 * Configured from the environment (/etc/rpod/env on device, shared with
 * rPod, so the state-file overrides agree):
 *   RPOD_WEB_PORT          port to listen on (80; 8080 in `make web-sim`)
 *   RPOD_WEB_ROOT          the static files (/usr/local/share/rpod/web)
 *   RPOD_WEB_IPOD          the iPod's font and icons (RPOD_WEB_ROOT/ipod)
 *   RPOD_WEB_PASSWORD      ask for this password (HTTP Basic); unset: don't
 *   RPOD_WEB_HOSTS         more host names it answers to, comma-separated
 *   RPOD_MPD_SOCKET        MPD (/run/mpd/socket)
 *   RPOD_MUSIC_DIR         the library (default: MPD's music_directory)
 *   RPOD_VIDEO_DIR         the videos folder (/media/videos), as rPod's
 *   RPOD_CONVERT           rpod-convert (beside this program)
 *   RPOD_CONTROL_SOCK      rPod's settings (/run/rpod-ui/control.sock)
 *   RPOD_THEME_STATE       rPod's theme file (/var/lib/rpod/theme)
 *   RPOD_LISTENBRAINZ_QUEUE  listens waiting to be sent
 */

#include "api.h"
#include "http.h"
#include "videos.h"
#include "audio/mpd_client.h"

#include <curl/curl.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <time.h>
#include <unistd.h>

/* Connections served at once; more wait in the listen backlog. Uploads
 * hold one each (the page sends two at a time), the rest are quick. */
#define MAX_CONNECTIONS 8

/* A client this quiet mid-request is gone. */
#define IO_TIMEOUT_S 30

static rpod_web_config_t g_cfg;
static sem_t g_slots;

static const char *env_or(const char *name, const char *def)
{
    const char *v = getenv(name);
    return v != NULL && v[0] != '\0' ? v : def;
}

static long ms_since(const struct timespec *t0)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - t0->tv_sec) * 1000 + (now.tv_nsec - t0->tv_nsec) / 1000000;
}

static void *serve(void *arg)
{
    rpod_http_req_t *req = arg;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    if (rpod_http_read_head(req)) {
        rpod_web_handle(&g_cfg, req);
        /* Quiet about the page's own polling; everything else is logged. */
        bool routine = req->status == 200 && (strcmp(req->method, "GET") == 0 || strcmp(req->method, "HEAD") == 0);
        if (!routine && req->status != 304) {
            fprintf(stderr, "rpod-web: %s %s %s -> %d (%ld ms)\n", req->peer, req->method, req->path, req->status,
                    ms_since(&t0));
        }
        /* A client still sending a body we refused gets a moment to stop
         * and read the answer, rather than a reset that loses it. */
        if (req->body_left > 0) {
            shutdown(req->fd, SHUT_WR);
            rpod_http_drain(req, 2000);
        }
    }
    close(req->fd);
    free(req);
    sem_post(&g_slots);
    return NULL;
}

static int listen_on(int port)
{
    int fd = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int one = 1, zero = 0;
    if (fd >= 0) {
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
        struct sockaddr_in6 a6 = { .sin6_family = AF_INET6, .sin6_port = htons((uint16_t)port),
                                   .sin6_addr = IN6ADDR_ANY_INIT };
        if (bind(fd, (struct sockaddr *)&a6, sizeof(a6)) == 0 && listen(fd, 32) == 0) {
            return fd;
        }
        close(fd);
    }
    /* No IPv6 here: IPv4 only. */
    fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a4 = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port),
                              .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(fd, (struct sockaddr *)&a4, sizeof(a4)) != 0 || listen(fd, 32) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void peer_name(const struct sockaddr_storage *ss, char *out, size_t out_size)
{
    snprintf(out, out_size, "?");
    if (ss->ss_family == AF_INET) {
        inet_ntop(AF_INET, &((const struct sockaddr_in *)ss)->sin_addr, out, (socklen_t)out_size);
    } else if (ss->ss_family == AF_INET6) {
        const struct in6_addr *a = &((const struct sockaddr_in6 *)ss)->sin6_addr;
        if (IN6_IS_ADDR_V4MAPPED(a)) {
            inet_ntop(AF_INET, &a->s6_addr[12], out, (socklen_t)out_size);
        } else {
            inet_ntop(AF_INET6, a, out, (socklen_t)out_size);
        }
    }
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    umask(022); /* uploads stay readable by MPD's user */
    curl_global_init(CURL_GLOBAL_DEFAULT);

    int port = atoi(env_or("RPOD_WEB_PORT", "80"));
    g_cfg.web_root = env_or("RPOD_WEB_ROOT", "/usr/local/share/rpod/web");
    static char ipod_default[PATH_MAX];
    snprintf(ipod_default, sizeof(ipod_default), "%s/ipod", g_cfg.web_root);
    g_cfg.ipod_root = env_or("RPOD_WEB_IPOD", ipod_default);
    g_cfg.mpd_socket = env_or("RPOD_MPD_SOCKET", "/run/mpd/socket");
    g_cfg.control_sock = env_or("RPOD_CONTROL_SOCK", "/run/rpod-ui/control.sock");
    g_cfg.theme_state = env_or("RPOD_THEME_STATE", "/var/lib/rpod/theme");
    g_cfg.lb_queue = env_or("RPOD_LISTENBRAINZ_QUEUE", "/var/lib/rpod/listenbrainz_queue.jsonl");
    g_cfg.password = getenv("RPOD_WEB_PASSWORD");
    g_cfg.extra_hosts = getenv("RPOD_WEB_HOSTS");
    if (gethostname(g_cfg.hostname, sizeof(g_cfg.hostname) - 1) != 0) {
        snprintf(g_cfg.hostname, sizeof(g_cfg.hostname), "rpod");
    }

    /* The library is wherever MPD keeps it. MPD may still be starting:
     * give it a few seconds before settling for the default. */
    const char *music = getenv("RPOD_MUSIC_DIR");
    if (music != NULL && music[0] != '\0') {
        snprintf(g_cfg.music_dir, sizeof(g_cfg.music_dir), "%s", music);
    } else {
        bool found = false;
        for (int tries = 0; tries < 10 && !found; tries++) {
            rpod_mpd_t *mpd = rpod_mpd_connect(g_cfg.mpd_socket);
            found = mpd != NULL && rpod_mpd_is_connected(mpd) &&
                    rpod_mpd_get_music_directory(mpd, g_cfg.music_dir, sizeof(g_cfg.music_dir));
            rpod_mpd_disconnect(mpd);
            if (!found) {
                sleep(1);
            }
        }
        if (!found) {
            snprintf(g_cfg.music_dir, sizeof(g_cfg.music_dir), "/media/music");
            fprintf(stderr, "rpod-web: MPD didn't say where the library is; using %s\n", g_cfg.music_dir);
        }
    }

    /* The videos folder, and what makes uploads into videos rPod plays:
     * rpod-convert, installed beside this. */
    snprintf(g_cfg.video_dir, sizeof(g_cfg.video_dir), "%s", env_or("RPOD_VIDEO_DIR", "/media/videos"));
    char convert[PATH_MAX];
    const char *convert_env = getenv("RPOD_CONVERT");
    ssize_t self = readlink("/proc/self/exe", convert, sizeof(convert) - 32);
    if (convert_env != NULL && convert_env[0] != '\0') {
        snprintf(convert, sizeof(convert), "%s", convert_env);
    } else if (self > 0) {
        convert[self] = '\0';
        char *slash = strrchr(convert, '/');
        snprintf(slash != NULL ? slash + 1 : convert, 32, "rpod-convert");
    } else {
        snprintf(convert, sizeof(convert), "/usr/local/bin/rpod-convert");
    }
    char abs_videos[PATH_MAX];
    rpod_videos_start(g_cfg.video_dir, convert);
    if (realpath(g_cfg.video_dir, abs_videos) != NULL) {
        snprintf(g_cfg.video_dir, sizeof(g_cfg.video_dir), "%s", abs_videos); /* as MPD has it */
    }

    int lfd = listen_on(port);
    if (lfd < 0) {
        fprintf(stderr, "rpod-web: can't listen on port %d: %s\n", port, strerror(errno));
        return 1;
    }
    fprintf(stderr, "rpod-web: http://%s.local%s%s/ -- library %s, videos %s%s\n", g_cfg.hostname,
            port == 80 ? "" : ":", port == 80 ? "" : env_or("RPOD_WEB_PORT", ""), g_cfg.music_dir, g_cfg.video_dir,
            g_cfg.password != NULL && g_cfg.password[0] != '\0' ? ", password set" : "");
    if (access(g_cfg.music_dir, W_OK) != 0) {
        fprintf(stderr, "rpod-web: %s isn't writable by this user: uploads will fail\n", g_cfg.music_dir);
    }

    sem_init(&g_slots, 0, MAX_CONNECTIONS);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 512 * 1024);

    for (;;) {
        while (sem_wait(&g_slots) != 0 && errno == EINTR) {
        }
        struct sockaddr_storage ss;
        socklen_t sl = sizeof(ss);
        int fd = accept4(lfd, (struct sockaddr *)&ss, &sl, SOCK_CLOEXEC);
        if (fd < 0) {
            sem_post(&g_slots);
            if (errno != EINTR && errno != ECONNABORTED) {
                fprintf(stderr, "rpod-web: accept: %s\n", strerror(errno));
                sleep(1);
            }
            continue;
        }
        struct timeval tv = { .tv_sec = IO_TIMEOUT_S };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        rpod_http_req_t *req = calloc(1, sizeof(*req));
        pthread_t t;
        if (req == NULL) {
            close(fd);
            sem_post(&g_slots);
            continue;
        }
        req->fd = fd;
        peer_name(&ss, req->peer, sizeof(req->peer));
        if (pthread_create(&t, &attr, serve, req) != 0) {
            close(fd);
            free(req);
            sem_post(&g_slots);
        }
    }
}
