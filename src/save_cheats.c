/*
 * Known save layouts: matching, editing and loading the database file.
 * See save_cheats.h for what a profile is.
 *
 * Database file layout (save-cheats.3db; numbers are 32-bit big-endian,
 * strings are fixed-size, zero-terminated and zero-padded). Version 2
 * ("OM3DCHT2") added the fields marked [v2]; version 1 files ("OM3DCHT1")
 * are still read, with fixed defaults for them.
 *
 *   header, 24 bytes:
 *     0   8   magic "OM3DCHT2" (or "OM3DCHT1")
 *     8   4   size of the whole file
 *     12  4   number of profiles
 *     16  4   CRC32 of everything after the header
 *     20  4   reserved, 0
 *   then each profile:
 *     0   4   length of this profile's whole record
 *     4   4   save size
 *     8   4   signature offset
 *     12  4   signature value
 *     16  4   checksum offset
 *     20  4   checksum seed
 *     24  4   number of fields
 *     28  4   number of presets
 *     32  4   [v2] signature width in bytes   (v1: always 4)
 *     36  4   [v2] checksum kind, 0 or 1      (v1: always 1)
 *     40  8   [v2] reserved, 0
 *     then 64 bytes id, 32 bytes game title, 32 bytes save file name
 *     then each field (v2: 48 bytes, v1: 40):
 *       0   28  label
 *       28  4   offset
 *       32  4   minimum
 *       36  4   maximum
 *       40  4   [v2] width in bytes   (v1: 4)
 *       44  4   [v2] mask             (v1: all bits)
 *     then each preset, 48 bytes:
 *       0   36  label
 *       36  4   field
 *       40  4   value
 *       44  4   [v2] number of extra changes (v1: 0)
 *       followed by that many 8-byte changes: field, value
 */
#include "save_cheats.h"
#include <string.h>
#include <stdlib.h>
#include "usb_art.h"
#include "save_cheat_db.h"

/* File and record sizes; see the layout above. */
#define DB_HEADER_BYTES  24
#define PROFILE_FIXED_V1 32
#define PROFILE_FIXED_V2 48
#define PROFILE_STRINGS  128 /* id (64), game (32), file name (32) */
#define FIELD_BYTES_V1   40
#define FIELD_BYTES_V2   48
#define PRESET_BYTES     48
#define CHANGE_BYTES     8

/* Limits on what one database may hold. */
#define MAX_PROFILES       128
#define MAX_FIELDS         160
#define MAX_PRESETS        32
#define MAX_PRESET_CHANGES 159
#define MAX_SAVE_BYTES     32768 /* the size of NVRAM */

/*
 * The database loaded from the drive, if any. While external_profiles is
 * NULL the built-in table `cheat_profiles` (from save_cheat_db.h) is used.
 * The profiles' strings point into external_bytes.
 */
static SaveCheatProfile *external_profiles;
static unsigned char *external_bytes;
static size_t external_count;

/* Reads a `width`-byte big-endian number. */
static unsigned long read_number(const unsigned char *data, size_t width)
{
    unsigned long value = 0;
    while (width--)
        value = (value << 8) | *data++;
    return value;
}

/* How far a mask's lowest set bit is from bit 0 (the mask must not be 0). */
static unsigned int lowest_bit(unsigned long mask)
{
    unsigned int position = 0;
    while (!(mask & 1)) {
        mask >>= 1;
        ++position;
    }
    return position;
}

unsigned long save_cheats_value(const SaveEntry *entry, const SaveCheatField *field)
{
    unsigned long stored = read_number(entry->data + field->offset, field->width);
    return (stored & field->mask) >> lowest_bit(field->mask);
}

/* The checksum a save should have: the seed plus every 32-bit word before
 * the checksum, wrapping around at 32 bits. */
