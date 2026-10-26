/* The OM3DVID7 player (om3dvid7_player.c) against an in-memory clip:
 * test_player CLIP.3dv
 *
 * Every frame over two loops must match a plain sequential decode of the
 * file (which restarts from black at each loop), both when chunks are read
 * on the spot and when a "reader thread" reads them. The thread is simulated
 * on one thread by calling om3dvid7_player_read at chosen moments, which
 * also lets the test starve it to force a stall. Also: memory is allocated
 * by kind and all freed, the sound callbacks are called in order, and a
 * canceled start, a damaged chunk and a file changed between loops fail
 * cleanly. */
#include "om3dvid7_player.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The clip as a source: reads only forwards, from an opened file. */
typedef struct Source {
    unsigned char *data;
    unsigned long size, position;
    int opened, opens, reads, damage_read, change_on_open;

    /* Memory: live allocations and bytes, and the kinds asked for. */
    long allocations, bytes;
    int kinds[4];

    /* Sound. */
    int sound_fails, starts, stops, paused;

    /* The simulated reader thread. */
    int threaded, reader_running, pace_calls, spare_seen;
    int cancel_after, cancel_calls, reclaims;
} Source;

static int source_open(void *context)
{
    Source *source = (Source *)context;
    assert(!source->opened);
    ++source->opens;
    if (source->change_on_open && source->opens == source->change_on_open)
        source->data[40] ^= 1; /* the header block is no longer the same */
    source->opened = 1;
    source->position = 0;
    return 0;
}

static int source_read_block(void *context, unsigned char *block)
{
    Source *source = (Source *)context;
    assert(source->opened && source->position + OM3DVID7_BLOCK_BYTES <= source->size);
    memcpy(block, source->data + source->position, OM3DVID7_BLOCK_BYTES);
    source->position += OM3DVID7_BLOCK_BYTES;
    if (++source->reads == source->damage_read)
        block[100] ^= 1;
    return 0;
}

static int source_close(void *context)
{
    Source *source = (Source *)context;
    assert(source->opened);
    source->opened = 0;
    return 0;
}

static void *source_allocate(void *context, unsigned long bytes, int kind)
{
    Source *source = (Source *)context;
    assert(kind >= 0 && kind < 4);
    ++source->kinds[kind];
    ++source->allocations;
    source->bytes += (long)bytes;
    return malloc(bytes);
}

static void source_release(void *context, void *memory, unsigned long bytes)
{
    Source *source = (Source *)context;
    --source->allocations;
    source->bytes -= (long)bytes;
    free(memory);
}

static int sound_start(void *context, signed char *ring, unsigned long samples, unsigned long rate)
{
    Source *source = (Source *)context;
    assert(ring && samples == OM3DVID7_PLAYER_RING && rate == OM3DVID7_AUDIO_RATE);
    ++source->starts;
    return source->sound_fails ? -1 : 0;
}

static void sound_pause(void *context, int pause)
{
    ((Source *)context)->paused = pause;
}

static void sound_stop(void *context)
{
    ++((Source *)context)->stops;
}

static void pace(void *context, int spare)
{
    Source *source = (Source *)context;
    ++source->pace_calls;
    if (spare)
        source->spare_seen = 1;
}

static int cancel(void *context)
{
    Source *source = (Source *)context;
    return source->cancel_after && ++source->cancel_calls >= source->cancel_after;
}

/* The simulated reader thread: read until there is nothing to do. */
static void run_reader(Om3dvid7Player *player)
{
    while (om3dvid7_player_read(player) == 0) {
    }
}

static int start_reader(void *context, Om3dvid7Player *player)
{
    (void)player;
    ((Source *)context)->reader_running = 1;
    return 0;
}

static void stop_reader(void *context, Om3dvid7Player *player)
{
    Source *source = (Source *)context;
    assert(player->stop && om3dvid7_player_read(player) == OM3DVID7_PLAYER_READ_STOP);
    om3dvid7_player_close_file(player);
    om3dvid7_player_reader_exit(player);
    assert(!player->running);
    source->reader_running = 0;
}

/* While opening waits, the thread gets to run. */
static void wait_for_reader(void *context)
{
    (void)context;
}

static int failing_start(void *context, Om3dvid7Player *player)
{
    (void)context;
    (void)player;
    return -1;
}

static void *no_memory(void *context, unsigned long bytes, int kind)
{
    (void)context;
    (void)bytes;
    (void)kind;
    return NULL;
}

