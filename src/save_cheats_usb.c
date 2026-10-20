#include "save_cheats_usb.h"
#include "save_cheats.h"
#include <stdlib.h>
#include <string.h>
int save_cheats_usb_load(Ode *ode, OdeReadBuffer reader)
{
    unsigned char header[24], *bytes=NULL;
    size_t size;
    int result=0;
    if (usb_file_open(ode,"save-cheats.3db")<0) return 0;
    if (usb_file_read(ode,reader,header,24,1)<0 || (memcmp(header,"OM3DCHT1",8) && memcmp(header,"OM3DCHT2",8))) goto done;
    size=(size_t)save_word(header+8);
    if (size<24 || size>SAVE_CHEAT_DB_MAX_BYTES) goto done;
    bytes=(unsigned char *)malloc(size);
    if (!bytes) goto done;
    memcpy(bytes,header,24);
    if (usb_file_read(ode,reader,bytes+24,size-24,0)<0) goto done;
    /* Close before publishing: transport failure cannot install a partial load. */
    if (usb_file_close(ode)<0) { free(bytes); return 0; }
    result=save_cheats_load(bytes,size)==0;
    free(bytes); return result;
done:
    free(bytes); usb_file_close(ode); return 0;
}
