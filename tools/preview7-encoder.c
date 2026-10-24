/* Encoder for OM3DVID7 streamed previews (see src/preview_v7.h), 320x240 at
 * 30 fps. Original MIT-licensed code, C99.
 *
 * The caller provides all memory as one workspace (oh3do_preview7_workspace
 * says how much), so the Windows DLL needs no C runtime and the desktop app
 * can encode several clips at once on different threads.
 *
 * 1. Two dictionaries are trained on the clip: 4x4 blocks and 2x2 blocks,
 *    by splitting the largest cluster in two until there are enough, then a
 *    few rounds of k-means.
 * 2. Every block of every frame gets its nearest 4x4 entry and its four
 *    nearest 2x2 entries (equal-average search with partial distances).
 * 3. Frames are encoded chunk by chunk. Each block keeps its picture, takes
 *    one 4x4 entry or four 2x2 entries, whichever costs least in
 *    error + lambda * bytes + mu * decode clocks. A first use of an entry also
 *    pays for sending it. mu stays zero unless a frame would take the console
 *    too long to decode; a chunk that would not fit its size limit is
 *    encoded again with a larger lambda.
 * 4. lambda is searched so the file averages the requested bytes per second,
 *    and a model of the console's reader picks how many chunks to buffer
 *    before playing.
 * 5. The finished file is decoded by the menu's own C decoder, and every
 *    frame must match the encoder's model of it. */
#include <stddef.h>
#include <stdint.h>
/* The menu's decoder: built with src/ (or wherever preview_v7.c sits) on the
 * include path. */
#include "preview_v7.c"

#if defined(_WIN32) && defined(OH3DO_PREVIEW_DLL)
#define EXPORT __declspec(dllexport)
#elif defined(__GNUC__)
#define EXPORT __attribute__((visibility("default")))
#else
#define EXPORT
#endif

#if defined(_WIN32) && defined(OH3DO_PREVIEW_DLL)
/* No CRT: the compiler may still call these. */
int _fltused=0;
void *memcpy(void *d,const void *s,size_t n) { uint8_t *a=(uint8_t *)d; const uint8_t *b=(const uint8_t *)s; while (n--) *a++=*b++; return d; }
void *memset(void *d,int c,size_t n) { uint8_t *a=(uint8_t *)d; while (n--) *a++=(uint8_t)c; return d; }
#endif

#define W PREVIEW7_WIDTH
#define H PREVIEW7_HEIGHT
#define BLOCKS PREVIEW7_BLOCKS
#define COLS PREVIEW7_COLUMNS
#define DIM4 48
#define DIM2 12
#ifndef SAMPLES
#define SAMPLES 393216
#endif
#ifndef LLOYD_ROUNDS
#define LLOYD_ROUNDS 4
#endif
#define SCRATCH_FRAME (BLOCKS*8+64)
/* ARM60 clocks of preview_v7_arm.s decoding a frame into the full-screen
 * bitmap, fitted to tests/test_preview_v7.py --clocks counts (within 300
 * clocks a frame): per frame, per op and per block of a run. The browser's
 * TV picture costs less per block. */
#define CYC_FRAME 721
#define CYC_SKIP 49
#define CYC_ONE 79
#define CYC_RUN4 60
#define CYC_FOUR 44
#define CYC_RUNQ 56
#define CYC_QUAD 68

typedef struct Preview7Params {
    uint32_t rate;          /* bytes per second the file should average */
    uint32_t read_rate;     /* bytes per second the console's reader is assumed to manage */
    uint32_t slot_bytes;    /* memory the player is assumed to have for chunks */
    uint32_t max_chunk;     /* largest padded chunk, a multiple of 2048 */
    uint32_t chunk_frames;
    uint32_t entries4, entries2;
    uint32_t cycle_cap;     /* modelled decode clocks per frame */
} Preview7Params;

typedef struct Book {
    int dim, k;
    uint8_t *v;        /* k*dim values, x8 scale, sorted by sum */
    uint32_t *sum;
    uint16_t *id;      /* original entry of each sorted position */
    uint8_t *byid;     /* k*dim, x8 scale, by entry */
} Book;

typedef struct Snapshot {
    uint8_t type[BLOCKS];       /* 0 black, 1 4x4, 2 quad */
    uint16_t four[BLOCKS];
    uint16_t quad[BLOCKS][4];
    uint32_t next4, next2;
} Snapshot;

typedef struct Work {
    uint64_t bytes;
    uint32_t frames, added;
    uint8_t *src;               /* frames*BLOCKS*48, x8 scale */
    uint16_t *l4; uint32_t *d4; /* nearest 4x4 entry and its error */
    uint16_t *l2; uint32_t *dq; /* nearest 2x2 entries (4 per block) and their summed error */
    Book b4, b2;
    uint8_t *sample;            /* SAMPLES*48 */
    uint32_t *label;            /* SAMPLES */
    uint32_t *leaf;             /* 3*entries: start, end, unsplittable */
    uint64_t *leaf_sse;
    int64_t *acc;               /* entries*48 */
    uint32_t *count;
    Snapshot cur, chunk_start, frame_start; /* in the workspace: too big for a stack */
    uint16_t *slot4, *slot2;    /* entry -> slot number, 0xffff unsent */
    float *send_cost4, *send_cost2; /* bytes charged for sending an entry: its size spread over its likely uses */
    uint16_t *new4, *new2;      /* entries a chunk sends, in slot order */
    uint32_t new4n, new2n;
    uint8_t *scratch;           /* packets of the chunk being encoded */
    uint8_t *mode;              /* BLOCKS: decisions of the frame */
    uint8_t *expected;          /* 320x240 RGB555 per frame of a chunk, plus the one before: the model */
    unsigned char *dec_screen, *dec_thumb;
    unsigned char *dec_state;
    uint32_t *chunk_bytes;      /* padded size of each chunk */
    double mu;
    uint32_t max_cycles;
} Work;

