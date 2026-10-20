#include "save_catalog.h"
#include "usb_art.h"
#include <stdlib.h>
#include <string.h>
static int valid_id(unsigned long id) {return id>0 && id<=SAVE_CATALOG_RECOVERY2;}
unsigned long save_catalog_id(unsigned int slot)
{
    return slot==SAVE_RECOVERY_SLOT ? SAVE_CATALOG_RECOVERY : slot==SAVE_RECOVERY2_SLOT ? SAVE_CATALOG_RECOVERY2 : (unsigned long)slot+1;
}
int save_catalog_slot_from_path(const char *path,unsigned int *slot)
{
    unsigned long number=0;size_t digits=0;const char *p;
    if (!strcmp(path,"saves/recovery.3ds")) {*slot=SAVE_RECOVERY_SLOT;return 0;}
    if (!strcmp(path,"saves/recovery2.3ds")) {*slot=SAVE_RECOVERY2_SLOT;return 0;}
    if (strncmp(path,"saves/slot",10)) return -1;
    p=path+10;
    while (*p>='0' && *p<='9') {
        if (++digits>9) return -1;
        number=number*10+(unsigned long)(*p++-'0');
    }
    if (digits<2 || !number || number>SAVE_SLOT_LIMIT || strcmp(p,".3ds") || (digits>2 && path[10]=='0')) return -1;
    *slot=(unsigned int)(number-1);return 0;
}
static unsigned int slot_index(unsigned long id)
{
    return id==SAVE_CATALOG_RECOVERY ? SAVE_RECOVERY_SLOT : id==SAVE_CATALOG_RECOVERY2 ? SAVE_RECOVERY2_SLOT : (unsigned int)(id-1);
}
void save_catalog_path(char *path,unsigned long id)
{
    if (!id) strcpy(path,SAVE_CATALOG_ROOT);
    else if (id==SAVE_CATALOG_RECOVERY) strcpy(path,"saves/recovery.3si");
    else if (id==SAVE_CATALOG_RECOVERY2) strcpy(path,"saves/recovery2.3si");
    else {
        char digits[12];size_t count=0,pos=10;
        strcpy(path,"saves/slot");
        do {digits[count++]=(char)('0'+id%10);id/=10;} while (id);
        if (count<2) path[pos++]='0';
        while (count) path[pos++]=digits[--count];
        strcpy(path+pos,".3si");
    }
}
static void put(unsigned char *p,unsigned long x)
{
    p[0]=(unsigned char)(x>>24);p[1]=(unsigned char)(x>>16);p[2]=(unsigned char)(x>>8);p[3]=(unsigned char)x;
}
static int valid(const SaveCatalogNode *n,int root)
{
    size_t i,j;
    if (root) return !n->id && (!n->next || valid_id(n->next));
    if (!valid_id(n->id) || n->next>=n->id || (n->state!=1 && n->state!=2) || n->count>SAVE_MAX_FILES || !memchr(n->label,0,25)) return 0;
    if (n->state==2 && n->count) return 0;
    for (j=0;n->label[j];++j) if ((unsigned char)n->label[j]<32 || (unsigned char)n->label[j]>126) return 0;
    for (i=0;i<n->count;++i) {
        if (!n->files[i][0] || !memchr(n->files[i],0,32)) return 0;
        for (j=0;n->files[i][j];++j) if ((unsigned char)n->files[i][j]<32 || (unsigned char)n->files[i][j]==127 || strchr("/\\:",n->files[i][j])) return 0;
    }
    return 1;
}
int save_catalog_decode(SaveCatalogNode *n,unsigned char *b,size_t length,int root)
{
    unsigned long crc;
    size_t size;
    if (length<24 || memcmp(b,root ? "OM3DSLC1" : "OM3DSLT1",8)) return -1;
    size=(size_t)save_word(b+8);
    if (size>length || size<28 || size>SAVE_CATALOG_BYTES || (root && size!=28) || (!root && size<68)) return -1;
    crc=save_word(b+20);put(b+20,0);
    if (usb_art_crc32(b,size)!=crc) {put(b+20,crc);return -1;}
    put(b+20,crc);memset(n,0,sizeof(*n));n->id=save_word(b+12);
    if (root) {if (save_word(b+16)) return -1;n->next=save_word(b+24);}
    else {
        n->next=save_word(b+16);n->state=save_word(b+24);n->length=save_word(b+28);n->archive_crc=save_word(b+32);
        memcpy(n->label,b+36,28);n->count=(size_t)save_word(b+64);
        if (n->count>SAVE_MAX_FILES || size!=68+n->count*32) return -1;
        memcpy(n->files,b+68,n->count*32);
    }
    return valid(n,root) ? 0 : -1;
}
int save_catalog_encode(const SaveCatalogNode *n,unsigned char *b,size_t capacity,int root)
{
    size_t size=root ? 28 : 68+n->count*32;
    if (!valid(n,root) || size>capacity) return -1;
    memset(b,0,size);memcpy(b,root ? "OM3DSLC1" : "OM3DSLT1",8);put(b+8,(unsigned long)size);put(b+12,n->id);
    if (root) put(b+24,n->next);
    else {
        put(b+16,n->next);put(b+24,n->state);put(b+28,n->length);put(b+32,n->archive_crc);
        strcpy((char *)b+36,n->label);put(b+64,(unsigned long)n->count);memcpy(b+68,n->files,n->count*32);
    }
    put(b+20,usb_art_crc32(b,size));return (int)size;
}
/* Return 1 only for physical EOF, including firmware's false read counts. */
static int read_node(Ode *ode,OdeReadBuffer reader,unsigned long id,SaveCatalogNode *n)
{
    unsigned char header[24],*b=NULL;
    char path[48];size_t i,size;int result=-1;
    save_catalog_path(path,id);
    if (usb_file_open(ode,path)<0) return -1;
    if (usb_file_probe(ode,reader,header,24,0xa5)<0) goto done;
    for (i=0;i<24 && header[i]==0xa5;++i) {}
    if (i==24) {
        if (usb_file_close(ode)<0 || usb_file_open(ode,path)<0) return -1;
        if (usb_file_probe(ode,reader,header,24,0x5a)<0) goto done;
        for (i=0;i<24 && header[i]==0x5a;++i) {}
        if (i==24) result=1;
        goto done;
    }
    size=(size_t)save_word(header+8);
    if (size<28 || size>SAVE_CATALOG_BYTES) goto done;
    b=(unsigned char *)malloc(size);if (!b) goto done;
    memcpy(b,header,24);
    if (usb_file_read(ode,reader,b+24,size-24,USB_FILE_CRC_CHECKED)<0) goto done;
    result=save_catalog_decode(n,b,size,!id);
    if (!result && n->id!=id) result=-1;
done:
    free(b);if (usb_file_close(ode)<0) result=-1;return result;
}
static int write_node(Ode *ode,OdeReadBuffer reader,const SaveCatalogNode *n,int root)
{
    unsigned char *b=(unsigned char *)malloc(SAVE_CATALOG_BYTES);
    char path[48];int size,result=-1;
    if (!b) return -1;
    size=save_catalog_encode(n,b,SAVE_CATALOG_BYTES,root);save_catalog_path(path,root ? 0 : n->id);
    if (size<0 || usb_file_open(ode,path)<0) goto done;
    result=usb_file_write(ode,b,(size_t)size);if (usb_file_close(ode)<0) result=-1;
    if (result<0 || usb_file_open(ode,path)<0) {result=-1;goto done;}
    result=usb_file_verify(ode,reader,b,(size_t)size);if (usb_file_close(ode)<0) result=-1;
done: free(b);return result;
}
int save_catalog_update(Ode *ode,OdeReadBuffer reader,unsigned int slot,const SaveArchive *archive,const char *label)
{
    SaveCatalogNode *n=(SaveCatalogNode *)calloc(1,sizeof(*n));
    unsigned long id=save_catalog_id(slot),cur,previous=0;size_t i;int result=-1,r;
    if (!n || !archive || !archive->bytes || !valid_id(id)) goto done;
    r=read_node(ode,reader,0,n);if (r<0) goto done;
    cur=r==1 ? 0 : n->next;
    while (cur>id) {
        if (read_node(ode,reader,cur,n)) goto done;
        previous=cur;cur=n->next;
    }
    if (cur==id) {if (read_node(ode,reader,id,n)) goto done;cur=n->next;}
    memset(n,0,sizeof(*n));n->id=id;n->next=cur;n->state=1;n->length=(unsigned long)archive->length;
    n->archive_crc=save_word(archive->bytes+16);n->count=archive->count;
    if (label) {strncpy(n->label,label,24);n->label[24]=0;}
    for (i=0;i<n->count;++i) memcpy(n->files[i],archive->entries[i].name,32);
    if (write_node(ode,reader,n,0)) goto done;
    if (previous) {if (read_node(ode,reader,previous,n)) goto done;n->next=id;}
    else {memset(n,0,sizeof(*n));n->next=id;}
    result=write_node(ode,reader,n,!previous);
done: free(n);return result;
}
int save_catalog_relabel(Ode *ode,OdeReadBuffer reader,unsigned int slot,const char *label)
{
    SaveCatalogNode *n=(SaveCatalogNode *)malloc(sizeof(*n));int result=-1;
    if (!n) return -1;
    if (!read_node(ode,reader,save_catalog_id(slot),n)) {
        strncpy(n->label,label,24);n->label[24]=0;result=write_node(ode,reader,n,0);
    }
    free(n);return result;
}
static int fold(int c) {return c>='A' && c<='Z' ? c+32 : c;}
static int contains(const char *text,const char *word,size_t length)
{
    size_t i;
    if (!text) return 0;
    for (;*text;++text) {
        for (i=0;i<length && text[i] && fold((unsigned char)text[i])==fold((unsigned char)word[i]);++i) {}
        if (i==length) return 1;
    }
    return 0;
}
int save_catalog_matches(const SaveCatalogNode *n,const SaveNames *names,const char *query)
{
    char path[48];size_t length,i;int matched;
    save_catalog_path(path,n->id);
    strcpy(path+strlen(path)-4,".3ds");
    for (;*query;) {
        while (*query==' ') ++query;
        length=0;while (query[length] && query[length]!=' ') ++length;
        if (!length) break;
        matched=contains(n->label,query,length) || contains(path,query,length);
        for (i=0;!matched && i<n->count;++i) matched=contains(n->files[i],query,length) || contains(save_names_find(names,n->files[i]),query,length);
        if (!matched) return 0;
        query+=length;
    }
    return 1;
}
int save_catalog_search(Ode *ode,OdeReadBuffer reader,const SaveNames *names,const char *query,int recovery,SaveCatalogResults *out)
{
    SaveCatalogNode *n=(SaveCatalogNode *)malloc(sizeof(*n));unsigned long cur;int result=-1;
    out->count=0;out->total=0;if (!n) return -1;
    if (read_node(ode,reader,0,n)) goto done;
    cur=n->next;
    while (cur) {
        if (read_node(ode,reader,cur,n)) goto done;
        if ((recovery || cur<=SAVE_SLOT_LIMIT) && save_catalog_matches(n,names,query)) {
            if (out->total>=out->offset && out->count<8) {
                SaveCatalogMatch *m=&out->rows[out->count++];const char *hint=NULL;size_t i;
                memset(m,0,sizeof(*m));m->slot=slot_index(cur);m->state=n->state;strcpy(m->label,n->label);
                for (i=0;i<n->count;++i) {
                    const char *title=save_names_find(names,n->files[i]);
                    if (title && (!hint || contains(title,query,strlen(query)))) hint=title;
                    if (title && *query && contains(title,query,strlen(query))) break;
                }
                strncpy(m->hint,hint ? hint : n->count ? n->files[0] : n->state==2 ? "Invalid backup - inspect on PC" : "Empty archive",SAVE_TITLE_BYTES-1);
            }
            ++out->total;
        }
        cur=n->next;
    }
    result=0;
done: free(n);if (result<0) {out->count=0;out->total=0;}return result;
}
