#include "save_archive.h"
#include "usb_art.h"
#include "menu_ui.h"
#include <stdlib.h>
#include <string.h>
unsigned long save_word(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8) | p[3];
}
void save_put_word(unsigned char *p, unsigned long value)
{
    p[0] = (unsigned char)(value >> 24); p[1] = (unsigned char)(value >> 16);
    p[2] = (unsigned char)(value >> 8); p[3] = (unsigned char)value;
}
void save_archive_free(SaveArchive *a)
{
    free(a->bytes); memset(a, 0, sizeof(*a));
}
int save_name_valid(const char *name)
{
    size_t i;
    for (i = 0; i < SAVE_NAME_BYTES; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (!c) return i && strcmp(name, ".") && strcmp(name, "..");
        if (c < 32 || c == 127 || c == '/' || c == '\\' || c == ':') return 0;
    }
    return 0;
}
int save_archive_parse(SaveArchive *a, const unsigned char *data, size_t size)
{
    SaveArchive next;
    size_t count, pos = SAVE_HEADER, i, j;
    memset(&next, 0, sizeof(next));
    if (size < SAVE_HEADER || size > SAVE_MAX_BYTES || memcmp(data, "OM3DSAV1", 8) ||
        save_word(data + 8) != size || save_word(data + 20)) return -1;
    count = (size_t)save_word(data + 12);
    if (count > SAVE_MAX_FILES || usb_art_crc32(data + SAVE_HEADER, size - SAVE_HEADER) != save_word(data + 16)) return -1;
    next.bytes = (unsigned char *)malloc(size);
    if (!next.bytes) return -1;
    memcpy(next.bytes, data, size); next.length = size; next.count = count;
    for (i = 0; i < count; ++i) {
        const unsigned char *record;
        SaveEntry *e = &next.entries[i];
        if (size - pos < SAVE_RECORD) goto bad;
        record = next.bytes + pos;
        memcpy(e->name, record, SAVE_NAME_BYTES);
        if (!save_name_valid(e->name) || save_word(record + 44)) goto bad;
        /* Canonical padding prevents hidden bytes after a filename. */
        for (j = strlen(e->name) + 1; j < SAVE_NAME_BYTES; ++j) if (record[j]) goto bad;
        for (j = 0; j < i; ++j)
            if (!menu_path_compare(next.entries[j].name, e->name)) goto bad;
        e->type = save_word(record + 32); e->size = (size_t)save_word(record + 36);
        pos += SAVE_RECORD;
        if (e->size > size - pos) goto bad;
        e->data = next.bytes + pos;
        if (usb_art_crc32(e->data, e->size) != save_word(record + 40)) goto bad;
        pos += e->size;
    }
    if (pos != size) goto bad;
    save_archive_free(a); *a = next; return 0;
bad:
    save_archive_free(&next); return -1;
}
int save_archive_build(SaveArchive *a, const SaveEntry *entries, size_t count)
{
    unsigned char *data;
    size_t total = SAVE_HEADER, i, pos = SAVE_HEADER;
    int result;
    if (count > SAVE_MAX_FILES) return -1;
    for (i = 0; i < count; ++i) {
        if (!save_name_valid(entries[i].name) || entries[i].size > SAVE_MAX_BYTES - total - SAVE_RECORD ||
            (entries[i].size && !entries[i].data)) return -1;
        total += SAVE_RECORD + entries[i].size;
        if (i + 1 < count && total > SAVE_MAX_BYTES - SAVE_RECORD) return -1;
    }
    data = (unsigned char *)calloc(total, 1);
    if (!data) return -1;
    memcpy(data, "OM3DSAV1", 8); save_put_word(data + 8, total); save_put_word(data + 12, count);
    for (i = 0; i < count; ++i) {
        memcpy(data + pos, entries[i].name, strlen(entries[i].name));
        save_put_word(data + pos + 32, entries[i].type);
        save_put_word(data + pos + 36, entries[i].size);
        save_put_word(data + pos + 40, usb_art_crc32(entries[i].data, entries[i].size));
        pos += SAVE_RECORD;
        if (entries[i].size) memcpy(data + pos, entries[i].data, entries[i].size);
        pos += entries[i].size;
    }
    save_put_word(data + 16, usb_art_crc32(data + SAVE_HEADER, total - SAVE_HEADER));
    result = save_archive_parse(a, data, total); free(data); return result;
}

static int same_save(const SaveEntry *a, const SaveEntry *b)
{
    return !menu_path_compare(a->name, b->name) && a->type == b->type && a->size == b->size &&
           (!a->size || !memcmp(a->data, b->data, a->size));
}
int save_restore_matches(const SaveArchive *before, const SaveArchive *after, const SaveEntry *target)
{
    size_t i, j;
    int replaced = 0, found = 0;
    for (i = 0; i < before->count; ++i) {
        const SaveEntry *old = &before->entries[i];
        if (!menu_path_compare(old->name, target->name)) { replaced = 1; continue; }
        for (j = 0; j < after->count; ++j)
            if (!strcmp(old->name, after->entries[j].name)) break;
        if (j == after->count || !same_save(old, &after->entries[j])) return 0;
    }
    for (j = 0; j < after->count; ++j)
        if (same_save(target, &after->entries[j])) found = 1;
    return found && after->count == before->count + (replaced ? 0 : 1);
}

int save_archive_merge(SaveArchive *out, const SaveArchive *destination, const SaveEntry *entry)
{
    SaveEntry *entries;
    size_t i, count=destination->count, match=count;
    int result;
    if (count>SAVE_MAX_FILES || !save_name_valid(entry->name)) return -1;
    for (i=0;i<count;++i) if (!menu_path_compare(destination->entries[i].name,entry->name)) match=i;
    if (match==count && count==SAVE_MAX_FILES) return -1;
    entries=(SaveEntry *)malloc((count+1)*sizeof(*entries));
    if (!entries) return -1;
    for (i=0;i<count;++i) entries[i]=destination->entries[i];
    entries[match]=*entry;
    if (match<count) strcpy(entries[match].name,destination->entries[match].name);
    else ++count;
    result=save_archive_build(out,entries,count);
    free(entries); return result;
}
