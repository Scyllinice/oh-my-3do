/* Original MIT-licensed encoder for OM3DVID5 menu previews, 160x120 at 30 fps
 * (see src/preview_v5.h for the bitstream). Allocation-free and CRT-free, so
 * the Windows DLL has no imports; prepare-preview.py loads it with ctypes and
 * the desktop app links it statically. Both callers search lambda themselves.
 *
 * Every 4x4 block takes the mode with the lowest distortion + lambda * bits
 * + mu * decode cycles, judged against the frame exactly as decoded so far
 * (decoding is in place). mu is zero unless a frame would take the console
 * longer than CYCLE_CAP to decode; then it rises until the frame fits.
 * Each finished packet is decoded again by an independent reference decoder,
 * and the result must equal the encoder's model of the frame.
 * Working state is static. The desktop app encodes several clips at once and
 * defines OH3DO_THREAD_LOCAL so each thread gets its own copy. */
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

#define W           160
#define H           120
#define PIXELS      (W * H)
#define FRAME_BYTES (PIXELS * 2)
#define BLOCKS_X    (W / 4)
#define BLOCKS      (BLOCKS_X * (H / 4))
#define CACHE       128
#define RANGE       8
#define SNAP        6
#define MAX_FRAMES  450
#define MAX_PACKET  (BLOCKS * 12)
#define MAXRUN      64
#define MOTION_RUN  32 /* longest MOTION run, so its parts never overlap */
/* Per-frame decode budget in ARM60 clocks (12.5 MHz, page-mode DRAM), from
 * a cycle count of src/preview_v5_arm.s. The console measured 1.28 times the
 * counted time, so this is about 20.5 ms of a 33.3 ms frame, leaving the
 * rest for the cel engine to draw it. Lower caps cost visible detail in busy
 * frames; uncapped, the busiest frames would take most of a frame. */
#define CYCLE_CAP 200000.0
enum { SKIP, MOTION, SOLID, GRAD };
#define MIX1(a, b, s) ((((2U * (((a) >> (s)) & 31U) + (((b) >> (s)) & 31U)) * 11U) >> 5) << (s))
#define MIX(a, b)     ((u16)(MIX1(a, b, 10) | MIX1(a, b, 5) | MIX1(a, b, 0)))

typedef unsigned short u16;
typedef unsigned char u8;
typedef char require_16_bit_short[sizeof(u16) == 2 ? 1 : -1];

#if defined(_WIN32) && defined(OH3DO_PREVIEW_DLL)
/* No CRT: the compiler may still call these, and floating point needs _fltused. */
int _fltused = 0;
void *memcpy(void *d, const void *s, unsigned long long n)
{
    u8 *a = (u8 *)d;
    const u8 *b = (const u8 *)s;
    while (n--)
        *a++ = *b++;
    return d;
}
void *memset(void *d, int c, unsigned long long n)
{
    u8 *a = (u8 *)d;
    while (n--)
        *a++ = (u8)c;
    return d;
}
#endif

static void copy(void *d, const void *s, int n)
{
    u8 *a = (u8 *)d;
    const u8 *b = (const u8 *)s;
    while (n-- > 0)
        *a++ = *b++;
}
static void zero(void *d, int n)
{
    u8 *a = (u8 *)d;
    while (n-- > 0)
        *a++ = 0;
}

/* ---- Reference decoder: straightforward, independent of the encoder's model ---- */

#define STATIC_REF STATE
typedef struct Reference {
    u16 cache[CACHE];
    int next;
} Reference;

static u16 get(const u8 *frame, int x, int y)
{
    return (u16)((frame[(y * W + x) * 2] << 8) | frame[(y * W + x) * 2 + 1]);
}
static void put(u8 *frame, int x, int y, u16 c)
{
    frame[(y * W + x) * 2] = (u8)(c >> 8);
    frame[(y * W + x) * 2 + 1] = (u8)c;
}

