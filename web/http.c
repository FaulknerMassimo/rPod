#include "http.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *status_text(int status)
{
    switch (status) {
    case 100: return "Continue";
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 507: return "Insufficient Storage";
    default: return "Unknown";
    }
}

/* Every response: no sniffing, no framing, no referrer, and scripts and
 * styles only from here. */
#define SECURITY_HEADERS                                                       \
    "X-Content-Type-Options: nosniff\r\n"                                      \
    "X-Frame-Options: DENY\r\n"                                                \
    "Referrer-Policy: no-referrer\r\n"                                         \
    "Content-Security-Policy: default-src 'self'; img-src 'self' blob: data:; " \
    "frame-ancestors 'none'; base-uri 'none'; form-action 'self'\r\n"

static bool write_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        p += n;
        len -= (size_t)n;
    }
    return true;
}

/* --- Parsing -------------------------------------------------------------- */

static int hexval(int c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c = tolower(c);
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

bool rpod_http_url_decode(const char *in, size_t len, char *out, size_t out_size, bool plus_is_space)
{
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        char c = in[i];
        if (c == '%') {
            if (i + 2 >= len) {
                return false;
            }
            int hi = hexval((unsigned char)in[i + 1]);
            int lo = hexval((unsigned char)in[i + 2]);
            if (hi < 0 || lo < 0) {
                return false;
            }
            c = (char)(hi * 16 + lo);
            if (c == '\0') {
                return false;
            }
            i += 2;
        } else if (c == '+' && plus_is_space) {
            c = ' ';
        }
        if (o + 1 >= out_size) {
            return false;
        }
        out[o++] = c;
    }
    if (out_size == 0) {
        return false;
    }
    out[o] = '\0';
    return true;
}

void rpod_http_each_param(const char *qs, bool (*fn)(const char *key, const char *value, void *ctx),
                          void *ctx)
{
    const char *p = qs;
    while (p != NULL && *p != '\0') {
        const char *amp = strchr(p, '&');
        size_t plen = amp != NULL ? (size_t)(amp - p) : strlen(p);
        const char *eq = memchr(p, '=', plen);
        size_t klen = eq != NULL ? (size_t)(eq - p) : plen;
        char key[128];
        /* Values can be long (a path), but never longer than the query. */
        size_t vlen = eq != NULL ? plen - klen - 1 : 0;
        char *value = malloc(vlen + 1);
        if (value != NULL && rpod_http_url_decode(p, klen, key, sizeof(key), true) &&
            rpod_http_url_decode(eq != NULL ? eq + 1 : "", vlen, value, vlen + 1, true)) {
            bool more = fn(key, value, ctx);
            free(value);
            if (!more) {
                return;
            }
        } else {
            free(value);
        }
        p = amp != NULL ? amp + 1 : NULL;
    }
}

typedef struct {
    const char *key;
    char *out;
    size_t out_size;
    bool found;
} param_find_t;

static bool param_find_cb(const char *key, const char *value, void *ctx)
{
    param_find_t *f = ctx;
    if (strcmp(key, f->key) != 0) {
        return true;
    }
    size_t n = strlen(value);
    if (n >= f->out_size) {
        return false; /* doesn't fit: as good as absent */
    }
    memcpy(f->out, value, n + 1);
    f->found = true;
    return false;
}

bool rpod_http_param(const char *qs, const char *key, char *out, size_t out_size)
{
    param_find_t f = { key, out, out_size, false };
    rpod_http_each_param(qs, param_find_cb, &f);
    return f.found;
}

static bool is_ipv4(const char *s, size_t n)
{
    if (n == 0) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (!isdigit((unsigned char)s[i]) && s[i] != '.') {
            return false;
        }
    }
    return true;
}

static bool name_eq(const char *a, size_t alen, const char *b)
{
    return strlen(b) == alen && strncasecmp(a, b, alen) == 0;
}

