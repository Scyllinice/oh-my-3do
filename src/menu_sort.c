#include "menu_sort.h"

#include <stdlib.h>
#include <string.h>

static unsigned char fold_ascii(unsigned char c)
{
    if (c >= 'A' && c <= 'Z')
        return (unsigned char)(c + ('a' - 'A'));
    return c;
}

static int is_digit(unsigned char c)
{
    return c >= '0' && c <= '9';
}

/* Compare digit runs by significant length and then digits. Do not convert
 * to an integer: arbitrarily long numbers must not overflow on ARM60.
 */
static int natural_compare(const char *left, const char *right)
{
    const unsigned char *a = (const unsigned char *)left;
    const unsigned char *b = (const unsigned char *)right;

    while (*a && *b) {
        if (is_digit(*a) && is_digit(*b)) {
            const unsigned char *a_end;
            const unsigned char *b_end;
            size_t a_length;
            size_t b_length;
            int result;

            while (*a == '0') ++a;
            while (*b == '0') ++b;
            a_end = a;
            b_end = b;
            while (is_digit(*a_end)) ++a_end;
            while (is_digit(*b_end)) ++b_end;
            a_length = (size_t)(a_end - a);
            b_length = (size_t)(b_end - b);
            if (a_length != b_length)
                return a_length < b_length ? -1 : 1;
            result = memcmp(a, b, a_length);
            if (result) return result;
            a = a_end;
            b = b_end;
        } else {
            unsigned char ac = fold_ascii(*a);
            unsigned char bc = fold_ascii(*b);
            if (ac != bc) return ac < bc ? -1 : 1;
            ++a;
            ++b;
        }
    }
    if (*a != *b) return *a ? 1 : -1;
    /* Deterministic tie-break for case and zero-padding variations. */
    return strcmp(left, right);
}

static int compare_entries(const void *left, const void *right)
{
    const MenuEntry *a = *(const MenuEntry *const *)left;
    const MenuEntry *b = *(const MenuEntry *const *)right;
    int result;

    if (!!a->is_directory != !!b->is_directory)
        return a->is_directory ? -1 : 1;
    result = natural_compare(a->name, b->name);
    if (result) return result;
    if (a->toc_id == b->toc_id) return 0;
    return a->toc_id < b->toc_id ? -1 : 1;
}

void menu_sort(const MenuEntry **view, size_t count)
{
    if (count > 1)
        qsort(view, count, sizeof(*view), compare_entries);
}