static void *carve(uint8_t **p,uint64_t bytes)
{
    void *r=(void *)*p;
    *p+=(bytes+15)&~(uint64_t)15;
    return r;
}

static uint64_t layout(Work *w,uint8_t *base,uint32_t frames)
{
    uint8_t *p=base;
    uint64_t f=frames;
    w->src=carve(&p,f*BLOCKS*DIM4);
    w->l4=carve(&p,f*BLOCKS*2); w->d4=carve(&p,f*BLOCKS*4);
    w->l2=carve(&p,f*BLOCKS*8); w->dq=carve(&p,f*BLOCKS*4);
    w->b4.v=carve(&p,8192*DIM4); w->b4.sum=carve(&p,8192*4); w->b4.id=carve(&p,8192*2); w->b4.byid=carve(&p,8192*DIM4);
    w->b2.v=carve(&p,4096*DIM2); w->b2.sum=carve(&p,4096*4); w->b2.id=carve(&p,4096*2); w->b2.byid=carve(&p,4096*DIM2);
    w->sample=carve(&p,(uint64_t)SAMPLES*DIM4);
    w->label=carve(&p,(uint64_t)SAMPLES*4);
    w->leaf=carve(&p,8192*3*4); w->leaf_sse=carve(&p,8192*8);
    w->acc=carve(&p,8192*DIM4*8); w->count=carve(&p,8192*4);
    w->slot4=carve(&p,8192*2); w->slot2=carve(&p,4096*2);
    w->send_cost4=carve(&p,8192*4); w->send_cost2=carve(&p,4096*4);
    w->new4=carve(&p,8192*2); w->new2=carve(&p,4096*2);
    w->scratch=carve(&p,(uint64_t)PREVIEW7_MAX_CHUNK_FRAMES*SCRATCH_FRAME*2+4096);
    w->mode=carve(&p,BLOCKS);
    w->expected=carve(&p,(PREVIEW7_MAX_CHUNK_FRAMES+1)*PREVIEW7_FRAME_BYTES);
    w->dec_screen=carve(&p,PREVIEW7_FRAME_BYTES); w->dec_thumb=carve(&p,PREVIEW7_THUMB_BYTES);
    w->dec_state=carve(&p,preview7_state_bytes()+64);
    w->chunk_bytes=carve(&p,PREVIEW7_MAX_CHUNKS*4);
    return (uint64_t)(p-base)+sizeof(Work)+64;
}

EXPORT uint32_t oh3do_preview7_abi(void) { return 1; }

/* Workspace bytes for a clip of this many frames. */
EXPORT uint64_t oh3do_preview7_workspace(uint32_t frames)
{
    Work w;
    if (!frames || frames>PREVIEW7_MAX_FRAMES) return 0;
    return layout(&w,(uint8_t *)(uintptr_t)64,frames); /* only sizes matter here */
}

static Work *work_of(void *workspace) { return (Work *)(((uintptr_t)workspace+15)&~(uintptr_t)15); }

/* Start a clip of up to `frames` frames. 0, or -1 for bad arguments. */
EXPORT int32_t oh3do_preview7_begin(void *workspace,uint64_t bytes,uint32_t frames)
{
    Work *w;
    if (!workspace || !frames || frames>PREVIEW7_MAX_FRAMES || bytes<oh3do_preview7_workspace(frames)) return -1;
    w=work_of(workspace);
    w->bytes=bytes; w->frames=frames; w->added=0;
    layout(w,(uint8_t *)(w+1),frames);
    return 0;
}

/* Add the next frame: 320x240 RGB24. */
EXPORT int32_t oh3do_preview7_frame(void *workspace,const uint8_t *rgb)
{
    Work *w=work_of(workspace);
    uint8_t *out;
    int b,i;
    if (!rgb || w->added>=w->frames) return -1;
    out=w->src+(uint64_t)w->added*BLOCKS*DIM4;
    for (b=0;b<BLOCKS;++b) {
        int x0=(b%COLS)*4,y0=(b/COLS)*4;
        for (i=0;i<16;++i) {
            const uint8_t *p=rgb+((y0+i/4)*W+x0+i%4)*3;
            int c;
            /* 5-bit units with three fraction bits: 255 -> 248. */
            for (c=0;c<3;++c) *out++=(uint8_t)((p[c]*248+127)/255);
        }
    }
    ++w->added;
    return 0;
}

/* ---- Dictionaries ---- */

static void sub_vector(const uint8_t *block,int s,uint8_t *out)
{
    int sy=s>>1,sx=s&1,r,c;
    for (r=0;r<2;++r) for (c=0;c<2;++c) {
        const uint8_t *p=block+((sy*2+r)*4+sx*2+c)*3;
        out[(r*2+c)*3]=p[0]; out[(r*2+c)*3+1]=p[1]; out[(r*2+c)*3+2]=p[2];
    }
}

static uint32_t vsum(const uint8_t *v,int dim) { uint32_t s=0; int i; for (i=0;i<dim;++i) s+=v[i]; return s; }

static uint32_t dist(const uint8_t *a,const uint8_t *b,int dim,uint32_t limit)
{
    uint32_t d=0;
    int i;
    for (i=0;i<dim;i+=12) {
        int k;
        for (k=i;k<i+12;++k) { int e=(int)a[k]-(int)b[k]; d+=(uint32_t)(e*e); }
        if (d>=limit) return d;
    }
    return d;
}

