/*
 * The FCare's browsing and launching commands. See ode.h for the protocol.
 */
#include "ode.h"

#include <stdlib.h>
#include <string.h>

/* Command codes. */
#define CMD_IDENTIFY      0x93
#define CMD_CHANGE_FOLDER 0xc0
#define CMD_READ_LISTING  0xc1
#define CMD_CLEAR_QUEUE   0xc3
#define CMD_ADD_TO_QUEUE  0xc4
#define CMD_START_QUEUE   0xc5

/* Reply lengths. A listing read returns the echo, 16 bytes and a status byte. */
#define IDENTIFY_REPLY 12
#define LISTING_REPLY  18
#define LISTING_CHUNK  16

/* Status bit set in a reply's last byte when the command failed. */
#define STATUS_ERROR 0x10

/* Listing record flags. */
#define RECORD_FILE   1
#define RECORD_FOLDER 2
#define RECORD_END    0xffffffffUL

/* An id's top byte names the USB volume; the low 24 bits the entry on it.
 * With every entry bit set, an id means "the parent of the current folder". */
#define VOLUME_BITS  0xff000000UL
#define ENTRY_BITS   0x00ffffffUL
#define PARENT_ENTRY 0x00ffffffUL

/* The deepest the recovery climb goes before giving up. */
#define RECOVERY_MAX_STEPS 32

static unsigned long read_be32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) |
           (unsigned long)p[3];
}

/* Records an error message and returns -1. */
static int fail(Ode *ode, const char *message)
{
    ode->error = message;
    return -1;
}

/*
 * Sends one command: the code, a 32-bit id and a 16-bit offset, all
 * big-endian. The reply must be exactly `expected` bytes and echo the code.
 * Returns 0, or -1 if the exchange failed, the reply was malformed (which
 * also marks the transport as uncertain) or the ODE reported an error.
 */
static int send_command(Ode *ode, unsigned char code, unsigned long id, unsigned int offset,
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
    /*
     * Starting the queue sets the error bit on purpose: it is how the drive
     * reports "the disc changed", which makes the console boot the new disc.
     */
    if (code != CMD_START_QUEUE && (reply[length - 1] & STATUS_ERROR))
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

    if (send_command(ode, CMD_IDENTIFY, 0, 0, reply, IDENTIFY_REPLY) < 0)
        return -1;
    /* Bytes 2 to 7 name the firmware; 8 to 10 are its version. */
    memcpy(ode->identity_reply, reply, IDENTIFY_REPLY);
    memcpy(ode->identity, reply + 2, 6);
    ode->identity[6] = 0;
    memcpy(ode->revision, reply + 8, 3);
    ode->error = NULL;
    return 0;
}

int ode_change_directory(Ode *ode, unsigned long id)
{
    unsigned char reply[32];

    if (send_command(ode, CMD_CHANGE_FOLDER, id, 0, reply, 2) < 0) {
        ode->navigation_known = 0;
        ode->navigation_failed = 1;
        return -1;
    }

    /*
     * Remember the entry number for the folder-change bug. Only the low 24
     * bits matter: going to a parent clears them even when the volume byte
     * changes too, and a parent id counts as entry 0. The startup request
     * (id 0) does not tell us the firmware's own value, so it stays unknown.
     */
    ode->navigation_key = id & ENTRY_BITS;
    if (ode->navigation_key == PARENT_ENTRY)
        ode->navigation_key = 0;
    ode->navigation_known = id != 0;
    return 0;
}

int ode_enter_directory(Ode *ode, const OdeListing *listing, const MenuEntry *entry, int workaround)
{
    size_t i;
    unsigned long key = entry->toc_id & ENTRY_BITS;

    if (!entry->is_directory)
        return fail(ode, "Select a folder");

    /* No bug to avoid: the workaround is off, the target is a volume, or the
     * last change is known to have used a different entry number. */
    if (!workaround || !key || (ode->navigation_known && ode->navigation_key != key))
        return ode_change_directory(ode, entry->toc_id);

    /*
     * Detour through a file first. On the affected FCare firmware, "changing
     * folder" to a file stays in the same folder but updates the remembered
     * entry number, so it safely breaks the collision even when the last
     * number is unknown. The file is never queued or mounted.
     */
    for (i = 0; i < listing->count; ++i) {
        const MenuEntry *pivot = listing->entries[i];
        if (!pivot->is_directory && (pivot->toc_id & ENTRY_BITS)) {
            if (ode_change_directory(ode, pivot->toc_id) < 0)
                return -1;
            return ode_change_directory(ode, entry->toc_id);
        }
    }

    /*
     * No files here: detour into a sibling folder and back out. That is only
     * safe when the last entry number is known: if the first step were
     * silently ignored, the step back to ".." would climb too far.
     */
    if (ode->navigation_known) {
        for (i = 0; i < listing->count; ++i) {
            const MenuEntry *pivot = listing->entries[i];
            if (pivot->is_directory && pivot->toc_id != entry->toc_id &&
                (pivot->toc_id & ENTRY_BITS)) {
                if (ode_change_directory(ode, pivot->toc_id) < 0 ||
                    ode_change_directory(ode, (pivot->toc_id & VOLUME_BITS) | PARENT_ENTRY) < 0)
                    return -1;
                return ode_change_directory(ode, entry->toc_id);
            }
        }
    }
    return fail(ode, "No safe folder workaround here");
}

