/*
 * The OM3DVID7 encoder: 320x240 RGB frames (and optional sound) in, a
 * streamed clip out (see FORMAT.md and om3dvid7.h). Original MIT-licensed
 * code, C99.
 *
 * The caller provides all memory as one workspace
 * (om3dvid7_encoder_workspace_bytes says how much), so the Windows DLL needs
 * no C runtime and several clips can be encoded at once on different
 * threads.
 *
 * How a clip is encoded:
 *
 *  1. Dictionaries. Two "books" of entries are trained on the clip's blocks:
 *     4x4 blocks and 2x2 blocks. Training starts with one cluster holding a
 *     sample of every block, splits the cluster with the largest error in two
 *     until there are enough, then refines them with a few rounds of k-means.
 *  2. Nearest entries. Every block of every frame gets its nearest 4x4 entry
 *     and the nearest 2x2 entry for each of its four quarters.
 *  3. Choosing. Frames are encoded a chunk at a time. Each block either keeps
 *     what it shows, or shows its 4x4 entry, or shows its four 2x2 entries:
 *     whichever costs least in error + lambda * bytes + mu * decode clocks.
 *     The first use of an entry also pays for sending it. mu (the price of a
 *     clock) stays zero unless a frame would take the console too long to
 *     decode. A chunk too big for its slot is chosen again with a larger
 *     lambda.
 *  4. Rate. lambda (the price of a byte) is searched so the file averages the
 *     requested bytes per second, and a model of the console's reader checks
 *     the chunks arrive in time.
 *  5. Checking. The finished file is decoded by the menu's own C decoder, and
 *     every frame must match the encoder's model of it.
 *
 * Pixel values are kept at 8 times their 5-bit value (0..248), so averages
 * keep three bits of fraction.
 */
#include <stddef.h>
#include <stdint.h>
#include "om3dvid7.h"
#include "om3dvid7_encoder.h"

#if defined(_WIN32) && defined(OM3DVID7_DLL)
/* The Windows DLL has no C runtime, but the compiler may still call these. */
int _fltused = 0;
void *memcpy(void *destination, const void *source, size_t bytes)
{
    uint8_t *to = (uint8_t *)destination;
    const uint8_t *from = (const uint8_t *)source;
    while (bytes--)
        *to++ = *from++;
    return destination;
}
void *memset(void *destination, int value, size_t bytes)
{
    uint8_t *to = (uint8_t *)destination;
    while (bytes--)
        *to++ = (uint8_t)value;
    return destination;
}
#endif

#define WIDTH         OM3DVID7_WIDTH
#define HEIGHT        OM3DVID7_HEIGHT
#define BLOCK_COUNT   OM3DVID7_BLOCKS  /* 4x4 blocks in a frame */
#define BLOCK_COLUMNS OM3DVID7_COLUMNS /* 4x4 blocks across */

/* Values in a block: 16 pixels of 3 colors for 4x4, 4 pixels for 2x2. */
#define VALUES4 48
#define VALUES2 12

/* The most entries a book can have (the format's limits). */
#define MOST_ENTRIES4 8192
#define MOST_ENTRIES2 4096

/* A file number not given out yet: the entry has not been sent. */
#define UNSENT 0xffff

/* Blocks the dictionaries are trained on, sampled from the whole clip, and
 * the rounds of k-means after splitting. */
#define TRAINING_SAMPLES 393216
#define REFINING_ROUNDS  4

/* Packet bytes of one frame at most, and the room for a chunk's packets
 * (twice its frames', as pre-sending keeps a copy). */
#define PACKET_ROOM  (BLOCK_COUNT * 8 + 64)
#define PACKETS_ROOM ((uint64_t)OM3DVID7_MAX_CHUNK_FRAMES * PACKET_ROOM * 2 + 4096)

/*
 * ARM60 clocks of om3dvid7_decoder_arm.s decoding a frame into the full-screen
 * bitmap, fitted to om3dvid/tests/test_decoder_arm.py --clocks counts (within
 * 300 clocks a frame): per frame, per op and per block of a run. The browser's
 * TV picture costs less per block.
 */
#define CLOCKS_FRAME    721 /* setting up a frame */
#define CLOCKS_KEEP_RUN 49  /* a run of unchanged blocks */
#define CLOCKS_ONE_FOUR 79  /* a lone 4x4 block */
#define CLOCKS_FOUR_RUN 60  /* starting a run of 4x4 blocks */
#define CLOCKS_FOUR     44  /* each block of that run */
#define CLOCKS_QUAD_RUN 56  /* starting a run of 2x2 quads */
#define CLOCKS_QUAD     68  /* each quad of that run */

/* What a block shows, and what the encoder chooses for it in a frame. The
 * numbers match: choosing a kind makes the block show that kind. */
#define SHOWS_BLACK 0 /* nothing drawn yet */
#define SHOWS_FOUR  1 /* one 4x4 entry */
#define SHOWS_QUAD  2 /* four 2x2 entries */
#define CHOOSE_KEEP 0 /* leave the block as it is */
#define CHOOSE_FOUR 1
#define CHOOSE_QUAD 2

/* A dictionary: its entries by number, and the same sorted by the sum of
 * their values for the nearest-entry search. */
typedef struct Book {
    int values; /* VALUES4 or VALUES2 per entry */
    int entries;
    uint8_t *sorted;        /* entries * values, sorted by sum */
    uint32_t *sorted_sum;   /* each sorted entry's sum */
    uint16_t *sorted_entry; /* each sorted position's entry number */
    uint8_t *by_entry;      /* entries * values, by entry number */
} Book;

/* What every block of the picture shows, as the console will have it. */
typedef struct Picture {
    uint8_t kind[BLOCK_COUNT];     /* SHOWS_* */
    uint16_t four[BLOCK_COUNT];    /* SHOWS_FOUR: the entry */
    uint16_t quad[BLOCK_COUNT][4]; /* SHOWS_QUAD: the entries of its four quarters */
    uint32_t next4, next2;         /* the next file numbers to give out */
} Picture;

/* Everything the encoder keeps, at the start of the workspace. The buffers
 * it points to follow it (see lay_out). */
typedef struct Work {
    uint64_t workspace_bytes;
    uint32_t frames, frames_added;

    /* The clip, and each block's nearest entries (step 2). */
    uint8_t *source;          /* frames * BLOCK_COUNT * VALUES4 */
    uint16_t *nearest4;       /* each block's nearest 4x4 entry */
    uint32_t *nearest4_error; /* and its error */
    uint16_t *nearest2;       /* each block's four quarters' nearest 2x2 entries */
    uint32_t *nearest2_error; /* and their summed error */
    uint8_t *visibility;      /* how much each block's error counts (see visibility) */
    Book book4, book2;

    /* Training (step 1). */
    uint8_t *samples;       /* TRAINING_SAMPLES * VALUES4 */
    uint32_t *sample_entry; /* each sample's nearest entry */
    uint32_t *leaves;       /* per cluster: first sample, end, cannot split */
    uint64_t *leaf_error;   /* per cluster: summed squared error */
    int64_t *sums;          /* per entry: summed values (also sorting scratch) */
    uint32_t *counts;       /* per entry: samples nearest it */

    /* Choosing (step 3). The picture now, and as it was when the chunk and
     * the frame being chosen began (in the workspace: too big for a stack). */
    Picture picture, at_chunk_start, at_frame_start;
    uint16_t *file_number4, *file_number2; /* entry -> its number in the file, or UNSENT */
    float *send_cost4, *send_cost2;        /* bytes charged for sending an entry: its
                                     * size spread over its likely uses */
    uint16_t *sent4, *sent2;               /* the entries this chunk sends, in file order */
    uint32_t sent4_count, sent2_count;
    uint8_t *packets;                          /* the packets of the chunk being encoded */
    uint8_t *choices;                          /* BLOCK_COUNT: the frame's CHOOSE_* */
    uint8_t *expected;                         /* RGB555 rows per frame of a chunk, plus the frame
                         * before it: the encoder's model of the console */
    unsigned char *check_screen, *check_thumb; /* step 5's decoder */
    unsigned char *check_state;
    uint32_t *chunk_bytes;       /* each chunk's padded size */
    double *read_done, *play_at; /* the reader model: two loops of chunks */
    double clock_price;          /* mu, carried from frame to frame */
    uint32_t slowest_frame_clocks;

    /* Pre-sending (see pre_send): the state after a chunk's frames, kept
     * while the next chunk is tried, and the entries that try sent. */
    Picture kept_picture, kept_chunk_start;
    uint16_t *kept_number4, *kept_number2, *kept_sent4, *kept_sent2, *wanted4, *wanted2;
    uint8_t *kept_packets;
    int trying_next_chunk;
    uint32_t lambda_raises; /* how often the last chunk raised lambda to fit */
} Work;

/* ---- The workspace -------------------------------------------------------- */

/* Hands out `bytes` at `*at`, keeping 16-byte alignment. */
static void *take(uint8_t **at, uint64_t bytes)
{
    void *memory = (void *)*at;
    *at += (bytes + 15) & ~(uint64_t)15;
    return memory;
}

