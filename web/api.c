#include "api.h"

#include "control_client.h"
#include "library.h"
#include "videos.h"
#include "audio/embedded_art.h"
#include "audio/mpd_client.h"
#include "video/video_library.h"

#include <arpa/inet.h>
#include <curl/curl.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <unistd.h>

#ifndef RPOD_VERSION
#define RPOD_VERSION "dev"
#endif

/* An upload leaves at least this much of the music partition free: the
 * system's logs and MPD's database share the card. */
#define UPLOAD_RESERVE (64LL * 1024 * 1024)

/* Uploads are written a piece at a time, and pushed to the card and
 * dropped from the page cache every so often: on 512 MB, an album's worth
 * of dirty pages would otherwise crowd out everything else (and page-cache
 * pressure once stalled the click wheel's DMA -- CLAUDE.md). */
#define UPLOAD_CHUNK (256 * 1024)
#define UPLOAD_FLUSH (8LL * 1024 * 1024)

/* Cover art bigger than this isn't sent (the same cap the UI's cache uses). */
#define MAX_COVER_BYTES (8u * 1024u * 1024u)

/* Form bodies are a few settings at most. */
#define MAX_FORM_BODY (64 * 1024)

/* --- Helpers ----------------------------------------------------------------- */

static rpod_mpd_t *mpd_open(const rpod_web_config_t *cfg, rpod_http_req_t *req)
{
    rpod_mpd_t *mpd = rpod_mpd_connect(cfg->mpd_socket);
    if (mpd == NULL || !rpod_mpd_is_connected(mpd)) {
        rpod_mpd_disconnect(mpd);
        if (req != NULL) {
            rpod_http_send_error(req, 502, "MPD isn't running");
        }
        return NULL;
    }
    return mpd;
}

static const char *state_name(rpod_mpd_state_t s)
{
    switch (s) {
    case RPOD_MPD_STATE_PLAY: return "play";
    case RPOD_MPD_STATE_PAUSE: return "pause";
    case RPOD_MPD_STATE_STOP: return "stop";
    default: return "unknown";
    }
}

/* The theme, straight from rPod's state file (src/ui/theme.c). */
static void read_theme(const rpod_web_config_t *cfg, char *out, size_t out_size)
{
    static const char *known[] = { "glass", "classic", "classic-dark", "wii" };
    snprintf(out, out_size, "glass");
    FILE *f = cfg->theme_state != NULL ? fopen(cfg->theme_state, "r") : NULL;
    if (f == NULL) {
        return;
    }
    char key[32];
    if (fscanf(f, "%31s", key) == 1) {
        for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
            if (strcmp(key, known[i]) == 0) {
                snprintf(out, out_size, "%s", key);
            }
        }
    }
    fclose(f);
}

static bool parse_bool(const char *v)
{
    return strcmp(v, "1") == 0 || strcmp(v, "on") == 0 || strcmp(v, "true") == 0;
}

static bool parse_uint(const char *s, unsigned long max, unsigned long *out)
{
    char *end;
    errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (s[0] == '\0' || s[0] == '-' || *end != '\0' || errno != 0 || v > max) {
        return false;
    }
    *out = v;
    return true;
}

static void storage(const char *path, long long *free_b, long long *total_b)
{
    struct statvfs vfs;
    if (statvfs(path, &vfs) == 0) {
        *free_b = (long long)vfs.f_bavail * (long long)vfs.f_frsize;
        *total_b = (long long)vfs.f_blocks * (long long)vfs.f_frsize;
    } else {
        *free_b = *total_b = -1;
    }
}

static void json_storage(rpod_json_t *j, const char *key, const char *path)
{
    long long free_b, total_b;
    storage(path, &free_b, &total_b);
    rpod_json_key(j, key);
    rpod_json_obj(j);
    rpod_json_kint(j, "free", free_b);
    rpod_json_kint(j, "total", total_b);
    rpod_json_end_obj(j);
}

static void json_song_fields(rpod_json_t *j, const rpod_mpd_song_t *s)
{
    rpod_json_kstr(j, "title", s->title);
    rpod_json_kstr(j, "artist", s->artist);
    rpod_json_kstr(j, "album", s->album);
    rpod_json_kstr(j, "album_artist", s->album_artist);
    rpod_json_kint(j, "track", s->track);
    rpod_json_kint(j, "duration", s->duration_s);
}

/* "battery": {level, state, mv} from rPod's `get` (src/power/battery.h),
 * or null -- no PiSugar, or rPod isn't running to read it. */
static void json_battery(rpod_json_t *j, const char *ui)
{
    char level[16], state[16], mv[16];
    rpod_json_key(j, "battery");
    if (ui == NULL || !rpod_ctl_value(ui, "battery", level, sizeof(level)) ||
        !rpod_ctl_value(ui, "battery_state", state, sizeof(state)) ||
        !rpod_ctl_value(ui, "battery_mv", mv, sizeof(mv))) {
        rpod_json_null(j);
        return;
    }
    rpod_json_obj(j);
    rpod_json_kint(j, "level", strtol(level, NULL, 10));
    rpod_json_kstr(j, "state", state);
    rpod_json_kint(j, "mv", strtol(mv, NULL, 10));
    rpod_json_end_obj(j);
}

/* --- Player -------------------------------------------------------------------- */

static void get_state(const rpod_web_config_t *cfg, rpod_http_req_t *req)
{
    char theme[32];
    read_theme(cfg, theme, sizeof(theme));

    rpod_json_t j;
    rpod_json_init(&j);
    rpod_json_obj(&j);
    rpod_json_kstr(&j, "theme", theme);
    rpod_json_kstr(&j, "hostname", cfg->hostname);
    /* Whether `make ipod-assets` put the iPod's font and icons beside the
     * page, so it only asks for them when they're there. */
    char manifest[PATH_MAX];
    snprintf(manifest, sizeof(manifest), "%s/manifest.json", cfg->ipod_root != NULL ? cfg->ipod_root : "");
    rpod_json_kbool(&j, "ipod_assets", cfg->ipod_root != NULL && access(manifest, R_OK) == 0);

    rpod_mpd_t *mpd = mpd_open(cfg, NULL);
    rpod_mpd_status_t st;
    rpod_mpd_stats_t stats;
    bool have = mpd != NULL && rpod_mpd_get_status(mpd, &st);
    bool have_stats = mpd != NULL && rpod_mpd_get_stats(mpd, &stats);
    rpod_mpd_disconnect(mpd);

    rpod_json_kbool(&j, "mpd", have);
    if (have) {
        rpod_json_key(&j, "player");
        rpod_json_obj(&j);
        rpod_json_kstr(&j, "state", state_name(st.state));
        rpod_json_kstr(&j, "title", st.title);
        rpod_json_kstr(&j, "artist", st.artist);
        rpod_json_kstr(&j, "album", st.album);
        rpod_json_kstr(&j, "uri", st.uri);
        /* A video playing on the device (src/ui/screens/video_screens.c):
         * MPD has its sound by its full path in the videos folder. */
        size_t vlen = strlen(cfg->video_dir);
        if (vlen > 0 && strncmp(st.uri, cfg->video_dir, vlen) == 0 && st.uri[vlen] == '/' &&
            strchr(st.uri + vlen + 1, '/') == NULL) {
            rpod_json_kstr(&j, "video", st.uri + vlen + 1);
        }
        rpod_json_kint(&j, "elapsed", st.elapsed_s);
        rpod_json_kint(&j, "duration", st.duration_s);
        rpod_json_kint(&j, "volume", st.volume);
        rpod_json_kint(&j, "pos", st.song_pos);
        rpod_json_kint(&j, "length", st.queue_len);
        rpod_json_kbool(&j, "repeat", st.repeat);
        rpod_json_kbool(&j, "single", st.single);
        rpod_json_kbool(&j, "random", st.random);
        rpod_json_end_obj(&j);
    }
    rpod_json_kbool(&j, "updating", have_stats && stats.update_job != 0);
    rpod_json_kint(&j, "db_update", have_stats ? (long long)stats.db_update : 0);

    char *ui = rpod_ctl_request(cfg->control_sock, "get");
    json_battery(&j, rpod_ctl_ok(ui) ? ui : NULL);
    free(ui);
    rpod_json_end_obj(&j);
    rpod_http_send_json(req, 200, &j);
    rpod_json_free(&j);
}

typedef struct {
    rpod_mpd_t *mpd;
    const char *error;
} player_ctx_t;