static void swap_vec(uint8_t *a,uint8_t *b,int dim) { int i; for (i=0;i<dim;++i) { uint8_t t=a[i]; a[i]=b[i]; b[i]=t; } }

/* Sort the book by entry sum (insertion into a counting order). */
static void book_index(Book *b,uint32_t *scratch)
{
    int i,max=b->dim*248+1;
    uint32_t *start=scratch;
    for (i=0;i<=max;++i) start[i]=0;
    for (i=0;i<b->k;++i) ++start[vsum(b->byid+i*b->dim,b->dim)+1];
    for (i=1;i<=max;++i) start[i]+=start[i-1];
    for (i=0;i<b->k;++i) {
        uint32_t s=vsum(b->byid+i*b->dim,b->dim),at=start[s]++;
        int k;
        b->sum[at]=s; b->id[at]=(uint16_t)i;
        for (k=0;k<b->dim;++k) b->v[at*b->dim+k]=b->byid[i*b->dim+k];
    }
}

/* Nearest entry: equal-average search outward from the query's sum. */
static uint32_t nearest(const Book *b,const uint8_t *q,uint32_t *best_d)
{
    uint32_t s=vsum(q,b->dim),best=0xffffffffu,who=0;
    int lo=0,hi=b->k,up,down;
    while (lo<hi) { int mid=(lo+hi)/2; if (b->sum[mid]<s) lo=mid+1; else hi=mid; }
    up=lo; down=lo-1;
    while (up<b->k || down>=0) {
        if (up<b->k) {
            uint32_t e=b->sum[up]-s;
            if ((uint64_t)e*e>=(uint64_t)best*b->dim) up=b->k;
            else {
                uint32_t d=dist(q,b->v+(size_t)up*b->dim,b->dim,best);
                if (d<best) { best=d; who=up; }
                ++up;
            }
        }
        if (down>=0) {
            uint32_t e=s-b->sum[down];
            if ((uint64_t)e*e>=(uint64_t)best*b->dim) down=-1;
            else {
                uint32_t d=dist(q,b->v+(size_t)down*b->dim,b->dim,best);
                if (d<best) { best=d; who=down; }
                --down;
            }
        }
    }
    *best_d=best;
    return b->id[who];
}

static uint64_t range_sse(const uint8_t *v,uint32_t a,uint32_t e,int dim,int64_t *mean_out)
{
    int64_t sum[DIM4]={0};
    uint64_t sse=0;
    uint32_t n=e-a,i;
    int k;
    for (i=a;i<e;++i) for (k=0;k<dim;++k) sum[k]+=v[(size_t)i*dim+k];
    for (k=0;k<dim;++k) mean_out[k]=(sum[k]+n/2)/n;
    for (i=a;i<e;++i) for (k=0;k<dim;++k) { int64_t d=v[(size_t)i*dim+k]-mean_out[k]; sse+=(uint64_t)(d*d); }
    return sse;
}

/* Split leaf range [a,e) in two; returns the split point, or a for none. */
static uint32_t split(uint8_t *v,uint32_t a,uint32_t e,int dim)
{
    int64_t mean[DIM4],var[DIM4]={0},ca[DIM4],cb[DIM4];
    uint32_t i,m=a;
    int k,axis=0,round;
    range_sse(v,a,e,dim,mean);
    for (i=a;i<e;++i) for (k=0;k<dim;++k) { int64_t d=v[(size_t)i*dim+k]-mean[k]; var[k]+=d*d; }
    for (k=1;k<dim;++k) if (var[k]>var[axis]) axis=k;
    if (!var[axis]) return a;
    /* First cut at the mean of the widest axis, then two-means rounds. */
    for (i=a,m=a;i<e;++i) if (v[(size_t)i*dim+axis]<=mean[axis]) swap_vec(v+(size_t)i*dim,v+(size_t)m++*dim,dim);
    for (round=0;round<4;++round) {
        int64_t dot_mid=0,dir[DIM4];
        uint32_t next;
        if (m==a || m==e) break;
        range_sse(v,a,m,dim,ca); range_sse(v,m,e,dim,cb);
        for (k=0;k<dim;++k) { dir[k]=cb[k]-ca[k]; dot_mid+=dir[k]*(ca[k]+cb[k]); }
        for (i=a,next=a;i<e;++i) {
            int64_t dot=0;
            for (k=0;k<dim;++k) dot+=dir[k]*v[(size_t)i*dim+k];
            if (2*dot<=dot_mid) swap_vec(v+(size_t)i*dim,v+(size_t)next++*dim,dim);
        }
        if (next==m) break;
        m=next;
    }
    if (m==a || m==e) m=a+(e-a)/2;
    return m;
}