/* Places the buffers for a clip of `frames` frames from `base`, and returns
 * the workspace bytes they and the Work structure need. */
static uint64_t lay_out(Work *w, uint8_t *base, uint32_t frames)
{
    uint8_t *at = base;
    uint64_t f = frames;
    w->source = take(&at, f * BLOCK_COUNT * VALUES4);
    w->nearest4 = take(&at, f * BLOCK_COUNT * 2);
    w->nearest4_error = take(&at, f * BLOCK_COUNT * 4);
    w->nearest2 = take(&at, f * BLOCK_COUNT * 8);
    w->nearest2_error = take(&at, f * BLOCK_COUNT * 4);
    w->visibility = take(&at, f * BLOCK_COUNT);
    w->book4.sorted = take(&at, MOST_ENTRIES4 * VALUES4);
    w->book4.sorted_sum = take(&at, MOST_ENTRIES4 * 4);
    w->book4.sorted_entry = take(&at, MOST_ENTRIES4 * 2);
    w->book4.by_entry = take(&at, MOST_ENTRIES4 * VALUES4);
    w->book2.sorted = take(&at, MOST_ENTRIES2 * VALUES2);
    w->book2.sorted_sum = take(&at, MOST_ENTRIES2 * 4);
    w->book2.sorted_entry = take(&at, MOST_ENTRIES2 * 2);
    w->book2.by_entry = take(&at, MOST_ENTRIES2 * VALUES2);
    w->samples = take(&at, (uint64_t)TRAINING_SAMPLES * VALUES4);
    w->sample_entry = take(&at, (uint64_t)TRAINING_SAMPLES * 4);
    w->leaves = take(&at, MOST_ENTRIES4 * 3 * 4);
    w->leaf_error = take(&at, MOST_ENTRIES4 * 8);
    w->sums = take(&at, MOST_ENTRIES4 * VALUES4 * 8);
    w->counts = take(&at, MOST_ENTRIES4 * 4);
    w->file_number4 = take(&at, MOST_ENTRIES4 * 2);
    w->file_number2 = take(&at, MOST_ENTRIES2 * 2);
    w->send_cost4 = take(&at, MOST_ENTRIES4 * 4);
    w->send_cost2 = take(&at, MOST_ENTRIES2 * 4);
    w->sent4 = take(&at, MOST_ENTRIES4 * 2);
    w->sent2 = take(&at, MOST_ENTRIES2 * 2);
    w->packets = take(&at, PACKETS_ROOM);
    w->kept_packets = take(&at, PACKETS_ROOM);
    w->kept_number4 = take(&at, MOST_ENTRIES4 * 2);
    w->kept_number2 = take(&at, MOST_ENTRIES2 * 2);
    w->kept_sent4 = take(&at, MOST_ENTRIES4 * 2);
    w->kept_sent2 = take(&at, MOST_ENTRIES2 * 2);
    w->wanted4 = take(&at, MOST_ENTRIES4 * 2);
    w->wanted2 = take(&at, MOST_ENTRIES2 * 2);
    w->choices = take(&at, BLOCK_COUNT);
    w->expected = take(&at, (OM3DVID7_MAX_CHUNK_FRAMES + 1) * OM3DVID7_FRAME_BYTES);
    w->check_screen = take(&at, OM3DVID7_FRAME_BYTES);
    w->check_thumb = take(&at, OM3DVID7_THUMB_BYTES);
    w->check_state = take(&at, om3dvid7_state_bytes() + 64);
    w->chunk_bytes = take(&at, OM3DVID7_MAX_CHUNKS * 4);
    w->read_done = take(&at, 2 * OM3DVID7_MAX_CHUNKS * sizeof(double));
    w->play_at = take(&at, 2 * OM3DVID7_MAX_CHUNKS * sizeof(double));
    return (uint64_t)(at - base) + sizeof(Work) + 64;
}

OM3DVID7_EXPORT uint32_t om3dvid7_encoder_abi(void)
{
    return OM3DVID7_ENCODER_ABI;
}

OM3DVID7_EXPORT void om3dvid7_encoder_default_settings(Om3dvid7EncoderSettings *settings)
{
    settings->rate = 200 * 1024;
    settings->read_rate = 275 * 1024;
    settings->slot_bytes = 640 * 1024;
    settings->max_chunk = 160 * 1024;
    settings->chunk_frames = 10;
    settings->entries4 = 8192;
    settings->entries2 = 4096;
    settings->cycle_cap = 230000;
}

OM3DVID7_EXPORT uint64_t om3dvid7_encoder_workspace_bytes(uint32_t frames)
{
    Work sizes_only;
    if (!frames || frames > OM3DVID7_MAX_FRAMES)
        return 0;
    /* Only the sizes matter here, so any base address does. */
    return lay_out(&sizes_only, (uint8_t *)(uintptr_t)64, frames);
}

/* The Work structure in a workspace: at its first 16-byte boundary. */
static Work *work_in(void *workspace)
{
    return (Work *)(((uintptr_t)workspace + 15) & ~(uintptr_t)15);
}

OM3DVID7_EXPORT int32_t om3dvid7_encoder_begin(void *workspace, uint64_t bytes, uint32_t frames)
{
    Work *w;
    if (!workspace || !frames || frames > OM3DVID7_MAX_FRAMES ||
        bytes < om3dvid7_encoder_workspace_bytes(frames))
        return -1;
    w = work_in(workspace);
    w->workspace_bytes = bytes;
    w->frames = frames;
    w->frames_added = 0;
    lay_out(w, (uint8_t *)(w + 1), frames);
    return 0;
}

/* ---- Taking in frames --------------------------------------------------- */

/* A square root by Newton's method (there is no C library to call). */
static double square_root(double x)
{
    double root = x > 1 ? x : 1;
    int i;
    for (i = 0; i < 60; ++i)
        root = (root + x / root) / 2;
    return root;
}

/*
 * How much a block's error counts, 1..255 for 0..1: fully in flat areas,
 * where blockiness shows, less in busy ones that hide it. From the variance
 * of its source brightness.
 */
#define FLAT_VARIANCE 400.0
static uint8_t visibility(const uint8_t *block)
{
    double brightness[16], mean = 0, variance = 0;
    int i;
    for (i = 0; i < 16; ++i) {
        brightness[i] =
            (77 * block[i * 3] + 150 * block[i * 3 + 1] + 29 * block[i * 3 + 2]) / 256.0;
        mean += brightness[i];
    }
    mean /= 16;
    for (i = 0; i < 16; ++i)
        variance += (brightness[i] - mean) * (brightness[i] - mean);
    return (uint8_t)(1 + 254 * square_root(FLAT_VARIANCE / (FLAT_VARIANCE + variance / 16)) + 0.5);
}

/* Stores the frame as blocks of 8x-scale values, and how visible each
 * block's errors are. */
OM3DVID7_EXPORT int32_t om3dvid7_encoder_add_frame(void *workspace, const uint8_t *rgb)
{
    Work *w = work_in(workspace);
    uint8_t *out;
    int block, i;
    if (!rgb || w->frames_added >= w->frames)
        return -1;
    out = w->source + (uint64_t)w->frames_added * BLOCK_COUNT * VALUES4;
    for (block = 0; block < BLOCK_COUNT; ++block) {
        int left = (block % BLOCK_COLUMNS) * 4, top = (block / BLOCK_COLUMNS) * 4;
        for (i = 0; i < 16; ++i) {
            const uint8_t *pixel = rgb + ((top + i / 4) * WIDTH + left + i % 4) * 3;
            int color;
            /* 5-bit units with three bits of fraction: 255 becomes 248. */
            for (color = 0; color < 3; ++color)
                *out++ = (uint8_t)((pixel[color] * 248 + 127) / 255);
        }
        w->visibility[(uint64_t)w->frames_added * BLOCK_COUNT + block] = visibility(out - VALUES4);
    }
    ++w->frames_added;
    return 0;
}

/* ---- Errors and the nearest entry ------------------------------------- */

/* Quarter `quarter` (0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right)
 * of a 4x4 block, as a 2x2 block. */
static void quarter_of(const uint8_t *block, int quarter, uint8_t *out)
{
    int quarter_row = quarter >> 1, quarter_column = quarter & 1, row, column;
    for (row = 0; row < 2; ++row)
        for (column = 0; column < 2; ++column) {
            const uint8_t *pixel =
                block + ((quarter_row * 2 + row) * 4 + quarter_column * 2 + column) * 3;
            out[(row * 2 + column) * 3] = pixel[0];
            out[(row * 2 + column) * 3 + 1] = pixel[1];
            out[(row * 2 + column) * 3 + 2] = pixel[2];
        }
}

static uint32_t value_sum(const uint8_t *values, int count)
{
    uint32_t sum = 0;
    int i;
    for (i = 0; i < count; ++i)
        sum += values[i];
    return sum;
}

/* A pixel's error as the eye weighs it: brightness in full, color at half
 * (Cb/Cr scaled as in YCbCr), so a gray error e counts 3*e*e as in RGB. */
