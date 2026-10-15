#include "usb_art.h"
#include "usb_file.h"
#include <stdlib.h>
#include <string.h>

static unsigned long word(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8) | p[3];
}

unsigned long usb_art_crc32(const unsigned char *data, size_t size)
{
    static const unsigned long table[16] = {
        0x00000000UL, 0x1db71064UL, 0x3b6e20c8UL, 0x26d930acUL,
        0x76dc4190UL, 0x6b6b51f4UL, 0x4db26158UL, 0x5005713cUL,
        0xedb88320UL, 0xf00f9344UL, 0xd6d6a3e8UL, 0xcb61b38cUL,
        0x9b64c2b0UL, 0x86d3d2d4UL, 0xa00ae278UL, 0xbdbdf21cUL
    };
    unsigned long crc=0xffffffffUL;
    size_t i;
    for (i=0;i<size;++i) {
        crc ^= data[i];
        crc=(crc>>4)^table[crc&15]; crc=(crc>>4)^table[crc&15];
    }
    return crc^0xffffffffUL;
}

static int exchange(Ode *ode, unsigned char *packet, unsigned char *reply, size_t expected)
{
    size_t length = 0;
    if (ode->navigation_failed) return -1;
    if (ode->transfer(ode->context, packet, reply, 32, &length) < 0 ||
        length != expected || reply[0] != packet[0]) {
        ode->navigation_failed = 1;
        return -1;
    }
    if (reply[length - 1] & 0x10) return -1;
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
    size_t length = strlen(name) + 1, i;
    if (strcmp(ode->identity, "LOCODE") || length > 128) return -1;
    for (i = 0; i < length; i += 4) {
        size_t n = length - i;
        if (n > 4) n = 4;
        memset(part, 0, sizeof(part));
        memcpy(part, name + i, n);
        if (buffer_word(ode, (unsigned int)(i / 4), part) < 0) return -1;
    }
    memset(packet, 0, sizeof(packet));
    packet[0] = 0xe1;
    packet[2] = (unsigned char)(length >> 8);
    packet[3] = (unsigned char)length;
    if (exchange(ode, packet, reply, 3) < 0 || !reply[1]) return -1;
    return 0;
}

int usb_file_close(Ode *ode)
{
    unsigned char packet[7] = {0xe5, 0, 0, 0, 0, 0, 0}, reply[32];
    return exchange(ode, packet, reply, 2);
}

int usb_file_read(Ode *ode, OdeReadBuffer reader, unsigned char *out, size_t size,
                     int header)
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
        for (i = 0; i < (header ? 6U : 1U); ++i)
            if (buffer_word(ode, i, zero) < 0) return -1;
        memset(packet, 0, sizeof(packet));
        packet[0] = 0xe3;
        packet[1] = (unsigned char)(n >> 8);
        packet[2] = (unsigned char)n;
        if (exchange(ode, packet, reply, 4) < 0 ||
            reply[1] != packet[1] || reply[2] != packet[2]) return -1;
        if (reader(ode->context, data, sizeof(data)) < 0) {
            ode->navigation_failed = 1;
            return -1;
        }
        memcpy(out, data, n);
        if (ode->progress) ode->progress(ode->progress_context, n);
        out += n;
        size -= n;
        header = 0;
    }
    return 0;
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
        (menu_path_compare(name+n-4,".3dc") && menu_path_compare(name+n-4,".png") && menu_path_compare(name+n-4,".3dv"))) return 0;
    for (i=0;i<n;++i) if ((unsigned char)name[i] < 32 || (unsigned char)name[i] == 127 ||
        name[i] == '/' || name[i] == '\\' || name[i] == ':' || name[i] == '|') return 0;
    return 1;
}

