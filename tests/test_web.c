/*
 * The web daemon's request parsing, JSON output and library path rules
 * (web/http.c, web/json.c, web/library.c) -- the parts that decide what a
 * browser can reach -- plus the library operations against a scratch
 * directory, symlinks pointing out of it included.
 */

#include "http.h"
#include "json.h"
#include "library.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);   \
            failures++;                                                                \
        }                                                                              \
    } while (0)

static void test_url_decode(void)
{
    char out[64];
    CHECK(rpod_http_url_decode("a%20b+c", 7, out, sizeof(out), true) && strcmp(out, "a b c") == 0);
    CHECK(rpod_http_url_decode("a%20b+c", 7, out, sizeof(out), false) && strcmp(out, "a b+c") == 0);
    CHECK(rpod_http_url_decode("%C3%A9", 6, out, sizeof(out), true) && strcmp(out, "\xc3\xa9") == 0);
    CHECK(!rpod_http_url_decode("a%2", 3, out, sizeof(out), true));  /* cut short */
    CHECK(!rpod_http_url_decode("a%", 2, out, sizeof(out), true));
    CHECK(!rpod_http_url_decode("%zz", 3, out, sizeof(out), true));
    CHECK(!rpod_http_url_decode("a%00b", 5, out, sizeof(out), true)); /* no NULs smuggled in */
    CHECK(!rpod_http_url_decode("abcdef", 6, out, 4, true));          /* no room */
    CHECK(rpod_http_url_decode("", 0, out, sizeof(out), true) && out[0] == '\0');
}

static void test_params(void)
{
    char out[64];
    const char *qs = "path=Some%20Album%2FCD%201&name=01+Song.flac&empty=&flag&x=%ZZ";
    CHECK(rpod_http_param(qs, "path", out, sizeof(out)) && strcmp(out, "Some Album/CD 1") == 0);
    CHECK(rpod_http_param(qs, "name", out, sizeof(out)) && strcmp(out, "01 Song.flac") == 0);
    CHECK(rpod_http_param(qs, "empty", out, sizeof(out)) && out[0] == '\0');
    CHECK(rpod_http_param(qs, "flag", out, sizeof(out)) && out[0] == '\0');
    CHECK(!rpod_http_param(qs, "x", out, sizeof(out)));   /* doesn't decode */
    CHECK(!rpod_http_param(qs, "nope", out, sizeof(out)));
    CHECK(!rpod_http_param(qs, "pat", out, sizeof(out)));  /* whole keys only */
    char tiny[4];
    CHECK(!rpod_http_param(qs, "name", tiny, sizeof(tiny))); /* doesn't fit */
    CHECK(rpod_http_param("a=1&a=2", "a", out, sizeof(out)) && strcmp(out, "1") == 0);
    CHECK(!rpod_http_param("", "a", out, sizeof(out)));
}

static void test_hosts(void)
{
    CHECK(rpod_http_host_ok("rpod.local", "rpod", NULL));
    CHECK(rpod_http_host_ok("RPOD.local:80", "rpod", NULL));
    CHECK(rpod_http_host_ok("rpod.local.", "rpod", NULL));
    CHECK(rpod_http_host_ok("rpod", "rpod", NULL));
    CHECK(rpod_http_host_ok("rpod.lan", "rpod", NULL));
    CHECK(rpod_http_host_ok("rpod.home.arpa", "rpod", NULL));
    CHECK(rpod_http_host_ok("192.168.1.113", "rpod", NULL));
    CHECK(rpod_http_host_ok("192.168.1.113:8080", "rpod", NULL));
    CHECK(rpod_http_host_ok("[fe80::1]:80", "rpod", NULL));
    CHECK(rpod_http_host_ok("localhost:8080", "rpod", NULL));
    /* A rebinding attacker's own names, however they're dressed up. */
    CHECK(!rpod_http_host_ok("rpod.attacker.example", "rpod", NULL));
    CHECK(!rpod_http_host_ok("attacker.example", "rpod", NULL));
    CHECK(!rpod_http_host_ok("rpodx.local", "rpod", NULL));
    CHECK(!rpod_http_host_ok("xrpod.local", "rpod", NULL));
    CHECK(!rpod_http_host_ok("1.2.3.4.attacker.example", "rpod", NULL));
    CHECK(!rpod_http_host_ok("", "rpod", NULL));
    CHECK(!rpod_http_host_ok(NULL, "rpod", NULL));
    CHECK(!rpod_http_host_ok("[fe80::1", "rpod", NULL));
    /* Names added by hand. */
    CHECK(rpod_http_host_ok("music.example:80", "rpod", "jukebox, music.example"));
    CHECK(rpod_http_host_ok("jukebox", "rpod", "jukebox, music.example"));
    CHECK(!rpod_http_host_ok("music", "rpod", "jukebox, music.example"));
}

