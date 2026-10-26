/*
 * Clip sound through the audio folio (the 3DO operating system's sound
 * library). See om3dvid7_sound.h.
 *
 * Two DSP instruments are chained: varmono8.dsp plays 8-bit mono samples at
 * an adjustable rate, and directout.dsp sends its output to both speakers.
 */
#include "om3dvid7_sound.h"
#include "audio.h"
#include "string.h"

/* The only sample rate clips use. */
#define SOUND_RATE 11025UL

/* The DSP's own output rate; instrument rates are given relative to it. */
#define DSP_RATE 44100UL

/* Rates are fixed-point with 0x8000 meaning "the DSP's own rate". */
#define DSP_RATE_ONE 32768UL

/* The playing volume: 0x6000 of the full 0x7FFF, about three quarters. */
#define AMPLITUDE 0x6000

/* The largest sound accepted, in samples. */
#define LONGEST_SOUND (1024UL * 1024UL)

void om3dvid7_sound_close(Om3dvid7Sound *sound)
{
    /* Stop both instruments, then unload in the reverse order of loading. */
    if (sound->player > 0)
        StopInstrument(sound->player, NULL);
    if (sound->output > 0)
        StopInstrument(sound->output, NULL);
    if (sound->attachment > 0)
        DetachSample(sound->attachment);
    if (sound->player > 0)
        UnloadInstrument(sound->player);
    if (sound->output > 0)
        UnloadInstrument(sound->output);
    if (sound->sample > 0)
        UnloadSample(sound->sample);
    if (sound->opened)
        CloseAudioFolio();
    memset(sound, 0, sizeof(*sound));
}

int om3dvid7_sound_restart(Om3dvid7Sound *sound)
{
    TagArg tags[3];

    if (sound->status != 1)
        return -1;
    if (StopInstrument(sound->player, NULL) < 0)
        goto failed;

    /* Start playing at the set volume, at the sound's rate relative to the DSP's. */
    memset(tags, 0, sizeof(tags));
    tags[0].ta_Tag = AF_TAG_AMPLITUDE;
    tags[0].ta_Arg = (void *)AMPLITUDE;
    tags[1].ta_Tag = AF_TAG_RATE;
    tags[1].ta_Arg = (void *)(sound->rate * DSP_RATE_ONE / DSP_RATE);
    if (StartInstrument(sound->player, tags) < 0)
        goto failed;
    return 0;

failed:
    om3dvid7_sound_close(sound);
    sound->status = -1;
    return -1;
}

/* Sets up the sample and instruments and starts playing; `loop` repeats the
 * whole sound until stopped. Returns 0, or -1. */
static int start_sound(Om3dvid7Sound *sound, unsigned char *data, unsigned long frames,
                       unsigned long rate, int loop)
{
    TagArg tags[8];

    om3dvid7_sound_close(sound);
    if (!data || frames < 4 || (frames & 3) || frames > LONGEST_SOUND || rate != SOUND_RATE)
        return -1;
    if (OpenAudioFolio() < 0)
        goto failed;
    sound->opened = 1;
    sound->rate = rate;

    /* Describe the sound: where it is, how long, 8-bit mono, its rate
     * (16.16 fixed point). */
    memset(tags, 0, sizeof(tags));
    tags[0].ta_Tag = AF_TAG_ADDRESS;
    tags[0].ta_Arg = data;
    tags[1].ta_Tag = AF_TAG_FRAMES;
    tags[1].ta_Arg = (void *)frames;
    tags[2].ta_Tag = AF_TAG_NUMBITS;
    tags[2].ta_Arg = (void *)8;
    tags[3].ta_Tag = AF_TAG_CHANNELS;
    tags[3].ta_Arg = (void *)1;
    tags[4].ta_Tag = AF_TAG_SAMPLE_RATE;
    tags[4].ta_Arg = (void *)(rate << 16);
    if (loop) {
        /* A sustain loop over the whole sample: it repeats until stopped. */
        tags[5].ta_Tag = AF_TAG_SUSTAINBEGIN;
        tags[5].ta_Arg = (void *)0;
        tags[6].ta_Tag = AF_TAG_SUSTAINEND;
        tags[6].ta_Arg = (void *)frames;
    }
    sound->sample = CreateSample(tags);
    if (sound->sample < 0)
        goto failed;

    /* The output to both speakers, and the player feeding it. */
    sound->output = LoadInstrument("directout.dsp", 0, 50);
    if (sound->output < 0)
        goto failed;
    sound->player = LoadInstrument("varmono8.dsp", 0, 100);
    if (sound->player < 0)
        goto failed;
    if (ConnectInstruments(sound->player, "Output", sound->output, "InputLeft") < 0 ||
        ConnectInstruments(sound->player, "Output", sound->output, "InputRight") < 0)
        goto failed;
    sound->attachment = AttachSample(sound->player, sound->sample, 0);
    if (sound->attachment < 0 || StartInstrument(sound->output, NULL) < 0)
        goto failed;

    sound->status = 1;
    return om3dvid7_sound_restart(sound);

failed:
    om3dvid7_sound_close(sound);
    sound->status = -1;
    return -1;
}

int om3dvid7_sound_start(Om3dvid7Sound *sound, unsigned char *data, unsigned long frames,
                         unsigned long rate)
{
    return start_sound(sound, data, frames, rate, 0);
}

int om3dvid7_sound_start_ring(Om3dvid7Sound *sound, unsigned char *data, unsigned long frames,
                              unsigned long rate)
{
    return start_sound(sound, data, frames, rate, 1);
}

void om3dvid7_sound_pause(Om3dvid7Sound *sound, int pause)
{
    if (sound->status != 1)
        return;
    if (pause)
        PauseInstrument(sound->player);
    else
        ResumeInstrument(sound->player);
}

int om3dvid7_sound_player_start(void *sound, signed char *ring, unsigned long samples,
                                unsigned long rate)
{
    return om3dvid7_sound_start_ring((Om3dvid7Sound *)sound, (unsigned char *)ring, samples, rate);
}

void om3dvid7_sound_player_pause(void *sound, int pause)
{
    om3dvid7_sound_pause((Om3dvid7Sound *)sound, pause);
}

void om3dvid7_sound_player_stop(void *sound)
{
    om3dvid7_sound_close((Om3dvid7Sound *)sound);
}
