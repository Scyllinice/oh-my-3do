#include "ode.h"
#include "ode_bus.h"
#include "menu_config.h"
#include "menu_ui.h"
#include "ui_font.h"
#include "preview_audio.h"
#include "usb_art.h"
#include "cover_cache.h"
#include "usb_media.h"
#include "art_navigation.h"
#include "save_usb.h"
#include "nvram_io.h"
#include "library_features.h"
#include "library_navigation.h"
#include "library_index.h"
#include "menu_settings.h"
#include "menu_position.h"

#include "controlpad.h"
#include "celutils.h"
#include "displayutils.h"
#include "event.h"
#include "graphics.h"
#include "io.h"
#include "mem.h"
#include "timerutils.h"
#include "stdio.h"
#include "stdarg.h"
#include "stdlib.h"
#include <string.h>

#define MENU_VERSION "0.16.0-rc7"
#define ROWS 7
#define TOOL_HOME 1
#define TOOL_SAVES 2
#define TOOL_SLOTS 3
#define TOOL_ARCHIVE 4
#define TOOL_HELP 5
#define TOOL_DIAG 6
#define TOOL_CONFIRM_BACKUP 7
#define TOOL_CONFIRM_RESTORE 8
#define TOOL_GROUP 9
#define TOOL_FAVORITES 10
#define TOOL_CONFIG 11
#define TOOL_BACKUP_DETAILS 13
#define TOOL_RECENT 14
#define TOOL_FIND 15
#define TOOL_RESULTS 16
#define MAX_DEPTH 32
#define GECKO (menu->config.theme == MENU_THEME_GECKO)
#define BG (GECKO ? MakeRGB15(2,0,4) : (menu->config.theme == 1 ? MakeRGB15(0,0,0) : MakeRGB15(1,2,4)))
#define TEXT (GECKO ? MakeRGB15(31,31,25) : (menu->config.theme == 1 ? MakeRGB15(31,31,31) : MakeRGB15(29,29,26)))
#define MUTED (GECKO ? MakeRGB15(27,29,25) : (menu->config.theme == 1 ? MakeRGB15(27,27,27) : MakeRGB15(25,27,29)))
#define ACCENT (GECKO ? MakeRGB15(23,30,5) : (menu->config.theme ? MakeRGB15(31,25,10) : MakeRGB15(7,26,22)))
#define PANEL (GECKO ? MakeRGB15(5,2,8) : (menu->config.theme == 1 ? MakeRGB15(1,1,1) : (menu->config.theme == 2 ? MakeRGB15(4,3,8) : MakeRGB15(2,4,7))))
#define LINE (GECKO ? MakeRGB15(13,8,18) : (menu->config.theme == 1 ? MakeRGB15(12,12,12) : (menu->config.theme == 2 ? MakeRGB15(11,7,15) : MakeRGB15(5,9,12))))
#define SELECT (GECKO ? MakeRGB15(11,5,15) : (menu->config.theme == 1 ? MakeRGB15(7,7,7) : (menu->config.theme == 2 ? MakeRGB15(10,6,13) : MakeRGB15(4,10,13))))
#define DIM (GECKO ? MakeRGB15(13,12,17) : (menu->config.theme == 1 ? MakeRGB15(13,13,13) : MakeRGB15(9,11,14)))
#define TV_ORANGE MakeRGB15(31,15,3)
#define AMBER MakeRGB15(30, 22, 9)

#define UI_TEXT_CACHE 64
typedef struct UiText {
    CCB *cel;
    int style, width, palette_valid;
    Color ink, background;
    char value[97];
} UiText;

typedef struct Menu {
    ScreenContext screen;
    PreviewAudio preview_audio;
    CCB *splash;
    int booting;
    LibraryCatalog catalog;
    unsigned long catalog_volume;
    size_t search_matches[LIBRARY_CATALOG_MAX], search_count;
    unsigned long search_redraw;
    u32 suppress_buttons;
    UiText text_cache[UI_TEXT_CACHE];
    unsigned int text_cursor;
    int text_style, text_failed;
    int text_background_set;
    Color text_background;
    Ode ode;
    OdeBus bus;
    OdeListing listing;
    OdePlaylist playlist;
    OdePlaylist group;
    MenuFavorites favorites;
    MenuFavorites recent;
    int recent_valid;
    int favorites_move;
    char *favorites_original;
    char filter[25];
    char find_query[25];
    int find_key;
    MenuPosition last_position;
    int position_result;
    int position_dirty;
    int recent_result;
    int diagnostics_result;
    CCB font_ccb;
    int have_font;
    int large_drawing;
    int skip_group;
    int queue_move;
    int c_pending;
    int backup_slot_valid;
    int details_new;
    int jump_letter;
    int alphabet_active;
    char favorites_prefix[ODE_MAX_NAME + 10];
    int favorites_valid;
    MenuConfig config;
    MenuConfig saved_config;
    /* Stage navigation/artwork changes until reboot, including while saving. */
    int settings_folder_workaround;
    int settings_usb_covers;
    int settings_remember_position;
    int settings_recent_games;
    int settings_video_previews;
    CoverCache cover_cache;
    unsigned long cover_load_ms, cover_commands;
    int cover_cached;
    UsbArtIndex usb_art;
    UsbArtIndex usb_previews;
    LibraryMetadata metadata;
    UsbPreview preview;
    CCB *preview_cel;
    int preview_attempted, media_cancelled;
    unsigned long preview_since;
    unsigned int preview_frame;
    unsigned long preview_load_ms;
    int preview_result;
    CCB *logo;
    CCB *cover;
    Item vbl;
    Item clock;
    size_t selected;
    size_t previous_selection[MAX_DEPTH];
    char path[MAX_DEPTH][ODE_MAX_NAME];
    unsigned long path_ids[MAX_DEPTH];
    char breadcrumb[35];
    unsigned int depth;
    SaveArchive nvram;
    SaveArchive backup;
    SaveArchive pending;
    NvramInfo nvram_info;
    int tools;
    size_t tool_selected;
    size_t save_selected;
    int backup_mode;
    unsigned int backup_slot;
    int usb_tested;
    int config_result;
    int backup_result;
    int restore_result;
    const char *tool_message;
    int ready;
    int demo;
    int review;
    int launching;
    int loading;
    int view_valid;
    int cover_attempted;
    int covers_enabled;
    int skip_usb_art;
    int notice_active;
    unsigned long selection_since;
    unsigned long notice_since;
    size_t read_count;
    size_t queue_selected;
    char status[80];
    char notice[80];
    const char *message;
} Menu;

static void refresh_usb_art(Menu *menu);
static void stop_preview(Menu *menu);
static void update_preview(Menu *menu);
static void draw_tools(Menu *menu);
static void tools_input(Menu *menu, u32 pressed);
static int offer_group(Menu *menu, const MenuEntry *entry);
static void activate(Menu *menu);
static int apply_queue(Menu *menu);
static u32 browser_shortcuts(Menu *menu, u32 buttons, u32 pressed, u32 released);
static void open_favorites(Menu *menu);
static void jump_letter(Menu *menu, int letter);
static int alphabet_input(Menu *menu, u32 pressed);
static void draw_cel(Menu *menu, CCB *cel);
static int save_root_operation(Menu *menu, int action, const char *path, SaveArchive *archive);
static void load_position(Menu *menu);
static void remember_position(Menu *menu, int force);
static void record_launch(Menu *menu, const char *path);
static int config_dirty(const Menu *menu);
static size_t menu_rows(const Menu *menu)
{
    if (!menu->review && menu->metadata.count) return menu->config.large_titles ? 4 : 5;
    return menu->config.large_titles ? 6 : ROWS;
}

/* Use quoted SDK headers: Norcroft's built-in <stdarg.h> uses a different
 * va_list layout from Portfolio vsprintf. With built-in headers, the formatter
 * reads a stack address as the first value. A host preview cannot catch this.
 * Norcroft also rewrites sprintf to an unavailable private runtime symbol.
 * All callers use fixed formats bounded below their destination buffers. */
static void format(char *buffer, const char *pattern, ...)
{
    va_list args;
    va_start(args, pattern);
#ifdef OH3DO_SDK
    {
        /* Fail compilation if built-in headers accidentally return. */
        char *portfolio_args = args;
        vsprintf(buffer, pattern, portfolio_args);
    }
#else
    vsprintf(buffer, pattern, args);
#endif
    va_end(args);
}

static unsigned long now_ms(Menu *menu)
{
    return (unsigned long)GetMSecTime(menu->clock);
}

static void stop_preview(Menu *menu)
{
    preview_audio_close(&menu->preview_audio);
    if (menu->preview_cel) UnloadCel(menu->preview_cel);
    menu->preview_cel = NULL;
    usb_preview_free(&menu->preview);
}

static void selection_changed(Menu *menu)
{
    stop_preview(menu); menu->preview_attempted = 0;
    menu->selection_since = now_ms(menu);
    menu->position_dirty = 1;
    menu->alphabet_active = 0;
    if (menu->cover) UnloadCel(menu->cover);
    menu->cover = NULL;
    menu->cover_attempted = 0;
}

static void notify(Menu *menu, const char *message)
{
    strncpy(menu->notice, message, sizeof(menu->notice) - 1);
    menu->notice[sizeof(menu->notice) - 1] = 0;
    menu->notice_since = now_ms(menu);
    menu->notice_active = 1;
    menu->message = NULL;
}

static void update_breadcrumb(Menu *menu)
{
    menu_breadcrumb(menu->breadcrumb, menu->depth > 1 ? menu->path[menu->depth - 2] : "",
                    menu->depth ? menu->path[menu->depth - 1] : "", menu->depth);
}

static void rectangle(Menu *menu, int x, int y, int w, int h, Color color)
{
    GrafCon gc;
    Rect rect;
    int margin = menu->config.screen_margin;
    if (x == 0 && y == 0 && w == 320 && h == 240) margin = 0;
    rect.rect_XLeft = margin + x * (320 - 2 * margin) / 320;
    rect.rect_YTop = margin + y * (240 - 2 * margin) / 240;
    rect.rect_XRight = margin + (x + w) * (320 - 2 * margin) / 320 - 1;
    rect.rect_YBottom = margin + (y + h) * (240 - 2 * margin) / 240 - 1;
    if (rect.rect_XRight < rect.rect_XLeft) rect.rect_XRight = rect.rect_XLeft;
    if (rect.rect_YBottom < rect.rect_YTop) rect.rect_YBottom = rect.rect_YTop;
    SetFGPen(&gc, color);
    FillRect(menu->screen.sc_BitmapItems[menu->screen.sc_curScreen], &gc, &rect);
}

static void draw_cel(Menu *menu, CCB *cel)
{
    CCB copy = *cel;
    int margin = menu->config.screen_margin;
    if (margin) {
        copy.ccb_XPos = (margin << 16) + (cel->ccb_XPos / 320) * (320 - 2 * margin);
        copy.ccb_YPos = (margin << 16) + (cel->ccb_YPos / 240) * (240 - 2 * margin);
        copy.ccb_HDX = (cel->ccb_HDX / 320) * (320 - 2 * margin);
        copy.ccb_HDY = (cel->ccb_HDY / 320) * (320 - 2 * margin);
        copy.ccb_VDX = (cel->ccb_VDX / 240) * (240 - 2 * margin);
        copy.ccb_VDY = (cel->ccb_VDY / 240) * (240 - 2 * margin);
    }
    copy.ccb_Flags |= CCB_LAST;
    DrawCels(menu->screen.sc_BitmapItems[menu->screen.sc_curScreen], &copy);
}
static void text(Menu *menu,int x,int y,const char *string,Color color)
{
    UiText *cached;
    char safe[97];
    size_t i;
    int style=menu->large_drawing ? UI_FONT_LARGE : menu->text_style;
    int margin=menu->config.screen_margin;
    int px=margin+x*(320-2*margin)/320;
    int py=margin+y*(240-2*margin)/240;
    int max_width=(308-x)*(320-2*margin)/320;
    Color background=BG;
    if (menu->text_background_set) background=menu->text_background;
    else if (y<52) background=GECKO ? MakeRGB15(8-(y/24)/2,2,13-(y/24)/2) : (menu->config.theme == 1 ? MakeRGB15(1,1,1) :
        (menu->config.theme == 2 ? MakeRGB15(4,3,9-(y/24)/3) : MakeRGB15(2,4,9-(y/24)/2)));
    else if (y<186) background=PANEL;
    if (max_width>UI_FONT_WIDTH) max_width=UI_FONT_WIDTH;
    if (max_width<=0 || !*string) return;
    for (i=0;i<96 && string[i];++i) {
        unsigned char c=(unsigned char)string[i]; safe[i]=c>=32 && c<=126 ? (char)c : '?';
    }
    safe[i]=0;
#ifdef UI_PREVIEW_TEXT
    UI_PREVIEW_TEXT(safe);
#endif
    cached=&menu->text_cache[menu->text_cursor<UI_TEXT_CACHE ? menu->text_cursor++ : UI_TEXT_CACHE-1];
    if (!cached->cel && !menu->text_failed) {
        cached->cel=CreateCel(UI_FONT_WIDTH,UI_FONT_HEIGHT,4,0,NULL);
        if (!cached->cel) menu->text_failed=1;
        else memset(cached->cel->ccb_PLUTPtr,0,64);
    }
    if (cached->cel) {
        CCB *cel=cached->cel;
        int width;
        if (cached->style != style || strcmp(cached->value,safe) || cached->width != max_width) {
            ui_font_render((unsigned char *)cel->ccb_SourcePtr,safe,style,max_width);
            strcpy(cached->value,safe); cached->style=style; cached->width=max_width;
        }
        width=ui_font_width(safe,style); if (width>max_width) width=max_width;
        if (!width) return;
        if (!cached->palette_valid || cached->ink!=color || cached->background!=background) {
            ui_font_palette((unsigned short *)cel->ccb_PLUTPtr,color,background);
            cached->ink=color; cached->background=background; cached->palette_valid=1;
        }
        cel->ccb_PRE0=(cel->ccb_PRE0 & ~0x0000ffc0UL)|((ui_font_height(style)-1)<<6);
        cel->ccb_PRE1=(cel->ccb_PRE1 & ~0x000007ffUL)|(width-1);
        cel->ccb_Width=width; cel->ccb_Height=ui_font_height(style);
        /* Keep text at native size and integer pixels, including with overscan margins. */
        cel->ccb_XPos=px<<16; cel->ccb_YPos=py<<16;
        cel->ccb_HDX=1<<20; cel->ccb_HDY=0; cel->ccb_VDX=0; cel->ccb_VDY=1<<16;
        cel->ccb_Flags |= CCB_LAST;
        DrawCels(menu->screen.sc_BitmapItems[menu->screen.sc_curScreen],cel);
    } else {
        GrafCon gc;
        char fallback[37];
        for (i=0;i<36 && i<(size_t)(max_width/8) && safe[i];++i) fallback[i]=safe[i];
        fallback[i]=0; SetFGPen(&gc,color); MoveTo(&gc,px,py);
        DrawText8(&gc,menu->screen.sc_BitmapItems[menu->screen.sc_curScreen],(const u8 *)fallback);
    }
}
static void label(Menu *menu,int x,int y,const char *value,Color color,int style)
{
    int previous=menu->text_style;
    menu->text_style=style; text(menu,x,y,value,color); menu->text_style=previous;
}
static void label_on(Menu *menu,int x,int y,const char *value,Color ink,int style,Color background)
{
    int previous=menu->text_background_set;
    Color old=menu->text_background;
    menu->text_background_set=1; menu->text_background=background;
    label(menu,x,y,value,ink,style);
    menu->text_background_set=previous; menu->text_background=old;
}
static void rounded(Menu *menu,int x,int y,int w,int h,Color color)
{
    rectangle(menu,x+3,y,w-6,h,color);
    rectangle(menu,x+1,y+1,w-2,h-2,color);
    rectangle(menu,x,y+3,w,h-6,color);
}
static void game_label(Menu *menu,int x,int y,int room,const char *name,int directory,
                       int style,Color ink,Color background,int selected)
{
    const char *kind=menu_image_format(name,directory);
    int scale=320-2*menu->config.screen_margin;
    char title[65];
    size_t length=menu_title_length(name,directory), visible, offset=0;
    if (kind) {
        int badge_width=(ui_font_width(kind,UI_FONT_SMALL)*320+scale-1)/scale;
        int badge_y=y+(ui_font_height(style)-ui_font_height(UI_FONT_SMALL))/2;
        label_on(menu,x+room-badge_width,badge_y,kind,ink,UI_FONT_SMALL,background);
        room-=badge_width+8;
    }
    room=room*scale/320;
    visible=ui_font_fit(name,length,room,style);
    if (visible>64) visible=64;
    if (selected) offset=menu_scroll_offset(length,visible,now_ms(menu)-menu->selection_since);
    visible=ui_font_fit(name+offset,length-offset,room,style);
    if (visible>64) visible=64;
    memcpy(title,name+offset,visible); title[visible]=0;
    label_on(menu,x,y,title,ink,style,background);
}
static void button(Menu *menu,int x,int y,const char *key,const char *action,Color color)
{
    int width=(ui_font_width(key,UI_FONT_SMALL)+8)*320/(320-2*menu->config.screen_margin);
    if (width<15) width=15;
    rounded(menu,x,y,width,14,color);
    label_on(menu,x+4,y+1,key,MakeRGB15(1,2,4),UI_FONT_SMALL,color);
    label(menu,x+width+4,y+1,action,TEXT,UI_FONT_SMALL);
}
static void backdrop(Menu *menu)
{
    int i;
    rectangle(menu,0,0,320,240,BG);
    for (i=0;i<8;++i) {
        Color shade=GECKO ? MakeRGB15(8-i/2,2,13-i/2) : (menu->config.theme == 1 ? MakeRGB15(1,1,1) :
            (menu->config.theme == 2 ? MakeRGB15(4,3,9-i/3) : MakeRGB15(2,4,9-i/2)));
        rectangle(menu,0,i*24,320,24,shade);
    }
    rectangle(menu,0,210,320,30,BG);
    if (GECKO) {
        /* Casing details stay outside text and artwork: no scanline overlay. */
        for (i=0;i<5;++i) {
            rectangle(menu,3,61+i*26,5,12,LINE);
            rectangle(menu,312,72+i*26,5,12,LINE);
        }
        rectangle(menu,14,208,292,1,LINE);
        rectangle(menu,14,208,48,1,ACCENT);
        rectangle(menu,64,208,18,1,TV_ORANGE);
    }
}


