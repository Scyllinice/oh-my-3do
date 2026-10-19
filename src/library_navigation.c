#include "library_navigation.h"
#include "menu_ui.h"
#include "library_features.h"
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
                     LibraryFileAction action, void *context, LibraryLocation *keep, int folder)
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
    if (ode->navigation_failed || depth > LIBRARY_DEPTH || (strncmp(path,"/Storage/",9) && !(folder && !strcmp(path,"/Storage")))) return -1;
    n = strlen(path);
    if (n > 9216) return -1;
    copy = (char *)malloc(n + 1);
    if (!copy) { ode->error = "Not enough navigation memory"; return -1; }
    memcpy(copy,path,n+1);
    /* Validate every component before changing firmware state. */
    part = copy + 9;
    if (!(folder && n == 8)) for (;;) {
        slash = strchr(part,'/'); if (slash) *slash = 0;
        if (!*part || !strcmp(part,".") || !strcmp(part,"..") || strlen(part) >= ODE_MAX_NAME || strchr(part,'\\') || strchr(part,':')) goto done;
        if (!slash) break;
        *slash = '/'; part = slash + 1;
    }
    ode->progress = NULL;
    for (i = depth; i; --i)
        if (ode_change_directory(ode,(route[i-1].id & 0xff000000UL) | 0xffffffUL) < 0) goto fatal;
    if (folder && n == 8) {
        if (ode_read_directory(ode,&listing) < 0) goto fatal;
        keep->listing = listing; ode_listing_init(&listing); keep->depth = 0; keep->selected = 0;
        result = 0; goto done;
    }
    part = copy + 9;
    directory_end = strrchr(path,'/');
    for (;;) {
        size_t j;
        slash = strchr(part,'/'); if (slash) *slash = 0;
        if (ode_read_directory(ode,&listing) < 0) goto fatal;
        for (j = 0; j < listing.count; ++j)
            if (!menu_path_compare(listing.entries[j]->name,part) && !!listing.entries[j]->is_directory == !!(slash || folder)) break;
        if (j == listing.count) { ode->error = "Queued/favorite path is missing"; goto restore; }
        if (!slash && !folder) {
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
        ++entered; ode_listing_free(&listing);
        if (!slash) {
            if (ode_read_directory(ode,&listing) < 0) goto fatal;
            keep->listing = listing; ode_listing_init(&listing); keep->depth = entered; keep->selected = 0;
            result = 0; goto done;
        }
        part = slash + 1;
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
    return navigate(ode,route,depth,current,workaround,path,action,context,NULL,0);
}
int library_open(Ode *ode, const ArtRouteStep *route, unsigned int depth,
                  const OdeListing *current, int workaround, const char *path,
                  LibraryLocation *location)
{
    memset(location,0,sizeof(*location));
    return navigate(ode,route,depth,current,workaround,path,NULL,NULL,location,0);
}

int library_open_directory(Ode *ode, const ArtRouteStep *route, unsigned int depth,
                  const OdeListing *current, int workaround, const char *path,
                  LibraryLocation *location)
{
    memset(location,0,sizeof(*location));
    return navigate(ode,route,depth,current,workaround,path,NULL,NULL,location,1);
}

void library_catalog_free(LibraryCatalog *catalog)
{
    size_t i;
    for (i=0;i<catalog->count;++i) free(catalog->paths[i]);
    free(catalog->paths); memset(catalog,0,sizeof(*catalog));
}
static int image_name(const char *name)
{
    size_t n=strlen(name);
    return n>4 && (!menu_path_compare(name+n-4,".iso") || !menu_path_compare(name+n-4,".cue"));
}
static int catalog_matches(const char *path,const char *query)
{
    const char *name=strrchr(path,'/');
    return menu_name_matches(name ? name+1 : path,query);
}
size_t library_catalog_count(const LibraryCatalog *catalog,const char *query)
{
    size_t i, count=0;
    for (i=0;i<catalog->count;++i) if (catalog_matches(catalog->paths[i],query)) ++count;
    return count;
}
const char *library_catalog_match(const LibraryCatalog *catalog,const char *query,size_t index)
{
    size_t i;
    for (i=0;i<catalog->count;++i)
        if (catalog_matches(catalog->paths[i],query) && !index--) return catalog->paths[i];
    return NULL;
}
typedef struct ScanFrame {
    OdeListing listing;
    size_t next, length;
    unsigned long parent;
    int root;
} ScanFrame;
int library_catalog_build(Ode *ode,const ArtRouteStep *route,unsigned int depth,
    const OdeListing *current,int workaround,LibraryCatalog *catalog,
    LibraryScanProgress progress,void *context)
{
    ScanFrame *frames;
    char *path;
    LibraryCatalog next;
    OdeProgress old_progress=ode->progress;
    unsigned int level=0, i;
    int result=-1;
    if (ode->navigation_failed || depth>LIBRARY_DEPTH) return -2;
    memset(&next,0,sizeof(next));
    frames=(ScanFrame *)calloc(LIBRARY_DEPTH+1,sizeof(*frames));
    path=(char *)malloc(9218);
    next.paths=(char **)calloc(LIBRARY_CATALOG_MAX,sizeof(*next.paths));
    if (!frames || !path || !next.paths) goto done;
    ode->progress=NULL;
    for (i=depth;i;--i)
        if (ode_change_directory(ode,(route[i-1].id&0xff000000UL)|0xffffffUL)<0) goto fatal;
    strcpy(path,"/Storage"); frames[0].length=8; frames[0].root=1;
    if (ode_read_directory(ode,&frames[0].listing)<0) goto fatal;
    for (;;) {
        ScanFrame *frame=&frames[level];
        const MenuEntry *entry;
        size_t n;
        if (progress && progress(context,next.count)) { result=1; goto restore; }
        if (frame->next==frame->listing.count) {
            if (!level) { result=0; goto restore; }
            if (ode_change_directory(ode,frame->parent)<0) goto fatal;
            ode_listing_free(&frame->listing); --level;
            path[frames[level].length]=0;
            continue;
        }
        entry=frame->listing.entries[frame->next++];
        if (menu_support_entry(entry->name,entry->is_directory,frame->root) &&
            !(entry->is_directory && !(entry->toc_id&0xffffffUL))) continue;
        n=strlen(entry->name);
        if (!n || !strcmp(entry->name,".") || !strcmp(entry->name,"..") ||
            strchr(entry->name,'/') || strchr(entry->name,'\\') || strchr(entry->name,':')) continue;
        if (frame->length+n+1>9216 || (entry->is_directory && level==LIBRARY_DEPTH)) {
            next.truncated=1; continue;
        }
        path[frame->length]='/'; strcpy(path+frame->length+1,entry->name);
        if (entry->is_directory) {
            ScanFrame *child=&frames[level+1];
            child->length=frame->length+n+1; child->next=0;
            child->root=!(entry->toc_id&0xffffffUL);
            child->parent=frame->listing.has_parent ? frame->listing.parent_id :
                (entry->toc_id&0xff000000UL)|0xffffffUL;
            if (ode_enter_directory(ode,&frame->listing,entry,workaround)<0) goto fatal;
            ++level;
            if (ode_read_directory(ode,&child->listing)<0) goto fatal;
        } else {
            if (image_name(entry->name)) {
                n=strlen(path)+1;
                if (next.count==LIBRARY_CATALOG_MAX || next.bytes+n>LIBRARY_CATALOG_BYTES) {
                    next.truncated=1; result=0; goto restore;
                }
                next.paths[next.count]=(char *)malloc(n);
                if (!next.paths[next.count]) { result=-1; goto restore; }
                memcpy(next.paths[next.count++],path,n); next.bytes+=n;
            }
            path[frame->length]=0;
        }
    }
restore:
    while (level) {
        if (ode_change_directory(ode,frames[level].parent)<0) goto fatal;
        ode_listing_free(&frames[level--].listing);
    }
    ode_listing_free(&frames[0].listing);
    if (descend(ode,route,depth,workaround)<0 ||
        ode_read_directory(ode,&frames[0].listing)<0 || !same_listing(&frames[0].listing,current)) goto fatal;
    if (!result) {
        next.valid=1; library_catalog_free(catalog); *catalog=next; memset(&next,0,sizeof(next));
    }
    goto done;
fatal:
    ode->navigation_failed=1; ode->error="Search folder restore failed"; result=-2;
done:
    ode->progress=old_progress;
    if (frames) for (i=0;i<=LIBRARY_DEPTH;++i) ode_listing_free(&frames[i].listing);
    free(frames); free(path); library_catalog_free(&next);
    return result;
}

int library_current_files(const OdeListing *listing,const char *directory,
    char *const *paths,size_t count,const MenuEntry **entries)
{
    size_t i, j, length=strlen(directory);
    /* Check every parent before reporting a missing local name; mixed-folder
     * queues must use the existing full-path resolver for all entries. */
    for (i=0;i<count;++i) {
        const char *slash=strrchr(paths[i],'/');
        if (!slash || (size_t)(slash-paths[i])!=length || strncmp(paths[i],directory,length)) return 0;
    }
    for (i=0;i<count;++i) {
        const char *name=paths[i]+length+1;
        for (j=0;j<listing->count;++j)
            if (!listing->entries[j]->is_directory && !menu_path_compare(listing->entries[j]->name,name)) break;
        if (j==listing->count) return -1;
        entries[i]=listing->entries[j];
    }
    return 1;
}