static bool player_param(const char *key, const char *v, void *ctx)
{
    player_ctx_t *p = ctx;
    unsigned long n;
    bool ok = true;
    if (strcmp(key, "action") == 0) {
        if (strcmp(v, "toggle") == 0) {
            rpod_mpd_state_t s;
            /* From stopped, "toggle" plays rather than doing nothing. */
            ok = rpod_mpd_get_state(p->mpd, &s) &&
                 (s == RPOD_MPD_STATE_STOP ? rpod_mpd_cue_first_paused(p->mpd) &&
                                                 rpod_mpd_set_paused(p->mpd, false)
                                           : rpod_mpd_toggle_pause(p->mpd));
        } else if (strcmp(v, "next") == 0) {
            ok = rpod_mpd_next(p->mpd);
        } else if (strcmp(v, "prev") == 0) {
            /* The device's Prev: restart a song more than 2 s in. */
            rpod_mpd_state_t s;
            unsigned el, dur;
            if (rpod_mpd_get_position(p->mpd, &s, &el, &dur) && el >= 2000 &&
                (s == RPOD_MPD_STATE_PLAY || s == RPOD_MPD_STATE_PAUSE)) {
                ok = rpod_mpd_seek(p->mpd, 0);
            } else {
                ok = rpod_mpd_previous(p->mpd);
            }
        } else {
            p->error = "unknown action";
            return false;
        }
    } else if (strcmp(key, "volume") == 0) {
        ok = parse_uint(v, 100, &n) && rpod_mpd_set_volume(p->mpd, (unsigned)n);
    } else if (strcmp(key, "seek") == 0) {
        ok = parse_uint(v, 24UL * 3600 * 1000, &n) && rpod_mpd_seek(p->mpd, (unsigned)n);
    } else if (strcmp(key, "repeat") == 0) {
        ok = rpod_mpd_set_repeat(p->mpd, parse_bool(v));
    } else if (strcmp(key, "random") == 0) {
        ok = rpod_mpd_set_random(p->mpd, parse_bool(v));
    } else if (strcmp(key, "single") == 0) {
        ok = rpod_mpd_set_single(p->mpd, parse_bool(v));
    } else {
        p->error = "unknown parameter";
        return false;
    }
    if (!ok) {
        p->error = "MPD refused that";
    }
    return ok;
}

static void post_player(const rpod_web_config_t *cfg, rpod_http_req_t *req, const char *form)
{
    rpod_mpd_t *mpd = mpd_open(cfg, req);
    if (mpd == NULL) {
        return;
    }
    player_ctx_t p = { mpd, NULL };
    rpod_http_each_param(form, player_param, &p);
    rpod_mpd_disconnect(mpd);
    if (p.error != NULL) {
        rpod_http_send_error(req, 400, p.error);
        return;
    }
    get_state(cfg, req);
}

/* --- Cover art ----------------------------------------------------------------- */

static bool read_small_file(int dir_fd, const char *name, unsigned char **out, size_t *out_size)
{
    int fd = openat(dir_fd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        (unsigned long long)st.st_size > MAX_COVER_BYTES) {
        if (fd >= 0) {
            close(fd);
        }
        return false;
    }
    unsigned char *buf = malloc((size_t)st.st_size);
    size_t got = 0;
    while (buf != NULL && got < (size_t)st.st_size) {
        ssize_t n = read(fd, buf + got, (size_t)st.st_size - got);
        if (n <= 0) {
            break;
        }
        got += (size_t)n;
    }
    close(fd);
    if (buf == NULL || got != (size_t)st.st_size) {
        free(buf);
        return false;
    }
    *out = buf;
    *out_size = got;
    return true;
}

static const char *image_type(const unsigned char *d, size_t n)
{
    if (n >= 3 && d[0] == 0xff && d[1] == 0xd8 && d[2] == 0xff) {
        return "image/jpeg";
    }
    if (n >= 8 && memcmp(d, "\x89PNG\r\n\x1a\n", 8) == 0) {
        return "image/png";
    }
    if (n >= 6 && (memcmp(d, "GIF87a", 6) == 0 || memcmp(d, "GIF89a", 6) == 0)) {
        return "image/gif";
    }
    if (n >= 12 && memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "WEBP", 4) == 0) {
        return "image/webp";
    }
    return NULL;
}

/* The song's own picture for FLAC (read straight off the card, never via
 * MPD's readpicture, which holds every client up -- CLAUDE.md), else a
 * cover image beside it. */
static void get_cover(const rpod_web_config_t *cfg, rpod_http_req_t *req)
{
    char uri[1100];
    if (!rpod_http_param(req->query, "uri", uri, sizeof(uri)) || !rpod_lib_path_ok(uri, false)) {
        rpod_http_send_error(req, 400, "bad uri");
        return;
    }
    char parent[1100];
    const char *base = strrchr(uri, '/');
    if (base == NULL) {
        parent[0] = '\0';
        base = uri;
    } else {
        snprintf(parent, sizeof(parent), "%.*s", (int)(base - uri), uri);
        base++;
    }
    int dir_fd = rpod_lib_open_dir(cfg->music_dir, parent, false);
    struct stat st;
    if (dir_fd < 0 || fstatat(dir_fd, base, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(st.st_mode)) {
        if (dir_fd >= 0) {
            close(dir_fd);
        }
        rpod_http_send_error(req, 404, "no such song");
        return;
    }

    /* Keyed on the song file: re-tagged art shows up, unchanged art costs a
     * stat and a 304. */
    char etag[64];
    snprintf(etag, sizeof(etag), "\"c%llx-%llx\"", (unsigned long long)st.st_mtime,
             (unsigned long long)st.st_size);
    char headers[160];
    snprintf(headers, sizeof(headers), "ETag: %s\r\nCache-Control: no-cache\r\n", etag);
    if (strcmp(req->if_none_match, etag) == 0) {
        close(dir_fd);
        rpod_http_send(req, 304, "image/jpeg", NULL, 0, headers);
        return;
    }

    unsigned char *data = NULL;
    size_t size = 0;
    const char *dot = strrchr(base, '.');
    if (dot != NULL && strcasecmp(dot, ".flac") == 0) {
        char path[PATH_MAX];
        if ((size_t)snprintf(path, sizeof(path), "%s/%s", cfg->music_dir, uri) < sizeof(path)) {
            rpod_embedded_art_read(path, MAX_COVER_BYTES, &data, &size);
        }
    }
    static const char *beside[] = { "cover.jpg", "cover.png", "folder.jpg", "folder.png",
                                    "front.jpg", "front.png", "Cover.jpg", "Folder.jpg" };
    for (size_t i = 0; data == NULL && i < sizeof(beside) / sizeof(beside[0]); i++) {
        read_small_file(dir_fd, beside[i], &data, &size);
    }
    close(dir_fd);

    const char *type = data != NULL ? image_type(data, size) : NULL;
    if (type == NULL) {
        free(data);
        rpod_http_send_error(req, 404, "no cover art");
        return;
    }
    rpod_http_send(req, 200, type, data, size, headers);
    free(data);
}

/* --- Library -------------------------------------------------------------------- */

static int song_cmp_uri(const void *a, const void *b)
{
    return strcmp(((const rpod_mpd_song_t *)a)->uri, ((const rpod_mpd_song_t *)b)->uri);
}

static int song_cmp_key(const void *key, const void *song)
{
    return strcmp(key, ((const rpod_mpd_song_t *)song)->uri);
}

static void get_library(const rpod_web_config_t *cfg, rpod_http_req_t *req)
{
    char path[1100] = "";
    rpod_http_param(req->query, "path", path, sizeof(path));
    if (!rpod_lib_path_ok(path, true)) {
        rpod_http_send_error(req, 400, "bad path");
        return;
    }
    int dir_fd = rpod_lib_open_dir(cfg->music_dir, path, false);
    if (dir_fd < 0) {
        rpod_http_send_error(req, dir_fd == -ENOENT || dir_fd == -ENOTDIR ? 404 : 500,
                             dir_fd == -ENOENT ? "no such folder" : strerror(-dir_fd));
        return;
    }
    bool writable = faccessat(dir_fd, ".", W_OK, 0) == 0;
    DIR *d = fdopendir(dir_fd);
    if (d == NULL) {
        close(dir_fd);
        rpod_http_send_error(req, 500, "couldn't read the folder");
        return;
    }

    /* What MPD knows about the songs here, by uri, for their tags. */
    rpod_mpd_song_t *songs = NULL;
    size_t song_count = 0;
    rpod_mpd_t *mpd = mpd_open(cfg, NULL);
    bool have_mpd = mpd != NULL;
    if (have_mpd) {
        rpod_mpd_list_dir_songs(mpd, path, &songs, &song_count);
        rpod_mpd_disconnect(mpd);
        if (song_count > 0) {
            qsort(songs, song_count, sizeof(*songs), song_cmp_uri);
        }
    }

    rpod_json_t j;
    rpod_json_init(&j);
    rpod_json_obj(&j);
    rpod_json_kstr(&j, "path", path);
    rpod_json_kbool(&j, "writable", writable);
    rpod_json_kbool(&j, "mpd", have_mpd);
    rpod_json_key(&j, "entries");
    rpod_json_arr(&j);
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') {
            continue; /* hidden, and uploads still arriving */
        }
        struct stat st;
        if (fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 ||
            !(S_ISDIR(st.st_mode) || S_ISREG(st.st_mode))) {
            continue; /* symlinks aren't followed here at all */
        }
        rpod_json_obj(&j);
        rpod_json_kstr(&j, "name", e->d_name);
        rpod_json_kbool(&j, "dir", S_ISDIR(st.st_mode));
        rpod_json_kint(&j, "size", (long long)st.st_size);
        rpod_json_kint(&j, "mtime", (long long)st.st_mtime);
        if (!rpod_lib_name_ok(e->d_name)) {
            /* Can't be named back to us safely: shown, but hands off. */
            rpod_json_kbool(&j, "odd_name", true);
        }
        char uri[1400];
        if (S_ISREG(st.st_mode) && song_count > 0 &&
            (size_t)snprintf(uri, sizeof(uri), "%s%s%s", path, path[0] != '\0' ? "/" : "", e->d_name) < sizeof(uri)) {
            const rpod_mpd_song_t *s = bsearch(uri, songs, song_count, sizeof(*songs), song_cmp_key);
            if (s != NULL) {
                rpod_json_key(&j, "song");
                rpod_json_obj(&j);
                json_song_fields(&j, s);
                rpod_json_end_obj(&j);
            }
        }
        rpod_json_end_obj(&j);
    }
    closedir(d);
    rpod_mpd_free_songs(songs);
    rpod_json_end_arr(&j);
    json_storage(&j, "storage", cfg->music_dir);
    rpod_json_end_obj(&j);
    rpod_http_send_json(req, 200, &j);
    rpod_json_free(&j);
}

