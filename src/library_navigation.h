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
#endif
