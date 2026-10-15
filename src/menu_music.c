#include "menu_music.h"
#include "audio.h"
#include "string.h"

void menu_music_close(MenuMusic *music)
{
    if (music->player > 0) StopInstrument(music->player,NULL);
    if (music->output > 0) StopInstrument(music->output,NULL);
    if (music->attachment > 0) DetachSample(music->attachment);
    if (music->player > 0) UnloadInstrument(music->player);
    if (music->output > 0) UnloadInstrument(music->output);
    if (music->sample > 0) UnloadSample(music->sample);
    if (music->opened) CloseAudioFolio();
    memset(music,0,sizeof(*music));
}
int menu_music_set(MenuMusic *music, int enabled)
{
    TagArg tags[6];
    long frames;
    if (!enabled) { menu_music_close(music); return 0; }
    if (music->status) return music->status == 1 ? 0 : -1;
    if (OpenAudioFolio() < 0) goto failed;
    music->opened=1;
    music->sample=LoadSample("$boot/menu-music.aiff");
    if (music->sample < 0) goto failed;
    memset(tags,0,sizeof(tags));
    tags[0].ta_Tag=AF_TAG_FRAMES;
    tags[1].ta_Tag=AF_TAG_CHANNELS;
    tags[2].ta_Tag=AF_TAG_NUMBITS;
    tags[3].ta_Tag=AF_TAG_SAMPLE_RATE;
    tags[4].ta_Tag=AF_TAG_COMPRESSIONTYPE;
    if (GetAudioItemInfo(music->sample,tags) < 0) goto failed;
    frames=(long)tags[0].ta_Arg;
    if (frames < 4 || frames > 512L*1024 || (frames & 3) ||
        (long)tags[1].ta_Arg != 1 || (long)tags[2].ta_Arg != 8 ||
        (unsigned long)tags[3].ta_Arg != (22050UL<<16) || (long)tags[4].ta_Arg != 0) goto failed;
    memset(tags,0,sizeof(tags));
    tags[0].ta_Tag=AF_TAG_SUSTAINBEGIN; tags[0].ta_Arg=(void *)0;
    tags[1].ta_Tag=AF_TAG_SUSTAINEND; tags[1].ta_Arg=(void *)frames;
    if (SetAudioItemInfo(music->sample,tags) < 0) goto failed;
    music->output=LoadInstrument("directout.dsp",0,50);
    if (music->output < 0) goto failed;
    music->player=LoadInstrument("halfmono8.dsp",0,100);
    if (music->player < 0) goto failed;
    if (ConnectInstruments(music->player,"Output",music->output,"InputLeft") < 0 ||
        ConnectInstruments(music->player,"Output",music->output,"InputRight") < 0) goto failed;
    music->attachment=AttachSample(music->player,music->sample,0);
    if (music->attachment < 0) goto failed;
    if (StartInstrument(music->output,NULL) < 0) goto failed;
    memset(tags,0,sizeof(tags));
    tags[0].ta_Tag=AF_TAG_AMPLITUDE; tags[0].ta_Arg=(void *)0x4000;
    if (StartInstrument(music->player,tags) < 0) goto failed;
    music->status=1;
    return 0;
failed:
    menu_music_close(music); music->status=-1;
    return -1;
}
