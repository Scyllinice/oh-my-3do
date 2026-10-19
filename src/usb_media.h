#ifndef OH3DO_USB_MEDIA_H
#define OH3DO_USB_MEDIA_H
#include "usb_art.h"
/* The menu's preview screen; OM3DVID5 clips are 160x120 at 30 fps. */
#define USB_PREVIEW_WIDTH 160
#define USB_PREVIEW_HEIGHT 120
#define USB_PREVIEW_FRAME_BYTES (USB_PREVIEW_WIDTH * USB_PREVIEW_HEIGHT * 2UL)
#define USB_PREVIEW_NO_MEMORY (-3)
/* Clips are stored in a few large chunks (VRAM first, then DRAM),
 * each holding whole frames plus the decoder's read-ahead. */
#define USB_PREVIEW_MAX_BYTES (4096UL * 1024UL)
#define USB_PREVIEW_MAX_FRAMES 450
#define USB_PREVIEW_CHUNKS 8
#define USB_PNG_MAX_BYTES (512UL * 1024UL)
typedef int (*MediaCancel)(void *context);
typedef void (*MediaLoadPhase)(void *context,int checking);
typedef struct UsbPreview {
    /* The frame table: source_frames + 1 offsets into the video bytes. */
    unsigned char *table;
    unsigned int source_frames;
    unsigned long table_bytes;
    unsigned int frames, fps;
    unsigned int width, height;
    unsigned long frame_bytes;
    unsigned long video_bytes, audio_bytes, audio_frames, audio_rate;
    unsigned char *audio, *frame_buffer;
    void *decoder;
    unsigned char *chunk[USB_PREVIEW_CHUNKS];
    unsigned long chunk_bytes[USB_PREVIEW_CHUNKS]; /* allocated size */
    unsigned long chunk_begin[USB_PREVIEW_CHUNKS], chunk_end[USB_PREVIEW_CHUNKS]; /* video offsets held */
    unsigned char chunk_vram[USB_PREVIEW_CHUNKS];
    unsigned int chunk_count;
    unsigned long vram_bytes; /* of the chunks, for diagnostics */
    /* Loading: where the next 2 KiB of video packets are read, and into
     * which chunk. */
    unsigned int stream_chunk;
    unsigned long stream_pos, stream_end;
} UsbPreview;
void usb_preview_free(UsbPreview *preview);
/* 0 when decoded. */
int usb_preview_frame(UsbPreview *preview, unsigned int frame, unsigned char *pixels);
/* Call only while at the selected USB root; names come from checked indices. */
int usb_png_cover(Ode *ode, OdeReadBuffer reader, const char *filename,
                   unsigned char *pixels, MediaCancel cancel, void *context);
int usb_fast_cover(Ode *ode,OdeReadBuffer reader,const char *filename,unsigned char *pixels,MediaCancel cancel,void *context);
/* The 192x240 NAME-large.3dc beside the indexed cover NAME.png or NAME.3dc. */
int usb_large_cover(Ode *ode,OdeReadBuffer reader,const char *filename,unsigned char *pixels,MediaCancel cancel,void *context);
int usb_preview_load(Ode *ode, OdeReadBuffer reader, const char *filename,
                     UsbPreview *preview, MediaCancel cancel, void *context);
/* Returns USB_PREVIEW_NO_MEMORY for failed frame/video/audio allocations.
 * Optional timing hook: 1 after payload reads, 2 after checks/close. */
int usb_preview_load_profiled(Ode *ode,OdeReadBuffer reader,const char *filename,
    UsbPreview *preview,MediaCancel cancel,void *context,MediaLoadPhase phase,void *phase_context);
int usb_preview_load_options(Ode *ode,OdeReadBuffer reader,const char *filename,
    UsbPreview *preview,MediaCancel cancel,void *context,MediaLoadPhase phase,void *phase_context,int sound);
int usb_media_index_load(Ode *ode, OdeReadBuffer reader, UsbArtIndex *index, const char *folder);
int usb_media_filename(const char *name);
#endif
