/*
 * Host stand-ins for the menu preview (see tools/preview-menu.py).
 *
 * This file comes first in the generated program, before slices of
 * src/main.c. It provides what the console's SDK would: graphics, the font,
 * the controller, timers, sound and USB reads. Instead of drawing, the
 * graphics calls print one line per drawing command, which preview-menu.py
 * turns into an SVG:
 *   L text                       the text of a label (not drawn)
 *   T x y color sx sy text       text in the SDK's 8x8 font
 *   B x y sx sy palette w h data a UI-font text bitmap (4 bits per pixel)
 *   P x y sx sy w h data         an RGB555 picture
 *   I tag x y sx sy              a picture by tag: 1 logo, 2 cover, 4 preview
 *   R x y w h color              a filled rectangle
 *   G x y w h color keep         a tinted "glass" panel
 * They also keep a small software screen, so tests can compare partial
 * redraws with full ones, and count calls and fail on request so the tests
 * can check how the menu reacts.
 */
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
#include "om3dvid7_sound.h"
#include "preview_stream.h"
#include "channel_gecko_data.h"
/* Text width measurements are counted to check the width cache. */
static unsigned long preview_width_calls;
static int preview_font_width(const char *value, int style)
{
    ++preview_width_calls;
    return ui_font_width(value, style);
}
#define ui_font_width preview_font_width
/* Preview sound: counts starts and can be made to fail. */
static int audio_starts, audio_restarts, audio_fail, display_count, audio_start_displays;
static unsigned long preview_now, preview_load_delay, preview_display_delay;
int om3dvid7_sound_start(Om3dvid7Sound *a, unsigned char *p, unsigned long n, unsigned long rate)
{
    (void)p;
    (void)n;
    (void)rate;
    ++audio_starts;
    audio_start_displays = display_count;
    a->status = audio_fail ? -1 : 1;
    return audio_fail ? -1 : 0;
}
int om3dvid7_sound_restart(Om3dvid7Sound *a)
{
    ++audio_restarts;
    return a->status == 1 ? 0 : -1;
}
void om3dvid7_sound_close(Om3dvid7Sound *a)
{
    a->status = 0;
}
int om3dvid7_sound_start_ring(Om3dvid7Sound *a, unsigned char *p, unsigned long n,
                              unsigned long rate)
{
    (void)p;
    (void)n;
    (void)rate;
    a->status = 1;
    return 0;
}
void om3dvid7_sound_pause(Om3dvid7Sound *a, int pause)
{
    (void)a;
    (void)pause;
}
/* The player's sound callbacks, as om3dvid7_sound.c defines them. */
int om3dvid7_sound_player_start(void *sound, signed char *ring, unsigned long samples,
                                unsigned long rate)
{
    return om3dvid7_sound_start_ring((Om3dvid7Sound *)sound, (unsigned char *)ring, samples, rate);
}
void om3dvid7_sound_player_pause(void *sound, int pause)
{
    om3dvid7_sound_pause((Om3dvid7Sound *)sound, pause);
}
void om3dvid7_sound_player_stop(void *sound)
{
    om3dvid7_sound_close((Om3dvid7Sound *)sound);
}
#define UI_PREVIEW_TEXT(s) printf("L %s\n", s)
/* Glass: screen*keep/8 + color*(8-keep)/8, placed exactly as rectangle() places fills. */
#define UI_PREVIEW_GLASS(menu, x, y, w, h, color, keep)                                            \
    do {                                                                                           \
        int m_ = (menu)->config.screen_margin;                                                     \
        printf("G %d %d %d %d %u %d\n", m_ + (x) * (320 - 2 * m_) / 320,                           \
               m_ + (y) * (240 - 2 * m_) / 240,                                                    \
               ((x) + (w)) * (320 - 2 * m_) / 320 - (x) * (320 - 2 * m_) / 320,                    \
               ((y) + (h)) * (240 - 2 * m_) / 240 - (y) * (240 - 2 * m_) / 240, (color), (keep));  \
    } while (0)
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#define abort()                                                                                    \
    do {                                                                                           \
        fprintf(stderr, "Preview assertion failed at line %d\n", __LINE__);                        \
        exit(1);                                                                                   \
    } while (0)
