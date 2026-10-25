#include "usb_art.h"
#include "usb_file.h"
#include <stdlib.h>
#include <string.h>

static unsigned long word(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) |
           p[3];
}

unsigned long usb_art_crc32_update(unsigned long crc, const unsigned char *data, size_t size)
{
    static unsigned long table[256];
    static int ready;
    size_t i;
    if (!ready) {
        unsigned int entry, bit;
        /* One KiB on ARM buys one lookup per byte instead of two. The menu
         * initializes this once, before bulk media reads. */
        for (entry = 0; entry < 256; ++entry) {
            unsigned long value = entry;
            for (bit = 0; bit < 8; ++bit)
                value = (value >> 1) ^ ((value & 1) ? 0xedb88320UL : 0);
            table[entry] = value;
        }
        ready = 1;
    }
    for (i = 0; i < size; ++i)
        crc = (crc >> 8) ^ table[(crc ^ data[i]) & 255];
    return crc;
}
unsigned long usb_art_crc32(const unsigned char *data, size_t size)
{
    return usb_art_crc32_update(0xffffffffUL, data, size) ^ 0xffffffffUL;
}

static int exchange(Ode *ode, unsigned char *packet, unsigned char *reply, size_t expected)
{
    size_t length = 0;
    if (ode->navigation_failed)
        return -1;
    if (ode->transfer(ode->context, packet, reply, 32, &length) < 0 || length != expected ||
        reply[0] != packet[0]) {
        ode->transport_uncertain = 1;
        ode->navigation_failed = 1;
        return -1;
    }
    if (reply[length - 1] & 0x10)
        return -1;
    return 0;
}

static int buffer_word(Ode *ode, unsigned int offset, const unsigned char *p)
{
    unsigned char packet[7], reply[32];
    packet[0] = 0xe6;
    packet[1] = (unsigned char)(offset >> 8);
    packet[2] = (unsigned char)offset;
    memcpy(packet + 3, p, 4);
    return exchange(ode, packet, reply, 2);
}

int usb_file_open(Ode *ode, const char *name)
{
    unsigned char packet[7], reply[32], part[4];
    char located[128];
    size_t length, i;
    const char *folder = ode->media_beside ? ode->media_folder : NULL;
    if (ode->navigation_failed)
        return -1;
    if ((folder ? strlen(folder) + 1 : 0) + strlen(name) + sizeof(".ohmy3do/") > sizeof(located))
        return -1;
    located[0] = 0;
    if (folder) {
        strcpy(located, folder);
        strcat(located, "/");
    }
    strcat(located, ".ohmy3do/");
    strcat(located, name);
    name = located;
    length = strlen(name) + 1;
    if (strcmp(ode->identity, "LOCODE") || length > 128)
        return -1;
    memcpy(ode->file_path, name, length);
    for (i = 0; i < length; i += 4) {
        size_t n = length - i;
        if (n > 4)
            n = 4;
        memset(part, 0, sizeof(part));
        memcpy(part, name + i, n);
        if (buffer_word(ode, (unsigned int)(i / 4), part) < 0)
            return -1;
    }
    memset(packet, 0, sizeof(packet));
    packet[0] = 0xe1;
    packet[2] = (unsigned char)(length >> 8);
    packet[3] = (unsigned char)length;
    if (exchange(ode, packet, reply, 3) < 0 || !reply[1])
        return -1;
    return 0;
}

int usb_file_close(Ode *ode)
{
    unsigned char packet[7] = {0xe5, 0, 0, 0, 0, 0, 0}, reply[32];
    return exchange(ode, packet, reply, 2);
}

