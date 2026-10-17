#include "videos.h"

#include "library.h"
#include "video/video_library.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* What uploads may be: what FFmpeg reads that cameras, phones and the
 * internet make. */
static const char *k_upload_exts[] = {
    "mp4", "m4v", "mov", "mkv", "webm", "avi", "wmv", "flv", "mpg", "mpeg", "m2v", "ts", "mts", "m2ts",
    "3gp", "3g2", "ogv", "vob", "divx", "asf",
};

/* Finished jobs are forgotten after this long: long enough for the page to
 * say how it went. */
#define DONE_KEEP_S 120
#define FAILED_KEEP_S 1800
#define MAX_JOBS 64

#define SOURCE_PREFIX ".rpod-source-"
#define JOB_SUFFIX ".job"

typedef enum { JOB_WAITING, JOB_CONVERTING, JOB_DONE, JOB_FAILED } job_state_t;

typedef struct job {
    unsigned id;
    char source[64];  /* the upload, hidden in the folder */
    char target[256]; /* what it becomes */
    bool overwrite;
    job_state_t state;
    double duration_s, done_s;
    double started, finished; /* now_s() */
    char error[160];
    pid_t pid;
    bool cancelled;
    struct job *next;
} job_t;

static struct {
    char dir[PATH_MAX];
    char convert[PATH_MAX];
    pthread_mutex_t lock;
    pthread_cond_t cond;
    job_t *jobs; /* in the order they came */
    unsigned next_id;
} g = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
    .next_id = 1,
};

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

bool rpod_videos_upload_name_ok(const char *name)
{
    if (!rpod_lib_name_ok(name)) {
        return false;
    }
    const char *dot = strrchr(name, '.');
    if (dot == NULL || dot == name) {
        return false;
    }
    for (size_t i = 0; i < sizeof(k_upload_exts) / sizeof(k_upload_exts[0]); i++) {
        if (strcasecmp(dot + 1, k_upload_exts[i]) == 0) {
            return true;
        }
    }
    return false;
}

void rpod_videos_target(const char *name, char *out, size_t size)
{
    const char *dot = strrchr(name, '.');
    int stem = dot != NULL && dot != name ? (int)(dot - name) : (int)strlen(name);
    /* Room for ".mp4" within a name's 255 bytes. */
    if (stem > 251) {
        stem = 251;
    }
    snprintf(out, size, "%.*s.mp4", stem, name);
}

static void path_of(const char *name, char *out, size_t size)
{
    snprintf(out, size, "%s/%s", g.dir, name);
}

/* --- Jobs on disk ---------------------------------------------------------------- */

static bool write_job_file(const job_t *j)
{
    char path[PATH_MAX + 128], tmp[PATH_MAX + 136];
    snprintf(path, sizeof(path), "%s/%s" JOB_SUFFIX, g.dir, j->source);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (f == NULL) {
        return false;
    }
    fprintf(f, "%s\n%d\n", j->target, j->overwrite ? 1 : 0);
    bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return false;
    }
    return true;
}

/* The upload, its job file and whatever its conversion had written. */
static void remove_job_files(const job_t *j)
{
    char path[PATH_MAX + 128];
    path_of(j->source, path, sizeof(path));
    unlink(path);
    snprintf(path, sizeof(path), "%s/%s" JOB_SUFFIX, g.dir, j->source);
    unlink(path);
    snprintf(path, sizeof(path), "%s/.rpod-convert-%u.mp4", g.dir, j->id);
    unlink(path);
    snprintf(path, sizeof(path), "%s/" RPOD_VIDEO_POSTER_DIR "/.rpod-poster-%u.jpg", g.dir, j->id);
    unlink(path);
}

static job_t *add_job(const char *source, const char *target, bool overwrite)
{
    job_t *j = calloc(1, sizeof(*j));
    if (j == NULL) {
        return NULL;
    }
    snprintf(j->source, sizeof(j->source), "%s", source);
    snprintf(j->target, sizeof(j->target), "%s", target);
    j->overwrite = overwrite;
    j->state = JOB_WAITING;
    j->id = g.next_id++;
    j->pid = -1;
    job_t **tail = &g.jobs;
    while (*tail != NULL) {
        tail = &(*tail)->next;
    }
    *tail = j;
    return j;
}

