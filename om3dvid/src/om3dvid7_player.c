/*
 * Playing OM3DVID7 clips while they stream in. See om3dvid7_player.h for
 * how the reader, the slots and the player fit together.
 *
 * The file is in three parts: reading, sound, and playing.
 */
#include "om3dvid7_player.h"
#include <stdlib.h>
#include <string.h>

/* "Not needed for a while" (see Om3dvid7PlayerHost.pace): this many chunks ahead. */
#define SPARE_CHUNKS 2

/* Playback holds this many frames before a chunk that has not arrived, so
 * the sound pauses before it runs out and both resume in step. */
#define LOOKAHEAD 3

/* The sound is written at most this far ahead of what the host has played;
 * the rest of the ring is what it may still be playing. */
#define RING_GUARD 2048UL

/* Spare bytes after each slot: the decoder reads up to two bytes past a packet. */
#define SLOT_SLACK 4UL

static unsigned int read_be16(const unsigned char *p)
{
    return ((unsigned int)p[0] << 8) | p[1];
}

/* ---- Memory ------------------------------------------------------------- */

static void *allocate(Om3dvid7Player *player, unsigned long bytes, int kind)
{
    if (player->host.allocate)
        return player->host.allocate(player->host.context, bytes, kind);
    return malloc((size_t)bytes);
}

static void release(Om3dvid7Player *player, void *memory, unsigned long bytes)
{
    if (!memory)
        return;
    if (player->host.release)
        player->host.release(player->host.context, memory, bytes);
    else
        free(memory);
}

/* Sizes of the decoder's buffers, in the order they are allocated. */
#define MAP_BYTES    (OM3DVID7_BLOCKS * 2UL * sizeof(unsigned long))
#define DICT4_BYTES  (OM3DVID7_MAX_ENTRIES4 * 32UL)
#define THUMB4_BYTES (OM3DVID7_MAX_ENTRIES4 * 8UL)
#define DICT2_BYTES  (OM3DVID7_MAX_ENTRIES2 * 8UL)
#define THUMB2_BYTES (OM3DVID7_MAX_ENTRIES2 * sizeof(unsigned long))

unsigned long om3dvid7_player_bytes(const Om3dvid7Header *header, unsigned int slots)
{
    return MAP_BYTES + DICT4_BYTES + THUMB4_BYTES + DICT2_BYTES + THUMB2_BYTES +
           OM3DVID7_THUMB_BYTES + slots * (header->max_chunk + SLOT_SLACK) + OM3DVID7_PLAYER_RING;
}

static void free_all(Om3dvid7Player *player)
{
    unsigned int i;
    for (i = 0; i < player->slots; ++i)
        release(player, player->slot[i], player->slot_bytes);
    release(player, player->dec.map, MAP_BYTES);
    release(player, player->dec.dict4, DICT4_BYTES);
    release(player, player->dec.thumb4, THUMB4_BYTES);
    release(player, player->dec.dict2, DICT2_BYTES);
    release(player, player->dec.thumb2, THUMB2_BYTES);
    release(player, player->thumb, OM3DVID7_THUMB_BYTES);
    release(player, player->ring, OM3DVID7_PLAYER_RING);
}

/*
 * Allocates `bytes` of `kind`. Main memory falls back to video memory (and
 * then `*in_vram`, if given, is set). If both fail, frees the host's caches
 * through reclaim and tries once more.
 */
static void *allocate_with_fallback(Om3dvid7Player *player, unsigned long bytes, int kind,
                                    unsigned char *in_vram)
{
    void *memory = allocate(player, bytes, kind);

    if (in_vram)
        *in_vram = 0;
    if (!memory && kind == OM3DVID7_MEMORY_MAIN) {
        memory = allocate(player, bytes, OM3DVID7_MEMORY_VIDEO);
        if (memory && in_vram)
            *in_vram = 1;
    }
    if (!memory && player->host.reclaim && player->host.reclaim(player->host.context))
        memory = allocate(player, bytes, kind);
    return memory;
}

/* ---- Sequences ---------------------------------------------------------- */

/* The chunk number within the clip, and its length in bytes, for a sequence. */
static unsigned int chunk_of(const Om3dvid7Player *player, unsigned long sequence)
{
    return (unsigned int)(sequence % player->header.chunks);
}

