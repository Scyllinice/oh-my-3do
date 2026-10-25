/*
 * Reading and writing save archives (.3ds) on the USB drive.
 *
 * Archives live in .ohmy3do/saves: numbered backup slots ("saves/slot01.3ds"
 * up to "saves/slot999999999.3ds") and the automatic recovery copies
 * ("saves/recovery.3ds", "saves/recovery2.3ds"). Only those names are ever
 * read or written, so a mistake elsewhere in the menu can never overwrite
 * another file.
 *
 * Call these with the drive's root folder open; the caller is responsible
 * for returning to the folder it was showing (see art_navigation.h).
 */
#ifndef OH_MY_3DO_SAVE_USB_H
#define OH_MY_3DO_SAVE_USB_H
#include "save_archive.h"
#include "usb_file.h"

/* Reads the archive at `path`. Returns 0, or -1 if it is invalid, missing or
 * unreadable. */
int save_usb_load(Ode *ode, OdeReadBuffer reader, const char *path, SaveArchive *out);

/*
 * Reads the archive at `path`, about to be written to. Returns 0 for a valid
 * archive, 1 for an empty (or new) file, in which case `out` is emptied, or
 * -1 if it holds something else or cannot be read.
 */
int save_usb_load_target(Ode *ode, OdeReadBuffer reader, const char *path, SaveArchive *out);

/*
 * Writes `archive` to `path`, but only if the file still holds exactly
 * `expected` (or is still empty when `expected` is NULL): what the person
 * saw when they confirmed. Then reads it back to check. Returns 0, or -1.
 */
int save_usb_store_checked(Ode *ode, OdeReadBuffer reader, const char *path,
                           const SaveArchive *archive, const SaveArchive *expected);

/* Writes `archive` to `path` and reads it back to check. Returns 0, or -1. */
int save_usb_store(Ode *ode, OdeReadBuffer reader, const char *path, const SaveArchive *archive);

/*
 * Checks that the drive really stores what the menu writes: writes two
 * different test patterns to saves/usb-test.bin and reads each back.
 * Returns 0, or -1.
 */
int save_usb_test(Ode *ode, OdeReadBuffer reader);
#endif
