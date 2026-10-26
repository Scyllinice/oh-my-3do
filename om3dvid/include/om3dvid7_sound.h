/*
 * Playing a clip's sound through the 3DO's audio DSP (Portfolio's audio
 * folio). Console builds only.
 *
 * The sound is 8-bit mono at 11025 samples a second. The audio folio plays
 * it straight from memory, so the caller owns that memory and must close the
 * sound before freeing it.
 *
 * To give the OM3DVID7 player (om3dvid7_player.h) sound on the console, set
 * its host's sound_context to an Om3dvid7Sound (zeroed at first) and its
 * sound_start, sound_pause and sound_stop to the om3dvid7_sound_player_*
 * functions below.
 */
#ifndef OM3DVID7_SOUND_H
#define OM3DVID7_SOUND_H

/* The audio folio items in use (each 0 when not loaded). */
typedef struct Om3dvid7Sound {
    long sample;     /* the sound data, described to the folio */
    long player;     /* the DSP instrument that plays it (varmono8) */
    long output;     /* the instrument that sends it to the speakers (directout) */
    long attachment; /* the sample attached to the player */
    unsigned long rate;
    int opened; /* the audio folio is open */
    int status; /* 1 playing (or paused), -1 failed, 0 never started */
} Om3dvid7Sound;

/*
 * Plays `frames` samples at `data` once from the start. `frames` must be a
 * whole number of words and `rate` 11025. Returns 0, or -1 (silently
 * continuing without sound is the caller's choice).
 */
int om3dvid7_sound_start(Om3dvid7Sound *sound, unsigned char *data, unsigned long frames,
                         unsigned long rate);

/* Plays the sound again from the start. Returns 0, or -1. */
int om3dvid7_sound_restart(Om3dvid7Sound *sound);

/*
 * Plays `frames` samples at `data` from the start and loops over them
 * forever: a ring the caller keeps filling ahead of the DSP.
 */
int om3dvid7_sound_start_ring(Om3dvid7Sound *sound, unsigned char *data, unsigned long frames,
                              unsigned long rate);

/* Pauses (1) or resumes (0) the sound. */
void om3dvid7_sound_pause(Om3dvid7Sound *sound, int pause);

/* Stops the sound and frees everything; safe to call more than once. */
void om3dvid7_sound_close(Om3dvid7Sound *sound);

/* The player's sound callbacks (Om3dvid7PlayerHost.sound_*), where the
 * context is an Om3dvid7Sound. */
int om3dvid7_sound_player_start(void *sound, signed char *ring, unsigned long samples,
                                unsigned long rate);
void om3dvid7_sound_player_pause(void *sound, int pause);
void om3dvid7_sound_player_stop(void *sound);
#endif
