/*
 * Going straight to a game or folder by its full path, and scanning the
 * drive for Find. See library_navigation.h.
 *
 * Every journey here starts from the folder the menu is showing and ends
 * either in a new folder (when opening a favorite, say) or back in the
 * original one (after visiting a file, or a failed search). Getting back is
 * checked by comparing listings; if it cannot be done, browsing is no longer
 * safe and ode->navigation_failed stops all further USB use (see
 * art_navigation.h for why).
 */
#include "library_navigation.h"
#include "menu_ui.h"
#include "library_features.h"
#include <stdlib.h>
#include <string.h>

/* An id's top byte names the USB volume; the low 24 bits the entry on it.
 * With every entry bit set, an id means "the parent of the current folder". */
#define VOLUME_BITS  0xff000000UL
#define ENTRY_BITS   0x00ffffffUL
#define PARENT_ENTRY 0x00ffffffUL

/* "/Storage" is the menu's name for the top of the drive. */
#define STORAGE_ROOT        "/Storage"
#define STORAGE_ROOT_LENGTH 8

/* The longest full path handled, not counting its zero byte. */
#define LONGEST_PATH 9216

/* What navigate() should do once it reaches the end of the path. */
enum {
    /* The path is a file: either run an action on it and come back
     * (library_visit), or open its folder with it selected (library_open). */
    GO_TO_FILE,
    /* The path is a folder: open it (library_open_directory). */
    OPEN_FOLDER,
    /* The path is a folder: open its parent with it selected
     * (library_select_directory). */
    SELECT_FOLDER
};

/* The id that climbs one level from the folder with id `folder_id`. */
static unsigned long parent_of(unsigned long folder_id)
{
    return (folder_id & VOLUME_BITS) | PARENT_ENTRY;
}

/* Whether an entry is a USB volume in the list of volumes (a folder whose
 * id has no entry bits). Volumes are never hidden as support files. */
static int is_volume(const MenuEntry *entry)
{
    return entry->is_directory && !(entry->toc_id & ENTRY_BITS);
}

/* Whether two listings hold the same entries in the same order. */
static int same_listing(const OdeListing *a, const OdeListing *b)
{
    size_t i;
    if (a->count != b->count || a->has_parent != b->has_parent ||
        (a->has_parent && a->parent_id != b->parent_id))
        return 0;
    for (i = 0; i < a->count; ++i) {
        if (a->entries[i]->toc_id != b->entries[i]->toc_id ||
            a->entries[i]->is_directory != b->entries[i]->is_directory ||
            strcmp(a->entries[i]->name, b->entries[i]->name))
            return 0;
    }
    return 1;
}

/* Climbs from the folder at the end of `route` (`depth` levels deep) to the
 * top. Returns 0, or -1 if a step failed. */
static int climb_to_top(Ode *ode, const ArtRouteStep *route, unsigned int depth)
{
    unsigned int level;
    for (level = depth; level > 0; --level) {
        if (ode_change_directory(ode, parent_of(route[level - 1].id)) < 0)
            return -1;
    }
    return 0;
}

/*
 * Walks from the top back down `route`, `depth` levels, finding each folder
 * by id and name in a full listing (which lets ode_enter_directory apply the
 * folder workaround). Returns 0, or -1 if a folder is missing or a step
 * failed.
 */
static int walk_down_route(Ode *ode, const ArtRouteStep *route, unsigned int depth, int workaround)
{
    OdeListing listing;
    unsigned int level;

    ode_listing_init(&listing);
    for (level = 0; level < depth; ++level) {
        size_t i;
        if (ode_read_directory(ode, &listing) < 0)
            goto failed;
        for (i = 0; i < listing.count; ++i) {
            const MenuEntry *entry = listing.entries[i];
            if (entry->toc_id == route[level].id && entry->is_directory &&
                !strcmp(entry->name, route[level].name))
                break;
        }
        if (i == listing.count ||
            ode_enter_directory(ode, &listing, listing.entries[i], workaround) < 0)
            goto failed;
        ode_listing_free(&listing);
    }
    return 0;

failed:
    ode_listing_free(&listing);
    return -1;
}

