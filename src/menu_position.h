/*
 * The remembered position: which folder was open and which entry was
 * selected, so the menu can reopen there at the next startup (when "Remember
 * position" is on). It is saved as .ohmy3do/last-position.txt:
 *
 *     OH3DO_POSITION1
 *     /Storage/Games/USA
 *     Road Rash (USA)
 *
 * The folder is a full menu path, starting at /Storage (the FCare's name for
 * the drive). The last line may be empty when nothing was selected.
 */
#ifndef OH_MY_3DO_MENU_POSITION_H
#define OH_MY_3DO_MENU_POSITION_H
#include "ode.h"

/* The largest position file the menu reads or writes. */
#define POSITION_BYTES 9216

typedef struct MenuPosition {
    char folder[8193];           /* e.g. "/Storage/Games/USA" */
    char selected[ODE_MAX_NAME]; /* name of the selected entry, or "" */
} MenuPosition;

/*
 * Reads a position file. An empty file is a valid "no position" and leaves
 * `position` empty. Returns 0, or -1 for a damaged file or one naming an
 * unsafe path (such as ".." or a name with a slash), which also leaves
 * `position` empty.
 */
int menu_position_parse(MenuPosition *position, const char *text, size_t length);

/*
 * Writes a position file as zero-terminated text. Returns 0, or -1 if the
 * position is not valid or does not fit in `capacity` bytes.
 */
int menu_position_format(const MenuPosition *position, char *out, size_t capacity);
#endif
