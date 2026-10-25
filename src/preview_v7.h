#ifndef OH3DO_PREVIEW_V7_H
#define OH3DO_PREVIEW_V7_H
/* OM3DVID7: streamed 320x240 previews at 30 fps (see docs/media-format.md).
 *
 * The file is read from the start in 2 KiB blocks and never seeks:
 *   header block   2048 bytes: header, then one BE16 per chunk giving its
 *                  length in 2 KiB blocks
 *   chunks         each padded with zeros to a multiple of 2048 bytes
 *
 * Header (big-endian): "OM3DVID7", width 320, height 240 (BE16 each), fps 30,
 * frames per chunk (BE16 each), frames (BE32), chunks, preroll (BE16 each),
 * 4x4 entries, 2x2 entries (BE16 each), audio rate 0 or 11025 (BE32), largest
 * chunk in bytes (BE32), nominal bytes per second (BE32), check (BE32), the
 * chunk slots the encoder assumed the player has (BE16), zeros to offset 64,
 * then the chunk table. Playback may start once preroll chunks are read.
 * The check is the preview word sum of the whole header block with the
 * check field zero.
 *
 * Chunk: check (BE32, the word sum of the rest of the padded chunk), chunk
 * number, frames (BE16 each), first 4x4 entry, 4x4 entry count, first 2x2
 * entry, 2x2 entry count (BE16 each), audio samples, video bytes (BE32 each);
 * signed 8-bit audio padded to a word; the new 4x4 entries and 2x2 entries,
 * in the decoder's own layout so storing them is a copy (see
 * preview7_entry4/2); frames+1 BE32 offsets from the start of the offset
 * table; the packets.
 *
 * A 4x4 entry is 40 bytes: two screen line pairs of four BE32 words (the
 * upper line's pixel in the high half), then its TV picture: two rows of
 * two BE RGB555 pixels, each the rounded average of a 2x2 square. A 2x2
 * entry is 12 bytes: one line pair of two words, then the average of its
 * four pixels in the high half of a BE32 word.
 *
 * Entries are numbered in the order chunks define them, are never redefined
 * with different pixels, and a packet in chunk C may use only entries that
 * C or earlier chunks define. Looping restarts the numbering from zero.
 *
 * A packet is BE16 words covering the 4800 4x4 blocks in raster order:
 *   0x0000-0x1fff  one 4x4 entry
 *   0x2000-0x3fff  leave (w&0x1fff)+1 blocks unchanged
 *   0x4000-0x5fff  (w&0x1fff)+1 4x4 entry numbers follow (each < 0x2000)
 *   0x6000-0x7fff  (w&0x1fff)+1 quad blocks follow, 3 words each: four
 *                  12-bit 2x2 entry numbers, top-left, top-right,
 *                  bottom-left, bottom-right, packed most significant first
 * Other words are invalid. Frames start black and persist until changed. */
#define PREVIEW7_WIDTH            320
#define PREVIEW7_HEIGHT           240
#define PREVIEW7_FPS              30
#define PREVIEW7_COLUMNS          80
#define PREVIEW7_BLOCKS           4800
#define PREVIEW7_FRAME_BYTES      (320UL * 240UL * 2UL)
#define PREVIEW7_THUMB_BYTES      (160UL * 120UL * 2UL)
#define PREVIEW7_BLOCK_BYTES      2048UL
#define PREVIEW7_HEADER_BYTES     64
#define PREVIEW7_MAX_CHUNKS       ((2048 - 64) / 2)
#define PREVIEW7_MAX_ENTRIES4     8192
#define PREVIEW7_MAX_ENTRIES2     4096
#define PREVIEW7_MAX_FRAMES       1800
#define PREVIEW7_MAX_CHUNK_FRAMES 30
#define PREVIEW7_CHUNK_HEADER     24
#define PREVIEW7_AUDIO_RATE       11025UL
#define PREVIEW7_ENTRY4           40UL
#define PREVIEW7_ENTRY2           12UL
/* The map value of a block nobody has drawn yet. */
#define PREVIEW7_BLACK 0x7fffffffUL

/* A checked header block (see the layout above). */
typedef struct Preview7Header {
    unsigned int chunk_frames; /* frames per chunk (the last may have fewer) */
    unsigned int chunks;
    unsigned int preroll;  /* chunks to read before playing starts */
    unsigned int entries4; /* 4x4 dictionary entries in the whole clip */
    unsigned int entries2; /* 2x2 dictionary entries in the whole clip */
    unsigned int slots;    /* chunk buffers the encoder assumed the player has */
    unsigned long frames;
    unsigned long audio_rate;   /* 0 (silent) or 11025 */
    unsigned long max_chunk;    /* the largest chunk, in bytes */
    unsigned long rate;         /* nominal bytes per second */
    const unsigned char *table; /* chunk lengths, BE16 in 2 KiB blocks */
} Preview7Header;

