#ifndef TEST_AUDIO_H
#define TEST_AUDIO_H
#include <stddef.h>
typedef struct TagArg {
    unsigned long ta_Tag;
    void *ta_Arg;
} TagArg;
#define TAG_END                 0
#define AF_TAG_FRAMES           1
#define AF_TAG_CHANNELS         2
#define AF_TAG_NUMBITS          3
#define AF_TAG_SAMPLE_RATE      4
#define AF_TAG_COMPRESSIONTYPE  5
#define AF_TAG_SUSTAINBEGIN     6
#define AF_TAG_SUSTAINEND       7
#define AF_TAG_AMPLITUDE        8
#define AF_TAG_ADDRESS          9
#define AF_TAG_COMPRESSIONRATIO 10
#define AF_TAG_RATE             11
long OpenAudioFolio(void);
long CloseAudioFolio(void);
long LoadSample(char *path);
long CreateSample(TagArg *tags);
long UnloadSample(long sample);
long LoadInstrument(char *name, int flags, int priority);
long UnloadInstrument(long item);
long GetAudioItemInfo(long sample, TagArg *tags);
long SetAudioItemInfo(long sample, TagArg *tags);
long ConnectInstruments(long from, char *output, long to, char *input);
long AttachSample(long instrument, long sample, char *hook);
long DetachSample(long attachment);
long StartInstrument(long item, TagArg *tags);
long StopInstrument(long item, TagArg *tags);
long PauseInstrument(long item);
long ResumeInstrument(long item);
#endif
