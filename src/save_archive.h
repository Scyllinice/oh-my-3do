#ifndef OH_MY_3DO_SAVE_ARCHIVE_H
#define OH_MY_3DO_SAVE_ARCHIVE_H
#include <stddef.h>
#define SAVE_MAX_FILES 128
#define SAVE_MAX_BYTES 65536UL
#define SAVE_NAME_BYTES 32
#define SAVE_HEADER 24
#define SAVE_RECORD 48

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
int save_restore_matches(const SaveArchive *before, const SaveArchive *after, const SaveEntry *target);
int save_archive_build(SaveArchive *archive, const SaveEntry *entries, size_t count);
/* Add/replace one filename (case insensitive), preserving destination order
 * and every other entry. Failure leaves out unchanged; aliases are allowed. */
int save_archive_merge(SaveArchive *out, const SaveArchive *destination, const SaveEntry *entry);
#endif
