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
