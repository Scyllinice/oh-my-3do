/*
 * Streaming OM3DVID7 previews from the USB drive, with the om3dvid library's
 * player (om3dvid7_player.h). This file is the menu's side of it: the clip
 * is read through the FCare ODE, and the sound plays through om3dvid7_sound.
 *
 * On the console, a reader thread with its own XBUS requests reads the
 * chunks, so reading overlaps decoding. On a PC (tests, and the menu preview
 * renderer) there are no threads: chunks are read on the spot whenever the
 * player needs one.
 *
 * While a stream is open the reader owns the ODE: close the stream before
 * any other USB access. Media sits beside the discs (the .ohmy3do folder in
 * the game's folder), and the file is reopened by the same relative path
 * for every loop.
 *
 * Once open, the menu plays it with the om3dvid7_player_* functions on
 * `player`, and closes it with preview_stream_close.
 */
#ifndef OH3DO_PREVIEW_STREAM_H
#define OH3DO_PREVIEW_STREAM_H
#include "menu_config.h"
#include "om3dvid7_player.h"
#include "om3dvid7_sound.h"

/* Asked while waiting; nonzero stops opening the stream. */
typedef int (*PreviewStreamCancel)(void *context);
/* Asked to free optional caches when memory is short; nonzero if it did. */
typedef int (*PreviewStreamReclaim)(void *context);

typedef struct PreviewStream {
    Om3dvid7Player player;

    /* Reading. */
    Ode ode;              /* the reader's own copy of the connection */
    OdeReadBuffer reader; /* how the reader fetches the FCare's buffer */
    char folder[128], name[128];
    long thread;
    PreviewStreamCancel cancel;
    void *cancel_context;
    PreviewStreamReclaim reclaim;
    void *reclaim_context;

    Om3dvid7Sound audio;

    /* Diagnostics: the reader's bus, and decoding time measured by the
     * menu: overall, with the reader idle throughout (no transfers or sums
     * competing for the bus or processor), and for full redraws. */
    unsigned long bus_commands;
    long reader_error_code;
    unsigned long decode_ms, decode_frames, idle_ms, idle_frames, full_ms, full_frames;
} PreviewStream;

/*
 * Starts streaming the clip whose header block the caller has read and
 * checked (with om3dvid7_header). `game_folder` is the collapsed game folder
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

/* Stops the reader and the sound and frees everything; the diagnostics are
 * kept. Safe on a closed or zeroed stream. */
void preview_stream_close(PreviewStream *stream);
#endif
