#ifndef OH_MY_3DO_MENU_CONFIG_H
#define OH_MY_3DO_MENU_CONFIG_H

#include "ode.h"

#define MENU_CONFIG_BYTES 512
#define MENU_CONFIG_NAME  "ohmy3do.ini"
#define MENU_CONFIG_END   "; End of Oh My 3DO settings."
/* The FCare firmware ignores a folder change to the ID it last entered; the
 * menu detours through a file first when that would happen. AUTO (0, the
 * shipped value) applies it to the FCare ODE only. */
#define MENU_WORKAROUND_AUTO 0
#define MENU_WORKAROUND_ON   1
#define MENU_WORKAROUND_OFF  2
typedef struct MenuConfig {
    int folder_workaround; /* MENU_WORKAROUND_AUTO, _ON or _OFF */
    int usb_covers;
    int screen_margin;
    int large_titles;
    int remember_position;
    int recent_games;
    int video_previews;
    int preview_sound;
    /* Hides the Adult category folder and its games from browsing and Find. */
    int hide_adult;
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
