#include "art_navigation.h"
#include <string.h>

int art_in_folder(Ode *ode,const OdeListing *current,const MenuEntry *folder,
                  int workaround,ArtRootRead read,void *context)
{
    OdeListing restored;
    OdeProgress progress=ode->progress;
    const char *media_folder=ode->media_folder;
    int result=-1;
    size_t i;
    if (ode->navigation_failed) return -2;
    if (!folder || !folder->is_directory) return -1;
    ode_listing_init(&restored);
    ode->progress=NULL;
    if (ode_enter_directory(ode,current,folder,workaround)<0) {
        if (ode->navigation_failed) goto failed;
        goto done;
    }
    ode->media_folder=NULL;
    result=read(context);
    ode->media_folder=media_folder;
    /* After a partial exchange, do not send a parent command into an
     * unsynchronized FIFO. The caller disables browsing until cold boot. */
    if (ode->navigation_failed) goto failed;
    if (ode_change_directory(ode,(folder->toc_id&0xff000000UL)|0xffffffUL)<0 ||
        ode_read_directory(ode,&restored)<0 || restored.count!=current->count ||
        restored.has_parent!=current->has_parent ||
        (restored.has_parent && restored.parent_id!=current->parent_id)) goto failed;
    for (i=0;i<restored.count;++i)
        if (restored.entries[i]->toc_id!=current->entries[i]->toc_id ||
            restored.entries[i]->is_directory!=current->entries[i]->is_directory ||
            strcmp(restored.entries[i]->name,current->entries[i]->name)) goto failed;
done:
    ode_listing_free(&restored);
    ode->progress=progress;
    return result;
failed:
    ode->navigation_failed=1;
    ode->error="USB game folder restore failed";
    result=-2;
    goto done;
}

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
        /* A short, freshly checked record is sufficient when no collision
         * detour is needed. Unknown firmware and equal local IDs keep the
         * complete listing path used by the compatibility workaround. */
        if (!strcmp(ode->identity,"LOCODE") && (!workaround ||
            (ode->navigation_known && ode->navigation_key!=(route[i].id&0xffffffUL)))) {
            int matched=ode_probe_directory(ode,route[i].id,route[i].name);
            if (matched<0) goto failed;
            if (matched) {
                if (ode_change_directory(ode,route[i].id)<0) goto failed;
                continue;
            }
        }
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
