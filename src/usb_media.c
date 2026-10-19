#include "usb_media.h"
#include "usb_file.h"
#include "preview_v5.h"
#include "../vendor/pngle/pngle.h"
#include <stdlib.h>
#include <string.h>
#include "../vendor/pngle/miniz.h"
#ifdef OH3DO_SDK
#include "mem.h"
#endif

/* The decoder may read this far past a packet. */
#define READ_AHEAD PREVIEW_V5_READ_AHEAD

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
/* Preview chunk memory. Reserves keep DRAM for the menu's text and covers.
 * Hosts have no VRAM and emulate a fragmented heap of fixed-size blocks. */
#define DRAM_RESERVE 65536UL
#define VRAM_RESERVE 32768UL
#define HOST_CHUNK (256UL * 1024UL)
static unsigned long chunk_room(int vram)
{
#ifdef OH3DO_SDK
    MemInfo memory;
    unsigned long free_bytes,largest,reserve=vram ? VRAM_RESERVE : DRAM_RESERVE;
    AvailMem(&memory,vram ? MEMTYPE_VRAM : MEMTYPE_DRAM);
    free_bytes=memory.minfo_SysFree+memory.minfo_TaskFree;
    largest=memory.minfo_SysLargest>memory.minfo_TaskLargest ? memory.minfo_SysLargest : memory.minfo_TaskLargest;
    if (free_bytes<=reserve) return 0;
    if (largest>free_bytes-reserve) largest=free_bytes-reserve;
    /* Leave a page for allocator rounding. */
    return largest>4096UL ? largest-4096UL : 0;
#else
    return vram ? 0 : HOST_CHUNK;
#endif
}
static unsigned long chunk_capacity(void)
{
#ifdef OH3DO_SDK
    MemInfo memory;
    unsigned long total=0,free_bytes;
    AvailMem(&memory,MEMTYPE_VRAM); free_bytes=memory.minfo_SysFree+memory.minfo_TaskFree;
    if (free_bytes>VRAM_RESERVE) total+=free_bytes-VRAM_RESERVE;
    AvailMem(&memory,MEMTYPE_DRAM); free_bytes=memory.minfo_SysFree+memory.minfo_TaskFree;
    if (free_bytes>DRAM_RESERVE) total+=free_bytes-DRAM_RESERVE;
    return total;
#else
    return USB_PREVIEW_CHUNKS*HOST_CHUNK;
#endif
}
static unsigned char *frame_memory(unsigned long bytes)
{
#ifdef OH3DO_SDK
    return (unsigned char *)AllocMem((int32)bytes,MEMTYPE_CEL);
#else
    return (unsigned char *)malloc((size_t)bytes);
#endif
}
static unsigned char *chunk_alloc(unsigned long bytes,int vram)
{
#ifdef OH3DO_SDK
    return (unsigned char *)AllocMem((int32)bytes,vram ? MEMTYPE_VRAM : MEMTYPE_DRAM);
#else
    (void)vram;
    return (unsigned char *)malloc((size_t)bytes);
#endif
}
void usb_preview_free(UsbPreview *preview)
{
    unsigned int i;
    for (i=0;i<preview->chunk_count;++i) {
#ifdef OH3DO_SDK
        FreeMem(preview->chunk[i],(int32)preview->chunk_bytes[i]);
#else
        free(preview->chunk[i]);
#endif
    }
#ifdef OH3DO_SDK
    if (preview->audio) FreeMem(preview->audio,(int32)preview->audio_bytes);
    if (preview->frame_buffer) FreeMem(preview->frame_buffer,(int32)preview->frame_bytes);
#else
    free(preview->audio);
    free(preview->frame_buffer);
#endif
    free(preview->table); free(preview->decoder);
    memset(preview,0,sizeof(*preview));
}
int usb_preview_frame(UsbPreview *preview,unsigned int frame,unsigned char *pixels)
{
    PreviewV5 *state=(PreviewV5 *)preview->decoder;
    if (!preview->table || !state || !preview->frame_buffer || frame>=preview->frames || !pixels) return -1;
    if ((unsigned long)frame+1<state->frames) {
        /* Looping back: a clip decodes forward from a black frame and empty cache. */
        memset(state,0,sizeof(*state)); memset(preview->frame_buffer,0,(size_t)preview->frame_bytes);
    }
    while (state->frames<=frame) {
        unsigned long first=word(preview->table+state->frames*4),last=word(preview->table+(state->frames+1)*4);
        unsigned int k=0;
        while (k<preview->chunk_count && !(first>=preview->chunk_begin[k] && last<=preview->chunk_end[k])) ++k;
        if (first>=last || k==preview->chunk_count) return -1;
        if (preview_v5_frame(state,preview->chunk[k]+(first-preview->chunk_begin[k]),last-first,preview->frame_buffer)<0) return -1;
    }
    if (pixels!=preview->frame_buffer) memcpy(pixels,preview->frame_buffer,(size_t)preview->frame_bytes);
    return 0;
}
/* "covers/NAME" under the root .ohmy3do, or just NAME when the media sits in
 * the .ohmy3do folder beside the current folder's discs. */