/*
 * Reads a listing byte by byte, fetching 16 bytes at a time from the FCare.
 * `start` is the entry number the listing starts after.
 */
typedef struct ListingReader {
    Ode *ode;
    unsigned long start;
    size_t position;                    /* the next byte to read */
    unsigned char chunk[LISTING_CHUNK]; /* the 16 bytes around `position` */
} ListingReader;

static int next_byte(ListingReader *reader, unsigned char *value)
{
    if (reader->position >= ODE_TOC_BYTES)
        return fail(reader->ode, "Truncated directory record");

    /* At the start of each 16-byte chunk, fetch it. */
    if (reader->position % LISTING_CHUNK == 0) {
        unsigned char reply[32];
        if (send_command(reader->ode, CMD_READ_LISTING, reader->start,
                         (unsigned int)reader->position, reply, LISTING_REPLY) < 0)
            return -1;
        memcpy(reader->chunk, reply + 1, LISTING_CHUNK);
    }
    *value = reader->chunk[reader->position % LISTING_CHUNK];
    ++reader->position;
    return 0;
}

static int next_word(ListingReader *reader, unsigned long *value)
{
    unsigned char bytes[4];
    int i;
    for (i = 0; i < 4; ++i) {
        if (next_byte(reader, &bytes[i]) < 0)
            return -1;
    }
    *value = read_be32(bytes);
    return 0;
}

/* Reads `length` bytes and compares them with `expected`. Returns 1 if they
 * match, 0 if not, or -1 on a USB failure. */
static int next_bytes_match(ListingReader *reader, const unsigned char *expected, size_t length)
{
    size_t i;
    for (i = 0; i < length; ++i) {
        unsigned char c;
        if (next_byte(reader, &c) < 0)
            return -1;
        if (c != expected[i])
            return 0;
    }
    return 1;
}

int ode_probe_directory(Ode *ode, unsigned long id, const char *name)
{
    static const unsigned char parent_name[3] = {'.', '.', 0};
    ListingReader reader;
    unsigned long entry_number = id & ENTRY_BITS;
    size_t name_length = strlen(name) + 1;
    int record;

    if (!entry_number || entry_number == PARENT_ENTRY || name_length > ODE_MAX_NAME)
        return 0;

    /*
     * A listing that starts just before the entry shows it first, perhaps
     * after the folder's ".." record. Check those (at most two) records.
     */
    memset(&reader, 0, sizeof(reader));
    reader.ode = ode;
    reader.start = entry_number - 1;
    for (record = 0; record < 2; ++record) {
        unsigned long flags, found_id, length;
        int matched;

        if (next_word(&reader, &flags) < 0)
            return -1;
        if (flags != RECORD_FOLDER)
            return 0; /* the end of the listing, or a file: not our folder */
        if (next_word(&reader, &found_id) < 0 || next_word(&reader, &length) < 0)
            return -1;
        if (length > ODE_TOC_BYTES - reader.position)
            return 0;

        /* Skip a ".." record and look at the next one. */
        if ((found_id & ENTRY_BITS) == PARENT_ENTRY && length == 3) {
            matched = next_bytes_match(&reader, parent_name, 3);
            if (matched <= 0)
                return matched;
            continue;
        }

        if (found_id != id || length != name_length)
            return 0;
        return next_bytes_match(&reader, (const unsigned char *)name, name_length);
    }
    return 0;
}

/*
 * Reads the records of one listing (starting after entry `start`) into
 * `listing`. Stores the highest entry number seen in *last_entry and how
 * many real entries (not volumes or "..") were added in *added.
 * Returns 0, or -1.
 */