static int storage_root(const Menu *menu)
{
    return menu->depth == 0 || (menu->depth == 1 && !(menu->path_ids[0] & 0xffffffUL));
}
static int hidden_entry(const Menu *menu, const MenuEntry *entry)
{
    /* Zero local IDs denote volume selectors, not support directories. */
    if (entry->is_directory && !(entry->toc_id & 0xffffffUL)) return 0;
    return menu_support_entry(entry->name, entry->is_directory, storage_root(menu));
}
static const MenuEntry *browser_entry(const Menu *menu, size_t index)
{
    size_t i;
    for (i = 0; i < menu->listing.count; ++i) {
        const MenuEntry *entry = menu->listing.entries[i];
        if (hidden_entry(menu, entry) || !menu_name_matches(entry->name,menu->filter)) continue;
        if (!index--) return entry;
    }
    return NULL;
}
static size_t row_count(const Menu *menu)
{
    size_t i, count = menu->listing.has_parent ? 1 : 0;
    if (menu->review) return menu->playlist.count;
    for (i = 0; i < menu->listing.count; ++i)
        if (!hidden_entry(menu, menu->listing.entries[i]) && menu_name_matches(menu->listing.entries[i]->name,menu->filter)) ++count;
    return count;
}

static void folder_icon(Menu *menu, int x, int y, Color color)
{
    rectangle(menu, x + 1, y, 4, 2, color);
    rectangle(menu, x, y + 2, 9, 5, color);
    rectangle(menu, x + 1, y + 3, 7, 3, PANEL);
}

/* One pass over visible entries; no allocation or ODE commands while drawing. */
static void alphabet_available(const Menu *menu, unsigned char *available)
{
    size_t i;
    memset(available,0,27);
    for (i = 0; i < menu->listing.count; ++i) {
        const MenuEntry *entry = menu->listing.entries[i];
        if (!hidden_entry(menu,entry) && menu_name_matches(entry->name,menu->filter))
            available[menu_initial(entry->name)] = 1;
    }
}
static void draw_alphabet(Menu *menu)
{
    unsigned char available[27];
    int i, current=menu->jump_letter;
    int scale=320-2*menu->config.screen_margin, x, total=0;
    alphabet_available(menu,available);
    if (!menu->alphabet_active) {
        const MenuEntry *entry=NULL;
        if (!(menu->listing.has_parent && !menu->selected))
            entry=browser_entry(menu,menu->selected-(menu->listing.has_parent ? 1 : 0));
        current=entry ? menu_initial(entry->name) : -1;
    }
    for (i=0;i<27;++i) {
        char letter[2]; letter[0]=i ? (char)('A'+i-1) : '#'; letter[1]=0;
        total+=(ui_font_width(letter,UI_FONT_SMALL)*320+scale-1)/scale+2;
    }
    x=14+(292-total)/2;
    if (menu->alphabet_active) rounded(menu,14,189,292,17,PANEL);
    for (i=0;i<27;++i) {
        char letter[2];
        int cell;
        Color ink=available[i] ? MUTED : DIM;
        letter[0]=i ? (char)('A'+i-1) : '#'; letter[1]=0;
        cell=(ui_font_width(letter,UI_FONT_SMALL)*320+scale-1)/scale+2;
        if (i==current) {
            rounded(menu,x-1,190,cell,14,menu->alphabet_active ? ACCENT : SELECT);
            ink=menu->alphabet_active ? MakeRGB15(1,2,4) : TEXT;
        }
        label_on(menu,x,191,letter,ink,UI_FONT_SMALL,
            i==current ? (menu->alphabet_active ? ACCENT : SELECT) : (menu->alphabet_active ? PANEL : BG));
        x+=cell;
    }
}
static void brand(Menu *menu,const char *title)
{
    if (GECKO) {
        rounded(menu,10,3,100,43,LINE);
        rounded(menu,13,6,94,36,SELECT);
        rectangle(menu,118,5,39,2,TV_ORANGE);
        rectangle(menu,159,5,17,2,ACCENT);
    }
    if (menu->logo) {
        CCB logo=*menu->logo;
        logo.ccb_XPos=(GECKO ? 16 : 12)<<16;
        logo.ccb_YPos=(GECKO ? 8 : 5)<<16;
        logo.ccb_HDX=(1<<20)*2/3; logo.ccb_VDY=(1<<16)*2/3;
        draw_cel(menu,&logo);
    } else if (GECKO) label_on(menu,20,15,"OH MY 3DO",ACCENT,UI_FONT_NORMAL,SELECT);
    else label(menu,14,15,"Oh My 3DO",TEXT,UI_FONT_NORMAL);
    label(menu,118,8,title,TEXT,UI_FONT_HEADING);
}
static void media_cel(Menu *menu,CCB *source)
{
    CCB copy=*source;
    copy.ccb_XPos=19<<16; copy.ccb_YPos=59<<16;
    copy.ccb_HDX=(1<<20)*84/source->ccb_Width; copy.ccb_VDY=(1<<16)*105/source->ccb_Height;
    draw_cel(menu,&copy);
}
static void media_placeholder(Menu *menu,int folder)
{
    int y=91;
    if (GECKO) {
        rounded(menu,29,87,59,46,LINE);
        rounded(menu,33,91,48,34,SELECT);
        if (folder) {
            rounded(menu,40,100,16,6,ACCENT);
            rounded(menu,39,104,33,15,ACCENT);
            rectangle(menu,43,109,25,6,SELECT);
        } else {
            /* Original little lizard silhouette: no game sprites or logos. */
            rounded(menu,43,98,23,23,ACCENT);
            rounded(menu,57,105,20,12,ACCENT);
            rectangle(menu,45,102,10,5,BG);
            rectangle(menu,58,102,10,5,BG);
            rectangle(menu,54,103,5,2,BG);
            rectangle(menu,62,112,12,2,SELECT);
        }
        rectangle(menu,82,96,3,3,TV_ORANGE);
        rectangle(menu,82,103,3,3,ACCENT);
        rectangle(menu,39,133,11,3,LINE);
        rectangle(menu,68,133,11,3,LINE);
        label(menu,folder ? 29 : 34,142,folder ? "Browse folder" : "No artwork",MUTED,UI_FONT_SMALL);
    } else if (folder) {
        rounded(menu,35,y+4,23,10,ACCENT);
        rounded(menu,32,y+11,49,32,ACCENT);
        rectangle(menu,36,y+17,41,20,SELECT);
        label(menu,29,142,"Browse folder",MUTED,UI_FONT_SMALL);
    } else {
        rounded(menu,34,y,45,44,LINE);
        rounded(menu,38,y+4,37,36,SELECT);
        rounded(menu,52,y+18,9,9,PANEL);
        label(menu,34,142,"No artwork",MUTED,UI_FONT_SMALL);
    }
}


static void draw(Menu *menu)
{
    char line[96];
    size_t total=row_count(menu), selected=menu->review ? menu->queue_selected : menu->selected;
    size_t rows=menu_rows(menu), start=(selected/rows)*rows, i;
    unsigned long now=now_ms(menu);
    const MenuEntry *chosen=NULL;
    const char *message=menu->message;
    int left=menu->review ? 14 : 118, width=menu->review ? 292 : 190;
    int row_height=menu->config.large_titles ? 21 : 18;
    if (menu->booting) {
        menu->screen.sc_curScreen^=1;
        if (menu->splash) DrawCels(menu->screen.sc_BitmapItems[menu->screen.sc_curScreen],menu->splash);
        else rectangle(menu,0,0,320,240,BG);
        DisplayScreen(menu->screen.sc_Screens[menu->screen.sc_curScreen],0);
        return;
    }
    if (menu->tools) { draw_tools(menu); return; }
    menu->text_cursor=0; menu->text_style=UI_FONT_NORMAL;
    if (menu->notice_active && now-menu->notice_since>=2500UL) menu->notice_active=0;
    if (!message && menu->notice_active) message=menu->notice;
    if (!menu->review && total && !(menu->listing.has_parent && !selected))
        chosen=browser_entry(menu,selected-(menu->listing.has_parent ? 1 : 0));
    menu->screen.sc_curScreen^=1;
    backdrop(menu); brand(menu,menu->review ? "Disc queue" : "Library");
    if (*menu->filter && !menu->review) format(line,"Search: %s",menu->filter);
    else if (menu->review) strcpy(line,"Play order / first to last");
    else strcpy(line,menu->depth ? menu->breadcrumb : "All games");
    line[ui_font_fit(line,strlen(line),130*(320-2*menu->config.screen_margin)/320,UI_FONT_SMALL)]=0;
    label(menu,118,32,line,MUTED,UI_FONT_SMALL);
    rounded(menu,246,9,60,21,menu->playlist.count ? ACCENT : LINE);
    format(line,"%lu discs",(unsigned long)menu->playlist.count);
    label_on(menu,252,14,line,menu->playlist.count ? MakeRGB15(1,2,4) : MUTED,UI_FONT_SMALL,menu->playlist.count ? ACCENT : LINE);
    rounded(menu,left,54,width,132,BG);
    if (GECKO) rounded(menu,left-2,50,width+4,136,LINE);
    rounded(menu,left,52,width,132,PANEL);
    if (menu->view_valid || menu->review) {
        for (i=start;i<total && i<start+rows;++i) {
            const char *name;
            int directory, y=55+(int)(i-start)*row_height;
            int style=menu->config.large_titles ? UI_FONT_LARGE : UI_FONT_NORMAL;
            int title_x=left+23, room=width-34;
            Color ink=i==selected && !menu->alphabet_active ? MakeRGB15(1,2,4) : (i==selected ? TEXT : MUTED);
            if (menu->review) { name=menu->playlist.paths[i]; if (!strncmp(name,"/Storage/",9)) name+=9; directory=0; }
            else if (menu->listing.has_parent && !i) { name="Back to parent folder"; directory=1; }
            else { const MenuEntry *entry=browser_entry(menu,i-(menu->listing.has_parent ? 1 : 0)); name=entry->name; directory=entry->is_directory; }
            if (i==selected) rounded(menu,left+3,y,width-6,row_height-1,menu->alphabet_active ? SELECT : ACCENT);
            if (menu->review) {
                format(line,"%lu",(unsigned long)i+1);
                label_on(menu,left+9,y+3,line,ink,UI_FONT_SMALL,i==selected ? (menu->alphabet_active ? SELECT : ACCENT) : PANEL);
                title_x=left+28; room=width-40;
            } else if (directory) folder_icon(menu,left+9,y+5,ink);
            else if (GECKO && i==selected) {
                rectangle(menu,left+12,y+8,4,4,ink);
                rectangle(menu,left+8,y+5,2,2,ink);
                rectangle(menu,left+12,y+3,2,2,ink);
                rectangle(menu,left+17,y+5,2,2,ink);
                rectangle(menu,left+18,y+10,2,2,ink);
            } else if (GECKO) {
                rectangle(menu,left+9,y+5,10,7,ink);
                rectangle(menu,left+11,y+7,6,3,PANEL);
                rectangle(menu,left+11,y+13,6,1,ink);
            } else {
                rounded(menu,left+10,y+5,7,7,ink);
                rectangle(menu,left+13,y+8,1,1,i==selected ? ACCENT : PANEL);
            }
            game_label(menu,title_x,y+(menu->config.large_titles ? 2 : 3),room,name,directory,
                style,ink,i==selected ? (menu->alphabet_active ? SELECT : ACCENT) : PANEL,i==selected);
        }
        if (!total) {
            label(menu,left+12,94,menu->review ? "No discs queued" : "Nothing here yet",TEXT,UI_FONT_NORMAL);
            label(menu,left+12,114,*menu->filter ? "Try another name" : "Choose another folder",MUTED,UI_FONT_SMALL);
        }
    }
    if (!menu->review) {
        if (GECKO) rounded(menu,12,50,98,136,LINE);
        rounded(menu,14,54,94,132,BG); rounded(menu,14,52,94,132,PANEL);
        if (menu->preview_cel && menu->preview.pixels) {
            unsigned long elapsed=now-menu->preview_since;
            unsigned long tick=(elapsed/1000)*menu->preview.fps+((elapsed%1000)*menu->preview.fps)/1000;
            unsigned int frame;
            if (tick>=menu->preview.frames) {
                /* Restart both timelines at the same VBL; audio never drifts across loops. */
                menu->preview_since=now; tick=0;
                if (menu->preview_audio.status==1) preview_audio_restart(&menu->preview_audio);
            }
            frame=(unsigned int)tick;
            if (frame!=menu->preview_frame) {
                if (usb_preview_frame(&menu->preview,frame,(unsigned char *)menu->preview_cel->ccb_SourcePtr)<0) stop_preview(menu);
                else menu->preview_frame=frame;
            }
            if (menu->preview_cel) media_cel(menu,menu->preview_cel);
        } else if (menu->cover) media_cel(menu,menu->cover);
        else media_placeholder(menu,chosen ? chosen->is_directory : (menu->listing.has_parent && !selected));
        label(menu,24,168,GECKO ? (menu->preview_cel ? "ON AIR" : "TV LIBRARY") : (menu->preview_cel ? "Playing preview" : (chosen && chosen->is_directory ? "Folder" : "Game library")),MUTED,UI_FONT_SMALL);
        if (GECKO) {
            rectangle(menu,22,180,26,1,LINE);
            rectangle(menu,51,180,26,1,LINE);
            rectangle(menu,96,177,4,4,menu->preview_cel ? ACCENT : TV_ORANGE);
        }
        draw_alphabet(menu);
        if (menu->metadata.count) {
            static char path[9217];
            const LibraryGameInfo *info=NULL;
            unsigned int part, first=menu->depth && !(menu->path_ids[0]&0xffffffUL) ? 1 : 0;
            size_t used=0;
            path[0]=0;
            if (chosen && !chosen->is_directory) {
                for (part=first;part<menu->depth;++part) {
                    size_t length=strlen(menu->path[part]);
                    if (used+length+1>=sizeof(path)) break;
                    memcpy(path+used,menu->path[part],length); used+=length; path[used++]='/';
                }
                if (part==menu->depth && used+strlen(chosen->name)<sizeof(path)) {
                    strcpy(path+used,chosen->name);
                    info=library_metadata_find(&menu->metadata,path);
                }
            }
            rectangle(menu,124,150,178,1,LINE);
            if (info) {
                const char *values[3];
                unsigned int field;
                values[0]=info->genre; values[1]=info->publisher; values[2]=info->released;
                for (field=0;field<3;++field) {
                    const char *prefix=field==0 ? "Genre: " : (field==1 ? "Publisher: " : "Released: ");
                    format(line,"%s%.80s",prefix,*values[field] ? values[field] : "Unknown");
                    line[ui_font_fit(line,strlen(line),178*(320-2*menu->config.screen_margin)/320,UI_FONT_SMALL)]=0;
                    label(menu,124,154+(int)field*10,line,MUTED,UI_FONT_SMALL);
                }
            } else label(menu,124,163,chosen && !chosen->is_directory ? "No game information" : "Select a game for details",MUTED,UI_FONT_SMALL);
        }
    } else label(menu,18,191,"Advance discs when the game asks",MUTED,UI_FONT_SMALL);
    if (total) {
        int bar=menu->review ? 190 : 117;
        int fill=(int)(bar*(selected/rows+1)/((total+rows-1)/rows));
        int bar_y=!menu->review && menu->metadata.count ? 146 : 184;
        rectangle(menu,left+4,bar_y,bar,2,LINE); if (fill) rectangle(menu,left+4,bar_y,fill,2,ACCENT);
        format(line,"%lu / %lu",(unsigned long)(selected+1),(unsigned long)total);
        label(menu,304-ui_font_width(line,UI_FONT_SMALL),33,line,MUTED,UI_FONT_SMALL);
    }
    if (menu->loading) { strcpy(line,"Reading folder..."); message=line; }
    if (menu->alphabet_active && !menu->notice_active) message="Choose a letter. A to jump, B to cancel.";
    if (message) label(menu,16,213,message,AMBER,UI_FONT_NORMAL);
    else {
        button(menu,14,212,"A",menu->review ? "Play" : "Open",ACCENT);
        button(menu,77,212,"B","Back",MUTED);
        button(menu,140,212,"C",menu->review ? "Move" : "Queue",MUTED);
        button(menu,223,212,"Start","Discs",MUTED);
    }
    if (!menu->ready && menu->bus.last_error<0) { format(line,"Connection error: %08lx",(unsigned long)menu->bus.last_error); label(menu,16,229,line,MUTED,UI_FONT_SMALL); }
    else {
        label(menu,16,229,menu->review ? "C + X  Clear queue" : "C + Up  Letters",MUTED,UI_FONT_SMALL);
        label(menu,145,229,"L + R  Tools",MUTED,UI_FONT_SMALL);
        if (menu->config.folder_workaround || config_dirty(menu))
            label(menu,244,229,config_dirty(menu) ? "Unsaved" : "Compat on",AMBER,UI_FONT_SMALL);
    }
    DisplayScreen(menu->screen.sc_Screens[menu->screen.sc_curScreen],0);
}

