#ifndef OH3DO_COVER_CACHE_H
#define OH3DO_COVER_CACHE_H
#include "usb_art.h"
#define COVER_CACHE_SLOTS 6
typedef struct CoverCacheEntry {
    char file[97];
    unsigned long volume, stamp;
    size_t bytes;
    unsigned char *pixels;
} CoverCacheEntry;
typedef struct CoverCache {
    CoverCacheEntry entries[COVER_CACHE_SLOTS];
    unsigned long clock;
} CoverCache;
void cover_cache_clear(CoverCache *cache);
/* Copy decoded pixels only; never retains a CEL or a pointer to index storage. */
int cover_cache_contains(const CoverCache *cache,unsigned long volume,const char *file);
int cover_cache_get(CoverCache *cache,unsigned long volume,const char *file,unsigned char *pixels,size_t bytes);
void cover_cache_put(CoverCache *cache,unsigned long volume,const char *file,const unsigned char *pixels,size_t bytes);
#endif
