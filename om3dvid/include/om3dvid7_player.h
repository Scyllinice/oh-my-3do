#ifndef OM3DVID7_PLAYER_H
#define OM3DVID7_PLAYER_H
/*
 * Playing an OM3DVID7 clip while it streams in from slow storage.
 *
 * A clip is split into chunks of about a second each (see om3dvid7.h). A
 * "reader" reads the chunks in order into a few fixed-size memory "slots",
 * checks each one, stores its dictionary entries, and starts over from the
 * top of the file after the last chunk, so the clip loops. Meanwhile the
 * player decodes frames from slots that are ready, copies each chunk's sound
 * into a looping ring buffer that the host plays, and frees a slot once both
 * are done with it.
 *
 * The player is plain C89 and knows nothing about the machine it runs on:
 * the host program supplies an Om3dvid7PlayerHost of callbacks for reading
 * the file, memory, sound, and (optionally) a reader thread.
 *
 *   - Without a reader thread (start_reader NULL), chunks are read on the
 *     spot, inside om3dvid7_player_open and om3dvid7_player_decode, whenever
 *     a slot is free. Simple, and fine where reading is fast.
 *   - With one, the host starts a thread that calls om3dvid7_player_read
 *     over and over, so reading overlaps decoding. The player and the reader
 *     share only the fields marked volatile below.
 *
 * Chunks are numbered by "sequence": counting from the first chunk of the
 * first play, across loops, so chunk 3 of the second loop of a 10-chunk clip
 * is sequence 13. Frames are counted the same way.
 *
 * The file is only ever read forwards, a 2 KiB block at a time, and opened
 * again from the start for every loop, so the source needs no seeking.
 *
 * Typical use, once per pass of the host's main loop:
 *
 *   due = om3dvid7_player_due(&player, now);
 *   result = om3dvid7_player_decode(&player, due, OM3DVID7_TV, NULL);
 *   om3dvid7_player_update(&player, now, result == OM3DVID7_PLAYER_NOT_READY);
 *   then show player.thumb (or the screen passed to decode)
 */
#include "om3dvid7.h"

/* The most chunk slots; the clip's header says how many the encoder assumed. */
#define OM3DVID7_PLAYER_SLOTS 8

/* The sound ring, in samples (about 3 seconds at 11025 Hz). */
#define OM3DVID7_PLAYER_RING 32768UL

/* om3dvid7_player_decode: the frame's chunk has not arrived yet. */
#define OM3DVID7_PLAYER_NOT_READY 1

/* om3dvid7_player_open's failures besides -1. */
#define OM3DVID7_PLAYER_NO_MEMORY -4
#define OM3DVID7_PLAYER_CANCELED  -5

/* om3dvid7_player_read's results besides 0 (a chunk was read) and -1. */
#define OM3DVID7_PLAYER_READ_LATER 1 /* nothing to read yet: wait a little */
#define OM3DVID7_PLAYER_READ_STOP  2 /* the player is closing: end the thread */

/* Om3dvid7Player.error. */
#define OM3DVID7_PLAYER_ERROR_START   1 /* the reader thread did not start */
#define OM3DVID7_PLAYER_ERROR_READ    2 /* a read failed or a chunk was damaged */
#define OM3DVID7_PLAYER_ERROR_DECODE  3 /* a frame would not decode */
#define OM3DVID7_PLAYER_ERROR_RUNAWAY 4 /* the reader thread would not stop */

/*
 * Kinds of memory the player asks for. On the 3DO these are main memory,
 * video memory, memory the cel engine can draw from, and memory the audio
 * DSP can play from; elsewhere they can all be ordinary memory.
 */
#define OM3DVID7_MEMORY_MAIN  0
#define OM3DVID7_MEMORY_VIDEO 1
#define OM3DVID7_MEMORY_CEL   2
#define OM3DVID7_MEMORY_SOUND 3

typedef struct Om3dvid7Player Om3dvid7Player;

/*
 * The host's side. Every callback gets `context`. Unless marked optional,
 * a callback must be set. Callbacks marked "reader" are called by the reader
 * thread when there is one, and by the player's own calls otherwise.
 */
