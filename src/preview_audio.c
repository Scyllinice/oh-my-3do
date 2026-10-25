/*
 * Preview sound through the audio folio (the 3DO operating system's sound
 * library). See preview_audio.h.
 *
 * Two DSP instruments are chained: varmono8.dsp plays 8-bit mono samples at
 * an adjustable rate, and directout.dsp sends its output to both speakers.
 */
#include "preview_audio.h"
#include "audio.h"
#include "string.h"

/* The only sample rate previews use. */
#define PREVIEW_AUDIO_RATE 11025UL

/* The DSP's own output rate; instrument rates are given relative to it. */
#define DSP_RATE 44100UL

/* Rates are fixed-point with 0x8000 meaning "the DSP's own rate". */
#define DSP_RATE_ONE 32768UL

/* The playing volume: 0x6000 of the full 0x7FFF, about three quarters. */
#define PREVIEW_AMPLITUDE 0x6000

/* The largest sound accepted, in samples. */
#define LONGEST_SOUND (1024UL * 1024UL)

void preview_audio_close(PreviewAudio *audio)
{
    /* Stop both instruments, then unload in the reverse order of loading. */
    if (audio->player > 0)
        StopInstrument(audio->player, NULL);
    if (audio->output > 0)
        StopInstrument(audio->output, NULL);
    if (audio->attachment > 0)
        DetachSample(audio->attachment);
    if (audio->player > 0)
        UnloadInstrument(audio->player);
    if (audio->output > 0)
        UnloadInstrument(audio->output);
    if (audio->sample > 0)
        UnloadSample(audio->sample);
    if (audio->opened)
        CloseAudioFolio();
    memset(audio, 0, sizeof(*audio));
}

int preview_audio_restart(PreviewAudio *audio)
{
    TagArg tags[3];

    if (audio->status != 1)
        return -1;
    if (StopInstrument(audio->player, NULL) < 0)
        goto failed;

    /* Start playing at the set volume, at the sound's rate relative to the DSP's. */
    memset(tags, 0, sizeof(tags));
    tags[0].ta_Tag = AF_TAG_AMPLITUDE;
    tags[0].ta_Arg = (void *)PREVIEW_AMPLITUDE;
    tags[1].ta_Tag = AF_TAG_RATE;
    tags[1].ta_Arg = (void *)(audio->rate * DSP_RATE_ONE / DSP_RATE);
    if (StartInstrument(audio->player, tags) < 0)
        goto failed;
    return 0;

failed:
    preview_audio_close(audio);
    audio->status = -1;
    return -1;
}

/* Sets up the sample and instruments and starts playing; `loop` repeats the
 * whole sound until stopped. Returns 0, or -1. */
static int start_sound(PreviewAudio *audio, unsigned char *data, unsigned long frames,
                       unsigned long rate, int loop)
{
    TagArg tags[8];

    preview_audio_close(audio);
    if (!data || frames < 4 || (frames & 3) || frames > LONGEST_SOUND || rate != PREVIEW_AUDIO_RATE)
        return -1;
    if (OpenAudioFolio() < 0)
        goto failed;
    audio->opened = 1;
    audio->rate = rate;

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
    audio->sample = CreateSample(tags);
    if (audio->sample < 0)
        goto failed;

    /* The output to both speakers, and the player feeding it. */
    audio->output = LoadInstrument("directout.dsp", 0, 50);
    if (audio->output < 0)
        goto failed;
    audio->player = LoadInstrument("varmono8.dsp", 0, 100);
    if (audio->player < 0)
        goto failed;
    if (ConnectInstruments(audio->player, "Output", audio->output, "InputLeft") < 0 ||
        ConnectInstruments(audio->player, "Output", audio->output, "InputRight") < 0)
        goto failed;
    audio->attachment = AttachSample(audio->player, audio->sample, 0);
    if (audio->attachment < 0 || StartInstrument(audio->output, NULL) < 0)
        goto failed;

    audio->status = 1;
    return preview_audio_restart(audio);

failed:
    preview_audio_close(audio);
    audio->status = -1;
    return -1;
}

int preview_audio_start(PreviewAudio *audio, unsigned char *data, unsigned long frames,
                        unsigned long rate)
{
    return start_sound(audio, data, frames, rate, 0);
}

int preview_audio_start_ring(PreviewAudio *audio, unsigned char *data, unsigned long frames,
                             unsigned long rate)
{
    return start_sound(audio, data, frames, rate, 1);
}

void preview_audio_pause(PreviewAudio *audio, int pause)
{
    if (audio->status != 1)
        return;
    if (pause)
        PauseInstrument(audio->player);
    else
        ResumeInstrument(audio->player);
}
