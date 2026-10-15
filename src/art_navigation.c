#include "art_navigation.h"
#include <string.h>

int art_at_root(Ode *ode, const ArtRouteStep *route, unsigned int depth,
                 const OdeListing *current, int workaround,
                 ArtRootRead read, void *context)
{
    unsigned int first = depth && !(route[0].id & 0xffffffUL) ? 1 : 0;
    unsigned int i;
    int result = -2;
    OdeProgress progress = ode->progress;
    OdeListing listing;
    ode_listing_init(&listing);
    ode->progress = NULL;
    for (i = depth; i > first; --i)
        if (ode_change_directory(ode, (route[i - 1].id & 0xff000000UL) | 0xffffffUL) < 0) goto failed;
    result = read(context);
    if (ode->navigation_failed) goto failed;
    for (i = first; i < depth; ++i) {
        const MenuEntry *entry = NULL;
        size_t j;
        if (ode_read_directory(ode, &listing) < 0) goto failed;
        for (j = 0; j < listing.count; ++j)
            if (listing.entries[j]->toc_id == route[i].id && listing.entries[j]->is_directory &&
                !strcmp(listing.entries[j]->name, route[i].name)) entry = listing.entries[j];
        if (!entry || ode_enter_directory(ode, &listing, entry, workaround) < 0) goto failed;
        ode_listing_free(&listing);
    }
    if (depth > first) {
        size_t j;
        if (ode_read_directory(ode, &listing) < 0 || listing.count != current->count ||
            listing.has_parent != current->has_parent ||
            (listing.has_parent && listing.parent_id != current->parent_id)) goto failed;
        for (j = 0; j < listing.count; ++j)
            if (listing.entries[j]->toc_id != current->entries[j]->toc_id ||
                listing.entries[j]->is_directory != current->entries[j]->is_directory ||
                strcmp(listing.entries[j]->name, current->entries[j]->name)) goto failed;
    }
    ode_listing_free(&listing);
    ode->progress = progress;
    return result;
failed:
    ode_listing_free(&listing);
    ode->progress = progress;
    ode->navigation_failed = 1;
    ode->error = "USB folder restore failed";
    return -2;
}