static void train(Work *w,Book *b,int dim,int k,uint32_t samples)
{
    uint8_t *v=w->sample;
    uint32_t leaves=1,i,round;
    int64_t mean[DIM4];
    w->leaf[0]=0; w->leaf[1]=samples; w->leaf[2]=0;
    w->leaf_sse[0]=range_sse(v,0,samples,dim,mean);
    while (leaves<(uint32_t)k) {
        uint32_t best=0,j,m,a,e;
        uint64_t top=0;
        for (j=0;j<leaves;++j) if (!w->leaf[3*j+2] && w->leaf_sse[j]>top) { top=w->leaf_sse[j]; best=j; }
        if (!top) break;
        a=w->leaf[3*best]; e=w->leaf[3*best+1];
        m=split(v,a,e,dim);
        if (m==a) { w->leaf[3*best+2]=1; continue; }
        w->leaf[3*best+1]=m; w->leaf_sse[best]=range_sse(v,a,m,dim,mean);
        w->leaf[3*leaves]=m; w->leaf[3*leaves+1]=e; w->leaf[3*leaves+2]=0;
        w->leaf_sse[leaves]=range_sse(v,m,e,dim,mean);
        ++leaves;
    }
    b->dim=dim; b->k=(int)leaves;
    for (i=0;i<leaves;++i) {
        int c;
        range_sse(v,w->leaf[3*i],w->leaf[3*i+1],dim,mean);
        for (c=0;c<dim;++c) b->byid[i*dim+c]=(uint8_t)mean[c];
    }
    for (round=0;round<=LLOYD_ROUNDS;++round) {
        uint32_t worst=0,worst_d=0;
        int c;
        book_index(b,(uint32_t *)w->acc);
        if (round==LLOYD_ROUNDS) break;
        for (i=0;i<leaves*(uint32_t)dim;++i) w->acc[i]=0;
        for (i=0;i<leaves;++i) w->count[i]=0;
        for (i=0;i<samples;++i) {
            uint32_t d,who=nearest(b,v+(size_t)i*dim,&d);
            w->label[i]=who; ++w->count[who];
            for (c=0;c<dim;++c) w->acc[who*dim+c]+=v[(size_t)i*dim+c];
            if (d>worst_d) { worst_d=d; worst=i; }
        }
        for (i=0;i<leaves;++i) {
            if (w->count[i]) for (c=0;c<dim;++c) b->byid[i*dim+c]=(uint8_t)((w->acc[i*dim+c]+w->count[i]/2)/w->count[i]);
            else { for (c=0;c<dim;++c) b->byid[i*dim+c]=v[(size_t)worst*dim+c]; worst_d=0; }
        }
    }
    /* What the console shows: whole 5-bit values. */
    for (i=0;i<leaves*(uint32_t)dim;++i) b->byid[i]=(uint8_t)(((b->byid[i]+4)>>3)<<3);
    book_index(b,(uint32_t *)w->acc);
}

/* A deterministic pseudo-random pick, so samples cannot alias with block
 * positions the way a fixed stride can. */
static uint64_t pick(uint64_t *seed,uint64_t n)
{
    *seed=*seed*6364136223846793005ULL+1442695040888963407ULL;
    return (*seed>>33)%n;
}

static void dictionaries(Work *w,const Preview7Params *p)
{
    uint64_t total=(uint64_t)w->frames*BLOCKS,seed=1,n,count;
    count=total<SAMPLES ? total : SAMPLES;
    for (n=0;n<count;++n) {
        uint64_t at=count==total ? n : pick(&seed,total);
        for (int c=0;c<DIM4;++c) w->sample[n*DIM4+c]=w->src[at*DIM4+c];
    }
    train(w,&w->b4,DIM4,(int)p->entries4,(uint32_t)count);
    count=4*total<SAMPLES ? 4*total : SAMPLES;
    for (n=0;n<count;++n) {
        uint64_t at=count==4*total ? n : pick(&seed,4*total);
        sub_vector(w->src+(at/4)*DIM4,(int)(at%4),w->sample+n*DIM2);
    }
    train(w,&w->b2,DIM2,(int)p->entries2,(uint32_t)count);
}

static int memcmp_(const uint8_t *a,const uint8_t *b)
{
    int i;
    for (i=0;i<DIM4;++i) if (a[i]!=b[i]) return 1;
    return 0;
}

static void assign(Work *w)
{
    uint64_t f,b;
    for (f=0;f<w->frames;++f) for (b=0;b<BLOCKS;++b) {
        uint64_t at=f*BLOCKS+b;
        const uint8_t *q=w->src+at*DIM4;
        int s;
        if (f && !memcmp_(q,q-(uint64_t)BLOCKS*DIM4)) {
            w->l4[at]=w->l4[at-BLOCKS]; w->d4[at]=w->d4[at-BLOCKS];
            for (s=0;s<4;++s) w->l2[at*4+s]=w->l2[(at-BLOCKS)*4+s];
            w->dq[at]=w->dq[at-BLOCKS];
            continue;
        }
        w->l4[at]=(uint16_t)nearest(&w->b4,q,&w->d4[at]);
        w->dq[at]=0;
        for (s=0;s<4;++s) {
            uint8_t v[DIM2];
            uint32_t d;
            sub_vector(q,s,v);
            w->l2[at*4+s]=(uint16_t)nearest(&w->b2,v,&d);
            w->dq[at]+=d;
        }
    }
}

/* ---- Encoding ---- */

static void put16(uint8_t *p,uint32_t v) { p[0]=(uint8_t)(v>>8); p[1]=(uint8_t)v; }
static void put32_(uint8_t *p,uint32_t v) { p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v; }
static void copy_bytes(void *d,const void *s,size_t n) { uint8_t *a=(uint8_t *)d; const uint8_t *b=(const uint8_t *)s; while (n--) *a++=*b++; }

/* The block's current picture, x8 scale. */
static void recon(const Work *w,const Snapshot *c,int b,uint8_t *out)
{
    int i,s;
    if (c->type[b]==0) { for (i=0;i<DIM4;++i) out[i]=0; return; }
    if (c->type[b]==1) { copy_bytes(out,w->b4.byid+(size_t)c->four[b]*DIM4,DIM4); return; }
    for (s=0;s<4;++s) {
        const uint8_t *e=w->b2.byid+(size_t)c->quad[b][s]*DIM2;
        int sy=s>>1,sx=s&1,r,x;
        for (r=0;r<2;++r) for (x=0;x<2;++x) for (i=0;i<3;++i) out[((sy*2+r)*4+sx*2+x)*3+i]=e[(r*2+x)*3+i];
    }
}

