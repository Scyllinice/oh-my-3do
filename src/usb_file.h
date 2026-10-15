#ifndef OH_MY_3DO_USB_FILE_H
#define OH_MY_3DO_USB_FILE_H
#include "menu_config.h"
int usb_file_text(Ode *ode, OdeReadBuffer reader, const char *name, char *out, size_t capacity, size_t *length);
/* Writes a terminating NUL, then reopens and verifies all bytes. */
int usb_file_text_write(Ode *ode, OdeReadBuffer reader, const char *name, const char *text);
int usb_file_open(Ode *ode, const char *name);
int usb_file_close(Ode *ode);
int usb_file_read(Ode *ode, OdeReadBuffer reader, unsigned char *out, size_t size, int header);
int usb_file_verify(Ode *ode, OdeReadBuffer reader, const unsigned char *expected, size_t size);
int usb_file_write(Ode *ode, const unsigned char *data, size_t size);
#endif
