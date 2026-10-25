/*
 * Small text helpers for the game list: turning file names into titles,
 * scrolling long titles, the folder "breadcrumb", and deciding which entries
 * on the drive are the menu's own support files rather than games.
 */
#ifndef OH_MY_3DO_MENU_UI_H
#define OH_MY_3DO_MENU_UI_H
#include "menu_categories.h"
#include <stddef.h>

/*
 * A cover built into the menu, for the offline demo (see cover_catalog.c).
 * The list is sorted by path, ignoring case, so it can be searched quickly.
 */
typedef struct MenuCover {
    const char *path; /* disc path relative to the drive root */
    const char *file; /* the cover image inside the menu's own ISO */
} MenuCover;
extern const MenuCover menu_covers[];
extern const size_t menu_cover_count;

/*
 * Compares two paths without regard to ASCII case, as FAT drives do.
 * Returns a negative number, zero or a positive number, like strcmp.
 */
int menu_path_compare(const char *a, const char *b);

/* "ISO" or "CUE" for a disc image file name, or NULL for anything else. */
const char *menu_image_format(const char *name, int directory);

/* The length of `name` without its ".iso" or ".cue" extension. */
size_t menu_title_length(const char *name, int directory);

/*
 * The length of the start of `name` that remains after removing its
 * extension and any region or language tags at the end:
 * "Road Rash (USA) (En,Fr).iso" gives the length of "Road Rash".
 * Removing stops at the first other tag from the end, so a title like
 * "Doom (Rev 1) (USA)" keeps "Doom (Rev 1)". Use menu_display_title to also
 * drop region tags that come before other tags.
 */
size_t menu_short_title_length(const char *name, int directory);

/*
 * Writes the title shown in the list into `out` (`capacity` bytes, always
 * zero-terminated) and returns its length: the name without its extension
 * and without any region or language tags, keeping every other tag.
 * "Doom (USA) (Rev 1).iso" becomes "Doom (Rev 1)". Folders keep their names.
 */
size_t menu_display_title(char *out, size_t capacity, const char *name, int directory);

/*
 * How many characters to scroll a title of `length` characters shown in
 * `visible` characters, `elapsed` milliseconds after it was selected. Long
 * titles pause, scroll to their end, pause there and start again.
 */
size_t menu_scroll_offset(size_t length, size_t visible, unsigned long elapsed);

/*
 * Writes the folder heading into `out` (at least 35 bytes) for a folder
 * `depth` levels below the drive root: "/ STORAGE" at the root, "/ Games" one
 * level down, and "... / Games / USA" deeper, shortened with "..." to fit.
 */
void menu_breadcrumb(char *out, const char *parent, const char *leaf, unsigned int depth);

/* The built-in demo cover for a disc path, or NULL. */
const char *menu_cover_lookup(const char *path);

/* Finds `path` in a sorted cover list (see MenuCover). Returns NULL if absent. */
const char *menu_cover_find(const MenuCover *covers, size_t count, const char *path);

/*
 * Whether an entry in a folder listing is one of the menu's support files,
 * which are never shown: the navigation helper and CUE track (.bin) files
 * anywhere, and in the drive's root folder the menu's own ISOs, checksums,
 * the .ohmy3do folder and category folders that are switched off.
 * `root` is nonzero when the listing is the drive's root folder.
 */
int menu_support_entry(const char *name, int directory, int root);

/* When set, the Adult category folder in the drive root is hidden too. */
void menu_set_hide_adult(int hide);

/* Whether a path is inside the Adult category: "Adult/..." or
 * "/Storage/[VOLUME/]Adult/...". */
int menu_adult_path(const char *path);
#endif
