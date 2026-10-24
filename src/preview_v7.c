/* OM3DVID7 decoder (see preview_v7.h). The console build runs the packet
 * loop and full redraws in preview_v7_arm.s; this C version runs everywhere else, is compiled
 * into the PC encoder to check its output, and is the reference the assembly
 * is tested against. Pixels are big-endian RGB555, as the cel engine reads
 * them. Uses no library calls, so the CRT-free encoder DLL can include it. */
#include "preview_v7.h"

static unsigned int be16(const unsigned char *p) { return ((unsigned int)p[0]<<8)|p[1]; }
static unsigned long be32(const unsigned char *p)
{
    return ((unsigned long)p[0]<<24)|((unsigned long)p[1]<<16)|((unsigned long)p[2]<<8)|p[3];
}
static void put32(unsigned char *p,unsigned long v)
{
    p[0]=(unsigned char)(v>>24); p[1]=(unsigned char)(v>>16); p[2]=(unsigned char)(v>>8); p[3]=(unsigned char)v;
}
static void copy4(unsigned char *d,const unsigned char *s) { d[0]=s[0]; d[1]=s[1]; d[2]=s[2]; d[3]=s[3]; }

#ifdef __CC_NORCROFT
void preview7_sum_arm(Preview7Sum *sum,const unsigned char *data,unsigned long bytes);
#endif
void preview7_sum_add(Preview7Sum *sum,const unsigned char *data,unsigned long bytes)
{
#ifdef __CC_NORCROFT
    preview7_sum_arm(sum,data,bytes&~3UL);
#else
    unsigned long a=sum->a,b=sum->b,i;
    for (i=0;i+4<=bytes;i+=4) { a=(a+be32(data+i))&0xffffffffUL; b=(b+a)&0xffffffffUL; }
    sum->a=a; sum->b=b;
#endif
}
unsigned long preview7_sum_value(const Preview7Sum *sum)
{
    return (sum->a^(((sum->b<<16)|(sum->b>>16))&0xffffffffUL))&0xffffffffUL;
}
unsigned long preview7_sum(const unsigned char *data,unsigned long bytes)
{
    Preview7Sum sum;
    sum.a=sum.b=0;
    preview7_sum_add(&sum,data,bytes);
    return preview7_sum_value(&sum);
}

unsigned long preview7_state_bytes(void)
{
    return PREVIEW7_BLOCKS*2UL*sizeof(unsigned long)+PREVIEW7_MAX_ENTRIES4*40UL+PREVIEW7_MAX_ENTRIES2*(8UL+sizeof(unsigned long));
}

int preview7_header(const unsigned char *block,Preview7Header *h)
{
    unsigned long check,sum,i;
    if (block[0]!='O' || block[1]!='M' || block[2]!='3' || block[3]!='D' || block[4]!='V' ||
        block[5]!='I' || block[6]!='D' || block[7]!='7') return -1;
    if (be16(block+8)!=PREVIEW7_WIDTH || be16(block+10)!=PREVIEW7_HEIGHT || be16(block+12)!=PREVIEW7_FPS) return -1;
    h->chunk_frames=be16(block+14); h->frames=be32(block+16);
    h->chunks=be16(block+20); h->preroll=be16(block+22);
    h->entries4=be16(block+24); h->entries2=be16(block+26);
    h->audio_rate=be32(block+28); h->max_chunk=be32(block+32); h->rate=be32(block+36);
    check=be32(block+40); h->slots=be16(block+44);
    h->table=block+PREVIEW7_HEADER_BYTES;
    if (!h->chunk_frames || h->chunk_frames>PREVIEW7_MAX_CHUNK_FRAMES || !h->frames || h->frames>PREVIEW7_MAX_FRAMES ||
        h->chunks!=(h->frames+h->chunk_frames-1)/h->chunk_frames || h->chunks>PREVIEW7_MAX_CHUNKS ||
        !h->preroll || h->preroll>h->chunks || h->entries4>PREVIEW7_MAX_ENTRIES4 || h->entries2>PREVIEW7_MAX_ENTRIES2 ||
        (h->audio_rate && h->audio_rate!=PREVIEW7_AUDIO_RATE) || !h->max_chunk || h->max_chunk%PREVIEW7_BLOCK_BYTES) return -1;
    if (h->slots<h->preroll) return -1;
    for (i=46;i<PREVIEW7_HEADER_BYTES;++i) if (block[i]) return -1;
    for (i=0;i<h->chunks;++i) {
        unsigned long n=be16(h->table+2*i)*PREVIEW7_BLOCK_BYTES;
        if (!n || n>h->max_chunk) return -1;
    }
    for (i=PREVIEW7_HEADER_BYTES+2UL*h->chunks;i<PREVIEW7_BLOCK_BYTES;++i) if (block[i]) return -1;
    /* The sum covers the block with the check field zero. */
    {
        unsigned long a=0,b=0,w;
        for (i=0;i<PREVIEW7_BLOCK_BYTES;i+=4) {
            w=i==40 ? 0 : be32(block+i);
            a=(a+w)&0xffffffffUL; b=(b+a)&0xffffffffUL;
        }
        sum=(a^(((b<<16)|(b>>16))&0xffffffffUL))&0xffffffffUL;
    }
    return sum==check ? 0 : -1;
}

