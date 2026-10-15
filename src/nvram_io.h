#ifndef OH_MY_3DO_NVRAM_IO_H
#define OH_MY_3DO_NVRAM_IO_H
#include "save_archive.h"
typedef struct NvramInfo {
    unsigned long free_bytes;
    unsigned long used_bytes;
    unsigned long max_file_bytes;
    unsigned long block_size;
    long last_error;
    const char *error;
} NvramInfo;
int nvram_snapshot(SaveArchive *archive, NvramInfo *info);
/* Preflight never changes NVRAM; write must follow a verified USB recovery. */
int nvram_preflight(const SaveEntry *entry, NvramInfo *info);
int nvram_restore(const SaveEntry *entry, NvramInfo *info);
#endif
