/*
 * The web daemon's HTTP/1.1, just what its API and static files need: one
 * request per connection (every response closes it), bodies by
 * Content-Length only, and request bodies read a piece at a time so an
 * upload streams to disk instead of into the Pi's 512 MB.
 *
 * The parsing helpers (URL decoding, query parameters, Host checks) are
 * pure logic (tests/test_web.c); the rest does socket I/O.
 */

#ifndef RPOD_WEB_HTTP_H
#define RPOD_WEB_HTTP_H

#include "json.h"

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#define RPOD_HTTP_HEAD_MAX 16384

typedef struct {
    int fd;
    char peer[64];

    char method[8];
    char path[2048];  /* the target's path, percent-decoded */
    char query[4096]; /* the raw query string, after '?' */

    /* The headers anything here looks at. */
    char host[256];
    char origin[256];
    char content_type[128];
    char authorization[512];
    char if_none_match[64];
    long long content_length; /* -1 without one */
    bool chunked;
    bool expect_continue;
    bool x_rpod; /* "X-Rpod: 1": sent by the web UI's own scripts (api.c) */

    /* Bytes read past the head: the start of the body. */
    char buf[RPOD_HTTP_HEAD_MAX];
    size_t buf_pos, buf_len;
    long long body_left; /* body bytes not read yet */

    int status; /* what was answered, for the log; 0 before */
} rpod_http_req_t;

/* Reads and parses the request head from req->fd (already set). False if
 * the connection should just be closed -- having answered a malformed
 * request with a 4xx where there's anyone left to answer. */
bool rpod_http_read_head(rpod_http_req_t *req);

/* Up to n bytes of the body; 0 at its end, -1 on a read error or timeout. */
ssize_t rpod_http_read_body(rpod_http_req_t *req, void *buf, size_t n);

/* The whole body, if it's no longer than max, NUL-terminated and malloc'd
 * (free() it); NULL if it's longer or doesn't arrive. */
char *rpod_http_read_small_body(rpod_http_req_t *req, size_t max);

/* Reads and throws away what's left of the body, for up to `ms` -- so a
 * client still sending one gets to read the answer. */
void rpod_http_drain(rpod_http_req_t *req, int ms);

/* Writes a whole response with Connection: close. extra_headers is "" or
 * complete "Name: value\r\n" lines. HEAD requests get the head only. */
void rpod_http_send(rpod_http_req_t *req, int status, const char *content_type, const void *body,
                    size_t len, const char *extra_headers);
void rpod_http_send_json(rpod_http_req_t *req, int status, const rpod_json_t *j);
/* {"error": message} */
void rpod_http_send_error(rpod_http_req_t *req, int status, const char *message);

/* Just the head, then the caller writes len body bytes with
 * rpod_http_write(). */
bool rpod_http_send_head(rpod_http_req_t *req, int status, const char *content_type, long long len,
                         const char *extra_headers);
bool rpod_http_write(rpod_http_req_t *req, const void *buf, size_t len);

/* Serves a file under root_dir by its URL path (already validated as a
 * plain relative path), with an ETag. 404 when it isn't there. */
void rpod_http_send_file(rpod_http_req_t *req, const char *root_dir, const char *rel);

/* --- Parsing (pure) ------------------------------------------------------- */

/* Percent-decodes in[0..len) into out; `plus_is_space` for form data.
 * False for a malformed escape, an encoded NUL, or no room. */
bool rpod_http_url_decode(const char *in, size_t len, char *out, size_t out_size, bool plus_is_space);

/* The first `key` parameter of a query string or form body, decoded. False
 * if there's none (or it doesn't decode or fit). */
bool rpod_http_param(const char *qs, const char *key, char *out, size_t out_size);

/* Walks a query string or form body: calls fn(key, value, ctx) for each
 * decodable parameter, stopping early if fn returns false. */
void rpod_http_each_param(const char *qs, bool (*fn)(const char *key, const char *value, void *ctx),
                          void *ctx);

/* Whether a Host header names this machine (the defence against DNS
 * rebinding, api.c): an IP literal, localhost, `hostname` itself or under
 * a LAN suffix (.local, .lan, .home, .internal, .home.arpa), or one of the
 * comma-separated `extra` names. Any port is ignored. */
bool rpod_http_host_ok(const char *host, const char *hostname, const char *extra);

/* Decodes standard base64 into out (NUL-terminated); false if it isn't. */
bool rpod_http_base64_decode(const char *in, char *out, size_t out_size);

#endif /* RPOD_WEB_HTTP_H */
