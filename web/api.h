/*
 * The web UI's requests (docs/PLAN.md §8.6): its static files, and the JSON
 * API its scripts call -- the player, the music library, rPod's settings
 * and About.
 *
 * Who may ask: the Host must name this machine (rpod_http_host_ok()), so a
 * web page elsewhere can't reach the API by rebinding its own DNS name to
 * rPod's address; anything that changes something must carry "X-Rpod: 1",
 * which a page on another origin can't add without a CORS preflight this
 * server never grants; and with RPOD_WEB_PASSWORD set, every request needs
 * it (HTTP Basic, any user name).
 */

#ifndef RPOD_WEB_API_H
#define RPOD_WEB_API_H

#include "http.h"

#include <limits.h>

typedef struct {
    const char *web_root;     /* the static files (web/static/) */
    const char *ipod_root;    /* the iPod's font and icons for Classic, if generated */
    const char *mpd_socket;
    char music_dir[PATH_MAX]; /* MPD's music_directory */
    char video_dir[PATH_MAX]; /* the videos folder (src/video/video_library.h, web/videos.h) */
    const char *control_sock; /* rPod's settings (src/ui/control.h) */
    const char *theme_state;  /* read directly for the look: no round trip to rPod */
    const char *lb_queue;     /* ListenBrainz listens waiting to be sent */
    const char *password;     /* NULL or "": no password */
    const char *extra_hosts;  /* more names this machine answers to */
    char hostname[256];
} rpod_web_config_t;

void rpod_web_handle(const rpod_web_config_t *cfg, rpod_http_req_t *req);

#endif /* RPOD_WEB_API_H */