static uint32_t pixel_error(int red_error, int green_error, int blue_error)
{
    int64_t luma = 77 * red_error + 150 * green_error + 29 * blue_error;
    int64_t blue_difference = blue_error * 256 - luma, red_difference = red_error * 256 - luma;
    double weighed = (double)luma * luma + 0.5 * (0.32 * (double)blue_difference * blue_difference +
                                                  0.51 * (double)red_difference * red_difference);
    return (uint32_t)(weighed * 3.0 / 65536.0 + 0.5);
}

/* pixel_error is never below this share of the plain RGB squared error
 * (0.39, with room for rounding); the nearest-entry search relies on it. */
#define METRIC_FLOOR 0.35

/* The error between two blocks of `count` values, stopping early (a row of
 * four pixels at a time) once it reaches `limit`. */
static uint32_t distance(const uint8_t *a, const uint8_t *b, int count, uint32_t limit)
{
    uint32_t total = 0;
    int row;
    for (row = 0; row < count; row += 12) {
        int k;
        for (k = row; k < row + 12; k += 3)
            total += pixel_error((int)a[k] - (int)b[k], (int)a[k + 1] - (int)b[k + 1],
                                 (int)a[k + 2] - (int)b[k + 2]);
        if (total >= limit)
            return total;
    }
    return total;
}

static void swap_values(uint8_t *a, uint8_t *b, int count)
{
    int i;
    for (i = 0; i < count; ++i) {
        uint8_t kept = a[i];
        a[i] = b[i];
        b[i] = kept;
    }
}

/* Sorts the book's entries by the sum of their values (a counting sort, so
 * entries with equal sums keep their order). `scratch` holds a count per
 * possible sum. */
static void sort_book(Book *book, uint32_t *scratch)
{
    int i, largest_sum = book->values * 248 + 1;
    uint32_t *start = scratch;
    for (i = 0; i <= largest_sum; ++i)
        start[i] = 0;
    for (i = 0; i < book->entries; ++i)
        ++start[value_sum(book->by_entry + i * book->values, book->values) + 1];
    for (i = 1; i <= largest_sum; ++i)
        start[i] += start[i - 1];
    for (i = 0; i < book->entries; ++i) {
        uint32_t sum = value_sum(book->by_entry + i * book->values, book->values),
                 position = start[sum]++;
        int k;
        book->sorted_sum[position] = sum;
        book->sorted_entry[position] = (uint16_t)i;
        for (k = 0; k < book->values; ++k)
            book->sorted[position * book->values + k] = book->by_entry[i * book->values + k];
    }
}

/*
 * The entry nearest `block`, and its error in `*best_error`. An equal-average
 * search: entries are sorted by their sum, and the search walks outward from
 * the block's own sum in both directions. Two blocks whose sums differ by e
 * are at least e*e/values apart in plain squared error, so each direction
 * stops once that bound (scaled by METRIC_FLOOR) passes the best error found.
 */
static uint32_t nearest_entry(const Book *book, const uint8_t *block, uint32_t *best_error)
{
    uint32_t sum = value_sum(block, book->values), best = 0xffffffffu, best_position = 0;
    int low = 0, high = book->entries, up, down;

    /* The first entry whose sum is at least the block's. */
    while (low < high) {
        int middle = (low + high) / 2;
        if (book->sorted_sum[middle] < sum)
            low = middle + 1;
        else
            high = middle;
    }
    up = low;
    down = low - 1;

    while (up < book->entries || down >= 0) {
        if (up < book->entries) {
            uint32_t gap = book->sorted_sum[up] - sum;
            if ((double)gap * gap * METRIC_FLOOR >= (double)best * book->values)
                up = book->entries;
            else {
                uint32_t error =
                    distance(block, book->sorted + (size_t)up * book->values, book->values, best);
                if (error < best) {
                    best = error;
                    best_position = up;
                }
                ++up;
            }
        }
        if (down >= 0) {
            uint32_t gap = sum - book->sorted_sum[down];
            if ((double)gap * gap * METRIC_FLOOR >= (double)best * book->values)
                down = -1;
            else {
                uint32_t error =
                    distance(block, book->sorted + (size_t)down * book->values, book->values, best);
                if (error < best) {
                    best = error;
                    best_position = down;
                }
                --down;
            }
        }
    }
    *best_error = best;
    return book->sorted_entry[best_position];
}

/* ---- Training the dictionaries (step 1) -------------------------------- */

/* The mean of blocks [first, end) of `blocks` (rounded) into `mean`, and
 * their summed squared distance from it. */
static uint64_t range_error(const uint8_t *blocks, uint32_t first, uint32_t end, int values,
                            int64_t *mean)
{
    int64_t sum[VALUES4] = {0};
    uint64_t error = 0;
    uint32_t count = end - first, i;
    int k;
    for (i = first; i < end; ++i)
        for (k = 0; k < values; ++k)
            sum[k] += blocks[(size_t)i * values + k];
    for (k = 0; k < values; ++k)
        mean[k] = (sum[k] + count / 2) / count;
    for (i = first; i < end; ++i)
        for (k = 0; k < values; ++k) {
            int64_t difference = blocks[(size_t)i * values + k] - mean[k];
            error += (uint64_t)(difference * difference);
        }
    return error;
}

/*
 * Splits the cluster of blocks [first, end) in two by reordering them.
 * Returns where the second part starts, or `first` if they are all equal.
 * The first cut is at the mean of the value that varies most; then a few
 * two-means rounds move each block to the side whose mean is nearer.
 */
static uint32_t split_range(uint8_t *blocks, uint32_t first, uint32_t end, int values)
{
    int64_t mean[VALUES4], variance[VALUES4] = {0}, first_mean[VALUES4], second_mean[VALUES4];
    uint32_t i, middle = first;
    int k, widest = 0, round;

    range_error(blocks, first, end, values, mean);
    for (i = first; i < end; ++i)
        for (k = 0; k < values; ++k) {
            int64_t difference = blocks[(size_t)i * values + k] - mean[k];
            variance[k] += difference * difference;
        }
    for (k = 1; k < values; ++k)
        if (variance[k] > variance[widest])
            widest = k;
    if (!variance[widest])
        return first;

    for (i = first, middle = first; i < end; ++i)
        if (blocks[(size_t)i * values + widest] <= mean[widest])
            swap_values(blocks + (size_t)i * values, blocks + (size_t)middle++ * values, values);

    for (round = 0; round < 4; ++round) {
        /* A block is nearer the first mean when its projection on the line
         * between the means is below the midpoint's. */
        int64_t midpoint_projection = 0, direction[VALUES4];
        uint32_t next_middle;
        if (middle == first || middle == end)
            break;
        range_error(blocks, first, middle, values, first_mean);
        range_error(blocks, middle, end, values, second_mean);
        for (k = 0; k < values; ++k) {
            direction[k] = second_mean[k] - first_mean[k];
            midpoint_projection += direction[k] * (first_mean[k] + second_mean[k]);
        }
        for (i = first, next_middle = first; i < end; ++i) {
            int64_t projection = 0;
            for (k = 0; k < values; ++k)
                projection += direction[k] * blocks[(size_t)i * values + k];
            if (2 * projection <= midpoint_projection)
                swap_values(blocks + (size_t)i * values, blocks + (size_t)next_middle++ * values,
                            values);
        }
        if (next_middle == middle)
            break;
        middle = next_middle;
    }
    if (middle == first || middle == end)
        middle = first + (end - first) / 2;
    return middle;
}

/* Splits the samples into up to `wanted` clusters, always splitting the one
 * with the largest error. Returns how many there are. */
static uint32_t grow_clusters(Work *w, int values, int wanted, uint32_t sample_count)
{
    uint8_t *samples = w->samples;
    uint32_t clusters = 1;
    int64_t mean[VALUES4];

    w->leaves[0] = 0;
    w->leaves[1] = sample_count;
    w->leaves[2] = 0;
    w->leaf_error[0] = range_error(samples, 0, sample_count, values, mean);
    while (clusters < (uint32_t)wanted) {
        uint32_t worst = 0, j, middle, first, end;
        uint64_t worst_error = 0;
        for (j = 0; j < clusters; ++j)
            if (!w->leaves[3 * j + 2] && w->leaf_error[j] > worst_error) {
                worst_error = w->leaf_error[j];
                worst = j;
            }
        if (!worst_error)
            break;
        first = w->leaves[3 * worst];
        end = w->leaves[3 * worst + 1];
        middle = split_range(samples, first, end, values);
        if (middle == first) {
            w->leaves[3 * worst + 2] = 1; /* all equal: it cannot split */
            continue;
        }
        w->leaves[3 * worst + 1] = middle;
        w->leaf_error[worst] = range_error(samples, first, middle, values, mean);
        w->leaves[3 * clusters] = middle;
        w->leaves[3 * clusters + 1] = end;
        w->leaves[3 * clusters + 2] = 0;
        w->leaf_error[clusters] = range_error(samples, middle, end, values, mean);
        ++clusters;
    }
    return clusters;
}

/*
 * k-means rounds: each sample joins its nearest entry, and each entry moves
 * to the mean of its samples. An entry no sample chose takes the sample
 * farthest from its own entry instead. The book is sorted before each round
 * and after the last.
 */