static void get_library_stats(const rpod_web_config_t *cfg, rpod_http_req_t *req)
{
    rpod_mpd_t *mpd = mpd_open(cfg, req);
    if (mpd == NULL) {
        return;
    }
    rpod_mpd_stats_t s;
    bool ok = rpod_mpd_get_stats(mpd, &s);
    rpod_mpd_disconnect(mpd);
    if (!ok) {
        rpod_http_send_error(req, 502, "MPD didn't answer");
        return;
    }
    rpod_json_t j;
    rpod_json_init(&j);
    rpod_json_obj(&j);
    rpod_json_kint(&j, "songs", s.songs);
    rpod_json_kint(&j, "albums", s.albums);
    rpod_json_kint(&j, "artists", s.artists);
    rpod_json_kint(&j, "playtime", (long long)s.db_playtime_s);
    rpod_json_kint(&j, "db_update", (long long)s.db_update);
    rpod_json_kbool(&j, "updating", s.update_job != 0);
    rpod_json_kbool(&j, "writable", access(cfg->music_dir, W_OK) == 0);
    json_storage(&j, "storage", cfg->music_dir);
    rpod_json_end_obj(&j);
    rpod_http_send_json(req, 200, &j);
    rpod_json_free(&j);
}

/* Pushes what's been written so far to the card and drops it from the page
 * cache: start writing the newest stretch, wait for the one before (long
 * since under way), then forget it. */
static void upload_flush(int fd, long long *flushed, long long written)
{
    long long len = written - *flushed;
    if (len <= 0) {
        return;
    }
    sync_file_range(fd, *flushed, len, SYNC_FILE_RANGE_WRITE);
    if (*flushed > 0) {
        long long prev = *flushed - UPLOAD_FLUSH < 0 ? 0 : *flushed - UPLOAD_FLUSH;
        sync_file_range(fd, prev, *flushed - prev,
                        SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER);
        posix_fadvise(fd, prev, *flushed - prev, POSIX_FADV_DONTNEED);
    }
    *flushed = written;
}

/* Streams the request body into `fd`, flushing as it goes, and gets it onto
 * the card. False if it didn't all arrive or couldn't be written, with
 * *err the errno; *written is how much was. */
static bool receive_body(rpod_http_req_t *req, int fd, long long *written, int *err)
{
    char *buf = malloc(UPLOAD_CHUNK);
    long long flushed = 0;
    bool ok = buf != NULL;
    *written = 0;
    *err = buf != NULL ? 0 : ENOMEM;
    while (ok && req->body_left > 0) {
        ssize_t n = rpod_http_read_body(req, buf, UPLOAD_CHUNK);
        if (n <= 0) {
            ok = false; /* the browser went away mid-file */
            break;
        }
        for (ssize_t off = 0; off < n;) {
            ssize_t w = write(fd, buf + off, (size_t)(n - off));
            if (w < 0 && errno == EINTR) {
                continue;
            }
            if (w <= 0) {
                *err = errno;
                ok = false;
                break;
            }
            off += w;
        }
        *written += n;
        if (*written - flushed >= UPLOAD_FLUSH) {
            upload_flush(fd, &flushed, *written);
        }
    }
    free(buf);
    /* Its data on the card before its name appears: a power cut can't leave
     * a file that's all there by name but not by content. */
    if (ok && fdatasync(fd) != 0) {
        ok = false;
        *err = errno;
    }
    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    return ok;
}

static void put_upload(const rpod_web_config_t *cfg, rpod_http_req_t *req)
{
    char dir[1100] = "", name[300], ow[8] = "0";
    rpod_http_param(req->query, "dir", dir, sizeof(dir));
    rpod_http_param(req->query, "overwrite", ow, sizeof(ow));
    if (!rpod_http_param(req->query, "name", name, sizeof(name)) || !rpod_lib_path_ok(dir, true) ||
        !rpod_lib_upload_name_ok(name)) {
        rpod_http_send_error(req, 400, "that name can't go in the library");
        return;
    }
    if (req->content_length < 0) {
        rpod_http_send_error(req, 411, "send a Content-Length");
        return;
    }
    bool overwrite = parse_bool(ow);

    long long free_b, total_b;
    storage(cfg->music_dir, &free_b, &total_b);
    if (free_b >= 0 && req->content_length + UPLOAD_RESERVE > free_b) {
        rpod_http_send_error(req, 507, "not enough space left");
        return;
    }
    if (!overwrite) {
        /* Before the body: no point taking a file that can't stay. */
        int dfd = rpod_lib_open_dir(cfg->music_dir, dir, false);
        struct stat st;
        bool exists = dfd >= 0 && fstatat(dfd, name, &st, AT_SYMLINK_NOFOLLOW) == 0;
        if (dfd >= 0) {
            close(dfd);
        }
        if (exists) {
            rpod_http_send_error(req, 409, "already in the library");
            return;
        }
    }

    int dir_fd;
    char tmp[64];
    int fd = rpod_lib_upload_begin(cfg->music_dir, dir, &dir_fd, tmp, sizeof(tmp));
    if (fd < 0) {
        rpod_http_send_error(req, fd == -EACCES || fd == -EROFS ? 403 : 500,
                             fd == -EACCES || fd == -EROFS ? "the library isn't writable" : strerror(-fd));
        return;
    }

    long long written;
    int write_err;
    bool ok = receive_body(req, fd, &written, &write_err);
    close(fd);

    if (!ok) {
        unlinkat(dir_fd, tmp, 0);
        close(dir_fd);
        if (req->body_left > 0) {
            req->status = 499; /* nobody left to answer */
            return;
        }
        rpod_http_send_error(req, write_err == ENOSPC ? 507 : 500,
                             write_err == ENOSPC ? "the card is full" : "couldn't write the file");
        return;
    }

    bool replaced;
    int rc = rpod_lib_upload_finish(dir_fd, tmp, name, overwrite, &replaced);
    fsync(dir_fd);
    close(dir_fd);
    if (rc != 0) {
        rpod_http_send_error(req, rc == -EEXIST ? 409 : 500,
                             rc == -EEXIST ? "already in the library" : strerror(-rc));
        return;
    }

    rpod_json_t j;
    rpod_json_init(&j);
    rpod_json_obj(&j);
    char path[1500];
    snprintf(path, sizeof(path), "%s%s%s", dir, dir[0] != '\0' ? "/" : "", name);
    rpod_json_kstr(&j, "path", path);
    rpod_json_kint(&j, "size", written);
    rpod_json_kbool(&j, "replaced", replaced);
    rpod_json_end_obj(&j);
    rpod_http_send_json(req, 201, &j);
    rpod_json_free(&j);
    fprintf(stderr, "rpod-web: uploaded %s (%lld bytes)%s\n", path, written, replaced ? ", replacing" : "");
}

