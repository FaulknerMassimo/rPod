#include "scores.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#define MAX_GAMES 8
#define NAME_MAX_LEN 16

static struct {
    struct {
        char name[NAME_MAX_LEN];
        unsigned best;
    } game[MAX_GAMES];
    int count;
    char path[PATH_MAX];
} g;

static int find(const char *game, bool add)
{
    for (int i = 0; i < g.count; i++) {
        if (strcmp(g.game[i].name, game) == 0) {
            return i;
        }
    }
    if (!add || g.count == MAX_GAMES || strlen(game) >= NAME_MAX_LEN) {
        return -1;
    }
    snprintf(g.game[g.count].name, NAME_MAX_LEN, "%s", game);
    g.game[g.count].best = 0;
    return g.count++;
}

static void load(void)
{
    FILE *f = fopen(g.path, "r");
    if (f == NULL) {
        return; /* no games played yet */
    }
    char name[NAME_MAX_LEN];
    unsigned best;
    while (fscanf(f, "%15s %u", name, &best) == 2) {
        int i = find(name, true);
        if (i >= 0) {
            g.game[i].best = best;
        }
    }
    fclose(f);
}

/* Written to a temp file and renamed over the real one, so a power cut
 * mid-write can't leave it truncated. */
static void save(void)
{
    if (g.path[0] == '\0') {
        return;
    }
    char tmp[sizeof(g.path) + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g.path);
    FILE *f = fopen(tmp, "w");
    if (f == NULL) {
        fprintf(stderr, "rpod: scores couldn't write %s: %s\n", tmp, strerror(errno));
        return;
    }
    for (int i = 0; i < g.count; i++) {
        fprintf(f, "%s %u\n", g.game[i].name, g.game[i].best);
    }
    if (fclose(f) != 0 || rename(tmp, g.path) != 0) {
        fprintf(stderr, "rpod: scores couldn't save %s: %s\n", g.path, strerror(errno));
        remove(tmp);
    }
}

void rpod_scores_init(const char *state_path)
{
    g.count = 0;
    g.path[0] = '\0';
    if (state_path != NULL && state_path[0] != '\0') {
        snprintf(g.path, sizeof(g.path), "%s", state_path);
        load();
    }
}

unsigned rpod_score_best(const char *game)
{
    int i = find(game, false);
    return i >= 0 ? g.game[i].best : 0;
}

bool rpod_score_submit(const char *game, unsigned score)
{
    int i = find(game, true);
    if (i < 0 || score <= g.game[i].best) {
        return false;
    }
    g.game[i].best = score;
    save();
    return true;
}