static void paint(const Work *w,const Snapshot *c,int b,uint8_t *frame)
{
    uint8_t v[DIM4];
    int i,x0=(b%COLS)*4,y0=(b/COLS)*4;
    recon(w,c,b,v);
    for (i=0;i<16;++i) {
        uint32_t px=((uint32_t)(v[i*3]>>3)<<10)|((uint32_t)(v[i*3+1]>>3)<<5)|(v[i*3+2]>>3);
        put16(frame+((y0+i/4)*W+x0+i%4)*2,px);
    }
}

static int send4(Work *w,uint32_t e)
{
    if (w->slot4[e]!=0xffff) return 0;
    w->slot4[e]=(uint16_t)w->cur.next4++; w->new4[w->new4n++]=(uint16_t)e;
    return 1;
}
static int send2(Work *w,uint32_t e)
{
    if (w->slot2[e]!=0xffff) return 0;
    w->slot2[e]=(uint16_t)w->cur.next2++; w->new2[w->new2n++]=(uint16_t)e;
    return 1;
}

/* Decide one frame; returns its modelled clocks. mode[] gets 0 keep, 1 4x4, 2 quad. */
static uint32_t decide(Work *w,uint32_t f,double lambda,double mu)
{
    const uint8_t *src=w->src+(uint64_t)f*BLOCKS*DIM4;
    uint64_t at=(uint64_t)f*BLOCKS;
    uint32_t cycles=CYC_FRAME;
    int b,last=-1;
    for (b=0;b<BLOCKS;++b) {
        uint8_t v[DIM4];
        uint32_t ek,e4=w->l4[at+b];
        const uint16_t *q=w->l2+(at+b)*4;
        double best,cost;
        int choice=0,s;
        recon(w,&w->cur,b,v);
        ek=dist(src+(size_t)b*DIM4,v,DIM4,0xffffffffu);
        best=ek;
        if (!(w->cur.type[b]==1 && w->cur.four[b]==e4)) {
            cost=w->d4[at+b]+lambda*(2+(w->slot4[e4]==0xffff ? w->send_cost4[e4] : 0))+mu*CYC_FOUR;
            if (cost<best) { best=cost; choice=1; }
        }
        if (!(w->cur.type[b]==2 && w->cur.quad[b][0]==q[0] && w->cur.quad[b][1]==q[1] && w->cur.quad[b][2]==q[2] && w->cur.quad[b][3]==q[3])) {
            float fresh=0;
            for (s=0;s<4;++s) {
                int k,dup=0;
                for (k=0;k<s;++k) if (q[k]==q[s]) dup=1;
                if (!dup && w->slot2[q[s]]==0xffff) fresh+=w->send_cost2[q[s]];
            }
            cost=w->dq[at+b]+lambda*(6+fresh)+mu*CYC_QUAD;
            if (cost<best) { best=cost; choice=2; }
        }
        w->mode[b]=(uint8_t)choice;
        if (choice==1) { w->cur.type[b]=1; w->cur.four[b]=(uint16_t)e4; send4(w,e4); cycles+=CYC_FOUR; }
        else if (choice==2) {
            w->cur.type[b]=2;
            for (s=0;s<4;++s) { w->cur.quad[b][s]=q[s]; send2(w,q[s]); }
            cycles+=CYC_QUAD;
        }
        if (choice!=last) cycles+=choice==1 ? CYC_RUN4 : choice==2 ? CYC_RUNQ : CYC_SKIP;
        last=choice;
    }
    return cycles;
}

/* Exact modelled clocks of the frame's packet as emit writes it. */
static uint32_t frame_cycles(const Work *w)
{
    uint32_t cycles=CYC_FRAME;
    int b=0;
    while (b<BLOCKS) {
        int m=w->mode[b],n=1;
        while (b+n<BLOCKS && w->mode[b+n]==m && n<8192) ++n;
        if (m==0) cycles+=CYC_SKIP;
        else if (m==1) cycles+=n==1 ? CYC_ONE : CYC_RUN4+n*CYC_FOUR;
        else cycles+=CYC_RUNQ+n*CYC_QUAD;
        b+=n;
    }
    return cycles;
}

/* The frame's packet; returns its length. */
static uint32_t emit(const Work *w,uint8_t *out)
{
    uint8_t *p=out;
    int b=0;
    while (b<BLOCKS) {
        int m=w->mode[b],n=1,i;
        while (b+n<BLOCKS && w->mode[b+n]==m && n<8192) ++n;
        if (m==0) { put16(p,0x2000|(n-1)); p+=2; }
        else if (m==1) {
            if (n>1) { put16(p,0x4000|(n-1)); p+=2; }
            for (i=0;i<n;++i) { put16(p,w->slot4[w->cur.four[b+i]]); p+=2; }
        } else {
            put16(p,0x6000|(n-1)); p+=2;
            for (i=0;i<n;++i) {
                const uint16_t *q=w->cur.quad[b+i];
                uint32_t hi=((uint32_t)w->slot2[q[0]]<<12)|w->slot2[q[1]],lo=((uint32_t)w->slot2[q[2]]<<12)|w->slot2[q[3]];
                p[0]=(uint8_t)(hi>>16); p[1]=(uint8_t)(hi>>8); p[2]=(uint8_t)hi;
                p[3]=(uint8_t)(lo>>16); p[4]=(uint8_t)(lo>>8); p[5]=(uint8_t)lo;
                p+=6;
            }
        }
        b+=n;
    }
    return (uint32_t)(p-out);
}

/* How often each entry becomes a block's nearest one (counting a block only
 * when its nearest entry changes) decides how much of its size a first use
 * pays: entries the clip will use again and again are cheap to send. */
