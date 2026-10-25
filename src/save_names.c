/*
 * Save names: friendly titles for the files games write to the console's
 * NVRAM, so the save manager can show "Road Rash" instead of "RRASH.SAV".
 *
 * Names come from three places, in order of preference:
 *   1. saves-learned.txt, which the menu writes itself (see the snapshot
 *      functions below);
 *   2. save-names.txt, which the PC tools build by scanning the discs;
 *   3. the built-in list in save_name_db.h, generated from known games.
 *
 * Both text files use one name per line:
 *   =FILE<TAB>Title     names the NVRAM file called exactly FILE
 *   *PREFIX<TAB>Title   names every NVRAM file whose name starts with PREFIX
 * Names are compared without regard to ASCII case, like the console's own
 * file system.
 *
 * This file also reads and writes backup slot labels (save_slot_labels_*).
 */
#include "save_names.h"
#include "usb_art.h"
#include <stdlib.h>
#include <string.h>
#include "save_name_db.h"

/* The first line of a save snapshot file, and its length in bytes. */
#define SNAPSHOT_HEADER        "OH3DO_SAVESNAP1\n"
#define SNAPSHOT_HEADER_LENGTH 16

/*
 * Room each NVRAM file needs in a snapshot besides its name: a tab, up to
 * ten size digits, a tab, eight CRC digits and a newline, rounded up.
 */
#define SNAPSHOT_LINE_EXTRA 22

