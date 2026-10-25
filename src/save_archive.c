/*
 * Reading, building and comparing save archives (.3ds files).
 * See save_archive.h for the file layout.
 */
#include "save_archive.h"
#include "usb_art.h"
#include "menu_ui.h"
#include <stdlib.h>
#include <string.h>

/* Header fields, by offset. */
#define HEADER_TOTAL_SIZE 8
#define HEADER_FILE_COUNT 12
#define HEADER_CRC        16
#define HEADER_RESERVED   20

/* Record fields, by offset (the name is at 0). */
#define RECORD_TYPE     32
#define RECORD_SIZE     36
#define RECORD_CRC      40
#define RECORD_RESERVED 44

unsigned long save_word(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) |
           p[3];
}

void save_put_word(unsigned char *p, unsigned long value)
{
    p[0] = (unsigned char)(value >> 24);
    p[1] = (unsigned char)(value >> 16);
    p[2] = (unsigned char)(value >> 8);
    p[3] = (unsigned char)value;
}

void save_archive_free(SaveArchive *archive)
{
    free(archive->bytes);
    memset(archive, 0, sizeof(*archive));
}

int save_name_valid(const char *name)
{
    size_t i;
    for (i = 0; i < SAVE_NAME_BYTES; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (c == 0)
            return i > 0 && strcmp(name, ".") && strcmp(name, "..");
        if (c < 32 || c == 127 || c == '/' || c == '\\' || c == ':')
            return 0;
    }
    return 0; /* no terminating zero within SAVE_NAME_BYTES: too long */
}

/*
 * Reads the record at `record` into `entry` and checks its name: valid, the
 * rest of the name field zero (so no hidden bytes ride along after it), the
 * reserved field zero. Returns 0, or -1 if invalid.
 */
static int read_record_name(const unsigned char *record, SaveEntry *entry)
{
    size_t i;

    memcpy(entry->name, record, SAVE_NAME_BYTES);
    if (!save_name_valid(entry->name) || save_word(record + RECORD_RESERVED))
        return -1;
    for (i = strlen(entry->name) + 1; i < SAVE_NAME_BYTES; ++i) {
        if (record[i] != 0)
            return -1;
    }
    entry->type = save_word(record + RECORD_TYPE);
    entry->size = (size_t)save_word(record + RECORD_SIZE);
    return 0;
}

int save_archive_parse(SaveArchive *archive, const unsigned char *data, size_t size)
{
    SaveArchive loaded;
    size_t count, position = SAVE_HEADER, i, j;

    memset(&loaded, 0, sizeof(loaded));

    /* The header: magic, size matching the data, reserved zero, file count
     * in range, and the checksum of everything after it. */
    if (size < SAVE_HEADER || size > SAVE_MAX_BYTES || memcmp(data, "OM3DSAV1", 8) ||
        save_word(data + HEADER_TOTAL_SIZE) != size || save_word(data + HEADER_RESERVED))
        return -1;
    count = (size_t)save_word(data + HEADER_FILE_COUNT);
    if (count > SAVE_MAX_FILES ||
        usb_art_crc32(data + SAVE_HEADER, size - SAVE_HEADER) != save_word(data + HEADER_CRC))
        return -1;

    /* Keep a copy; the entries will point into it. */
    loaded.bytes = (unsigned char *)malloc(size);
    if (!loaded.bytes)
        return -1;
    memcpy(loaded.bytes, data, size);
    loaded.length = size;
    loaded.count = count;

    for (i = 0; i < count; ++i) {
        SaveEntry *entry = &loaded.entries[i];
        const unsigned char *record;

        if (size - position < SAVE_RECORD)
            goto invalid;
        record = loaded.bytes + position;
        if (read_record_name(record, entry) < 0)
            goto invalid;
        /* No two files may share a name, ignoring case (as NVRAM does). */
        for (j = 0; j < i; ++j) {
            if (!menu_path_compare(loaded.entries[j].name, entry->name))
                goto invalid;
        }

        /* The contents follow the record, and must match their checksum. */
        position += SAVE_RECORD;
        if (entry->size > size - position)
            goto invalid;
        entry->data = loaded.bytes + position;
        if (usb_art_crc32(entry->data, entry->size) != save_word(record + RECORD_CRC))
            goto invalid;
        position += entry->size;
    }
    /* The files must fill the archive exactly. */
    if (position != size)
        goto invalid;

    save_archive_free(archive);
    *archive = loaded;
    return 0;

invalid:
    save_archive_free(&loaded);
    return -1;
}

int save_archive_build(SaveArchive *archive, const SaveEntry *entries, size_t count)
{
    unsigned char *data;
    size_t total = SAVE_HEADER, i, position = SAVE_HEADER;
    int result;

    if (count > SAVE_MAX_FILES)
        return -1;

    /* Check every file and add up the size, stopping before it could pass
     * SAVE_MAX_BYTES (written to avoid overflowing the arithmetic). */
    for (i = 0; i < count; ++i) {
        if (!save_name_valid(entries[i].name) ||
            entries[i].size > SAVE_MAX_BYTES - total - SAVE_RECORD ||
            (entries[i].size && !entries[i].data))
            return -1;
        total += SAVE_RECORD + entries[i].size;
        if (i + 1 < count && total > SAVE_MAX_BYTES - SAVE_RECORD)
            return -1;
    }

    /* calloc zeroes everything, including name padding and reserved fields. */
    data = (unsigned char *)calloc(total, 1);
    if (!data)
        return -1;
    memcpy(data, "OM3DSAV1", 8);
    save_put_word(data + HEADER_TOTAL_SIZE, total);
    save_put_word(data + HEADER_FILE_COUNT, count);
    for (i = 0; i < count; ++i) {
        unsigned char *record = data + position;
        memcpy(record, entries[i].name, strlen(entries[i].name));
        save_put_word(record + RECORD_TYPE, entries[i].type);
        save_put_word(record + RECORD_SIZE, entries[i].size);
        save_put_word(record + RECORD_CRC, usb_art_crc32(entries[i].data, entries[i].size));
        position += SAVE_RECORD;
        if (entries[i].size)
            memcpy(data + position, entries[i].data, entries[i].size);
        position += entries[i].size;
    }
    save_put_word(data + HEADER_CRC, usb_art_crc32(data + SAVE_HEADER, total - SAVE_HEADER));

    /* Read the result back, so it is checked like any archive from the drive. */
    result = save_archive_parse(archive, data, total);
    free(data);
    return result;
}

