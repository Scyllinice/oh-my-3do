#ifndef OH3DO_LIBRARY_INDEX_H
#define OH3DO_LIBRARY_INDEX_H
#include "library_navigation.h"
#include "usb_media.h"
/* Requires the selected USB root. Atomic replacement; CRC and poisoned footer
 * reject corrupt/truncated reads. No directory scan or file writes.
 * 0 success, 1 cancelled, -1 invalid/I/O, -3 allocation failed. */
int library_index_load(Ode *ode,OdeReadBuffer reader,const char *prefix,
    LibraryCatalog *catalog,MediaCancel cancel,void *context);
#endif
