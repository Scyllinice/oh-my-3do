/*
 * On-screen help: holding P on any screen shows that screen's controls.
 *
 * P (the "play" button) also has its own action on many screens, so a quick
 * tap must still do that. menu_help_poll tells the two apart: a tap is passed
 * on when P is released, and only holding P for MENU_HELP_HOLD_MS opens help.
 */
#ifndef MENU_HELP_H
#define MENU_HELP_H

/* Lines of help per screen. */
#define MENU_HELP_ROWS 10

/* How long P must be held to open help, in milliseconds. */
#define MENU_HELP_HOLD_MS 750UL

/* The screens that have help, one per entry in menu_help.c's tables. */
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

/* Tracks one press of P while deciding whether it is a tap or a hold. */
typedef struct MenuHelpHold {
    int pending;         /* P is down and might still become a hold */
    int active;          /* help is showing; the caller clears this when it closes */
    unsigned long since; /* when P went down, in milliseconds */
} MenuHelpHold;

/* What menu_help_poll asks the caller to do. */
#define MENU_HELP_NOTHING 0 /* carry on; while pending, ignore this press of P */
#define MENU_HELP_TAP     1 /* P was tapped: do P's normal action now */
#define MENU_HELP_OPEN    2 /* P was held: open help for the current screen */
#define MENU_HELP_SHOWING 3 /* help is already showing */

/*
 * Call once per frame with the current time and P's state: `down` (held
 * now), `pressed` (went down this frame) and `released` (went up this
 * frame), plus `other`, whether any other button is held. Pressing another
 * button with P cancels both the tap and the hold, so combinations such as
 * C + P keep working. Returns one of the MENU_HELP_ values above.
 */
int menu_help_poll(MenuHelpHold *hold, unsigned long now, int down, int pressed, int released,
                   int other);

/* The heading for a screen's help ("Controls" for an unknown topic). */
const char *menu_help_title(MenuHelpTopic topic);

/* One line of a screen's help; "" past the last line or for an unknown topic. */
const char *menu_help_line(MenuHelpTopic topic, unsigned int row);
#endif