static void test_base64(void)
{
    char out[64];
    CHECK(rpod_http_base64_decode("cnBvZDpzZWNyZXQ=", out, sizeof(out)) && strcmp(out, "rpod:secret") == 0);
    CHECK(rpod_http_base64_decode("OnB3", out, sizeof(out)) && strcmp(out, ":pw") == 0);
    CHECK(rpod_http_base64_decode("", out, sizeof(out)) && out[0] == '\0');
    CHECK(!rpod_http_base64_decode("not base64!", out, sizeof(out)));
    CHECK(!rpod_http_base64_decode("cnBvZDpzZWNyZXQ=", out, 4));
}

static void test_json(void)
{
    rpod_json_t j;
    rpod_json_init(&j);
    rpod_json_obj(&j);
    rpod_json_kstr(&j, "s", "a\"b\\c\nd\x01");
    rpod_json_kint(&j, "n", -42);
    rpod_json_kbool(&j, "b", true);
    rpod_json_key(&j, "arr");
    rpod_json_arr(&j);
    rpod_json_int(&j, 1);
    rpod_json_obj(&j);
    rpod_json_end_obj(&j);
    rpod_json_null(&j);
    rpod_json_end_arr(&j);
    rpod_json_kstr(&j, "u", "\xc3\xa9\xe2\x80\xa6");     /* é… pass through */
    rpod_json_kstr(&j, "bad", "x\xffy\xc3(");              /* stray bytes: U+FFFD */
    rpod_json_end_obj(&j);
    CHECK(!j.oom);
    CHECK(strcmp(j.s, "{\"s\":\"a\\\"b\\\\c\\nd\\u0001\",\"n\":-42,\"b\":true,\"arr\":[1,{},null],"
                      "\"u\":\"\xc3\xa9\xe2\x80\xa6\",\"bad\":\"x\xef\xbf\xbdy\xef\xbf\xbd(\"}") == 0);
    rpod_json_free(&j);

    CHECK(rpod_utf8_valid("plain"));
    CHECK(rpod_utf8_valid("\xc3\xa9t\xc3\xa9"));
    CHECK(rpod_utf8_valid("\xf0\x9f\x8e\xb5"));          /* 🎵 */
    CHECK(!rpod_utf8_valid("\xc0\xaf"));                  /* overlong '/' */
    CHECK(!rpod_utf8_valid("\xed\xa0\x80"));              /* a surrogate */
    CHECK(!rpod_utf8_valid("\xf4\x90\x80\x80"));          /* past U+10FFFF */
    CHECK(!rpod_utf8_valid("\xe2\x82"));                  /* cut short */
}

static void test_names(void)
{
    CHECK(rpod_lib_name_ok("01 Song.flac"));
    CHECK(rpod_lib_name_ok("Always Aliv\xc3\xab.flac"));
    CHECK(rpod_lib_name_ok("AC-DC"));
    CHECK(!rpod_lib_name_ok(""));
    CHECK(!rpod_lib_name_ok("."));
    CHECK(!rpod_lib_name_ok(".."));
    CHECK(!rpod_lib_name_ok(".hidden"));
    CHECK(!rpod_lib_name_ok("a/b"));
    CHECK(!rpod_lib_name_ok("a\\b"));
    CHECK(!rpod_lib_name_ok("What?.flac"));             /* exFAT won't have it */
    CHECK(!rpod_lib_name_ok("a:b"));
    CHECK(!rpod_lib_name_ok("trailing."));
    CHECK(!rpod_lib_name_ok("trailing "));
    CHECK(!rpod_lib_name_ok("tab\there"));
    CHECK(!rpod_lib_name_ok("bad\xff"));
    char long_name[300];
    memset(long_name, 'a', 256);
    long_name[256] = '\0';
    CHECK(!rpod_lib_name_ok(long_name));
    long_name[255] = '\0';
    CHECK(rpod_lib_name_ok(long_name));

    CHECK(rpod_lib_path_ok("", true));
    CHECK(!rpod_lib_path_ok("", false));
    CHECK(rpod_lib_path_ok("Artist/Album/01 Song.flac", false));
    CHECK(!rpod_lib_path_ok("/etc/passwd", false));
    CHECK(!rpod_lib_path_ok("Artist/../../etc", false));
    CHECK(!rpod_lib_path_ok("Artist//Album", false));
    CHECK(!rpod_lib_path_ok("Artist/", false));
    CHECK(!rpod_lib_path_ok("Artist/./Album", false));
    CHECK(!rpod_lib_path_ok("Artist/.rpod-upload-1-2.part", false));

    CHECK(rpod_lib_upload_name_ok("01 Song.flac"));
    CHECK(rpod_lib_upload_name_ok("Song.MP3"));
    CHECK(rpod_lib_upload_name_ok("cover.jpg"));
    CHECK(!rpod_lib_upload_name_ok("notes.txt"));
    CHECK(!rpod_lib_upload_name_ok("flac"));
    CHECK(!rpod_lib_upload_name_ok("evil.sh"));
    CHECK(!rpod_lib_upload_name_ok(".flac"));
}

static bool exists(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0;
}

