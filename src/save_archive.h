/*
 * Save archives (.3ds files): copies of console saves kept on the USB drive.
 *
 * The console keeps game saves as small files in its NVRAM (battery-backed
 * memory). The menu backs them up into archive files on the drive, in
 * .ohmy3do/saves (numbered backup slots, plus recovery.3ds and recovery2.3ds
 * made automatically before any restore). An archive holds any number of
 * save files, each with its name, type and contents.
 *
 * File layout (numbers are 32-bit big-endian):
 *
 *   header, 24 bytes:
 *     0   8   magic "OM3DSAV1"
 *     8   4   size of the whole archive in bytes
 *     12  4   number of save files
 *     16  4   CRC32 of everything after the header
 *     20  4   reserved, 0
 *   then for each save file, a 48-byte record followed by its contents:
 *     0   32  name, zero-terminated and zero-padded
 *     32  4   type (the NVRAM file type, kept as is)
 *     36  4   size of the contents in bytes
 *     40  4   CRC32 of the contents
 *     44  4   reserved, 0
 */
#ifndef OH_MY_3DO_SAVE_ARCHIVE_H
#define OH_MY_3DO_SAVE_ARCHIVE_H
#include <stddef.h>

/* The highest numbered backup slot. */
#define SAVE_SLOT_LIMIT 999999999UL
/* Pseudo slot numbers for the two recovery archives. */
#define SAVE_RECOVERY_SLOT  0xfffffffeUL
#define SAVE_RECOVERY2_SLOT 0xffffffffUL

/* Limits: save files per archive, and the size of a whole archive. */
#define SAVE_MAX_FILES 128
#define SAVE_MAX_BYTES 65536UL

/* The longest save file name, including its terminating zero. */
#define SAVE_NAME_BYTES 32

/* Sizes of the archive header and of each file's record (see above). */
#define SAVE_HEADER 24
#define SAVE_RECORD 48

/* One save file. In a parsed archive, `data` points into SaveArchive.bytes. */
typedef struct SaveEntry {
    char name[SAVE_NAME_BYTES];
    unsigned long type;
    size_t size;
    const unsigned char *data;
} SaveEntry;

/* A whole archive in memory. Free with save_archive_free. */
typedef struct SaveArchive {
    unsigned char *bytes; /* the archive file, which the entries point into */
    size_t length;        /* its size */
    size_t count;
    SaveEntry entries[SAVE_MAX_FILES];
} SaveArchive;

/* Frees the archive and leaves it empty. */
void save_archive_free(SaveArchive *archive);

/*
 * Whether a save file name is acceptable: 1 to 31 characters, not "." or
 * "..", without control characters, slashes, backslashes or colons (it may
 * become part of a path on the drive).
 */
int save_name_valid(const char *name);

/* Read and write 32-bit big-endian numbers. */
unsigned long save_word(const unsigned char *p);
void save_put_word(unsigned char *p, unsigned long value);

/*
 * Reads an archive from `size` bytes of `data`, copying them. Everything is
 * checked before the archive is accepted: sizes, checksums, names (valid,
 * zero-padded, no two the same ignoring case) and that the files fill the
 * archive exactly. The old archive is replaced only on success.
 * Returns 0, or -1 for an invalid archive or when memory runs out.
 */
int save_archive_parse(SaveArchive *archive, const unsigned char *data, size_t size);

/*
 * Checks that restoring `target` into NVRAM did exactly that: `after` (NVRAM
 * read back) holds `target`, and every other file from `before` unchanged.
 * Returns 1 if so, 0 if anything else changed or is missing.
 */
int save_restore_matches(const SaveArchive *before, const SaveArchive *after,
                         const SaveEntry *target);

/*
 * Builds an archive from `count` save files (which are copied), then reads
 * it back with save_archive_parse, so a built archive is checked like a
 * loaded one. Returns 0, or -1 if a name is invalid or it would be too big.
 */
int save_archive_build(SaveArchive *archive, const SaveEntry *entries, size_t count);

/*
 * Makes `out` from `destination` with `entry` added, or replacing the file of
 * the same name (ignoring case, and keeping the existing name's case). Every
 * other file and the order are kept. On failure `out` is unchanged; `out`
 * may be the same archive as `destination`.
 */
int save_archive_merge(SaveArchive *out, const SaveArchive *destination, const SaveEntry *entry);

/* How a file differs between two archives (see save_archive_diff). */
enum { SAVE_DIFF_SAME, SAVE_DIFF_CHANGED, SAVE_DIFF_ADDED, SAVE_DIFF_REMOVED };

typedef struct SaveDifference {
    size_t before;        /* the file's index in `before`, or before->count if absent */
    size_t after;         /* its index in `after`, or after->count if absent */
    size_t length;        /* the longer of the two sizes */
    size_t changed_bytes; /* bytes that differ, counting bytes only one side has */
    int state;            /* SAVE_DIFF_... */
    int metadata_changed; /* the type, or the name's case, differs */
} SaveDifference;

/*
 * Comparing two archives file by file. The rows are every file of `before`
 * in order, then the files only `after` has. _count returns the number of
 * rows; _diff describes row `index` and returns 0, or -1 if there is no such
 * row. Files are matched by name, ignoring case.
 */
size_t save_archive_diff_count(const SaveArchive *before, const SaveArchive *after);
int save_archive_diff(const SaveArchive *before, const SaveArchive *after, size_t index,
                      SaveDifference *out);
#endif