static unsigned long expected_checksum(const SaveEntry *entry, const SaveCheatProfile *profile)
{
    size_t i;
    unsigned long sum = profile->checksum_seed;
    for (i = 0; i < profile->checksum_offset; i += 4)
        sum = (sum + save_word(entry->data + i)) & 0xffffffffUL;
    return sum;
}

/* Whether a save matches a profile completely (see save_cheats.h). */
static int profile_matches(const SaveEntry *entry, const SaveCheatProfile *profile)
{
    size_t i;

    if (!entry || !entry->data || strcmp(entry->name, profile->filename) ||
        entry->size != profile->size)
        return 0;
    if (read_number(entry->data + profile->signature_offset, profile->signature_width) !=
        profile->signature)
        return 0;
    if (profile->checksum_kind &&
        save_word(entry->data + profile->checksum_offset) != expected_checksum(entry, profile))
        return 0;

    for (i = 0; i < profile->field_count; ++i) {
        unsigned long value = save_cheats_value(entry, &profile->fields[i]);
        if (value < profile->fields[i].min || value > profile->fields[i].max)
            return 0;
    }
    return 1;
}

/* The profiles in use, and how many. */
static const SaveCheatProfile *active_profiles(size_t *count)
{
    if (external_profiles) {
        *count = external_count;
        return external_profiles;
    }
    *count = sizeof(cheat_profiles) / sizeof(cheat_profiles[0]);
    return cheat_profiles;
}

const SaveCheatProfile *save_cheats_find(const SaveEntry *entry)
{
    size_t count, i;
    const SaveCheatProfile *profiles = active_profiles(&count);
    for (i = 0; i < count; ++i) {
        if (profile_matches(entry, &profiles[i]))
            return &profiles[i];
    }
    return NULL;
}

/* Writes `value` into a field's bits, leaving the other bits of its bytes alone. */
static void write_field(SaveEntry *entry, const SaveCheatField *field, unsigned long value)
{
    unsigned char *bytes = (unsigned char *)entry->data + field->offset;
    unsigned long stored = read_number(bytes, field->width);
    unsigned long other_bits = stored & (~field->mask & 0xffffffffUL);
    unsigned long updated = other_bits | ((value << lowest_bit(field->mask)) & field->mask);
    size_t i = field->width;

    /* Store big-endian: the last byte gets the lowest 8 bits. */
    while (i--) {
        bytes[i] = (unsigned char)updated;
        updated >>= 8;
    }
}

/* Recomputes and stores the save's checksum, if its layout has one. */
static void fix_checksum(SaveEntry *entry, const SaveCheatProfile *profile)
{
    if (profile->checksum_kind)
        save_put_word((unsigned char *)entry->data + profile->checksum_offset,
                      expected_checksum(entry, profile));
}

int save_cheats_set(SaveEntry *entry, const SaveCheatProfile *profile, size_t field,
                    unsigned long value)
{
    if (!profile || save_cheats_find(entry) != profile || field >= profile->field_count ||
        value < profile->fields[field].min || value > profile->fields[field].max)
        return -1;
    write_field(entry, &profile->fields[field], value);
    fix_checksum(entry, profile);
    return 0;
}

int save_cheats_preset(SaveEntry *entry, const SaveCheatProfile *profile, size_t preset)
{
    const SaveCheatPreset *chosen;
    size_t i;

    if (!profile || save_cheats_find(entry) != profile || preset >= profile->preset_count)
        return -1;
    /* The loader checked every preset's fields and values are valid. */
    chosen = &profile->presets[preset];
    write_field(entry, &profile->fields[chosen->field], chosen->value);
    for (i = 0; i < chosen->additional_count; ++i)
        write_field(entry, &profile->fields[chosen->additional[i].field],
                    chosen->additional[i].value);
    fix_checksum(entry, profile);
    return 0;
}

/* Frees profiles made by save_cheats_load (not their strings, which point
 * into the database copy). */