static void refine_entries(Work *w, Book *book, uint32_t sample_count)
{
    uint8_t *samples = w->samples;
    int values = book->values;
    uint32_t entries = (uint32_t)book->entries, i, round;

    for (round = 0; round <= REFINING_ROUNDS; ++round) {
        uint32_t farthest = 0, farthest_error = 0;
        int c;
        sort_book(book, (uint32_t *)w->sums);
        if (round == REFINING_ROUNDS)
            break;
        for (i = 0; i < entries * (uint32_t)values; ++i)
            w->sums[i] = 0;
        for (i = 0; i < entries; ++i)
            w->counts[i] = 0;
        for (i = 0; i < sample_count; ++i) {
            uint32_t error, entry = nearest_entry(book, samples + (size_t)i * values, &error);
            w->sample_entry[i] = entry;
            ++w->counts[entry];
            for (c = 0; c < values; ++c)
                w->sums[entry * values + c] += samples[(size_t)i * values + c];
            if (error > farthest_error) {
                farthest_error = error;
                farthest = i;
            }
        }
        for (i = 0; i < entries; ++i) {
            if (w->counts[i])
                for (c = 0; c < values; ++c)
                    book->by_entry[i * values + c] =
                        (uint8_t)((w->sums[i * values + c] + w->counts[i] / 2) / w->counts[i]);
            else {
                for (c = 0; c < values; ++c)
                    book->by_entry[i * values + c] = samples[(size_t)farthest * values + c];
                farthest_error = 0;
            }
        }
    }
}

/* Trains `book` with up to `wanted` entries of `values` values on the first
 * `sample_count` samples. */
static void train_book(Work *w, Book *book, int values, int wanted, uint32_t sample_count)
{
    uint32_t clusters = grow_clusters(w, values, wanted, sample_count), i;
    int64_t mean[VALUES4];

    /* Each cluster's mean is an entry to start from. */
    book->values = values;
    book->entries = (int)clusters;
    for (i = 0; i < clusters; ++i) {
        int c;
        range_error(w->samples, w->leaves[3 * i], w->leaves[3 * i + 1], values, mean);
        for (c = 0; c < values; ++c)
            book->by_entry[i * values + c] = (uint8_t)mean[c];
    }
    refine_entries(w, book, sample_count);

    /* What the console shows: whole 5-bit values. */
    for (i = 0; i < clusters * (uint32_t)values; ++i)
        book->by_entry[i] = (uint8_t)(((book->by_entry[i] + 4) >> 3) << 3);
    sort_book(book, (uint32_t *)w->sums);
}

/* A deterministic pseudo-random index below `n`, so samples cannot alias
 * with block positions the way a fixed stride can. */
static uint64_t random_index(uint64_t *seed, uint64_t n)
{
    *seed = *seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return (*seed >> 33) % n;
}

/* Trains both books: on every block (or quarter) of a short clip, or on a
 * random sample of a long one. */
static void train_dictionaries(Work *w, const Om3dvid7EncoderSettings *settings)
{
    uint64_t blocks = (uint64_t)w->frames * BLOCK_COUNT, seed = 1, n, count;

    count = blocks < TRAINING_SAMPLES ? blocks : TRAINING_SAMPLES;
    for (n = 0; n < count; ++n) {
        uint64_t block = count == blocks ? n : random_index(&seed, blocks);
        for (int c = 0; c < VALUES4; ++c)
            w->samples[n * VALUES4 + c] = w->source[block * VALUES4 + c];
    }
    train_book(w, &w->book4, VALUES4, (int)settings->entries4, (uint32_t)count);

    count = 4 * blocks < TRAINING_SAMPLES ? 4 * blocks : TRAINING_SAMPLES;
    for (n = 0; n < count; ++n) {
        uint64_t quarter = count == 4 * blocks ? n : random_index(&seed, 4 * blocks);
        quarter_of(w->source + (quarter / 4) * VALUES4, (int)(quarter % 4),
                   w->samples + n * VALUES2);
    }
    train_book(w, &w->book2, VALUES2, (int)settings->entries2, (uint32_t)count);
}

/* ---- Nearest entries (step 2) ------------------------------------------ */

static int blocks_differ(const uint8_t *a, const uint8_t *b)
{
    int i;
    for (i = 0; i < VALUES4; ++i)
        if (a[i] != b[i])
            return 1;
    return 0;
}

/* Every block's nearest 4x4 entry, and its quarters' nearest 2x2 entries. A
 * block unchanged since the frame before copies that block's. */
static void find_nearest_entries(Work *w)
{
    uint64_t frame, block;
    for (frame = 0; frame < w->frames; ++frame)
        for (block = 0; block < BLOCK_COUNT; ++block) {
            uint64_t at = frame * BLOCK_COUNT + block;
            const uint8_t *values = w->source + at * VALUES4;
            int quarter;
            if (frame && !blocks_differ(values, values - (uint64_t)BLOCK_COUNT * VALUES4)) {
                w->nearest4[at] = w->nearest4[at - BLOCK_COUNT];
                w->nearest4_error[at] = w->nearest4_error[at - BLOCK_COUNT];
                for (quarter = 0; quarter < 4; ++quarter)
                    w->nearest2[at * 4 + quarter] = w->nearest2[(at - BLOCK_COUNT) * 4 + quarter];
                w->nearest2_error[at] = w->nearest2_error[at - BLOCK_COUNT];
                continue;
            }
            w->nearest4[at] = (uint16_t)nearest_entry(&w->book4, values, &w->nearest4_error[at]);
            w->nearest2_error[at] = 0;
            for (quarter = 0; quarter < 4; ++quarter) {
                uint8_t quarter_values[VALUES2];
                uint32_t error;
                quarter_of(values, quarter, quarter_values);
                w->nearest2[at * 4 + quarter] =
                    (uint16_t)nearest_entry(&w->book2, quarter_values, &error);
                w->nearest2_error[at] += error;
            }
        }
}

/*
 * How often each entry becomes a block's nearest one (counting a block only
 * when its nearest entry changes) decides how much of its size a first use
 * pays: entries the clip will use again and again are cheap to send.
 */
#define USES_PER_SEND 1.0f
static void price_entries(Work *w)
{
    uint64_t frame, block;
    int i, quarter;
    for (i = 0; i < MOST_ENTRIES4; ++i)
        w->send_cost4[i] = 0;
    for (i = 0; i < MOST_ENTRIES2; ++i)
        w->send_cost2[i] = 0;
    for (frame = 0; frame < w->frames; ++frame)
        for (block = 0; block < BLOCK_COUNT; ++block) {
            uint64_t at = frame * BLOCK_COUNT + block;
            if (!frame || w->nearest4[at] != w->nearest4[at - BLOCK_COUNT])
                w->send_cost4[w->nearest4[at]] += 1;
            for (quarter = 0; quarter < 4; ++quarter)
                if (!frame ||
                    w->nearest2[at * 4 + quarter] != w->nearest2[(at - BLOCK_COUNT) * 4 + quarter])
                    w->send_cost2[w->nearest2[at * 4 + quarter]] += 1;
        }
    for (i = 0; i < MOST_ENTRIES4; ++i) {
        float uses = w->send_cost4[i] * USES_PER_SEND;
        w->send_cost4[i] = 32 / (uses > 1 ? uses : 1);
    }
    for (i = 0; i < MOST_ENTRIES2; ++i) {
        float uses = w->send_cost2[i] * USES_PER_SEND;
        w->send_cost2[i] = 8 / (uses > 1 ? uses : 1);
    }
}

/* ---- Choosing (step 3) ------------------------------------------------- */

static void put_be16(uint8_t *at, uint32_t value)
{
    at[0] = (uint8_t)(value >> 8);
    at[1] = (uint8_t)value;
}

static void put_be32(uint8_t *at, uint32_t value)
{
    at[0] = (uint8_t)(value >> 24);
    at[1] = (uint8_t)(value >> 16);
    at[2] = (uint8_t)(value >> 8);
    at[3] = (uint8_t)value;
}

static void copy_bytes(void *destination, const void *source, size_t bytes)
{
    uint8_t *to = (uint8_t *)destination;
    const uint8_t *from = (const uint8_t *)source;
    while (bytes--)
        *to++ = *from++;
}

/* What block `block` of `picture` shows, as 8x-scale values. */
static void block_values(const Work *w, const Picture *picture, int block, uint8_t *out)
{
    int i, quarter;
    if (picture->kind[block] == SHOWS_BLACK) {
        for (i = 0; i < VALUES4; ++i)
            out[i] = 0;
        return;
    }
    if (picture->kind[block] == SHOWS_FOUR) {
        copy_bytes(out, w->book4.by_entry + (size_t)picture->four[block] * VALUES4, VALUES4);
        return;
    }
    for (quarter = 0; quarter < 4; ++quarter) {
        const uint8_t *entry = w->book2.by_entry + (size_t)picture->quad[block][quarter] * VALUES2;
        int quarter_row = quarter >> 1, quarter_column = quarter & 1, row, column;
        for (row = 0; row < 2; ++row)
            for (column = 0; column < 2; ++column)
                for (i = 0; i < 3; ++i)
                    out[((quarter_row * 2 + row) * 4 + quarter_column * 2 + column) * 3 + i] =
                        entry[(row * 2 + column) * 3 + i];
    }
}

