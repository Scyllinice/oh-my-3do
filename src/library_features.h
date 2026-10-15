#ifndef OH_MY_3DO_LIBRARY_FEATURES_H
#define OH_MY_3DO_LIBRARY_FEATURES_H
#include "ode.h"
#define FAVORITES_BYTES 8192
#define FAVORITES_MAX 64
#define FAVORITE_PATH 1024
typedef struct MenuFavorites {
    char *storage;
    const char *paths[FAVORITES_MAX];
    size_t count;
} MenuFavorites;
void menu_favorites_free(MenuFavorites *favorites);
int menu_favorites_parse(MenuFavorites *favorites, const char *data, size_t size);
/* Canonical edit; preserves list on failure. 1 = already present/missing. */
int menu_favorites_edit(MenuFavorites *favorites, const char *path, int remove);
int menu_relative_path(const char *path);
/* Strict same-folder sets, contiguous disc numbers from 1; duplicates/gaps
 * reject the suggestion. Matching never launches or changes a playlist. */
size_t menu_disc_group(const OdeListing *listing, const MenuEntry *selected,
                        const MenuEntry **out, size_t capacity);
int menu_initial(const char *name); /* 0 = #, 1..26 = A..Z */
#endif
