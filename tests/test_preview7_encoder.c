/* The OM3DVID7 encoder (tools/preview7-encoder.c) on a synthetic clip:
 *     test_preview7_encoder OUT.3dv
 * 75 frames of moving shapes over a gradient, with a tone. The file must
 * parse, decode with the menu's C decoder near the source, keep every
 * frame within the decode budget the encoder models, buffer within the slots
 * it assumes, and stay near the requested rate. Writes the clip for
 * test_preview_stream. Bad arguments and a short output fail cleanly. */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/preview_v7.h"

typedef struct Preview7Params {
    uint32_t rate, read_rate, slot_bytes, max_chunk, chunk_frames, entries4, entries2, cycle_cap;
} Preview7Params;
uint32_t oh3do_preview7_abi(void);
uint64_t oh3do_preview7_workspace(uint32_t frames);
int32_t oh3do_preview7_begin(void *workspace,uint64_t bytes,uint32_t frames);
int32_t oh3do_preview7_frame(void *workspace,const uint8_t *rgb);
int64_t oh3do_preview7_encode(void *workspace,const Preview7Params *p,const int8_t *audio,uint64_t audio_samples,
                              uint8_t *out,uint64_t capacity,uint32_t *stats);

#define FRAMES 75
#define W 320
#define H 240

static void synthesize(uint8_t *rgb,int f)
{
    int x,y;
    for (y=0;y<H;++y) for (x=0;x<W;++x) {
        uint8_t *p=rgb+(y*W+x)*3;
        int dx=x-(40+f*3),dy=y-(60+f),ex=x-(250-f*2),ey=y-(170-f);
        p[0]=(uint8_t)(x*255/W); p[1]=(uint8_t)(y*255/H); p[2]=(uint8_t)(((x+y+f*4)&63)*4);
        if (dx*dx+dy*dy<900) { p[0]=240; p[1]=200; p[2]=40; }
        if (ex>0 && ex<50 && ey>0 && ey<30) { p[0]=20; p[1]=(uint8_t)(((x^y)&8) ? 220 : 60); p[2]=200; }
    }
}

int main(int argc,char **argv)
{
    Preview7Params p={204800,307200,655360,163840,10,1024,512,230000};
    uint64_t bytes=oh3do_preview7_workspace(FRAMES),samples=(uint64_t)FRAMES*11025/30,capacity=4u<<20;
    uint8_t *ws=(uint8_t *)malloc(bytes),*rgb=(uint8_t *)malloc(W*H*3),*out=(uint8_t *)malloc(capacity);
    int8_t *audio=(int8_t *)malloc(samples);
    uint32_t stats[8];
    int64_t n;
    uint64_t i;
    int f;
    Preview7Header h;
    FILE *file;
    assert(argc==2 && ws && rgb && out && audio && oh3do_preview7_abi()==1);
    for (i=0;i<samples;++i) audio[i]=(int8_t)(60*sin((double)i*2*3.14159265*440/11025));

    /* Bad arguments. */
    assert(oh3do_preview7_begin(ws,bytes-1,FRAMES)<0 && oh3do_preview7_begin(ws,bytes,0)<0);
    assert(!oh3do_preview7_begin(ws,bytes,FRAMES));
    assert(oh3do_preview7_encode(ws,&p,audio,samples,out,capacity,stats)==-1); /* frames missing */
    for (f=0;f<FRAMES;++f) { synthesize(rgb,f); assert(!oh3do_preview7_frame(ws,rgb)); }
    assert(oh3do_preview7_frame(ws,rgb)<0);
    assert(oh3do_preview7_encode(ws,&p,audio,samples-1,out,capacity,stats)==-1);
    p.max_chunk=1000; assert(oh3do_preview7_encode(ws,&p,audio,samples,out,capacity,stats)==-1); p.max_chunk=163840;
    assert(oh3do_preview7_encode(ws,&p,audio,samples,out,4096,stats)==-2);

    n=oh3do_preview7_encode(ws,&p,audio,samples,out,capacity,stats);
    assert(n>0);
    assert(!preview7_header(out,&h));
    assert(h.frames==FRAMES && h.chunk_frames==10 && h.chunks==8 && h.audio_rate==11025);
    assert(h.entries4==stats[2] && h.entries2==stats[3] && h.entries4<=1024 && h.entries2<=512);
    assert(stats[1]<=p.cycle_cap && h.preroll>=1 && h.preroll<=h.slots && h.slots==4 && h.max_chunk<=p.max_chunk);
    assert((uint64_t)n<=(uint64_t)p.rate*FRAMES/30+2048);

    /* Decode: every chunk checks, and the picture is close to the source. */
    {
        Preview7 d;
        uint64_t at=PREVIEW7_BLOCK_BYTES;
        double error=0;
        unsigned int c,k,frame=0;
        memset(&d,0,sizeof(d));
        d.screen=(unsigned char *)calloc(1,PREVIEW7_FRAME_BYTES); d.thumb=(unsigned char *)calloc(1,PREVIEW7_THUMB_BYTES);
        d.map=(unsigned long *)malloc(PREVIEW7_BLOCKS*2*sizeof(unsigned long));
        d.dict4=(unsigned char *)calloc(8192,32); d.thumb4=(unsigned char *)calloc(8192,8);
        d.dict2=(unsigned char *)calloc(4096,8); d.thumb2=(unsigned long *)calloc(4096,sizeof(unsigned long));
        preview7_reset(&d);
        for (c=0;c<h.chunks;++c) {
            Preview7Chunk info;
            unsigned long size=((unsigned long)h.table[2*c]<<8|h.table[2*c+1])*2048UL;
            assert(at+size<=(uint64_t)n && !preview7_chunk(&h,c,out+at,size,&info));
            assert(info.audio_samples && !memcmp(info.audio,audio+(uint64_t)frame*11025/30,info.audio_samples));
            preview7_define(&d,&info);
            for (k=0;k<info.frames;++k,++frame) {
                int x,y;
                assert(!preview7_frame(&d,&info,k,PREVIEW7_SCREEN));
                synthesize(rgb,(int)frame);
                for (y=0;y<H;++y) for (x=0;x<W;++x) {
                    unsigned int at2=((y>>1)*W+x)*4+(y&1)*2,px=((unsigned int)d.screen[at2]<<8)|d.screen[at2+1];
                    const uint8_t *s=rgb+(y*W+x)*3;
                    int ch;
                    for (ch=0;ch<3;++ch) {
                        double v=(double)((px>>(10-5*ch))&31)*255/31-s[ch];
                        error+=v*v;
                    }
                }
            }
            at+=size;
        }
        assert(frame==FRAMES && at==(uint64_t)n);
        error/=(double)FRAMES*W*H*3;
        printf("preview7 encoder: %lld bytes, %.1f dB, %u+%u entries, preroll %u, slowest frame %u clocks\n",
               (long long)n,10*log10(255.0*255/error),stats[2],stats[3],stats[4],stats[1]);
        /* The stripes shift everywhere every frame: hard on purpose. */
        assert(10*log10(255.0*255/error)>20);
        free(d.screen); free(d.thumb); free(d.map); free(d.dict4); free(d.thumb4); free(d.dict2); free(d.thumb2);
    }
    file=fopen(argv[1],"wb");
    assert(file && fwrite(out,1,(size_t)n,file)==(size_t)n);
    fclose(file);
    free(ws); free(rgb); free(out); free(audio);
    return 0;
}
