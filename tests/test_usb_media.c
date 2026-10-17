#include "usb_media.h"
#include "library_index.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef OH3DO_SDK
#include "mem.h"
static struct { void *data; int32 bytes; unsigned long flags; } sdk_memory[4];
static int sdk_calls,sdk_fail;
static unsigned long sdk_available=2UL*1024*1024;
void availMem(MemInfo *memory,unsigned long flags) { assert(flags==MEMTYPE_DRAM);memset(memory,0,sizeof(*memory));memory->minfo_SysFree=sdk_available;memory->minfo_SysLargest=8192; }
void *AllocMem(int32 bytes,unsigned long flags)
{
    int i;
    assert(bytes>0 && (flags==MEMTYPE_CEL || flags==MEMTYPE_AUDIO));
    if (++sdk_calls==sdk_fail) return NULL;
    for (i=0;i<4;++i) if (!sdk_memory[i].data) {
        sdk_memory[i].data=malloc((size_t)bytes);
        sdk_memory[i].bytes=bytes; sdk_memory[i].flags=flags;
        return sdk_memory[i].data;
    }
    assert(0); return NULL;
}
void FreeMem(void *data,int32 bytes)
{
    int i;
    for (i=0;i<4;++i) if (sdk_memory[i].data==data) {
        assert(bytes==sdk_memory[i].bytes);
        free(data);sdk_memory[i].data=NULL;return;
    }
    assert(0);
}
static void sdk_empty(void)
{
    int i;
    for (i=0;i<4;++i) assert(!sdk_memory[i].data);
}
#endif
typedef struct Fake {
    unsigned char *data, buffer[2048];
    size_t size, pos;
    int calls, fail_at, opened, closed, cancel_after;
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
        assert(!strncmp((char *)f->buffer,"covers/",7) || !strncmp((char *)f->buffer,"previews/",9) || !strcmp((char *)f->buffer,"library.3di") || !strcmp((char *)f->buffer,"metadata.3di"));
        f->pos=0; f->opened=1; reply[1]=1; *length=3;
    } else if (packet[0] == 0xe3) {
        size_t n=(size_t)packet[1]*256+packet[2], actual=n;
        assert(f->opened && n <= 2048);
        if (actual > f->size-f->pos) actual=f->size-f->pos;
        memcpy(f->buffer,f->data+f->pos,actual); f->pos+=actual;
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
    f->pos=0; f->calls=f->fail_at=f->opened=f->closed=f->cancel_after=0;
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
    int (*decode)(Ode *,OdeReadBuffer,const char *,unsigned char *,MediaCancel,void *)=usb_png_pixels;
    assert(argc >= 3); memset(&f,0,sizeof(f)); memset(&preview,0,sizeof(preview));
    assert(usb_art_crc32((const unsigned char *)"123456789",9)==0xcbf43926UL);
    f.data=load(argv[2],&f.size); original=f.size; reset(&f,&ode);
    if (!strcmp(argv[1],"cover")) decode=usb_png_cover;
    if (!strcmp(argv[1],"fastcover")) decode=usb_fast_cover;
    if (!strcmp(argv[1],"png") || !strcmp(argv[1],"cover") || !strcmp(argv[1],"fastcover")) {
        assert(argc == 4); expected=load(argv[3],&bytes); assert(bytes == (strcmp(argv[1],"png") ? USB_COVER_PIXELS : USB_ART_PIXELS));
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
    } else if (!strcmp(argv[1],"legacycover")) {
        size_t pixel;
        assert(!usb_legacy_cover(&ode,reader,"Game.3dc",pixels,cancel,&f));
        for (pixel=0;pixel<USB_COVER_PIXELS;pixel+=2) assert(pixels[pixel]==0x7c && !pixels[pixel+1]);
        reset(&f,&ode); f.cancel_after=10;
        assert(usb_legacy_cover(&ode,reader,"Game.3dc",pixels,cancel,&f)<0 && f.closed);
    } else if (!strcmp(argv[1],"smooth")) {
        unsigned int center;
        assert(!usb_png_cover(&ode,reader,"Game.png",pixels,cancel,&f));
        center=((unsigned int)pixels[((USB_COVER_HEIGHT/2)*USB_COVER_WIDTH+USB_COVER_WIDTH/2)*2]<<8)|pixels[((USB_COVER_HEIGHT/2)*USB_COVER_WIDTH+USB_COVER_WIDTH/2)*2+1];
        assert(center && center!=0x7fff && (center&31)==((center>>5)&31) && (center&31)==((center>>10)&31));
    } else if (!strcmp(argv[1],"badpng")) {
        assert(usb_png_pixels(&ode,reader,"Game.png",pixels,cancel,&f) < 0 && f.closed);
    } else if (!strcmp(argv[1],"badfastcover")) {
        assert(usb_fast_cover(&ode,reader,"Game.3dc",pixels,cancel,&f)<0 && f.closed);
    } else if (!strcmp(argv[1],"budgetvideo2")) {
        unsigned int frame;
#ifdef OH3DO_SDK
        sdk_available=192UL*1024;
#endif
        assert(!usb_preview_load(&ode,reader,"Game.3dv",&preview,cancel,&f));
        assert(preview.frames<preview.source_frames && preview.frames>1 && preview.block_count>1);
        assert(preview.video_bytes+preview.audio_bytes<=USB_PREVIEW_CACHE_BYTES);
#ifdef OH3DO_SDK
        assert(preview.video_bytes+preview.audio_bytes<=128UL*1024);
#endif
        for (frame=0;frame<preview.frames;++frame) {
            assert(!usb_preview_frame(&preview,frame,pixels));
            for (bytes=0;bytes<preview.frame_bytes;++bytes) assert(pixels[bytes]==(unsigned char)(bytes*73UL+bytes/17));
        }
        usb_preview_free(&preview);reset(&f,&ode);
        assert(!usb_preview_load_options(&ode,reader,"Game.3dv",&preview,cancel,&f,NULL,NULL,0));
        assert(!preview.audio && !preview.audio_bytes);usb_preview_free(&preview);
        reset(&f,&ode);f.data[original-9]^=1;
        assert(usb_preview_load(&ode,reader,"Game.3dv",&preview,cancel,&f)<0 && f.closed && !preview.blocks);
#ifdef OH3DO_SDK
        sdk_empty();
#endif
    } else if (!strcmp(argv[1],"badvideo2")) {
        assert(usb_preview_load(&ode,reader,"Game.3dv",&preview,cancel,&f)<0 && f.closed && !preview.pixels && !preview.audio);
    } else if (!strcmp(argv[1],"latebadvideo2")) {
        assert(!usb_preview_load(&ode,reader,"Game.3dv",&preview,cancel,&f));
        assert(preview.version==2 && preview.frames>1);
        assert(!usb_preview_frame(&preview,0,pixels));
        assert(usb_preview_frame(&preview,1,pixels)<0);
        usb_preview_free(&preview);
    } else if (!strcmp(argv[1],"video2") || !strcmp(argv[1],"video2large") || !strcmp(argv[1],"video2sdk")) {
        assert(!usb_preview_load_profiled(&ode,reader,"Game.3dv",&preview,cancel,&f,phase,&f));
        assert(read_phase==1 && check_phase==1);
        assert(preview.version==2 && preview.frames && preview.fps && preview.decoder);
        assert(!usb_preview_frame(&preview,0,pixels));
        assert(!memcmp(preview.frame_buffer,pixels,(size_t)preview.frame_bytes));
        if (argc==4) {
            expected=load(argv[3],&bytes);assert(bytes==preview.frame_bytes && !memcmp(pixels,expected,bytes));free(expected);
        }
        if (preview.audio_bytes) assert(preview.audio && preview.audio_rate==11025 && preview.audio_frames==preview.audio_bytes);
        calls=f.calls;usb_preview_free(&preview);
        assert(!preview.pixels && !preview.audio && !preview.decoder);
#ifdef OH3DO_SDK
        sdk_empty();
        if (!strcmp(argv[1],"video2sdk")) {
            for (i=1;i<=2;++i) {
                reset(&f,&ode);sdk_calls=0;sdk_fail=i;
                assert(usb_preview_load(&ode,reader,"Game.3dv",&preview,cancel,&f)==USB_PREVIEW_NO_MEMORY);
                assert(f.closed && !ode.navigation_failed && !preview.pixels && !preview.frame_buffer && !preview.audio);
                sdk_empty();
            }
            sdk_fail=0;
        }
#endif
        if (!strcmp(argv[1],"video2")) {
            for (i=1;i<=calls;++i) {
                reset(&f,&ode); f.fail_at=i;
                assert(usb_preview_load(&ode,reader,"Game.3dv",&preview,cancel,&f)<0 && !preview.pixels && !preview.audio);
            }
            for (i=1;i<=12;++i) {
                reset(&f,&ode);f.size=original-i;
                assert(usb_preview_load(&ode,reader,"Game.3dv",&preview,cancel,&f)<0 && f.closed && !preview.audio);
            }
            f.size=original;reset(&f,&ode);f.cancel_after=10;
            assert(usb_preview_load(&ode,reader,"Game.3dv",&preview,cancel,&f)<0 && f.closed && !ode.navigation_failed);
            reset(&f,&ode);f.data[original-9]^=1;
            assert(usb_preview_load(&ode,reader,"Game.3dv",&preview,cancel,&f)<0 && f.closed && !preview.pixels);
        }
    } else if (!strcmp(argv[1],"video")) {
        assert(!usb_preview_load(&ode,reader,"Game.3dv",&preview,cancel,&f));
        assert(preview.frames && preview.fps && !memcmp(preview.pixels,f.data+24,original-32));
        usb_preview_free(&preview); assert(!preview.pixels); calls=f.calls;
        for (i=1;i<=calls;++i) {
            reset(&f,&ode); f.fail_at=i;
            assert(usb_preview_load(&ode,reader,"Game.3dv",&preview,cancel,&f) < 0 && !preview.pixels);
        }
        for (i=1;i<=12;++i) {
            reset(&f,&ode); f.size=original-i;
            assert(usb_preview_load(&ode,reader,"Game.3dv",&preview,cancel,&f) < 0 && f.closed && !preview.pixels);
        }
        f.size=original; reset(&f,&ode); f.cancel_after=10;
        assert(usb_preview_load(&ode,reader,"Game.3dv",&preview,cancel,&f) < 0 && f.closed && !ode.navigation_failed);
        reset(&f,&ode); f.data[20]=1;
        assert(usb_preview_load(&ode,reader,"Game.3dv",&preview,cancel,&f) < 0);
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
