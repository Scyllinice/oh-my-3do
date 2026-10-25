/*
 * Cover and preview indexes, and the CRC32 checksum used by every file
 * format the menu reads.
 *
 * The desktop app writes two indexes to the drive, .ohmy3do/covers/index.3di
 * and .ohmy3do/previews/index.3di, listing which disc has which cover or
 * preview file. Layout (numbers are 32-bit big-endian):
 *
 *   0   8 bytes  "OM3DIDX1"
 *   8   4        number of entries (at most 2048)
 *   12  4        payload size (at most 128 KiB)
 *   16  4        CRC32 of the payload
 *   20  4        where the media files are: 0 = in the root covers/ and
 *                previews/ folders, 1 = in the .ohmy3do folder beside each disc
 *   24  ...      payload: per entry, two 2-byte lengths (path and file name,
 *                each counting its zero byte), then the disc path and the
 *                media file name, each zero-terminated
 *
 * Entries are sorted by path without regard to case, for binary search.
 */
#ifndef OH_MY_3DO_USB_ART_H
#define OH_MY_3DO_USB_ART_H
#include "menu_config.h"
#include "menu_ui.h"

/* A cover as shown in the game list: 96x120 pixels, 2 bytes each. */
#define USB_COVER_WIDTH  96
#define USB_COVER_HEIGHT 120
#define USB_COVER_PIXELS (USB_COVER_WIDTH * USB_COVER_HEIGHT * 2)

/* NAME-large.3dc, beside NAME.png or NAME.3dc: the same box art at twice the
 * size, for the full-screen view. */
#define USB_LARGE_COVER_WIDTH  192
#define USB_LARGE_COVER_HEIGHT 240
#define USB_LARGE_COVER_PIXELS (USB_LARGE_COVER_WIDTH * USB_LARGE_COVER_HEIGHT * 2)

/* A loaded index. Its entries point into `storage`. */
typedef struct UsbArtIndex {
    MenuCover *entries; /* path -> media file name */
    size_t count;
    /* Header flag 1: each disc's media is in a .ohmy3do folder beside it,
     * which the ODE opens relative to the current folder. 0: root folders. */
    int beside;
    unsigned char *storage;
} UsbArtIndex;

/*
 * Whether a media file name is usable: 5 to 96 characters, ending in .3dc,
 * .png or .3dv, not starting with '.', and with no control characters or
 * path separators.
 */
int usb_media_filename(const char *name);

/*
 * The media file name for a disc path, or NULL if the disc has none or its
 * name is unusable. (Loading only checks each entry's shape; names are
 * checked here, when used.)
 */
const char *usb_media_find(const UsbArtIndex *index, const char *path);

/*
 * Loads covers/index.3di or previews/index.3di (`folder` is "covers" or
 * "previews"). Call with the drive's root folder open. The old index is
 * freed first; on failure the index stays empty. Returns 0, or -1.
 */
int usb_media_index_load(Ode *ode, OdeReadBuffer reader, UsbArtIndex *index, const char *folder);

/* Frees an index and leaves it empty. */
void usb_art_index_free(UsbArtIndex *index);

/* Loads the cover index (usb_media_index_load with "covers"). */
int usb_art_index_load(Ode *ode, OdeReadBuffer reader, UsbArtIndex *index);

/* The standard CRC32 (as used by zip and PNG) of `size` bytes. */
unsigned long usb_art_crc32(const unsigned char *data, size_t size);

/*
 * Continues a CRC32 over more bytes, for checksums computed piece by piece.
 * Start with 0xFFFFFFFF and finish by XORing with 0xFFFFFFFF.
 */
unsigned long usb_art_crc32_update(unsigned long crc, const unsigned char *data, size_t size);
#endif
