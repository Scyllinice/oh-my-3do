/*
 * Covers built into the menu itself.
 *
 * The normal menu has none: it reads covers from the USB drive. Only the
 * offline demo ISO, which runs without a drive, carries sample covers. For
 * that build, tools/build-covers.py writes a replacement of this file listing
 * them, and tools/build-menu compiles it instead.
 *
 * The array has one empty element because C does not allow empty arrays;
 * menu_cover_count says it holds no covers.
 */
#include "menu_ui.h"

const MenuCover menu_covers[1] = {{NULL, NULL}};
const size_t menu_cover_count = 0;
