/*
 * Streaming OM3DVID7 previews from the USB drive. See preview_stream.h for
 * how the reader, the slots and the player fit together.
 *
 * The file is in three parts: reading (the reader thread on the console, or
 * on-the-spot reads on a PC), sound, and playing.
 */
#include "preview_stream.h"
#include "usb_file.h"
#include <stdlib.h>
#include <string.h>
#ifdef OH3DO_SDK
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
#define READER_SPARE_CHUNKS   2 /* "not needed for a while": this many chunks ahead */

/* Playback holds this many frames before a chunk that has not arrived, so
 * the sound pauses before it runs out and both resume in step. */
#define LOOKAHEAD 3

#define READER_STACK 8192

/* The sound is written at most this far ahead of what the DSP has played;
 * the rest of the ring is what it may still be playing. */
#define RING_GUARD 2048UL

/* Spare bytes after each slot: the decoder reads up to two bytes past a packet. */
#define SLOT_SLACK 4UL

static unsigned int read_be16(const unsigned char *p)
{
    return ((unsigned int)p[0] << 8) | p[1];
}

/* Kinds of memory: main, video, CEL (drawable) and sound memory. PCs just use malloc. */
enum { MEM_DRAM, MEM_VRAM, MEM_CEL, MEM_AUDIO };

static void *allocate(unsigned long bytes, int kind)
{
#ifdef OH3DO_SDK
    static const unsigned long types[] = {MEMTYPE_DRAM, MEMTYPE_VRAM, MEMTYPE_CEL, MEMTYPE_AUDIO};
    return AllocMem((int32)bytes, types[kind]);
#else
    (void)kind;
    return malloc((size_t)bytes);
#endif
}

static void release(void *data, unsigned long bytes)
{
    if (!data)
        return;
#ifdef OH3DO_SDK
    FreeMem(data, (int32)bytes); /* Portfolio needs the allocated size */
#else
    (void)bytes;
    free(data);
#endif
}

/* Sizes of the decoder's buffers, in the order they are allocated. */
#define MAP_BYTES    (PREVIEW7_BLOCKS * 2UL * sizeof(unsigned long))
#define DICT4_BYTES  (PREVIEW7_MAX_ENTRIES4 * 32UL)
#define THUMB4_BYTES (PREVIEW7_MAX_ENTRIES4 * 8UL)
#define DICT2_BYTES  (PREVIEW7_MAX_ENTRIES2 * 8UL)
#define THUMB2_BYTES (PREVIEW7_MAX_ENTRIES2 * sizeof(unsigned long))

unsigned long preview_stream_bytes(const Preview7Header *header, unsigned int slots)
{
    return MAP_BYTES + DICT4_BYTES + THUMB4_BYTES + DICT2_BYTES + THUMB2_BYTES +
           PREVIEW7_THUMB_BYTES + slots * (header->max_chunk + SLOT_SLACK) + PREVIEW_STREAM_RING;
}

static void free_all(PreviewStream *stream)
{
    unsigned int i;
    for (i = 0; i < stream->slots; ++i)
        release(stream->slot[i], stream->slot_bytes);
    release(stream->dec.map, MAP_BYTES);
    release(stream->dec.dict4, DICT4_BYTES);
    release(stream->dec.thumb4, THUMB4_BYTES);
    release(stream->dec.dict2, DICT2_BYTES);
    release(stream->dec.thumb2, THUMB2_BYTES);
    release(stream->thumb, PREVIEW7_THUMB_BYTES);
    release(stream->ring, PREVIEW_STREAM_RING);
}

/*
 * Allocates `bytes` of `kind`. Main memory falls back to video memory (and
 * then `*in_vram`, if given, is set). If both fail, frees caches through
 * `reclaim` and tries once more.
 */
