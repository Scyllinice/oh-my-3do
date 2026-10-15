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
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent

STUBS = r'''
#include "ode.h"
#include "menu_config.h"
#include "menu_ui.h"
#include "usb_art.h"
#include "art_navigation.h"
#include "save_usb.h"
#include "nvram_io.h"
#include "library_features.h"
#include "library_navigation.h"
#include "menu_settings.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned long u32;
typedef unsigned char u8;
typedef int Item;
typedef unsigned int Color;
typedef struct { int tag; long ccb_XPos, ccb_YPos; unsigned long ccb_Flags; void *ccb_SourcePtr; long ccb_HDX,ccb_HDY,ccb_VDX,ccb_VDY; } CCB;
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
#define ControlUp 1
#define ControlDown 2
#define ControlLeft 4
#define ControlRight 8
#define ControlA 16
#define ControlB 32
#define ControlC 64
#define ControlX 128
#define ControlStart 256

typedef struct { Color color; int x, y; } GrafCon;
typedef struct { int rect_XLeft, rect_YTop, rect_XRight, rect_YBottom; } Rect;
#define MakeRGB15(r,g,b) (((r)<<10)|((g)<<5)|(b))
static void SetFGPen(GrafCon *g, Color c) { g->color = c; }
static void FillRect(Item item, GrafCon *g, Rect *r) {
    (void)item;
    printf("R %d %d %d %d %u\n", r->rect_XLeft, r->rect_YTop,
        r->rect_XRight-r->rect_XLeft+1, r->rect_YBottom-r->rect_YTop+1, g->color);
}
static void MoveTo(GrafCon *g, int x, int y) { g->x=x; g->y=y; }
static void DrawText8(GrafCon *g, Item item, const u8 *s) {
    double sx=preview_font.ccb_HDX ? preview_font.ccb_HDX/1048576.0 : 1.0;
    double sy=preview_font.ccb_VDY ? preview_font.ccb_VDY/65536.0 : 1.0;
    (void)item; printf("T %d %d %u %.6f %.6f %s\n", g->x, g->y, g->color, sx, sy, s);
}
static unsigned long preview_now;
static int preview_unloads;
static int preview_cel_calls, preview_cel_fail;
static CCB preview_loaded={2, 0, 0, 0};
#define MEMTYPE_CEL 0
static CCB *LoadCel(char *file, unsigned long flags) {
    (void)file; (void)flags; ++preview_cel_calls;
    return preview_cel_fail ? NULL : &preview_loaded;
}
static CCB *CreateCel(int w, int h, int bpp, int flags, void *data) {
    static unsigned char pixels[USB_ART_PIXELS];
    (void)w; (void)h; (void)bpp; (void)flags; (void)data;
    preview_loaded.ccb_SourcePtr=pixels;
    ++preview_cel_calls;
    return preview_cel_fail ? NULL : &preview_loaded;
}
static int ode_bus_read_buffer(void *context, unsigned char *data, size_t size) {
    (void)context; (void)data; (void)size; return -1;
}
static long GetMSecTime(Item item) { (void)item; return (long)preview_now; }
static void UnloadCel(CCB *cel) { (void)cel; ++preview_unloads; }
static int GetVBLTime(Item item, u32 *hi, u32 *frame) {
    (void)item; *hi=0; *frame=0; return 0;
}
static void DrawCels(Item item, CCB *cel) {
    double sx=cel->ccb_HDX ? cel->ccb_HDX/1048576.0 : 1.0;
    double sy=cel->ccb_VDY ? cel->ccb_VDY/65536.0 : 1.0;
    (void)item; printf("I %d %.6f %.6f %.6f %.6f\n",cel->tag,cel->ccb_XPos/65536.0,cel->ccb_YPos/65536.0,sx,sy);
}
static void DisplayScreen(Item item, int flag) { (void)item; (void)flag; }
static int preview_read_directory(Ode *ode, OdeListing *listing);
const MenuCover menu_covers[] = {
    {"Alone in the Dark.iso", "$boot/covers/dark.cel"},
    {"Road Rash.iso", "$boot/covers/road.cel"}
};
const size_t menu_cover_count=2;
'''

