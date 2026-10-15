#include "usb_media.h"
#include "usb_file.h"
#include "../vendor/pngle/pngle.h"
#include <stdlib.h>
#include <string.h>

static unsigned long word(const unsigned char *p)
{
    return ((unsigned long)p[0]<<24)|((unsigned long)p[1]<<16)|((unsigned long)p[2]<<8)|p[3];
}
void usb_preview_free(UsbPreview *preview)
{
    free(preview->pixels); memset(preview,0,sizeof(*preview));
}
int usb_preview_load(Ode *ode, OdeReadBuffer reader, const char *filename,
                     UsbPreview *preview, MediaCancel cancel, void *context)
{
    unsigned char header[24];
    UsbPreview next;
    unsigned long bytes;
    size_t pos;
    char path[128];
    int result = -1;
    memset(&next,0,sizeof(next));
    if (!usb_media_filename(filename) || menu_path_compare(filename+strlen(filename)-4,".3dv")) return -1;
    strcpy(path,"previews/"); strcat(path,filename);
    if (usb_file_open(ode,path) < 0) return -1;
    if (usb_file_read(ode,reader,header,24,1) < 0 || memcmp(header,"OM3DVID1",8) ||
        header[8] || header[9] != 64 || header[10] || header[11] != 80) goto done;
    next.frames = header[20]*256U+header[21]; next.fps = header[22]*256U+header[23];
    bytes = word(header+12);
    if (!next.frames || next.frames > USB_PREVIEW_MAX_FRAMES || !next.fps || next.fps > 12 ||
        bytes != next.frames * (unsigned long)USB_ART_PIXELS) goto done;
    next.pixels = (unsigned char *)malloc((size_t)bytes);
    if (!next.pixels) goto done;
    for (pos = 0; pos < (size_t)bytes;) {
        size_t n = bytes-pos > 2048 ? 2048 : (size_t)bytes-pos;
        if ((cancel && cancel(context)) || usb_file_read(ode,reader,next.pixels+pos,n,0) < 0) goto done;
        pos += n;
    }
    if (cancel && cancel(context)) goto done;
    if (usb_art_crc32(next.pixels,(size_t)bytes) != word(header+16) ||
        usb_file_verify(ode,reader,(const unsigned char *)"END3DV01",8) < 0) goto done;
    result = 0;
done:
    if (usb_file_close(ode) < 0) result = -1;
    if (!result) { usb_preview_free(preview); *preview = next; memset(&next,0,sizeof(next)); }
    usb_preview_free(&next);
    return result;
}

typedef struct PngCanvas {
    unsigned char *pixels;
    unsigned short xb[1025], yb[1025];
    unsigned int width, height, left, top;
    int complete, native;
    unsigned int rendered;
    MediaCancel cancel;
    void *context;
} PngCanvas;

static void png_init(pngle_t *png, uint32_t w, uint32_t h)
{
    PngCanvas *canvas = (PngCanvas *)pngle_get_user_data(png);
    unsigned int i;
    canvas->native=w==64 && h==80;
    canvas->width = 64; canvas->height = (unsigned int)(h*64/w);
    if (canvas->height > 80) { canvas->height = 80; canvas->width = (unsigned int)(w*80/h); }
    if (!canvas->width) canvas->width = 1;
    if (!canvas->height) canvas->height = 1;
    canvas->left = (64-canvas->width)/2; canvas->top = (80-canvas->height)/2;
    for (i=0;i<=w;++i) canvas->xb[i]=(unsigned short)((i*canvas->width+w-1)/w);
    for (i=0;i<=h;++i) canvas->yb[i]=(unsigned short)((i*canvas->height+h-1)/h);
}
static void png_draw(pngle_t *png, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t rgba[4])
{
    PngCanvas *c = (PngCanvas *)pngle_get_user_data(png);
    unsigned int dx, dy, color, r, g, b, a = rgba[3];
    if (!(++c->rendered & 4095U) && c->cancel && c->cancel(c->context)) { pngle_abort(png); return; }
    if (x+w > 1024 || y+h > 1024 || c->xb[x] == c->xb[x+w] || c->yb[y] == c->yb[y+h]) return;
    if (a==255) color=((rgba[0]>>3)<<10)|((rgba[1]>>3)<<5)|(rgba[2]>>3);
    else {
        r=(rgba[0]*a+8*(255-a)+127)/255; g=(rgba[1]*a+16*(255-a)+127)/255; b=(rgba[2]*a+33*(255-a)+127)/255;
        color=((r>>3)<<10)|((g>>3)<<5)|(b>>3);
    }
    if (c->native && w==1 && h==1) {
        unsigned int pos;
        if (x>=64 || y>=80) return;
        pos=(y*64+x)*2;
        c->pixels[pos]=(unsigned char)(color>>8); c->pixels[pos+1]=(unsigned char)color;
        return;
    }
    for (dy=c->yb[y];dy<c->yb[y+h];++dy) for (dx=c->xb[x];dx<c->xb[x+w];++dx) {
        unsigned int pos=((dy+c->top)*64+dx+c->left)*2;
        c->pixels[pos]=(unsigned char)(color>>8); c->pixels[pos+1]=(unsigned char)color;
    }
}
static void png_done(pngle_t *png)
{
    ((PngCanvas *)pngle_get_user_data(png))->complete=1;
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
int usb_png_pixels(Ode *ode, OdeReadBuffer reader, const char *filename,
                   unsigned char *pixels, MediaCancel cancel, void *context)
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
    canvas->pixels=pixels; canvas->cancel=cancel; canvas->context=context;
    for (i=0;i<USB_ART_PIXELS;i+=2) { pixels[i]=4; pixels[i+1]=0x44; }
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
    pngle_destroy(png); free(canvas);
    if (usb_file_close(ode) < 0) result=-1;
    return result;
}
