/*
 * Reading library.3di (for Find) and metadata.3di (genre, publisher and
 * release date). See library_index.h for the file layout.
 */
#include "library_index.h"
#include "usb_file.h"
#include <stdlib.h>
#include <string.h>

/* The header is 24 bytes; the footer 8. */
#define INDEX_HEADER_BYTES 24
#define INDEX_FOOTER_BYTES 8

/* Header fields, by offset. */
#define HEADER_COUNT    8
#define HEADER_BYTES    12
#define HEADER_CRC      16
#define HEADER_RESERVED 20

/* The payload is read from the FCare in chunks of its 2048-byte buffer. */
#define READ_CHUNK 2048

/* The longest full menu path a Find entry may have, including its zero byte. */
#define LONGEST_CATALOG_PATH 9217

/* The longest metadata text field (genre, publisher or date), with its zero. */
#define LONGEST_METADATA_FIELD 97

/* Reads a 32-bit big-endian number. */
static unsigned long read_be32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) |
           p[3];
}

/* Reads a 16-bit big-endian number. */
static size_t read_be16(const unsigned char *p)
{
    return (size_t)p[0] * 256 + p[1];
}

/*
 * Whether `path` is a safe relative path to a disc image (see
 * library_path_safe). `folders_already` counts folders that will be put in
 * front of it (a volume folder), which also count towards LIBRARY_DEPTH.
 */
static int is_safe_disc_path(const char *path, unsigned int folders_already)
{
    const char *name_start = path;
    const char *p = path;
    unsigned int depth = folders_already;

    if (*path == 0 || strlen(path) > 8192)
        return 0;

    /* Walk the path, checking each name when its '/' or the end is reached. */
    for (;; ++p) {
        unsigned char c = (unsigned char)*p;

        if (c != 0 && (c < 32 || c == 127 || c == '\\' || c == ':'))
            return 0;

        if (c == 0 || c == '/') {
            size_t name_length = (size_t)(p - name_start);
            if (name_length == 0 || name_length >= ODE_MAX_NAME)
                return 0;
            if ((name_length == 1 && *name_start == '.') ||
                (name_length == 2 && !memcmp(name_start, "..", 2)))
                return 0;

            if (c == 0) {
                /* The last name is the disc itself: "x.iso" or "x.cue" at least. */
                return name_length > 4 &&
                       (!menu_path_compare(p - 4, ".iso") || !menu_path_compare(p - 4, ".cue"));
            }

            ++depth;
            if (depth > LIBRARY_DEPTH)
                return 0;
            name_start = p + 1;
        }
    }
}

int library_path_safe(const char *path)
{
    return is_safe_disc_path(path, 0);
}

/*
 * Reads `bytes` of an open file's payload into `data`, a chunk at a time.
 * Returns 0, 1 if `cancel` asked to stop (checked before every chunk), or -1
 * on a USB failure.
 */
static int read_payload(Ode *ode, OdeReadBuffer reader, unsigned char *data, size_t bytes,
                        MediaCancel cancel, void *context)
{
    size_t done = 0;
    while (done < bytes) {
        size_t chunk = bytes - done > READ_CHUNK ? READ_CHUNK : bytes - done;
        if (cancel && cancel(context))
            return 1;
        if (usb_file_read(ode, reader, data + done, chunk, 0) < 0)
            return -1;
        done += chunk;
    }
    return 0;
}

/*
 * Reads one length-prefixed string from a payload at `*position`, checking
 * that it fits within the payload's `bytes` and ends with its zero byte.
 * On success stores where the string starts and its length (including the
 * zero) and moves `*position` past it. Returns 0, or -1 if malformed.
 */
static int next_string(const unsigned char *payload, size_t bytes, size_t *position,
                       const char **text, size_t *length)
{
    size_t string_length;

    if (bytes - *position < 2)
        return -1;
    string_length = read_be16(payload + *position);
    *position += 2;
    if (string_length < 1 || string_length > bytes - *position ||
        payload[*position + string_length - 1] != 0)
        return -1;

    *text = (const char *)payload + *position;
    *length = string_length;
    *position += string_length;
    return 0;
}

