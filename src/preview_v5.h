#ifndef OH3DO_PREVIEW_V5_H
#define OH3DO_PREVIEW_V5_H
/* Block codec for 160x120 RGB555 previews at 30 fps (format OM3DVID5).
 *
 * A packet is a stream of op bytes covering the 1200 4x4 blocks in raster
 * order. Op byte = mode<<6 | (run-1); each op covers `run` (1-64) blocks:
 *   0 SKIP    leave the blocks as they are
 *   1 MOTION  one vector byte (dx+8)<<4|(dy+8), shared by the run of at
 *             most 32 blocks; each block copies the 4x4 block at that offset.
 *             The whole run copies from the frame as it was before the run.
 *             (A run's parts in two block rows never overlap: 32 blocks plus
 *             the two-block vector reach stay inside the 40-block row.)
 *   2 SOLID   one color, shared by the run
 *   3 GRAD    per block: colors a,b, then 32 bits of 2-bit indexes (MSB
 *             first, raster order) into a, b, MIX(a,b), MIX(b,a)
 * A color is a byte with the top bit set, reusing entry (byte & 127) of a
 * 128-entry cache of recent colors, or a big-endian RGB555 literal in two
 * bytes, which is then stored in the cache at the next position (round robin).
 * The cache persists from frame to frame. Frames decode in place: pixels hold
 * the previous frame (all zero before the first). */
#define PREVIEW_V5_WIDTH  160 /* also WIDTH and HEIGHT in preview_v5_arm.s */
#define PREVIEW_V5_HEIGHT 120
#define PREVIEW_V5_CACHE  128
/* A damaged run may read this far past the packet before the check that
 * rejects it; writes always stay inside the frame. */
#define PREVIEW_V5_READ_AHEAD 1024
/* The longest run a MOTION op may cover. */
#define PREVIEW_V5_MOTION_RUN 32
/*
 * MIX(a, b) is the color two thirds of the way from b to a: each 5-bit
 * channel becomes (2a + b) / 3, computed as (2a + b) * 11 / 32, which is
 * close enough and avoids a division on the ARM60.
 */
#define PREVIEW_V5_CHANNEL(c, shift) (((c) >> (shift)) & 31U)
#define PREVIEW_V5_MIX1(a, b, shift)                                                               \
    ((((2U * PREVIEW_V5_CHANNEL(a, shift) + PREVIEW_V5_CHANNEL(b, shift)) * 11U) >> 5) << (shift))
#define PREVIEW_V5_MIX(a, b)                                                                       \
    (PREVIEW_V5_MIX1(a, b, 10) | PREVIEW_V5_MIX1(a, b, 5) | PREVIEW_V5_MIX1(a, b, 0))
/* The decoder's state between frames. The assembly decoder relies on the
 * field offsets, so do not reorder them. */
typedef struct PreviewV5 {
    unsigned int cache[PREVIEW_V5_CACHE]; /* recent colors; at offset 0 */
    unsigned int next;                    /* the cache slot the next literal goes in; offset 512 */
    unsigned long frames;                 /* packets decoded since the state was cleared */
} PreviewV5;

/*
 * Decodes one packet (`bytes` long) into `pixels`, which hold the previous
 * frame. To restart a clip, zero the state and the pixels. `pixels` must be
 * 4-byte aligned, and PREVIEW_V5_READ_AHEAD bytes after the packet must be
 * readable. The console build runs preview_v5_arm.s, everything else the C
 * decoder. Returns 0, or -1 for a damaged packet.
 */
int preview_v5_frame(PreviewV5 *state, const unsigned char *data, unsigned long bytes,
                     unsigned char *pixels);
#endif
