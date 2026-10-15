#include "usb_art.h"
#include "usb_file.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct FakeUsb {
    unsigned char index[4096], image[24 + USB_ART_PIXELS], buffer[2048];
    size_t index_size, image_size, position;
    const unsigned char *file;
    size_t size;
    int calls, fail_at, closed, opened, words, reads;
} FakeUsb;

static void put32(unsigned char *p, unsigned long value)
{
    p[0]=(unsigned char)(value>>24); p[1]=(unsigned char)(value>>16);
    p[2]=(unsigned char)(value>>8); p[3]=(unsigned char)value;
}

static int fault(FakeUsb *fake) { return ++fake->calls == fake->fail_at; }

static int transfer(void *context, const unsigned char *packet,
                    unsigned char *reply, size_t capacity, size_t *length)
{
    FakeUsb *fake=(FakeUsb *)context;
    assert(capacity >= 4);
    if (fault(fake)) return -1;
    memset(reply,0,capacity);
    reply[0]=packet[0];
    *length=2;
    if (packet[0] == 0xe6) {
        size_t offset=((size_t)packet[1]*256+packet[2])*4;
        ++fake->words;
        assert(offset+4 <= sizeof(fake->buffer));
        memcpy(fake->buffer+offset,packet+3,4);
    } else if (packet[0] == 0xe1) {
        size_t name_length=(size_t)packet[2]*256+packet[3];
        assert(!packet[1] && !packet[4] && !packet[5] && !packet[6]);
        assert(name_length == strlen((char *)fake->buffer)+1);
        assert(!strncmp((char *)fake->buffer,"covers/",7) || !strcmp((char *)fake->buffer,"recent.txt"));
        if (!strcmp((char *)fake->buffer,"covers/index.3di")) {
            fake->file=fake->index; fake->size=fake->index_size;
        } else { fake->file=fake->image; fake->size=fake->image_size; }
        fake->position=0;
        fake->opened=1;
        reply[1]=1;
        *length=3;
    } else if (packet[0] == 0xe3) {
        size_t requested=(size_t)packet[1]*256+packet[2], actual=requested;
        ++fake->reads;
        assert(fake->opened && requested <= 2048);
        if (actual > fake->size-fake->position) actual=fake->size-fake->position;
        memcpy(fake->buffer,fake->file+fake->position,actual);
        fake->position+=actual;
        /* Leave the tail untouched, and lie about the count, just as FCare
         * does on EOF. The menu must validate size/header/CRC independently. */
        reply[1]=packet[1]; reply[2]=packet[2]; *length=4;
    } else if (packet[0] == 0xe5) {
        ++fake->closed; fake->opened=0;
    } else assert(0); /* No seek, file-write, playlist or navigation commands. */
    return 0;
}

static int reader(void *context, unsigned char *out, size_t size)
{
    FakeUsb *fake=(FakeUsb *)context;
    assert(size == 2048);
    if (fault(fake)) return -1;
    memcpy(out,fake->buffer,size);
    return 0;
}

static void setup(FakeUsb *fake, Ode *ode)
{
    static const char key[]="Games/Road Rash.iso";
    static const char name[]="aaaaaaaaaaaaaaaaaaaaaaaa.3dc";
    size_t payload=4+sizeof(key)+sizeof(name), i;
    memset(fake,0,sizeof(*fake));
    memset(ode,0,sizeof(*ode));
    memset(fake->buffer,0xa5,sizeof(fake->buffer));
    ode->transfer=transfer; ode->context=fake; strcpy(ode->identity,"LOCODE");
    memcpy(fake->index,"OM3DIDX1",8);
    put32(fake->index+8,1); put32(fake->index+12,(unsigned long)payload);
    fake->index[25]=sizeof(key); fake->index[27]=sizeof(name);
    memcpy(fake->index+28,key,sizeof(key));
    memcpy(fake->index+28+sizeof(key),name,sizeof(name));
    put32(fake->index+16,usb_art_crc32(fake->index+24,payload));
    fake->index_size=24+payload;
    memcpy(fake->image,"OM3DCOV1",8);
    fake->image[9]=64; fake->image[11]=80;
    put32(fake->image+12,USB_ART_PIXELS);
    for (i=0;i<USB_ART_PIXELS;++i) fake->image[24+i]=(unsigned char)(i*7+(i/2048)*13+3);
    put32(fake->image+16,usb_art_crc32(fake->image+24,USB_ART_PIXELS));
    fake->image_size=sizeof(fake->image);
}