/* Audio samples from the clip start to frame f. */
static unsigned long samples_to(unsigned long f) { return f*PREVIEW7_AUDIO_RATE/PREVIEW7_FPS; }

int preview7_chunk(const Preview7Header *h,unsigned int number,const unsigned char *data,unsigned long bytes,Preview7Chunk *c)
{
    return preview7_chunk_summed(h,number,data,bytes,preview7_sum(data+4,bytes-4),c);
}
int preview7_chunk_summed(const Preview7Header *h,unsigned int number,const unsigned char *data,unsigned long bytes,unsigned long sum,Preview7Chunk *c)
{
    unsigned long at,first_frame,i,audio_padded,table_at,end;
    if (number>=h->chunks || bytes!=be16(h->table+2*number)*PREVIEW7_BLOCK_BYTES) return -1;
    if (sum!=be32(data)) return -1;
    c->number=be16(data+4); c->frames=be16(data+6);
    c->first4=be16(data+8); c->count4=be16(data+10); c->first2=be16(data+12); c->count2=be16(data+14);
    c->audio_samples=be32(data+16); c->video_bytes=be32(data+20);
    first_frame=(unsigned long)number*h->chunk_frames;
    if (c->number!=number || !c->frames || c->frames!=(h->frames-first_frame<h->chunk_frames ? h->frames-first_frame : h->chunk_frames) ||
        (unsigned long)c->first4+c->count4>h->entries4 || (unsigned long)c->first2+c->count2>h->entries2) return -1;
    if (c->audio_samples!=(h->audio_rate ? samples_to(first_frame+c->frames)-samples_to(first_frame) : 0)) return -1;
    audio_padded=(c->audio_samples+3)&~3UL;
    at=PREVIEW7_CHUNK_HEADER;
    c->audio=(const signed char *)(data+at); at+=audio_padded;
    c->entries4=data+at; at+=c->count4*PREVIEW7_ENTRY4;
    c->entries2=data+at; at+=c->count2*PREVIEW7_ENTRY2;
    table_at=at;
    if (at>bytes || c->video_bytes>bytes-at || c->video_bytes<(c->frames+1)*4UL) return -1;
    c->offsets=data+table_at;
    end=c->video_bytes;
    if (be32(c->offsets)!=(c->frames+1)*4UL || be32(c->offsets+c->frames*4UL)!=end) return -1;
    for (i=0;i<c->frames;++i) {
        unsigned long a=be32(c->offsets+i*4),b=be32(c->offsets+i*4+4);
        if (b<a+2 || (b-a)&1) return -1;
    }
    for (i=audio_padded-c->audio_samples;i;--i) if (data[PREVIEW7_CHUNK_HEADER+audio_padded-i]) return -1;
    for (i=table_at+end;i<bytes;++i) if (data[i]) return -1;
    return 0;
}

