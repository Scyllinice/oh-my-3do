/*
 * Reading and restoring the console's NVRAM, where games keep their saves.
 *
 * NVRAM is 32 KiB of battery-backed memory that the 3DO's operating system
 * (Portfolio) presents as a small file system, /NVRAM, holding one file per
 * save. These functions use Portfolio's file calls, so they only build with
 * the 3DO SDK (tests build them against stand-in headers in tests/nvram_sdk).
 *
 * Restoring changes NVRAM, which is not undoable. The menu therefore always
 * backs NVRAM up to the USB drive first (and checks that backup) before it
 * calls nvram_restore.
 */
#ifndef OH_MY_3DO_NVRAM_IO_H
#define OH_MY_3DO_NVRAM_IO_H
#include "save_archive.h"

/* What the menu learns about NVRAM, and the last error if something failed. */
typedef struct NvramInfo {
    unsigned long free_bytes;     /* space left for saves */
    unsigned long used_bytes;     /* space in use */
    unsigned long max_file_bytes; /* the largest file NVRAM could hold now */
    unsigned long block_size;     /* NVRAM allocates space in blocks of this size */
    long last_error;              /* Portfolio's error code, or -1 */
    const char *error;            /* a message for the screen, or NULL */
} NvramInfo;

/*
 * Reads every save in NVRAM into `archive`, sorted by name, and fills `info`.
 * Returns 0, or -1 (with info->error set) if NVRAM could not be read
 * completely: a partial backup is never presented as a full one.
 */
int nvram_snapshot(SaveArchive *archive, NvramInfo *info);

/*
 * Checks, without changing anything, that `entry` could be written to NVRAM:
 * a valid name and size, and enough free space (counting the space an
 * existing file of that name already has). Returns 0, or -1 with
 * info->error set.
 */
int nvram_preflight(const SaveEntry *entry, NvramInfo *info);

/*
 * Writes `entry` into NVRAM, creating or replacing the file of that name,
 * then reads it back to check the contents, size and type. Call only after a
 * checked USB backup of NVRAM. Returns 0, or -1 with info->error set.
 */
int nvram_restore(const SaveEntry *entry, NvramInfo *info);
#endif
