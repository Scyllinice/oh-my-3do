/*
 * Category folders and which of them are hidden.
 *
 * The drive's top level is organized into category folders: Games, Demos,
 * Samplers and so on. The desktop app writes their names and whether each is
 * hidden to .ohmy3do/categories.json:
 *
 *     {"schema":1,
 *      "categories":[{"id":"category-0","name":"Games","hidden":false}, ...],
 *      ...}
 *
 * The menu can hide or show a category itself (Tools > Settings > Category
 * visibility), and then writes the file back. To keep everything else in the
 * file exactly as the desktop app wrote it, the menu only ever changes the
 * "hidden" values in place.
 *
 * There is a single set of categories for the whole menu, kept inside
 * menu_categories.c. Reading and writing the file itself is main.c's job.
 */
#ifndef OH_MY_3DO_MENU_CATEGORIES_H
#define OH_MY_3DO_MENU_CATEGORIES_H
#include <stddef.h>

/* The largest categories.json the menu reads (one byte is kept for a zero). */
#define MENU_CATEGORIES_BYTES 8192

/* The most categories a file may list. */
#define MENU_CATEGORIES_MAX 32

/*
 * Goes back to the eight default categories, all shown, as if no file had
 * been read. menu_categories_configured then returns 0.
 */
void menu_categories_reset(void);

/*
 * Reads categories.json text. On success the new categories replace the
 * current ones and 0 is returned; on any error the current ones are kept and
 * -1 is returned.
 */
int menu_categories_parse(const char *text, size_t length);

/* How many categories there are. */
size_t menu_categories_count(void);

/* The name of category `index`, or "" if there is no such category. */
const char *menu_category_name(size_t index);

/* Whether category `index` is hidden. */
int menu_category_hidden(size_t index);

/*
 * Hides category `index` if it is shown, or shows it if hidden, by rewriting
 * its "hidden" value in the stored text. Returns 0, or -1 on failure.
 * Save the result with menu_categories_text.
 */
int menu_category_toggle(size_t index);

/* The current categories.json text, ready to write back to the drive. */
const char *menu_categories_text(void);

/*
 * Whether a path lies in a hidden category: "Demos/..." or
 * "/Storage/Demos/..." (or "/Storage/VOLUME/Demos/..." when the FCare shows a
 * volume folder first). Only whole folder names count: hiding "Demos" does
 * not hide "Demos 2" or "Games/Demos".
 */
int menu_categories_hidden_path(const char *path);

/* Whether `name` is the folder name of a hidden category. */
int menu_categories_hidden_name(const char *name);

/* Whether the categories came from the drive's file (1) or are the defaults (0). */
int menu_categories_configured(void);
#endif
