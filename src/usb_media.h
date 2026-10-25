/*
 * Loading covers and gameplay previews from the USB drive.
 *
 * Covers are either PNG files or "prepared" covers (.3dc) that the desktop
 * app has already scaled and compressed for the console. Previews (.3dv) are
 * either short block-codec clips (OM3DVID5), loaded whole into memory, or
 * longer streamed clips (OM3DVID7), which preview_stream.h plays straight
 * from the drive. docs/media-format.md describes every format.
 *
 * Call the loaders with the folder holding the media open (the drive root,
 * or the disc's folder when media sits beside it; see ode.h). File names
 * come from the checked media indexes (usb_art.h).
 */
#ifndef OH3DO_USB_MEDIA_H
#define OH3DO_USB_MEDIA_H
#include "usb_art.h"

/* The preview picture: 160x120 pixels at 30 frames a second, 2 bytes each. */
#define USB_PREVIEW_WIDTH       160
#define USB_PREVIEW_HEIGHT      120
#define USB_PREVIEW_FRAME_BYTES (USB_PREVIEW_WIDTH * USB_PREVIEW_HEIGHT * 2UL)

/* Loader result: memory ran out (as distinct from a bad file, -1). */
#define USB_PREVIEW_NO_MEMORY (-3)

/* Loader result: the clip streams (OM3DVID7). Nothing was loaded; its first
 * 2 KiB are in UsbPreview.stream_block, for preview_stream_open. */
#define USB_PREVIEW_STREAM 2

/*
 * Limits on a loaded clip. Its video is stored in up to USB_PREVIEW_CHUNKS
 * large blocks of memory (video memory first, then main memory), each
 * holding whole frames plus the decoder's read-ahead.
 */
#define USB_PREVIEW_MAX_BYTES  (4096UL * 1024UL)
#define USB_PREVIEW_MAX_FRAMES 450
#define USB_PREVIEW_CHUNKS     8

/* The largest PNG cover file read. */
#define USB_PNG_MAX_BYTES (512UL * 1024UL)

/* Asked regularly during a load; returning nonzero stops it (the person
 * has moved on to another game). */
typedef int (*MediaCancel)(void *context);

/* Asked to free optional cached data (such as cached covers) when memory is
 * short. Returns nonzero only if it freed something. */
typedef int (*MediaReclaim)(void *context);

/* Optional timing hook: called with 1 after the payload is read and 2 after
 * it is checked and the file closed. */
typedef void (*MediaLoadPhase)(void *context, int checking);

/* A loaded (or streamed) preview clip. */
typedef struct UsbPreview {
    /* The frame table: source_frames + 1 offsets into the video bytes. */
    unsigned char *table;
    unsigned int source_frames; /* frames in the file */
    unsigned long table_bytes;
    unsigned int frames; /* frames loaded (memory may hold fewer than the file) */
    unsigned int fps;
    unsigned int width, height;
    unsigned int format;              /* 5: block codec, loaded; 7: streamed */
    unsigned char stream_block[2048]; /* format 7: the file's first 2 KiB */
    unsigned long frame_bytes;        /* size of one decoded frame */
    unsigned long video_bytes;        /* video bytes kept in the chunks */
    unsigned long audio_bytes, audio_frames, audio_rate;
    unsigned char *audio;        /* 8-bit samples, in sound memory */
    unsigned char *frame_buffer; /* the current decoded frame */
    void *decoder;               /* the block codec's state (PreviewV5) */
    /* The video, in up to USB_PREVIEW_CHUNKS pieces. */
    unsigned char *chunk[USB_PREVIEW_CHUNKS];
    unsigned long chunk_bytes[USB_PREVIEW_CHUNKS]; /* allocated size */
    unsigned long chunk_begin[USB_PREVIEW_CHUNKS]; /* video offsets each holds... */
    unsigned long chunk_end[USB_PREVIEW_CHUNKS];   /* ...up to here */
    unsigned char chunk_vram[USB_PREVIEW_CHUNKS];  /* 1 if in video memory */
    unsigned int chunk_count;
    unsigned long vram_bytes; /* of the chunks, for diagnostics */
    /* While loading: where the next 2 KiB of video is read from, and which
     * chunk it goes into. */
    unsigned int stream_chunk;
    unsigned long stream_pos, stream_end;
} UsbPreview;

/* Frees a clip and leaves it empty. */
void usb_preview_free(UsbPreview *preview);

/*
 * Decodes frame `frame` of a loaded clip into `pixels` (frame_bytes long).
 * Frames decode forward from the start, so going back (looping) starts
 * again from frame 0. Returns 0, or -1.
 */
int usb_preview_frame(UsbPreview *preview, unsigned int frame, unsigned char *pixels);

/* Loads a PNG cover, scaled to fit 96x120 and centered. Returns 0, or -1. */
int usb_png_cover(Ode *ode, OdeReadBuffer reader, const char *filename, unsigned char *pixels,
                  MediaCancel cancel, void *context);

/* Loads a prepared 96x120 cover (.3dc). Returns 0, or -1. */
int usb_fast_cover(Ode *ode, OdeReadBuffer reader, const char *filename, unsigned char *pixels,
                   MediaCancel cancel, void *context);

/* Loads the 192x240 NAME-large.3dc that goes with the cover NAME.png or
 * NAME.3dc, for the full-screen view. Returns 0, or -1. */
int usb_large_cover(Ode *ode, OdeReadBuffer reader, const char *filename, unsigned char *pixels,
                    MediaCancel cancel, void *context);

/*
 * Loads a preview clip, with sound. The old clip in `preview` is replaced
 * only on success. Returns 0, USB_PREVIEW_STREAM (format 7: play it with
 * preview_stream.h), USB_PREVIEW_NO_MEMORY, or -1. The variants add the
 * timing hook, a choice of sound, and a reclaim hook: reclaiming happens
 * only between steps, never in the middle of a USB read.
 */
int usb_preview_load(Ode *ode, OdeReadBuffer reader, const char *filename, UsbPreview *preview,
                     MediaCancel cancel, void *context);
int usb_preview_load_profiled(Ode *ode, OdeReadBuffer reader, const char *filename,
                              UsbPreview *preview, MediaCancel cancel, void *context,
                              MediaLoadPhase phase, void *phase_context);
int usb_preview_load_options(Ode *ode, OdeReadBuffer reader, const char *filename,
                             UsbPreview *preview, MediaCancel cancel, void *context,
                             MediaLoadPhase phase, void *phase_context, int sound);
int usb_preview_load_reclaim(Ode *ode, OdeReadBuffer reader, const char *filename,
                             UsbPreview *preview, MediaCancel cancel, void *context,
                             MediaLoadPhase phase, void *phase_context, int sound,
                             MediaReclaim reclaim, void *reclaim_context);

/* Declared in usb_art.h; repeated here for callers that only include this. */
int usb_media_index_load(Ode *ode, OdeReadBuffer reader, UsbArtIndex *index, const char *folder);
int usb_media_filename(const char *name);
#endif
