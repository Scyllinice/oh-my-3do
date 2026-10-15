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
int art_at_root(Ode *ode, const ArtRouteStep *route, unsigned int depth,
                 const OdeListing *current, int workaround,
                 ArtRootRead read, void *context);
#endif