typedef struct Om3dvid7PlayerHost {
    void *context;

    /* Reader. Opens the clip at its start, reads its next 2048 bytes, or
     * closes it. Each returns 0, or -1 on failure. */
    int (*open)(void *context);
    int (*read_block)(void *context, unsigned char *block);
    int (*close)(void *context);

    /*
     * Reader, optional. Called before reading a block when the answer to
     * "is this block not needed for a while?" changes: `spare` is 1 when it
     * is not needed yet, 0 when the player is (or soon will be) waiting for
     * it. A reader thread can run at a lower priority while spare.
     */
    void (*pace)(void *context, int spare);

    /*
     * Optional: memory of a kind (OM3DVID7_MEMORY_*), and freeing it (given
     * the size it was allocated with). Without them the player uses malloc
     * and free. If main memory runs out the player tries video memory; if
     * that fails too, reclaim (optional) may free the host's caches,
     * returning 1 if it freed anything, and the player tries once more.
     */
    void *(*allocate)(void *context, unsigned long bytes, int kind);
    void (*release)(void *context, void *memory, unsigned long bytes);
    int (*reclaim)(void *context);

    /*
     * Optional sound. start plays `samples` signed 8-bit samples at `ring`
     * at `rate` Hz, looping over them forever (the player keeps writing
     * ahead of it); it returns 0, or -1 to play on silently. pause pauses
     * (1) or resumes (0) it; stop stops it before the ring is freed, and is
     * called after any start, even one that failed, so it can clean up.
     * Without start the clip plays silently. They get sound_context if it
     * is set, and context otherwise; om3dvid7_sound.h has ready-made ones
     * for the 3DO.
     */
    void *sound_context;
    int (*sound_start)(void *context, signed char *ring, unsigned long samples, unsigned long rate);
    void (*sound_pause)(void *context, int pause);
    void (*sound_stop)(void *context);

    /*
     * Optional reader thread. start_reader starts a thread that calls
     * om3dvid7_player_read until it returns OM3DVID7_PLAYER_READ_STOP or
     * -1, then om3dvid7_player_close_file and, last,
     * om3dvid7_player_reader_exit. It returns 0, or -1 if the
     * thread could not start. stop_reader is called by om3dvid7_player_close
     * after it asks the reader to stop; it waits for the thread to exit
     * (player->running becomes 0) and cleans up after it. If the thread will
     * not exit it sets player->error to OM3DVID7_PLAYER_ERROR_RUNAWAY.
     */
    int (*start_reader)(void *context, Om3dvid7Player *player);
    void (*stop_reader)(void *context, Om3dvid7Player *player);

    /*
     * Optional, while om3dvid7_player_open waits for the first chunks.
     * cancel returns nonzero to give up; wait sleeps for a few milliseconds
     * (only used with a reader thread, to let it run).
     */
    int (*cancel)(void *context);
    void (*wait)(void *context);
} Om3dvid7PlayerHost;

struct Om3dvid7Player {
    int open;    /* the first frame is drawn */
    int playing; /* the clock and sound have started */
    Om3dvid7PlayerHost host;
    Om3dvid7Header header;
    unsigned char block[OM3DVID7_BLOCK_BYTES]; /* the file's header block */
    Om3dvid7Decoder dec;                       /* the decoder's state */
    unsigned char *thumb;                      /* the 160x120 TV picture */

    /* Chunk slots: sequence N goes in slot N % slots. */
    unsigned char *slot[OM3DVID7_PLAYER_SLOTS];
    unsigned char slot_vram[OM3DVID7_PLAYER_SLOTS]; /* 1 if in video memory */
    unsigned long slot_bytes;
    unsigned int slots;

    /*
     * Shared between the reader and the player (hence volatile). ready[k] is
     * the sequence + 1 of the chunk slot k holds, once it is checked and its
     * entries are stored (0: nothing yet). Sequences below `released` are
     * finished with, so their slots may be refilled.
     */
    Om3dvid7Chunk info[OM3DVID7_PLAYER_SLOTS];
    volatile unsigned long ready[OM3DVID7_PLAYER_SLOTS];
    volatile unsigned long released;
    volatile int stop;    /* the player asks the reader to stop */
    volatile int running; /* the reader thread is alive */
    volatile int error;   /* OM3DVID7_PLAYER_ERROR_*, or 0 */
    volatile int reading; /* the reader is in the middle of reading a chunk */

    /* The reader's own. */
    unsigned long slot_sum[OM3DVID7_PLAYER_SLOTS]; /* each chunk's word sum, as read */
    unsigned long next_read;                       /* the next sequence to read */
    int file_open;                                 /* the source is open */
    int spare;                                     /* the last answer given to pace */
    int reader_started;                            /* start_reader succeeded */