int library_index_load(Ode *ode, OdeReadBuffer reader, const char *prefix, LibraryCatalog *catalog,
                       MediaCancel cancel, void *context)
{
    unsigned char header[INDEX_HEADER_BYTES];
    unsigned char *payload = NULL;
    LibraryCatalog loaded;
    unsigned long count, bytes;
    size_t prefix_length = strlen(prefix);
    /* A prefix longer than "/Storage" names a volume folder, one extra level. */
    unsigned int volume_folders = prefix_length > 8 ? 1 : 0;
    size_t position = 0, i;
    int result = -1;
    int read_result;

    memset(&loaded, 0, sizeof(loaded));

    /* The prefix must be "/Storage" or "/Storage/VOLUME". */
    if (strncmp(prefix, "/Storage", 8) || (prefix[8] && prefix[8] != '/') ||
        prefix_length > ODE_MAX_NAME + 9)
        return -1;

    if (usb_file_open(ode, "library.3di") < 0)
        return -1;

    /* Header: magic, sizes within the menu's limits, reserved field zero. */
    if (usb_file_read(ode, reader, header, INDEX_HEADER_BYTES, 1) < 0 ||
        memcmp(header, "OM3DLIB1", 8) || read_be32(header + HEADER_RESERVED))
        goto done;
    count = read_be32(header + HEADER_COUNT);
    bytes = read_be32(header + HEADER_BYTES);
    /* Each entry needs at least 3 bytes: a length and a one-character path... */
    if (count > LIBRARY_CATALOG_MAX || bytes > LIBRARY_CATALOG_BYTES ||
        (count && bytes < count * 3) || (!count && bytes))
        goto done;

    /* (malloc(0) may return NULL, so ask for at least one byte.) */
    payload = (unsigned char *)malloc(bytes ? (size_t)bytes : 1);
    loaded.paths = (char **)calloc(count ? (size_t)count : 1, sizeof(char *));
    if (!payload || !loaded.paths) {
        result = -3;
        goto done;
    }

    read_result = read_payload(ode, reader, payload, (size_t)bytes, cancel, context);
    if (read_result != 0) {
        result = read_result; /* 1 when cancelled, -1 on failure */
        goto done;
    }

    /* The whole payload arrived intact, and the file ends where it should. */
    if (usb_art_crc32(payload, (size_t)bytes) != read_be32(header + HEADER_CRC) ||
        usb_file_verify(ode, reader, (const unsigned char *)"END3DL01", INDEX_FOOTER_BYTES) < 0)
        goto done;

    /* Turn each stored path into a full menu path: prefix + "/" + path. */
    for (i = 0; i < (size_t)count; ++i) {
        const char *relative_path;
        size_t length, full_length;
        char *full_path;

        if (next_string(payload, (size_t)bytes, &position, &relative_path, &length) < 0)
            goto done;
        /* At least one character, and no zero byte before the end. */
        if (length < 2 || memchr(relative_path, 0, length - 1))
            goto done;
        if (!is_safe_disc_path(relative_path, volume_folders))
            goto done;

        full_length = prefix_length + 1 + length;
        if (full_length > LONGEST_CATALOG_PATH ||
            loaded.bytes + full_length > LIBRARY_CATALOG_BYTES)
            goto done;
        full_path = (char *)malloc(full_length);
        if (!full_path) {
            result = -3;
            goto done;
        }
        memcpy(full_path, prefix, prefix_length);
        full_path[prefix_length] = '/';
        memcpy(full_path + prefix_length + 1, relative_path, length);

        loaded.paths[loaded.count++] = full_path;
        loaded.bytes += full_length;
    }
    /* The entries must use up the payload exactly. */
    if (position != bytes)
        goto done;

    loaded.valid = 1;
    result = 0;

done:
    if (usb_file_close(ode) < 0)
        result = -1;
    if (result == 0) {
        /* Swap in the new catalog; `loaded` then holds nothing to free. */
        library_catalog_free(catalog);
        *catalog = loaded;
        memset(&loaded, 0, sizeof(loaded));
    }
    library_catalog_free(&loaded);
    free(payload);
    return result;
}

void library_metadata_free(LibraryMetadata *metadata)
{
    free(metadata->entries);
    free(metadata->storage);
    memset(metadata, 0, sizeof(*metadata));
}

/*
 * Checks a metadata.3di header (`data` points at the magic). Stores the entry
 * count and payload size, and returns 0, or -1 if invalid or over the limits.
 */
static int check_metadata_header(const unsigned char *data, unsigned long *count,
                                 unsigned long *bytes)
{
    if (memcmp(data, "OM3DMET1", 8) || read_be32(data + HEADER_RESERVED))
        return -1;
    *count = read_be32(data + HEADER_COUNT);
    *bytes = read_be32(data + HEADER_BYTES);
    if (*count > LIBRARY_METADATA_MAX || *bytes > LIBRARY_METADATA_BYTES)
        return -1;
    /* Payload plus a LibraryGameInfo (16 bytes) per entry must fit the budget. */
    if (*bytes + 16UL * *count > LIBRARY_METADATA_BYTES)
        return -1;
    return 0;
}

