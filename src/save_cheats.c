#include "save_cheats.h"
#include <string.h>
#include <stdlib.h>
#include "usb_art.h"
#include "save_cheat_db.h"
static SaveCheatProfile *external_profiles;
static unsigned char *external_bytes;
static size_t external_count;
static unsigned long number(const unsigned char *data, size_t width)
{
    unsigned long value=0;
    while (width--) value=(value<<8)|*data++;
    return value;
}
static unsigned int shift(unsigned long mask)
{
    unsigned int n=0;
    while (!(mask&1)) { mask>>=1; ++n; }
    return n;
}
unsigned long save_cheats_value(const SaveEntry *entry, const SaveCheatField *f)
{
    return (number(entry->data+f->offset,f->width)&f->mask)>>shift(f->mask);
}
static unsigned long checksum(const SaveEntry *entry, const SaveCheatProfile *p)
{
    size_t i;
    unsigned long sum=p->checksum_seed;
    for (i=0;i<p->checksum_offset;i+=4) sum=(sum+save_word(entry->data+i))&0xffffffffUL;
    return sum;
}
static int matches(const SaveEntry *entry, const SaveCheatProfile *p)
{
    size_t i;
    if (!entry || !entry->data || strcmp(entry->name,p->filename) || entry->size!=p->size ||
        number(entry->data+p->signature_offset,p->signature_width)!=p->signature ||
        (p->checksum_kind && save_word(entry->data+p->checksum_offset)!=checksum(entry,p))) return 0;
    for (i=0;i<p->field_count;++i) {
        unsigned long value=save_cheats_value(entry,&p->fields[i]);
        if (value<p->fields[i].min || value>p->fields[i].max) return 0;
    }
    return 1;
}
const SaveCheatProfile *save_cheats_find(const SaveEntry *entry)
{
    size_t i;
    const SaveCheatProfile *profiles=external_profiles ? external_profiles : cheat_profiles;
    size_t count=external_profiles ? external_count : sizeof(cheat_profiles)/sizeof(cheat_profiles[0]);
    for (i=0;i<count;++i) if (matches(entry,&profiles[i])) return &profiles[i];
    return NULL;
}
static void change(SaveEntry *entry, const SaveCheatField *f, unsigned long value)
{
    size_t i=f->width;
    unsigned long next=(number(entry->data+f->offset,f->width)&(~f->mask&0xffffffffUL)) | ((value<<shift(f->mask))&f->mask);
    while (i--) { ((unsigned char *)entry->data)[f->offset+i]=(unsigned char)next; next>>=8; }
}
static void repair(SaveEntry *entry, const SaveCheatProfile *p)
{
    if (p->checksum_kind) save_put_word((unsigned char *)entry->data+p->checksum_offset,checksum(entry,p));
}
int save_cheats_set(SaveEntry *entry, const SaveCheatProfile *p, size_t field, unsigned long value)
{
    if (!p || save_cheats_find(entry)!=p || field>=p->field_count || value<p->fields[field].min || value>p->fields[field].max) return -1;
    change(entry,&p->fields[field],value); repair(entry,p); return 0;
}
int save_cheats_preset(SaveEntry *entry, const SaveCheatProfile *p, size_t preset)
{
    const SaveCheatPreset *x;
    size_t i;
    if (!p || save_cheats_find(entry)!=p || preset>=p->preset_count) return -1;
    x=&p->presets[preset]; change(entry,&p->fields[x->field],x->value);
    for (i=0;i<x->additional_count;++i) change(entry,&p->fields[x->additional[i].field],x->additional[i].value);
    repair(entry,p); return 0;
}
static void free_profiles(SaveCheatProfile *p, size_t count)
{
    size_t i,j;
    if (!p) return;
    for (i=0;i<count;++i) {
        if (p[i].presets) for (j=0;j<p[i].preset_count;++j) free((void *)p[i].presets[j].additional);
        free((void *)p[i].fields); free((void *)p[i].presets);
    }
    free(p);
}
void save_cheats_reset(void)
{
    free_profiles(external_profiles,external_count); free(external_bytes);
    external_profiles=NULL; external_bytes=NULL; external_count=0;
}
int save_cheats_external(void) { return external_profiles!=NULL; }
size_t save_cheats_profile_count(void) { return external_profiles ? external_count : sizeof(cheat_profiles)/sizeof(cheat_profiles[0]); }
static int string_ok(const unsigned char *s, size_t capacity, size_t limit)
{
    size_t i=0;
    while (i<capacity && s[i]) { if (s[i]<32 || s[i]>126) return 0; ++i; }
    if (!i || i>limit || i==capacity) return 0;
    while (i<capacity) if (s[i++]) return 0;
    return 1;
}
static int width_ok(size_t width) { return width==1 || width==2 || width==4; }
static int value_ok(const SaveCheatProfile *p, size_t field, unsigned long value)
{ return field<p->field_count && value>=p->fields[field].min && value<=p->fields[field].max; }
int save_cheats_load(const unsigned char *bytes, size_t size)
{
    unsigned char *copy=NULL,*occupied=NULL;
    SaveCheatProfile *profiles=NULL;
    size_t count,pos=24,i,j,k,header,field_bytes;
    int result=-1,modern;
    if (!bytes || size<24 || size>SAVE_CHEAT_DB_MAX_BYTES) return -1;
    modern=!memcmp(bytes,"OM3DCHT2",8);
    if ((!modern && memcmp(bytes,"OM3DCHT1",8)) || save_word(bytes+8)!=size || save_word(bytes+20) || save_word(bytes+16)!=usb_art_crc32(bytes+24,size-24)) return -1;
    header=modern ? 176 : 160; field_bytes=modern ? 48 : 40;
    count=(size_t)save_word(bytes+12);
    if (!count || count>128 || count>(size-24)/(header+field_bytes+48)) return -1;
    copy=(unsigned char *)malloc(size); profiles=(SaveCheatProfile *)calloc(count,sizeof(*profiles));
    if (!copy || !profiles) goto done;
    memcpy(copy,bytes,size);
    for (i=0;i<count;++i) {
        SaveCheatProfile *p=&profiles[i];
        const unsigned char *r=copy+pos;
        SaveCheatField *fields;
        SaveCheatPreset *presets;
        size_t length,end;
        if (size-pos<header) goto done;
        length=(size_t)save_word(r); if (length<header || length>size-pos) goto done; end=pos+length;
        p->size=(size_t)save_word(r+4); p->signature_offset=(size_t)save_word(r+8); p->signature=save_word(r+12);
        p->checksum_offset=(size_t)save_word(r+16); p->checksum_seed=save_word(r+20);
        p->field_count=(size_t)save_word(r+24); p->preset_count=(size_t)save_word(r+28);
        p->signature_width=modern ? (size_t)save_word(r+32) : 4;
        p->checksum_kind=modern ? (int)save_word(r+36) : 1;
        if (p->size<8 || p->size>32768 || p->size%4 || !width_ok(p->signature_width) ||
            p->checksum_offset>p->size || p->signature_width>p->checksum_offset || p->signature_offset>p->checksum_offset-p->signature_width ||
            (p->signature_width<4 && p->signature>=(1UL<<(p->signature_width*8))) ||
            (p->checksum_kind!=0 && p->checksum_kind!=1) ||
            (p->checksum_kind ? p->checksum_offset!=p->size-4 : (p->checksum_offset!=p->size || p->checksum_seed)) ||
            !p->field_count || p->field_count>160 || !p->preset_count || p->preset_count>32 ||
            (modern && (save_word(r+40) || save_word(r+44)))) goto done;
        r+=modern ? 48 : 32;
        if (!string_ok(r,64,63) || !string_ok(r+64,32,31) || !string_ok(r+96,32,31)) goto done;
        p->id=(const char *)r; p->game=(const char *)r+64; p->filename=(const char *)r+96;
        if (!save_name_valid(p->filename)) goto done;
        for (j=0;j<i;++j) if (!strcmp(p->id,profiles[j].id) || (!strcmp(p->filename,profiles[j].filename) && p->size==profiles[j].size && p->signature_offset==profiles[j].signature_offset && p->signature==profiles[j].signature)) goto done;
        if (p->field_count>(length-header)/field_bytes) goto done;
        fields=(SaveCheatField *)calloc(p->field_count,sizeof(*fields)); presets=(SaveCheatPreset *)calloc(p->preset_count,sizeof(*presets));
        p->fields=fields; p->presets=presets;
        occupied=(unsigned char *)calloc(p->size,1);
        if (!fields || !presets || !occupied) goto done;
        memset(occupied+p->signature_offset,255,p->signature_width);
        pos+=header; r=copy+pos;
        for (j=0;j<p->field_count;++j,r+=field_bytes,pos+=field_bytes) {
            unsigned long shifted;
            if (!string_ok(r,28,25)) goto done;
            fields[j].label=(const char *)r; fields[j].offset=(size_t)save_word(r+28); fields[j].min=save_word(r+32); fields[j].max=save_word(r+36);
            fields[j].width=modern ? (size_t)save_word(r+40) : 4; fields[j].mask=modern ? save_word(r+44) : 0xffffffffUL;
            if (!width_ok(fields[j].width) || fields[j].width>p->checksum_offset || fields[j].offset>p->checksum_offset-fields[j].width || !fields[j].mask || fields[j].min>fields[j].max || (fields[j].width<4 && fields[j].mask>=(1UL<<(fields[j].width*8)))) goto done;
            shifted=fields[j].mask>>shift(fields[j].mask);
            if ((shifted&((shifted+1)&0xffffffffUL)) || fields[j].max>shifted) goto done;
            for (k=0;k<fields[j].width;++k) {
                unsigned char bits=(unsigned char)(fields[j].mask>>((fields[j].width-1-k)*8));
                if (occupied[fields[j].offset+k]&bits) goto done;
                occupied[fields[j].offset+k]|=bits;
            }
        }
        free(occupied); occupied=NULL;
        for (j=0;j<p->preset_count;++j) {
            SaveCheatChange *changes;
            size_t n;
            if (end-pos<48) goto done;
            r=copy+pos;
            if (!string_ok(r,36,34)) goto done;
            presets[j].label=(const char *)r; presets[j].field=(size_t)save_word(r+36); presets[j].value=save_word(r+40);
            n=(size_t)save_word(r+44); pos+=48;
            if ((!modern && n) || n>=160 || n>(end-pos)/8 || !value_ok(p,presets[j].field,presets[j].value)) goto done;
            presets[j].additional_count=n;
            changes=n ? (SaveCheatChange *)calloc(n,sizeof(*changes)) : NULL; presets[j].additional=changes;
            if (n && !changes) goto done;
            for (k=0;k<n;++k,pos+=8) {
                size_t a;
                changes[k].field=(size_t)save_word(copy+pos); changes[k].value=save_word(copy+pos+4);
                if (!value_ok(p,changes[k].field,changes[k].value) || changes[k].field==presets[j].field) goto done;
                for (a=0;a<k;++a) if (changes[a].field==changes[k].field) goto done;
            }
        }
        if (pos!=end) goto done;
    }
    if (pos!=size) goto done;
    save_cheats_reset(); external_profiles=profiles; external_bytes=copy; external_count=count;
    profiles=NULL; copy=NULL; result=0;
done:
    free(occupied); free_profiles(profiles,count); free(copy); return result;
}
