#ifndef OH3DO_LIBRARY_INDEX_H
#define OH3DO_LIBRARY_INDEX_H
#include "library_navigation.h"
#include "usb_media.h"
/* Requires the selected USB root. Atomic replacement; CRC and poisoned footer
 * reject corrupt/truncated reads. No directory scan or file writes.
 * 0 success, 1 cancelled, -1 invalid/I/O, -3 allocation failed. */
int library_index_load(Ode *ode,OdeReadBuffer reader,const char *prefix,
    LibraryCatalog *catalog,MediaCancel cancel,void *context);
#define LIBRARY_METADATA_MAX 2048
#define LIBRARY_METADATA_BYTES (192UL * 1024UL)
typedef struct LibraryGameInfo {
    const char *path, *genre, *publisher, *released;
} LibraryGameInfo;
typedef struct LibraryMetadata {
    LibraryGameInfo *entries;
    unsigned char *storage;
    size_t count;
} LibraryMetadata;
void library_metadata_free(LibraryMetadata *metadata);
/* Atomic replacement. Missing/corrupt optional metadata leaves the old index
 * intact. No game-selection I/O: lookup searches the resident index only. */
int library_metadata_parse(const unsigned char *data,size_t size,LibraryMetadata *metadata);
int library_metadata_load(Ode *ode,OdeReadBuffer reader,LibraryMetadata *metadata);
const LibraryGameInfo *library_metadata_find(const LibraryMetadata *metadata,const char *path);
/* Whether a library path is a relative path to a disc image the menu may
 * open: no empty, '.' or '..' parts, no control or reserved characters. */
int library_path_safe(const char *path);
#endif
