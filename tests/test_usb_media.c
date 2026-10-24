#include "usb_media.h"
#include "preview_v5.h"
#include "preview_v7.h"
#include "library_index.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef OH3DO_SDK
#include "mem.h"
#define SDK_SLOTS 16
static struct { void *data; int32 bytes; unsigned long flags; } sdk_memory[SDK_SLOTS];
static int sdk_calls,sdk_fail;
static unsigned long sdk_available=2UL*1024*1024,sdk_largest=8192,sdk_vram=0,sdk_vram_largest=0;
static unsigned long sdk_bytes(unsigned long flags);
/* Free memory shrinks as the loader allocates; CEL and AUDIO memory is DRAM. */
void availMem(MemInfo *memory,unsigned long flags)
{
    unsigned long total=flags==MEMTYPE_VRAM ? sdk_vram : sdk_available,largest=flags==MEMTYPE_VRAM ? sdk_vram_largest : sdk_largest;
    unsigned long used=flags==MEMTYPE_VRAM ? sdk_bytes(MEMTYPE_VRAM) : sdk_bytes(MEMTYPE_DRAM)+sdk_bytes(MEMTYPE_CEL)+sdk_bytes(MEMTYPE_AUDIO);
    assert(flags==MEMTYPE_DRAM || flags==MEMTYPE_VRAM);
    memset(memory,0,sizeof(*memory));
    memory->minfo_SysFree=used<total ? total-used : 0;
    memory->minfo_SysLargest=largest<memory->minfo_SysFree ? largest : memory->minfo_SysFree;
}
void *AllocMem(int32 bytes,unsigned long flags)
{
    int i;
    assert(bytes>0 && (flags==MEMTYPE_CEL || flags==MEMTYPE_AUDIO || flags==MEMTYPE_DRAM || flags==MEMTYPE_VRAM));
    if (flags==MEMTYPE_VRAM) assert((unsigned long)bytes<=sdk_vram_largest);
    if (flags==MEMTYPE_DRAM) assert((unsigned long)bytes<=sdk_largest);
    if (++sdk_calls==sdk_fail) return NULL;
    for (i=0;i<SDK_SLOTS;++i) if (!sdk_memory[i].data) {
        sdk_memory[i].data=malloc((size_t)bytes);
        sdk_memory[i].bytes=bytes; sdk_memory[i].flags=flags;
        return sdk_memory[i].data;
    }
    assert(0); return NULL;
}
void FreeMem(void *data,int32 bytes)
{
    int i;
    for (i=0;i<SDK_SLOTS;++i) if (sdk_memory[i].data==data) {
        assert(bytes==sdk_memory[i].bytes);
        free(data);sdk_memory[i].data=NULL;return;
    }
    assert(0);
}
static void sdk_empty(void)
{
    int i;
    for (i=0;i<SDK_SLOTS;++i) assert(!sdk_memory[i].data);
}
static void *optional_cache;
static int reclaimed;
static unsigned long reclaim_largest;
static int reclaim_cache(void *context)
{
    (void)context;
    if (!optional_cache) return 0;
    FreeMem(optional_cache,65536);optional_cache=NULL;++reclaimed;
    if (reclaim_largest) sdk_largest=reclaim_largest;
    return 1;
}
static unsigned long sdk_bytes(unsigned long flags)
{
    unsigned long total=0; int i;
    for (i=0;i<SDK_SLOTS;++i) if (sdk_memory[i].data && sdk_memory[i].flags==flags) total+=(unsigned long)sdk_memory[i].bytes;
    return total;
}
#endif
typedef struct Fake {
    unsigned char *data, buffer[2048];
    size_t size, pos;
    int calls, fail_at, opened, closed, cancel_after, opens;
    int reads, stale_at; /* E3 count; the stale_at-th leaves the old buffer in place, as FCare can */
} Fake;
static int transfer(void *context,const unsigned char *packet,unsigned char *reply,size_t capacity,size_t *length)
{
    Fake *f=(Fake *)context;
    assert(capacity >= 4);
    if (++f->calls == f->fail_at) return -1;
    memset(reply,0,capacity); reply[0]=packet[0]; *length=2;
    if (packet[0] == 0xe6) {
        size_t offset=((size_t)packet[1]*256+packet[2])*4;
        assert(offset+4 <= sizeof(f->buffer)); memcpy(f->buffer+offset,packet+3,4);
    } else if (packet[0] == 0xe1) {
        const char *name=(const char *)f->buffer;
        assert(!strncmp(name,".ohmy3do/",9)); name+=9;
        assert(!strncmp(name,"covers/",7) || !strncmp(name,"previews/",9) || !strcmp(name,"library.3di") || !strcmp(name,"metadata.3di"));
        f->pos=0; f->opened=1; ++f->opens; reply[1]=1; *length=3;
    } else if (packet[0] == 0xe3) {
        size_t n=(size_t)packet[1]*256+packet[2], actual=n;
        assert(f->opened && n <= 2048);
        if (actual > f->size-f->pos) actual=f->size-f->pos;
        if (++f->reads != f->stale_at) memcpy(f->buffer,f->data+f->pos,actual);
        f->pos+=actual;
        reply[1]=packet[1]; reply[2]=packet[2]; *length=4;
    } else if (packet[0] == 0xe5) { f->opened=0; ++f->closed; }
    else assert(0); /* No writes, seeks, queue operations or navigation. */
    return 0;
}
static int reader(void *context,unsigned char *data,size_t size)
{
    Fake *f=(Fake *)context; assert(size == 2048);
    if (++f->calls == f->fail_at) return -1;
    memcpy(data,f->buffer,size); return 0;
}
static int cancel(void *context)
{
    Fake *f=(Fake *)context; return f->cancel_after && f->calls >= f->cancel_after;
}
static int read_phase, check_phase;
static void phase(void *context,int checking)
{
    Fake *f=(Fake *)context;
    if (checking==1) { assert(f->opened && f->pos+8==f->size && !read_phase && !check_phase); ++read_phase; }
    else { assert(checking==2 && !f->opened && f->closed && f->pos==f->size && read_phase==1 && !check_phase); ++check_phase; }
}
static void reset(Fake *f,Ode *ode)
{
    f->pos=0; f->calls=f->fail_at=f->opened=f->closed=f->cancel_after=f->opens=0; f->reads=f->stale_at=0;
    memset(f->buffer,0xa5,sizeof(f->buffer)); memset(ode,0,sizeof(*ode));
    ode->transfer=transfer; ode->context=f; strcpy(ode->identity,"LOCODE");
}
static unsigned char *load(const char *name,size_t *size)
{
    FILE *file=fopen(name,"rb"); unsigned char *data;
    assert(file && !fseek(file,0,SEEK_END)); *size=(size_t)ftell(file); rewind(file);
    data=(unsigned char *)malloc(*size+1); assert(data);
    assert(fread(data,1,*size,file) == *size); fclose(file); return data;
}
int main(int argc,char **argv)
{
    Fake f; Ode ode; UsbPreview preview; unsigned char pixels[USB_PREVIEW_FRAME_BYTES], *expected;
    size_t bytes, original; int calls, i;
    int (*decode)(Ode *,OdeReadBuffer,const char *,unsigned char *,MediaCancel,void *)=usb_png_cover;
    assert(argc >= 3); memset(&f,0,sizeof(f)); memset(&preview,0,sizeof(preview));
    assert(usb_art_crc32((const unsigned char *)"123456789",9)==0xcbf43926UL);
    f.data=load(argv[2],&f.size); original=f.size; reset(&f,&ode);
    if (!strcmp(argv[1],"fastcover")) decode=usb_fast_cover;
    if (!strcmp(argv[1],"png") || !strcmp(argv[1],"fastcover")) {
        assert(argc == 4); expected=load(argv[3],&bytes); assert(bytes == USB_COVER_PIXELS);
        assert(!decode(&ode,reader,"Game.png",pixels,cancel,&f));
        assert(!memcmp(pixels,expected,bytes)); free(expected); calls=f.calls;
        for (i=1;i<=calls;++i) {
            reset(&f,&ode); f.fail_at=i;
            assert(decode(&ode,reader,"Game.png",pixels,cancel,&f) < 0);
        }
        for (i=1;i<=12 && (size_t)i < original;++i) {
            reset(&f,&ode); f.size=original-i;
            assert(decode(&ode,reader,"Game.png",pixels,cancel,&f) < 0 && f.closed);
        }
        f.size=original; reset(&f,&ode); f.cancel_after=10;
        assert(decode(&ode,reader,"Game.png",pixels,cancel,&f) < 0 && f.closed && !ode.navigation_failed);
        reset(&f,&ode); f.data[original-1]^=1;
        assert(decode(&ode,reader,"Game.png",pixels,cancel,&f) < 0 && f.closed);
    } else if (!strcmp(argv[1],"smooth")) {
        unsigned int center;
        assert(!usb_png_cover(&ode,reader,"Game.png",pixels,cancel,&f));
        center=((unsigned int)pixels[((USB_COVER_HEIGHT/2)*USB_COVER_WIDTH+USB_COVER_WIDTH/2)*2]<<8)|pixels[((USB_COVER_HEIGHT/2)*USB_COVER_WIDTH+USB_COVER_WIDTH/2)*2+1];
        assert(center && center!=0x7fff && (center&31)==((center>>5)&31) && (center&31)==((center>>10)&31));
    } else if (!strcmp(argv[1],"badpng")) {
        assert(usb_png_cover(&ode,reader,"Game.png",pixels,cancel,&f) < 0 && f.closed);
    } else if (!strcmp(argv[1],"badfastcover")) {
        assert(usb_fast_cover(&ode,reader,"Game.3dc",pixels,cancel,&f)<0 && f.closed);
    } else if (!strcmp(argv[1],"largecover")) {
        static unsigned char large[USB_LARGE_COVER_PIXELS];
        assert(argc == 4); expected=load(argv[3],&bytes); assert(bytes == USB_LARGE_COVER_PIXELS);
        assert(!usb_large_cover(&ode,reader,"Game.png",large,cancel,&f) && !memcmp(large,expected,bytes));
        free(expected); calls=f.calls;
        for (i=1;i<=calls;++i) {
            reset(&f,&ode); f.fail_at=i;
            assert(usb_large_cover(&ode,reader,"Game.png",large,cancel,&f) < 0);
        }
        reset(&f,&ode); assert(usb_fast_cover(&ode,reader,"Game.3dc",pixels,cancel,&f) < 0 && f.closed);
        reset(&f,&ode); assert(usb_large_cover(&ode,reader,"Game",large,cancel,&f) < 0);
        reset(&f,&ode); f.data[original-1]^=1;
        assert(usb_large_cover(&ode,reader,"Game.3dc",large,cancel,&f) < 0 && f.closed);
    } else if (!strcmp(argv[1],"streamvideo")) {
        /* OM3DVID7 loads nothing: the header block comes back for the stream,
         * and the file is closed. A damaged header fails. */
        assert(usb_preview_load_options(&ode,reader,"Game.3dv",&preview,cancel,&f,NULL,NULL,1)==USB_PREVIEW_STREAM);
        assert(preview.format==7 && preview.width==320 && preview.height==240 && preview.frames);
        assert(!preview.decoder && !preview.frame_buffer && !preview.table && !preview.chunk_count && !preview.audio);
        assert(!memcmp(preview.stream_block,f.data,2048) && !f.opened && f.closed);
        usb_preview_free(&preview);
        reset(&f,&ode); f.data[100]^=1;
        assert(usb_preview_load_options(&ode,reader,"Game.3dv",&preview,cancel,&f,NULL,NULL,1)<0 && !f.opened);
    } else if (!strcmp(argv[1],"video") || !strcmp(argv[1],"budgetvideo")) {
        /* argv[3], if given, holds every frame as the encoder's reference decoder drew it. */
        unsigned long frame_bytes;
        unsigned int frame,full;
#ifdef OH3DO_SDK
        unsigned int baseline_frames;
#endif
        expected=NULL; bytes=0;
        if (argc==4) expected=load(argv[3],&bytes);
#ifdef OH3DO_SDK
        sdk_largest=512UL*1024; sdk_vram=sdk_vram_largest=768UL*1024;
        if (!strcmp(argv[1],"budgetvideo")) { sdk_available=sdk_largest=160UL*1024; sdk_vram=sdk_vram_largest=96UL*1024; }
#endif
        assert(!usb_preview_load_profiled(&ode,reader,"Game.3dv",&preview,cancel,&f,phase,&f));
        assert(read_phase==1 && check_phase==1);
        assert(preview.frames && preview.decoder && preview.chunk_count);
        frame_bytes=preview.frame_bytes;
        assert(frame_bytes==preview.width*preview.height*2UL && preview.fps==30);
        assert(!expected || (bytes%frame_bytes==0 && preview.source_frames==bytes/frame_bytes));
        full=preview.frames==preview.source_frames;
        if (!strcmp(argv[1],"budgetvideo")) assert(!full && preview.chunk_count>1);
        else assert(full);
#ifdef OH3DO_SDK
        assert(preview.chunk_vram[0] && preview.vram_bytes && sdk_bytes(MEMTYPE_VRAM)==preview.vram_bytes);
        if (!strcmp(argv[1],"budgetvideo")) assert(sdk_bytes(MEMTYPE_DRAM));
#else
        /* A fixture larger than one emulated chunk must span several. */
        assert(!preview.vram_bytes && (preview.video_bytes<=256UL*1024 || preview.chunk_count>1));
#endif
        for (frame=0;frame<preview.frames;++frame) {
            assert(!usb_preview_frame(&preview,frame,pixels));
            assert(!expected || !memcmp(pixels,expected+frame*frame_bytes,(size_t)frame_bytes));
        }
        /* Looping restarts from black; skipping ahead decodes the frames between. */
        assert(!usb_preview_frame(&preview,0,pixels) && (!expected || !memcmp(pixels,expected,(size_t)frame_bytes)));
        frame=preview.frames-1;
        assert(!usb_preview_frame(&preview,frame,pixels) && (!expected || !memcmp(pixels,expected+frame*frame_bytes,(size_t)frame_bytes)));
        assert(usb_preview_frame(&preview,preview.frames,pixels)<0);
        if (preview.audio) assert(preview.audio_rate==11025 && preview.audio_frames<=preview.audio_bytes && preview.audio_frames);
#ifdef OH3DO_SDK
        baseline_frames=preview.frames;
#endif
        calls=f.calls; usb_preview_free(&preview);
        assert(!preview.chunk_count && !preview.audio && !preview.decoder);
#ifdef OH3DO_SDK
        sdk_empty();
        /* Optional cover storage survives when the whole clip fits, and is
         * reclaimed before any loss of frames or firmware read retry. */
        optional_cache=AllocMem(65536,MEMTYPE_DRAM);assert(optional_cache);reclaimed=0;
        reset(&f,&ode);read_phase=check_phase=0;
        assert(!usb_preview_load_reclaim(&ode,reader,"Game.3dv",&preview,cancel,&f,phase,&f,1,reclaim_cache,NULL));
        assert(preview.frames==baseline_frames && f.calls==calls && f.opens==1);
        for (frame=0;frame<preview.frames;++frame) {
            assert(!usb_preview_frame(&preview,frame,pixels));
            assert(!expected || !memcmp(pixels,expected+frame*frame_bytes,(size_t)frame_bytes));
        }
        assert(reclaimed==(!strcmp(argv[1],"budgetvideo") ? 1 : 0));
        usb_preview_free(&preview);reclaim_cache(NULL);sdk_empty();
        if (!strcmp(argv[1],"video")) for (i=1;i<=4;++i) {
            optional_cache=AllocMem(65536,MEMTYPE_DRAM);assert(optional_cache);reclaimed=0;
            reset(&f,&ode);sdk_calls=0;sdk_fail=i;
            assert(!usb_preview_load_reclaim(&ode,reader,"Game.3dv",&preview,cancel,&f,NULL,NULL,1,reclaim_cache,NULL));
            assert(preview.frames==baseline_frames && f.calls==calls && f.opens==1 && !f.opened && f.closed);
            assert(!usb_preview_frame(&preview,preview.frames-1,pixels));
            assert(!expected || !memcmp(pixels,expected+(preview.frames-1)*frame_bytes,(size_t)frame_bytes));
            sdk_fail=0;usb_preview_free(&preview);reclaim_cache(NULL);sdk_empty();
        }
        if (!strcmp(argv[1],"video")) {
            unsigned long saved_vram=sdk_vram,saved_largest=sdk_largest;
            optional_cache=AllocMem(65536,MEMTYPE_DRAM);assert(optional_cache);
            sdk_vram=0;sdk_largest=4096;reclaim_largest=saved_largest;reclaimed=0;
            reset(&f,&ode);
            assert(!usb_preview_load_reclaim(&ode,reader,"Game.3dv",&preview,cancel,&f,NULL,NULL,1,reclaim_cache,NULL));
            assert(reclaimed==1 && preview.frames==baseline_frames && f.calls==calls && f.opens==1);
            usb_preview_free(&preview);sdk_empty();sdk_vram=saved_vram;reclaim_largest=0;
            optional_cache=AllocMem(65536,MEMTYPE_DRAM);assert(optional_cache);reclaimed=0;
            reset(&f,&ode);f.fail_at=calls/2;
            assert(usb_preview_load_reclaim(&ode,reader,"Game.3dv",&preview,cancel,&f,NULL,NULL,1,reclaim_cache,NULL)<0);
            assert(f.opens==1 && !reclaimed && !preview.chunk_count && f.calls<=f.fail_at+1);
            reclaim_cache(NULL);sdk_empty();
        }
        for (i=1;i<=24;++i) {
            reset(&f,&ode); sdk_calls=0; sdk_fail=i;
            frame=(unsigned int)usb_preview_load(&ode,reader,"Game.3dv",&preview,cancel,&f);
            /* A failed chunk shortens the clip; any other failure gives up cleanly. */
            assert(!frame || (int)frame==USB_PREVIEW_NO_MEMORY);
            assert(f.closed && !ode.navigation_failed);
            if (!frame) assert(preview.frames && !usb_preview_frame(&preview,preview.frames-1,pixels));
            else assert(!preview.chunk_count && !preview.audio && !preview.frame_buffer);
            usb_preview_free(&preview); sdk_empty();
        }
        sdk_fail=0;
#endif
        if (!strcmp(argv[1],"video")) {
            for (i=1;i<=calls;i+=calls/40+1) {
                reset(&f,&ode); f.fail_at=i;
                assert(usb_preview_load(&ode,reader,"Game.3dv",&preview,cancel,&f)<0 && !preview.chunk_count && !preview.audio);
            }
            for (i=1;i<=12;++i) {
                reset(&f,&ode); f.size=original-i;
                assert(usb_preview_load(&ode,reader,"Game.3dv",&preview,cancel,&f)<0 && f.closed && !preview.chunk_count);
            }
            /* Payload blocks skip the buffer clear; the file CRC must still catch a stale one. */
            for (i=2;i<=(int)(original/2048)+1;i+=(int)(original/2048)/6+1) {
                reset(&f,&ode); f.size=original; f.stale_at=i;
                assert(usb_preview_load(&ode,reader,"Game.3dv",&preview,cancel,&f)<0 && f.closed && !preview.chunk_count);
            }
            f.size=original; reset(&f,&ode); f.cancel_after=10;
            assert(usb_preview_load(&ode,reader,"Game.3dv",&preview,cancel,&f)<0 && f.closed && !ode.navigation_failed);
            reset(&f,&ode); f.data[original-9]^=1;
            assert(usb_preview_load(&ode,reader,"Game.3dv",&preview,cancel,&f)<0 && f.closed && !preview.chunk_count);
            f.data[original-9]^=1;
        }
#ifdef OH3DO_SDK
        sdk_empty();
#endif
        free(expected);
    } else if (!strcmp(argv[1],"badvideo")) {
        assert(usb_preview_load(&ode,reader,"Game.3dv",&preview,cancel,&f)<0 && f.closed && !preview.chunk_count && !preview.audio);
    } else if (!strcmp(argv[1],"latebadvideo")) {
        /* The first frame decodes during loading; a damaged later frame fails at playback. */
        unsigned int frame;
        assert(!usb_preview_load(&ode,reader,"Game.3dv",&preview,cancel,&f));
        for (frame=0;frame<preview.frames && !usb_preview_frame(&preview,frame,pixels);++frame) {}
        assert(frame>0 && frame<preview.frames);
        usb_preview_free(&preview);
    } else if (!strcmp(argv[1],"metadata")) {
        LibraryMetadata metadata;
        const LibraryGameInfo *info;
        memset(&metadata,0,sizeof(metadata));
        assert(!library_metadata_parse(f.data,f.size,&metadata));
        assert(metadata.count==2);
        info=library_metadata_find(&metadata,"games/usa/alpha/disc 1.ISO");
        assert(info && !strcmp(info->genre,"Action") && !strcmp(info->publisher,"Cafe Studio") && !strcmp(info->released,"1995-02-03"));
        assert(!library_metadata_find(&metadata,"Games/USA/Alpha/Disc 2.iso"));
        for (i=0;(size_t)i<f.size;++i) assert(library_metadata_parse(f.data,(size_t)i,&metadata)<0 && metadata.count==2);
        for (i=0;(size_t)i<f.size;++i) {
            f.data[i]^=1;
            assert(library_metadata_parse(f.data,f.size,&metadata)<0 && metadata.count==2);
            f.data[i]^=1;
        }
        assert(!library_metadata_load(&ode,reader,&metadata)); calls=f.calls;
        for (i=1;i<=calls;++i) {
            reset(&f,&ode); f.fail_at=i;
            assert(library_metadata_load(&ode,reader,&metadata)<0 && metadata.count==2);
        }
        for (i=1;i<=12;++i) {
            reset(&f,&ode); f.size=original-i;
            assert(library_metadata_load(&ode,reader,&metadata)<0 && f.closed && metadata.count==2);
        }
        f.size=original; library_metadata_free(&metadata); assert(!metadata.count && !metadata.entries && !metadata.storage);
    } else if (!strcmp(argv[1],"metadatavalid")) {
        LibraryMetadata metadata; memset(&metadata,0,sizeof(metadata));
        assert(!library_metadata_parse(f.data,f.size,&metadata));
        assert(!library_metadata_load(&ode,reader,&metadata));
        printf("Console metadata: %lu entries validated\n",(unsigned long)metadata.count);
        library_metadata_free(&metadata);
    } else if (!strcmp(argv[1],"badmetadata")) {
        LibraryMetadata metadata; memset(&metadata,0,sizeof(metadata));
        assert(library_metadata_parse(f.data,f.size,&metadata)<0 && !metadata.count);
        assert(library_metadata_load(&ode,reader,&metadata)<0 && f.closed && !metadata.count);
    } else if (!strcmp(argv[1],"emptylibrary")) {
        LibraryCatalog catalog;
        memset(&catalog,0,sizeof(catalog));
        assert(!library_index_load(&ode,reader,"/Storage",&catalog,cancel,&f));
        assert(catalog.valid && !catalog.count); library_catalog_free(&catalog);
    } else if (!strcmp(argv[1],"library") || !strcmp(argv[1],"badlibrary")) {
        LibraryCatalog catalog;
        memset(&catalog,0,sizeof(catalog));
        if (!strcmp(argv[1],"badlibrary")) {
            assert(library_index_load(&ode,reader,"/Storage",&catalog,cancel,&f)<0 && !catalog.valid);
        } else {
            assert(!library_index_load(&ode,reader,"/Storage/USB A",&catalog,cancel,&f));
            assert(catalog.valid && catalog.count==1 && !strcmp(catalog.paths[0],"/Storage/USB A/Game.iso"));
            calls=f.calls; library_catalog_free(&catalog);
            for (i=1;i<=calls;++i) {
                reset(&f,&ode); f.fail_at=i;
                assert(library_index_load(&ode,reader,"/Storage",&catalog,cancel,&f)<0 && !catalog.valid);
            }
            for (i=1;i<=8;++i) {
                reset(&f,&ode); f.size=original-i;
                assert(library_index_load(&ode,reader,"/Storage",&catalog,cancel,&f)<0 && !catalog.valid && f.closed);
            }
            f.size=original; reset(&f,&ode); f.cancel_after=10;
            assert(library_index_load(&ode,reader,"/Storage",&catalog,cancel,&f)==1 && f.closed && !catalog.valid);
        }
        library_catalog_free(&catalog);
    } else if (!strcmp(argv[1],"index")) {
        UsbArtIndex index; memset(&index,0,sizeof(index));
        assert(!usb_media_index_load(&ode,reader,&index,"covers"));
        assert(index.count == 1 && !strcmp(index.entries[0].file,argc>3 ? argv[3] : "Game.png"));
        usb_art_index_free(&index);
    } else assert(0);
    free(f.data); return 0;
}
