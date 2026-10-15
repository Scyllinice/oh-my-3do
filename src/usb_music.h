#ifndef OH3DO_USB_MUSIC_H
#define OH3DO_USB_MUSIC_H
#include "usb_art.h"
#define USB_MUSIC_MAX_BYTES (768UL*1024UL)
#define USB_MUSIC_PATH "music/menu.3dm"
typedef struct UsbMusicSpec { unsigned long rate,frames,bytes; } UsbMusicSpec;
typedef unsigned char *(*UsbMusicAlloc)(void *context,const UsbMusicSpec *spec);
typedef int (*UsbMusicCancel)(void *context);
/* Selected USB root only. Allocator owns its buffer, including on failure.
 * The complete mono ADP4 payload is verified before it can be played. */
int usb_music_load(Ode *ode,OdeReadBuffer reader,UsbMusicAlloc allocate,void *context,
                   UsbMusicCancel cancel,void *cancel_context);
#endif
