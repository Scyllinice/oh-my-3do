#ifndef OH_MY_3DO_MENU_CATEGORIES_H
#define OH_MY_3DO_MENU_CATEGORIES_H
#include <stddef.h>
#define MENU_CATEGORIES_BYTES 8192
#define MENU_CATEGORIES_MAX   32
/* File I/O is performed at the selected volume root by main.c. */
void menu_categories_reset(void);
int menu_categories_parse(const char *text, size_t length);
size_t menu_categories_count(void);
const char *menu_category_name(size_t index);
int menu_category_hidden(size_t index);
int menu_category_toggle(size_t index);
const char *menu_categories_text(void);
/* Matches the category component, never substrings or nested folder names. */
int menu_categories_hidden_path(const char *path);
int menu_categories_hidden_name(const char *name);
int menu_categories_configured(void);
#endif
