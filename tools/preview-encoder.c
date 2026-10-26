/*
 * The encoder for OM3DVID5 menu previews: 160x120 at 30 fps, loaded whole
 * into the console's memory (see src/preview_v5.h for the bitstream).
 * Original MIT-licensed code, C89.
 *
 * It allocates nothing and uses no C runtime, so the Windows DLL has no
 * imports. prepare-preview.py loads it with ctypes and the desktop app links
 * it in; both search lambda themselves (see oh3do_preview_encode).
 *
 * Each 4x4 block of a frame takes whichever mode costs least in
 * error + lambda * bits + mu * decode clocks, judged against the frame
 * exactly as decoded so far (the decoder works in place):
 *   SKIP    leave the block as it is
 *   MOTION  copy a 4x4 area from up to 8 pixels away
 *   SOLID   one color
 *   GRAD    two endpoint colors and two mixes of them, a 2-bit index a pixel
 * Colors are sent through a 128-entry cache that the decoder keeps too: a
 * color in the cache costs one byte, a new one two. mu stays zero unless a
 * frame would take the console longer than CYCLE_CAP to decode; then it
 * rises until the frame fits. Each finished packet is decoded again by an
 * independent reference decoder, and the result must equal the encoder's
 * model of the frame.
 *
 * The working state is static. The desktop app encodes several clips at once
 * and defines OH3DO_THREAD_LOCAL so each thread gets its own copy.
 */

/* OH3DO_PREVIEW_DLL: the standalone CRT-free Windows DLL. Without it the file
 * is an ordinary source, also compiled into the desktop app. */
#if defined(_WIN32) && defined(OH3DO_PREVIEW_DLL)
#define EXPORT __declspec(dllexport)
#elif defined(__GNUC__)
#define EXPORT __attribute__((visibility("default")))
#else
#define EXPORT
#endif

#ifdef OH3DO_THREAD_LOCAL
#define STATE static OH3DO_THREAD_LOCAL
#else
#define STATE static
#endif

#define WIDTH              160
#define HEIGHT             120
#define PIXELS             (WIDTH * HEIGHT)
#define FRAME_BYTES        (PIXELS * 2)
#define BLOCK_COLUMNS      (WIDTH / 4)
#define BLOCK_COUNT        (BLOCK_COLUMNS * (HEIGHT / 4))
#define CACHE_SIZE         128 /* colors the color cache holds */
#define MOTION_RANGE       8   /* motion vectors reach -8..7 pixels each way */
#define NEARBY_COLORS      6   /* cached colors tried near a block's own */
#define MAX_FRAMES         450
#define LARGEST_PACKET     (BLOCK_COUNT * 12)
#define LONGEST_RUN        64 /* blocks one op can cover */
#define LONGEST_MOTION_RUN 32 /* for MOTION, so a run's sources never overlap */

/*
 * The decode budget of a frame in ARM60 clocks (12.5 MHz, page-mode DRAM),
 * from a cycle count of src/preview_v5_arm.s. The console measured 1.28 times
 * the counted time, so this is about 20.5 ms of a 33.3 ms frame, leaving the
 * rest for the cel engine to draw it. Lower caps cost visible detail in busy
 * frames; uncapped, the busiest frames would take most of a frame.
 */
#define CYCLE_CAP 200000.0

/* The modes, as the top two bits of an op byte. */
enum { SKIP, MOTION, SOLID, GRAD };

typedef unsigned short u16;
typedef unsigned char u8;
typedef char require_16_bit_short[sizeof(u16) == 2 ? 1 : -1];

#if defined(_WIN32) && defined(OH3DO_PREVIEW_DLL)
/* No C runtime: the compiler may still call these, and floating point needs
 * _fltused. */
int _fltused = 0;
void *memcpy(void *destination, const void *source, unsigned long long bytes)
{
    u8 *to = (u8 *)destination;
    const u8 *from = (const u8 *)source;
    while (bytes--)
        *to++ = *from++;
    return destination;
}
void *memset(void *destination, int value, unsigned long long bytes)
{
    u8 *to = (u8 *)destination;
    while (bytes--)
        *to++ = (u8)value;
    return destination;
}
#endif

static void copy(void *destination, const void *source, int bytes)
{
    u8 *to = (u8 *)destination;
    const u8 *from = (const u8 *)source;
    while (bytes-- > 0)
        *to++ = *from++;
}

static void zero(void *destination, int bytes)
{
    u8 *to = (u8 *)destination;
    while (bytes-- > 0)
        *to++ = 0;
}

/* One 5-bit component (at `shift`) of the color two thirds of the way from b
 * to a: (2a + b) * 11 / 32, as the console's decoder computes it. */
static unsigned int mix_component(u16 a, u16 b, int shift)
{
    return (((2U * ((a >> shift) & 31U) + ((b >> shift) & 31U)) * 11U) >> 5) << shift;
}

/* A GRAD block's mixed colors: mix(a, b) is nearer a, mix(b, a) nearer b. */
static u16 mix(u16 a, u16 b)
{
    return (u16)(mix_component(a, b, 10) | mix_component(a, b, 5) | mix_component(a, b, 0));
}