NVRAM_STUBS = r'''
static void activate(Menu *menu) { (void)menu; }
static int preview_nvram_reads, preview_nvram_writes, preview_nvram_fail;
static int preview_nvram_fixture(SaveArchive *a) {
    SaveEntry e[2];
    memset(e, 0, sizeof(e));
    strcpy(e[0].name, "Road Rash"); e[0].data=(const unsigned char *)"save"; e[0].size=4; e[0].type=0x33444f46UL;
    strcpy(e[1].name, "The Need for Speed"); e[1].data=(const unsigned char *)"other"; e[1].size=5;
    return save_archive_build(a, e, 2);
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
static int reading_fail;
static int preview_read_directory(Ode *ode, OdeListing *listing) {
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
    menu.font_ccb=*GetCurrentFont()->font_CCB; menu.have_font=1;
    logo.ccb_XPos=16L<<16; logo.ccb_YPos=8L<<16; logo.ccb_HDX=1L<<20; logo.ccb_VDY=1L<<16;
    cover.ccb_XPos=236L<<16; cover.ccb_YPos=86L<<16; cover.ccb_HDX=1L<<20; cover.ccb_VDY=1L<<16;
    show_demo(&menu);
    menu.logo=&logo;
    menu.message=NULL;
    if (argc > 1 && !strcmp(argv[1], "self-test")) {
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
        preview_now+=349;
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
        preview_now+=350;
        update_cover(&menu);
        if (preview_cel_calls != 2 || menu.cover) abort();
        update_cover(&menu);
        if (preview_cel_calls != 2) abort();
        menu.selected=0; /* folder */
        selection_changed(&menu);
        preview_now+=350;
        update_cover(&menu);
        if (preview_cel_calls != 2) abort();
        menu.selected=5; /* file without a matching cover */
        selection_changed(&menu);
        preview_now+=350;
        update_cover(&menu);
        if (preview_cel_calls != 2 || !menu.cover_attempted) abort();
        /* Tools has four entries; Help/Diagnostics never touch USB. */
        menu.tools=TOOL_HOME; menu.tool_selected=0;
        tools_input(&menu, ControlUp);
        if (menu.tool_selected != 6) abort();
        tools_input(&menu, ControlDown);
        if (menu.tool_selected != 0) abort();
        menu.tool_selected=5; tools_input(&menu, ControlA);
        if (menu.tools != TOOL_HELP || menu.usb_tested) abort();
        /* Actual tools/controller code: entering/cancelling never writes. */
        menu.tools=TOOL_HOME; menu.tool_selected=0;
        tools_input(&menu, ControlA);
        if (menu.tools != TOOL_SAVES || preview_nvram_reads != 1 || preview_nvram_writes) abort();
        tools_input(&menu, ControlA);
        if (menu.tools != TOOL_SLOTS || menu.pending.count != 1 || preview_nvram_writes) abort();
        tools_input(&menu, ControlA);
        if (menu.tools != TOOL_BACKUP_DETAILS || preview_nvram_writes) abort();
        tools_input(&menu, ControlA);
        if (menu.tools != TOOL_CONFIRM_BACKUP || preview_nvram_writes) abort();
        tools_input(&menu, ControlB);
        if (menu.tools != TOOL_SLOTS || preview_nvram_writes) abort();
        menu.tools=TOOL_HOME; menu.tool_selected=1; tools_input(&menu, ControlA);
        if (menu.tools != TOOL_SLOTS || menu.backup_mode) abort();
        preview_nvram_fixture(&menu.backup);
        menu.tools=TOOL_ARCHIVE; menu.tool_selected=0;
        tools_input(&menu, ControlA);
        if (menu.tools != TOOL_CONFIRM_RESTORE || preview_nvram_writes) abort();
        tools_input(&menu, ControlB);
        if (menu.tools != TOOL_ARCHIVE || preview_nvram_writes) abort();
        tools_input(&menu, ControlA); tools_input(&menu, ControlA);
        if (preview_nvram_writes || menu.restore_result != -1) abort(); /* demo USB blocked */
        menu.ode.navigation_failed=1; menu.tools=TOOL_HOME; menu.tool_selected=6;
        tools_input(&menu, ControlA); draw(&menu);
        if (menu.tools != TOOL_DIAG || preview_nvram_writes) abort();
        tools_input(&menu, ControlRight); draw(&menu); tools_input(&menu, ControlRight); draw(&menu);
        menu.ode.navigation_failed=0;
        /* Filtering leaves source entries and directory IDs intact. */
        {
            const MenuEntry hidden={123,0,"boot.iso"};
            const MenuEntry game={456,0,"Game.iso"};
            const MenuEntry volume={0x01000000UL,1,"covers"};
            const MenuEntry *entries[2]={&hidden,&game};
            OdeListing original=menu.listing;
            menu.listing.entries=entries; menu.listing.count=2; menu.listing.has_parent=0;
            menu.depth=0;
            if (row_count(&menu) != 1 || browser_entry(&menu,0) != &game || menu.listing.count != 2) abort();
            menu.depth=1; menu.path_ids[0]=9;
            if (row_count(&menu) != 2 || browser_entry(&menu,0) != &hidden) abort();
            menu.path_ids[0]=0x01000000UL;
            if (row_count(&menu) != 1) abort();
            if (hidden_entry(&menu, &volume)) abort();
            menu.listing=original;
        }
        /* All INI controls; navigation stays active until reboot, bypass never writes. */
        menu.tools=TOOL_CONFIG; menu.tool_selected=0; menu.config.screen_margin=0;
        tools_input(&menu,ControlLeft); if (menu.config.screen_margin != 12) abort();
        menu.tool_selected=1; tools_input(&menu,ControlA); if (!menu.config.large_titles) abort();
        menu.tool_selected=2; tools_input(&menu,ControlRight); if (menu.config.theme != 1) abort();
        menu.settings_folder_workaround=menu.config.folder_workaround=1;
        menu.settings_usb_covers=menu.config.usb_covers=0;
        menu.tool_selected=3; tools_input(&menu,ControlA);
        menu.tool_selected=4; tools_input(&menu,ControlRight);
        if (menu.settings_folder_workaround || !menu.settings_usb_covers ||
            !menu.config.folder_workaround || menu.config.usb_covers) abort();
        menu.config_result=2; tools_input(&menu,ControlC);
        if (strcmp(menu.tool_message,"Load a valid INI before saving.")) abort();
        menu.tool_selected=5; tools_input(&menu,ControlA);
        if (menu.config.screen_margin || menu.config.large_titles || menu.config.theme) abort();
        if (menu.settings_folder_workaround || !menu.settings_usb_covers) abort();
        tools_input(&menu,ControlB);
        menu.tool_selected=4; tools_input(&menu,ControlA);
        if (menu.tools != TOOL_CONFIG || menu.settings_folder_workaround || !menu.settings_usb_covers) abort();
        tools_input(&menu,ControlUp); if (menu.tool_selected != 6) abort();
        tools_input(&menu,ControlA);
        if (strcmp(menu.tool_message,"Load a valid INI before saving.")) abort();
        /* Holding C for a letter jump never queues an image. */
        menu.tools=0; menu.review=menu.demo=0; menu.ready=1; menu.playlist.synchronized=1;
        if (browser_shortcuts(&menu,ControlC,ControlC,0) & ControlC) abort();
        browser_shortcuts(&menu,ControlC|ControlRight,ControlRight,0);
        browser_shortcuts(&menu,0,0,ControlC);
        if (menu.playlist.count || menu.c_pending) abort();
        menu.demo=1;
        /* A suggested group is queued only after acceptance; cancel frees it. */
        {
            const MenuEntry first={1,0,"D (Disc 1).iso"}, second={2,0,"D (Disc 2).iso"};
            const MenuEntry *entries[2]={&second,&first};
            OdeListing original=menu.listing;
            menu.listing.entries=entries; menu.listing.count=2; menu.listing.has_parent=0;
            menu.depth=0; menu.playlist.synchronized=1;
            if (!offer_group(&menu,&first) || menu.group.count != 2 || menu.playlist.count) abort();
            tools_input(&menu,ControlB); if (menu.group.count || menu.playlist.count) abort();
            if (!offer_group(&menu,&first)) abort();
            tools_input(&menu,ControlA);
            if (menu.tools || !menu.review || menu.playlist.count != 2 || !menu.playlist.dirty ||
                strcmp(menu.playlist.paths[0],"/Storage/D (Disc 1).iso")) abort();
            ode_playlist_dispose(&menu.playlist); menu.review=0;
            menu.selected=0; menu.ready=1; jump_letter(&menu,4);
            if (menu.selected != 0) abort();
            menu.listing=original;
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
    if (argc > 1 && !strcmp(argv[1], "saves")) {
        menu.tools=TOOL_SAVES; scan_nvram(&menu);
    }
    if (argc > 1 && !strcmp(argv[1], "restore")) {
        preview_nvram_fixture(&menu.backup); menu.tools=TOOL_CONFIRM_RESTORE;
    }
    if (argc > 1 && !strcmp(argv[1],"display")) {
        menu.tools=TOOL_CONFIG; menu.settings_usb_covers=menu.config.usb_covers;
        menu.settings_folder_workaround=menu.config.folder_workaround;
    }
    if (argc > 1 && !strcmp(argv[1],"large")) menu.config.large_titles=1;
    if (argc > 1 && !strcmp(argv[1],"margin")) menu.config.screen_margin=12;
    if (argc > 1 && !strcmp(argv[1],"contrast")) menu.config.theme=1;
    if (argc > 1 && !strcmp(argv[1],"favorites")) {
        menu.tools=TOOL_FAVORITES;
        menu_favorites_parse(&menu.favorites,"Road Rash.iso\nGames/D (Disc 1).iso",strlen("Road Rash.iso\nGames/D (Disc 1).iso"));
    }
    if (argc > 1 && !strcmp(argv[1],"slot-details")) {
        preview_nvram_fixture(&menu.backup); menu.tools=TOOL_BACKUP_DETAILS; menu.backup_slot_valid=1;
    }
    draw(&menu);
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
    queue = source[source.index("static void queue_selected_disc("):source.index("/* Tools stay available")]
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
    output = ROOT / "build"
    output.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="oh3do-preview-") as directory:
        code, binary = Path(directory) / "preview.c", Path(directory) / "preview"
        code.write_text(STUBS + drawing + '\n#define ode_read_directory preview_read_directory\n' +
                        loading + '\n#undef ode_read_directory\n' + covers + demo + NVRAM_STUBS + queue + tools + MAIN)
        subprocess.run([os.environ.get("CC", "cc"), "-std=c99", "-O1", "-I", str(ROOT / "src"),
                        str(code), str(ROOT / "src/ode.c"), str(ROOT / "src/menu_sort.c"),
                        str(ROOT / "src/menu_ui.c"),
                        str(ROOT / "src/usb_art.c"), str(ROOT / "src/art_navigation.c"),
                        str(ROOT / "src/save_archive.c"), str(ROOT / "src/save_usb.c"),
                        str(ROOT / "src/library_features.c"), str(ROOT / "src/library_navigation.c"),
                        str(ROOT / "src/menu_settings.c"), str(ROOT / "src/menu_config.c"),
                        "-o", str(binary)], check=True)
        subprocess.run([str(binary), "self-test"], check=True, stdout=subprocess.DEVNULL)
        print("Menu view: retained loading list, failure gating, selection/notice timing and cover lifecycle passed")
        for mode in ("library", "queue", "cover", "loading", "nested", "tools", "help", "diagnostics", "diagnostics-state", "diagnostics-checked", "saves", "restore", "display", "large", "margin", "contrast", "favorites", "slot-details"):
            commands = subprocess.check_output([str(binary), mode], text=True).splitlines()
            if mode in ("tools", "diagnostics-state"):
                assert not any("USB write" in command for command in commands)
            if mode == "diagnostics-checked":
                assert any("USB write check: PASS" in command for command in commands)
            svg = ['<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="320" height="240" '
                   'viewBox="0 0 320 240" shape-rendering="crispEdges">']
            for command in commands:
                if command.startswith("I "):
                    _, tag, x, y, sx, sy = command.split()
                    x,y,sx,sy=map(float,(x,y,sx,sy))
                    source_art = ROOT / ("assets/demo-cover.png" if tag == "2" else "assets/logo-menu.png")
                    art = base64.b64encode(source_art.read_bytes()).decode()
                    width,height=(64,80) if tag == "2" else (133,48)
                    svg.append(f'<image x="{x}" y="{y}" width="{width*sx}" height="{height*sy}" '
                               f'xlink:href="data:image/png;base64,{art}"/>')
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
                    str(ROOT / "assets/demo-cover.png")))
                png = target.with_suffix(".png")
                subprocess.run(["magick", str(local), "-filter", "point",
                                "-resize", "960x720", str(png)], check=True)
                print(png)


if __name__ == "__main__":
    main()
