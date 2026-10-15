#include "library_navigation.h"
#include "menu_sort.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Two volumes and repeated directory-local IDs. ODE-facing operations are
 * stubbed here; the separate firmware fixture runs actual FCare navigation. */
typedef struct Node { int parent, directory; unsigned long id; const char *name; } Node;
static const Node tree[] = {
    {-1,1,0,"root"}, {0,1,0x01000000UL,"USB A"}, {0,1,0x02000000UL,"USB B"},
    {1,1,0x01000001UL,"Games"}, {3,1,0x01000001UL,"Nested"},
    {3,0,0x01000002UL,"D (Disc 1).iso"}, {2,0,0x02000001UL,"Other.iso"},
    {1,1,0x01000002UL,"covers"}, {1,0,0x01000003UL,"boot.iso"}
};
static int current, calls, fail_at, callback_count, mutate_on_callback;
void ode_listing_init(OdeListing *l) { memset(l,0,sizeof(*l)); }
void ode_listing_free(OdeListing *l) {
    size_t i; for(i=0;i<l->count;++i) free((void *)l->entries[i]); free(l->entries); ode_listing_init(l);
}
static int fault(Ode *ode) { if (++calls == fail_at) { ode->navigation_failed=1; return 1; } return 0; }
int ode_read_directory(Ode *ode, OdeListing *l)
{
    size_t i;
    if(fault(ode)) return -1;
    ode_listing_free(l); l->entries=(const MenuEntry **)calloc(10,sizeof(MenuEntry *)); assert(l->entries);
    l->has_parent=current != 0; l->parent_id=(tree[current].id & 0xff000000UL)|0xffffffUL;
    for(i=1;i<sizeof(tree)/sizeof(tree[0]);++i) if(tree[i].parent == current) {
        MenuEntry *e=(MenuEntry *)malloc(sizeof(*e)); assert(e);
        e->name=tree[i].name; e->toc_id=tree[i].id; e->is_directory=tree[i].directory;
        if(mutate_on_callback && current == 4) { free(e); continue; }
        l->entries[l->count++]=e;
    }
    menu_sort(l->entries,l->count); return 0;
}
int ode_enter_directory(Ode *ode,const OdeListing *l,const MenuEntry *e,int workaround)
{
    size_t i; (void)l; assert(workaround);
    if(fault(ode)) return -1;
    for(i=1;i<sizeof(tree)/sizeof(tree[0]);++i) if(tree[i].parent == current && tree[i].id == e->toc_id && tree[i].directory) { current=(int)i; return 0; }
    return -1;
}
int ode_change_directory(Ode *ode,unsigned long id)
{
    assert((id & 0xffffffUL) == 0xffffffUL);
    if(fault(ode)) return -1;
    assert(current > 0); current=tree[current].parent; return 0;
}
static int action(void *context,const MenuEntry *e,const char *directory)
{
    (void)context; ++callback_count;
    assert(current == 2 && !strcmp(e->name,"Other.iso") && !strcmp(directory,"/Storage/USB B")); return 0;
}
static void setup(Ode *ode,OdeListing *snapshot)
{
    memset(ode,0,sizeof(*ode)); calls=fail_at=callback_count=mutate_on_callback=0;
    current=4; ode_listing_init(snapshot); assert(!ode_read_directory(ode,snapshot)); calls=0;
}
int main(void)
{
    Ode ode; OdeListing snapshot;
    ArtRouteStep route[3]; LibraryLocation *location;
    int total, i;
    route[0].id=0x01000000UL; route[0].name="USB A";
    route[1].id=0x01000001UL; route[1].name="Games";
    route[2].id=0x01000001UL; route[2].name="Nested";
    setup(&ode,&snapshot);
    assert(!library_visit(&ode,route,3,&snapshot,1,"/Storage/USB B/Other.iso",action,NULL));
    assert(current == 4 && callback_count == 1 && !ode.navigation_failed); total=calls;
    assert(library_visit(&ode,route,3,&snapshot,1,"/Storage/USB B/Missing.iso",action,NULL) == -1);
    assert(current == 4 && callback_count == 1 && !ode.navigation_failed);
    for(i=1;i<=total;++i) {
        ode_listing_free(&snapshot); setup(&ode,&snapshot); fail_at=i;
        assert(library_visit(&ode,route,3,&snapshot,1,"/Storage/USB B/Other.iso",action,NULL) == -2);
        assert(ode.navigation_failed);
    }
    ode_listing_free(&snapshot); setup(&ode,&snapshot);
    location=(LibraryLocation *)malloc(sizeof(*location)); assert(location);
    assert(!library_open(&ode,route,3,&snapshot,1,"/Storage/USB A/Games/D (Disc 1).iso",location));
    assert(current == 3 && location->depth == 2 && location->selected == 2);
    assert(location->previous[0] == 0 && location->previous[1] == 1); /* covers is hidden */
    assert(!strcmp(location->names[0],"USB A") && !strcmp(location->names[1],"Games"));
    ode_listing_free(&location->listing); free(location);
    ode_listing_free(&snapshot); setup(&ode,&snapshot);
    location=(LibraryLocation *)malloc(sizeof(*location)); assert(location);
    assert(library_open(&ode,route,3,&snapshot,1,"/Storage/USB A/boot.iso",location) == -1);
    assert(current == 4 && !ode.navigation_failed); free(location);
    calls=0; assert(library_visit(&ode,route,3,&snapshot,1,"/Storage/../bad",action,NULL) < 0 && !calls && current == 4);
    ode_listing_free(&snapshot);
    puts("Library navigation: cross-volume paths, repeated IDs, missing paths, every failure and retained favorite selection passed");
    return 0;
}
