/*
 * OM3DVID7 streamed preview decoder (see preview_v7.h for the format).
 *
 * The console build runs the packet loop and full redraws in
 * preview_v7_arm.s. This C version runs everywhere else: it is compiled
 * into the PC encoder to check its output, and is the reference the
 * assembly is tested against. Pixels are big-endian RGB555, as the 3DO's
 * cel engine reads them. It uses no C library calls, so the encoder DLL,
 * which has no C runtime, can include it.
 *
 * The block map. For each of the 4800 4x4 blocks, the map keeps two words
 * saying what the block shows, so either picture can be redrawn at any time:
 *   first word PREVIEW7_BLACK               the block is black
 *   first word with the top bit clear       4x4 dictionary entry number
 *   first word with the top bit set         four 2x2 entries: the first word's
 *                                           low 24 bits are top-left << 12 |
 *                                           top-right, the second word is
 *                                           bottom-left << 12 | bottom-right
 */
#include "preview_v7.h"

/* The top bit of a map word marks a block made of four 2x2 entries. */
#define QUAD_BLOCK 0x80000000UL

/* Screen bitmap geometry: each 32-bit word holds two lines' pixels, so a
 * "line pair" row of 320 words is 1280 bytes, and a block row of two line
 * pairs is 2560 bytes. */
#define LINE_PAIR_BYTES (PREVIEW7_WIDTH * 4UL)
#define BLOCK_ROW_BYTES (2 * LINE_PAIR_BYTES)

/* The TV picture: 160 pixels of 2 bytes per row. */
#define TV_ROW_BYTES (PREVIEW7_WIDTH / 2 * 2UL)

/* Header fields, by offset (see preview_v7.h). */
#define HEADER_CHECK  40
#define HEADER_FIELDS 46 /* zeros from here to PREVIEW7_HEADER_BYTES */

static unsigned int read_be16(const unsigned char *p)
{
    return ((unsigned int)p[0] << 8) | p[1];
}

static unsigned long read_be32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) |
           p[3];
}

static void write_be32(unsigned char *p, unsigned long value)
{
    p[0] = (unsigned char)(value >> 24);
    p[1] = (unsigned char)(value >> 16);
    p[2] = (unsigned char)(value >> 8);
    p[3] = (unsigned char)value;
}

/* Copies 4 bytes (no library call: see the top of the file). */
static void copy_word(unsigned char *destination, const unsigned char *source)
{
    destination[0] = source[0];
    destination[1] = source[1];
    destination[2] = source[2];
    destination[3] = source[3];
}

/* ---- The preview word sum --------------------------------------------- */

#ifdef __CC_NORCROFT
/* The console build's version, in preview_v7_arm.s. */
void preview7_sum_arm(Preview7Sum *sum, const unsigned char *data, unsigned long bytes);
#endif

void preview7_sum_add(Preview7Sum *sum, const unsigned char *data, unsigned long bytes)
{
#ifdef __CC_NORCROFT
    preview7_sum_arm(sum, data, bytes & ~3UL);
#else
    unsigned long a = sum->a, b = sum->b, i;
    for (i = 0; i + 4 <= bytes; i += 4) {
        a = (a + read_be32(data + i)) & 0xffffffffUL;
        b = (b + a) & 0xffffffffUL;
    }
    sum->a = a;
    sum->b = b;
#endif
}

unsigned long preview7_sum_value(const Preview7Sum *sum)
{
    unsigned long rotated_b = ((sum->b << 16) | (sum->b >> 16)) & 0xffffffffUL;
    return (sum->a ^ rotated_b) & 0xffffffffUL;
}

unsigned long preview7_sum(const unsigned char *data, unsigned long bytes)
{
    Preview7Sum sum;
    sum.a = sum.b = 0;
    preview7_sum_add(&sum, data, bytes);
    return preview7_sum_value(&sum);
}

/* ---- Header and chunks -------------------------------------------------- */

unsigned long preview7_state_bytes(void)
{
    /* The map (two words per block), the 4x4 dictionary (32 bytes of screen
     * pixels and 8 of TV pixels per entry), and the 2x2 dictionary (8 bytes
     * and a word per entry). */
    return PREVIEW7_BLOCKS * 2UL * sizeof(unsigned long) + PREVIEW7_MAX_ENTRIES4 * 40UL +
           PREVIEW7_MAX_ENTRIES2 * (8UL + sizeof(unsigned long));
}