static void send_ok(rpod_http_req_t *req)
{
    rpod_http_send(req, 200, "application/json", "{\"ok\":true}", 11, "Cache-Control: no-store\r\n");
}

static void send_errno(rpod_http_req_t *req, int rc)
{
    switch (-rc) {
    case EINVAL: rpod_http_send_error(req, 400, "that name can't go in the library"); break;
    case ENOENT: rpod_http_send_error(req, 404, "it isn't there any more"); break;
    case EEXIST: rpod_http_send_error(req, 409, "something's already called that"); break;
    case EACCES:
    case EPERM:
    case EROFS: rpod_http_send_error(req, 403, "the library isn't writable"); break;
    case ENOTEMPTY: rpod_http_send_error(req, 409, "the folder isn't empty"); break;
    default: rpod_http_send_error(req, 500, strerror(-rc)); break;
    }
}

/* Tells MPD to look at what changed: an update of just the folder touched
 * when there is one. auto_update would get there too, but not on every
 * filesystem, and not right away. */
static void mpd_update_quietly(const rpod_web_config_t *cfg, const char *path)
{
    rpod_mpd_t *mpd = mpd_open(cfg, NULL);
    if (mpd != NULL) {
        rpod_mpd_update(mpd, path, false, NULL);
        rpod_mpd_disconnect(mpd);
    }
}

static void parent_of(const char *path, char *out, size_t out_size)
{
    const char *slash = strrchr(path, '/');
    snprintf(out, out_size, "%.*s", slash != NULL ? (int)(slash - path) : 0, path);
}

static void post_mkdir(const rpod_web_config_t *cfg, rpod_http_req_t *req, const char *form)
{
    char path[1100];
    if (!rpod_http_param(form, "path", path, sizeof(path)) || !rpod_lib_path_ok(path, false)) {
        rpod_http_send_error(req, 400, "that name can't go in the library");
        return;
    }
    int fd = rpod_lib_open_dir(cfg->music_dir, path, true);
    if (fd < 0) {
        send_errno(req, fd);
        return;
    }
    close(fd);
    send_ok(req);
}

static void post_delete(const rpod_web_config_t *cfg, rpod_http_req_t *req, const char *form)
{
    char path[1100];
    if (!rpod_http_param(form, "path", path, sizeof(path)) || !rpod_lib_path_ok(path, false)) {
        rpod_http_send_error(req, 400, "bad path");
        return;
    }
    int rc = rpod_lib_delete(cfg->music_dir, path);
    if (rc != 0) {
        send_errno(req, rc);
        return;
    }
    fprintf(stderr, "rpod-web: deleted %s\n", path);
    char parent[1100];
    parent_of(path, parent, sizeof(parent));
    mpd_update_quietly(cfg, parent);
    send_ok(req);
}

static void post_rename(const rpod_web_config_t *cfg, rpod_http_req_t *req, const char *form)
{
    char path[1100], name[300];
    if (!rpod_http_param(form, "path", path, sizeof(path)) || !rpod_http_param(form, "name", name, sizeof(name))) {
        rpod_http_send_error(req, 400, "bad request");
        return;
    }
    int rc = rpod_lib_rename(cfg->music_dir, path, name);
    if (rc != 0) {
        send_errno(req, rc);
        return;
    }
    char parent[1100];
    parent_of(path, parent, sizeof(parent));
    mpd_update_quietly(cfg, parent);
    send_ok(req);
}

static void post_update(const rpod_web_config_t *cfg, rpod_http_req_t *req, const char *form)
{
    char path[1100] = "", rescan[8] = "0";
    rpod_http_param(form, "path", path, sizeof(path));
    rpod_http_param(form, "rescan", rescan, sizeof(rescan));
    if (!rpod_lib_path_ok(path, true)) {
        rpod_http_send_error(req, 400, "bad path");
        return;
    }
    rpod_mpd_t *mpd = mpd_open(cfg, req);
    if (mpd == NULL) {
        return;
    }
    unsigned job = 0;
    bool ok = rpod_mpd_update(mpd, path, parse_bool(rescan), &job);
    rpod_mpd_disconnect(mpd);
    if (!ok) {
        rpod_http_send_error(req, 502, "MPD wouldn't update");
        return;
    }
    get_library_stats(cfg, req);
}

static int song_cmp_track(const void *a, const void *b)
{
    const rpod_mpd_song_t *x = a, *y = b;
    if (x->track != y->track) {
        return x->track < y->track ? -1 : 1;
    }
    return strcmp(x->uri, y->uri);
}

/* Plays a song the way choosing it in an album does on the device: the
 * album queued, from this song on. */
static void post_play(const rpod_web_config_t *cfg, rpod_http_req_t *req, const char *form)
{
    char uri[1100];
    if (!rpod_http_param(form, "path", uri, sizeof(uri)) || !rpod_lib_path_ok(uri, false)) {
        rpod_http_send_error(req, 400, "bad path");
        return;
    }
    rpod_mpd_t *mpd = mpd_open(cfg, req);
    if (mpd == NULL) {
        return;
    }
    char parent[1100];
    parent_of(uri, parent, sizeof(parent));
    rpod_mpd_song_t *here = NULL, *album = NULL;
    size_t here_n = 0, album_n = 0;
    const rpod_mpd_song_t *song = NULL;
    if (rpod_mpd_list_dir_songs(mpd, parent, &here, &here_n)) {
        for (size_t i = 0; i < here_n && song == NULL; i++) {
            if (strcmp(here[i].uri, uri) == 0) {
                song = &here[i];
            }
        }
    }
    bool ok;
    if (song == NULL) {
        ok = false;
    } else if (song->album[0] != '\0' && rpod_mpd_list_songs(mpd, NULL, song->album, &album, &album_n) &&
               album_n > 0) {
        qsort(album, album_n, sizeof(*album), song_cmp_track);
        size_t start = 0;
        for (size_t i = 0; i < album_n; i++) {
            if (strcmp(album[i].uri, uri) == 0) {
                start = i;
            }
        }
        ok = rpod_mpd_play_songs_from(mpd, album, album_n, start);
    } else {
        ok = rpod_mpd_play_uri(mpd, uri);
    }
    rpod_mpd_free_songs(here);
    rpod_mpd_free_songs(album);
    rpod_mpd_disconnect(mpd);
    if (!ok) {
        rpod_http_send_error(req, song == NULL ? 404 : 502,
                             song == NULL ? "MPD hasn't found that song yet" : "MPD wouldn't play it");
        return;
    }
    get_state(cfg, req);
}

/* --- Videos (docs/PLAN.md §8.7) --------------------------------------------------- */

/* A video's name from a query or form: one of the folder's own. */
static bool video_param(const char *qs, char *out, size_t size)
{
    return rpod_http_param(qs, "name", out, size) && rpod_lib_name_ok(out) && rpod_video_name_ok(out);
}

