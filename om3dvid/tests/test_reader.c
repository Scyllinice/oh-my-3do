/* The PC reader (om3dvid7_reader.c): test_reader CLIP.3dv
 *
 * Over two loops, switching between the screen and TV pictures, every RGB
 * frame must match a plain sequential decode of the file converted here
 * independently. Also: the sound matches the chunks', a workspace on an odd
 * address works, and bad workspaces, wrong sizes and damaged chunks fail. */
#include "om3dvid7.h"
#include "om3dvid7_reader.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* A 5-bit component as 0-255: its bits, then its top three bits again. */
static unsigned char channel(unsigned int pixel, int shift)
{
    unsigned int value = (pixel >> shift) & 31;
    return (unsigned char)(value * 8 + value / 4);
}

/* RGB from a big-endian RGB555 pixel. */
static void to_rgb(const unsigned char *pixel, unsigned char *rgb)
{
    unsigned int value = ((unsigned int)pixel[0] << 8) | pixel[1];
    rgb[0] = channel(value, 10);
    rgb[1] = channel(value, 5);
    rgb[2] = channel(value, 0);
}

/* The reference: decode the whole file in order, every frame of one loop,
 * as RGB rows. */
static unsigned char *reference(const unsigned char *data, const Om3dvid7Header *h, int tv,
                                signed char *audio, unsigned long *samples)
{
    Om3dvid7Decoder d;
    unsigned long frame = 0, at = OM3DVID7_BLOCK_BYTES, x, y;
    unsigned long bytes = tv ? OM3DVID7_READER_TV_RGB_BYTES : OM3DVID7_READER_SCREEN_RGB_BYTES;
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
    *samples = 0;
    om3dvid7_reset(&d);
    for (c = 0; c < h->chunks; ++c) {
        Om3dvid7Chunk info;
        unsigned long n = ((unsigned long)h->table[2 * c] << 8 | h->table[2 * c + 1]) * 2048UL;
        assert(!om3dvid7_chunk(h, c, data + at, n, &info));
        memcpy(audio + *samples, info.audio, info.audio_samples);
        *samples += info.audio_samples;
        om3dvid7_define(&d, &info);
        for (k = 0; k < info.frames; ++k, ++frame) {
            unsigned char *rgb = out + frame * bytes;
            assert(!om3dvid7_frame(&d, &info, k, tv ? OM3DVID7_TV : OM3DVID7_SCREEN));
            if (tv) {
                for (x = 0; x < 160UL * 120UL; ++x)
                    to_rgb(d.thumb + x * 2, rgb + x * 3);
            } else {
                /* Pixel (x, y) is halfword (y & 1) of word (y / 2) * 320 + x. */
                for (y = 0; y < 240; ++y)
                    for (x = 0; x < 320; ++x)
                        to_rgb(d.screen + ((y / 2) * 320 + x) * 4 + (y & 1) * 2,
                               rgb + (y * 320 + x) * 3);
            }
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

int main(int argc, char **argv)
{
    unsigned long size, samples, frame;
    unsigned char *clip, *full, *tv, *rgb, *memory;
    signed char *audio, *read_audio;
    void *workspace;
    uint64_t bytes = om3dvid7_reader_workspace_bytes();
    Om3dvid7Header h;
    int picture = OM3DVID7_TV;

    assert(argc == 2);
    clip = load(argv[1], &size);
    assert(!om3dvid7_header(clip, &h));
    audio = (signed char *)malloc(h.frames * 368 + 4);
    read_audio = (signed char *)malloc(h.frames * 368 + 4);
    full = reference(clip, &h, 0, audio, &samples);
    tv = reference(clip, &h, 1, audio, &samples);
    rgb = (unsigned char *)malloc(OM3DVID7_READER_SCREEN_RGB_BYTES);
    assert(om3dvid7_reader_abi() == OM3DVID7_READER_ABI);

    /* A workspace starting on an odd address. */
    memory = (unsigned char *)malloc(bytes + 3);
    workspace = memory + 3;
    assert(!om3dvid7_reader_open(workspace, bytes, clip, size));
    assert(om3dvid7_reader_frames(workspace) == h.frames);
    assert(om3dvid7_reader_audio_rate(workspace) == h.audio_rate);

    /* Two loops, switching pictures every 37 frames. */
    for (frame = 0; frame < 2 * h.frames; ++frame) {
        unsigned long local = frame % h.frames;
        if (frame % 37 == 36)
            picture = picture == OM3DVID7_TV ? OM3DVID7_SCREEN : OM3DVID7_TV;
        assert(om3dvid7_reader_next(workspace, picture, rgb) == (int32_t)local);
        if (picture == OM3DVID7_TV)
            assert(!memcmp(rgb, tv + local * OM3DVID7_READER_TV_RGB_BYTES,
                           OM3DVID7_READER_TV_RGB_BYTES));
        else
            assert(!memcmp(rgb, full + local * OM3DVID7_READER_SCREEN_RGB_BYTES,
                           OM3DVID7_READER_SCREEN_RGB_BYTES));
    }
    om3dvid7_reader_rewind(workspace);
    assert(om3dvid7_reader_next(workspace, OM3DVID7_SCREEN, rgb) == 0);
    assert(!memcmp(rgb, full, OM3DVID7_READER_SCREEN_RGB_BYTES));
    assert(om3dvid7_reader_next(workspace, 7, rgb) == -1); /* no such picture */

    /* The sound, whole or cut short. */
    assert(om3dvid7_reader_audio(workspace, NULL, 0) == (int64_t)samples);
    assert(om3dvid7_reader_audio(workspace, (int8_t *)read_audio, samples) == (int64_t)samples);
    assert(!memcmp(read_audio, audio, samples));
    if (samples > 10) {
        memset(read_audio, 0x55, samples);
        assert(om3dvid7_reader_audio(workspace, (int8_t *)read_audio, 10) == (int64_t)samples);
        assert(!memcmp(read_audio, audio, 10) && read_audio[10] == 0x55);
    }

    /* Bad workspaces and wrong sizes. */
    assert(om3dvid7_reader_open(workspace, bytes - 1, clip, size) == -1);
    assert(om3dvid7_reader_open(NULL, bytes, clip, size) == -1);
    assert(om3dvid7_reader_open(workspace, bytes, clip, size - 2048) == -1);
    assert(om3dvid7_reader_open(workspace, bytes, clip, size + 2048) == -1);
    assert(om3dvid7_reader_open(workspace, bytes, clip, 100) == -1);

    /* A damaged last chunk: frames before it decode, then the reader fails
     * and stays failed; the sound check fails too. */
    clip[size - 100] ^= 1;
    assert(!om3dvid7_reader_open(workspace, bytes, clip, size));
    for (frame = 0; frame < (h.chunks - 1) * h.chunk_frames; ++frame)
        assert(om3dvid7_reader_next(workspace, OM3DVID7_SCREEN, rgb) == (int32_t)frame);
    assert(om3dvid7_reader_next(workspace, OM3DVID7_SCREEN, rgb) == -1);
    assert(om3dvid7_reader_next(workspace, OM3DVID7_SCREEN, rgb) == -1);
    assert(om3dvid7_reader_audio(workspace, NULL, 0) == -1);

    free(memory);
    free(clip);
    free(full);
    free(tv);
    free(rgb);
    free(audio);
    free(read_audio);
    puts("om3dvid7 reader: ok");
    return 0;
}