static int reclaim_nothing(void *context)
{
    ++((Source *)context)->reclaims;
    return 0;
}

static Source *waiting_source;
static Om3dvid7Player *waiting_player;
static void wait_and_read(void *context)
{
    assert(context == waiting_source);
    run_reader(waiting_player);
}

static unsigned char *load(const char *name, unsigned long *size)
{
    FILE *file = fopen(name, "rb");
    unsigned char *data;
    long n;
    assert(file);
    fseek(file, 0, SEEK_END);
    n = ftell(file);
    fseek(file, 0, SEEK_SET);
    data = (unsigned char *)malloc((size_t)n);
    assert(data && fread(data, 1, (size_t)n, file) == (size_t)n);
    fclose(file);
    *size = (unsigned long)n;
    return data;
}

/* The reference: decode the whole file in order, every frame of one loop. */
static unsigned char *reference(const unsigned char *data, const Om3dvid7Header *h, int tv)
{
    Om3dvid7Decoder d;
    unsigned long frame = 0, at = OM3DVID7_BLOCK_BYTES;
    unsigned long bytes = tv ? OM3DVID7_THUMB_BYTES : OM3DVID7_FRAME_BYTES;
    unsigned char *out = (unsigned char *)malloc(h->frames * bytes);
    unsigned int c, k;
    memset(&d, 0, sizeof(d));
    d.screen = (unsigned char *)calloc(1, OM3DVID7_FRAME_BYTES);
    d.thumb = (unsigned char *)calloc(1, OM3DVID7_THUMB_BYTES);
    d.map = (unsigned long *)malloc(OM3DVID7_BLOCKS * 2 * sizeof(unsigned long));
    d.dict4 = (unsigned char *)calloc(OM3DVID7_MAX_ENTRIES4, 32);
    d.thumb4 = (unsigned char *)calloc(OM3DVID7_MAX_ENTRIES4, 8);
    d.dict2 = (unsigned char *)calloc(OM3DVID7_MAX_ENTRIES2, 8);
    d.thumb2 = (unsigned long *)calloc(OM3DVID7_MAX_ENTRIES2, sizeof(unsigned long));
    d.entries4 = OM3DVID7_MAX_ENTRIES4;
    d.entries2 = OM3DVID7_MAX_ENTRIES2;
    om3dvid7_reset(&d);
    for (c = 0; c < h->chunks; ++c) {
        Om3dvid7Chunk info;
        unsigned long n = ((unsigned long)h->table[2 * c] << 8 | h->table[2 * c + 1]) * 2048UL;
        assert(!om3dvid7_chunk(h, c, data + at, n, &info));
        om3dvid7_define(&d, &info);
        for (k = 0; k < info.frames; ++k, ++frame) {
            assert(!om3dvid7_frame(&d, &info, k, tv ? OM3DVID7_TV : OM3DVID7_SCREEN));
            memcpy(out + frame * bytes, tv ? d.thumb : d.screen, bytes);
        }
        at += n;
    }
    free(d.screen);
    free(d.thumb);
    free(d.map);
    free(d.dict4);
    free(d.thumb4);
    free(d.dict2);
    free(d.thumb2);
    return out;
}

static unsigned char *clip, *full, *tv;
static unsigned long clip_size;
static Om3dvid7Header header;

static Om3dvid7PlayerHost make_host(Source *source, int threaded)
{
    Om3dvid7PlayerHost host;
    memset(source, 0, sizeof(*source));
    source->data = clip;
    source->size = clip_size;
    source->threaded = threaded;
    memset(&host, 0, sizeof(host));
    host.context = source;
    host.open = source_open;
    host.read_block = source_read_block;
    host.close = source_close;
    host.allocate = source_allocate;
    host.release = source_release;
    host.sound_start = sound_start;
    host.sound_pause = sound_pause;
    host.sound_stop = sound_stop;
    host.cancel = cancel;
    if (threaded) {
        host.pace = pace;
        host.start_reader = start_reader;
        host.stop_reader = stop_reader;
        host.wait = wait_for_reader;
    }
    return host;
}