static void progress(void *context, size_t count)
{
    Menu *menu = (Menu *)context;
    menu->read_count = count;
    draw(menu);
}

static void load_directory(Menu *menu, size_t selected)
{
    OdeListing next;
    menu->filter[0] = 0;
    ode_listing_init(&next);
    menu->ready = 0;
    menu->loading = 1;
    menu->read_count = 0;
    menu->notice_active = 0;
    draw(menu);
    if (ode_read_directory(&menu->ode, &next) == 0) {
        ode_listing_free(&menu->listing);
        menu->listing = next;
        menu->ready = menu->view_valid = 1;
        menu->selected = selected;
        menu->message = NULL;
        if (menu->selected >= row_count(menu)) menu->selected = 0;
    } else {
        menu->view_valid = 0;
        menu->message = menu->bus.timed_out ? "ODE timeout. Restart console." : menu->ode.error;
    }
    menu->loading = 0;
    update_breadcrumb(menu);
    selection_changed(menu);
    if (menu->ready) refresh_usb_art(menu);
}

static void refresh_usb_art(Menu *menu)
{
    unsigned int root = menu->depth && !(menu->path_ids[0] & 0xffffffUL) ? 1 : 0;
    size_t i;
    const char *message;
    if (menu->demo || menu->depth != root) return;
    if (!menu->depth) {
        for (i = 0; i < menu->listing.count; ++i)
            if (!(menu->listing.entries[i]->toc_id & 0xffffffUL)) {
                usb_art_index_free(&menu->usb_art);
                usb_art_index_free(&menu->usb_previews);
                library_metadata_free(&menu->metadata);
                menu->covers_enabled = 0;
                return; /* virtual device list is not a filesystem root */
            }
    }
    usb_art_index_free(&menu->usb_art);
    usb_art_index_free(&menu->usb_previews);
    library_metadata_free(&menu->metadata);
    menu->covers_enabled = 0;
    if (menu->config_result != 0 || strcmp(menu->ode.identity,"LOCODE")) return;
    message = menu->message;
    menu->message = "Reading cover index...";
    draw(menu);
    if (menu->config.usb_covers) usb_art_index_load(&menu->ode,ode_bus_read_buffer,&menu->usb_art);
    if (menu->config.video_previews && !menu->bus.timed_out && !menu->ode.navigation_failed)
        usb_media_index_load(&menu->ode,ode_bus_read_buffer,&menu->usb_previews,"previews");
    if (!menu->bus.timed_out && !menu->ode.navigation_failed)
        library_metadata_load(&menu->ode,ode_bus_read_buffer,&menu->metadata);
    menu->message = message;
    menu->covers_enabled = menu->usb_art.count != 0;
    if (menu->bus.timed_out || menu->ode.navigation_failed) {
        menu->ode.navigation_failed = 1;
        menu->ready = menu->view_valid = 0;
        menu->message = "Cover I/O failed. Restart console.";
    }
}

static int directory_path(Menu *menu, char *buffer, size_t capacity)
{
    unsigned int i;
    size_t used = 8;
    strcpy(buffer, "/Storage");
    for (i = 0; i < menu->depth; ++i) {
        size_t n = strlen(menu->path[i]);
        if (used + n + 2 > capacity) return -1;
        buffer[used++] = '/';
        memcpy(buffer + used, menu->path[i], n + 1);
        used += n;
    }
    return 0;
}

typedef struct UsbCoverRead {
    Menu *menu;
    const char *filename;
    unsigned char *pixels;
} UsbCoverRead;

static int media_cancel(void *context)
{
    Menu *menu = (Menu *)context;
    ControlPadEventData pad;
    if (GetControlPad(1,0,&pad) >= 0 && pad.cped_ButtonBits) menu->media_cancelled=1;
    return menu->media_cancelled;
}

static int read_usb_cover(void *context)
{
    UsbCoverRead *read = (UsbCoverRead *)context;
    if (!menu_path_compare(read->filename+strlen(read->filename)-4,".png"))
        return usb_png_cover(&read->menu->ode,ode_bus_read_buffer,read->filename,read->pixels,media_cancel,read->menu);
    return usb_legacy_cover(&read->menu->ode,ode_bus_read_buffer,read->filename,read->pixels,media_cancel,read->menu);
}

static void update_cover(Menu *menu)
{
    const MenuEntry *entry;
    const char *file;
    char path[9218];
    const char *lookup;
    if (menu->tools || !menu->covers_enabled || menu->review || !menu->ready || menu->loading ||
        menu->cover_attempted || menu->bus.timed_out || menu->ode.navigation_failed) return;
    if (!row_count(menu) || (menu->listing.has_parent && !menu->selected)) return;
    entry = browser_entry(menu, menu->selected - (menu->listing.has_parent ? 1 : 0));
    if (entry->is_directory || directory_path(menu, path, 8193) < 0) return;
    strcat(path, "/");
    strcat(path, entry->name);
    if (menu->demo) {
        if (now_ms(menu)-menu->selection_since<350UL) return;
        menu->cover_attempted=1;
        file = menu_cover_lookup(path);
        if (!file) return;
        menu->cover = LoadCel((char *)file, MEMTYPE_CEL);
    } else {
        ArtRouteStep route[MAX_DEPTH];
        UsbCoverRead read;
        CCB *cover;
        const char *message;
        unsigned int i;
        int result, width, height;
        size_t bytes;
        unsigned long started, commands, volume;
        lookup = path + 9; /* remove /Storage/ */
        if (menu->depth && !(menu->path_ids[0] & 0xffffffUL))
            lookup += strlen(menu->path[0]) + 1; /* device labels are not USB path components */
        file = menu_cover_find(menu->usb_art.entries, menu->usb_art.count, lookup);
        if (!file) { menu->cover_attempted=1; return; }
        volume=entry->toc_id&0xff000000UL;
        if (!cover_cache_contains(&menu->cover_cache,volume,file) && now_ms(menu)-menu->selection_since<350UL) return;
        menu->cover_attempted=1;
        width=USB_COVER_WIDTH; height=USB_COVER_HEIGHT;
        bytes=(size_t)width*height*2;
        cover = CreateCel(width,height,16,CREATECEL_UNCODED,NULL);
        if (!cover && menu->cover_cache.clock) {
            cover_cache_clear(&menu->cover_cache);
            cover=CreateCel(width,height,16,CREATECEL_UNCODED,NULL);
            if (cover && now_ms(menu)-menu->selection_since<350UL) {
                UnloadCel(cover); menu->cover_attempted=0; return;
            }
        }
        if (!cover) return;
        for (i = 0; i < menu->depth; ++i) {
            route[i].id = menu->path_ids[i];
            route[i].name = menu->path[i];
        }
        if (cover_cache_get(&menu->cover_cache,volume,file,(unsigned char *)cover->ccb_SourcePtr,bytes)) {
            menu->cover_cached=1; menu->cover_load_ms=menu->cover_commands=0;
            menu->cover=cover; goto positioned;
        }
        read.menu = menu;
        read.filename = file;
        read.pixels = (unsigned char *)cover->ccb_SourcePtr;
        message = menu->message;
        menu->message = "Loading cover...";
        draw(menu);
        started = now_ms(menu); commands=menu->bus.commands;
        menu->cover_cached=0;
        menu->media_cancelled = 0;
        result = art_at_root(&menu->ode,route,menu->depth,&menu->listing,
                             menu->config.folder_workaround,read_usb_cover,&read);
        menu->cover_load_ms=now_ms(menu)-started;
        menu->cover_commands=menu->bus.commands-commands;
        menu->selection_since += menu->cover_load_ms;
        menu->message = message;
        if (result < 0 || menu->media_cancelled) UnloadCel(cover);
        else {
            menu->cover = cover;
            cover_cache_put(&menu->cover_cache,volume,file,(const unsigned char *)cover->ccb_SourcePtr,bytes);
        }
        if (result == -2 || menu->bus.timed_out) {
            menu->ode.navigation_failed = 1;
            menu->ready = menu->view_valid = 0;
            menu->message = "Cover I/O failed. Restart console.";
            return;
        }
    }
positioned:
    if (menu->cover) {
        menu->cover->ccb_XPos = 236 << 16;
        menu->cover->ccb_YPos = 86 << 16;
        menu->cover->ccb_Flags |= CCB_LAST | CCB_BGND;
    }
}

typedef struct PreviewRead {
    Menu *menu;
    const char *file;
} PreviewRead;
static int read_usb_preview(void *context)
{
    PreviewRead *read = (PreviewRead *)context;
    return usb_preview_load(&read->menu->ode,ode_bus_read_buffer,read->file,
                            &read->menu->preview,media_cancel,read->menu);
}
static void update_preview(Menu *menu)
{
    const MenuEntry *entry;
    const char *file, *lookup, *message;
    char *path;
    ArtRouteStep route[MAX_DEPTH];
    PreviewRead read;
    unsigned int i;
    int result;
    unsigned long started;
    if (menu->config_result != 0 || strcmp(menu->ode.identity,"LOCODE") || !menu->config.video_previews || menu->preview_attempted || menu->demo || menu->tools ||
        menu->review || menu->alphabet_active || !menu->ready || menu->loading || menu->bus.timed_out ||
        menu->ode.navigation_failed || !menu->usb_previews.count || now_ms(menu)-menu->selection_since < 1200UL) return;
    menu->preview_attempted = 1;
    if (!row_count(menu) || (menu->listing.has_parent && !menu->selected)) return;
    entry = browser_entry(menu,menu->selected-(menu->listing.has_parent ? 1 : 0));
    if (entry->is_directory) return;
    path = (char *)malloc(9218);
    if (!path) return;
    if (directory_path(menu,path,8193) < 0) { free(path); return; }
    strcat(path,"/"); strcat(path,entry->name); lookup = path+9;
    if (menu->depth && !(menu->path_ids[0]&0xffffffUL)) lookup += strlen(menu->path[0])+1;
    file = menu_cover_find(menu->usb_previews.entries,menu->usb_previews.count,lookup);
    free(path);
    if (!file) return;
    for (i=0;i<menu->depth;++i) { route[i].id=menu->path_ids[i]; route[i].name=menu->path[i]; }
    read.menu=menu; read.file=file; message=menu->message;
    menu->message="Loading preview... Press to cancel."; draw(menu);
    menu->media_cancelled=0; started=now_ms(menu);
    result=art_at_root(&menu->ode,route,menu->depth,&menu->listing,
                       menu->config.folder_workaround,read_usb_preview,&read);
    menu->message=message;
    menu->preview_load_ms=now_ms(menu)-started; menu->preview_result=result ? -1 : 1;
    if (!result && menu->media_cancelled) { usb_preview_free(&menu->preview); result=-1; menu->preview_result=-1; }
    if (!result) {
        menu->preview_cel=CreateCel(64,80,16,CREATECEL_UNCODED,NULL);
        if (menu->preview_cel) {
            menu->preview_cel->ccb_XPos=236<<16; menu->preview_cel->ccb_YPos=86<<16;
            menu->preview_cel->ccb_Flags |= CCB_LAST|CCB_BGND;
            usb_preview_frame(&menu->preview,0,(unsigned char *)menu->preview_cel->ccb_SourcePtr);
            if (menu->config.preview_sound && menu->preview.audio)
                preview_audio_start(&menu->preview_audio,menu->preview.audio,menu->preview.audio_frames,menu->preview.audio_rate);
            menu->preview_frame=0; menu->preview_since=now_ms(menu);
        } else { usb_preview_free(&menu->preview); menu->preview_result=-1; }
    }
    if (result == -2 || menu->bus.timed_out || menu->ode.navigation_failed) {
        stop_preview(menu); menu->ode.navigation_failed=1; menu->ready=menu->view_valid=0;
        menu->message="Preview I/O failed. Restart console.";
    }
}

static void show_demo(Menu *menu)
{
    static const MenuEntry examples[] = {
        { 5, 0, "Road Rash.iso" }, { 2, 1, "Multi Disc" },
        { 9, 0, "Disc 10.iso" }, { 3, 1, "Favorites" },
        { 1, 0, "Alone in the Dark.iso" }, { 7, 0, "Disc 2.iso" },
        { 4, 0, "Disc 1.iso" }, { 6, 0, "The Need for Speed.iso" },
        { 8, 0, "Super Street Fighter II Turbo.iso" }
    };
    size_t i;
    if (menu->playlist.count) {
        menu->message = "Start: Review queue before demo";
        return;
    }
    menu->review = 0;
    usb_art_index_free(&menu->usb_art);
    usb_art_index_free(&menu->usb_previews);
    library_metadata_free(&menu->metadata);
    menu->covers_enabled = menu_cover_count != 0;
    ode_listing_free(&menu->listing);
    menu->listing.entries = (const MenuEntry **)malloc(9 * sizeof(MenuEntry *));
    if (!menu->listing.entries) { menu->message = "Not enough memory"; return; }
    for (i = 0; i < 9; ++i) {
        size_t n = strlen(examples[i].name) + 1;
        MenuEntry *entry = (MenuEntry *)malloc(sizeof(*entry) + n);
        if (!entry) {
            ode_listing_free(&menu->listing);
            menu->ready = 0;
            menu->message = "Not enough memory";
            return;
        }
        *entry = examples[i];
        entry->name = (char *)(entry + 1);
        memcpy((void *)entry->name, examples[i].name, n);
        menu->listing.entries[menu->listing.count++] = entry;
    }
    menu_sort(menu->listing.entries, menu->listing.count);
    menu->demo = menu->ready = menu->view_valid = 1;
    menu->depth = 0;
    menu->selected = 0;
    menu->message = NULL;
    update_breadcrumb(menu);
    selection_changed(menu);
}

static void connect_ode(Menu *menu)
{
    int config_result;
    ControlPadEventData pad;
    menu->booting=1;
    library_catalog_free(&menu->catalog);
    ode_playlist_dispose(&menu->playlist);
    menu->review = 0;
    menu->demo = menu->ready = menu->view_valid = 0;
    menu->selected = menu->depth = 0;
    menu->notice_active = 0;
    selection_changed(menu);
    update_breadcrumb(menu);
    menu->usb_tested = menu->backup_result = menu->restore_result = 0;
    menu->position_result = menu->recent_result = menu->diagnostics_result = 0;
    memset(&menu->last_position,0,sizeof(menu->last_position));
    menu_favorites_free(&menu->recent); menu->recent_valid = 0;
    usb_art_index_free(&menu->usb_previews);
    menu->config_result = 3; /* not attempted */
    memset(menu->ode.identity, 0, sizeof(menu->ode.identity));
    memset(menu->ode.revision, 0, sizeof(menu->ode.revision));
    memset(menu->ode.identity_reply, 0, sizeof(menu->ode.identity_reply));
    menu_config_defaults(&menu->config);
    usb_art_index_free(&menu->usb_art);
    cover_cache_clear(&menu->cover_cache);
    menu->covers_enabled = 0;
    menu->skip_usb_art = 0;
    menu->ode.navigation_known = 0;
    menu->ode.navigation_failed = 0;
    menu->ode.support_directory = 0;
    ode_listing_free(&menu->listing);
    menu->message = "Connecting to ODE...";
    draw(menu);
    if (menu->bus.request < 0 && ode_bus_open(&menu->bus) < 0) {
        menu->message = "Cannot open XBUS. C: View demo";
        goto done;
    }
    if (ode_identify(&menu->ode) < 0 || ode_change_directory(&menu->ode, 0) < 0) {
        menu->message = menu->bus.timed_out ? "No ODE reply. C: View demo" : "ODE not available. C: View demo";
        goto done;
    }
    menu->message = "Reading USB config...";
    draw(menu);
    if (GetControlPad(1, 0, &pad) >= 0 && (pad.cped_ButtonBits & ControlX))
        config_result = 2; /* Recovery path: do not send any file commands. */
    else config_result = menu_config_load(&menu->ode, ode_bus_read_buffer, &menu->config);
    menu->config_result = config_result;
    menu->skip_usb_art = config_result != 0 || !menu->config.usb_covers;
    menu->settings_folder_workaround = menu->config.folder_workaround;
    menu->settings_usb_covers = menu->config.usb_covers;
    menu->settings_remember_position = menu->config.remember_position;
    menu->settings_recent_games = menu->config.recent_games;
    menu->settings_video_previews = menu->config.video_previews;
    menu->saved_config = menu->config;
    if (menu->bus.timed_out) {
        menu->message = "Config I/O failed. Restart console.";
        goto done;
    }
    if (ode_playlist_clear(&menu->ode, &menu->playlist) < 0) {
        menu->message = menu->ode.error;
        goto done;
    }
    load_directory(menu, 0);
    if (menu->ready && config_result == 0 && menu->config.remember_position) load_position(menu);
    if (menu->ready && config_result < 0) menu->message = "Config not loaded. Compat OFF.";
    else if (menu->ready && config_result == 1) menu->message = "USB config unsupported. Compat OFF.";
    else if (menu->ready && config_result == 2) menu->message = "Config skipped. Compat OFF.";
done:
    menu->booting=0;
    if (menu->splash) { UnloadCel(menu->splash); menu->splash=NULL; }
    draw(menu);
}

