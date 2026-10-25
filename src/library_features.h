/*
 * Favorites, recent games, name search and multi-disc sets.
 *
 * Favorites and recent games are plain text files in .ohmy3do on the drive
 * (favorites.txt and recent.txt), one disc path per line, relative to the
 * drive root:
 *
 *     Games/USA/Road Rash (USA)/Road Rash (USA).iso
 *
 * Blank lines and lines starting with '#' or ';' are ignored, so the files
 * can be edited by hand. Both use the MenuFavorites list type.
 */
#ifndef OH_MY_3DO_LIBRARY_FEATURES_H
#define OH_MY_3DO_LIBRARY_FEATURES_H
#include "ode.h"

/* The largest favorites or recent file, the most entries, and the longest
 * path (including its zero byte). */
#define FAVORITES_BYTES 8192
#define FAVORITES_MAX   64
#define FAVORITE_PATH   1024

/* The number of games recent.txt remembers. */
#define RECENT_MAX 10

/* A list of disc paths, from favorites.txt or recent.txt. */
typedef struct MenuFavorites {
    char *storage;                    /* the file's text; the paths point into it */
    const char *paths[FAVORITES_MAX]; /* in the file's order */
    size_t count;
} MenuFavorites;

/* Frees a list and leaves it empty. */
void menu_favorites_free(MenuFavorites *favorites);

/*
 * Reads a list from a file's text (`size` bytes, no zero bytes). The old
 * list is replaced only on success. Returns 0, or -1 if the file is too big,
 * has too many entries, an unsafe path or the same path twice.
 */
int menu_favorites_parse(MenuFavorites *favorites, const char *data, size_t size);

/*
 * Adds `path` to the end of the list, or with `remove` set removes it.
 * Returns 0 when changed, 1 when there was nothing to do (already present,
 * or not there to remove), or -1 on failure, which leaves the list as it was.
 */
int menu_favorites_edit(MenuFavorites *favorites, const char *path, int remove);

/* Writes the list as zero-terminated text, one path per line. Returns 0, or
 * -1 if it does not fit in `capacity` bytes. */
int menu_favorites_format(const MenuFavorites *favorites, char *out, size_t capacity);

/* Moves the entry at `from` to position `to`, shifting those between.
 * Returns 0, or -1 if either position is out of range. */
int menu_favorites_move(MenuFavorites *favorites, size_t from, size_t to);

/*
 * The favorites the person can see: those not in a hidden category (nor in
 * the Adult category when `hide_adult` is set). _count counts them;
 * _index turns the position of one in that visible list into its position
 * in the full list (or the list's count if there is no such entry).
 */
size_t menu_favorites_visible_count(const MenuFavorites *favorites, int hide_adult);
size_t menu_favorites_visible_index(const MenuFavorites *favorites, int hide_adult, size_t visible);

/*
 * Puts `path` at the top of the recent games list, removing any older entry
 * for it and keeping at most RECENT_MAX games. Returns 0, or -1 on failure.
 */
int menu_recent_push(MenuFavorites *recent, const char *path);

/* Whether `query` appears anywhere in `name`, ignoring case. An empty query
 * matches everything. */
int menu_name_matches(const char *name, const char *query);

/*
 * Whether `path` is a safe relative path: not empty, not starting with '/',
 * shorter than FAVORITE_PATH, no empty, "." or ".." folder names, and no
 * control characters, backslashes or colons.
 */
int menu_relative_path(const char *path);

/*
 * Finds the multi-disc set the selected disc belongs to, among the discs in
 * the same folder. Discs belong together when their names differ only in the
 * disc number: "Game (Disc 1).iso", "Game (Disc 2).iso" (also "Disk 1" or
 * "CD1"). The set must be complete: numbered 1, 2, 3... with no gaps or
 * repeats, at least two discs, and no more than `capacity`.
 *
 * On success fills `out` with the set in disc order, stores the selected
 * disc's place in it in `*position`, and returns the set's size; otherwise
 * returns 0. It only looks: it never changes or starts the disc queue.
 */
size_t menu_disc_group(const OdeListing *listing, const MenuEntry *selected, const MenuEntry **out,
                       size_t capacity, size_t *position);

/* The alphabet strip letter for a name: 1 to 26 for A to Z, or 0 ('#') for
 * anything else. */
int menu_initial(const char *name);
#endif
