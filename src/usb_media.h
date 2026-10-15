#ifndef OH3DO_USB_MEDIA_H
#define OH3DO_USB_MEDIA_H
#include "usb_art.h"
#define USB_PREVIEW_MAX_FRAMES 32
#define USB_PNG_MAX_BYTES (512UL * 1024UL)
typedef int (*MediaCancel)(void *context);
typedef struct UsbPreview {
    unsigned char *pixels;
    unsigned int frames, fps;
} UsbPreview;
void usb_preview_free(UsbPreview *preview);
/* Call only while at the selected USB root; names come from checked indices. */
int usb_png_pixels(Ode *ode, OdeReadBuffer reader, const char *filename,
                   unsigned char *pixels, MediaCancel cancel, void *context);
int usb_preview_load(Ode *ode, OdeReadBuffer reader, const char *filename,
                     UsbPreview *preview, MediaCancel cancel, void *context);
int usb_media_index_load(Ode *ode, OdeReadBuffer reader, UsbArtIndex *index, const char *folder);
int usb_media_filename(const char *name);
#endif