/* A GRAD block's four colors from its two endpoints. */
static void gradient_palette(u16 a, u16 b, u16 *palette)
{
    palette[0] = a;
    palette[1] = b;
    palette[2] = mix(a, b);
    palette[3] = mix(b, a);
}

/* ---- The reference decoder ------------------------------------------------
 *
 * A straightforward decoder, independent of the encoder's model, that every
 * packet must pass. Frames are big-endian RGB555 rows.
 */

typedef struct Reference {
    u16 cache[CACHE_SIZE];
    int next; /* the cache slot the next new color goes in */
} Reference;

static u16 get_pixel(const u8 *frame, int x, int y)
{
    return (u16)((frame[(y * WIDTH + x) * 2] << 8) | frame[(y * WIDTH + x) * 2 + 1]);
}

static void put_pixel(u8 *frame, int x, int y, u16 color)
{
    frame[(y * WIDTH + x) * 2] = (u8)(color >> 8);
    frame[(y * WIDTH + x) * 2 + 1] = (u8)color;
}

/* Reads `count` color codes: a byte with the top bit set names a cached
 * color; otherwise two bytes are a new color, which joins the cache. */
static int read_colors(Reference *reference, const u8 **data, const u8 *end, u16 *out, int count)
{
    const u8 *at = *data;
    int i;
    for (i = 0; i < count; ++i) {
        if (at >= end)
            return -1;
        if (*at & 0x80)
            out[i] = reference->cache[*at++ & (CACHE_SIZE - 1)];
        else {
            if (end - at < 2)
                return -1;
            out[i] = (u16)((at[0] << 8) | at[1]);
            at += 2;
            reference->cache[reference->next] = out[i];
            reference->next = (reference->next + 1) & (CACHE_SIZE - 1);
        }
    }
    *data = at;
    return 0;
}

/* Decodes one packet over `frame`. Returns 0, or -1 if it is malformed. */
static int reference_frame(Reference *reference, const u8 *data, long bytes, u8 *frame)
{
    STATE u16 copied[LONGEST_RUN][16];
    const u8 *end = data + bytes;
    int block = 0;
    while (block < BLOCK_COUNT) {
        int op, mode, run, i, k, vector = 0;
        u16 colors[2];
        if (data >= end)
            return -1;
        op = *data++;
        mode = op >> 6;
        run = (op & 63) + 1;
        if (block + run > BLOCK_COUNT)
            return -1;
        if (mode == MOTION) {
            if (data >= end || run > LONGEST_MOTION_RUN)
                return -1;
            vector = *data++;
            /* Every source is read before any destination of the run is written. */
            for (i = 0; i < run; ++i) {
                int b = block + i, source_x = (b % BLOCK_COLUMNS) * 4 + (vector >> 4) - 8,
                    source_y = (b / BLOCK_COLUMNS) * 4 + (vector & 15) - 8;
                if (source_x < 0 || source_y < 0 || source_x > WIDTH - 4 || source_y > HEIGHT - 4)
                    return -1;
                for (k = 0; k < 16; ++k)
                    copied[i][k] = get_pixel(frame, source_x + (k & 3), source_y + (k >> 2));
            }
        } else if (mode == SOLID && read_colors(reference, &data, end, colors, 1))
            return -1;
        for (i = 0; i < run; ++i, ++block) {
            int left = (block % BLOCK_COLUMNS) * 4, top = (block / BLOCK_COLUMNS) * 4;
            u16 out[16];
            if (mode == SKIP)
                continue;
            if (mode == MOTION)
                copy(out, copied[i], 32);
            else if (mode == SOLID)
                for (k = 0; k < 16; ++k)
                    out[k] = colors[0];
            else {
                u16 palette[4];
                unsigned long indexes;
                if (read_colors(reference, &data, end, colors, 2) || end - data < 4)
                    return -1;
                indexes = ((unsigned long)data[0] << 24) | ((unsigned long)data[1] << 16) |
                          ((unsigned long)data[2] << 8) | data[3];
                data += 4;
                gradient_palette(colors[0], colors[1], palette);
                for (k = 0; k < 16; ++k)
                    out[k] = palette[(indexes >> (30 - 2 * k)) & 3];
            }
            for (k = 0; k < 16; ++k)
                put_pixel(frame, left + (k & 3), top + (k >> 2), out[k]);
        }
    }
    return data == end ? 0 : -1;
}

/* ---- Colors and blocks ---------------------------------------------------- */

/* What the encoder chose for a block. */
typedef struct Choice {
    int mode;
    int param;    /* MOTION: the vector byte; SOLID: the color */
    u16 color[2]; /* GRAD: the endpoints */
    u8 index[16]; /* GRAD: each pixel's palette index */
} Choice;

/* The color cache, as the decoder will have it. */
typedef struct Cache {
    u16 color[CACHE_SIZE];
    int next;
} Cache;
STATE Cache cache;