/* What a restart left: jobs to take up again, and the leftovers of
 * conversions and uploads it cut short. */
static void recover(void)
{
    DIR *d = opendir(g.dir);
    if (d == NULL) {
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *n = e->d_name;
        size_t len = strlen(n);
        char path[PATH_MAX + 300];
        if (strncmp(n, ".rpod-upload-", 13) == 0 || strncmp(n, ".rpod-convert-", 14) == 0 ||
            (len > 4 && strcmp(n + len - 4, ".tmp") == 0 && strncmp(n, SOURCE_PREFIX, strlen(SOURCE_PREFIX)) == 0)) {
            path_of(n, path, sizeof(path));
            unlink(path);
            continue;
        }
        if (strncmp(n, SOURCE_PREFIX, strlen(SOURCE_PREFIX)) != 0 || len <= strlen(JOB_SUFFIX) ||
            strcmp(n + len - strlen(JOB_SUFFIX), JOB_SUFFIX) != 0) {
            continue;
        }
        char source[64];
        snprintf(source, sizeof(source), "%.*s", (int)(len - strlen(JOB_SUFFIX)), n);
        char target[300] = "";
        int overwrite = 0;
        path_of(n, path, sizeof(path));
        FILE *f = fopen(path, "r");
        if (f != NULL) {
            if (fgets(target, sizeof(target), f) != NULL) {
                target[strcspn(target, "\n")] = '\0';
            }
            if (fscanf(f, "%d", &overwrite) != 1) {
                overwrite = 0;
            }
            fclose(f);
        }
        char src_path[PATH_MAX + 64];
        path_of(source, src_path, sizeof(src_path));
        struct stat st;
        if (target[0] == '\0' || !rpod_lib_name_ok(target) || stat(src_path, &st) != 0) {
            unlink(path);
            unlink(src_path);
            continue;
        }
        if (add_job(source, target, overwrite != 0) != NULL) {
            fprintf(stderr, "rpod-web: taking up the conversion of %s again\n", target);
        }
    }
    closedir(d);
    /* Upload names from a crashed run can't come round again (they're by
     * pid), but a stray source without its job file is no use. */
    d = opendir(g.dir);
    while (d != NULL && (e = readdir(d)) != NULL) {
        const char *n = e->d_name;
        if (strncmp(n, SOURCE_PREFIX, strlen(SOURCE_PREFIX)) != 0 || strstr(n, JOB_SUFFIX) != NULL) {
            continue;
        }
        bool known = false;
        for (job_t *j = g.jobs; j != NULL; j = j->next) {
            known = known || strcmp(j->source, n) == 0;
        }
        if (!known) {
            char path[PATH_MAX + 300];
            path_of(n, path, sizeof(path));
            unlink(path);
        }
    }
    if (d != NULL) {
        closedir(d);
    }
    char posters[PATH_MAX + 32];
    snprintf(posters, sizeof(posters), "%s/" RPOD_VIDEO_POSTER_DIR, g.dir);
    d = opendir(posters);
    while (d != NULL && (e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, ".rpod-poster-", 13) == 0) {
            char path[PATH_MAX + 300];
            snprintf(path, sizeof(path), "%s/%s", posters, e->d_name);
            unlink(path);
        }
    }
    if (d != NULL) {
        closedir(d);
    }
}

/* --- Converting ------------------------------------------------------------------ */

/* In the child, before rpod-convert: only what nothing else wants. */
static void become_idle(void)
{
    struct sched_param sp = { 0 };
    sched_setscheduler(0, SCHED_IDLE, &sp);
    setpriority(PRIO_PROCESS, 0, 19);
    /* IOPRIO_WHO_PROCESS, IOPRIO_CLASS_IDLE: no ioprio_set() in libc. */
    syscall(SYS_ioprio_set, 1, 0, 3 << 13);
    /* If a big source runs the Pi out of memory, this goes, not rPod. */
    int fd = open("/proc/self/oom_score_adj", O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        if (write(fd, "1000", 4) != 4) {
            /* not worth failing the conversion over */
        }
        close(fd);
    }
}

