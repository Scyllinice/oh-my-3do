/*
 * Reading OM3DVID7 clips on a PC. See om3dvid7_reader.h.
 *
 * Like the encoder, this uses no C runtime (the Windows DLL has none): all
 * memory is the caller's workspace.
 */
#include <stddef.h>
#include <stdint.h>
#include "om3dvid7.h"
#include "om3dvid7_reader.h"

/* The decoder's buffers (see Om3dvid7Decoder in om3dvid7.h). */
#define MAP_BYTES    (OM3DVID7_BLOCKS * 2UL * sizeof(unsigned long))
#define DICT4_BYTES  (OM3DVID7_MAX_ENTRIES4 * 32UL)
#define THUMB4_BYTES (OM3DVID7_MAX_ENTRIES4 * 8UL)
#define DICT2_BYTES  (OM3DVID7_MAX_ENTRIES2 * 8UL)
#define THUMB2_BYTES (OM3DVID7_MAX_ENTRIES2 * sizeof(unsigned long))

/* Everything is placed on 8-byte boundaries; the workspace may start on any
 * byte, so it has room to move up to the first boundary. */
#define ALIGNMENT 8UL

typedef struct Reader {
    Om3dvid7Header header;
    Om3dvid7Decoder dec;
    Om3dvid7Chunk info; /* the chunk being played */
    const uint8_t *data;
    uint64_t size;
    uint64_t chunk_at;    /* where that chunk starts in data */
    uint32_t chunk;       /* its number */
    int chunk_ready;      /* it is checked and its entries are stored */
    uint32_t frame;       /* the next frame to decode */
    int failed;           /* the clip turned out to be damaged */
    int picture_valid[2]; /* the screen / TV picture shows the map */
} Reader;

static uint64_t round_up(uint64_t value)
{
    return (value + ALIGNMENT - 1) & ~(uint64_t)(ALIGNMENT - 1);
}

/* The reader inside a workspace: at its first 8-byte boundary. */
static Reader *reader_in(const void *workspace)
{
    return (Reader *)(void *)(uintptr_t)round_up((uint64_t)(uintptr_t)workspace);
}

uint32_t om3dvid7_reader_abi(void)
{
    return OM3DVID7_READER_ABI;
}

uint64_t om3dvid7_reader_workspace_bytes(void)
{
    return ALIGNMENT + round_up(sizeof(Reader)) + round_up(MAP_BYTES) + round_up(DICT4_BYTES) +
           round_up(THUMB4_BYTES) + round_up(DICT2_BYTES) + round_up(THUMB2_BYTES) +
           round_up(OM3DVID7_FRAME_BYTES) + round_up(OM3DVID7_THUMB_BYTES);
}

/* Hands out the next `bytes` of the workspace after `*at`. */
static unsigned char *take(unsigned char **at, uint64_t bytes)
{
    unsigned char *memory = *at;
    *at += round_up(bytes);
    return memory;
}

static uint64_t chunk_bytes(const Reader *reader, uint32_t chunk)
{
    const unsigned char *entry = reader->header.table + 2 * chunk;
    return (((uint64_t)entry[0] << 8) | entry[1]) * OM3DVID7_BLOCK_BYTES;
}

void om3dvid7_reader_rewind(void *workspace)
{
    Reader *reader = reader_in(workspace);
    reader->frame = 0;
    reader->chunk = 0;
    reader->chunk_at = OM3DVID7_BLOCK_BYTES;
    reader->chunk_ready = 0;
    reader->picture_valid[0] = reader->picture_valid[1] = 0;
    /* Each loop decodes from black, with the dictionary numbered afresh. */
    om3dvid7_reset(&reader->dec);
}

int32_t om3dvid7_reader_open(void *workspace, uint64_t bytes, const uint8_t *data, uint64_t size)
{
    Reader *reader;
    unsigned char *at;
    uint64_t total = OM3DVID7_BLOCK_BYTES, i;
    uint32_t chunk;

    if (!workspace || !data || bytes < om3dvid7_reader_workspace_bytes() ||
        size < OM3DVID7_BLOCK_BYTES)
        return -1;
    reader = reader_in(workspace);
    for (i = 0; i < sizeof(Reader); ++i)
        ((unsigned char *)reader)[i] = 0;
    if (om3dvid7_header(data, &reader->header))
        return -1;
    reader->data = data;
    reader->size = size;

    /* The chunks the table lists must fill the rest of the file exactly. */
    for (chunk = 0; chunk < reader->header.chunks; ++chunk)
        total += chunk_bytes(reader, chunk);
    if (total != size)
        return -1;

    at = (unsigned char *)reader + round_up(sizeof(Reader));
    reader->dec.map = (unsigned long *)(void *)take(&at, MAP_BYTES);
    reader->dec.dict4 = take(&at, DICT4_BYTES);
    reader->dec.thumb4 = take(&at, THUMB4_BYTES);
    reader->dec.dict2 = take(&at, DICT2_BYTES);
    reader->dec.thumb2 = (unsigned long *)(void *)take(&at, THUMB2_BYTES);
    reader->dec.screen = take(&at, OM3DVID7_FRAME_BYTES);
    reader->dec.thumb = take(&at, OM3DVID7_THUMB_BYTES);
    reader->dec.entries4 = OM3DVID7_MAX_ENTRIES4;
    reader->dec.entries2 = OM3DVID7_MAX_ENTRIES2;
    om3dvid7_reader_rewind(workspace);
    return 0;
}

