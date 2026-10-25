/*
 * Reading a file from another folder without losing the current one.
 *
 * The FCare opens files relative to the folder the menu is in, and has no
 * "open this full path" command. To read a file in the drive root (an index,
 * say) while browsing Games/USA, the menu must climb to the root, read the
 * file, climb back down to Games/USA and check it really is back where it
 * started. These functions do that round trip.
 *
 * If the way back cannot be retraced and checked, browsing is no longer
 * safe: the menu might show one folder while the FCare is in another, and
 * launching a game would start the wrong disc. So any such failure sets
 * ode->navigation_failed, which stops all further USB use until the console
 * restarts, and returns -2. These functions never touch the disc queue or
 * launch anything.
 */
#ifndef OH_MY_3DO_ART_NAVIGATION_H
#define OH_MY_3DO_ART_NAVIGATION_H
#include "ode.h"

/* One folder on the path from the drive root to the current folder. */
typedef struct ArtRouteStep {
    unsigned long id; /* the folder's FCare id (see MenuEntry.toc_id) */
    const char *name; /* its name, to check the id still means the same folder */
} ArtRouteStep;

/* Reads whatever the caller needs once the right folder is open. Returns 0
 * on success or -1 if the file is unavailable. */
typedef int (*ArtRootRead)(void *context);

/*
 * Climbs from the current folder to the root, calls `read`, then returns to
 * the current folder and checks its listing still matches `current`.
 *
 * `route` lists the `depth` folders from the root down to the current one;
 * `workaround` is the folder workaround setting (see MENU_WORKAROUND_AUTO).
 * Returns what `read` returned, or -2 if the way back failed.
 */
int art_at_root(Ode *ode, const ArtRouteStep *route, unsigned int depth, const OdeListing *current,
                int workaround, ArtRootRead read, void *context);

/*
 * Enters `folder` (a subfolder of the current folder, such as a game folder
 * whose media sits inside it), calls `read`, then returns and checks the
 * current folder as art_at_root does. The `current` listing stays the
 * caller's. Returns what `read` returned, -1 if the folder could not be
 * entered, or -2 if the way back failed.
 */
int art_in_folder(Ode *ode, const OdeListing *current, const MenuEntry *folder, int workaround,
                  ArtRootRead read, void *context);
#endif
