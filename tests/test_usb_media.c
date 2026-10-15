#include "usb_media.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
        assert(!strncmp((char *)f->buffer,"covers/",7) || !strncmp((char *)f->buffer,"previews/",9));
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
    Fake f; Ode ode; UsbPreview preview; unsigned char pixels[USB_ART_PIXELS], *expected;
    size_t bytes, original; int calls, i;
    assert(argc >= 3); memset(&f,0,sizeof(f)); memset(&preview,0,sizeof(preview));
    f.data=load(argv[2],&f.size); original=f.size; reset(&f,&ode);
    if (!strcmp(argv[1],"png")) {
        assert(argc == 4); expected=load(argv[3],&bytes); assert(bytes == USB_ART_PIXELS);
        assert(!usb_png_pixels(&ode,reader,"Game.png",pixels,cancel,&f));
        assert(!memcmp(pixels,expected,bytes)); free(expected); calls=f.calls;
        for (i=1;i<=calls;++i) {
            reset(&f,&ode); f.fail_at=i;
            assert(usb_png_pixels(&ode,reader,"Game.png",pixels,cancel,&f) < 0);
        }
        for (i=1;i<=12 && (size_t)i < original;++i) {
            reset(&f,&ode); f.size=original-i;
            assert(usb_png_pixels(&ode,reader,"Game.png",pixels,cancel,&f) < 0 && f.closed);
        }
        f.size=original; reset(&f,&ode); f.cancel_after=10;
        assert(usb_png_pixels(&ode,reader,"Game.png",pixels,cancel,&f) < 0 && f.closed && !ode.navigation_failed);
        reset(&f,&ode); f.data[original-1]^=1;
        assert(usb_png_pixels(&ode,reader,"Game.png",pixels,cancel,&f) < 0 && f.closed);
    } else if (!strcmp(argv[1],"badpng")) {
        assert(usb_png_pixels(&ode,reader,"Game.png",pixels,cancel,&f) < 0 && f.closed);
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
    } else if (!strcmp(argv[1],"index")) {
        UsbArtIndex index; memset(&index,0,sizeof(index));
        assert(!usb_media_index_load(&ode,reader,&index,"covers"));
        assert(index.count == 1 && !strcmp(index.entries[0].file,"Game.png"));
        usb_art_index_free(&index);
    } else assert(0);
    free(f.data); return 0;
}
