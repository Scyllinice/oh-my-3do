/*
 * The decompressor Pngle uses to unpack PNG data, from the vendored miniz
 * library (public domain; its notice is in vendor/pngle).
 *
 * Including the library's source here compiles it as part of the menu with
 * the menu's own build settings.
 */
#include "../vendor/pngle/miniz.c"
