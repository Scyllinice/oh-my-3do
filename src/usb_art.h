#ifndef OH_MY_3DO_USB_ART_H
#define OH_MY_3DO_USB_ART_H
#include "menu_config.h"
#include "menu_ui.h"

#define USB_COVER_WIDTH  96
#define USB_COVER_HEIGHT 120
#define USB_COVER_PIXELS (USB_COVER_WIDTH * USB_COVER_HEIGHT * 2)
/* NAME-large.3dc beside NAME.png/.3dc: the same box art at twice the size, for the full-screen view. */
#define USB_LARGE_COVER_WIDTH  192
#define USB_LARGE_COVER_HEIGHT 240
#define USB_LARGE_COVER_PIXELS (USB_LARGE_COVER_WIDTH * USB_LARGE_COVER_HEIGHT * 2)
typedef struct UsbArtIndex {
    MenuCover *entries;
    size_t count;
    /* Header flag 1: each disc's media is in a .ohmy3do folder beside it,
     * which the ODE opens relative to the current folder. 0: root folders. */
    int beside;
    unsigned char *storage;
} UsbArtIndex;
int usb_media_filename(const char *name);
/* Loading checks only each entry's shape; a file name is checked here, on
 * use. NULL when the game has no entry or its file name is unusable. */
const char *usb_media_find(const UsbArtIndex *index, const char *path);
int usb_media_index_load(Ode *ode, OdeReadBuffer reader, UsbArtIndex *index, const char *folder);
void usb_art_index_free(UsbArtIndex *index);
/* All file operations here require the selected USB volume's root directory. */
int usb_art_index_load(Ode *ode, OdeReadBuffer reader, UsbArtIndex *index);
unsigned long usb_art_crc32(const unsigned char *data, size_t size);
unsigned long usb_art_crc32_update(unsigned long crc, const unsigned char *data, size_t size);
#endif
