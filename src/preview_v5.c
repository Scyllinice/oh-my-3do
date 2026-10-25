/* OM3DVID5 decoder. The console build (Norcroft armcc) uses the ARM60
 * assembly in preview_v5_arm.s; this C version runs everywhere else and is
 * the reference the assembly is tested against (tests/test_preview_asm.py).
 * Pixels are big-endian RGB555 halfwords, as the cel engine reads them. */
#include "preview_v5.h"

#define WIDTH   PREVIEW_V5_WIDTH
#define HEIGHT  PREVIEW_V5_HEIGHT
#define COLUMNS (WIDTH / 4)
#define BLOCKS  (COLUMNS * (HEIGHT / 4))
#define MAX_RUN 64
enum { SKIP, MOTION, SOLID, GRAD };

#ifdef __CC_NORCROFT
int preview_v5_decode(PreviewV5 *state, const unsigned char *data, unsigned long bytes,
                      unsigned char *pixels);
#else
static unsigned int get(const unsigned char *pixels, int x, int y)
{
    const unsigned char *p = pixels + (y * WIDTH + x) * 2;
    return ((unsigned int)p[0] << 8) | p[1];
}
static void put(unsigned char *pixels, int x, int y, unsigned int c)
{
    unsigned char *p = pixels + (y * WIDTH + x) * 2;
    p[0] = (unsigned char)(c >> 8);
    p[1] = (unsigned char)c;
}
static unsigned int colour(PreviewV5 *state, const unsigned char **data)
{
    const unsigned char *p = *data;
    unsigned int c;
    if (p[0] & 0x80) {
        c = state->cache[p[0] & (PREVIEW_V5_CACHE - 1)];
        *data = p + 1;
    } else {
        c = ((unsigned int)p[0] << 8) | p[1];
        *data = p + 2;
        state->cache[state->next] = c;
        state->next = (state->next + 1) & (PREVIEW_V5_CACHE - 1);
    }
    return c;
}
/* Like the assembly, a run may read up to PREVIEW_V5_READ_AHEAD bytes past
 * the packet before the check after it rejects the packet. */
static int preview_v5_decode(PreviewV5 *state, const unsigned char *data, unsigned long bytes,
                             unsigned char *pixels)
{
    static unsigned short source[MAX_RUN][16];
    const unsigned char *end = data + bytes;
    int block = 0;
    while (block < BLOCKS) {
        int op = *data++, mode = op >> 6, run = (op & 63) + 1, i, k;
        unsigned int c = 0;
        if (block + run > BLOCKS)
            return -1;
        if (mode == MOTION) {
            int dx = (*data >> 4) - 8, dy = (*data & 15) - 8;
            if (run > PREVIEW_V5_MOTION_RUN)
                return -1;
            ++data;
            /* The run copies from the frame as it was before the run. */
            for (i = 0; i < run; ++i) {
                int sx = ((block + i) % COLUMNS) * 4 + dx, sy = ((block + i) / COLUMNS) * 4 + dy;
                if (sx < 0 || sy < 0 || sx > WIDTH - 4 || sy > HEIGHT - 4)
                    return -1;
                for (k = 0; k < 16; ++k)
                    source[i][k] = (unsigned short)get(pixels, sx + (k & 3), sy + (k >> 2));
            }
        } else if (mode == SOLID)
            c = colour(state, &data);
        for (i = 0; i < run; ++i, ++block) {
            int x = (block % COLUMNS) * 4, y = (block / COLUMNS) * 4;
            if (mode == SKIP)
                continue;
            if (mode == MOTION)
                for (k = 0; k < 16; ++k)
                    put(pixels, x + (k & 3), y + (k >> 2), source[i][k]);
            else if (mode == SOLID)
                for (k = 0; k < 16; ++k)
                    put(pixels, x + (k & 3), y + (k >> 2), c);
            else {
                unsigned int palette[4];
                unsigned long bits;
                palette[0] = colour(state, &data);
                palette[1] = colour(state, &data);
                palette[2] = PREVIEW_V5_MIX(palette[0], palette[1]);
                palette[3] = PREVIEW_V5_MIX(palette[1], palette[0]);
                bits = ((unsigned long)data[0] << 24) | ((unsigned long)data[1] << 16) |
                       ((unsigned long)data[2] << 8) | data[3];
                data += 4;
                for (k = 0; k < 16; ++k)
                    put(pixels, x + (k & 3), y + (k >> 2), palette[(bits >> (30 - 2 * k)) & 3]);
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
