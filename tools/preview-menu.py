#!/usr/bin/env python3
"""Render the real draw() routine with host graphics stubs and SDK font pixels.

This checks layout only; it is not an emulator or a console screenshot.
Requires cc and the reference Portfolio checkout used by test-firmware.
"""
import base64
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent

STUBS = r'''
#include "ode.h"
#include "menu_config.h"
#include "menu_ui.h"
#include "menu_help.h"
#include "game_info.h"
#include "usb_art.h"
#include "cover_cache.h"
#include "usb_media.h"
#include "preview_v5.h"
#include "art_navigation.h"
#include "save_usb.h"
#include "save_names.h"
#include "save_catalog.h"
#include "save_cheats.h"
#include "save_cheats_usb.h"
#include "nvram_io.h"
#include "library_features.h"
#include "library_navigation.h"
#include "library_index.h"
#include "menu_settings.h"
#include "menu_position.h"
#include "ui_font.h"
#include "preview_audio.h"
#include "channel_gecko_data.h"
static int audio_starts,audio_restarts,audio_fail,display_count,audio_start_displays;
static unsigned long preview_now,preview_load_delay,preview_display_delay;
int preview_audio_start(PreviewAudio *a,unsigned char *p,unsigned long n,unsigned long rate) { (void)p;(void)n;(void)rate;++audio_starts;audio_start_displays=display_count;a->status=audio_fail ? -1 : 1;return audio_fail ? -1 : 0; }
int preview_audio_restart(PreviewAudio *a) { ++audio_restarts; return a->status==1 ? 0 : -1; }
void preview_audio_close(PreviewAudio *a) { a->status=0; }
#define UI_PREVIEW_TEXT(s) printf("L %s\n",s)
/* Glass: screen*keep/8 + colour*(8-keep)/8, placed exactly as rectangle() places fills. */
#define UI_PREVIEW_GLASS(menu,x,y,w,h,colour,keep) do { int m_=(menu)->config.screen_margin; \
    printf("G %d %d %d %d %u %d\n",m_+(x)*(320-2*m_)/320,m_+(y)*(240-2*m_)/240, \
        ((x)+(w))*(320-2*m_)/320-(x)*(320-2*m_)/320,((y)+(h))*(240-2*m_)/240-(y)*(240-2*m_)/240,(colour),(keep)); } while (0)
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#define abort() do { fprintf(stderr,"Preview assertion failed at line %d\n",__LINE__); exit(1); } while (0)
typedef unsigned long u32;
typedef unsigned char u8;
typedef int Item;
typedef unsigned int Color;
typedef unsigned long CelData;
typedef struct { int tag; long ccb_XPos, ccb_YPos; unsigned long ccb_Flags; void *ccb_SourcePtr; long ccb_HDX,ccb_HDY,ccb_VDX,ccb_VDY; void *ccb_PLUTPtr; unsigned long ccb_PRE0,ccb_PRE1; int ccb_Width,ccb_Height; } CCB;
typedef struct { CCB *font_CCB; } Font;
static CCB preview_font;
static Font preview_font_state={&preview_font};
static Font *GetCurrentFont(void) { return &preview_font_state; }
static void SetCurrentFontCCB(CCB *font) { preview_font=*font; }
#define CCB_LAST 0x40000000UL
#define CCB_BGND 0x20UL
#define CREATECEL_UNCODED 0
typedef struct { int sc_curScreen; Item sc_BitmapItems[2], sc_Screens[2]; } ScreenContext;
typedef struct { int last_error, timed_out; unsigned long commands; unsigned char last_command, last_reply[32], failed_command, failed_status; int failure_error; size_t last_length; } OdeBus;
static int preview_text_enabled, preview_reads, preview_writes, preview_read_fail, preview_write_fail;
static char preview_position[9216], preview_recent[8192], preview_favorites[8192], preview_diagnostics[4096];
static char *preview_text_slot(const char *name)
{
    if (!strcmp(name,"last-position.txt")) return preview_position;
    if (!strcmp(name,"recent.txt")) return preview_recent;
    if (!strcmp(name,"favorites.txt")) return preview_favorites;
    if (!strcmp(name,"diagnostics.txt")) return preview_diagnostics;
    return NULL;
}
static int preview_text_read(Ode *ode, OdeReadBuffer reader, const char *name, char *out, size_t capacity, size_t *length)
{
    char *slot=preview_text_slot(name);
    (void)ode; (void)reader;
    if (!preview_text_enabled || !slot) return -1;
    ++preview_reads;
    if (preview_read_fail || strlen(slot) >= capacity) return -1;
    strcpy(out,slot); *length=strlen(slot); return 0;
}
static int preview_text_write(Ode *ode, OdeReadBuffer reader, const char *name, const char *text)
{
    char *slot=preview_text_slot(name);
    (void)reader;
    if (!preview_text_enabled || !slot) return -1;
    ++preview_writes;
    if (preview_write_fail) return -1;
    strcpy(slot,text);
    ((OdeBus *)ode->context)->last_command=0xe4;
    return 0;
}
#define ControlUp 0x40000000UL
#define ControlDown 0x80000000UL
#define ControlLeft 0x10000000UL
#define ControlRight 0x20000000UL
#define ControlRightShift 0x00400000UL
#define ControlLeftShift 0x00200000UL
#define ControlA 0x08000000UL
#define ControlB 0x04000000UL
#define ControlC 0x02000000UL
#define ControlX 0x00800000UL
#define ControlStart 0x01000000UL

static int preview_png_loads, preview_png_fail;
static int preview_png_load(Ode *ode,OdeReadBuffer reader,const char *file,unsigned char *pixels,MediaCancel cancel,void *context) {
    (void)ode; (void)reader; (void)file;
    ++preview_png_loads;
    if (preview_png_fail || (cancel && cancel(context))) return -1;
    memset(pixels,preview_png_loads,USB_COVER_PIXELS); return 0;
}
static int preview_index_reads;
static int preview_art_index(Ode *ode,OdeReadBuffer read,UsbArtIndex *index) {
    (void)ode;(void)read;usb_art_index_free(index);++preview_index_reads;return 0;
}
static int preview_video_index(Ode *ode,OdeReadBuffer read,UsbArtIndex *index,const char *folder) {
    (void)folder;return preview_art_index(ode,read,index);
}
static int preview_metadata_index(Ode *ode,OdeReadBuffer read,LibraryMetadata *index) {
    (void)ode;(void)read;library_metadata_free(index);++preview_index_reads;return 0;
}
static unsigned long preview_buttons;
typedef struct { unsigned long cped_ButtonBits; } ControlPadEventData;
static int GetControlPad(int a, int b, ControlPadEventData *pad) { (void)a; (void)b; pad->cped_ButtonBits=preview_buttons; return 0; }
typedef struct { Color color; int x, y; } GrafCon;
typedef struct { int rect_XLeft, rect_YTop, rect_XRight, rect_YBottom; } Rect;
/* Small software display for comparing incremental draws with full redraws. */
static unsigned short preview_screen[2][320*240];
static int preview_raster,preview_front,preview_primitives;
static void preview_fill(int item,int x,int y,int w,int h,unsigned short color) {
    int row,col;
    if (!preview_raster) return;
    for (row=y;row<y+h && row<240;++row) for (col=x;col<x+w && col<320;++col)
        if (row>=0 && col>=0) preview_screen[item][row*320+col]=color;
}
static void preview_raster_cel(int item,CCB *cel,double sx,double sy) {
    int x,y,w=cel->ccb_Width,h=cel->ccb_Height;
    int stride=(int)(((cel->ccb_PRE1>>24)&255)+2)*4;
    double px=cel->ccb_XPos/65536.0,py=cel->ccb_YPos/65536.0;
    if (!preview_raster) return;
    if (cel->tag==1) { w=96;h=32; }
    for (y=0;y<h;++y) for (x=0;x<w;++x) {
        unsigned short color;
        int left=(int)(px+x*sx),top=(int)(py+y*sy),right=(int)(px+(x+1)*sx),bottom=(int)(py+(y+1)*sy);
        if (sx<0) { int t=left;left=right+1;right=t+1; }
        if (sy<0) { int t=top;top=bottom+1;bottom=t+1; }
        if (cel->tag==3) {
            int coverage=(((unsigned char *)cel->ccb_SourcePtr)[y*stride+x/2]>>(x%2 ? 0 : 4))&15;
            if (!coverage) continue;
            color=((unsigned short *)cel->ccb_PLUTPtr)[coverage];
        } else if (cel->tag==5) {
            color=((unsigned short *)cel->ccb_SourcePtr)[y*(stride/2)+x];
            if (!color) continue;
        } else color=(unsigned short)(cel->tag==1 ? 0x3abc : 0x1234+(x+y)%16);
        preview_fill(item,left,top,right-left,bottom-top,color);
    }
}
#define MakeRGB15(r,g,b) (((r)<<10)|((g)<<5)|(b))
static void SetFGPen(GrafCon *g, Color c) { g->color = c; }
static void FillRect(Item item, GrafCon *g, Rect *r) {
    ++preview_primitives;
    preview_fill(item,r->rect_XLeft,r->rect_YTop,r->rect_XRight-r->rect_XLeft+1,r->rect_YBottom-r->rect_YTop+1,(unsigned short)g->color);
    printf("R %d %d %d %d %u\n", r->rect_XLeft, r->rect_YTop,
        r->rect_XRight-r->rect_XLeft+1, r->rect_YBottom-r->rect_YTop+1, g->color);
}
static void MoveTo(GrafCon *g, int x, int y) { g->x=x; g->y=y; }
static void DrawText8(GrafCon *g, Item item, const u8 *s) {
    double sx=preview_font.ccb_HDX ? preview_font.ccb_HDX/1048576.0 : 1.0;
    double sy=preview_font.ccb_VDY ? preview_font.ccb_VDY/65536.0 : 1.0;
    (void)item; printf("T %d %d %u %.6f %.6f %s\n", g->x, g->y, g->color, sx, sy, s);
}
static int preview_video_enabled, preview_video_loads;
/* A two-frame OM3DVID5 clip: solid 0x1111, then solid 0x2222, each as SOLID
 * runs over the 1200 blocks (the second colour is the cache's entry 1). */
static int preview_video_load(Ode *ode, OdeReadBuffer reader, const char *file, UsbPreview *preview, MediaCancel cancel, void *context) {
    unsigned char *packet;
    int frame, op;
    (void)ode; (void)reader; (void)file;
    ++preview_video_loads;
    if (preview_video_enabled<0) return preview_video_enabled;
    if (!preview_video_enabled || (cancel && cancel(context))) return -1;
    preview->table=(unsigned char *)malloc(12);
    preview->chunk[0]=(unsigned char *)calloc(1,78+PREVIEW_V5_READ_AHEAD);
    preview->decoder=calloc(1,sizeof(PreviewV5));
    preview->frame_buffer=(unsigned char *)calloc(1,USB_PREVIEW_FRAME_BYTES);
    preview->audio=(unsigned char *)malloc(4);
    if (!preview->table || !preview->chunk[0] || !preview->decoder || !preview->frame_buffer || !preview->audio) abort();
    for (frame=0;frame<3;++frame) {
        unsigned long offset=12UL+39UL*(unsigned long)frame;
        preview->table[frame*4]=0; preview->table[frame*4+1]=0;
        preview->table[frame*4+2]=(unsigned char)(offset>>8); preview->table[frame*4+3]=(unsigned char)offset;
    }
    for (frame=0;frame<2;++frame) {
        packet=preview->chunk[0]+39*frame;
        *packet++=0xbf; *packet++=(unsigned char)(0x11*(frame+1)); *packet++=(unsigned char)(0x11*(frame+1));
        for (op=0;op<17;++op) { *packet++=0xbf; *packet++=(unsigned char)(0x80|frame); }
        *packet++=0xaf; *packet=(unsigned char)(0x80|frame);
    }
    preview->chunk_bytes[0]=78+PREVIEW_V5_READ_AHEAD; preview->chunk_begin[0]=12; preview->chunk_end[0]=90; preview->chunk_count=1;
    preview->audio_bytes=preview->audio_frames=4; preview->audio_rate=11025;
    preview->width=USB_PREVIEW_WIDTH; preview->height=USB_PREVIEW_HEIGHT; preview->frame_bytes=USB_PREVIEW_FRAME_BYTES;
    preview->frames=preview->source_frames=2; preview->fps=6; preview->table_bytes=12; preview->video_bytes=90;
    /* Like the real loader, frame 0 is decoded before playback starts. */
    if (usb_preview_frame(preview,0,preview->frame_buffer)<0) abort();
    return 0;
}
static int preview_video_load_profiled(Ode *ode,OdeReadBuffer reader,const char *file,UsbPreview *preview,MediaCancel cancel,void *context,MediaLoadPhase phase,void *phase_context,int sound) {
    int result;
    (void)sound;
    result=preview_video_load(ode,reader,file,preview,cancel,context);
    preview_now+=preview_load_delay;
    if (phase) { phase(phase_context,1); phase(phase_context,2); }
    return result;
}
static int preview_unloads;
static int preview_cel_calls, preview_cel_fail;
static CCB preview_loaded={2, 0, 0, 0};
#define MEMTYPE_CEL 0
static CCB *LoadCel(char *file, unsigned long flags) {
    (void)file; (void)flags; ++preview_cel_calls;
    preview_loaded.ccb_Width=64; preview_loaded.ccb_Height=80;
    return preview_cel_fail ? NULL : &preview_loaded;
}
static CCB *CreateCel(int w, int h, int bpp, int flags, void *data) {
    static unsigned char pixels[USB_PREVIEW_FRAME_BYTES];
    (void)flags; (void)data;
    if (bpp==4) {
        int stride=w/2;
        CCB *cel=(CCB *)calloc(1,sizeof(CCB));
        if (stride<8) stride=8;
        if (!cel) abort();
        cel->tag=3;cel->ccb_SourcePtr=calloc((size_t)stride,h);cel->ccb_PLUTPtr=calloc(32,2);
        if (!cel->ccb_SourcePtr || !cel->ccb_PLUTPtr) abort();
        cel->ccb_PRE1=(unsigned long)(stride/4-2)<<24;
        cel->ccb_HDX=1<<20;cel->ccb_VDY=1<<16;
        cel->ccb_Width=w;cel->ccb_Height=h;return cel;
    }
    if (bpp==16 && ((data && w==86 && h==120) || (!data && ((w==38 && h==16) || (w==80 && h==8) || (w==16 && h==12) || (w==9 && h==9))))) {
        /* Gecko TV interface cels: TV static, channel digits, bar, CH, screen corner. */
        int stride=(w*2+3)&~3;
        CCB *cel=(CCB *)calloc(1,sizeof(CCB));
        if (stride<8) stride=8;
        if (!cel) abort();
        cel->tag=5;cel->ccb_SourcePtr=data ? data : calloc((size_t)stride,h);
        if (!cel->ccb_SourcePtr) abort();
        cel->ccb_PRE1=(unsigned long)(stride/4-2)<<24;
        cel->ccb_HDX=1<<20;cel->ccb_VDY=1<<16;cel->ccb_Width=w;cel->ccb_Height=h;return cel;
    }
    preview_loaded.ccb_SourcePtr=data ? data : pixels; preview_loaded.ccb_Width=w; preview_loaded.ccb_Height=h;
    ++preview_cel_calls;
    return preview_cel_fail ? NULL : &preview_loaded;
}
static int ode_bus_read_buffer(void *context, unsigned char *data, size_t size) {
    (void)context; (void)data; (void)size; return -1;
}
static int ode_bus_transfer(void *context, const unsigned char *command, unsigned char *reply, size_t capacity, size_t *length) {
    (void)context; (void)command; (void)reply; (void)capacity; (void)length; return -1;
}
static long GetMSecTime(Item item) { (void)item; return (long)preview_now; }
static void WaitVBL(Item item, int fields) { (void)item; (void)fields; }
static void UnloadCel(CCB *cel) { if(cel->tag==3) {free(cel->ccb_SourcePtr);free(cel->ccb_PLUTPtr);free(cel);} else ++preview_unloads; }
static int GetVBLTime(Item item, u32 *hi, u32 *frame) {
    (void)item; *hi=0; *frame=0; return 0;
}
static void DrawCels(Item item, CCB *cel) {
    double sx=cel->ccb_HDX ? cel->ccb_HDX/1048576.0 : 1.0;
    double sy=cel->ccb_VDY ? cel->ccb_VDY/65536.0 : 1.0;
    ++preview_primitives;
    preview_raster_cel(item,cel,sx,sy);
    if (cel->tag==3) {
        int i;
        printf("B %.6f %.6f %.6f %.6f ",cel->ccb_XPos/65536.0,cel->ccb_YPos/65536.0,sx,sy);
        for (i=0;i<16;++i) printf("%s%u",i ? "," : "",((unsigned short *)cel->ccb_PLUTPtr)[i]);
        printf(" %d %d ",cel->ccb_Width,cel->ccb_Height);
        for (i=0;i<UI_FONT_STRIDE*cel->ccb_Height;++i) {
            int stride=(int)(((cel->ccb_PRE1>>24)&255)+2)*4;
            int row=i/UI_FONT_STRIDE,col=i%UI_FONT_STRIDE;
            printf("%02x",col<stride ? ((unsigned char *)cel->ccb_SourcePtr)[row*stride+col] : 0);
        }
        printf("\n"); return;
    }
    if (cel->tag==5) {
        int i;
        int stride=(int)(((cel->ccb_PRE1>>24)&255)+2)*2;
        printf("P %.6f %.6f %.6f %.6f %d %d ",cel->ccb_XPos/65536.0,cel->ccb_YPos/65536.0,sx,sy,cel->ccb_Width,cel->ccb_Height);
        for (i=0;i<cel->ccb_Width*cel->ccb_Height;++i) printf("%04x",((unsigned short *)cel->ccb_SourcePtr)[(i/cel->ccb_Width)*stride+i%cel->ccb_Width]);
        printf("\n"); return;
    }
    (void)item; printf("I %d %.6f %.6f %.6f %.6f\n",cel->tag==2 && cel->ccb_Width==160 ? 4 : cel->tag,cel->ccb_XPos/65536.0,cel->ccb_YPos/65536.0,sx,sy);
}
static void DisplayScreen(Item item, int flag) { preview_front=item; (void)flag;++display_count;preview_now+=preview_display_delay; }
static int preview_read_directory(Ode *ode, OdeListing *listing);
const MenuCover menu_covers[] = {
    {"Alone in the Dark.iso", "$boot/covers/dark.cel"},
    {"Road Rash.iso", "$boot/covers/road.cel"}
};
const size_t menu_cover_count=2;
'''