static unsigned int average(const unsigned char *p,unsigned int a,unsigned int b,unsigned int c,unsigned int d)
{
    unsigned int pa=be16(p+a),pb=be16(p+b),pc=be16(p+c),pd=be16(p+d),out=0,shift;
    for (shift=0;shift<15;shift+=5) {
        unsigned int s=((pa>>shift)&31)+((pb>>shift)&31)+((pc>>shift)&31)+((pd>>shift)&31);
        out|=((s+2)>>2)<<shift;
    }
    return out;
}

/* Two pixels of the same column on consecutive lines, as one screen word. */
static unsigned long pair(const unsigned char *upper,const unsigned char *lower)
{
    return ((unsigned long)be16(upper)<<16)|be16(lower);
}

void preview7_entry4(const unsigned char *rows,unsigned char *entry)
{
    unsigned int k;
    /* Rows are 8 bytes apart; line pairs (0,1) and (2,3). */
    for (k=0;k<4;++k) {
        put32(entry+k*4,pair(rows+k*2,rows+8+k*2));
        put32(entry+16+k*4,pair(rows+16+k*2,rows+24+k*2));
    }
    put32(entry+32,((unsigned long)average(rows,0,2,8,10)<<16)|average(rows,4,6,12,14));
    put32(entry+36,((unsigned long)average(rows,16,18,24,26)<<16)|average(rows,20,22,28,30));
}

void preview7_entry2(const unsigned char *rows,unsigned char *entry)
{
    put32(entry,pair(rows,rows+4));
    put32(entry+4,pair(rows+2,rows+6));
    put32(entry+8,(unsigned long)average(rows,0,2,4,6)<<16);
}

void preview7_define(Preview7 *s,const Preview7Chunk *c)
{
    unsigned int i,k;
    for (i=0;i<c->count4;++i) {
        const unsigned char *src=c->entries4+i*PREVIEW7_ENTRY4;
        unsigned char *dst=s->dict4+(c->first4+i)*32UL,*t=s->thumb4+(c->first4+i)*8UL;
        for (k=0;k<32;k+=4) copy4(dst+k,src+k);
        copy4(t,src+32); copy4(t+4,src+36);
    }
    for (i=0;i<c->count2;++i) {
        const unsigned char *src=c->entries2+i*PREVIEW7_ENTRY2;
        unsigned char *dst=s->dict2+(c->first2+i)*8UL;
        copy4(dst,src); copy4(dst+4,src+4);
        s->thumb2[c->first2+i]=be32(src+8);
    }
}

/* Draw one block from its map words: into a screen bitmap or the thumb. */
static void draw(Preview7 *s,unsigned long block,unsigned long w0,unsigned long w1,unsigned char *bitmap)
{
    unsigned long bx=block%PREVIEW7_COLUMNS,by=block/PREVIEW7_COLUMNS;
    unsigned int k;
    if (!bitmap) {
        unsigned char *d=s->thumb+by*2UL*320UL+bx*4UL;
        if (w0==PREVIEW7_BLACK) { put32(d,0); put32(d+320,0); }
        else if (!(w0&0x80000000UL)) { copy4(d,s->thumb4+w0*8UL); copy4(d+320,s->thumb4+w0*8UL+4); }
        else {
            put32(d,s->thumb2[(w0>>12)&0xfff]|(s->thumb2[w0&0xfff]>>16));
            put32(d+320,s->thumb2[(w1>>12)&0xfff]|(s->thumb2[w1&0xfff]>>16));
        }
        return;
    }
    {
        /* Block row by covers line pairs 2by and 2by+1, 1280 bytes each. */
        unsigned char *d=bitmap+by*2560UL+bx*16UL;
        if (w0==PREVIEW7_BLACK) {
            for (k=0;k<16;k+=4) { put32(d+k,0); put32(d+1280+k,0); }
        } else if (!(w0&0x80000000UL)) {
            const unsigned char *e=s->dict4+w0*32UL;
            for (k=0;k<16;k+=4) { copy4(d+k,e+k); copy4(d+1280+k,e+16+k); }
        } else {
            const unsigned char *tl=s->dict2+((w0>>12)&0xfff)*8UL,*tr=s->dict2+(w0&0xfff)*8UL;
            const unsigned char *bl=s->dict2+((w1>>12)&0xfff)*8UL,*br=s->dict2+(w1&0xfff)*8UL;
            copy4(d,tl); copy4(d+4,tl+4); copy4(d+8,tr); copy4(d+12,tr+4);
            copy4(d+1280,bl); copy4(d+1284,bl+4); copy4(d+1288,br); copy4(d+1292,br+4);
        }
    }
}