static void media_path(const Ode *ode,char *path,const char *folder,const char *filename)
{
    if (ode->media_beside) { strcpy(path,filename); return; }
    strcpy(path,folder); strcat(path,"/"); strcat(path,filename);
}
/* Payload checks. Covers use CRC32. Previews use a word sum, which costs
 * about a tenth as much on the ARM60 (CRC32 took longer than the USB read
 * itself): over big-endian 32-bit words, a += w; b += a; the stored value is
 * a ^ (b rotated by 16), all modulo 2^32. A preview payload is the frame
 * table, the audio, the video packets, then zero padding to a word boundary,
 * so every read is word-aligned and audio comes first. */
typedef struct PreviewCheck {
    int words, bad;
    unsigned long crc, a, b;
} PreviewCheck;
static void check_start(PreviewCheck *check,int words)
{
    memset(check,0,sizeof(*check)); check->words=words; check->crc=0xffffffffUL;
}
static void check_update(PreviewCheck *check,const unsigned char *data,size_t bytes)
{
    unsigned long a=check->a,b=check->b;
    size_t words=bytes>>2;
    if (!check->words) { check->crc=usb_art_crc32_update(check->crc,data,bytes); return; }
    if (bytes&3U) check->bad=1;
#ifdef __BIG_ENDIAN
    if (!((unsigned long)data&3UL)) {
        const unsigned long *w=(const unsigned long *)(const void *)data;
        while (words>=4) { a+=w[0]; b+=a; a+=w[1]; b+=a; a+=w[2]; b+=a; a+=w[3]; b+=a; w+=4; words-=4; }
        while (words--) { a+=*w++; b+=a; }
        check->a=a; check->b=b; return;
    }
#endif
    while (words--) {
        a+=((unsigned long)data[0]<<24)|((unsigned long)data[1]<<16)|((unsigned long)data[2]<<8)|data[3];
        b+=a; data+=4;
    }
    check->a=a; check->b=b;
}
static int check_matches(const PreviewCheck *check,unsigned long expected)
{
    unsigned long a=check->a&0xffffffffUL,b=check->b&0xffffffffUL;
    if (!check->words) return (check->crc^0xffffffffUL)==expected;
    return !check->bad && ((a^((b<<16)|(b>>16)))&0xffffffffUL)==expected;
}
static int preview_bytes(Ode *ode,OdeReadBuffer reader,unsigned char *data,unsigned long bytes,
                         PreviewCheck *check,MediaCancel cancel,void *context)
{
    unsigned long pos=0;
    while (pos<bytes) {
        size_t n=bytes-pos>2048 ? 2048 : (size_t)(bytes-pos);
        if ((cancel && cancel(context)) || usb_file_read(ode,reader,data+pos,n,USB_FILE_CRC_CHECKED)<0) return -1;
        check_update(check,data+pos,n); pos+=n;
    }
    return 0;
}
/* Reads source_bytes, keeping the first keep_bytes in linear (the rest is
 * only checked, so the file's checksum and footer still cover it). */
