#ifndef OH3DO_SAVE_CHEATS_USB_H
#define OH3DO_SAVE_CHEATS_USB_H
#include "usb_file.h"
/* Call at the USB root before editing; missing/invalid files keep defaults. */
int save_cheats_usb_load(Ode *ode, OdeReadBuffer reader);
#endif