static bool poster_exists(const rpod_web_config_t *cfg, const char *name)
{
    char path[PATH_MAX + 300];
    rpod_video_poster_path(cfg->video_dir, name, path, sizeof(path));
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static void get_videos(const rpod_web_config_t *cfg, rpod_http_req_t *req)
{
    rpod_video_entry_t *list = NULL;
    size_t n = 0;
    bool listed = rpod_video_list(cfg->video_dir, &list, &n);
    rpod_json_t j;
    rpod_json_init(&j);
    rpod_json_obj(&j);
    rpod_json_kbool(&j, "folder", listed);
    rpod_json_kbool(&j, "writable", access(cfg->video_dir, W_OK) == 0);
    rpod_json_key(&j, "videos");
    rpod_json_arr(&j);
    for (size_t i = 0; i < n; i++) {
        rpod_json_obj(&j);
        rpod_json_kstr(&j, "name", list[i].name);
        rpod_json_kstr(&j, "title", list[i].title);
        rpod_json_kint(&j, "size", list[i].size);
        rpod_json_kint(&j, "mtime", (long long)list[i].mtime);
        rpod_json_kint(&j, "duration", list[i].duration_ms);
        rpod_json_kint(&j, "width", list[i].width);
        rpod_json_kint(&j, "height", list[i].height);
        rpod_json_kbool(&j, "poster", poster_exists(cfg, list[i].name));
        rpod_json_end_obj(&j);
    }
    rpod_json_end_arr(&j);
    free(list);
    rpod_json_key(&j, "jobs");
    rpod_videos_json_jobs(&j);
    json_storage(&j, "storage", cfg->video_dir);
    rpod_json_end_obj(&j);
    rpod_http_send_json(req, 200, &j);
    rpod_json_free(&j);
}

static void put_video_upload(const rpod_web_config_t *cfg, rpod_http_req_t *req)
{
    char name[300], ow[8] = "0", target[300];
    rpod_http_param(req->query, "overwrite", ow, sizeof(ow));
    if (!rpod_http_param(req->query, "name", name, sizeof(name)) || !rpod_videos_upload_name_ok(name)) {
        rpod_http_send_error(req, 400, "that isn't a kind of video rPod takes");
        return;
    }
    if (req->content_length < 0) {
        rpod_http_send_error(req, 411, "send a Content-Length");
        return;
    }
    bool overwrite = parse_bool(ow);
    rpod_videos_target(name, target, sizeof(target));

    long long free_b, total_b;
    storage(cfg->video_dir, &free_b, &total_b);
    if (free_b >= 0 && req->content_length + UPLOAD_RESERVE > free_b) {
        rpod_http_send_error(req, 507, "not enough space left");
        return;
    }
    if (!overwrite) {
        /* Before the body, as for music. */
        char path[PATH_MAX + 300];
        snprintf(path, sizeof(path), "%s/%s", cfg->video_dir, target);
        struct stat st;
        if (lstat(path, &st) == 0 || rpod_videos_pending(target)) {
            rpod_http_send_error(req, 409, "already in Videos");
            return;
        }
    }

    int dir_fd;
    char tmp[64];
    int fd = rpod_lib_upload_begin(cfg->video_dir, "", &dir_fd, tmp, sizeof(tmp));
    if (fd < 0) {
        rpod_http_send_error(req, fd == -EACCES || fd == -EROFS ? 403 : 500,
                             fd == -EACCES || fd == -EROFS ? "the videos folder isn't writable" : strerror(-fd));
        return;
    }
    long long written;
    int write_err;
    bool ok = receive_body(req, fd, &written, &write_err);
    close(fd);
    if (!ok) {
        unlinkat(dir_fd, tmp, 0);
        close(dir_fd);
        if (req->body_left > 0) {
            req->status = 499;
            return;
        }
        rpod_http_send_error(req, write_err == ENOSPC ? 507 : 500,
                             write_err == ENOSPC ? "the card is full" : "couldn't write the file");
        return;
    }
    close(dir_fd);

    unsigned id = rpod_videos_queue(tmp, target, overwrite);
    if (id == 0) {
        rpod_http_send_error(req, 503, "couldn't line it up for converting");
        return;
    }
    rpod_json_t j;
    rpod_json_init(&j);
    rpod_json_obj(&j);
    rpod_json_kint(&j, "job", id);
    rpod_json_kstr(&j, "name", target);
    rpod_json_kint(&j, "size", written);
    rpod_json_end_obj(&j);
    rpod_http_send_json(req, 202, &j);
    rpod_json_free(&j);
    fprintf(stderr, "rpod-web: uploaded %s (%lld bytes) to convert as %s\n", name, written, target);
}

static void post_video_delete(const rpod_web_config_t *cfg, rpod_http_req_t *req, const char *form)
{
    char name[300];
    if (!video_param(form, name, sizeof(name))) {
        rpod_http_send_error(req, 400, "bad name");
        return;
    }
    int dfd = rpod_lib_open_dir(cfg->video_dir, "", false);
    if (dfd < 0) {
        send_errno(req, dfd);
        return;
    }
    struct stat st;
    int rc = fstatat(dfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0 ? -errno
             : !S_ISREG(st.st_mode)                            ? -EINVAL
             : unlinkat(dfd, name, 0) != 0                     ? -errno
                                                               : 0;
    close(dfd);
    if (rc != 0) {
        send_errno(req, rc);
        return;
    }
    char poster[PATH_MAX + 300];
    rpod_video_poster_path(cfg->video_dir, name, poster, sizeof(poster));
    unlink(poster);
    fprintf(stderr, "rpod-web: deleted video %s\n", name);
    send_ok(req);
}

static void post_video_rename(const rpod_web_config_t *cfg, rpod_http_req_t *req, const char *form)
{
    char name[300], title[300], to[320];
    if (!video_param(form, name, sizeof(name)) || !rpod_http_param(form, "title", title, sizeof(title))) {
        rpod_http_send_error(req, 400, "bad request");
        return;
    }
    const char *ext = strrchr(name, '.');
    if ((size_t)snprintf(to, sizeof(to), "%s%s", title, ext) >= sizeof(to) || !rpod_lib_name_ok(to) ||
        !rpod_video_name_ok(to)) {
        rpod_http_send_error(req, 400, "a video can't be called that");
        return;
    }
    int rc = rpod_lib_rename(cfg->video_dir, name, to);
    if (rc != 0) {
        send_errno(req, rc);
        return;
    }
    char from_poster[PATH_MAX + 300], to_poster[PATH_MAX + 300];
    rpod_video_poster_path(cfg->video_dir, name, from_poster, sizeof(from_poster));
    rpod_video_poster_path(cfg->video_dir, to, to_poster, sizeof(to_poster));
    rename(from_poster, to_poster);
    send_ok(req);
}

static void post_video_cancel(rpod_http_req_t *req, const char *form)
{
    char id[16];
    unsigned long v;
    if (!rpod_http_param(form, "job", id, sizeof(id)) || !parse_uint(id, 0xffffffffu, &v)) {
        rpod_http_send_error(req, 400, "bad job");
        return;
    }
    if (!rpod_videos_cancel((unsigned)v)) {
        rpod_http_send_error(req, 404, "it isn't converting any more");
        return;
    }
    send_ok(req);
}

/* Play on rPod: the device puts it up (src/ui/control.c's `play video`). */
static void post_video_play(const rpod_web_config_t *cfg, rpod_http_req_t *req, const char *form)
{
    char name[300], line[400];
    if (!video_param(form, name, sizeof(name))) {
        rpod_http_send_error(req, 400, "bad name");
        return;
    }
    snprintf(line, sizeof(line), "play video %s", name);
    char *reply = rpod_ctl_request(cfg->control_sock, line);
    if (reply == NULL) {
        rpod_http_send_error(req, 503, "rPod isn't running");
        return;
    }
    if (!rpod_ctl_ok(reply)) {
        char why[200];
        snprintf(why, sizeof(why), "%s", strncmp(reply, "err ", 4) == 0 ? reply + 4 : "rPod wouldn't play it");
        why[strcspn(why, "\n")] = '\0';
        free(reply);
        rpod_http_send_error(req, 409, why);
        return;
    }
    free(reply);
    send_ok(req);
}

static const char *video_type(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot != NULL && (strcasecmp(dot, ".mov") == 0)) {
        return "video/quicktime";
    }
    if (dot != NULL && strcasecmp(dot, ".webm") == 0) {
        return "video/webm";
    }
    if (dot != NULL && strcasecmp(dot, ".mkv") == 0) {
        return "video/x-matroska";
    }
    return "video/mp4";
}

/* The video itself, or its poster, for the page: seekable (Range), and
 * never through a symlink. */
static void get_video_file(const rpod_web_config_t *cfg, rpod_http_req_t *req, bool poster)
{
    char name[300];
    if (!video_param(req->query, name, sizeof(name))) {
        rpod_http_send_error(req, 400, "bad name");
        return;
    }
    char sub[PATH_MAX + 32], file[300];
    if (poster) {
        snprintf(sub, sizeof(sub), "%s/" RPOD_VIDEO_POSTER_DIR, cfg->video_dir);
        const char *dot = strrchr(name, '.');
        snprintf(file, sizeof(file), "%.*s.jpg", (int)(dot - name), name);
    } else {
        snprintf(sub, sizeof(sub), "%s", cfg->video_dir);
        snprintf(file, sizeof(file), "%s", name);
    }
    int dfd = open(sub, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int fd = dfd >= 0 ? openat(dfd, file, O_RDONLY | O_NOFOLLOW | O_CLOEXEC) : -1;
    if (dfd >= 0) {
        close(dfd);
    }
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        if (fd >= 0) {
            close(fd);
        }
        rpod_http_send_error(req, 404, poster ? "no poster" : "no such video");
        return;
    }
    rpod_http_send_fd(req, fd, (long long)st.st_size, poster ? "image/jpeg" : video_type(name),
                      "Cache-Control: no-cache\r\n");
}

/* --- Settings ------------------------------------------------------------------ */

static long long count_lines(const char *path)
{
    FILE *f = path != NULL ? fopen(path, "r") : NULL;
    if (f == NULL) {
        return 0;
    }
    long long n = 0;
    int c, last = '\n';
    while ((c = getc(f)) != EOF) {
        if (c == '\n' && last != '\n') {
            n++;
        }
        last = c;
    }
    if (last != '\n') {
        n++;
    }
    fclose(f);
    return n;
}

/* A `get` value as a number, or JSON null without one. */
static void json_ctl_num(rpod_json_t *j, const char *reply, const char *key, const char *json_key)
{
    char v[64];
    rpod_json_key(j, json_key);
    if (reply != NULL && rpod_ctl_value(reply, key, v, sizeof(v))) {
        rpod_json_num(j, strtod(v, NULL));
    } else {
        rpod_json_null(j);
    }
}

static void get_settings(const rpod_web_config_t *cfg, rpod_http_req_t *req)
{
    char *ui = rpod_ctl_request(cfg->control_sock, "get");
    if (ui != NULL && !rpod_ctl_ok(ui)) {
        free(ui);
        ui = NULL;
    }
    char v[512];

    rpod_json_t j;
    rpod_json_init(&j);
    rpod_json_obj(&j);
    rpod_json_kbool(&j, "ui", ui != NULL);

    if (ui != NULL && rpod_ctl_value(ui, "theme", v, sizeof(v))) {
        rpod_json_kstr(&j, "theme", v);
    } else {
        read_theme(cfg, v, sizeof(v));
        rpod_json_kstr(&j, "theme", v);
    }
    json_ctl_num(&j, ui, "backlight", "backlight");
    json_ctl_num(&j, ui, "sleep", "sleep");
    json_ctl_num(&j, ui, "shutdown", "shutdown");
    json_ctl_num(&j, ui, "sleep_remaining_ms", "sleep_remaining_ms");
    json_ctl_num(&j, ui, "video_sync", "video_sync");

    rpod_json_key(&j, "alarm");
    if (ui != NULL) {
        rpod_json_obj(&j);
        rpod_json_kbool(&j, "on", rpod_ctl_value(ui, "alarm", v, sizeof(v)) && strcmp(v, "on") == 0);
        rpod_json_kstr(&j, "time", rpod_ctl_value(ui, "alarm_time", v, sizeof(v)) ? v : "07:00");
        rpod_json_kstr(&j, "sound", rpod_ctl_value(ui, "alarm_sound", v, sizeof(v)) ? v : "");
        rpod_json_kbool(&j, "ringing", rpod_ctl_value(ui, "alarm_ringing", v, sizeof(v)) && strcmp(v, "1") == 0);
        rpod_json_end_obj(&j);
    } else {
        rpod_json_null(&j);
    }

    /* The wheel's values, in the order rPod lists them. */
    rpod_json_key(&j, "wheel");
    rpod_json_arr(&j);
    for (const char *line = ui; line != NULL && *line != '\0';) {
        const char *end = strchr(line, '\n');
        size_t len = end != NULL ? (size_t)(end - line) : strlen(line);
        const char *eq = memchr(line, '=', len);
        if (eq != NULL && len > 6 && strncmp(line, "wheel.", 6) == 0 && memchr(line + 6, '.', (size_t)(eq - line - 6)) == NULL) {
            char key[64];
            snprintf(key, sizeof(key), "%.*s", (int)(eq - line - 6), line + 6);
            char full[96];
            rpod_json_obj(&j);
            rpod_json_kstr(&j, "key", key);
            snprintf(full, sizeof(full), "wheel.%s", key);
            json_ctl_num(&j, ui, full, "value");
            snprintf(full, sizeof(full), "wheel.%s.reset", key);
            json_ctl_num(&j, ui, full, "reset");
            snprintf(full, sizeof(full), "wheel.%s.min", key);
            json_ctl_num(&j, ui, full, "min");
            snprintf(full, sizeof(full), "wheel.%s.max", key);
            json_ctl_num(&j, ui, full, "max");
            rpod_json_end_obj(&j);
        }
        line = end != NULL ? end + 1 : NULL;
    }
    rpod_json_end_arr(&j);

    rpod_json_key(&j, "listenbrainz");
    rpod_json_obj(&j);
    if (ui != NULL && rpod_ctl_value(ui, "listenbrainz", v, sizeof(v))) {
        rpod_json_kbool(&j, "on", strcmp(v, "on") == 0);
    } else {
        rpod_json_key(&j, "on");
        rpod_json_null(&j);
    }
    rpod_json_kint(&j, "queued", count_lines(cfg->lb_queue));
    rpod_json_end_obj(&j);

    /* Home Assistant: where, and how rPod's getting on with it (never the
     * token, which rPod doesn't hand out). */
    rpod_json_key(&j, "home_assistant");
    if (ui != NULL) {
        rpod_json_obj(&j);
        rpod_json_kbool(&j, "on", rpod_ctl_value(ui, "ha", v, sizeof(v)) && strcmp(v, "on") == 0);
        rpod_json_kstr(&j, "url", rpod_ctl_value(ui, "ha_url", v, sizeof(v)) ? v : "");
        rpod_json_kstr(&j, "state", rpod_ctl_value(ui, "ha_state", v, sizeof(v)) ? v : "off");
        rpod_json_kstr(&j, "error", rpod_ctl_value(ui, "ha_error", v, sizeof(v)) ? v : "");
        rpod_json_kstr(&j, "name", rpod_ctl_value(ui, "ha_name", v, sizeof(v)) ? v : "");
        rpod_json_kstr(&j, "version", rpod_ctl_value(ui, "ha_version", v, sizeof(v)) ? v : "");
        rpod_json_end_obj(&j);
    } else {
        rpod_json_null(&j);
    }
    free(ui);

    rpod_mpd_t *mpd = mpd_open(cfg, NULL);
    rpod_json_kbool(&j, "mpd", mpd != NULL);
    if (mpd != NULL) {
        unsigned xfade;
        rpod_json_key(&j, "crossfade");
        if (rpod_mpd_get_crossfade(mpd, &xfade)) {
            rpod_json_int(&j, xfade);
        } else {
            rpod_json_null(&j);
        }
        rpod_json_key(&j, "replay_gain");
        if (rpod_mpd_get_replay_gain(mpd, v, sizeof(v))) {
            rpod_json_str(&j, v);
        } else {
            rpod_json_null(&j);
        }
        int vol;
        rpod_json_key(&j, "volume");
        if (rpod_mpd_get_volume(mpd, &vol)) {
            rpod_json_int(&j, vol);
        } else {
            rpod_json_null(&j);
        }

        rpod_mpd_item_t *playlists = NULL;
        size_t n = 0;
        rpod_json_key(&j, "playlists");
        rpod_json_arr(&j);
        if (rpod_mpd_list_playlists(mpd, &playlists, &n)) {
            for (size_t i = 0; i < n; i++) {
                rpod_json_str(&j, playlists[i].name);
            }
            rpod_mpd_free_items(playlists);
        }
        rpod_json_end_arr(&j);
        rpod_mpd_disconnect(mpd);
    }
    rpod_json_end_obj(&j);
    rpod_http_send_json(req, 200, &j);
    rpod_json_free(&j);
}

typedef struct {
    const rpod_web_config_t *cfg;
    rpod_mpd_t *mpd;
    char error[256];
} settings_ctx_t;

static bool settings_param(const char *key, const char *v, void *ctx)
{
    settings_ctx_t *s = ctx;
    static const char *ui_keys[] = { "theme", "backlight", "sleep", "shutdown", "alarm", "alarm_time",
                                     "alarm_sound", "video_sync", "listenbrainz_token",
                                     "ha_url", "ha_token" };
    bool for_ui = strncmp(key, "wheel.", 6) == 0;
    for (size_t i = 0; !for_ui && i < sizeof(ui_keys) / sizeof(ui_keys[0]); i++) {
        for_ui = strcmp(key, ui_keys[i]) == 0;
    }
    if (strchr(v, '\n') != NULL || strchr(v, '\r') != NULL || strchr(key, ' ') != NULL) {
        snprintf(s->error, sizeof(s->error), "bad value for %s", key);
        return false;
    }

    if (for_ui) {
        char *line = malloc(strlen(key) + strlen(v) + 8);
        if (line == NULL) {
            snprintf(s->error, sizeof(s->error), "out of memory");
            return false;
        }
        sprintf(line, "set %s %s", key, v);
        char *reply = rpod_ctl_request(s->cfg->control_sock, line);
        free(line);
        if (reply == NULL) {
            snprintf(s->error, sizeof(s->error), "rPod isn't running");
            return false;
        }
        bool ok = rpod_ctl_ok(reply);
        if (!ok) {
            /* "err <why>": the why, for the page to show. */
            const char *why = strncmp(reply, "err ", 4) == 0 ? reply + 4 : reply;
            snprintf(s->error, sizeof(s->error), "%.*s", (int)strcspn(why, "\n"), why);
        }
        free(reply);
        return ok;
    }

    if (s->mpd == NULL) {
        s->mpd = mpd_open(s->cfg, NULL);
        if (s->mpd == NULL) {
            snprintf(s->error, sizeof(s->error), "MPD isn't running");
            return false;
        }
    }
    unsigned long n;
    bool ok;
    if (strcmp(key, "crossfade") == 0) {
        ok = parse_uint(v, 30, &n) && rpod_mpd_set_crossfade(s->mpd, (unsigned)n);
    } else if (strcmp(key, "replay_gain") == 0) {
        ok = rpod_mpd_set_replay_gain(s->mpd, v);
    } else if (strcmp(key, "volume") == 0) {
        ok = parse_uint(v, 100, &n) && rpod_mpd_set_volume(s->mpd, (unsigned)n);
    } else {
        snprintf(s->error, sizeof(s->error), "unknown setting %s", key);
        return false;
    }
    if (!ok) {
        snprintf(s->error, sizeof(s->error), "MPD refused %s", key);
    }
    return ok;
}

static void post_settings(const rpod_web_config_t *cfg, rpod_http_req_t *req, const char *form)
{
    settings_ctx_t s = { .cfg = cfg };
    rpod_http_each_param(form, settings_param, &s);
    rpod_mpd_disconnect(s.mpd);
    if (s.error[0] != '\0') {
        rpod_http_send_error(req, 400, s.error);
        return;
    }
    get_settings(cfg, req);
}

static void post_wheel_reset(const rpod_web_config_t *cfg, rpod_http_req_t *req)
{
    char *reply = rpod_ctl_request(cfg->control_sock, "reset wheel");
    bool ok = rpod_ctl_ok(reply);
    free(reply);
    if (!ok) {
        rpod_http_send_error(req, 503, "rPod isn't running");
        return;
    }
    get_settings(cfg, req);
}

/* --- ListenBrainz ---------------------------------------------------------------- */

typedef struct {
    char *s;
    size_t len;
} grow_buf_t;

static size_t collect(char *data, size_t size, size_t nmemb, void *user)
{
    grow_buf_t *b = user;
    size_t n = size * nmemb;
    if (b->len + n > 64 * 1024) {
        return 0; /* far more than the answer */
    }
    char *grown = realloc(b->s, b->len + n + 1);
    if (grown == NULL) {
        return 0;
    }
    b->s = grown;
    memcpy(b->s + b->len, data, n);
    b->len += n;
    b->s[b->len] = '\0';
    return n;
}

/* The string value of "key" in a flat JSON answer -- all
 * validate-token's needs; escapes are kept as they come. */
static bool json_find_str(const char *json, const char *key, char *out, size_t out_size)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (p == NULL) {
        return false;
    }
    p += strlen(pat);
    while (*p == ' ' || *p == ':') {
        p++;
    }
    if (*p != '"') {
        return false;
    }
    p++;
    size_t i = 0;
    while (*p != '\0' && *p != '"' && i + 1 < out_size) {
        if (*p == '\\' && p[1] != '\0') {
            p++;
        }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return true;
}

/* Asks ListenBrainz whose token this is, before saving it. */
static void post_lb_check(rpod_http_req_t *req, const char *form)
{
    char token[160];
    if (!rpod_http_param(form, "token", token, sizeof(token)) || token[0] == '\0' ||
        strspn(token, "0123456789abcdefABCDEF-_ghijklmnopqrstuvwxyzGHIJKLMNOPQRSTUVWXYZ") != strlen(token)) {
        rpod_http_send_error(req, 400, "that isn't a ListenBrainz token");
        return;
    }
    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        rpod_http_send_error(req, 500, "out of memory");
        return;
    }
    char auth[200];
    snprintf(auth, sizeof(auth), "Authorization: Token %s", token);
    struct curl_slist *headers = curl_slist_append(NULL, auth);
    grow_buf_t body = { NULL, 0 };
    curl_easy_setopt(curl, CURLOPT_URL, "https://api.listenbrainz.org/1/validate-token");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK || body.s == NULL) {
        free(body.s);
        rpod_http_send_error(req, 502, "couldn't reach ListenBrainz -- is rPod online?");
        return;
    }
    char user[128] = "";
    bool valid = status == 200 &&
                 (strstr(body.s, "\"valid\": true") != NULL || strstr(body.s, "\"valid\":true") != NULL);
    if (valid) {
        json_find_str(body.s, "user_name", user, sizeof(user));
    }
    free(body.s);

    rpod_json_t j;
    rpod_json_init(&j);
    rpod_json_obj(&j);
    rpod_json_kbool(&j, "valid", valid);
    rpod_json_kstr(&j, "user", user);
    rpod_json_end_obj(&j);
    rpod_http_send_json(req, 200, &j);
    rpod_json_free(&j);
}