static int read_listing_batch(Ode *ode, OdeListing *listing, unsigned long start,
                              unsigned long *last_entry, size_t *added)
{
    ListingReader reader;

    memset(&reader, 0, sizeof(reader));
    reader.ode = ode;
    reader.start = start;
    *last_entry = start;
    *added = 0;

    while (reader.position + 4 <= ODE_TOC_BYTES) {
        unsigned long flags, id, length;
        MenuEntry *entry;
        char *name;
        size_t i;

        if (next_word(&reader, &flags) < 0)
            return -1;
        if (flags == RECORD_END)
            break;
        if (flags != RECORD_FILE && flags != RECORD_FOLDER)
            return fail(ode, "Unknown directory entry type");
        if (next_word(&reader, &id) < 0 || next_word(&reader, &length) < 0)
            return -1;
        if (length < 1 || length > ODE_MAX_NAME || length > ODE_TOC_BYTES - reader.position)
            return fail(ode, "Invalid directory name length");

        /* The entry and its name share one allocation, name just after. */
        entry = (MenuEntry *)malloc(sizeof(*entry) + (size_t)length);
        if (!entry)
            return fail(ode, "Not enough memory for directory");
        name = (char *)(entry + 1);
        for (i = 0; i < (size_t)length; ++i) {
            unsigned char byte;
            if (next_byte(&reader, &byte) < 0) {
                free(entry);
                return -1;
            }
            name[i] = (char)byte;
        }
        /* Exactly one zero byte, at the end. */
        if (name[length - 1] != 0 || strlen(name) != (size_t)length - 1) {
            free(entry);
            return fail(ode, "Invalid directory name");
        }

        /* ".." becomes the listing's parent, not an entry. */
        if (flags == RECORD_FOLDER && !strcmp(name, "..")) {
            listing->has_parent = 1;
            listing->parent_id = id;
            free(entry);
            continue;
        }

        if ((id & ENTRY_BITS) == 0) {
            /* A volume in the top-level volume list. That list always fits in
             * one listing, so volumes can only appear in the first batch. */
            if (start != 0) {
                free(entry);
                return fail(ode, "Invalid directory cursor");
            }
        } else {
            /* Entry numbers must keep rising, or the batches would never end. */
            unsigned long entry_number = id & ENTRY_BITS;
            if (entry_number <= *last_entry || entry_number == PARENT_ENTRY) {
                free(entry);
                return fail(ode, "Directory cursor did not advance");
            }
            *last_entry = entry_number;
            ++*added;
        }

        if (listing->count == ODE_MAX_ENTRIES) {
            free(entry);
            return fail(ode, "Directory exceeds 2048 entries");
        }
        entry->toc_id = id;
        entry->is_directory = flags == RECORD_FOLDER;
        entry->name = name;
        listing->entries[listing->count++] = entry;
    }
    return 0;
}

int ode_read_directory(Ode *ode, OdeListing *listing)
{
    unsigned long start = 0;
    int batches = 0;

    ode_listing_free(listing);
    ode->error = NULL;
    listing->entries = (const MenuEntry **)malloc(ODE_MAX_ENTRIES * sizeof(*listing->entries));
    if (!listing->entries)
        return fail(ode, "Not enough memory for directory");

    /*
     * Read listing after listing: each starts after the last entry of the
     * one before, until one adds nothing new. The FCare fills its listing
     * when asked for offset 0 of a new start, which next_byte does.
     */
    while (++batches <= ODE_MAX_ENTRIES + 1) {
        unsigned long last_entry;
        size_t added;

        if (read_listing_batch(ode, listing, start, &last_entry, &added) < 0)
            goto error;
        if (ode->progress)
            ode->progress(ode->progress_context, listing->count);
        if (added == 0) {
            menu_sort(listing->entries, listing->count);
            return 0;
        }
        start = last_entry;
    }
    fail(ode, "Directory enumeration did not finish");

error:
    ode_listing_free(listing);
    return -1;
}

