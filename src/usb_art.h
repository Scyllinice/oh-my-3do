#ifndef OH_MY_3DO_USB_ART_H
#define OH_MY_3DO_USB_ART_H
#include "menu_config.h"
#include "menu_ui.h"

#define USB_ART_PIXELS (64 * 80 * 2)
#define USB_COVER_WIDTH 84
#define USB_COVER_HEIGHT 105
#define USB_COVER_PIXELS (USB_COVER_WIDTH * USB_COVER_HEIGHT * 2)
typedef struct UsbArtIndex {
    MenuCover *entries;
    size_t count;
    unsigned char *storage;
} UsbArtIndex;
int usb_media_filename(const char *name);
int usb_media_index_load(Ode *ode, OdeReadBuffer reader, UsbArtIndex *index, const char *folder);
void usb_art_index_free(UsbArtIndex *index);
/* All file operations here require the selected USB volume's root directory. */
int usb_art_index_load(Ode *ode, OdeReadBuffer reader, UsbArtIndex *index);
int usb_art_pixels(Ode *ode, OdeReadBuffer reader, const char *filename,
                   unsigned char *pixels);
unsigned long usb_art_crc32(const unsigned char *data, size_t size);
#endif