/*
 * Whether every folder and file name in a path (after "/Storage/") is safe:
 * not empty, "." or "..", not too long, and without backslashes or colons.
 * The path is checked in full before the FCare is asked to move at all.
 */
static int path_names_are_safe(char *names)
{
    char *name = names;
    for (;;) {
        char *slash = strchr(name, '/');
        int safe;

        if (slash)
            *slash = 0; /* look at one name at a time */
        safe = *name && strcmp(name, ".") && strcmp(name, "..") && strlen(name) < ODE_MAX_NAME &&
               !strchr(name, '\\') && !strchr(name, ':');
        if (slash)
            *slash = '/';

        if (!safe)
            return 0;
        if (!slash)
            return 1;
        name = slash + 1;
    }
}

/*
 * Where entry `index` of `listing` appears on screen: after the ".." row (if
 * the folder has one), and counting only entries that are not hidden as
 * support files. `at_top` says whether the listing is the drive's (or a
 * volume's) top folder, where more support files are hidden.
 */
static size_t screen_position(const OdeListing *listing, size_t index, int at_top)
{
    size_t position = index + (listing->has_parent ? 1 : 0);
    size_t i;
    for (i = 0; i < index; ++i) {
        const MenuEntry *entry = listing->entries[i];
        if (!is_volume(entry) && menu_support_entry(entry->name, entry->is_directory, at_top))
            --position;
    }
    return position;
}

/*
 * The work behind library_visit, library_open, library_open_directory and
 * library_select_directory. Follows `path` from the top of the drive, one
 * name at a time, then does what `mode` asks (see the enum above).
 *
 * With `keep` (the open functions), on success the FCare stays in the new
 * folder and `keep` receives its listing, route and selection. Without it
 * (library_visit), `action` runs on the file and the menu returns to the
 * original folder. After any failure the menu returns to the original folder
 * too.
 *
 * Returns 0 (or the action's result), -1 if the path is invalid or missing,
 * or -2 if the original folder could not be restored.
 */
