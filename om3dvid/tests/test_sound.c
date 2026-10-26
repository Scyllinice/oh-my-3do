/* The clip sound player (om3dvid7_sound.c) against a stand-in audio folio:
 * every call it makes, every failure, and the player adapters. */
#include "om3dvid7_sound.h"
#include "audio.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int calls, fail_at, opened, resources, ring, paused;
static unsigned char pcm[32];
static int fault(void)
{
    return ++calls == fail_at;
}
long OpenAudioFolio(void)
{
    if (fault())
        return -1;
    assert(!opened);
    opened = 1;
    return 0;
}
long CloseAudioFolio(void)
{
    assert(opened && !resources);
    opened = 0;
    return 0;
}
long CreateSample(TagArg *tags)
{
    assert(opened && tags[0].ta_Tag == AF_TAG_ADDRESS && tags[0].ta_Arg == pcm);
    assert(tags[1].ta_Tag == AF_TAG_FRAMES && (long)tags[1].ta_Arg == 32);
    assert(tags[2].ta_Tag == AF_TAG_NUMBITS && (long)tags[2].ta_Arg == 8);
    assert(tags[3].ta_Tag == AF_TAG_CHANNELS && (long)tags[3].ta_Arg == 1);
    assert(tags[4].ta_Tag == AF_TAG_SAMPLE_RATE && (long)tags[4].ta_Arg == (11025L << 16));
    /* A ring sustains over the whole sample, so it repeats until stopped. */
    if (ring)
        assert(tags[5].ta_Tag == AF_TAG_SUSTAINBEGIN && !tags[5].ta_Arg &&
               tags[6].ta_Tag == AF_TAG_SUSTAINEND && (long)tags[6].ta_Arg == 32 &&
               !tags[7].ta_Tag);
    else
        assert(!tags[5].ta_Tag);
    if (fault())
        return -1;
    resources |= 2;
    return 1;
}
long UnloadSample(long item)
{
    assert(item == 1 && (resources & 2) && !(resources & 16));
    resources &= ~2;
    return 0;
}
long LoadInstrument(char *name, int flags, int priority)
{
    int item = !strcmp(name, "directout.dsp") ? 2 : 3;
    (void)flags;
    (void)priority;
    assert(opened && (item == 2 || !strcmp(name, "varmono8.dsp")));
    if (fault())
        return -1;
    resources |= 1 << item;
    return item;
}
long UnloadInstrument(long item)
{
    assert((resources & (1 << item)) && !(resources & 16));
    resources &= ~(1 << item);
    return 0;
}
long ConnectInstruments(long from, char *out, long to, char *in)
{
    assert(from == 3 && to == 2 && !strcmp(out, "Output") &&
           (!strcmp(in, "InputLeft") || !strcmp(in, "InputRight")));
    return fault() ? -1 : 0;
}
long AttachSample(long instrument, long sample, char *hook)
{
    assert(instrument == 3 && sample == 1 && !hook);
    if (fault())
        return -1;
    resources |= 16;
    return 4;
}
long DetachSample(long item)
{
    assert(item == 4 && (resources & 16));
    resources &= ~16;
    return 0;
}
long PauseInstrument(long item)
{
    assert(item == 3 && !paused);
    paused = 1;
    return 0;
}
long ResumeInstrument(long item)
{
    assert(item == 3 && paused);
    paused = 0;
    return 0;
}
long StopInstrument(long item, TagArg *tags)
{
    assert((item == 2 || item == 3) && !tags);
    return fault() ? -1 : 0;
}
long StartInstrument(long item, TagArg *tags)
{
    assert(item == 2 || item == 3);
    if (item == 3) {
        assert(tags[0].ta_Tag == AF_TAG_AMPLITUDE && (long)tags[0].ta_Arg == 0x6000);
        assert(tags[1].ta_Tag == AF_TAG_RATE && (long)tags[1].ta_Arg == 8192 && !tags[2].ta_Tag);
    }
    return fault() ? -1 : 0;
}
int main(void)
{
    Om3dvid7Sound audio;
    int count, i;
    memset(&audio, 0, sizeof(audio));
    memset(pcm, 0xa5, sizeof(pcm));
    assert(!om3dvid7_sound_start(&audio, pcm, 32, 11025) && audio.status == 1 && resources == 30);
    count = calls;
    assert(!om3dvid7_sound_restart(&audio));
    om3dvid7_sound_close(&audio);
    om3dvid7_sound_close(&audio);
    assert(!opened && !resources && pcm[0] == 0xa5);
    for (i = 1; i <= count; ++i) {
        calls = 0;
        fail_at = i;
        assert(om3dvid7_sound_start(&audio, pcm, 32, 11025) < 0 && audio.status == -1);
        assert(!opened && !resources && pcm[0] == 0xa5);
        om3dvid7_sound_close(&audio);
    }
    fail_at = 0;
    calls = 0;
    assert(om3dvid7_sound_start(&audio, NULL, 32, 11025) < 0 && !calls);
    assert(om3dvid7_sound_start(&audio, pcm, 31, 11025) < 0 && !calls);
    assert(om3dvid7_sound_start(&audio, pcm, 32, 22050) < 0 && !calls);
    assert(!om3dvid7_sound_start(&audio, pcm, 32, 11025));
    fail_at = calls + 1;
    assert(om3dvid7_sound_restart(&audio) < 0 && !resources && !opened);
    om3dvid7_sound_close(&audio);
    /* The streaming ring loops; pausing reaches the player only while it plays. */
    ring = 1;
    fail_at = 0;
    calls = 0;
    assert(!om3dvid7_sound_start_ring(&audio, pcm, 32, 11025) && audio.status == 1);
    om3dvid7_sound_pause(&audio, 1);
    assert(paused);
    om3dvid7_sound_pause(&audio, 0);
    assert(!paused);
    om3dvid7_sound_close(&audio);
    om3dvid7_sound_pause(&audio, 1);
    assert(!paused && !resources && !opened);
    /* The same through the player's callbacks. */
    calls = 0;
    assert(!om3dvid7_sound_player_start(&audio, (signed char *)pcm, 32, 11025) &&
           audio.status == 1);
    om3dvid7_sound_player_pause(&audio, 1);
    assert(paused);
    om3dvid7_sound_player_pause(&audio, 0);
    assert(!paused);
    om3dvid7_sound_player_stop(&audio);
    assert(!resources && !opened && audio.status == 0);
    puts("om3dvid7 sound: looping ring and pause, signed PCM, stereo routing, synchronized "
         "restart, every startup failure, borrowed-buffer cleanup and the player callbacks passed");
    return 0;
}