#ifndef USES_PER_SEND
#define USES_PER_SEND 1.0f
#endif
static void send_costs(Work *w)
{
    uint64_t f,b;
    int i,s;
    for (i=0;i<8192;++i) w->send_cost4[i]=0;
    for (i=0;i<4096;++i) w->send_cost2[i]=0;
    for (f=0;f<w->frames;++f) for (b=0;b<BLOCKS;++b) {
        uint64_t at=f*BLOCKS+b;
        if (!f || w->l4[at]!=w->l4[at-BLOCKS]) w->send_cost4[w->l4[at]]+=1;
        for (s=0;s<4;++s) if (!f || w->l2[at*4+s]!=w->l2[(at-BLOCKS)*4+s]) w->send_cost2[w->l2[at*4+s]]+=1;
    }
    for (i=0;i<8192;++i) { float u=w->send_cost4[i]*USES_PER_SEND; w->send_cost4[i]=32/(u>1 ? u : 1); }
    for (i=0;i<4096;++i) { float u=w->send_cost2[i]*USES_PER_SEND; w->send_cost2[i]=8/(u>1 ? u : 1); }
}

static uint64_t samples_at(uint64_t f) { return f*PREVIEW7_AUDIO_RATE/PREVIEW7_FPS; }
static double sqrt_(double x) { double r=x>1 ? x : 1; int i; for (i=0;i<60;++i) r=(r+x/r)/2; return r; }

typedef struct Pass {
    const Preview7Params *p;
    const int8_t *audio;      /* NULL: silent */
    uint8_t *out;             /* NULL: only measure */
    uint64_t capacity, used;
    int verify, full;
    Preview7 dec;
    Preview7Header header;
    uint8_t table[2*PREVIEW7_MAX_CHUNKS];
} Pass;

/* Encode chunk c at lambda; returns its padded size, or 0 if it cannot fit. */
static uint32_t chunk(Work *w,Pass *ps,uint32_t c,double lambda)
{
    const Preview7Params *p=ps->p;
    uint32_t first=c*p->chunk_frames,frames=w->frames-first<p->chunk_frames ? w->frames-first : p->chunk_frames;
    uint32_t audio=ps->audio ? (uint32_t)(samples_at(first+frames)-samples_at(first)) : 0;
    uint32_t tries,k,f;
    for (tries=0;tries<40;++tries,lambda*=1.4) {
        uint32_t packets=0,at,size,offsets[PREVIEW7_MAX_CHUNK_FRAMES+1];
        uint32_t n4,n2;
        copy_bytes(&w->chunk_start,&w->cur,sizeof(Snapshot));
        w->new4n=w->new2n=0;
        for (k=0;k<frames;++k) {
            uint32_t save4,save2,cycles;
            double mu=w->mu<lambda/4096 ? 0 : w->mu*0.6;
            f=first+k;
            copy_bytes(&w->frame_start,&w->cur,sizeof(Snapshot));
            save4=w->new4n; save2=w->new2n;
            for (;;) {
                decide(w,f,lambda,mu);
                cycles=frame_cycles(w);
                if (cycles<=p->cycle_cap || mu>1e12) break;
                /* Undo the frame and price decode clocks higher. */
                while (w->new4n>save4) w->slot4[w->new4[--w->new4n]]=0xffff;
                while (w->new2n>save2) w->slot2[w->new2[--w->new2n]]=0xffff;
                copy_bytes(&w->cur,&w->frame_start,sizeof(Snapshot));
                mu=mu>0 ? mu*1.6 : lambda/16;
            }
            w->mu=mu>1e12 ? 0 : mu;
            if (cycles>w->max_cycles) w->max_cycles=cycles;
            offsets[k]=packets;
            packets+=emit(w,w->scratch+packets);
            if (ps->verify) {
                uint8_t *was=w->expected+(size_t)k*PREVIEW7_FRAME_BYTES,*now=was+PREVIEW7_FRAME_BYTES;
                int b;
                copy_bytes(now,was,PREVIEW7_FRAME_BYTES);
                for (b=0;b<BLOCKS;++b) if (w->mode[b]) paint(w,&w->cur,b,now);
            }
        }
        offsets[frames]=packets;
        n4=w->new4n; n2=w->new2n;
        size=PREVIEW7_CHUNK_HEADER+((audio+3)&~3u)+n4*PREVIEW7_ENTRY4+n2*PREVIEW7_ENTRY2+(frames+1)*4+packets;
        size=(size+PREVIEW7_BLOCK_BYTES-1)/PREVIEW7_BLOCK_BYTES*PREVIEW7_BLOCK_BYTES;
        if (size>p->max_chunk) {
            /* Too big: forget this chunk's entries and decisions, raise lambda. */
            while (w->new4n) w->slot4[w->new4[--w->new4n]]=0xffff;
            while (w->new2n) w->slot2[w->new2[--w->new2n]]=0xffff;
            copy_bytes(&w->cur,&w->chunk_start,sizeof(Snapshot));
            continue;
        }
        if (ps->out) {
            uint8_t *o;
            uint32_t i;
            if (ps->capacity-ps->used<size) { ps->full=1; return 0; }
            o=ps->out+ps->used;
            for (i=0;i<size;++i) o[i]=0;
            put16(o+4,c); put16(o+6,frames);
            put16(o+8,w->chunk_start.next4); put16(o+10,n4); put16(o+12,w->chunk_start.next2); put16(o+14,n2);
            put32_(o+16,audio);
            at=PREVIEW7_CHUNK_HEADER;
            for (i=0;i<audio;++i) o[at+i]=(uint8_t)ps->audio[samples_at(first)+i];
            at+=(audio+3)&~3u;
            for (i=0;i<n4;++i,at+=PREVIEW7_ENTRY4) {
                const uint8_t *v=w->b4.byid+(size_t)w->new4[i]*DIM4;
                uint8_t rows[32];
                int x;
                for (x=0;x<16;++x) put16(rows+x*2,((uint32_t)(v[x*3]>>3)<<10)|((uint32_t)(v[x*3+1]>>3)<<5)|(v[x*3+2]>>3));
                preview7_entry4(rows,o+at);
            }
            for (i=0;i<n2;++i,at+=PREVIEW7_ENTRY2) {
                const uint8_t *v=w->b2.byid+(size_t)w->new2[i]*DIM2;
                uint8_t rows[8];
                int x;
                for (x=0;x<4;++x) put16(rows+x*2,((uint32_t)(v[x*3]>>3)<<10)|((uint32_t)(v[x*3+1]>>3)<<5)|(v[x*3+2]>>3));
                preview7_entry2(rows,o+at);
            }
            put32_(o+20,(frames+1)*4+packets);
            for (i=0;i<=frames;++i) put32_(o+at+i*4,(frames+1)*4+offsets[i]);
            copy_bytes(o+at+(frames+1)*4,w->scratch,packets);
            put32_(o,(uint32_t)preview7_sum(o+4,size-4));
            put16(ps->table+2*c,size/PREVIEW7_BLOCK_BYTES);
            if (ps->verify) {
                Preview7Chunk info;
                if (preview7_chunk(&ps->header,c,o,size,&info)) return 0;
                preview7_define(&ps->dec,&info);
                for (k=0;k<frames;++k) {
                    uint32_t i2;
                    const uint8_t *want=w->expected+(size_t)(k+1)*PREVIEW7_FRAME_BYTES;
                    if (preview7_frame(&ps->dec,&info,k,PREVIEW7_SCREEN)) return 0;
                    /* The screen holds two lines per word, the upper in the high half. */
                    for (i2=0;i2<PREVIEW7_FRAME_BYTES;i2+=2) {
                        uint32_t x=(i2/2)%W,y=(i2/2)/W,at=((y>>1)*W+x)*4+(y&1)*2;
                        if (w->dec_screen[at]!=want[i2] || w->dec_screen[at+1]!=want[i2+1]) return 0;
                    }
                }
                copy_bytes(w->expected,w->expected+(size_t)frames*PREVIEW7_FRAME_BYTES,PREVIEW7_FRAME_BYTES);
            }
            ps->used+=size;
        }
        return size;
    }
    return 0;
}