/* The SDK's basic types, cels (CCB) and font. */
typedef unsigned long u32;
typedef unsigned char u8;
typedef int Item;
typedef unsigned int Color;
typedef unsigned long CelData;
typedef struct {
    int tag;
    long ccb_XPos, ccb_YPos;
    unsigned long ccb_Flags;
    void *ccb_SourcePtr;
    long ccb_HDX, ccb_HDY, ccb_VDX, ccb_VDY;
    void *ccb_PLUTPtr;
    unsigned long ccb_PRE0, ccb_PRE1;
    int ccb_Width, ccb_Height;
} CCB;
typedef struct {
    CCB *font_CCB;
} Font;
static CCB preview_font;
static Font preview_font_state = {&preview_font};
static Font *GetCurrentFont(void)
{
    return &preview_font_state;
}
static void SetCurrentFontCCB(CCB *font)
{
    preview_font = *font;
}
#define CCB_LAST          0x40000000UL
#define CCB_BGND          0x20UL
#define CREATECEL_UNCODED 0
typedef struct {
    int sc_curScreen;
    Item sc_BitmapItems[2], sc_Screens[2];
} ScreenContext;
/* The USB bus's state, as the diagnostics screen reads it. */
typedef struct {
    int last_error, timed_out;
    unsigned long commands;
    unsigned char last_command, last_reply[32], failed_command, failed_status;
    int failure_error;
    size_t last_length;
    unsigned long kind_count[17], kind_ms[17];
} OdeBus;
/* The USB drive's text files (positions, recent games, favorites, diagnostics), kept in
 * memory; reads and writes can be turned off or made to fail. */
static int preview_text_enabled, preview_reads, preview_writes, preview_read_fail,
    preview_write_fail;
static char preview_position[9216], preview_recent[8192], preview_favorites[8192],
    preview_diagnostics[4096];
static char *preview_text_slot(const char *name)
{
    if (!strcmp(name, "last-position.txt"))
        return preview_position;
    if (!strcmp(name, "recent.txt"))
        return preview_recent;
    if (!strcmp(name, "favorites.txt"))
        return preview_favorites;
    if (!strcmp(name, "diagnostics.txt"))
        return preview_diagnostics;
    return NULL;
}
static int preview_text_read(Ode *ode, OdeReadBuffer reader, const char *name, char *out,
                             size_t capacity, size_t *length)
{
    char *slot = preview_text_slot(name);
    (void)ode;
    (void)reader;
    if (!preview_text_enabled || !slot)
        return -1;
    ++preview_reads;
    if (preview_read_fail || strlen(slot) >= capacity)
        return -1;
    strcpy(out, slot);
    *length = strlen(slot);
    return 0;
}
static int preview_text_write(Ode *ode, OdeReadBuffer reader, const char *name, const char *text)
{
    char *slot = preview_text_slot(name);
    (void)reader;
    if (!preview_text_enabled || !slot)
        return -1;
    ++preview_writes;
    if (preview_write_fail)
        return -1;
    strcpy(slot, text);
    ((OdeBus *)ode->context)->last_command = 0xe4;
    return 0;
}
/* Controller buttons, as the SDK numbers them. */
#define ControlUp         0x40000000UL
#define ControlDown       0x80000000UL
#define ControlLeft       0x10000000UL
#define ControlRight      0x20000000UL
#define ControlRightShift 0x00400000UL
#define ControlLeftShift  0x00200000UL
#define ControlA          0x08000000UL
#define ControlB          0x04000000UL
#define ControlC          0x02000000UL
#define ControlX          0x00800000UL
#define ControlStart      0x01000000UL

