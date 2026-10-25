#ifndef TEST_NVRAM_SDK_H
#define TEST_NVRAM_SDK_H
#include <stddef.h>
typedef long Item;
typedef long Err;
typedef unsigned char uint8;
typedef unsigned long uint32;
typedef long int32;
typedef long s32;
typedef struct {
    void *iob_Buffer;
    long iob_Len;
} IOBuf;
typedef struct {
    unsigned char ioi_Command, ioi_Unit;
    long ioi_Offset;
    IOBuf ioi_Send, ioi_Recv;
} IOInfo;
typedef struct {
    long io_Error, io_Actual;
} IOReq;
typedef struct {
    unsigned long ds_DeviceBlockSize, ds_DeviceBlockCount;
} DeviceStatus;
typedef struct {
    DeviceStatus fs;
    unsigned long fs_ByteCount;
} FileStatus;
typedef struct {
    unsigned long fst_BitMap, fst_CreateTime, fst_BlockSize, fst_Size, fst_MaxFileSize, fst_Free,
        fst_Used;
} FileSystemStat;
typedef struct {
    unsigned long de_Flags, de_Type, de_ByteCount;
    char de_FileName[32];
} DirectoryEntry;
typedef struct {
    int index;
} Directory;
#define CMD_WRITE           1
#define CMD_READ            2
#define CMD_STATUS          3
#define FILECMD_ALLOCBLOCKS 6
#define FILECMD_SETEOF      7
#define FILECMD_SETTYPE     10
#define FILECMD_FSSTAT      12
#define FILE_IS_DIRECTORY   1
#define FSSTAT_BLOCKSIZE    2
#define FSSTAT_MAXFILESIZE  8
#define FSSTAT_FREE         16
#define FSSTAT_USED         32
#define ER_SEVERE           2
#define ER_C_NSTND          1
#define ER_Fs_NoFile        1
#define MakeFErr(s, c, e)   (-1000L - (e))
Item OpenDiskFile(char *path);
long CloseDiskFile(Item item);
Item CreateFile(char *path);
Item CreateIOReq(char *name, int priority, Item file, int port);
void DeleteIOReq(Item item);
void *LookupItem(Item item);
Err DoIO(Item request, IOInfo *info);
Directory *OpenDirectoryPath(char *path);
int32 ReadDirectory(Directory *dir, DirectoryEntry *entry);
void CloseDirectory(Directory *dir);
Item FindAndOpenDevice(char *name);
long CloseNamedDevice(Item item);
Item GetTimerIOReq(void);
long GetMSecTime(Item item);
Err SendIO(Item request, IOInfo *info);
int CheckIO(Item request);
Err WaitIO(Item request);
Err AbortIO(Item request);
void SleepMSec(Item item, long ms);
#endif
