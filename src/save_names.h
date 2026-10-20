#ifndef OH_MY_3DO_SAVE_NAMES_H
#define OH_MY_3DO_SAVE_NAMES_H
#include "save_archive.h"
/* save-names.txt is written by the PC tools from the discs; saves-learned.txt
 * by the menu. Lines are "=FILE<TAB>Title" (exact) or "*PREFIX<TAB>Title". */
#define SAVE_NAMES_BYTES 16384
#define SAVE_SNAPSHOT_BYTES 8192
#define SAVE_TITLE_BYTES 64
typedef struct SaveNameEntry {
    const char *key;
    const char *title;
    int prefix;
    int learned;
} SaveNameEntry;
typedef struct SaveNames {
    char *storage;
    SaveNameEntry *entries;
    size_t count;
} SaveNames;
int save_names_parse(SaveNames *names, const char *disc, size_t disc_length, const char *learned, size_t learned_length);
void save_names_free(SaveNames *names);
/* Learned names win, then exact disc names, then the longest prefix. */
const char *save_names_find(const SaveNames *names, const char *file);
/* "OH3DO_SAVESNAP1\nTitle\n" then "FILE<TAB>size<TAB>crc\n" per NVRAM file. */
int save_snapshot_format(char *out, size_t capacity, const char *title, size_t title_length, const SaveArchive *nvram);
/* Appends "=FILE<TAB>Title\n" to learned for each file that is new or changed
 * since the snapshot and has no name yet. Returns the number added, or -1. */
int save_snapshot_learn(const char *snapshot, size_t length, const SaveArchive *nvram, const SaveNames *names,
                        char *learned, size_t capacity);
#define SAVE_SLOT_LABEL_BYTES 25
#define SAVE_SLOT_LABEL_FILE_BYTES 512
typedef struct SaveSlotLabels { char labels[8][SAVE_SLOT_LABEL_BYTES]; } SaveSlotLabels;
int save_slot_labels_parse(SaveSlotLabels *out, const char *text, size_t length);
int save_slot_labels_format(const SaveSlotLabels *labels, char *out, size_t capacity);
#endif