static void *allocate_with_fallback(unsigned long bytes, int kind, PreviewStreamReclaim reclaim,
                                    void *context, unsigned char *in_vram)
{
    void *memory = allocate(bytes, kind);

    if (in_vram)
        *in_vram = 0;
    if (!memory && kind == MEM_DRAM) {
        memory = allocate(bytes, MEM_VRAM);
        if (memory && in_vram)
            *in_vram = 1;
    }
    if (!memory && reclaim && reclaim(context))
        memory = allocate(bytes, kind);
    return memory;
}

/* The chunk number within the clip, and its length in bytes, for a sequence. */
static unsigned int chunk_of(const PreviewStream *stream, unsigned long sequence)
{
    return (unsigned int)(sequence % stream->header.chunks);
}

static unsigned long chunk_bytes(const PreviewStream *stream, unsigned long sequence)
{
    return read_be16(stream->header.table + 2 * chunk_of(stream, sequence)) * PREVIEW7_BLOCK_BYTES;
}

/* The first frame (counted across loops) of a sequence's chunk. */
static unsigned long sequence_frame(const PreviewStream *stream, unsigned long sequence)
{
    return (sequence / stream->header.chunks) * stream->header.frames +
           (sequence % stream->header.chunks) * (unsigned long)stream->header.chunk_frames;
}

/* The sequence holding frame `frame` (counted across loops). */
static unsigned long frame_sequence(const PreviewStream *stream, unsigned long frame)
{
    return (frame / stream->header.frames) * stream->header.chunks +
           (frame % stream->header.frames) / stream->header.chunk_frames;
}

/* ---- Reading ------------------------------------------------------------ */

static void adjust_reader_priority(PreviewStream *stream, unsigned long sequence);

/*
 * Reads sequence `sequence` into its slot, a block at a time, summing it as
 * it arrives. The file must be open at the start of that chunk.
 */
static int read_chunk_bytes(PreviewStream *stream, OdeReadBuffer reader, unsigned long sequence)
{
    unsigned int slot = (unsigned int)(sequence % stream->slots);
    unsigned long bytes = chunk_bytes(stream, sequence), at;
    Preview7Sum sum;

    sum.a = sum.b = 0;
    stream->reading = 1;
    for (at = 0; at < bytes; at += PREVIEW7_BLOCK_BYTES) {
        unsigned char *block = stream->slot[slot] + at;
        adjust_reader_priority(stream, sequence);
        if (usb_file_read_block(&stream->ode, reader, block) < 0) {
            stream->reading = 0;
            return -1;
        }
        /* The chunk's own check, its first word, is not part of the sum. */
        if (at == 0)
            preview7_sum_add(&sum, block + 4, PREVIEW7_BLOCK_BYTES - 4);
        else
            preview7_sum_add(&sum, block, PREVIEW7_BLOCK_BYTES);
    }
    stream->reading = 0;
    stream->slot_sum[slot] = preview7_sum_value(&sum);
    return 0;
}

/* Checks the chunk just read, stores its entries and hands it to the player. */
static int finish_chunk(PreviewStream *stream, unsigned long sequence)
{
    unsigned int slot = (unsigned int)(sequence % stream->slots);

    if (preview7_chunk_summed(&stream->header, chunk_of(stream, sequence), stream->slot[slot],
                              chunk_bytes(stream, sequence), stream->slot_sum[slot],
                              &stream->info[slot]))
        return -1;
    preview7_define(&stream->dec, &stream->info[slot]);
    ++stream->chunks_read;
    stream->ready[slot] = sequence + 1; /* last: this publishes the chunk to the player */
    return 0;
}

static int read_chunk(PreviewStream *stream, OdeReadBuffer reader, unsigned long sequence)
{
    if (read_chunk_bytes(stream, reader, sequence) < 0)
        return -1;
    return finish_chunk(stream, sequence);
}

/* Opens the file and reads past its header block, which must not have
 * changed since the stream was opened. */