static int read_colours(Reference *r, const u8 **data, const u8 *end, u16 *out, int count)
{
    const u8 *p = *data;
    int i;
    for (i = 0; i < count; ++i) {
        if (p >= end)
            return -1;
        if (*p & 0x80)
            out[i] = r->cache[*p++ & (CACHE - 1)];
        else {
            if (end - p < 2)
                return -1;
            out[i] = (u16)((p[0] << 8) | p[1]);
            p += 2;
            r->cache[r->next] = out[i];
            r->next = (r->next + 1) & (CACHE - 1);
        }
    }
    *data = p;
    return 0;
}

static int reference_frame(Reference *r, const u8 *data, long bytes, u8 *frame)
{
    STATIC_REF u16 copied[MAXRUN][16];
    const u8 *end = data + bytes;
    int block = 0;
    while (block < BLOCKS) {
        int op, mode, run, i, k, vector = 0;
        u16 c[2];
        if (data >= end)
            return -1;
        op = *data++;
        mode = op >> 6;
        run = (op & 63) + 1;
        if (block + run > BLOCKS)
            return -1;
        if (mode == MOTION) {
            if (data >= end || run > MOTION_RUN)
                return -1;
            vector = *data++;
            /* Every source is read before any destination of the run is written. */
            for (i = 0; i < run; ++i) {
                int b = block + i, sx = (b % BLOCKS_X) * 4 + (vector >> 4) - 8,
                    sy = (b / BLOCKS_X) * 4 + (vector & 15) - 8;
                if (sx < 0 || sy < 0 || sx > W - 4 || sy > H - 4)
                    return -1;
                for (k = 0; k < 16; ++k)
                    copied[i][k] = get(frame, sx + (k & 3), sy + (k >> 2));
            }
        } else if (mode == SOLID && read_colours(r, &data, end, c, 1))
            return -1;
        for (i = 0; i < run; ++i, ++block) {
            int bx = (block % BLOCKS_X) * 4, by = (block / BLOCKS_X) * 4;
            u16 out[16];
            if (mode == SKIP)
                continue;
            if (mode == MOTION)
                copy(out, copied[i], 32);
            else if (mode == SOLID)
                for (k = 0; k < 16; ++k)
                    out[k] = c[0];
            else {
                u16 palette[4];
                unsigned long bits;
                if (read_colours(r, &data, end, c, 2) || end - data < 4)
                    return -1;
                bits = ((unsigned long)data[0] << 24) | ((unsigned long)data[1] << 16) |
                       ((unsigned long)data[2] << 8) | data[3];
                data += 4;
                palette[0] = c[0];
                palette[1] = c[1];
                palette[2] = MIX(c[0], c[1]);
                palette[3] = MIX(c[1], c[0]);
                for (k = 0; k < 16; ++k)
                    out[k] = palette[(bits >> (30 - 2 * k)) & 3];
            }
            for (k = 0; k < 16; ++k)
                put(frame, bx + (k & 3), by + (k >> 2), out[k]);
        }
    }
    return data == end ? 0 : -1;
}

/* ---- Encoder ---- */

typedef struct Choice {
    int mode, param;
    u16 color[2];
    u8 index[16];
} Choice;
typedef struct Cache {
    u16 c[CACHE];
    int next;
} Cache;
STATE Cache cache;
/* For the decision cache only: slot+1 of each nonzero color, 0 if absent.
 * Nonzero colors are never cached twice; zero fills the initial cache. */
STATE u8 slot[32768];

static int dist(u16 a, u16 b)
{
    int r = (int)((a >> 10) & 31) - (int)((b >> 10) & 31),
        g = (int)((a >> 5) & 31) - (int)((b >> 5) & 31), bl = (int)(a & 31) - (int)(b & 31);
    return r * r + g * g + bl * bl;
}

static void load_block(const u16 *frame, int x, int y, u16 *px)
{
    int i;
    for (i = 0; i < 16; ++i)
        px[i] = frame[(y + i / 4) * W + x + i % 4];
}