bool rpod_http_host_ok(const char *host, const char *hostname, const char *extra)
{
    if (host == NULL || host[0] == '\0') {
        return false;
    }
    if (host[0] == '[') {
        /* An IPv6 literal: nothing a DNS name can turn into. */
        const char *end = strchr(host, ']');
        return end != NULL && (end[1] == '\0' || end[1] == ':');
    }
    size_t n = strcspn(host, ":");
    if (n > 0 && host[n - 1] == '.') {
        n--; /* a fully qualified "rpod.local." */
    }
    if (is_ipv4(host, n) || name_eq(host, n, "localhost")) {
        return true;
    }
    if (hostname != NULL && hostname[0] != '\0') {
        static const char *suffixes[] = { "", ".local", ".lan", ".home", ".internal", ".home.arpa" };
        size_t hn = strlen(hostname);
        for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
            size_t sn = strlen(suffixes[i]);
            if (n == hn + sn && strncasecmp(host, hostname, hn) == 0 &&
                strncasecmp(host + hn, suffixes[i], sn) == 0) {
                return true;
            }
        }
    }
    for (const char *p = extra; p != NULL && *p != '\0';) {
        size_t len = strcspn(p, ",");
        while (len > 0 && *p == ' ') {
            p++;
            len--;
        }
        size_t trimmed = len;
        while (trimmed > 0 && p[trimmed - 1] == ' ') {
            trimmed--;
        }
        if (trimmed > 0 && trimmed == n && strncasecmp(p, host, n) == 0) {
            return true;
        }
        p += len;
        if (*p == ',') {
            p++;
        }
    }
    return false;
}

bool rpod_http_base64_decode(const char *in, char *out, size_t out_size)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned acc = 0;
    int bits = 0;
    size_t o = 0;
    size_t i = 0;
    for (; in[i] != '\0' && in[i] != '='; i++) {
        const char *pos = strchr(alphabet, in[i]);
        if (pos == NULL) {
            return false;
        }
        acc = (acc << 6) | (unsigned)(pos - alphabet);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o + 1 >= out_size) {
                return false;
            }
            out[o++] = (char)((acc >> bits) & 0xff);
        }
    }
    for (; in[i] == '='; i++) {
    }
    if (in[i] != '\0' || out_size == 0) {
        return false;
    }
    out[o] = '\0';
    return true;
}

/* --- Reading ----------------------------------------------------------------- */

static void copy_header(char *dst, size_t dst_size, const char *v)
{
    snprintf(dst, dst_size, "%s", v);
}

static bool parse_head(rpod_http_req_t *req, char *head)
{
    char *line_end = strstr(head, "\r\n");
    if (line_end == NULL) {
        return false;
    }
    *line_end = '\0';

    /* METHOD SP target SP HTTP/1.x */
    char *sp1 = strchr(head, ' ');
    char *sp2 = sp1 != NULL ? strchr(sp1 + 1, ' ') : NULL;
    if (sp1 == NULL || sp2 == NULL || (size_t)(sp1 - head) >= sizeof(req->method) ||
        strncmp(sp2 + 1, "HTTP/1.", 7) != 0) {
        return false;
    }
    memcpy(req->method, head, (size_t)(sp1 - head));
    req->method[sp1 - head] = '\0';
    char *target = sp1 + 1;
    *sp2 = '\0';
    if (target[0] != '/') {
        return false;
    }
    char *q = strchr(target, '?');
    size_t plen = q != NULL ? (size_t)(q - target) : strlen(target);
    if (q != NULL) {
        if (strlen(q + 1) >= sizeof(req->query)) {
            return false;
        }
        strcpy(req->query, q + 1);
    }
    if (!rpod_http_url_decode(target, plen, req->path, sizeof(req->path), false)) {
        return false;
    }

    req->content_length = -1;
    for (char *line = line_end + 2; *line != '\0';) {
        char *end = strstr(line, "\r\n");
        if (end == NULL) {
            break;
        }
        *end = '\0';
        char *colon = strchr(line, ':');
        if (colon != NULL) {
            *colon = '\0';
            char *v = colon + 1;
            while (*v == ' ' || *v == '\t') {
                v++;
            }
            size_t vl = strlen(v);
            while (vl > 0 && (v[vl - 1] == ' ' || v[vl - 1] == '\t')) {
                v[--vl] = '\0';
            }
            if (strcasecmp(line, "Host") == 0) {
                copy_header(req->host, sizeof(req->host), v);
            } else if (strcasecmp(line, "Origin") == 0) {
                copy_header(req->origin, sizeof(req->origin), v);
            } else if (strcasecmp(line, "Content-Type") == 0) {
                copy_header(req->content_type, sizeof(req->content_type), v);
            } else if (strcasecmp(line, "Authorization") == 0) {
                copy_header(req->authorization, sizeof(req->authorization), v);
            } else if (strcasecmp(line, "If-None-Match") == 0) {
                copy_header(req->if_none_match, sizeof(req->if_none_match), v);
            } else if (strcasecmp(line, "Content-Length") == 0) {
                char *e;
                errno = 0;
                long long cl = strtoll(v, &e, 10);
                if (e == v || *e != '\0' || cl < 0 || errno != 0 ||
                    (req->content_length >= 0 && req->content_length != cl)) {
                    return false;
                }
                req->content_length = cl;
            } else if (strcasecmp(line, "Transfer-Encoding") == 0) {
                req->chunked = true; /* anything but identity: refused below */
            } else if (strcasecmp(line, "Expect") == 0) {
                req->expect_continue = strcasecmp(v, "100-continue") == 0;
            } else if (strcasecmp(line, "X-Rpod") == 0) {
                req->x_rpod = strcmp(v, "1") == 0;
            }
        }
        line = end + 2;
    }
    return true;
}

