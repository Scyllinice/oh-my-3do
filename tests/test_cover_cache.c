#include "cover_cache.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    CoverCache cache;
    unsigned char pixels[USB_ART_PIXELS], out[USB_ART_PIXELS];
    unsigned char native[USB_COVER_PIXELS], guarded[USB_COVER_PIXELS+2];
    char name[16];
    int i;
    memset(&cache,0,sizeof(cache)); memset(pixels,11,sizeof(pixels));
    assert(!cover_cache_get(&cache,1,"Game.png",out,USB_ART_PIXELS));
    cover_cache_put(&cache,1,"Game.png",pixels,USB_ART_PIXELS); memset(pixels,22,sizeof(pixels));
    assert(cover_cache_get(&cache,1,"GAME.PNG",out,USB_ART_PIXELS) && out[0]==11 && out[USB_ART_PIXELS-1]==11);
    assert(!cover_cache_get(&cache,2,"Game.png",out,USB_ART_PIXELS));
    cover_cache_put(&cache,2,"Game.png",pixels,USB_ART_PIXELS);
    assert(cover_cache_get(&cache,2,"Game.png",out,USB_ART_PIXELS) && out[0]==22);
    cover_cache_clear(&cache);
    for (i=0;i<COVER_CACHE_SLOTS;++i) {
        sprintf(name,"%d.png",i); memset(pixels,i,sizeof(pixels)); cover_cache_put(&cache,1,name,pixels,USB_ART_PIXELS);
    }
    assert(cover_cache_get(&cache,1,"0.png",out,USB_ART_PIXELS));
    memset(pixels,99,sizeof(pixels)); cover_cache_put(&cache,1,"new.png",pixels,USB_ART_PIXELS);
    assert(!cover_cache_contains(&cache,1,"1.png") && cover_cache_contains(&cache,1,"0.png"));
    assert(cover_cache_get(&cache,1,"new.png",out,USB_ART_PIXELS) && out[0]==99);
    cache.clock=ULONG_MAX;
    assert(cover_cache_get(&cache,1,"new.png",out,USB_ART_PIXELS) && cache.clock==1);
    cover_cache_put(&cache,1,"../bad.png",pixels,USB_ART_PIXELS); assert(!cover_cache_contains(&cache,1,"../bad.png"));
    memset(native,211,sizeof(native)); memset(guarded,0xa5,sizeof(guarded));
    cover_cache_put(&cache,1,"new.png",native,USB_COVER_PIXELS);
    assert(!cover_cache_get(&cache,1,"new.png",out,USB_ART_PIXELS));
    assert(cover_cache_get(&cache,1,"new.png",guarded+1,USB_COVER_PIXELS));
    assert(guarded[0]==0xa5 && guarded[sizeof(guarded)-1]==0xa5 && guarded[1]==211);
    cover_cache_clear(&cache); cover_cache_clear(&cache);
    for (i=0;i<COVER_CACHE_SLOTS;++i) assert(!cache.entries[i].pixels);
    puts("Cover cache: owned pixels, case-insensitive names, volume isolation, LRU, clock wrap and cleanup passed");
    return 0;
}
