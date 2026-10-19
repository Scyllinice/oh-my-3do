#include "nvram_io.h"
#include "sdk.h"
#include "menu_ui.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MISSING MakeFErr(ER_SEVERE, ER_C_NSTND, ER_Fs_NoFile)

typedef struct File { char name[32]; unsigned char data[32768]; size_t size, blocks; unsigned long type; } File;
static File files[8];
static int count, writes, creates, calls, fail_at, scan_error, short_io, allocate_fail, write_error, corrupt_write;
static IOReq req;
static Item current_file;
static unsigned long free_bytes = 16000, block_size = 1;
static int fault(void) { return ++calls == fail_at; }
Item OpenDiskFile(char *path)
{
    int i;
    if (!strcmp(path, "/NVRAM")) return 100;
    assert(!strncmp(path, "/NVRAM/", 7));
    for (i = 0; i < count; ++i) if (!menu_path_compare(path + 7, files[i].name)) return i + 1;
    return MISSING;
}
long CloseDiskFile(Item item) { assert(item > 0); return 0; }
Item CreateFile(char *path)
{
    File *f;
    assert(!strncmp(path, "/NVRAM/", 7) && count < 8); ++creates;
    f = &files[count++]; memset(f, 0, sizeof(*f)); strcpy(f->name, path + 7); return count;
}
Item CreateIOReq(char *name, int priority, Item file, int port)
{
    (void)name; (void)priority; (void)port;
    current_file = file; memset(&req, 0, sizeof(req)); return 200;
}
void DeleteIOReq(Item item) { assert(item == 200); }
void *LookupItem(Item item) { assert(item == 200); return &req; }
Err DoIO(Item request, IOInfo *info)
{
    File *f = current_file >= 1 && current_file <= count ? &files[current_file - 1] : NULL;
    size_t n;
    assert(request == 200);
    if (fault()) { req.io_Error = -55; return -55; }
    req.io_Error = 0; req.io_Actual = 0;
    if (info->ioi_Command == FILECMD_FSSTAT) {
        FileSystemStat *s = (FileSystemStat *)info->ioi_Recv.iob_Buffer;
        assert(current_file == 100); memset(s, 0, sizeof(*s));
        s->fst_BitMap = 2 | 8 | 16 | 32; s->fst_BlockSize = block_size;
        s->fst_Free = free_bytes / block_size; s->fst_MaxFileSize = free_bytes / block_size; s->fst_Used = 15000 / block_size;
    } else if (info->ioi_Command == CMD_STATUS) {
        FileStatus *s = (FileStatus *)info->ioi_Recv.iob_Buffer;
        assert(f); s->fs.ds_DeviceBlockSize = block_size;
        s->fs.ds_DeviceBlockCount = f->blocks / block_size; s->fs_ByteCount = f->size;
    } else if (info->ioi_Command == CMD_READ) {
        assert(f); n = info->ioi_Recv.iob_Len;
        assert(!info->ioi_Offset && n <= f->blocks);
        memcpy(info->ioi_Recv.iob_Buffer, f->data, n); req.io_Actual = short_io && n ? n - 1 : n;
    } else if (info->ioi_Command == CMD_WRITE) {
        assert(f); ++writes; n = info->ioi_Send.iob_Len; assert(n <= f->blocks);
        if (write_error) { req.io_Error = -77; return -77; }
        /* The real linked-memory driver leaves io_Actual at zero on writes. */
        memcpy(f->data, info->ioi_Send.iob_Buffer, n);
        if (corrupt_write && n) f->data[n - 1] ^= 1;
    } else if (info->ioi_Command == FILECMD_ALLOCBLOCKS) {
        assert(f); if (allocate_fail) return -66;
        assert(info->ioi_Offset >= 0); f->blocks += info->ioi_Offset * block_size;
    } else if (info->ioi_Command == FILECMD_SETEOF) {
        assert(f); assert((size_t)info->ioi_Offset <= f->blocks); f->size = info->ioi_Offset;
    } else if (info->ioi_Command == FILECMD_SETTYPE) { assert(f); f->type = info->ioi_Offset; }
    else assert(0);
    return 0;
}
Directory *OpenDirectoryPath(char *path)
{
    static Directory d;
    assert(!strcmp(path, "/NVRAM")); d.index = 0; return &d;
}
int32 ReadDirectory(Directory *dir, DirectoryEntry *entry)
{
    File *f;
    if (scan_error && dir->index) return -99;
    if (dir->index == count) return MISSING;
    f = &files[dir->index++]; memset(entry, 0, sizeof(*entry));
    strcpy(entry->de_FileName, f->name); entry->de_Type = f->type; entry->de_ByteCount = f->size;
    return 0;
}
void CloseDirectory(Directory *dir) { (void)dir; }
static void setup(void)
{
    memset(files, 0, sizeof(files)); count = 2;
    strcpy(files[0].name, "Existing"); files[0].size = files[0].blocks = 3; memcpy(files[0].data, "old", 3); files[0].type = 1;
    strcpy(files[1].name, "Other game"); files[1].size = files[1].blocks = 4; memcpy(files[1].data, "keep", 4); files[1].type = 2;
    calls = writes = creates = fail_at = scan_error = short_io = allocate_fail = write_error = corrupt_write = 0; free_bytes = 16000; block_size = 1;
}
int main(void)
{
    SaveArchive a;
    SaveEntry entry;
    NvramInfo info;
    int limit, i;
    memset(&a, 0, sizeof(a)); memset(&entry, 0, sizeof(entry)); memset(&info, 0, sizeof(info));
    setup(); assert(!nvram_snapshot(&a, &info)); assert(a.count == 2 && !writes && !creates);
    assert(!strcmp(a.entries[0].name, "Existing") && a.entries[0].size == 3 && !memcmp(a.entries[0].data, "old", 3));
    limit = calls;
    for (i = 1; i <= limit; ++i) { setup(); fail_at = i; assert(nvram_snapshot(&a, &info) < 0); assert(!writes && !creates); }
    setup(); scan_error = 1; assert(nvram_snapshot(&a, &info) < 0); assert(a.count == 2);
    setup(); short_io = 1; assert(nvram_snapshot(&a, &info) < 0);
    setup(); strcpy(entry.name, "Existing"); entry.type = 0x33444f46UL; entry.data = (const unsigned char *)"replacement"; entry.size = 11;
    assert(!nvram_preflight(&entry, &info)); assert(!writes && !creates);
    calls = 0;
    assert(!nvram_restore(&entry, &info)); assert(writes == 1 && !creates);
    assert(files[0].size == 11 && !memcmp(files[0].data, entry.data, 11) && files[0].type == entry.type);
    assert(files[1].size == 4 && !memcmp(files[1].data, "keep", 4) && files[1].type == 2);
    limit = calls;
    for (i = 1; i <= limit; ++i) {
        setup(); fail_at = i; assert(nvram_restore(&entry, &info) < 0);
        assert(files[1].size == 4 && !memcmp(files[1].data, "keep", 4) && files[1].type == 2);
    }
    setup(); write_error = 1; assert(nvram_restore(&entry, &info) < 0);
    assert(!memcmp(files[0].data, "old", 3));
    setup(); corrupt_write = 1; assert(nvram_restore(&entry, &info) < 0);
    setup(); short_io = 1; assert(nvram_restore(&entry, &info) < 0);
    setup(); block_size = 4; files[0].blocks = files[1].blocks = 4;
    assert(!nvram_restore(&entry, &info)); assert(files[0].blocks == 12 && files[0].size == 11);
    assert(!memcmp(files[0].data, entry.data, entry.size) && files[0].data[11] == 0);
    setup(); strcpy(entry.name, "EXISTING");
    assert(!nvram_restore(&entry, &info)); assert(!strcmp(files[0].name, "Existing"));
    strcpy(entry.name, "Existing");
    setup(); free_bytes = 0; assert(nvram_restore(&entry, &info) < 0); assert(!writes && !memcmp(files[0].data, "old", 3));
    setup(); allocate_fail = 1; assert(nvram_restore(&entry, &info) < 0); assert(!writes && !memcmp(files[0].data, "old", 3));
    setup(); strcpy(entry.name, "../Bad"); assert(nvram_restore(&entry, &info) < 0); assert(!writes && !creates);
    setup(); strcpy(entry.name, "New save"); assert(!nvram_restore(&entry, &info)); assert(creates == 1 && count == 3);
    assert(!memcmp(files[0].data, "old", 3) && !memcmp(files[1].data, "keep", 4));
    setup(); strcpy(entry.name, "Existing"); entry.size = 1;
    assert(!nvram_restore(&entry, &info)); assert(files[0].size == 1);
    setup(); entry.size = 0; assert(!nvram_restore(&entry, &info)); assert(!files[0].size);
    save_archive_free(&a);
    puts("NVRAM: actual backend scan, failures, capacity checks, create/replace, types and readback passed");
    return 0;
}