/* Whole clip at lambda; fills chunk_bytes; returns total bytes, 0 on failure. */
static uint64_t pass(Work *w,Pass *ps,double lambda)
{
    uint32_t chunks=(w->frames+ps->p->chunk_frames-1)/ps->p->chunk_frames,c,b;
    uint64_t total=PREVIEW7_BLOCK_BYTES;
    for (b=0;b<BLOCKS;++b) w->cur.type[b]=0;
    w->cur.next4=w->cur.next2=0;
    for (b=0;b<8192;++b) w->slot4[b]=0xffff;
    for (b=0;b<4096;++b) w->slot2[b]=0xffff;
    w->mu=0; w->max_cycles=0;
    for (c=0;c<chunks;++c) {
        uint32_t n=chunk(w,ps,c,lambda);
        if (!n) return 0;
        w->chunk_bytes[c]=n; total+=n;
    }
    return total;
}

/* The fewest chunks to buffer before playing so that a reader of read_rate
 * bytes/s with `slots` chunk buffers stays ahead of playback for two loops;
 * 0 if none works. A slot frees when its chunk has finished playing. */
static uint32_t preroll(const Work *w,const Preview7Params *p,uint32_t chunks,uint32_t slots)
{
    const double t=(double)p->chunk_frames/PREVIEW7_FPS,margin=0.1;
    uint32_t pre;
    for (pre=1;pre<=slots && pre<=chunks;++pre) {
        double reader=(double)PREVIEW7_BLOCK_BYTES/p->read_rate,start=0;
        uint32_t k;
        int ok=1;
        for (k=0;k<2*chunks && ok;++k) {
            if (k>=slots) {
                double free_at=start+(double)(k-slots+1)*t;
                if (free_at>reader) reader=free_at;
            }
            reader+=(double)w->chunk_bytes[k%chunks]/p->read_rate;
            if (k+1==pre) start=reader;
            if (k>=pre && reader>start+(double)k*t-margin) ok=0;
        }
        if (ok) return pre;
    }
    return 0;
}

static void header_block(uint8_t *o,const Preview7Params *p,const Work *w,uint32_t chunks,uint32_t pre,uint32_t slots,uint32_t audio_rate,uint32_t max_chunk,const uint8_t *table)
{
    uint32_t i,a=0,b=0;
    for (i=0;i<PREVIEW7_BLOCK_BYTES;++i) o[i]=0;
    copy_bytes(o,"OM3DVID7",8);
    put16(o+8,W); put16(o+10,H); put16(o+12,PREVIEW7_FPS); put16(o+14,p->chunk_frames);
    put32_(o+16,w->frames); put16(o+20,chunks); put16(o+22,pre);
    put16(o+24,w->cur.next4); put16(o+26,w->cur.next2);
    put32_(o+28,audio_rate); put32_(o+32,max_chunk); put32_(o+36,p->rate);
    put16(o+44,slots);
    copy_bytes(o+PREVIEW7_HEADER_BYTES,table,2*chunks);
    for (i=0;i<PREVIEW7_BLOCK_BYTES;i+=4) {
        a+=((uint32_t)o[i]<<24)|((uint32_t)o[i+1]<<16)|((uint32_t)o[i+2]<<8)|o[i+3];
        b+=a;
    }
    put32_(o+40,a^((b<<16)|(b>>16)));
}