static void free_profiles(SaveCheatProfile *profiles, size_t count)
{
    size_t i, j;
    if (!profiles)
        return;
    for (i = 0; i < count; ++i) {
        if (profiles[i].presets) {
            for (j = 0; j < profiles[i].preset_count; ++j)
                free((void *)profiles[i].presets[j].additional);
        }
        free((void *)profiles[i].fields);
        free((void *)profiles[i].presets);
    }
    free(profiles);
}

void save_cheats_reset(void)
{
    free_profiles(external_profiles, external_count);
    free(external_bytes);
    external_profiles = NULL;
    external_bytes = NULL;
    external_count = 0;
}

int save_cheats_external(void)
{
    return external_profiles != NULL;
}

size_t save_cheats_profile_count(void)
{
    size_t count;
    active_profiles(&count);
    return count;
}

/*
 * Whether a fixed-size string field (`capacity` bytes) holds 1 to `longest`
 * printable ASCII characters, then zeros to the end of the field.
 */
static int is_valid_string(const unsigned char *text, size_t capacity, size_t longest)
{
    size_t length = 0;

    while (length < capacity && text[length]) {
        if (text[length] < 32 || text[length] > 126)
            return 0;
        ++length;
    }
    if (length == 0 || length > longest || length == capacity)
        return 0;
    while (length < capacity) {
        if (text[length++])
            return 0;
    }
    return 1;
}

static int is_valid_width(size_t width)
{
    return width == 1 || width == 2 || width == 4;
}

/* Whether `field` exists in the profile and `value` is within its range. */
static int is_valid_change(const SaveCheatProfile *profile, size_t field, unsigned long value)
{
    return field < profile->field_count && value >= profile->fields[field].min &&
           value <= profile->fields[field].max;
}

/*
 * Reads a profile's fixed part at `record` and checks it describes a
 * sensible layout: save size, signature and checksum placement, counts.
 * Returns 0, or -1.
 */
static int read_profile_fixed(SaveCheatProfile *profile, const unsigned char *record, int version2)
{
    profile->size = (size_t)save_word(record + 4);
    profile->signature_offset = (size_t)save_word(record + 8);
    profile->signature = save_word(record + 12);
    profile->checksum_offset = (size_t)save_word(record + 16);
    profile->checksum_seed = save_word(record + 20);
    profile->field_count = (size_t)save_word(record + 24);
    profile->preset_count = (size_t)save_word(record + 28);
    profile->signature_width = version2 ? (size_t)save_word(record + 32) : 4;
    profile->checksum_kind = version2 ? (int)save_word(record + 36) : 1;

    /* A whole number of 32-bit words, no bigger than NVRAM. */
    if (profile->size < 8 || profile->size > MAX_SAVE_BYTES || profile->size % 4)
        return -1;
    /* The signature lies before the checksum, and its value fits its width. */
    if (!is_valid_width(profile->signature_width) || profile->checksum_offset > profile->size ||
        profile->signature_width > profile->checksum_offset ||
        profile->signature_offset > profile->checksum_offset - profile->signature_width)
        return -1;
    if (profile->signature_width < 4 &&
        profile->signature >= (1UL << (profile->signature_width * 8)))
        return -1;
    /* A checksum is the last word; without one, there is no seed either. */
    if (profile->checksum_kind != 0 && profile->checksum_kind != 1)
        return -1;
    if (profile->checksum_kind
            ? profile->checksum_offset != profile->size - 4
            : (profile->checksum_offset != profile->size || profile->checksum_seed))
        return -1;
    if (!profile->field_count || profile->field_count > MAX_FIELDS || !profile->preset_count ||
        profile->preset_count > MAX_PRESETS)
        return -1;
    if (version2 && (save_word(record + 40) || save_word(record + 44)))
        return -1;
    return 0;
}

/*
 * Reads and checks one field record. `occupied` has one byte per save byte,
 * with the bits already claimed by the signature or earlier fields set; the
 * field's own bits are added. Fields may not overlap each other or the
 * signature, and must end before the checksum. Returns 0, or -1.
 */