void preview7_reset(Preview7 *s)
{
    unsigned long i;
    for (i=0;i<PREVIEW7_BLOCKS;++i) { s->map[2*i]=PREVIEW7_BLACK; s->map[2*i+1]=0; }
}

#ifdef __CC_NORCROFT
void preview7_render_arm(Preview7 *s,int picture);
#endif
void preview7_render(Preview7 *s,int picture)
{
#ifdef __CC_NORCROFT
    preview7_render_arm(s,picture);
#else
    unsigned long i;
    for (i=0;i<PREVIEW7_BLOCKS;++i) draw(s,i,s->map[2*i],s->map[2*i+1],picture==PREVIEW7_TV ? 0 : s->screen);
#endif
}

#ifdef __CC_NORCROFT
int preview7_decode(Preview7 *s,const unsigned char *data,unsigned long bytes,int picture);
#else
static void set(Preview7 *s,unsigned long block,unsigned long w0,unsigned long w1,int picture)
{
    s->map[2*block]=w0; s->map[2*block+1]=w1;
    draw(s,block,w0,w1,picture==PREVIEW7_TV ? 0 : s->screen);
}
static int preview7_decode(Preview7 *s,const unsigned char *data,unsigned long bytes,int picture)
{
    const unsigned char *end=data+bytes;
    unsigned int limit4=s->limit4,limit2=s->limit2;
    unsigned long block=0;
    while (data<end) {
        unsigned int w=be16(data),n,i;
        data+=2;
        n=(w&0x1fff)+1;
        switch (w>>13) {
        case 0:
            if (w>=limit4 || block>=PREVIEW7_BLOCKS) return -1;
            set(s,block++,w,0,picture);
            break;
        case 1:
            if (n>PREVIEW7_BLOCKS-block) return -1;
            block+=n;
            break;
        case 2:
            if (n>PREVIEW7_BLOCKS-block || (unsigned long)(end-data)<n*2UL) return -1;
            for (i=0;i<n;++i,data+=2) {
                unsigned int e=be16(data);
                if (e>=limit4) return -1;
                set(s,block++,e,0,picture);
            }
            break;
        case 3:
            if (n>PREVIEW7_BLOCKS-block || (unsigned long)(end-data)<n*6UL) return -1;
            for (i=0;i<n;++i,data+=6) {
                unsigned long hi=((unsigned long)be16(data)<<8)|(data[2]),lo=((unsigned long)data[3]<<16)|be16(data+4);
                if (((hi>>12)&0xfff)>=limit2 || (hi&0xfff)>=limit2 || ((lo>>12)&0xfff)>=limit2 || (lo&0xfff)>=limit2) return -1;
                set(s,block++,0x80000000UL|hi,lo,picture);
            }
            break;
        default:
            return -1;
        }
    }
    return block==PREVIEW7_BLOCKS ? 0 : -1;
}
#endif

int preview7_frame(Preview7 *s,const Preview7Chunk *c,unsigned int frame,int picture)
{
    unsigned long a,b;
    if (frame>=c->frames) return -1;
    a=be32(c->offsets+frame*4UL); b=be32(c->offsets+frame*4UL+4);
    s->limit4=c->first4+c->count4; s->limit2=c->first2+c->count2;
    return preview7_decode(s,c->offsets+a,b-a,picture);
}
