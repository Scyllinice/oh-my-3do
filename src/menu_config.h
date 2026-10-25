/*
 * The menu's settings, kept in .ohmy3do/ohmy3do.ini on the drive.
 *
 * The file is a small INI file people may also edit on a PC:
 *
 *     ; Saved by Oh My 3DO.
 *     [compatibility]
 *     folder_workaround=0
 *     [artwork]
 *     usb_covers=1
 *     ...
 *     ; End of Oh My 3DO settings.
 *
 * Every setting is a small whole number. Missing settings keep their
 * defaults; anything unexpected (an unknown key, a value out of range, a
 * setting given twice) makes the whole file invalid, and the menu then runs
 * with defaults rather than guessing.
 */
#ifndef OH_MY_3DO_MENU_CONFIG_H
#define OH_MY_3DO_MENU_CONFIG_H

#include "ode.h"

/*
 * The settings file is always exactly this size when the menu writes it
 * (see menu_settings.c), and the menu never reads more than this.
 */
#define MENU_CONFIG_BYTES 512
#define MENU_CONFIG_NAME  "ohmy3do.ini"

/* The last line the menu writes. Anything after it is padding and ignored. */
#define MENU_CONFIG_END "; End of Oh My 3DO settings."

/*
 * The FCare firmware ignores a request to enter the folder it entered last,
 * which would leave the menu showing the wrong folder. The workaround detours
 * through another entry first whenever that would happen. AUTO (the shipped
 * value) uses it only on the FCare, ON always, OFF never.
 */
#define MENU_WORKAROUND_AUTO 0
#define MENU_WORKAROUND_ON   1
#define MENU_WORKAROUND_OFF  2

/* The largest screen_margin, in pixels on each side. */
#define MENU_MARGIN_MAX 12

typedef struct MenuConfig {
    int folder_workaround; /* MENU_WORKAROUND_AUTO, _ON or _OFF */
    int usb_covers;        /* 1: show covers from the drive */
    int screen_margin;     /* 0 to 12: pixels kept clear at the screen edges for TV overscan */
    int large_titles;      /* 1: bigger text in the game list */
    int remember_position; /* 1: reopen the last folder at startup */
    int recent_games;      /* 1: keep a list of recently played games */
    int video_previews;    /* 1: play gameplay previews */
    int preview_sound;     /* 1: play previews with sound */
    /* 1: hide the Adult category folder and its games from browsing and Find. */
    int hide_adult;
} MenuConfig;

/*
 * Reads the FCare's 2048-byte data buffer into `data` (the E7 command) and
 * checks its status reply. The console's own USB code provides this; tests
 * substitute their own.
 */
typedef int (*OdeReadBuffer)(void *context, unsigned char *data, size_t size);

/* Fills `config` with the shipped defaults. */
void menu_config_defaults(MenuConfig *config);

/*
 * Reads settings from `length` bytes of INI text. Returns 0, or -1 if the
 * text is invalid, in which case `config` holds the defaults.
 */
int menu_config_parse(MenuConfig *config, const char *data, size_t length);

/*
 * Loads the settings file from the drive. Call it while the drive's root
 * folder is open, before browsing.
 *
 * Returns 0 when the settings loaded, 1 when the drive is not an FCare (whose
 * commands this needs, so nothing is sent and defaults are used), or -1 when
 * the file is invalid or a USB command failed. Whenever it does not return 0,
 * the folder workaround is left on AUTO.
 */
int menu_config_load(Ode *ode, OdeReadBuffer read_buffer, MenuConfig *config);

#endif