static unsigned long chunk_bytes(const Om3dvid7Player *player, unsigned long sequence)
{
    return read_be16(player->header.table + 2 * chunk_of(player, sequence)) * OM3DVID7_BLOCK_BYTES;
}

/* The first frame (counted across loops) of a sequence's chunk. */
static unsigned long sequence_frame(const Om3dvid7Player *player, unsigned long sequence)
{
    return (sequence / player->header.chunks) * player->header.frames +
           (sequence % player->header.chunks) * (unsigned long)player->header.chunk_frames;
}

/* The sequence holding frame `frame` (counted across loops). */
static unsigned long frame_sequence(const Om3dvid7Player *player, unsigned long frame)
{
    return (frame / player->header.frames) * player->header.chunks +
           (frame % player->header.frames) / player->header.chunk_frames;
}

/* ---- Reading ------------------------------------------------------------ */

/*
 * Tells the host whether the block about to be read is "spare": playback is
 * running and will not need this chunk for a while. Loading before the first
 * frame, or while playback waits, is always urgent. The player's fields are
 * read as volatile, since with a reader thread the player is changing them.
 */
static void pace_reader(Om3dvid7Player *player, unsigned long sequence)
{
    unsigned long needed_at;
    int spare;

    if (!player->host.pace)
        return;
    needed_at = sequence_frame(player, sequence);
    spare = *(volatile int *)&player->playing && !*(volatile int *)&player->paused &&
            needed_at > *(volatile unsigned long *)&player->frame +
                            SPARE_CHUNKS * (unsigned long)player->header.chunk_frames;
    if (spare != player->spare) {
        player->spare = spare;
        player->host.pace(player->host.context, spare);
    }
    if (spare)
        ++player->spare_blocks;
}

/*
 * Reads sequence `sequence` into its slot, a block at a time, summing it as
 * it arrives. The file must be open at the start of that chunk.
 */
static int read_chunk_bytes(Om3dvid7Player *player, unsigned long sequence)
{
    unsigned int slot = (unsigned int)(sequence % player->slots);
    unsigned long bytes = chunk_bytes(player, sequence), at;
    Om3dvid7Sum sum;

    sum.a = sum.b = 0;
    player->reading = 1;
    for (at = 0; at < bytes; at += OM3DVID7_BLOCK_BYTES) {
        unsigned char *block = player->slot[slot] + at;
        pace_reader(player, sequence);
        if (player->host.read_block(player->host.context, block) < 0) {
            player->reading = 0;
            return -1;
        }
        /* The chunk's own check, its first word, is not part of the sum. */
        if (at == 0)
            om3dvid7_sum_add(&sum, block + 4, OM3DVID7_BLOCK_BYTES - 4);
        else
            om3dvid7_sum_add(&sum, block, OM3DVID7_BLOCK_BYTES);
    }
    player->reading = 0;
    player->slot_sum[slot] = om3dvid7_sum_value(&sum);
    return 0;
}

/* Checks the chunk just read, stores its entries and hands it to the player. */
static int finish_chunk(Om3dvid7Player *player, unsigned long sequence)
{
    unsigned int slot = (unsigned int)(sequence % player->slots);

    if (om3dvid7_chunk_summed(&player->header, chunk_of(player, sequence), player->slot[slot],
                              chunk_bytes(player, sequence), player->slot_sum[slot],
                              &player->info[slot]))
        return -1;
    om3dvid7_define(&player->dec, &player->info[slot]);
    ++player->chunks_read;
    player->ready[slot] = sequence + 1; /* last: this publishes the chunk to the player */
    return 0;
}

static int close_file(Om3dvid7Player *player)
{
    if (!player->file_open)
        return 0;
    player->file_open = 0;
    return player->host.close(player->host.context);
}

/*
 * Opens the file and reads past its header block, which must not have
 * changed since the player was opened. The header is read into the slot
 * that `sequence` is about to fill, which is free.
 */
static int open_file(Om3dvid7Player *player, unsigned long sequence)
{
    unsigned char *block = player->slot[sequence % player->slots];

    if (player->host.open(player->host.context) < 0)
        return -1;
    player->file_open = 1;
    if (player->host.read_block(player->host.context, block) < 0 ||
        memcmp(block, player->block, OM3DVID7_BLOCK_BYTES)) {
        close_file(player);
        return -1;
    }
    return 0;
}

