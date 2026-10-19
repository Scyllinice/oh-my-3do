#ifndef OH_MY_3DO_MENU_UI_H
#define OH_MY_3DO_MENU_UI_H
#include <stddef.h>

typedef struct MenuCover {
    const char *path;
    const char *file;
} MenuCover;
extern const MenuCover menu_covers[];
extern const size_t menu_cover_count;

size_t menu_title_length(const char *name, int directory);
/* Prefix-only form: remove trailing region/language tags and the extension.
 * Use menu_display_title when retained edition tags follow region tags. */
size_t menu_short_title_length(const char *name, int directory);
/* Hide only region/language suffix groups; retain all other designators. */
size_t menu_display_title(char *out,size_t capacity,const char *name,int directory);
const char *menu_image_format(const char *name, int directory);
size_t menu_scroll_offset(size_t length, size_t visible, unsigned long elapsed);
void menu_breadcrumb(char *out, const char *parent, const char *leaf,
                     unsigned int depth);
const char *menu_cover_lookup(const char *path);
const char *menu_cover_find(const MenuCover *covers, size_t count, const char *path);
int menu_support_entry(const char *name, int directory, int root);
/* With it set, the USB root's Adult folder is a hidden entry too. */
void menu_set_hide_adult(int hide);
/* A path inside the Adult category: Adult/... or /Storage/[VOLUME/]Adult/... */
int menu_adult_path(const char *path);
int menu_path_compare(const char *a, const char *b);
#endif