uint32_t om3dvid7_reader_frames(const void *workspace)
{
    return (uint32_t)reader_in(workspace)->header.frames;
}

uint32_t om3dvid7_reader_audio_rate(const void *workspace)
{
    return (uint32_t)reader_in(workspace)->header.audio_rate;
}

/* An RGB555 component (0-31) as 0-255. */
static uint8_t expand5(unsigned int value)
{
    return (uint8_t)((value << 3) | (value >> 2));
}

static void put_rgb(uint8_t *rgb, unsigned int pixel)
{
    rgb[0] = expand5((pixel >> 10) & 31);
    rgb[1] = expand5((pixel >> 5) & 31);
    rgb[2] = expand5(pixel & 31);
}

/* The screen bitmap (two lines per word, the upper line in the high half,
 * big-endian) as RGB rows. */
static void screen_to_rgb(const unsigned char *screen, uint8_t *rgb)
{
    unsigned int x, y;
    for (y = 0; y < OM3DVID7_HEIGHT; ++y) {
        for (x = 0; x < OM3DVID7_WIDTH; ++x) {
            const unsigned char *pixel =
                screen + ((y >> 1) * OM3DVID7_WIDTH + x) * 4UL + (y & 1) * 2UL;
            put_rgb(rgb, ((unsigned int)pixel[0] << 8) | pixel[1]);
            rgb += 3;
        }
    }
}

/* The TV picture (big-endian pixels in rows) as RGB rows. */
static void tv_to_rgb(const unsigned char *thumb, uint8_t *rgb)
{
    unsigned long i;
    for (i = 0; i < OM3DVID7_THUMB_BYTES; i += 2) {
        put_rgb(rgb, ((unsigned int)thumb[i] << 8) | thumb[i + 1]);
        rgb += 3;
    }
}

/* Makes the chunk holding the next frame the current one: checked, with its
 * entries stored. Returns 0, or -1 if it is damaged. */
static int reach_chunk(Reader *reader)
{
    uint32_t wanted = reader->frame / reader->header.chunk_frames;
    uint64_t bytes;

    if (reader->chunk_ready && reader->chunk == wanted)
        return 0;
    if (reader->chunk_ready) {
        /* Chunks are played in order, so this is the next one. */
        reader->chunk_at += chunk_bytes(reader, reader->chunk);
        reader->chunk = wanted;
    }
    bytes = chunk_bytes(reader, reader->chunk);
    if (om3dvid7_chunk(&reader->header, reader->chunk, reader->data + reader->chunk_at,
                       (unsigned long)bytes, &reader->info))
        return -1;
    om3dvid7_define(&reader->dec, &reader->info);
    reader->chunk_ready = 1;
    return 0;
}

int32_t om3dvid7_reader_next(void *workspace, int32_t picture, uint8_t *rgb)
{
    Reader *reader = reader_in(workspace);
    int index = picture == OM3DVID7_TV;
    uint32_t frame;

    if (reader->failed || !rgb || (picture != OM3DVID7_SCREEN && picture != OM3DVID7_TV))
        return -1;
    if (reader->frame == reader->header.frames)
        om3dvid7_reader_rewind(workspace);
    if (reach_chunk(reader)) {
        reader->failed = 1;
        return -1;
    }

    /* A picture that missed earlier frames is redrawn whole from the map. */
    if (!reader->picture_valid[index]) {
        om3dvid7_render(&reader->dec, picture);
        reader->picture_valid[index] = 1;
    }
    if (om3dvid7_frame(&reader->dec, &reader->info, reader->frame % reader->header.chunk_frames,
                       picture)) {
        reader->failed = 1;
        return -1;
    }
    reader->picture_valid[!index] = 0;

    if (index)
        tv_to_rgb(reader->dec.thumb, rgb);
    else
        screen_to_rgb(reader->dec.screen, rgb);
    frame = reader->frame++;
    return (int32_t)frame;
}

int64_t om3dvid7_reader_audio(void *workspace, int8_t *out, uint64_t capacity)
{
    Reader *reader = reader_in(workspace);
    uint64_t at = OM3DVID7_BLOCK_BYTES, total = 0;
    uint32_t chunk;

    for (chunk = 0; chunk < reader->header.chunks; ++chunk) {
        uint64_t bytes = chunk_bytes(reader, chunk), i;
        Om3dvid7Chunk info;
        if (om3dvid7_chunk(&reader->header, chunk, reader->data + at, (unsigned long)bytes, &info))
            return -1;
        for (i = 0; i < info.audio_samples; ++i, ++total) {
            if (out && total < capacity)
                out[total] = info.audio[i];
        }
        at += bytes;
    }
    return (int64_t)total;
}
