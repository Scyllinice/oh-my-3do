#include "usb_art.h"
#include "usb_file.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct FakeUsb {
    unsigned char index[4096], image[1024], buffer[2048], text[20000], blank[2048];
    size_t index_size, image_size, text_size, blank_size, position;
    const unsigned char *file;
    size_t size;
    int calls, fail_at, closed, opened, words, reads, writes, write_fail, on_blank;
    const char *folder; /* expected subfolder before .ohmy3do/ */
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
        const char *name=(const char *)fake->buffer;
        size_t name_length=(size_t)packet[2]*256+packet[3];
        assert(!packet[1] && !packet[4] && !packet[5] && !packet[6]);
        assert(name_length == strlen((char *)fake->buffer)+1);
        if (fake->folder) {
            assert(!strncmp(name,fake->folder,strlen(fake->folder)) && name[strlen(fake->folder)]=='/');
            name+=strlen(fake->folder)+1;
        }
        assert(!strncmp(name,".ohmy3do/",9)); name+=9;
        assert(!strncmp(name,"covers/",7) || !strcmp(name,"recent.txt") || !strcmp(name,"save-names.txt") ||
               (!strcmp(name,"blank.bin") && !fake->folder));
        fake->on_blank=!strcmp(name,"blank.bin");
        if (fake->on_blank) {
            fake->file=fake->blank; fake->size=fake->blank_size;
        } else if (!strcmp(name,"covers/index.3di")) {
            fake->file=fake->index; fake->size=fake->index_size;
        } else if (!strcmp(name,"save-names.txt")) {
            fake->file=fake->text; fake->size=fake->text_size;
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
    } else if (packet[0] == 0xe4) {
        /* The menu writes only its blank file of zeros. */
        size_t n=(size_t)packet[1]*256+packet[2];
        assert(fake->opened && fake->on_blank && fake->position+n <= sizeof(fake->blank));
        ++fake->writes;
        reply[1]=packet[1]; reply[2]=packet[2]; *length=4;
        if (fake->write_fail) reply[3]=0x10;
        else {
            memcpy(fake->blank+fake->position,fake->buffer,n); fake->position+=n;
            if (fake->position>fake->blank_size) fake->blank_size=fake->position;
        }
    } else if (packet[0] == 0xe5) {
        ++fake->closed; fake->opened=0;
    } else assert(0); /* No seek, playlist or navigation commands. */
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
    for (i=0;i<sizeof(fake->image);++i) fake->image[i]=(unsigned char)(i*7+3);
    fake->image_size=sizeof(fake->image);
    fake->blank_size=sizeof(fake->blank); /* made on an earlier read */
}

/* Opens a cover the way the menu's loaders do; the fake checks the path. */
static int open_cover(Ode *ode, const char *name)
{
    char path[128];
    int result;
    strcpy(path,"covers/"); strcat(path,name);
    result=usb_file_open(ode,path);
    if (!result) usb_file_close(ode);
    return result;
}