unsigned long ode_listing_fingerprint(const OdeListing *listing)
{
    /* FNV-1a, a simple and well-known hash: for each byte, XOR it in and
     * multiply by a fixed prime. */
    const unsigned long fnv_offset = 2166136261UL;
    const unsigned long fnv_prime = 16777619UL;
    unsigned long hash = fnv_offset;
    size_t i;

    /* The ".." entry first, then every entry: kind, id bytes, name bytes
     * (with its zero, so "ab"+"c" differs from "a"+"bc"). */
    for (i = 0; i < listing->count + 1; ++i) {
        unsigned long id = i ? listing->entries[i - 1]->toc_id : listing->parent_id;
        const char *name = i ? listing->entries[i - 1]->name : "";
        int kind = i ? listing->entries[i - 1]->is_directory : listing->has_parent;
        int byte;

        hash = (hash ^ (unsigned long)kind) * fnv_prime;
        for (byte = 0; byte < 4; ++byte) {
            hash = (hash ^ (id & 255UL)) * fnv_prime;
            id >>= 8;
        }
        do {
            hash = (hash ^ (unsigned char)*name) * fnv_prime;
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

    /* Browsing stays off unless every step below succeeds. */
    ode->navigation_failed = 1;
    ode->progress = NULL;

    /* Check the FCare still answers sensibly. */
    if (ode_identify(ode) < 0 || strcmp(ode->identity, "LOCODE"))
        goto done;

    /* Climb through ".." entries until a folder has none: the top. */
    for (steps = 0; steps <= RECOVERY_MAX_STEPS; ++steps) {
        if (ode_read_directory(ode, &current) < 0)
            goto done;
        if (!current.has_parent)
            break;
        if (steps == RECOVERY_MAX_STEPS || (current.parent_id & ENTRY_BITS) != PARENT_ENTRY ||
            ode_change_directory(ode, current.parent_id) < 0)
            goto done;
        ode_listing_free(&current);
    }

    /* On a multi-volume drive, go back into the volume the menu was using. */
    if (volume) {
        size_t i;
        for (i = 0; i < current.count; ++i) {
            if (current.entries[i]->is_directory && current.entries[i]->toc_id == volume &&
                !(volume & ENTRY_BITS))
                break;
        }
        if (i == current.count || ode_change_directory(ode, volume) < 0)
            goto done;
        ode_listing_free(&current);
        if (ode_read_directory(ode, &current) < 0)
            goto done;
    }

    /* It must be the same top folder as at startup, twice in a row. */
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

/*
 * The FCare resolves a queued id against its current listing batch. Sorting
 * may have put the entry before the batch the FCare last read, so ask for
 * the batch that starts just before it. (Volume entries need nothing.)
 */
static int prepare_listing_for(Ode *ode, const MenuEntry *entry)
{
    unsigned char reply[32];
    unsigned long entry_number = entry->toc_id & ENTRY_BITS;
    if (!entry_number)
        return 0;
    return send_command(ode, CMD_READ_LISTING, entry_number - 1, 0, reply, LISTING_REPLY);
}

int ode_launch(Ode *ode, const MenuEntry *entry)
{
    unsigned char reply[32];

    if (entry->is_directory)
        return fail(ode, "Select a disc image to launch");

    if (prepare_listing_for(ode, entry) < 0)
        return -1;
    if (send_command(ode, CMD_CLEAR_QUEUE, 0, 0, reply, 2) < 0)
        return -1;
    if (send_command(ode, CMD_ADD_TO_QUEUE, entry->toc_id, 0, reply, 4) < 0)
        return -1;
    /* Bytes 1 and 2 of the reply are nonzero when the disc was queued. */
    if (!reply[1] || !reply[2])
        return fail(ode, "ODE could not queue this image");
    return send_command(ode, CMD_START_QUEUE, 0, 0, reply, 2);
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
    if (send_command(ode, CMD_CLEAR_QUEUE, 0, 0, reply, 2) < 0) {
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

    /* The disc's full path, for display and to refuse duplicates. */
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

    /*
     * Add it while its folder is the current one: the FCare copies the
     * resolved path into its queue, so later browsing cannot change which
     * disc this id meant.
     */
    if (prepare_listing_for(ode, entry) < 0)
        goto uncertain;
    if (send_command(ode, CMD_ADD_TO_QUEUE, entry->toc_id, 0, reply, 4) < 0)
        goto uncertain;
    if (!reply[1] || !reply[2]) {
        free(path);
        return fail(ode, "ODE could not queue this image");
    }
    playlist->paths[playlist->count++] = path;
    return 0;

uncertain:
    /*
     * A lost reply may hide a disc that was added after all. Never retry
     * blindly: refuse to start the queue until a confirmed clear makes it
     * known again.
     */
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

    /*
     * One start request launches the queue as it stands. It is not cleared
     * or rebuilt here, and a start whose reply is lost is never retried
     * (the console may already be booting the disc).
     */
    playlist->synchronized = 0;
    return send_command(ode, CMD_START_QUEUE, 0, 0, reply, 2);
}

int ode_playlist_append_local(Ode *ode, OdePlaylist *playlist, const char *path)
{
    size_t i, length = strlen(path);
    char *copy;

    if (!playlist->synchronized)
        return fail(ode, "Queue uncertain. Clear it first.");
    if (playlist->count == ODE_MAX_DISCS || length > 9216)
        return fail(ode, "Disc queue full or path too long");
    for (i = 0; i < playlist->count; ++i) {
        if (!strcmp(playlist->paths[i], path))
            return fail(ode, "This disc is already queued");
    }

    copy = (char *)malloc(length + 1);
    if (!copy)
        return fail(ode, "Not enough queue memory");
    memcpy(copy, path, length + 1);
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
    char *moving;
    size_t i;

    if (!playlist->synchronized || from >= playlist->count || to >= playlist->count)
        return -1;
    if (from == to)
        return 0;

    /* Shift the discs in between by one place, then drop the moved one in. */
    moving = playlist->paths[from];
    if (from < to) {
        for (i = from; i < to; ++i)
            playlist->paths[i] = playlist->paths[i + 1];
    } else {
        for (i = from; i > to; --i)
            playlist->paths[i] = playlist->paths[i - 1];
    }
    playlist->paths[to] = moving;
    playlist->dirty = 1;
    return 0;
}