static long block_dist(const u16 *px, const u16 *frame, int x, int y)
{
    long d = 0;
    int i;
    for (i = 0; i < 16; ++i)
        d += dist(px[i], frame[(y + i / 4) * W + x + i % 4]);
    return d;
}

/* Like block_dist, but may stop early once the sum reaches limit. */
static long block_dist_limit(const u16 *px, const u16 *frame, int x, int y, double limit)
{
    long d = 0;
    int r;
    for (r = 0; r < 4; ++r) {
        const u16 *row = frame + (y + r) * W + x, *p = px + r * 4;
        d += dist(p[0], row[0]) + dist(p[1], row[1]) + dist(p[2], row[2]) + dist(p[3], row[3]);
        if (d >= limit)
            break;
    }
    return d;
}

static int luma(u16 c)
{
    return (int)((c >> 10) & 31) + (int)((c >> 5) & 31) + (int)(c & 31);
}

static long kmeans(const u16 *px, int k, u16 *center, u8 *index)
{
    int order[16], i, j, it;
    long d = 0;
    for (i = 0; i < 16; ++i)
        order[i] = i;
    for (i = 1; i < 16; ++i)
        for (j = i; j > 0 && luma(px[order[j]]) < luma(px[order[j - 1]]); --j) {
            int t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
    for (j = 0; j < k; ++j)
        center[j] = px[order[k == 1 ? 0 : j * 15 / (k - 1)]];
    for (it = 0; it < 10; ++it) {
        long sum[4][3] = {{0}};
        int count[4] = {0}, changed = 0;
        for (i = 0; i < 16; ++i) {
            int best = 0, bd = dist(px[i], center[0]);
            for (j = 1; j < k; ++j) {
                int e = dist(px[i], center[j]);
                if (e < bd) {
                    bd = e;
                    best = j;
                }
            }
            index[i] = (u8)best;
            sum[best][0] += (px[i] >> 10) & 31;
            sum[best][1] += (px[i] >> 5) & 31;
            sum[best][2] += px[i] & 31;
            ++count[best];
        }
        for (j = 0; j < k; ++j)
            if (count[j]) {
                u16 c = (u16)((((sum[j][0] * 2 + count[j]) / (2 * count[j])) << 10) |
                              (((sum[j][1] * 2 + count[j]) / (2 * count[j])) << 5) |
                              ((sum[j][2] * 2 + count[j]) / (2 * count[j])));
                if (c != center[j]) {
                    center[j] = c;
                    changed = 1;
                }
            }
        if (!changed)
            break;
    }
    for (i = 0; i < 16; ++i) {
        int best = 0, bd = dist(px[i], center[0]);
        for (j = 1; j < k; ++j) {
            int e = dist(px[i], center[j]);
            if (e < bd) {
                bd = e;
                best = j;
            }
        }
        index[i] = (u8)best;
        d += bd;
    }
    return d;
}

static long grad_assign(const u16 *px, int n, u16 a, u16 b, u8 *index)
{
    u16 palette[4];
    long d = 0;
    int i, j;
    palette[0] = a;
    palette[1] = b;
    palette[2] = MIX(a, b);
    palette[3] = MIX(b, a);
    for (i = 0; i < n; ++i) {
        int best = 0, bd = dist(px[i], palette[0]);
        for (j = 1; j < 4; ++j) {
            int e = dist(px[i], palette[j]);
            if (e < bd) {
                bd = e;
                best = j;
            }
        }
        index[i] = (u8)best;
        d += bd;
    }
    return d;
}

/* Least-squares endpoints for fixed indexes; weights on a are 1, 0, 2/3, 1/3. */
static int grad_refine(const u16 *px, int n, const u8 *index, u16 *a, u16 *b)
{
    static const double weight[4] = {1, 0, 2.0 / 3, 1.0 / 3};
    double aa = 0, ab = 0, bb = 0, det;
    int i, s;
    u16 na = 0, nb = 0;
    for (i = 0; i < n; ++i) {
        double w = weight[index[i]];
        aa += w * w;
        ab += w * (1 - w);
        bb += (1 - w) * (1 - w);
    }
    det = aa * bb - ab * ab;
    if (det < 1e-6)
        return 0;
    for (s = 0; s < 15; s += 5) {
        double xa = 0, xb = 0, va, vb;
        for (i = 0; i < n; ++i) {
            double w = weight[index[i]], x = (px[i] >> s) & 31;
            xa += w * x;
            xb += (1 - w) * x;
        }
        va = (bb * xa - ab * xb) / det;
        vb = (aa * xb - ab * xa) / det;
        va = va < 0 ? 0 : va > 31 ? 31 : va;
        vb = vb < 0 ? 0 : vb > 31 ? 31 : vb;
        na |= (u16)((int)(va + 0.5) << s);
        nb |= (u16)((int)(vb + 0.5) << s);
    }
    *a = na;
    *b = nb;
    return 1;
}

/* Best endpoints from candidate pairs, each refined by least squares. */
static long grad_search(const u16 *px, int n, u16 (*cand)[2], int count, u16 *ends, u8 *index)
{
    u16 a, b;
    u8 idx[16], tmp[16];
    long best = -1, d;
    int i, it;
    for (i = 0; i < count; ++i) {
        a = cand[i][0];
        b = cand[i][1];
        d = grad_assign(px, n, a, b, tmp);
        for (it = 0; it < 2; ++it) {
            u16 ra = a, rb = b;
            long e;
            if (!grad_refine(px, n, tmp, &ra, &rb))
                break;
            e = grad_assign(px, n, ra, rb, idx);
            if (e >= d)
                break;
            a = ra;
            b = rb;
            d = e;
            copy(tmp, idx, n);
        }
        if (best < 0 || d < best) {
            best = d;
            ends[0] = a;
            ends[1] = b;
            copy(index, tmp, n);
        }
    }
    return best;
}

static long grad_fit(const u16 *px, u16 *ends, u8 *index)
{
    u16 center[4], cand[7][2];
    u8 idx[16];
    int n = 0, i, j;
    kmeans(px, 2, center, idx);
    cand[n][0] = center[0];
    cand[n][1] = center[1];
    ++n;
    kmeans(px, 4, center, idx);
    for (i = 0; i < 4; ++i)
        for (j = i + 1; j < 4; ++j) {
            cand[n][0] = center[i];
            cand[n][1] = center[j];
            ++n;
        }
    return grad_search(px, 16, cand, n, ends, index);
}

static void put16(u8 **p, u16 v)
{
    (*p)[0] = (u8)(v >> 8);
    (*p)[1] = (u8)v;
    *p += 2;
}

/* Lowest cache index holding c, or -1. */
static int cache_find(const Cache *k, u16 c)
{
    int i;
    if (k == &cache && c)
        return (int)slot[c] - 1;
    for (i = 0; i < CACHE; ++i)
        if (k->c[i] == c)
            return i;
    return -1;
}

/* Emit one color code (or only model it when out is null). Returns its bits. */
static int cache_put(Cache *k, u16 c, u8 **out)
{
    int i = cache_find(k, c);
    if (i >= 0) {
        if (out)
            *(*out)++ = (u8)(0x80 | i);
        return 8;
    }
    if (k == &cache) {
        u16 old = k->c[k->next];
        if (old && slot[old] == k->next + 1)
            slot[old] = 0;
        if (c)
            slot[c] = (u8)(k->next + 1);
    }
    k->c[k->next] = c;
    k->next = (k->next + 1) & (CACHE - 1);
    if (out)
        put16(out, c);
    return 16;
}

/* Bits cache_put would spend on these colors in order, without changing k. */
static int colour_bits(const Cache *k, const u16 *c, int n)
{
    u16 literal[16];
    int literals = 0, bits = 0, i, j;
    for (i = 0; i < n; ++i) {
        int hit = 0;
        for (j = 0; j < literals && !hit; ++j)
            hit = literal[j] == c[i];
        if (!hit && c[i] && k == &cache) {
            j = (int)slot[c[i]] - 1;
            hit = j >= 0 && ((j - k->next) & (CACHE - 1)) >= literals;
        } else
            for (j = 0; j < CACHE && !hit; ++j)
                hit = k->c[j] == c[i] && ((j - k->next) & (CACHE - 1)) >= literals;
        if (hit)
            bits += 8;
        else {
            literal[literals++] = c[i];
            bits += 16;
        }
    }
    return bits;
}

/* Up to want cached colors nearest to c (excluding c), nearest first; ties
 * keep the lower cache index. A partial sum already past the worst kept
 * match rules a color out. */
static int nearest(const Cache *k, u16 c, u16 *out, int want)
{
    int dd[SNAP], n = 0, i, j, r = (c >> 10) & 31, g = (c >> 5) & 31, b = c & 31;
    for (i = 0; i < CACHE; ++i) {
        u16 e = k->c[i];
        int d = ((e >> 10) & 31) - r, dup = 0;
        d *= d;
        if (n == want && d >= dd[n - 1])
            continue;
        j = ((e >> 5) & 31) - g;
        d += j * j;
        if (n == want && d >= dd[n - 1])
            continue;
        j = (e & 31) - b;
        d += j * j;
        if ((n == want && d >= dd[n - 1]) || e == c)
            continue;
        for (j = 0; j < n; ++j)
            dup |= out[j] == e;
        if (dup)
            continue;
        if (n < want)
            ++n;
        for (j = n - 1; j > 0 && dd[j - 1] > d; --j) {
            dd[j] = dd[j - 1];
            out[j] = out[j - 1];
        }
        dd[j] = d;
        out[j] = e;
    }
    return n;
}

/* GRAD over n pixels, trying nearby cached colors for each endpoint. */
static double grad_rd(const u16 *px, int n, double lambda, u16 *ends, u8 *index, long fitted)
{
    double best = fitted + lambda * colour_bits(&cache, ends, 2);
    int s, i;
    for (s = 0; s < 2; ++s) {
        u16 alt[SNAP];
        int m = nearest(&cache, ends[s], alt, SNAP);
        for (i = 0; i < m; ++i) {
            u16 t[2];
            u8 idx[16];
            double j;
            t[0] = ends[0];
            t[1] = ends[1];
            t[s] = alt[i];
            j = grad_assign(px, n, t[0], t[1], idx) + lambda * colour_bits(&cache, t, 2);
            if (j < best) {
                best = j;
                ends[0] = t[0];
                ends[1] = t[1];
                copy(index, idx, n);
            }
        }
    }
    return best;
}

static int colour_count(const Choice *c)
{
    return c->mode == GRAD ? 2 : 0;
}

/* Old pixels of the current run's blocks: a motion run copies from the frame
 * as it was before the run, so a source inside the run sees these. */
STATE u16 saved[MAXRUN][16];
STATE int run_start;

static void load_source(const u16 *frame, int x, int y, int b, u16 *px)
{
    int i;
    for (i = 0; i < 16; ++i) {
        int sx = x + (i & 3), sy = y + (i >> 2), bi = (sy >> 2) * BLOCKS_X + (sx >> 2);
        px[i] = bi >= run_start && bi < b ? saved[bi - run_start][(sy & 3) * 4 + (sx & 3)]
                                          : frame[sy * W + sx];
    }
}

/* Draw a chosen block into the frame exactly as the decoder will. */
static void reconstruct(const Choice *c, u16 *frame, int x, int y, int b)
{
    u16 out[16], palette[4];
    int i;
    switch (c->mode) {
    case SKIP:
        return;
    case MOTION:
        load_source(frame, x + (c->param >> 4) - 8, y + (c->param & 15) - 8, b, out);
        break;
    case SOLID:
        for (i = 0; i < 16; ++i)
            out[i] = (u16)c->param;
        break;
    default:
        palette[0] = c->color[0];
        palette[1] = c->color[1];
        palette[2] = MIX(c->color[0], c->color[1]);
        palette[3] = MIX(c->color[1], c->color[0]);
        for (i = 0; i < 16; ++i)
            out[i] = palette[c->index[i]];
        break;
    }
    for (i = 0; i < 16; ++i)
        frame[(y + i / 4) * W + x + i % 4] = out[i];
}

/* ARM60 clocks a block adds to the frame's decode, from a model fitted to the
 * assembly decoder: every op 32; a one-block motion run 90; longer motion
 * runs 136 plus 37 a block; odd dx about 20 a block more; solid runs 84 plus
 * 21 a block; GRAD 224 a block. */
static double block_cycles(int mode, int cont, int run, int param)
{
    int odd = mode == MOTION && (((param >> 4) - 8) & 1);
    switch (mode) {
    case SKIP:
        return cont ? 0 : 32;
    case MOTION:
        return cont ? (run == 1 ? 120 : 37) + (odd ? 20 : 0) : 122 + (odd ? 23 : 0);
    case SOLID:
        return cont ? 21 : 137;
    default:
        return 224 + (cont ? 0 : 32);
    }
}
#define FRAME_CYCLES 600

/* Encode one frame. frame holds the previous decoded frame and is updated in
 * place, block by block, as the decoder will; cache likewise. Returns the
 * packet length, or -1 if the emitter and the decision model disagree. */
static long encode_frame(const u16 *src, u16 *frame, double lambda, double mu, u8 *packet,
                         double *cycles)
{
    double spent = FRAME_CYCLES;
    STATE Choice choice[BLOCKS];
    STATE Cache start;
    int b, last_mode = -1, last_param = -1, run = 0;
    u8 *p = packet;
    copy(&start, &cache, sizeof(cache));
    run_start = 0;
    for (b = 0; b < BLOCKS; ++b) {
        int x = (b % BLOCKS_X) * 4, y = (b / BLOCKS_X) * 4, dx, dy, cont, i;
        u16 px[16], ends[2];
        u8 index[16];
        double best, cost;
        Choice *c = &choice[b];
        load_block(src, x, y, px);
        cont = run > 0 && run < MAXRUN;
        if (!cont)
            run_start = b;
            /* Op byte bits: a block continuing the previous op shares its byte. */
#define CONTINUES(m, param)                                                                        \
    (cont && last_mode == (m) && (last_param == (param) || (m) == GRAD) &&                         \
     ((m) != MOTION || run < MOTION_RUN))
#define OP(m, param)  (CONTINUES(m, param) ? 1 : 8 + ((m) == MOTION ? 8 : 0))
#define CYC(m, param) (mu * block_cycles(m, CONTINUES(m, param), run, param))
        best = block_dist(px, frame, x, y) + lambda * OP(SKIP, 0) + CYC(SKIP, 0);
        c->mode = SKIP;
        c->param = 0;
        for (dy = -RANGE; dy < RANGE; ++dy)
            for (dx = -RANGE; dx < RANGE; ++dx) {
                int v = ((dx + 8) << 4) | (dy + 8);
                double rate;
                if ((!dx && !dy) || x + dx < 0 || y + dy < 0 || x + dx > W - 4 || y + dy > H - 4)
                    continue;
                rate = lambda * OP(MOTION, v) + CYC(MOTION, v);
                if (rate >= best)
                    continue;
                if (CONTINUES(MOTION, v)) {
                    /* Inside the run the source is the frame before the run. */
                    u16 s[16];
                    long d = 0;
                    load_source(frame, x + dx, y + dy, b, s);
                    for (i = 0; i < 16; ++i)
                        d += dist(px[i], s[i]);
                    cost = d + rate;
                } else
                    cost = block_dist_limit(px, frame, x + dx, y + dy, best - rate) + rate;
                if (cost < best) {
                    best = cost;
                    c->mode = MOTION;
                    c->param = v;
                }
            }
        /* SOLID: the block mean, cached colors near it, or the previous run's color */
        {
            u16 cand[SNAP + 2];
            int n;
            kmeans(px, 1, cand, index);
            n = 1 + nearest(&cache, cand[0], cand + 1, SNAP);
            if (last_mode == SOLID)
                cand[n++] = (u16)last_param;
            for (i = 0; i < n; ++i) {
                long d = 0;
                int k;
                for (k = 0; k < 16; ++k)
                    d += dist(px[k], cand[i]);
                cost =
                    d +
                    lambda * (OP(SOLID, cand[i]) +
                              (CONTINUES(SOLID, cand[i]) ? 0 : colour_bits(&cache, cand + i, 1))) +
                    CYC(SOLID, cand[i]);
                if (cost < best) {
                    best = cost;
                    c->mode = SOLID;
                    c->param = cand[i];
                }
            }
        }
        {
            long fitted = grad_fit(px, ends, index);
            cost = grad_rd(px, 16, lambda, ends, index, fitted) + lambda * (OP(GRAD, 0) + 32) +
                   CYC(GRAD, 0);
            if (cost < best) {
                best = cost;
                c->mode = GRAD;
                c->param = 0;
                copy(c->color, ends, 4);
                copy(c->index, index, 16);
            }
        }
#undef OP
        /* Model what the emitter will do with the cache. */
        if (c->mode == SOLID) {
            if (!CONTINUES(SOLID, c->param))
                cache_put(&cache, (u16)c->param, 0);
        } else
            for (i = 0; i < colour_count(c); ++i)
                cache_put(&cache, c->color[i], 0);
        spent += block_cycles(c->mode, CONTINUES(c->mode, c->param), run, c->param);
        if (CONTINUES(c->mode, c->param))
            ++run;
        else {
            run = 1;
            run_start = b;
        }
#undef CONTINUES
#undef CYC
        last_mode = c->mode;
        last_param = c->param;
        load_block(frame, x, y, saved[b - run_start]);
        reconstruct(c, frame, x, y, b);
    }
    /* Emit runs, replaying the cache from the start of the frame. */
    for (b = 0; b < BLOCKS;) {
        int n = 1, i, j;
        while (b + n < BLOCKS && n < (choice[b].mode == MOTION ? MOTION_RUN : MAXRUN) &&
               choice[b + n].mode == choice[b].mode &&
               (choice[b].mode == GRAD || choice[b + n].param == choice[b].param))
            ++n;
        *p++ = (u8)((choice[b].mode << 6) | (n - 1));
        if (choice[b].mode == MOTION)
            *p++ = (u8)choice[b].param;
        else if (choice[b].mode == SOLID)
            cache_put(&start, (u16)choice[b].param, &p);
        else if (choice[b].mode == GRAD)
            for (i = 0; i < n; ++i) {
                Choice *c = &choice[b + i];
                unsigned long bits = 0;
                for (j = 0; j < 2; ++j)
                    cache_put(&start, c->color[j], &p);
                for (j = 0; j < 16; ++j)
                    bits = (bits << 2) | c->index[j];
                *p++ = (u8)(bits >> 24);
                *p++ = (u8)(bits >> 16);
                *p++ = (u8)(bits >> 8);
                *p++ = (u8)bits;
            }
        b += n;
    }
    for (b = 0; b < CACHE; ++b)
        if (start.c[b] != cache.c[b])
            return -1;
    if (start.next != cache.next)
        return -1;
    *cycles = spent;
    return p - packet;
}

EXPORT unsigned int oh3do_preview_abi(void)
{
    return 2;
}

STATE double cycles_max;
/* Modelled decode clocks of the slowest frame in the last encode. */
EXPORT double oh3do_preview_max_cycles(void)
{
    return cycles_max;
}

/* Encode a frame within CYCLE_CAP. The previous frame's mu, scaled down, is
 * the first guess (busy frames come in runs); mu grows until the frame fits.
 * The result depends only on this call's input. */
static long encode_capped(const u16 *src, u16 *frame, double lambda, double *mu, u8 *packet,
                          double *cycles)
{
    STATE u16 frame0[PIXELS];
    STATE Cache cache0;
    STATE u8 slot0[32768];
    double guess = *mu < lambda / 256 ? 0 : *mu * 0.6;
    long n;
    copy(frame0, frame, sizeof(frame0));
    copy(&cache0, &cache, sizeof(cache));
    copy(slot0, slot, sizeof(slot));
    for (;;) {
        n = encode_frame(src, frame, lambda, guess, packet, cycles);
        if (n < 0 || *cycles <= CYCLE_CAP || guess > 1e9)
            break;
        copy(frame, frame0, sizeof(frame0));
        copy(&cache, &cache0, sizeof(cache));
        copy(slot, slot0, sizeof(slot));
        guess = guess > 0 ? guess * 1.6 : (*mu > 0 ? *mu : lambda / 64);
    }
    *mu = guess;
    return n;
}

/* frames: count frames of 160x120 RGB24, truncated to RGB555 as the console
 * shows them. The first `split` frames use lambda_first, the rest lambda.
 * out receives the .3dv video section: (count+1) big-endian offsets, then the
 * packets. decoded, if not null, receives every decoded frame
 * (count*38400 bytes). Returns the section length, -1 for bad arguments,
 * -2 if out is too small, -3 if a packet failed its reference-decoder check
 * (an encoder bug). */
EXPORT long oh3do_preview_encode(const unsigned char *frames, unsigned int count,
                                 double lambda_first, unsigned int split, double lambda,
                                 unsigned char *out, unsigned long capacity, unsigned char *decoded)
{
    STATE u16 source[PIXELS], frame[PIXELS];
    double cycles = 0, mu = 0;
    STATE u8 packet[MAX_PACKET], check[FRAME_BYTES];
    STATE Reference reference;
    unsigned long total;
    unsigned int f, i;
    if (!frames || !out || !count || count > MAX_FRAMES || !(lambda > 0) || !(lambda_first > 0))
        return -1;
    total = (count + 1) * 4UL;
    if (capacity < total)
        return -2;
    cycles_max = 0;
    zero(frame, sizeof(frame));
    zero(check, sizeof(check));
    zero(&cache, sizeof(cache));
    zero(slot, sizeof(slot));
    zero(&reference, sizeof(reference));
    for (f = 0; f < count; ++f) {
        const unsigned char *rgb = frames + (unsigned long)f * PIXELS * 3;
        long n;
        for (i = 0; i < PIXELS; ++i)
            source[i] = (u16)(((rgb[i * 3] >> 3) << 10) | ((rgb[i * 3 + 1] >> 3) << 5) |
                              (rgb[i * 3 + 2] >> 3));
        n = encode_capped(source, frame, f < split ? lambda_first : lambda, &mu, packet, &cycles);
        if (cycles > cycles_max)
            cycles_max = cycles;
        if (n < 0 || reference_frame(&reference, packet, n, check))
            return -3;
        for (i = 0; i < PIXELS; ++i)
            if (get(check, i % W, i / W) != frame[i])
                return -3;
        if (capacity - total < (unsigned long)n)
            return -2;
        out[f * 4] = (u8)(total >> 24);
        out[f * 4 + 1] = (u8)(total >> 16);
        out[f * 4 + 2] = (u8)(total >> 8);
        out[f * 4 + 3] = (u8)total;
        copy(out + total, packet, (int)n);
        total += (unsigned long)n;
        if (decoded)
            copy(decoded + (unsigned long)f * FRAME_BYTES, check, FRAME_BYTES);
    }
    out[count * 4] = (u8)(total >> 24);
    out[count * 4 + 1] = (u8)(total >> 16);
    out[count * 4 + 2] = (u8)(total >> 8);
    out[count * 4 + 3] = (u8)total;
    return (long)total;
}
