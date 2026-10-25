#ifndef OH_MY_3DO_USB_FILE_H
#define OH_MY_3DO_USB_FILE_H
#include "menu_config.h"
int usb_file_text(Ode *ode, OdeReadBuffer reader, const char *name, char *out, size_t capacity,
                  size_t *length);
/* Writes a terminating NUL, then reopens and verifies all bytes. */
int usb_file_text_write(Ode *ode, OdeReadBuffer reader, const char *name, const char *text);
int usb_file_open(Ode *ode, const char *name);
int usb_file_close(Ode *ode);
/* header: 1 clears the 24-byte header area before the first block, 0 clears
 * one word per block. USB_FILE_CRC_CHECKED clears nothing: only for payloads
 * whose whole-file CRC is verified, which catches a stale EOF buffer anyway.
 * The clears cost about a fifth of the read time. */
#define USB_FILE_CRC_CHECKED 2
int usb_file_read(Ode *ode, OdeReadBuffer reader, unsigned char *out, size_t size, int header);
/* The next 2048 bytes straight into out, with no clears and no copy: only for
 * data whose checksum the caller verifies (a stale EOF buffer fails it). */
int usb_file_read_block(Ode *ode, OdeReadBuffer reader, unsigned char *out);
/* Read a small header against a nonzero sentinel, exposing true EOF despite
 * firmware reporting the requested length on short reads. */
int usb_file_probe(Ode *ode, OdeReadBuffer reader, unsigned char *out, size_t size,
                   unsigned char fill);
int usb_file_verify(Ode *ode, OdeReadBuffer reader, const unsigned char *expected, size_t size);
int usb_file_write(Ode *ode, const unsigned char *data, size_t size);
#endif