static int open_file(PreviewStream *stream, OdeReadBuffer reader)
{
    static unsigned char block[PREVIEW7_BLOCK_BYTES]; /* static: too big for the stack */

    if (usb_file_open(&stream->ode, stream->name) < 0)
        return -1;
    if (usb_file_read(&stream->ode, reader, block, PREVIEW7_BLOCK_BYTES, USB_FILE_CRC_CHECKED) <
            0 ||
        memcmp(block, stream->block, PREVIEW7_BLOCK_BYTES)) {
        usb_file_close(&stream->ode);
        return -1;
    }
    return 0;
}

/* Before reading the first chunk of a loop, (re)open the file at its start;
 * the FCare cannot seek back. */
static int prepare_for(PreviewStream *stream, OdeReadBuffer reader, unsigned long sequence)
{
    if (chunk_of(stream, sequence) != 0)
        return 0;
    if (sequence > 0 && usb_file_close(&stream->ode) < 0)
        return -1;
    return open_file(stream, reader);
}

#ifdef OH3DO_SDK
/* The stream the reader thread serves (one at a time), and whether it is at
 * the lower priority now. */
static PreviewStream *reader_stream;
static int reader_spare;

/* Reads at the lower priority while the chunk is not needed for a while
 * (see READER_SPARE_PRIORITY). Loading before the first frame, or while
 * playback waits, is always urgent. */
static void adjust_reader_priority(PreviewStream *stream, unsigned long sequence)
{
    unsigned long needed_at = sequence_frame(stream, sequence);
    int spare = *(volatile int *)&stream->playing && !*(volatile int *)&stream->paused &&
                needed_at > *(volatile unsigned long *)&stream->frame +
                                READER_SPARE_CHUNKS * (unsigned long)stream->header.chunk_frames;

    if (spare != reader_spare) {
        reader_spare = spare;
        SetItemPri(CURRENTTASK->t.n_Item, (uint8)(spare ? READER_SPARE_PRIORITY : READER_PRIORITY));
    }
    if (spare)
        ++stream->spare_blocks;
}

/* The reader thread: read chunk after chunk while there are free slots. */
static void reader_main(void)
{
    PreviewStream *stream = reader_stream;
    OdeBus bus;
    unsigned long sequence = 0;

    reader_spare = 0;
    if (ode_bus_open(&bus) < 0) {
        stream->error = 1;
        stream->running = 0;
        return;
    }
    /* Its own bus connection, waiting for completions rather than polling. */
    bus.blocking = 1;
    stream->ode.transfer = ode_bus_transfer;
    stream->ode.context = &bus;
    stream->ode.progress = NULL;

    while (!stream->stop) {
        unsigned long started, spent;

        /* Every slot is full: wait for the player to free one. */
        if (sequence >= stream->released + stream->slots) {
            SleepMSec(bus.timer, 4);
            continue;
        }
        /*
         * Start the next loop only once this one has played: the clip's end
         * plays without the reader competing, then the picture holds while
         * every slot fills (see decode_to).
         */
        if (sequence && !chunk_of(stream, sequence) &&
            *(volatile unsigned long *)&stream->frame < sequence_frame(stream, sequence)) {
            SleepMSec(bus.timer, 4);
            continue;
        }

        started = (unsigned long)GetMSecTime(bus.timer);
        if (prepare_for(stream, ode_bus_read_buffer, sequence) < 0 ||
            read_chunk_bytes(stream, ode_bus_read_buffer, sequence) < 0) {
            stream->reader_error_code = (long)bus.last_error;
            stream->error = 2;
            break;
        }
        spent = (unsigned long)GetMSecTime(bus.timer) - started;
        stream->read_ms += spent;
        if (spent > stream->read_max_ms)
            stream->read_max_ms = spent;
        if (finish_chunk(stream, sequence) < 0) {
            stream->error = 2;
            break;
        }
        ++sequence;
    }

    if (!bus.timed_out)
        usb_file_close(&stream->ode);
    stream->bus_commands = bus.commands;
    ode_bus_close(&bus);
    stream->running = 0;
}

