/*
 * The PNG decoder for covers, from the vendored Pngle library.
 *
 * Including the library's source here compiles it as part of the menu with
 * the menu's own build settings. Gamma correction is turned off, so every
 * pixel comes out exactly as stored in the file, matching the desktop app's
 * prepared covers.
 */
#define PNGLE_NO_GAMMA_CORRECTION
#include "../vendor/pngle/pngle.c"