NVRAM_STUBS = r'''
static int preview_mounts;
static int preview_mount(void *context,const unsigned char *packet,unsigned char *reply,size_t capacity,size_t *length) {
    Menu *menu=(Menu *)context;
    if (packet[0]!=0xc5 || menu->preview_audio.status || capacity<2) abort();
    ++preview_mounts; reply[0]=0xc5; reply[1]=0; *length=2; return 0;
}
static void activate(Menu *menu) { (void)menu; }
static int preview_nvram_reads, preview_nvram_writes, preview_nvram_fail;
static int preview_nvram_fixture(SaveArchive *a) {
    SaveEntry e[3];
    memset(e, 0, sizeof(e));
    strcpy(e[0].name, "EA_RRX"); e[0].data=(const unsigned char *)"save"; e[0].size=4; e[0].type=0x33444f46UL;
    strcpy(e[1].name, "NFS_Config"); e[1].data=(const unsigned char *)"other"; e[1].size=5;
    strcpy(e[2].name, "SY-TETSU-1J"); e[2].data=(const unsigned char *)"tetsujin"; e[2].size=8;
    return save_archive_build(a, e, 3);
}
int nvram_snapshot(SaveArchive *a, NvramInfo *info) {
    ++preview_nvram_reads;
    info->free_bytes=14000; info->used_bytes=18000;
    if (preview_nvram_fail) { info->error="Fixture NVRAM error"; return -1; }
    return preview_nvram_fixture(a);
}
int nvram_preflight(const SaveEntry *entry, NvramInfo *info) { (void)entry; (void)info; return 0; }
int nvram_restore(const SaveEntry *entry, NvramInfo *info) {
    (void)entry; (void)info; ++preview_nvram_writes; return 0;
}
'''