int library_metadata_parse(const unsigned char *data, size_t size, LibraryMetadata *metadata)
{
    LibraryMetadata loaded;
    unsigned long count, bytes;
    size_t i, position = 0;

    memset(&loaded, 0, sizeof(loaded));

    /* Header, footer exactly after the payload, and the payload's CRC. */
    if (size < INDEX_HEADER_BYTES + INDEX_FOOTER_BYTES)
        return -1;
    if (check_metadata_header(data, &count, &bytes) < 0)
        return -1;
    if (size != INDEX_HEADER_BYTES + bytes + INDEX_FOOTER_BYTES ||
        memcmp(data + INDEX_HEADER_BYTES + bytes, "END3DM01", INDEX_FOOTER_BYTES) ||
        usb_art_crc32(data + INDEX_HEADER_BYTES, (size_t)bytes) != read_be32(data + HEADER_CRC))
        return -1;

    /* Keep a copy of the payload; the entries will point into it. */
    loaded.storage = (unsigned char *)malloc(bytes ? (size_t)bytes : 1);
    loaded.entries = (LibraryGameInfo *)calloc(count ? (size_t)count : 1, sizeof(LibraryGameInfo));
    if (!loaded.storage || !loaded.entries) {
        library_metadata_free(&loaded);
        return -3;
    }
    memcpy(loaded.storage, data + INDEX_HEADER_BYTES, (size_t)bytes);

    for (i = 0; i < (size_t)count; ++i) {
        const char *fields[4]; /* path, genre, publisher, release date */
        unsigned int field;

        /*
         * Only the shape of each entry is checked here: every string ends
         * where its length says. Checking every entry's content took most of
         * a second on the console, and it is not needed: paths are only
         * compared with other paths (and checked with library_path_safe
         * before use), text is drawn with anything unprintable shown as '?',
         * and an entry out of order is simply not found.
         */
        for (field = 0; field < 4; ++field) {
            size_t length;
            if (next_string(loaded.storage, (size_t)bytes, &position, &fields[field], &length) < 0)
                goto invalid;
            /* The path needs at least one character; the others may be empty
             * but have a size limit. */
            if (field == 0 && length < 2)
                goto invalid;
            if (field > 0 && length > LONGEST_METADATA_FIELD)
                goto invalid;
        }
        loaded.entries[i].path = fields[0];
        loaded.entries[i].genre = fields[1];
        loaded.entries[i].publisher = fields[2];
        loaded.entries[i].released = fields[3];
    }
    if (position != bytes)
        goto invalid;

    loaded.count = (size_t)count;
    library_metadata_free(metadata);
    *metadata = loaded;
    return 0;

invalid:
    library_metadata_free(&loaded);
    return -1;
}

int library_metadata_load(Ode *ode, OdeReadBuffer reader, LibraryMetadata *metadata)
{
    unsigned char header[INDEX_HEADER_BYTES];
    unsigned char *file = NULL; /* the whole file, rebuilt in memory */
    unsigned long count, bytes;
    LibraryMetadata loaded;
    int result = -1;

    memset(&loaded, 0, sizeof(loaded));
    if (usb_file_open(ode, "metadata.3di") < 0)
        return -1;

    if (usb_file_read(ode, reader, header, INDEX_HEADER_BYTES, 1) < 0)
        goto done;
    if (check_metadata_header(header, &count, &bytes) < 0)
        goto done;

    /* Rebuild the complete file in memory, header to footer, so
     * library_metadata_parse can check it all in one place. */
    file = (unsigned char *)malloc((size_t)bytes + INDEX_HEADER_BYTES + INDEX_FOOTER_BYTES);
    if (!file) {
        result = -3;
        goto done;
    }
    memcpy(file, header, INDEX_HEADER_BYTES);
    if (read_payload(ode, reader, file + INDEX_HEADER_BYTES, (size_t)bytes, NULL, NULL) < 0)
        goto done;
    /* The footer must really be on the drive right after the payload. */
    if (usb_file_verify(ode, reader, (const unsigned char *)"END3DM01", INDEX_FOOTER_BYTES) < 0)
        goto done;
    memcpy(file + INDEX_HEADER_BYTES + bytes, "END3DM01", INDEX_FOOTER_BYTES);

    result = library_metadata_parse(file, (size_t)bytes + INDEX_HEADER_BYTES + INDEX_FOOTER_BYTES,
                                    &loaded);

done:
    if (usb_file_close(ode) < 0)
        result = -1;
    if (result == 0) {
        /* Swap in the new metadata; `loaded` then holds nothing to free. */
        library_metadata_free(metadata);
        *metadata = loaded;
        memset(&loaded, 0, sizeof(loaded));
    }
    library_metadata_free(&loaded);
    free(file);
    return result;
}

const LibraryGameInfo *library_metadata_find(const LibraryMetadata *metadata, const char *path)
{
    size_t low = 0, high = metadata->count;

    /* Binary search: halve the range [low, high) until the path is found. */
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        int order = menu_path_compare(path, metadata->entries[middle].path);
        if (order == 0)
            return metadata->entries + middle;
        if (order < 0)
            high = middle;
        else
            low = middle + 1;
    }
    return NULL;
}
