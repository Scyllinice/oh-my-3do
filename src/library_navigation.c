#include "library_navigation.h"
#include "menu_ui.h"
#include <stdlib.h>
#include <string.h>
static int same_listing(const OdeListing *a, const OdeListing *b)
{
    size_t i;
    if (a->count != b->count || a->has_parent != b->has_parent || (a->has_parent && a->parent_id != b->parent_id)) return 0;
    for (i = 0; i < a->count; ++i)
        if (a->entries[i]->toc_id != b->entries[i]->toc_id || a->entries[i]->is_directory != b->entries[i]->is_directory || strcmp(a->entries[i]->name,b->entries[i]->name)) return 0;
    return 1;
}
static int descend(Ode *ode, const ArtRouteStep *route, unsigned int depth, int workaround)
{
    OdeListing listing;
    unsigned int i;
    ode_listing_init(&listing);
    for (i = 0; i < depth; ++i) {
        size_t j;
        if (ode_read_directory(ode,&listing) < 0) goto bad;
        for (j = 0; j < listing.count; ++j)
            if (listing.entries[j]->toc_id == route[i].id && listing.entries[j]->is_directory && !strcmp(listing.entries[j]->name,route[i].name)) break;
        if (j == listing.count || ode_enter_directory(ode,&listing,listing.entries[j],workaround) < 0) goto bad;
        ode_listing_free(&listing);
    }
    return 0;
bad:
    ode_listing_free(&listing); return -1;
}
static int navigate(Ode *ode, const ArtRouteStep *route, unsigned int depth,
                     const OdeListing *current, int workaround, const char *path,
                     LibraryFileAction action, void *context, LibraryLocation *keep)
{
    OdeListing listing;
    unsigned long parents[LIBRARY_DEPTH];
    OdeProgress progress = ode->progress;
    unsigned int entered = 0, i;
    char *copy = NULL, *part, *slash;
    const char *directory_end;
    int result = -1;
    size_t n;
    ode_listing_init(&listing);
    if (ode->navigation_failed || depth > LIBRARY_DEPTH || strncmp(path,"/Storage/",9)) return -1;
    n = strlen(path);
    if (n > 9216) return -1;
    copy = (char *)malloc(n + 1);
    if (!copy) { ode->error = "Not enough navigation memory"; return -1; }
    memcpy(copy,path,n+1);
    /* Validate every component before changing firmware state. */
    part = copy + 9;
    for (;;) {
        slash = strchr(part,'/'); if (slash) *slash = 0;
        if (!*part || !strcmp(part,".") || !strcmp(part,"..") || strlen(part) >= ODE_MAX_NAME || strchr(part,'\\') || strchr(part,':')) goto done;
        if (!slash) break;
        *slash = '/'; part = slash + 1;
    }
    ode->progress = NULL;
    for (i = depth; i; --i)
        if (ode_change_directory(ode,(route[i-1].id & 0xff000000UL) | 0xffffffUL) < 0) goto fatal;
    part = copy + 9;
    directory_end = strrchr(path,'/');
    for (;;) {
        size_t j;
        slash = strchr(part,'/'); if (slash) *slash = 0;
        if (ode_read_directory(ode,&listing) < 0) goto fatal;
        for (j = 0; j < listing.count; ++j)
            if (!menu_path_compare(listing.entries[j]->name,part) && !!listing.entries[j]->is_directory == !!slash) break;
        if (j == listing.count) { ode->error = "Queued/favorite path is missing"; goto restore; }
        if (!slash) {
            if (keep) {
                size_t k;
                int root = entered == 0 || (entered == 1 && !(keep->ids[0] & 0xffffffUL));
                if (menu_support_entry(listing.entries[j]->name,0,root)) {
                    ode->error = "Favorite is a menu support file"; goto restore;
                }
                keep->listing = listing; ode_listing_init(&listing);
                keep->selected = j + (keep->listing.has_parent ? 1 : 0);
                for (k = 0; k < j; ++k) {
                    const MenuEntry *e = keep->listing.entries[k];
                    if (!(e->is_directory && !(e->toc_id & 0xffffffUL)) && menu_support_entry(e->name,e->is_directory,root)) --keep->selected;
                }
                keep->depth = entered;
                result = 0; goto done;
            } else {
                char directory[9218];
                size_t length = (size_t)(directory_end - path);
                memcpy(directory,path,length); directory[length] = 0;
                result = action ? action(context,listing.entries[j],directory) : 0;
                goto restore;
            }
        }
        if (entered == LIBRARY_DEPTH) { ode->error = "Maximum folder depth reached"; goto restore; }
        parents[entered] = listing.has_parent ? listing.parent_id : (listing.entries[j]->toc_id & 0xff000000UL) | 0xffffffUL;
        if (keep) {
            strcpy(keep->names[entered],listing.entries[j]->name);
            keep->ids[entered] = listing.entries[j]->toc_id;
            {
                size_t k;
                int root = entered == 0 || (entered == 1 && !(keep->ids[0] & 0xffffffUL));
                keep->previous[entered] = j + (listing.has_parent ? 1 : 0);
                for (k = 0; k < j; ++k) {
                    const MenuEntry *e = listing.entries[k];
                    if (!(e->is_directory && !(e->toc_id & 0xffffffUL)) && menu_support_entry(e->name,e->is_directory,root)) --keep->previous[entered];
                }
            }
        }
        if (ode_enter_directory(ode,&listing,listing.entries[j],workaround) < 0) {
            if (ode->navigation_failed) goto fatal;
            goto restore;
        }
        ++entered; ode_listing_free(&listing); part = slash + 1;
    }
restore:
    ode_listing_free(&listing);
    if (ode->navigation_failed) goto fatal;
    while (entered) if (ode_change_directory(ode,parents[--entered]) < 0) goto fatal;
    if (descend(ode,route,depth,workaround) < 0 || ode_read_directory(ode,&listing) < 0 || !same_listing(&listing,current)) goto fatal;
    goto done;
fatal:
    ode->navigation_failed = 1; ode->error = "Library folder restore failed"; result = -2;
done:
    ode_listing_free(&listing); free(copy); ode->progress = progress; return result;
}
int library_visit(Ode *ode, const ArtRouteStep *route, unsigned int depth,
                   const OdeListing *current, int workaround, const char *path,
                   LibraryFileAction action, void *context)
{
    return navigate(ode,route,depth,current,workaround,path,action,context,NULL);
}
int library_open(Ode *ode, const ArtRouteStep *route, unsigned int depth,
                  const OdeListing *current, int workaround, const char *path,
                  LibraryLocation *location)
{
    memset(location,0,sizeof(*location));
    return navigate(ode,route,depth,current,workaround,path,NULL,NULL,location);
}