static int preview_stream(Ode *ode,OdeReadBuffer reader,unsigned char *linear,
    unsigned long source_bytes,unsigned long keep_bytes,PreviewCheck *check,MediaCancel cancel,void *context)
{
    unsigned long scratch_words[512];
    unsigned char *scratch=(unsigned char *)scratch_words;
    unsigned long pos=0;
    while (pos<source_bytes) {
        size_t n=source_bytes-pos>2048 ? 2048 : (size_t)(source_bytes-pos);
        size_t kept=pos<keep_bytes ? (keep_bytes-pos<n ? (size_t)(keep_bytes-pos) : n) : 0;
        if ((cancel && cancel(context)) || usb_file_read(ode,reader,scratch,n,USB_FILE_CRC_CHECKED)<0) return -1;
        check_update(check,scratch,n);
        if (kept) memcpy(linear+pos,scratch,kept);
        pos+=n;
    }
    return 0;
}
/* Reads the next 2 KiB of the video packets into the chunks, which
 * hold consecutive ranges; bytes past the frames kept are only checked. */
static int preview_block(Ode *ode,OdeReadBuffer reader,UsbPreview *next,PreviewCheck *check,MediaCancel cancel,void *context)
{
    unsigned long scratch_words[512];
    unsigned char *scratch=(unsigned char *)scratch_words;
    unsigned long pos=next->stream_pos,kept=next->video_bytes;
    size_t n=next->stream_end-pos>2048 ? 2048 : (size_t)(next->stream_end-pos),used=0;
    unsigned int k=next->stream_chunk;
    if ((cancel && cancel(context)) || usb_file_read(ode,reader,scratch,n,USB_FILE_CRC_CHECKED)<0) return -1;
    check_update(check,scratch,n);
    while (used<n && pos+used<kept) {
        unsigned long at=pos+used;
        size_t take=n-used;
        while (at>=next->chunk_end[k]) ++k;
        if (take>next->chunk_end[k]-at) take=(size_t)(next->chunk_end[k]-at);
        memcpy(next->chunk[k]+(at-next->chunk_begin[k]),scratch+used,take); used+=take;
    }
    next->stream_pos=pos+n; next->stream_chunk=k;
    return 0;
}
/* Keep the longest prefix of frames that fits, in up to
 * USB_PREVIEW_CHUNKS allocations. Each chunk holds whole packets plus
 * READ_AHEAD bytes, so a packet is always contiguous. VRAM is
 * filled first. Audio is allocated before the chunks claim DRAM. */