/* Plays two loops, switching pictures every 37 frames, checking each frame. */
static void play_two_loops(int threaded)
{
    Source source;
    Om3dvid7PlayerHost host = make_host(&source, threaded);
    Om3dvid7Player *player = (Om3dvid7Player *)malloc(sizeof(Om3dvid7Player));
    unsigned char *screen = (unsigned char *)calloc(1, OM3DVID7_FRAME_BYTES);
    unsigned long frame, now = 1000;
    int picture = OM3DVID7_TV;

    if (threaded) {
        host.wait = wait_and_read;
        waiting_source = &source;
        waiting_player = player;
    }
    assert(!om3dvid7_player_open(player, &host, clip, 1));
    assert(player->open && !player->playing && source.opened);
    assert(source.kinds[OM3DVID7_MEMORY_CEL] == 1 &&
           source.kinds[OM3DVID7_MEMORY_SOUND] == (header.audio_rate != 0));
    assert(om3dvid7_player_due(player, now + 5000) == 0 && !source.starts);
    assert(!memcmp(player->thumb, tv, OM3DVID7_THUMB_BYTES));
    om3dvid7_player_play(player, now);
    assert(source.starts == (header.audio_rate != 0));

    for (frame = 1; frame < 2 * header.frames; ++frame) {
        unsigned long local = frame % header.frames;
        if (frame % 37 == 0)
            picture = picture == OM3DVID7_TV ? OM3DVID7_SCREEN : OM3DVID7_TV;
        if (threaded)
            run_reader(player);
        assert(om3dvid7_player_decode(player, frame, picture, screen) == 0);
        if (picture == OM3DVID7_TV)
            assert(!memcmp(player->thumb, tv + local * OM3DVID7_THUMB_BYTES, OM3DVID7_THUMB_BYTES));
        else
            assert(!memcmp(screen, full + local * OM3DVID7_FRAME_BYTES, OM3DVID7_FRAME_BYTES));
        now = 1000 + (frame * 1000UL + 29) / 30;
        assert(om3dvid7_player_due(player, now) == frame);
        om3dvid7_player_update(player, now, 0);
    }
    assert(!player->error && player->stalls == 0 && source.opens == 2);
    /* Each chunk of both loops is read exactly once: the next loop is not
     * started before this one has played. */
    assert(player->chunks_read == 2 * header.chunks);
    if (threaded)
        assert(source.pace_calls > 0 && (source.spare_seen || header.chunks <= player->slots));

    om3dvid7_player_close(player);
    assert(!player->open && !source.opened && !source.reader_running);
    assert(source.allocations == 0 && source.bytes == 0);
    assert(source.stops == (header.audio_rate != 0));
    assert(player->chunks_read == 2 * header.chunks); /* kept for diagnostics */
    om3dvid7_player_close(player);                    /* twice is harmless */
    free(player);
    free(screen);
}

/* A reader thread that falls behind: playback waits and the sound pauses. */
static void starved_reader(void)
{
    Source source;
    Om3dvid7PlayerHost host = make_host(&source, 1);
    Om3dvid7Player *player = (Om3dvid7Player *)malloc(sizeof(Om3dvid7Player));
    unsigned long frame, now = 0, due;
    int result = 0;

    host.wait = wait_and_read;
    waiting_source = &source;
    waiting_player = player;
    assert(!om3dvid7_player_open(player, &host, clip, 1));
    om3dvid7_player_play(player, now);
    /* Play without letting the reader run until playback has to wait. */
    for (frame = 1; frame < header.frames; ++frame) {
        now = (frame * 1000UL + 29) / 30;
        result = om3dvid7_player_decode(player, frame, OM3DVID7_TV, NULL);
        om3dvid7_player_update(player, now, result == OM3DVID7_PLAYER_NOT_READY);
        if (result == OM3DVID7_PLAYER_NOT_READY)
            break;
        assert(result == 0);
    }
    if (header.chunks <= player->slots) {
        /* The whole clip fits: nothing to wait for in the first loop. */
        assert(result == 0);
    } else {
        assert(result == OM3DVID7_PLAYER_NOT_READY && player->stalls == 1 && player->paused);
        assert(source.paused == (header.audio_rate != 0));
        /* The clock stops while paused. */
        due = om3dvid7_player_due(player, now);
        assert(om3dvid7_player_due(player, now + 500) == due);
        /* The reader catches up: playback resumes from where it stopped. */
        run_reader(player);
        assert(om3dvid7_player_decode(player, frame, OM3DVID7_TV, NULL) == 0);
        assert(!memcmp(player->thumb, tv + frame * OM3DVID7_THUMB_BYTES, OM3DVID7_THUMB_BYTES));
        om3dvid7_player_update(player, now + 500, 0);
        assert(!player->paused && !source.paused && player->stall_ms == 500);
        assert(om3dvid7_player_due(player, now + 500) == due);
    }
    om3dvid7_player_close(player);
    assert(source.allocations == 0 && player->stalls == (header.chunks > player->slots));
    free(player);
}

