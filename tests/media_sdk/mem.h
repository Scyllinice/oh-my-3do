#ifndef OH3DO_TEST_MEDIA_MEM_H
#define OH3DO_TEST_MEDIA_MEM_H
typedef int int32;
typedef struct MemInfo { unsigned long minfo_SysFree,minfo_SysLargest,minfo_TaskFree,minfo_TaskLargest; } MemInfo;
#define MEMTYPE_DRAM 1UL
#define MEMTYPE_VRAM 0x00010000UL
void availMem(MemInfo *memory,unsigned long flags);
#define AvailMem availMem
#define MEMTYPE_CEL 0x00040000UL
#define MEMTYPE_AUDIO 0x00100000UL
void *AllocMem(int32 bytes,unsigned long flags);
void FreeMem(void *data,int32 bytes);
#endif
