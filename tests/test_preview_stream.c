/* Streamed OM3DVID7 playback (src/preview_stream.c) on the host, where the
 * reader runs synchronously: test_preview_stream CLIP.3dv
 *
 * The clip plays through a fake ODE for two loops, switching between the
 * screen and the TV picture, and every frame must match a plain sequential
 * decode of the file (which restarts from black at each loop). Also: the
 * file is reopened per loop and only ever read forwards, slots are reused,
 * sound reaches the ring in order, a canceled preroll and a damaged chunk
 * fail cleanly. */
#include "preview_stream.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Sound stubs: the ring is checked directly. */
static int audio_started, audio_paused;
int preview_audio_start_ring(PreviewAudio *audio, unsigned char *data, unsigned long frames,
                             unsigned long rate)
{
    assert(data && frames == PREVIEW_STREAM_RING && rate == 11025);
    audio->status = 1;
    ++audio_started;
    return 0;
}
void preview_audio_pause(PreviewAudio *audio, int pause)
{
    if (audio->status == 1)
        audio_paused = pause;
}
void preview_audio_close(PreviewAudio *audio)
{
    memset(audio, 0, sizeof(*audio));
}

typedef struct Fake {
    unsigned char *data, buffer[2048];
    size_t size, pos;
    int opened, opens, corrupt_read, reads;
    char path[128];
} Fake;
static int transfer(void *context, const unsigned char *packet, unsigned char *reply,
                    size_t capacity, size_t *length)
{
    Fake *f = (Fake *)context;
    assert(capacity >= 4);
    memset(reply, 0, capacity);
    reply[0] = packet[0];
    *length = 2;
    if (packet[0] == 0xe6) {
        size_t offset = ((size_t)packet[1] * 256 + packet[2]) * 4;
        assert(offset + 4 <= sizeof(f->buffer));
        memcpy(f->buffer + offset, packet + 3, 4);
    } else if (packet[0] == 0xe1) {
        assert(!f->opened);
        strcpy(f->path, (const char *)f->buffer);
        f->pos = 0;
        f->opened = 1;
        ++f->opens;
        reply[1] = 1;
        *length = 3;
    } else if (packet[0] == 0xe3) {
        size_t n = (size_t)packet[1] * 256 + packet[2], actual = n;
        assert(f->opened && n <= 2048);
        if (actual > f->size - f->pos)
            actual = f->size - f->pos;
        memcpy(f->buffer, f->data + f->pos, actual);
        if (++f->reads == f->corrupt_read)
            f->buffer[100] ^= 1;
        f->pos += actual;
        reply[1] = packet[1];
        reply[2] = packet[2];
        *length = 4;
    } else if (packet[0] == 0xe5) {
        assert(f->opened);
        f->opened = 0;
    } else
        assert(0); /* reads only, never a seek */
    return 0;
}
static int reader(void *context, unsigned char *data, size_t size)
{
    Fake *f = (Fake *)context;
    assert(size == 2048);
    memcpy(data, f->buffer, size);
    return 0;
}
static int cancel_after, cancel_calls;
static int cancel(void *context)
{
    (void)context;
    return cancel_after && ++cancel_calls >= cancel_after;
}

static unsigned char *load(const char *name, size_t *size)
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
    *size = (size_t)n;
    return data;
}

