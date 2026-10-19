#include "cover_cache.h"
#include <stdlib.h>
#include <string.h>
void cover_cache_clear(CoverCache *cache)
{
    int i;
    for (i=0;i<COVER_CACHE_SLOTS;++i) free(cache->entries[i].pixels);
    memset(cache,0,sizeof(*cache));
}
static unsigned long tick(CoverCache *cache)
{
    int i;
    if (++cache->clock==0) {
        /* Wraparound is rare; reset ages without invalidating any pixels. */
        for (i=0;i<COVER_CACHE_SLOTS;++i) cache->entries[i].stamp=0;
        cache->clock=1;
    }
    return cache->clock;
}
int cover_cache_contains(const CoverCache *cache,unsigned long volume,const char *file)
{
    int i;
    for (i=0;i<COVER_CACHE_SLOTS;++i)
        if (cache->entries[i].pixels && cache->entries[i].volume==volume && !menu_path_compare(cache->entries[i].file,file)) return 1;
    return 0;
}
int cover_cache_get(CoverCache *cache,unsigned long volume,const char *file,unsigned char *pixels,size_t bytes)
{
    int i;
    for (i=0;i<COVER_CACHE_SLOTS;++i) {
        CoverCacheEntry *entry=&cache->entries[i];
        if (entry->pixels && entry->bytes==bytes && entry->volume==volume && !menu_path_compare(entry->file,file)) {
            memcpy(pixels,entry->pixels,bytes); entry->stamp=tick(cache); return 1;
        }
    }
    return 0;
}
void cover_cache_put(CoverCache *cache,unsigned long volume,const char *file,const unsigned char *pixels,size_t bytes)
{
    int i, slot=0;
    CoverCacheEntry *entry;
    if (!usb_media_filename(file) || bytes!=USB_COVER_PIXELS) return;
    for (i=0;i<COVER_CACHE_SLOTS;++i) {
        if (!cache->entries[i].pixels || (cache->entries[i].volume==volume && !menu_path_compare(cache->entries[i].file,file))) {
            slot=i; break;
        }
        if (cache->entries[i].stamp<cache->entries[slot].stamp) slot=i;
    }
    entry=&cache->entries[slot];
    if (!entry->pixels || entry->bytes<bytes) {
        unsigned char *next=(unsigned char *)realloc(entry->pixels,bytes);
        if (!next) return;
        entry->pixels=next;
    }
    if (!entry->pixels) return; /* Caching is optional under memory pressure. */
    entry->bytes=bytes; memcpy(entry->pixels,pixels,bytes); strcpy(entry->file,file);
    entry->volume=volume; entry->stamp=tick(cache);
}