/* Reads rpod-convert's progress lines and its last error until it's done. */
static void follow(job_t *j, int out_fd, int err_fd)
{
    char out[512], err[2048];
    size_t out_len = 0, err_len = 0;
    struct pollfd fds[2] = { { .fd = out_fd, .events = POLLIN }, { .fd = err_fd, .events = POLLIN } };
    while (fds[0].fd >= 0 || fds[1].fd >= 0) {
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        for (int i = 0; i < 2; i++) {
            if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) {
                continue;
            }
            char *buf = i == 0 ? out : err;
            size_t *len = i == 0 ? &out_len : &err_len;
            size_t cap = i == 0 ? sizeof(out) : sizeof(err);
            ssize_t n = read(fds[i].fd, buf + *len, cap - 1 - *len);
            if (n <= 0) {
                close(fds[i].fd);
                fds[i].fd = -1;
                continue;
            }
            *len += (size_t)n;
            buf[*len] = '\0';
            char *nl;
            while ((nl = strchr(buf, '\n')) != NULL) {
                *nl = '\0';
                double v;
                pthread_mutex_lock(&g.lock);
                if (i == 0 && sscanf(buf, "duration %lf", &v) == 1) {
                    j->duration_s = v;
                } else if (i == 0 && sscanf(buf, "time %lf", &v) == 1) {
                    j->done_s = v;
                } else if (i == 1 && strncmp(buf, "error: ", 7) == 0) {
                    snprintf(j->error, sizeof(j->error), "%s", buf + 7);
                } else if (i == 1) {
                    fprintf(stderr, "%s\n", buf); /* its own notes, to the journal */
                }
                pthread_mutex_unlock(&g.lock);
                size_t rest = *len - (size_t)(nl + 1 - buf);
                memmove(buf, nl + 1, rest + 1);
                *len = rest;
            }
            if (*len == cap - 1) {
                *len = 0; /* a line too long to be one of ours */
            }
        }
    }
}

/* fsync()s a file by name, so a video is all there before it's named. */
static bool sync_file(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    bool ok = fsync(fd) == 0;
    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    close(fd);
    return ok;
}

/* Puts a finished conversion in place: the video by its name (or the next
 * free "<name> 2"... if one's appeared meanwhile and it isn't to replace
 * it), then its poster. False if neither could be. */
static bool install(job_t *j, const char *poster_tmp, char *final, size_t final_size)
{
    int dfd = open(g.dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) {
        return false;
    }
    char tmp_name[64];
    snprintf(tmp_name, sizeof(tmp_name), ".rpod-convert-%u.mp4", j->id);
    char name[256];
    snprintf(name, sizeof(name), "%s", j->target);
    int rc;
    if (j->overwrite) {
        bool replaced;
        rc = rpod_lib_upload_finish(dfd, tmp_name, name, true, &replaced);
    } else {
        rc = rpod_lib_rename_noreplace(dfd, tmp_name, name);
        char stem[256];
        snprintf(stem, sizeof(stem), "%.*s", (int)(strlen(j->target) - 4), j->target);
        for (int n = 2; rc == -EEXIST && n < 100; n++) {
            snprintf(name, sizeof(name), "%.*s %d.mp4", 240, stem, n);
            rc = rpod_lib_rename_noreplace(dfd, tmp_name, name);
        }
    }
    fsync(dfd);
    close(dfd);
    if (rc != 0) {
        return false;
    }
    snprintf(final, final_size, "%s", name);
    char poster[PATH_MAX + 300];
    rpod_video_poster_path(g.dir, name, poster, sizeof(poster));
    if (rename(poster_tmp, poster) != 0) {
        unlink(poster_tmp);
    }
    return true;
}

