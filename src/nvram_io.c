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

static int failure(NvramInfo *info, const char *message, long err)
{
    info->error = message; info->last_error = err; return -1;
}
static int io(Item request, int cmd, long offset, void *buffer, size_t size, int writing)
{
    IOInfo i;
    IOReq *r;
    Err err;
    memset(&i, 0, sizeof(i)); i.ioi_Command = (uint8)cmd; i.ioi_Offset = offset;
    if (writing) { i.ioi_Send.iob_Buffer = buffer; i.ioi_Send.iob_Len = size; }
    else { i.ioi_Recv.iob_Buffer = buffer; i.ioi_Recv.iob_Len = size; }
    err = DoIO(request, &i); r = (IOReq *)LookupItem(request);
    if (err < 0 || !r || r->io_Error < 0) return err < 0 ? (int)err : (r ? (int)r->io_Error : -1);
    if ((cmd == CMD_READ || cmd == CMD_WRITE) && (size_t)r->io_Actual != size) return -1;
    return 0;
}
static int space(NvramInfo *info)
{
    Item file, request;
    FileSystemStat stat;
    int result = -1;
    file = OpenDiskFile("/NVRAM");
    if (file < 0) return failure(info, "NVRAM is unavailable", file);
    request = CreateIOReq(NULL, 0, file, 0);
    if (request >= 0) {
        memset(&stat, 0, sizeof(stat));
        result = io(request, FILECMD_FSSTAT, 0, &stat, sizeof(stat), 0);
        if (!result && (stat.fst_BitMap & (FSSTAT_BLOCKSIZE | FSSTAT_FREE | FSSTAT_USED | FSSTAT_MAXFILESIZE)) ==
                (FSSTAT_BLOCKSIZE | FSSTAT_FREE | FSSTAT_USED | FSSTAT_MAXFILESIZE) &&
                stat.fst_BlockSize && stat.fst_BlockSize <= 2048 &&
                stat.fst_Free <= SAVE_MAX_BYTES / stat.fst_BlockSize &&
                stat.fst_Used <= SAVE_MAX_BYTES / stat.fst_BlockSize &&
                stat.fst_MaxFileSize <= SAVE_MAX_BYTES / stat.fst_BlockSize) {
            info->free_bytes = stat.fst_Free * stat.fst_BlockSize;
            info->used_bytes = stat.fst_Used * stat.fst_BlockSize;
            info->max_file_bytes = stat.fst_MaxFileSize * stat.fst_BlockSize;
            info->block_size = stat.fst_BlockSize;
        } else result = -1;
        DeleteIOReq(request);
    }
    CloseDiskFile(file);
    return result < 0 ? failure(info, "Cannot read NVRAM free space", result) : 0;
}
static int file_read(const SaveEntry *entry, unsigned char *out)
{
    char path[40];
    Item file, request;
    FileStatus stat;
    unsigned char *buffer = NULL;
    size_t rounded;
    int result = -1;
    strcpy(path, "/NVRAM/"); strcat(path, entry->name);
    file = OpenDiskFile(path);
    if (file < 0) return (int)file;
    request = CreateIOReq(NULL, 0, file, 0);
    if (request < 0) goto done;
    memset(&stat, 0, sizeof(stat));
    if (io(request, CMD_STATUS, 0, &stat, sizeof(stat), 0) < 0 || stat.fs_ByteCount != entry->size ||
        !stat.fs.ds_DeviceBlockSize || stat.fs.ds_DeviceBlockSize > 2048) goto done;
    rounded = ((entry->size + stat.fs.ds_DeviceBlockSize - 1) / stat.fs.ds_DeviceBlockSize) * stat.fs.ds_DeviceBlockSize;
    if (rounded) {
        buffer = (unsigned char *)malloc(rounded);
        if (!buffer || io(request, CMD_READ, 0, buffer, rounded, 0) < 0) goto done;
        memcpy(out, buffer, entry->size);
    }
    result = 0;
done:
    free(buffer); if (request >= 0) DeleteIOReq(request); CloseDiskFile(file); return result;
}
int nvram_snapshot(SaveArchive *archive, NvramInfo *info)
{
    Directory *dir;
    DirectoryEntry de;
    SaveEntry *entries;
    unsigned char *storage;
    size_t count = 0, used = 0, i;
    int result = -1;
    int32 end;
    memset(info, 0, sizeof(*info));
    if (space(info) < 0) return -1;
    dir = OpenDirectoryPath("/NVRAM");
    if (!dir) return failure(info, "Cannot open NVRAM directory", -1);
    entries = (SaveEntry *)calloc(SAVE_MAX_FILES, sizeof(*entries));
    storage = (unsigned char *)malloc(SAVE_MAX_BYTES);
    if (!entries || !storage) goto done;
    while ((end = ReadDirectory(dir, &de)) >= 0) {
        SaveEntry *e;
        if (count == SAVE_MAX_FILES || !save_name_valid(de.de_FileName) || (de.de_Flags & FILE_IS_DIRECTORY) ||
            de.de_ByteCount > SAVE_MAX_BYTES - used) goto done;
        e = &entries[count++]; strcpy(e->name, de.de_FileName);
        e->type = de.de_Type; e->size = de.de_ByteCount; e->data = storage + used;
        if (file_read(e, storage + used) < 0) goto done;
        used += e->size;
    }
    /* NoFile is the linked-memory directory's normal end marker. Other errors
     * must not turn an incomplete scan into a supposedly complete backup. */
    if (end != MakeFErr(ER_SEVERE, ER_C_NSTND, ER_Fs_NoFile)) goto done;
    for (i = 1; i < count; ++i) {
        SaveEntry e = entries[i]; size_t j = i;
        while (j && menu_path_compare(entries[j - 1].name, e.name) > 0) {
            entries[j] = entries[j - 1]; --j;
        }
        entries[j] = e;
    }
    result = save_archive_build(archive, entries, count);
done:
    free(entries); free(storage); CloseDirectory(dir);
    return result < 0 ? failure(info, "NVRAM scan failed; no backup made", -1) : 0;
}
int nvram_preflight(const SaveEntry *entry, NvramInfo *info)
{
    char path[40];
    Item file, request;
    FileStatus stat;
    unsigned long allocated = 0, needed;
    int result;
    if (!save_name_valid(entry->name) || entry->size > 32768UL) return failure(info, "Invalid save file", -1);
    if (space(info) < 0) return -1;
    strcpy(path, "/NVRAM/"); strcat(path, entry->name);
    file = OpenDiskFile(path);
    if (file >= 0) {
        request = CreateIOReq(NULL, 0, file, 0);
        result = request < 0 ? -1 : io(request, CMD_STATUS, 0, &stat, sizeof(stat), 0);
        if (request >= 0) DeleteIOReq(request);
        CloseDiskFile(file);
        if (result < 0 || stat.fs.ds_DeviceBlockSize != info->block_size ||
            stat.fs.ds_DeviceBlockCount > SAVE_MAX_BYTES / info->block_size)
            return failure(info, "Cannot check existing save", -1);
        allocated = stat.fs.ds_DeviceBlockCount * info->block_size;
    } else if (file != MakeFErr(ER_SEVERE, ER_C_NSTND, ER_Fs_NoFile))
        return failure(info, "Cannot check existing save", file);
    needed = ((entry->size + info->block_size - 1) / info->block_size) * info->block_size;
    /* Conservative metadata allowance for a new file. No delete-to-make-room. */
    if (needed > allocated && (needed - allocated + (file < 0 ? 128UL : 0UL) > info->free_bytes ||
        needed - allocated + (file < 0 ? 128UL : 0UL) > info->max_file_bytes))
        return failure(info, "Not enough NVRAM space", -1);
    return 0;
}
int nvram_restore(const SaveEntry *entry, NvramInfo *info)
{
    char path[40];
    Item file, request = -1;
    FileStatus stat;
    unsigned char *buffer = NULL, *check = NULL;
    size_t rounded;
    int result = -1;
    if (nvram_preflight(entry, info) < 0) return -1;
    strcpy(path, "/NVRAM/"); strcat(path, entry->name);
    file = OpenDiskFile(path);
    if (file < 0) {
        Item created = CreateFile(path);
        if (created < 0) goto done;
        file = OpenDiskFile(path);
        if (file < 0) goto done;
    }
    request = CreateIOReq(NULL, 0, file, 0);
    if (request < 0 || io(request, CMD_STATUS, 0, &stat, sizeof(stat), 0) < 0 ||
        stat.fs.ds_DeviceBlockSize != info->block_size) goto done;
    rounded = ((entry->size + info->block_size - 1) / info->block_size) * info->block_size;
    buffer = (unsigned char *)calloc(rounded ? rounded : 1, 1);
    check = (unsigned char *)malloc(entry->size ? entry->size : 1);
    if (!buffer || !check) goto done;
    if (entry->size) memcpy(buffer, entry->data, entry->size);
    if (rounded / info->block_size > stat.fs.ds_DeviceBlockCount &&
        io(request, FILECMD_ALLOCBLOCKS, rounded / info->block_size - stat.fs.ds_DeviceBlockCount, NULL, 0, 0) < 0) goto done;
    if ((rounded && io(request, CMD_WRITE, 0, buffer, rounded, 1) < 0) ||
        io(request, FILECMD_SETEOF, entry->size, NULL, 0, 0) < 0 ||
        io(request, FILECMD_SETTYPE, (long)entry->type, NULL, 0, 0) < 0) goto done;
    DeleteIOReq(request); request = -1;
    CloseDiskFile(file); file = -1;
    if (file_read(entry, check) < 0 || (entry->size && memcmp(check, entry->data, entry->size))) goto done;
    /* Verify the file type too, through a new directory scan. */
    {
        Directory *dir = OpenDirectoryPath("/NVRAM");
        DirectoryEntry de;
        if (!dir) goto done;
        while (ReadDirectory(dir, &de) >= 0)
            if (!menu_path_compare(de.de_FileName, entry->name) && de.de_Type == entry->type && de.de_ByteCount == entry->size) result = 0;
        CloseDirectory(dir);
    }
done:
    free(buffer); free(check);
    if (request >= 0) DeleteIOReq(request);
    if (file >= 0) CloseDiskFile(file);
    return result < 0 ? failure(info, "Restore failed. Keep recovery backup.", -1) : 0;
}