int usb_file_read(Ode *ode, OdeReadBuffer reader, unsigned char *out, size_t size, int header)
{
    unsigned char data[2048], packet[7], reply[32];
    static const unsigned char zero[4] = {0, 0, 0, 0};
    while (size) {
        size_t n = size > sizeof(data) ? sizeof(data) : size;
        unsigned int i;
        /* EOF is misreported by FCare. Clear the complete fixed header before
         * reading it; payloads are accepted only after checking their CRC32.
         * Clear the first word on later blocks to avoid accepting a stale EOF
         * buffer verbatim. No E2 seek or E4 file-write commands are used. */
        for (i = 0; i < (header == USB_FILE_CRC_CHECKED ? 0U : (header ? 6U : 1U)); ++i)
            if (buffer_word(ode, i, zero) < 0)
                return -1;
        memset(packet, 0, sizeof(packet));
        packet[0] = 0xe3;
        packet[1] = (unsigned char)(n >> 8);
        packet[2] = (unsigned char)n;
        if (exchange(ode, packet, reply, 4) < 0 || reply[1] != packet[1] || reply[2] != packet[2])
            return -1;
        if (reader(ode->context, data, sizeof(data)) < 0) {
            ode->transport_uncertain = 1;
            ode->navigation_failed = 1;
            return -1;
        }
        memcpy(out, data, n);
        if (ode->progress)
            ode->progress(ode->progress_context, n);
        out += n;
        size -= n;
        if (header != USB_FILE_CRC_CHECKED)
            header = 0;
    }
    return 0;
}

int usb_file_read_block(Ode *ode, OdeReadBuffer reader, unsigned char *out)
{
    unsigned char packet[7], reply[32];
    memset(packet, 0, sizeof(packet));
    packet[0] = 0xe3;
    packet[1] = 2048 >> 8;
    if (exchange(ode, packet, reply, 4) < 0 || reply[1] != packet[1] || reply[2] != packet[2])
        return -1;
    if (reader(ode->context, out, 2048) < 0) {
        ode->transport_uncertain = 1;
        ode->navigation_failed = 1;
        return -1;
    }
    return 0;
}

int usb_file_probe(Ode *ode, OdeReadBuffer reader, unsigned char *out, size_t size,
                   unsigned char fill)
{
    unsigned char word[4];
    size_t i;
    if (!size || size > 24)
        return -1;
    memset(word, fill, sizeof(word));
    for (i = 0; i < (size + 3) / 4; ++i)
        if (buffer_word(ode, (unsigned int)i, word) < 0)
            return -1;
    return usb_file_read(ode, reader, out, size, USB_FILE_CRC_CHECKED);
}

void usb_art_index_free(UsbArtIndex *index)
{
    free(index->entries);
    free(index->storage);
    memset(index, 0, sizeof(*index));
}

int usb_media_filename(const char *name)
{
    size_t i, n = strlen(name);
    if (n < 5 || n > 96 || name[0] == '.' ||
        (menu_path_compare(name + n - 4, ".3dc") && menu_path_compare(name + n - 4, ".png") &&
         menu_path_compare(name + n - 4, ".3dv")))
        return 0;
    for (i = 0; i < n; ++i)
        if ((unsigned char)name[i] < 32 || (unsigned char)name[i] == 127 || name[i] == '/' ||
            name[i] == '\\' || name[i] == ':' || name[i] == '|')
            return 0;
    return 1;
}

const char *usb_media_find(const UsbArtIndex *index, const char *path)
{
    const char *file = menu_cover_find(index->entries, index->count, path);
    return file && usb_media_filename(file) ? file : NULL;
}

