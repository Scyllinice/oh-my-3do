#ifndef TEST_AUDIO_MEM_H
#define TEST_AUDIO_MEM_H
typedef long int32;
#define MEMTYPE_AUDIO 0x00100000UL
void *AllocMem(long bytes, unsigned long flags);
void FreeMem(void *data, long bytes);
#endif