/* Converts an ASCII capital letter to lower case; other bytes are unchanged. */
static int to_lower_ascii(int c)
{
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

/* Whether the first `length` characters of a and b match, ignoring case. */
static int same_ignoring_case(const char *a, const char *b, size_t length)
{
    size_t i;
    for (i = 0; i < length; ++i) {
        if (to_lower_ascii((unsigned char)a[i]) != to_lower_ascii((unsigned char)b[i]))
            return 0;
    }
    return 1;
}

/* Whether a byte is printable ASCII (a space through '~'). */
static int is_printable_ascii(unsigned char c)
{
    return c >= 32 && c <= 126;
}

/*
 * The most lines `text` can hold: one more than its number of newlines.
 * Used to size the entries array before any line is read.
 */
static size_t count_lines(const char *text, size_t length)
{
    size_t i, newlines = 0;
    for (i = 0; i < length; ++i) {
        if (text[i] == '\n')
            ++newlines;
    }
    return newlines + 1;
}

/*
 * Adds the names in one file's text to `names`.
 *
 * Each line looks like "=RRASH.SAV<TAB>Road Rash" (one exact file) or
 * "*SLOT<TAB>Doom" (every file starting with SLOT). Lines in any other shape
 * are skipped rather than treated as errors, since the file may have been
 * edited by hand.
 *
 * To save memory on the console, nothing is copied: the text is cut up in
 * place by writing zero bytes over the tab and newline characters, and each
 * entry points straight into it. So `text` must stay allocated for as long
 * as `names` is used, and must have a zero byte just past its end.
 */
static void add_names_from_text(SaveNames *names, char *text, size_t length, int learned)
{
    char *line = text;
    char *text_end = text + length;

    while (line < text_end) {
        /* Find where this line ends: the next newline, or the end of the text. */
        char *line_end = (char *)memchr(line, '\n', (size_t)(text_end - line));
        char kind = line[0]; /* '=' for an exact name, '*' for a prefix */
        char *tab;
        size_t key_length;

        if (!line_end)
            line_end = text_end;

        /* End the line here, dropping a Windows-style "\r" if there is one. */
        *line_end = 0;
        if (line_end > line && line_end[-1] == '\r')
            line_end[-1] = 0;

        /* The key runs from after the '=' or '*' up to the tab. */
        tab = strchr(line, '\t');
        key_length = tab ? (size_t)(tab - line - 1) : 0;

        /* Keep the line only if it has a kind, a key that fits an NVRAM file
         * name, and a title after the tab. */
        if ((kind == '=' || kind == '*') && tab && key_length > 0 && key_length < SAVE_NAME_BYTES &&
            tab[1] != 0) {
            SaveNameEntry *entry = &names->entries[names->count++];
            *tab = 0; /* ends the key, so it can be read as its own string */
            entry->key = line + 1;
            entry->title = tab + 1;
            entry->prefix = kind == '*';
            entry->learned = learned;
        }

        line = line_end + 1;
    }
}

int save_names_parse(SaveNames *names, const char *disc, size_t disc_length, const char *learned,
                     size_t learned_length)
{
    size_t most_entries;
    char *learned_copy, *disc_copy;

    memset(names, 0, sizeof(*names));
    if (disc_length > SAVE_NAMES_BYTES || learned_length > SAVE_NAMES_BYTES)
        return -1;

    /*
     * One allocation holds both texts, each followed by a zero byte:
     *     [learned text] 0 [disc text] 0
     * The entries array is sized for every line to be a valid name.
     */
    most_entries = count_lines(disc, disc_length) + count_lines(learned, learned_length);
    names->storage = (char *)malloc(learned_length + 1 + disc_length + 1);
    names->entries = (SaveNameEntry *)malloc(most_entries * sizeof(SaveNameEntry));
    if (!names->storage || !names->entries) {
        save_names_free(names);
        return -1;
    }

    learned_copy = names->storage;
    disc_copy = names->storage + learned_length + 1;
    memcpy(learned_copy, learned, learned_length);
    learned_copy[learned_length] = 0;
    memcpy(disc_copy, disc, disc_length);
    disc_copy[disc_length] = 0;

    add_names_from_text(names, learned_copy, learned_length, 1);
    add_names_from_text(names, disc_copy, disc_length, 0);
    return 0;
}

void save_names_free(SaveNames *names)
{
    free(names->storage);
    free(names->entries);
    memset(names, 0, sizeof(*names));
}

const char *save_names_find(const SaveNames *names, const char *file)
{
    const SaveNameEntry *best = NULL;
    size_t file_length = strlen(file);
    /*
     * The length of the longest prefix matched so far. Only a longer prefix
     * can replace it. An exact match sets it past the file name's length, so
     * no prefix (which is never longer than the name) can replace that.
     */
    size_t longest_match = 0;
    size_t builtin_count = sizeof(builtin_save_names) / sizeof(builtin_save_names[0]);
    size_t i;

    /* First the drive's own names, learned and from the discs. */
    for (i = 0; i < names->count; ++i) {
        const SaveNameEntry *entry = &names->entries[i];
        size_t key_length = strlen(entry->key);

        if (entry->prefix) {
            int matches =
                key_length <= file_length && same_ignoring_case(entry->key, file, key_length);
            if (matches && key_length > longest_match) {
                best = entry;
                longest_match = key_length;
            }
        } else if (key_length == file_length && same_ignoring_case(entry->key, file, file_length)) {
            /* A name the menu learned itself always wins. */
            if (entry->learned)
                return entry->title;
            /* Otherwise the first exact name from the discs wins over prefixes. */
            if (!best || best->prefix) {
                best = entry;
                longest_match = file_length + 1;
            }
        }
    }

    /*
     * Then the built-in list, which can only add to what the drive left open:
     * it never replaces an exact name found above.
     */
    for (i = 0; i < builtin_count; ++i) {
        const SaveNameEntry *entry = &builtin_save_names[i];
        size_t key_length = strlen(entry->key);
        int open_to_replace = !best || best->prefix;

        if (!entry->prefix) {
            if (key_length == file_length && same_ignoring_case(entry->key, file, file_length) &&
                open_to_replace)
                best = entry;
        } else if (key_length <= file_length && key_length > longest_match &&
                   same_ignoring_case(entry->key, file, key_length) && open_to_replace) {
            best = entry;
            longest_match = key_length;
        }
    }

    return best ? best->title : NULL;
}

/*
 * Writes `value` as exactly eight lower-case hexadecimal digits at `out`
 * (no terminating zero) and returns the position just after them.
 */
static char *write_hex8(char *out, unsigned long value)
{
    static const char digits[] = "0123456789abcdef";
    int shift;
    for (shift = 28; shift >= 0; shift -= 4)
        *out++ = digits[(value >> shift) & 15];
    return out;
}

/*
 * Writes `value` in decimal at `out` (no terminating zero) and returns the
 * position just after it. The menu avoids sprintf to keep its code small.
 */
static char *write_decimal(char *out, unsigned long value)
{
    char reversed[12]; /* digits from least to most significant */
    int count = 0;

    do {
        reversed[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);

    while (count)
        *out++ = reversed[--count];
    return out;
}

/*
 * Writes "size<TAB>crc32" for one NVRAM file at `out`, as it appears in a
 * snapshot line after the file name. Returns the position just after it.
 */
static char *write_size_and_checksum(char *out, const SaveEntry *file)
{
    out = write_decimal(out, (unsigned long)file->size);
    *out++ = '\t';
    return write_hex8(out, usb_art_crc32(file->data, file->size));
}

int save_snapshot_format(char *out, size_t capacity, const char *title, size_t title_length,
                         const SaveArchive *nvram)
{
    char *position = out;
    size_t largest_snapshot = SNAPSHOT_HEADER_LENGTH + SAVE_TITLE_BYTES +
                              nvram->count * (SAVE_NAME_BYTES + SNAPSHOT_LINE_EXTRA);
    size_t i;

    /* Check everything first, so nothing is written unless it all fits. */
    if (title_length == 0 || title_length >= SAVE_TITLE_BYTES || capacity < largest_snapshot)
        return -1;
    for (i = 0; i < title_length; ++i) {
        if ((unsigned char)title[i] < 32)
            return -1; /* a control character would break the line format */
    }

    /* Header line, then the title on its own line. */
    memcpy(position, SNAPSHOT_HEADER, SNAPSHOT_HEADER_LENGTH);
    position += SNAPSHOT_HEADER_LENGTH;
    memcpy(position, title, title_length);
    position += title_length;
    *position++ = '\n';

    /* One "FILE<TAB>size<TAB>crc32" line per NVRAM file. */
    for (i = 0; i < nvram->count; ++i) {
        const SaveEntry *file = &nvram->entries[i];
        size_t name_length = strlen(file->name);
        memcpy(position, file->name, name_length);
        position += name_length;
        *position++ = '\t';
        position = write_size_and_checksum(position, file);
        *position++ = '\n';
    }

    *position = 0;
    return 0;
}

/*
 * Finds the line for NVRAM file `file` in a snapshot's file lines (`body` up
 * to `end`) and returns where its "size<TAB>crc32" part starts, or NULL if
 * the file was not in the snapshot.
 */
static const char *find_snapshot_line(const char *body, const char *end, const char *file)
{
    size_t name_length = strlen(file);

    while (body < end) {
        const char *line_end = (const char *)memchr(body, '\n', (size_t)(end - body));
        size_t line_length;

        if (!line_end)
            line_end = end;
        line_length = (size_t)(line_end - body);

        /* The line must be this exact name followed by a tab. */
        if (line_length > name_length && body[name_length] == '\t' &&
            !memcmp(body, file, name_length))
            return body + name_length + 1;

        body = line_end + 1;
    }
    return NULL;
}

/*
 * Whether a snapshot recorded exactly `expected` (a "size<TAB>crc32" text of
 * `expected_length` bytes) for a file whose line continues at `recorded`.
 * The recorded value must match completely and then end its line.
 */
static int snapshot_value_matches(const char *recorded, const char *end, const char *expected,
                                  size_t expected_length)
{
    if (!recorded || end - recorded < (long)expected_length)
        return 0;
    if (memcmp(recorded, expected, expected_length) != 0)
        return 0;
    return recorded + expected_length == end || recorded[expected_length] == '\n';
}

int save_snapshot_learn(const char *snapshot, size_t length, const SaveArchive *nvram,
                        const SaveNames *names, char *learned, size_t capacity)
{
    const char *end = snapshot + length;
    const char *title, *body;
    size_t title_length;
    size_t used = strlen(learned); /* new lines go after the text already there */
    int added = 0;
    size_t i;

    /* The snapshot starts with its header line, then the title line. */
    if (length < SNAPSHOT_HEADER_LENGTH + 2 ||
        memcmp(snapshot, SNAPSHOT_HEADER, SNAPSHOT_HEADER_LENGTH) != 0)
        return -1;
    title = snapshot + SNAPSHOT_HEADER_LENGTH;
    body = (const char *)memchr(title, '\n', (size_t)(end - title));
    if (!body)
        return -1;
    title_length = (size_t)(body - title);
    if (title_length == 0 || title_length >= SAVE_TITLE_BYTES)
        return -1;
    ++body; /* the file lines start after the title's newline */

    for (i = 0; i < nvram->count; ++i) {
        const SaveEntry *file = &nvram->entries[i];
        size_t name_length = strlen(file->name);
        char now[24]; /* this file's "size<TAB>crc32" as it is now */
        size_t now_length = (size_t)(write_size_and_checksum(now, file) - now);
        const char *recorded = find_snapshot_line(body, end, file->name);

        /* Unchanged since the game started: it says nothing about that game. */
        if (snapshot_value_matches(recorded, end, now, now_length))
            continue;
        /* Already named, by the drive's files or the built-in list. */
        if (save_names_find(names, file->name))
            continue;

        /* Append "=FILE<TAB>Title\n", keeping room for the terminating zero. */
        if (used + 1 + name_length + 1 + title_length + 1 + 1 > capacity)
            return added;
        learned[used++] = '=';
        memcpy(learned + used, file->name, name_length);
        used += name_length;
        learned[used++] = '\t';
        memcpy(learned + used, title, title_length);
        used += title_length;
        learned[used++] = '\n';
        learned[used] = 0;
        ++added;
    }
    return added;
}

int save_slot_labels_parse(SaveSlotLabels *out, const char *text, size_t length)
{
    SaveSlotLabels parsed;
    unsigned int seen_slots = 0; /* bit N is set once slot N has a label */
    size_t position = 0;

    memset(&parsed, 0, sizeof(parsed));
    if (length > SAVE_SLOT_LABEL_FILE_BYTES)
        return -1;

    /* Read line by line, stopping at the end or at a zero byte. */
    while (position < length && text[position]) {
        size_t line_start = position, line_end, label_length, i;
        unsigned int slot;

        while (position < length && text[position] && text[position] != '\n')
            ++position;
        line_end = position;
        if (line_end > line_start && text[line_end - 1] == '\r')
            --line_end; /* a Windows-style line ending */
        if (position < length && text[position] == '\n')
            ++position;
        if (line_end == line_start)
            continue; /* blank lines are allowed */

        /* "S<TAB>Label", with S from 1 to 8. */
        if (line_end - line_start < 2 || text[line_start] < '1' || text[line_start] > '8' ||
            text[line_start + 1] != '\t')
            return -1;
        slot = (unsigned int)(text[line_start] - '1');
        label_length = line_end - line_start - 2;

        /* Each slot at most once, with a label that fits. */
        if ((seen_slots & (1U << slot)) || label_length >= SAVE_SLOT_LABEL_BYTES)
            return -1;
        for (i = 0; i < label_length; ++i) {
            if (!is_printable_ascii((unsigned char)text[line_start + 2 + i]))
                return -1;
        }

        seen_slots |= 1U << slot;
        memcpy(parsed.labels[slot], text + line_start + 2, label_length);
    }

    *out = parsed;
    return 0;
}

int save_slot_labels_format(const SaveSlotLabels *labels, char *out, size_t capacity)
{
    size_t used = 0;
    size_t slot, i;

    for (slot = 0; slot < SAVE_SLOTS_PER_LABEL_FILE; ++slot) {
        const char *label = labels->labels[slot];
        size_t label_length = 0;

        if (!label[0])
            continue; /* unlabelled slots get no line */

        /* The label must end within its array: a full array has no zero. */
        while (label_length < SAVE_SLOT_LABEL_BYTES && label[label_length])
            ++label_length;
        if (label_length == SAVE_SLOT_LABEL_BYTES)
            return -1;
        /* Room for "S<TAB>", the label, a newline and the final zero. */
        if (used + label_length + 4 > capacity)
            return -1;
        for (i = 0; i < label_length; ++i) {
            if (!is_printable_ascii((unsigned char)label[i]))
                return -1;
        }

        out[used++] = (char)('1' + slot);
        out[used++] = '\t';
        memcpy(out + used, label, label_length);
        used += label_length;
        out[used++] = '\n';
    }

    if (used >= capacity)
        return -1;
    out[used] = 0;
    return 0;
}