/*
 * Starts the reader as a thread that may use the XBUS: CreateThread cannot
 * grant the privilege it needs, so the task item is created directly. (The
 * kernel's CREATETASK_TAG_SUPER is CREATETASK_TAG_PRIVATE0 in the public
 * headers.) Returns the thread's item, or a negative error.
 */
static long start_reader(PreviewStream *stream)
{
    TagArg tags[8];
    unsigned char *stack = (unsigned char *)AllocMem(READER_STACK, MEMTYPE_ANY);
    Item item;

    if (!stack)
        return -1;
    reader_stream = stream;
    tags[0].ta_Tag = TAG_ITEM_PRI;
    tags[0].ta_Arg = (void *)READER_PRIORITY;
    tags[1].ta_Tag = TAG_ITEM_NAME;
    tags[1].ta_Arg = (void *)"Oh My 3DO preview reader";
    tags[2].ta_Tag = CREATETASK_TAG_PC;
    tags[2].ta_Arg = (void *)reader_main;
    tags[3].ta_Tag = CREATETASK_TAG_STACKSIZE;
    tags[3].ta_Arg = (void *)READER_STACK;
    tags[4].ta_Tag = CREATETASK_TAG_SP;
    tags[4].ta_Arg = (void *)(stack + READER_STACK); /* stacks grow downward */
    tags[5].ta_Tag = CREATETASK_TAG_ALLOCDTHREADSP;
    tags[5].ta_Arg = 0;
    tags[6].ta_Tag = CREATETASK_TAG_PRIVATE0;
    tags[6].ta_Arg = 0;
    tags[7].ta_Tag = TAG_END;

    stream->running = 1;
    item = CreateItem(MKNODEID(KERNELNODE, TASKNODE), tags);
    if (item < 0) {
        FreeMem(stack, READER_STACK);
        stream->running = 0;
    }
    return item;
}

static void sleep_ms(unsigned long ms)
{
    Item timer = GetTimerIOReq();
    if (timer >= 0) {
        SleepMSec(timer, ms);
        DeleteIOReq(timer);
    }
}
#else
/* PCs read on the spot, so there is no priority to adjust. */
static void adjust_reader_priority(PreviewStream *stream, unsigned long sequence)
{
    (void)stream;
    (void)sequence;
}

/* PCs: the reader to use, set by preview_stream_open. */
static OdeReadBuffer host_reader;

/* PCs: read every chunk there is a free slot for, now. */
static void read_ahead(PreviewStream *stream)
{
    while (!stream->error && stream->next_read < stream->released + stream->slots) {
        if (prepare_for(stream, host_reader, stream->next_read) < 0 ||
            read_chunk(stream, host_reader, stream->next_read) < 0) {
            stream->error = 2;
            return;
        }
        ++stream->next_read;
    }
}
#endif

/* Lets a PC build catch up on reading; the console's thread needs nothing. */
static void read_if_needed(PreviewStream *stream)
{
#ifdef OH3DO_SDK
    (void)stream;
#else
    read_ahead(stream);
#endif
}

/* ---- Sound -------------------------------------------------------------- */

/* Audio samples from the start of the clip to frame `frame` (within one loop). */
static unsigned long samples_to(unsigned long frame)
{
    return frame * PREVIEW7_AUDIO_RATE / PREVIEW7_FPS;
}

/* Audio samples from the first start to frame `frame`, across loops. */
static unsigned long clip_samples(const PreviewStream *stream, unsigned long frame)
{
    return (frame / stream->header.frames) * samples_to(stream->header.frames) +
           samples_to(frame % stream->header.frames);
}

/* ms * rate / 1000, without overflowing 32 bits after a few minutes. */
static unsigned long scale_ms(unsigned long ms, unsigned long rate)
{
    return (ms / 1000UL) * rate + (ms % 1000UL) * rate / 1000UL;
}

