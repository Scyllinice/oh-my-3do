#include "menu_music.h"
#include "audio.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int calls, fail_at, opened, resources, wrong_format;
static int fault(void) { return ++calls == fail_at; }
long OpenAudioFolio(void) { if (fault()) return -1; assert(!opened); opened=1; return 0; }
long CloseAudioFolio(void) { assert(opened && !resources); opened=0; return 0; }
long LoadSample(char *path) {
    assert(opened && !strcmp(path,"$boot/menu-music.aiff"));
    if (fault()) return -1;
    resources|=2; return 1;
}
long UnloadSample(long sample) { assert(sample==1 && (resources&2) && !(resources&16)); resources&=~2; return 0; }
long LoadInstrument(char *name,int flags,int priority) {
    int item=!strcmp(name,"directout.dsp") ? 2 : 3;
    (void)flags; (void)priority; assert(item==2 || !strcmp(name,"halfmono8.dsp"));
    if (fault()) return -1;
    resources|=1<<item; return item;
}
long UnloadInstrument(long item) { assert((resources&(1<<item)) && !(resources&16)); resources&=~(1<<item); return 0; }
long GetAudioItemInfo(long sample,TagArg *tags) {
    assert(sample==1);
    if (fault()) return -1;
    assert(tags[0].ta_Tag==AF_TAG_FRAMES && tags[4].ta_Tag==AF_TAG_COMPRESSIONTYPE && !tags[5].ta_Tag);
    tags[0].ta_Arg=(void *)441000L; tags[1].ta_Arg=(void *)(wrong_format ? 2L : 1L);
    tags[2].ta_Arg=(void *)8; tags[3].ta_Arg=(void *)(22050UL<<16); tags[4].ta_Arg=0; return 0;
}
long SetAudioItemInfo(long sample,TagArg *tags) {
    assert(sample==1 && tags[0].ta_Tag==AF_TAG_SUSTAINBEGIN && !tags[0].ta_Arg);
    assert(tags[1].ta_Tag==AF_TAG_SUSTAINEND && (long)tags[1].ta_Arg==441000L && !tags[2].ta_Tag);
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
    if (item==3) assert(tags && tags[0].ta_Tag==AF_TAG_AMPLITUDE && (long)tags[0].ta_Arg==0x4000 && !tags[1].ta_Tag);
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
    puts("Menu music: looping tags, stereo routing, live toggle, every startup failure and cleanup passed");
    return 0;
}