static int safe_path(const char *name)
{
    const char *part = name, *p;
    if (!*name || *name == '/' || strlen(name) > 8192) return 0;
    for (p = name;; ++p) {
        if (*p && ((unsigned char)*p < 32 || *p == '\\' || *p == ':' || *p == 127)) return 0;
        if (!*p || *p == '/') {
            size_t n = (size_t)(p - part);
            if (!n || (n == 1 && part[0] == '.') || (n == 2 && !memcmp(part, "..", 2))) return 0;
            if (!*p) break;
            part = p + 1;
        }
    }
    return 1;
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
    if (strcmp(folder,"covers") && strcmp(folder,"previews")) return -1;
    strcpy(path,folder); strcat(path,"/index.3di");
    if (usb_file_open(ode,path) < 0) return -1;
    if (usb_file_read(ode, reader, header, sizeof(header), 1) < 0 ||
        memcmp(header, "OM3DIDX1", 8) || word(header + 20)) goto done;
    count = (size_t)word(header + 8);
    bytes = (size_t)word(header + 12);
    if (!count && !bytes && !word(header + 16)) { result = 0; goto done; }
    if (!count || count > 2048 || bytes < count * 8 || bytes > 128UL * 1024UL) goto done;
    next.storage = (unsigned char *)malloc(bytes);
    next.entries = (MenuCover *)malloc(count * sizeof(MenuCover));
    if (!next.storage || !next.entries) goto done;
    if (usb_file_read(ode, reader, next.storage, bytes, 0) < 0 ||
        usb_art_crc32(next.storage, bytes) != word(header + 16)) goto done;
    for (i = 0; i < count; ++i) {
        size_t key, file;
        if (bytes - pos < 4) goto done;
        key = (size_t)next.storage[pos] * 256 + next.storage[pos + 1];
        file = (size_t)next.storage[pos + 2] * 256 + next.storage[pos + 3];
        pos += 4;
        if (key < 2 || (file < 6 || file > 97) || key + file > bytes - pos) goto done;
        if (next.storage[pos + key - 1] || next.storage[pos + key + file - 1] ||
            memchr(next.storage + pos, 0, key - 1) ||
            memchr(next.storage + pos + key, 0, file - 1)) goto done;
        next.entries[i].path = (char *)next.storage + pos;
        next.entries[i].file = (char *)next.storage + pos + key;
        if (!safe_path(next.entries[i].path) || !usb_media_filename(next.entries[i].file) ||
            (i && menu_path_compare(next.entries[i - 1].path, next.entries[i].path) >= 0)) goto done;
        pos += key + file;
    }
    if (pos != bytes) goto done;
    next.count = count;
    result = 0;
done:
    if (usb_file_close(ode) < 0) result = -1;
    if (!result) { *index = next; memset(&next, 0, sizeof(next)); }
    usb_art_index_free(&next);
    return result;
}

int usb_art_index_load(Ode *ode, OdeReadBuffer reader, UsbArtIndex *index)
{
    return usb_media_index_load(ode,reader,index,"covers");
}

int usb_art_pixels(Ode *ode, OdeReadBuffer reader, const char *filename,
                   unsigned char *pixels)
{
    unsigned char header[24];
    char name[128];
    int result = -1;
    if (!usb_media_filename(filename) || menu_path_compare(filename+strlen(filename)-4,".3dc")) return -1;
    strcpy(name, "covers/");
    strcat(name, filename);
    if (usb_file_open(ode, name) < 0) return -1;
    if (usb_file_read(ode, reader, header, sizeof(header), 1) < 0 ||
        memcmp(header, "OM3DCOV1", 8) || header[8] || header[9] != 64 ||
        header[10] || header[11] != 80 || word(header + 12) != USB_ART_PIXELS ||
        word(header + 20)) goto done;
    if (usb_file_read(ode, reader, pixels, USB_ART_PIXELS, 0) < 0 ||
        usb_art_crc32(pixels, USB_ART_PIXELS) != word(header + 16)) goto done;
    result = 0;
done:
    if (usb_file_close(ode) < 0) result = -1;
    return result;
}

/* FCare writes from FILE_BUFFER; use E6 words, never unsupported E0/E2.
 * The caller must close, reopen and verify: E4 does not prove a full write. */
