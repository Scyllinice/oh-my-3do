/*
 * A small cache of recently shown cover pictures.
 *
 * Reading a cover over the FCare's USB link takes a noticeable moment, and
 * people often scroll back and forth over the same few games. Keeping the last
 * few decoded covers in memory makes those returns instant.
 *
 * The cache keeps its own copy of each cover's pixels. It never holds on to a
 * CEL (the console's drawing object) or to memory owned by the media index,
 * so either can be freed at any time without leaving the cache pointing at
 * freed memory.
 */
#ifndef OH3DO_COVER_CACHE_H
#define OH3DO_COVER_CACHE_H
#include "usb_art.h"

/* How many covers to keep. Each one is a full 96x120 picture (about 23 KiB). */
#define COVER_CACHE_SLOTS 6

typedef struct CoverCacheEntry {
    char file[97];         /* the cover's file name, up to 96 bytes plus a zero */
    unsigned long volume;  /* which USB volume the file is on (top byte of its TOC id) */
    unsigned long stamp;   /* when it was last used, on the cache's own clock */
    size_t bytes;          /* size of `pixels` */
    unsigned char *pixels; /* decoded pixels, or NULL for an empty slot */
} CoverCacheEntry;

typedef struct CoverCache {
    CoverCacheEntry entries[COVER_CACHE_SLOTS];
    /* Counts every use. The least recently used cover has the lowest stamp
     * and is replaced first when the cache is full. */
    unsigned long clock;
} CoverCache;

/* Frees every cached cover and empties the cache. */
void cover_cache_clear(CoverCache *cache);

/* Whether the cover `file` on `volume` is cached. */
int cover_cache_contains(const CoverCache *cache, unsigned long volume, const char *file);

/*
 * Copies a cached cover's pixels into `pixels` (`bytes` long) and marks it as
 * just used. Returns 1 if it was cached, 0 if not.
 */
int cover_cache_get(CoverCache *cache, unsigned long volume, const char *file,
                    unsigned char *pixels, size_t bytes);

/*
 * Keeps a copy of a cover just read, replacing the least recently used one
 * when all slots are full. Only full-size covers with valid file names are
 * kept. Caching is optional: if memory is short, nothing happens.
 */
void cover_cache_put(CoverCache *cache, unsigned long volume, const char *file,
                     const unsigned char *pixels, size_t bytes);
#endif