/* Before reading the first chunk of a loop, (re)open the file at its start:
 * the source need not seek back. */
static int prepare_for(Om3dvid7Player *player, unsigned long sequence)
{
    if (chunk_of(player, sequence) != 0)
        return 0;
    if (close_file(player) < 0)
        return -1;
    return open_file(player, sequence);
}

int om3dvid7_player_read(Om3dvid7Player *player)
{
    unsigned long sequence = player->next_read;

    if (player->stop)
        return OM3DVID7_PLAYER_READ_STOP;
    /* Every slot is full: wait for the player to free one. */
    if (sequence >= player->released + player->slots)
        return OM3DVID7_PLAYER_READ_LATER;
    /*
     * Start the next loop only once this one has played: the clip's end
     * plays without the reader competing, then the picture holds while
     * every slot fills (see must_wait).
     */
    if (sequence && !chunk_of(player, sequence) &&
        *(volatile unsigned long *)&player->frame < sequence_frame(player, sequence))
        return OM3DVID7_PLAYER_READ_LATER;

    if (prepare_for(player, sequence) < 0 || read_chunk_bytes(player, sequence) < 0 ||
        finish_chunk(player, sequence) < 0) {
        player->error = OM3DVID7_PLAYER_ERROR_READ;
        return -1;
    }
    ++player->next_read;
    return 0;
}

void om3dvid7_player_close_file(Om3dvid7Player *player)
{
    close_file(player);
}

void om3dvid7_player_reader_exit(Om3dvid7Player *player)
{
    player->running = 0;
}

/* Without a reader thread: read every chunk there is a free slot for, now. */
static void read_if_needed(Om3dvid7Player *player)
{
    if (player->host.start_reader)
        return;
    while (!player->error && om3dvid7_player_read(player) == 0) {
    }
}

/* ---- Sound -------------------------------------------------------------- */

/* Audio samples from the start of the clip to frame `frame` (within one loop). */
static unsigned long samples_to(unsigned long frame)
{
    return frame * OM3DVID7_AUDIO_RATE / OM3DVID7_FPS;
}

/* Audio samples from the first start to frame `frame`, across loops. */
static unsigned long clip_samples(const Om3dvid7Player *player, unsigned long frame)
{
    return (frame / player->header.frames) * samples_to(player->header.frames) +
           samples_to(frame % player->header.frames);
}

/* ms * rate / 1000, without overflowing 32 bits after a few minutes. */
static unsigned long scale_ms(unsigned long ms, unsigned long rate)
{
    return (ms / 1000UL) * rate + (ms % 1000UL) * rate / 1000UL;
}

/* Milliseconds of play so far, not counting pauses. */
static unsigned long played_ms(const Om3dvid7Player *player, unsigned long now)
{
    unsigned long paused = player->audio_paused_ms + (player->paused ? now - player->pause_at : 0);
    return now - player->audio_since - paused;
}

/* Copies ready chunks' sound into the ring, in order, while there is room. */
static void fill_ring(Om3dvid7Player *player, unsigned long now)
{
    unsigned long played = scale_ms(played_ms(player, now), OM3DVID7_AUDIO_RATE);

    for (;;) {
        unsigned long sequence = player->audio_written;
        unsigned long slot = sequence % player->slots;
        const Om3dvid7Chunk *info;
        unsigned long start, count, i;

        if (sequence >= player->released + player->slots || player->ready[slot] != sequence + 1)
            return; /* the next chunk has not arrived */
        info = &player->info[slot];
        count = info->audio_samples;
        start = clip_samples(player, sequence_frame(player, sequence));
        if (start + count > played + OM3DVID7_PLAYER_RING - RING_GUARD)
            return; /* no room yet: the host may still be playing that part */
        if (player->ring) {
            for (i = 0; i < count; ++i)
                player->ring[(start + i) % OM3DVID7_PLAYER_RING] = info->audio[i];
        }
        ++player->audio_written;
    }
}

/* Frees slots whose frames are all decoded and whose sound is in the ring. */
static void release_finished(Om3dvid7Player *player)
{
    for (;;) {
        unsigned long sequence = player->released;
        unsigned int slot = (unsigned int)(sequence % player->slots);

        if (player->ready[slot] != sequence + 1 ||
            sequence_frame(player, sequence) + player->info[slot].frames > player->frame)
            return;
        if (player->sound && sequence >= player->audio_written)
            return;
        player->released = sequence + 1;
    }
}