/* Milliseconds of play so far, not counting pauses. */
static unsigned long played_ms(const PreviewStream *stream, unsigned long now)
{
    unsigned long paused = stream->audio_paused_ms + (stream->paused ? now - stream->pause_at : 0);
    return now - stream->audio_since - paused;
}

/* Copies ready chunks' sound into the ring, in order, while there is room. */
static void fill_ring(PreviewStream *stream, unsigned long now)
{
    unsigned long played = scale_ms(played_ms(stream, now), PREVIEW7_AUDIO_RATE);

    for (;;) {
        unsigned long sequence = stream->audio_written;
        unsigned long slot = sequence % stream->slots;
        const Preview7Chunk *info;
        unsigned long start, count, i;

        if (sequence >= stream->released + stream->slots || stream->ready[slot] != sequence + 1)
            return; /* the next chunk has not arrived */
        info = &stream->info[slot];
        count = info->audio_samples;
        start = clip_samples(stream, sequence_frame(stream, sequence));
        if (start + count > played + PREVIEW_STREAM_RING - RING_GUARD)
            return; /* no room yet: the DSP may still be playing that part */
        if (stream->ring) {
            for (i = 0; i < count; ++i)
                stream->ring[(start + i) % PREVIEW_STREAM_RING] = info->audio[i];
        }
        ++stream->audio_written;
    }
}

/* Frees slots whose frames are all decoded and whose sound is in the ring. */
static void release_finished(PreviewStream *stream)
{
    for (;;) {
        unsigned long sequence = stream->released;
        unsigned int slot = (unsigned int)(sequence % stream->slots);

        if (stream->ready[slot] != sequence + 1 ||
            sequence_frame(stream, sequence) + stream->info[slot].frames > stream->frame)
            return;
        if (stream->sound && sequence >= stream->audio_written)
            return;
        stream->released = sequence + 1;
    }
}

/* Records a new stall for diagnostics (the first four in detail). */
static void log_stall(PreviewStream *stream, unsigned long now)
{
    unsigned long *entry = stream->stall_log[stream->stalls];
    unsigned long slots = 0;
    unsigned int k;

    for (k = 0; k < stream->slots && k < 8; ++k)
        slots |= (stream->ready[k] & 15UL) << (4 * k);
    entry[0] = stream->frame;
    entry[1] = stream->released;
    entry[2] = slots;
    entry[3] = played_ms(stream, now);
}

void preview_stream_audio(PreviewStream *stream, unsigned long now, int waiting)
{
    if (!stream->playing)
        return;

    if (waiting && !stream->paused) {
        /* The video has to wait: pause the sound with it. */
        stream->paused = stream->loop_hold ? 2 : 1;
        stream->pause_at = now;
        preview_audio_pause(&stream->audio, 1);
        if (!stream->loop_hold && stream->stalls < 4)
            log_stall(stream, now);
        if (stream->loop_hold)
            ++stream->loop_holds;
        else
            ++stream->stalls;
    } else if (!waiting && stream->paused) {
        /* The chunk arrived: resume. */
        if (stream->paused == 2)
            stream->loop_hold_ms += now - stream->pause_at;
        else
            stream->stall_ms += now - stream->pause_at;
        stream->paused = 0;
        stream->audio_paused_ms += now - stream->pause_at;
        preview_audio_pause(&stream->audio, 0);
    }

    if (!stream->paused && stream->sound)
        fill_ring(stream, now);
    release_finished(stream);
}

unsigned long preview_stream_due(PreviewStream *stream, unsigned long now)
{
    return stream->playing ? scale_ms(played_ms(stream, now), PREVIEW7_FPS) : 0;
}

