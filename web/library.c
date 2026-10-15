#include "library.h"

#include "json.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* Deeper than any real library; a bound for the recursive delete. */
#define MAX_DEPTH 32

bool rpod_lib_name_ok(const char *name)
{
    size_t n = strlen(name);
    if (n == 0 || n > 255 || name[0] == '.' || name[n - 1] == ' ' || name[n - 1] == '.') {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)name; *p != '\0'; p++) {
        if (*p < 0x20 || *p == 0x7f || strchr("/\\\"*:<>?|", *p) != NULL) {
            return false;
        }
    }
    return rpod_utf8_valid(name);
}

bool rpod_lib_path_ok(const char *path, bool allow_root)
{
    if (path[0] == '\0') {
        return allow_root;
    }
    if (strlen(path) > 1024) {
        return false;
    }
    char comp[256];
    for (const char *p = path;;) {
        size_t n = strcspn(p, "/");
        if (n == 0 || n >= sizeof(comp)) {
            return false; /* an empty component: a leading, trailing or double slash */
        }
        memcpy(comp, p, n);
        comp[n] = '\0';
        if (!rpod_lib_name_ok(comp)) {
            return false;
        }
        if (p[n] == '\0') {
            return true;
        }
        p += n + 1;
    }
}

bool rpod_lib_upload_name_ok(const char *name)
{
    static const char *exts[] = {
        /* What MPD's decoders play (docs/PLAN.md §6.1). */
        "flac", "mp3", "m4a", "mp4", "aac", "alac", "ogg", "oga", "opus", "wav", "wv",
        "ape", "aif", "aiff", "aifc", "mpc", "dsf", "dff",
        /* Beside them in an album's folder. */
        "jpg", "jpeg", "png", "cue", "lrc",
    };
    if (!rpod_lib_name_ok(name)) {
        return false;
    }
    const char *dot = strrchr(name, '.');
    if (dot == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        if (strcasecmp(dot + 1, exts[i]) == 0) {
            return true;
        }
    }
    return false;
}

int rpod_lib_open_dir(const char *root, const char *path, bool create)
{
    if (!rpod_lib_path_ok(path, true)) {
        return -EINVAL;
    }
    int fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        return -errno;
    }
    char comp[256];
    for (const char *p = path; *p != '\0';) {
        size_t n = strcspn(p, "/");
        memcpy(comp, p, n);
        comp[n] = '\0';
        int next = openat(fd, comp, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0 && errno == ENOENT && create) {
            if (mkdirat(fd, comp, 0755) != 0 && errno != EEXIST) {
                int err = errno;
                close(fd);
                return -err;
            }
            next = openat(fd, comp, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        }
        int err = errno;
        close(fd);
        if (next < 0) {
            /* O_NOFOLLOW on a symlink fails with ELOOP, or ENOTDIR with
             * O_DIRECTORY: either way, not a directory we'll go into. */
            return -(err == ELOOP ? ENOTDIR : err);
        }
        fd = next;
        p += n;
        if (*p == '/') {
            p++;
        }
    }
    return fd;
}

/* Splits "a/b/c" into its parent "a/b" (or "") and last component "c". */
static void split(const char *path, char *parent, size_t parent_size, const char **base)
{
    const char *slash = strrchr(path, '/');
    if (slash == NULL) {
        parent[0] = '\0';
        *base = path;
        return;
    }
    size_t n = (size_t)(slash - path);
    if (n >= parent_size) {
        n = parent_size - 1;
    }
    memcpy(parent, path, n);
    parent[n] = '\0';
    *base = slash + 1;
}

/* Deletes `name` in dir_fd, recursing into a directory -- without ever
 * following a symlink, which is deleted as itself. */
