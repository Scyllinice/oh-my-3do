#include "ode.h"

#include <stdlib.h>
#include <string.h>

static unsigned long read_be32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) |
           (unsigned long)p[3];
}

static int fail(Ode *ode, const char *message)
{
    ode->error = message;
    return -1;
}

static int command(Ode *ode, unsigned char code, unsigned long id, unsigned int offset,
                   unsigned char *reply, size_t expected)
{
    unsigned char bytes[7];
    size_t length = 0;
    bytes[0] = code;
    bytes[1] = (unsigned char)(id >> 24);
    bytes[2] = (unsigned char)(id >> 16);
    bytes[3] = (unsigned char)(id >> 8);
    bytes[4] = (unsigned char)id;
    bytes[5] = (unsigned char)(offset >> 8);
    bytes[6] = (unsigned char)offset;
    if (ode->transfer(ode->context, bytes, reply, 32, &length) < 0) {
        ode->transport_uncertain = 1;
        return fail(ode, "ODE communication failed");
    }
    if (length != expected || reply[0] != code) {
        ode->transport_uncertain = 1;
        return fail(ode, "Unexpected ODE response");
    }
    /* Launch intentionally raises the media-change/check-error status bit. */
    if (code != 0xc5 && (reply[length - 1] & 0x10))
        return fail(ode, "ODE rejected command");
    return 0;
}

void ode_listing_init(OdeListing *listing)
{
    memset(listing, 0, sizeof(*listing));
}

void ode_listing_free(OdeListing *listing)
{
    size_t i;
    for (i = 0; i < listing->count; ++i)
        free((void *)listing->entries[i]);
    free(listing->entries);
    ode_listing_init(listing);
}

int ode_identify(Ode *ode)
{
    unsigned char reply[32];
    if (command(ode, 0x93, 0, 0, reply, 12) < 0)
        return -1;
    memcpy(ode->identity_reply, reply, 12);
    memcpy(ode->identity, reply + 2, 6);
    ode->identity[6] = 0;
    memcpy(ode->revision, reply + 8, 3);
    ode->error = NULL;
    return 0;
}

int ode_change_directory(Ode *ode, unsigned long id)
{
    unsigned char reply[32];
    if (command(ode, 0xc0, id, 0, reply, 2) < 0) {
        ode->navigation_known = 0;
        ode->navigation_failed = 1;
        return -1;
    }
    /* Track the low 24 bits only: parent navigation clears those bits even
     * when a multi-device root changes the medium prefix too. A zero startup
     * request does not establish a known firmware key. */
    ode->navigation_key = id & 0xffffffUL;
    if (ode->navigation_key == 0xffffffUL)
        ode->navigation_key = 0;
    ode->navigation_known = id != 0;
    return 0;
}

int ode_enter_directory(Ode *ode, const OdeListing *listing, const MenuEntry *entry, int workaround)
{
    size_t i;
    unsigned long key = entry->toc_id & 0xffffffUL;
    if (!entry->is_directory)
        return fail(ode, "Select a folder");
    if (!workaround || !key || (ode->navigation_known && ode->navigation_key != key))
        return ode_change_directory(ode, entry->toc_id);
    /* In the affected FCare implementation, C0 on a file stays in the same
     * directory but updates current_toc. A valid file therefore safely breaks
     * the collision even when the startup key is unknown. It is never queued
     * or mounted. The setting explicitly opts into these firmware semantics. */
    for (i = 0; i < listing->count; ++i) {
        const MenuEntry *pivot = listing->entries[i];
        if (!pivot->is_directory && (pivot->toc_id & 0xffffffUL)) {
            if (ode_change_directory(ode, pivot->toc_id) < 0)
                return -1;
            return ode_change_directory(ode, entry->toc_id);
        }
    }
    /* A sibling detour is safe only when its ID differs from the known key.
     * Otherwise a silently ignored first step could make '..' go too far. */
    if (ode->navigation_known) {
        for (i = 0; i < listing->count; ++i) {
            const MenuEntry *pivot = listing->entries[i];
            if (pivot->is_directory && pivot->toc_id != entry->toc_id &&
                (pivot->toc_id & 0xffffffUL)) {
                if (ode_change_directory(ode, pivot->toc_id) < 0 ||
                    ode_change_directory(ode, (pivot->toc_id & 0xff000000UL) | 0xffffffUL) < 0)
                    return -1;
                return ode_change_directory(ode, entry->toc_id);
            }
        }
    }
    return fail(ode, "No safe folder workaround here");
}