/* --- About ------------------------------------------------------------------------ */

static bool read_line(const char *path, char *out, size_t out_size)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return false;
    }
    bool ok = fgets(out, (int)out_size, f) != NULL;
    fclose(f);
    if (ok) {
        out[strcspn(out, "\n")] = '\0';
    }
    return ok;
}

static long long meminfo(const char *field)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (f == NULL) {
        return -1;
    }
    char line[128];
    long long kb = -1;
    size_t n = strlen(field);
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, field, n) == 0 && line[n] == ':') {
            kb = strtoll(line + n + 1, NULL, 10);
            break;
        }
    }
    fclose(f);
    return kb < 0 ? -1 : kb * 1024;
}

static void json_addresses(rpod_json_t *j)
{
    rpod_json_key(j, "addresses");
    rpod_json_arr(j);
    struct ifaddrs *ifs;
    if (getifaddrs(&ifs) == 0) {
        for (struct ifaddrs *ifa = ifs; ifa != NULL; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == NULL || strcmp(ifa->ifa_name, "lo") == 0) {
                continue;
            }
            char addr[INET6_ADDRSTRLEN];
            if (ifa->ifa_addr->sa_family == AF_INET) {
                inet_ntop(AF_INET, &((struct sockaddr_in *)(void *)ifa->ifa_addr)->sin_addr, addr, sizeof(addr));
            } else if (ifa->ifa_addr->sa_family == AF_INET6) {
                const struct in6_addr *a6 = &((struct sockaddr_in6 *)(void *)ifa->ifa_addr)->sin6_addr;
                if (IN6_IS_ADDR_LINKLOCAL(a6)) {
                    continue;
                }
                inet_ntop(AF_INET6, a6, addr, sizeof(addr));
            } else {
                continue;
            }
            rpod_json_obj(j);
            rpod_json_kstr(j, "interface", ifa->ifa_name);
            rpod_json_kstr(j, "address", addr);
            rpod_json_end_obj(j);
        }
        freeifaddrs(ifs);
    }
    rpod_json_end_arr(j);
}

