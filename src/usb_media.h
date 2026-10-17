#ifndef OH3DO_USB_MEDIA_H
#define OH3DO_USB_MEDIA_H
#include "usb_art.h"
#define USB_PREVIEW_MAX_FRAMES 32
#define USB_PREVIEW_V2_MAX_FRAMES 720
#define USB_PREVIEW_MAX_BYTES (1024UL * 1024UL)
#define USB_PREVIEW_WIDTH 160
#define USB_PREVIEW_HEIGHT 120
#define USB_PREVIEW_FRAME_BYTES (USB_PREVIEW_WIDTH * USB_PREVIEW_HEIGHT * 2UL)
#define USB_PNG_MAX_BYTES (512UL * 1024UL)
typedef int (*MediaCancel)(void *context);
typedef void (*MediaLoadPhase)(void *context,int checking);
typedef struct UsbPreview {
    unsigned char *pixels;
    unsigned int frames, fps;
    unsigned int width, height;
    unsigned long frame_bytes;
    unsigned int version;
    unsigned long video_bytes, audio_bytes, audio_frames, audio_rate;
    unsigned char *audio, *frame_buffer;
    void *decoder;
} UsbPreview;
void usb_preview_free(UsbPreview *preview);
int usb_preview_frame(UsbPreview *preview, unsigned int frame, unsigned char *pixels);
/* Call only while at the selected USB root; names come from checked indices. */
int usb_png_pixels(Ode *ode, OdeReadBuffer reader, const char *filename,
                   unsigned char *pixels, MediaCancel cancel, void *context);
int usb_png_cover(Ode *ode, OdeReadBuffer reader, const char *filename,
                   unsigned char *pixels, MediaCancel cancel, void *context);
int usb_legacy_cover(Ode *ode, OdeReadBuffer reader, const char *filename,
                   unsigned char *pixels, MediaCancel cancel, void *context);
int usb_preview_load(Ode *ode, OdeReadBuffer reader, const char *filename,
                     UsbPreview *preview, MediaCancel cancel, void *context);
/* Optional timing hook: 1 after payload reads, 2 after checks/close. */
int usb_preview_load_profiled(Ode *ode,OdeReadBuffer reader,const char *filename,
    UsbPreview *preview,MediaCancel cancel,void *context,MediaLoadPhase phase,void *phase_context);
int usb_media_index_load(Ode *ode, OdeReadBuffer reader, UsbArtIndex *index, const char *folder);
int usb_media_filename(const char *name);
#endif