int usb_media_index_load(Ode *ode, OdeReadBuffer reader, UsbArtIndex *index, const char *folder)
{
    unsigned char header[24];
    char path[32];
    UsbArtIndex next;
    size_t bytes, count, pos = 0, i;
    int result = -1;
    memset(&next, 0, sizeof(next));
    usb_art_index_free(index);
    if (strcmp(folder, "covers") && strcmp(folder, "previews"))
        return -1;
    strcpy(path, folder);
    strcat(path, "/index.3di");
    if (usb_file_open(ode, path) < 0)
        return -1;
    if (usb_file_read(ode, reader, header, sizeof(header), 1) < 0 ||
        memcmp(header, "OM3DIDX1", 8) || word(header + 20) > 1)
        goto done;
    next.beside = (int)word(header + 20);
    count = (size_t)word(header + 8);
    bytes = (size_t)word(header + 12);
    if (!count && !bytes && !word(header + 16)) {
        result = 0;
        goto done;
    }
    if (!count || count > 2048 || bytes < count * 8 || bytes > 128UL * 1024UL)
        goto done;
    next.storage = (unsigned char *)malloc(bytes);
    next.entries = (MenuCover *)malloc(count * sizeof(MenuCover));
    if (!next.storage || !next.entries)
        goto done;
    if (usb_file_read(ode, reader, next.storage, bytes, 0) < 0 ||
        usb_art_crc32(next.storage, bytes) != word(header + 16))
        goto done;
    for (i = 0; i < count; ++i) {
        size_t key, file;
        if (bytes - pos < 4)
            goto done;
        key = (size_t)next.storage[pos] * 256 + next.storage[pos + 1];
        file = (size_t)next.storage[pos + 2] * 256 + next.storage[pos + 3];
        pos += 4;
        /* Only the shape here: each string ends where its length says. Checking
         * every entry's content took most of a second on the console; a file
         * name is checked when used (usb_media_find), and the paths are only
         * ever compared. An entry out of order is just not found. */
        if (key < 2 || (file < 6 || file > 97) || key + file > bytes - pos)
            goto done;
        if (next.storage[pos + key - 1] || next.storage[pos + key + file - 1])
            goto done;
        next.entries[i].path = (char *)next.storage + pos;
        next.entries[i].file = (char *)next.storage + pos + key;
        pos += key + file;
    }
    if (pos != bytes)
        goto done;
    next.count = count;
    result = 0;
done:
    if (usb_file_close(ode) < 0)
        result = -1;
    if (!result) {
        *index = next;
        memset(&next, 0, sizeof(next));
    }
    usb_art_index_free(&next);
    return result;
}

int usb_art_index_load(Ode *ode, OdeReadBuffer reader, UsbArtIndex *index)
{
    return usb_media_index_load(ode, reader, index, "covers");
}

/* FCare writes from FILE_BUFFER; use E6 words, never unsupported E0/E2.
 * The caller must close, reopen and verify: E4 does not prove a full write. */
int usb_file_write(Ode *ode, const unsigned char *data, size_t size)
{
    unsigned char packet[7], reply[32], part[4];
    if (strcmp(ode->identity, "LOCODE"))
        return -1;
    while (size) {
        size_t n = size > 2048 ? 2048 : size, i;
        for (i = 0; i < n; i += 4) {
            size_t take = n - i;
            if (take > 4)
                take = 4;
            memset(part, 0, sizeof(part));
            memcpy(part, data + i, take);
            if (buffer_word(ode, (unsigned int)(i / 4), part) < 0)
                return -1;
        }
        memset(packet, 0, sizeof(packet));
        packet[0] = 0xe4;
        packet[1] = (unsigned char)(n >> 8);
        packet[2] = (unsigned char)n;
        if (exchange(ode, packet, reply, 4) < 0 || reply[1] != packet[1] || reply[2] != packet[2])
            return -1;
        data += n;
        size -= n;
        if (ode->progress)
            ode->progress(ode->progress_context, n);
    }
    return 0;
}

/* Poison every requested byte with the complement of its expected value.
 * A lying short read cannot then pass byte-for-byte backup verification. */
int usb_file_verify(Ode *ode, OdeReadBuffer reader, const unsigned char *expected, size_t size)
{
    unsigned char data[2048], part[4], packet[7], reply[32];
    while (size) {
        size_t n = size > sizeof(data) ? sizeof(data) : size, i;
        for (i = 0; i < n; i += 4) {
            size_t j;
            for (j = 0; j < 4; ++j)
                part[j] = i + j < n ? (unsigned char)(expected[i + j] ^ 255) : 0;
            if (buffer_word(ode, (unsigned int)(i / 4), part) < 0)
                return -1;
        }
        memset(packet, 0, sizeof(packet));
        packet[0] = 0xe3;
        packet[1] = (unsigned char)(n >> 8);
        packet[2] = (unsigned char)n;
        if (exchange(ode, packet, reply, 4) < 0 || reply[1] != packet[1] || reply[2] != packet[2])
            return -1;
        if (reader(ode->context, data, sizeof(data)) < 0) {
            ode->navigation_failed = 1;
            return -1;
        }
        if (memcmp(data, expected, n))
            return -1;
        if (ode->progress)
            ode->progress(ode->progress_context, n);
        expected += n;
        size -= n;
    }
    return 0;
}