static int navigate(Ode *ode, const ArtRouteStep *route, unsigned int depth,
                    const OdeListing *current, int workaround, const char *path,
                    LibraryFileAction action, void *context, LibraryLocation *keep, int mode)
{
    OdeListing listing;
    /* For each folder entered, the id that leads back out of it. */
    unsigned long way_back[LIBRARY_DEPTH];
    OdeProgress progress = ode->progress;
    unsigned int entered = 0; /* folders entered below the top */
    char *names = NULL;       /* a writable copy of the path */
    char *name;
    const char *last_slash;
    size_t path_length;
    int opening_top = 0; /* "/Storage" itself, when opening a folder */
    int result = -1;

    ode_listing_init(&listing);

    /* Only full paths starting "/Storage/" ("/Storage" itself is allowed when
     * opening a folder: the top of the drive). */
    if (ode->navigation_failed || depth > LIBRARY_DEPTH)
        return -1;
    if (mode != GO_TO_FILE && !strcmp(path, STORAGE_ROOT))
        opening_top = 1;
    else if (strncmp(path, STORAGE_ROOT "/", STORAGE_ROOT_LENGTH + 1))
        return -1;
    path_length = strlen(path);
    if (path_length > LONGEST_PATH)
        return -1;

    names = (char *)malloc(path_length + 1);
    if (!names) {
        ode->error = "Not enough navigation memory";
        return -1;
    }
    memcpy(names, path, path_length + 1);
    if (!opening_top && !path_names_are_safe(names + STORAGE_ROOT_LENGTH + 1))
        goto done;

    /* Everything checked: hide progress and climb to the top of the drive. */
    ode->progress = NULL;
    if (climb_to_top(ode, route, depth) < 0)
        goto fatal;

    if (opening_top) {
        if (ode_read_directory(ode, &listing) < 0)
            goto fatal;
        keep->listing = listing; /* `keep` now owns the listing */
        ode_listing_init(&listing);
        keep->depth = 0;
        keep->selected = 0;
        result = 0;
        goto done;
    }

    /* Follow the path down, one name at a time. */
    name = names + STORAGE_ROOT_LENGTH + 1;
    last_slash = strrchr(path, '/');
    for (;;) {
        char *slash = strchr(name, '/');
        int is_last = slash == NULL;
        /* The last name is a file unless the path names a folder. */
        int want_folder = !is_last || mode != GO_TO_FILE;
        int at_top = entered == 0 || (entered == 1 && keep && !(keep->ids[0] & ENTRY_BITS));
        const MenuEntry *entry;
        size_t index;

        if (slash)
            *slash = 0;

        /* Find this name (ignoring case, as FAT does) among the entries. */
        if (ode_read_directory(ode, &listing) < 0)
            goto fatal;
        for (index = 0; index < listing.count; ++index) {
            if (!menu_path_compare(listing.entries[index]->name, name) &&
                !!listing.entries[index]->is_directory == !!want_folder)
                break;
        }
        if (index == listing.count) {
            ode->error = "Queued/favorite path is missing";
            goto restore;
        }
        entry = listing.entries[index];

        /* The end of the path, when it names a file or a folder to select. */
        if (is_last && mode != OPEN_FOLDER) {
            if (keep) {
                /* Open the folder holding it, with it selected. */
                if (menu_support_entry(entry->name, entry->is_directory, at_top)) {
                    ode->error = "Favorite is a menu support file";
                    goto restore;
                }
                keep->listing = listing;
                ode_listing_init(&listing);
                keep->selected = screen_position(&keep->listing, index, at_top);
                keep->depth = entered;
                result = 0;
                goto done;
            } else {
                /* Run the action on the file, then go back. */
                char directory[LONGEST_PATH + 2];
                size_t directory_length = (size_t)(last_slash - path);
                memcpy(directory, path, directory_length);
                directory[directory_length] = 0;
                result = action ? action(context, entry, directory) : 0;
                goto restore;
            }
        }

        /* A folder on the way: enter it, remembering the way back. */
        if (entered == LIBRARY_DEPTH) {
            ode->error = "Maximum folder depth reached";
            goto restore;
        }
        way_back[entered] = listing.has_parent ? listing.parent_id : parent_of(entry->toc_id);
        if (keep) {
            strcpy(keep->names[entered], entry->name);
            keep->ids[entered] = entry->toc_id;
            /* Where it sits in its parent, so Back returns to it selected. */
            keep->previous[entered] = screen_position(&listing, index, at_top);
        }
        if (ode_enter_directory(ode, &listing, entry, workaround) < 0) {
            if (ode->navigation_failed)
                goto fatal;
            goto restore;
        }
        ++entered;
        ode_listing_free(&listing);

        /* The end of the path, when it names a folder to open. */
        if (is_last) {
            if (ode_read_directory(ode, &listing) < 0)
                goto fatal;
            keep->listing = listing;
            ode_listing_init(&listing);
            keep->depth = entered;
            keep->selected = 0;
            result = 0;
            goto done;
        }
        name = slash + 1;
    }

restore:
    /* Back out of every folder entered, return along the original route,
     * and check the original folder is really back. */
    ode_listing_free(&listing);
    if (ode->navigation_failed)
        goto fatal;
    while (entered) {
        if (ode_change_directory(ode, way_back[--entered]) < 0)
            goto fatal;
    }
    if (walk_down_route(ode, route, depth, workaround) < 0 ||
        ode_read_directory(ode, &listing) < 0 || !same_listing(&listing, current))
        goto fatal;
    goto done;

fatal:
    ode->navigation_failed = 1;
    ode->error = "Library folder restore failed";
    result = -2;

done:
    ode_listing_free(&listing);
    free(names);
    ode->progress = progress;
    return result;
}

int library_visit(Ode *ode, const ArtRouteStep *route, unsigned int depth,
                  const OdeListing *current, int workaround, const char *path,
                  LibraryFileAction action, void *context)
{
    return navigate(ode, route, depth, current, workaround, path, action, context, NULL,
                    GO_TO_FILE);
}

int library_open(Ode *ode, const ArtRouteStep *route, unsigned int depth, const OdeListing *current,
                 int workaround, const char *path, LibraryLocation *location)
{
    memset(location, 0, sizeof(*location));
    return navigate(ode, route, depth, current, workaround, path, NULL, NULL, location, GO_TO_FILE);
}

