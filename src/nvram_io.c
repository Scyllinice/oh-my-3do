/*
 * NVRAM access through the 3DO operating system. See nvram_io.h.
 *
 * A note on Portfolio's I/O, used throughout: files are opened as "Items"
 * (Portfolio's handles for system objects), and every operation on a file is
 * an I/O request: create an IOReq for the file, fill in an IOInfo with a
 * command (read, write, status...), an offset and a buffer, and call DoIO,
 * which waits for it to finish.
 */
#include "nvram_io.h"
#include "menu_ui.h"
#include "types.h"
#include "io.h"
#include "item.h"
#include "filesystem.h"
#include "filefunctions.h"
#include "directory.h"
#include "directoryfunctions.h"
#include "operror.h"
#include <stdlib.h>
#include <string.h>

/* NVRAM's total size; no save can be bigger. */
#define NVRAM_BYTES 32768UL

/* The largest block size the menu accepts from the file system. */
#define LARGEST_BLOCK 2048

/*
 * Extra room to allow for when a restore creates a new file, besides its
 * contents: the file system needs some space to record the file itself.
 * This is a cautious guess; the menu never deletes saves to make room.
 */
#define NEW_FILE_ALLOWANCE 128UL

/* Portfolio's "no such file" error, which also marks the end of a directory. */
#define NO_FILE_ERROR MakeFErr(ER_SEVERE, ER_C_NSTND, ER_Fs_NoFile)

/* Records an error in `info` and returns -1. */
static int failure(NvramInfo *info, const char *message, long error)
{
    info->error = message;
    info->last_error = error;
    return -1;
}

/* Writes "/NVRAM/" and the save's name into `path` (40 bytes). */
static void nvram_path(char *path, const char *name)
{
    strcpy(path, "/NVRAM/");
    strcat(path, name);
}

/* Rounds a byte count up to whole blocks. */
static unsigned long round_up_to_blocks(unsigned long bytes, unsigned long block_size)
{
    return ((bytes + block_size - 1) / block_size) * block_size;
}

/*
 * Runs one I/O command on an open file and waits for it. `writing` says
 * whether `buffer` is sent to the file or receives from it. Returns 0, or a
 * negative Portfolio error.
 */
static int do_io(Item request, int command, long offset, void *buffer, size_t size, int writing)
{
    IOInfo info;
    IOReq *finished;
    Err error;

    memset(&info, 0, sizeof(info));
    info.ioi_Command = (uint8)command;
    info.ioi_Offset = offset;
    if (writing) {
        info.ioi_Send.iob_Buffer = buffer;
        info.ioi_Send.iob_Len = size;
    } else {
        info.ioi_Recv.iob_Buffer = buffer;
        info.ioi_Recv.iob_Len = size;
    }

    error = DoIO(request, &info);
    finished = (IOReq *)LookupItem(request);
    if (error < 0)
        return (int)error;
    if (!finished)
        return -1;
    if (finished->io_Error < 0)
        return (int)finished->io_Error;

    /*
     * Only reads are checked for their length. NVRAM writes finish inside the
     * file system (through its "extend end of file" and "done" steps), which
     * never fill in io_Actual, so a successful write can report zero bytes.
     * nvram_restore checks writes by reading the file back instead.
     */
    if (command == CMD_READ && (size_t)finished->io_Actual != size)
        return -1;
    return 0;
}

/* Fills in `info` with NVRAM's free and used space. Returns 0, or -1. */
static int read_space(NvramInfo *info)
{
    const unsigned long wanted = FSSTAT_BLOCKSIZE | FSSTAT_FREE | FSSTAT_USED | FSSTAT_MAXFILESIZE;
    Item file, request;
    FileSystemStat stat;
    int result = -1;

    /* Status of the whole file system comes from opening its top folder. */
    file = OpenDiskFile("/NVRAM");
    if (file < 0)
        return failure(info, "NVRAM is unavailable", file);

    request = CreateIOReq(NULL, 0, file, 0);
    if (request >= 0) {
        memset(&stat, 0, sizeof(stat));
        result = do_io(request, FILECMD_FSSTAT, 0, &stat, sizeof(stat), 0);

        /* Every value must be reported, and plausible for 32 KiB of NVRAM:
         * a bad reading must not make a restore look like it fits. */
        if (result == 0 && (stat.fst_BitMap & wanted) == wanted && stat.fst_BlockSize &&
            stat.fst_BlockSize <= LARGEST_BLOCK &&
            stat.fst_Free <= SAVE_MAX_BYTES / stat.fst_BlockSize &&
            stat.fst_Used <= SAVE_MAX_BYTES / stat.fst_BlockSize &&
            stat.fst_MaxFileSize <= SAVE_MAX_BYTES / stat.fst_BlockSize) {
            info->free_bytes = stat.fst_Free * stat.fst_BlockSize;
            info->used_bytes = stat.fst_Used * stat.fst_BlockSize;
            info->max_file_bytes = stat.fst_MaxFileSize * stat.fst_BlockSize;
            info->block_size = stat.fst_BlockSize;
        } else {
            result = -1;
        }
        DeleteIOReq(request);
    }
    CloseDiskFile(file);
    return result < 0 ? failure(info, "Cannot read NVRAM free space", result) : 0;
}

