/*
 * Sorting a folder's entries for display.
 *
 * Folders come first, then files, each in "natural" order: names compare
 * without regard to case, and runs of digits compare as numbers, so "Disc 2"
 * comes before "Disc 10".
 */
#ifndef OH_MY_3DO_MENU_SORT_H
#define OH_MY_3DO_MENU_SORT_H

#include <stddef.h>

/*
 * One entry in a folder listing, as the menu keeps it in memory.
 *
 * This is not the layout the FCare sends over USB; the transport code
 * (ode.c) decodes the drive's table of contents into these. The name must
 * be a zero-terminated string the listing owns, not a buffer that the next
 * USB read will overwrite.
 */
typedef struct MenuEntry {
    /*
     * The FCare's id for this entry, used to open or enter it. The top byte
     * says which USB volume it is on and the low 24 bits identify the entry
     * there. (unsigned long is 32 bits on the 3DO.)
     */
    unsigned long toc_id;
    int is_directory; /* nonzero for a folder */
    const char *name;
} MenuEntry;

/*
 * Sorts `count` entry pointers in place. Only the pointers move, so each
 * entry's name, kind and id stay together.
 *
 * Sort the whole folder before splitting it into pages, and keep extra rows
 * such as ".." (parent folder) out of the array. Every pointer and name must
 * be valid when count > 0, and must stay alive while the sorted view is used.
 */
void menu_sort(const MenuEntry **view, size_t count);

#endif