int library_open_directory(Ode *ode, const ArtRouteStep *route, unsigned int depth,
                           const OdeListing *current, int workaround, const char *path,
                           LibraryLocation *location)
{
    memset(location, 0, sizeof(*location));
    return navigate(ode, route, depth, current, workaround, path, NULL, NULL, location,
                    OPEN_FOLDER);
}

int library_select_directory(Ode *ode, const ArtRouteStep *route, unsigned int depth,
                             const OdeListing *current, int workaround, const char *path,
                             LibraryLocation *location)
{
    memset(location, 0, sizeof(*location));
    return navigate(ode, route, depth, current, workaround, path, NULL, NULL, location,
                    SELECT_FOLDER);
}

void library_catalog_free(LibraryCatalog *catalog)
{
    size_t i;
    for (i = 0; i < catalog->count; ++i)
        free(catalog->paths[i]);
    free(catalog->paths);
    memset(catalog, 0, sizeof(*catalog));
}

/* Whether a file name is a disc image the menu can launch. */
static int is_disc_image(const char *name)
{
    size_t length = strlen(name);
    return length > 4 && (!menu_path_compare(name + length - 4, ".iso") ||
                          !menu_path_compare(name + length - 4, ".cue"));
}

/* Whether a catalog path's file name (not its folders) matches a Find query. */
static int catalog_matches(const char *path, const char *query)
{
    const char *last_slash = strrchr(path, '/');
    return menu_name_matches(last_slash ? last_slash + 1 : path, query);
}

size_t library_catalog_count(const LibraryCatalog *catalog, const char *query)
{
    size_t i, count = 0;
    for (i = 0; i < catalog->count; ++i) {
        if (catalog_matches(catalog->paths[i], query))
            ++count;
    }
    return count;
}

const char *library_catalog_match(const LibraryCatalog *catalog, const char *query, size_t index)
{
    size_t i;
    for (i = 0; i < catalog->count; ++i) {
        /* Count down through the matches until reaching the index-th one. */
        if (catalog_matches(catalog->paths[i], query)) {
            if (index == 0)
                return catalog->paths[i];
            --index;
        }
    }
    return NULL;
}

/*
 * One folder on the scan's current path. The scan goes depth first, keeping
 * one frame per folder from the top down to the folder it is in, instead of
 * recursing (the console's stack is small).
 */
typedef struct ScanFrame {
    OdeListing listing;   /* the folder's entries */
    size_t next;          /* the next entry to look at */
    size_t length;        /* length of this folder's path in the path buffer */
    unsigned long parent; /* the id that leads back out of this folder */
    int at_top;           /* the drive's or a volume's top folder */
} ScanFrame;

