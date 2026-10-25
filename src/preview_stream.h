/*
 * Playing an OM3DVID7 preview while it streams from the USB drive.
 *
 * A clip is split into chunks of about a second each (see preview_v7.h).
 * On the console, a reader thread with its own XBUS requests reads the
 * chunks in order into a few fixed-size memory "slots", checks each one,
 * stores its dictionary entries, and starts over from the top of the file
 * after the last chunk, so the clip loops. Meanwhile the player decodes
 * frames from slots that are ready, copies each chunk's sound into a looping
 * ring buffer that the audio DSP plays, and frees a slot once both are done
 * with it.
 *
 * On a PC (tests, and the menu preview renderer) there are no threads:
 * chunks are read on the spot whenever the player needs one.
 *
 * Chunks are numbered by "sequence": counting from the first chunk of the
 * first play, across loops, so chunk 3 of the second loop of a 10-chunk clip
 * is sequence 13. Frames are counted the same way.
 *
 * While a stream is open the reader owns the ODE: close the stream before
 * any other USB access. Media sits beside the discs (the .ohmy3do folder in
 * the game's folder), and the file is reopened by the same relative path
 * for every loop.
 */
#ifndef OH3DO_PREVIEW_STREAM_H
#define OH3DO_PREVIEW_STREAM_H
#include "menu_config.h"
#include "preview_v7.h"
#include "preview_audio.h"

/* The most chunk slots; the clip's header says how many the encoder assumed. */
#define PREVIEW_STREAM_SLOTS 8

/* The sound ring, in samples (about 3 seconds at 11025 Hz). */
#define PREVIEW_STREAM_RING 32768UL

/* preview_stream_decode: the frame's chunk has not arrived yet. */
#define PREVIEW_STREAM_NOT_READY 1

/* Asked while waiting; nonzero stops opening the stream. */
typedef int (*PreviewStreamCancel)(void *context);
/* Asked to free optional caches when memory is short; nonzero if it did. */
typedef int (*PreviewStreamReclaim)(void *context);

typedef struct PreviewStream {
    int open;    /* the first frame is drawn */
    int playing; /* the clock and sound have started */
    Preview7Header header;
    unsigned char block[PREVIEW7_BLOCK_BYTES]; /* the file's header block */
    Preview7 dec;                              /* the decoder's state */
    unsigned char *state, *thumb;              /* thumb: the TV picture */

    /* Chunk slots: sequence N goes in slot N % slots. */
    unsigned char *slot[PREVIEW_STREAM_SLOTS];
    unsigned char slot_vram[PREVIEW_STREAM_SLOTS]; /* 1 if in video memory */
    unsigned long slot_bytes;
    unsigned int slots;

    /*
     * Shared between the reader and the player (hence volatile). ready[k] is
     * the sequence + 1 of the chunk slot k holds, once it is checked and its
     * entries are stored (0: nothing yet). Sequences below `released` are
     * finished with, so their slots may be refilled.
     */
    Preview7Chunk info[PREVIEW_STREAM_SLOTS];
    volatile unsigned long ready[PREVIEW_STREAM_SLOTS];
    volatile unsigned long released;
    volatile int stop;    /* the player asks the reader to stop */
    volatile int running; /* the reader thread is alive */
    volatile int error;   /* 1 reader start, 2 read, 3 decode, 4 reader would not stop */
    volatile int reading; /* the reader is in the middle of moving a chunk */
    unsigned long slot_sum[PREVIEW_STREAM_SLOTS]; /* each chunk's word sum, as read */
    unsigned long next_read;                      /* PC builds: the next sequence to read */

    /* The player: the next frame to decode, and the sequences whose sound is
     * already in the ring. */
    unsigned long frame, audio_written;
    int picture_valid[2]; /* the screen / TV picture shows the map */

    /* Sound. */
    signed char *ring;
    PreviewAudio audio;
    int sound;
    int paused;                    /* 1: waiting for a chunk; 2: holding at the start of a loop */
    unsigned long audio_since;     /* when playing started */
    unsigned long audio_paused_ms; /* time spent paused so far */
    unsigned long pause_at;        /* when the current pause began */

    /* Reading. */
    Ode ode; /* the reader's own copy of the connection */
    char folder[128], name[128];
    long thread;
    unsigned long bus_commands;

    /* Diagnostics. */
    unsigned long chunks_read, read_ms, read_max_ms;
    unsigned long stalls, stall_ms; /* waits for a chunk mid-clip */
    int loop_hold;                  /* holding at a loop until every slot fills (not a stall) */
    unsigned long loop_holds, loop_hold_ms;
    unsigned long spare_blocks; /* blocks read at the lower "spare time" priority */
    /* The first four stalls: frame, chunks released, the slots' sequences
     * (4 bits each), milliseconds played. */
    unsigned long stall_log[4][4];
    long reader_error_code;
    /* Decoding time measured by the menu: overall, with the reader idle
     * throughout (no transfers or sums competing for the bus or processor),
     * and for full redraws. */
    unsigned long decode_ms, decode_frames, idle_ms, idle_frames, full_ms, full_frames;
} PreviewStream;

/* Bytes a stream allocates for a clip with this header and slot count. */
unsigned long preview_stream_bytes(const Preview7Header *header, unsigned int slots);

/*
 * Starts streaming the clip whose header block the caller has read and
 * checked (with preview7_header). `game_folder` is the collapsed game folder
 * the file sits in, or "" for the current folder; `name` is the file's name
 * in .ohmy3do. Allocates memory (calling `reclaim` for more room), starts
 * the reader, waits until every slot holds a chunk (asking `cancel` while it
 * waits) and draws the first frame into the TV picture. `reader` is used on
 * PCs only; the console's reader thread has its own XBUS requests.
 *
 * Returns 0, -1 on an error, -4 if out of memory or -5 if canceled. On
 * failure the stream is closed.
 */
int preview_stream_open(PreviewStream *stream, const Ode *ode, OdeReadBuffer reader,
                        const char *game_folder, const char *name,
                        const unsigned char *header_block, int sound, PreviewStreamCancel cancel,
                        void *cancel_context, PreviewStreamReclaim reclaim, void *reclaim_context);

/* Starts the clock and the sound, as the first frame appears. */
void preview_stream_play(PreviewStream *stream, unsigned long now_ms);

/*
 * Decodes up to and including frame `frame` (counted across loops) into the
 * picture (PREVIEW7_SCREEN into `screen`, or PREVIEW7_TV), first redrawing
 * it whole if it does not show the map yet. Returns 0,
 * PREVIEW_STREAM_NOT_READY if a chunk has not arrived (nothing past it was
 * decoded), or -1 on an error.
 */
int preview_stream_decode(PreviewStream *stream, unsigned long frame, int picture,
                          unsigned char *screen);

/* Notes that a picture no longer shows the map (something was drawn over it). */
void preview_stream_invalidate(PreviewStream *stream, int picture);

/* Keeps the sound ring filled, and pauses the sound while the video waits
 * for a chunk. Call on every pass of the main loop. */
void preview_stream_audio(PreviewStream *stream, unsigned long now_ms, int waiting);

/* The frame due now, counted across loops, by the sound's clock (0 until
 * preview_stream_play). */
unsigned long preview_stream_due(PreviewStream *stream, unsigned long now_ms);

/* Stops the reader and the sound and frees everything; the diagnostics are
 * kept. Safe on a closed or zeroed stream. */
void preview_stream_close(PreviewStream *stream);
#endif