static void go_back(Menu *menu)
{
    unsigned long parent_id;
    size_t selected = 0;
    if (!menu->ready || (!menu->listing.has_parent && !menu->depth) || menu->demo) return;
    parent_id = menu->listing.has_parent ? menu->listing.parent_id : 0xffffffffUL;
    if (ode_change_directory(&menu->ode, parent_id) < 0) {
        menu->ready = 0;
        menu->message = "Folder I/O failed. Restart console.";
        return;
    }
    if (menu->depth) selected = menu->previous_selection[--menu->depth];
    load_directory(menu, selected);
}

static void activate(Menu *menu)
{
    const MenuEntry *entry;
    if (!menu->ready || menu->demo || !row_count(menu)) return;
    if (menu->listing.has_parent && menu->selected == 0) { go_back(menu); return; }
    entry = browser_entry(menu, menu->selected - (menu->listing.has_parent ? 1 : 0));
    if (entry->is_directory) {
        if (menu->depth == MAX_DEPTH) { menu->message = "Maximum folder depth reached"; return; }
        if (ode_enter_directory(&menu->ode, &menu->listing, entry,
                                menu->config.folder_workaround) < 0) {
            if (menu->ode.navigation_failed) {
                menu->ready = 0;
                menu->message = "Folder I/O failed. Restart console.";
            } else menu->message = menu->ode.error;
            return;
        }
        menu->previous_selection[menu->depth] = menu->listing.has_parent ? 1 : 0;
        {
            size_t i;
            for (i = 0; i < menu->listing.count && menu->listing.entries[i] != entry; ++i)
                if (!hidden_entry(menu,menu->listing.entries[i])) ++menu->previous_selection[menu->depth];
        }
        menu->path_ids[menu->depth] = entry->toc_id;
        strcpy(menu->path[menu->depth], entry->name);
        ++menu->depth;
        load_directory(menu, 0);
    } else {
        if (!menu->skip_group && !menu->playlist.count && menu->playlist.synchronized && offer_group(menu,entry)) return;
        menu->skip_group = 0;
        if (menu->playlist.count || !menu->playlist.synchronized) {
            menu->message = "Start: Review or clear disc queue";
            return;
        }
        {
            /* activate also calls the disc-group helper, which owns large
             * path buffers. Keep this buffer off the 32 KiB task stack. */
            char *full = (char *)malloc(9218);
            if (full) {
                if (!directory_path(menu,full,9218) && strlen(full)+strlen(entry->name)+2 <= 9218) {
                    strcat(full,"/"); strcat(full,entry->name); record_launch(menu,full);
                }
                free(full);
            }
            if (!menu->ready || menu->ode.navigation_failed || menu->bus.timed_out) return;
        }
        menu->message = "Starting game...";
        draw(menu);
        menu->playlist.synchronized = 0;
        stop_preview(menu);
        if (ode_launch(&menu->ode, entry) < 0) menu->message = menu->ode.error;
        else {
            menu->launching = 1;
            menu->ready = menu->view_valid = 0;
            menu->message = "Image mounted. Waiting for boot...";
            ode_listing_free(&menu->listing);
        }
    }
}

static void queue_selected_disc(Menu *menu)
{
    char directory[8193], full[9218];
    const MenuEntry *entry;
    if (!menu->ready || !row_count(menu)) return;
    if (menu->listing.has_parent && menu->selected == 0) {
        menu->message = "Select a disc image to queue";
        return;
    }
    entry = browser_entry(menu, menu->selected - (menu->listing.has_parent ? 1 : 0));
    if (directory_path(menu, directory, sizeof(directory)) < 0) {
        menu->message = "Folder path too long for disc queue";
        return;
    }
    if (entry->is_directory) { menu->message = "Select a disc image to queue"; return; }
    strcpy(full,directory); strcat(full,"/"); strcat(full,entry->name);
    if (ode_playlist_append_local(&menu->ode, &menu->playlist, full) < 0) {
        menu->message = menu->ode.error;
        return;
    }
    format(menu->status, "Queued disc %lu. Start: Review", (unsigned long)menu->playlist.count);
    notify(menu, menu->status);
}

static void review_queue(Menu *menu)
{
    menu->review = 1;
    menu->queue_selected = 0;
    menu->message = menu->playlist.synchronized ? NULL : "Queue uncertain. X:Clear to retry";
    menu->notice_active = 0;
    selection_changed(menu);
}

static void start_queue(Menu *menu)
{
    if (menu->playlist.dirty && apply_queue(menu) < 0) { menu->message = menu->ode.error; return; }
    if (menu->playlist.count) record_launch(menu,menu->playlist.paths[0]);
    if (!menu->ready || menu->ode.navigation_failed || menu->bus.timed_out) return;
    if (menu->playlist.count && menu->playlist.synchronized) stop_preview(menu);
    menu->message="Starting first queued disc..."; draw(menu);
    if (ode_playlist_start(&menu->ode, &menu->playlist) < 0) {
        menu->message = menu->ode.error;
        return;
    }
    menu->review = menu->ready = menu->view_valid = 0;
    menu->launching = 1;
    menu->message = "Image mounted. Waiting for boot...";
    ode_listing_free(&menu->listing);
}

/* Tools stay available after a transport failure; opening diagnostics sends no
 * commands, and no action is allowed to retry a poisoned ODE connection. */
static void current_route(Menu *menu, ArtRouteStep *route)
{
    unsigned int i;
    for (i = 0; i < menu->depth; ++i) { route[i].id = menu->path_ids[i]; route[i].name = menu->path[i]; }
}
static int queue_file_action(void *context, const MenuEntry *entry, const char *directory)
{
    Menu *menu = (Menu *)context;
    return ode_playlist_add(&menu->ode,&menu->group,entry,directory);
}
static int apply_queue(Menu *menu)
{
    ArtRouteStep route[MAX_DEPTH];
    const MenuEntry *entries[ODE_MAX_DISCS];
    OdeListing fresh;
    char *directory;
    size_t i;
    int local=0, result=-1;
    current_route(menu,route); ode_listing_init(&fresh);
    if (menu->bus.timed_out || menu->ode.navigation_failed || !menu->playlist.synchronized) return -1;
    directory=(char *)malloc(8193);
    if (!directory) { menu->ode.error="Not enough queue memory"; return -1; }
    menu->message="Checking queued game paths..."; draw(menu);
    if (!directory_path(menu,directory,8193))
        local=library_current_files(&menu->listing,directory,menu->playlist.paths,menu->playlist.count,entries);
    if (local<0) { menu->ode.error="Queued path is missing"; goto checked; }
    if (local) {
        /* Re-read once for fresh TOC IDs. Same-folder sets need no root trips. */
        if (ode_read_directory(&menu->ode,&fresh)<0) goto checked;
        if (library_current_files(&fresh,directory,menu->playlist.paths,menu->playlist.count,entries)!=1) {
            menu->ode.error="Queued path is missing"; goto checked;
        }
    } else {
        for (i=0;i<menu->playlist.count;++i)
            if (library_visit(&menu->ode,route,menu->depth,&menu->listing,menu->config.folder_workaround,
                menu->playlist.paths[i],NULL,NULL)<0) goto checked;
    }
    ode_playlist_dispose(&menu->group);
    menu->message="Applying disc queue..."; draw(menu);
    if (ode_playlist_clear(&menu->ode,&menu->group)<0) goto uncertain;
    for (i=0;i<menu->playlist.count;++i) {
        if (local) {
            if (ode_playlist_add(&menu->ode,&menu->group,entries[i],directory)<0) goto uncertain;
        } else if (library_visit(&menu->ode,route,menu->depth,&menu->listing,menu->config.folder_workaround,
            menu->playlist.paths[i],queue_file_action,menu)<0) goto uncertain;
    }
    menu->playlist.dirty=0; result=0; goto applied;
uncertain:
    menu->playlist.synchronized=0;
applied:
    ode_playlist_dispose(&menu->group);
checked:
    ode_listing_free(&fresh); free(directory); return result;
}
static int offer_group(Menu *menu, const MenuEntry *entry)
{
    const MenuEntry *entries[ODE_MAX_DISCS];
    char directory[8193], path[9218];
    size_t count = menu_disc_group(&menu->listing,entry,entries,ODE_MAX_DISCS), i;
    if (!count || directory_path(menu,directory,sizeof(directory)) < 0) return 0;
    ode_playlist_dispose(&menu->group); menu->group.synchronized = 1;
    for (i = 0; i < count; ++i) {
        strcpy(path,directory); strcat(path,"/"); strcat(path,entries[i]->name);
        if (ode_playlist_append_local(&menu->ode,&menu->group,path) < 0) { ode_playlist_dispose(&menu->group); return 0; }
    }
    menu->tools = TOOL_GROUP; menu->tool_selected = 0; menu->tool_message = NULL;
    return 1;
}
static void jump_letter(Menu *menu, int letter)
{
    size_t i, count = row_count(menu), parent = menu->listing.has_parent ? 1 : 0;
    if (menu->review || !menu->ready) return;
    for (i = parent; i < count; ++i) {
        const MenuEntry *entry = browser_entry(menu,i-parent);
        if (menu_initial(entry->name) == letter) {
            menu->selected = i; selection_changed(menu); menu->message = NULL; return;
        }
    }
    menu->message = "No entries for that letter.";
}
static void focus_alphabet(Menu *menu)
{
    const MenuEntry *entry = NULL;
    if (!(menu->listing.has_parent && !menu->selected))
        entry = browser_entry(menu,menu->selected-(menu->listing.has_parent ? 1 : 0));
    menu->jump_letter = entry ? menu_initial(entry->name) : 1;
    menu->alphabet_active = 1; menu->c_pending = 0;
    menu->notice_active = 0; menu->message = NULL;
}

/* The alphabet is a browser focus target, never a tool or a queued disc. */
static int alphabet_input(Menu *menu, u32 pressed)
{
    size_t count;
    if (menu->review || menu->tools || !menu->ready || menu->loading || menu->ode.navigation_failed) {
        menu->alphabet_active = 0;
        return 0;
    }
    count = row_count(menu);
    if (!menu->alphabet_active) {
        if (!count || !(((pressed & ControlUp) && !menu->selected) ||
                       ((pressed & ControlDown) && menu->selected+1 == count))) return 0;
        focus_alphabet(menu);
        return 1;
    }
    menu->c_pending = 0;
    if (pressed & ControlLeft) menu->jump_letter = (menu->jump_letter+26)%27;
    else if (pressed & ControlRight) menu->jump_letter = (menu->jump_letter+1)%27;
    else if (pressed & ControlB) menu->alphabet_active = 0;
    else if (pressed & ControlDown) { menu->selected = 0; selection_changed(menu); }
    else if (pressed & ControlA) {
        unsigned char available[27];
        alphabet_available(menu,available);
        if (available[menu->jump_letter]) {
            menu->alphabet_active = 0;
            jump_letter(menu,menu->jump_letter);
        } else notify(menu,"No entries for that letter.");
    }
    return 1;
}

/* Defer a plain C press until release. Holding C for letter navigation must
 * never also add the selected game to the queue. */
