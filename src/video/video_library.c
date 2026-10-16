#include "video_library.h"

#include "mp4_info.h"
#include "ui/alpha_sort.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* What FFmpeg (the picture) and MPD's ffmpeg plugin (the sound) both read. */
static const char *k_exts[] = { "mp4", "m4v", "mov", "mkv", "webm", "avi", "mpg", "mpeg", "ts" };

bool rpod_video_name_ok(const char *name)
{
    if (name[0] == '.') {
        return false;
    }
    const char *dot = strrchr(name, '.');
    if (dot == NULL || dot == name) {
        return false;
    }
    for (size_t i = 0; i < sizeof(k_exts) / sizeof(k_exts[0]); i++) {
        if (strcasecmp(dot + 1, k_exts[i]) == 0) {
            return true;
        }
    }
    return false;
}

static int by_title(const void *a, const void *b)
{
    return rpod_alpha_compare(((const rpod_video_entry_t *)a)->title, ((const rpod_video_entry_t *)b)->title);
}

bool rpod_video_list(const char *dir, rpod_video_entry_t **out, size_t *count)
{
    *out = NULL;
    *count = 0;
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    DIR *d = dfd >= 0 ? fdopendir(dfd) : NULL;
    if (d == NULL) {
        if (dfd >= 0) {
            close(dfd);
        }
        return false;
    }
    rpod_video_entry_t *list = NULL;
    size_t n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        struct stat st;
        if (!rpod_video_name_ok(e->d_name) || fstatat(dfd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISREG(st.st_mode) || strlen(e->d_name) >= sizeof(list->name)) {
            continue;
        }
        if (n == cap) {
            size_t ncap = cap == 0 ? 16 : cap * 2;
            rpod_video_entry_t *grown = realloc(list, ncap * sizeof(*list));
            if (grown == NULL) {
                break;
            }
            list = grown;
            cap = ncap;
        }
        rpod_video_entry_t *v = &list[n++];
        memset(v, 0, sizeof(*v));
        snprintf(v->name, sizeof(v->name), "%s", e->d_name);
        snprintf(v->title, sizeof(v->title), "%s", e->d_name);
        *strrchr(v->title, '.') = '\0';
        v->size = (long long)st.st_size;
        v->mtime = st.st_mtime;
        int fd = openat(dfd, e->d_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd >= 0) {
            rpod_mp4_info_t info;
            if (rpod_mp4_info_read(fd, &info)) {
                v->duration_ms = info.duration_ms;
                v->width = info.width;
                v->height = info.height;
            }
            close(fd);
        }
    }
    closedir(d);
    if (n > 1) {
        qsort(list, n, sizeof(*list), by_title);
    }
    *out = list;
    *count = n;
    return true;
}

void rpod_video_poster_path(const char *dir, const char *name, char *out, size_t size)
{
    const char *dot = strrchr(name, '.');
    int stem = dot != NULL && dot != name ? (int)(dot - name) : (int)strlen(name);
    snprintf(out, size, "%s/" RPOD_VIDEO_POSTER_DIR "/%.*s.jpg", dir, stem, name);
}