typedef struct TocReader {
    Ode *ode;
    unsigned long index;
    size_t position;
    unsigned char chunk[16];
} TocReader;

static int next_byte(TocReader *reader, unsigned char *value)
{
    if (reader->position >= ODE_TOC_BYTES)
        return fail(reader->ode, "Truncated directory record");
    if ((reader->position & 15) == 0) {
        unsigned char reply[32];
        if (command(reader->ode, 0xc1, reader->index, (unsigned int)reader->position, reply, 18) <
            0)
            return -1;
        memcpy(reader->chunk, reply + 1, 16);
    }
    *value = reader->chunk[reader->position & 15];
    ++reader->position;
    return 0;
}

static int next_word(TocReader *reader, unsigned long *value)
{
    unsigned char bytes[4];
    int i;
    for (i = 0; i < 4; ++i)
        if (next_byte(reader, &bytes[i]) < 0)
            return -1;
    *value = read_be32(bytes);
    return 0;
}

int ode_probe_directory(Ode *ode, unsigned long id, const char *name)
{
    TocReader reader;
    unsigned long local = id & 0xffffffUL;
    size_t expected = strlen(name) + 1;
    int record;
    if (!local || local == 0xffffffUL || expected > ODE_MAX_NAME)
        return 0;
    memset(&reader, 0, sizeof(reader));
    reader.ode = ode;
    reader.index = local - 1;
    for (record = 0; record < 2; ++record) {
        unsigned long flags, found, length;
        size_t i;
        if (next_word(&reader, &flags) < 0)
            return -1;
        if (flags == 0xffffffffUL)
            return 0;
        if (flags != 2)
            return 0;
        if (next_word(&reader, &found) < 0 || next_word(&reader, &length) < 0)
            return -1;
        if (length > ODE_TOC_BYTES - reader.position)
            return 0;
        if ((found & 0xffffffUL) == 0xffffffUL && length == 3) {
            static const unsigned char parent[3] = {'.', '.', 0};
            for (i = 0; i < 3; ++i) {
                unsigned char c;
                if (next_byte(&reader, &c) < 0)
                    return -1;
                if (c != parent[i])
                    return 0;
            }
            continue;
        }
        if (found != id || length != expected)
            return 0;
        for (i = 0; i < expected; ++i) {
            unsigned char c;
            if (next_byte(&reader, &c) < 0)
                return -1;
            if (c != (unsigned char)name[i])
                return 0;
        }
        return 1;
    }
    return 0;
}

/* FCare's firmware fills a cache on GET_TOC(offset=0). Read that cache via
 * 16-byte status chunks until its sentinel. The low 24 bits of the last real
 * ID are the next filesystem seek index; the high byte selects the medium.
 */
