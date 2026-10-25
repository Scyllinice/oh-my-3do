/* ARM60 timing driver for the menu's preview decoder (src/preview_v5_arm.s):
 * arm60.py fills bench_video with a clip's video section (frame table, then
 * packets), calls bench_reset once and bench_frame for every frame, and
 * counts the clocks each call takes. Packets are contiguous, as in one of the
 * menu's chunks. */
#include "preview_v5.h"
#include <string.h>

int preview_v5_decode(PreviewV5 *state, const unsigned char *data, unsigned long bytes,
                      unsigned char *pixels);

unsigned char *bench_video;
unsigned int bench_pixel_words[PREVIEW_V5_WIDTH * PREVIEW_V5_HEIGHT / 2];
PreviewV5 bench_state;

static unsigned long word(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) |
           p[3];
}
int bench_reset(void)
{
    memset(&bench_state, 0, sizeof(bench_state));
    memset(bench_pixel_words, 0, sizeof(bench_pixel_words));
    return 0;
}
int bench_frame(unsigned int frame)
{
    unsigned long first = word(bench_video + frame * 4), last = word(bench_video + frame * 4 + 4);
    return preview_v5_decode(&bench_state, bench_video + first, last - first,
                             (unsigned char *)bench_pixel_words);
}
