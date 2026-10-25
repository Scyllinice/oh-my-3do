/*
 * A small least-recently-used cache of decoded cover pictures.
 * See cover_cache.h for why it exists and what it promises.
 */
#include "cover_cache.h"
#include <stdlib.h>
#include <string.h>

void cover_cache_clear(CoverCache *cache)
{
    int i;
    for (i = 0; i < COVER_CACHE_SLOTS; ++i)
        free(cache->entries[i].pixels);
    memset(cache, 0, sizeof(*cache));
}

/*
 * Advances the cache's clock and returns the new time, for stamping a cover
 * as just used.
 */
static unsigned long next_stamp(CoverCache *cache)
{
    int i;
    ++cache->clock;
    if (cache->clock == 0) {
        /*
         * The clock went past its largest value back to zero. That takes
         * billions of uses, so it is fine to forget which covers are older:
         * every stamp goes back to zero and the pixels themselves stay valid.
         */
        for (i = 0; i < COVER_CACHE_SLOTS; ++i)
            cache->entries[i].stamp = 0;
        cache->clock = 1;
    }
    return cache->clock;
}

/* Whether a slot holds the cover `file` on `volume`. File names are compared
 * the way the drive does, without regard to case. */
static int entry_is(const CoverCacheEntry *entry, unsigned long volume, const char *file)
{
    return entry->pixels && entry->volume == volume && !menu_path_compare(entry->file, file);
}

int cover_cache_contains(const CoverCache *cache, unsigned long volume, const char *file)
{
    int i;
    for (i = 0; i < COVER_CACHE_SLOTS; ++i) {
        if (entry_is(&cache->entries[i], volume, file))
            return 1;
    }
    return 0;
}

int cover_cache_get(CoverCache *cache, unsigned long volume, const char *file,
                    unsigned char *pixels, size_t bytes)
{
    int i;
    for (i = 0; i < COVER_CACHE_SLOTS; ++i) {
        CoverCacheEntry *entry = &cache->entries[i];
        if (entry->bytes == bytes && entry_is(entry, volume, file)) {
            memcpy(pixels, entry->pixels, bytes);
            entry->stamp = next_stamp(cache);
            return 1;
        }
    }
    return 0;
}

/*
 * Chooses the slot for a new cover: an empty slot, or the slot already
 * holding this cover, if there is one; otherwise the least recently used.
 */
static int choose_slot(const CoverCache *cache, unsigned long volume, const char *file)
{
    int i, oldest = 0;
    for (i = 0; i < COVER_CACHE_SLOTS; ++i) {
        const CoverCacheEntry *entry = &cache->entries[i];
        if (!entry->pixels || entry_is(entry, volume, file))
            return i;
        if (entry->stamp < cache->entries[oldest].stamp)
            oldest = i;
    }
    return oldest;
}

void cover_cache_put(CoverCache *cache, unsigned long volume, const char *file,
                     const unsigned char *pixels, size_t bytes)
{
    CoverCacheEntry *entry;

    /* Only full-size covers, whose names fit the entry's file field. */
    if (!usb_media_filename(file) || bytes != USB_COVER_PIXELS)
        return;

    entry = &cache->entries[choose_slot(cache, volume, file)];

    /* Make sure the slot has room, reusing its memory when it already does. */
    if (!entry->pixels || entry->bytes < bytes) {
        unsigned char *larger = (unsigned char *)realloc(entry->pixels, bytes);
        if (!larger)
            return; /* caching is optional when memory is short */
        entry->pixels = larger;
    }

    entry->bytes = bytes;
    memcpy(entry->pixels, pixels, bytes);
    strcpy(entry->file, file);
    entry->volume = volume;
    entry->stamp = next_stamp(cache);
}