int library_catalog_build(Ode *ode, const ArtRouteStep *route, unsigned int depth,
                          const OdeListing *current, int workaround, LibraryCatalog *catalog,
                          LibraryScanProgress progress, void *context)
{
    ScanFrame *frames;
    char *path; /* the full path of the entry being looked at */
    LibraryCatalog found;
    OdeProgress old_progress = ode->progress;
    unsigned int level = 0; /* the frame for the current folder */
    unsigned int i;
    int result = -1;

    if (ode->navigation_failed || depth > LIBRARY_DEPTH)
        return -2;

    memset(&found, 0, sizeof(found));
    frames = (ScanFrame *)calloc(LIBRARY_DEPTH + 1, sizeof(*frames));
    path = (char *)malloc(LONGEST_PATH + 2);
    found.paths = (char **)calloc(LIBRARY_CATALOG_MAX, sizeof(*found.paths));
    if (!frames || !path || !found.paths)
        goto done;

    /* Start at the top of the drive. */
    ode->progress = NULL;
    if (climb_to_top(ode, route, depth) < 0)
        goto fatal;
    strcpy(path, STORAGE_ROOT);
    frames[0].length = STORAGE_ROOT_LENGTH;
    frames[0].at_top = 1;
    if (ode_read_directory(ode, &frames[0].listing) < 0)
        goto fatal;

    for (;;) {
        ScanFrame *frame = &frames[level];
        const MenuEntry *entry;
        size_t name_length;

        /* Let the caller show progress, and stop if asked. */
        if (progress && progress(context, found.count)) {
            result = 1;
            goto restore;
        }

        /* Finished this folder: climb back to its parent, or stop at the top. */
        if (frame->next == frame->listing.count) {
            if (level == 0) {
                result = 0;
                goto restore;
            }
            if (ode_change_directory(ode, frame->parent) < 0)
                goto fatal;
            ode_listing_free(&frame->listing);
            --level;
            path[frames[level].length] = 0;
            continue;
        }

        entry = frame->listing.entries[frame->next++];

        /* Skip the menu's own files (but always look inside volumes). */
        if (menu_support_entry(entry->name, entry->is_directory, frame->at_top) &&
            !is_volume(entry))
            continue;
        /* Skip names that could not be part of a safe path. */
        name_length = strlen(entry->name);
        if (!name_length || !strcmp(entry->name, ".") || !strcmp(entry->name, "..") ||
            strchr(entry->name, '/') || strchr(entry->name, '\\') || strchr(entry->name, ':'))
            continue;
        /* Too long or too deep to record: note that the list is incomplete. */
        if (frame->length + name_length + 1 > LONGEST_PATH ||
            (entry->is_directory && level == LIBRARY_DEPTH)) {
            found.truncated = 1;
            continue;
        }

        path[frame->length] = '/';
        strcpy(path + frame->length + 1, entry->name);

        if (entry->is_directory) {
            /* Go into the folder; its frame continues the scan from there. */
            ScanFrame *child = &frames[level + 1];
            child->length = frame->length + name_length + 1;
            child->next = 0;
            child->at_top = !(entry->toc_id & ENTRY_BITS);
            child->parent =
                frame->listing.has_parent ? frame->listing.parent_id : parent_of(entry->toc_id);
            if (ode_enter_directory(ode, &frame->listing, entry, workaround) < 0)
                goto fatal;
            ++level;
            if (ode_read_directory(ode, &child->listing) < 0)
                goto fatal;
        } else {
            if (is_disc_image(entry->name)) {
                size_t bytes = strlen(path) + 1;
                /* Full: keep what was found, marked incomplete. */
                if (found.count == LIBRARY_CATALOG_MAX ||
                    found.bytes + bytes > LIBRARY_CATALOG_BYTES) {
                    found.truncated = 1;
                    result = 0;
                    goto restore;
                }
                found.paths[found.count] = (char *)malloc(bytes);
                if (!found.paths[found.count]) {
                    result = -1;
                    goto restore;
                }
                memcpy(found.paths[found.count++], path, bytes);
                found.bytes += bytes;
            }
            path[frame->length] = 0; /* drop the file name again */
        }
    }

restore:
    /* Climb out of every open folder, return along the original route and
     * check the original folder is back. */
    while (level) {
        if (ode_change_directory(ode, frames[level].parent) < 0)
            goto fatal;
        ode_listing_free(&frames[level--].listing);
    }
    ode_listing_free(&frames[0].listing);
    if (walk_down_route(ode, route, depth, workaround) < 0 ||
        ode_read_directory(ode, &frames[0].listing) < 0 ||
        !same_listing(&frames[0].listing, current))
        goto fatal;

    if (result == 0) {
        /* Replace the old catalog; `found` then holds nothing to free. */
        found.valid = 1;
        library_catalog_free(catalog);
        *catalog = found;
        memset(&found, 0, sizeof(found));
    }
    goto done;

fatal:
    ode->navigation_failed = 1;
    ode->error = "Search folder restore failed";
    result = -2;

done:
    ode->progress = old_progress;
    if (frames) {
        for (i = 0; i <= LIBRARY_DEPTH; ++i)
            ode_listing_free(&frames[i].listing);
    }
    free(frames);
    free(path);
    library_catalog_free(&found);
    return result;
}