/* Cover and index loading from the USB drive. */
static int preview_png_loads, preview_png_fail;
static int preview_png_load(Ode *ode, OdeReadBuffer reader, const char *file, unsigned char *pixels,
                            MediaCancel cancel, void *context)
{
    (void)ode;
    (void)reader;
    (void)file;
    ++preview_png_loads;
    if (preview_png_fail || (cancel && cancel(context)))
        return -1;
    memset(pixels, preview_png_loads, USB_COVER_PIXELS);
    return 0;
}
static int preview_index_reads;
static int preview_art_index(Ode *ode, OdeReadBuffer read, UsbArtIndex *index)
{
    (void)ode;
    (void)read;
    usb_art_index_free(index);
    ++preview_index_reads;
    return 0;
}
static int preview_video_index(Ode *ode, OdeReadBuffer read, UsbArtIndex *index, const char *folder)
{
    (void)folder;
    return preview_art_index(ode, read, index);
}
static int preview_metadata_index(Ode *ode, OdeReadBuffer read, LibraryMetadata *index)
{
    (void)ode;
    (void)read;
    library_metadata_free(index);
    ++preview_index_reads;
    return 0;
}
/* The controller: the buttons the test is "holding". */
static unsigned long preview_buttons;
typedef struct {
    unsigned long cped_ButtonBits;
} ControlPadEventData;
static int GetControlPad(int a, int b, ControlPadEventData *pad)
{
    (void)a;
    (void)b;
    pad->cped_ButtonBits = preview_buttons;
    return 0;
}
typedef struct {
    Color color;
    int x, y;
} GrafCon;
typedef struct {
    int rect_XLeft, rect_YTop, rect_XRight, rect_YBottom;
} Rect;
/* Small software display for comparing incremental draws with full redraws. */
static unsigned short preview_screen[2][320 * 240];
static int preview_raster, preview_front, preview_primitives;
static void preview_fill(int item, int x, int y, int w, int h, unsigned short color)
{
    int row, col;
    if (!preview_raster)
        return;
    for (row = y; row < y + h && row < 240; ++row)
        for (col = x; col < x + w && col < 320; ++col)
            if (row >= 0 && col >= 0)
                preview_screen[item][row * 320 + col] = color;
}
static void preview_raster_cel(int item, CCB *cel, double sx, double sy)
{
    int x, y, w = cel->ccb_Width, h = cel->ccb_Height;
    int stride = (int)(((cel->ccb_PRE1 >> 24) & 255) + 2) * 4;
    double px = cel->ccb_XPos / 65536.0, py = cel->ccb_YPos / 65536.0;
    if (!preview_raster)
        return;
    if (cel->tag == 1) {
        w = 96;
        h = 32;
    }
    for (y = 0; y < h; ++y)
        for (x = 0; x < w; ++x) {
            unsigned short color;
            int left = (int)(px + x * sx), top = (int)(py + y * sy),
                right = (int)(px + (x + 1) * sx), bottom = (int)(py + (y + 1) * sy);
            if (sx < 0) {
                int t = left;
                left = right + 1;
                right = t + 1;
            }
            if (sy < 0) {
                int t = top;
                top = bottom + 1;
                bottom = t + 1;
            }
            if (cel->tag == 3) {
                int coverage =
                    (((unsigned char *)cel->ccb_SourcePtr)[y * stride + x / 2] >> (x % 2 ? 0 : 4)) &
                    15;
                if (!coverage)
                    continue;
                color = ((unsigned short *)cel->ccb_PLUTPtr)[coverage];
            } else if (cel->tag == 5) {
                color = ((unsigned short *)cel->ccb_SourcePtr)[y * (stride / 2) + x];
                if (!color)
                    continue;
            } else
                color = (unsigned short)(cel->tag == 1 ? 0x3abc : 0x1234 + (x + y) % 16);
            preview_fill(item, left, top, right - left, bottom - top, color);
        }
}
/* The SDK's simple drawing calls, printed as R and T commands. */
#define MakeRGB15(r, g, b) (((r) << 10) | ((g) << 5) | (b))
static void SetFGPen(GrafCon *g, Color c)
{
    g->color = c;
}
static void FillRect(Item item, GrafCon *g, Rect *r)
{
    ++preview_primitives;
    preview_fill(item, r->rect_XLeft, r->rect_YTop, r->rect_XRight - r->rect_XLeft + 1,
                 r->rect_YBottom - r->rect_YTop + 1, (unsigned short)g->color);
    printf("R %d %d %d %d %u\n", r->rect_XLeft, r->rect_YTop, r->rect_XRight - r->rect_XLeft + 1,
           r->rect_YBottom - r->rect_YTop + 1, g->color);
}
static void MoveTo(GrafCon *g, int x, int y)
{
    g->x = x;
    g->y = y;
}
static void DrawText8(GrafCon *g, Item item, const u8 *s)
{
    double sx = preview_font.ccb_HDX ? preview_font.ccb_HDX / 1048576.0 : 1.0;
    double sy = preview_font.ccb_VDY ? preview_font.ccb_VDY / 65536.0 : 1.0;
    (void)item;
    printf("T %d %d %u %.6f %.6f %s\n", g->x, g->y, g->color, sx, sy, s);
}
/* Preview clip loading. */
static int preview_video_enabled, preview_video_loads, preview_video_reclaim;
/* A two-frame OM3DVID5 clip: solid 0x1111, then solid 0x2222, each as SOLID
 * runs over the 1200 blocks (the second color is the cache's entry 1). */
