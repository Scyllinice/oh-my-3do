#ifndef OH_MY_3DO_MENU_CONFIG_H
#define OH_MY_3DO_MENU_CONFIG_H

#include "ode.h"

#define MENU_CONFIG_BYTES 512
#define MENU_CONFIG_NAME "ohmy3do.ini"
typedef struct MenuConfig {
    int folder_workaround;
    int usb_covers;
    int screen_margin;
    int large_titles;
    int theme;
} MenuConfig;

/* Reads the E7 data FIFO and validates its status reply, exactly 2048 bytes. */
typedef int (*OdeReadBuffer)(void *context, unsigned char *data, size_t size);
void menu_config_defaults(MenuConfig *config);
int menu_config_parse(MenuConfig *config, const char *data, size_t length);
/* Call at the boot USB root. Unknown firmware identities are left untouched.
 * Returns 0 on success, 1 if unsupported, -1 on invalid config/I/O failure.
 * All unsuccessful results leave the workaround disabled. */
int menu_config_load(Ode *ode, OdeReadBuffer read_buffer, MenuConfig *config);

#endif