void preview_stream_play(PreviewStream *stream, unsigned long now)
{
    if (!stream->open || stream->playing)
        return;
    stream->audio_since = now;
    stream->playing = 1;
    if (stream->sound) {
        fill_ring(stream, now);
        if (preview_audio_start_ring(&stream->audio, (unsigned char *)stream->ring,
                                     PREVIEW_STREAM_RING, PREVIEW7_AUDIO_RATE) < 0)
            stream->sound = 0; /* play on silently */
    }
}

/* ---- Playing ------------------------------------------------------------ */

/*
 * Whether playback must wait before decoding toward `frame`: the chunk a few
 * frames ahead has not arrived (so the sound pauses in time), or a new loop
 * is starting and its slots are not all filled yet. Sets loop_hold for the
 * latter.
 */
static int must_wait(PreviewStream *stream, unsigned long frame)
{
    unsigned long ahead = frame + LOOKAHEAD;
    unsigned long sequence = frame_sequence(stream, ahead);

    read_if_needed(stream);
    stream->loop_hold = 0;

    /* Only within this loop: the next loop's chunks wait for this one to end. */
    if (ahead / stream->header.frames == frame / stream->header.frames &&
        stream->ready[sequence % stream->slots] != sequence + 1 &&
        sequence < stream->released + stream->slots)
        return 1;

    if (stream->frame && !(stream->frame % stream->header.frames)) {
        /* Start each loop with every slot full, as the first start does, so
         * its opening chunks are not being read while they play. */
        unsigned long first = (stream->frame / stream->header.frames) * stream->header.chunks;
        unsigned long count =
            stream->slots < stream->header.chunks ? stream->slots : stream->header.chunks;
        unsigned long j;

        read_if_needed(stream);
        for (j = 0; j < count; ++j) {
            if (stream->ready[(first + j) % stream->slots] != first + j + 1) {
                stream->loop_hold = 1;
                return 1;
            }
        }
    }
    return 0;
}

static int decode_to(PreviewStream *stream, unsigned long frame, int picture, unsigned char *screen)
{
    int picture_index = picture == PREVIEW7_TV;

    if (stream->error)
        return -1;
    stream->dec.screen = screen;
    if (!stream->picture_valid[picture_index]) {
        preview7_render(&stream->dec, picture);
        stream->picture_valid[picture_index] = 1;
    }

    if (stream->frame <= frame && must_wait(stream, frame))
        return stream->error ? -1 : PREVIEW_STREAM_NOT_READY;

    while (stream->frame <= frame) {
        unsigned long frame_in_loop = stream->frame % stream->header.frames;
        unsigned long sequence = frame_sequence(stream, stream->frame);
        unsigned int slot = (unsigned int)(sequence % stream->slots);

        read_if_needed(stream);
        if (stream->ready[slot] != sequence + 1)
            return stream->error ? -1 : PREVIEW_STREAM_NOT_READY;

        if (frame_in_loop == 0 && stream->frame) {
            /* Each loop decodes from black, as the encoder did. */
            preview7_reset(&stream->dec);
            preview7_render(&stream->dec, picture);
            stream->picture_valid[!picture_index] = 0;
        }
        if (preview7_frame(&stream->dec, &stream->info[slot],
                           (unsigned int)(frame_in_loop % stream->header.chunk_frames), picture)) {
            stream->error = 3;
            return -1;
        }
        /* Only this picture now shows the map. */
        stream->picture_valid[!picture_index] = 0;

        /* A finished chunk frees its slot for the reader at once. */
        ++stream->frame;
        if (stream->frame == sequence_frame(stream, sequence) + stream->info[slot].frames)
            release_finished(stream);
    }
    release_finished(stream);
    return 0;
}

int preview_stream_decode(PreviewStream *stream, unsigned long frame, int picture,
                          unsigned char *screen)
{
    return stream->open ? decode_to(stream, frame, picture, screen) : -1;
}

void preview_stream_invalidate(PreviewStream *stream, int picture)
{
    stream->picture_valid[picture == PREVIEW7_TV] = 0;
}