/* Text files have no checksum/length header. Clear every byte before each
 * read, so FCare's lying EOF count cannot expose stale text as favorites. */
/* Fill the ODE's buffer with the next n bytes of the open file. */
static int file_fill(Ode *ode, size_t n)
{
    unsigned char packet[7], reply[32];
    memset(packet, 0, sizeof(packet));
    packet[0] = 0xe3;
    packet[1] = (unsigned char)(n >> 8);
    packet[2] = (unsigned char)n;
    return exchange(ode, packet, reply, 4) < 0 || reply[1] != packet[1] || reply[2] != packet[2]
               ? -1
               : 0;
}

/* Text up to its end, in pieces read over a cleared buffer, until *used
 * reaches limit. 1 at the end of the text, 0 at the limit, -1 on failure. */
static int text_pieces(Ode *ode, OdeReadBuffer reader, unsigned char *block, char *out,
                       size_t capacity, size_t limit, size_t *used)
{
    static const unsigned char zero[4] = {0, 0, 0, 0};
    while (*used < capacity && *used < limit) {
        size_t i, n = capacity - *used;
        /* Short text is common at launch. Clear/read 128 bytes at a time
         * instead of issuing 512 FIFO-word commands before its first byte. */
        if (n > 128)
            n = 128;
        for (i = 0; i < (n + 3) / 4; ++i)
            if (buffer_word(ode, (unsigned int)i, zero) < 0)
                return -1;
        if (file_fill(ode, n) < 0)
            return -1;
        if (reader(ode->context, block, 2048) < 0) {
            ode->navigation_failed = 1;
            return -1;
        }
        for (i = 0; i < n && block[i]; ++i) {
        }
        memcpy(out + *used, block, i);
        *used += i;
        if (i < n)
            return 1;
    }
    return 0;
}

/* FCare has no command to clear its buffer, and clearing it word by word
 * costs a command per 4 bytes. Reading a file of zeros clears it with one;
 * the menu makes .ohmy3do/blank.bin on first use and checks the buffer
 * every time, so a damaged file only means the slower way. 1 when clear. */
static int blank_unwritable;
static int zero_buffer(Ode *ode, OdeReadBuffer reader, unsigned char *block)
{
    size_t i;
    int attempt, clear = 0;
    for (attempt = 0; attempt < 2 && !clear; ++attempt) {
        if (attempt && blank_unwritable)
            break;
        if (usb_file_open(ode, "blank.bin") < 0)
            break;
        if (attempt) {
            memset(block, 0, 2048);
            if (usb_file_write(ode, block, 2048) < 0) {
                blank_unwritable = 1;
                usb_file_close(ode);
                break;
            }
            if (usb_file_close(ode) < 0)
                break;
            if (usb_file_open(ode, "blank.bin") < 0)
                break;
        }
        if (file_fill(ode, 2048) < 0) {
            usb_file_close(ode);
            break;
        }
        if (reader(ode->context, block, 2048) < 0) {
            ode->navigation_failed = 1;
            usb_file_close(ode);
            break;
        }
        for (i = 0; i < 2048 && !block[i]; ++i) {
        }
        clear = i == 2048;
        if (usb_file_close(ode) < 0)
            return 0;
    }
    return clear;
}

/* FCare reports a full read at the end of a file and leaves the rest of its
 * buffer stale, so text ends where a cleared buffer stays zero. Over a buffer
 * zeroed with blank.bin, one read finds the end of text under 2 KiB, once the
 * path the open left in its first words is cleared. Otherwise the first piece
 * is read over words cleared one by one. Past that, read whole blocks; a
 * block is complete once the next read brings new bytes (an end leaves the
 * buffer as it was). Then reopen, pass over the complete blocks with fills
 * alone, and read the last one over a buffer cleared from where it last
 * differed from the block before it: bytes up to there are certainly the
 * file's. */
