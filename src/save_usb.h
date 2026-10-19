#ifndef OH_MY_3DO_SAVE_USB_H
#define OH_MY_3DO_SAVE_USB_H
#include "save_archive.h"
#include "usb_file.h"
/* Call at the selected volume root; caller restores navigation. */
int save_usb_load(Ode *ode, OdeReadBuffer reader, const char *path, SaveArchive *out);
/* 0 valid archive, 1 physically empty/new file, -1 invalid or unreadable. */
int save_usb_load_target(Ode *ode, OdeReadBuffer reader, const char *path, SaveArchive *out);
/* Recheck reviewed destination before writing; NULL expected means empty. */
int save_usb_store_checked(Ode *ode, OdeReadBuffer reader, const char *path, const SaveArchive *archive, const SaveArchive *expected);
int save_usb_store(Ode *ode, OdeReadBuffer reader, const char *path, const SaveArchive *archive);
int save_usb_test(Ode *ode, OdeReadBuffer reader);
#endif