/* Encode the frames added since begin. audio: signed 8-bit 11025 Hz samples
 * for those frames (frames*11025/30, rounded down), or NULL. stats, if not
 * NULL, receives 8 words: lambda*1000, slowest frame's modelled clocks,
 * 4x4 entries, 2x2 entries, preroll, slots, largest chunk, chunks.
 * Returns the file length, -1 for bad arguments, -2 if out is too small,
 * -3 if the decoder disagrees with the encoder (a bug), -4 if no setting
 * meets the rate and the reader model. */
EXPORT int64_t oh3do_preview7_encode(void *workspace,const Preview7Params *p,const int8_t *audio,uint64_t audio_samples,
                                     uint8_t *out,uint64_t capacity,uint32_t *stats)
{
    Work *w=work_of(workspace);
    Pass ps;
    uint32_t chunks,slots,pre,c,max_chunk=0;
    uint64_t target,total;
    double lo=1,hi=1e7,lambda;
    int i;
    if (!w->added) return -1;
    w->frames=w->added; /* the buffers were laid out for at least this many */
    if (!p || !p->chunk_frames || p->chunk_frames>PREVIEW7_MAX_CHUNK_FRAMES || !p->rate || !p->read_rate ||
        !p->entries4 || p->entries4>8192 || !p->entries2 || p->entries2>4096 || !p->max_chunk || p->max_chunk%PREVIEW7_BLOCK_BYTES ||
        p->max_chunk>65535UL*PREVIEW7_BLOCK_BYTES || !p->cycle_cap || (audio && audio_samples!=samples_at(w->frames)) || !out) return -1;
    chunks=(w->frames+p->chunk_frames-1)/p->chunk_frames;
    if (chunks>PREVIEW7_MAX_CHUNKS) return -1;
    slots=p->slot_bytes/p->max_chunk;
    if (slots<1) return -1;
    if (slots>chunks) slots=chunks;
    dictionaries(w,p);
    assign(w);
    send_costs(w);
    for (i=0;i<(int)sizeof(ps);++i) ((uint8_t *)&ps)[i]=0;
    ps.p=p; ps.audio=audio;
    target=(uint64_t)p->rate*w->frames/PREVIEW7_FPS;
    /* The smallest lambda (best picture) that meets the rate and the reader. */
    total=pass(w,&ps,hi);
    if (!total || total>target || !preroll(w,p,chunks,slots)) return -4;
    for (i=0;i<22 && hi/lo>1.02;++i) {
        double mid=sqrt_(lo*hi);
        total=pass(w,&ps,mid);
        if (total && total<=target && preroll(w,p,chunks,slots)) hi=mid; else lo=mid;
    }
    lambda=hi;
    /* Final pass: write, decode and compare. */
    {
        uint8_t *m=w->dec_state;
        ps.dec.map=(unsigned long *)(void *)m; m+=PREVIEW7_BLOCKS*2*sizeof(unsigned long);
        ps.dec.dict4=m; m+=8192*32; ps.dec.thumb4=m; m+=8192*8;
        ps.dec.dict2=m; m+=4096*8; ps.dec.thumb2=(unsigned long *)(void *)m;
        ps.dec.entries4=8192; ps.dec.entries2=4096;
        ps.dec.thumb=w->dec_thumb; ps.dec.screen=w->dec_screen;
        for (i=0;i<(int)PREVIEW7_FRAME_BYTES;++i) w->dec_screen[i]=0;
        preview7_reset(&ps.dec);
    }
    ps.header.chunk_frames=p->chunk_frames; ps.header.chunks=chunks; ps.header.frames=w->frames;
    ps.header.entries4=8192; ps.header.entries2=4096; ps.header.audio_rate=audio ? PREVIEW7_AUDIO_RATE : 0;
    ps.header.max_chunk=p->max_chunk; ps.header.table=ps.table; ps.header.preroll=1; ps.header.slots=slots;
    for (i=0;i<(int)PREVIEW7_FRAME_BYTES;++i) w->expected[i]=0;
    if (capacity<PREVIEW7_BLOCK_BYTES) return -2;
    ps.out=out; ps.capacity=capacity; ps.used=PREVIEW7_BLOCK_BYTES; ps.verify=1;
    total=pass(w,&ps,lambda);
    if (!total) return ps.full ? -2 : -3;
    for (c=0;c<chunks;++c) if (w->chunk_bytes[c]>max_chunk) max_chunk=w->chunk_bytes[c];
    pre=preroll(w,p,chunks,slots);
    if (!pre) return -4;
    header_block(out,p,w,chunks,pre,slots,audio ? PREVIEW7_AUDIO_RATE : 0,max_chunk,ps.table);
    {
        Preview7Header check;
        if (preview7_header(out,&check)) return -3;
    }
    if (stats) {
        stats[0]=(uint32_t)(lambda*1000); stats[1]=w->max_cycles; stats[2]=w->cur.next4; stats[3]=w->cur.next2;
        stats[4]=pre; stats[5]=slots; stats[6]=max_chunk; stats[7]=chunks;
    }
    return (int64_t)ps.used;
}

/* Mean nearest-entry error per block of the last encode (x8 scale squared):
 * out[0] 4x4, out[1] quads. For tuning the dictionaries. */
EXPORT void oh3do_preview7_debug(void *workspace,double *out)
{
    Work *w=work_of(workspace);
    uint64_t i,n=(uint64_t)w->frames*BLOCKS;
    double a=0,b=0;
    for (i=0;i<n;++i) { a+=w->d4[i]; b+=w->dq[i]; }
    out[0]=a/n; out[1]=b/n;
}
