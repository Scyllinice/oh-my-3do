#include "usb_media.h"
#include "usb_file.h"
#include "../vendor/pngle/pngle.h"
#include <stdlib.h>
#include <string.h>
#include "../vendor/pngle/miniz.h"
#ifdef OH3DO_SDK
#include "mem.h"
#endif

static unsigned long word(const unsigned char *p)
{
    return ((unsigned long)p[0]<<24)|((unsigned long)p[1]<<16)|((unsigned long)p[2]<<8)|p[3];
}
static unsigned char *audio_memory(unsigned long bytes)
{
#ifdef OH3DO_SDK
    return (unsigned char *)AllocMem((int32)bytes,MEMTYPE_AUDIO);
#else
    return (unsigned char *)malloc((size_t)bytes);
#endif
}
void usb_preview_free(UsbPreview *preview)
{
#ifdef OH3DO_SDK
    if (preview->audio) FreeMem(preview->audio,(int32)preview->audio_bytes);
#else
    free(preview->audio);
#endif
    free(preview->pixels); free(preview->frame_buffer); free(preview->decoder);
    memset(preview,0,sizeof(*preview));
}
int usb_preview_frame(UsbPreview *preview,unsigned int frame,unsigned char *pixels)
{
    unsigned long begin,end;
    size_t in_bytes,out_bytes=USB_ART_PIXELS;
    tinfl_status status;
    tinfl_decompressor *decoder=(tinfl_decompressor *)preview->decoder;
    if (!preview->pixels || frame>=preview->frames || !pixels) return -1;
    if (preview->version==1) {
        memcpy(pixels,preview->pixels+frame*USB_ART_PIXELS,USB_ART_PIXELS); return 0;
    }
    if (preview->version!=2 || !decoder) return -1;
    begin=word(preview->pixels+frame*4); end=word(preview->pixels+(frame+1)*4);
    if (begin<(preview->frames+1)*4UL || begin>=end || end>preview->video_bytes) return -1;
    in_bytes=(size_t)(end-begin); tinfl_init(decoder);
    status=tinfl_decompress(decoder,preview->pixels+begin,&in_bytes,pixels,pixels,&out_bytes,
                            TINFL_FLAG_PARSE_ZLIB_HEADER|TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    return status==TINFL_STATUS_DONE && out_bytes==USB_ART_PIXELS && in_bytes==(size_t)(end-begin) ? 0 : -1;
}
static int preview_bytes(Ode *ode,OdeReadBuffer reader,unsigned char *data,unsigned long bytes,
                         unsigned long *crc,MediaCancel cancel,void *context)
{
    unsigned long pos=0;
    while (pos<bytes) {
        size_t n=bytes-pos>2048 ? 2048 : (size_t)(bytes-pos);
        if ((cancel && cancel(context)) || usb_file_read(ode,reader,data+pos,n,0)<0) return -1;
        *crc=usb_art_crc32_update(*crc,data+pos,n); pos+=n;
    }
    return 0;
}
int usb_preview_load(Ode *ode, OdeReadBuffer reader, const char *filename,
                     UsbPreview *preview, MediaCancel cancel, void *context)
{
    unsigned char header[40];
    UsbPreview next;
    unsigned long bytes,crc=0xffffffffUL;
    unsigned int frame;
    char path[128];
    int result=-1;
    memset(&next,0,sizeof(next));
    if (!usb_media_filename(filename) || menu_path_compare(filename+strlen(filename)-4,".3dv")) return -1;
    strcpy(path,"previews/"); strcat(path,filename);
    if (usb_file_open(ode,path)<0) return -1;
    if (usb_file_read(ode,reader,header,24,1)<0 ||
        (memcmp(header,"OM3DVID1",8) && memcmp(header,"OM3DVID2",8)) ||
        header[8] || header[9]!=64 || header[10] || header[11]!=80) goto done;
    next.version=header[7]=='2' ? 2 : 1;
    next.frames=header[20]*256U+header[21]; next.fps=header[22]*256U+header[23];
    bytes=word(header+12);
    if (!next.frames || !next.fps || next.fps>12) goto done;
    if (next.version==1) {
        if (next.frames>USB_PREVIEW_MAX_FRAMES || bytes!=next.frames*(unsigned long)USB_ART_PIXELS) goto done;
        next.video_bytes=bytes;
    } else {
        unsigned long expected;
        if (next.frames>USB_PREVIEW_V2_MAX_FRAMES || next.frames>next.fps*120U || bytes>USB_PREVIEW_MAX_BYTES ||
            usb_file_read(ode,reader,header+24,16,1)<0) goto done;
        next.video_bytes=word(header+24); next.audio_bytes=word(header+28);
        next.audio_rate=word(header+32); next.audio_frames=word(header+36);
        if (next.video_bytes>bytes || next.audio_bytes!=bytes-next.video_bytes || next.video_bytes<=(next.frames+1)*4UL) goto done;
        if (next.audio_bytes) {
            expected=(((next.frames*11025UL+next.fps-1)/next.fps)+3)&~3UL;
            if (next.audio_rate!=11025 || next.audio_bytes!=next.audio_frames || next.audio_frames!=expected) goto done;
        } else if (next.audio_rate || next.audio_frames) goto done;
        next.decoder=malloc(sizeof(tinfl_decompressor)); next.frame_buffer=(unsigned char *)malloc(USB_ART_PIXELS);
        if (!next.decoder || !next.frame_buffer) goto done;
    }
    next.pixels=(unsigned char *)malloc((size_t)next.video_bytes);
    if (!next.pixels) goto done;
    if (preview_bytes(ode,reader,next.pixels,next.video_bytes,&crc,cancel,context)<0) goto done;
    if (next.audio_bytes) {
        next.audio=audio_memory(next.audio_bytes);
        if (!next.audio || preview_bytes(ode,reader,next.audio,next.audio_bytes,&crc,cancel,context)<0) goto done;
    }
    if ((cancel && cancel(context)) || (crc^0xffffffffUL)!=word(header+16) ||
        usb_file_verify(ode,reader,(const unsigned char *)(next.version==1 ? "END3DV01" : "END3DV02"),8)<0) goto done;
    if (next.version==2) {
        if (word(next.pixels)!=(next.frames+1)*4UL || word(next.pixels+next.frames*4)!=next.video_bytes) goto done;
        for (frame=0;frame<next.frames;++frame) {
            if ((cancel && cancel(context)) || usb_preview_frame(&next,frame,next.frame_buffer)<0) goto done;
        }
    }
    result=0;
done:
    if (usb_file_close(ode)<0) result=-1;
    if (!result) { usb_preview_free(preview); *preview=next; memset(&next,0,sizeof(next)); }
    usb_preview_free(&next);
    return result;
}

typedef struct PngCanvas {
    unsigned char *pixels;
    unsigned short xb[1025], yb[1025];
    unsigned int width, height, left, top, target_width, target_height;
    unsigned int source_width, source_height;
    unsigned char *small;
    int complete, native;
    unsigned int rendered;
    MediaCancel cancel;
    void *context;
} PngCanvas;

static void png_init(pngle_t *png, uint32_t w, uint32_t h)
{
    PngCanvas *canvas = (PngCanvas *)pngle_get_user_data(png);
    unsigned int i;
    canvas->source_width=w; canvas->source_height=h;
    canvas->native=w==canvas->target_width && h==canvas->target_height;
    canvas->width=canvas->target_width; canvas->height=(unsigned int)(h*canvas->target_width/w);
    if (canvas->height>canvas->target_height) { canvas->height=canvas->target_height; canvas->width=(unsigned int)(w*canvas->target_height/h); }
    if (w<=canvas->target_width && h<=canvas->target_height && !canvas->native) {
        canvas->small=(unsigned char *)calloc(w*h,2);
        if (!canvas->small) { pngle_abort(png); return; }
    }
    if (!canvas->width) canvas->width = 1;
    if (!canvas->height) canvas->height = 1;
    canvas->left = (canvas->target_width-canvas->width)/2; canvas->top = (canvas->target_height-canvas->height)/2;
    for (i=0;i<=w;++i) canvas->xb[i]=(unsigned short)((i*canvas->width+w-1)/w);
    for (i=0;i<=h;++i) canvas->yb[i]=(unsigned short)((i*canvas->height+h-1)/h);
}
static void png_draw(pngle_t *png, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t rgba[4])
{
    PngCanvas *c = (PngCanvas *)pngle_get_user_data(png);
    unsigned int dx, dy, color, r, g, b, a = rgba[3];
    if (!(++c->rendered & 4095U) && c->cancel && c->cancel(c->context)) { pngle_abort(png); return; }
    if (x+w>c->source_width || y+h>c->source_height || x+w>1024 || y+h>1024) return;
    if (!c->small && (c->xb[x]==c->xb[x+w] || c->yb[y]==c->yb[y+h])) return;
    if (a==255) color=((rgba[0]>>3)<<10)|((rgba[1]>>3)<<5)|(rgba[2]>>3);
    else {
        r=(rgba[0]*a+8*(255-a)+127)/255; g=(rgba[1]*a+16*(255-a)+127)/255; b=(rgba[2]*a+33*(255-a)+127)/255;
        color=((r>>3)<<10)|((g>>3)<<5)|(b>>3);
    }
    if (c->small) {
        for (dy=y;dy<y+h;++dy) for (dx=x;dx<x+w;++dx) {
            unsigned int pos=(dy*c->source_width+dx)*2;
            c->small[pos]=(unsigned char)(color>>8); c->small[pos+1]=(unsigned char)color;
        }
        return;
    }
    if (c->native && w==1 && h==1) {
        unsigned int pos;
        if (x>=c->target_width || y>=c->target_height) return;
        pos=(y*c->target_width+x)*2;
        c->pixels[pos]=(unsigned char)(color>>8); c->pixels[pos+1]=(unsigned char)color;
        return;
    }
    for (dy=c->yb[y];dy<c->yb[y+h];++dy) for (dx=c->xb[x];dx<c->xb[x+w];++dx) {
        unsigned int pos=((dy+c->top)*c->target_width+dx+c->left)*2;
        c->pixels[pos]=(unsigned char)(color>>8); c->pixels[pos+1]=(unsigned char)color;
    }
}
static unsigned int small_pixel(const PngCanvas *c,unsigned int x,unsigned int y)
{
    unsigned int pos=(y*c->source_width+x)*2;
    return ((unsigned int)c->small[pos]<<8)|c->small[pos+1];
}
static int smooth_canvas(PngCanvas *c)
{
    unsigned int x,y;
    if (c->small) for (y=0;y<c->height;++y) {
        unsigned long sy=c->height>1 ? (y*(c->source_height-1)*256UL)/(c->height-1) : 0;
        unsigned int y0=(unsigned int)(sy>>8), y1=y0+1<c->source_height ? y0+1 : y0, fy=(unsigned int)(sy&255);
        if (!(y&7U) && c->cancel && c->cancel(c->context)) return -1;
        for (x=0;x<c->width;++x) {
            unsigned long sx=c->width>1 ? (x*(c->source_width-1)*256UL)/(c->width-1) : 0;
            unsigned int x0=(unsigned int)(sx>>8), x1=x0+1<c->source_width ? x0+1 : x0, fx=(unsigned int)(sx&255);
            unsigned int colors[4], channel, color=0, pos;
            colors[0]=small_pixel(c,x0,y0); colors[1]=small_pixel(c,x1,y0);
            colors[2]=small_pixel(c,x0,y1); colors[3]=small_pixel(c,x1,y1);
            for (channel=0;channel<3;++channel) {
                unsigned int shift=channel*5;
                unsigned long top=((colors[0]>>shift)&31)*(256UL-fx)+((colors[1]>>shift)&31)*fx;
                unsigned long bottom=((colors[2]>>shift)&31)*(256UL-fx)+((colors[3]>>shift)&31)*fx;
                unsigned int value=(unsigned int)((top*(256-fy)+bottom*fy+32768UL)>>16);
                color|=value<<shift;
            }
            pos=((y+c->top)*c->target_width+x+c->left)*2;
            c->pixels[pos]=(unsigned char)(color>>8); c->pixels[pos+1]=(unsigned char)color;
        }
    }
    return 0;
}
static void png_done(pngle_t *png)
{
    PngCanvas *c=(PngCanvas *)pngle_get_user_data(png);
    if (smooth_canvas(c)<0) { pngle_abort(png); return; }
    c->complete=1;
}
static int png_feed(pngle_t *png, unsigned char *pending, size_t *used, const unsigned char *data, size_t bytes)
{
    int consumed;
    if (*used+bytes > 2064) return -1;
    memcpy(pending+*used,data,bytes); *used += bytes;
    consumed=pngle_feed(png,pending,*used);
    if (consumed < 0 || (size_t)consumed > *used) return -1;
    *used -= (size_t)consumed;
    memmove(pending,pending+consumed,*used);
    return 0;
}
static int png_pixels(Ode *ode, OdeReadBuffer reader, const char *filename,
                   unsigned char *pixels, MediaCancel cancel, void *context,unsigned int width,unsigned int height)
{
    static const unsigned char iend_crc[4] = {0xae,0x42,0x60,0x82};
    unsigned char block[2048], pending[2064];
    char path[128];
    pngle_t *png = NULL;
    PngCanvas *canvas = NULL;
    unsigned long bytes = 33;
    size_t used = 0, i;
    int result = -1;
    if (!usb_media_filename(filename) || menu_path_compare(filename+strlen(filename)-4,".png")) return -1;
    strcpy(path,"covers/"); strcat(path,filename);
    if (usb_file_open(ode,path) < 0) return -1;
    if (usb_file_read(ode,reader,block,33,1) < 0 || memcmp(block,"\211PNG\r\n\032\n",8) ||
        word(block+8) != 13 || memcmp(block+12,"IHDR",4) || !word(block+16) || !word(block+20) ||
        word(block+16) > 1024 || word(block+20) > 1024 || block[28] > 1) goto done;
    png = pngle_new(); canvas = (PngCanvas *)calloc(1,sizeof(*canvas));
    if (!png || !canvas) goto done;
    canvas->pixels=pixels; canvas->cancel=cancel; canvas->context=context; canvas->target_width=width; canvas->target_height=height;
    for (i=0;i<width*height*2;i+=2) { pixels[i]=4; pixels[i+1]=0x44; }
    pngle_set_user_data(png,canvas); pngle_set_init_callback(png,png_init);
    pngle_set_draw_callback(png,png_draw); pngle_set_done_callback(png,png_done);
    if (png_feed(png,pending,&used,block,33) < 0) goto done;
    while (!canvas->complete) {
        unsigned long remaining;
        int end;
        if ((cancel && cancel(context)) || bytes > USB_PNG_MAX_BYTES-12 ||
            usb_file_read(ode,reader,block,8,1) < 0) goto done;
        for (i=4;i<8;++i) if (!((block[i]>='A' && block[i]<='Z') || (block[i]>='a' && block[i]<='z'))) goto done;
        if (block[6]&32) goto done;
        if (!(block[4]&32) && memcmp(block+4,"IHDR",4) && memcmp(block+4,"PLTE",4) &&
            memcmp(block+4,"IDAT",4) && memcmp(block+4,"IEND",4)) goto done;
        remaining=word(block); end=!memcmp(block+4,"IEND",4); bytes+=12;
        if (remaining > USB_PNG_MAX_BYTES-bytes || png_feed(png,pending,&used,block,8) < 0) goto done;
        bytes+=remaining;
        while (remaining) {
            size_t n=remaining > 2048 ? 2048 : (size_t)remaining;
            if ((cancel && cancel(context)) || usb_file_read(ode,reader,block,n,0) < 0 ||
                png_feed(png,pending,&used,block,n) < 0) goto done;
            remaining-=n;
        }
        if (end) {
            /* A known IEND CRC is poisoned byte-for-byte before the final read.
             * Stale EOF bytes cannot masquerade as a complete PNG. */
            if (usb_file_verify(ode,reader,iend_crc,4) < 0 || png_feed(png,pending,&used,iend_crc,4) < 0) goto done;
        } else if (usb_file_read(ode,reader,block,4,1) < 0 || png_feed(png,pending,&used,block,4) < 0) goto done;
    }
    if (!used) result=0;
done:
    pngle_destroy(png); if (canvas) free(canvas->small); free(canvas);
    if (usb_file_close(ode) < 0) result=-1;
    return result;
}

int usb_png_pixels(Ode *ode,OdeReadBuffer reader,const char *filename,unsigned char *pixels,MediaCancel cancel,void *context)
{
    return png_pixels(ode,reader,filename,pixels,cancel,context,64,80);
}
int usb_png_cover(Ode *ode,OdeReadBuffer reader,const char *filename,unsigned char *pixels,MediaCancel cancel,void *context)
{
    return png_pixels(ode,reader,filename,pixels,cancel,context,USB_COVER_WIDTH,USB_COVER_HEIGHT);
}

int usb_legacy_cover(Ode *ode,OdeReadBuffer reader,const char *filename,unsigned char *pixels,MediaCancel cancel,void *context)
{
    PngCanvas *c=(PngCanvas *)calloc(1,sizeof(*c));
    int result=-1;
    if (!c) return -1;
    c->small=(unsigned char *)malloc(USB_ART_PIXELS);
    if (!c->small) { free(c); return -1; }
    c->source_width=64; c->source_height=80;
    c->target_width=c->width=USB_COVER_WIDTH; c->target_height=c->height=USB_COVER_HEIGHT;
    c->pixels=pixels; c->cancel=cancel; c->context=context;
    if (!(cancel && cancel(context)) && !usb_art_pixels(ode,reader,filename,c->small)) result=smooth_canvas(c);
    free(c->small); free(c); return result;
}