/* Records a new stall for diagnostics (the first four in detail). */
static void log_stall(Om3dvid7Player *player, unsigned long now)
{
    unsigned long *entry = player->stall_log[player->stalls];
    unsigned long slots = 0;
    unsigned int k;

    for (k = 0; k < player->slots && k < 8; ++k)
        slots |= (player->ready[k] & 15UL) << (4 * k);
    entry[0] = player->frame;
    entry[1] = player->released;
    entry[2] = slots;
    entry[3] = played_ms(player, now);
}

/* What the sound callbacks are given (see Om3dvid7PlayerHost.sound_context). */
static void *sound_context(const Om3dvid7Player *player)
{
    return player->host.sound_context ? player->host.sound_context : player->host.context;
}

static void pause_sound(Om3dvid7Player *player, int pause)
{
    if (player->sound && player->host.sound_pause)
        player->host.sound_pause(sound_context(player), pause);
}

void om3dvid7_player_update(Om3dvid7Player *player, unsigned long now, int waiting)
{
    if (!player->playing)
        return;

    if (waiting && !player->paused) {
        /* The video has to wait: pause the sound with it. */
        player->paused = player->loop_hold ? 2 : 1;
        player->pause_at = now;
        pause_sound(player, 1);
        if (!player->loop_hold && player->stalls < 4)
            log_stall(player, now);
        if (player->loop_hold)
            ++player->loop_holds;
        else
            ++player->stalls;
    } else if (!waiting && player->paused) {
        /* The chunk arrived: resume. */
        if (player->paused == 2)
            player->loop_hold_ms += now - player->pause_at;
        else
            player->stall_ms += now - player->pause_at;
        player->paused = 0;
        player->audio_paused_ms += now - player->pause_at;
        pause_sound(player, 0);
    }

    if (!player->paused && player->sound)
        fill_ring(player, now);
    release_finished(player);
}

unsigned long om3dvid7_player_due(const Om3dvid7Player *player, unsigned long now)
{
    return player->playing ? scale_ms(played_ms(player, now), OM3DVID7_FPS) : 0;
}

void om3dvid7_player_play(Om3dvid7Player *player, unsigned long now)
{
    if (!player->open || player->playing)
        return;
    player->audio_since = now;
    player->playing = 1;
    if (player->sound) {
        fill_ring(player, now);
        player->sound_started = 1;
        if (player->host.sound_start(sound_context(player), player->ring, OM3DVID7_PLAYER_RING,
                                     OM3DVID7_AUDIO_RATE) < 0)
            player->sound = 0; /* play on silently */
    }
}

/* ---- Playing ------------------------------------------------------------ */

/*
 * Whether playback must wait before decoding toward `frame`: the chunk a few
 * frames ahead has not arrived (so the sound pauses in time), or a new loop
 * is starting and its slots are not all filled yet. Sets loop_hold for the
 * latter.
 */
static int must_wait(Om3dvid7Player *player, unsigned long frame)
{
    unsigned long ahead = frame + LOOKAHEAD;
    unsigned long sequence = frame_sequence(player, ahead);

    read_if_needed(player);
    player->loop_hold = 0;

    /* Only within this loop: the next loop's chunks wait for this one to end. */
    if (ahead / player->header.frames == frame / player->header.frames &&
        player->ready[sequence % player->slots] != sequence + 1 &&
        sequence < player->released + player->slots)
        return 1;

    if (player->frame && !(player->frame % player->header.frames)) {
        /* Start each loop with every slot full, as the first start does, so
         * its opening chunks are not being read while they play. */
        unsigned long first = (player->frame / player->header.frames) * player->header.chunks;
        unsigned long count =
            player->slots < player->header.chunks ? player->slots : player->header.chunks;
        unsigned long j;

        read_if_needed(player);
        for (j = 0; j < count; ++j) {
            if (player->ready[(first + j) % player->slots] != first + j + 1) {
                player->loop_hold = 1;
                return 1;
            }
        }
    }
    return 0;
}