int preview7_header(const unsigned char *block, Preview7Header *header)
{
    unsigned long stored_check, i;
    unsigned long a = 0, b = 0;

    if (block[0] != 'O' || block[1] != 'M' || block[2] != '3' || block[3] != 'D' ||
        block[4] != 'V' || block[5] != 'I' || block[6] != 'D' || block[7] != '7')
        return -1;
    if (read_be16(block + 8) != PREVIEW7_WIDTH || read_be16(block + 10) != PREVIEW7_HEIGHT ||
        read_be16(block + 12) != PREVIEW7_FPS)
        return -1;

    header->chunk_frames = read_be16(block + 14);
    header->frames = read_be32(block + 16);
    header->chunks = read_be16(block + 20);
    header->preroll = read_be16(block + 22);
    header->entries4 = read_be16(block + 24);
    header->entries2 = read_be16(block + 26);
    header->audio_rate = read_be32(block + 28);
    header->max_chunk = read_be32(block + 32);
    header->rate = read_be32(block + 36);
    stored_check = read_be32(block + HEADER_CHECK);
    header->slots = read_be16(block + 44);
    header->table = block + PREVIEW7_HEADER_BYTES;

    /* Every value within the format's limits and consistent with the others. */
    if (!header->chunk_frames || header->chunk_frames > PREVIEW7_MAX_CHUNK_FRAMES ||
        !header->frames || header->frames > PREVIEW7_MAX_FRAMES)
        return -1;
    if (header->chunks != (header->frames + header->chunk_frames - 1) / header->chunk_frames ||
        header->chunks > PREVIEW7_MAX_CHUNKS || !header->preroll ||
        header->preroll > header->chunks)
        return -1;
    if (header->entries4 > PREVIEW7_MAX_ENTRIES4 || header->entries2 > PREVIEW7_MAX_ENTRIES2)
        return -1;
    if ((header->audio_rate && header->audio_rate != PREVIEW7_AUDIO_RATE) || !header->max_chunk ||
        header->max_chunk % PREVIEW7_BLOCK_BYTES)
        return -1;
    if (header->slots < header->preroll)
        return -1;

    /* Unused header bytes are zero. */
    for (i = HEADER_FIELDS; i < PREVIEW7_HEADER_BYTES; ++i) {
        if (block[i])
            return -1;
    }
    /* Every chunk has a length, none longer than the stated largest. */
    for (i = 0; i < header->chunks; ++i) {
        unsigned long bytes = read_be16(header->table + 2 * i) * PREVIEW7_BLOCK_BYTES;
        if (!bytes || bytes > header->max_chunk)
            return -1;
    }
    /* And the rest of the block after the chunk table is zero. */
    for (i = PREVIEW7_HEADER_BYTES + 2UL * header->chunks; i < PREVIEW7_BLOCK_BYTES; ++i) {
        if (block[i])
            return -1;
    }

    /* The check is the word sum of the whole block, with the check field
     * itself counted as zero. */
    for (i = 0; i < PREVIEW7_BLOCK_BYTES; i += 4) {
        unsigned long word = i == HEADER_CHECK ? 0 : read_be32(block + i);
        a = (a + word) & 0xffffffffUL;
        b = (b + a) & 0xffffffffUL;
    }
    return (a ^ (((b << 16) | (b >> 16)) & 0xffffffffUL)) == stored_check ? 0 : -1;
}

/* Audio samples from the start of the clip to frame `frame`. */
static unsigned long samples_to(unsigned long frame)
{
    return frame * PREVIEW7_AUDIO_RATE / PREVIEW7_FPS;
}

int preview7_chunk(const Preview7Header *header, unsigned int number, const unsigned char *data,
                   unsigned long bytes, Preview7Chunk *chunk)
{
    return preview7_chunk_summed(header, number, data, bytes, preview7_sum(data + 4, bytes - 4),
                                 chunk);
}