void preview_stream_close(PreviewStream *stream)
{
    PreviewStream cleared;

#ifdef OH3DO_SDK
    /* Ask the reader to stop and give it up to 5 seconds. */
    if (stream->thread > 0) {
        unsigned long waited = 0;
        stream->stop = 1;
        while (stream->running && waited < 5000UL) {
            sleep_ms(5);
            waited += 5;
        }
        DeleteThread((Item)stream->thread);
        if (stream->running)
            stream->error = 4;
    }
#endif
    preview_audio_close(&stream->audio);
    free_all(stream);

    /* Clear everything except the diagnostics. */
    memset(&cleared, 0, sizeof(cleared));
    cleared.chunks_read = stream->chunks_read;
    cleared.read_ms = stream->read_ms;
    cleared.read_max_ms = stream->read_max_ms;
    cleared.stalls = stream->stalls;
    cleared.stall_ms = stream->stall_ms;
    cleared.error = stream->error;
    cleared.loop_holds = stream->loop_holds;
    cleared.loop_hold_ms = stream->loop_hold_ms;
    cleared.spare_blocks = stream->spare_blocks;
    memcpy(cleared.stall_log, stream->stall_log, sizeof(cleared.stall_log));
    cleared.reader_error_code = stream->reader_error_code;
    cleared.bus_commands = stream->bus_commands;
    cleared.slots = stream->slots;
    cleared.decode_ms = stream->decode_ms;
    cleared.decode_frames = stream->decode_frames;
    cleared.idle_ms = stream->idle_ms;
    cleared.idle_frames = stream->idle_frames;
    cleared.full_ms = stream->full_ms;
    cleared.full_frames = stream->full_frames;
    *stream = cleared;
}

/* Allocates the decoder, the TV picture and the sound ring. Returns 0, or -4. */
static int allocate_decoder(PreviewStream *stream, PreviewStreamReclaim reclaim,
                            void *reclaim_context)
{
    /* The decoder first: it is used on every frame. */
    stream->dec.map =
        (unsigned long *)allocate_with_fallback(MAP_BYTES, MEM_DRAM, reclaim, reclaim_context, 0);
    stream->dec.dict4 =
        (unsigned char *)allocate_with_fallback(DICT4_BYTES, MEM_DRAM, reclaim, reclaim_context, 0);
    stream->dec.thumb4 = (unsigned char *)allocate_with_fallback(THUMB4_BYTES, MEM_DRAM, reclaim,
                                                                 reclaim_context, 0);
    stream->dec.dict2 =
        (unsigned char *)allocate_with_fallback(DICT2_BYTES, MEM_DRAM, reclaim, reclaim_context, 0);
    stream->dec.thumb2 = (unsigned long *)allocate_with_fallback(THUMB2_BYTES, MEM_DRAM, reclaim,
                                                                 reclaim_context, 0);
    stream->thumb = (unsigned char *)allocate_with_fallback(PREVIEW7_THUMB_BYTES, MEM_CEL, reclaim,
                                                            reclaim_context, 0);
    if (stream->sound)
        stream->ring = (signed char *)allocate_with_fallback(PREVIEW_STREAM_RING, MEM_AUDIO,
                                                             reclaim, reclaim_context, 0);
    stream->dec.thumb = stream->thumb;
    stream->dec.entries4 = PREVIEW7_MAX_ENTRIES4;
    stream->dec.entries2 = PREVIEW7_MAX_ENTRIES2;

    if (!stream->dec.map || !stream->dec.dict4 || !stream->dec.thumb4 || !stream->dec.dict2 ||
        !stream->dec.thumb2 || !stream->thumb || (stream->sound && !stream->ring))
        return -4;
    return 0;
}

/* Allocates as many chunk slots as the encoder assumed, video memory first.
 * Returns 0, or -4 if too few fit to stream at all. */
