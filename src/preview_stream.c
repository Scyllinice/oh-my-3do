/*
 * The menu's side of streaming OM3DVID7 previews: the player's host
 * callbacks. See preview_stream.h.
 */
#include "preview_stream.h"
#include "usb_file.h"
#include <string.h>
#ifdef OH3DO_SDK
#include "om3dvid7_portfolio.h"
#include "mem.h"
#include "kernel.h"
#include "task.h"
#include "io.h"
#include "item.h"
#include "nodes.h"
#include "kernelnodes.h"
#include "timerutils.h"
#include "ode_bus.h"
#endif

/* ---- Reading the clip through the FCare --------------------------------- */

static int open_clip(void *context)
{
    PreviewStream *stream = (PreviewStream *)context;
    return usb_file_open(&stream->ode, stream->name);
}

static int read_clip_block(void *context, unsigned char *block)
{
    PreviewStream *stream = (PreviewStream *)context;
    return usb_file_read_block(&stream->ode, stream->reader, block);
}

static int close_clip(void *context)
{
    PreviewStream *stream = (PreviewStream *)context;
    return usb_file_close(&stream->ode);
}

/* ---- Memory and waiting ------------------------------------------------ */

static int reclaim_memory(void *context)
{
    PreviewStream *stream = (PreviewStream *)context;
    return stream->reclaim && stream->reclaim(stream->reclaim_context);
}

static int cancel_opening(void *context)
{
    PreviewStream *stream = (PreviewStream *)context;
    return stream->cancel && stream->cancel(stream->cancel_context);
}

#ifdef OH3DO_SDK
/* ---- The console's reader thread ---------------------------------------- */

/*
 * The reader thread's priority. It runs above the menu's own task (150), so
 * a finished USB transfer is followed up at once. It spends nearly all its
 * time waiting for the drive: blocks land straight in a slot, are summed as
 * they arrive, and a chunk's entries are stored with plain copies. But the
 * bus moves every byte through the processor, so while the chunk being read
 * is not needed for a while ("spare" reading), the reader drops below the
 * menu and only reads while the menu is idle.
 */
#define READER_PRIORITY       160
#define READER_SPARE_PRIORITY 140

#define READER_STACK 8192

/* How long the reader rests when there is nothing to read yet. */
#define READER_REST_MS 4

/* How long closing waits for the reader to stop. */
#define READER_STOP_MS 5000UL

/* The stream the reader thread serves (one at a time). */
static PreviewStream *reader_stream;

static void pace_reader(void *context, int spare)
{
    (void)context;
    SetItemPri(CURRENTTASK->t.n_Item, (uint8)(spare ? READER_SPARE_PRIORITY : READER_PRIORITY));
}

/* The reader thread: read chunk after chunk while there are free slots. */
static void reader_main(void)
{
    PreviewStream *stream = reader_stream;
    Om3dvid7Player *player = &stream->player;
    OdeBus bus;

    if (ode_bus_open(&bus) < 0) {
        player->error = OM3DVID7_PLAYER_ERROR_START;
        om3dvid7_player_reader_exit(player);
        return;
    }
    /* Its own bus connection, waiting for completions rather than polling. */
    bus.blocking = 1;
    stream->ode.transfer = ode_bus_transfer;
    stream->ode.context = &bus;
    stream->ode.progress = NULL;
    stream->reader = ode_bus_read_buffer;

    for (;;) {
        unsigned long started = (unsigned long)GetMSecTime(bus.timer), spent;
        int result = om3dvid7_player_read(player);

        if (result == OM3DVID7_PLAYER_READ_LATER) {
            SleepMSec(bus.timer, READER_REST_MS);
            continue;
        }
        if (result == OM3DVID7_PLAYER_READ_STOP)
            break;
        if (result < 0) {
            stream->reader_error_code = (long)bus.last_error;
            break;
        }
        spent = (unsigned long)GetMSecTime(bus.timer) - started;
        player->read_ms += spent;
        if (spent > player->read_max_ms)
            player->read_max_ms = spent;
    }

    /* After a timeout the FCare cannot be trusted to answer a close. */
    if (!bus.timed_out)
        om3dvid7_player_close_file(player);
    stream->bus_commands = bus.commands;
    ode_bus_close(&bus);
    om3dvid7_player_reader_exit(player); /* last: the menu may delete the thread now */
}

/* The reader uses the XBUS, so it needs a privileged thread. */
static int start_reader(void *context, Om3dvid7Player *player)
{
    PreviewStream *stream = (PreviewStream *)context;

    (void)player;
    reader_stream = stream;
    stream->thread = om3dvid7_portfolio_start_thread(reader_main, "Oh My 3DO preview reader",
                                                     READER_PRIORITY, READER_STACK, 1);
    return stream->thread < 0 ? -1 : 0;
}

/* Waits for the reader to stop, then deletes it. */
static void stop_reader(void *context, Om3dvid7Player *player)
{
    PreviewStream *stream = (PreviewStream *)context;
    unsigned long waited = 0;

    while (player->running && waited < READER_STOP_MS) {
        om3dvid7_portfolio_sleep(5);
        waited += 5;
    }
    DeleteThread((Item)stream->thread);
    stream->thread = 0;
    if (player->running)
        player->error = OM3DVID7_PLAYER_ERROR_RUNAWAY;
}

static void wait_for_reader(void *context)
{
    (void)context;
    om3dvid7_portfolio_sleep(5);
}
#endif

/* ---- Opening and closing ------------------------------------------------ */

int preview_stream_open(PreviewStream *stream, const Ode *ode, OdeReadBuffer reader,
                        const char *game_folder, const char *name,
                        const unsigned char *header_block, int sound, PreviewStreamCancel cancel,
                        void *cancel_context, PreviewStreamReclaim reclaim, void *reclaim_context)
{
    Om3dvid7PlayerHost host;

    memset(stream, 0, sizeof(*stream));
    if (strlen(game_folder) >= sizeof(stream->folder) || strlen(name) >= sizeof(stream->name))
        return -1;
    strcpy(stream->folder, game_folder);
    strcpy(stream->name, name);
    stream->cancel = cancel;
    stream->cancel_context = cancel_context;
    stream->reclaim = reclaim;
    stream->reclaim_context = reclaim_context;

    /* The reader's copy of the connection opens media beside the disc. */
    stream->ode = *ode;
    stream->ode.navigation_failed = 0;
    stream->ode.media_beside = 1;
    stream->ode.media_folder = stream->folder[0] ? stream->folder : NULL;
    stream->reader = reader;

    memset(&host, 0, sizeof(host));
    host.context = stream;
    host.open = open_clip;
    host.read_block = read_clip_block;
    host.close = close_clip;
    host.reclaim = reclaim_memory;
    host.sound_context = &stream->audio;
    host.sound_start = om3dvid7_sound_player_start;
    host.sound_pause = om3dvid7_sound_player_pause;
    host.sound_stop = om3dvid7_sound_player_stop;
    host.cancel = cancel_opening;
#ifdef OH3DO_SDK
    host.allocate = om3dvid7_portfolio_allocate;
    host.release = om3dvid7_portfolio_release;
    host.pace = pace_reader;
    host.start_reader = start_reader;
    host.stop_reader = stop_reader;
    host.wait = wait_for_reader;
#endif

    return om3dvid7_player_open(&stream->player, &host, header_block, sound);
}

void preview_stream_close(PreviewStream *stream)
{
    om3dvid7_player_close(&stream->player);
}