int usb_file_write(Ode *ode, const unsigned char *data, size_t size)
{
    unsigned char packet[7], reply[32], part[4];
    if (strcmp(ode->identity, "LOCODE")) return -1;
    while (size) {
        size_t n = size > 2048 ? 2048 : size, i;
        for (i = 0; i < n; i += 4) {
            size_t take = n - i;
            if (take > 4) take = 4;
            memset(part, 0, sizeof(part)); memcpy(part, data + i, take);
            if (buffer_word(ode, (unsigned int)(i / 4), part) < 0) return -1;
        }
        memset(packet, 0, sizeof(packet)); packet[0] = 0xe4;
        packet[1] = (unsigned char)(n >> 8); packet[2] = (unsigned char)n;
        if (exchange(ode, packet, reply, 4) < 0 || reply[1] != packet[1] || reply[2] != packet[2]) return -1;
        data += n; size -= n;
        if (ode->progress) ode->progress(ode->progress_context, n);
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
            for (j = 0; j < 4; ++j) part[j] = i + j < n ? (unsigned char)(expected[i + j] ^ 255) : 0;
            if (buffer_word(ode, (unsigned int)(i / 4), part) < 0) return -1;
        }
        memset(packet, 0, sizeof(packet)); packet[0] = 0xe3;
        packet[1] = (unsigned char)(n >> 8); packet[2] = (unsigned char)n;
        if (exchange(ode, packet, reply, 4) < 0 || reply[1] != packet[1] || reply[2] != packet[2]) return -1;
        if (reader(ode->context, data, sizeof(data)) < 0) { ode->navigation_failed = 1; return -1; }
        if (memcmp(data, expected, n)) return -1;
        if (ode->progress) ode->progress(ode->progress_context, n);
        expected += n; size -= n;
    }
    return 0;
}

/* Text files have no checksum/length header. Clear every byte before each
 * read, so FCare's lying EOF count cannot expose stale text as favorites. */
int usb_file_text(Ode *ode, OdeReadBuffer reader, const char *name, char *out, size_t capacity, size_t *length)
{
    static const unsigned char zero[4] = {0,0,0,0};
    unsigned char block[2048], packet[7], reply[32];
    size_t used = 0;
    int result = -1;
    if (!capacity || usb_file_open(ode,name) < 0) return -1;
    while (used < capacity) {
        size_t i, n = capacity - used;
        /* Short text is common at launch. Clear/read 128 bytes at a time
         * instead of issuing 512 FIFO-word commands before its first byte. */
        if (n > 128) n = 128;
        for (i = 0; i < (n+3)/4; ++i) if (buffer_word(ode,(unsigned int)i,zero) < 0) goto done;
        memset(packet,0,sizeof(packet)); packet[0] = 0xe3; packet[1] = (unsigned char)(n >> 8); packet[2] = (unsigned char)n;
        if (exchange(ode,packet,reply,4) < 0 || reply[1] != packet[1] || reply[2] != packet[2]) goto done;
        if (reader(ode->context,block,sizeof(block)) < 0) { ode->navigation_failed = 1; goto done; }
        for (i = 0; i < n && block[i]; ++i) {}
        memcpy(out+used,block,i); used += i;
        if (i < n) { out[used] = 0; *length = used; result = 0; goto done; }
    }
done:
    if (usb_file_close(ode) < 0) result = -1;
    return result;
}

int usb_file_text_write(Ode *ode, OdeReadBuffer reader, const char *name, const char *text)
{
    size_t size = strlen(text) + 1;
    int result;
    if (usb_file_open(ode,name) < 0) return -1;
    result = usb_file_write(ode,(const unsigned char *)text,size);
    if (usb_file_close(ode) < 0) result = -1;
    if (result < 0 || usb_file_open(ode,name) < 0) return -1;
    result = usb_file_verify(ode,reader,(const unsigned char *)text,size);
    if (usb_file_close(ode) < 0) result = -1;
    return result;
}
