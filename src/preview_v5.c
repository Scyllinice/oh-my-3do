/* OM3DVID5 decoder. The console build (Norcroft armcc) uses the ARM60
 * assembly in preview_v5_arm.s; this C version runs everywhere else and is
 * the reference the assembly is tested against (tests/test_preview_asm.py).
 * Pixels are big-endian RGB555 halfwords, as the cel engine reads them. */
#include "preview_v5.h"

#define WIDTH   PREVIEW_V5_WIDTH
#define HEIGHT  PREVIEW_V5_HEIGHT
#define COLUMNS (WIDTH / 4)              /* 4x4 blocks across */
#define BLOCKS  (COLUMNS * (HEIGHT / 4)) /* blocks in a frame */
#define MAX_RUN 64
enum { SKIP, MOTION, SOLID, GRAD };

#ifdef __CC_NORCROFT
/* The console build: the decoder is preview_v5_decode in preview_v5_arm.s. */
int preview_v5_decode(PreviewV5 *state, const unsigned char *data, unsigned long bytes,
                      unsigned char *pixels);
#else
/* One big-endian RGB555 pixel of the frame. */
static unsigned int read_pixel(const unsigned char *pixels, int x, int y)
{
    const unsigned char *p = pixels + (y * WIDTH + x) * 2;
    return ((unsigned int)p[0] << 8) | p[1];
}

static void write_pixel(unsigned char *pixels, int x, int y, unsigned int color)
{
    unsigned char *p = pixels + (y * WIDTH + x) * 2;
    p[0] = (unsigned char)(color >> 8);
    p[1] = (unsigned char)color;
}

/*
 * Reads one color at *data and moves past it: one byte with the top bit set
 * names a cached color; otherwise two bytes are a new color, which is also
 * stored in the cache (round robin).
 */
static unsigned int read_colour(PreviewV5 *state, const unsigned char **data)
{
    const unsigned char *p = *data;
    unsigned int color;

    if (p[0] & 0x80) {
        color = state->cache[p[0] & (PREVIEW_V5_CACHE - 1)];
        *data = p + 1;
    } else {
        color = ((unsigned int)p[0] << 8) | p[1];
        *data = p + 2;
        state->cache[state->next] = color;
        state->next = (state->next + 1) & (PREVIEW_V5_CACHE - 1);
    }
    return color;
}

/*
 * Decodes one packet (see preview_v5.h). Like the assembly, a damaged run
 * may read up to PREVIEW_V5_READ_AHEAD bytes past the packet before the
 * check after it rejects the packet; writes always stay inside the frame.
 */
static int preview_v5_decode(PreviewV5 *state, const unsigned char *data, unsigned long bytes,
                             unsigned char *pixels)
{
    /* MOTION copies of a whole run, taken before the run changes anything. */
    static unsigned short copied[MAX_RUN][16];
    const unsigned char *end = data + bytes;
    int block = 0;

    while (block < BLOCKS) {
        int op = *data++;
        int mode = op >> 6;
        int run = (op & 63) + 1;
        unsigned int color = 0;
        int i, k;

        if (block + run > BLOCKS)
            return -1;

        if (mode == MOTION) {
            int dx = (*data >> 4) - 8;
            int dy = (*data & 15) - 8;
            if (run > PREVIEW_V5_MOTION_RUN)
                return -1;
            ++data;
            /* The run copies from the frame as it was before the run. */
            for (i = 0; i < run; ++i) {
                int source_x = ((block + i) % COLUMNS) * 4 + dx;
                int source_y = ((block + i) / COLUMNS) * 4 + dy;
                if (source_x < 0 || source_y < 0 || source_x > WIDTH - 4 || source_y > HEIGHT - 4)
                    return -1;
                for (k = 0; k < 16; ++k)
                    copied[i][k] =
                        (unsigned short)read_pixel(pixels, source_x + (k & 3), source_y + (k >> 2));
            }
        } else if (mode == SOLID) {
            color = read_colour(state, &data);
        }

        /* Paint each block of the run; pixel k of a block is at (k % 4, k / 4). */
        for (i = 0; i < run; ++i, ++block) {
            int x = (block % COLUMNS) * 4;
            int y = (block / COLUMNS) * 4;

            if (mode == SKIP)
                continue;
            if (mode == MOTION) {
                for (k = 0; k < 16; ++k)
                    write_pixel(pixels, x + (k & 3), y + (k >> 2), copied[i][k]);
            } else if (mode == SOLID) {
                for (k = 0; k < 16; ++k)
                    write_pixel(pixels, x + (k & 3), y + (k >> 2), color);
            } else {
                /* GRAD: two colors, two mixes, and a 2-bit choice per pixel. */
                unsigned int palette[4];
                unsigned long choices;
                palette[0] = read_colour(state, &data);
                palette[1] = read_colour(state, &data);
                palette[2] = PREVIEW_V5_MIX(palette[0], palette[1]);
                palette[3] = PREVIEW_V5_MIX(palette[1], palette[0]);
                choices = ((unsigned long)data[0] << 24) | ((unsigned long)data[1] << 16) |
                          ((unsigned long)data[2] << 8) | data[3];
                data += 4;
                for (k = 0; k < 16; ++k)
                    write_pixel(pixels, x + (k & 3), y + (k >> 2),
                                palette[(choices >> (30 - 2 * k)) & 3]);
            }
        }
        if (data > end)
            return -1;
    }
    return data == end ? 0 : -1;
}
#endif

int preview_v5_frame(PreviewV5 *state, const unsigned char *data, unsigned long bytes,
                     unsigned char *pixels)
{
    if (preview_v5_decode(state, data, bytes, pixels))
        return -1;
    ++state->frames;
    return 0;
}
