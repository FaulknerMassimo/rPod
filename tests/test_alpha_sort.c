/*
 * The browse lists' alphabetical order and the alphabet scrub's letter jumps
 * (src/ui/alpha_sort.c).
 */

#include "ui/alpha_sort.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);   \
            failures++;                                                                \
        }                                                                              \
    } while (0)

static void test_letters(void)
{
    CHECK(rpod_alpha_letter("Abba") == 'A');
    CHECK(rpod_alpha_letter("abba") == 'A');
    CHECK(rpod_alpha_letter("The Beatles") == 'B');
    CHECK(rpod_alpha_letter("the xx") == 'X');
    CHECK(rpod_alpha_letter("The") == 'T');           /* nothing after it */
    CHECK(rpod_alpha_letter("Theory of a Deadman") == 'T');
    CHECK(rpod_alpha_letter("(What's the Story) Morning Glory?") == 'W');
    CHECK(rpod_alpha_letter("\"Heroes\"") == 'H');
    CHECK(rpod_alpha_letter("\xc3\x89" "dith Piaf") == 'E');  /* Édith */
    CHECK(rpod_alpha_letter("\xc3\xb6zil") == 'O');           /* özil */
    CHECK(rpod_alpha_letter("\xc5\xbd" "eljko") == 'Z');      /* Željko */
    CHECK(rpod_alpha_letter("\xc5\x92uvre") == 'O');          /* Œuvre */
    CHECK(rpod_alpha_letter("\xc3\x9f") == 'S');              /* ß */
    CHECK(rpod_alpha_letter("1999") == '#');
    CHECK(rpod_alpha_letter("4X4") == '#');
    CHECK(rpod_alpha_letter("...") == '#');
    CHECK(rpod_alpha_letter("") == '#');
    CHECK(rpod_alpha_letter("\xe6\x9d\xb1\xe4\xba\xac") == '#'); /* 東京 */
    CHECK(rpod_alpha_letter("\xc3\x97") == '#');              /* × is no letter */
    CHECK(rpod_alpha_letter("\xc3") == '#');                  /* truncated UTF-8 */
}

static int cmp_qsort(const void *a, const void *b)
{
    return rpod_alpha_compare(*(const char *const *)a, *(const char *const *)b);
}

static void test_order(void)
{
    const char *names[] = {
        "Zedd", "1999", "beabadoobee", "The Beatles", "ABBA", "\xc3\x89" "dith Piaf",
        "Eagles", "AC/DC", "Abba", "4X4", "zz top", "Earth, Wind & Fire", "The Weeknd",
    };
    const char *want[] = {
        "ABBA", "Abba", "AC/DC", "beabadoobee", "The Beatles", "Eagles", "Earth, Wind & Fire",
        "\xc3\x89" "dith Piaf", "The Weeknd", "Zedd", "zz top", "1999", "4X4",
    };
    size_t n = sizeof(names) / sizeof(names[0]);
    qsort(names, n, sizeof(names[0]), cmp_qsort);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(names[i], want[i]) != 0) {
            fprintf(stderr, "order[%zu]: got \"%s\", want \"%s\"\n", i, names[i], want[i]);
            failures++;
        }
    }

    CHECK(rpod_alpha_compare("abc", "ABC") != 0);  /* still a total order */
    CHECK(rpod_alpha_compare("abc", "abc") == 0);
    CHECK(rpod_alpha_compare("Ab", "Abc") < 0);
    CHECK(rpod_alpha_compare("A B", "AB") < 0);    /* word break first */
}

static const char *name_at(const void *ctx, size_t i)
{
    return ((const char *const *)ctx)[i];
}

static void test_jump(void)
{
    /* 0-2 A, 3-4 B, 5 D, 6-7 # */
    const char *list[] = { "Abba", "AC/DC", "Adele", "Beck", "The Black Keys", "Daft Punk", "2Pac", "50 Cent" };
    size_t n = sizeof(list) / sizeof(list[0]);

    CHECK(rpod_alpha_jump(name_at, list, n, 0, +1) == 3);
    CHECK(rpod_alpha_jump(name_at, list, n, 1, +1) == 3);
    CHECK(rpod_alpha_jump(name_at, list, n, 3, +1) == 5);
    CHECK(rpod_alpha_jump(name_at, list, n, 5, +1) == 6);
    CHECK(rpod_alpha_jump(name_at, list, n, 6, +1) == 6);  /* last letter: stay */
    CHECK(rpod_alpha_jump(name_at, list, n, 7, +1) == 7);

    CHECK(rpod_alpha_jump(name_at, list, n, 7, -1) == 5);  /* # -> D */
    CHECK(rpod_alpha_jump(name_at, list, n, 5, -1) == 3);  /* D -> B */
    CHECK(rpod_alpha_jump(name_at, list, n, 4, -1) == 0);  /* mid-B -> A */
    CHECK(rpod_alpha_jump(name_at, list, n, 2, -1) == 0);  /* first letter: its top */
    CHECK(rpod_alpha_jump(name_at, list, n, 0, -1) == 0);

    CHECK(rpod_alpha_jump(name_at, list, n, 4, 0) == 4);
    CHECK(rpod_alpha_jump(name_at, list, 0, 0, +1) == 0);
    CHECK(rpod_alpha_jump(name_at, list, n, 99, -1) == 5);  /* out of range clamps */
}

int main(void)
{
    test_letters();
    test_order();
    test_jump();
    if (failures != 0) {
        fprintf(stderr, "test_alpha_sort: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_alpha_sort: ok\n");
    return 0;
}
