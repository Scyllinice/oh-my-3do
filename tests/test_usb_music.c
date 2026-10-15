#include "usb_music.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct Fake {
    unsigned char *file, buffer[2048], *audio;
    size_t size,pos;
    int calls,fail_at,opened,closes,cancel_after,oom;
    UsbMusicSpec spec;
} Fake;
static int transfer(void *context,const unsigned char *p,unsigned char *reply,size_t capacity,size_t *length)
{
    Fake *f=(Fake *)context;
    assert(capacity>=4);
    if (++f->calls==f->fail_at) return -1;
    memset(reply,0,capacity);reply[0]=p[0];*length=2;
    if (p[0]==0xe6) {
        size_t offset=((size_t)p[1]*256+p[2])*4;
        assert(offset+4<=sizeof(f->buffer));memcpy(f->buffer+offset,p+3,4);
    } else if (p[0]==0xe1) {
        assert(!strcmp((char *)f->buffer,USB_MUSIC_PATH));f->pos=0;f->opened=1;reply[1]=1;*length=3;
    } else if (p[0]==0xe3) {
        size_t n=(size_t)p[1]*256+p[2],actual=n;
        assert(f->opened && n<=2048);
        if (actual>f->size-f->pos) actual=f->size-f->pos;
        memcpy(f->buffer,f->file+f->pos,actual);f->pos+=actual;
        reply[1]=p[1];reply[2]=p[2];*length=4;
    } else if (p[0]==0xe5) { f->opened=0;++f->closes; }
    else assert(0); /* No disk write, seek, playlist or mount commands. */
    return 0;
}
static int reader(void *context,unsigned char *data,size_t size)
{
    Fake *f=(Fake *)context;assert(size==2048);
    if (++f->calls==f->fail_at) return -1;
    memcpy(data,f->buffer,size);return 0;
}
static unsigned char *allocate(void *context,const UsbMusicSpec *spec)
{
    Fake *f=(Fake *)context;
    assert(!f->audio && spec->bytes<=USB_MUSIC_MAX_BYTES && spec->bytes==spec->frames/2);
    f->spec=*spec;
    if (f->oom) return NULL;
    f->audio=(unsigned char *)malloc((size_t)spec->bytes);assert(f->audio);return f->audio;
}
static int cancel(void *context) { Fake *f=(Fake *)context;return f->cancel_after && f->calls>=f->cancel_after; }
static void reset(Fake *f,Ode *ode)
{
    free(f->audio);f->audio=NULL;f->calls=f->opened=f->closes=f->fail_at=f->oom=f->cancel_after=0;f->pos=0;
    memset(f->buffer,0x73,sizeof(f->buffer));memset(ode,0,sizeof(*ode));strcpy(ode->identity,"LOCODE");ode->transfer=transfer;ode->context=f;
}
int main(int argc,char **argv)
{
    Fake f;Ode ode;FILE *file;long length;int calls,i;size_t original;
    assert(argc==2 || argc==3);memset(&f,0,sizeof(f));file=fopen(argv[1],"rb");assert(file);
    assert(!fseek(file,0,SEEK_END));length=ftell(file);assert(length>=36);rewind(file);
    f.size=(size_t)length;f.file=(unsigned char *)malloc(f.size);assert(f.file);assert(fread(f.file,1,f.size,file)==f.size);fclose(file);original=f.size;
    reset(&f,&ode);assert(!usb_music_load(&ode,reader,allocate,&f,cancel,&f));
    assert(f.closes && !f.opened && !memcmp(f.audio,f.file+24,f.spec.bytes));calls=f.calls;
    if (argc==3) {
        free(f.audio);free(f.file);puts("USB music: complete track validated");return 0;
    }
    for (i=1;i<=calls;++i) {
        reset(&f,&ode);f.fail_at=i;
        assert(usb_music_load(&ode,reader,allocate,&f,cancel,&f)<0);
    }
    for (i=1;i<=12;++i) {
        reset(&f,&ode);f.size=original-i;
        assert(usb_music_load(&ode,reader,allocate,&f,cancel,&f)<0 && f.closes);
    }
    f.size=original;reset(&f,&ode);f.cancel_after=20;
    assert(usb_music_load(&ode,reader,allocate,&f,cancel,&f)<0 && f.closes && !ode.navigation_failed);
    reset(&f,&ode);f.oom=1;
    assert(usb_music_load(&ode,reader,allocate,&f,cancel,&f)<0 && f.closes && !f.audio);
    reset(&f,&ode);f.file[20]^=1;
    assert(usb_music_load(&ode,reader,allocate,&f,cancel,&f)<0 && f.closes);f.file[20]^=1;
    reset(&f,&ode);f.file[16]^=0x80;
    assert(usb_music_load(&ode,reader,allocate,&f,cancel,&f)<0 && f.closes && !f.audio);f.file[16]^=0x80;
    free(f.audio);free(f.file);puts("USB music: CRC, stale EOF, allocation, cancellation and every I/O failure passed");return 0;
}
