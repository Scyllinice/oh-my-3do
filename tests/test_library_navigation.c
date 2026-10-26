#include "library_navigation.h"
#include "menu_sort.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Two volumes and repeated directory-local IDs. ODE-facing operations are
 * stubbed here; the separate firmware fixture runs actual FCare navigation. */
typedef struct Node {
    int parent, directory;
    unsigned long id;
    const char *name;
} Node;
static const Node tree[] = {{-1, 1, 0, "root"},
                            {0, 1, 0x01000000UL, "USB A"},
                            {0, 1, 0x02000000UL, "USB B"},
                            {1, 1, 0x01000001UL, "Games"},
                            {3, 1, 0x01000001UL, "Nested"},
                            {3, 0, 0x01000002UL, "D (Disc 1).iso"},
                            {2, 0, 0x02000001UL, "Other.iso"},
                            {1, 1, 0x01000002UL, ".ohmy3do"},
                            {1, 0, 0x01000003UL, "boot.iso"},
                            /* Listed only with second_disc: a two-disc set in Games. */
                            {3, 0, 0x01000003UL, "D (Disc 2).iso"}};
#define SECOND_DISC_NODE 9
static int second_disc;
static int cancel_after, scan_ticks;
static int scan_progress(void *context, size_t games)
{
    (void)context;
    (void)games;
    return cancel_after && ++scan_ticks == cancel_after;
}
static int bulk_mode;
static int current, calls, fail_at, callback_count, mutate_on_callback;
void ode_listing_init(OdeListing *l)
{
    memset(l, 0, sizeof(*l));
}
void ode_listing_free(OdeListing *l)
{
    size_t i;
    for (i = 0; i < l->count; ++i)
        free((void *)l->entries[i]);
    free(l->entries);
    ode_listing_init(l);
}
static int fault(Ode *ode)
{
    if (++calls == fail_at) {
        ode->navigation_failed = 1;
        return 1;
    }
    return 0;
}
int ode_read_directory(Ode *ode, OdeListing *l)
{
    size_t i;
    if (fault(ode))
        return -1;
    ode_listing_free(l);
    l->entries = (const MenuEntry **)calloc(bulk_mode ? 1100 : 10, sizeof(MenuEntry *));
    assert(l->entries);
    l->has_parent = current != 0;
    l->parent_id = (tree[current].id & 0xff000000UL) | 0xffffffUL;
    for (i = 1; i < sizeof(tree) / sizeof(tree[0]); ++i)
        if (tree[i].parent == current && (i != SECOND_DISC_NODE || second_disc)) {
            MenuEntry *e = (MenuEntry *)malloc(sizeof(*e));
            assert(e);
            e->name = tree[i].name;
            e->toc_id = tree[i].id;
            e->is_directory = tree[i].directory;
            if (mutate_on_callback && current == 4) {
                free(e);
                continue;
            }
            l->entries[l->count++] = e;
        }
    if (bulk_mode && (current == 2 || current == 3 || current == 4))
        for (i = 0; i < 1000; ++i) {
            MenuEntry *e = (MenuEntry *)malloc(sizeof(*e) + 32);
            assert(e);
            e->name = (char *)(e + 1);
            sprintf((char *)e->name, "Bulk %04lu.iso", (unsigned long)i);
            e->toc_id = (tree[current].id & 0xff000000UL) | (i + 100);
            e->is_directory = 0;
            l->entries[l->count++] = e;
        }
    menu_sort(l->entries, l->count);
    return 0;
}
int ode_enter_directory(Ode *ode, const OdeListing *l, const MenuEntry *e, int workaround)
{
    size_t i;
    (void)l;
    assert(workaround);
    if (fault(ode))
        return -1;
    for (i = 1; i < sizeof(tree) / sizeof(tree[0]); ++i)
        if (tree[i].parent == current && tree[i].id == e->toc_id && tree[i].directory) {
            current = (int)i;
            return 0;
        }
    return -1;
}
int ode_change_directory(Ode *ode, unsigned long id)
{
    assert((id & 0xffffffUL) == 0xffffffUL);
    if (fault(ode))
        return -1;
    assert(current > 0);
    current = tree[current].parent;
    return 0;
}
static int action(void *context, const MenuEntry *e, const char *directory)
{
    (void)context;
    ++callback_count;
    assert(current == 2 && !strcmp(e->name, "Other.iso") && !strcmp(directory, "/Storage/USB B"));
    return 0;
}
/* library_visit_folder's action: records the discs it is given, which must
 * be found inside Games. */
