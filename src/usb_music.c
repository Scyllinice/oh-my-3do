#include "usb_music.h"
#include "usb_file.h"
#include <string.h>
static unsigned long word(const unsigned char *p)
{
    return ((unsigned long)p[0]<<24)|((unsigned long)p[1]<<16)|((unsigned long)p[2]<<8)|p[3];
}
int usb_music_load(Ode *ode,OdeReadBuffer reader,UsbMusicAlloc allocate,void *context,
                   UsbMusicCancel cancel,void *cancel_context)
{
    unsigned char header[24], *pixels;
    UsbMusicSpec spec;
    unsigned long pos;
    int result=-1;
    if (usb_file_open(ode,USB_MUSIC_PATH)<0) return -1;
    if (usb_file_read(ode,reader,header,24,1)<0 || memcmp(header,"OM3DMUS2",8)) goto done;
    spec.rate=word(header+8); spec.frames=word(header+12); spec.bytes=word(header+16);
    if ((spec.rate!=11025 && spec.rate!=22050) || spec.frames<8 || (spec.frames&7) ||
        spec.bytes>USB_MUSIC_MAX_BYTES || spec.bytes!=spec.frames/2) goto done;
    if (cancel && cancel(cancel_context)) goto done;
    pixels=allocate(context,&spec);
    if (!pixels) goto done;
    for (pos=0;pos<spec.bytes;) {
        size_t n=spec.bytes-pos>2048 ? 2048 : (size_t)(spec.bytes-pos);
        if ((cancel && cancel(cancel_context)) || usb_file_read(ode,reader,pixels+pos,n,0)<0) goto done;
        pos+=n;
    }
    if ((cancel && cancel(cancel_context)) || usb_art_crc32(pixels,(size_t)spec.bytes)!=word(header+20) ||
        usb_file_verify(ode,reader,(const unsigned char *)"END3DM02",8)<0) goto done;
    result=0;
done:
    if (usb_file_close(ode)<0) result=-1;
    return result;
}
