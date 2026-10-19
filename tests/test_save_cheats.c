#include "save_cheats.h"
#include "usb_art.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void fixture(SaveEntry *entry, unsigned char data[40])
{
    unsigned long words[10]={0x4c57UL,2,1,15,15,3,1,2,0,0};
    unsigned long sum=12345;
    size_t i;
    memset(entry,0,sizeof(*entry)); strcpy(entry->name,"DoomPrefs"); entry->size=40; entry->data=data;
    for (i=0;i<9;++i) sum+=words[i];
    words[9]=sum;
    for (i=0;i<10;++i) save_put_word(data+i*4,words[i]);
}
int main(int argc, char **argv)
{
    SaveEntry entry;
    unsigned char data[40], before[40];
    const SaveCheatProfile *p;
    size_t i;
    fixture(&entry,data); p=save_cheats_find(&entry);
    assert(p && !strcmp(p->game,"DOOM") && p->field_count==8 && p->preset_count==1);
    assert(!save_cheats_preset(&entry,p,0));
    assert(save_word(data+24)==23 && save_word(data+36)==12345+0x4c57UL+2+1+15+15+3+23+2);
    assert(save_cheats_find(&entry)==p);
    for (i=0;i<p->field_count;++i) {
        fixture(&entry,data);
        assert(!save_cheats_set(&entry,p,i,p->fields[i].min));
        assert(save_cheats_find(&entry)==p);
        assert(!save_cheats_set(&entry,p,i,p->fields[i].max));
        assert(save_cheats_find(&entry)==p);
        memcpy(before,data,40);
        assert(save_cheats_set(&entry,p,i,p->fields[i].max+1)<0 && !memcmp(before,data,40));
    }
    assert(save_cheats_set(&entry,p,8,0)<0 && !memcmp(before,data,40));
    assert(save_cheats_preset(&entry,p,1)<0 && !memcmp(before,data,40));
    fixture(&entry,data); data[39]^=1; memcpy(before,data,40);
    assert(!save_cheats_find(&entry) && save_cheats_preset(&entry,p,0)<0 && !memcmp(before,data,40));
    fixture(&entry,data); data[0]^=1; assert(!save_cheats_find(&entry));
    fixture(&entry,data); entry.size=39; assert(!save_cheats_find(&entry));
    fixture(&entry,data); strcpy(entry.name,"Other"); assert(!save_cheats_find(&entry));
    fixture(&entry,data);
    for (i=0;i<40;i+=4) { unsigned char a=data[i],b=data[i+1]; data[i]=data[i+3];data[i+1]=data[i+2];data[i+2]=b;data[i+3]=a; }
    assert(!save_cheats_find(&entry) && !save_cheats_find(NULL));
    if (argc>1) {
        unsigned char binary[65536], damaged[65536];
        FILE *file=fopen(argv[1],"rb");
        size_t length;
        assert(file); length=fread(binary,1,sizeof(binary),file); fclose(file);
        /* Change the external preset, proving it overrides the bundled value. */
        assert(length>=552);
        save_put_word(binary+24+160+8*40+40,2);
        save_put_word(binary+16,usb_art_crc32(binary+24,length-24));
        assert(!save_cheats_load(binary,length) && save_cheats_external());
        fixture(&entry,data); p=save_cheats_find(&entry);
        assert(p && p->presets[0].value==2 && !save_cheats_preset(&entry,p,0) && save_word(data+24)==2);
        memcpy(damaged,binary,length); damaged[length-1]^=1;
        assert(save_cheats_load(damaged,length)<0 && save_cheats_find(&entry)==p);
        assert(save_cheats_load(binary,length-1)<0);
        /* CRC-valid malformed records must also fail atomically. */
        for (i=0;i<5;++i) {
            memcpy(damaged,binary,length);
            if (i==0) save_put_word(damaged+24+160+28,65532); /* field past EOF */
            if (i==1) save_put_word(damaged+24+160+40+28,4); /* overlaps another field */
            if (i==2) memset(damaged+24+32,'x',64); /* unterminated ID */
            if (i==3) save_put_word(damaged+24+160+8*40+36,99); /* missing preset field */
            if (i==4) save_put_word(damaged+24+24,0xffffffffUL); /* abusive field count */
            save_put_word(damaged+16,usb_art_crc32(damaged+24,length-24));
            assert(save_cheats_load(damaged,length)<0 && save_cheats_find(&entry)==p);
        }
        /* Mutate CRC-valid untrusted records; all accepted profiles must stay
         * safe to match/apply, and every failed load must be reclaimable. */
        for (i=0;i<1200;++i) {
            size_t offset=24+(i*37)%(length-24);
            memcpy(damaged,binary,length); damaged[offset]^=(unsigned char)(1U<<(i%8));
            save_put_word(damaged+16,usb_art_crc32(damaged+24,length-24));
            save_cheats_load(damaged,length); fixture(&entry,data); p=save_cheats_find(&entry);
            if (p) { assert(!save_cheats_preset(&entry,p,0)); assert(save_cheats_find(&entry)==p); }
            save_cheats_reset();
        }
        save_cheats_reset(); fixture(&entry,data); p=save_cheats_find(&entry);
        assert(p && p->presets[0].value==23 && !save_cheats_external());
    }
    puts("Save cheats: verified offsets, ranges, checksum repair, presets, incompatible saves and no-change rejection passed");
    return 0;
}