/* The reference: decode the whole file in order into rows of 320x240. */
typedef struct Reference {
    Preview7 dec;
    unsigned char *screen, *frames;
} Reference;
static unsigned char *reference(const unsigned char *data, const Preview7Header *h, int tv)
{
    Preview7 d;
    unsigned long frame = 0, at = PREVIEW7_BLOCK_BYTES,
                  bytes = tv ? PREVIEW7_THUMB_BYTES : PREVIEW7_FRAME_BYTES;
    unsigned char *out = (unsigned char *)malloc(h->frames * bytes);
    unsigned int c, k;
    memset(&d, 0, sizeof(d));
    d.screen = (unsigned char *)calloc(1, PREVIEW7_FRAME_BYTES);
    d.thumb = (unsigned char *)calloc(1, PREVIEW7_THUMB_BYTES);
    d.map = (unsigned long *)malloc(PREVIEW7_BLOCKS * 2 * sizeof(unsigned long));
    d.dict4 = (unsigned char *)calloc(8192, 32);
    d.thumb4 = (unsigned char *)calloc(8192, 8);
    d.dict2 = (unsigned char *)calloc(4096, 8);
    d.thumb2 = (unsigned long *)calloc(4096, sizeof(unsigned long));
    preview7_reset(&d);
    for (c = 0; c < h->chunks; ++c) {
        Preview7Chunk info;
        unsigned long n = ((unsigned long)h->table[2 * c] << 8 | h->table[2 * c + 1]) * 2048UL;
        assert(!preview7_chunk(h, c, data + at, n, &info));
        preview7_define(&d, &info);
        for (k = 0; k < info.frames; ++k, ++frame) {
            assert(!preview7_frame(&d, &info, k, tv ? PREVIEW7_TV : PREVIEW7_SCREEN));
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

int main(int argc, char **argv)
{
    size_t size;
    unsigned char *data, *screen = (unsigned char *)calloc(1, PREVIEW7_FRAME_BYTES), *full, *tv;
    Preview7Header h;
    PreviewStream s;
    Fake f;
    Ode ode;
    unsigned long frame, loops = 2, now = 1000;
    int picture = PREVIEW7_TV;
    assert(argc == 2);
    data = load(argv[1], &size);
    assert(!preview7_header(data, &h));
    full = reference(data, &h, 0);
    tv = reference(data, &h, 1);
    memset(&f, 0, sizeof(f));
    f.data = data;
    f.size = size;
    memset(&ode, 0, sizeof(ode));
    ode.transfer = transfer;
    ode.context = &f;
    strcpy(ode.identity, "LOCODE");

    /* A clean run over two loops, switching pictures every 37 frames. */
    assert(!preview_stream_open(&s, &ode, reader, "Road Rash (USA)", "preview-x.3dv", data, 1,
                                cancel, NULL, NULL, NULL));
    assert(preview_stream_due(&s, now + 5000) == 0 && !audio_started);
    preview_stream_play(&s, now);
    assert(!strcmp(f.path, "Road Rash (USA)/.ohmy3do/preview-x.3dv") && f.opens == 1 &&
           audio_started == (h.audio_rate != 0));
    assert(s.slots == h.slots && s.slots >= h.preroll && (!h.audio_rate || s.audio_written >= 1));
    assert(!memcmp(s.thumb, tv, PREVIEW7_THUMB_BYTES));
    for (frame = 1; frame < loops * h.frames; ++frame) {
        unsigned long local = frame % h.frames;
        if (frame % 37 == 0)
            picture = picture == PREVIEW7_TV ? PREVIEW7_SCREEN : PREVIEW7_TV;
        assert(preview_stream_decode(&s, frame, picture, screen) == 0);
        if (picture == PREVIEW7_TV)
            assert(!memcmp(s.thumb, tv + local * PREVIEW7_THUMB_BYTES, PREVIEW7_THUMB_BYTES));
        else
            assert(!memcmp(screen, full + local * PREVIEW7_FRAME_BYTES, PREVIEW7_FRAME_BYTES));
        /* The sound clock runs ahead with the frames. */
        now = 1000 + (frame * 1000UL + 29) / 30;
        assert(preview_stream_due(&s, now) == frame);
        preview_stream_audio(&s, now, 0);
        assert(s.released + s.slots >= s.audio_written);
    }
    assert(f.opens >= 2 && !s.error);
    /* Sound copied in order: the ring holds the clip's samples at their place. */
    {
        unsigned long seq = s.released, k = seq % s.slots;
        if (s.ready[k] == seq + 1 && seq < s.audio_written) {
            unsigned long first = (seq / h.chunks) * h.frames + (seq % h.chunks) * h.chunk_frames;
            unsigned long at =
                (first / h.frames) * (h.frames * 11025UL / 30) + (first % h.frames) * 11025UL / 30;
            assert(s.info[k].audio_samples == 0 ||
                   !memcmp(s.ring + at % PREVIEW_STREAM_RING, s.info[k].audio, 1));
        }
    }
    preview_stream_close(&s);
    assert(!s.open && !s.dec.map && s.chunks_read >= 2 * h.chunks - 1);

    /* Pausing while waiting stops the clock. */
    memset(&f, 0, sizeof(f));
    f.data = data;
    f.size = size;
    assert(!preview_stream_open(&s, &ode, reader, "", "preview-x.3dv", data, 1, NULL, NULL, NULL,
                                NULL));
    preview_stream_play(&s, 5000);
    preview_stream_audio(&s, 5100, 1);
    assert(s.stalls == 1 && (audio_paused || !h.audio_rate));
    preview_stream_audio(&s, 5600, 0);
    assert(!audio_paused && s.stall_ms == 500);
    assert(preview_stream_due(&s, 5600) == (100UL * 30) / 1000);
    preview_stream_close(&s);

    /* Canceled while buffering. */
    memset(&f, 0, sizeof(f));
    f.data = data;
    f.size = size;
    cancel_after = 1;
    cancel_calls = 0;
    assert(preview_stream_open(&s, &ode, reader, "", "preview-x.3dv", data, 0, cancel, NULL, NULL,
                               NULL) == -5 &&
           !s.open);
    cancel_after = 0;

    /* A damaged chunk stops playback with an error. */
    memset(&f, 0, sizeof(f));
    f.data = data;
    f.size = size;
    f.corrupt_read = 3;
    assert(preview_stream_open(&s, &ode, reader, "", "preview-x.3dv", data, 0, NULL, NULL, NULL,
                               NULL) < 0 &&
           !s.open);

    free(data);
    free(full);
    free(tv);
    free(screen);
    puts("preview stream: ok");
    return 0;
}