static void run(job_t *j)
{
    char src[PATH_MAX + 64], out[PATH_MAX + 64], posters[PATH_MAX + 32], poster_tmp[PATH_MAX + 64];
    path_of(j->source, src, sizeof(src));
    snprintf(out, sizeof(out), "%s/.rpod-convert-%u.mp4", g.dir, j->id);
    snprintf(posters, sizeof(posters), "%s/" RPOD_VIDEO_POSTER_DIR, g.dir);
    mkdir(posters, 0755);
    snprintf(poster_tmp, sizeof(poster_tmp), "%s/.rpod-poster-%u.jpg", posters, j->id);
    char title[256];
    snprintf(title, sizeof(title), "%.*s", (int)(strlen(j->target) - 4), j->target);

    int out_pipe[2], err_pipe[2];
    if (pipe2(out_pipe, O_CLOEXEC) != 0 || pipe2(err_pipe, O_CLOEXEC) != 0) {
        snprintf(j->error, sizeof(j->error), "couldn't start the conversion");
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        become_idle();
        execl(g.convert, "rpod-convert", "--title", title, "--poster", poster_tmp, src, out, (char *)NULL);
        fprintf(stderr, "error: rpod-convert isn't installed (%s)\n", g.convert);
        _exit(127);
    }
    close(out_pipe[1]);
    close(err_pipe[1]);
    if (pid < 0) {
        close(out_pipe[0]);
        close(err_pipe[0]);
        snprintf(j->error, sizeof(j->error), "couldn't start the conversion");
        return;
    }
    pthread_mutex_lock(&g.lock);
    j->pid = pid;
    bool cancelled = j->cancelled;
    pthread_mutex_unlock(&g.lock);
    if (cancelled) {
        kill(pid, SIGTERM);
    }
    follow(j, out_pipe[0], err_pipe[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    pthread_mutex_lock(&g.lock);
    j->pid = -1;
    cancelled = j->cancelled;
    pthread_mutex_unlock(&g.lock);

    bool ok = !cancelled && WIFEXITED(status) && WEXITSTATUS(status) == 0 && sync_file(out);
    char final[256] = "";
    if (ok) {
        ok = install(j, poster_tmp, final, sizeof(final));
        if (!ok) {
            snprintf(j->error, sizeof(j->error), "couldn't put the video in place");
        }
    } else if (!cancelled && j->error[0] == '\0') {
        if (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL) {
            snprintf(j->error, sizeof(j->error), "it ran out of memory -- too big a video for the Pi");
        } else {
            snprintf(j->error, sizeof(j->error), "the conversion failed");
        }
    }
    remove_job_files(j);
    pthread_mutex_lock(&g.lock);
    j->state = ok ? JOB_DONE : JOB_FAILED;
    j->finished = now_s();
    if (ok) {
        snprintf(j->target, sizeof(j->target), "%s", final);
    }
    pthread_mutex_unlock(&g.lock);
    if (ok) {
        fprintf(stderr, "rpod-web: converted %s in %.0f s\n", final, j->finished - j->started);
    } else if (!cancelled) {
        fprintf(stderr, "rpod-web: couldn't convert %s: %s\n", j->target, j->error);
    }
}

/* Drops finished jobs past their time, and cancelled ones. Lock held. */
static void prune(void)
{
    double now = now_s();
    for (job_t **p = &g.jobs; *p != NULL;) {
        job_t *j = *p;
        bool gone = (j->state == JOB_DONE && now - j->finished > DONE_KEEP_S) ||
                    (j->state == JOB_FAILED && (j->cancelled || now - j->finished > FAILED_KEEP_S));
        if (gone) {
            *p = j->next;
            free(j);
        } else {
            p = &j->next;
        }
    }
}

static void *worker(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g.lock);
    for (;;) {
        job_t *j = NULL;
        for (job_t *k = g.jobs; k != NULL && j == NULL; k = k->next) {
            if (k->state == JOB_WAITING) {
                j = k;
            }
        }
        if (j == NULL) {
            pthread_cond_wait(&g.cond, &g.lock);
            continue;
        }
        j->state = JOB_CONVERTING;
        j->started = now_s();
        pthread_mutex_unlock(&g.lock);
        run(j);
        pthread_mutex_lock(&g.lock);
        prune();
    }
    return NULL;
}

/* --- The API's side -------------------------------------------------------------- */

bool rpod_videos_start(const char *dir, const char *convert_bin)
{
    snprintf(g.dir, sizeof(g.dir), "%s", dir);
    snprintf(g.convert, sizeof(g.convert), "%s", convert_bin);
    struct stat st;
    if (stat(g.dir, &st) != 0 && mkdir(g.dir, 0755) != 0) {
        fprintf(stderr, "rpod-web: no videos folder %s: %s\n", g.dir, strerror(errno));
        return false;
    }
    pthread_mutex_lock(&g.lock);
    recover();
    pthread_mutex_unlock(&g.lock);
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    bool ok = pthread_create(&t, &attr, worker, NULL) == 0;
    pthread_attr_destroy(&attr);
    if (access(g.convert, X_OK) != 0) {
        fprintf(stderr, "rpod-web: %s isn't there: video uploads won't convert\n", g.convert);
    }
    return ok;
}

bool rpod_videos_busy(void)
{
    pthread_mutex_lock(&g.lock);
    bool busy = false;
    for (job_t *j = g.jobs; j != NULL; j = j->next) {
        busy = busy || j->state == JOB_WAITING || j->state == JOB_CONVERTING;
    }
    pthread_mutex_unlock(&g.lock);
    return busy;
}

bool rpod_videos_pending(const char *target)
{
    pthread_mutex_lock(&g.lock);
    bool pending = false;
    for (job_t *j = g.jobs; j != NULL; j = j->next) {
        pending = pending || ((j->state == JOB_WAITING || j->state == JOB_CONVERTING) && strcmp(j->target, target) == 0);
    }
    pthread_mutex_unlock(&g.lock);
    return pending;
}

unsigned rpod_videos_queue(const char *tmp_name, const char *target, bool overwrite)
{
    static unsigned seq;
    char from[PATH_MAX + 64], to[PATH_MAX + 64];
    path_of(tmp_name, from, sizeof(from));
    pthread_mutex_lock(&g.lock);
    prune();
    unsigned count = 0;
    for (job_t *j = g.jobs; j != NULL; j = j->next) {
        count++;
    }
    char source[64];
    snprintf(source, sizeof(source), SOURCE_PREFIX "%d-%u", (int)getpid(), seq++);
    path_of(source, to, sizeof(to));
    job_t *j = count < MAX_JOBS && rename(from, to) == 0 ? add_job(source, target, overwrite) : NULL;
    if (j == NULL || !write_job_file(j)) {
        unlink(from);
        unlink(to);
        if (j != NULL) {
            j->state = JOB_FAILED;
            j->cancelled = true;
            prune();
        }
        pthread_mutex_unlock(&g.lock);
        return 0;
    }
    unsigned id = j->id;
    pthread_cond_signal(&g.cond);
    pthread_mutex_unlock(&g.lock);
    return id;
}

bool rpod_videos_cancel(unsigned id)
{
    pthread_mutex_lock(&g.lock);
    job_t *j = g.jobs;
    while (j != NULL && j->id != id) {
        j = j->next;
    }
    bool found = j != NULL && (j->state == JOB_WAITING || j->state == JOB_CONVERTING);
    if (found) {
        j->cancelled = true;
        if (j->state == JOB_WAITING) {
            remove_job_files(j);
            j->state = JOB_FAILED;
            prune();
        } else if (j->pid > 0) {
            kill(j->pid, SIGTERM); /* the worker cleans up after it */
        }
    }
    pthread_mutex_unlock(&g.lock);
    return found;
}

void rpod_videos_clear_finished(void)
{
    pthread_mutex_lock(&g.lock);
    for (job_t *j = g.jobs; j != NULL; j = j->next) {
        if (j->state == JOB_DONE || j->state == JOB_FAILED) {
            j->state = JOB_FAILED;
            j->cancelled = true;
        }
    }
    prune();
    pthread_mutex_unlock(&g.lock);
}

void rpod_videos_json_jobs(rpod_json_t *js)
{
    static const char *names[] = { "waiting", "converting", "done", "failed" };
    pthread_mutex_lock(&g.lock);
    prune();
    double now = now_s();
    rpod_json_arr(js);
    for (job_t *j = g.jobs; j != NULL; j = j->next) {
        rpod_json_obj(js);
        rpod_json_kint(js, "id", j->id);
        rpod_json_kstr(js, "name", j->target);
        rpod_json_kstr(js, "state", names[j->state]);
        double progress = j->state == JOB_DONE ? 1.0 : j->duration_s > 0 ? j->done_s / j->duration_s : 0.0;
        rpod_json_knum(js, "progress", progress > 1.0 ? 1.0 : progress);
        double left = -1;
        double took = now - j->started;
        if (j->state == JOB_CONVERTING && j->done_s > 1.0 && took > 5.0 && j->duration_s > j->done_s) {
            left = (j->duration_s - j->done_s) * took / j->done_s;
        }
        rpod_json_knum(js, "left", left);
        rpod_json_kstr(js, "error", j->error);
        rpod_json_end_obj(js);
    }
    rpod_json_end_arr(js);
    pthread_mutex_unlock(&g.lock);
}
