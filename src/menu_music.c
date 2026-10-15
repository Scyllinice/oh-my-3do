#include "menu_music.h"
#include "audio.h"
#include "mem.h"
#include "string.h"

void menu_music_close(MenuMusic *music)
{
    if (music->player > 0) StopInstrument(music->player,NULL);
    if (music->output > 0) StopInstrument(music->output,NULL);
    if (music->attachment > 0) DetachSample(music->attachment);
    if (music->player > 0) UnloadInstrument(music->player);
    if (music->output > 0) UnloadInstrument(music->output);
    if (music->decoder > 0) UnloadInstrument(music->decoder);
    if (music->sample > 0) UnloadSample(music->sample);
    if (music->data) FreeMem(music->data,(int32)music->bytes);
    if (music->opened) CloseAudioFolio();
    memset(music,0,sizeof(*music));
}
unsigned char *menu_music_buffer(MenuMusic *music,unsigned long frames,unsigned long rate)
{
    if (frames<8 || frames>768UL*1024UL*2 || (frames&7) || (rate!=11025 && rate!=22050)) return NULL;
    menu_music_close(music);
    music->data=(unsigned char *)AllocMem((int32)(frames/2),MEMTYPE_AUDIO);
    if (music->data) { music->frames=frames; music->bytes=frames/2; music->rate=rate; }
    return music->data;
}
int menu_music_set(MenuMusic *music, int enabled)
{
    TagArg tags[8];
    long frames;
    if (!enabled) { menu_music_close(music); return 0; }
    if (music->status) return music->status == 1 ? 0 : -1;
    if (OpenAudioFolio() < 0) goto failed;
    music->opened=1;
    if (music->data) {
        memset(tags,0,sizeof(tags));
        tags[0].ta_Tag=AF_TAG_ADDRESS; tags[0].ta_Arg=music->data;
        tags[1].ta_Tag=AF_TAG_FRAMES; tags[1].ta_Arg=(void *)music->frames;
        tags[2].ta_Tag=AF_TAG_NUMBITS; tags[2].ta_Arg=(void *)16;
        tags[3].ta_Tag=AF_TAG_CHANNELS; tags[3].ta_Arg=(void *)1;
        tags[4].ta_Tag=AF_TAG_SAMPLE_RATE; tags[4].ta_Arg=(void *)(music->rate<<16);
        tags[5].ta_Tag=AF_TAG_COMPRESSIONTYPE; tags[5].ta_Arg=(void *)0x41445034UL;
        tags[6].ta_Tag=AF_TAG_COMPRESSIONRATIO; tags[6].ta_Arg=(void *)4;
        music->sample=CreateSample(tags);
        music->source=2;
    } else {
        music->sample=LoadSample("$boot/menu-music.aiff");
        music->source=1;
    }
    if (music->sample < 0) goto failed;
    memset(tags,0,sizeof(tags));
    tags[0].ta_Tag=AF_TAG_FRAMES;
    tags[1].ta_Tag=AF_TAG_CHANNELS;
    tags[2].ta_Tag=AF_TAG_NUMBITS;
    tags[3].ta_Tag=AF_TAG_SAMPLE_RATE;
    tags[4].ta_Tag=AF_TAG_COMPRESSIONTYPE;
    if (GetAudioItemInfo(music->sample,tags) < 0) goto failed;
    frames=(long)tags[0].ta_Arg;
    if ((long)tags[1].ta_Arg!=1) goto failed;
    if (music->data) {
        if ((unsigned long)frames!=music->frames || (long)tags[2].ta_Arg!=16 ||
            (unsigned long)tags[3].ta_Arg!=(music->rate<<16) || (long)tags[4].ta_Arg!=0x41445034L) goto failed;
    } else if (frames<4 || frames>512L*1024 || (frames&3) || (long)tags[2].ta_Arg!=8 ||
               (unsigned long)tags[3].ta_Arg!=(22050UL<<16) || (long)tags[4].ta_Arg) goto failed;
    memset(tags,0,sizeof(tags));
    tags[0].ta_Tag=AF_TAG_SUSTAINBEGIN; tags[0].ta_Arg=(void *)0;
    tags[1].ta_Tag=AF_TAG_SUSTAINEND; tags[1].ta_Arg=(void *)frames;
    if (SetAudioItemInfo(music->sample,tags) < 0) goto failed;
    music->output=LoadInstrument("directout.dsp",0,50);
    if (music->output < 0) goto failed;
    if (music->data) {
        music->decoder=LoadInstrument("decodeadpcm.dsp",0,0);
        if (music->decoder<0) goto failed;
    }
    music->player=LoadInstrument(music->data ? "adpcmvarmono.dsp" : "halfmono8.dsp",0,100);
    if (music->player < 0) goto failed;
    if (ConnectInstruments(music->player,"Output",music->output,"InputLeft") < 0 ||
        ConnectInstruments(music->player,"Output",music->output,"InputRight") < 0) goto failed;
    music->attachment=AttachSample(music->player,music->sample,0);
    if (music->attachment < 0) goto failed;
    if (StartInstrument(music->output,NULL) < 0) goto failed;
    memset(tags,0,sizeof(tags));
    tags[0].ta_Tag=AF_TAG_AMPLITUDE; tags[0].ta_Arg=(void *)0x4000;
    if (music->data) { tags[1].ta_Tag=AF_TAG_RATE; tags[1].ta_Arg=(void *)(music->rate*32768UL/44100UL); }
    if (StartInstrument(music->player,tags) < 0) goto failed;
    music->status=1;
    return 0;
failed:
    menu_music_close(music); music->status=-1;
    return -1;
}