/* For the main cache only, to find a color fast: the slot + 1 of each nonzero
 * color in it, or 0 if it is not. Nonzero colors are never cached twice;
 * zero fills the cache at the start. */
STATE u8 cache_slot[32768];

/* The squared error between two RGB555 colors. */
static int color_error(u16 a, u16 b)
{
    int red = (int)((a >> 10) & 31) - (int)((b >> 10) & 31),
        green = (int)((a >> 5) & 31) - (int)((b >> 5) & 31), blue = (int)(a & 31) - (int)(b & 31);
    return red * red + green * green + blue * blue;
}

static void load_block(const u16 *frame, int x, int y, u16 *pixels)
{
    int i;
    for (i = 0; i < 16; ++i)
        pixels[i] = frame[(y + i / 4) * WIDTH + x + i % 4];
}

/* The error between `pixels` and the 4x4 area of `frame` at (x, y). */
static long block_error(const u16 *pixels, const u16 *frame, int x, int y)
{
    long total = 0;
    int i;
    for (i = 0; i < 16; ++i)
        total += color_error(pixels[i], frame[(y + i / 4) * WIDTH + x + i % 4]);
    return total;
}

/* The same, but it may stop early (a row at a time) once the sum reaches
 * `limit`. */
static long block_error_limited(const u16 *pixels, const u16 *frame, int x, int y, double limit)
{
    long total = 0;
    int row;
    for (row = 0; row < 4; ++row) {
        const u16 *area = frame + (y + row) * WIDTH + x, *own = pixels + row * 4;
        total += color_error(own[0], area[0]) + color_error(own[1], area[1]) +
                 color_error(own[2], area[2]) + color_error(own[3], area[3]);
        if (total >= limit)
            break;
    }
    return total;
}

static int brightness(u16 color)
{
    return (int)((color >> 10) & 31) + (int)((color >> 5) & 31) + (int)(color & 31);
}

/*
 * Splits a block's 16 pixels into `k` (1, 2 or 4) colors by k-means, starting
 * from pixels spread across the brightness order. Returns the total error,
 * with each pixel's nearest center in `index`.
 */
static long cluster_colors(const u16 *pixels, int k, u16 *center, u8 *index)
{
    int order[16], i, j, round;
    long total = 0;

    /* The pixels from darkest to brightest (an insertion sort, so it is
     * stable). */
    for (i = 0; i < 16; ++i)
        order[i] = i;
    for (i = 1; i < 16; ++i)
        for (j = i; j > 0 && brightness(pixels[order[j]]) < brightness(pixels[order[j - 1]]); --j) {
            int kept = order[j];
            order[j] = order[j - 1];
            order[j - 1] = kept;
        }
    for (j = 0; j < k; ++j)
        center[j] = pixels[order[k == 1 ? 0 : j * 15 / (k - 1)]];

    for (round = 0; round < 10; ++round) {
        long sum[4][3] = {{0}};
        int count[4] = {0}, changed = 0;
        for (i = 0; i < 16; ++i) {
            int best = 0, best_error = color_error(pixels[i], center[0]);
            for (j = 1; j < k; ++j) {
                int error = color_error(pixels[i], center[j]);
                if (error < best_error) {
                    best_error = error;
                    best = j;
                }
            }
            index[i] = (u8)best;
            sum[best][0] += (pixels[i] >> 10) & 31;
            sum[best][1] += (pixels[i] >> 5) & 31;
            sum[best][2] += pixels[i] & 31;
            ++count[best];
        }
        /* Each center moves to its pixels' rounded mean. */
        for (j = 0; j < k; ++j)
            if (count[j]) {
                u16 mean = (u16)((((sum[j][0] * 2 + count[j]) / (2 * count[j])) << 10) |
                                 (((sum[j][1] * 2 + count[j]) / (2 * count[j])) << 5) |
                                 ((sum[j][2] * 2 + count[j]) / (2 * count[j])));
                if (mean != center[j]) {
                    center[j] = mean;
                    changed = 1;
                }
            }
        if (!changed)
            break;
    }

    for (i = 0; i < 16; ++i) {
        int best = 0, best_error = color_error(pixels[i], center[0]);
        for (j = 1; j < k; ++j) {
            int error = color_error(pixels[i], center[j]);
            if (error < best_error) {
                best_error = error;
                best = j;
            }
        }
        index[i] = (u8)best;
        total += best_error;
    }
    return total;
}

/* Gives each of `count` pixels its nearest color of the gradient a..b.
 * Returns the total error. */
static long assign_gradient(const u16 *pixels, int count, u16 a, u16 b, u8 *index)
{
    u16 palette[4];
    long total = 0;
    int i, j;
    gradient_palette(a, b, palette);
    for (i = 0; i < count; ++i) {
        int best = 0, best_error = color_error(pixels[i], palette[0]);
        for (j = 1; j < 4; ++j) {
            int error = color_error(pixels[i], palette[j]);
            if (error < best_error) {
                best_error = error;
                best = j;
            }
        }
        index[i] = (u8)best;
        total += best_error;
    }
    return total;
}