static size_t load_file(const char *path, unsigned char *data, size_t capacity)
{
    FILE *file=fopen(path,"rb");
    size_t n;
    assert(file);
    n=fread(data,1,capacity,file);
    assert(!ferror(file) && fgetc(file) == EOF);
    fclose(file);
    return n;
}

int main(int argc, char **argv)
{
    FakeUsb fake;
    Ode ode;
    UsbArtIndex index;
    unsigned char pixels[USB_ART_PIXELS];
    int calls, i;
    memset(&index,0,sizeof(index));
    setup(&fake,&ode);
    if (argc == 3) {
        fake.index_size=load_file(argv[1],fake.index,sizeof(fake.index));
        fake.image_size=load_file(argv[2],fake.image,sizeof(fake.image));
        assert(usb_art_index_load(&ode,reader,&index) == 0 && index.count == 1);
        assert(usb_art_pixels(&ode,reader,index.entries[0].file,pixels) == 0);
        assert(!memcmp(pixels,fake.image+24,USB_ART_PIXELS));
        usb_art_index_free(&index);
        {
        char text[512]; size_t length;
        setup(&fake,&ode); memcpy(fake.image,"Games/D.iso\n",12); fake.image_size=12;
        assert(!usb_file_text(&ode,reader,"recent.txt",text,sizeof(text),&length));
        assert(length==12 && !strcmp(text,"Games/D.iso\n") && fake.closed);
        assert(fake.reads==1 && fake.words<40); /* formerly 128+ for this capacity, 512 for favorites */
        calls=fake.calls;
        for (i=1;i<=calls;++i) {
            setup(&fake,&ode); fake.image_size=0; fake.fail_at=i;
            assert(usb_file_text(&ode,reader,"recent.txt",text,sizeof(text),&length)<0);
        }
        setup(&fake,&ode); memset(fake.image,'x',256); fake.image_size=256;
        assert(!usb_file_text(&ode,reader,"recent.txt",text,sizeof(text),&length));
        assert(length==256 && fake.reads==3 && text[256]==0);
        setup(&fake,&ode); memset(fake.image,'x',512); fake.image_size=512;
        assert(usb_file_text(&ode,reader,"recent.txt",text,sizeof(text),&length)<0 && fake.closed);
        setup(&fake,&ode); fake.image_size=0;
        assert(!usb_file_text(&ode,reader,"recent.txt",text,sizeof(text),&length) && !length);
    }
    puts("USB text: bounded short reads, poisoned stale EOF, exact block boundaries and every I/O failure passed");
    puts("USB artwork: Python exporter -> C runtime roundtrip passed");
        return 0;
    }
    assert(usb_art_crc32((const unsigned char *)"123456789",9) == 0xcbf43926UL);
    assert(usb_art_index_load(&ode,reader,&index) == 0 && index.count == 1);
    assert(!strcmp(menu_cover_find(index.entries,index.count,"Games/Road Rash.iso"),
                   "aaaaaaaaaaaaaaaaaaaaaaaa.3dc"));
    calls=fake.calls;
    usb_art_index_free(&index);
    for (i=1;i<=calls;++i) {
        setup(&fake,&ode); fake.fail_at=i;
        assert(usb_art_index_load(&ode,reader,&index) < 0 && !index.count && !index.entries);
    }
    setup(&fake,&ode);
    assert(usb_art_pixels(&ode,reader,"aaaaaaaaaaaaaaaaaaaaaaaa.3dc",pixels) == 0);
    assert(!memcmp(pixels,fake.image+24,sizeof(pixels)));
    calls=fake.calls;
    for (i=1;i<=calls;++i) {
        setup(&fake,&ode); fake.fail_at=i;
        assert(usb_art_pixels(&ode,reader,"aaaaaaaaaaaaaaaaaaaaaaaa.3dc",pixels) < 0);
    }
    setup(&fake,&ode); fake.image_size=0;
    assert(usb_art_pixels(&ode,reader,"aaaaaaaaaaaaaaaaaaaaaaaa.3dc",pixels) < 0 && fake.closed);
    setup(&fake,&ode); fake.image_size-=7;
    assert(usb_art_pixels(&ode,reader,"aaaaaaaaaaaaaaaaaaaaaaaa.3dc",pixels) < 0);
    setup(&fake,&ode); fake.image[24+200] ^= 1;
    assert(usb_art_pixels(&ode,reader,"aaaaaaaaaaaaaaaaaaaaaaaa.3dc",pixels) < 0);
    setup(&fake,&ode); fake.image[9]=65;
    assert(usb_art_pixels(&ode,reader,"aaaaaaaaaaaaaaaaaaaaaaaa.3dc",pixels) < 0);
    setup(&fake,&ode); fake.index_size=0;
    assert(usb_art_index_load(&ode,reader,&index) < 0 && !index.count);
    setup(&fake,&ode); --fake.index_size;
    assert(usb_art_index_load(&ode,reader,&index) < 0 && !index.count);
    setup(&fake,&ode); put32(fake.index+12,131073);
    assert(usb_art_index_load(&ode,reader,&index) < 0 && !index.count);
    setup(&fake,&ode); fake.index[28]='.'; fake.index[29]='.'; fake.index[30]='/';
    put32(fake.index+16,usb_art_crc32(fake.index+24,fake.index_size-24));
    assert(usb_art_index_load(&ode,reader,&index) < 0 && !index.count);
    setup(&fake,&ode); strcpy(ode.identity,"OTHER!");
    assert(usb_art_index_load(&ode,reader,&index) < 0 && !fake.calls);
    assert(usb_art_pixels(&ode,reader,"../../bad",pixels) < 0 && !fake.calls);
    {
        char text[512]; size_t length;
        setup(&fake,&ode); memcpy(fake.image,"Games/D.iso\n",12); fake.image_size=12;
        assert(!usb_file_text(&ode,reader,"recent.txt",text,sizeof(text),&length));
        assert(length==12 && !strcmp(text,"Games/D.iso\n") && fake.closed);
        assert(fake.reads==1 && fake.words<40); /* formerly 128+ for this capacity, 512 for favorites */
        calls=fake.calls;
        for (i=1;i<=calls;++i) {
            setup(&fake,&ode); fake.image_size=0; fake.fail_at=i;
            assert(usb_file_text(&ode,reader,"recent.txt",text,sizeof(text),&length)<0);
        }
        setup(&fake,&ode); memset(fake.image,'x',256); fake.image_size=256;
        assert(!usb_file_text(&ode,reader,"recent.txt",text,sizeof(text),&length));
        assert(length==256 && fake.reads==3 && text[256]==0);
        setup(&fake,&ode); memset(fake.image,'x',512); fake.image_size=512;
        assert(usb_file_text(&ode,reader,"recent.txt",text,sizeof(text),&length)<0 && fake.closed);
        setup(&fake,&ode); fake.image_size=0;
        assert(!usb_file_text(&ode,reader,"recent.txt",text,sizeof(text),&length) && !length);
    }
    puts("USB text: bounded short reads, poisoned stale EOF, exact block boundaries and every I/O failure passed");
    puts("USB artwork: sequential reads, CRC, EOF, bounds and I/O failures passed");
    return 0;
}
