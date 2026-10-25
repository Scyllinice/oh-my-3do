#ifndef OH3DO_SAVE_CHEATS_H
#define OH3DO_SAVE_CHEATS_H
#include "save_archive.h"
typedef struct SaveCheatField {
    const char *label;
    size_t offset;
    unsigned long min, max;
    size_t width;
    unsigned long mask;
} SaveCheatField;
typedef struct SaveCheatChange {
    size_t field;
    unsigned long value;
} SaveCheatChange;
typedef struct SaveCheatPreset {
    const char *label;
    size_t field;
    unsigned long value;
    const SaveCheatChange *additional;
    size_t additional_count;
} SaveCheatPreset;
typedef struct SaveCheatProfile {
    const char *id, *game, *filename;
    size_t size, signature_offset;
    unsigned long signature;
    size_t checksum_offset;
    unsigned long checksum_seed;
    const SaveCheatField *fields;
    size_t field_count;
    const SaveCheatPreset *presets;
    size_t preset_count;
    size_t signature_width;
    int checksum_kind;
} SaveCheatProfile;
/* Only recognizes a complete, valid layout and checksum. Offsets are save-file
 * offsets, never runtime RAM addresses. Rejected edits never change bytes. */
const SaveCheatProfile *save_cheats_find(const SaveEntry *entry);
unsigned long save_cheats_value(const SaveEntry *entry, const SaveCheatField *field);
int save_cheats_set(SaveEntry *entry, const SaveCheatProfile *profile, size_t field,
                    unsigned long value);
int save_cheats_preset(SaveEntry *entry, const SaveCheatProfile *profile, size_t preset);
#define SAVE_CHEAT_DB_MAX_BYTES 65536UL
/* Installs a fully validated owned copy; failure keeps the previous database. */
int save_cheats_load(const unsigned char *bytes, size_t size);
void save_cheats_reset(void);
size_t save_cheats_profile_count(void);
int save_cheats_external(void);
#endif