/* An 8x-scale pixel as big-endian RGB555. */
static uint32_t rgb555(const uint8_t *pixel)
{
    return ((uint32_t)(pixel[0] >> 3) << 10) | ((uint32_t)(pixel[1] >> 3) << 5) | (pixel[2] >> 3);
}

/* Draws block `block` of `picture` into `frame`: RGB555 rows. */
static void paint_block(const Work *w, const Picture *picture, int block, uint8_t *frame)
{
    uint8_t values[VALUES4];
    int i, left = (block % BLOCK_COLUMNS) * 4, top = (block / BLOCK_COLUMNS) * 4;
    block_values(w, picture, block, values);
    for (i = 0; i < 16; ++i)
        put_be16(frame + ((top + i / 4) * WIDTH + left + i % 4) * 2, rgb555(values + i * 3));
}

/* Gives a 4x4 or 2x2 entry its file number if it has none yet, adding it to
 * the chunk's entries to send. Returns 1 if it is newly sent. */
static int send_entry4(Work *w, uint32_t entry)
{
    if (w->file_number4[entry] != UNSENT)
        return 0;
    w->file_number4[entry] = (uint16_t)w->picture.next4++;
    w->sent4[w->sent4_count++] = (uint16_t)entry;
    return 1;
}

static int send_entry2(Work *w, uint32_t entry)
{
    if (w->file_number2[entry] != UNSENT)
        return 0;
    w->file_number2[entry] = (uint16_t)w->picture.next2++;
    w->sent2[w->sent2_count++] = (uint16_t)entry;
    return 1;
}

/* The bytes a block's four 2x2 entries would cost to send, counting each
 * unsent entry once. */
static float quad_send_cost(const Work *w, const uint16_t *quad)
{
    float cost = 0;
    int quarter;
    for (quarter = 0; quarter < 4; ++quarter) {
        int k, repeated = 0;
        for (k = 0; k < quarter; ++k)
            if (quad[k] == quad[quarter])
                repeated = 1;
        if (!repeated && w->file_number2[quad[quarter]] == UNSENT)
            cost += w->send_cost2[quad[quarter]];
    }
    return cost;
}

/* Whether `picture` already shows these four 2x2 entries in `block`. */
static int shows_quad(const Picture *picture, int block, const uint16_t *quad)
{
    return picture->kind[block] == SHOWS_QUAD && picture->quad[block][0] == quad[0] &&
           picture->quad[block][1] == quad[1] && picture->quad[block][2] == quad[2] &&
           picture->quad[block][3] == quad[3];
}

/*
 * Chooses every block of frame `frame` (into w->choices) at the given prices
 * of a byte (lambda) and of a decode clock (mu), updating the picture and
 * sending the entries the choices use. Returns the frame's estimated clocks.
 */
static uint32_t choose_blocks(Work *w, uint32_t frame, double lambda, double mu)
{
    const uint8_t *source = w->source + (uint64_t)frame * BLOCK_COUNT * VALUES4;
    uint64_t at = (uint64_t)frame * BLOCK_COUNT;
    uint32_t clocks = CLOCKS_FRAME;
    int block, last_choice = -1;

    for (block = 0; block < BLOCK_COUNT; ++block) {
        uint8_t shown[VALUES4];
        uint32_t keep_error, entry4 = w->nearest4[at + block];
        const uint16_t *quad = w->nearest2 + (at + block) * 4;
        double best, cost, weight = w->visibility[at + block] / 255.0;
        int choice = CHOOSE_KEEP, quarter;

        /* Keeping costs only its error. */
        block_values(w, &w->picture, block, shown);
        keep_error = distance(source + (size_t)block * VALUES4, shown, VALUES4, 0xffffffffu);
        best = weight * keep_error;

        /* A 4x4 entry: one word, plus sending it the first time. */
        if (!(w->picture.kind[block] == SHOWS_FOUR && w->picture.four[block] == entry4)) {
            cost = weight * w->nearest4_error[at + block] +
                   lambda * (2 + (w->file_number4[entry4] == UNSENT ? w->send_cost4[entry4] : 0)) +
                   mu * CLOCKS_FOUR;
            if (cost < best) {
                best = cost;
                choice = CHOOSE_FOUR;
            }
        }
        /* Four 2x2 entries: three words, plus sending them. */
        if (!shows_quad(&w->picture, block, quad)) {
            float send = quad_send_cost(w, quad);
            cost = weight * w->nearest2_error[at + block] + lambda * (6 + send) + mu * CLOCKS_QUAD;
            if (cost < best) {
                best = cost;
                choice = CHOOSE_QUAD;
            }
        }

        w->choices[block] = (uint8_t)choice;
        if (choice == CHOOSE_FOUR) {
            w->picture.kind[block] = SHOWS_FOUR;
            w->picture.four[block] = (uint16_t)entry4;
            send_entry4(w, entry4);
            clocks += CLOCKS_FOUR;
        } else if (choice == CHOOSE_QUAD) {
            w->picture.kind[block] = SHOWS_QUAD;
            for (quarter = 0; quarter < 4; ++quarter) {
                w->picture.quad[block][quarter] = quad[quarter];
                send_entry2(w, quad[quarter]);
            }
            clocks += CLOCKS_QUAD;
        }
        if (choice != last_choice)
            clocks += choice == CHOOSE_FOUR   ? CLOCKS_FOUR_RUN
                      : choice == CHOOSE_QUAD ? CLOCKS_QUAD_RUN
                                              : CLOCKS_KEEP_RUN;
        last_choice = choice;
    }
    return clocks;
}

/* The length of the run of equal choices starting at `block` (at most 8192,
 * the most one packet word can count). */
static int run_length(const Work *w, int block)
{
    int choice = w->choices[block], length = 1;
    while (block + length < BLOCK_COUNT && w->choices[block + length] == choice && length < 8192)
        ++length;
    return length;
}

/* The frame's modelled decode clocks, exactly as write_packet will code it. */
static uint32_t frame_clocks(const Work *w)
{
    uint32_t clocks = CLOCKS_FRAME;
    int block = 0;
    while (block < BLOCK_COUNT) {
        int choice = w->choices[block], length = run_length(w, block);
        if (choice == CHOOSE_KEEP)
            clocks += CLOCKS_KEEP_RUN;
        else if (choice == CHOOSE_FOUR)
            clocks += length == 1 ? CLOCKS_ONE_FOUR : CLOCKS_FOUR_RUN + length * CLOCKS_FOUR;
        else
            clocks += CLOCKS_QUAD_RUN + length * CLOCKS_QUAD;
        block += length;
    }
    return clocks;
}

/* Writes the frame's packet (see FORMAT.md) and returns its length. */
static uint32_t write_packet(const Work *w, uint8_t *out)
{
    uint8_t *at = out;
    int block = 0;
    while (block < BLOCK_COUNT) {
        int choice = w->choices[block], length = run_length(w, block), i;
        if (choice == CHOOSE_KEEP) {
            put_be16(at, 0x2000 | (length - 1));
            at += 2;
        } else if (choice == CHOOSE_FOUR) {
            /* A lone entry is its own word; a run has a count first. */
            if (length > 1) {
                put_be16(at, 0x4000 | (length - 1));
                at += 2;
            }
            for (i = 0; i < length; ++i) {
                put_be16(at, w->file_number4[w->picture.four[block + i]]);
                at += 2;
            }
        } else {
            put_be16(at, 0x6000 | (length - 1));
            at += 2;
            for (i = 0; i < length; ++i) {
                /* Four 12-bit numbers in three words. */
                const uint16_t *quad = w->picture.quad[block + i];
                uint32_t upper =
                             ((uint32_t)w->file_number2[quad[0]] << 12) | w->file_number2[quad[1]],
                         lower =
                             ((uint32_t)w->file_number2[quad[2]] << 12) | w->file_number2[quad[3]];
                at[0] = (uint8_t)(upper >> 16);
                at[1] = (uint8_t)(upper >> 8);
                at[2] = (uint8_t)upper;
                at[3] = (uint8_t)(lower >> 16);
                at[4] = (uint8_t)(lower >> 8);
                at[5] = (uint8_t)lower;
                at += 6;
            }
        }
        block += length;
    }
    return (uint32_t)(at - out);
}

/* ---- Chunks ------------------------------------------------------------ */

/* Sound samples from the start of the clip to frame `frame`. */
static uint64_t samples_before(uint64_t frame)
{
    return frame * OM3DVID7_AUDIO_RATE / OM3DVID7_FPS;
}

/* One pass over the clip at a given lambda: measuring, or writing (and
 * checking) the file. */
typedef struct Pass {
    const Om3dvid7EncoderSettings *settings;
    const int8_t *audio; /* NULL: silent */
    uint8_t *out;        /* NULL: only measure */
    uint64_t capacity, used;
    int verify;      /* decode what is written and compare */
    int out_of_room; /* `out` was too small */
    Om3dvid7Decoder decoder;
    Om3dvid7Header header;
    uint8_t table[2 * OM3DVID7_MAX_CHUNKS]; /* the header block's chunk table */
} Pass;