MAIN = r'''
static Menu *reading_menu;
static int reading_fail,reading_transition;
static int preview_read_directory(Ode *ode, OdeListing *listing) {
    if (reading_transition) {
        int before=preview_primitives;
        if (reading_menu->listing.count!=4 || !reading_menu->loading_notice ||
            reading_menu->ready || strcmp(reading_menu->breadcrumb,"Root filtered") || before>3) abort();
        preview_now+=100;ode->progress(ode->progress_context,4);
        if (preview_primitives!=before) abort(); /* Never redraw the old root at the new depth. */
        (void)listing;return 0;
    }
    /* Exercise the actual load/progress code while checking old ownership. */
    if (reading_menu->listing.count != 9 || !reading_menu->loading ||
        reading_menu->ready || !reading_menu->view_valid ||
        strcmp(reading_menu->breadcrumb, "/ STORAGE")) abort();
    ode->progress(ode->progress_context, 1);
    if (reading_menu->listing.count != 9 || reading_menu->selected != 2) abort();
    (void)listing;
    if (reading_fail) { ode->error="Fixture read failed"; return -1; }
    return 0; /* successful empty directory */
}
int main(int argc, char **argv) {
    Menu menu;
    CCB logo={1,0,0,0}, cover={2,0,0,0};
    memset(&menu, 0, sizeof(menu));
    menu_config_defaults(&menu.config);
    menu.saved_config=menu.config; menu.settings_remember_position=menu.settings_recent_games=1;
    menu.settings_usb_covers=1;
    menu.font_ccb=*GetCurrentFont()->font_CCB; menu.have_font=1;
    logo.ccb_XPos=16L<<16; logo.ccb_YPos=8L<<16; logo.ccb_HDX=1L<<20; logo.ccb_VDY=1L<<16;
    cover.ccb_Width=64; cover.ccb_Height=80;
    cover.ccb_XPos=236L<<16; cover.ccb_YPos=86L<<16; cover.ccb_HDX=1L<<20; cover.ccb_VDY=1L<<16;
    show_demo(&menu);
    init_channel_gecko(&menu);
    menu.logo=&logo;
    menu.message=NULL;
    if (argc>1 && !strcmp(argv[1],"browser-render")) {
        static const int selections[]={2,2,2,3,4,2,2,12,13,12,1,0,24,23,23,23,23,23,23,23,23,23,23,23,23,23,23,23,23,23};
        static unsigned short actual[320*240];
        MenuEntry entries[25]; const MenuEntry *pointers[25]; char names[25][96],notice[80];
        int margin,large,step,i,partial=0,full=0,idle=0;
        free_listing(&menu);
        for (i=0;i<25;++i) {
            sprintf(names[i],i%2 ? "A very long game title number %02d with extra words.iso" : "Game %02d.iso",i);
            entries[i].name=names[i];entries[i].toc_id=i+1;entries[i].is_directory=i==1;
            pointers[i]=entries+i;
        }
        menu.listing.entries=pointers;menu.listing.count=25;menu.listing.has_parent=0;
        menu.screen.sc_BitmapItems[0]=menu.screen.sc_Screens[0]=0;
        menu.screen.sc_BitmapItems[1]=menu.screen.sc_Screens[1]=1;
        preview_raster=1;
        for (large=0;large<2;++large) for (margin=0;margin<=12;++margin) {
            menu.config.screen_margin=margin;menu.config.large_titles=large;
            menu.browser_frames[0].valid=menu.browser_frames[1].valid=0;
            menu.selected=2;menu.cover=NULL;menu.notice_active=0;menu.tools=menu.alphabet_active=0;
            menu.clear_frames=2;preview_now=10000;menu.selection_since=preview_now;
            for (step=0;step<79;++step) {
                BrowserFrame saved[2]; int current,cost,front;
                int selected=selections[step<30 ? step : 29];
                if (menu.selected!=(size_t)selected) selection_changed(&menu);
                menu.selected=selected;
                if (step==14) { cover.ccb_Width=64;cover.ccb_Height=80;menu.cover=&cover; }
                if (step==16) menu.cover=NULL;
                if (step==17) { strcpy(notice,"Loading cancelled");menu.message=notice; }
                if (step==18) strcpy(notice,"Same buffer, different message");
                if (step==19) menu.message=NULL;
                if (step==20) { notify(&menu,"Disc added"); }
                if (step==21) preview_now+=2600;
                if (step==22) menu.alphabet_active=1;
                if (step==23) menu.alphabet_active=0;
                if (step==24) menu.tools=TOOL_HOME;
                if (step==25) menu.tools=0;
                if (step==26) { menu.gecko_active=0; selection_changed(&menu); }
                if (step>26) preview_now+=20;
                if (step==48) { menu.alphabet_active=1;menu.jump_letter=0; }
                if (step>=49 && step<=75) menu.jump_letter=(step-48)%27;
                if (step==76) menu.alphabet_active=0;
                preview_primitives=0;draw(&menu);cost=preview_primitives;
                if (step==2) { if (cost) abort(); ++idle; }
                if (step>=28 && step<=42 && cost>1) abort(); /* Both buffers primed; only the mascot changes. */
                if (step>=50 && step<=75 && cost>5) abort(); /* Only the old and new letters change. */
                partial+=cost;front=preview_front;current=menu.screen.sc_curScreen;
                memcpy(actual,preview_screen[front],sizeof(actual));
                memcpy(saved,menu.browser_frames,sizeof(saved));
                menu.browser_frames[0].valid=menu.browser_frames[1].valid=0;
                menu.screen.sc_curScreen=front^1;
                preview_primitives=0;draw(&menu);full+=preview_primitives;
                if (memcmp(actual,preview_screen[front],sizeof(actual))) {
                    for (i=0;i<320*240;++i) if (actual[i]!=preview_screen[front][i]) break;
                    fprintf(stderr,"Browser mismatch: margin %d large %d step %d pixel %d,%d\n",margin,large,step,i%320,i/320);abort();
                }
                memcpy(menu.browser_frames,saved,sizeof(saved));menu.screen.sc_curScreen=current;
            }
        }
        if (!idle || partial>=full*3/4) abort();
        fprintf(stderr,"Browser rendering: 26 margin/font combinations match full redraws; %d versus %d drawing calls; idle draws zero\n",partial,full);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "self-test")) {
        /* Check the corner mask using the SDK's padded source row layout. */
        {
            int x,y;
            unsigned short *pixels;
            crt_cels(&menu);
            if (!menu.screen_corner || ((menu.screen_corner->ccb_PRE1>>24)&255)!=3) abort();
            pixels=(unsigned short *)menu.screen_corner->ccb_SourcePtr;
            for (y=0;y<9;++y) for (x=0;x<10;++x) {
                int dx=2*(9-x)-1,dy=2*(9-y)-1;
                unsigned short expected=(x<9 && dx*dx+dy*dy>18*18) ? (unsigned short)INK : 0;
                if (pixels[y*10+x]!=expected) abort();
            }
        }
        /* A held direction must not keep resetting the remote to frame zero. */
        preview_now=1000;menu.gecko_active=0;selection_changed(&menu);
        if (!menu.gecko_active || menu.gecko_since!=1000 || channel_gecko_frame(&menu,1000)!=0) abort();
        preview_now+=66;selection_changed(&menu);
        if (menu.gecko_since!=1000 || channel_gecko_frame(&menu,1180)!=9) abort();
        if (channel_gecko_frame(&menu,1320)!=0 || menu.gecko_active) abort();
        {
            Menu probe;
            LibraryGameInfo info={"Folder/Disc.iso","","",""};
            MenuEntry folder={1,1,"Folder"};
            memset(&probe,0,sizeof(probe));
            probe.metadata.entries=&info;probe.metadata.count=1;
            if (!game_entry(&probe,&folder) || probe.entry_type_next!=1) abort();
            if (!game_entry(&probe,&folder) || probe.entry_type_next!=1) abort(); /* Cached. */
            probe.metadata.count=0;
            if (game_entry(&probe,&folder)) abort();
            probe.metadata.count=1;++probe.listing_generation;
            if (!game_entry(&probe,&folder)) abort();
        }
        menu.tool_selected=0;
        {
            Menu titles;
            MenuEntry entries[]={
                {1,0,"Game (Europe).iso"},{2,0,"Game (Japan) (RE1).iso"},
                {3,0,"Game (USA).iso"},{4,0,"Game (USA) (RE1).iso"},
                {5,0,"Game (USA) (Unl).iso"}};
            const MenuEntry *pointers[5];int i;
            memset(&titles,0,sizeof(titles));menu_config_defaults(&titles.config);
            for (i=0;i<5;++i) pointers[i]=entries+i;
            titles.listing.entries=pointers;titles.listing.count=5;titles.ready=titles.view_valid=1;
            if (title_variant(&titles,0)!=1 || title_variant(&titles,2)!=2 ||
                title_variant(&titles,1)!=1 || title_variant(&titles,3)!=2 || title_variant(&titles,4)) abort();
            titles.selected=2;draw_row(&titles,2,0,2,UI_FONT_SMALL,14);
            if (strcmp(titles.text_cache[0].value,"Game [2]")) abort();
            titles.text_cursor=0;draw_title_card(&titles,entries+2,5,NULL,320);
            if (strcmp(titles.text_cache[0].value,"Game [2]")) abort();
            strcpy(titles.filter,"Europe");if (title_variant(&titles,0)) abort();
            titles.filter[0]=0;if (title_variant(&titles,2)!=2) abort();
            free(titles.title_variants);free((void *)titles.visible);
        }
        {
            Menu label_menu;
            const char *name="Dennis Miller - That's News To Me";
            size_t length=strlen(name);
            int margin,style,card;
            memset(&label_menu,0,sizeof(label_menu));menu_config_defaults(&label_menu.config);
            for (margin=0;margin<=12;++margin) for (style=0;style<4;++style) for (card=0;card<2;++card) {
                int x=card ? 160 : 16,room=card ? 152 : 122,scale=320-2*margin;
                int actual=room*scale/320,limit=(308-x)*scale/320;
                size_t tail;
                UiText *cached;
                if (actual>limit) actual=limit;
                label_menu.config.screen_margin=margin;
                tail=ui_font_tail_start(name,length,actual,style);
                label_menu.scroll_elapsed=1200UL+(unsigned long)tail*180UL+300UL;
                label_menu.scroll_count=0;label_menu.text_cursor=0;
                scroll_label(&label_menu,x,181,room,name,length,style,TEXT,BG,1);
                cached=&label_menu.text_cache[0];
                if (strcmp(cached->value,name+tail) || cached->value[strlen(cached->value)-1]!='e' ||
                    ui_font_fit(cached->value,strlen(cached->value),cached->width,style)!=strlen(cached->value)) abort();
                if (label_menu.scroll_visible[0]!=length-tail) abort(); /* Cache uses the same end stop. */
            }
        }
        {
            Menu root;
            static const char *names[]={"Games","Adult","boot.iso",".ohmy3do"};
            int i;
            memset(&root,0,sizeof(root));menu_config_defaults(&root.config);
            root.ready=root.view_valid=root.demo=1;strcpy(root.breadcrumb,"Root filtered");
            root.listing.entries=(const MenuEntry **)malloc(4*sizeof(MenuEntry *));root.listing.count=4;
            for (i=0;i<4;++i) {
                MenuEntry *entry=(MenuEntry *)malloc(sizeof(MenuEntry)+strlen(names[i])+1);
                if (!entry) abort();
                entry->name=(char *)(entry+1);strcpy((char *)entry->name,names[i]);
                entry->is_directory=i!=2;entry->toc_id=i+1;root.listing.entries[i]=entry;
            }
            menu_set_hide_adult(1);
            if (row_count(&root)!=1 || strcmp(browser_entry(&root,0)->name,"Games")) abort();
            draw(&root); /* The already-filtered root that the user sees. */
            root.depth=1;root.path_ids[0]=1;strcpy(root.path[0],"Games");
            root.ode.progress=progress;root.ode.progress_context=&root;
            reading_menu=&root;reading_transition=1;preview_primitives=0;
            load_directory(&root,0);
            if (!root.ready || root.loading || root.listing.count || strcmp(root.breadcrumb,"/ Games")) abort();
            reading_transition=0;menu_set_hide_adult(menu.config.hide_adult);
        }
        reading_menu=&menu;
        menu.selected=2;
        menu.ode.progress=progress;
        menu.ode.progress_context=&menu;
        menu.depth=1;
        strcpy(menu.path[0], "Destination");
        load_directory(&menu, 0);
        if (!menu.ready || !menu.view_valid || menu.loading || menu.listing.count ||
            strcmp(menu.breadcrumb,"/ Destination")) abort();
        show_demo(&menu);
        menu.selected=2;
        reading_fail=1;
        load_directory(&menu, 0);
        if (menu.ready || menu.view_valid || menu.loading || menu.listing.count != 9 ||
            strcmp(menu.message, "Fixture read failed")) abort();
        preview_now=10000;
        menu.cover=&cover;
        selection_changed(&menu);
        if (menu.selection_since != 10000 || menu.cover || preview_unloads != 1) abort();
        notify(&menu,"Disc added");
        preview_now+=2499;
        draw(&menu);
        if (!menu.notice_active) abort();
        ++preview_now;
        draw(&menu);
        if (menu.notice_active) abort();
        show_demo(&menu);
        menu.selected=2;
        menu.covers_enabled=1;
        preview_now=20000;
        selection_changed(&menu);
        preview_now+=COVER_IDLE_MS-1;
        update_cover(&menu);
        if (preview_cel_calls) abort();
        ++preview_now;
        update_cover(&menu);
        if (preview_cel_calls != 1 || !menu.cover ||
            menu.cover->ccb_XPos != 236L*65536 || menu.cover->ccb_YPos != 86L*65536) abort();
        update_cover(&menu);
        if (preview_cel_calls != 1) abort();
        selection_changed(&menu);
        if (menu.cover) abort();
        preview_cel_fail=1;
        preview_now+=COVER_IDLE_MS;
        update_cover(&menu);
        if (preview_cel_calls != 2 || menu.cover) abort();
        update_cover(&menu);
        if (preview_cel_calls != 2) abort();
        menu.selected=0; /* folder */
        selection_changed(&menu);
        preview_now+=COVER_IDLE_MS;
        update_cover(&menu);
        if (preview_cel_calls != 2) abort();
        menu.selected=5; /* file without a matching cover */
        selection_changed(&menu);
        preview_now+=COVER_IDLE_MS;
        update_cover(&menu);
        if (preview_cel_calls != 2 || !menu.cover_attempted) abort();
        /* Real cover scheduling/cache path; replace only the file decoder. */
        menu.demo=0; menu.ready=menu.view_valid=menu.covers_enabled=1;
        menu.config_result=0; strcpy(menu.ode.identity,"LOCODE");
        menu.usb_art.entries=(MenuCover *)malloc(2*sizeof(MenuCover)); menu.usb_art.count=2;
        menu.usb_art.entries[0].path="Alone in the Dark.iso"; menu.usb_art.entries[0].file="Dark.png";
        menu.usb_art.entries[1].path="Disc 1.iso"; menu.usb_art.entries[1].file="Disc.png";
        menu.selected=2; preview_cel_fail=0;
        selection_changed(&menu); preview_now+=COVER_IDLE_MS; update_cover(&menu);
        if (!menu.cover || preview_png_loads!=1 || menu.cover_cached || menu.cover->ccb_Width!=USB_COVER_WIDTH || menu.cover->ccb_Height!=USB_COVER_HEIGHT) abort();
        menu.selected=3; selection_changed(&menu); preview_now+=COVER_IDLE_MS-1; update_cover(&menu);
        if (menu.cover || preview_png_loads!=1) abort();
        ++preview_now; update_cover(&menu); if (!menu.cover || preview_png_loads!=2) abort();
        menu.selected=2; selection_changed(&menu); update_cover(&menu);
        if (!menu.cover || preview_png_loads!=2 || !menu.cover_cached || menu.cover_commands ||
            ((unsigned char *)menu.cover->ccb_SourcePtr)[0]!=1) abort();
        cover_cache_clear(&menu.cover_cache); selection_changed(&menu); preview_png_fail=1;
        preview_now+=COVER_IDLE_MS; update_cover(&menu);
        if (menu.cover || cover_cache_contains(&menu.cover_cache,0,"Dark.png")) abort();
        preview_png_fail=0; preview_buttons=ControlB; selection_changed(&menu);
        preview_now+=COVER_IDLE_MS; update_cover(&menu);
        if (menu.cover || cover_cache_contains(&menu.cover_cache,0,"Dark.png")) abort();
        preview_buttons=0; usb_art_index_free(&menu.usb_art);
        /* RAM playback uses the real loader scheduling, draw and cleanup code. */
        menu.demo=0; menu.selected=2; menu.ready=1; menu.covers_enabled=1;
        menu.config_result=0; strcpy(menu.ode.identity,"LOCODE");
        menu.usb_previews.entries=(MenuCover *)malloc(sizeof(MenuCover));
        if (!menu.usb_previews.entries) abort();
        menu.usb_previews.entries[0].path="Alone in the Dark.iso";
        menu.usb_previews.entries[0].file="Game.3dv"; menu.usb_previews.count=1;
        menu.config.video_previews=1; menu.config.preview_sound=1; preview_video_enabled=1; preview_cel_fail=0;
        selection_changed(&menu); preview_now+=PREVIEW_IDLE_MS-1; update_preview(&menu);
        if (preview_video_loads || menu.preview_attempted) abort();
        ++preview_now;
        menu.tools=TOOL_HOME; update_preview(&menu); menu.tools=0;
        menu.tools_requested=1;update_preview(&menu);menu.tools_requested=0;
        menu.alphabet_active=1; update_preview(&menu); menu.alphabet_active=0;
        menu.review=1; update_preview(&menu); menu.review=0;
        /* A direction queued by a cancelled cover read must move the list
         * before another media load can start. */
        menu.cancel_directions=ControlDown; update_preview(&menu); menu.cancel_directions=0;
        menu.config.video_previews=0; update_preview(&menu);
        if (preview_video_loads) abort();
        menu.config.video_previews=1; preview_load_delay=5000;preview_display_delay=17;update_preview(&menu);
        preview_load_delay=preview_display_delay=0;
        if (preview_video_loads != 1 || !menu.preview_cel || !menu.preview.table || audio_starts!=1 || menu.preview_audio.status!=1) abort();
        if (menu.preview_priming || menu.preview_frame || menu.clear_frames || menu.panel_frames!=2 || audio_start_displays!=display_count) abort();
        /* The title holds its exact pre-load scroll position through a slow
         * read and both initial redraws, then resumes from that position. */
        if (menu.scroll_paused || menu.scroll_elapsed!=PREVIEW_IDLE_MS || preview_now-menu.selection_since!=PREVIEW_IDLE_MS) abort();
        {
            unsigned long before=overlay_key(&menu,NULL);
            preview_now+=20;
            if (overlay_key(&menu,NULL)!=before) abort(); /* Uses this frame's snapshot. */
            preview_now-=20;
        }
        preview_now+=167; draw(&menu);
        if (menu.preview_frame != 1 || ((unsigned char *)menu.preview_cel->ccb_SourcePtr)[0] != 0x22) abort();
        preview_now+=167; draw(&menu);
        if (menu.preview_frame || ((unsigned char *)menu.preview_cel->ccb_SourcePtr)[0] != 0x11 || audio_restarts!=1) abort();
        free(menu.preview.decoder); menu.preview.decoder=NULL; preview_now+=167; draw(&menu);
        if (menu.preview_result!=-3) abort();
        update_preview(&menu);
        if (menu.preview.table || menu.preview.audio || menu.preview_cel || menu.preview_audio.status || preview_video_loads != 1) abort();
        selection_changed(&menu); preview_now+=PREVIEW_IDLE_MS+270; preview_buttons=ControlB;
        update_preview(&menu); preview_buttons=0;
        /* A held button cancels during the channel blink, before any read. */
        if (menu.preview.table || menu.preview_cel || !menu.preview_attempted || preview_video_loads != 1 || menu.preview_result!=-5) abort();
        selection_changed(&menu); preview_now+=PREVIEW_IDLE_MS+270; preview_cel_fail=1;
        update_preview(&menu); preview_cel_fail=0;
        if (menu.preview.table || menu.preview_cel || menu.preview_result!=-4) abort();
        menu.config.preview_sound=0; selection_changed(&menu); preview_now+=PREVIEW_IDLE_MS+270;
        update_preview(&menu);
        if (!menu.preview_cel || menu.preview_audio.status || audio_starts!=1) abort();
        menu.config.preview_sound=1; selection_changed(&menu); preview_now+=PREVIEW_IDLE_MS+270; audio_fail=1;
        update_preview(&menu); audio_fail=0;
        if (!menu.preview_cel || menu.preview_audio.status!=-1) abort();
        stop_preview(&menu);
        /* X swaps the playing preview for the cover until it is pressed again;
         * the next load starts at once, and moving on forgets the choice. */
        {
            int loads;
            selection_changed(&menu); preview_now+=PREVIEW_IDLE_MS+270; update_preview(&menu);
            loads=preview_video_loads;
            if (!menu.preview_cel || !toggle_preview(&menu) || !menu.preview_off || menu.preview_cel) abort();
            update_preview(&menu); if (preview_video_loads!=loads) abort();
            if (!toggle_preview(&menu) || menu.preview_off) abort();
            update_preview(&menu); if (preview_video_loads!=loads+1 || !menu.preview_cel) abort();
            /* Exercise the actual tap/hold dispatcher while video is playing. */
            if (media_x_input(&menu,ControlX,ControlX,0) || !menu.x_pending || !menu.preview_cel) abort();
            preview_now+=100;
            if (!(media_x_input(&menu,0,0,ControlX)&ControlX) || menu.x_pending) abort();
            if (!toggle_preview(&menu) || !menu.preview_off || menu.preview_cel) abort();
            if (!toggle_preview(&menu)) abort(); update_preview(&menu);
            loads=preview_video_loads;
            if (media_x_input(&menu,ControlX,ControlX,0) || !menu.x_pending) abort();
            preview_now+=499; media_x_input(&menu,ControlX,0,0);
            if (menu.full_view) abort();
            preview_now+=1; media_x_input(&menu,ControlX,0,0);
            if (menu.full_view!=2 || !menu.preview_cel || menu.x_pending || preview_video_loads!=loads) abort();
            {
                unsigned int frame=menu.preview_frame;
                long hdx=menu.preview_cel->ccb_HDX;
                menu.preview_since=preview_now; preview_now+=167; draw_full_cover(&menu);
                if (menu.preview_frame==frame || menu.preview_cel->ccb_HDX!=hdx) abort();
            }
            close_full_cover(&menu);
            if (menu.full_view || !menu.preview_cel || preview_video_loads!=loads) abort();
            if (!toggle_preview(&menu)) abort();
            menu.cover=&cover; menu.cover_file=NULL;
            media_x_input(&menu,ControlX,ControlX,0); preview_now+=500;
            media_x_input(&menu,ControlX,0,0);
            if (menu.full_view!=1 || menu.x_pending || !menu.preview_off) abort();
            /* A held X must not cancel the large cover's USB read; a fresh
             * press after release still cancels immediately. */
            menu.media_cancelled=0; preview_buttons=ControlX;
            if (media_cancel(&menu)) abort();
            preview_buttons=0; if (media_cancel(&menu) || menu.suppress_buttons) abort();
            preview_buttons=ControlX; if (!media_cancel(&menu)) abort();
            preview_buttons=0; menu.media_cancelled=0;
            close_full_cover(&menu); menu.cover=NULL;
            if (media_x_input(&menu,0,0,ControlX) || menu.full_view) abort();
            if (!toggle_preview(&menu)) abort(); update_preview(&menu);
            menu.suppress_buttons=0;
            /* Another button cancels the hold without a delayed X tap. */
            media_x_input(&menu,ControlX,ControlX,0);
            media_x_input(&menu,ControlX|ControlDown,ControlDown,0);
            if (menu.x_pending || media_x_input(&menu,0,0,ControlX)) abort();
            menu.preview_off=1; selection_changed(&menu); if (menu.preview_off || menu.preview_cel) abort();
            stop_preview(&menu);
        }
        selection_changed(&menu);preview_now+=PREVIEW_IDLE_MS+270;preview_video_enabled=2;
        {
            unsigned char cache_pixels[USB_COVER_PIXELS];
            memset(cache_pixels,0,sizeof(cache_pixels));
            cover_cache_put(&menu.cover_cache,0,"Cached.png",cache_pixels,sizeof(cache_pixels));
            update_preview(&menu);
            if (!menu.preview_cel || menu.preview_cel->ccb_SourcePtr!=menu.preview.frame_buffer ||
                ((unsigned char *)menu.preview_cel->ccb_SourcePtr)[0]!=0x11 || menu.cover_cache.clock) abort();
        }
        stop_preview(&menu);
        selection_changed(&menu);preview_now+=PREVIEW_IDLE_MS+270;preview_video_enabled=USB_PREVIEW_NO_MEMORY;
        update_preview(&menu);
        if (menu.preview_result!=-4 || menu.ode.navigation_failed || !menu.ready ||
            strcmp(menu.notice,"Preview needs more free memory.")) abort();
        selection_changed(&menu);preview_now+=PREVIEW_IDLE_MS+270;preview_video_enabled=-1;
        update_preview(&menu);
        if (menu.preview_result!=-1 || strcmp(menu.notice,"Preview failed. Check diagnostics.")) abort();
        usb_art_index_free(&menu.usb_previews); preview_video_enabled=0;
        menu.config.video_previews=0; menu.demo=1;
        /* Cancelling with A must not also open/play on the next frame.
         * Directional cancellation still advances the browser selection. */
        menu.media_cancelled=0; menu.suppress_buttons=0; preview_buttons=ControlA;
        if (!media_cancel(&menu) || !(menu.suppress_buttons&ControlA)) abort();
        menu.media_cancelled=0;
        if (media_cancel(&menu)) abort(); /* Still held, already consumed. */
        preview_buttons=0; media_cancel(&menu);
        if (menu.suppress_buttons) abort();
        menu.cancel_directions=0; preview_buttons=ControlDown;
        if (!media_cancel(&menu) || menu.suppress_buttons || !(menu.cancel_directions&ControlDown)) abort();
        menu.cancel_directions=0; preview_buttons=0;
        /* A complete L+R press during a media cancellation poll survives
         * release before the next main-loop iteration. It fires only once. */
        menu.tools=0; menu.tools_requested=menu.tools_chord_down=0;
        menu.media_cancelled=0;preview_buttons=ControlLeftShift;
        if (media_cancel(&menu)) abort();
        preview_buttons=ControlLeftShift|ControlRightShift;
        if (!media_cancel(&menu)) abort();
        preview_buttons=0; media_cancel(&menu);
        if (!dispatch_tools_request(&menu) || menu.tools!=TOOL_HOME) abort();
        if (dispatch_tools_request(&menu)) abort();
        observe_tools_chord(&menu,ControlLeftShift);
        observe_tools_chord(&menu,ControlLeftShift|ControlRightShift);
        if (!dispatch_tools_request(&menu) || menu.tools) abort();
        observe_tools_chord(&menu,ControlRightShift);
        observe_tools_chord(&menu,ControlLeftShift|ControlRightShift);
        if (dispatch_tools_request(&menu)) abort();
        observe_tools_chord(&menu,0);
        observe_tools_chord(&menu,ControlLeftShift|ControlRightShift);
        if (!dispatch_tools_request(&menu) || menu.tools!=TOOL_HOME) abort();
        observe_tools_chord(&menu,0); menu.tools=0;menu.media_cancelled=0;
        /* L/R is emitted only on a solo release, never on the first half
         * of a staggered chord or after a different action consumes the hold. */
        {
            u32 l=ControlLeftShift,r=ControlRightShift;
            size_t selection=menu.selected;
            menu.shoulder_pending=0;
            observe_tools_chord(&menu,l);
            if (shoulder_input(&menu,l,l,0)) abort();
            preview_now+=1000;
            observe_tools_chord(&menu,l|r);
            if (shoulder_input(&menu,l|r,r,0)) abort();
            if (!dispatch_tools_request(&menu) || menu.tools!=TOOL_HOME || menu.selected!=selection) abort();
            observe_tools_chord(&menu,l);
            if (shoulder_input(&menu,l,0,r)) abort();
            observe_tools_chord(&menu,0);
            if (shoulder_input(&menu,0,0,l)) abort();
            menu.tools=0;
            observe_tools_chord(&menu,r);
            if (shoulder_input(&menu,r,r,0)) abort();
            observe_tools_chord(&menu,l|r);
            if (shoulder_input(&menu,l|r,l,0)) abort();
            if (!dispatch_tools_request(&menu)) abort();
            observe_tools_chord(&menu,0);
            if (shoulder_input(&menu,0,0,l|r)) abort();
            menu.tools=0;
            if (shoulder_input(&menu,l,l,0) || shoulder_input(&menu,l,0,0)) abort();
            if (shoulder_input(&menu,0,0,l)!=l || shoulder_input(&menu,0,0,0)) abort();
            if (shoulder_input(&menu,r,r,0) || shoulder_input(&menu,r,0,0)) abort();
            if (shoulder_input(&menu,0,0,r)!=r) abort();
            shoulder_input(&menu,l,l,0);
            if (shoulder_input(&menu,l|ControlA,ControlA,0)!=ControlA) abort();
            if (shoulder_input(&menu,0,0,l|ControlA)) abort();
            menu.tools_requested=1;shoulder_input(&menu,l,l,0);
            if (!dispatch_tools_request(&menu) || menu.shoulder_pending) abort();
            observe_tools_chord(&menu,0);menu.tools=0;
        }
        /* Root index reads are cached even when empty/missing. Virtual roots
         * retain the cache; changing volume or explicit refresh reloads it. */
        show_demo(&menu);menu.demo=0;menu.config_result=0;
        strcpy(menu.ode.identity,"LOCODE");menu.config.usb_covers=menu.config.video_previews=1;
        menu.depth=1;menu.path_ids[0]=0x01000000UL;
        preview_index_reads=0;
        refresh_usb_art(&menu);refresh_usb_art(&menu);
        menu.depth=2;refresh_usb_art(&menu);menu.depth=1;refresh_usb_art(&menu);
        if (preview_index_reads!=3) abort();
        {
            const MenuEntry volume={0x01000000UL,1,"USB"};
            const MenuEntry *volumes[1];
            const MenuEntry **saved=menu.listing.entries;
            size_t count=menu.listing.count;
            volumes[0]=&volume;menu.listing.entries=volumes;menu.listing.count=1;menu.depth=0;
            refresh_usb_art(&menu);
            menu.listing.entries=saved;menu.listing.count=count;menu.depth=1;
            refresh_usb_art(&menu);
            if (preview_index_reads!=3) abort();
        }
        menu.path_ids[0]=0x02000000UL;refresh_usb_art(&menu);
        if (preview_index_reads!=6) abort();
        menu.art_indexes_valid=0;refresh_usb_art(&menu);
        if (preview_index_reads!=9) abort();
        show_demo(&menu);
        /* Details must leave the browser selection and storage untouched. */
        {
            size_t before=menu.selected, last;
            int reads=preview_reads, writes=preview_writes, nvram=preview_nvram_reads;
            menu.tools=0;menu.demo=0;menu.ready=1;
            browser_shortcuts(&menu,ControlC,ControlC,0);
            if(browser_shortcuts(&menu,ControlC|ControlStart,ControlStart,0) || menu.c_pending) abort();
            menu.demo=1;
            if (menu.tools!=TOOL_GAME_DETAILS || menu.game_details_page) abort();
            draw(&menu);
            if (!menu.game_details_pages) abort();
            /* A full-length name must survive wrapping across several pages. */
            {
                char long_name[1024]; size_t rows=0;
                memset(long_name,'W',sizeof(long_name)-1); long_name[sizeof(long_name)-1]=0;
                game_details_text(&menu,long_name,&rows,TEXT);
                if (rows<=10) abort();
                menu.game_details_pages=(rows+9)/10;
            }
            last=menu.game_details_pages-1;
            while (menu.game_details_page<last) tools_input(&menu,ControlRight);
            tools_input(&menu,ControlRight);
            if (menu.game_details_page!=last) abort();
            while (menu.game_details_page) tools_input(&menu,ControlLeft);
            tools_input(&menu,ControlLeft);
            if (menu.game_details_page || menu.selected!=before) abort();
            tools_input(&menu,ControlB);
            if (menu.tools || menu.selected!=before || reads!=preview_reads ||
                writes!=preview_writes || nvram!=preview_nvram_reads) abort();
        }
        /* Tools wraps across seven entries; viewing Help/Diagnostics is read-only. */
        menu.tools=TOOL_HOME; menu.tool_selected=0;
        tools_input(&menu, ControlUp);
        if (menu.tool_selected != 3) abort();
        tools_input(&menu, ControlDown);
        if (menu.tool_selected != 0) abort();
        tools_input(&menu, ControlRight); if (menu.tool_selected != 4) abort();
        tools_input(&menu, ControlLeft); if (menu.tool_selected != 0) abort();
        menu.tool_selected=3; tools_input(&menu, ControlA);
        if (menu.tools != TOOL_HELP || menu.usb_tested) abort();
        {
            Menu slots;char path[40];memset(&slots,0,sizeof(slots));
            if (slot_count(&slots)!=10) abort();
            slot_select(&slots,8);if (slots.slot_page!=1 || slots.tool_selected || slot_count(&slots)!=8) abort();
            slot_path(path,slot_at(&slots,0));if (strcmp(path,"saves/slot09.3ds")) abort();
            slot_select(&slots,999999998);if (slot_count(&slots)!=7 || slots.tool_selected!=6) abort();
            slot_path(path,slot_at(&slots,6));if (strcmp(path,"saves/slot999999999.3ds")) abort();
            slot_select(&slots,SAVE_RECOVERY_SLOT);if (slots.slot_page || slots.tool_selected!=8) abort();
            slot_path(path,slot_at(&slots,8));if (strcmp(path,"saves/recovery.3ds")) abort();
            slots.demo=slots.ready=1;slots.tools=TOOL_SLOTS;
            tools_input(&slots,ControlStart);
            if(slots.tools!=TOOL_SLOT_FIND) abort();
            tools_input(&slots,ControlA);
            if(slots.tools!=TOOL_SLOT_FIND || !slots.tool_message || strcmp(slots.tool_message,"USB save access unavailable.")) abort();
            slots.demo=slots.ready=1;slots.tools=TOOL_ARCHIVE;slots.tool_selected=1;slots.backup_slot=12;
            preview_nvram_fixture(&slots.backup);tools_input(&slots,ControlStart);
            if (slots.tools!=TOOL_SLOTS || !slots.backup_merge || slots.pending.count!=1 || slots.copy_original.count!=3 || slots.backup.bytes) abort();
            if (strcmp(slots.pending.entries[0].name,"NFS_Config")) abort();
            tools_input(&slots,ControlB);
            if (slots.tools!=TOOL_ARCHIVE || slots.backup.count!=3 || slots.backup_slot!=12 || slots.tool_selected!=1 || slots.copy_original.bytes) abort();
            save_archive_free(&slots.backup);
        }
        /* Actual tools/controller code: entering/cancelling never writes. */
        menu.tools=TOOL_HOME; menu.tool_selected=0;
        tools_input(&menu, ControlA);
        if (menu.tools != TOOL_SAVES || preview_nvram_reads != 1 || preview_nvram_writes) abort();
        tools_input(&menu, ControlA);
        if (menu.tools != TOOL_SLOTS || menu.pending.count != 1 || preview_nvram_writes) abort();
        tools_input(&menu, ControlA);
        if (menu.tools != TOOL_SLOTS || !menu.backup_merge || preview_nvram_writes) abort();
        /* Demo I/O cannot read a merge destination. Simulate its reviewed page. */
        menu.tools=TOOL_BACKUP_DETAILS;
        tools_input(&menu, ControlA);
        if (menu.tools != TOOL_CONFIRM_BACKUP || preview_nvram_writes) abort();
        tools_input(&menu, ControlB);
        if (menu.tools != TOOL_SLOTS || preview_nvram_writes) abort();
        tools_input(&menu, ControlB);
        if (menu.tools != TOOL_SAVES || menu.backup_mode || menu.pending.count) abort();
        tools_input(&menu, ControlRight);
        if (menu.tools != TOOL_SLOTS || menu.backup_mode || menu.tool_selected) abort();
        tools_input(&menu, ControlB);
        if (menu.tools != TOOL_HOME) abort();
        tools_input(&menu, ControlA);
        if (menu.tools != TOOL_SAVES) abort();
        tools_input(&menu, ControlLeft);
        if (menu.tools != TOOL_SLOTS || menu.backup_mode) abort();
        tools_input(&menu, ControlRight);
        if (menu.tools != TOOL_SAVES || menu.tool_selected || preview_nvram_writes) abort();
        tools_input(&menu, ControlLeft);
        preview_nvram_fixture(&menu.backup);
        menu.tools=TOOL_ARCHIVE; menu.tool_selected=0;
        tools_input(&menu, ControlA);
        if (menu.tools != TOOL_CONFIRM_RESTORE || preview_nvram_writes) abort();
        tools_input(&menu, ControlB);
        if (menu.tools != TOOL_ARCHIVE || preview_nvram_writes) abort();
        tools_input(&menu, ControlA); tools_input(&menu, ControlA);
        if (preview_nvram_writes || menu.restore_result != -1) abort(); /* demo USB blocked */
        /* Hex edits are private; cancelling a byte or discarding preserves source.
         * Save as rebuilds CRCs and cannot write USB/NVRAM before confirmation. */
        preview_nvram_fixture(&menu.backup); menu.tools=TOOL_ARCHIVE; menu.tool_selected=0;
        {
            unsigned char original=menu.backup.entries[0].data[0];
            SaveArchive parsed;
            memset(&parsed,0,sizeof(parsed));
            tools_input(&menu,ControlC);
            if (menu.tools!=TOOL_HEX || menu.hex_dirty || menu.hex_archive.count!=3) abort();
            tools_input(&menu,ControlA); tools_input(&menu,ControlUp); tools_input(&menu,ControlB);
            if (menu.hex_dirty || menu.hex_archive.entries[0].data[0]!=original) abort();
            tools_input(&menu,ControlA); tools_input(&menu,ControlRight); tools_input(&menu,ControlUp); tools_input(&menu,ControlA);
            if (!menu.hex_dirty || menu.hex_archive.entries[0].data[0]==original || menu.hex_original.entries[0].data[0]!=original) abort();
            tools_input(&menu,ControlC);
            if (menu.tools!=TOOL_SLOTS || !menu.backup_mode || preview_nvram_writes) abort();
            if (save_archive_parse(&parsed,menu.pending.bytes,menu.pending.length)<0 || parsed.count!=1 || parsed.entries[0].data[0]==original) abort();
            save_archive_free(&parsed);
            /* Reviewing a destination replaces the working USB listing, not
             * the editor's preserved source archive. */
            preview_nvram_fixture(&menu.backup); menu.tools=TOOL_BACKUP_DETAILS; menu.backup_slot=6;
            tools_input(&menu,ControlB);
            if (menu.tools!=TOOL_SLOTS) abort();
            tools_input(&menu,ControlB);
            if (menu.tools!=TOOL_HEX || !menu.hex_dirty) abort();
            toggle_tools(&menu);
            if (menu.tools!=TOOL_HEX_DISCARD) abort();
            tools_input(&menu,ControlB); tools_input(&menu,ControlB);
            if (menu.tools!=TOOL_HEX_DISCARD) abort();
            tools_input(&menu,ControlA);
            if (menu.tools!=TOOL_ARCHIVE || menu.backup.entries[0].data[0]!=original || menu.hex_archive.bytes || preview_nvram_writes) abort();
        }
        {
            SaveEntry entry;
            unsigned char data[65];
            memset(&entry,0,sizeof(entry)); memset(data,255,sizeof(data));
            strcpy(entry.name,"Bounds"); entry.size=sizeof(data); entry.data=data;
            save_archive_free(&menu.backup);
            if (save_archive_build(&menu.backup,&entry,1)<0) abort();
            menu.save_selected=0; open_hex(&menu);
            tools_input(&menu,ControlRightShift); if (menu.hex_offset!=32) abort();
            tools_input(&menu,ControlRightShift); if (menu.hex_offset!=64) abort();
            tools_input(&menu,ControlRightShift); tools_input(&menu,ControlRight); tools_input(&menu,ControlDown);
            if (menu.hex_offset!=64) abort();
            tools_input(&menu,ControlA); tools_input(&menu,ControlUp); tools_input(&menu,ControlA);
            if (menu.hex_archive.entries[0].data[64]!=15) abort();
            tools_input(&menu,ControlLeftShift); tools_input(&menu,ControlLeftShift); tools_input(&menu,ControlLeftShift);
            if (menu.hex_offset) abort();
            close_hex(&menu);
            save_archive_free(&menu.backup); entry.size=0;
            if (save_archive_build(&menu.backup,&entry,1)<0) abort();
            open_hex(&menu); tools_input(&menu,ControlA); tools_input(&menu,ControlRightShift); draw(&menu);
            if (menu.hex_offset || menu.hex_editing || menu.hex_dirty) abort();
            tools_input(&menu,ControlB);
            if (menu.tools!=TOOL_ARCHIVE || preview_nvram_writes) abort();
        }
        {
            SaveEntry entry;
            unsigned char data[40];
            unsigned long words[10]={0x4c57,2,1,15,15,3,1,2,0,12345};
            size_t i;
            SaveArchive parsed;
            memset(&parsed,0,sizeof(parsed)); memset(&entry,0,sizeof(entry));
            for (i=0;i<9;++i) words[9]+=words[i];
            for (i=0;i<10;++i) save_put_word(data+i*4,words[i]);
            strcpy(entry.name,"DoomPrefs"); entry.data=data; entry.size=40;
            save_archive_free(&menu.backup);
            if (save_archive_build(&menu.backup,&entry,1)<0) abort();
            menu.tools=TOOL_ARCHIVE; menu.tool_selected=0; tools_input(&menu,ControlX);
            if (menu.tools!=TOOL_CHEATS || menu.hex_dirty) abort();
            draw(&menu); tools_input(&menu,ControlA);
            if (!menu.hex_dirty || save_word(menu.hex_archive.entries[0].data+24)!=23 ||
                save_word(menu.hex_original.entries[0].data+24)!=1 || !save_cheats_find(&menu.hex_archive.entries[0])) abort();
            tools_input(&menu,ControlC);
            if (menu.tools!=TOOL_SLOTS || preview_nvram_writes || save_archive_parse(&parsed,menu.pending.bytes,menu.pending.length)<0 ||
                !save_cheats_find(&parsed.entries[0])) abort();
            save_archive_free(&parsed); tools_input(&menu,ControlB);
            if (menu.tools!=TOOL_CHEATS) abort();
            tools_input(&menu,ControlDown); tools_input(&menu,ControlA);
            if (save_word(menu.hex_archive.entries[0].data+4)!=2) abort();
            tools_input(&menu,ControlRight);
            if (save_word(menu.hex_archive.entries[0].data+4)!=3 || !save_cheats_find(&menu.hex_archive.entries[0])) abort();
            tools_input(&menu,ControlB); if (menu.tools!=TOOL_HEX_DISCARD) abort();
            tools_input(&menu,ControlB); if (menu.tools!=TOOL_CHEATS) abort();
            tools_input(&menu,ControlX); if (menu.tools!=TOOL_HEX) abort();
            tools_input(&menu,ControlX); if (menu.tools!=TOOL_CHEATS) abort();
            tools_input(&menu,ControlB); tools_input(&menu,ControlA);
            if (menu.tools!=TOOL_ARCHIVE || save_word(menu.backup.entries[0].data+24)!=1 || preview_nvram_writes) abort();
            menu.tools=TOOL_ARCHIVE; menu.tool_selected=0; tools_input(&menu,ControlC);
            ((unsigned char *)menu.hex_archive.entries[0].data)[0]^=1;
            tools_input(&menu,ControlX);
            if (menu.tools!=TOOL_HEX) abort();
            close_hex(&menu);
        }
        menu.ode.navigation_failed=1; menu.tools=TOOL_HOME; menu.tool_selected=4;
        tools_input(&menu, ControlA); draw(&menu);
        if (menu.tools != TOOL_DIAG || preview_nvram_writes) abort();
        tools_input(&menu, ControlRight); draw(&menu); tools_input(&menu, ControlRight); draw(&menu);
        tools_input(&menu, ControlRight); draw(&menu); if (menu.tool_selected!=3) abort();
        tools_input(&menu, ControlRight); draw(&menu); if (menu.tool_selected!=4) abort();
        tools_input(&menu, ControlRight); if (menu.tool_selected) abort();
        tools_input(&menu, ControlLeft); if (menu.tool_selected!=4) abort();
        menu.ode.navigation_failed=0;
        /* Filtering leaves source entries and directory IDs intact. */
        {
            const MenuEntry hidden={123,0,"boot.iso"};
            const MenuEntry game={456,0,"Game.iso"};
            const MenuEntry cue={457,0,"Game.cue"};
            const MenuEntry bin={458,0,"Game.BIN"};
            const MenuEntry volume={0x01000000UL,1,"covers"};
            const MenuEntry *entries[4]={&hidden,&game,&cue,&bin};
            OdeListing original=menu.listing;
            menu.listing.entries=entries; menu.listing.count=4; menu.listing.has_parent=0;
            menu.depth=0;
            if (row_count(&menu) != 2 || browser_entry(&menu,0) != &game || browser_entry(&menu,1) != &cue ||
                menu.listing.count != 4 || browser_entry(&menu,0)->toc_id != 456 || browser_entry(&menu,1)->toc_id != 457) abort();
            menu.depth=1; menu.path_ids[0]=9;
            if (row_count(&menu) != 3 || browser_entry(&menu,0) != &hidden) abort();
            menu.path_ids[0]=0x01000000UL;
            if (row_count(&menu) != 2) abort();
            if (hidden_entry(&menu, &volume)) abort();
            menu.listing=original;
        }
        /* All INI controls; navigation stays active until reboot, bypass never writes. */
        menu.tools=TOOL_CONFIG; menu.tool_selected=0; menu.config.screen_margin=0;
        tools_input(&menu,ControlLeft); if (menu.config.screen_margin != 12) abort();
        menu.tool_selected=1; tools_input(&menu,ControlA); if (!menu.config.large_titles) abort();
        menu.settings_folder_workaround=menu.config.folder_workaround=1;
        menu.settings_usb_covers=menu.config.usb_covers=0;
        menu.tool_selected=2; tools_input(&menu,ControlA);
        menu.tool_selected=3; tools_input(&menu,ControlRight);
        /* The workaround setting cycles automatic, always, off: from always it goes off. */
        if (menu.settings_folder_workaround != MENU_WORKAROUND_OFF || !menu.settings_usb_covers ||
            !menu.config.folder_workaround || menu.config.usb_covers) abort();
        menu.config_result=2; tools_input(&menu,ControlC);
        if (strcmp(menu.tool_message,"Load a valid INI before saving.")) abort();
        menu.tool_selected=9; tools_input(&menu,ControlA);
        if (menu.config.screen_margin || menu.config.large_titles) abort();
        if (menu.settings_folder_workaround != MENU_WORKAROUND_OFF || !menu.settings_usb_covers) abort();
        tools_input(&menu,ControlB);
        menu.tool_selected=2; tools_input(&menu,ControlA);
        if (menu.tools != TOOL_CONFIG || menu.settings_folder_workaround != MENU_WORKAROUND_OFF || !menu.settings_usb_covers) abort();
        tools_input(&menu,ControlUp); if (menu.tool_selected != 11) abort();
        tools_input(&menu,ControlA);
        if (strcmp(menu.tool_message,"Load a valid INI before saving.")) abort();
        menu.config_result=0; menu.ready=1; menu.demo=1;
        menu.saved_config=menu.config;
        menu.tool_selected=7; tools_input(&menu,ControlA);
        if (menu.config.preview_sound || !config_dirty(&menu)) abort();
        tools_input(&menu,ControlX);
        if (!menu.config.preview_sound || config_dirty(&menu)) abort();
        menu.demo=1;
        /* Unsaved marker tracks all settings; discard restores the saved snapshot. */
        menu.saved_config=menu.config;
        menu.settings_folder_workaround=menu.config.folder_workaround;
        menu.settings_usb_covers=menu.config.usb_covers;
        menu.settings_remember_position=menu.config.remember_position;
        menu.settings_recent_games=menu.config.recent_games;
        if (config_dirty(&menu)) abort();
        menu.tool_selected=4; tools_input(&menu,ControlA);
        if (!config_dirty(&menu)) abort();
        tools_input(&menu,ControlX); if (config_dirty(&menu)) abort();
        menu.tools=TOOL_FIND; menu.find_query[0]=0; menu.find_key=0;
        tools_input(&menu,ControlA); tools_input(&menu,ControlRight); tools_input(&menu,ControlA);
        if (strcmp(menu.find_query,"AB")) abort();
        tools_input(&menu,ControlX); if (strcmp(menu.find_query,"A")) abort();
        tools_input(&menu,ControlC); if (*menu.find_query) abort();
        tools_input(&menu,ControlLeft); if (menu.find_key != 0) abort();
        tools_input(&menu,ControlA); tools_input(&menu,ControlStart);
        if (menu.tools!=TOOL_FIND || !menu.tool_message) abort();
        menu.demo=0; menu.ready=1; menu.depth=0; menu.catalog.valid=1;
        menu.catalog.paths=(char **)calloc(2,sizeof(char *));
        menu.catalog.paths[0]=strcpy((char *)malloc(40),"/Storage/Games/Alpha.iso");
        menu.catalog.paths[1]=strcpy((char *)malloc(40),"/Storage/Other/Beta.iso"); menu.catalog.count=2;
        tools_input(&menu,ControlStart);
        if (menu.tools!=TOOL_RESULTS || library_catalog_count(&menu.catalog,menu.find_query)!=2) abort();
        tools_input(&menu,ControlB); if (menu.tools!=TOOL_FIND) abort();
        library_catalog_free(&menu.catalog); menu.demo=1; menu.tools=0;
        toggle_tools(&menu); if (menu.tools!=TOOL_HOME) abort();
        menu.tools=TOOL_FIND; toggle_tools(&menu); if (menu.tools || menu.review) abort();
        menu.tools=TOOL_CONFIG; toggle_tools(&menu); if (menu.tools) abort();
        preview_buttons=ControlB;
        if (!search_progress(&menu,0) || menu.suppress_buttons!=ControlB) abort();
        preview_buttons=0; menu.suppress_buttons=0;
        menu.filter[0]=0;
        /* Move cancellation is local and never writes to the demo USB bus. */
        menu_favorites_parse(&menu.favorites,"First.iso\nSecond.iso\n",21);
        menu.favorites_valid=1; menu.tools=TOOL_FAVORITES; menu.tool_selected=0;
        tools_input(&menu,ControlRightShift); tools_input(&menu,ControlDown);
        if (!menu.favorites_move || strcmp(menu.favorites.paths[0],"Second.iso")) abort();
        tools_input(&menu,ControlB);
        if (menu.favorites_move || strcmp(menu.favorites.paths[0],"First.iso")) abort();
        tools_input(&menu,ControlRightShift); tools_input(&menu,ControlDown);
        toggle_tools(&menu);
        if (menu.tools || menu.favorites_move || strcmp(menu.favorites.paths[0],"First.iso")) abort();
        menu_favorites_free(&menu.favorites);
        /* Actual persistence controllers, with only the USB file transport replaced. */
        preview_text_enabled=1; preview_reads=preview_writes=0;
        menu.demo=menu.tools=menu.review=0; menu.ready=1; menu.depth=0; menu.selected=2;
        menu.config_result=0; menu.config.remember_position=menu.config.recent_games=1;
        strcpy(menu.ode.identity,"LOCODE"); menu.ode.context=&menu.bus;
        menu.bus.timed_out=menu.ode.navigation_failed=0;
        menu.position_result=0; menu.position_dirty=1; memset(&menu.last_position,0,sizeof(menu.last_position));
        menu.selection_since=0; preview_now=9000;
        remember_position(&menu,0);
        if (preview_writes != 1 || menu.position_result != 1 || !strstr(preview_position,"/Storage\n")) abort();
        remember_position(&menu,0); if (preview_writes != 1) abort();
        menu.selected=3; selection_changed(&menu); remember_position(&menu,0);
        if (preview_writes != 1) abort();
        preview_now+=2000; remember_position(&menu,0); if (preview_writes != 2) abort();
        menu.config_result=2; menu.selected=4; remember_position(&menu,1);
        if (preview_writes != 2) abort();
        menu.config_result=0; menu.config.remember_position=0;
        record_launch(&menu,"/Storage/Games/D (Disc 1).iso");
        if (menu.recent_result != 1 || strcmp(preview_recent,"Games/D (Disc 1).iso\n")) abort();
        record_launch(&menu,"/Storage/Other.iso");
        if (strcmp(preview_recent,"Other.iso\nGames/D (Disc 1).iso\n")) abort();
        {
            int reads=preview_reads, writes=preview_writes;
            record_launch(&menu,"/Storage/Other.iso");
            if (preview_reads!=reads || preview_writes!=writes) abort();
        }
        {
            int writes=preview_writes;
            menu.recent_valid=0; strcpy(preview_recent,"../bad.iso\n"); record_launch(&menu,"/Storage/New.iso");
            if (preview_writes != writes || menu.recent_result != -1) abort();
            preview_read_fail=1; record_launch(&menu,"/Storage/New.iso");
            if (preview_writes != writes) abort();
            preview_read_fail=0; preview_recent[0]=0; preview_write_fail=1;
            record_launch(&menu,"/Storage/New.iso"); if (menu.recent_result != -1) abort();
            preview_write_fail=0;
        }
        menu.bus.last_command=0x93; menu.bus.failed_command=0xe3; menu.bus.failed_status=0x10;
        export_diagnostics(&menu);
        if (menu.diagnostics_result != 1 || !strstr(preview_diagnostics,"last_command=93\n") ||
            !strstr(preview_diagnostics,"last_fault_command=e3\n") || menu.bus.last_command != 0xe4) abort();
        {
            int reads=preview_reads, writes=preview_writes;
            menu.bus.timed_out=1; export_diagnostics(&menu);
            if (menu.diagnostics_result != -1 || preview_reads != reads || preview_writes != writes) abort();
            menu.bus.timed_out=0; menu.config_result=2; export_diagnostics(&menu);
            if (preview_reads != reads || preview_writes != writes) abort();
            menu.config_result=0;
        }
        strcpy(preview_favorites,"First.iso\nSecond.iso\n");
        menu_favorites_parse(&menu.favorites,preview_favorites,strlen(preview_favorites));
        menu.favorites_valid=1; menu.tools=TOOL_FAVORITES; menu.tool_selected=0;
        begin_favorites_move(&menu); menu_favorites_move(&menu.favorites,0,1); save_favorites_order(&menu);
        if (menu.favorites_move || strcmp(preview_favorites,"Second.iso\nFirst.iso\n")) abort();
        begin_favorites_move(&menu); menu_favorites_move(&menu.favorites,0,1);
        strcpy(preview_favorites,"PC changed.iso\n");
        {
            int writes=preview_writes; save_favorites_order(&menu);
            if (!menu.favorites_move || preview_writes != writes || !strstr(menu.tool_message,"changed")) abort();
        }
        cancel_favorites_move(&menu); menu_favorites_free(&menu.favorites); menu_favorites_free(&menu.recent);
        menu.config.remember_position=1; menu.position_result=0; menu.selected=5;
        preview_write_fail=1; remember_position(&menu,1);
        if (menu.position_result != -1) abort();
        {
            int writes=preview_writes; remember_position(&menu,1);
            if (preview_writes != writes) abort();
        }
        preview_write_fail=0;
        preview_text_enabled=0; menu.config_result=2; menu.demo=1; menu.tools=0;
        /* Actual alphabet focus/input: cancel is local, confirm only selects. */
        menu.tools=menu.review=0; menu.ready=1; menu.selected=0;
        menu.c_pending=1;
        if (!alphabet_input(&menu,ControlUp) || !menu.alphabet_active || menu.c_pending) abort();
        alphabet_input(&menu,ControlLeft); alphabet_input(&menu,ControlRight);
        menu.jump_letter=0; alphabet_input(&menu,ControlLeft);
        if (menu.jump_letter != 26) abort();
        alphabet_input(&menu,ControlRight);
        if (menu.jump_letter) abort();
        {
            size_t selected=menu.selected;
            unsigned long commands=menu.bus.commands;
            unsigned char available[27];
            alphabet_available(&menu,available);
            if (!available[4] || available[26]) abort();
            menu.jump_letter=26; alphabet_input(&menu,ControlA);
            if (!menu.alphabet_active || menu.selected != selected) abort();
            alphabet_input(&menu,ControlC|ControlStart|ControlX);
            if (!menu.alphabet_active || menu.c_pending || menu.bus.commands != commands) abort();
            alphabet_input(&menu,ControlB);
            if (menu.alphabet_active || menu.selected != selected) abort();
            alphabet_input(&menu,ControlUp); menu.jump_letter=4;
            alphabet_input(&menu,ControlA);
            if (menu.alphabet_active || menu_initial(browser_entry(&menu,menu.selected)->name) != 4 || menu.bus.commands != commands) abort();
        }
        menu.selected=row_count(&menu)-1;
        if (!alphabet_input(&menu,ControlDown) || !menu.alphabet_active) abort();
        alphabet_input(&menu,ControlDown);
        if (menu.alphabet_active || menu.selected) abort();
        strcpy(menu.filter,"Road");
        {
            unsigned char available[27]; alphabet_available(&menu,available);
            if (!available[18] || available[4]) abort();
        }
        menu.filter[0]=0; menu.review=1; menu.alphabet_active=1;
        if (alphabet_input(&menu,ControlUp) || menu.alphabet_active) abort();
        menu.review=0; menu.ready=0;
        if (alphabet_input(&menu,ControlUp) || menu.alphabet_active) abort();
        menu.ready=1;
        menu.demo=0; menu.selected=2; menu.c_pending=1;
        if (browser_shortcuts(&menu,ControlC|ControlUp,ControlUp,0) || !menu.alphabet_active || menu.c_pending) abort();
        if (browser_shortcuts(&menu,0,0,ControlC) || menu.c_pending) abort();
        alphabet_input(&menu,ControlB);
        /* Holding C for a letter jump never queues an image. */
        menu.tools=0; menu.review=menu.demo=0; menu.ready=1; menu.playlist.synchronized=1;
        if (browser_shortcuts(&menu,ControlC,ControlC,0) & ControlC) abort();
        browser_shortcuts(&menu,ControlC|ControlRight,ControlRight,0);
        browser_shortcuts(&menu,0,0,ControlC);
        if (menu.playlist.count || menu.c_pending) abort();
        menu.demo=1;
        /* Only visible favorites participate in selection and reorder; the
         * complete file, including hidden slots, survives moving/cancelling. */
        {
            static const char favorites[]="Adult/Japan/Hidden.iso\nGames/A.iso\nAdult/USA/Hidden2.iso\nGames/B.iso\n";
            char out[FAVORITES_BYTES];
            menu.tools=TOOL_FAVORITES; menu.config.hide_adult=1; menu.favorites_valid=1;
            menu.tool_selected=0;
            if (menu_favorites_parse(&menu.favorites,favorites,strlen(favorites))) abort();
            tools_input(&menu,ControlRightShift);
            if (!menu.favorites_move) abort();
            tools_input(&menu,ControlDown);
            if (menu.tool_selected!=1 || strcmp(menu.favorites.paths[0],"Adult/Japan/Hidden.iso") ||
                strcmp(menu.favorites.paths[1],"Games/B.iso") || strcmp(menu.favorites.paths[2],"Adult/USA/Hidden2.iso") ||
                strcmp(menu.favorites.paths[3],"Games/A.iso")) abort();
            if (menu_favorites_format(&menu.favorites,out,sizeof(out)) || !strstr(out,"Hidden2.iso")) abort();
            tools_input(&menu,ControlB);
            if (menu.favorites_move || menu_favorites_format(&menu.favorites,out,sizeof(out)) || strcmp(out,favorites)) abort();
            menu_favorites_free(&menu.favorites); menu.config.hide_adult=0; menu.tools=0; menu.tool_selected=0;
        }
        /* A folder holding one game (a disc, or a complete disc set) acts as
         * that game; nested folders or unrelated discs keep it a folder. */
        {
            static LibraryGameInfo games[]={{"A/A (USA).iso","Racing","Pub","1994"},{"B/B (Disc 1).iso","","",""},
                {"B/B (Disc 2).iso","","",""},{"C/x/C.iso","","",""},{"D/D.iso","","",""},{"D/E.iso","","",""}};
            const MenuEntry a={10,1,"A"}, b={11,1,"b"}, c={12,1,"C"}, d={13,1,"D"}, disc={14,0,"A (USA).iso"};
            const char *discs[ODE_MAX_DISCS];
            LibraryMetadata saved=menu.metadata;
            unsigned int depth=menu.depth;
            menu.metadata.entries=games; menu.metadata.count=6; menu.depth=0;
            if (folder_game(&menu,&a,discs)!=1 || strcmp(discs[0],"A (USA).iso")) abort();
            if (folder_game(&menu,&b,discs)!=2 || strcmp(discs[0],"B (Disc 1).iso") || strcmp(discs[1],"B (Disc 2).iso")) abort();
            if (folder_game(&menu,&c,discs) || folder_game(&menu,&d,discs) || folder_game(&menu,&disc,discs)) abort();
            if (!game_entry(&menu,&a) || game_entry(&menu,&d) || !game_entry(&menu,&disc)) abort();
            if (game_info(&menu,&a)!=games || game_info(&menu,&b)!=games+1 || game_info(&menu,&d)) abort();
            menu.metadata=saved; menu.depth=depth;
        }
        /* A disc of a set queues itself and the discs after it; the last
         * disc alone, or a held C (single_disc), launches only that disc. */
        {
            const MenuEntry first={1,0,"D (Disc 1).iso"}, second={2,0,"D (Disc 2).iso"}, third={3,0,"D (Disc 3).iso"};
            const MenuEntry *entries[3]={&second,&third,&first};
            OdeListing original=menu.listing;
            menu.listing.entries=entries; menu.listing.count=3; menu.listing.has_parent=0;
            menu.depth=0; menu.playlist.synchronized=1;
            if (!queue_set(&menu,&first) || menu.playlist.count != 3 || !menu.playlist.dirty ||
                strcmp(menu.playlist.paths[0],"/Storage/D (Disc 1).iso") || strcmp(menu.playlist.paths[2],"/Storage/D (Disc 3).iso")) abort();
            ode_playlist_dispose(&menu.playlist); menu.playlist.synchronized=1;
            if (!queue_set(&menu,&second) || menu.playlist.count != 2 ||
                strcmp(menu.playlist.paths[0],"/Storage/D (Disc 2).iso")) abort();
            ode_playlist_dispose(&menu.playlist); menu.playlist.synchronized=1;
            if (queue_set(&menu,&third) || menu.playlist.count) abort();
            menu.selected=0; menu.ready=1; jump_letter(&menu,4);
            if (menu.selected != 0) abort();
            menu.listing=original;
        }
        {
            Menu launching;
            memset(&launching,0,sizeof(launching));
            launching.ready=1; launching.preview_audio.status=1; launching.playlist.synchronized=1;
            launching.ode.transfer=preview_mount; launching.ode.context=&launching;
            start_queue(&launching);
            if (launching.preview_audio.status!=1 || preview_mounts || launching.launching) abort();
            if (ode_playlist_append_local(&launching.ode,&launching.playlist,"/Storage/Game.iso")) abort();
            launching.playlist.dirty=0;
            start_queue(&launching);
            if (launching.preview_audio.status || preview_mounts!=1 || !launching.launching) abort();
            {
                int displayed=display_count;
                /* The mounted image owns the handoff. Its released browser
                 * listing must never replace the last launch frame. */
                draw(&launching);
                if (display_count!=displayed) abort();
            }
            ode_playlist_dispose(&launching.playlist);
        }
        save_archive_free(&menu.nvram); save_archive_free(&menu.pending); save_archive_free(&menu.backup);
        ode_listing_free(&menu.listing);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "cover")) {
        menu.covers_enabled=1;
        menu.selected=2;
        menu.cover=&cover;
    }
    if (argc > 1 && !strcmp(argv[1], "loading")) {
        draw(&menu);
        menu.loading=1;
        menu.ready=0;
        menu.read_count=37;
    }
    if (argc > 1 && !strcmp(argv[1], "nested")) {
        menu.depth=3;
        strcpy(menu.path[1], "Adventure");
        strcpy(menu.path[2], "Multi Disc");
        update_breadcrumb(&menu);
    }
    if (argc > 1 && !strcmp(argv[1], "queue")) {
        menu.review=1;
        menu.demo=0;
        menu.playlist.count=2;
        menu.playlist.paths[0]="/Storage/Wing Commander III/Disc 1.iso";
        menu.playlist.paths[1]="/Storage/Wing Commander III/Disc 2.iso";
        menu.config.folder_workaround=1;
    }
    if (argc > 1 && !strcmp(argv[1], "tools")) menu.tools=TOOL_HOME;
    if (argc > 1 && !strcmp(argv[1], "context-help")) menu.help_hold.active=1;
    if (argc > 1 && !strcmp(argv[1], "context-hex")) {menu.help_hold.active=1;menu.tools=TOOL_HEX;}
    if (argc > 1 && !strcmp(argv[1], "context-confirm")) {menu.help_hold.active=1;menu.tools=TOOL_CONFIRM_RESTORE;}
    if (argc > 1 && !strcmp(argv[1], "help")) menu.tools=TOOL_HELP;
    if (argc > 1 && !strcmp(argv[1], "diagnostics")) {
        menu.tools=TOOL_DIAG; memcpy(menu.ode.identity,"LOCODE",7);
        menu.ode.revision[0]=menu.ode.revision[1]=1;
        { unsigned char raw[]={0x93,1,'L','O','C','O','D','E',1,1,0,0}; memcpy(menu.ode.identity_reply,raw,12); }
    }
    if (argc > 1 && (!strcmp(argv[1], "diagnostics-state") || !strcmp(argv[1], "diagnostics-checked"))) {
        menu.tools=TOOL_DIAG; menu.tool_selected=1;
        menu.usb_tested=!strcmp(argv[1], "diagnostics-checked") ? 1 : 0;
    }
    if (argc>1 && !strcmp(argv[1],"diagnostics-preview")) {
        menu.tools=TOOL_DIAG; menu.tool_selected=3; menu.preview_result=1;
        menu.preview_load_ms=31420; menu.preview_read_ms=24100; menu.preview_check_ms=30;
        menu.preview_navigation_ms=7100; menu.preview_setup_ms=190; menu.preview_bytes=1015000;
        menu.preview_frames=225; menu.preview_fps=15; menu.preview_commands=1620;
    }
    if (argc>1 && !strcmp(argv[1],"diagnostics-file")) {
        menu.tools=TOOL_DIAG; menu.tool_selected=4; menu.ode.navigation_failed=menu.bus.timed_out=1;
        strcpy(menu.ode.file_path,"Road Rash (USA)/.ohmy3do/cover-fde7132585997845f9befacb.3dc");
        menu.usb_art.count=536; menu.usb_previews.count=282; menu.metadata.count=483;
    }
    if (argc > 1 && !strcmp(argv[1], "saves")) {
        static const char names[]="=EA_RRX\tRoad Rash\n*NFS_\tThe Need for Speed\n";
        if (save_names_parse(&menu.save_names,names,sizeof(names)-1,"",0)) abort();
        menu.save_names_loaded=1; menu.tools=TOOL_SAVES; scan_nvram(&menu);
    }
    if (argc > 1 && !strcmp(argv[1], "save-hex")) {
        preview_nvram_fixture(&menu.backup); menu.save_selected=0; open_hex(&menu);
    }
    if (argc > 1 && !strcmp(argv[1], "save-usb")) { menu.tools=TOOL_SLOTS; menu.backup_mode=0; }
    if (argc > 1 && !strcmp(argv[1], "restore")) {
        preview_nvram_fixture(&menu.backup); menu.tools=TOOL_CONFIRM_RESTORE;
    }
    if (argc > 1 && !strcmp(argv[1],"display")) {
        menu.tools=TOOL_CONFIG; menu.settings_usb_covers=menu.config.usb_covers;
        menu.settings_folder_workaround=menu.config.folder_workaround;
    }
    if (argc > 1 && !strcmp(argv[1],"display-save")) { menu.tools=TOOL_CONFIG; menu.tool_selected=11; }
    if (argc>1 && !strcmp(argv[1],"results")) {
        menu.tools=TOOL_RESULTS; strcpy(menu.find_query,"doom"); menu.catalog.valid=1;
        menu.catalog.paths=(char **)calloc(2,sizeof(char *));
        menu.catalog.paths[0]="/Storage/Games/US/Doom.iso";
        menu.catalog.paths[1]="/Storage/Games/Japan/Doom.cue"; menu.catalog.count=2;
        menu.search_count=2; menu.search_matches[0]=0; menu.search_matches[1]=1;
    }
    if (argc>1 && (!strcmp(argv[1],"formats") || !strcmp(argv[1],"formats-large"))) {
        size_t i;
        for (i=0;i<menu.listing.count;++i) {
            MenuEntry *entry=(MenuEntry *)menu.listing.entries[i];
            if (!entry->is_directory && i%2) {
                char *name=(char *)entry->name;
                size_t n=strlen(name);
                memcpy(name+n-4,".cue",5);
            }
        }
        if (!strcmp(argv[1],"formats-large")) {
            menu.config.large_titles=1; menu.config.screen_margin=12;
        }
    }
    if (argc > 1 && !strcmp(argv[1],"find")) { menu.tools=TOOL_FIND; strcpy(menu.find_query,"ROAD"); menu.find_key=17; }
    if (argc > 1 && !strcmp(argv[1],"filtered")) strcpy(menu.filter,"disc 1");
    if (argc > 1 && !strcmp(argv[1],"unsaved")) { menu.tools=TOOL_CONFIG; menu.settings_recent_games=0; }
    if (argc > 1 && !strcmp(argv[1],"recent")) {
        menu.tools=TOOL_RECENT; menu_favorites_parse(&menu.recent,"Games/D (Disc 1).iso\nRoad Rash.iso\n",strlen("Games/D (Disc 1).iso\nRoad Rash.iso\n"));
    }
    if (argc > 1 && !strcmp(argv[1],"alphabet")) {
        menu.selected=0; alphabet_input(&menu,ControlUp); menu.jump_letter=4;
    }
    if (argc > 1 && !strcmp(argv[1],"alphabet-large")) {
        menu.config.large_titles=1; menu.config.screen_margin=12;
        menu.selected=0; alphabet_input(&menu,ControlUp); menu.jump_letter=18;
    }
    if (argc > 1 && !strcmp(argv[1],"large")) menu.config.large_titles=1;
    if (argc > 1 && !strcmp(argv[1],"margin")) menu.config.screen_margin=12;
    if (argc > 1 && !strcmp(argv[1],"tools-margin")) { menu.tools=TOOL_HOME; menu.config.screen_margin=12; }
    if (argc > 1 && !strcmp(argv[1],"settings-margin")) { menu.tools=TOOL_CONFIG; menu.config.screen_margin=12; }
    if (argc > 1 && !strcmp(argv[1],"favorites")) {
        menu.tools=TOOL_FAVORITES;
        menu_favorites_parse(&menu.favorites,"Road Rash.iso\nGames/D (Disc 1).iso",strlen("Road Rash.iso\nGames/D (Disc 1).iso"));
    }
    if (argc > 1 && !strcmp(argv[1],"slot-details")) {
        preview_nvram_fixture(&menu.backup); menu.tools=TOOL_BACKUP_DETAILS; menu.backup_slot_valid=1;
    }
    if (argc>1 && !strncmp(argv[1],"gecko",5)) {
        static LibraryGameInfo gecko_info={"Alone in the Dark.iso","Adventure","Infogrames","1994-09-29"};
        menu.selected=2;
        if (strcmp(argv[1],"gecko") && strcmp(argv[1],"gecko-folder")) { menu.metadata.entries=&gecko_info; menu.metadata.count=1; }
        if (!strcmp(argv[1],"gecko-folder")) menu.selected=0;
        if (!strcmp(argv[1],"gecko-cover")) menu.cover=&cover;
        if (!strcmp(argv[1],"gecko-full")) { menu.cover=&cover; menu.full_view=1; menu.preview_off=1; menu.preview_has=1; }
        if (!strcmp(argv[1],"gecko-tools")) menu.tools=TOOL_HOME;
        if (!strcmp(argv[1],"gecko-settings")) { menu.tools=TOOL_CONFIG; menu.tool_selected=2; }
        if (!strcmp(argv[1],"gecko-margin")) menu.config.screen_margin=12;
        if (!strcmp(argv[1],"gecko-large")) menu.config.large_titles=1;
        if (!strcmp(argv[1],"gecko-alphabet")) { menu.selected=0; alphabet_input(&menu,ControlUp); }
        if (!strcmp(argv[1],"gecko-folder-game")) {
            /* "Multi Disc" holds one game, so it lists and plays as that game. */
            static LibraryGameInfo folder_info={"Multi Disc/Alone in the Dark.iso","Adventure","Infogrames","1994-09-29"};
            menu.metadata.entries=&folder_info; menu.metadata.count=1; menu.selected=1;
        }
        if (!strcmp(argv[1],"gecko-queued")) {
            menu.playlist.synchronized=1;
            if (ode_playlist_append_local(&menu.ode,&menu.playlist,"/Storage/D (Disc 1).iso") < 0 ||
                ode_playlist_append_local(&menu.ode,&menu.playlist,"/Storage/D (Disc 2).iso") < 0) abort();
        }
        if (!strcmp(argv[1],"gecko-tuning")) { menu.cover=&cover; menu.tuning=1; menu.tuning_bytes=700UL*1024UL; preview_now=1340; }
        menu.saved_config=menu.config;
    }
    if (argc>1 && (!strcmp(argv[1],"gecko-preview") || !strcmp(argv[1],"gecko-preview-full"))) {
        static CCB video={2,0,0,0};
        video.ccb_Width=160; video.ccb_Height=120;
        menu.preview_cel=&video; menu.preview.table=(unsigned char *)"synthetic";
        menu.preview.frames=1; menu.preview.fps=15;
    }
    if (argc>1 && !strncmp(argv[1],"metadata",8)) {
        static LibraryGameInfo info={"Alone in the Dark.iso","Adventure","Infogrames","1994-09-29"};
        menu.metadata.entries=&info; menu.metadata.count=1; menu.selected=2; menu.cover=&cover;
        if (!strcmp(argv[1],"metadata-large")) menu.config.large_titles=1;
        if (!strcmp(argv[1],"metadata-margin")) menu.config.screen_margin=12;
        if (!strcmp(argv[1],"metadata-missing")) menu.selected=3;
        if (!strcmp(argv[1],"metadata-long")) {
            info.genre="Adventure; Role-Playing; Strategy";
            info.publisher="A very long publishing company name";
        }
    }
    if (argc>1 && !strcmp(argv[1],"gecko-playback")) {
        /* Playback: two full frames fill both screen buffers with the list
         * panel; later frames redraw only the picture side and caption. */
        static CCB video={2,0,0,0};
        int step;
        menu.selected=2;
        video.ccb_Width=160; video.ccb_Height=120;
        menu.preview_cel=&video; menu.preview.table=(unsigned char *)"synthetic";
        menu.preview.frames=1; menu.preview.fps=15;
        for (step=0;step<4;++step) { printf("FRAME %d\n",step); draw(&menu); }
        if (menu.panel_frames!=2) abort();
        menu.selected=3; printf("FRAME 4\n"); draw(&menu);
        if (menu.panel_frames!=1) abort();
        return 0;
    }
    if (argc>1 && !strcmp(argv[1],"gecko-tuning-progress")) {
        /* Loading: the loader polls for cancel before each 2 KiB block; the
         * static redraws about 15 times a second, fully only for the first two. */
        int step;
        menu.cover=&cover; menu.tuning=1; preview_now=5000;
        for (step=0;step<5;++step) { preview_now+=70; printf("FRAME %d\n",step); media_cancel(&menu); }
        if (menu.tuning_draws!=5 || menu.tuning_full!=2 || menu.tuning_bytes!=5*2048) abort();
        return 0;
    }
    if (argc>1 && !strcmp(argv[1],"gecko-preview-full")) menu.full_view=2;
    draw(&menu);
    if (argc>1 && !strcmp(argv[1],"font-memory")) {
        unsigned long compact=0,baseline=0;
        int slot;
        for (slot=0;slot<UI_TEXT_CACHE;++slot) if(menu.text_cache[slot].cel) {
            compact+=(unsigned long)menu.text_cache[slot].capacity_width/2*UI_FONT_HEIGHT;
            baseline+=UI_FONT_STRIDE*UI_FONT_HEIGHT;
        }
        if (!baseline || compact>=baseline/2) abort();
        fprintf(stderr,"Font texture storage: %lu bytes; previously %lu bytes\n",compact,baseline);
    }
    menu_favorites_free(&menu.favorites);
    save_archive_free(&menu.nvram); save_archive_free(&menu.backup);
    ode_listing_free(&menu.listing);
    return 0;
}
'''


