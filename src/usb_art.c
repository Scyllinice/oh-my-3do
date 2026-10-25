/*
 * Cover and preview indexes, and CRC32. See usb_art.h for the index layout.
 */
#include "usb_art.h"
#include "usb_file.h"
#include <stdlib.h>
#include <string.h>

/* Index header fields, by offset. */
#define INDEX_HEADER_BYTES 24
#define HEADER_COUNT       8
#define HEADER_BYTES       12
#define HEADER_CRC         16
#define HEADER_LOCATION    20

/* Limits on an index. */
#define INDEX_MAX_ENTRIES 2048
#define INDEX_MAX_BYTES   (128UL * 1024UL)

/* The longest media file name, and the shortest ("x.png"), with their zeros. */
#define LONGEST_MEDIA_NAME  97
#define SHORTEST_MEDIA_NAME 6

/* The CRC32 polynomial, in the bit order zip and PNG use. */
#define CRC32_POLYNOMIAL 0xedb88320UL

static unsigned long read_be32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) |
           p[3];
}

unsigned long usb_art_crc32_update(unsigned long crc, const unsigned char *data, size_t size)
{
    /*
     * A table of the CRC of every possible byte, built on first use. It takes
     * 1 KiB, but lets the ARM60 process a byte with one lookup instead of
     * eight bit steps. The menu builds it before any bulk media read.
     */
    static unsigned long table[256];
    static int table_ready;
    size_t i;

    if (!table_ready) {
        unsigned int byte, bit;
        for (byte = 0; byte < 256; ++byte) {
            unsigned long value = byte;
            for (bit = 0; bit < 8; ++bit)
                value = (value >> 1) ^ ((value & 1) ? CRC32_POLYNOMIAL : 0);
            table[byte] = value;
        }
        table_ready = 1;
    }

    for (i = 0; i < size; ++i)
        crc = (crc >> 8) ^ table[(crc ^ data[i]) & 255];
    return crc;
}

unsigned long usb_art_crc32(const unsigned char *data, size_t size)
{
    return usb_art_crc32_update(0xffffffffUL, data, size) ^ 0xffffffffUL;
}

void usb_art_index_free(UsbArtIndex *index)
{
    free(index->entries);
    free(index->storage);
    memset(index, 0, sizeof(*index));
}

int usb_media_filename(const char *name)
{
    size_t i, length = strlen(name);

    if (length < 5 || length > 96 || name[0] == '.')
        return 0;
    if (menu_path_compare(name + length - 4, ".3dc") &&
        menu_path_compare(name + length - 4, ".png") &&
        menu_path_compare(name + length - 4, ".3dv"))
        return 0;
    for (i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (c < 32 || c == 127 || c == '/' || c == '\\' || c == ':' || c == '|')
            return 0;
    }
    return 1;
}

const char *usb_media_find(const UsbArtIndex *index, const char *path)
{
    const char *file = menu_cover_find(index->entries, index->count, path);
    return file && usb_media_filename(file) ? file : NULL;
}

int usb_media_index_load(Ode *ode, OdeReadBuffer reader, UsbArtIndex *index, const char *folder)
{
    unsigned char header[INDEX_HEADER_BYTES];
    char path[32];
    UsbArtIndex loaded;
    size_t bytes, count, position = 0, i;
    unsigned long location;
    int result = -1;

    memset(&loaded, 0, sizeof(loaded));
    usb_art_index_free(index);
    if (strcmp(folder, "covers") && strcmp(folder, "previews"))
        return -1;

    strcpy(path, folder);
    strcat(path, "/index.3di");
    if (usb_file_open(ode, path) < 0)
        return -1;

    /* The header: magic, and a known media location. */
    if (usb_file_read(ode, reader, header, sizeof(header), 1) < 0 || memcmp(header, "OM3DIDX1", 8))
        goto done;
    location = read_be32(header + HEADER_LOCATION);
    if (location > 1)
        goto done;
    loaded.beside = (int)location;
    count = (size_t)read_be32(header + HEADER_COUNT);
    bytes = (size_t)read_be32(header + HEADER_BYTES);

    /* An empty index (no entries, no payload, zero CRC) is fine. */
    if (!count && !bytes && !read_be32(header + HEADER_CRC)) {
        result = 0;
        goto done;
    }
    /* Each entry needs at least 8 bytes of payload. */
    if (!count || count > INDEX_MAX_ENTRIES || bytes < count * 8 || bytes > INDEX_MAX_BYTES)
        goto done;

    loaded.storage = (unsigned char *)malloc(bytes);
    loaded.entries = (MenuCover *)malloc(count * sizeof(MenuCover));
    if (!loaded.storage || !loaded.entries)
        goto done;
    if (usb_file_read(ode, reader, loaded.storage, bytes, 0) < 0 ||
        usb_art_crc32(loaded.storage, bytes) != read_be32(header + HEADER_CRC))
        goto done;

    for (i = 0; i < count; ++i) {
        size_t path_length, name_length;

        if (bytes - position < 4)
            goto done;
        path_length = (size_t)loaded.storage[position] * 256 + loaded.storage[position + 1];
        name_length = (size_t)loaded.storage[position + 2] * 256 + loaded.storage[position + 3];
        position += 4;

        /*
         * Only the shape is checked here: each string ends where its length
         * says. Checking every entry's content took most of a second on the
         * console. A file name is checked when it is used (usb_media_find),
         * paths are only ever compared, and an entry out of order is simply
         * not found.
         */
        if (path_length < 2 || name_length < SHORTEST_MEDIA_NAME ||
            name_length > LONGEST_MEDIA_NAME || path_length + name_length > bytes - position)
            goto done;
        if (loaded.storage[position + path_length - 1] ||
            loaded.storage[position + path_length + name_length - 1])
            goto done;

        loaded.entries[i].path = (char *)loaded.storage + position;
        loaded.entries[i].file = (char *)loaded.storage + position + path_length;
        position += path_length + name_length;
    }
    if (position != bytes)
        goto done;
    loaded.count = count;
    result = 0;

done:
    if (usb_file_close(ode) < 0)
        result = -1;
    if (result == 0) {
        /* Hand the new index over; `loaded` then holds nothing to free. */
        *index = loaded;
        memset(&loaded, 0, sizeof(loaded));
    }
    usb_art_index_free(&loaded);
    return result;
}

int usb_art_index_load(Ode *ode, OdeReadBuffer reader, UsbArtIndex *index)
{
    return usb_media_index_load(ode, reader, index, "covers");
}