static int remove_at(int dir_fd, const char *name, int depth)
{
    struct stat st;
    if (fstatat(dir_fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        return -errno;
    }
    if (!S_ISDIR(st.st_mode)) {
        return unlinkat(dir_fd, name, 0) == 0 ? 0 : -errno;
    }
    if (depth >= MAX_DEPTH) {
        return -ELOOP;
    }
    int fd = openat(dir_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return -errno;
    }
    DIR *d = fdopendir(fd);
    if (d == NULL) {
        int err = errno;
        close(fd);
        return -err;
    }
    int rc = 0;
    struct dirent *e;
    while (rc == 0 && (e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        rc = remove_at(fd, e->d_name, depth + 1);
    }
    closedir(d);
    if (rc != 0) {
        return rc;
    }
    return unlinkat(dir_fd, name, AT_REMOVEDIR) == 0 ? 0 : -errno;
}

int rpod_lib_delete(const char *root, const char *path)
{
    if (!rpod_lib_path_ok(path, false)) {
        return -EINVAL;
    }
    char parent[1100];
    const char *base;
    split(path, parent, sizeof(parent), &base);
    int dir_fd = rpod_lib_open_dir(root, parent, false);
    if (dir_fd < 0) {
        return dir_fd;
    }
    int rc = remove_at(dir_fd, base, 0);
    close(dir_fd);
    return rc;
}

/* renameat2(RENAME_NOREPLACE), or where the filesystem doesn't take the
 * flag, a check first -- racy, but only against another upload of the
 * same name. */
static int rename_noreplace(int dir_fd, const char *from, const char *to)
{
    if (renameat2(dir_fd, from, dir_fd, to, RENAME_NOREPLACE) == 0) {
        return 0;
    }
    if (errno != EINVAL && errno != ENOSYS) {
        return -errno;
    }
    struct stat st;
    if (fstatat(dir_fd, to, &st, AT_SYMLINK_NOFOLLOW) == 0) {
        return -EEXIST;
    }
    return renameat(dir_fd, from, dir_fd, to) == 0 ? 0 : -errno;
}

int rpod_lib_rename(const char *root, const char *path, const char *new_name)
{
    if (!rpod_lib_path_ok(path, false) || !rpod_lib_name_ok(new_name)) {
        return -EINVAL;
    }
    char parent[1100];
    const char *base;
    split(path, parent, sizeof(parent), &base);
    int dir_fd = rpod_lib_open_dir(root, parent, false);
    if (dir_fd < 0) {
        return dir_fd;
    }
    int rc = strcmp(base, new_name) == 0 ? 0 : rename_noreplace(dir_fd, base, new_name);
    close(dir_fd);
    return rc;
}

int rpod_lib_upload_begin(const char *root, const char *dir, int *dir_fd, char *tmp_name, size_t tmp_size)
{
    static atomic_uint counter;
    int dfd = rpod_lib_open_dir(root, dir, true);
    if (dfd < 0) {
        return dfd;
    }
    /* Hidden, and a suffix no decoder claims: MPD skips it either way. */
    snprintf(tmp_name, tmp_size, ".rpod-upload-%d-%u.part", (int)getpid(), atomic_fetch_add(&counter, 1));
    int fd = openat(dfd, tmp_name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) {
        int err = errno;
        close(dfd);
        return -err;
    }
    *dir_fd = dfd;
    return fd;
}

int rpod_lib_upload_finish(int dir_fd, const char *tmp_name, const char *name, bool overwrite, bool *replaced)
{
    *replaced = false;
    int rc = rename_noreplace(dir_fd, tmp_name, name);
    if (rc == -EEXIST && overwrite) {
        /* Only ever over a plain file: never a directory or a symlink. */
        struct stat st;
        if (fstatat(dir_fd, name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(st.st_mode)) {
            rc = renameat(dir_fd, tmp_name, dir_fd, name) == 0 ? 0 : -errno;
            *replaced = rc == 0;
        }
    }
    if (rc != 0) {
        unlinkat(dir_fd, tmp_name, 0);
    }
    return rc;
}