static int read_field(SaveCheatField *field, const unsigned char *record, int version2,
                      const SaveCheatProfile *profile, unsigned char *occupied)
{
    unsigned long value_bits;
    size_t k;

    if (!is_valid_string(record, 28, 25))
        return -1;
    field->label = (const char *)record;
    field->offset = (size_t)save_word(record + 28);
    field->min = save_word(record + 32);
    field->max = save_word(record + 36);
    field->width = version2 ? (size_t)save_word(record + 40) : 4;
    field->mask = version2 ? save_word(record + 44) : 0xffffffffUL;

    if (!is_valid_width(field->width) || field->width > profile->checksum_offset ||
        field->offset > profile->checksum_offset - field->width || !field->mask ||
        field->min > field->max)
        return -1;
    if (field->width < 4 && field->mask >= (1UL << (field->width * 8)))
        return -1;

    /*
     * The mask must be one unbroken run of bits: shifted down to bit 0 it is
     * then 2^n - 1, and adding 1 to that clears all its bits. The largest
     * value must fit in the run.
     */
    value_bits = field->mask >> lowest_bit(field->mask);
    if ((value_bits & ((value_bits + 1) & 0xffffffffUL)) || field->max > value_bits)
        return -1;

    /* Claim the field's bits, byte by byte (the mask's top byte is the field's first). */
    for (k = 0; k < field->width; ++k) {
        unsigned char bits = (unsigned char)(field->mask >> ((field->width - 1 - k) * 8));
        if (occupied[field->offset + k] & bits)
            return -1;
        occupied[field->offset + k] |= bits;
    }
    return 0;
}