    /* The player: the next frame to decode, and the sequences whose sound is
     * already in the ring. */
    unsigned long frame, audio_written;
    int picture_valid[2]; /* the screen / TV picture shows the map */

    /* Sound. */
    signed char *ring;
    int sound;
    int sound_started;             /* sound_start was called */
    int paused;                    /* 1: waiting for a chunk; 2: holding at the start of a loop */
    unsigned long audio_since;     /* when playing started */
    unsigned long audio_paused_ms; /* time spent paused so far */
    unsigned long pause_at;        /* when the current pause began */

    /* Diagnostics (read_ms and read_max_ms are the host's to fill in, since
     * only the reader thread can time itself). */
    unsigned long chunks_read, read_ms, read_max_ms;
    unsigned long stalls, stall_ms; /* waits for a chunk mid-clip */
    int loop_hold;                  /* holding at a loop until every slot fills (not a stall) */
    unsigned long loop_holds, loop_hold_ms;
    unsigned long spare_blocks; /* blocks read while pace said "spare" */
    /* The first four stalls: frame, chunks released, the slots' sequences
     * (4 bits each), milliseconds played. */
    unsigned long stall_log[4][4];
};

/* Bytes a player allocates for a clip with this header and slot count. */
unsigned long om3dvid7_player_bytes(const Om3dvid7Header *header, unsigned int slots);

/*
 * Starts playing the clip whose header block the caller has read and
 * checked (with om3dvid7_header); `host` is copied. Allocates memory, starts
 * the reader, waits until every slot holds a chunk (asking host->cancel
 * while it waits) and draws the first frame into the TV picture. `sound` 0
 * plays silently.
 *
 * Returns 0, -1 on an error, OM3DVID7_PLAYER_NO_MEMORY or
 * OM3DVID7_PLAYER_CANCELED. On failure the player is closed.
 */
int om3dvid7_player_open(Om3dvid7Player *player, const Om3dvid7PlayerHost *host,
                         const unsigned char *header_block, int sound);

/* Starts the clock and the sound, as the first frame appears. */
void om3dvid7_player_play(Om3dvid7Player *player, unsigned long now_ms);

/*
 * Decodes up to and including frame `frame` (counted across loops) into the
 * picture (OM3DVID7_SCREEN into `screen`, or OM3DVID7_TV), first redrawing
 * it whole if it does not show the map yet. Returns 0,
 * OM3DVID7_PLAYER_NOT_READY if a chunk has not arrived (nothing past it was
 * decoded), or -1 on an error.
 */
int om3dvid7_player_decode(Om3dvid7Player *player, unsigned long frame, int picture,
                           unsigned char *screen);

/* Notes that a picture no longer shows the map (something was drawn over it). */
void om3dvid7_player_invalidate(Om3dvid7Player *player, int picture);

/* Keeps the sound ring filled, pauses the sound while the video waits for a
 * chunk (`waiting`), and frees finished slots. Call on every pass of the
 * main loop. */
void om3dvid7_player_update(Om3dvid7Player *player, unsigned long now_ms, int waiting);

/* The frame due now, counted across loops, by the sound's clock (0 until
 * om3dvid7_player_play). */
unsigned long om3dvid7_player_due(const Om3dvid7Player *player, unsigned long now_ms);

/* Stops the reader and the sound and frees everything; the diagnostics are
 * kept. Safe on a closed or zeroed player. */
void om3dvid7_player_close(Om3dvid7Player *player);

/*
 * For the host's reader thread: reads the next chunk if a slot is free for
 * it. Returns 0 after reading one, OM3DVID7_PLAYER_READ_LATER if there is
 * nothing to read yet, OM3DVID7_PLAYER_READ_STOP once the player asks the
 * reader to stop, or -1 after a failure (player->error is set).
 */
int om3dvid7_player_read(Om3dvid7Player *player);

/* For the host's reader thread, as it finishes: closes the file if it is
 * open. (Skip it when the source can no longer be trusted to answer.) */
void om3dvid7_player_close_file(Om3dvid7Player *player);

/* For the host's reader thread, the very last thing it does: marks it as
 * finished, after which stop_reader may delete it. */
void om3dvid7_player_reader_exit(Om3dvid7Player *player);
#endif