int usb_file_text(Ode *ode, OdeReadBuffer reader, const char *name, char *out, size_t capacity,
                  size_t *length)
{
    static const unsigned char zero[4] = {0, 0, 0, 0};
    unsigned char block[2048];
    size_t used = 0, skip, from = 0, i;
    int result = -1, status = 0, pending = 0, known = 0;
    /* Not beside a game: blank.bin belongs in the volume's own .ohmy3do. */
    int clear = capacity && !ode->media_beside && zero_buffer(ode, reader, block);
    const char *last = NULL; /* the block the text may end in */
    if (!capacity || usb_file_open(ode, name) < 0)
        return -1;
    if (clear) {
        for (i = 0; i < (strlen(ode->file_path) + 4) / 4; ++i)
            if (buffer_word(ode, (unsigned int)i, zero) < 0)
                goto done;
        if (file_fill(ode, sizeof(block)) < 0)
            goto done;
        if (reader(ode->context, block, sizeof(block)) < 0) {
            ode->navigation_failed = 1;
            goto done;
        }
        for (i = 0; i < sizeof(block) && block[i]; ++i) {
        }
        if (i >= capacity)
            goto done;
        memcpy(out, block, i);
        used = i;
        if (i < sizeof(block))
            status = 1;
        else
            known = 1; /* a whole block of text: the next read's background */
    } else
        status = text_pieces(ode, reader, block, out, capacity, 128, &used);
    while (!status && (pending || used + sizeof(block) < capacity)) {
        if (usb_file_read_block(ode, reader, block) < 0)
            goto done;
        if (pending) {
            const char *end = (const char *)memchr(out + used, 0, sizeof(block));
            if (!memcmp(block, out + used, sizeof(block))) {
                last = out + used;
                break;
            }
            if (end) {
                used = (size_t)(end - out);
                status = 1;
                break;
            }
            used += sizeof(block);
            known = 1;
            pending = 0;
        }
        /* Without room to keep it, compare this block where it was read. */
        if (used + sizeof(block) >= capacity) {
            last = (const char *)block;
            break;
        }
        memcpy(out + used, block, sizeof(block));
        pending = 1;
    }
    if (!status && last) {
        if (known) {
            for (i = sizeof(block); i && last[i - 1] == out[used - sizeof(block) + i - 1]; --i) {
            }
            from = i & ~(size_t)3;
        }
        if (usb_file_close(ode) < 0 || usb_file_open(ode, name) < 0)
            return -1;
        for (skip = used; skip; skip -= skip > sizeof(block) ? sizeof(block) : skip)
            if (file_fill(ode, skip > sizeof(block) ? sizeof(block) : skip) < 0)
                goto done;
        /* Past halfway, clearing the tail costs less than pieces from the start. */
        if (from >= sizeof(block) / 2) {
            for (i = from / 4; i < sizeof(block) / 4; ++i)
                if (buffer_word(ode, (unsigned int)i, zero) < 0)
                    goto done;
            if (file_fill(ode, sizeof(block)) < 0)
                goto done;
            if (reader(ode->context, block, sizeof(block)) < 0) {
                ode->navigation_failed = 1;
                goto done;
            }
            for (i = 0; i < sizeof(block) && block[i]; ++i) {
            }
            if (i >= capacity - used)
                goto done;
            memcpy(out + used, block, i);
            used += i;
            if (i < sizeof(block))
                status = 1;
        }
    }
    if (!status)
        status = text_pieces(ode, reader, block, out, capacity, capacity, &used);
    if (status == 1) {
        out[used] = 0;
        *length = used;
        result = 0;
    }
done:
    if (usb_file_close(ode) < 0)
        result = -1;
    return result;
}

int usb_file_text_write(Ode *ode, OdeReadBuffer reader, const char *name, const char *text)
{
    size_t size = strlen(text) + 1;
    int result;
    if (usb_file_open(ode, name) < 0)
        return -1;
    result = usb_file_write(ode, (const unsigned char *)text, size);
    if (usb_file_close(ode) < 0)
        result = -1;
    if (result < 0 || usb_file_open(ode, name) < 0)
        return -1;
    result = usb_file_verify(ode, reader, (const unsigned char *)text, size);
    if (usb_file_close(ode) < 0)
        result = -1;
    return result;
}