static int decode_to(Om3dvid7Player *player, unsigned long frame, int picture,
                     unsigned char *screen)
{
    int picture_index = picture == OM3DVID7_TV;

    if (player->error)
        return -1;
    player->dec.screen = screen;
    if (!player->picture_valid[picture_index]) {
        om3dvid7_render(&player->dec, picture);
        player->picture_valid[picture_index] = 1;
    }

    if (player->frame <= frame && must_wait(player, frame))
        return player->error ? -1 : OM3DVID7_PLAYER_NOT_READY;

    while (player->frame <= frame) {
        unsigned long frame_in_loop = player->frame % player->header.frames;
        unsigned long sequence = frame_sequence(player, player->frame);
        unsigned int slot = (unsigned int)(sequence % player->slots);

        read_if_needed(player);
        if (player->ready[slot] != sequence + 1)
            return player->error ? -1 : OM3DVID7_PLAYER_NOT_READY;

        if (frame_in_loop == 0 && player->frame) {
            /* Each loop decodes from black, as the encoder did. */
            om3dvid7_reset(&player->dec);
            om3dvid7_render(&player->dec, picture);
            player->picture_valid[!picture_index] = 0;
        }
        if (om3dvid7_frame(&player->dec, &player->info[slot],
                           (unsigned int)(frame_in_loop % player->header.chunk_frames), picture)) {
            player->error = OM3DVID7_PLAYER_ERROR_DECODE;
            return -1;
        }
        /* Only this picture now shows the map. */
        player->picture_valid[!picture_index] = 0;

        /* A finished chunk frees its slot for the reader at once. */
        ++player->frame;
        if (player->frame == sequence_frame(player, sequence) + player->info[slot].frames)
            release_finished(player);
    }
    release_finished(player);
    return 0;
}

int om3dvid7_player_decode(Om3dvid7Player *player, unsigned long frame, int picture,
                           unsigned char *screen)
{
    return player->open ? decode_to(player, frame, picture, screen) : -1;
}

void om3dvid7_player_invalidate(Om3dvid7Player *player, int picture)
{
    player->picture_valid[picture == OM3DVID7_TV] = 0;
}

void om3dvid7_player_close(Om3dvid7Player *player)
{
    Om3dvid7Player cleared;

    if (player->reader_started) {
        player->stop = 1;
        player->host.stop_reader(player->host.context, player);
    } else {
        close_file(player);
    }
    if (player->sound_started && player->host.sound_stop)
        player->host.sound_stop(sound_context(player));
    free_all(player);

    /* Clear everything except the diagnostics. */
    memset(&cleared, 0, sizeof(cleared));
    cleared.chunks_read = player->chunks_read;
    cleared.read_ms = player->read_ms;
    cleared.read_max_ms = player->read_max_ms;
    cleared.stalls = player->stalls;
    cleared.stall_ms = player->stall_ms;
    cleared.error = player->error;
    cleared.loop_holds = player->loop_holds;
    cleared.loop_hold_ms = player->loop_hold_ms;
    cleared.spare_blocks = player->spare_blocks;
    memcpy(cleared.stall_log, player->stall_log, sizeof(cleared.stall_log));
    cleared.slots = player->slots;
    *player = cleared;
}

/* Allocates the decoder, the TV picture and the sound ring. Returns 0, or
 * OM3DVID7_PLAYER_NO_MEMORY. */
static int allocate_decoder(Om3dvid7Player *player)
{
    /* The decoder first: it is used on every frame. */
    player->dec.map =
        (unsigned long *)allocate_with_fallback(player, MAP_BYTES, OM3DVID7_MEMORY_MAIN, 0);
    player->dec.dict4 =
        (unsigned char *)allocate_with_fallback(player, DICT4_BYTES, OM3DVID7_MEMORY_MAIN, 0);
    player->dec.thumb4 =
        (unsigned char *)allocate_with_fallback(player, THUMB4_BYTES, OM3DVID7_MEMORY_MAIN, 0);
    player->dec.dict2 =
        (unsigned char *)allocate_with_fallback(player, DICT2_BYTES, OM3DVID7_MEMORY_MAIN, 0);
    player->dec.thumb2 =
        (unsigned long *)allocate_with_fallback(player, THUMB2_BYTES, OM3DVID7_MEMORY_MAIN, 0);
    player->thumb = (unsigned char *)allocate_with_fallback(player, OM3DVID7_THUMB_BYTES,
                                                            OM3DVID7_MEMORY_CEL, 0);
    if (player->sound)
        player->ring = (signed char *)allocate_with_fallback(player, OM3DVID7_PLAYER_RING,
                                                             OM3DVID7_MEMORY_SOUND, 0);
    player->dec.thumb = player->thumb;
    player->dec.entries4 = OM3DVID7_MAX_ENTRIES4;
    player->dec.entries2 = OM3DVID7_MAX_ENTRIES2;

    if (!player->dec.map || !player->dec.dict4 || !player->dec.thumb4 || !player->dec.dict2 ||
        !player->dec.thumb2 || !player->thumb || (player->sound && !player->ring))
        return OM3DVID7_PLAYER_NO_MEMORY;
    return 0;
}