/* A checked chunk: where its parts lie in the chunk's data. */
typedef struct Preview7Chunk {
    unsigned int number, frames;
    unsigned int first4, count4; /* the 4x4 entries it defines */
    unsigned int first2, count2; /* the 2x2 entries it defines */
    unsigned long audio_samples, video_bytes;
    const signed char *audio;
    const unsigned char *entries4, *entries2;
    const unsigned char *offsets; /* frames+1 BE32 offsets; packets follow */
} Preview7Chunk;

/* Decoder state. Decoding a packet draws the blocks it changes into one
 * picture and records in the map what every block shows, so the other
 * picture can be redrawn whole after switching:
 *   PREVIEW7_SCREEN  a 320x240 screen bitmap, drawn 1:1. Screen bitmaps hold
 *                    two lines per 32-bit word, the upper line in the high
 *                    half: pixel (x,y) is halfword (y&1) of word (y>>1)*320+x.
 *   PREVIEW7_TV      the 160x120 TV picture of 2x2 averages, in rows.
 * The caller provides all memory:
 *   thumb   160x120 RGB555, 4-byte aligned
 *   map     2*4800 words
 *   dict4   8192*32 bytes (stored in screen layout: two line pairs of four
 *           words), thumb4 8192*8 bytes (2x2 averages in rows)
 *   dict2   4096*8 bytes (one line pair of two words),
 *   thumb2  4096 words (average << 16)
 * Every entry number a packet can express has room, so the console's
 * decoder does not check them (the C decoder does). The caller also sets
 * `screen` to the bitmap PREVIEW7_SCREEN draws into.
 *
 * The assembly decoder relies on this structure's field offsets: do not
 * reorder them. */
#define PREVIEW7_SCREEN 0
#define PREVIEW7_TV     1
typedef struct Preview7 {
    unsigned char *screen, *thumb;
    unsigned long *map;
    unsigned char *dict4, *thumb4, *dict2;
    unsigned long *thumb2;
    unsigned int entries4, entries2;
    unsigned int limit4, limit2; /* set by preview7_frame for the decoder */
} Preview7;

/* Bytes the map and dictionaries need. */
unsigned long preview7_state_bytes(void);
/* Parse and check the 2048-byte header block. 0 or -1. */
int preview7_header(const unsigned char *block, Preview7Header *header);
/* Check a whole padded chunk read for chunk `number` of a clip with this
 * header (its sum, header fields and frame table, not the packets) and fill
 * info. Does not touch the state. 0 or -1. */
int preview7_chunk(const Preview7Header *header, unsigned int number, const unsigned char *data,
                   unsigned long bytes, Preview7Chunk *info);
/* The same, given the word sum of the chunk after its first 4 bytes. */
int preview7_chunk_summed(const Preview7Header *header, unsigned int number,
                          const unsigned char *data, unsigned long bytes, unsigned long sum,
                          Preview7Chunk *info);
/* A file's entries from 16 (4x4) or 4 (2x2) BE RGB555 pixels in rows. */
void preview7_entry4(const unsigned char *rows, unsigned char *entry);
void preview7_entry2(const unsigned char *rows, unsigned char *entry);
/* Store a checked chunk's entries (copies). */
void preview7_define(Preview7 *state, const Preview7Chunk *info);
/* Every block black (the pictures are not drawn). */
void preview7_reset(Preview7 *state);
/* Decode packet `frame` of a checked chunk, drawing the blocks it changes
 * into `picture`. 0, or -1 for a malformed packet, which may leave the
 * picture and map partly updated. The C decoder also rejects entry numbers
 * the chunk has not defined; the console's assembly does not. */
int preview7_frame(Preview7 *state, const Preview7Chunk *info, unsigned int frame, int picture);
/* Draw every block of `picture` from the map, e.g. after switching. */
void preview7_render(Preview7 *state, int picture);
/* The preview word sum (see usb_media.c) of whole words, all at once or a
 * piece at a time (start from zero; add pieces of whole words in order). */
typedef struct Preview7Sum {
    unsigned long a, b;
} Preview7Sum;
unsigned long preview7_sum(const unsigned char *data, unsigned long bytes);
void preview7_sum_add(Preview7Sum *sum, const unsigned char *data, unsigned long bytes);
unsigned long preview7_sum_value(const Preview7Sum *sum);
#endif
