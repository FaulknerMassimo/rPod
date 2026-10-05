#include "alpha_sort.h"

#include <stdint.h>
#include <string.h>

/* Base letters for U+00C0-U+00FF (Latin-1 Supplement's letters) and
 * U+0100-U+017F (Latin Extended-A) -- the accented range the UI's fonts
 * cover (src/ui/lv_conf.h). '.' marks the two that aren't letters (x, ÷). */
static const char k_latin1[] =
    "AAAAAAACEEEEIIIIDNOOOOO.OUUUUYTS"
    "AAAAAAACEEEEIIIIDNOOOOO.OUUUUYTY";
static const char k_latin_ext_a[] =
    "AAAAAACCCCCCCCDDDDEEEEEEEEEEGGGGGGGGHHHHIIIIIIIIIIIIJJKKKLLLLLLLLLL"
    "NNNNNNNNNOOOOOOOORRRRRRSSSSSSSSTTTTTTUUUUUUUUUUUUWWYYYZZZZZZS";
_Static_assert(sizeof(k_latin1) == 64 + 1, "one entry per U+00C0-U+00FF");
_Static_assert(sizeof(k_latin_ext_a) == 128 + 1, "one entry per U+0100-U+017F");

#define REPLACEMENT_CHAR 0xFFFDu

/* Decodes one UTF-8 code point at *s and advances past it -- except at the
 * NUL, which returns 0 and stays put. A malformed byte comes back as U+FFFD
 * (not a letter), one byte at a time. */
static uint32_t next_cp(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t c = p[0];
    if (c == 0) {
        return 0;
    }
    if (c < 0x80) {
        *s += 1;
        return c;
    }
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (extra == 0) {
        *s += 1;
        return REPLACEMENT_CHAR;
    }
    uint32_t cp = c & (0x3Fu >> extra);
    for (int i = 1; i <= extra; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            *s += 1;
            return REPLACEMENT_CHAR;
        }
        cp = (cp << 6) | (p[i] & 0x3Fu);
    }
    *s += 1 + extra;
    return cp;
}

/* The upper-case base letter of `cp`, or 0 if it isn't a letter. */
static char base_letter(uint32_t cp)
{
    if (cp >= 'a' && cp <= 'z') {
        return (char)(cp - 'a' + 'A');
    }
    if (cp >= 'A' && cp <= 'Z') {
        return (char)cp;
    }
    if (cp >= 0xC0 && cp <= 0xFF) {
        char b = k_latin1[cp - 0xC0];
        return b == '.' ? 0 : b;
    }
    if (cp >= 0x100 && cp <= 0x17F) {
        return k_latin_ext_a[cp - 0x100];
    }
    return 0;
}

static int is_digit(uint32_t cp)
{
    return cp >= '0' && cp <= '9';
}

/* Where sorting starts: past leading punctuation and spaces, then past a
 * leading "The " if anything follows it. */
static const char *significant(const char *name)
{
    const char *s = name;
    for (;;) {
        const char *at = s;
        uint32_t cp = next_cp(&s);
        if (cp == 0 || base_letter(cp) != 0 || is_digit(cp)) {
            s = at;
            break;
        }
    }
    if ((s[0] == 'T' || s[0] == 't') && (s[1] == 'H' || s[1] == 'h') &&
        (s[2] == 'E' || s[2] == 'e') && s[3] == ' ') {
        const char *rest = s + 4;
        while (*rest == ' ') {
            rest++;
        }
        if (*rest != '\0') {
            return significant(rest);
        }
    }
    return s;
}

/* Letters A-Z sort as 0-25, everything else (digits, symbols, empty) as 26. */
static int letter_rank(const char *sig)
{
    uint32_t cp = next_cp(&sig);
    char b = base_letter(cp);
    return b != 0 ? b - 'A' : 26;
}

char rpod_alpha_letter(const char *name)
{
    int rank = letter_rank(significant(name));
    return rank < 26 ? (char)('A' + rank) : '#';
}

int rpod_alpha_compare(const char *a, const char *b)
{
    const char *sa = significant(a);
    const char *sb = significant(b);
    int ra = letter_rank(sa);
    int rb = letter_rank(sb);
    if (ra != rb) {
        return ra < rb ? -1 : 1;
    }
    for (;;) {
        uint32_t ca = next_cp(&sa);
        uint32_t cb = next_cp(&sb);
        char la = base_letter(ca);
        char lb = base_letter(cb);
        uint32_t ka = la != 0 ? (uint32_t)la : ca;
        uint32_t kb = lb != 0 ? (uint32_t)lb : cb;
        if (ka != kb) {
            return ka < kb ? -1 : 1;
        }
        if (ca == 0) {
            break;
        }
    }
    return strcmp(a, b);
}

size_t rpod_alpha_jump(rpod_alpha_name_fn name_at, const void *ctx, size_t count, size_t cur,
                       int dir)
{
    if (count == 0 || dir == 0) {
        return cur;
    }
    if (cur >= count) {
        cur = count - 1;
    }
    char here = rpod_alpha_letter(name_at(ctx, cur));

    if (dir > 0) {
        for (size_t i = cur + 1; i < count; i++) {
            if (rpod_alpha_letter(name_at(ctx, i)) != here) {
                return i;
            }
        }
        return cur;
    }

    /* Back past the rest of this letter, then to the top of the one before. */
    size_t i = cur;
    while (i > 0 && rpod_alpha_letter(name_at(ctx, i - 1)) == here) {
        i--;
    }
    if (i == 0) {
        return 0;
    }
    char prev = rpod_alpha_letter(name_at(ctx, i - 1));
    i--;
    while (i > 0 && rpod_alpha_letter(name_at(ctx, i - 1)) == prev) {
        i--;
    }
    return i;
}