int ode_read_directory(Ode *ode, OdeListing *listing)
{
    unsigned long index = 0;
    int batches = 0;
    ode_listing_free(listing);
    ode->error = NULL;
    listing->entries = (const MenuEntry **)malloc(ODE_MAX_ENTRIES * sizeof(*listing->entries));
    if (!listing->entries)
        return fail(ode, "Not enough memory for directory");

    while (++batches <= ODE_MAX_ENTRIES + 1) {
        TocReader reader;
        unsigned long next_index = index;
        size_t real_entries = 0;
        memset(&reader, 0, sizeof(reader));
        reader.ode = ode;
        reader.index = index;
        while (reader.position + 4 <= ODE_TOC_BYTES) {
            unsigned long flags, id, length;
            MenuEntry *entry;
            char *name;
            size_t i;
            if (next_word(&reader, &flags) < 0)
                goto error;
            if (flags == 0xffffffffUL)
                break;
            if (flags != 1 && flags != 2) {
                fail(ode, "Unknown directory entry type");
                goto error;
            }
            if (next_word(&reader, &id) < 0 || next_word(&reader, &length) < 0)
                goto error;
            if (length < 1 || length > ODE_MAX_NAME || length > ODE_TOC_BYTES - reader.position) {
                fail(ode, "Invalid directory name length");
                goto error;
            }
            entry = (MenuEntry *)malloc(sizeof(*entry) + (size_t)length);
            if (!entry) {
                fail(ode, "Not enough memory for directory");
                goto error;
            }
            name = (char *)(entry + 1);
            for (i = 0; i < (size_t)length; ++i) {
                unsigned char byte;
                if (next_byte(&reader, &byte) < 0) {
                    free(entry);
                    goto error;
                }
                name[i] = (char)byte;
            }
            if (name[length - 1] != 0 || strlen(name) != (size_t)length - 1) {
                free(entry);
                fail(ode, "Invalid directory name");
                goto error;
            }
            if (flags == 2 && !strcmp(name, "..")) {
                listing->has_parent = 1;
                listing->parent_id = id;
                free(entry);
                continue;
            }
            if ((id & 0xffffffUL) == 0) {
                /* Device-list entries use a zero local ID. A device list
                 * fits in a single cache and has no filesystem continuation. */
                if (index != 0) {
                    free(entry);
                    fail(ode, "Invalid directory cursor");
                    goto error;
                }
            } else {
                unsigned long local_id = id & 0xffffffUL;
                if (local_id <= next_index || local_id == 0xffffffUL) {
                    free(entry);
                    fail(ode, "Directory cursor did not advance");
                    goto error;
                }
                next_index = local_id;
                ++real_entries;
            }
            if (listing->count == ODE_MAX_ENTRIES) {
                free(entry);
                fail(ode, "Directory exceeds 2048 entries");
                goto error;
            }
            entry->toc_id = id;
            entry->is_directory = flags == 2;
            entry->name = name;
            listing->entries[listing->count++] = entry;
        }
        if (ode->progress)
            ode->progress(ode->progress_context, listing->count);
        if (!real_entries) {
            menu_sort(listing->entries, listing->count);
            return 0;
        }
        index = next_index;
    }
    fail(ode, "Directory enumeration did not finish");
error:
    ode_listing_free(listing);
    return -1;
}

