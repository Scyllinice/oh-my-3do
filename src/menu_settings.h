/*
 * Saving the menu's settings back to the drive (Tools > Settings).
 */
#ifndef OH_MY_3DO_MENU_SETTINGS_H
#define OH_MY_3DO_MENU_SETTINGS_H
#include "usb_file.h"

/*
 * Writes `config` to .ohmy3do/ohmy3do.ini, then reads the file back and
 * compares it byte for byte. Returns 0, or -1 if a setting is out of range or
 * the write or check failed.
 *
 * The FCare cannot shorten a file, so writing a shorter file over a longer
 * one would leave the old file's end behind. The menu therefore always
 * writes exactly MENU_CONFIG_BYTES, padding after the end line with spaces,
 * which the reader ignores.
 */
int menu_settings_save(Ode *ode, OdeReadBuffer reader, const MenuConfig *config);
#endif