int main(void)
{
    FakeUsb fake;
    Ode ode;
    UsbArtIndex index;
    int calls, i;
    memset(&index,0,sizeof(index));
    setup(&fake,&ode);
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
    assert(!usb_art_index_load(&ode,reader,&index) && index.count==1);
    assert(!open_cover(&ode,index.entries[0].file) && fake.closed); usb_art_index_free(&index);
    /* A folder acting as its game: its media sits in that subfolder's .ohmy3do. */
    setup(&fake,&ode);
    ode.media_beside=1; ode.media_folder="Road Rash (USA)"; fake.folder="Road Rash (USA)";
    assert(!open_cover(&ode,"aaaaaaaaaaaaaaaaaaaaaaaa.3dc"));
    assert(!strcmp(ode.file_path,"Road Rash (USA)/.ohmy3do/covers/aaaaaaaaaaaaaaaaaaaaaaaa.3dc"));
    ode.media_beside=0; fake.folder=NULL; /* only beside reads use the subfolder */
    assert(!open_cover(&ode,"aaaaaaaaaaaaaaaaaaaaaaaa.3dc"));
    /* Preserve the attempted filename when E1 gets no reply. A blocked
     * follow-up (including diagnostic export) must not erase that evidence. */
    setup(&fake,&ode);
    fake.fail_at=(int)((strlen(".ohmy3do/covers/aaaaaaaaaaaaaaaaaaaaaaaa.3dc")+4)/4)+1;
    assert(open_cover(&ode,"aaaaaaaaaaaaaaaaaaaaaaaa.3dc")<0 && ode.navigation_failed);
    assert(!strcmp(ode.file_path,".ohmy3do/covers/aaaaaaaaaaaaaaaaaaaaaaaa.3dc"));
    calls=fake.calls;
    assert(usb_file_open(&ode,"diagnostics.txt")<0 && fake.calls==calls);
    assert(!strcmp(ode.file_path,".ohmy3do/covers/aaaaaaaaaaaaaaaaaaaaaaaa.3dc"));
    setup(&fake,&ode); fake.index_size=0;
    assert(usb_art_index_load(&ode,reader,&index) < 0 && !index.count);
    setup(&fake,&ode); --fake.index_size;
    assert(usb_art_index_load(&ode,reader,&index) < 0 && !index.count);
    setup(&fake,&ode); put32(fake.index+12,131073);
    assert(usb_art_index_load(&ode,reader,&index) < 0 && !index.count);
    /* Entries are checked on use: a bad one costs only its own game's media. */
    setup(&fake,&ode);
    assert(!usb_art_index_load(&ode,reader,&index));
    assert(!strcmp(usb_media_find(&index,"Games/Road Rash.iso"),"aaaaaaaaaaaaaaaaaaaaaaaa.3dc"));
    assert(!usb_media_find(&index,"Games/Other.iso")); usb_art_index_free(&index);
    setup(&fake,&ode); fake.index[28]='.'; fake.index[29]='.'; fake.index[30]='/';
    put32(fake.index+16,usb_art_crc32(fake.index+24,fake.index_size-24));
    assert(!usb_art_index_load(&ode,reader,&index) && index.count==1);
    assert(!usb_media_find(&index,"Games/Road Rash.iso")); usb_art_index_free(&index);
    setup(&fake,&ode); fake.index[28+20]='/';
    put32(fake.index+16,usb_art_crc32(fake.index+24,fake.index_size-24));
    assert(!usb_art_index_load(&ode,reader,&index) && index.count==1);
    assert(menu_cover_find(index.entries,index.count,"Games/Road Rash.iso") && !usb_media_find(&index,"Games/Road Rash.iso"));
    usb_art_index_free(&index);
    setup(&fake,&ode); strcpy(ode.identity,"OTHER!");
    assert(usb_art_index_load(&ode,reader,&index) < 0 && !fake.calls);
    {
        char text[512]; size_t length;
        setup(&fake,&ode); memcpy(fake.image,"Games/D.iso\n",12); fake.image_size=12;
        assert(!usb_file_text(&ode,reader,"recent.txt",text,sizeof(text),&length));
        assert(length==12 && !strcmp(text,"Games/D.iso\n") && fake.closed);
        assert(fake.reads==2 && fake.words<40 && !fake.writes); /* the blank file, then the text */
        calls=fake.calls;
        for (i=1;i<=calls;++i) {
            setup(&fake,&ode); fake.image_size=0; fake.fail_at=i;
            assert(usb_file_text(&ode,reader,"recent.txt",text,sizeof(text),&length)<0);
        }
        setup(&fake,&ode); memset(fake.image,'x',256); fake.image_size=256;
        assert(!usb_file_text(&ode,reader,"recent.txt",text,sizeof(text),&length));
        assert(length==256 && fake.reads==2 && text[256]==0);
        setup(&fake,&ode); memset(fake.image,'x',512); fake.image_size=512;
        assert(usb_file_text(&ode,reader,"recent.txt",text,sizeof(text),&length)<0 && fake.closed);
        setup(&fake,&ode); fake.image_size=0;
        assert(!usb_file_text(&ode,reader,"recent.txt",text,sizeof(text),&length) && !length);
    }
    {
        /* Long text: whole blocks after the first 2 KiB, the end over a
         * cleared buffer. Every result must match a byte-exact read. */
        static char text[20480];
        static const size_t sizes[] = {2047,2048,2049,4095,4096,4097,6143,6144,6145,10000,16378,20000};
        size_t length, k, j;
        for (k = 0; k < sizeof(sizes)/sizeof(sizes[0]); ++k) {
            setup(&fake,&ode); fake.text_size=sizes[k];
            for (j = 0; j < fake.text_size; ++j) fake.text[j]=(unsigned char)('a'+(j*7+j/13)%26);
            assert(!usb_file_text(&ode,reader,"save-names.txt",text,sizeof(text),&length));
            assert(length==sizes[k] && !memcmp(text,fake.text,length) && !text[length] && !fake.opened);
            if (sizes[k]==16378) assert(fake.calls < 150); /* 4,360 clearing it all in pieces */
        }
        /* A 16 KiB buffer, nearly full. */
        {
            static const size_t tight[] = {12000,16378,16383,16384};
            for (k = 0; k < sizeof(tight)/sizeof(tight[0]); ++k) {
                setup(&fake,&ode); fake.text_size=tight[k];
                for (j = 0; j < fake.text_size; ++j) fake.text[j]=(unsigned char)('a'+(j*7+j/13)%26);
                if (tight[k] == 16384) { assert(usb_file_text(&ode,reader,"save-names.txt",text,16384,&length)<0 && !fake.opened); continue; }
                assert(!usb_file_text(&ode,reader,"save-names.txt",text,16384,&length));
                assert(length==tight[k] && !memcmp(text,fake.text,length) && !text[length]);
                if (tight[k] == 16378) assert(fake.calls < 150);
            }
        }
        /* Text ends at a zero byte, also inside a complete later block. */
        setup(&fake,&ode); fake.text_size=9000;
        for (j = 0; j < fake.text_size; ++j) fake.text[j]=(unsigned char)('a'+j%26);
        fake.text[5000]=0;
        assert(!usb_file_text(&ode,reader,"save-names.txt",text,sizeof(text),&length));
        assert(length==5000 && !memcmp(text,fake.text,5000));
        /* Repeated blocks look like an end; the careful read still gets it right. */
        setup(&fake,&ode); fake.text_size=10000; memset(fake.text,'x',fake.text_size);
        assert(!usb_file_text(&ode,reader,"save-names.txt",text,sizeof(text),&length));
        assert(length==10000 && !memcmp(text,fake.text,length));
        /* Text that leaves no room for its terminator fails. */
        setup(&fake,&ode); fake.text_size=9000;
        for (j = 0; j < fake.text_size; ++j) fake.text[j]=(unsigned char)('a'+j%26);
        assert(usb_file_text(&ode,reader,"save-names.txt",text,9000,&length)<0 && !fake.opened);
        assert(!usb_file_text(&ode,reader,"save-names.txt",text,9001,&length) && length==9000);
        setup(&fake,&ode); fake.text_size=9000;
        for (j = 0; j < fake.text_size; ++j) fake.text[j]=(unsigned char)('a'+j%26);
        assert(!usb_file_text(&ode,reader,"save-names.txt",text,sizeof(text),&length));
        calls=fake.calls;
        for (i=1;i<=calls;++i) {
            setup(&fake,&ode); fake.text_size=9000; fake.fail_at=i;
            for (j = 0; j < fake.text_size; ++j) fake.text[j]=(unsigned char)('a'+j%26);
            assert(usb_file_text(&ode,reader,"save-names.txt",text,sizeof(text),&length)<0);
        }
    }
    {
        /* The blank file: made on first use, remade when not all zeros, and a
         * card that refuses the write falls back to clearing word by word. */
        char text[512]; size_t length; unsigned int k;
        setup(&fake,&ode); fake.blank_size=0; memcpy(fake.image,"Games/D.iso\n",12); fake.image_size=12;
        assert(!usb_file_text(&ode,reader,"recent.txt",text,sizeof(text),&length) && length==12 && !strcmp(text,"Games/D.iso\n"));
        assert(fake.writes==1 && fake.blank_size==2048);
        for (k=0;k<2048;++k) assert(!fake.blank[k]);
        setup(&fake,&ode); memset(fake.blank,'x',sizeof(fake.blank)); memcpy(fake.image,"Games/D.iso\n",12); fake.image_size=12;
        assert(!usb_file_text(&ode,reader,"recent.txt",text,sizeof(text),&length) && length==12);
        assert(fake.writes==1 && !fake.blank[0] && !fake.blank[2047]);
        setup(&fake,&ode); fake.blank_size=0; fake.write_fail=1; memcpy(fake.image,"Games/D.iso\n",12); fake.image_size=12;
        assert(!usb_file_text(&ode,reader,"recent.txt",text,sizeof(text),&length) && length==12 && !strcmp(text,"Games/D.iso\n"));
        assert(fake.writes==1);
        assert(!usb_file_text(&ode,reader,"recent.txt",text,sizeof(text),&length) && length==12 && fake.writes==1);
    }
    puts("USB text: bounded short reads, poisoned stale EOF, exact block boundaries, long text in whole blocks, the blank file and every I/O failure passed");
    puts("USB artwork: index CRC, bounds, layouts, media beside a game and I/O failures passed");
    return 0;
}
