#ifndef OH_MY_3DO_ART_NAVIGATION_H
#define OH_MY_3DO_ART_NAVIGATION_H
#include "ode.h"

typedef struct ArtRouteStep {
    unsigned long id;
    const char *name;
} ArtRouteStep;
typedef int (*ArtRootRead)(void *context);
/* Returns -2 if navigation cannot be restored and verified; never clear or
 * launch a playlist. The callback returns 0 on art success, -1 if unavailable. */
int art_at_root(Ode *ode, const ArtRouteStep *route, unsigned int depth, const OdeListing *current,
                int workaround, ArtRootRead read, void *context);
/* Read media inside a game folder using a short local filename. The browser
 * listing stays owned by its caller; restore and verify its directory before
 * returning. A transport failure blocks further navigation and returns -2. */
int art_in_folder(Ode *ode, const OdeListing *current, const MenuEntry *folder, int workaround,
                  ArtRootRead read, void *context);
#endif
