#ifndef OH_MY_3DO_MENU_SETTINGS_H
#define OH_MY_3DO_MENU_SETTINGS_H
#include "usb_file.h"
/* Fixed zero-padded INI prevents stale tails without firmware truncate/seek. */
int menu_settings_save(Ode *ode, OdeReadBuffer reader, const MenuConfig *config);
#endif
