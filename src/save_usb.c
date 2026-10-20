#include "save_usb.h"
#include <stdlib.h>
#include <string.h>

static int allowed(const char *path)
{
    const char *p;
    unsigned long slot=0;
    size_t digits=0;
    if (!strcmp(path,"saves/recovery.3ds") || !strcmp(path,"saves/recovery2.3ds")) return 1;
    if (strncmp(path,"saves/slot",10)) return 0;
    p=path+10;
    while (*p>='0' && *p<='9') {
        if (++digits>9) return 0;
        slot=slot*10+(unsigned long)(*p++-'0');
    }
    if (digits<2 || !slot || slot>SAVE_SLOT_LIMIT || strcmp(p,".3ds")) return 0;
    return digits==2 || path[10]!='0';
}
static int load(Ode *ode, OdeReadBuffer reader, const char *path, SaveArchive *out, int allow_empty)
{
    unsigned char header[SAVE_HEADER], *bytes = NULL;
    size_t size;
    int result = -1;
    if (!allowed(path) || usb_file_open(ode, path) < 0) return -1;
    if (allow_empty) {
        size_t i;
        if (usb_file_probe(ode,reader,header,sizeof(header),0xa5)<0) goto done;
        for (i=0;i<sizeof(header) && header[i]==0xa5;++i) {}
        if (i==sizeof(header)) {
            /* A second, distinct sentinel distinguishes true EOF from an
             * existing file whose bytes happen to equal the first sentinel. */
            if (usb_file_close(ode)<0) return -1;
            if (usb_file_open(ode,path)<0) return -1;
            if (usb_file_probe(ode,reader,header,sizeof(header),0x5a)<0) goto done;
            for (i=0;i<sizeof(header) && header[i]==0x5a;++i) {}
            if (i==sizeof(header)) { result=1; goto done; }
            goto done;
        }
    } else if (usb_file_read(ode,reader,header,sizeof(header),1)<0) goto done;
    if (memcmp(header,"OM3DSAV1",8)) goto done;
    size = (size_t)save_word(header + 8);
    if (size < SAVE_HEADER || size > SAVE_MAX_BYTES) goto done;
    bytes = (unsigned char *)malloc(size);
    if (!bytes) goto done;
    memcpy(bytes, header, SAVE_HEADER);
    if (usb_file_read(ode, reader, bytes + SAVE_HEADER, size - SAVE_HEADER, 0) < 0) goto done;
    result = save_archive_parse(out, bytes, size);
done:
    free(bytes);
    if (usb_file_close(ode) < 0) result = -1;
    if (result==1) save_archive_free(out);
    return result;
}
int save_usb_load(Ode *ode, OdeReadBuffer reader, const char *path, SaveArchive *out)
{
    return load(ode,reader,path,out,0);
}
int save_usb_load_target(Ode *ode, OdeReadBuffer reader, const char *path, SaveArchive *out)
{
    return load(ode,reader,path,out,1);
}
/* Full byte comparison, not just a command acknowledgement. File lengths are
 * self-described: FCare cannot truncate existing USB files. Ignore old tails. */
static int verify(Ode *ode, OdeReadBuffer reader, const char *path,
                  const unsigned char *bytes, size_t length)
{
    int result;
    if (usb_file_open(ode, path) < 0) return -1;
    result = usb_file_verify(ode, reader, bytes, length);
    if (usb_file_close(ode) < 0) result = -1;
    return result;
}

static int store(Ode *ode, OdeReadBuffer reader, const char *path,
                 const unsigned char *bytes, size_t length)
{
    int result;
    if (usb_file_open(ode, path) < 0) return -1;
    result = usb_file_write(ode, bytes, length);
    if (usb_file_close(ode) < 0) result = -1;
    if (result < 0) return -1;
    return verify(ode, reader, path, bytes, length);
}
int save_usb_store(Ode *ode, OdeReadBuffer reader, const char *path, const SaveArchive *a)
{
    if (!allowed(path) || !a->bytes || a->length < SAVE_HEADER) return -1;
    return store(ode, reader, path, a->bytes, a->length);
}
int save_usb_test(Ode *ode, OdeReadBuffer reader)
{
    unsigned char bytes[4099];
    size_t i;
    /* Two different patterns catch silent no-ops and stale prior test data. */
    int pass;
    for (pass = 0; pass < 2; ++pass) {
        for (i = 0; i < sizeof(bytes); ++i)
            bytes[i] = (unsigned char)((i * 37 + i / 2048 + 71 * pass + 19) & 255);
        if (store(ode, reader, "saves/usb-test.bin", bytes, sizeof(bytes)) < 0) return -1;
    }
    return 0;
}

int save_usb_store_checked(Ode *ode, OdeReadBuffer reader, const char *path,
                           const SaveArchive *archive, const SaveArchive *expected)
{
    SaveArchive *current=(SaveArchive *)calloc(1,sizeof(*current));
    int result, matches;
    if (!current) return -1;
    result=save_usb_load_target(ode,reader,path,current);
    matches=expected ? result==0 && current->length==expected->length &&
        !memcmp(current->bytes,expected->bytes,current->length) : result==1;
    save_archive_free(current); free(current);
    if (!matches) return -1;
    return save_usb_store(ode,reader,path,archive);
}
