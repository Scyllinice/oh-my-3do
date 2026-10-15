#include "cover_cache.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    CoverCache cache;
    unsigned char pixels[USB_ART_PIXELS], out[USB_ART_PIXELS];
    char name[16];
    int i;
    memset(&cache,0,sizeof(cache)); memset(pixels,11,sizeof(pixels));
    assert(!cover_cache_get(&cache,1,"Game.png",out));
    cover_cache_put(&cache,1,"Game.png",pixels); memset(pixels,22,sizeof(pixels));
    assert(cover_cache_get(&cache,1,"GAME.PNG",out) && out[0]==11 && out[USB_ART_PIXELS-1]==11);
    assert(!cover_cache_get(&cache,2,"Game.png",out));
    cover_cache_put(&cache,2,"Game.png",pixels);
    assert(cover_cache_get(&cache,2,"Game.png",out) && out[0]==22);
    cover_cache_clear(&cache);
    for (i=0;i<COVER_CACHE_SLOTS;++i) {
        sprintf(name,"%d.png",i); memset(pixels,i,sizeof(pixels)); cover_cache_put(&cache,1,name,pixels);
    }
    assert(cover_cache_get(&cache,1,"0.png",out));
    memset(pixels,99,sizeof(pixels)); cover_cache_put(&cache,1,"new.png",pixels);
    assert(!cover_cache_contains(&cache,1,"1.png") && cover_cache_contains(&cache,1,"0.png"));
    assert(cover_cache_get(&cache,1,"new.png",out) && out[0]==99);
    cache.clock=ULONG_MAX;
    assert(cover_cache_get(&cache,1,"new.png",out) && cache.clock==1);
    cover_cache_put(&cache,1,"../bad.png",pixels); assert(!cover_cache_contains(&cache,1,"../bad.png"));
    cover_cache_clear(&cache); cover_cache_clear(&cache);
    for (i=0;i<COVER_CACHE_SLOTS;++i) assert(!cache.entries[i].pixels);
    puts("Cover cache: owned pixels, case-insensitive names, volume isolation, LRU, clock wrap and cleanup passed");
    return 0;
}