/*
 * The least-squares endpoints for fixed palette indexes (each index weighs
 * endpoint a by 1, 0, 2/3 or 1/3, and b by the rest), into *a and *b.
 * Returns 0 if the indexes cannot decide them (all one endpoint, say).
 */
static int refine_gradient(const u16 *pixels, int count, const u8 *index, u16 *a, u16 *b)
{
    static const double weight[4] = {1, 0, 2.0 / 3, 1.0 / 3};
    double aa = 0, ab = 0, bb = 0, determinant;
    int i, shift;
    u16 new_a = 0, new_b = 0;
    for (i = 0; i < count; ++i) {
        double w = weight[index[i]];
        aa += w * w;
        ab += w * (1 - w);
        bb += (1 - w) * (1 - w);
    }
    determinant = aa * bb - ab * ab;
    if (determinant < 1e-6)
        return 0;
    /* Each 5-bit component on its own. */
    for (shift = 0; shift < 15; shift += 5) {
        double xa = 0, xb = 0, value_a, value_b;
        for (i = 0; i < count; ++i) {
            double w = weight[index[i]], x = (pixels[i] >> shift) & 31;
            xa += w * x;
            xb += (1 - w) * x;
        }
        value_a = (bb * xa - ab * xb) / determinant;
        value_b = (aa * xb - ab * xa) / determinant;
        value_a = value_a < 0 ? 0 : value_a > 31 ? 31 : value_a;
        value_b = value_b < 0 ? 0 : value_b > 31 ? 31 : value_b;
        new_a |= (u16)((int)(value_a + 0.5) << shift);
        new_b |= (u16)((int)(value_b + 0.5) << shift);
    }
    *a = new_a;
    *b = new_b;
    return 1;
}

/* The best endpoints of `candidates` pairs, each refined by least squares
 * (twice at most, while it helps). Returns the error, with the endpoints in
 * `ends` and the indexes in `index`. */
static long search_gradients(const u16 *pixels, int count, u16 (*candidates)[2],
                             int candidate_count, u16 *ends, u8 *index)
{
    u16 a, b;
    u8 refined_index[16], candidate_index[16];
    long best = -1, error;
    int i, round;
    for (i = 0; i < candidate_count; ++i) {
        a = candidates[i][0];
        b = candidates[i][1];
        error = assign_gradient(pixels, count, a, b, candidate_index);
        for (round = 0; round < 2; ++round) {
            u16 refined_a = a, refined_b = b;
            long refined_error;
            if (!refine_gradient(pixels, count, candidate_index, &refined_a, &refined_b))
                break;
            refined_error = assign_gradient(pixels, count, refined_a, refined_b, refined_index);
            if (refined_error >= error)
                break;
            a = refined_a;
            b = refined_b;
            error = refined_error;
            copy(candidate_index, refined_index, count);
        }
        if (best < 0 || error < best) {
            best = error;
            ends[0] = a;
            ends[1] = b;
            copy(index, candidate_index, count);
        }
    }
    return best;
}

/* The block's best gradient, trying the two colors of 2-means and every pair
 * of the four colors of 4-means as endpoints. */
static long fit_gradient(const u16 *pixels, u16 *ends, u8 *index)
{
    u16 center[4], candidates[7][2];
    u8 unused_index[16];
    int count = 0, i, j;
    cluster_colors(pixels, 2, center, unused_index);
    candidates[count][0] = center[0];
    candidates[count][1] = center[1];
    ++count;
    cluster_colors(pixels, 4, center, unused_index);
    for (i = 0; i < 4; ++i)
        for (j = i + 1; j < 4; ++j) {
            candidates[count][0] = center[i];
            candidates[count][1] = center[j];
            ++count;
        }
    return search_gradients(pixels, 16, candidates, count, ends, index);
}

static void put_be16(u8 **at, u16 value)
{
    (*at)[0] = (u8)(value >> 8);
    (*at)[1] = (u8)value;
    *at += 2;
}

/* The lowest slot of `colors` holding `color`, or -1. */
static int cache_find(const Cache *colors, u16 color)
{
    int i;
    if (colors == &cache && color)
        return (int)cache_slot[color] - 1;
    for (i = 0; i < CACHE_SIZE; ++i)
        if (colors->color[i] == color)
            return i;
    return -1;
}

/* Codes one color through `colors` (writing it at *out, unless out is NULL,
 * which only models it), adding it if new. Returns the bits it took. */
static int cache_code(Cache *colors, u16 color, u8 **out)
{
    int found = cache_find(colors, color);
    if (found >= 0) {
        if (out)
            *(*out)++ = (u8)(0x80 | found);
        return 8;
    }
    if (colors == &cache) {
        u16 replaced = colors->color[colors->next];
        if (replaced && cache_slot[replaced] == colors->next + 1)
            cache_slot[replaced] = 0;
        if (color)
            cache_slot[color] = (u8)(colors->next + 1);
    }
    colors->color[colors->next] = color;
    colors->next = (colors->next + 1) & (CACHE_SIZE - 1);
    if (out)
        put_be16(out, color);
    return 16;
}