unsigned long ode_listing_fingerprint(const OdeListing *listing)
{
    unsigned long hash = 2166136261UL;
    size_t i;
    for (i = 0; i < listing->count + 1; ++i) {
        unsigned long id = i ? listing->entries[i - 1]->toc_id : listing->parent_id;
        const char *name = i ? listing->entries[i - 1]->name : "";
        int byte, kind = i ? listing->entries[i - 1]->is_directory : listing->has_parent;
        hash = (hash ^ (unsigned long)kind) * 16777619UL;
        for (byte = 0; byte < 4; ++byte) {
            hash = (hash ^ (id & 255UL)) * 16777619UL;
            id >>= 8;
        }
        do {
            hash = (hash ^ (unsigned char)*name) * 16777619UL;
        } while (*name++);
    }
    return hash;
}
int ode_recover_root(Ode *ode, unsigned long fingerprint, unsigned long volume, OdeListing *out)
{
    OdeListing current, verified;
    OdeProgress progress = ode->progress;
    unsigned int steps;
    int result = -1;
    ode_listing_init(&current);
    ode_listing_init(&verified);
    if (ode->transport_uncertain || strcmp(ode->identity, "LOCODE"))
        return -1;
    ode->navigation_failed = 1;
    ode->progress = NULL;
    if (ode_identify(ode) < 0 || strcmp(ode->identity, "LOCODE"))
        goto done;
    for (steps = 0; steps <= 32; ++steps) {
        if (ode_read_directory(ode, &current) < 0)
            goto done;
        if (!current.has_parent)
            break;
        if (steps == 32 || (current.parent_id & 0xffffffUL) != 0xffffffUL ||
            ode_change_directory(ode, current.parent_id) < 0)
            goto done;
        ode_listing_free(&current);
    }
    if (volume) {
        size_t i;
        for (i = 0; i < current.count; ++i)
            if (current.entries[i]->is_directory && current.entries[i]->toc_id == volume &&
                !(volume & 0xffffffUL))
                break;
        if (i == current.count || ode_change_directory(ode, volume) < 0)
            goto done;
        ode_listing_free(&current);
        if (ode_read_directory(ode, &current) < 0)
            goto done;
    }
    if (ode_listing_fingerprint(&current) != fingerprint ||
        ode_read_directory(ode, &verified) < 0 || ode_listing_fingerprint(&verified) != fingerprint)
        goto done;
    ode_listing_free(out);
    *out = verified;
    ode_listing_init(&verified);
    ode->navigation_failed = 0;
    ode->error = NULL;
    result = 0;
done:
    ode_listing_free(&current);
    ode_listing_free(&verified);
    ode->progress = progress;
    return result;
}

int ode_launch(Ode *ode, const MenuEntry *entry)
{
    unsigned char reply[32];
    unsigned long local_id;
    if (entry->is_directory)
        return fail(ode, "Select a disc image to launch");
    /* Re-prime the ODE cache, since sorting can select an earlier batch.
     * For device-list files (CD drives), the zero local ID needs no re-prime. */
    local_id = entry->toc_id & 0xffffffUL;
    if (local_id && command(ode, 0xc1, local_id - 1, 0, reply, 18) < 0)
        return -1;
    if (command(ode, 0xc3, 0, 0, reply, 2) < 0)
        return -1;
    if (command(ode, 0xc4, entry->toc_id, 0, reply, 4) < 0)
        return -1;
    if (!reply[1] || !reply[2])
        return fail(ode, "ODE could not queue this image");
    return command(ode, 0xc5, 0, 0, reply, 2);
}

void ode_playlist_init(OdePlaylist *playlist)
{
    memset(playlist, 0, sizeof(*playlist));
}

void ode_playlist_dispose(OdePlaylist *playlist)
{
    size_t i;
    for (i = 0; i < playlist->count; ++i)
        free(playlist->paths[i]);
    ode_playlist_init(playlist);
}

int ode_playlist_clear(Ode *ode, OdePlaylist *playlist)
{
    unsigned char reply[32];
    if (command(ode, 0xc3, 0, 0, reply, 2) < 0) {
        playlist->synchronized = 0;
        return -1;
    }
    ode_playlist_dispose(playlist);
    playlist->synchronized = 1;
    return 0;
}