int preview7_chunk_summed(const Preview7Header *header, unsigned int number,
                          const unsigned char *data, unsigned long bytes, unsigned long sum,
                          Preview7Chunk *chunk)
{
    unsigned long position, first_frame, expected_frames, expected_samples, audio_padded;
    unsigned long table_at, i;

    /* The right length (from the header's table) and the right sum. */
    if (number >= header->chunks ||
        bytes != read_be16(header->table + 2 * number) * PREVIEW7_BLOCK_BYTES)
        return -1;
    if (sum != read_be32(data))
        return -1;

    chunk->number = read_be16(data + 4);
    chunk->frames = read_be16(data + 6);
    chunk->first4 = read_be16(data + 8);
    chunk->count4 = read_be16(data + 10);
    chunk->first2 = read_be16(data + 12);
    chunk->count2 = read_be16(data + 14);
    chunk->audio_samples = read_be32(data + 16);
    chunk->video_bytes = read_be32(data + 20);

    /* Its number, frame count and entries fit the header. Every chunk has
     * chunk_frames frames except perhaps the last. */
    first_frame = (unsigned long)number * header->chunk_frames;
    expected_frames = header->frames - first_frame < header->chunk_frames
                          ? header->frames - first_frame
                          : header->chunk_frames;
    if (chunk->number != number || !chunk->frames || chunk->frames != expected_frames ||
        (unsigned long)chunk->first4 + chunk->count4 > header->entries4 ||
        (unsigned long)chunk->first2 + chunk->count2 > header->entries2)
        return -1;

    /* Exactly the audio samples for its frames. */
    expected_samples =
        header->audio_rate ? samples_to(first_frame + chunk->frames) - samples_to(first_frame) : 0;
    if (chunk->audio_samples != expected_samples)
        return -1;

    /* Lay out the parts: audio (padded to a word), 4x4 entries, 2x2
     * entries, then the frame offsets and packets. */
    audio_padded = (chunk->audio_samples + 3) & ~3UL;
    position = PREVIEW7_CHUNK_HEADER;
    chunk->audio = (const signed char *)(data + position);
    position += audio_padded;
    chunk->entries4 = data + position;
    position += chunk->count4 * PREVIEW7_ENTRY4;
    chunk->entries2 = data + position;
    position += chunk->count2 * PREVIEW7_ENTRY2;
    table_at = position;
    if (position > bytes || chunk->video_bytes > bytes - position ||
        chunk->video_bytes < (chunk->frames + 1) * 4UL)
        return -1;
    chunk->offsets = data + table_at;

    /* Offsets run from just after the table to the end of the video, each
     * packet at least one 16-bit word long. */
    if (read_be32(chunk->offsets) != (chunk->frames + 1) * 4UL ||
        read_be32(chunk->offsets + chunk->frames * 4UL) != chunk->video_bytes)
        return -1;
    for (i = 0; i < chunk->frames; ++i) {
        unsigned long begin = read_be32(chunk->offsets + i * 4);
        unsigned long end = read_be32(chunk->offsets + i * 4 + 4);
        if (end < begin + 2 || (end - begin) & 1)
            return -1;
    }

    /* Padding is zero: after the audio, and after the video to the end. */
    for (i = audio_padded - chunk->audio_samples; i; --i) {
        if (data[PREVIEW7_CHUNK_HEADER + audio_padded - i])
            return -1;
    }
    for (i = table_at + chunk->video_bytes; i < bytes; ++i) {
        if (data[i])
            return -1;
    }
    return 0;
}

/* ---- Dictionary entries ------------------------------------------------- */

/*
 * The rounded average of four RGB555 pixels at byte offsets a, b, c and d
 * of `pixels`, channel by channel.
 */
static unsigned int average_of_four(const unsigned char *pixels, unsigned int a, unsigned int b,
                                    unsigned int c, unsigned int d)
{
    unsigned int pa = read_be16(pixels + a), pb = read_be16(pixels + b);
    unsigned int pc = read_be16(pixels + c), pd = read_be16(pixels + d);
    unsigned int result = 0, shift;

    for (shift = 0; shift < 15; shift += 5) {
        unsigned int total = ((pa >> shift) & 31) + ((pb >> shift) & 31) + ((pc >> shift) & 31) +
                             ((pd >> shift) & 31);
        result |= ((total + 2) >> 2) << shift;
    }
    return result;
}

/* Two pixels of the same column on consecutive lines, as one screen word
 * (the upper line's pixel in the high half). */
static unsigned long line_pair(const unsigned char *upper, const unsigned char *lower)
{
    return ((unsigned long)read_be16(upper) << 16) | read_be16(lower);
}

void preview7_entry4(const unsigned char *rows, unsigned char *entry)
{
    unsigned int k;

    /* `rows` holds four rows of four pixels, 8 bytes apart. Screen words:
     * line pair (rows 0, 1), then line pair (rows 2, 3). */
    for (k = 0; k < 4; ++k) {
        write_be32(entry + k * 4, line_pair(rows + k * 2, rows + 8 + k * 2));
        write_be32(entry + 16 + k * 4, line_pair(rows + 16 + k * 2, rows + 24 + k * 2));
    }
    /* TV pixels: the four 2x2 averages, two per word. */
    write_be32(entry + 32, ((unsigned long)average_of_four(rows, 0, 2, 8, 10) << 16) |
                               average_of_four(rows, 4, 6, 12, 14));
    write_be32(entry + 36, ((unsigned long)average_of_four(rows, 16, 18, 24, 26) << 16) |
                               average_of_four(rows, 20, 22, 28, 30));
}