/* The bits cache_code would spend on these colors in order, without
 * changing the cache. (A color coded new earlier in the list pushes out the
 * oldest entries, so they no longer count as cached.) */
static int color_bits(const Cache *colors, const u16 *list, int count)
{
    u16 literal[16];
    int literals = 0, bits = 0, i, j;
    for (i = 0; i < count; ++i) {
        int hit = 0;
        for (j = 0; j < literals && !hit; ++j)
            hit = literal[j] == list[i];
        if (!hit && list[i] && colors == &cache) {
            j = (int)cache_slot[list[i]] - 1;
            hit = j >= 0 && ((j - colors->next) & (CACHE_SIZE - 1)) >= literals;
        } else
            for (j = 0; j < CACHE_SIZE && !hit; ++j)
                hit = colors->color[j] == list[i] &&
                      ((j - colors->next) & (CACHE_SIZE - 1)) >= literals;
        if (hit)
            bits += 8;
        else {
            literal[literals++] = list[i];
            bits += 16;
        }
    }
    return bits;
}

/*
 * Up to `wanted` cached colors nearest to `color` (not `color` itself),
 * nearest first; ties keep the lower cache slot. A partial sum already past
 * the worst kept match rules a color out. Returns how many there are.
 */
static int nearby_colors(const Cache *colors, u16 color, u16 *out, int wanted)
{
    int kept_error[NEARBY_COLORS], kept = 0, i, j, red = (color >> 10) & 31,
                                   green = (color >> 5) & 31, blue = color & 31;
    for (i = 0; i < CACHE_SIZE; ++i) {
        u16 candidate = colors->color[i];
        int error = ((candidate >> 10) & 31) - red, repeated = 0;
        error *= error;
        if (kept == wanted && error >= kept_error[kept - 1])
            continue;
        j = ((candidate >> 5) & 31) - green;
        error += j * j;
        if (kept == wanted && error >= kept_error[kept - 1])
            continue;
        j = (candidate & 31) - blue;
        error += j * j;
        if ((kept == wanted && error >= kept_error[kept - 1]) || candidate == color)
            continue;
        for (j = 0; j < kept; ++j)
            repeated |= out[j] == candidate;
        if (repeated)
            continue;
        /* Insert it in order, dropping the worst if the list is full. */
        if (kept < wanted)
            ++kept;
        for (j = kept - 1; j > 0 && kept_error[j - 1] > error; --j) {
            kept_error[j] = kept_error[j - 1];
            out[j] = out[j - 1];
        }
        kept_error[j] = error;
        out[j] = candidate;
    }
    return kept;
}

/* A GRAD block's cost in error + lambda * color bits, also trying nearby
 * cached colors for each endpoint (cheaper to send). `fitted` is the error
 * of `ends` as fit_gradient found them; `ends` and `index` take the best. */
static double gradient_cost(const u16 *pixels, int count, double lambda, u16 *ends, u8 *index,
                            long fitted)
{
    double best = fitted + lambda * color_bits(&cache, ends, 2);
    int end, i;
    for (end = 0; end < 2; ++end) {
        u16 nearby[NEARBY_COLORS];
        int found = nearby_colors(&cache, ends[end], nearby, NEARBY_COLORS);
        for (i = 0; i < found; ++i) {
            u16 tried[2];
            u8 tried_index[16];
            double cost;
            tried[0] = ends[0];
            tried[1] = ends[1];
            tried[end] = nearby[i];
            cost = assign_gradient(pixels, count, tried[0], tried[1], tried_index) +
                   lambda * color_bits(&cache, tried, 2);
            if (cost < best) {
                best = cost;
                ends[0] = tried[0];
                ends[1] = tried[1];
                copy(index, tried_index, count);
            }
        }
    }
    return best;
}

/* The colors a choice sends through the cache with each block. */
static int colors_sent(const Choice *choice)
{
    return choice->mode == GRAD ? 2 : 0;
}

/* The old pixels of the current run's blocks: a MOTION run copies from the
 * frame as it was before the run, so a source inside the run sees these. */
STATE u16 run_before[LONGEST_RUN][16];
STATE int run_start;

/* The 4x4 area at (x, y) as a MOTION copy for block `block` sees it. */
static void load_source(const u16 *frame, int x, int y, int block, u16 *pixels)
{
    int i;
    for (i = 0; i < 16; ++i) {
        int source_x = x + (i & 3), source_y = y + (i >> 2),
            source_block = (source_y >> 2) * BLOCK_COLUMNS + (source_x >> 2);
        pixels[i] = source_block >= run_start && source_block < block
                        ? run_before[source_block - run_start][(source_y & 3) * 4 + (source_x & 3)]
                        : frame[source_y * WIDTH + source_x];
    }
}