bool rpod_http_read_head(rpod_http_req_t *req)
{
    size_t len = 0;
    char *head_end = NULL;
    while (head_end == NULL) {
        if (len == sizeof(req->buf) - 1) {
            rpod_http_send_error(req, 431, "request head too large");
            return false;
        }
        ssize_t n = recv(req->fd, req->buf + len, sizeof(req->buf) - 1 - len, 0);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false; /* gone, or timed out before sending anything useful */
        }
        len += (size_t)n;
        req->buf[len] = '\0';
        head_end = strstr(req->buf, "\r\n\r\n");
    }

    size_t head_len = (size_t)(head_end - req->buf) + 4;
    char *head = malloc(head_len + 1);
    if (head == NULL) {
        return false;
    }
    memcpy(head, req->buf, head_len);
    head[head_len] = '\0';
    bool ok = parse_head(req, head);
    free(head);

    /* What came in past the head is the body's start. */
    memmove(req->buf, req->buf + head_len, len - head_len);
    req->buf_pos = 0;
    req->buf_len = len - head_len;

    if (!ok) {
        rpod_http_send_error(req, 400, "malformed request");
        return false;
    }
    if (req->chunked) {
        rpod_http_send_error(req, 411, "send a Content-Length");
        return false;
    }
    req->body_left = req->content_length > 0 ? req->content_length : 0;
    if ((long long)req->buf_len > req->body_left) {
        req->buf_len = (size_t)req->body_left; /* pipelined extra: ignored */
    }
    if (req->expect_continue && req->body_left > (long long)req->buf_len) {
        static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
        write_all(req->fd, cont, sizeof(cont) - 1);
    }
    return true;
}

ssize_t rpod_http_read_body(rpod_http_req_t *req, void *buf, size_t n)
{
    if (req->body_left <= 0 || n == 0) {
        return 0;
    }
    if ((long long)n > req->body_left) {
        n = (size_t)req->body_left;
    }
    if (req->buf_pos < req->buf_len) {
        size_t avail = req->buf_len - req->buf_pos;
        if (n > avail) {
            n = avail;
        }
        memcpy(buf, req->buf + req->buf_pos, n);
        req->buf_pos += n;
        req->body_left -= (long long)n;
        return (ssize_t)n;
    }
    for (;;) {
        ssize_t r = recv(req->fd, buf, n, 0);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            return -1; /* the body ended early: an aborted upload, or a timeout */
        }
        req->body_left -= r;
        return r;
    }
}

char *rpod_http_read_small_body(rpod_http_req_t *req, size_t max)
{
    if (req->body_left > (long long)max) {
        return NULL;
    }
    size_t want = (size_t)req->body_left;
    char *body = malloc(want + 1);
    if (body == NULL) {
        return NULL;
    }
    size_t got = 0;
    while (got < want) {
        ssize_t n = rpod_http_read_body(req, body + got, want - got);
        if (n <= 0) {
            free(body);
            return NULL;
        }
        got += (size_t)n;
    }
    body[got] = '\0';
    return body;
}

void rpod_http_drain(rpod_http_req_t *req, int ms)
{
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    char sink[16384];
    req->buf_pos = req->buf_len;
    while (req->body_left > 0) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed >= ms) {
            return;
        }
        struct pollfd pfd = { .fd = req->fd, .events = POLLIN };
        if (poll(&pfd, 1, (int)(ms - elapsed)) <= 0) {
            return;
        }
        size_t want = req->body_left < (long long)sizeof(sink) ? (size_t)req->body_left : sizeof(sink);
        ssize_t n = recv(req->fd, sink, want, 0);
        if (n <= 0) {
            return;
        }
        req->body_left -= n;
    }
}