/* A chunk's frames: where they start and how many. */
static uint32_t chunk_first_frame(const Pass *pass, uint32_t chunk)
{
    return chunk * pass->settings->chunk_frames;
}

static uint32_t chunk_frame_count(const Work *w, const Pass *pass, uint32_t chunk)
{
    uint32_t first = chunk_first_frame(pass, chunk);
    return w->frames - first < pass->settings->chunk_frames ? w->frames - first
                                                            : pass->settings->chunk_frames;
}

/* The clip's chunk count: frames / frames per chunk, rounded up. */
static uint32_t chunk_count(const Work *w, const Om3dvid7EncoderSettings *settings)
{
    return (w->frames + settings->chunk_frames - 1) / settings->chunk_frames;
}

/* A chunk's size before padding: header, sound padded to a word, new
 * entries, frame table and packets. */
static uint32_t chunk_content_bytes(uint32_t audio, uint32_t sent4, uint32_t sent2, uint32_t frames,
                                    uint32_t packets)
{
    return OM3DVID7_CHUNK_HEADER + ((audio + 3) & ~3u) + sent4 * OM3DVID7_ENTRY4 +
           sent2 * OM3DVID7_ENTRY2 + (frames + 1) * 4 + packets;
}

/* Rounded up to whole 2048-byte blocks. */
static uint32_t padded_to_blocks(uint32_t bytes)
{
    return (bytes + OM3DVID7_BLOCK_BYTES - 1) / OM3DVID7_BLOCK_BYTES * OM3DVID7_BLOCK_BYTES;
}

/* Takes back the file numbers given since the chunk sent `keep4` and `keep2`
 * entries. */
static void unsend_since(Work *w, uint32_t keep4, uint32_t keep2)
{
    while (w->sent4_count > keep4)
        w->file_number4[w->sent4[--w->sent4_count]] = UNSENT;
    while (w->sent2_count > keep2)
        w->file_number2[w->sent2[--w->sent2_count]] = UNSENT;
}

/*
 * Chooses frame `frame` at `lambda`. If it would take the console longer than
 * the settings allow to decode, the frame is chosen again with decode clocks
 * priced higher (mu), until it fits. mu carries over to the next frame, but
 * eases off, and stops once it is negligible next to lambda. Returns the
 * frame's clocks.
 */
static uint32_t choose_frame(Work *w, const Pass *pass, uint32_t frame, double lambda)
{
    uint32_t keep4, keep2, clocks;
    double mu = w->clock_price < lambda / 4096 ? 0 : w->clock_price * 0.6;

    copy_bytes(&w->at_frame_start, &w->picture, sizeof(Picture));
    keep4 = w->sent4_count;
    keep2 = w->sent2_count;
    for (;;) {
        choose_blocks(w, frame, lambda, mu);
        clocks = frame_clocks(w);
        if (clocks <= pass->settings->cycle_cap || mu > 1e12)
            break;
        /* Undo the frame and price decode clocks higher. */
        unsend_since(w, keep4, keep2);
        copy_bytes(&w->picture, &w->at_frame_start, sizeof(Picture));
        mu = mu > 0 ? mu * 1.6 : lambda / 16;
    }
    w->clock_price = mu > 1e12 ? 0 : mu;
    if (clocks > w->slowest_frame_clocks)
        w->slowest_frame_clocks = clocks;
    return clocks;
}

/* The encoder's model of frame `index` of the chunk, from the one before it
 * and the blocks this frame changed. */
static void model_frame(Work *w, uint32_t index)
{
    uint8_t *before = w->expected + (size_t)index * OM3DVID7_FRAME_BYTES,
            *after = before + OM3DVID7_FRAME_BYTES;
    int block;
    copy_bytes(after, before, OM3DVID7_FRAME_BYTES);
    for (block = 0; block < BLOCK_COUNT; ++block)
        if (w->choices[block] != CHOOSE_KEEP)
            paint_block(w, &w->picture, block, after);
}

/* Writes an entry as the file stores it, from its 8x-scale values. */
static void write_entry4(const uint8_t *values, uint8_t *out)
{
    uint8_t rows[32];
    int i;
    for (i = 0; i < 16; ++i)
        put_be16(rows + i * 2, rgb555(values + i * 3));
    om3dvid7_entry4(rows, out);
}

static void write_entry2(const uint8_t *values, uint8_t *out)
{
    uint8_t rows[8];
    int i;
    for (i = 0; i < 4; ++i)
        put_be16(rows + i * 2, rgb555(values + i * 3));
    om3dvid7_entry2(rows, out);
}

/* Lays out chunk `chunk` (`size` bytes, zeroed first) at `out`: header,
 * sound, new entries, frame table and the packets in w->packets. */
static void write_chunk(const Work *w, const Pass *pass, uint32_t chunk, uint8_t *out,
                        uint32_t size, uint32_t audio, const uint32_t *offsets, uint32_t packets)
{
    uint32_t first = chunk_first_frame(pass, chunk), frames = chunk_frame_count(w, pass, chunk);
    uint32_t at, i;

    for (i = 0; i < size; ++i)
        out[i] = 0;
    put_be16(out + 4, chunk);
    put_be16(out + 6, frames);
    put_be16(out + 8, w->at_chunk_start.next4);
    put_be16(out + 10, w->sent4_count);
    put_be16(out + 12, w->at_chunk_start.next2);
    put_be16(out + 14, w->sent2_count);
    put_be32(out + 16, audio);
    at = OM3DVID7_CHUNK_HEADER;
    for (i = 0; i < audio; ++i)
        out[at + i] = (uint8_t)pass->audio[samples_before(first) + i];
    at += (audio + 3) & ~3u;
    for (i = 0; i < w->sent4_count; ++i, at += OM3DVID7_ENTRY4)
        write_entry4(w->book4.by_entry + (size_t)w->sent4[i] * VALUES4, out + at);
    for (i = 0; i < w->sent2_count; ++i, at += OM3DVID7_ENTRY2)
        write_entry2(w->book2.by_entry + (size_t)w->sent2[i] * VALUES2, out + at);

    put_be32(out + 20, (frames + 1) * 4 + packets);
    for (i = 0; i <= frames; ++i)
        put_be32(out + at + i * 4, (frames + 1) * 4 + offsets[i]);
    copy_bytes(out + at + (frames + 1) * 4, w->packets, packets);
    put_be32(out, (uint32_t)om3dvid7_sum(out + 4, size - 4));
}

/* Decodes the chunk just written with the menu's decoder; every frame must
 * match the encoder's model. Returns 0, or -1 if one does not. */
static int verify_chunk(Work *w, Pass *pass, uint32_t chunk, const uint8_t *data, uint32_t size)
{
    uint32_t frames = chunk_frame_count(w, pass, chunk), index;
    Om3dvid7Chunk info;

    if (om3dvid7_chunk(&pass->header, chunk, data, size, &info))
        return -1;
    om3dvid7_define(&pass->decoder, &info);
    for (index = 0; index < frames; ++index) {
        const uint8_t *wanted = w->expected + (size_t)(index + 1) * OM3DVID7_FRAME_BYTES;
        uint32_t i;
        if (om3dvid7_frame(&pass->decoder, &info, index, OM3DVID7_SCREEN))
            return -1;
        /* The screen holds two lines per word, the upper in the high half. */
        for (i = 0; i < OM3DVID7_FRAME_BYTES; i += 2) {
            uint32_t x = (i / 2) % WIDTH, y = (i / 2) / WIDTH,
                     at = ((y >> 1) * WIDTH + x) * 4 + (y & 1) * 2;
            if (w->check_screen[at] != wanted[i] || w->check_screen[at + 1] != wanted[i + 1])
                return -1;
        }
    }
    /* The chunk's last frame is where the next chunk's model starts. */
    copy_bytes(w->expected, w->expected + (size_t)frames * OM3DVID7_FRAME_BYTES,
               OM3DVID7_FRAME_BYTES);
    return 0;
}

static uint32_t encode_chunk(Work *w, Pass *pass, uint32_t chunk, double lambda);

/* Keeps the chunk being encoded while the next one is tried, and puts it
 * back afterwards (with the entries it had sent). */
static void keep_chunk_state(Work *w)
{
    copy_bytes(w->kept_packets, w->packets, PACKETS_ROOM);
    copy_bytes(&w->kept_picture, &w->picture, sizeof(Picture));
    copy_bytes(&w->kept_chunk_start, &w->at_chunk_start, sizeof(Picture));
    copy_bytes(w->kept_number4, w->file_number4, MOST_ENTRIES4 * 2);
    copy_bytes(w->kept_number2, w->file_number2, MOST_ENTRIES2 * 2);
    copy_bytes(w->kept_sent4, w->sent4, w->sent4_count * 2);
    copy_bytes(w->kept_sent2, w->sent2, w->sent2_count * 2);
}