static u32 browser_shortcuts(Menu *menu, u32 buttons, u32 pressed, u32 released)
{
    if (menu->alphabet_active) { menu->c_pending = 0; return 0; }
    if (menu->review || menu->demo || !menu->ready) { menu->c_pending = 0; return pressed; }
    if (pressed & (ControlA | ControlB | ControlX | ControlStart)) menu->c_pending = 0;
    if (pressed & ControlC) { menu->c_pending = 1; pressed &= ~ControlC; }
    if ((buttons & ControlC) && (pressed & ControlUp)) {
        focus_alphabet(menu);
        return 0;
    }
    if ((buttons & ControlC) && (pressed & (ControlLeft | ControlRight))) {
        const MenuEntry *entry = NULL;
        int letter = 0, direction = (pressed & ControlRight) ? 1 : -1, attempt;
        menu->c_pending = 0;
        if (row_count(menu) > (menu->listing.has_parent ? 1U : 0U) && !(menu->listing.has_parent && !menu->selected))
            entry = browser_entry(menu,menu->selected-(menu->listing.has_parent ? 1 : 0));
        if (entry) letter = menu_initial(entry->name);
        for (attempt = 0; attempt < 27; ++attempt) {
            size_t before = menu->selected;
            letter = (letter + direction + 27) % 27; jump_letter(menu,letter);
            if (menu->selected != before) break;
        }
        pressed &= ~(ControlLeft | ControlRight);
    }
    if ((released & ControlC) && menu->c_pending) { menu->c_pending = 0; queue_selected_disc(menu); }
    return pressed;
}
typedef struct TextOperation {
    Menu *menu;
    const char *name;
    const char *write;
    char *read;
    size_t capacity;
    size_t length;
} TextOperation;
static int text_at_root_callback(void *context)
{
    TextOperation *op = (TextOperation *)context;
    if (op->write) return usb_file_text_write(&op->menu->ode,ode_bus_read_buffer,op->name,op->write);
    return usb_file_text(&op->menu->ode,ode_bus_read_buffer,op->name,op->read,op->capacity,&op->length);
}
static int text_at_root(Menu *menu, const char *name, const char *write, char *read, size_t capacity, size_t *length)
{
    TextOperation op;
    ArtRouteStep route[MAX_DEPTH];
    int result;
    size_t i;
    if (menu->demo || !menu->ready || menu->bus.timed_out || menu->ode.navigation_failed ||
        menu->config_result == 2 || strcmp(menu->ode.identity,"LOCODE")) return -1;
    if (!menu->depth) for (i = 0; i < menu->listing.count; ++i)
        if (!(menu->listing.entries[i]->toc_id & 0xffffffUL)) return -1;
    current_route(menu,route);
    op.menu = menu; op.name = name; op.write = write; op.read = read; op.capacity = capacity; op.length = 0;
    result = art_at_root(&menu->ode,route,menu->depth,&menu->listing,menu->config.folder_workaround,text_at_root_callback,&op);
    if (result == -2 || menu->bus.timed_out || menu->ode.navigation_failed) {
        menu->ready = menu->view_valid = 0;
        menu->message = "USB I/O failed. Restart console.";
        result = -1;
    }
    if (length) *length = op.length;
    return result;
}
static void adopt_location(Menu *menu, LibraryLocation *location)
{
    size_t i;
    menu->filter[0] = 0;
    ode_listing_free(&menu->listing); menu->listing = location->listing;
    menu->depth = location->depth; menu->selected = location->selected;
    for (i = 0; i < menu->depth; ++i) {
        strcpy(menu->path[i],location->names[i]); menu->path_ids[i] = location->ids[i];
        menu->previous_selection[i] = location->previous[i];
    }
    menu->tools = menu->review = 0; menu->message = NULL; menu->ready = menu->view_valid = 1;
    update_breadcrumb(menu); selection_changed(menu);
}
static void load_position(Menu *menu)
{
    char *data = (char *)malloc(POSITION_BYTES);
    LibraryLocation *location;
    size_t length, i;
    int result = -1;
    if (!data) return;
    /* Same boot filesystem context used by the INI loader, before leaving root. */
    if (!usb_file_text(&menu->ode,ode_bus_read_buffer,"last-position.txt",data,POSITION_BYTES,&length))
        result = menu_position_parse(&menu->last_position,data,length);
    free(data);
    if (result < 0 || menu->bus.timed_out || menu->ode.navigation_failed) {
        menu->position_result = -1;
        if (menu->bus.timed_out || menu->ode.navigation_failed) menu->ready = menu->view_valid = 0;
        return;
    }
    if (!*menu->last_position.folder) return;
    location = (LibraryLocation *)malloc(sizeof(*location));
    if (!location) return;
    result = library_open_directory(&menu->ode,NULL,0,&menu->listing,menu->config.folder_workaround,menu->last_position.folder,location);
    if (!result) {
        adopt_location(menu,location);
        for (i = 0; i < row_count(menu); ++i) {
            const MenuEntry *entry;
            if (menu->listing.has_parent && !i) continue;
            entry = browser_entry(menu,i-(menu->listing.has_parent ? 1 : 0));
            if (entry && !menu_path_compare(entry->name,menu->last_position.selected)) { menu->selected = i; break; }
        }
    } else if (result == -2) menu->ready = menu->view_valid = 0;
    /* A renamed/missing folder simply leaves the boot listing in place. */
    free(location);
}
static void remember_position(Menu *menu, int force)
{
    MenuPosition *position;
    char *data;
    const MenuEntry *entry = NULL;
    if (!menu->config.remember_position || menu->config_result != 0 || menu->position_result < 0 ||
        menu->demo || !menu->ready || menu->loading || (!force && (!menu->position_dirty || menu->tools || menu->review || now_ms(menu)-menu->selection_since < 2000UL))) return;
    if (!menu->depth) {
        size_t i;
        for (i = 0; i < menu->listing.count; ++i) if (!(menu->listing.entries[i]->toc_id & 0xffffffUL)) return;
    }
    menu->position_dirty = 0;
    position = (MenuPosition *)malloc(sizeof(*position)); data = (char *)malloc(POSITION_BYTES);
    if (!position || !data) { free(position); free(data); return; }
    memset(position,0,sizeof(*position));
    if (row_count(menu) && !(menu->listing.has_parent && !menu->selected))
        entry = browser_entry(menu,menu->selected-(menu->listing.has_parent ? 1 : 0));
    if (entry) strcpy(position->selected,entry->name);
    if (!directory_path(menu,position->folder,sizeof(position->folder)) &&
        (strcmp(position->folder,menu->last_position.folder) || strcmp(position->selected,menu->last_position.selected))) {
        if (menu_position_format(position,data,POSITION_BYTES) < 0 || text_at_root(menu,"last-position.txt",data,NULL,0,NULL) < 0) {
            menu->position_result = -1; notify(menu,"Position not saved. See diagnostics.");
        } else { menu->last_position = *position; menu->position_result = 1; }
    }
    free(position); free(data);
}
typedef struct RecentLaunch {
    Menu *menu;
    const char *path;
} RecentLaunch;
static int record_recent_at_root(void *context)
{
    RecentLaunch *op=(RecentLaunch *)context;
    Menu *menu=op->menu;
    char *data=(char *)malloc(FAVORITES_BYTES);
    size_t length;
    int result=-1;
    if (!data) return -1;
    if (!menu->recent_valid) {
        if (usb_file_text(&menu->ode,ode_bus_read_buffer,"recent.txt",data,FAVORITES_BYTES,&length)<0 ||
            menu_favorites_parse(&menu->recent,data,length)<0 || menu->recent.count>10) goto done;
        menu->recent_valid=1;
    }
    if (menu->recent.count && !menu_path_compare(menu->recent.paths[0],op->path)) { result=0; goto done; }
    if (menu_recent_push(&menu->recent,op->path)<0 || menu_favorites_format(&menu->recent,data,FAVORITES_BYTES)<0) goto done;
    result=usb_file_text_write(&menu->ode,ode_bus_read_buffer,"recent.txt",data);
done:
    if (result) menu->recent_valid=0;
    free(data); return result;
}
static void record_launch(Menu *menu,const char *path)
{
    ArtRouteStep route[MAX_DEPTH];
    RecentLaunch op;
    int result;
    menu->message="Saving launch history..."; draw(menu);
    remember_position(menu,1);
    if (!menu->config.recent_games || menu->config_result!=0 || !menu->ready ||
        menu->demo || menu->bus.timed_out || menu->ode.navigation_failed || strncmp(path,"/Storage/",9)) return;
    if (menu->recent_valid && menu->recent.count && !menu_path_compare(menu->recent.paths[0],path+9)) return;
    current_route(menu,route); op.menu=menu; op.path=path+9;
    result=art_at_root(&menu->ode,route,menu->depth,&menu->listing,menu->config.folder_workaround,record_recent_at_root,&op);
    menu->recent_result=result ? -1 : 1;
    if (result==-2 || menu->bus.timed_out || menu->ode.navigation_failed) {
        menu->ready=menu->view_valid=0; menu->message="History I/O failed. Restart console.";
    }
}
static void open_recent(Menu *menu)
{
    char *data = (char *)malloc(FAVORITES_BYTES);
    size_t length;
    menu->recent_valid = 0; menu_favorites_free(&menu->recent);
    if (data && !text_at_root(menu,"recent.txt",NULL,data,FAVORITES_BYTES,&length) &&
        !menu_favorites_parse(&menu->recent,data,length) && menu->recent.count <= 10) {
        menu->recent_valid = 1; menu->tool_message = menu->recent.count ? NULL : "No recent launch selections.";
    } else menu->tool_message = "Recent list unavailable or invalid.";
    free(data); menu->tool_selected = 0;
}
static void export_diagnostics(Menu *menu)
{
    OdeBus snapshot = menu->bus;
    char data[4096], line[128];
    size_t i;
    strcpy(data,"Oh My 3DO diagnostics\n");
    format(line,"menu=%s\nidentity=%.6s\nrevision=%u.%u.%u\n",MENU_VERSION,menu->ode.identity,
           menu->ode.revision[0],menu->ode.revision[1],menu->ode.revision[2]); strcat(data,line);
    strcat(data,"identity_reply=");
    for (i = 0; i < 12; ++i) { format(line,"%02x ",menu->ode.identity_reply[i]); strcat(data,line); }
    format(line,"\ncommands=%lu\nlast_command=%02x\nreply_length=%lu\nlast_reply=",snapshot.commands,snapshot.last_command,(unsigned long)snapshot.last_length); strcat(data,line);
    for (i = 0; i < snapshot.last_length && i < 32; ++i) { format(line,"%02x ",snapshot.last_reply[i]); strcat(data,line); }
    format(line,"\nlast_error=%08lx\nlast_fault_command=%02x\nlast_fault_status=%02x\nfault_error=%08lx\n",(unsigned long)snapshot.last_error & 0xffffffffUL,snapshot.failed_command,snapshot.failed_status,(unsigned long)snapshot.failure_error & 0xffffffffUL); strcat(data,line);
    format(line,"timeout=%d\nnavigation_blocked=%d\nconfig_result=%d\n",snapshot.timed_out,menu->ode.navigation_failed,menu->config_result); strcat(data,line);
    format(line,"folder_workaround=%d\nusb_covers=%d\nscreen_margin=%d\nlarge_titles=%d\ntheme=%d\n",menu->config.folder_workaround,menu->config.usb_covers,menu->config.screen_margin,menu->config.large_titles,menu->config.theme); strcat(data,line);
    format(line,"preview_sound=%d\npreview_audio_status=%d\n",menu->config.preview_sound,menu->preview_audio.status); strcat(data,line);
    format(line,"cover_load_ms=%lu\ncover_commands=%lu\ncover_cached=%d\n",menu->cover_load_ms,menu->cover_commands,menu->cover_cached); strcat(data,line);
    format(line,"remember_position=%d\nrecent_games=%d\nvideo_previews=%d\nunsaved_settings=%d\n",menu->config.remember_position,menu->config.recent_games,menu->config.video_previews,config_dirty(menu)); strcat(data,line);
    format(line,"queue_count=%lu\nqueue_synchronized=%d\nqueue_dirty=%d\n",(unsigned long)menu->playlist.count,menu->playlist.synchronized,menu->playlist.dirty); strcat(data,line);
    format(line,"usb_write_check=%d\nbackup_result=%d\nrestore_result=%d\nnvram_error=%08lx\n",menu->usb_tested,menu->backup_result,menu->restore_result,(unsigned long)menu->nvram_info.last_error & 0xffffffffUL); strcat(data,line);
    format(line,"position_result=%d\nrecent_result=%d\nfolder=%.64s\n",menu->position_result,menu->recent_result,menu->breadcrumb); strcat(data,line);
    format(line,"preview_result=%d\npreview_load_ms=%lu\n",menu->preview_result,menu->preview_load_ms); strcat(data,line);
    strcat(data,"Identity/revision may be hard-coded; exact firmware build is unknown.\nSnapshot taken before export file I/O.\n");
    /* Fixed report extent replaces previous reports without firmware truncate. */
    i = strlen(data); memset(data+i,'\n',sizeof(data)-1-i); data[sizeof(data)-1] = 0;
    menu->tool_message = "Writing / verifying diagnostics..."; draw(menu);
    menu->diagnostics_result = text_at_root(menu,"diagnostics.txt",data,NULL,0,NULL) < 0 ? -1 : 1;
    menu->tool_message = menu->diagnostics_result > 0 ? "Saved USB diagnostics.txt." : "USB export unavailable or failed.";
}
static int favorites_read(void *context)
{
    Menu *menu = (Menu *)context;
    char *data = (char *)malloc(FAVORITES_BYTES);
    size_t length;
    int result = -1;
    if (data && !usb_file_text(&menu->ode,ode_bus_read_buffer,"favorites.txt",data,FAVORITES_BYTES,&length))
        result = menu_favorites_parse(&menu->favorites,data,length);
    menu->favorites_valid = result == 0;
    free(data); return result;
}
static void open_favorites(Menu *menu)
{
    ArtRouteStep route[MAX_DEPTH];
    int result;
    menu_favorites_free(&menu->favorites);
    menu->favorites_valid = 0;
    strcpy(menu->favorites_prefix,"/Storage/");
    if (menu->depth && !(menu->path_ids[0] & 0xffffffUL)) {
        strcat(menu->favorites_prefix,menu->path[0]); strcat(menu->favorites_prefix,"/");
    }
    if (menu->demo || !menu->ready || menu->ode.navigation_failed || menu->bus.timed_out ||
        menu->config_result == 2 || strcmp(menu->ode.identity,"LOCODE")) {
        menu->tool_message = "USB favorites unavailable."; return;
    }
    if (!menu->depth) {
        size_t i;
        for (i = 0; i < menu->listing.count; ++i) if (!(menu->listing.entries[i]->toc_id & 0xffffffUL)) {
            menu->tool_message = "Enter a USB volume first."; return;
        }
    }
    current_route(menu,route);
    menu->tool_message = "Reading USB favorites..."; draw(menu);
    result = art_at_root(&menu->ode,route,menu->depth,&menu->listing,menu->config.folder_workaround,favorites_read,menu);
    menu->tool_selected = 0;
    if (result == -2 || menu->bus.timed_out || menu->ode.navigation_failed) {
        menu->ready = menu->view_valid = 0; menu->tool_message = "USB I/O failed. Restart console.";
    } else menu->tool_message = result ? "Invalid favorites.txt on USB." : (menu->favorites.count ? NULL : "C adds the selected browser game.");
}
typedef struct FavoritesEdit {
    Menu *menu;
    const char *path;
    int remove;
} FavoritesEdit;
static int favorites_edit_at_root(void *context)
{
    FavoritesEdit *edit = (FavoritesEdit *)context;
    char *data;
    size_t i, used = 0;
    int result;
    /* Re-read before every edit: never overwrite a malformed/unreadable list. */
    if (favorites_read(edit->menu) < 0) return -1;
    result = menu_favorites_edit(&edit->menu->favorites,edit->path,edit->remove);
    if (result) return result;
    data = (char *)malloc(FAVORITES_BYTES);
    if (!data) return -1;
    for (i = 0; i < edit->menu->favorites.count; ++i) {
        size_t n = strlen(edit->menu->favorites.paths[i]);
        memcpy(data+used,edit->menu->favorites.paths[i],n); used += n; data[used++] = '\n';
    }
    data[used] = 0;
    result = usb_file_text_write(&edit->menu->ode,ode_bus_read_buffer,"favorites.txt",data);
    free(data);
    if (result < 0) edit->menu->favorites_valid = 0;
    return result;
}
static void edit_favorites(Menu *menu, int remove)
{
    ArtRouteStep route[MAX_DEPTH];
    FavoritesEdit edit;
    char path[FAVORITE_PATH];
    size_t used = 0;
    unsigned int i, first = menu->depth && !(menu->path_ids[0] & 0xffffffUL) ? 1 : 0;
    int result;
    if (menu->demo || !menu->ready || menu->ode.navigation_failed || menu->bus.timed_out ||
        menu->config_result == 2 || strcmp(menu->ode.identity,"LOCODE")) {
        menu->tool_message = "USB favorites unavailable."; return;
    }
    if (remove) {
        if (!menu->favorites_valid || menu->tool_selected >= menu->favorites.count) {
            menu->tool_message = "Select a favorite to remove."; return;
        }
        strcpy(path,menu->favorites.paths[menu->tool_selected]);
    } else {
        const MenuEntry *entry;
        if (menu->listing.has_parent && !menu->selected) goto select_game;
        if (!row_count(menu)) goto select_game;
        entry = browser_entry(menu,menu->selected-(menu->listing.has_parent ? 1 : 0));
        if (!entry || entry->is_directory) goto select_game;
        for (i = first; i < menu->depth; ++i) {
            size_t n = strlen(menu->path[i]);
            if (used + n + 1 >= sizeof(path)) goto too_long;
            memcpy(path+used,menu->path[i],n); used += n; path[used++] = '/';
        }
        if (used + strlen(entry->name) >= sizeof(path)) goto too_long;
        strcpy(path+used,entry->name);
    }
    current_route(menu,route);
    edit.menu = menu; edit.path = path; edit.remove = remove;
    menu->tool_message = "Saving / verifying favorites..."; draw(menu);
    result = art_at_root(&menu->ode,route,menu->depth,&menu->listing,
                         menu->config.folder_workaround,favorites_edit_at_root,&edit);
    if (result == -2 || menu->bus.timed_out || menu->ode.navigation_failed) {
        menu->ready = menu->view_valid = 0; menu->tool_message = "USB I/O failed. Restart console.";
    } else if (result < 0) menu->tool_message = "Favorites not saved. Check USB file.";
    else if (result == 1) menu->tool_message = remove ? "Favorite already removed." : "Game is already a favorite.";
    else menu->tool_message = remove ? "Favorite removed and saved." : "Favorite added and saved.";
    if (menu->tool_selected >= menu->favorites.count)
        menu->tool_selected = menu->favorites.count ? menu->favorites.count-1 : 0;
    return;
select_game:
    menu->tool_message = "Select a game in the browser first."; return;
too_long:
    menu->tool_message = "Favorite path is too long.";
}
static void open_saved_path(Menu *menu, const char *path)
{
    ArtRouteStep route[MAX_DEPTH];
    LibraryLocation *location;
    int result;
    if (menu->ode.navigation_failed || menu->bus.timed_out || !menu->ready) return;
    location = (LibraryLocation *)malloc(sizeof(*location));
    if (!location) { menu->tool_message = "Not enough memory."; return; }
    current_route(menu,route);
    menu->tool_message = "Opening saved game folder..."; draw(menu);
    result = library_open(&menu->ode,route,menu->depth,&menu->listing,menu->config.folder_workaround,path,location);
    if (!result) adopt_location(menu,location);
    else {
        menu->tool_message = result == -2 ? "Folder I/O failed. Restart console." : "Saved path not found on USB.";
        if (result == -2) menu->ready = menu->view_valid = 0;
    }
    free(location);
}
static void open_favorite(Menu *menu)
{
    char path[FAVORITE_PATH + ODE_MAX_NAME + 10];
    const char *selected;
    if (menu->tools == TOOL_RECENT) {
        if (!menu->recent_valid || menu->tool_selected >= menu->recent.count) return;
        selected = menu->recent.paths[menu->tool_selected]; strcpy(path,"/Storage/");
    } else {
        if (!menu->favorites_valid || menu->tool_selected >= menu->favorites.count) return;
        selected = menu->favorites.paths[menu->tool_selected]; strcpy(path,menu->favorites_prefix);
    }
    strcat(path,selected); open_saved_path(menu,path);
}
static void settings_values(const Menu *menu, MenuConfig *settings)
{
    *settings = menu->config;
    settings->folder_workaround = menu->settings_folder_workaround;
    settings->usb_covers = menu->settings_usb_covers;
    settings->remember_position = menu->settings_remember_position;
    settings->recent_games = menu->settings_recent_games;
    settings->video_previews = menu->settings_video_previews;
}
static int config_dirty(const Menu *menu)
{
    MenuConfig current;
    const MenuConfig *saved = &menu->saved_config;
    settings_values(menu,&current);
    return current.folder_workaround != saved->folder_workaround || current.usb_covers != saved->usb_covers ||
        current.screen_margin != saved->screen_margin || current.large_titles != saved->large_titles ||
        current.theme != saved->theme || current.remember_position != saved->remember_position || current.recent_games != saved->recent_games || current.video_previews != saved->video_previews || current.preview_sound != saved->preview_sound;
}
static void discard_config(Menu *menu)
{
    menu->config.preview_sound = menu->saved_config.preview_sound;
    menu->config.screen_margin = menu->saved_config.screen_margin;
    menu->config.large_titles = menu->saved_config.large_titles; menu->config.theme = menu->saved_config.theme;
    menu->settings_folder_workaround = menu->saved_config.folder_workaround;
    menu->settings_usb_covers = menu->saved_config.usb_covers;
    menu->settings_remember_position = menu->saved_config.remember_position;
    menu->settings_recent_games = menu->saved_config.recent_games;
    menu->settings_video_previews = menu->saved_config.video_previews;
    menu->tool_message = "Unsaved settings discarded."; selection_changed(menu);
}
static void save_config(Menu *menu)
{
    if (menu->config_result != 0) { menu->tool_message = "Load a valid INI before saving."; return; }
    if (!save_root_operation(menu,3,MENU_CONFIG_NAME,NULL)) {
        settings_values(menu,&menu->saved_config); menu->tool_message = "Saved. Non-display: after reboot.";
    }
}
static void change_config(Menu *menu, int direction)
{
    menu->tool_message = "Changed. C:Save to USB";
    if (menu->tool_selected == 11) { save_config(menu); return; }
    if (menu->tool_selected == 8) { menu->config.preview_sound = !menu->config.preview_sound; return; }
    if (menu->tool_selected == 5) { menu->settings_video_previews = !menu->settings_video_previews; return; }
    if (!menu->tool_selected) menu->config.screen_margin = (menu->config.screen_margin + direction + 13) % 13;
    else if (menu->tool_selected == 1) menu->config.large_titles = !menu->config.large_titles;
    else if (menu->tool_selected == 2) menu->config.theme = (menu->config.theme + direction + MENU_THEME_COUNT) % MENU_THEME_COUNT;
    else if (menu->tool_selected == 3) menu->settings_folder_workaround = !menu->settings_folder_workaround;
    else if (menu->tool_selected == 4) menu->settings_usb_covers = !menu->settings_usb_covers;
    else if (menu->tool_selected == 6) menu->settings_remember_position = !menu->settings_remember_position;
    else if (menu->tool_selected == 7) menu->settings_recent_games = !menu->settings_recent_games;
    else if (menu->tool_selected == 10) discard_config(menu);
    else { menu->config.screen_margin = menu->config.large_titles = menu->config.theme = 0; }
    selection_changed(menu);
}

