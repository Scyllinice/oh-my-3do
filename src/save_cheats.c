#include "save_cheats.h"
#include <string.h>
#include <stdlib.h>
#include "usb_art.h"
#include "save_cheat_db.h"
static SaveCheatProfile *external_profiles;
static unsigned char *external_bytes;
static size_t external_count;
static unsigned long checksum(const SaveEntry *entry, const SaveCheatProfile *p)
{
    size_t i;
    unsigned long sum=p->checksum_seed;
    for (i=0; i<p->checksum_offset; i+=4) sum=(sum+save_word(entry->data+i))&0xffffffffUL;
    return sum;
}
static int matches(const SaveEntry *entry, const SaveCheatProfile *p)
{
    size_t i;
    if (!entry || !entry->data || strcmp(entry->name,p->filename) || entry->size!=p->size ||
        save_word(entry->data+p->signature_offset)!=p->signature ||
        save_word(entry->data+p->checksum_offset)!=checksum(entry,p)) return 0;
    for (i=0; i<p->field_count; ++i) {
        unsigned long value=save_word(entry->data+p->fields[i].offset);
        if (value<p->fields[i].min || value>p->fields[i].max) return 0;
    }
    return 1;
}
const SaveCheatProfile *save_cheats_find(const SaveEntry *entry)
{
    size_t i;
    const SaveCheatProfile *profiles=external_profiles ? external_profiles : cheat_profiles;
    size_t count=external_profiles ? external_count : sizeof(cheat_profiles)/sizeof(cheat_profiles[0]);
    for (i=0; i<count; ++i)
        if (matches(entry,&profiles[i])) return &profiles[i];
    return NULL;
}
int save_cheats_set(SaveEntry *entry, const SaveCheatProfile *p, size_t field, unsigned long value)
{
    if (!p || save_cheats_find(entry)!=p || field>=p->field_count ||
        value<p->fields[field].min || value>p->fields[field].max) return -1;
    save_put_word((unsigned char *)entry->data+p->fields[field].offset,value);
    save_put_word((unsigned char *)entry->data+p->checksum_offset,checksum(entry,p));
    return 0;
}
int save_cheats_preset(SaveEntry *entry, const SaveCheatProfile *p, size_t preset)
{
    if (!p || save_cheats_find(entry)!=p || preset>=p->preset_count) return -1;
    return save_cheats_set(entry,p,p->presets[preset].field,p->presets[preset].value);
}

static void free_profiles(SaveCheatProfile *p, size_t count)
{
    size_t i;
    if (!p) return;
    for (i=0; i<count; ++i) { free((void *)p[i].fields); free((void *)p[i].presets); }
    free(p);
}
void save_cheats_reset(void)
{
    free_profiles(external_profiles,external_count); free(external_bytes);
    external_profiles=NULL; external_bytes=NULL; external_count=0;
}
int save_cheats_external(void) { return external_profiles!=NULL; }
size_t save_cheats_profile_count(void)
{
    return external_profiles ? external_count : sizeof(cheat_profiles)/sizeof(cheat_profiles[0]);
}
static int string_ok(const unsigned char *s, size_t capacity, size_t limit)
{
    size_t i=0;
    while (i<capacity && s[i]) { if (s[i]<32 || s[i]>126) return 0; ++i; }
    if (!i || i>limit || i==capacity) return 0;
    while (i<capacity) if (s[i++]) return 0;
    return 1;
}
int save_cheats_load(const unsigned char *bytes, size_t size)
{
    unsigned char *copy=NULL;
    SaveCheatProfile *profiles=NULL;
    size_t count, pos=24, i, j, k;
    int result=-1;
    if (!bytes || size<24 || size>SAVE_CHEAT_DB_MAX_BYTES || memcmp(bytes,"OM3DCHT1",8) ||
        save_word(bytes+8)!=size || save_word(bytes+20) ||
        save_word(bytes+16)!=usb_art_crc32(bytes+24,size-24)) return -1;
    count=(size_t)save_word(bytes+12);
    if (!count || count>128 || count>(size-24)/248) return -1;
    copy=(unsigned char *)malloc(size); profiles=(SaveCheatProfile *)calloc(count,sizeof(*profiles));
    if (!copy || !profiles) goto done;
    memcpy(copy,bytes,size);
    for (i=0; i<count; ++i) {
        SaveCheatProfile *p=&profiles[i];
        const unsigned char *r=copy+pos;
        SaveCheatField *fields;
        SaveCheatPreset *presets;
        size_t length;
        if (size-pos<160) goto done;
        p->size=(size_t)save_word(r+4); p->signature_offset=(size_t)save_word(r+8);
        p->signature=save_word(r+12); p->checksum_offset=(size_t)save_word(r+16);
        p->checksum_seed=save_word(r+20); p->field_count=(size_t)save_word(r+24); p->preset_count=(size_t)save_word(r+28);
        if (p->size<8 || p->size>32768 || p->size%4 || p->checksum_offset!=p->size-4 ||
            p->signature_offset%4 || p->signature_offset>=p->checksum_offset ||
            !p->field_count || p->field_count>32 || !p->preset_count || p->preset_count>32) goto done;
        length=160+p->field_count*40+p->preset_count*48;
        if (save_word(r)!=length || length>size-pos ||
            !string_ok(r+32,64,63) || !string_ok(r+96,32,31) || !string_ok(r+128,32,31)) goto done;
        p->id=(const char *)r+32; p->game=(const char *)r+96; p->filename=(const char *)r+128;
        if (!save_name_valid(p->filename)) goto done;
        for (j=0; j<i; ++j) if (!strcmp(p->id,profiles[j].id) || (!strcmp(p->filename,profiles[j].filename) && p->size==profiles[j].size && p->signature_offset==profiles[j].signature_offset && p->signature==profiles[j].signature)) goto done;
        fields=(SaveCheatField *)calloc(p->field_count,sizeof(*fields));
        presets=(SaveCheatPreset *)calloc(p->preset_count,sizeof(*presets));
        p->fields=fields; p->presets=presets;
        if (!fields || !presets) goto done;
        r+=160;
        for (j=0; j<p->field_count; ++j,r+=40) {
            if (!string_ok(r,28,25)) goto done;
            fields[j].label=(const char *)r; fields[j].offset=(size_t)save_word(r+28);
            fields[j].min=save_word(r+32); fields[j].max=save_word(r+36);
            if (fields[j].offset%4 || fields[j].offset>=p->checksum_offset ||
                fields[j].offset==p->signature_offset || fields[j].min>fields[j].max) goto done;
            for (k=0; k<j; ++k) if (fields[k].offset==fields[j].offset) goto done;
        }
        for (j=0; j<p->preset_count; ++j,r+=48) {
            if (!string_ok(r,36,34) || save_word(r+44)) goto done;
            presets[j].label=(const char *)r; presets[j].field=(size_t)save_word(r+36); presets[j].value=save_word(r+40);
            if (presets[j].field>=p->field_count || presets[j].value<fields[presets[j].field].min ||
                presets[j].value>fields[presets[j].field].max) goto done;
        }
        pos+=length;
    }
    if (pos!=size) goto done;
    save_cheats_reset(); external_profiles=profiles; external_bytes=copy; external_count=count;
    profiles=NULL; copy=NULL; result=0;
done:
    free_profiles(profiles,count); free(copy); return result;
}
