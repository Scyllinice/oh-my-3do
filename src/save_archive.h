#ifndef OH_MY_3DO_SAVE_ARCHIVE_H
#define OH_MY_3DO_SAVE_ARCHIVE_H
#include <stddef.h>
#define SAVE_SLOT_LIMIT     999999999UL
#define SAVE_RECOVERY_SLOT  0xfffffffeUL
#define SAVE_RECOVERY2_SLOT 0xffffffffUL
#define SAVE_MAX_FILES      128
#define SAVE_MAX_BYTES      65536UL
#define SAVE_NAME_BYTES     32
#define SAVE_HEADER         24
#define SAVE_RECORD         48

typedef struct SaveEntry {
    char name[SAVE_NAME_BYTES];
    unsigned long type;
    size_t size;
    const unsigned char *data;
} SaveEntry;
typedef struct SaveArchive {
    unsigned char *bytes;
    size_t length;
    size_t count;
    SaveEntry entries[SAVE_MAX_FILES];
} SaveArchive;
void save_archive_free(SaveArchive *archive);
int save_name_valid(const char *name);
unsigned long save_word(const unsigned char *p);
void save_put_word(unsigned char *p, unsigned long value);
/* Parse copies input, validates all bounds/checksums/names/duplicates first. */
int save_archive_parse(SaveArchive *archive, const unsigned char *data, size_t size);
int save_restore_matches(const SaveArchive *before, const SaveArchive *after,
                         const SaveEntry *target);
int save_archive_build(SaveArchive *archive, const SaveEntry *entries, size_t count);
/* Add/replace one filename (case insensitive), preserving destination order
 * and every other entry. Failure leaves out unchanged; aliases are allowed. */
int save_archive_merge(SaveArchive *out, const SaveArchive *destination, const SaveEntry *entry);
enum { SAVE_DIFF_SAME, SAVE_DIFF_CHANGED, SAVE_DIFF_ADDED, SAVE_DIFF_REMOVED };
typedef struct SaveDifference {
    size_t before, after, length, changed_bytes;
    int state, metadata_changed;
} SaveDifference;
size_t save_archive_diff_count(const SaveArchive *before, const SaveArchive *after);
int save_archive_diff(const SaveArchive *before, const SaveArchive *after, size_t index,
                      SaveDifference *out);
#endif