/* Draws a chosen block into the frame exactly as the decoder will. */
static void draw_choice(const Choice *choice, u16 *frame, int x, int y, int block)
{
    u16 out[16], palette[4];
    int i;
    switch (choice->mode) {
    case SKIP:
        return;
    case MOTION:
        load_source(frame, x + (choice->param >> 4) - 8, y + (choice->param & 15) - 8, block, out);
        break;
    case SOLID:
        for (i = 0; i < 16; ++i)
            out[i] = (u16)choice->param;
        break;
    default:
        gradient_palette(choice->color[0], choice->color[1], palette);
        for (i = 0; i < 16; ++i)
            out[i] = palette[choice->index[i]];
        break;
    }
    for (i = 0; i < 16; ++i)
        frame[(y + i / 4) * WIDTH + x + i % 4] = out[i];
}

/*
 * The ARM60 clocks a block adds to the frame's decode, from a model fitted to
 * the assembly decoder: every op 32; a one-block motion run 90; longer motion
 * runs 136 plus 37 a block; odd dx about 20 a block more; solid runs 84 plus
 * 21 a block; GRAD 224 a block. `continues`: the block shares the previous
 * block's op.
 */
static double block_clocks(int mode, int continues, int run, int param)
{
    int odd = mode == MOTION && (((param >> 4) - 8) & 1);
    switch (mode) {
    case SKIP:
        return continues ? 0 : 32;
    case MOTION:
        return continues ? (run == 1 ? 120 : 37) + (odd ? 20 : 0) : 122 + (odd ? 23 : 0);
    case SOLID:
        return continues ? 21 : 137;
    default:
        return 224 + (continues ? 0 : 32);
    }
}
#define FRAME_CLOCKS 600 /* setting up a frame */

/* ---- Encoding a frame ------------------------------------------------------ */

/* The run of blocks the current op covers so far, which decides whether a
 * block can share it. */
typedef struct Run {
    int open;        /* the run can take another block */
    int length;      /* blocks in it */
    int mode, param; /* of its blocks (-1 before the first block) */
} Run;

/* Whether a block of `mode` with `param` would continue the run: same mode,
 * same param (GRAD blocks carry their own colors), and room left. */
static int continues_run(const Run *run, int mode, int param)
{
    return run->open && run->mode == mode && (run->param == param || mode == GRAD) &&
           (mode != MOTION || run->length < LONGEST_MOTION_RUN);
}

/* The op bits a block costs: none of its own when it continues the run,
 * else an op byte (and a vector byte for MOTION). */
static int op_bits(const Run *run, int mode, int param)
{
    return continues_run(run, mode, param) ? 1 : 8 + (mode == MOTION ? 8 : 0);
}

/* The price of the decode clocks a block adds. */
static double clock_cost(const Run *run, double mu, int mode, int param)
{
    return mu * block_clocks(mode, continues_run(run, mode, param), run->length, param);
}

/* The best MOTION choice for a block that beats `best`, if any, into
 * `choice`. Returns the (possibly lowered) best cost. */
static double try_motion(const Run *run, const u16 *pixels, const u16 *frame, int x, int y,
                         int block, double lambda, double mu, double best, Choice *choice)
{
    int dx, dy, i;
    for (dy = -MOTION_RANGE; dy < MOTION_RANGE; ++dy)
        for (dx = -MOTION_RANGE; dx < MOTION_RANGE; ++dx) {
            int vector = ((dx + 8) << 4) | (dy + 8);
            double rate, cost;
            if ((!dx && !dy) || x + dx < 0 || y + dy < 0 || x + dx > WIDTH - 4 ||
                y + dy > HEIGHT - 4)
                continue;
            rate = lambda * op_bits(run, MOTION, vector) + clock_cost(run, mu, MOTION, vector);
            if (rate >= best)
                continue;
            if (continues_run(run, MOTION, vector)) {
                /* Inside the run the source is the frame before the run. */
                u16 source[16];
                long error = 0;
                load_source(frame, x + dx, y + dy, block, source);
                for (i = 0; i < 16; ++i)
                    error += color_error(pixels[i], source[i]);
                cost = error + rate;
            } else
                cost = block_error_limited(pixels, frame, x + dx, y + dy, best - rate) + rate;
            if (cost < best) {
                best = cost;
                choice->mode = MOTION;
                choice->param = vector;
            }
        }
    return best;
}

/* The best SOLID choice that beats `best`: the block's mean color, cached
 * colors near it, or the run's own color. */
static double try_solid(const Run *run, const u16 *pixels, double lambda, double mu, double best,
                        Choice *choice)
{
    u16 candidates[NEARBY_COLORS + 2];
    u8 unused_index[16];
    int count, i;
    cluster_colors(pixels, 1, candidates, unused_index);
    count = 1 + nearby_colors(&cache, candidates[0], candidates + 1, NEARBY_COLORS);
    if (run->mode == SOLID)
        candidates[count++] = (u16)run->param;
    for (i = 0; i < count; ++i) {
        long error = 0;
        int k;
        double cost;
        for (k = 0; k < 16; ++k)
            error += color_error(pixels[k], candidates[i]);
        cost = error +
               lambda * (op_bits(run, SOLID, candidates[i]) +
                         (continues_run(run, SOLID, candidates[i])
                              ? 0
                              : color_bits(&cache, candidates + i, 1))) +
               clock_cost(run, mu, SOLID, candidates[i]);
        if (cost < best) {
            best = cost;
            choice->mode = SOLID;
            choice->param = candidates[i];
        }
    }
    return best;
}