static void touch(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    if (f != NULL) {
        fputs(content, f);
        fclose(f);
    }
}

/* Writes `content` the way an upload does. */
static int upload(const char *root, const char *dir, const char *name, const char *content, bool overwrite,
                  bool *replaced)
{
    int dir_fd;
    char tmp[64];
    int fd = rpod_lib_upload_begin(root, dir, &dir_fd, tmp, sizeof(tmp));
    if (fd < 0) {
        return fd;
    }
    CHECK(tmp[0] == '.'); /* hidden from MPD while it arrives */
    CHECK(write(fd, content, strlen(content)) == (ssize_t)strlen(content));
    close(fd);
    int rc = rpod_lib_upload_finish(dir_fd, tmp, name, overwrite, replaced);
    close(dir_fd);
    return rc;
}

static void test_library(void)
{
    char root[] = "/tmp/rpod-test-web-XXXXXX";
    char outside[] = "/tmp/rpod-test-out-XXXXXX";
    if (mkdtemp(root) == NULL || mkdtemp(outside) == NULL) {
        fprintf(stderr, "mkdtemp: %s\n", strerror(errno));
        failures++;
        return;
    }
    char p[512], q[512];

    /* Uploads make their folders, never replace by accident, and leave no
     * temp files behind. */
    bool replaced;
    CHECK(upload(root, "Artist/Album", "01 Song.flac", "one", false, &replaced) == 0 && !replaced);
    snprintf(p, sizeof(p), "%s/Artist/Album/01 Song.flac", root);
    CHECK(exists(p));
    CHECK(upload(root, "Artist/Album", "01 Song.flac", "two", false, &replaced) == -EEXIST);
    CHECK(upload(root, "Artist/Album", "01 Song.flac", "three", true, &replaced) == 0 && replaced);
    FILE *f = fopen(p, "r");
    char buf[16] = "";
    if (f != NULL) {
        CHECK(fgets(buf, sizeof(buf), f) != NULL);
        fclose(f);
    }
    CHECK(strcmp(buf, "three") == 0);
    snprintf(q, sizeof(q), "ls -A '%s/Artist/Album' | grep -c rpod-upload > /dev/null", root);
    CHECK(system(q) != 0); /* grep found nothing */

    /* Never over a directory, even when told to replace. */
    snprintf(p, sizeof(p), "%s/Artist/Album/dir.flac", root);
    mkdir(p, 0755);
    CHECK(upload(root, "Artist/Album", "dir.flac", "x", true, &replaced) == -EEXIST);

    /* Symlinks lead nowhere: not into, not through. */
    snprintf(p, sizeof(p), "%s/escape", root);
    CHECK(symlink(outside, p) == 0);
    CHECK(rpod_lib_open_dir(root, "escape", false) < 0);
    CHECK(rpod_lib_open_dir(root, "escape/sub", true) < 0);
    CHECK(upload(root, "escape", "evil.flac", "x", false, &replaced) < 0);
    snprintf(q, sizeof(q), "%s/evil.flac", outside);
    CHECK(!exists(q));
    CHECK(rpod_lib_open_dir(root, "..", false) == -EINVAL);

    /* Deleting a symlinked folder deletes the link, not what it points at. */
    snprintf(q, sizeof(q), "%s/keep.flac", outside);
    touch(q, "keep");
    CHECK(rpod_lib_delete(root, "escape") == 0);
    CHECK(!exists(p));
    CHECK(exists(q));

    /* Renames stay in their folder and don't clobber. */
    CHECK(upload(root, "Artist/Album", "02 Other.flac", "2", false, &replaced) == 0);
    CHECK(rpod_lib_rename(root, "Artist/Album/02 Other.flac", "01 Song.flac") == -EEXIST);
    CHECK(rpod_lib_rename(root, "Artist/Album/02 Other.flac", "../02 Other.flac") == -EINVAL);
    CHECK(rpod_lib_rename(root, "Artist/Album/02 Other.flac", "02 Renamed.flac") == 0);
    snprintf(p, sizeof(p), "%s/Artist/Album/02 Renamed.flac", root);
    CHECK(exists(p));

    /* A whole tree goes, but never the root itself. */
    CHECK(rpod_lib_delete(root, "") == -EINVAL);
    CHECK(rpod_lib_delete(root, "Artist") == 0);
    snprintf(p, sizeof(p), "%s/Artist", root);
    CHECK(!exists(p));
    CHECK(rpod_lib_delete(root, "Artist") == -ENOENT);
    CHECK(exists(root));

    snprintf(q, sizeof(q), "rm -rf '%s' '%s'", root, outside);
    CHECK(system(q) == 0);
}

int main(void)
{
    test_url_decode();
    test_params();
    test_hosts();
    test_base64();
    test_json();
    test_names();
    test_library();
    if (failures != 0) {
        fprintf(stderr, "test_web: %d check(s) failed\n", failures);
        return 1;
    }
    printf("test_web: ok\n");
    return 0;
}