static void failures(void)
{
    Source source;
    Om3dvid7PlayerHost host;
    Om3dvid7Player *player = (Om3dvid7Player *)malloc(sizeof(Om3dvid7Player));

    /* Canceled while buffering. */
    host = make_host(&source, 0);
    source.cancel_after = 1;
    assert(om3dvid7_player_open(player, &host, clip, 0) == OM3DVID7_PLAYER_CANCELED);
    assert(!player->open && source.allocations == 0 && !source.opened);

    /* A damaged chunk. */
    host = make_host(&source, 0);
    source.damage_read = 3;
    assert(om3dvid7_player_open(player, &host, clip, 0) == -1);
    assert(player->error == OM3DVID7_PLAYER_ERROR_READ && source.allocations == 0);

    /* The reader thread fails to start. */
    host = make_host(&source, 1);
    host.start_reader = failing_start;
    assert(om3dvid7_player_open(player, &host, clip, 0) == -1);
    assert(!player->running && !source.reader_running && source.allocations == 0);

    /* Without memory callbacks, malloc and free are used. */
    host = make_host(&source, 0);
    host.allocate = NULL;
    host.release = NULL;
    assert(!om3dvid7_player_open(player, &host, clip, 0));
    assert(source.kinds[OM3DVID7_MEMORY_MAIN] == 0);
    om3dvid7_player_close(player);

    /* Out of memory: reclaim is asked once per allocation, then it gives up. */
    host = make_host(&source, 0);
    host.allocate = no_memory;
    host.reclaim = reclaim_nothing;
    assert(om3dvid7_player_open(player, &host, clip, 0) == OM3DVID7_PLAYER_NO_MEMORY);
    assert(source.reclaims > 0 && !player->open);

    /* Sound that will not start: plays on silently, and is still stopped. */
    host = make_host(&source, 0);
    source.sound_fails = 1;
    assert(!om3dvid7_player_open(player, &host, clip, 1));
    om3dvid7_player_play(player, 0);
    assert(!player->sound && source.starts == (header.audio_rate != 0));
    om3dvid7_player_update(player, 100, 1);
    assert(!source.paused); /* no pause sent to sound that never started */
    om3dvid7_player_close(player);
    assert(source.stops == source.starts);

    /* The sound callbacks get sound_context when it is set. */
    if (header.audio_rate) {
        Source sound_side;
        memset(&sound_side, 0, sizeof(sound_side));
        host = make_host(&source, 0);
        host.sound_context = &sound_side;
        assert(!om3dvid7_player_open(player, &host, clip, 1));
        om3dvid7_player_play(player, 0);
        om3dvid7_player_update(player, 100, 1);
        assert(sound_side.starts == 1 && sound_side.paused && !source.starts && !source.paused);
        om3dvid7_player_close(player);
        assert(sound_side.stops == 1 && !source.stops);
    }

    /* Without sound callbacks the clip is silent and has no ring. */
    host = make_host(&source, 0);
    host.sound_start = NULL;
    assert(!om3dvid7_player_open(player, &host, clip, 1));
    assert(!player->sound && !player->ring && !source.kinds[OM3DVID7_MEMORY_SOUND]);
    om3dvid7_player_close(player);

    /* The file changes before the second loop: the reader reports an error. */
    {
        unsigned long frame;
        int result = 0;
        host = make_host(&source, 0);
        source.change_on_open = 2;
        assert(!om3dvid7_player_open(player, &host, clip, 0));
        om3dvid7_player_play(player, 0);
        for (frame = 1; frame <= header.frames && result == 0; ++frame)
            result = om3dvid7_player_decode(player, frame, OM3DVID7_TV, NULL);
        assert(result == -1 && player->error == OM3DVID7_PLAYER_ERROR_READ);
        om3dvid7_player_close(player);
        assert(source.allocations == 0 && !source.opened);
        clip[40] ^= 1; /* undo the change */
    }
    free(player);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    clip = load(argv[1], &clip_size);
    assert(!om3dvid7_header(clip, &header));
    full = reference(clip, &header, 0);
    tv = reference(clip, &header, 1);

    play_two_loops(0);
    play_two_loops(1);
    starved_reader();
    failures();

    free(clip);
    free(full);
    free(tv);
    puts("om3dvid7 player: ok");
    return 0;
}