/* The GRAD choice if it beats `best`: the fitted gradient, 32 index bits,
 * and its colors. */
static double try_gradient(const Run *run, const u16 *pixels, double lambda, double mu, double best,
                           Choice *choice)
{
    u16 ends[2];
    u8 index[16];
    long fitted = fit_gradient(pixels, ends, index);
    double cost = gradient_cost(pixels, 16, lambda, ends, index, fitted) +
                  lambda * (op_bits(run, GRAD, 0) + 32) + clock_cost(run, mu, GRAD, 0);
    if (cost < best) {
        best = cost;
        choice->mode = GRAD;
        choice->param = 0;
        copy(choice->color, ends, 4);
        copy(choice->index, index, 16);
    }
    return best;
}

/* Every block's choice for the frame being encoded. */
STATE Choice choices[BLOCK_COUNT];

/*
 * Chooses every block of the frame `source`, drawing each choice into `frame`
 * (the previous decoded frame, updated in place as the decoder will) and
 * modelling the color cache. Returns the frame's decode clocks.
 */
static double choose_blocks(const u16 *source, u16 *frame, double lambda, double mu)
{
    double clocks = FRAME_CLOCKS;
    Run run;
    int block;

    run.length = 0;
    run.mode = -1;
    run.param = -1;
    run_start = 0;
    for (block = 0; block < BLOCK_COUNT; ++block) {
        int x = (block % BLOCK_COLUMNS) * 4, y = (block / BLOCK_COLUMNS) * 4, i;
        u16 pixels[16];
        double best;
        Choice *choice = &choices[block];

        load_block(source, x, y, pixels);
        run.open = run.length > 0 && run.length < LONGEST_RUN;
        if (!run.open)
            run_start = block;

        /* SKIP costs only its error; the others must beat it. */
        best = block_error(pixels, frame, x, y) + lambda * op_bits(&run, SKIP, 0) +
               clock_cost(&run, mu, SKIP, 0);
        choice->mode = SKIP;
        choice->param = 0;
        best = try_motion(&run, pixels, frame, x, y, block, lambda, mu, best, choice);
        best = try_solid(&run, pixels, lambda, mu, best, choice);
        try_gradient(&run, pixels, lambda, mu, best, choice);

        /* Model what writing the packet will do with the cache. */
        if (choice->mode == SOLID) {
            if (!continues_run(&run, SOLID, choice->param))
                cache_code(&cache, (u16)choice->param, 0);
        } else
            for (i = 0; i < colors_sent(choice); ++i)
                cache_code(&cache, choice->color[i], 0);

        clocks += block_clocks(choice->mode, continues_run(&run, choice->mode, choice->param),
                               run.length, choice->param);
        if (continues_run(&run, choice->mode, choice->param))
            ++run.length;
        else {
            run.length = 1;
            run_start = block;
        }
        run.mode = choice->mode;
        run.param = choice->param;
        load_block(frame, x, y, run_before[block - run_start]);
        draw_choice(choice, frame, x, y, block);
    }
    return clocks;
}

/* Writes the frame's packet from the choices, coding colors through `colors`
 * (the cache as it was at the start of the frame). Returns its length. */
static long write_packet(Cache *colors, u8 *packet)
{
    u8 *at = packet;
    int block, i, j;
    for (block = 0; block < BLOCK_COUNT;) {
        const Choice *first = &choices[block];
        int length = 1;
        /* Blocks join the run while their mode (and param, except GRAD's)
         * match, up to the longest run. */
        while (block + length < BLOCK_COUNT &&
               length < (first->mode == MOTION ? LONGEST_MOTION_RUN : LONGEST_RUN) &&
               choices[block + length].mode == first->mode &&
               (first->mode == GRAD || choices[block + length].param == first->param))
            ++length;
        *at++ = (u8)((first->mode << 6) | (length - 1));
        if (first->mode == MOTION)
            *at++ = (u8)first->param;
        else if (first->mode == SOLID)
            cache_code(colors, (u16)first->param, &at);
        else if (first->mode == GRAD)
            for (i = 0; i < length; ++i) {
                const Choice *choice = &choices[block + i];
                unsigned long indexes = 0;
                for (j = 0; j < 2; ++j)
                    cache_code(colors, choice->color[j], &at);
                for (j = 0; j < 16; ++j)
                    indexes = (indexes << 2) | choice->index[j];
                *at++ = (u8)(indexes >> 24);
                *at++ = (u8)(indexes >> 16);
                *at++ = (u8)(indexes >> 8);
                *at++ = (u8)indexes;
            }
        block += length;
    }
    return at - packet;
}

/*
 * Encodes one frame: chooses its blocks, then writes the packet, replaying
 * the color cache from the start of the frame. Returns the packet's length,
 * or -1 if writing and choosing disagree about the cache.
 */
