/*
 * Round trips to another folder to read a file. See art_navigation.h.
 */
#include "art_navigation.h"
#include <string.h>

/* An id's top byte names the USB volume; the low 24 bits the entry on it. */
#define VOLUME_BITS 0xff000000UL
#define ENTRY_BITS  0x00ffffffUL

/* With every entry bit set, an id means "the parent of the current folder". */
#define PARENT_ENTRY 0x00ffffffUL

/* The id that climbs one level from the folder with id `folder_id`. */
static unsigned long parent_of(unsigned long folder_id)
{
    return (folder_id & VOLUME_BITS) | PARENT_ENTRY;
}

/*
 * Whether a fresh listing matches the one the menu is showing, entry for
 * entry: same ids, kinds and names in the same order. If so, the FCare is
 * back in the folder the menu thinks it is in.
 */
static int same_listing(const OdeListing *fresh, const OdeListing *expected)
{
    size_t i;

    if (fresh->count != expected->count || fresh->has_parent != expected->has_parent)
        return 0;
    if (fresh->has_parent && fresh->parent_id != expected->parent_id)
        return 0;

    for (i = 0; i < fresh->count; ++i) {
        const MenuEntry *a = fresh->entries[i];
        const MenuEntry *b = expected->entries[i];
        if (a->toc_id != b->toc_id || a->is_directory != b->is_directory ||
            strcmp(a->name, b->name))
            return 0;
    }
    return 1;
}

int art_in_folder(Ode *ode, const OdeListing *current, const MenuEntry *folder, int workaround,
                  ArtRootRead read, void *context)
{
    OdeListing restored;
    /* Saved so they can be put back afterwards. */
    OdeProgress progress = ode->progress;
    const char *media_folder = ode->media_folder;
    int result = -1;

    if (ode->navigation_failed)
        return -2;
    if (!folder || !folder->is_directory)
        return -1;

    ode_listing_init(&restored);
    /* No progress display for this quick, invisible trip. */
    ode->progress = NULL;

    if (ode_enter_directory(ode, current, folder, workaround) < 0) {
        if (ode->navigation_failed)
            goto failed;
        goto done; /* simply could not enter: nothing has moved */
    }

    /* Inside the folder, its media is "beside" the reader, not in a subfolder. */
    ode->media_folder = NULL;
    result = read(context);
    ode->media_folder = media_folder;

    /*
     * If a USB exchange was cut off part-way, the FCare's command queue may
     * be out of step, and sending "go to parent" now could do anything. Stop
     * here; the caller turns browsing off until the console restarts.
     */
    if (ode->navigation_failed)
        goto failed;

    /* Climb back out and check we are where we started. */
    if (ode_change_directory(ode, parent_of(folder->toc_id)) < 0 ||
        ode_read_directory(ode, &restored) < 0 || !same_listing(&restored, current))
        goto failed;

done:
    ode_listing_free(&restored);
    ode->progress = progress;
    return result;

failed:
    ode->navigation_failed = 1;
    ode->error = "USB game folder restore failed";
    result = -2;
    goto done;
}

/*
 * Enters the folder at `step` from the current folder (its parent), using
 * the quickest way that is still safe. Returns 0, or -1 on failure.
 * `listing` is scratch space for a full folder listing.
 */
static int enter_route_step(Ode *ode, const ArtRouteStep *step, int workaround, OdeListing *listing)
{
    const MenuEntry *entry = NULL;
    size_t i;

    /*
     * Quick way, for the FCare only: check just the target folder's own
     * record and enter it directly. That is safe unless the folder
     * workaround might be needed: when the FCare's last folder change is
     * unknown, or had the same entry id as this one (the case its firmware
     * bug ignores).
     */
    if (!strcmp(ode->identity, "LOCODE") &&
        (!workaround ||
         (ode->navigation_known && ode->navigation_key != (step->id & ENTRY_BITS)))) {
        int matched = ode_probe_directory(ode, step->id, step->name);
        if (matched < 0)
            return -1;
        if (matched)
            return ode_change_directory(ode, step->id) < 0 ? -1 : 0;
        /* Not matched: fall through to the full listing. */
    }

    /*
     * Full way: list the folder, find the entry by id and name, and enter it
     * with ode_enter_directory, which applies the workaround when needed.
     */
    if (ode_read_directory(ode, listing) < 0)
        return -1;
    for (i = 0; i < listing->count; ++i) {
        const MenuEntry *candidate = listing->entries[i];
        if (candidate->toc_id == step->id && candidate->is_directory &&
            !strcmp(candidate->name, step->name))
            entry = candidate;
    }
    if (!entry || ode_enter_directory(ode, listing, entry, workaround) < 0)
        return -1;
    ode_listing_free(listing);
    return 0;
}

int art_at_root(Ode *ode, const ArtRouteStep *route, unsigned int depth, const OdeListing *current,
                int workaround, ArtRootRead read, void *context)
{
    /*
     * On drives with several volumes the menu's top level is a list of
     * volumes, not a folder; the first route step is then a volume (an id
     * with no entry bits), and the "root" to read from is that volume's root,
     * one level down.
     */
    unsigned int root_level = (depth > 0 && !(route[0].id & ENTRY_BITS)) ? 1 : 0;
    OdeProgress progress = ode->progress;
    OdeListing listing;
    int result;
    unsigned int level;

    ode_listing_init(&listing);
    ode->progress = NULL; /* no progress display for this invisible trip */

    /* Climb to the root, one parent at a time. */
    for (level = depth; level > root_level; --level) {
        if (ode_change_directory(ode, parent_of(route[level - 1].id)) < 0)
            goto failed;
    }

    result = read(context);
    if (ode->navigation_failed)
        goto failed; /* see art_in_folder: the command queue may be out of step */

    /* Walk back down the route... */
    for (level = root_level; level < depth; ++level) {
        if (enter_route_step(ode, &route[level], workaround, &listing) < 0)
            goto failed;
    }

    /* ...and check we arrived back where we started. */
    if (depth > root_level) {
        if (ode_read_directory(ode, &listing) < 0 || !same_listing(&listing, current))
            goto failed;
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
