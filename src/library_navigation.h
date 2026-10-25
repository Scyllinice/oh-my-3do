#ifndef OH_MY_3DO_LIBRARY_NAVIGATION_H
#define OH_MY_3DO_LIBRARY_NAVIGATION_H
#include "art_navigation.h"
#define LIBRARY_DEPTH 32
typedef struct LibraryLocation {
    OdeListing listing;
    char names[LIBRARY_DEPTH][ODE_MAX_NAME];
    unsigned long ids[LIBRARY_DEPTH];
    size_t previous[LIBRARY_DEPTH];
    unsigned int depth;
    size_t selected;
} LibraryLocation;
typedef int (*LibraryFileAction)(void *context, const MenuEntry *entry, const char *directory);
/* Absolute /Storage paths include a volume label on multi-volume systems.
 * A visit restores and verifies the original folder; open keeps the resolved
 * parent listing and route on success. Missing paths never launch anything. */
int library_visit(Ode *ode, const ArtRouteStep *route, unsigned int depth,
                   const OdeListing *current, int workaround, const char *path,
                   LibraryFileAction action, void *context);
int library_open(Ode *ode, const ArtRouteStep *route, unsigned int depth,
                  const OdeListing *current, int workaround, const char *path,
                  LibraryLocation *location);
int library_open_directory(Ode *ode, const ArtRouteStep *route, unsigned int depth,
                  const OdeListing *current, int workaround, const char *path,
                  LibraryLocation *location);
/* Like library_open, for a folder: its parent opens with it selected. */
int library_select_directory(Ode *ode, const ArtRouteStep *route, unsigned int depth,
                  const OdeListing *current, int workaround, const char *path,
                  LibraryLocation *location);
/* Resolve all paths against an already current directory, without I/O.
 * 1 = all local, 0 = includes another directory, -1 = local file missing. */
int library_current_files(const OdeListing *listing,const char *directory,
    char *const *paths,size_t count,const MenuEntry **entries);
#define LIBRARY_CATALOG_MAX 2048
#define LIBRARY_CATALOG_BYTES (128UL*1024UL)
typedef struct LibraryCatalog {
    char **paths;
    size_t count, bytes;
    int valid, truncated;
} LibraryCatalog;
typedef int (*LibraryScanProgress)(void *context, size_t games);
void library_catalog_free(LibraryCatalog *catalog);
/* Build once on demand. No file writes or mount/queue commands. Cancellation
 * and ordinary allocation/limit failures unwind and verify the original folder.
 * Returns 0 on success (possibly truncated), 1 on cancel, -1 on allocation
 * failure, -2 when folder state cannot be restored safely. */
int library_catalog_build(Ode *ode, const ArtRouteStep *route, unsigned int depth,
    const OdeListing *current, int workaround, LibraryCatalog *catalog,
    LibraryScanProgress progress, void *context);
size_t library_catalog_count(const LibraryCatalog *catalog, const char *query);
const char *library_catalog_match(const LibraryCatalog *catalog, const char *query, size_t index);
#endif