static long encode_frame(const u16 *source, u16 *frame, double lambda, double mu, u8 *packet,
                         double *clocks)
{
    STATE Cache at_start;
    double spent;
    long length;
    int slot;

    copy(&at_start, &cache, sizeof(cache));
    spent = choose_blocks(source, frame, lambda, mu);
    length = write_packet(&at_start, packet);
    for (slot = 0; slot < CACHE_SIZE; ++slot)
        if (at_start.color[slot] != cache.color[slot])
            return -1;
    if (at_start.next != cache.next)
        return -1;
    *clocks = spent;
    return length;
}

EXPORT unsigned int oh3do_preview_abi(void)
{
    return 2;
}

STATE double slowest_frame_clocks;

/* The modelled decode clocks of the slowest frame in the last encode. */
EXPORT double oh3do_preview_max_cycles(void)
{
    return slowest_frame_clocks;
}

/*
 * Encodes a frame within CYCLE_CAP. The previous frame's mu, scaled down, is
 * the first guess (busy frames come in runs); mu grows until the frame fits.
 * The result depends only on this call's input.
 */
static long encode_capped(const u16 *source, u16 *frame, double lambda, double *mu, u8 *packet,
                          double *clocks)
{
    STATE u16 frame_before[PIXELS];
    STATE Cache cache_before;
    STATE u8 slots_before[32768];
    double guess = *mu < lambda / 256 ? 0 : *mu * 0.6;
    long length;
    copy(frame_before, frame, sizeof(frame_before));
    copy(&cache_before, &cache, sizeof(cache));
    copy(slots_before, cache_slot, sizeof(cache_slot));
    for (;;) {
        length = encode_frame(source, frame, lambda, guess, packet, clocks);
        if (length < 0 || *clocks <= CYCLE_CAP || guess > 1e9)
            break;
        /* Too slow to decode: undo the frame and price clocks higher. */
        copy(frame, frame_before, sizeof(frame_before));
        copy(&cache, &cache_before, sizeof(cache));
        copy(cache_slot, slots_before, sizeof(cache_slot));
        guess = guess > 0 ? guess * 1.6 : (*mu > 0 ? *mu : lambda / 64);
    }
    *mu = guess;
    return length;
}

static void put_be32(u8 *at, unsigned long value)
{
    at[0] = (u8)(value >> 24);
    at[1] = (u8)(value >> 16);
    at[2] = (u8)(value >> 8);
    at[3] = (u8)value;
}

/*
 * Encodes `count` frames of 160x120 RGB24, truncated to RGB555 as the console
 * shows them. The first `split` frames use lambda_first, the rest lambda.
 * `out` receives the .3dv video section: (count + 1) big-endian offsets, then
 * the packets. `decoded`, if not NULL, receives every decoded frame
 * (count * 38400 bytes). Returns the section length, -1 for bad arguments,
 * -2 if out is too small, or -3 if a packet failed its reference-decoder
 * check (an encoder bug).
 */
EXPORT long oh3do_preview_encode(const unsigned char *frames, unsigned int count,
                                 double lambda_first, unsigned int split, double lambda,
                                 unsigned char *out, unsigned long capacity, unsigned char *decoded)
{
    STATE u16 source[PIXELS], frame[PIXELS];
    double clocks = 0, mu = 0;
    STATE u8 packet[LARGEST_PACKET], check[FRAME_BYTES];
    STATE Reference reference;
    unsigned long total;
    unsigned int f, i;

    if (!frames || !out || !count || count > MAX_FRAMES || !(lambda > 0) || !(lambda_first > 0))
        return -1;
    total = (count + 1) * 4UL;
    if (capacity < total)
        return -2;
    slowest_frame_clocks = 0;
    zero(frame, sizeof(frame));
    zero(check, sizeof(check));
    zero(&cache, sizeof(cache));
    zero(cache_slot, sizeof(cache_slot));
    zero(&reference, sizeof(reference));

    for (f = 0; f < count; ++f) {
        const unsigned char *rgb = frames + (unsigned long)f * PIXELS * 3;
        long length;
        for (i = 0; i < PIXELS; ++i)
            source[i] = (u16)(((rgb[i * 3] >> 3) << 10) | ((rgb[i * 3 + 1] >> 3) << 5) |
                              (rgb[i * 3 + 2] >> 3));
        length =
            encode_capped(source, frame, f < split ? lambda_first : lambda, &mu, packet, &clocks);
        if (clocks > slowest_frame_clocks)
            slowest_frame_clocks = clocks;

        /* The reference decoder must draw exactly the encoder's model. */
        if (length < 0 || reference_frame(&reference, packet, length, check))
            return -3;
        for (i = 0; i < PIXELS; ++i)
            if (get_pixel(check, i % WIDTH, i / WIDTH) != frame[i])
                return -3;

        if (capacity - total < (unsigned long)length)
            return -2;
        put_be32(out + f * 4, total);
        copy(out + total, packet, (int)length);
        total += (unsigned long)length;
        if (decoded)
            copy(decoded + (unsigned long)f * FRAME_BYTES, check, FRAME_BYTES);
    }
    put_be32(out + count * 4, total);
    return (long)total;
}