/* --- Writing ------------------------------------------------------------------- */

bool rpod_http_send_head(rpod_http_req_t *req, int status, const char *content_type, long long len,
                         const char *extra_headers)
{
    char head[2048];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %lld\r\n"
                     "Connection: close\r\n"
                     SECURITY_HEADERS
                     "%s\r\n",
                     status, status_text(status), content_type, len, extra_headers != NULL ? extra_headers : "");
    req->status = status;
    if (n < 0 || (size_t)n >= sizeof(head)) {
        return false;
    }
    return write_all(req->fd, head, (size_t)n);
}

bool rpod_http_write(rpod_http_req_t *req, const void *buf, size_t len)
{
    return write_all(req->fd, buf, len);
}

void rpod_http_send(rpod_http_req_t *req, int status, const char *content_type, const void *body,
                    size_t len, const char *extra_headers)
{
    if (!rpod_http_send_head(req, status, content_type, (long long)len, extra_headers)) {
        return;
    }
    if (strcmp(req->method, "HEAD") != 0 && len > 0) {
        write_all(req->fd, body, len);
    }
}

void rpod_http_send_json(rpod_http_req_t *req, int status, const rpod_json_t *j)
{
    if (j->oom || j->s == NULL) {
        rpod_http_send_error(req, 500, "out of memory");
        return;
    }
    rpod_http_send(req, status, "application/json", j->s, j->len, "Cache-Control: no-store\r\n");
}

void rpod_http_send_error(rpod_http_req_t *req, int status, const char *message)
{
    rpod_json_t j;
    rpod_json_init(&j);
    rpod_json_obj(&j);
    rpod_json_kstr(&j, "error", message);
    rpod_json_end_obj(&j);
    if (j.oom) {
        static const char fallback[] = "{\"error\":\"out of memory\"}";
        rpod_http_send(req, status, "application/json", fallback, sizeof(fallback) - 1, "");
    } else {
        rpod_http_send(req, status, "application/json", j.s, j.len, "Cache-Control: no-store\r\n");
    }
    rpod_json_free(&j);
}

static const char *mime_type(const char *name)
{
    static const struct {
        const char *ext;
        const char *type;
    } types[] = {
        { ".html", "text/html; charset=utf-8" },
        { ".css", "text/css; charset=utf-8" },
        { ".js", "text/javascript; charset=utf-8" },
        { ".json", "application/json" },
        { ".svg", "image/svg+xml" },
        { ".png", "image/png" },
        { ".jpg", "image/jpeg" },
        { ".ico", "image/x-icon" },
        { ".ttf", "font/ttf" },
        { ".woff2", "font/woff2" },
        { ".webmanifest", "application/manifest+json" },
        { ".txt", "text/plain; charset=utf-8" },
    };
    const char *dot = strrchr(name, '.');
    for (size_t i = 0; dot != NULL && i < sizeof(types) / sizeof(types[0]); i++) {
        if (strcasecmp(dot, types[i].ext) == 0) {
            return types[i].type;
        }
    }
    return "application/octet-stream";
}

void rpod_http_send_file(rpod_http_req_t *req, const char *root_dir, const char *rel)
{
    char path[4096];
    if ((size_t)snprintf(path, sizeof(path), "%s/%s", root_dir, rel) >= sizeof(path)) {
        rpod_http_send_error(req, 404, "not found");
        return;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        if (fd >= 0) {
            close(fd);
        }
        rpod_http_send_error(req, 404, "not found");
        return;
    }

    /* Revalidated every time (no-cache), which costs a 304 at most: the
     * files change with every deploy. */
    char etag[64];
    snprintf(etag, sizeof(etag), "\"%llx-%llx\"", (unsigned long long)st.st_mtime,
             (unsigned long long)st.st_size);
    char headers[256];
    snprintf(headers, sizeof(headers), "ETag: %s\r\nCache-Control: no-cache\r\n", etag);
    if (strcmp(req->if_none_match, etag) == 0) {
        close(fd);
        rpod_http_send(req, 304, mime_type(rel), NULL, 0, headers);
        return;
    }
    if (!rpod_http_send_head(req, 200, mime_type(rel), (long long)st.st_size, headers) ||
        strcmp(req->method, "HEAD") == 0) {
        close(fd);
        return;
    }
    off_t off = 0;
    while (off < st.st_size) {
        ssize_t n = sendfile(req->fd, fd, &off, (size_t)(st.st_size - off));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
    }
    close(fd);
}
