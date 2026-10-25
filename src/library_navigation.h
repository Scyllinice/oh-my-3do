/*
 * Going straight to a game or folder by its full path (for Find, Favorites,
 * Recent games and the disc queue), and scanning the whole drive to build
 * the list Find searches.
 *
 * Full paths start at "/Storage", the menu's name for the top of the drive:
 * "/Storage/Games/USA/Road Rash (USA)/Road Rash (USA).iso". On drives with
 * several volumes, the volume's name comes next: "/Storage/VOLUME/Games/...".
 *
 * All of these start from the folder the menu is showing, described by a
 * route (see art_navigation.h) and its current listing. They either end in a
 * new folder or return to the original one and check it; if that check
 * fails they return -2 and set ode->navigation_failed (see
 * art_navigation.h). A missing path never launches anything.
 */
#ifndef OH_MY_3DO_LIBRARY_NAVIGATION_H
#define OH_MY_3DO_LIBRARY_NAVIGATION_H
#include "art_navigation.h"

/* The deepest folder nesting the menu follows. */
#define LIBRARY_DEPTH 32

/* Where the menu ends up after opening a path: what the browser needs to
 * show the new folder. */
typedef struct LibraryLocation {
    OdeListing listing;                      /* the new folder's entries */
    char names[LIBRARY_DEPTH][ODE_MAX_NAME]; /* the route to it: folder names... */
    unsigned long ids[LIBRARY_DEPTH];        /* ...and their ids */
    /* For each folder on the route, its position in its parent's list, so
     * pressing Back returns to it selected. */
    size_t previous[LIBRARY_DEPTH];
    unsigned int depth; /* folders below the top */
    size_t selected;    /* the row to select in the new folder */
} LibraryLocation;

/* Called by library_visit on the file it found, with the full path of the
 * file's folder. Its return value is passed back. */
typedef int (*LibraryFileAction)(void *context, const MenuEntry *entry, const char *directory);

/*
 * Goes to the file at `path`, calls `action` on it there, then returns to
 * the original folder and checks it. Used to queue a disc from another
 * folder, for example.
 */
int library_visit(Ode *ode, const ArtRouteStep *route, unsigned int depth,
                  const OdeListing *current, int workaround, const char *path,
                  LibraryFileAction action, void *context);

/*
 * Opens the folder holding the file at `path`, with the file selected, and
 * stays there: on success `location` describes the new folder. On failure
 * the original folder is restored.
 */
int library_open(Ode *ode, const ArtRouteStep *route, unsigned int depth, const OdeListing *current,
                 int workaround, const char *path, LibraryLocation *location);

/* Like library_open, but `path` is a folder, which is opened ("/Storage"
 * itself opens the top of the drive). */
int library_open_directory(Ode *ode, const ArtRouteStep *route, unsigned int depth,
                           const OdeListing *current, int workaround, const char *path,
                           LibraryLocation *location);

/* Like library_open, but `path` is a folder: its parent opens with it selected. */
int library_select_directory(Ode *ode, const ArtRouteStep *route, unsigned int depth,
                             const OdeListing *current, int workaround, const char *path,
                             LibraryLocation *location);

/*
 * Finds several files in the folder that is already open, without any USB
 * commands, when all of `paths` are in `directory` (the open folder's full
 * path). Fills `entries` in the same order. Returns 1 when all were found, 0
 * when some are in another folder (use library_visit instead), or -1 when
 * one is missing from this folder.
 */
int library_current_files(const OdeListing *listing, const char *directory, char *const *paths,
                          size_t count, const MenuEntry **entries);

/* Limits on the Find list: entries, and total bytes of path text. */
#define LIBRARY_CATALOG_MAX   2048
#define LIBRARY_CATALOG_BYTES (128UL * 1024UL)

/* Every disc on the drive, as full paths, for Find. */
typedef struct LibraryCatalog {
    char **paths;
    size_t count;
    size_t bytes;  /* total length of the paths, counted against the limit */
    int valid;     /* the list has been loaded or built */
    int truncated; /* the drive held more than the limits allow */
} LibraryCatalog;

/* Called during a scan with the number of discs found so far. Returning
 * nonzero cancels the scan. */
typedef int (*LibraryScanProgress)(void *context, size_t games);

/* Frees the list and leaves it empty. */
void library_catalog_free(LibraryCatalog *catalog);

/*
 * Builds the Find list by visiting every folder on the drive (used when the
 * drive has no library.3di index). Only reads: it never writes files,
 * changes the disc queue or launches anything. Whatever happens, it returns
 * to the original folder and checks it. The old list is replaced only on
 * success.
 *
 * Returns 0 on success (possibly truncated), 1 if canceled, -1 if memory
 * ran out, or -2 if the original folder could not be restored.
 */
int library_catalog_build(Ode *ode, const ArtRouteStep *route, unsigned int depth,
                          const OdeListing *current, int workaround, LibraryCatalog *catalog,
                          LibraryScanProgress progress, void *context);

/* How many discs' file names contain `query` (ignoring case). */
size_t library_catalog_count(const LibraryCatalog *catalog, const char *query);

/* The full path of the index-th disc whose file name contains `query`, or
 * NULL if there are not that many. */
const char *library_catalog_match(const LibraryCatalog *catalog, const char *query, size_t index);
#endif
