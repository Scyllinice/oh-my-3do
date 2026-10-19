#ifndef OH_MY_3DO_MENU_SORT_H
#define OH_MY_3DO_MENU_SORT_H

#include <stddef.h>

/* Host-side model, NOT a wire structure. The transport must decode TOC data.
 * toc_id is a decoded 32-bit value (unsigned long is 32 bits on the 3DO).
 * Names must be owned, NUL-terminated strings, not a reused receive buffer.
 */
typedef struct MenuEntry {
    unsigned long toc_id;
    int is_directory;
    const char *name;
} MenuEntry;

/* Sort the complete current directory before splitting it into UI pages.
 * Only pointers move: each name, kind and TOC ID remains together.
 * All pointers and names must be non-NULL for count > 0; count == 0 permits NULL.
 * Keep synthetic navigation rows (e.g. parent directory) outside this array.
 * Entries and names must remain alive while the sorted view is in use.
 */
void menu_sort(const MenuEntry **view, size_t count);

#endif