int ode_playlist_add(Ode *ode, OdePlaylist *playlist, const MenuEntry *entry, const char *directory)
{
    unsigned char reply[32];
    unsigned long local_id;
    size_t directory_length, name_length, i;
    char *path;
    if (entry->is_directory)
        return fail(ode, "Select a disc image to queue");
    if (!playlist->synchronized)
        return fail(ode, "Queue uncertain. Clear it first.");
    if (playlist->dirty)
        return fail(ode, "Apply queue edits before adding remotely");
    if (playlist->count == ODE_MAX_DISCS)
        return fail(ode, "Disc queue is full (16 discs)");
    directory_length = strlen(directory);
    name_length = strlen(entry->name);
    if (directory_length > 8192 || name_length > ODE_MAX_NAME)
        return fail(ode, "Disc queue path is too long");
    path = (char *)malloc(directory_length + name_length + 2);
    if (!path)
        return fail(ode, "Not enough memory for disc queue");
    memcpy(path, directory, directory_length);
    path[directory_length] = '/';
    memcpy(path + directory_length + 1, entry->name, name_length + 1);
    for (i = 0; i < playlist->count; ++i) {
        if (!strcmp(playlist->paths[i], path)) {
            free(path);
            return fail(ode, "This disc is already queued");
        }
    }
    local_id = entry->toc_id & 0xffffffUL;
    if (local_id && command(ode, 0xc1, local_id - 1, 0, reply, 18) < 0)
        goto uncertain;
    /* Resolve the path while this directory is current. FCare copies it into
     * its playlist, so subsequent navigation cannot reinterpret this TOC ID. */
    if (command(ode, 0xc4, entry->toc_id, 0, reply, 4) < 0)
        goto uncertain;
    if (!reply[1] || !reply[2]) {
        free(path);
        return fail(ode, "ODE could not queue this image");
    }
    playlist->paths[playlist->count++] = path;
    return 0;
uncertain:
    /* A lost reply may hide a successful append. Refuse to launch until an
     * acknowledged clear restores a known playlist; never blindly retry. */
    free(path);
    playlist->synchronized = 0;
    return -1;
}

int ode_playlist_start(Ode *ode, OdePlaylist *playlist)
{
    unsigned char reply[32];
    if (!playlist->synchronized)
        return fail(ode, "Queue uncertain. Clear it first.");
    if (playlist->dirty)
        return fail(ode, "Apply queue edits before launch");
    if (!playlist->count)
        return fail(ode, "Disc queue is empty");
    /* One launch request starts the already assembled sequence. Do not clear
     * or re-add entries here, and do not retry an unacknowledged launch. */
    playlist->synchronized = 0;
    return command(ode, 0xc5, 0, 0, reply, 2);
}

int ode_playlist_append_local(Ode *ode, OdePlaylist *playlist, const char *path)
{
    size_t i, n = strlen(path);
    char *copy;
    if (!playlist->synchronized)
        return fail(ode, "Queue uncertain. Clear it first.");
    if (playlist->count == ODE_MAX_DISCS || n > 9216)
        return fail(ode, "Disc queue full or path too long");
    for (i = 0; i < playlist->count; ++i)
        if (!strcmp(playlist->paths[i], path))
            return fail(ode, "This disc is already queued");
    copy = (char *)malloc(n + 1);
    if (!copy)
        return fail(ode, "Not enough queue memory");
    memcpy(copy, path, n + 1);
    playlist->paths[playlist->count++] = copy;
    playlist->dirty = 1;
    return 0;
}
int ode_playlist_remove_local(OdePlaylist *playlist, size_t index)
{
    size_t i;
    if (!playlist->synchronized || index >= playlist->count)
        return -1;
    free(playlist->paths[index]);
    for (i = index + 1; i < playlist->count; ++i)
        playlist->paths[i - 1] = playlist->paths[i];
    playlist->paths[--playlist->count] = NULL;
    playlist->dirty = 1;
    return 0;
}
int ode_playlist_move_local(OdePlaylist *playlist, size_t from, size_t to)
{
    char *path;
    size_t i;
    if (!playlist->synchronized || from >= playlist->count || to >= playlist->count)
        return -1;
    if (from == to)
        return 0;
    path = playlist->paths[from];
    if (from < to)
        for (i = from; i < to; ++i)
            playlist->paths[i] = playlist->paths[i + 1];
    else
        for (i = from; i > to; --i)
            playlist->paths[i] = playlist->paths[i - 1];
    playlist->paths[to] = path;
    playlist->dirty = 1;
    return 0;
}