/* Allocates as many chunk slots as the encoder assumed, video memory first.
 * Returns 0, or OM3DVID7_PLAYER_NO_MEMORY if too few fit to stream at all. */
static int allocate_slots(Om3dvid7Player *player)
{
    unsigned int wanted, i;

    player->slot_bytes = player->header.max_chunk + SLOT_SLACK;
    wanted =
        player->header.slots < OM3DVID7_PLAYER_SLOTS ? player->header.slots : OM3DVID7_PLAYER_SLOTS;
    for (i = 0; i < wanted; ++i) {
        player->slot[i] =
            (unsigned char *)allocate(player, player->slot_bytes, OM3DVID7_MEMORY_VIDEO);
        player->slot_vram[i] = 1;
        if (!player->slot[i]) {
            player->slot[i] = (unsigned char *)allocate_with_fallback(
                player, player->slot_bytes, OM3DVID7_MEMORY_MAIN, &player->slot_vram[i]);
        }
        if (!player->slot[i])
            break;
        /* The slack after the largest chunk is always zero. */
        player->slot[i][player->header.max_chunk] = 0;
        player->slot[i][player->header.max_chunk + 1] = 0;
        ++player->slots;
    }

    /*
     * Streaming needs at least two slots: one playing while another is read.
     * Fewer slots than the encoder assumed only means longer waits.
     */
    if (player->slots < 2 && player->slots < player->header.chunks)
        return OM3DVID7_PLAYER_NO_MEMORY;
    return 0;
}

/* Whether the first slots (as many as there are chunks) all hold their chunk. */
static int preroll_ready(const Om3dvid7Player *player)
{
    unsigned int wanted =
        player->slots < player->header.chunks ? player->slots : player->header.chunks;
    unsigned int k;

    for (k = 0; k < wanted; ++k)
        if (player->ready[k] != k + 1)
            return 0;
    return 1;
}

int om3dvid7_player_open(Om3dvid7Player *player, const Om3dvid7PlayerHost *host,
                         const unsigned char *header_block, int sound)
{
    int result = -1;

    memset(player, 0, sizeof(*player));
    player->host = *host;

    /* Keep our own copy of the header block (the table points into it). */
    memcpy(player->block, header_block, OM3DVID7_BLOCK_BYTES);
    if (om3dvid7_header(player->block, &player->header))
        return -1;
    player->header.table = player->block + OM3DVID7_HEADER_BYTES;
    player->sound = sound && player->header.audio_rate && host->sound_start;

    result = allocate_decoder(player);
    if (result == 0)
        result = allocate_slots(player);
    if (result < 0)
        goto failed;
    result = -1;
    if (player->ring)
        memset(player->ring, 0, OM3DVID7_PLAYER_RING);
    om3dvid7_reset(&player->dec);

    if (host->start_reader) {
        player->running = 1;
        if (host->start_reader(host->context, player) < 0) {
            player->running = 0;
            goto failed;
        }
        player->reader_started = 1;
    }

    /*
     * Wait until every slot (at least the clip's preroll) holds a chunk. The
     * first chunks carry most of the dictionary, and checking them uses the
     * host's spare time, which is short just after a clip starts.
     */
    while (!preroll_ready(player)) {
        if (player->error)
            goto failed;
        if (host->cancel && host->cancel(host->context)) {
            result = OM3DVID7_PLAYER_CANCELED;
            goto failed;
        }
        if (host->start_reader) {
            if (host->wait)
                host->wait(host->context);
        } else {
            read_if_needed(player);
        }
    }

    if (decode_to(player, 0, OM3DVID7_TV, NULL))
        goto failed;
    player->open = 1;
    return 0;

failed:
    om3dvid7_player_close(player);
    return result;
}