/*
 * Reads the NVRAM file for `entry` into `out` (entry->size bytes). The file
 * must still be exactly that size. Returns 0, or a negative error.
 */
static int read_file(const SaveEntry *entry, unsigned char *out)
{
    char path[40];
    Item file, request;
    FileStatus stat;
    unsigned char *buffer = NULL;
    size_t rounded;
    int result = -1;

    nvram_path(path, entry->name);
    file = OpenDiskFile(path);
    if (file < 0)
        return (int)file;

    request = CreateIOReq(NULL, 0, file, 0);
    if (request < 0)
        goto done;
    memset(&stat, 0, sizeof(stat));
    if (do_io(request, CMD_STATUS, 0, &stat, sizeof(stat), 0) < 0 ||
        stat.fs_ByteCount != entry->size || !stat.fs.ds_DeviceBlockSize ||
        stat.fs.ds_DeviceBlockSize > LARGEST_BLOCK)
        goto done;

    /* Reads must be whole blocks, so read into a rounded-up buffer and copy
     * out just the file's bytes. */
    rounded = round_up_to_blocks(entry->size, stat.fs.ds_DeviceBlockSize);
    if (rounded) {
        buffer = (unsigned char *)malloc(rounded);
        if (!buffer || do_io(request, CMD_READ, 0, buffer, rounded, 0) < 0)
            goto done;
        memcpy(out, buffer, entry->size);
    }
    result = 0;

done:
    free(buffer);
    if (request >= 0)
        DeleteIOReq(request);
    CloseDiskFile(file);
    return result;
}

/* Sorts save files by name, ignoring case (an insertion sort: NVRAM holds
 * only a handful of files). */
static void sort_by_name(SaveEntry *entries, size_t count)
{
    size_t i;
    for (i = 1; i < count; ++i) {
        SaveEntry moving = entries[i];
        size_t j = i;
        while (j > 0 && menu_path_compare(entries[j - 1].name, moving.name) > 0) {
            entries[j] = entries[j - 1];
            --j;
        }
        entries[j] = moving;
    }
}

int nvram_snapshot(SaveArchive *archive, NvramInfo *info)
{
    Directory *directory;
    DirectoryEntry found;
    SaveEntry *entries;
    unsigned char *storage; /* every file's contents, one after another */
    size_t count = 0, used = 0;
    int result = -1;
    int32 end;

    memset(info, 0, sizeof(*info));
    if (read_space(info) < 0)
        return -1;

    directory = OpenDirectoryPath("/NVRAM");
    if (!directory)
        return failure(info, "Cannot open NVRAM directory", -1);
    entries = (SaveEntry *)calloc(SAVE_MAX_FILES, sizeof(*entries));
    storage = (unsigned char *)malloc(SAVE_MAX_BYTES);
    if (!entries || !storage)
        goto done;

    /* Read each file in the directory. Anything unexpected (a folder, a bad
     * name, too many or too big) stops the whole backup. */
    while ((end = ReadDirectory(directory, &found)) >= 0) {
        SaveEntry *entry;
        if (count == SAVE_MAX_FILES || !save_name_valid(found.de_FileName) ||
            (found.de_Flags & FILE_IS_DIRECTORY) || found.de_ByteCount > SAVE_MAX_BYTES - used)
            goto done;
        entry = &entries[count++];
        strcpy(entry->name, found.de_FileName);
        entry->type = found.de_Type;
        entry->size = found.de_ByteCount;
        entry->data = storage + used;
        if (read_file(entry, storage + used) < 0)
            goto done;
        used += entry->size;
    }

    /*
     * The directory ends with a "no such file" error. Any other error means
     * the listing stopped early, and an incomplete scan must not pass for a
     * complete backup.
     */
    if (end != NO_FILE_ERROR)
        goto done;

    sort_by_name(entries, count);
    result = save_archive_build(archive, entries, count);

done:
    free(entries);
    free(storage);
    CloseDirectory(directory);
    return result < 0 ? failure(info, "NVRAM scan failed; no backup made", -1) : 0;
}

