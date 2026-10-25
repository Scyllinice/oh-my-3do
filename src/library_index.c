#include "library_index.h"
#include "usb_file.h"
#include <stdlib.h>
#include <string.h>
static unsigned long word(const unsigned char *p)
{
    return ((unsigned long)p[0]<<24)|((unsigned long)p[1]<<16)|((unsigned long)p[2]<<8)|p[3];
}
static int relative(const char *path,unsigned int prefix_depth)
{
    const char *part=path, *p=path;
    unsigned int depth=prefix_depth;
    if (!*path || strlen(path)>8192) return 0;
    for (;;++p) {
        unsigned char c=(unsigned char)*p;
        if (c && (c<32 || c==127 || c=='\\' || c==':')) return 0;
        if (!c || c=='/') {
            size_t n=(size_t)(p-part);
            if (!n || n>=ODE_MAX_NAME || (n==1 && *part=='.') || (n==2 && !memcmp(part,"..",2))) return 0;
            if (!c) {
                return n>4 && (!menu_path_compare(p-4,".iso") || !menu_path_compare(p-4,".cue"));
            }
            if (++depth>LIBRARY_DEPTH) return 0;
            part=p+1;
        }
    }
}
int library_index_load(Ode *ode,OdeReadBuffer reader,const char *prefix,
    LibraryCatalog *catalog,MediaCancel cancel,void *context)
{
    unsigned char header[24], *data=NULL;
    LibraryCatalog next;
    unsigned long count, bytes;
    size_t pos=0, i, prefix_length=strlen(prefix);
    int result=-1;
    memset(&next,0,sizeof(next));
    if (strncmp(prefix,"/Storage",8) || (prefix[8] && prefix[8]!='/') || prefix_length>ODE_MAX_NAME+9) return -1;
    if (usb_file_open(ode,"library.3di")<0) return -1;
    if (usb_file_read(ode,reader,header,24,1)<0 || memcmp(header,"OM3DLIB1",8) || word(header+20)) goto done;
    count=word(header+8); bytes=word(header+12);
    if (count>LIBRARY_CATALOG_MAX || bytes>LIBRARY_CATALOG_BYTES || (count && bytes<count*3) || (!count && bytes)) goto done;
    data=(unsigned char *)malloc(bytes ? (size_t)bytes : 1);
    next.paths=(char **)calloc(count ? (size_t)count : 1,sizeof(char *));
    if (!data || !next.paths) { result=-3; goto done; }
    while (pos<(size_t)bytes) {
        size_t n=bytes-pos>2048 ? 2048 : (size_t)bytes-pos;
        if (cancel && cancel(context)) { result=1; goto done; }
        if (usb_file_read(ode,reader,data+pos,n,0)<0) goto done;
        pos+=n;
    }
    if (usb_art_crc32(data,(size_t)bytes)!=word(header+16) ||
        usb_file_verify(ode,reader,(const unsigned char *)"END3DL01",8)<0) goto done;
    pos=0;
    for (i=0;i<(size_t)count;++i) {
        size_t length, total;
        char *path;
        if (bytes-pos<2) goto done;
        length=(size_t)data[pos]*256+data[pos+1]; pos+=2;
        if (length<2 || length>bytes-pos || data[pos+length-1] || memchr(data+pos,0,length-1)) goto done;
        if (!relative((char *)data+pos,prefix_length>8 ? 1 : 0)) goto done;
        total=prefix_length+1+length;
        if (total>9217 || next.bytes+total>LIBRARY_CATALOG_BYTES) goto done;
        path=(char *)malloc(total); if (!path) { result=-3; goto done; }
        memcpy(path,prefix,prefix_length); path[prefix_length]='/'; memcpy(path+prefix_length+1,data+pos,length);
        next.paths[next.count++]=path; next.bytes+=total; pos+=length;
    }
    if (pos!=bytes) goto done;
    next.valid=1; result=0;
done:
    if (usb_file_close(ode)<0) result=-1;
    if (!result) { library_catalog_free(catalog); *catalog=next; memset(&next,0,sizeof(next)); }
    library_catalog_free(&next); free(data); return result;
}

int library_path_safe(const char *path)
{
    return relative(path,0);
}

void library_metadata_free(LibraryMetadata *metadata)
{
    free(metadata->entries); free(metadata->storage);
    memset(metadata,0,sizeof(*metadata));
}