static int preview_video_load(Ode *ode, OdeReadBuffer reader, const char *file, UsbPreview *preview,
                              MediaCancel cancel, void *context)
{
    unsigned char *packet;
    int frame, op;
    (void)ode;
    (void)reader;
    (void)file;
    ++preview_video_loads;
    if (preview_video_enabled < 0)
        return preview_video_enabled;
    if (!preview_video_enabled || (cancel && cancel(context)))
        return -1;
    preview->table = (unsigned char *)malloc(12);
    preview->chunk[0] = (unsigned char *)calloc(1, 78 + PREVIEW_V5_READ_AHEAD);
    preview->decoder = calloc(1, sizeof(PreviewV5));
    preview->frame_buffer = (unsigned char *)calloc(1, USB_PREVIEW_FRAME_BYTES);
    preview->audio = (unsigned char *)malloc(4);
    if (!preview->table || !preview->chunk[0] || !preview->decoder || !preview->frame_buffer ||
        !preview->audio)
        abort();
    for (frame = 0; frame < 3; ++frame) {
        unsigned long offset = 12UL + 39UL * (unsigned long)frame;
        preview->table[frame * 4] = 0;
        preview->table[frame * 4 + 1] = 0;
        preview->table[frame * 4 + 2] = (unsigned char)(offset >> 8);
        preview->table[frame * 4 + 3] = (unsigned char)offset;
    }
    for (frame = 0; frame < 2; ++frame) {
        packet = preview->chunk[0] + 39 * frame;
        *packet++ = 0xbf;
        *packet++ = (unsigned char)(0x11 * (frame + 1));
        *packet++ = (unsigned char)(0x11 * (frame + 1));
        for (op = 0; op < 17; ++op) {
            *packet++ = 0xbf;
            *packet++ = (unsigned char)(0x80 | frame);
        }
        *packet++ = 0xaf;
        *packet = (unsigned char)(0x80 | frame);
    }
    preview->chunk_bytes[0] = 78 + PREVIEW_V5_READ_AHEAD;
    preview->chunk_begin[0] = 12;
    preview->chunk_end[0] = 90;
    preview->chunk_count = 1;
    preview->audio_bytes = preview->audio_frames = 4;
    preview->audio_rate = 11025;
    preview->width = USB_PREVIEW_WIDTH;
    preview->height = USB_PREVIEW_HEIGHT;
    preview->frame_bytes = USB_PREVIEW_FRAME_BYTES;
    preview->frames = preview->source_frames = 2;
    preview->fps = 6;
    preview->table_bytes = 12;
    preview->video_bytes = 90;
    /* Like the real loader, frame 0 is decoded before playback starts. */
    if (usb_preview_frame(preview, 0, preview->frame_buffer) < 0)
        abort();
    return 0;
}
static int preview_video_load_profiled(Ode *ode, OdeReadBuffer reader, const char *file,
                                       UsbPreview *preview, MediaCancel cancel, void *context,
                                       MediaLoadPhase phase, void *phase_context, int sound,
                                       MediaReclaim reclaim, void *reclaim_context)
{
    int result;
    if (preview_video_reclaim && reclaim)
        reclaim(reclaim_context);
    (void)sound;
    result = preview_video_load(ode, reader, file, preview, cancel, context);
    preview_now += preview_load_delay;
    if (phase) {
        phase(phase_context, 1);
        phase(phase_context, 2);
    }
    return result;
}
/* Cels: loading, creating and freeing, with failures on request. */
static int preview_unloads;
static int preview_cel_calls, preview_cel_fail;
static CCB preview_loaded = {2, 0, 0, 0};
#define MEMTYPE_CEL 0
static CCB *LoadCel(char *file, unsigned long flags)
{
    (void)file;
    (void)flags;
    ++preview_cel_calls;
    preview_loaded.ccb_Width = 64;
    preview_loaded.ccb_Height = 80;
    return preview_cel_fail ? NULL : &preview_loaded;
}
static CCB *CreateCel(int w, int h, int bpp, int flags, void *data)
{
    static unsigned char pixels[USB_PREVIEW_FRAME_BYTES];
    (void)flags;
    (void)data;
    if (bpp == 4) {
        int stride = w / 2;
        CCB *cel = (CCB *)calloc(1, sizeof(CCB));
        if (stride < 8)
            stride = 8;
        if (!cel)
            abort();
        cel->tag = 3;
        cel->ccb_SourcePtr = calloc((size_t)stride, h);
        cel->ccb_PLUTPtr = calloc(32, 2);
        if (!cel->ccb_SourcePtr || !cel->ccb_PLUTPtr)
            abort();
        cel->ccb_PRE1 = (unsigned long)(stride / 4 - 2) << 24;
        cel->ccb_HDX = 1 << 20;
        cel->ccb_VDY = 1 << 16;
        cel->ccb_Width = w;
        cel->ccb_Height = h;
        return cel;
    }
    if (bpp == 16 && ((data && w == 86 && h == 120) ||
                      (!data && ((w == 38 && h == 16) || (w == 80 && h == 8) ||
                                 (w == 16 && h == 12) || (w == 9 && h == 9))))) {
        /* Gecko TV interface cels: TV static, channel digits, bar, CH, screen corner. */
        int stride = (w * 2 + 3) & ~3;
        CCB *cel = (CCB *)calloc(1, sizeof(CCB));
        if (stride < 8)
            stride = 8;
        if (!cel)
            abort();
        cel->tag = 5;
        cel->ccb_SourcePtr = data ? data : calloc((size_t)stride, h);
        if (!cel->ccb_SourcePtr)
            abort();
        cel->ccb_PRE1 = (unsigned long)(stride / 4 - 2) << 24;
        cel->ccb_HDX = 1 << 20;
        cel->ccb_VDY = 1 << 16;
        cel->ccb_Width = w;
        cel->ccb_Height = h;
        return cel;
    }
    preview_loaded.ccb_SourcePtr = data ? data : pixels;
    preview_loaded.ccb_Width = w;
    preview_loaded.ccb_Height = h;
    ++preview_cel_calls;
    return preview_cel_fail ? NULL : &preview_loaded;
}
/* The USB bus, timers and the display. DrawCels prints B, P and I commands. */
static int ode_bus_read_buffer(void *context, unsigned char *data, size_t size)
{
    (void)context;
    (void)data;
    (void)size;
    return -1;
}
static int ode_bus_transfer(void *context, const unsigned char *command, unsigned char *reply,
                            size_t capacity, size_t *length)
{
    (void)context;
    (void)command;
    (void)reply;
    (void)capacity;
    (void)length;
    return -1;
}
static long GetMSecTime(Item item)
{
    (void)item;
    return (long)preview_now;
}
static void WaitVBL(Item item, int fields)
{
    (void)item;
    (void)fields;
}
static void UnloadCel(CCB *cel)
{
    if (cel->tag == 3) {
        free(cel->ccb_SourcePtr);
        free(cel->ccb_PLUTPtr);
        free(cel);
    } else
        ++preview_unloads;
}
static int GetVBLTime(Item item, u32 *hi, u32 *frame)
{
    (void)item;
    *hi = 0;
    *frame = 0;
    return 0;
}
static void DrawCels(Item item, CCB *cel)
{
    double sx = cel->ccb_HDX ? cel->ccb_HDX / 1048576.0 : 1.0;
    double sy = cel->ccb_VDY ? cel->ccb_VDY / 65536.0 : 1.0;
    ++preview_primitives;
    preview_raster_cel(item, cel, sx, sy);
    if (cel->tag == 3) {
        int i;
        printf("B %.6f %.6f %.6f %.6f ", cel->ccb_XPos / 65536.0, cel->ccb_YPos / 65536.0, sx, sy);
        for (i = 0; i < 16; ++i)
            printf("%s%u", i ? "," : "", ((unsigned short *)cel->ccb_PLUTPtr)[i]);
        printf(" %d %d ", cel->ccb_Width, cel->ccb_Height);
        for (i = 0; i < UI_FONT_STRIDE * cel->ccb_Height; ++i) {
            int stride = (int)(((cel->ccb_PRE1 >> 24) & 255) + 2) * 4;
            int row = i / UI_FONT_STRIDE, col = i % UI_FONT_STRIDE;
            printf("%02x",
                   col < stride ? ((unsigned char *)cel->ccb_SourcePtr)[row * stride + col] : 0);
        }
        printf("\n");
        return;
    }
    if (cel->tag == 5) {
        int i;
        int stride = (int)(((cel->ccb_PRE1 >> 24) & 255) + 2) * 2;
        printf("P %.6f %.6f %.6f %.6f %d %d ", cel->ccb_XPos / 65536.0, cel->ccb_YPos / 65536.0, sx,
               sy, cel->ccb_Width, cel->ccb_Height);
        for (i = 0; i < cel->ccb_Width * cel->ccb_Height; ++i)
            printf("%04x",
                   ((unsigned short *)
                        cel->ccb_SourcePtr)[(i / cel->ccb_Width) * stride + i % cel->ccb_Width]);
        printf("\n");
        return;
    }
    (void)item;
    printf("I %d %.6f %.6f %.6f %.6f\n", cel->tag == 2 && cel->ccb_Width == 160 ? 4 : cel->tag,
           cel->ccb_XPos / 65536.0, cel->ccb_YPos / 65536.0, sx, sy);
}
static void DisplayScreen(Item item, int flag)
{
    preview_front = item;
    (void)flag;
    ++display_count;
    preview_now += preview_display_delay;
}
static int preview_read_directory(Ode *ode, OdeListing *listing);
const MenuCover menu_covers[] = {{"Alone in the Dark.iso", "$boot/covers/dark.cel"},
                                 {"Road Rash.iso", "$boot/covers/road.cel"}};
const size_t menu_cover_count = 2;