int save_cheats_load(const unsigned char *bytes, size_t size)
{
    unsigned char *copy = NULL;     /* the database, which strings will point into */
    unsigned char *occupied = NULL; /* per profile: bits already claimed (see read_field) */
    SaveCheatProfile *profiles = NULL;
    size_t count, position = DB_HEADER_BYTES, i, j, k;
    size_t profile_header, field_bytes;
    int result = -1, version2;

    /* The file header: magic, size, reserved, checksum. */
    if (!bytes || size < DB_HEADER_BYTES || size > SAVE_CHEAT_DB_MAX_BYTES)
        return -1;
    version2 = !memcmp(bytes, "OM3DCHT2", 8);
    if ((!version2 && memcmp(bytes, "OM3DCHT1", 8)) || save_word(bytes + 8) != size ||
        save_word(bytes + 20) ||
        save_word(bytes + 16) != usb_art_crc32(bytes + DB_HEADER_BYTES, size - DB_HEADER_BYTES))
        return -1;

    profile_header = (version2 ? PROFILE_FIXED_V2 : PROFILE_FIXED_V1) + PROFILE_STRINGS;
    field_bytes = version2 ? FIELD_BYTES_V2 : FIELD_BYTES_V1;

    /* Every profile needs at least its header, one field and one preset. */
    count = (size_t)save_word(bytes + 12);
    if (!count || count > MAX_PROFILES ||
        count > (size - DB_HEADER_BYTES) / (profile_header + field_bytes + PRESET_BYTES))
        return -1;

    copy = (unsigned char *)malloc(size);
    profiles = (SaveCheatProfile *)calloc(count, sizeof(*profiles));
    if (!copy || !profiles)
        goto done;
    memcpy(copy, bytes, size);

    for (i = 0; i < count; ++i) {
        SaveCheatProfile *profile = &profiles[i];
        const unsigned char *record = copy + position;
        const unsigned char *strings;
        SaveCheatField *fields;
        SaveCheatPreset *presets;
        size_t length, end;

        /* The profile's record, which must fit in the file. */
        if (size - position < profile_header)
            goto done;
        length = (size_t)save_word(record);
        if (length < profile_header || length > size - position)
            goto done;
        end = position + length;
        if (read_profile_fixed(profile, record, version2) < 0)
            goto done;

        /* Its id, game title and save file name. */
        strings = record + (version2 ? PROFILE_FIXED_V2 : PROFILE_FIXED_V1);
        if (!is_valid_string(strings, 64, 63) || !is_valid_string(strings + 64, 32, 31) ||
            !is_valid_string(strings + 96, 32, 31))
            goto done;
        profile->id = (const char *)strings;
        profile->game = (const char *)strings + 64;
        profile->filename = (const char *)strings + 96;
        if (!save_name_valid(profile->filename))
            goto done;

        /* No repeated ids, and no two profiles that would match the same save. */
        for (j = 0; j < i; ++j) {
            if (!strcmp(profile->id, profiles[j].id) ||
                (!strcmp(profile->filename, profiles[j].filename) &&
                 profile->size == profiles[j].size &&
                 profile->signature_offset == profiles[j].signature_offset &&
                 profile->signature == profiles[j].signature))
                goto done;
        }

        /* The fields. */
        if (profile->field_count > (length - profile_header) / field_bytes)
            goto done;
        fields = (SaveCheatField *)calloc(profile->field_count, sizeof(*fields));
        presets = (SaveCheatPreset *)calloc(profile->preset_count, sizeof(*presets));
        profile->fields = fields;
        profile->presets = presets;
        occupied = (unsigned char *)calloc(profile->size, 1);
        if (!fields || !presets || !occupied)
            goto done;
        memset(occupied + profile->signature_offset, 255, profile->signature_width);
        position += profile_header;
        for (j = 0; j < profile->field_count; ++j) {
            if (read_field(&fields[j], copy + position, version2, profile, occupied) < 0)
                goto done;
            position += field_bytes;
        }
        free(occupied);
        occupied = NULL;

        /* The presets, each followed by its extra changes. */
        for (j = 0; j < profile->preset_count; ++j) {
            SaveCheatChange *changes;
            size_t change_count;

            if (end - position < PRESET_BYTES)
                goto done;
            record = copy + position;
            if (!is_valid_string(record, 36, 34))
                goto done;
            presets[j].label = (const char *)record;
            presets[j].field = (size_t)save_word(record + 36);
            presets[j].value = save_word(record + 40);
            change_count = (size_t)save_word(record + 44);
            position += PRESET_BYTES;
            if ((!version2 && change_count) || change_count > MAX_PRESET_CHANGES ||
                change_count > (end - position) / CHANGE_BYTES ||
                !is_valid_change(profile, presets[j].field, presets[j].value))
                goto done;

            presets[j].additional_count = change_count;
            changes =
                change_count ? (SaveCheatChange *)calloc(change_count, sizeof(*changes)) : NULL;
            presets[j].additional = changes;
            if (change_count && !changes)
                goto done;
            for (k = 0; k < change_count; ++k) {
                size_t earlier;
                changes[k].field = (size_t)save_word(copy + position);
                changes[k].value = save_word(copy + position + 4);
                position += CHANGE_BYTES;
                /* Each extra change sets a different field from the preset's
                 * own and from every other extra change. */
                if (!is_valid_change(profile, changes[k].field, changes[k].value) ||
                    changes[k].field == presets[j].field)
                    goto done;
                for (earlier = 0; earlier < k; ++earlier) {
                    if (changes[earlier].field == changes[k].field)
                        goto done;
                }
            }
        }

        /* The record must be used up exactly. */
        if (position != end)
            goto done;
    }
    /* And so must the file. */
    if (position != size)
        goto done;

    /* All checked: replace the profiles in use. */
    save_cheats_reset();
    external_profiles = profiles;
    external_bytes = copy;
    external_count = count;
    profiles = NULL;
    copy = NULL;
    result = 0;

done:
    free(occupied);
    free_profiles(profiles, count);
    free(copy);
    return result;
}
