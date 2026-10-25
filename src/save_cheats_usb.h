/*
 * Loading the save cheat database (.ohmy3do/save-cheats.3db) from the drive.
 */
#ifndef OH3DO_SAVE_CHEATS_USB_H
#define OH3DO_SAVE_CHEATS_USB_H
#include "usb_file.h"

/*
 * Reads the database and installs it with save_cheats_load. Call with the
 * drive's root folder open, before showing the cheat editor. A missing or
 * invalid file is not an error: the built-in profiles stay in use.
 * Returns 1 if the drive's database was installed, 0 otherwise.
 */
int save_cheats_usb_load(Ode *ode, OdeReadBuffer reader);
#endif