static int allocate_slots(PreviewStream *stream, PreviewStreamReclaim reclaim,
                          void *reclaim_context)
{
    unsigned int wanted, i;

    stream->slot_bytes = stream->header.max_chunk + SLOT_SLACK;
    wanted =
        stream->header.slots < PREVIEW_STREAM_SLOTS ? stream->header.slots : PREVIEW_STREAM_SLOTS;
    for (i = 0; i < wanted; ++i) {
        stream->slot[i] = (unsigned char *)allocate(stream->slot_bytes, MEM_VRAM);
        stream->slot_vram[i] = 1;
        if (!stream->slot[i]) {
            stream->slot[i] = (unsigned char *)allocate_with_fallback(
                stream->slot_bytes, MEM_DRAM, reclaim, reclaim_context, &stream->slot_vram[i]);
        }
        if (!stream->slot[i])
            break;
        /* The slack after the largest chunk is always zero. */
        stream->slot[i][stream->header.max_chunk] = 0;
        stream->slot[i][stream->header.max_chunk + 1] = 0;
        ++stream->slots;
    }

    /*
     * Streaming needs at least two slots: one playing while another is read.
     * Fewer slots than the encoder assumed only means longer waits.
     */
    if (stream->slots < 2 && stream->slots < stream->header.chunks)
        return -4;
    return 0;
}

int preview_stream_open(PreviewStream *stream, const Ode *ode, OdeReadBuffer reader,
                        const char *game_folder, const char *name,
                        const unsigned char *header_block, int sound, PreviewStreamCancel cancel,
                        void *cancel_context, PreviewStreamReclaim reclaim, void *reclaim_context)
{
    unsigned int wanted, ready;
    int result = -1;

    memset(stream, 0, sizeof(*stream));
    if (strlen(game_folder) >= sizeof(stream->folder) || strlen(name) >= sizeof(stream->name))
        return -1;

    /* Keep our own copy of the header block (the table points into it). */
    memcpy(stream->block, header_block, PREVIEW7_BLOCK_BYTES);
    if (preview7_header(stream->block, &stream->header))
        return -1;
    stream->header.table = stream->block + PREVIEW7_HEADER_BYTES;
    strcpy(stream->folder, game_folder);
    strcpy(stream->name, name);

    /* The reader's copy of the connection opens media beside the disc. */
    stream->ode = *ode;
    stream->ode.navigation_failed = 0;
    stream->ode.media_beside = 1;
    stream->ode.media_folder = stream->folder[0] ? stream->folder : NULL;
    stream->sound = sound && stream->header.audio_rate;

    result = allocate_decoder(stream, reclaim, reclaim_context);
    if (result == 0)
        result = allocate_slots(stream, reclaim, reclaim_context);
    if (result < 0)
        goto failed;
    result = -1;
    if (stream->ring)
        memset(stream->ring, 0, PREVIEW_STREAM_RING);
    preview7_reset(&stream->dec);

#ifdef OH3DO_SDK
    (void)reader;
    stream->thread = start_reader(stream);
    if (stream->thread < 0)
        goto failed;
#else
    host_reader = reader;
#endif

    /*
     * Wait until every slot (at least the clip's preroll) holds a chunk. The
     * first chunks carry most of the dictionary, and checking them uses the
     * menu's spare time, which is short just after a preview starts.
     */
    wanted = stream->slots < stream->header.chunks ? stream->slots : stream->header.chunks;
    for (;;) {
        for (ready = 0; ready < wanted && stream->ready[ready] == ready + 1; ++ready) {
        }
        if (ready == wanted)
            break;
        if (stream->error)
            goto failed;
        if (cancel && cancel(cancel_context)) {
            result = -5;
            goto failed;
        }
#ifdef OH3DO_SDK
        sleep_ms(5);
#else
        read_ahead(stream);
#endif
    }

    if (decode_to(stream, 0, PREVIEW7_TV, NULL))
        goto failed;
    stream->open = 1;
    return 0;

failed:
    preview_stream_close(stream);
    return result;
}
