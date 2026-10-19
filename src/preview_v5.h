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
 *   2 SOLID   one colour, shared by the run
 *   3 GRAD    per block: colours a,b, then 32 bits of 2-bit indexes (MSB
 *             first, raster order) into a, b, MIX(a,b), MIX(b,a)
 * A colour is a byte with the top bit set, reusing entry (byte & 127) of a
 * 128-entry cache of recent colours, or a big-endian RGB555 literal in two
 * bytes, which is then stored in the cache at the next position (round robin).
 * The cache persists from frame to frame. Frames decode in place: pixels hold
 * the previous frame (all zero before the first). */
#define PREVIEW_V5_WIDTH 160 /* also WIDTH and HEIGHT in preview_v5_arm.s */
#define PREVIEW_V5_HEIGHT 120
#define PREVIEW_V5_CACHE 128
/* A damaged run may read this far past the packet before the check that
 * rejects it; writes always stay inside the frame. */
#define PREVIEW_V5_READ_AHEAD 1024
#define PREVIEW_V5_MOTION_RUN 32
#define PREVIEW_V5_CHANNEL(c,shift) (((c)>>(shift))&31U)
#define PREVIEW_V5_MIX1(a,b,shift) ((((2U*PREVIEW_V5_CHANNEL(a,shift)+PREVIEW_V5_CHANNEL(b,shift))*11U)>>5)<<(shift))
#define PREVIEW_V5_MIX(a,b) (PREVIEW_V5_MIX1(a,b,10)|PREVIEW_V5_MIX1(a,b,5)|PREVIEW_V5_MIX1(a,b,0))
typedef struct PreviewV5 {
    unsigned int cache[PREVIEW_V5_CACHE]; /* offset 0 for preview_v5_arm.s */
    unsigned int next;                    /* offset 512 */
    unsigned long frames; /* packets decoded since the state was cleared */
} PreviewV5;
/* Zero the state and the pixels to restart a clip. pixels must be 4-byte
 * aligned; PREVIEW_V5_READ_AHEAD bytes after the packet must be readable.
 * Built with armcc it runs preview_v5_arm.s, elsewhere the C decoder. 0 or -1. */
int preview_v5_frame(PreviewV5 *state,const unsigned char *data,unsigned long bytes,unsigned char *pixels);
#endif
