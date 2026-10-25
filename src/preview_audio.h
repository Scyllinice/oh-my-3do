#ifndef OH3DO_PREVIEW_AUDIO_H
#define OH3DO_PREVIEW_AUDIO_H
/* The preview loader owns PCM storage; close audio before freeing that storage. */
typedef struct PreviewAudio {
    long sample, player, output, attachment;
    unsigned long rate;
    int opened, status;
} PreviewAudio;
int preview_audio_start(PreviewAudio *audio, unsigned char *data, unsigned long frames,
                        unsigned long rate);
int preview_audio_restart(PreviewAudio *audio);
/* Play frames of data from the start and loop over them forever: a ring the
 * caller keeps filling ahead of the DSP. */
int preview_audio_start_ring(PreviewAudio *audio, unsigned char *data, unsigned long frames,
                             unsigned long rate);
void preview_audio_pause(PreviewAudio *audio, int pause);
void preview_audio_close(PreviewAudio *audio);
#endif
