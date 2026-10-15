#ifndef OH_MY_3DO_MENU_POSITION_H
#define OH_MY_3DO_MENU_POSITION_H
#include "ode.h"
#define POSITION_BYTES 9216
typedef struct MenuPosition {
    char folder[8193];
    char selected[ODE_MAX_NAME];
} MenuPosition;
int menu_position_parse(MenuPosition *position, const char *text, size_t length);
int menu_position_format(const MenuPosition *position, char *out, size_t capacity);
#endif