static void restore_chunk_state(Work *w, uint32_t sent4_count, uint32_t sent2_count)
{
    copy_bytes(w->packets, w->kept_packets, PACKETS_ROOM);
    copy_bytes(&w->picture, &w->kept_picture, sizeof(Picture));
    copy_bytes(&w->at_chunk_start, &w->kept_chunk_start, sizeof(Picture));
    copy_bytes(w->file_number4, w->kept_number4, MOST_ENTRIES4 * 2);
    copy_bytes(w->file_number2, w->kept_number2, MOST_ENTRIES2 * 2);
    copy_bytes(w->sent4, w->kept_sent4, sent4_count * 2);
    copy_bytes(w->sent2, w->kept_sent2, sent2_count * 2);
    w->sent4_count = sent4_count;
    w->sent2_count = sent2_count;
}

/*
 * Before chunk `chunk` (whose content is `content` bytes) is written, tries
 * the next one. If that had to give up quality to fit, the entries it needs
 * are sent here instead, in the order it first used them, while this chunk
 * has room. Scene changes, the opening after a fade-in above all, need far
 * more new entries than one chunk can carry, and a decoder only needs an
 * entry before its first use.
 */
static void pre_send(Work *w, Pass *pass, uint32_t chunk, double lambda, uint32_t content)
{
    uint32_t chunks = chunk_count(w, pass->settings), sent4_count = w->sent4_count,
             sent2_count = w->sent2_count, wanted4_count, wanted2_count, i, j, room;
    double clock_price = w->clock_price;
    uint32_t slowest_frame_clocks = w->slowest_frame_clocks;
    Pass trial;

    if (chunk + 1 >= chunks || content + OM3DVID7_ENTRY2 > pass->settings->max_chunk)
        return;
    room = pass->settings->max_chunk - content;

    /* Try the next chunk, only measuring, and note the entries it sends. */
    keep_chunk_state(w);
    copy_bytes(&trial, pass, sizeof(Pass));
    trial.out = NULL;
    trial.verify = 0;
    w->trying_next_chunk = 1;
    encode_chunk(w, &trial, chunk + 1, lambda);
    w->trying_next_chunk = 0;
    wanted4_count = w->sent4_count;
    wanted2_count = w->sent2_count;
    copy_bytes(w->wanted4, w->sent4, wanted4_count * 2);
    copy_bytes(w->wanted2, w->sent2, wanted2_count * 2);
    restore_chunk_state(w, sent4_count, sent2_count);
    w->clock_price = clock_price;
    w->slowest_frame_clocks = slowest_frame_clocks;
    if (!w->lambda_raises)
        return; /* it fitted at full quality: nothing to help with */

    /* Send its entries, the two kinds interleaved by how far through their
     * lists they are, while they fit. */
    for (i = 0, j = 0; i < wanted4_count || j < wanted2_count;) {
        int four = j >= wanted2_count || (i < wanted4_count && (uint64_t)i * wanted2_count <=
                                                                   (uint64_t)j * wanted4_count);
        uint32_t bytes = four ? OM3DVID7_ENTRY4 : OM3DVID7_ENTRY2;
        if (bytes > room)
            break;
        if (four ? send_entry4(w, w->wanted4[i++]) : send_entry2(w, w->wanted2[j++]))
            room -= bytes;
    }
}

/*
 * Encodes chunk `chunk` at `lambda`, raising lambda (by 1.4 each time, up to
 * 40 times) until it fits the largest chunk size, then pre-sends for the next
 * chunk and, when the pass writes, writes and checks it. Returns its padded
 * size, or 0 if it cannot fit, the output is full, or the check fails.
 */
static uint32_t encode_chunk(Work *w, Pass *pass, uint32_t chunk, double lambda)
{
    const Om3dvid7EncoderSettings *settings = pass->settings;
    uint32_t first = chunk_first_frame(pass, chunk), frames = chunk_frame_count(w, pass, chunk);
    uint32_t audio =
        pass->audio ? (uint32_t)(samples_before(first + frames) - samples_before(first)) : 0;
    uint32_t raises, index;

    for (raises = 0; raises < 40; ++raises, lambda *= 1.4) {
        uint32_t packets = 0, size, offsets[OM3DVID7_MAX_CHUNK_FRAMES + 1];

        copy_bytes(&w->at_chunk_start, &w->picture, sizeof(Picture));
        w->sent4_count = w->sent2_count = 0;
        for (index = 0; index < frames; ++index) {
            choose_frame(w, pass, first + index, lambda);
            offsets[index] = packets;
            packets += write_packet(w, w->packets + packets);
            if (pass->verify)
                model_frame(w, index);
        }
        offsets[frames] = packets;
        size = padded_to_blocks(
            chunk_content_bytes(audio, w->sent4_count, w->sent2_count, frames, packets));
        if (size > settings->max_chunk) {
            /* Too big: forget this chunk's entries and choices, raise lambda. */
            unsend_since(w, 0, 0);
            copy_bytes(&w->picture, &w->at_chunk_start, sizeof(Picture));
            continue;
        }
        w->lambda_raises = raises;
        if (!w->trying_next_chunk) {
            pre_send(w, pass, chunk, lambda,
                     chunk_content_bytes(audio, w->sent4_count, w->sent2_count, frames, packets));
            size = padded_to_blocks(
                chunk_content_bytes(audio, w->sent4_count, w->sent2_count, frames, packets));
        }
        if (pass->out) {
            uint8_t *out;
            if (pass->capacity - pass->used < size) {
                pass->out_of_room = 1;
                return 0;
            }
            out = pass->out + pass->used;
            write_chunk(w, pass, chunk, out, size, audio, offsets, packets);
            put_be16(pass->table + 2 * chunk, size / OM3DVID7_BLOCK_BYTES);
            if (pass->verify && verify_chunk(w, pass, chunk, out, size))
                return 0;
            pass->used += size;
        }
        return size;
    }
    w->lambda_raises = raises;
    return 0;
}

/* Encodes the whole clip at `lambda`, filling w->chunk_bytes. Returns the
 * file's size, or 0 if a chunk failed. */
static uint64_t encode_pass(Work *w, Pass *pass, double lambda)
{
    uint32_t chunks = chunk_count(w, pass->settings), chunk, i;
    uint64_t total = OM3DVID7_BLOCK_BYTES;

    /* Every pass starts from black, with nothing sent. */
    for (i = 0; i < BLOCK_COUNT; ++i)
        w->picture.kind[i] = SHOWS_BLACK;
    w->picture.next4 = w->picture.next2 = 0;
    for (i = 0; i < MOST_ENTRIES4; ++i)
        w->file_number4[i] = UNSENT;
    for (i = 0; i < MOST_ENTRIES2; ++i)
        w->file_number2[i] = UNSENT;
    w->clock_price = 0;
    w->slowest_frame_clocks = 0;

    for (chunk = 0; chunk < chunks; ++chunk) {
        uint32_t size = encode_chunk(w, pass, chunk, lambda);
        if (!size)
            return 0;
        w->chunk_bytes[chunk] = size;
        total += size;
    }
    return total;
}

/* ---- The reader model (step 4) -------------------------------------------- */

/*
 * Whether a player keeps up with the clip. The player starts, and starts each
 * loop again, only once every slot holds a chunk; it reads the next loop only
 * after this one has played, while the picture holds and the reader has the
 * console to itself (at HOLD_READ_RATE). Between those points each chunk
 * must arrive before it plays, from a reader of read_rate bytes a second
 * sharing the console with playback. A slot frees when its chunk has
 * finished playing. Two loops are checked.
 *
 * Returns the chunks read before playing (the slots, or every chunk of a
 * short clip), or 0 if the reader falls behind mid-loop or a loop would hold
 * for more than LOOP_WAIT seconds.
 */
#define LOOP_WAIT      2.0
#define HOLD_READ_RATE (400.0 * 1024)
static uint32_t reader_preroll(const Work *w, const Om3dvid7EncoderSettings *settings,
                               uint32_t chunks, uint32_t slots)
{
    const double chunk_seconds = (double)settings->chunk_frames / OM3DVID7_FPS, margin = 0.1;
    double *read_done = w->read_done, *play_at = w->play_at, reader = 0;
    uint32_t k, j, fill = slots < chunks ? slots : chunks;

    for (k = 0; k < 2 * chunks; ++k) {
        if (k % chunks == 0) {
            /* A loop starts: the reader fills every slot, then play starts. */
            double natural = k ? play_at[k - 1] + chunk_seconds : 0;
            if (natural > reader)
                reader = natural;
            for (j = k; j < k + fill; ++j) {
                if (j >= slots && play_at[j - slots] + chunk_seconds > reader)
                    reader = play_at[j - slots] + chunk_seconds; /* wait for a free slot */
                reader += (double)w->chunk_bytes[j % chunks] / HOLD_READ_RATE;
                read_done[j] = reader;
            }
            play_at[k] = read_done[k + fill - 1] > natural ? read_done[k + fill - 1] : natural;
            if (k && play_at[k] - natural > LOOP_WAIT)
                return 0;
        } else {
            /* Mid-loop: read while playing, each chunk before it plays. */
            play_at[k] = play_at[k - 1] + chunk_seconds;
            if (k % chunks >= fill) {
                if (k >= slots && play_at[k - slots] + chunk_seconds > reader)
                    reader = play_at[k - slots] + chunk_seconds;
                reader += (double)w->chunk_bytes[k % chunks] / settings->read_rate;
                read_done[k] = reader;
            }
            if (read_done[k] > play_at[k] - margin)
                return 0;
        }
    }
    return fill;
}

