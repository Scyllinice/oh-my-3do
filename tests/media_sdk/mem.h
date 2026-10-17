#ifndef OH3DO_TEST_MEDIA_MEM_H
#define OH3DO_TEST_MEDIA_MEM_H
typedef int int32;
#define MEMTYPE_CEL 0x00040000UL
#define MEMTYPE_AUDIO 0x00100000UL
void *AllocMem(int32 bytes,unsigned long flags);
void FreeMem(void *data,int32 bytes);
#endif