static void cancel_favorites_move(Menu *menu)
{
    int result = 0;
    if (menu->favorites_original)
        result = menu_favorites_parse(&menu->favorites,menu->favorites_original,strlen(menu->favorites_original));
    free(menu->favorites_original); menu->favorites_original = NULL; menu->favorites_move = 0;
    if (result < 0) menu->favorites_valid = 0;
    menu->tool_message = result < 0 ? "Reload favorites before opening." : "Local ordering edits discarded.";
}
static void begin_favorites_move(Menu *menu)
{
    if (!menu->favorites_valid || !menu->favorites.count) { menu->tool_message = "No favorites to move."; return; }
    menu->favorites_original = (char *)malloc(FAVORITES_BYTES);
    if (!menu->favorites_original || menu_favorites_format(&menu->favorites,menu->favorites_original,FAVORITES_BYTES) < 0) {
        free(menu->favorites_original); menu->favorites_original = NULL; menu->tool_message = "Not enough memory."; return;
    }
    menu->favorites_move = 1; menu->tool_message = NULL;
}
static void save_favorites_order(Menu *menu)
{
    MenuFavorites current;
    char *latest = (char *)malloc(FAVORITES_BYTES), *desired = (char *)malloc(FAVORITES_BYTES);
    size_t length;
    int result = -1;
    memset(&current,0,sizeof(current));
    if (!latest || !desired || !menu->favorites_original) goto done;
    if (menu_favorites_format(&menu->favorites,desired,FAVORITES_BYTES) < 0 ||
        text_at_root(menu,"favorites.txt",NULL,latest,FAVORITES_BYTES,&length) < 0 ||
        menu_favorites_parse(&current,latest,length) < 0 || menu_favorites_format(&current,latest,FAVORITES_BYTES) < 0) goto done;
    if (strcmp(latest,menu->favorites_original)) {
        menu->tool_message = "USB list changed. Cancel and reload."; result = 1; goto done;
    }
    if (text_at_root(menu,"favorites.txt",desired,NULL,0,NULL) < 0) { menu->favorites_valid = 0; goto done; }
    free(menu->favorites_original); menu->favorites_original = NULL; menu->favorites_move = 0;
    menu->tool_message = "Favorite order saved and verified."; result = 0;
done:
    if (result < 0) menu->tool_message = "Favorite order not saved.";
    menu_favorites_free(&current); free(latest); free(desired);
}
static const char find_keys[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_.";
static int search_progress(void *context,size_t games)
{
    Menu *menu=(Menu *)context;
    ControlPadEventData pad;
    if (GetControlPad(1,0,&pad)>=0 && (pad.cped_ButtonBits&ControlB)) {
        menu->suppress_buttons|=ControlB; return 1;
    }
    if (now_ms(menu)-menu->search_redraw>=150UL) {
        (void)games; strcpy(menu->status,"Loading search index... B cancels");
        menu->tool_message=menu->status; draw(menu); menu->search_redraw=now_ms(menu);
    }
    return 0;
}
typedef struct SearchIndexRead {
    Menu *menu;
    char prefix[ODE_MAX_NAME+10];
} SearchIndexRead;
static int index_cancel(void *context) { return search_progress(context,0); }
static int read_search_index(void *context)
{
    SearchIndexRead *read=(SearchIndexRead *)context;
    int result=library_index_load(&read->menu->ode,ode_bus_read_buffer,read->prefix,
        &read->menu->catalog,index_cancel,read->menu);
    if (result==-3) {
        cover_cache_clear(&read->menu->cover_cache);
        result=library_index_load(&read->menu->ode,ode_bus_read_buffer,read->prefix,
            &read->menu->catalog,index_cancel,read->menu);
    }
    return result;
}
static void search_library(Menu *menu,int rebuild)
{
    ArtRouteStep route[MAX_DEPTH];
    SearchIndexRead *read;
    unsigned long volume=menu->depth ? menu->path_ids[0]&0xff000000UL :
        (menu->listing.count ? menu->listing.entries[0]->toc_id&0xff000000UL : 0);
    int result=0;
    size_t i;
    if (menu->demo) { menu->tool_message="Library search needs an ODE."; return; }
    if (menu->config_result!=0 || strcmp(menu->ode.identity,"LOCODE")) {
        menu->tool_message="Search needs a valid USB config."; return;
    }
    if (!menu->ready || menu->bus.timed_out || menu->ode.navigation_failed) {
        menu->tool_message="ODE unavailable. Restart console."; return;
    }
    if (!menu->depth) for (i=0;i<menu->listing.count;++i)
        if (menu->listing.entries[i]->is_directory && !(menu->listing.entries[i]->toc_id&0xffffffUL)) {
            menu->tool_message="Open a USB drive before searching."; return;
        }
    if (!menu->catalog.valid || rebuild || menu->catalog_volume!=volume) {
        read=(SearchIndexRead *)malloc(sizeof(*read));
        if (!read) { menu->tool_message="Not enough search memory."; return; }
        read->menu=menu; strcpy(read->prefix,"/Storage");
        if (menu->depth && !(menu->path_ids[0]&0xffffffUL)) { strcat(read->prefix,"/"); strcat(read->prefix,menu->path[0]); }
        stop_preview(menu); current_route(menu,route); menu->search_redraw=0;
        menu->tool_message="Loading search index... B cancels"; draw(menu);
        result=art_at_root(&menu->ode,route,menu->depth,&menu->listing,
            menu->config.folder_workaround,read_search_index,read);
        free(read);
        if (result) {
            menu->tool_message=result==1 ? "Search cancelled." : (result==-2 ? "Folder I/O failed. Restart console." : (result==-3 ? "Not enough search memory." : "Run index-media.py on the USB drive."));
            if (result==-2 || menu->bus.timed_out || menu->ode.navigation_failed) menu->ready=menu->view_valid=0;
            return;
        }
        menu->catalog_volume=volume;
    }
    menu->search_count=0;
    for (i=0;i<menu->catalog.count;++i) {
        const char *path=menu->catalog.paths[i], *name=strrchr(path,'/');
        if (menu_name_matches(name ? name+1 : path,menu->find_query)) menu->search_matches[menu->search_count++]=i;
    }
    menu->tool_message=NULL; menu->tools=TOOL_RESULTS; menu->tool_selected=0; menu->selection_since=now_ms(menu);
}
static void find_input(Menu *menu, u32 pressed)
{
    size_t length = strlen(menu->find_query);
    if (pressed & ControlLeft) menu->find_key = menu->find_key/10*10+(menu->find_key+9)%10;
    if (pressed & ControlRight) menu->find_key = menu->find_key/10*10+(menu->find_key+1)%10;
    if (pressed & ControlUp) menu->find_key = (menu->find_key+30)%40;
    if (pressed & ControlDown) menu->find_key = (menu->find_key+10)%40;
    if ((pressed & ControlA) && length < sizeof(menu->find_query)-1) {
        menu->find_query[length++] = find_keys[menu->find_key]; menu->find_query[length] = 0;
    }
    if ((pressed & ControlX) && length) menu->find_query[length-1] = 0;
    if (pressed & ControlC) menu->find_query[0] = 0;
    if (pressed & ControlStart) {
        search_library(menu,0);
    }
}
static void tool_line(Menu *menu, int row, const char *line, Color color)
{
    text(menu, 22, 82 + row * 12, line, color);
}
static void slot_path(char *path, unsigned int slot)
{
    if (slot == 8) strcpy(path, "saves/recovery.3ds");
    else if (slot == 9) strcpy(path, "saves/recovery2.3ds");
    else format(path, "saves/slot%02u.3ds", slot + 1);
}
static void draw_tools(Menu *menu)
{
    char line[80];
    const char *title = "Menu";
    size_t i, count = 0, start;
    if (menu->tools == TOOL_SAVES) title = "Internal saves";
    if (menu->tools == TOOL_SLOTS) title = menu->backup_mode ? "CHOOSE BACKUP SLOT" : "USB BACKUPS";
    if (menu->tools == TOOL_ARCHIVE) title = "Backup contents";
    if (menu->tools == TOOL_HELP) title = "Controls & help";
    if (menu->tools == TOOL_DIAG) title = "Diagnostics";
    if (menu->tools == TOOL_CONFIRM_BACKUP || menu->tools == TOOL_CONFIRM_RESTORE) title = "Confirm action";
    if (menu->tools == TOOL_GROUP) title = "Matching discs";
    if (menu->tools == TOOL_FAVORITES) title = "Favorites";
    if (menu->tools == TOOL_CONFIG) title = config_dirty(menu) ? "Settings / unsaved" : "Settings";
    if (menu->tools == TOOL_RECENT) title = "Recent games";
    if (menu->tools == TOOL_FIND) title = "Find a game";
    if (menu->tools == TOOL_RESULTS) title = "Search results";
    if (menu->tools == TOOL_FAVORITES && menu->favorites_move) title = "Move favorite";
    if (menu->tools == TOOL_BACKUP_DETAILS) title = menu->details_new ? "New backup contents" : "Existing slot contents";
    menu->text_cursor=0; menu->text_style=UI_FONT_SMALL;
    menu->screen.sc_curScreen ^= 1;
    backdrop(menu); brand(menu,"Tools");
    label(menu,118,32,"Make it yours",MUTED,UI_FONT_SMALL);
    rounded(menu,12,58,296,148,PANEL);
    label(menu,22,64,title,TEXT,UI_FONT_NORMAL);
    rectangle(menu, 20, 77, 280, 1, LINE);
    if (menu->tools == TOOL_HOME) {
        static const char *items[]={"Save manager","USB backups","Favorites","Settings","Help & controls","Diagnostics","Recent games","Find a game"};
        for (i=0;i<8;++i) {
            int x=18+(i/4)*144, y=84+(i%4)*28;
            int active=i==menu->tool_selected;
            rounded(menu,x,y,138,24,active ? ACCENT : SELECT);
            label_on(menu,x+9,y+5,i==3 && config_dirty(menu) ? "Settings *" : items[i],active ? MakeRGB15(1,2,4) : TEXT,UI_FONT_NORMAL,active ? ACCENT : SELECT);
        }
    } else if (menu->tools == TOOL_SAVES || menu->tools == TOOL_ARCHIVE || menu->tools == TOOL_SLOTS || menu->tools == TOOL_BACKUP_DETAILS || menu->tools == TOOL_GROUP || menu->tools == TOOL_FAVORITES || menu->tools == TOOL_RECENT) {
        SaveArchive *archive = menu->tools == TOOL_SAVES ? &menu->nvram : (menu->tools == TOOL_BACKUP_DETAILS && menu->details_new ? &menu->pending : &menu->backup);
        count = menu->tools == TOOL_SLOTS ? (menu->backup_mode ? 8 : 10) : (menu->tools == TOOL_GROUP ? menu->group.count : (menu->tools == TOOL_FAVORITES ? menu->favorites.count : (menu->tools == TOOL_RECENT ? menu->recent.count : archive->count)));
        start = menu->tool_selected / 8 * 8;
        for (i = start; i < count && i < start + 8; ++i) {
            menu->text_background_set=1; menu->text_background=i==menu->tool_selected ? LINE : PANEL;
            if (i == menu->tool_selected) rectangle(menu, 16, 81 + (i - start) * 12, 287, 12, LINE);
            if (menu->tools == TOOL_SLOTS) {
                slot_path(line, (unsigned int)i);
                tool_line(menu, (int)(i - start), line + 6, i == menu->tool_selected ? TEXT : MUTED);
            } else if (menu->tools == TOOL_GROUP || menu->tools == TOOL_FAVORITES || menu->tools == TOOL_RECENT) {
                const char *name = menu->tools == TOOL_GROUP ? menu->group.paths[i] : (menu->tools == TOOL_RECENT ? menu->recent.paths[i] : menu->favorites.paths[i]);
                if (!strncmp(name,"/Storage/",9)) name+=9;
                game_label(menu,22,82+(int)(i-start)*12,276,name,0,UI_FONT_SMALL,
                    i==menu->tool_selected ? TEXT : MUTED,i==menu->tool_selected ? LINE : PANEL,i==menu->tool_selected);
            } else {
                const SaveEntry *e = &archive->entries[i];
                size_t n = strlen(e->name);
                if (n > 24) n = 24;
                memcpy(line, e->name, n); line[n] = 0;
                tool_line(menu, (int)(i - start), line, i == menu->tool_selected ? TEXT : MUTED);
                format(line, "%5lu", (unsigned long)e->size);
                text(menu, 260, 82 + (i - start) * 12, line, MUTED);
            }
        }
        menu->text_background_set=0;
        if (!count) tool_line(menu,2,(menu->tools == TOOL_FAVORITES || menu->tools == TOOL_RECENT) ? "No games listed." :
             (menu->tools == TOOL_BACKUP_DETAILS && !menu->backup_slot_valid ? "Empty or invalid backup slot." : "No saves to display."),MUTED);
        if (menu->tools == TOOL_SAVES) {
            format(line, "%lu files %lu used %lu free", (unsigned long)count, menu->nvram_info.used_bytes, menu->nvram_info.free_bytes);
            text(menu, 22, 188, line, MUTED);
        } else {
            format(line, "%lu / %lu", (unsigned long)(count ? menu->tool_selected + 1 : 0), (unsigned long)count);
            text(menu, 22, 188, line, MUTED);
        }
    } else if (menu->tools == TOOL_CONFIG) {
        start=menu->tool_selected/9*9;
        for (i = start; i < 12 && i < start+9; ++i) {
            if (!i) format(line,"Screen margin: %d",menu->config.screen_margin);
            else if (i == 1) format(line,"Large game titles: %s",menu->config.large_titles ? "ON" : "OFF");
            else if (i == 2) format(line,"Theme: %s",GECKO ? "Gecko TV" : (menu->config.theme == 1 ? "High contrast" : (menu->config.theme == 2 ? "Amber" : "Teal")));
            else if (i == 3) format(line,"Folder workaround: %s",menu->settings_folder_workaround ? "ON" : "OFF");
            else if (i == 4) format(line,"USB covers: %s",menu->settings_usb_covers ? "ON" : "OFF");
            else if (i == 6) format(line,"Remember position: %s",menu->settings_remember_position ? "ON" : "OFF");
            else if (i == 7) format(line,"Recent games: %s",menu->settings_recent_games ? "ON" : "OFF");
            else if (i == 8) format(line,"Preview sound: %s",menu->config.preview_sound ? "ON" : "OFF");
            else if (i == 9) strcpy(line,"Restore display defaults");
            else if (i == 10) strcpy(line,"Discard unsaved changes");
            else if (i == 11) strcpy(line,"Save changes to USB");
            else format(line,"Video previews: %s",menu->settings_video_previews ? "ON" : "OFF");
            if (i == menu->tool_selected) rectangle(menu,16,81+(i-start)*12,287,12,LINE);
            label_on(menu,24,82+(i-start)*12,line,i == menu->tool_selected ? TEXT : MUTED,UI_FONT_SMALL,i==menu->tool_selected ? LINE : PANEL);
        }
        label(menu,24,194,"Sound + display apply now. C saves.",MUTED,UI_FONT_SMALL);
        label(menu,270,64,start ? "2 / 2" : "1 / 2",MUTED,UI_FONT_SMALL);
    } else if (menu->tools == TOOL_RESULTS) {
        count=menu->search_count;
        start=menu->tool_selected/6*6;
        format(line,"%lu matches for: %.24s",(unsigned long)count,*menu->find_query ? menu->find_query : "All games");
        label(menu,22,82,line,MUTED,UI_FONT_SMALL);
        for (i=start;i<count && i<start+6;++i) {
            const char *path=menu->catalog.paths[menu->search_matches[i]];
            const char *name=strrchr(path,'/')+1;
            int y=99+(int)(i-start)*13;
            if (i==menu->tool_selected) rectangle(menu,16,y-1,287,13,LINE);
            game_label(menu,22,y,276,name,0,UI_FONT_SMALL,i==menu->tool_selected ? TEXT : MUTED,
                i==menu->tool_selected ? LINE : PANEL,i==menu->tool_selected);
        }
        if (!count) label(menu,22,112,"No games matched. Start to edit.",TEXT,UI_FONT_SMALL);
        else {
            const char *path=menu->catalog.paths[menu->search_matches[menu->tool_selected]];
            size_t length=(size_t)(strrchr(path,'/')-path);
            int room=280*(320-2*menu->config.screen_margin)/320;
            size_t start=0;
            while (start<length && ui_font_fit(path+start,length-start,room,UI_FONT_SMALL)<length-start) ++start;
            length-=start; if (length>70) length=70;
            memcpy(line,path+start,length); line[length]=0;
            label(menu,22,185,line,MUTED,UI_FONT_SMALL);
        }
    } else if (menu->tools == TOOL_FIND) {
        format(line,"Find: %s_",menu->find_query); tool_line(menu,0,line,TEXT);
        tool_line(menu,1,"Drive-wide search / PC index.",MUTED);
        for (i = 0; i < 40; ++i) {
            char key[3]; key[0] = find_keys[i]; key[1] = 0;
            if (key[0] == ' ') strcpy(key,"SP");
            if (i == (size_t)menu->find_key) rectangle(menu,22+(i%10)*27,111+(i/10)*17,19,13,LINE);
            text(menu,27+(i%10)*27,113+(i/10)*17,key,TEXT);
        }
        text(menu,24,189,"Start:Search X:Erase C:Clear",MUTED);
    } else if (menu->tools == TOOL_HELP) {
        static const char *help[] = {
            "A: Open folder / launch game", "B: Parent folder / cancel", "Up/Down: Select (hold to repeat)",
            "Left/Right or L/R: Page", "C: Add selected disc to queue", "Start: Review queued discs",
            "X: Refresh current folder", "L+R together: Open tools", "Hold X at boot: Skip USB extras", "C+Up or list ends: Alphabet"
        };
        static const char *more[] = {
            "Queue: X removes one disc", "Queue: C toggles move mode", "Move mode: Up/Down reorders",
            "Hold C, press X: Clear queue", "Hold C + Left/Right: Letter", "Disc 1: Offer matching disc set",
            "Favorites: A opens game folder", "Favorites: C adds, X removes", "Config: C saves, X discards", "ABC: Left/Right A:Jump B:Cancel"
        };
        for (i = 0; i < 10; ++i) tool_line(menu,(int)i,menu->tool_selected ? more[i] : help[i],i < 8 ? TEXT : MUTED);
    } else if (menu->tools == TOOL_DIAG) {
        if (!menu->tool_selected) {
            format(line, "Menu %s / XBUS unit 0", MENU_VERSION); tool_line(menu, 0, line, TEXT);
            format(line, "ODE ID: %.6s", menu->ode.identity[0] ? menu->ode.identity : "------"); tool_line(menu, 1, line, TEXT);
            format(line, "Reported revision: %u.%u.%u", menu->ode.revision[0], menu->ode.revision[1], menu->ode.revision[2]); tool_line(menu, 2, line, TEXT);
            format(line, "Boot flags: %02x Status: %02x", menu->ode.identity_reply[1], menu->ode.identity_reply[11]); tool_line(menu, 3, line, MUTED);
            tool_line(menu, 4, "Identity bytes (93 reply):", MUTED);
            format(line, "%02x %02x %02x %02x %02x %02x", menu->ode.identity_reply[0], menu->ode.identity_reply[1], menu->ode.identity_reply[2], menu->ode.identity_reply[3], menu->ode.identity_reply[4], menu->ode.identity_reply[5]); tool_line(menu, 5, line, TEXT);
            format(line, "%02x %02x %02x %02x %02x %02x", menu->ode.identity_reply[6], menu->ode.identity_reply[7], menu->ode.identity_reply[8], menu->ode.identity_reply[9], menu->ode.identity_reply[10], menu->ode.identity_reply[11]); tool_line(menu, 6, line, TEXT);
            tool_line(menu, 8, "ID/revision can be hard-coded.", AMBER);
            tool_line(menu, 9, "Exact firmware build is unknown.", AMBER);
        } else if (menu->tool_selected == 1) {
            int row = 4;
            format(line, "Folder workaround: %s", menu->config.folder_workaround ? "ON" : "OFF"); tool_line(menu, 0, line, TEXT);
            format(line, "Config result: %d (0 = loaded)", menu->config_result); tool_line(menu, 1, line, TEXT);
            format(line, "USB covers: %s", menu->skip_usb_art ? "disabled" : "enabled"); tool_line(menu, 2, line, TEXT);
            format(line, "Cover index: %lu entries", (unsigned long)menu->usb_art.count); tool_line(menu, 3, line, MUTED);
            if (menu->usb_tested) {
                format(line, "USB write check: %s", menu->usb_tested > 0 ? "PASS" : "FAIL");
                tool_line(menu, row++, line, TEXT);
            }
            format(line, "Backup: %s", menu->backup_result > 0 ? "verified" : (menu->backup_result < 0 ? "failed" : "not run")); tool_line(menu, row++, line, TEXT);
            format(line, "Restore: %s", menu->restore_result > 0 ? "verified" : (menu->restore_result < 0 ? "failed" : "not run")); tool_line(menu, row++, line, TEXT);
            format(line, "Queued discs: %lu / 16", (unsigned long)menu->playlist.count); tool_line(menu, row++, line, MUTED);
            format(line, "NVRAM error: %08lx", (unsigned long)menu->nvram_info.last_error & 0xffffffffUL); tool_line(menu, row++, line, MUTED);
            tool_line(menu, row, "Results apply to this session.", MUTED);
        } else {
            format(line, "Commands sent: %lu", menu->bus.commands); tool_line(menu, 0, line, TEXT);
            format(line, "Last command: %02x (%lu bytes)", menu->bus.last_command, (unsigned long)menu->bus.last_length); tool_line(menu, 1, line, TEXT);
            format(line, "Reply: %02x %02x %02x %02x", menu->bus.last_reply[0], menu->bus.last_reply[1], menu->bus.last_reply[2], menu->bus.last_reply[3]); tool_line(menu, 2, line, TEXT);
            format(line, "XBUS error: %08lx", (unsigned long)menu->bus.last_error & 0xffffffffUL); tool_line(menu, 3, line, MUTED);
            format(line, "Transport timed out: %s", menu->bus.timed_out ? "YES" : "no"); tool_line(menu, 4, line, MUTED);
            format(line, "Navigation blocked: %s", menu->ode.navigation_failed ? "YES" : "no"); tool_line(menu, 5, line, MUTED);
            format(line, "Last fault: cmd %02x status %02x", menu->bus.failed_command, menu->bus.failed_status); tool_line(menu, 6, line, MUTED);
            format(line, "Fault error: %08lx", (unsigned long)menu->bus.failure_error & 0xffffffffUL); tool_line(menu, 7, line, MUTED);
            format(line,"Cover: %lums / %lu cmds%s",menu->cover_load_ms,menu->cover_commands,menu->cover_cached ? " RAM" : ""); tool_line(menu,8,line,MUTED);
            format(line,"Diagnostic export: %d",menu->diagnostics_result); tool_line(menu,9,line,MUTED);
        }
    } else if (menu->tools == TOOL_CONFIRM_BACKUP) {
        slot_path(line, menu->backup_slot); tool_line(menu, 0, line, TEXT);
        format(line, "Back up %lu save file(s)?", (unsigned long)menu->pending.count); tool_line(menu, 2, line, TEXT);
        if (menu->backup_slot_valid) format(line,"Replaces %lu existing save(s).",(unsigned long)menu->backup.count);
        else strcpy(line,"Replaces this USB backup slot.");
        tool_line(menu,4,line,AMBER);
        tool_line(menu, 6, "Internal saves stay unchanged.", ACCENT);
    } else if (menu->tools == TOOL_CONFIRM_RESTORE) {
        const SaveEntry *e = &menu->backup.entries[menu->save_selected];
        tool_line(menu, 0, e->name, TEXT);
        format(line, "%lu bytes / type %08lx", (unsigned long)e->size, e->type); tool_line(menu, 1, line, MUTED);
        tool_line(menu, 3, "Restore this save to NVRAM?", AMBER);
        tool_line(menu, 4, "Replaces the matching save.", AMBER);
        tool_line(menu, 6, "First: verified recovery backup.", TEXT);
        tool_line(menu, 7, menu->backup_slot == 8 ? "Replaces USB recovery2.3ds." : "Replaces USB recovery.3ds.", MUTED);
        tool_line(menu, 9, "Keep power on until finished.", AMBER);
    }
    rectangle(menu, 16, 208, 288, 1, LINE);
    {
        const char *controls = "A:Select B:Back";
        if (menu->tools == TOOL_GROUP) controls = "A:Queue set X:Only this B:Cancel";
        else if (menu->tools == TOOL_FAVORITES) controls = menu->favorites_move ? "Up/Down:Move A/R:Save B:Cancel" : "A:Open C:Add X:Remove B:Back";
        else if (menu->tools == TOOL_RESULTS) controls = "A:Open Start:Edit X:Reload B:Back";
        else if (menu->tools == TOOL_FIND) controls = "Arrows:Choose A:Key B:Cancel";
        else if (menu->tools == TOOL_CONFIG) controls = "Left/Right:Change C:Save B:Back";
        else if (menu->tools == TOOL_BACKUP_DETAILS) controls = "A:Continue C:Old/New B:Back";
        else if (menu->tools == TOOL_SAVES) controls = "A:Back up one C:All X:Refresh";
        else if (menu->tools == TOOL_ARCHIVE) controls = "A:Restore selected B:Back";
        else if (menu->tools == TOOL_CONFIRM_BACKUP || menu->tools == TOOL_CONFIRM_RESTORE) controls = "A:Confirm B:Cancel";
        else if (menu->tools == TOOL_DIAG) controls = "C:Export USB B:Back";
        else if (menu->tools == TOOL_HELP) controls = "B:Back";
        else if (menu->tools == TOOL_RECENT) controls = "A:Open folder Start:Reload B:Back";
        if (menu->tools==TOOL_HOME && !menu->tool_message) {
            button(menu,16,212,"A","Select",ACCENT); button(menu,110,212,"B","Back",MUTED);
            label(menu,207,214,"Arrows to browse",MUTED,UI_FONT_SMALL);
        } else text(menu,16,212,menu->tool_message ? menu->tool_message : controls,menu->tool_message ? AMBER : ACCENT);
    }
    text(menu, 16, 224, menu->tools == TOOL_FAVORITES ? "R:Move order   Start:Reload" :
         ((menu->tools == TOOL_DIAG || menu->tools == TOOL_HELP) ? "Left/Right: More pages" : "L + R  Return to library"), MUTED);
    DisplayScreen(menu->screen.sc_Screens[menu->screen.sc_curScreen], 0);
}

typedef struct SaveRootOperation {
    Menu *menu;
    int action; /* 0 load, 1 save, 2 disposable write test, 3 settings */
    const char *path;
    SaveArchive *archive;
} SaveRootOperation;
static void save_progress(void *context, size_t bytes)
{
    Menu *menu = (Menu *)context;
    menu->read_count += bytes;
    format(menu->status, "USB transfer / verify: %lu KiB", (unsigned long)((menu->read_count + 1023) / 1024));
    menu->tool_message = menu->status;
    draw(menu);
}
static int save_root_callback(void *context)
{
    SaveRootOperation *op = (SaveRootOperation *)context;
    Ode *ode = &op->menu->ode;
    OdeProgress previous_progress = ode->progress;
    void *previous_context = ode->progress_context;
    int result;
    op->menu->read_count = 0;
    ode->progress = save_progress;
    ode->progress_context = op->menu;
    if (op->action == 3) {
        MenuConfig settings;
        settings_values(op->menu,&settings);
        result = menu_settings_save(ode,ode_bus_read_buffer,&settings);
    }
    else if (op->action == 2) result = save_usb_test(ode, ode_bus_read_buffer);
    else if (op->action == 1) result = save_usb_store(ode, ode_bus_read_buffer, op->path, op->archive);
    else result = save_usb_load(ode, ode_bus_read_buffer, op->path, op->archive);
    ode->progress = previous_progress;
    ode->progress_context = previous_context;
    return result;
}
static int save_root_operation(Menu *menu, int action, const char *path, SaveArchive *archive)
{
    ArtRouteStep route[MAX_DEPTH];
    SaveRootOperation op;
    unsigned int i;
    int result;
    if (menu->demo || !menu->ready || menu->bus.timed_out || menu->ode.navigation_failed ||
        strcmp(menu->ode.identity, "LOCODE") || menu->config_result == 2) {
        menu->tool_message = "USB save access unavailable."; return -1;
    }
    /* A virtual device list has no current filesystem for relative file I/O. */
    if (!menu->depth) for (i = 0; i < menu->listing.count; ++i)
        if (!(menu->listing.entries[i]->toc_id & 0xffffffUL)) {
            menu->tool_message = "Enter a USB volume first."; return -1;
        }
    for (i = 0; i < menu->depth; ++i) { route[i].id = menu->path_ids[i]; route[i].name = menu->path[i]; }
    op.menu = menu; op.action = action; op.path = path; op.archive = archive;
    menu->tool_message = action ? "Writing / verifying USB..." : "Reading / validating backup...";
    draw(menu);
    result = art_at_root(&menu->ode, route, menu->depth, &menu->listing,
                         menu->config.folder_workaround, save_root_callback, &op);
    if (result == -2 || menu->bus.timed_out || menu->ode.navigation_failed) {
        menu->ready = menu->view_valid = 0;
        menu->tool_message = "USB I/O failed. Restart console.";
        return -1;
    }
    menu->tool_message = result < 0 ? "USB failed. Check /saves folder." : NULL;
    return result;
}
static void scan_nvram(Menu *menu)
{
    menu->tool_message = "Reading internal saves..."; draw(menu);
    save_archive_free(&menu->nvram);
    if (nvram_snapshot(&menu->nvram, &menu->nvram_info) < 0) menu->tool_message = menu->nvram_info.error;
    else menu->tool_message = NULL;
    menu->tool_selected = 0;
}
static int test_save_usb(Menu *menu)
{
    int result = save_root_operation(menu, 2, NULL, NULL);
    menu->usb_tested = result < 0 ? -1 : 1;
    return result;
}
static void restore_selected(Menu *menu)
{
    SaveArchive current;
    SaveArchive verified;
    const SaveEntry *entry = &menu->backup.entries[menu->save_selected];
    NvramInfo info;
    const char *recovery = menu->backup_slot == 8 ? "saves/recovery2.3ds" : "saves/recovery.3ds";
    int result = -1;
    memset(&current, 0, sizeof(current)); memset(&verified, 0, sizeof(verified));
    menu->restore_result = -1;
    if (menu->usb_tested != 1 && test_save_usb(menu) < 0) return;
    if (nvram_preflight(entry, &menu->nvram_info) < 0) { menu->tool_message = menu->nvram_info.error; return; }
    if (nvram_snapshot(&current, &menu->nvram_info) < 0) { menu->tool_message = menu->nvram_info.error; return; }
    /* The complete current filesystem must reach USB and be reread before the
     * first write to NVRAM. A directory-restore failure also cancels restore. */
    if (save_root_operation(menu, 1, recovery, &current) < 0) goto done;
    menu->tool_message = "Restoring internal save..."; draw(menu);
    if (nvram_restore(entry, &menu->nvram_info) < 0) { menu->tool_message = menu->nvram_info.error; goto done; }
    /* Confirm filename, type, length and payload in a fresh complete snapshot. */
    if (nvram_snapshot(&verified, &info) == 0 && save_restore_matches(&current, &verified, entry)) result = 0;
    menu->restore_result = result ? -1 : 1;
    menu->tool_message = result ? "Verify failed. Keep recovery backup." : "Save restored and verified.";
done:
    save_archive_free(&current); save_archive_free(&verified);
}
static void tools_input(Menu *menu, u32 pressed)
{
    size_t count = 0;
    int page = menu->tools;
    if (page == TOOL_FIND && !(pressed & ControlB)) { find_input(menu,pressed); return; }
    if (page == TOOL_FAVORITES && menu->favorites_move) {
        if (pressed & ControlB) cancel_favorites_move(menu);
        else if (pressed & (ControlRightShift | ControlA)) save_favorites_order(menu);
        else if (pressed & (ControlUp | ControlDown)) {
            size_t to = menu->tool_selected;
            if ((pressed & ControlUp) && to) --to;
            if ((pressed & ControlDown) && to+1 < menu->favorites.count) ++to;
            if (!menu_favorites_move(&menu->favorites,menu->tool_selected,to)) menu->tool_selected = to;
        }
        return;
    }
    if (page == TOOL_FAVORITES && (pressed & ControlRightShift)) { begin_favorites_move(menu); return; }
    if (pressed & ControlB) {
        menu->tool_message = NULL;
        if (page == TOOL_HOME) { menu->tools = 0; selection_changed(menu); }
        else if (page == TOOL_RESULTS) { menu->tools=TOOL_FIND; menu->tool_selected=0; }
        else if (page == TOOL_GROUP) { menu->tools = 0; ode_playlist_dispose(&menu->group); selection_changed(menu); }
        else if (page == TOOL_BACKUP_DETAILS) { menu->tools = TOOL_SLOTS; menu->tool_selected = menu->backup_slot; }
        else if (page == TOOL_ARCHIVE) { menu->tools = TOOL_SLOTS; menu->backup_mode = 0; menu->tool_selected = menu->backup_slot; }
        else if (page == TOOL_CONFIRM_RESTORE) { menu->tools = TOOL_ARCHIVE; menu->tool_selected = menu->save_selected; }
        else if (page == TOOL_CONFIRM_BACKUP) { menu->tools = TOOL_SLOTS; menu->tool_selected = menu->backup_slot; }
        else { menu->tools = TOOL_HOME; menu->tool_selected = 0; save_archive_free(&menu->pending); }
        return;
    }
    if (page == TOOL_RESULTS) {
        if (pressed & ControlStart) { menu->tools=TOOL_FIND; menu->tool_message=NULL; return; }
        if (pressed & ControlX) { search_library(menu,1); return; }
        count=menu->search_count;
    }
    if (page==TOOL_RESULTS && count) {
        if (pressed & (ControlRight | ControlRightShift)) {
            menu->tool_selected+=6; if (menu->tool_selected>=count) menu->tool_selected=count-1;
        } else if (pressed & (ControlLeft | ControlLeftShift))
            menu->tool_selected=menu->tool_selected>=6 ? menu->tool_selected-6 : 0;
    }
    if (page == TOOL_HOME) count = 8;
    if (page == TOOL_SAVES) count = menu->nvram.count;
    if (page == TOOL_ARCHIVE) count = menu->backup.count;
    if (page == TOOL_SLOTS) count = menu->backup_mode ? 8 : 10;
    if (page == TOOL_CONFIG) count = 12;
    if (page == TOOL_GROUP) count = menu->group.count;
    if (page == TOOL_FAVORITES) count = menu->favorites.count;
    if (page == TOOL_RECENT) count = menu->recent.count;
    if (page == TOOL_BACKUP_DETAILS) count = menu->details_new ? menu->pending.count : menu->backup.count;
    if (page==TOOL_HOME) {
        size_t column=menu->tool_selected/4, row=menu->tool_selected%4;
        if (pressed & (ControlLeft | ControlRight)) column=!column;
        if (pressed & ControlUp) row=(row+3)%4;
        if (pressed & ControlDown) row=(row+1)%4;
        menu->tool_selected=column*4+row;
    } else if (count) {
        if (pressed & ControlUp) menu->tool_selected = menu->tool_selected ? menu->tool_selected - 1 : count - 1;
        if (pressed & ControlDown) menu->tool_selected = (menu->tool_selected + 1) % count;
    }
    if (page == TOOL_HELP) {
        if (pressed & (ControlLeft | ControlRight)) menu->tool_selected = !menu->tool_selected;
        return;
    }
    if (page == TOOL_CONFIG && (pressed & ControlX)) { discard_config(menu); return; }
    if (page == TOOL_CONFIG && (pressed & ControlC)) { save_config(menu); return; }
    if (page == TOOL_CONFIG && (pressed & (ControlA | ControlLeft | ControlRight))) {
        change_config(menu,(pressed & ControlLeft) ? -1 : 1); return;
    }
    if (page == TOOL_RECENT && (pressed & ControlStart)) { open_recent(menu); return; }
    if (page == TOOL_FAVORITES && (pressed & ControlStart)) { open_favorites(menu); return; }
    if (page == TOOL_FAVORITES && (pressed & (ControlC | ControlX))) {
        edit_favorites(menu,(pressed & ControlX) != 0); return;
    }
    if (page == TOOL_BACKUP_DETAILS && (pressed & ControlC)) { menu->details_new = !menu->details_new; menu->tool_selected = 0; return; }
    if (page == TOOL_GROUP && (pressed & ControlX)) {
        ode_playlist_dispose(&menu->group); menu->tools = 0; menu->skip_group = 1; activate(menu); return;
    }
    if (page == TOOL_DIAG) {
        if (pressed & ControlC) { export_diagnostics(menu); return; }
        if (pressed & ControlRight) menu->tool_selected = (menu->tool_selected + 1) % 3;
        if (pressed & ControlLeft) menu->tool_selected = (menu->tool_selected + 2) % 3;
        return;
    }
    if (page == TOOL_SAVES && (pressed & ControlX)) { scan_nvram(menu); return; }
    if (page == TOOL_SAVES && (pressed & (ControlA | ControlC))) {
        /* Snapshot again at the moment the user requests a backup. */
        size_t selected = menu->tool_selected;
        char selected_name[SAVE_NAME_BYTES];
        selected_name[0] = 0;
        if (selected < menu->nvram.count) strcpy(selected_name, menu->nvram.entries[selected].name);
        scan_nvram(menu);
        if (menu->tool_message || !menu->nvram.count) return;
        if (!(pressed & ControlC)) {
            for (selected = 0; selected < menu->nvram.count; ++selected)
                if (!strcmp(selected_name, menu->nvram.entries[selected].name)) break;
            if (selected == menu->nvram.count) { menu->tool_message = "Save list changed. Select again."; return; }
        }
        save_archive_free(&menu->pending);
        if (save_archive_build(&menu->pending, (pressed & ControlC) ? menu->nvram.entries : &menu->nvram.entries[selected],
                (pressed & ControlC) ? menu->nvram.count : 1) < 0) { menu->tool_message = "Cannot prepare backup."; return; }
        menu->backup_mode = 1; menu->tools = TOOL_SLOTS; menu->tool_selected = 0; return;
    }
    if (!(pressed & ControlA)) return;
    menu->tool_message = NULL;
    if (page == TOOL_HOME) {
        size_t selected = menu->tool_selected;
        menu->tool_selected = 0;
        if (!selected) { menu->tools = TOOL_SAVES; scan_nvram(menu); }
        else if (selected == 1) { menu->tools = TOOL_SLOTS; menu->backup_mode = 0; }
        else if (selected == 2) { menu->tools = TOOL_FAVORITES; open_favorites(menu); }
        else if (selected == 3) menu->tools = TOOL_CONFIG;
        else if (selected == 4) menu->tools = TOOL_HELP;
        else if (selected == 5) menu->tools = TOOL_DIAG;
        else if (selected == 6) { menu->tools = TOOL_RECENT; open_recent(menu); }
        else { menu->tools = TOOL_FIND; menu->find_key = 0; }
    } else if (page == TOOL_SLOTS) {
        char path[40];
        menu->backup_slot = (unsigned int)menu->tool_selected;
        if (menu->backup_mode) {
            slot_path(path,menu->backup_slot); save_archive_free(&menu->backup);
            menu->backup_slot_valid = !save_root_operation(menu,0,path,&menu->backup);
            if (menu->ode.navigation_failed || menu->bus.timed_out || !menu->ready) return;
            menu->tools = TOOL_BACKUP_DETAILS; menu->tool_selected = 0; menu->details_new = 0; menu->tool_message = NULL;
        } else {
            slot_path(path, menu->backup_slot); save_archive_free(&menu->backup);
            if (!save_root_operation(menu, 0, path, &menu->backup)) {
                menu->tools = TOOL_ARCHIVE; menu->tool_selected = 0;
            } else if (menu->tool_message && !strcmp(menu->tool_message, "USB failed. Check /saves folder."))
                menu->tool_message = "No valid backup in this slot.";
        }
    } else if (page == TOOL_GROUP) {
        if (!menu->playlist.count && menu->playlist.synchronized) {
            menu->playlist = menu->group; ode_playlist_init(&menu->group);
            menu->tools = 0; review_queue(menu);
        }
    } else if (page == TOOL_RESULTS && count) {
        const char *path=menu->catalog.paths[menu->search_matches[menu->tool_selected]];
        if (path) open_saved_path(menu,path);
    } else if (page == TOOL_FAVORITES || page == TOOL_RECENT) open_favorite(menu);
    else if (page == TOOL_BACKUP_DETAILS) menu->tools = TOOL_CONFIRM_BACKUP;
    else if (page == TOOL_ARCHIVE && count) {
        menu->save_selected = menu->tool_selected; menu->tools = TOOL_CONFIRM_RESTORE;
    } else if (page == TOOL_CONFIRM_BACKUP) {
        char path[40];
        slot_path(path, menu->backup_slot); menu->backup_result = -1;
        if ((menu->usb_tested == 1 || !test_save_usb(menu)) &&
            !save_root_operation(menu, 1, path, &menu->pending)) {
            menu->backup_result = 1; menu->tool_message = "Backup saved and verified.";
        }
        menu->tools = TOOL_SLOTS; menu->tool_selected = menu->backup_slot;
    } else if (page == TOOL_CONFIRM_RESTORE) {
        restore_selected(menu); menu->tools = TOOL_ARCHIVE; menu->tool_selected = menu->save_selected;
    }
}

static void toggle_tools(Menu *menu)
{
    if (menu->favorites_move) cancel_favorites_move(menu);
    if (menu->tools) {
        if (menu->tools==TOOL_GROUP) ode_playlist_dispose(&menu->group);
        save_archive_free(&menu->pending);
        menu->tools=menu->review=0; selection_changed(menu);
    } else menu->tools=TOOL_HOME;
    menu->tool_selected=0; menu->tool_message=NULL;
    menu->c_pending=menu->alphabet_active=0;
}

int main(int argc, char **argv)
{
    static Menu menu;
    u32 previous = 0;
    unsigned int held = 0;
    (void)argc;
    (void)argv;
    menu_config_defaults(&menu.config);
    menu.settings_usb_covers = menu.config.usb_covers;
    menu.settings_remember_position = menu.config.remember_position;
    menu.settings_recent_games = menu.config.recent_games;
    menu.settings_video_previews = menu.config.video_previews;
    menu.saved_config = menu.config;
    menu.config_result = 3;
    menu.bus.device = menu.bus.request = menu.bus.timer = -1;
    menu.ode.transfer = ode_bus_transfer;
    menu.ode.context = &menu.bus;
    menu.ode.progress = progress;
    menu.ode.progress_context = &menu;
    if (InitControlPad(1) < 0) return 1;
    if (CreateBasicDisplay(&menu.screen, DI_TYPE_DEFAULT, 2) < 0) {
        KillControlPad();
        return 1;
    }
    menu.vbl = CreateVBLIOReq();
    if (menu.vbl < 0) { DeleteBasicDisplay(&menu.screen); KillControlPad(); return 1; }
    menu.clock = GetTimerIOReq();
    if (menu.clock < 0) { DeleteIOReq(menu.vbl); DeleteBasicDisplay(&menu.screen); KillControlPad(); return 1; }
    {
        Font *font = GetCurrentFont();
        if (font && font->font_CCB) { menu.font_ccb = *font->font_CCB; menu.have_font = 1; }
    }
    menu.covers_enabled = menu_cover_count != 0;
#ifndef MENU_DEMO
    menu.splash=LoadCel("$boot/startup.cel",MEMTYPE_CEL);
    if (menu.splash) menu.splash->ccb_Flags |= CCB_LAST;
    menu.booting=1; draw(&menu);
#endif
    menu.logo = LoadCel("$boot/logo.cel", MEMTYPE_CEL);
    if (menu.logo) {
        menu.logo->ccb_XPos = 16 << 16;
        menu.logo->ccb_YPos = 8 << 16;
        menu.logo->ccb_Flags |= CCB_LAST;
    }
#ifdef MENU_DEMO
    show_demo(&menu);
#else
    connect_ode(&menu);
#endif
    for (;;) {
        ControlPadEventData pad;
        u32 buttons;
        u32 pressed;
        u32 released;
        size_t total;
        size_t *selection;
        size_t old_selection;
        int old_review;
        if (menu.launching) { WaitVBL(menu.vbl, 1); continue; }
        if (GetControlPad(1, 0, &pad) < 0) {
            menu.message = "Controller read failed";
            draw(&menu);
            WaitVBL(menu.vbl, 1);
            continue;
        }
        buttons = pad.cped_ButtonBits;
        menu.suppress_buttons &= buttons;
        buttons &= ~menu.suppress_buttons;
        pressed = buttons & ~previous;
        released = previous & ~buttons;
        if (buttons & (ControlUp | ControlDown | ControlLeft | ControlRight)) {
            if (buttons == previous) ++held;
            else held = 0;
            if (held > 20 && (held % 5) == 0)
                pressed |= buttons & (ControlUp | ControlDown | ControlLeft | ControlRight);
        } else held = 0;
        previous = buttons;
        if (pressed) { stop_preview(&menu); }
        if ((buttons & (ControlLeftShift | ControlRightShift)) ==
                (ControlLeftShift | ControlRightShift) &&
                (pressed & (ControlLeftShift | ControlRightShift))) {
            toggle_tools(&menu);
            pressed = 0;
        }
        if (menu.tools) {
            tools_input(&menu, pressed);
            draw(&menu); WaitVBL(menu.vbl, 1); continue;
        }
        if (menu.ode.navigation_failed) {
            menu.message = "Folder I/O failed. Restart console.";
            draw(&menu);
            WaitVBL(menu.vbl, 1);
            continue;
        }
        if (alphabet_input(&menu,pressed)) {
            draw(&menu); WaitVBL(menu.vbl,1); continue;
        }
        pressed = browser_shortcuts(&menu,buttons,pressed,released);
        total = row_count(&menu);
        selection = menu.review ? &menu.queue_selected : &menu.selected;
        old_selection = *selection;
        old_review = menu.review;
        if (menu.review && menu.queue_move && total && (pressed & (ControlUp | ControlDown))) {
            size_t to = menu.queue_selected;
            if ((pressed & ControlUp) && to) --to;
            if ((pressed & ControlDown) && to + 1 < total) ++to;
            if (!ode_playlist_move_local(&menu.playlist,menu.queue_selected,to)) menu.queue_selected = to;
            pressed &= ~(ControlUp | ControlDown);
        }
        if ((menu.ready || menu.review) && total) {
            if (pressed & ControlUp) *selection = *selection ? *selection - 1 : total - 1;
            else if (pressed & ControlDown) *selection = (*selection + 1) % total;
            else if (pressed & (ControlRight | ControlRightShift)) {
                *selection += menu_rows(&menu);
                if (*selection >= total) *selection = total - 1;
            } else if (pressed & (ControlLeft | ControlLeftShift))
                *selection = *selection >= menu_rows(&menu) ? *selection - menu_rows(&menu) : 0;
        }
        if (menu.review) {
            if (pressed & ControlB) {
                if (menu.queue_move) { menu.queue_move = 0; menu.message = NULL; }
                else { menu.review = 0; menu.message = NULL; }
            } else if ((pressed & ControlX) && (buttons & ControlC)) {
                if (ode_playlist_clear(&menu.ode,&menu.playlist) < 0) menu.message = menu.ode.error;
                else { menu.queue_selected = 0; menu.queue_move = 0; notify(&menu,"Queue cleared. B:Back"); }
            } else if (pressed & ControlX) {
                if (!ode_playlist_remove_local(&menu.playlist,menu.queue_selected)) {
                    if (menu.queue_selected >= menu.playlist.count) menu.queue_selected = menu.playlist.count ? menu.playlist.count-1 : 0;
                    notify(&menu,"Disc removed. A:Play B:Back");
                } else menu.message = "Queue uncertain. C+X:Clear";
            } else if (pressed & ControlC) {
                menu.queue_move = !menu.queue_move;
                menu.message = menu.queue_move ? "Up/Down:Move C/B:Done" : NULL;
            } else if (pressed & (ControlA | ControlStart)) {
                if (menu.queue_move) { menu.queue_move = 0; menu.message = NULL; }
                else start_queue(&menu);
            }
        } else if ((pressed & ControlStart) && !menu.demo &&
                   (menu.ready || menu.playlist.count || menu.ode.identity[0])) {
            review_queue(&menu);
        } else if (pressed & ControlC) {
            if (menu.demo) connect_ode(&menu);
            else if (!menu.ready) show_demo(&menu);
            else queue_selected_disc(&menu);
        } else if (pressed & ControlX) {
            cover_cache_clear(&menu.cover_cache);
            if (*menu.filter) { menu.filter[0] = 0; menu.selected = 0; menu.message = NULL; selection_changed(&menu); }
            else if (menu.demo) show_demo(&menu);
            else if (menu.ode.identity[0]) load_directory(&menu, menu.selected);
            else connect_ode(&menu);
        } else if (pressed & ControlB) go_back(&menu);
        else if (pressed & ControlA) activate(&menu);
        if (old_review != menu.review || old_selection !=
            (menu.review ? menu.queue_selected : menu.selected)) selection_changed(&menu);
        remember_position(&menu,0);
        update_cover(&menu);
        update_preview(&menu);
        draw(&menu);
        WaitVBL(menu.vbl, 1);
    }
}
