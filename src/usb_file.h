/*
 * Reading and writing files on the USB drive through the FCare.
 *
 * The FCare has a 2048-byte data buffer and a handful of file commands:
 *
 *   0xE1  open the file whose path is in the buffer (relative to the folder
 *         the FCare is in); a missing file is created, empty
 *   0xE3  read up to 2048 bytes from the open file into the buffer
 *   0xE4  write bytes from the buffer to the open file
 *   0xE5  close the file
 *   0xE6  put 4 bytes into the buffer at a given position
 *   0xE7  send the buffer to the console (see ode_bus_read_buffer)
 *
 * Its quirks shape everything here:
 *   - At the end of a file, a read still reports the full amount asked for,
 *     and leaves the rest of the buffer as it was. So the menu clears (or
 *     marks) the buffer before reading whenever it needs to know where a
 *     file really ends, or checks a CRC that a stale buffer would fail.
 *   - A file cannot be shortened or truncated: writing less than the file's
 *     size leaves its old end in place. Formats therefore record their own
 *     length (or end with a zero byte).
 *   - Writes are acknowledged before they are known to be complete, so
 *     anything written is closed, reopened and read back to check.
 *
 * All paths are inside a .ohmy3do folder: the one in the folder the FCare is
 * in (the drive root, usually), or, with ode->media_beside, the one beside
 * the current folder's discs (see ode.h).
 */
#ifndef OH_MY_3DO_USB_FILE_H
#define OH_MY_3DO_USB_FILE_H
#include "menu_config.h"

/*
 * Reads a text file (ending at its first zero byte or its real end) into
 * `out`, which holds `capacity` bytes including the terminating zero, and
 * stores its length. Returns 0, or -1 if it cannot be read or is too long.
 */
int usb_file_text(Ode *ode, OdeReadBuffer reader, const char *name, char *out, size_t capacity,
                  size_t *length);

/* Writes `text` and its terminating zero (which marks its end on later
 * reads), then reads it back to check. Returns 0, or -1. */
int usb_file_text_write(Ode *ode, OdeReadBuffer reader, const char *name, const char *text);

/* Opens .ohmy3do/NAME (see above). Returns 0, or -1. */
int usb_file_open(Ode *ode, const char *name);

/* Closes the open file. Returns 0, or -1. */
int usb_file_close(Ode *ode);

/*
 * The `header` setting of usb_file_read: how much of the FCare's buffer to
 * clear before each read, so a stale buffer at the end of a file cannot pass
 * for data. 1 clears the first 24 bytes before the first read (for a file's
 * fixed-size header), 0 clears the first word before each read, and
 * USB_FILE_CRC_CHECKED clears nothing: only for data whose whole-file CRC is
 * checked afterwards, which a stale buffer would fail anyway. Clearing costs
 * about a fifth of the read time.
 */
#define USB_FILE_CRC_CHECKED 2

/* Reads the next `size` bytes of the open file into `out`. Returns 0, or -1. */
int usb_file_read(Ode *ode, OdeReadBuffer reader, unsigned char *out, size_t size, int header);

/*
 * Reads the next 2048 bytes straight into `out`, with nothing cleared: only
 * for data whose checksum the caller verifies (a stale buffer fails it).
 */
int usb_file_read_block(Ode *ode, OdeReadBuffer reader, unsigned char *out);

/*
 * Reads up to 24 bytes after filling that part of the buffer with `fill`.
 * Bytes still equal to `fill` afterwards were probably not in the file,
 * which reveals where a short file really ends (see save_usb.c).
 */
int usb_file_probe(Ode *ode, OdeReadBuffer reader, unsigned char *out, size_t size,
                   unsigned char fill);

/*
 * Reads the next `size` bytes of the open file and checks they equal
 * `expected`. Returns 0 if they do, or -1.
 */
int usb_file_verify(Ode *ode, OdeReadBuffer reader, const unsigned char *expected, size_t size);

/*
 * Writes `size` bytes to the open file. The FCare only acknowledges the
 * command, so close, reopen and usb_file_verify afterwards. Returns 0, or -1.
 */
int usb_file_write(Ode *ode, const unsigned char *data, size_t size);
#endif