static const char *visited[4];
static int visited_count, fail_visit;
static int folder_action(void *context, const MenuEntry *e, const char *directory)
{
    (void)context;
    assert(current == 3 && !strcmp(directory, "/Storage/USB A/Games"));
    if (fail_visit && visited_count + 1 == fail_visit)
        return -7;
    visited[visited_count++] = e->name;
    return 0;
}
static void setup_at(Ode *ode, OdeListing *snapshot, int folder)
{
    memset(ode, 0, sizeof(*ode));
    calls = fail_at = callback_count = mutate_on_callback = 0;
    visited_count = fail_visit = 0;
    current = folder;
    ode_listing_init(snapshot);
    assert(!ode_read_directory(ode, snapshot));
    calls = 0;
}
static void setup(Ode *ode, OdeListing *snapshot)
{
    memset(ode, 0, sizeof(*ode));
    calls = fail_at = callback_count = mutate_on_callback = 0;
    current = 4;
    ode_listing_init(snapshot);
    assert(!ode_read_directory(ode, snapshot));
    calls = 0;
}
int main(void)
{
    Ode ode;
    OdeListing snapshot;
    ArtRouteStep route[3];
    LibraryLocation *location;
    int total, i;
    LibraryCatalog catalog;
    memset(&catalog, 0, sizeof(catalog));
    route[0].id = 0x01000000UL;
    route[0].name = "USB A";
    route[1].id = 0x01000001UL;
    route[1].name = "Games";
    route[2].id = 0x01000001UL;
    route[2].name = "Nested";
    setup(&ode, &snapshot);
    assert(!library_visit(&ode, route, 3, &snapshot, 1, "/Storage/USB B/Other.iso", action, NULL));
    assert(current == 4 && callback_count == 1 && !ode.navigation_failed);
    total = calls;
    assert(library_visit(&ode, route, 3, &snapshot, 1, "/Storage/USB B/Missing.iso", action,
                         NULL) == -1);
    assert(current == 4 && callback_count == 1 && !ode.navigation_failed);
    for (i = 1; i <= total; ++i) {
        ode_listing_free(&snapshot);
        setup(&ode, &snapshot);
        fail_at = i;
        assert(library_visit(&ode, route, 3, &snapshot, 1, "/Storage/USB B/Other.iso", action,
                             NULL) == -2);
        assert(ode.navigation_failed);
    }
    ode_listing_free(&snapshot);
    setup(&ode, &snapshot);
    location = (LibraryLocation *)malloc(sizeof(*location));
    assert(location);
    assert(!library_open(&ode, route, 3, &snapshot, 1, "/Storage/USB A/Games/D (Disc 1).iso",
                         location));
    assert(current == 3 && location->depth == 2 && location->selected == 2);
    assert(location->previous[0] == 0 && location->previous[1] == 1); /* .ohmy3do is hidden */
    assert(!strcmp(location->names[0], "USB A") && !strcmp(location->names[1], "Games"));
    ode_listing_free(&location->listing);
    free(location);
    ode_listing_free(&snapshot);
    setup(&ode, &snapshot);
    location = (LibraryLocation *)malloc(sizeof(*location));
    assert(location);
    assert(library_open(&ode, route, 3, &snapshot, 1, "/Storage/USB A/boot.iso", location) == -1);
    assert(current == 4 && !ode.navigation_failed);
    free(location);
    calls = 0;
    assert(library_visit(&ode, route, 3, &snapshot, 1, "/Storage/../bad", action, NULL) < 0 &&
           !calls && current == 4);
    ode_listing_free(&snapshot);
    setup(&ode, &snapshot);
    location = (LibraryLocation *)malloc(sizeof(*location));
    assert(location);
    assert(!library_open_directory(&ode, route, 3, &snapshot, 1, "/Storage/USB A/Games", location));
    assert(current == 3 && location->depth == 2 && !location->selected);
    total = calls;
    ode_listing_free(&location->listing);
    free(location);
    ode_listing_free(&snapshot);
    for (i = 1; i <= total; ++i) {
        setup(&ode, &snapshot);
        fail_at = i;
        location = (LibraryLocation *)malloc(sizeof(*location));
        assert(location);
        assert(library_open_directory(&ode, route, 3, &snapshot, 1, "/Storage/USB A/Games",
                                      location) == -2);
        assert(ode.navigation_failed);
        free(location);
        ode_listing_free(&snapshot);
    }
    setup(&ode, &snapshot);
    location = (LibraryLocation *)malloc(sizeof(*location));
    assert(location);
    assert(library_open_directory(&ode, route, 3, &snapshot, 1, "/Storage/USB A/Missing",
                                  location) == -1);
    assert(current == 4 && !ode.navigation_failed);
    free(location);
    ode_listing_free(&snapshot);
    /* A folder acting as its game: its parent opens with it selected. */
    setup(&ode, &snapshot);
    location = (LibraryLocation *)malloc(sizeof(*location));
    assert(location);
    assert(
        !library_select_directory(&ode, route, 3, &snapshot, 1, "/Storage/USB A/Games", location));
    assert(current == 1 && location->depth == 1 && location->selected == 1 &&
           !strcmp(location->names[0], "USB A"));
    total = calls;
    ode_listing_free(&location->listing);
    free(location);
    ode_listing_free(&snapshot);
    for (i = 1; i <= total; ++i) {
        setup(&ode, &snapshot);
        fail_at = i;
        location = (LibraryLocation *)malloc(sizeof(*location));
        assert(location);
        assert(library_select_directory(&ode, route, 3, &snapshot, 1, "/Storage/USB A/Games",
                                        location) == -2);
        assert(ode.navigation_failed);
        free(location);
        ode_listing_free(&snapshot);
    }
    setup(&ode, &snapshot);
    location = (LibraryLocation *)malloc(sizeof(*location));
    assert(location);
    assert(library_select_directory(&ode, route, 3, &snapshot, 1, "/Storage/USB A/Missing",
                                    location) == -1);
    assert(library_select_directory(&ode, route, 3, &snapshot, 1,
                                    "/Storage/USB A/Games/D (Disc 1).iso", location) == -1);
    assert(current == 4 && !ode.navigation_failed);
    free(location);
    ode_listing_free(&snapshot);
    setup(&ode, &snapshot);
    location = (LibraryLocation *)malloc(sizeof(*location));
    assert(location);
    assert(!library_open_directory(&ode, route, 3, &snapshot, 1, "/Storage", location));
    assert(current == 0 && !location->depth && location->listing.count == 2);
    ode_listing_free(&location->listing);
    free(location);
    ode_listing_free(&snapshot);
    setup(&ode, &snapshot);
    assert(!library_catalog_build(&ode, route, 3, &snapshot, 1, &catalog, scan_progress, NULL));
    assert(current == 4 && catalog.valid && !catalog.truncated && catalog.count == 2);
    assert(library_catalog_count(&catalog, "d (disc") == 1 &&
           library_catalog_count(&catalog, "OTHER") == 1);
    assert(!strcmp(library_catalog_match(&catalog, "other", 0), "/Storage/USB B/Other.iso"));
    assert(!library_catalog_match(&catalog, "missing", 0));
    total = calls;
    ode_listing_free(&snapshot);
    for (i = 1; i <= total; ++i) {
        setup(&ode, &snapshot);
        fail_at = i;
        assert(library_catalog_build(&ode, route, 3, &snapshot, 1, &catalog, NULL, NULL) == -2);
        assert(ode.navigation_failed && catalog.valid && catalog.count == 2);
        ode_listing_free(&snapshot);
    }
    /* Cancel at every scan checkpoint, including while inside another volume. */
    for (i = 1; i <= 12; ++i) {
        setup(&ode, &snapshot);
        cancel_after = i;
        scan_ticks = 0;
        assert(library_catalog_build(&ode, route, 3, &snapshot, 1, &catalog, scan_progress, NULL) ==
               1);
        assert(current == 4 && !ode.navigation_failed && catalog.count == 2);
        ode_listing_free(&snapshot);
    }
    /* Discs in one folder of the open one (a folder acting as its game). */
    {
        static char d1[] = "/Storage/USB A/Games/D (Disc 1).iso",
                    d2[] = "/Storage/USB A/Games/D (Disc 2).iso",
                    loose[] = "/Storage/USB A/boot.iso",
                    deeper[] = "/Storage/USB A/Games/Nested/X.iso",
                    missing[] = "/Storage/USB A/Games/D (Disc 3).iso",
                    other[] = "/Storage/USB A/Nowhere/D (Disc 1).iso";
        char *set[] = {d1, d2}, *mixed[] = {d1, loose}, *nested[] = {deeper},
             *gap[] = {d1, missing}, *elsewhere[] = {other};
        const MenuEntry *folder;
        int visit_calls, old_calls;
        second_disc = 1;
        setup_at(&ode, &snapshot, 1); /* the open folder is USB A */
        folder = library_files_folder(&snapshot, "/Storage/USB A", set, 2);
        assert(folder && !strcmp(folder->name, "Games"));
        assert(!library_files_folder(&snapshot, "/Storage/USB A", mixed, 2));
        assert(!library_files_folder(&snapshot, "/Storage/USB A", nested, 1));
        assert(!library_files_folder(&snapshot, "/Storage/USB A", elsewhere, 1));
        assert(!library_files_folder(&snapshot, "/Storage/USB A/Games", set, 2));
        assert(!library_files_folder(&snapshot, "/Storage/USB A", set, 0));

        /* Both discs, in order, from one visit; then back in USB A. */
        assert(!library_visit_folder(&ode, &snapshot, folder, 1, "/Storage/USB A/Games", set, 2,
                                     folder_action, NULL));
        assert(current == 1 && visited_count == 2 && !ode.navigation_failed);
        assert(!strcmp(visited[0], "D (Disc 1).iso") && !strcmp(visited[1], "D (Disc 2).iso"));
        visit_calls = calls;

        /* The old way: library_visit checks each disc, then visits it again
         * to queue it. */
        calls = 0;
        for (i = 0; i < 4; ++i)
            assert(!library_visit(&ode, route, 1, &snapshot, 1, set[i % 2], NULL, NULL));
        old_calls = calls;
        printf("Queueing a two-disc folder: %d USB steps, down from %d\n", visit_calls, old_calls);
        assert(visit_calls * 2 < old_calls);

        /* A missing disc: no action at all, and back in USB A. */
        setup_at(&ode, &snapshot, 1);
        assert(library_visit_folder(&ode, &snapshot, folder, 1, "/Storage/USB A/Games", gap, 2,
                                    folder_action, NULL) == -1);
        assert(current == 1 && !visited_count && !ode.navigation_failed);

        /* An action's failure stops the rest and is passed back. */
        setup_at(&ode, &snapshot, 1);
        folder = library_files_folder(&snapshot, "/Storage/USB A", set, 2);
        fail_visit = 2;
        assert(library_visit_folder(&ode, &snapshot, folder, 1, "/Storage/USB A/Games", set, 2,
                                    folder_action, NULL) == -7);
        assert(current == 1 && visited_count == 1 && !ode.navigation_failed);

        /* Every USB step failing in turn leaves navigation failed. */
        for (i = 1; i <= visit_calls; ++i) {
            ode_listing_free(&snapshot);
            setup_at(&ode, &snapshot, 1);
            folder = library_files_folder(&snapshot, "/Storage/USB A", set, 2);
            fail_at = i;
            assert(library_visit_folder(&ode, &snapshot, folder, 1, "/Storage/USB A/Games", set, 2,
                                        folder_action, NULL) == -2);
            assert(ode.navigation_failed);
        }
        ode_listing_free(&snapshot);
        second_disc = 0;
    }
    bulk_mode = 1;
    cancel_after = 0;
    setup(&ode, &snapshot);
    assert(!library_catalog_build(&ode, route, 3, &snapshot, 1, &catalog, NULL, NULL));
    assert(catalog.valid && catalog.truncated && catalog.count == LIBRARY_CATALOG_MAX &&
           current == 4);
    assert(library_catalog_count(&catalog, "Bulk") > 0);
    ode_listing_free(&snapshot);
    bulk_mode = 0;
    library_catalog_free(&catalog);
    assert(!catalog.paths && !catalog.valid);
    puts("Library catalog: both volumes, name matching, hidden support files, cancellation and "
         "every I/O failure passed");
    puts("Library navigation: cross-volume paths, repeated IDs, missing paths, every failure and "
         "retained favorite selection passed");
    return 0;
}