static int preview_store(Ode *ode,OdeReadBuffer reader,UsbPreview *next,unsigned long source_video,unsigned long source_audio,
    int sound,PreviewCheck *check,MediaCancel cancel,void *context)
{
    unsigned long capacity=chunk_capacity();
    unsigned int frame,plan=0,placed=0,k=0;
    for (frame=1;frame<=next->source_frames;++frame) {
        unsigned long audio=source_audio && sound ? (((frame*11025UL+next->fps-1)/next->fps)+3)&~3UL : 0;
        unsigned long need=word(next->table+frame*4)-next->table_bytes+audio+USB_PREVIEW_CHUNKS*READ_AHEAD;
        if (need<=capacity) plan=frame;
    }
    if (!plan) return USB_PREVIEW_NO_MEMORY;
    if (source_audio && sound) {
        next->audio_bytes=next->audio_frames=(((plan*11025UL+next->fps-1)/next->fps)+3)&~3UL;
        next->audio_rate=11025; next->audio=audio_memory(next->audio_bytes);
        if (!next->audio) return USB_PREVIEW_NO_MEMORY;
    }
    while (placed<plan && k<USB_PREVIEW_CHUNKS) {
        unsigned long vram_room=chunk_room(1),room,begin=word(next->table+placed*4),bytes;
        unsigned int last=placed;
        int vram=vram_room>=word(next->table+(placed+1)*4)-begin+READ_AHEAD;
        room=vram ? vram_room : chunk_room(0);
        while (last<plan && word(next->table+(last+1)*4)-begin+READ_AHEAD<=room) ++last;
        if (last==placed) break;
        bytes=word(next->table+last*4)-begin+READ_AHEAD;
        next->chunk[k]=chunk_alloc(bytes,vram);
        if (!next->chunk[k]) break;
        memset(next->chunk[k]+bytes-READ_AHEAD,0,READ_AHEAD);
        next->chunk_bytes[k]=bytes; next->chunk_vram[k]=(unsigned char)vram;
        next->chunk_begin[k]=begin; next->chunk_end[k]=word(next->table+last*4);
        if (vram) next->vram_bytes+=bytes;
        next->chunk_count=++k; placed=last;
    }
    if (!placed) return USB_PREVIEW_NO_MEMORY;
    next->frames=placed; next->video_bytes=word(next->table+placed*4);
    if (next->audio) next->audio_frames=(((placed*11025UL+next->fps-1)/next->fps)+3)&~3UL;
    /* The audio follows the frame table, so it is complete before any video. */
    if (preview_stream(ode,reader,next->audio,source_audio,next->audio_frames,check,cancel,context)<0) return -1;
    next->stream_pos=next->table_bytes; next->stream_end=source_video; next->stream_chunk=0;
    /* Read to the end, past any frames memory could not keep, for the check. */
    while (next->stream_pos<next->stream_end)
        if (preview_block(ode,reader,next,check,cancel,context)<0) return -1;
    return 0;
}
int usb_preview_load_options(Ode *ode,OdeReadBuffer reader,const char *filename,
    UsbPreview *preview,MediaCancel cancel,void *context,MediaLoadPhase phase,void *phase_context,int sound)
{
    unsigned char header[40];
    UsbPreview next;
    unsigned long bytes,source_video,source_audio,source_rate,source_samples,expected,pad;
    PreviewCheck check;
    unsigned int frame;
    char path[128];
    int result=-1;
    memset(&next,0,sizeof(next));
    if (!usb_media_filename(filename) || menu_path_compare(filename+strlen(filename)-4,".3dv")) return -1;
    media_path(ode,path,"previews",filename);
    if (usb_file_open(ode,path)<0) return -1;
    if (usb_file_read(ode,reader,header,40,1)<0 || memcmp(header,"OM3DVID5",8)) goto done;
    check_start(&check,1);
    next.width=header[8]*256U+header[9]; next.height=header[10]*256U+header[11];
    next.frames=next.source_frames=header[20]*256U+header[21]; next.fps=header[22]*256U+header[23];
    bytes=word(header+12);
    if (next.width!=PREVIEW_V5_WIDTH || next.height!=PREVIEW_V5_HEIGHT || next.fps!=30 ||
        !next.frames || next.frames>USB_PREVIEW_MAX_FRAMES || bytes>USB_PREVIEW_MAX_BYTES) goto done;
    next.frame_bytes=(unsigned long)next.width*next.height*2UL;
    source_video=word(header+24); source_audio=word(header+28);
    source_rate=word(header+32); source_samples=word(header+36);
    next.table_bytes=(next.source_frames+1)*4UL;
    /* Zero padding after the video packets ends on a word boundary. */
    pad=(4UL-(source_video&3UL))&3UL;
    if (source_video+pad>bytes || source_audio!=bytes-source_video-pad || source_video<=next.table_bytes) goto done;
    expected=(((next.source_frames*11025UL+next.fps-1)/next.fps)+3)&~3UL;
    if (source_audio ? (source_rate!=11025 || source_audio!=source_samples || source_samples!=expected) : (source_rate || source_samples)) goto done;
    next.decoder=calloc(1,sizeof(PreviewV5));
    next.frame_buffer=frame_memory(next.frame_bytes);
    next.table=(unsigned char *)malloc((size_t)next.table_bytes);
    if (!next.decoder || !next.frame_buffer || !next.table) { result=USB_PREVIEW_NO_MEMORY;goto done; }
    if (preview_bytes(ode,reader,next.table,next.table_bytes,&check,cancel,context)<0) goto done;
    if (word(next.table)!=next.table_bytes || word(next.table+next.source_frames*4)!=source_video) goto done;
    for (frame=0;frame<next.source_frames;++frame) {
        unsigned long begin=word(next.table+frame*4),end=word(next.table+(frame+1)*4);
        if (begin<next.table_bytes || begin>=end || end>source_video) goto done;
    }
    memset(next.frame_buffer,0,(size_t)next.frame_bytes);
    result=preview_store(ode,reader,&next,source_video+pad,source_audio,sound,&check,cancel,context);
    if (result) goto done;
    result=-1;
    if (phase) phase(phase_context,1);
    if ((cancel && cancel(context)) || !check_matches(&check,word(header+16)) ||
        usb_file_verify(ode,reader,(const unsigned char *)"END3DV05",8)<0 ||
        usb_preview_frame(&next,0,next.frame_buffer)<0) goto done;
    result=0;
done:
    if (usb_file_close(ode)<0) result=-1;
    if (phase) phase(phase_context,2);
    if (!result) { usb_preview_free(preview);*preview=next;memset(&next,0,sizeof(next)); }
    usb_preview_free(&next);return result;
}
int usb_preview_load_profiled(Ode *ode,OdeReadBuffer reader,const char *filename,
    UsbPreview *preview,MediaCancel cancel,void *context,MediaLoadPhase phase,void *phase_context)
{
    return usb_preview_load_options(ode,reader,filename,preview,cancel,context,phase,phase_context,1);
}
int usb_preview_load(Ode *ode,OdeReadBuffer reader,const char *filename,UsbPreview *preview,MediaCancel cancel,void *context)
{
    return usb_preview_load_options(ode,reader,filename,preview,cancel,context,NULL,NULL,1);
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
    media_path(ode,path,"covers",filename);
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

int usb_png_cover(Ode *ode,OdeReadBuffer reader,const char *filename,unsigned char *pixels,MediaCancel cancel,void *context)
{
    return png_pixels(ode,reader,filename,pixels,cancel,context,USB_COVER_WIDTH,USB_COVER_HEIGHT);
}

/* Prepared native covers skip PNG filters, scaling and per-pixel callbacks. */
/* A prepared OM3DCOV2 cover of exactly width x height pixels. */
static int prepared_cover(Ode *ode,OdeReadBuffer reader,const char *folder,const char *filename,unsigned char *pixels,
                          int width,int height,MediaCancel cancel,void *context)
{
    unsigned char header[24];
    unsigned char *data=NULL;
    tinfl_decompressor *decoder=NULL;
    unsigned long bytes, limit=(unsigned long)width*height*2;
    PreviewCheck check;
    size_t in_bytes,out_bytes=(size_t)limit;
    tinfl_status status;
    char path[128];
    int result=-1;
    if (!usb_media_filename(filename)) return -1;
    media_path(ode,path,folder,filename);
    if (usb_file_open(ode,path)<0) return -1;
    if (usb_file_read(ode,reader,header,sizeof(header),1)<0) goto done;
    if (memcmp(header,"OM3DCOV2",8) || header[8] || header[9]!=width ||
        header[10] || header[11]!=height || word(header+20)!=limit) goto done;
    bytes=word(header+12);
    /* zlib can grow incompressible pixels slightly past their raw size. */
    if (!bytes || bytes>(width==USB_COVER_WIDTH ? 32768UL : limit+1024UL)) goto done;
    data=(unsigned char *)malloc((size_t)bytes);decoder=(tinfl_decompressor *)malloc(sizeof(*decoder));
    check_start(&check,0);
    if (!data || !decoder || preview_bytes(ode,reader,data,bytes,&check,cancel,context)<0 ||
        !check_matches(&check,word(header+16)) || usb_file_verify(ode,reader,(const unsigned char *)"END3DC02",8)<0) goto done;
    in_bytes=(size_t)bytes;tinfl_init(decoder);
    status=tinfl_decompress(decoder,data,&in_bytes,pixels,pixels,&out_bytes,
        TINFL_FLAG_PARSE_ZLIB_HEADER|TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (status==TINFL_STATUS_DONE && in_bytes==bytes && out_bytes==limit) result=0;
done:
    free(data);free(decoder);
    if (usb_file_close(ode)<0) return -1;
    return result;
}
int usb_fast_cover(Ode *ode,OdeReadBuffer reader,const char *filename,unsigned char *pixels,MediaCancel cancel,void *context)
{
    return prepared_cover(ode,reader,"covers",filename,pixels,USB_COVER_WIDTH,USB_COVER_HEIGHT,cancel,context);
}
int usb_large_cover(Ode *ode,OdeReadBuffer reader,const char *filename,unsigned char *pixels,MediaCancel cancel,void *context)
{
    char name[104];
    size_t n=strlen(filename);
    if (n<5 || n>96 || filename[n-4]!='.') return -1;
    memcpy(name,filename,n-4); strcpy(name+n-4,"-large.3dc");
    return prepared_cover(ode,reader,"covers",name,pixels,USB_LARGE_COVER_WIDTH,USB_LARGE_COVER_HEIGHT,cancel,context);
}
