#include "menu_music.h"
#include "audio.h"
#include "mem.h"
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int calls, fail_at, opened, resources, wrong_format, custom;
static int fault(void);
static unsigned char *memory;
static long frames=1207640, rate=11025;
void *AllocMem(long bytes,unsigned long flags) {
    assert(!memory && flags==MEMTYPE_AUDIO && bytes==frames/2);
    if (fault()) return NULL;
    memory=(unsigned char *)malloc((size_t)bytes); return memory;
}
void FreeMem(void *data,long bytes) { assert(data==memory && bytes==frames/2 && !resources); free(memory); memory=NULL; }
static int fault(void) { return ++calls == fail_at; }
long OpenAudioFolio(void) { if (fault()) return -1; assert(!opened); opened=1; return 0; }
long CloseAudioFolio(void) { assert(opened && !resources); opened=0; return 0; }
long LoadSample(char *path) {
    assert(opened && !strcmp(path,"$boot/menu-music.aiff"));
    if (fault()) return -1;
    resources|=2; return 1;
}
long UnloadSample(long sample) { assert(sample==1 && (resources&2) && !(resources&16)); resources&=~2; return 0; }
long CreateSample(TagArg *tags) {
    assert(opened && memory && tags[0].ta_Tag==AF_TAG_ADDRESS && tags[0].ta_Arg==memory);
    assert(tags[1].ta_Tag==AF_TAG_FRAMES && (long)tags[1].ta_Arg==frames);
    assert(tags[2].ta_Tag==AF_TAG_NUMBITS && (long)tags[2].ta_Arg==16);
    assert(tags[3].ta_Tag==AF_TAG_CHANNELS && (long)tags[3].ta_Arg==1);
    assert(tags[4].ta_Tag==AF_TAG_SAMPLE_RATE && (long)tags[4].ta_Arg==(rate<<16));
    assert(tags[5].ta_Tag==AF_TAG_COMPRESSIONTYPE && (long)tags[5].ta_Arg==0x41445034L);
    assert(tags[6].ta_Tag==AF_TAG_COMPRESSIONRATIO && (long)tags[6].ta_Arg==4 && !tags[7].ta_Tag);
    if (fault()) return -1;
    resources|=2; return 1;
}
long LoadInstrument(char *name,int flags,int priority) {
    int item=!strcmp(name,"directout.dsp") ? 2 : (!strcmp(name,"decodeadpcm.dsp") ? 5 : 3);
    (void)flags; (void)priority; assert(item==2 || item==5 || !strcmp(name,custom ? "adpcmvarmono.dsp" : "halfmono8.dsp"));
    if (fault()) return -1;
    resources|=1<<item; return item;
}
long UnloadInstrument(long item) { assert((resources&(1<<item)) && !(resources&16)); resources&=~(1<<item); return 0; }
long GetAudioItemInfo(long sample,TagArg *tags) {
    assert(sample==1);
    if (fault()) return -1;
    assert(tags[0].ta_Tag==AF_TAG_FRAMES && tags[4].ta_Tag==AF_TAG_COMPRESSIONTYPE && !tags[5].ta_Tag);
    tags[0].ta_Arg=(void *)(custom ? frames : 441000L); tags[1].ta_Arg=(void *)(wrong_format ? 2L : 1L);
    tags[2].ta_Arg=(void *)(custom ? 16L : 8L); tags[3].ta_Arg=(void *)((custom ? (unsigned long)rate : 22050UL)<<16); tags[4].ta_Arg=(void *)(custom ? 0x41445034L : 0); return 0;
}
long SetAudioItemInfo(long sample,TagArg *tags) {
    assert(sample==1 && tags[0].ta_Tag==AF_TAG_SUSTAINBEGIN && !tags[0].ta_Arg);
    assert(tags[1].ta_Tag==AF_TAG_SUSTAINEND && (long)tags[1].ta_Arg==(custom ? frames : 441000L) && !tags[2].ta_Tag);
    return fault() ? -1 : 0;
}
long ConnectInstruments(long from,char *output,long to,char *input) {
    assert(from==3 && to==2 && !strcmp(output,"Output"));
    assert(!strcmp(input,"InputLeft") || !strcmp(input,"InputRight"));
    return fault() ? -1 : 0;
}
long AttachSample(long instrument,long sample,char *hook) {
    assert(instrument==3 && sample==1 && !hook);
    if (fault()) return -1;
    resources|=16; return 4;
}
long DetachSample(long attachment) { assert(attachment==4 && (resources&16)); resources&=~16; return 0; }
long StartInstrument(long item,TagArg *tags) {
    assert(item==2 || item==3);
    if (item==3) {
        assert(tags && tags[0].ta_Tag==AF_TAG_AMPLITUDE && (long)tags[0].ta_Arg==0x4000);
        if (custom) assert(tags[1].ta_Tag==AF_TAG_RATE && (long)tags[1].ta_Arg==rate*32768L/44100L && !tags[2].ta_Tag);
        else assert(!tags[1].ta_Tag);
    }
    return fault() ? -1 : 0;
}
long StopInstrument(long item,TagArg *tags) { assert(resources&(1<<item)); (void)tags; return 0; }
int main(void)
{
    MenuMusic music;
    int count,i;
    memset(&music,0,sizeof(music));
    assert(!menu_music_set(&music,0) && !calls);
    assert(!menu_music_set(&music,1) && music.status==1 && resources==30 && opened);
    count=calls;
    assert(!menu_music_set(&music,1) && calls==count);
    menu_music_close(&music); menu_music_close(&music);
    assert(!opened && !resources && !music.status);
    for (i=1;i<=count;++i) {
        calls=0; fail_at=i;
        assert(menu_music_set(&music,1)<0 && music.status==-1 && !resources && !opened);
        assert(menu_music_set(&music,1)<0 && calls==i);
        assert(!menu_music_set(&music,0) && !music.status);
    }
    calls=fail_at=0; wrong_format=1;
    assert(menu_music_set(&music,1)<0 && !resources && !opened);
    menu_music_close(&music); wrong_format=0;
    assert(!menu_music_set(&music,1));
    assert(!menu_music_set(&music,0) && !opened && !resources);
    custom=1; calls=fail_at=0;
    assert(menu_music_buffer(&music,(unsigned long)frames,(unsigned long)rate));
    assert(!menu_music_set(&music,1) && music.source==2 && resources==62);
    count=calls; menu_music_close(&music);
    assert(!resources && !memory && !opened);
    for (i=1;i<=count;++i) {
        calls=0; fail_at=i;
        if (menu_music_buffer(&music,(unsigned long)frames,(unsigned long)rate))
            assert(menu_music_set(&music,1)<0);
        assert(!resources && !memory && !opened);
        menu_music_close(&music);
    }
    calls=fail_at=0; rate=22050;
    assert(menu_music_buffer(&music,(unsigned long)frames,(unsigned long)rate));
    assert(!menu_music_set(&music,1)); menu_music_close(&music);
    assert(!menu_music_buffer(&music,1572872,11025));
    assert(!menu_music_buffer(&music,9,11025));
    assert(!menu_music_buffer(&music,frames,44100));
    puts("Menu music: looping tags, stereo routing, live toggle, every startup failure and cleanup passed");
    return 0;
}