/* Whether two save files are identical: name (ignoring case), type and contents. */
static int same_save(const SaveEntry *a, const SaveEntry *b)
{
    return !menu_path_compare(a->name, b->name) && a->type == b->type && a->size == b->size &&
           (!a->size || !memcmp(a->data, b->data, a->size));
}

int save_restore_matches(const SaveArchive *before, const SaveArchive *after,
                         const SaveEntry *target)
{
    size_t i, j;
    int replaced = 0, found = 0;

    /* Every file that was there before, other than the target, must still be
     * there and be unchanged. */
    for (i = 0; i < before->count; ++i) {
        const SaveEntry *old = &before->entries[i];
        if (!menu_path_compare(old->name, target->name)) {
            replaced = 1; /* the target replaced an older copy of itself */
            continue;
        }
        for (j = 0; j < after->count; ++j) {
            if (!strcmp(old->name, after->entries[j].name))
                break;
        }
        if (j == after->count || !same_save(old, &after->entries[j]))
            return 0;
    }

    /* The target must now be there, and nothing else may have appeared. */
    for (j = 0; j < after->count; ++j) {
        if (same_save(target, &after->entries[j]))
            found = 1;
    }
    return found && after->count == before->count + (replaced ? 0 : 1);
}

int save_archive_merge(SaveArchive *out, const SaveArchive *destination, const SaveEntry *entry)
{
    SaveEntry *entries;
    size_t i, count = destination->count, match = count;
    int result;

    if (count > SAVE_MAX_FILES || !save_name_valid(entry->name))
        return -1;

    /* Is there already a file of this name? */
    for (i = 0; i < count; ++i) {
        if (!menu_path_compare(destination->entries[i].name, entry->name))
            match = i;
    }
    if (match == count && count == SAVE_MAX_FILES)
        return -1; /* no room for another file */

    /* A working copy of the file list (not the contents), with the new file
     * in its place or at the end. */
    entries = (SaveEntry *)malloc((count + 1) * sizeof(*entries));
    if (!entries)
        return -1;
    for (i = 0; i < count; ++i)
        entries[i] = destination->entries[i];
    entries[match] = *entry;
    if (match < count)
        strcpy(entries[match].name,
               destination->entries[match].name); /* keep the old name's case */
    else
        ++count;

    /* The new archive is built (copying all contents) before `out` changes,
     * which is why `out` may also be `destination`. */
    result = save_archive_build(out, entries, count);
    free(entries);
    return result;
}

/* The index of the file named `name` (ignoring case) in `archive`, or
 * archive->count if there is none. */
static size_t find_file(const SaveArchive *archive, const char *name)
{
    size_t i;
    for (i = 0; i < archive->count; ++i) {
        if (!menu_path_compare(name, archive->entries[i].name))
            break;
    }
    return i;
}

size_t save_archive_diff_count(const SaveArchive *before, const SaveArchive *after)
{
    size_t i, rows = before->count;
    for (i = 0; i < after->count; ++i) {
        if (find_file(before, after->entries[i].name) == before->count)
            ++rows; /* only in `after` */
    }
    return rows;
}

int save_archive_diff(const SaveArchive *before, const SaveArchive *after, size_t index,
                      SaveDifference *out)
{
    const SaveEntry *old_file = NULL, *new_file = NULL;
    size_t i;

    memset(out, 0, sizeof(*out));
    out->before = before->count;
    out->after = after->count;

    if (index < before->count) {
        /* A row for a file of `before`, which may or may not be in `after`. */
        size_t match;
        out->before = index;
        old_file = &before->entries[index];
        match = find_file(after, old_file->name);
        if (match < after->count) {
            out->after = match;
            new_file = &after->entries[match];
        }
    } else {
        /* A row for a file only `after` has: count those until reaching it. */
        size_t row = before->count;
        for (i = 0; i < after->count; ++i) {
            if (find_file(before, after->entries[i].name) < before->count)
                continue;
            if (row == index) {
                out->after = i;
                new_file = &after->entries[i];
                break;
            }
            ++row;
        }
    }
    if (!old_file && !new_file)
        return -1;

    /* Count differing bytes over the longer file; bytes beyond the shorter
     * one count as changed. */
    out->length = old_file ? old_file->size : 0;
    if (new_file && new_file->size > out->length)
        out->length = new_file->size;
    for (i = 0; i < out->length; ++i) {
        if (!old_file || !new_file || i >= old_file->size || i >= new_file->size ||
            old_file->data[i] != new_file->data[i])
            ++out->changed_bytes;
    }

    out->metadata_changed =
        old_file && new_file &&
        (old_file->type != new_file->type || strcmp(old_file->name, new_file->name));
    if (!old_file)
        out->state = SAVE_DIFF_ADDED;
    else if (!new_file)
        out->state = SAVE_DIFF_REMOVED;
    else if (out->changed_bytes || out->metadata_changed)
        out->state = SAVE_DIFF_CHANGED;
    else
        out->state = SAVE_DIFF_SAME;
    return 0;
}
