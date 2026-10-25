#ifndef OH3DO_PREVIEW_STREAM_H
#define OH3DO_PREVIEW_STREAM_H
/* Playing an OM3DVID7 preview while it streams from the USB drive.
 *
 * A reader thread with its own XBUS requests reads the clip's chunks in
 * order into a few fixed-size slots, checks each, stores its dictionary
 * entries, and starts over from the top of the file after the last chunk.
 * The player decodes frames from ready slots, copies each chunk's sound into
 * a looping ring the audio DSP plays, and releases a slot once both are done.
 * Hosts (tests, the menu preview renderer) have no threads: reads happen
 * synchronously whenever the player needs a chunk.
 *
 * The reader owns the ODE while the stream is open: close the stream before
 * any other USB access. Media sits beside the discs (.ohmy3do in the game's
 * folder); the file is reopened by the same relative path for every loop. */
#include "menu_config.h"
#include "preview_v7.h"
#include "preview_audio.h"

#define PREVIEW_STREAM_SLOTS     8
#define PREVIEW_STREAM_RING      32768UL /* audio samples, about 3 s */
#define PREVIEW_STREAM_NOT_READY 1

typedef int (*PreviewStreamCancel)(void *context);
typedef int (*PreviewStreamReclaim)(void *context);

typedef struct PreviewStream {
    int open;    /* the first frame is drawn */
    int playing; /* the clock and sound have started */
    Preview7Header header;
    unsigned char block[PREVIEW7_BLOCK_BYTES]; /* the header block */
    Preview7 dec;
    unsigned char *state, *thumb;
    unsigned char *slot[PREVIEW_STREAM_SLOTS];
    unsigned char slot_vram[PREVIEW_STREAM_SLOTS];
    unsigned long slot_bytes;
    unsigned int slots;
    /* Reader to player: ready[k] is the chunk sequence (counting across
     * loops) + 1 that slot k holds, once checked and its entries stored. */
    Preview7Chunk info[PREVIEW_STREAM_SLOTS];
    volatile unsigned long ready[PREVIEW_STREAM_SLOTS];
    volatile unsigned long released; /* sequences below this are done with */
    volatile int stop, running, error;
    volatile int reading; /* the reader is moving a chunk's blocks */
    unsigned long slot_sum[PREVIEW_STREAM_SLOTS];
    unsigned long next_read; /* host builds: the next sequence to read */
    /* The player: the next frame to decode and audio already in the ring,
     * both counted from the start across loops. */
    unsigned long frame, audio_written;
    int picture_valid[2]; /* the screen / TV picture shows the map */
    /* Sound */
    signed char *ring;
    PreviewAudio audio;
    int sound, paused; /* paused: 1 waiting for a chunk, 2 holding at a loop */
    unsigned long audio_since, audio_paused_ms, pause_at;
    /* Reading */
    Ode ode;
    char folder[128], name[128];
    long thread;
    unsigned long bus_commands;
    /* Diagnostics */
    unsigned long chunks_read, read_ms, read_max_ms, stalls, stall_ms;
    /* Waits at a loop for every slot to fill (not stalls). */
    int loop_hold;
    unsigned long loop_holds, loop_hold_ms;
    unsigned long spare_blocks; /* blocks read below the menu's priority */
    /* The first stalls: frame, chunks released, the slots' sequences, ms played. */
    unsigned long stall_log[4][4];
    long reader_error_code;
    /* Decoding time measured by the menu: overall, and with the reader idle
     * throughout (no transfers or sums competing for the bus or CPU). */
    unsigned long decode_ms, decode_frames, idle_ms, idle_frames, full_ms, full_frames;
} PreviewStream;

/* Bytes the stream allocates for a clip with this header and slot count. */
unsigned long preview_stream_bytes(const Preview7Header *header, unsigned int slots);
/* Start streaming the clip whose header block the caller has read (and
 * checked with preview7_header). game_folder is the collapsed game folder the
 * file sits in, or "" for the current folder; name the file under .ohmy3do.
 * Allocates (calling reclaim for more room), starts the reader, waits until
 * every slot holds a chunk, polling cancel, and draws the first
 * frame into the TV picture. reader is used on hosts only (the console's
 * reader thread has its own XBUS requests). 0, -1 error, -4 out of memory,
 * -5 canceled. On failure the stream is closed. */
int preview_stream_open(PreviewStream *stream, const Ode *ode, OdeReadBuffer reader,
                        const char *game_folder, const char *name,
                        const unsigned char *header_block, int sound, PreviewStreamCancel cancel,
                        void *cancel_context, PreviewStreamReclaim reclaim, void *reclaim_context);
/* Start the clock and the sound, as the first frame shows. */
void preview_stream_play(PreviewStream *stream, unsigned long now_ms);
/* Decode up to and including frame `frame` (counted across loops) into the
 * picture (PREVIEW7_SCREEN into screen, or PREVIEW7_TV), redrawing it whole
 * first if it does not show the map yet. 0, PREVIEW_STREAM_NOT_READY if a
 * chunk has not arrived (nothing was decoded past it), -1 on an error. */
int preview_stream_decode(PreviewStream *stream, unsigned long frame, int picture,
                          unsigned char *screen);
/* The other picture no longer shows the map (it was drawn over). */
void preview_stream_invalidate(PreviewStream *stream, int picture);
/* Keep the sound ring filled; pause it while the video waits for a chunk.
 * Call every main-loop pass. */
void preview_stream_audio(PreviewStream *stream, unsigned long now_ms, int waiting);
/* The frame due now, counted across loops, from the sound's clock (0 until
 * preview_stream_play). */
unsigned long preview_stream_due(PreviewStream *stream, unsigned long now_ms);
/* Stop the reader, the sound and free everything; diagnostics stay. Safe on
 * a closed or zeroed stream. */
void preview_stream_close(PreviewStream *stream);
#endif