void preview7_entry2(const unsigned char *rows, unsigned char *entry)
{
    /* Two rows of two pixels, 4 bytes apart: one line pair, then the average. */
    write_be32(entry, line_pair(rows, rows + 4));
    write_be32(entry + 4, line_pair(rows + 2, rows + 6));
    write_be32(entry + 8, (unsigned long)average_of_four(rows, 0, 2, 4, 6) << 16);
}

void preview7_define(Preview7 *state, const Preview7Chunk *chunk)
{
    unsigned int i, k;

    /* 4x4 entries: 32 bytes of screen words, then 8 bytes of TV pixels. */
    for (i = 0; i < chunk->count4; ++i) {
        const unsigned char *source = chunk->entries4 + i * PREVIEW7_ENTRY4;
        unsigned char *screen_part = state->dict4 + (chunk->first4 + i) * 32UL;
        unsigned char *tv_part = state->thumb4 + (chunk->first4 + i) * 8UL;
        for (k = 0; k < 32; k += 4)
            copy_word(screen_part + k, source + k);
        copy_word(tv_part, source + 32);
        copy_word(tv_part + 4, source + 36);
    }

    /* 2x2 entries: 8 bytes of screen words, then the average. */
    for (i = 0; i < chunk->count2; ++i) {
        const unsigned char *source = chunk->entries2 + i * PREVIEW7_ENTRY2;
        unsigned char *screen_part = state->dict2 + (chunk->first2 + i) * 8UL;
        copy_word(screen_part, source);
        copy_word(screen_part + 4, source + 4);
        state->thumb2[chunk->first2 + i] = read_be32(source + 8);
    }
}

/* ---- Drawing ------------------------------------------------------------ */

/* Draws one block of the TV picture from its map words. */
static void draw_tv_block(Preview7 *state, unsigned long block_x, unsigned long block_y,
                          unsigned long first, unsigned long second)
{
    /* A block is 2x2 TV pixels: 4 bytes on each of two rows. */
    unsigned char *pixel = state->thumb + block_y * 2UL * TV_ROW_BYTES + block_x * 4UL;

    if (first == PREVIEW7_BLACK) {
        write_be32(pixel, 0);
        write_be32(pixel + TV_ROW_BYTES, 0);
    } else if (!(first & QUAD_BLOCK)) {
        copy_word(pixel, state->thumb4 + first * 8UL);
        copy_word(pixel + TV_ROW_BYTES, state->thumb4 + first * 8UL + 4);
    } else {
        /* Each 2x2 entry's average sits in the high half of its word. */
        write_be32(pixel,
                   state->thumb2[(first >> 12) & 0xfff] | (state->thumb2[first & 0xfff] >> 16));
        write_be32(pixel + TV_ROW_BYTES,
                   state->thumb2[(second >> 12) & 0xfff] | (state->thumb2[second & 0xfff] >> 16));
    }
}

/* Draws one block of a screen bitmap from its map words. */
static void draw_screen_block(Preview7 *state, unsigned long block_x, unsigned long block_y,
                              unsigned long first, unsigned long second, unsigned char *bitmap)
{
    /* A block covers two line pairs: 16 bytes in each. */
    unsigned char *upper = bitmap + block_y * BLOCK_ROW_BYTES + block_x * 16UL;
    unsigned char *lower = upper + LINE_PAIR_BYTES;
    unsigned int k;

    if (first == PREVIEW7_BLACK) {
        for (k = 0; k < 16; k += 4) {
            write_be32(upper + k, 0);
            write_be32(lower + k, 0);
        }
    } else if (!(first & QUAD_BLOCK)) {
        const unsigned char *entry = state->dict4 + first * 32UL;
        for (k = 0; k < 16; k += 4) {
            copy_word(upper + k, entry + k);
            copy_word(lower + k, entry + 16 + k);
        }
    } else {
        const unsigned char *top_left = state->dict2 + ((first >> 12) & 0xfff) * 8UL;
        const unsigned char *top_right = state->dict2 + (first & 0xfff) * 8UL;
        const unsigned char *bottom_left = state->dict2 + ((second >> 12) & 0xfff) * 8UL;
        const unsigned char *bottom_right = state->dict2 + (second & 0xfff) * 8UL;
        copy_word(upper, top_left);
        copy_word(upper + 4, top_left + 4);
        copy_word(upper + 8, top_right);
        copy_word(upper + 12, top_right + 4);
        copy_word(lower, bottom_left);
        copy_word(lower + 4, bottom_left + 4);
        copy_word(lower + 8, bottom_right);
        copy_word(lower + 12, bottom_right + 4);
    }
}