static void get_about(const rpod_web_config_t *cfg, rpod_http_req_t *req)
{
    rpod_json_t j;
    rpod_json_init(&j);
    rpod_json_obj(&j);
    rpod_json_kstr(&j, "version", RPOD_VERSION);
    rpod_json_kstr(&j, "hostname", cfg->hostname);

    char line[256];
    /* The device tree's model ends in a NUL rather than a newline. */
    rpod_json_kstr(&j, "model", read_line("/proc/device-tree/model", line, sizeof(line)) ? line : "");
    struct utsname u;
    rpod_json_kstr(&j, "kernel", uname(&u) == 0 ? u.release : "");

    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        rpod_json_kint(&j, "uptime", si.uptime);
        rpod_json_key(&j, "load");
        rpod_json_arr(&j);
        for (int i = 0; i < 3; i++) {
            rpod_json_num(&j, (double)si.loads[i] / (double)(1 << SI_LOAD_SHIFT));
        }
        rpod_json_end_arr(&j);
    }
    rpod_json_kint(&j, "mem_total", meminfo("MemTotal"));
    rpod_json_kint(&j, "mem_available", meminfo("MemAvailable"));
    rpod_json_key(&j, "temperature");
    if (read_line("/sys/class/thermal/thermal_zone0/temp", line, sizeof(line))) {
        rpod_json_num(&j, strtod(line, NULL) / 1000.0);
    } else {
        rpod_json_null(&j);
    }
    json_addresses(&j);
    rpod_json_kstr(&j, "music_dir", cfg->music_dir);
    json_storage(&j, "music", cfg->music_dir);
    json_storage(&j, "system", "/");

    char *ui = rpod_ctl_request(cfg->control_sock, "get");
    rpod_json_kbool(&j, "ui", rpod_ctl_ok(ui));
    json_battery(&j, rpod_ctl_ok(ui) ? ui : NULL);
    free(ui);

    rpod_mpd_t *mpd = mpd_open(cfg, NULL);
    rpod_json_key(&j, "mpd");
    rpod_mpd_stats_t s;
    if (mpd != NULL && rpod_mpd_get_stats(mpd, &s)) {
        char version[32];
        rpod_mpd_server_version(mpd, version, sizeof(version));
        rpod_json_obj(&j);
        rpod_json_kstr(&j, "version", version);
        rpod_json_kint(&j, "songs", s.songs);
        rpod_json_kint(&j, "albums", s.albums);
        rpod_json_kint(&j, "artists", s.artists);
        rpod_json_kint(&j, "playtime", (long long)s.db_playtime_s);
        rpod_json_kint(&j, "db_update", (long long)s.db_update);
        rpod_json_kint(&j, "uptime", (long long)s.uptime_s);
        rpod_json_end_obj(&j);
    } else {
        rpod_json_null(&j);
    }
    rpod_mpd_disconnect(mpd);
    rpod_json_end_obj(&j);
    rpod_http_send_json(req, 200, &j);
    rpod_json_free(&j);
}

