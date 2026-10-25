/*
 * Playing a preview's sound through the 3DO's audio DSP.
 *
 * The sound is 8-bit mono at 11025 samples a second. The audio folio plays
 * it straight from memory, so the caller owns that memory and must close the
 * audio before freeing it.
 */
#ifndef OH3DO_PREVIEW_AUDIO_H
#define OH3DO_PREVIEW_AUDIO_H

/* The audio folio items in use (each 0 when not loaded). */
typedef struct PreviewAudio {
    long sample;     /* the sound data, described to the folio */
    long player;     /* the DSP instrument that plays it (varmono8) */
    long output;     /* the instrument that sends it to the speakers (directout) */
    long attachment; /* the sample attached to the player */
    unsigned long rate;
    int opened; /* the audio folio is open */
    int status; /* 1 playing (or paused), -1 failed, 0 never started */
} PreviewAudio;

/*
 * Plays `frames` samples at `data` once from the start. `frames` must be a
 * whole number of words and `rate` 11025. Returns 0, or -1 (silently
 * continuing without sound is the caller's choice).
 */
int preview_audio_start(PreviewAudio *audio, unsigned char *data, unsigned long frames,
                        unsigned long rate);

/* Plays the sound again from the start. Returns 0, or -1. */
int preview_audio_restart(PreviewAudio *audio);

/*
 * Plays `frames` samples at `data` from the start and loops over them
 * forever: a ring the caller keeps filling ahead of the DSP.
 */
int preview_audio_start_ring(PreviewAudio *audio, unsigned char *data, unsigned long frames,
                             unsigned long rate);

/* Pauses (1) or resumes (0) the sound. */
void preview_audio_pause(PreviewAudio *audio, int pause);

/* Stops the sound and frees everything; safe to call more than once. */
void preview_audio_close(PreviewAudio *audio);
#endif
