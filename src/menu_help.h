#ifndef MENU_HELP_H
#define MENU_HELP_H
#define MENU_HELP_ROWS    10
#define MENU_HELP_HOLD_MS 750UL
typedef enum MenuHelpTopic {
    HELP_LIBRARY,
    HELP_QUEUE,
    HELP_ALPHABET,
    HELP_TOOLS,
    HELP_SAVES,
    HELP_SLOTS,
    HELP_DESTINATION,
    HELP_ARCHIVE,
    HELP_HEX,
    HELP_HEX_BYTE,
    HELP_CHEATS,
    HELP_SETTINGS,
    HELP_CATEGORIES,
    HELP_KEYBOARD,
    HELP_FAVORITES,
    HELP_RECENT,
    HELP_RESULTS,
    HELP_COMPARE,
    HELP_COMPARE_BYTES,
    HELP_CONFIRM,
    HELP_DIAGNOSTICS,
    HELP_DETAILS,
    HELP_FAVORITE_MOVE,
    HELP_SLOT_JUMP,
    HELP_GENERAL,
    HELP_SLOT_FIND,
    HELP_BACKUP_DETAILS,
    HELP_COMPARE_PICK,
    HELP_TOPIC_COUNT
} MenuHelpTopic;
typedef struct MenuHelpHold {
    int pending, active;
    unsigned long since;
} MenuHelpHold;
/* 0: no action, 1: ordinary tap on release, 2: open help, 3: help active. */
int menu_help_poll(MenuHelpHold *hold, unsigned long now, int down, int pressed, int released,
                   int other);
const char *menu_help_title(MenuHelpTopic topic);
const char *menu_help_line(MenuHelpTopic topic, unsigned int row);
#endif