int nvram_preflight(const SaveEntry *entry, NvramInfo *info)
{
    char path[40];
    Item file, request;
    FileStatus stat;
    unsigned long already_allocated = 0, needed, extra;
    int result;

    if (!save_name_valid(entry->name) || entry->size > NVRAM_BYTES)
        return failure(info, "Invalid save file", -1);
    if (read_space(info) < 0)
        return -1;

    /* If the file exists, the space it already has will be reused. */
    nvram_path(path, entry->name);
    file = OpenDiskFile(path);
    if (file >= 0) {
        request = CreateIOReq(NULL, 0, file, 0);
        result = request < 0 ? -1 : do_io(request, CMD_STATUS, 0, &stat, sizeof(stat), 0);
        if (request >= 0)
            DeleteIOReq(request);
        CloseDiskFile(file);
        if (result < 0 || stat.fs.ds_DeviceBlockSize != info->block_size ||
            stat.fs.ds_DeviceBlockCount > SAVE_MAX_BYTES / info->block_size)
            return failure(info, "Cannot check existing save", -1);
        already_allocated = stat.fs.ds_DeviceBlockCount * info->block_size;
    } else if (file != NO_FILE_ERROR) {
        return failure(info, "Cannot check existing save", file);
    }

    /* Only growth needs free space; a new file also needs the allowance. */
    needed = round_up_to_blocks(entry->size, info->block_size);
    if (needed > already_allocated) {
        extra = needed - already_allocated + (file < 0 ? NEW_FILE_ALLOWANCE : 0UL);
        if (extra > info->free_bytes || extra > info->max_file_bytes)
            return failure(info, "Not enough NVRAM space", -1);
    }
    return 0;
}

/* Whether a fresh directory listing shows `entry` with its name, type and size. */
static int listed_as_written(const SaveEntry *entry)
{
    Directory *directory = OpenDirectoryPath("/NVRAM");
    DirectoryEntry found;
    int listed = 0;

    if (!directory)
        return 0;
    while (ReadDirectory(directory, &found) >= 0) {
        if (!menu_path_compare(found.de_FileName, entry->name) && found.de_Type == entry->type &&
            found.de_ByteCount == entry->size)
            listed = 1;
    }
    CloseDirectory(directory);
    return listed;
}

int nvram_restore(const SaveEntry *entry, NvramInfo *info)
{
    char path[40];
    Item file, request = -1;
    FileStatus stat;
    unsigned char *buffer = NULL, *check = NULL;
    size_t rounded;
    unsigned long blocks_needed;
    int result = -1;

    if (nvram_preflight(entry, info) < 0)
        return -1;

    /* Open the save, creating it if it does not exist yet. */
    nvram_path(path, entry->name);
    file = OpenDiskFile(path);
    if (file < 0) {
        Item created = CreateFile(path);
        if (created < 0)
            goto done;
        file = OpenDiskFile(path);
        if (file < 0)
            goto done;
    }

    request = CreateIOReq(NULL, 0, file, 0);
    if (request < 0 || do_io(request, CMD_STATUS, 0, &stat, sizeof(stat), 0) < 0 ||
        stat.fs.ds_DeviceBlockSize != info->block_size)
        goto done;

    /* Writes are whole blocks too: copy the contents into a zero-padded buffer. */
    rounded = round_up_to_blocks(entry->size, info->block_size);
    buffer = (unsigned char *)calloc(rounded ? rounded : 1, 1);
    check = (unsigned char *)malloc(entry->size ? entry->size : 1);
    if (!buffer || !check)
        goto done;
    if (entry->size)
        memcpy(buffer, entry->data, entry->size);

    /* Grow the file to enough blocks, write, then set its exact size and type. */
    blocks_needed = rounded / info->block_size;
    if (blocks_needed > stat.fs.ds_DeviceBlockCount &&
        do_io(request, FILECMD_ALLOCBLOCKS, blocks_needed - stat.fs.ds_DeviceBlockCount, NULL, 0,
              0) < 0)
        goto done;
    if ((rounded && do_io(request, CMD_WRITE, 0, buffer, rounded, 1) < 0) ||
        do_io(request, FILECMD_SETEOF, entry->size, NULL, 0, 0) < 0 ||
        do_io(request, FILECMD_SETTYPE, (long)entry->type, NULL, 0, 0) < 0)
        goto done;

    /* Close it, then check through a fresh open: the contents... */
    DeleteIOReq(request);
    request = -1;
    CloseDiskFile(file);
    file = -1;
    if (read_file(entry, check) < 0 || (entry->size && memcmp(check, entry->data, entry->size)))
        goto done;
    /* ...and the type and size, through a fresh directory listing. */
    if (listed_as_written(entry))
        result = 0;

done:
    free(buffer);
    free(check);
    if (request >= 0)
        DeleteIOReq(request);
    if (file >= 0)
        CloseDiskFile(file);
    return result < 0 ? failure(info, "Restore failed. Keep recovery backup.", -1) : 0;
}