/* Draws one block from its map words into `picture`. */
static void draw_block(Preview7 *state, unsigned long block, unsigned long first,
                       unsigned long second, int picture)
{
    unsigned long block_x = block % PREVIEW7_COLUMNS;
    unsigned long block_y = block / PREVIEW7_COLUMNS;

    if (picture == PREVIEW7_TV)
        draw_tv_block(state, block_x, block_y, first, second);
    else
        draw_screen_block(state, block_x, block_y, first, second, state->screen);
}

void preview7_reset(Preview7 *state)
{
    unsigned long i;
    for (i = 0; i < PREVIEW7_BLOCKS; ++i) {
        state->map[2 * i] = PREVIEW7_BLACK;
        state->map[2 * i + 1] = 0;
    }
}

#ifdef __CC_NORCROFT
/* The console build's version, in preview_v7_arm.s. */
void preview7_render_arm(Preview7 *state, int picture);
#endif

void preview7_render(Preview7 *state, int picture)
{
#ifdef __CC_NORCROFT
    preview7_render_arm(state, picture);
#else
    unsigned long i;
    for (i = 0; i < PREVIEW7_BLOCKS; ++i)
        draw_block(state, i, state->map[2 * i], state->map[2 * i + 1], picture);
#endif
}

/* ---- Packets ------------------------------------------------------------ */

#ifdef __CC_NORCROFT
/* The console build's packet loop, in preview_v7_arm.s. */
int preview7_decode(Preview7 *state, const unsigned char *data, unsigned long bytes, int picture);
#else
/* Records what a block now shows, and draws it. */
static void store_block(Preview7 *state, unsigned long block, unsigned long first,
                        unsigned long second, int picture)
{
    state->map[2 * block] = first;
    state->map[2 * block + 1] = second;
    draw_block(state, block, first, second, picture);
}

/* Decodes one packet (see preview_v7.h for the word codes). Returns 0, or -1. */
static int preview7_decode(Preview7 *state, const unsigned char *data, unsigned long bytes,
                           int picture)
{
    const unsigned char *end = data + bytes;
    unsigned int limit4 = state->limit4, limit2 = state->limit2;
    unsigned long block = 0;

    while (data < end) {
        unsigned int word = read_be16(data);
        unsigned int count = (word & 0x1fff) + 1;
        unsigned int i;
        data += 2;

        switch (word >> 13) {
        case 0: /* one 4x4 entry */
            if (word >= limit4 || block >= PREVIEW7_BLOCKS)
                return -1;
            store_block(state, block++, word, 0, picture);
            break;

        case 1: /* skip `count` unchanged blocks */
            if (count > PREVIEW7_BLOCKS - block)
                return -1;
            block += count;
            break;

        case 2: /* `count` 4x4 entry numbers follow */
            if (count > PREVIEW7_BLOCKS - block || (unsigned long)(end - data) < count * 2UL)
                return -1;
            for (i = 0; i < count; ++i, data += 2) {
                unsigned int entry = read_be16(data);
                if (entry >= limit4)
                    return -1;
                store_block(state, block++, entry, 0, picture);
            }
            break;

        case 3: /* `count` blocks of four 12-bit 2x2 entry numbers (6 bytes each) */
            if (count > PREVIEW7_BLOCKS - block || (unsigned long)(end - data) < count * 6UL)
                return -1;
            for (i = 0; i < count; ++i, data += 6) {
                unsigned long top = ((unsigned long)read_be16(data) << 8) | data[2];
                unsigned long bottom = ((unsigned long)data[3] << 16) | read_be16(data + 4);
                if (((top >> 12) & 0xfff) >= limit2 || (top & 0xfff) >= limit2 ||
                    ((bottom >> 12) & 0xfff) >= limit2 || (bottom & 0xfff) >= limit2)
                    return -1;
                store_block(state, block++, QUAD_BLOCK | top, bottom, picture);
            }
            break;

        default:
            return -1;
        }
    }
    return block == PREVIEW7_BLOCKS ? 0 : -1;
}
#endif

int preview7_frame(Preview7 *state, const Preview7Chunk *chunk, unsigned int frame, int picture)
{
    unsigned long begin, end;

    if (frame >= chunk->frames)
        return -1;
    begin = read_be32(chunk->offsets + frame * 4UL);
    end = read_be32(chunk->offsets + frame * 4UL + 4);
    /* Packets may only use entries defined so far (the C decoder checks). */
    state->limit4 = chunk->first4 + chunk->count4;
    state->limit2 = chunk->first2 + chunk->count2;
    return preview7_decode(state, chunk->offsets + begin, end - begin, picture);
}