int library_visit_folder(Ode *ode, const OdeListing *current, const MenuEntry *folder,
                         int workaround, const char *folder_path, char *const *paths, size_t count,
                         LibraryFileAction action, void *context)
{
    OdeListing listing;
    const MenuEntry **entries;
    OdeProgress progress = ode->progress;
    unsigned long way_back;
    size_t i, j;
    int result = -1;

    if (ode->navigation_failed || !count)
        return -1;
    entries = (const MenuEntry **)malloc(count * sizeof(*entries));
    if (!entries) {
        ode->error = "Not enough navigation memory";
        return -1;
    }
    ode_listing_init(&listing);
    ode->progress = NULL;

    if (ode_enter_directory(ode, current, folder, workaround) < 0) {
        if (ode->navigation_failed)
            goto fatal;
        goto done; /* refused before any command: still in the open folder */
    }
    if (ode_read_directory(ode, &listing) < 0)
        goto fatal;
    way_back = listing.has_parent ? listing.parent_id : parent_of(folder->toc_id);

    /* Find every file before acting on any. */
    for (i = 0; i < count; ++i) {
        const char *name = strrchr(paths[i], '/') + 1;
        for (j = 0; j < listing.count; ++j) {
            if (!listing.entries[j]->is_directory &&
                !menu_path_compare(listing.entries[j]->name, name))
                break;
        }
        if (j == listing.count) {
            ode->error = "Queued/favorite path is missing";
            goto restore;
        }
        entries[i] = listing.entries[j];
    }
    result = 0;
    for (i = 0; i < count && !result; ++i)
        result = action ? action(context, entries[i], folder_path) : 0;

restore:
    /* Back to the open folder, and check it really is. */
    ode_listing_free(&listing);
    if (ode->navigation_failed || ode_change_directory(ode, way_back) < 0 ||
        ode_read_directory(ode, &listing) < 0 || !same_listing(&listing, current))
        goto fatal;
    goto done;

fatal:
    ode->navigation_failed = 1;
    ode->error = "Library folder restore failed";
    result = -2;

done:
    ode_listing_free(&listing);
    free(entries);
    ode->progress = progress;
    return result;
}

int library_current_files(const OdeListing *listing, const char *directory, char *const *paths,
                          size_t count, const MenuEntry **entries)
{
    size_t i, j, directory_length = strlen(directory);

    /*
     * First check that every path is in `directory`, before reporting any
     * missing name: if even one is elsewhere, the caller must use the full
     * path resolver for all of them.
     */
    for (i = 0; i < count; ++i) {
        const char *last_slash = strrchr(paths[i], '/');
        if (!last_slash || (size_t)(last_slash - paths[i]) != directory_length ||
            strncmp(paths[i], directory, directory_length))
            return 0;
    }

    /* Then find each file in the listing. */
    for (i = 0; i < count; ++i) {
        const char *name = paths[i] + directory_length + 1;
        for (j = 0; j < listing->count; ++j) {
            if (!listing->entries[j]->is_directory &&
                !menu_path_compare(listing->entries[j]->name, name))
                break;
        }
        if (j == listing->count)
            return -1;
        entries[i] = listing->entries[j];
    }
    return 1;
}

const MenuEntry *library_files_folder(const OdeListing *listing, const char *directory,
                                      char *const *paths, size_t count)
{
    size_t directory_length = strlen(directory), folder_length, i;
    const char *folder_name, *last_slash;

    if (!count)
        return NULL;
    /* The first path names the folder: directory/FOLDER/file. */
    last_slash = strrchr(paths[0], '/');
    if (!last_slash)
        return NULL;
    folder_length = (size_t)(last_slash - paths[0]);
    if (folder_length <= directory_length + 1 || strncmp(paths[0], directory, directory_length) ||
        paths[0][directory_length] != '/')
        return NULL;
    folder_name = paths[0] + directory_length + 1;
    if (memchr(folder_name, '/', folder_length - directory_length - 1))
        return NULL; /* deeper than one folder down */

    /* Every path must be in that same folder. */
    for (i = 1; i < count; ++i) {
        last_slash = strrchr(paths[i], '/');
        if (!last_slash || (size_t)(last_slash - paths[i]) != folder_length ||
            strncmp(paths[i], paths[0], folder_length))
            return NULL;
    }

    /* And the folder must be in the open folder. */
    for (i = 0; i < listing->count; ++i) {
        const MenuEntry *entry = listing->entries[i];
        if (entry->is_directory && strlen(entry->name) == folder_length - directory_length - 1 &&
            !strncmp(entry->name, folder_name, folder_length - directory_length - 1))
            return entry;
    }
    return NULL;
}