int library_metadata_parse(const unsigned char *data,size_t size,LibraryMetadata *metadata)
{
    LibraryMetadata next;
    unsigned long count, bytes;
    size_t i, pos=0;
    memset(&next,0,sizeof(next));
    if (size<32 || memcmp(data,"OM3DMET1",8) || word(data+20)) return -1;
    count=word(data+8); bytes=word(data+12);
    if (count>LIBRARY_METADATA_MAX || bytes>LIBRARY_METADATA_BYTES ||
        bytes+16UL*count>LIBRARY_METADATA_BYTES || size!=24UL+bytes+8 ||
        memcmp(data+24+bytes,"END3DM01",8) ||
        usb_art_crc32(data+24,(size_t)bytes)!=word(data+16)) return -1;
    next.storage=(unsigned char *)malloc(bytes ? (size_t)bytes : 1);
    next.entries=(LibraryGameInfo *)calloc(count ? (size_t)count : 1,sizeof(LibraryGameInfo));
    if (!next.storage || !next.entries) { library_metadata_free(&next); return -3; }
    memcpy(next.storage,data+24,(size_t)bytes);
    for (i=0;i<(size_t)count;++i) {
        const char *fields[4];
        unsigned int field;
        /* Only the shape: each string ends where its length says. Checking
         * every entry's content took most of a second on the console. Paths
         * are compared, and checked before use (library_path_safe); text is
         * drawn with anything unprintable as '?'. An entry out of order is
         * just not found. */
        for (field=0;field<4;++field) {
            size_t length;
            if (bytes-pos<2) goto invalid;
            length=(size_t)next.storage[pos]*256+next.storage[pos+1]; pos+=2;
            if (length<(field ? 1U : 2U) || length>bytes-pos || next.storage[pos+length-1] ||
                (field && length>97)) goto invalid;
            fields[field]=(const char *)next.storage+pos;
            pos+=length;
        }
        next.entries[i].path=fields[0]; next.entries[i].genre=fields[1];
        next.entries[i].publisher=fields[2]; next.entries[i].released=fields[3];
    }
    if (pos!=bytes) goto invalid;
    next.count=(size_t)count;
    library_metadata_free(metadata); *metadata=next; return 0;
invalid:
    library_metadata_free(&next); return -1;
}

int library_metadata_load(Ode *ode,OdeReadBuffer reader,LibraryMetadata *metadata)
{
    unsigned char header[24], *data=NULL;
    unsigned long bytes;
    size_t pos;
    LibraryMetadata next;
    int result=-1;
    memset(&next,0,sizeof(next));
    if (usb_file_open(ode,"metadata.3di")<0) return -1;
    if (usb_file_read(ode,reader,header,24,1)<0 || memcmp(header,"OM3DMET1",8) || word(header+20)) goto done;
    bytes=word(header+12);
    if (word(header+8)>LIBRARY_METADATA_MAX || bytes>LIBRARY_METADATA_BYTES ||
        bytes+16UL*word(header+8)>LIBRARY_METADATA_BYTES) goto done;
    data=(unsigned char *)malloc((size_t)bytes+32);
    if (!data) { result=-3; goto done; }
    memcpy(data,header,24); pos=0;
    while (pos<(size_t)bytes) {
        size_t n=bytes-pos>2048 ? 2048 : (size_t)bytes-pos;
        if (usb_file_read(ode,reader,data+24+pos,n,0)<0) goto done;
        pos+=n;
    }
    if (usb_file_verify(ode,reader,(const unsigned char *)"END3DM01",8)<0) goto done;
    memcpy(data+24+bytes,"END3DM01",8);
    result=library_metadata_parse(data,(size_t)bytes+32,&next);
done:
    if (usb_file_close(ode)<0) result=-1;
    if (!result) { library_metadata_free(metadata); *metadata=next; memset(&next,0,sizeof(next)); }
    library_metadata_free(&next); free(data); return result;
}

const LibraryGameInfo *library_metadata_find(const LibraryMetadata *metadata,const char *path)
{
    size_t low=0, high=metadata->count;
    while (low<high) {
        size_t mid=low+(high-low)/2;
        int order=menu_path_compare(path,metadata->entries[mid].path);
        if (!order) return metadata->entries+mid;
        if (order<0) high=mid; else low=mid+1;
    }
    return NULL;
}