def color(value):
    value = int(value)
    return "#" + "".join(f"{((value >> shift) & 31) * 255 // 31:02x}"
                          for shift in (10, 5, 0))


def main():
    source = (ROOT / "src/main.c").read_text()
    drawing = source[source.index("#define MENU_VERSION"):source.index("static void progress(")]
    loading = source[source.index("static void progress("):source.index("static int directory_path(")]
    covers = source[source.index("static int directory_path("):source.index("static void show_demo(")]
    demo = source[source.index("static void show_demo("):source.index("static void connect_ode(")]
    back = source[source.index("static void go_back(Menu *menu)\n{"):source.index("static void activate(Menu *menu)")]
    queue = source[source.index("static void queue_selected_disc(Menu *menu)\n{"):source.index("/* Tools stay available")]
    tools = source[source.index("/* Tools stay available"):source.index("int main(int argc") ]
    font = (ROOT / "third_party/portfolio_os/src/graphics/text_deffont.c").read_text()
    images = {}
    for name, body in re.findall(r"CelData (Image_\w+)\[\]\s*=\s*\{(.*?)\};", font, re.S):
        words = re.findall(r"0x([0-9a-fA-F]{8})", body)
        assert len(words) == 24, name
        images[name] = [int(word, 16) >> 24 for word in words[::3]]
    glyphs = {char: images[name] for char, name in
              re.findall(r"\(int32\)'(.)', 8, (Image_\w+)", font)}
    logo = base64.b64encode((ROOT / "assets/logo-menu.png").read_bytes()).decode()
    logo_size = struct.unpack_from('>II', (ROOT / "assets/logo-menu.png").read_bytes(), 16)
    output = ROOT / "build"
    output.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="oh3do-preview-") as directory:
        code, binary = Path(directory) / "preview.c", Path(directory) / "preview"
        code.write_text(STUBS + "\n#define usb_file_text preview_text_read\n#define usb_file_text_write preview_text_write\n" + drawing + '\n#define ode_read_directory preview_read_directory\n' +
                        '\n#define usb_art_index_load preview_art_index\n#define usb_media_index_load preview_video_index\n#define library_metadata_load preview_metadata_index\n' + loading + '\n#undef usb_art_index_load\n#undef usb_media_index_load\n#undef library_metadata_load\n#undef ode_read_directory\n#define usb_preview_load_options preview_video_load_profiled\n#define usb_png_cover preview_png_load\n' + covers + '\n#undef usb_preview_load_options\n#undef usb_png_cover\n' + demo + back + NVRAM_STUBS + "\n#define usb_file_text preview_text_read\n#define usb_file_text_write preview_text_write\n" + queue + tools + MAIN)
        (output / "menu-preview.c").write_text(code.read_text())
        subprocess.run([os.environ.get("CC", "cc"), "-std=c99", "-O1", "-I", str(ROOT / "src"),
                        str(code), str(ROOT / "src/ode.c"), str(ROOT / "src/menu_sort.c"),
                        str(ROOT / "src/menu_help.c"), str(ROOT / "src/game_info.c"), str(ROOT / "src/menu_ui.c"), str(ROOT / "src/menu_categories.c"), str(ROOT / "src/ui_font.c"),
                        str(ROOT / "src/usb_art.c"), str(ROOT / "src/cover_cache.c"), str(ROOT / "src/usb_media.c"), str(ROOT / "src/preview_v5.c"), str(ROOT / "src/png_decode.c"), str(ROOT / "src/png_inflate.c"), str(ROOT / "src/art_navigation.c"),
                        str(ROOT / "src/save_archive.c"), str(ROOT / "src/save_cheats.c"), str(ROOT / "src/save_cheats_usb.c"), str(ROOT / "src/save_usb.c"), str(ROOT / "src/save_names.c"), str(ROOT / "src/save_catalog.c"),
                        str(ROOT / "src/library_index.c"), str(ROOT / "src/library_features.c"), str(ROOT / "src/library_navigation.c"),
                        str(ROOT / "src/menu_settings.c"), str(ROOT / "src/menu_config.c"), str(ROOT / "src/menu_position.c"),
                        "-o", str(binary)], check=True)
        subprocess.run([str(binary), "self-test"], check=True, stdout=subprocess.DEVNULL)
        subprocess.run([str(binary), "browser-render"], check=True, stdout=subprocess.DEVNULL)
        print("Menu view/input: preview scheduling/playback/cancellation, alphabet focus/jump/cancel, browser, configuration discard, find keyboard, history persistence, favorite ordering and diagnostics export passed")
        subprocess.run([str(binary),"font-memory"],stdout=subprocess.DEVNULL,check=True)
        playback=subprocess.check_output([str(binary),"gecko-playback"],text=True).split("FRAME ")[1:]
        logos=[sum(line.startswith("I 1 ") for line in frame.splitlines()) for frame in playback]
        assert logos==[1,1,0,0,1], logos  # light frames leave the list panel alone; a new selection redraws it
        print("Gecko playback: picture-only frames once both buffers hold the list panel")
        timing_lines=subprocess.check_output([str(binary),"diagnostics-preview"],text=True).splitlines()
        assert "L Last preview load" in timing_lines and "L Read + CRC: 24100ms" in timing_lines
        assert "L Folder navigation: 7100ms" in timing_lines and "L Frames: 225 at 15 fps" in timing_lines
        file_lines=subprocess.check_output([str(binary),"diagnostics-file"],text=True).splitlines()
        assert "L Last requested USB file:" in file_lines and "L Indexes: 536 covers / 282 clips" in file_lines
        assert "L Game metadata: 483 entries" in file_lines and "L Retained after a USB timeout." in file_lines
        for mode in ("context-help", "context-hex", "context-confirm", "library", "queue", "cover", "loading", "nested", "tools", "help", "diagnostics", "diagnostics-state", "diagnostics-checked", "saves", "save-usb", "save-hex", "restore", "display", "display-save", "large", "margin", "favorites", "slot-details", "find", "filtered", "unsaved", "recent", "alphabet", "alphabet-large", "tools-margin", "settings-margin", "results", "formats", "formats-large", "metadata", "metadata-large", "metadata-margin", "metadata-missing", "metadata-long", "gecko", "gecko-metadata", "gecko-cover", "gecko-preview", "gecko-folder", "gecko-tools", "gecko-settings", "gecko-large", "gecko-margin", "gecko-alphabet", "gecko-tuning", "gecko-queued", "gecko-folder-game", "gecko-full", "gecko-preview-full"):
            commands = subprocess.check_output([str(binary), mode], text=True).splitlines()
            if mode == "gecko-cover":
                assert sum(command.startswith("I 2 ") for command in commands)==1, "Draw one centered cover without a stretched duplicate"
            if mode.startswith("gecko") and mode not in ("gecko-full", "gecko-preview-full"):
                assert sum(command.startswith("I 1 ") for command in commands) == 1, "Gecko TV must retain the graphical logo"
            if mode == "gecko-folder-game":
                assert "L Adventure  -  1994" in commands and "L Play" in commands and "L Browse folder" not in commands
            if mode == "gecko-queued":
                assert "L QUEUE 2" in commands and "L Queue" in commands  # OSD readout and the P prompt
            if mode == "gecko-preview-full":
                assert "I 4 0.000000 0.000000 2.000000 2.000000" in commands
            if mode == "gecko-preview":
                assert "L Cover" in commands  # X swaps the playing preview for the cover
                assert "I 4 152.000000 45.000000 1.000000 1.000000" in commands  # the whole frame at native size, beside the solid list
            if mode == "gecko-settings":
                assert "L Folder workaround" in commands and "L AUTO" in commands and not any("Theme" in c for c in commands)
            if mode in ("metadata","metadata-large","metadata-margin"):
                assert "L Adventure  -  1994" in commands and "L Infogrames" in commands
            if mode == "metadata-missing":
                assert "L No game information" in commands
            if mode == "results":
                assert "L ISO" in commands and "L CUE" in commands
            if mode in ("tools", "diagnostics-state"):
                assert not any("USB write" in command for command in commands)
            if mode == "diagnostics-checked":
                assert any("USB write check: PASS" in command for command in commands)
            svg = ['<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="320" height="240" '
                   'viewBox="0 0 320 240" shape-rendering="crispEdges">']
            for command in commands:
                if command.startswith("L "):
                    continue
                if command.startswith("B "):
                    _, x, y, sx, sy, c, width, height, data = command.split()
                    x,y,sx,sy=map(float,(x,y,sx,sy))
                    data=bytes.fromhex(data)
                    palette=c.split(",")
                    # Text is drawn in screen pixels; the mascot uses the
                    # menu's margin transform like the other artwork cels.
                    assert 0 < sx <= 1 and 0 < sy <= 1
                    for row in range(int(height)):
                        for col in range(int(width)):
                            coverage=(data[row*144+col//2] >> (0 if col%2 else 4)) & 15
                            if coverage:
                                assert 0 <= x+col*sx < 320 and 0 <= y+row*sy < 240
                                svg.append(f'<rect x="{x+col*sx}" y="{y+row*sy}" width="{sx}" height="{sy}" fill="{color(palette[coverage])}"/>')
                elif command.startswith("P "):
                    _, x, y, sx, sy, width, height, data = command.split()
                    x,y,sx,sy=map(float,(x,y,sx,sy))
                    width,height=int(width),int(height)
                    for row in range(height):
                        top=y+row*sy if sy>0 else y+row*sy+sy+1
                        for col in range(width):
                            value=int(data[(row*width+col)*4:(row*width+col)*4+4],16)
                            left=x+col*sx
                            if value and -abs(sx) < left < 320 and -abs(sy) < top < 240:
                                svg.append(f'<rect x="{left}" y="{top}" width="{sx}" height="{abs(sy)}" fill="{color(str(value))}"/>')
                elif command.startswith("I "):
                    _, tag, x, y, sx, sy = command.split()
                    x,y,sx,sy=map(float,(x,y,sx,sy))
                    source_art = ROOT / ("assets/demo-preview.svg" if tag == "4" else "assets/demo-cover.png" if tag == "2" else "assets/logo-menu.png")
                    art = base64.b64encode(source_art.read_bytes()).decode()
                    width,height=(160,120) if tag == "4" else (64,80) if tag == "2" else logo_size
                    svg.append(f'<image x="{x}" y="{y}" width="{width*sx}" height="{height*sy}" '
                               f'xlink:href="data:{"image/svg+xml" if tag == "4" else "image/png"};base64,{art}"/>')
                elif command.startswith("G "):
                    _, x, y, w, h, c, keep = command.split()
                    assert 0 <= int(x) < int(x) + int(w) <= 320
                    assert 0 <= int(y) < int(y) + int(h) <= 240
                    svg.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="{color(c)}" fill-opacity="{(8-int(keep))/8}"/>')
                elif command.startswith("R "):
                    _, x, y, w, h, c = command.split()
                    assert 0 <= int(x) < int(x) + int(w) <= 320
                    assert 0 <= int(y) < int(y) + int(h) <= 240
                    svg.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="{color(c)}"/>')
                else:
                    _, x, y, c, sx, sy, label = command.split(" ", 6)
                    x, y = float(x), float(y)
                    sx,sy=float(sx),float(sy)
                    assert x >= 0 and x + (len(label)-1)*8 + 8*sx <= 320.01, label
                    assert y >= 0 and y + 8*sy <= 240.01, label
                    for char in label.upper():
                        for row, bits in enumerate(glyphs.get(char, images["Image_WhatTheHell"])):
                            for col in range(8):
                                if bits & (128 >> col):
                                    svg.append(f'<rect x="{x+col*sx}" y="{y+row*sy}" width="{sx}" '
                                               f'height="{sy}" fill="{color(c)}"/>')
                        x += 8
            svg.append("</svg>")
            target = output / f"menu-preview-{mode}.svg"
            target.write_text("\n".join(svg))
            print(target)
            if shutil.which("magick"):
                # ImageMagick's internal SVG reader cannot resolve data URLs.
                # Use an absolute asset path in its temporary input only.
                local = Path(directory) / "render.svg"
                local.write_text(target.read_text().replace(
                    f"data:image/png;base64,{logo}", str(ROOT / "assets/logo-menu.png")).replace(
                    "data:image/png;base64," + base64.b64encode((ROOT / "assets/demo-cover.png").read_bytes()).decode(),
                    str(ROOT / "assets/demo-cover.png")).replace(
                    "data:image/svg+xml;base64," + base64.b64encode((ROOT / "assets/demo-preview.svg").read_bytes()).decode(),
                    str(ROOT / "assets/demo-preview.svg")))
                png = target.with_suffix(".png")
                subprocess.run(["magick", str(local), "-filter", "point",
                                "-resize", "960x720", str(png)], check=True)
                print(png)


if __name__ == "__main__":
    main()
