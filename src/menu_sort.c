/*
 * Natural-order sorting of folder listings. See menu_sort.h.
 */
#include "menu_sort.h"

#include <stdlib.h>
#include <string.h>

/* Converts an ASCII capital letter to lower case; other bytes are unchanged. */
static unsigned char to_lower_ascii(unsigned char c)
{
    if (c >= 'A' && c <= 'Z')
        return (unsigned char)(c + ('a' - 'A'));
    return c;
}

static int is_digit(unsigned char c)
{
    return c >= '0' && c <= '9';
}

/*
 * Compares two names the way people expect a list to be ordered: ignoring
 * case, and with numbers in numeric order ("Disc 2" before "Disc 10").
 * Returns a negative number, zero or a positive number, like strcmp.
 *
 * Digit runs are compared as text rather than converted to integers, so an
 * arbitrarily long number can never overflow the ARM60's 32-bit arithmetic:
 * after skipping leading zeros, the run with more digits is the larger
 * number, and runs of the same length compare digit by digit.
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

            /* "007" and "7" are the same number. */
            while (*a == '0')
                ++a;
            while (*b == '0')
                ++b;

            /* Measure each run of digits. */
            a_end = a;
            b_end = b;
            while (is_digit(*a_end))
                ++a_end;
            while (is_digit(*b_end))
                ++b_end;
            a_length = (size_t)(a_end - a);
            b_length = (size_t)(b_end - b);

            /* More digits means a larger number. */
            if (a_length != b_length)
                return a_length < b_length ? -1 : 1;
            /* Same length: the first differing digit decides. */
            result = memcmp(a, b, a_length);
            if (result)
                return result;

            a = a_end;
            b = b_end;
        } else {
            unsigned char a_char = to_lower_ascii(*a);
            unsigned char b_char = to_lower_ascii(*b);
            if (a_char != b_char)
                return a_char < b_char ? -1 : 1;
            ++a;
            ++b;
        }
    }

    /* One name ran out first: the shorter one comes first. */
    if (*a != *b)
        return *a ? 1 : -1;

    /*
     * The names are equal apart from case or leading zeros ("Disc 1" and
     * "disc 01"). Fall back to a plain comparison so they always come out in
     * the same order.
     */
    return strcmp(left, right);
}

/*
 * Orders two entries (given as pointers to entry pointers, as a sort passes
 * them): folders before files, then natural name order, then by id so that
 * no two different entries ever compare equal.
 */
static int compare_entries(const void *left, const void *right)
{
    const MenuEntry *a = *(const MenuEntry *const *)left;
    const MenuEntry *b = *(const MenuEntry *const *)right;
    int result;

    /* "!!" turns any nonzero value into 1, so only folder-or-not counts. */
    if (!!a->is_directory != !!b->is_directory)
        return a->is_directory ? -1 : 1;

    result = natural_compare(a->name, b->name);
    if (result)
        return result;

    if (a->toc_id == b->toc_id)
        return 0;
    return a->toc_id < b->toc_id ? -1 : 1;
}

/*
 * Sorts with a Shell sort rather than the C library's qsort, which on the
 * ARM60 took about a millisecond per entry: around 150 ms for a folder of 150
 * games, enough to notice on every folder change.
 *
 * A Shell sort is an insertion sort run several times, first on elements far
 * apart (`gap` positions) and finally on neighbours, so most elements are
 * already near their place by the last pass. These gaps are Ciura's
 * sequence, which works well in practice.
 *
 * Shell sort is not stable (equal elements may swap), but compare_entries
 * never calls two different entries equal, so the result is always the same.
 */
void menu_sort(const MenuEntry **view, size_t count)
{
    static const size_t gaps[] = {701, 301, 132, 57, 23, 10, 4, 1};
    size_t gap_index, i, j;

    for (gap_index = 0; gap_index < sizeof(gaps) / sizeof(gaps[0]); ++gap_index) {
        size_t gap = gaps[gap_index];

        /* Insertion sort on every gap-th element. */
        for (i = gap; i < count; ++i) {
            const MenuEntry *entry = view[i];
            /* Shift larger elements up by one gap until entry's place is found. */
            for (j = i; j >= gap && compare_entries(&view[j - gap], &entry) > 0; j -= gap)
                view[j] = view[j - gap];
            view[j] = entry;
        }
    }
}