/* --- Dispatch -------------------------------------------------------------------- */

/* Static files: a plain relative path of [A-Za-z0-9._-] components, none
 * hidden -- nothing to escape the root with. */
static bool static_path_ok(const char *rel)
{
    if (rel[0] == '\0' || strlen(rel) > 200) {
        return false;
    }
    bool comp_start = true;
    for (const char *p = rel; *p != '\0'; p++) {
        char c = *p;
        if (c == '/') {
            if (comp_start) {
                return false;
            }
            comp_start = true;
            continue;
        }
        if (comp_start && c == '.') {
            return false;
        }
        comp_start = false;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
              c == '_' || c == '-')) {
            return false;
        }
    }
    return !comp_start;
}

static bool authorized(const rpod_web_config_t *cfg, const rpod_http_req_t *req)
{
    if (cfg->password == NULL || cfg->password[0] == '\0') {
        return true;
    }
    if (strncasecmp(req->authorization, "Basic ", 6) != 0) {
        return false;
    }
    char decoded[512];
    if (!rpod_http_base64_decode(req->authorization + 6, decoded, sizeof(decoded))) {
        return false;
    }
    const char *colon = strchr(decoded, ':');
    if (colon == NULL) {
        return false;
    }
    /* Compared in constant time for a given length. */
    const char *given = colon + 1;
    size_t a = strlen(given), b = strlen(cfg->password);
    unsigned diff = a != b;
    for (size_t i = 0; i < a; i++) {
        diff |= (unsigned char)given[i] ^ (unsigned char)cfg->password[i % b];
    }
    return diff == 0;
}

/* An Origin, when there is one, has to be this same host. */
static bool same_origin(const rpod_http_req_t *req)
{
    if (req->origin[0] == '\0') {
        return true;
    }
    const char *o = strstr(req->origin, "://");
    return o != NULL && strcasecmp(o + 3, req->host) == 0;
}

static void handle_api(const rpod_web_config_t *cfg, rpod_http_req_t *req)
{
    const char *p = req->path + 4; /* past "/api" */
    bool get = strcmp(req->method, "GET") == 0 || strcmp(req->method, "HEAD") == 0;
    bool post = strcmp(req->method, "POST") == 0;
    bool put = strcmp(req->method, "PUT") == 0;

    if (!get) {
        if (!req->x_rpod || !same_origin(req)) {
            rpod_http_send_error(req, 403, "not from rPod's own page");
            return;
        }
    }

    if (get) {
        if (strcmp(p, "/state") == 0) {
            get_state(cfg, req);
        } else if (strcmp(p, "/cover") == 0) {
            get_cover(cfg, req);
        } else if (strcmp(p, "/library") == 0) {
            get_library(cfg, req);
        } else if (strcmp(p, "/library/stats") == 0) {
            get_library_stats(cfg, req);
        } else if (strcmp(p, "/settings") == 0) {
            get_settings(cfg, req);
        } else if (strcmp(p, "/about") == 0) {
            get_about(cfg, req);
        } else if (strcmp(p, "/videos") == 0) {
            get_videos(cfg, req);
        } else if (strcmp(p, "/videos/poster") == 0) {
            get_video_file(cfg, req, true);
        } else if (strcmp(p, "/videos/file") == 0) {
            get_video_file(cfg, req, false);
        } else {
            rpod_http_send_error(req, 404, "no such API");
        }
        return;
    }
    if (put) {
        if (strcmp(p, "/library/upload") == 0) {
            put_upload(cfg, req);
        } else if (strcmp(p, "/videos/upload") == 0) {
            put_video_upload(cfg, req);
        } else {
            rpod_http_send_error(req, 405, "method not allowed");
        }
        return;
    }
    if (!post) {
        rpod_http_send_error(req, 405, "method not allowed");
        return;
    }

    char *form = rpod_http_read_small_body(req, MAX_FORM_BODY);
    if (form == NULL) {
        rpod_http_send_error(req, 413, "request body too large");
        return;
    }
    if (strcmp(p, "/player") == 0) {
        post_player(cfg, req, form);
    } else if (strcmp(p, "/library/mkdir") == 0) {
        post_mkdir(cfg, req, form);
    } else if (strcmp(p, "/library/delete") == 0) {
        post_delete(cfg, req, form);
    } else if (strcmp(p, "/library/rename") == 0) {
        post_rename(cfg, req, form);
    } else if (strcmp(p, "/library/update") == 0) {
        post_update(cfg, req, form);
    } else if (strcmp(p, "/library/play") == 0) {
        post_play(cfg, req, form);
    } else if (strcmp(p, "/settings") == 0) {
        post_settings(cfg, req, form);
    } else if (strcmp(p, "/settings/wheel/reset") == 0) {
        post_wheel_reset(cfg, req);
    } else if (strcmp(p, "/listenbrainz/check") == 0) {
        post_lb_check(req, form);
    } else if (strcmp(p, "/videos/delete") == 0) {
        post_video_delete(cfg, req, form);
    } else if (strcmp(p, "/videos/rename") == 0) {
        post_video_rename(cfg, req, form);
    } else if (strcmp(p, "/videos/cancel") == 0) {
        post_video_cancel(req, form);
    } else if (strcmp(p, "/videos/clear") == 0) {
        rpod_videos_clear_finished();
        send_ok(req);
    } else if (strcmp(p, "/videos/play") == 0) {
        post_video_play(cfg, req, form);
    } else {
        rpod_http_send_error(req, 404, "no such API");
    }
    free(form);
}

void rpod_web_handle(const rpod_web_config_t *cfg, rpod_http_req_t *req)
{
    if (!rpod_http_host_ok(req->host, cfg->hostname, cfg->extra_hosts)) {
        rpod_http_send_error(req, 403, "unknown host name -- add it to RPOD_WEB_HOSTS");
        return;
    }
    if (!authorized(cfg, req)) {
        rpod_http_send(req, 401, "text/plain; charset=utf-8", "rPod needs its password.\n", 25,
                       "WWW-Authenticate: Basic realm=\"rPod\", charset=\"UTF-8\"\r\n");
        return;
    }
    if (strcmp(req->path, "/api") == 0 || strncmp(req->path, "/api/", 5) == 0) {
        handle_api(cfg, req);
        return;
    }
    if (strcmp(req->method, "GET") != 0 && strcmp(req->method, "HEAD") != 0) {
        rpod_http_send_error(req, 405, "method not allowed");
        return;
    }
    /* Every page is the one app (its screens live in the URL's #fragment). */
    if (strcmp(req->path, "/") == 0) {
        rpod_http_send_file(req, cfg->web_root, "index.html");
        return;
    }
    const char *rel = req->path + 1;
    if (strncmp(rel, "ipod/", 5) == 0) {
        if (cfg->ipod_root == NULL || !static_path_ok(rel + 5)) {
            rpod_http_send_error(req, 404, "not found");
            return;
        }
        rpod_http_send_file(req, cfg->ipod_root, rel + 5);
        return;
    }
    if (!static_path_ok(rel)) {
        rpod_http_send_error(req, 404, "not found");
        return;
    }
    rpod_http_send_file(req, cfg->web_root, rel);
}