/* ---- Finishing (step 5) -------------------------------------------------- */

/* Writes the header block (see FORMAT.md) at `out`. */
static void write_header_block(uint8_t *out, const Om3dvid7EncoderSettings *settings, const Work *w,
                               uint32_t chunks, uint32_t preroll, uint32_t slots,
                               uint32_t audio_rate, uint32_t largest_chunk, const uint8_t *table)
{
    uint32_t i, a = 0, b = 0;
    for (i = 0; i < OM3DVID7_BLOCK_BYTES; ++i)
        out[i] = 0;
    copy_bytes(out, "OM3DVID7", 8);
    put_be16(out + 8, WIDTH);
    put_be16(out + 10, HEIGHT);
    put_be16(out + 12, OM3DVID7_FPS);
    put_be16(out + 14, settings->chunk_frames);
    put_be32(out + 16, w->frames);
    put_be16(out + 20, chunks);
    put_be16(out + 22, preroll);
    put_be16(out + 24, w->picture.next4);
    put_be16(out + 26, w->picture.next2);
    put_be32(out + 28, audio_rate);
    put_be32(out + 32, largest_chunk);
    put_be32(out + 36, settings->rate);
    put_be16(out + 44, slots);
    copy_bytes(out + OM3DVID7_HEADER_BYTES, table, 2 * chunks);
    /* The word checksum, with its own field still zero. */
    for (i = 0; i < OM3DVID7_BLOCK_BYTES; i += 4) {
        a += ((uint32_t)out[i] << 24) | ((uint32_t)out[i + 1] << 16) | ((uint32_t)out[i + 2] << 8) |
             out[i + 3];
        b += a;
    }
    put_be32(out + 40, a ^ ((b << 16) | (b >> 16)));
}

/* Whether the settings and arguments can make a clip of w->frames frames. */
static int arguments_valid(const Work *w, const Om3dvid7EncoderSettings *settings,
                           const int8_t *audio, uint64_t audio_samples, const uint8_t *out)
{
    return settings && settings->chunk_frames &&
           settings->chunk_frames <= OM3DVID7_MAX_CHUNK_FRAMES && settings->rate &&
           settings->read_rate && settings->entries4 && settings->entries4 <= MOST_ENTRIES4 &&
           settings->entries2 && settings->entries2 <= MOST_ENTRIES2 && settings->max_chunk &&
           !(settings->max_chunk % OM3DVID7_BLOCK_BYTES) &&
           settings->max_chunk <= 65535UL * OM3DVID7_BLOCK_BYTES && settings->cycle_cap &&
           !(audio && audio_samples != samples_before(w->frames)) && out;
}

/*
 * The smallest lambda (the best picture) whose file meets both the rate and
 * the reader model: a binary search between 1 and 10 million, in ratios.
 * Returns it, or 0 if even the largest does not.
 */
static double search_lambda(Work *w, Pass *pass, uint32_t chunks, uint32_t slots)
{
    const Om3dvid7EncoderSettings *settings = pass->settings;
    uint64_t target = (uint64_t)settings->rate * w->frames / OM3DVID7_FPS, total;
    double low = 1, high = 1e7;
    int i;

    total = encode_pass(w, pass, high);
    if (!total || total > target || !reader_preroll(w, settings, chunks, slots))
        return 0;
    for (i = 0; i < 22 && high / low > 1.02; ++i) {
        double middle = square_root(low * high);
        total = encode_pass(w, pass, middle);
        if (total && total <= target && reader_preroll(w, settings, chunks, slots))
            high = middle;
        else
            low = middle;
    }
    return high;
}

/* Sets up the final pass to decode what it writes with the menu's decoder,
 * in the workspace's own buffers. */
static void prepare_check(Work *w, Pass *pass, uint32_t chunks, uint32_t slots, int has_sound)
{
    uint8_t *at = w->check_state;
    uint32_t i;

    pass->decoder.map = (unsigned long *)(void *)at;
    at += OM3DVID7_BLOCKS * 2 * sizeof(unsigned long);
    pass->decoder.dict4 = at;
    at += 8192 * 32;
    pass->decoder.thumb4 = at;
    at += 8192 * 8;
    pass->decoder.dict2 = at;
    at += 4096 * 8;
    pass->decoder.thumb2 = (unsigned long *)(void *)at;
    pass->decoder.entries4 = 8192;
    pass->decoder.entries2 = 4096;
    pass->decoder.thumb = w->check_thumb;
    pass->decoder.screen = w->check_screen;
    for (i = 0; i < OM3DVID7_FRAME_BYTES; ++i)
        w->check_screen[i] = 0;
    om3dvid7_reset(&pass->decoder);

    /* What om3dvid7_chunk needs of the header. */
    pass->header.chunk_frames = pass->settings->chunk_frames;
    pass->header.chunks = chunks;
    pass->header.frames = w->frames;
    pass->header.entries4 = 8192;
    pass->header.entries2 = 4096;
    pass->header.audio_rate = has_sound ? OM3DVID7_AUDIO_RATE : 0;
    pass->header.max_chunk = pass->settings->max_chunk;
    pass->header.table = pass->table;
    pass->header.preroll = 1;
    pass->header.slots = slots;

    /* The model starts from black too. */
    for (i = 0; i < OM3DVID7_FRAME_BYTES; ++i)
        w->expected[i] = 0;
}

OM3DVID7_EXPORT int64_t om3dvid7_encoder_finish(void *workspace,
                                                const Om3dvid7EncoderSettings *settings,
                                                const int8_t *audio, uint64_t audio_samples,
                                                uint8_t *out, uint64_t capacity, uint32_t *stats)
{
    Work *w = work_in(workspace);
    Pass pass;
    uint32_t chunks, slots, preroll, chunk, largest_chunk = 0;
    uint64_t total;
    double lambda;
    int i;

    if (!w->frames_added)
        return -1;
    w->frames = w->frames_added; /* the buffers were laid out for at least this many */
    if (!arguments_valid(w, settings, audio, audio_samples, out))
        return -1;
    chunks = chunk_count(w, settings);
    if (chunks > OM3DVID7_MAX_CHUNKS)
        return -1;
    slots = settings->slot_bytes / settings->max_chunk;
    if (slots < 1)
        return -1;
    if (slots > chunks)
        slots = chunks;

    train_dictionaries(w, settings);
    find_nearest_entries(w);
    price_entries(w);

    for (i = 0; i < (int)sizeof(pass); ++i)
        ((uint8_t *)&pass)[i] = 0;
    pass.settings = settings;
    pass.audio = audio;
    lambda = search_lambda(w, &pass, chunks, slots);
    if (!lambda)
        return -4;

    /* The final pass: write, decode and compare. */
    prepare_check(w, &pass, chunks, slots, audio != NULL);
    if (capacity < OM3DVID7_BLOCK_BYTES)
        return -2;
    pass.out = out;
    pass.capacity = capacity;
    pass.used = OM3DVID7_BLOCK_BYTES;
    pass.verify = 1;
    total = encode_pass(w, &pass, lambda);
    if (!total)
        return pass.out_of_room ? -2 : -3;
    for (chunk = 0; chunk < chunks; ++chunk)
        if (w->chunk_bytes[chunk] > largest_chunk)
            largest_chunk = w->chunk_bytes[chunk];
    preroll = reader_preroll(w, settings, chunks, slots);
    if (!preroll)
        return -4;
    write_header_block(out, settings, w, chunks, preroll, slots, audio ? OM3DVID7_AUDIO_RATE : 0,
                       largest_chunk, pass.table);
    {
        Om3dvid7Header written;
        if (om3dvid7_header(out, &written))
            return -3;
    }

    if (stats) {
        stats[OM3DVID7_STAT_LAMBDA_X1000] = (uint32_t)(lambda * 1000);
        stats[OM3DVID7_STAT_SLOWEST_FRAME] = w->slowest_frame_clocks;
        stats[OM3DVID7_STAT_ENTRIES4] = w->picture.next4;
        stats[OM3DVID7_STAT_ENTRIES2] = w->picture.next2;
        stats[OM3DVID7_STAT_PREROLL] = preroll;
        stats[OM3DVID7_STAT_SLOTS] = slots;
        stats[OM3DVID7_STAT_LARGEST_CHUNK] = largest_chunk;
        stats[OM3DVID7_STAT_CHUNKS] = chunks;
    }
    return (int64_t)pass.used;
}

/* For tuning the dictionaries: the mean error per block of the nearest 4x4
 * entry (out[0]) and of the nearest four 2x2 entries (out[1]) in the last
 * encode (8x-scale values, squared). */
OM3DVID7_EXPORT void om3dvid7_encoder_debug(void *workspace, double *out)
{
    Work *w = work_in(workspace);
    uint64_t i, blocks = (uint64_t)w->frames * BLOCK_COUNT;
    double four = 0, quad = 0;
    for (i = 0; i < blocks; ++i) {
        four += w->nearest4_error[i];
        quad += w->nearest2_error[i];
    }
    out[0] = four / blocks;
    out[1] = quad / blocks;
}
