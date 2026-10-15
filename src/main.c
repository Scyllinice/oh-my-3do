#include "ode.h"
#include "ode_bus.h"
#include "menu_config.h"
#include "menu_ui.h"
#include "usb_art.h"
#include "art_navigation.h"
#include "save_usb.h"
#include "nvram_io.h"
#include "library_features.h"
#include "library_navigation.h"
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

#define MENU_VERSION "0.5.2"
#define ROWS 10
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
#define TOOL_JUMP 12
#define TOOL_BACKUP_DETAILS 13
#define TOOL_RECENT 14
#define TOOL_FIND 15
#define MAX_DEPTH 32
#define BG (menu->config.theme == 1 ? MakeRGB15(0,0,0) : MakeRGB15(1,2,4))
#define TEXT (menu->config.theme == 1 ? MakeRGB15(31,31,31) : MakeRGB15(29,29,26))
#define MUTED (menu->config.theme == 1 ? MakeRGB15(27,27,27) : MakeRGB15(17,20,23))
#define ACCENT (menu->config.theme ? MakeRGB15(31,25,10) : MakeRGB15(7,26,22))
#define PANEL MakeRGB15(2, 4, 7)
#define LINE MakeRGB15(5, 9, 12)
#define AMBER MakeRGB15(30, 22, 9)

typedef struct Menu {
    ScreenContext screen;
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
    char favorites_prefix[ODE_MAX_NAME + 10];
    int favorites_valid;
    MenuConfig config;
    MenuConfig saved_config;
    /* Stage navigation/artwork changes until reboot, including while saving. */
    int settings_folder_workaround;
    int settings_usb_covers;
    int settings_remember_position;
    int settings_recent_games;
    UsbArtIndex usb_art;
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
static void draw_tools(Menu *menu);
static void tools_input(Menu *menu, u32 pressed);
static int offer_group(Menu *menu, const MenuEntry *entry);
static void activate(Menu *menu);
static int apply_queue(Menu *menu);
static u32 browser_shortcuts(Menu *menu, u32 buttons, u32 pressed, u32 released);
static void open_favorites(Menu *menu);
static void jump_letter(Menu *menu, int letter);
static void draw_cel(Menu *menu, CCB *cel);
static int save_root_operation(Menu *menu, int action, const char *path, SaveArchive *archive);
static void load_position(Menu *menu);
static void remember_position(Menu *menu, int force);
static void record_launch(Menu *menu, const char *path);
static int config_dirty(const Menu *menu);
static size_t menu_rows(const Menu *menu) { return menu->config.large_titles ? 7 : ROWS; }

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

static void selection_changed(Menu *menu)
{
    menu->selection_since = now_ms(menu);
    menu->position_dirty = 1;
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
static void text(Menu *menu, int x, int y, const char *string, Color color)
{
    GrafCon gc;
    char safe[37];
    size_t i;
    int margin = menu->config.screen_margin;
    int advance = menu->large_drawing ? 12 : 8;
    CCB font;
    for (i = 0; i < 36 && string[i]; ++i) {
        unsigned char c = (unsigned char)string[i];
        safe[i] = c >= 32 && c <= 126 ? (char)c : '?';
    }
    safe[i] = 0;
    if (menu->have_font) {
        font = menu->font_ccb;
        font.ccb_HDX = (long)((1UL << 20) * (320 - 2 * margin) / 320);
        font.ccb_VDY = (long)((1UL << 16) * (240 - 2 * margin) / 240);
        if (menu->large_drawing) { font.ccb_HDX = font.ccb_HDX * 3 / 2; font.ccb_VDY = font.ccb_VDY * 3 / 2; }
        SetCurrentFontCCB(&font);
    }
    SetFGPen(&gc, color);
    if (!menu->large_drawing && !margin) {
        MoveTo(&gc,x,y);
        DrawText8(&gc,menu->screen.sc_BitmapItems[menu->screen.sc_curScreen],(const u8 *)safe);
    } else for (i = 0; safe[i]; ++i) {
        char one[2]; one[0] = safe[i]; one[1] = 0;
        if (x + (int)(i+1) * advance > 308) break;
        MoveTo(&gc,margin + (x + (int)i * advance) * (320 - 2 * margin) / 320,
                   margin + y * (240 - 2 * margin) / 240);
        DrawText8(&gc,menu->screen.sc_BitmapItems[menu->screen.sc_curScreen],(const u8 *)one);
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
    rectangle(menu, x, y, 4, 1, color);
    rectangle(menu, x, y + 2, 8, 5, color);
    rectangle(menu, x + 1, y + 3, 6, 3, PANEL);
}

static void draw(Menu *menu)
{
    char line[80];
    size_t total = row_count(menu);
    size_t selected = menu->review ? menu->queue_selected : menu->selected;
    size_t rows = menu_rows(menu);
    size_t start = (selected / rows) * rows;
    size_t i;
    unsigned long now = now_ms(menu);
    int artwork = menu->covers_enabled && !menu->review && menu->view_valid && total;
    const char *message = menu->message;
    if (menu->tools) { draw_tools(menu); return; }
    if (menu->notice_active && now - menu->notice_since >= 2500UL)
        menu->notice_active = 0;
    if (!message && menu->notice_active) message = menu->notice;
    menu->screen.sc_curScreen ^= 1;
    rectangle(menu, 0, 0, 320, 240, BG);
    if (menu->logo)
        draw_cel(menu, menu->logo);
    else text(menu, 16, 28, "OH MY 3DO", TEXT);
    text(menu, 192, 17, menu->demo ? "DEMO MODE" :
         (menu->review ? "DISC QUEUE" : "GAME LIBRARY"), MUTED);
    format(line, "%2lu / 16 DISCS", (unsigned long)menu->playlist.count);
    text(menu, 192, 35, line, menu->playlist.count ? AMBER : ACCENT);
    if (menu->config.folder_workaround) text(menu,192,48,config_dirty(menu) ? "COMPAT ON *" : "COMPAT ON",AMBER);
    else if (config_dirty(menu)) text(menu,192,48,"SETTINGS *",AMBER);
    rectangle(menu, 12, 60, 296, 132, PANEL);
    rectangle(menu, 12, 60, 2, 15, ACCENT);
    text(menu, 21, 64, menu->review ? "PLAY ORDER / FIRST TO LAST" :
         menu->breadcrumb, TEXT);
    if (*menu->filter && !menu->review) {
        rectangle(menu,20,61,280,15,PANEL);
        format(line,"Find: %s",menu->filter); text(menu,21,64,line,ACCENT);
    }
    rectangle(menu, 20, 77, 280, 1, LINE);
    if (menu->view_valid || menu->review) {
        for (i = start; i < total && i < start + rows; ++i) {
            const char *name;
            int directory;
            int y = 83 + (int)(i - start) * (menu->config.large_titles ? 15 : 11);
            if (menu->review) {
                name = menu->playlist.paths[i];
                /* Keep folder context visible for Disc 1/game.iso and
                 * Disc 2/game.iso, which otherwise have identical labels. */
                if (!strncmp(name, "/Storage/", 9)) name += 9;
                directory = 0;
            } else if (menu->listing.has_parent && i == 0) {
                name = ".. (parent folder)";
                directory = 1;
            } else {
                const MenuEntry *entry = browser_entry(menu, i - (menu->listing.has_parent ? 1 : 0));
                name = entry->name;
                directory = entry->is_directory;
            }
            if (i == selected) {
                rectangle(menu, 16, y - 1, artwork ? 208 : 287, menu->config.large_titles ? 14 : 10, MakeRGB15(4, 10, 13));
                rectangle(menu, 16, y - 1, 1, 10, ACCENT);
            }
            if (menu->review) {
                /* Leave room for a full machine-word value as well as the
                 * normal two-digit disc index. */
                char number[24];
                format(number, "%2lu", (unsigned long)i + 1);
                menu->large_drawing = menu->config.large_titles;
                text(menu, 23, y, number, AMBER);
                menu->large_drawing = 0;
            } else if (directory) folder_icon(menu, 23, y + 1, ACCENT);
            else {
                rectangle(menu, 25, y + 2, 4, 4, MUTED);
                rectangle(menu, 26, y + 3, 2, 2, PANEL);
            }
            /* Leave room for icons and the safe area at the right edge. */
            {
                char label[34];
                size_t length = menu_title_length(name, directory);
                size_t offset = 0;
                size_t visible = menu->config.large_titles ? (artwork ? 15 : (menu->review ? 20 : 22)) : (artwork ? 23 : (menu->review ? 31 : 32));
                if (i == selected)
                    offset = menu_scroll_offset(length, visible, now - menu->selection_since);
                if (visible > length - offset) visible = length - offset;
                memcpy(label, name + offset, visible);
                label[visible] = 0;
                menu->large_drawing = menu->config.large_titles;
                text(menu, menu->review ? (menu->config.large_titles ? 58 : 46) : 38, y, label, i == selected ? TEXT : MUTED);
                menu->large_drawing = 0;
            }
        }
        if (!total) {
            text(menu, 24, 109, menu->review ? "Your disc queue is empty." : (*menu->filter ? "No matching names." : "This folder is empty."), TEXT);
            text(menu, 24, 131, menu->review ? "B:Browse   C:Add a disc" : "B:Return to parent folder", MUTED);
        }
        format(line, "%lu / %lu",
                (unsigned long)(total ? selected + 1 : 0),
                (unsigned long)total);
        text(menu, 16, 196, line, MUTED);
        format(line, "P%lu/%lu", (unsigned long)(total ? selected / rows + 1 : 1),
                (unsigned long)(total ? (total + rows - 1) / rows : 1));
        text(menu, 304 - (int)strlen(line) * 8, 196, line, MUTED);
    }
    if (artwork) {
        rectangle(menu, 232, 82, 72, 88, LINE);
        rectangle(menu, 234, 84, 68, 84, BG);
        if (menu->cover) draw_cel(menu, menu->cover);
        else {
            folder_icon(menu, 264, 109, MUTED);
            text(menu, 244, 140, "IMAGE", MUTED);
        }
        text(menu, 244, 178, "COVER", MUTED);
    }
    rectangle(menu, 16, 208, 288, 1, LINE);
    if (menu->loading) {
        if (menu->read_count) format(line, "Reading directory... %lu", (unsigned long)menu->read_count);
        else strcpy(line, "Reading directory...");
        message = line;
    }
    text(menu, 16, 212, message ? message :
         (menu->demo ? "Demo only. C: Connect to ODE" :
          (menu->review ? "A:Play C:Move X:Remove B:Back" : "A:Open C:Add disc Start:Queue")),
         message ? AMBER : ACCENT);
    if (!menu->ready && menu->bus.last_error < 0) {
        format(line, "XBUS error: %08lx", (unsigned long)menu->bus.last_error);
        text(menu, 16, 224, line, MUTED);
    } else text(menu, 16, 224, menu->review ? "C+X:Clear all / same game discs" : (*menu->filter ? "B:Back X:Clear find L+R:Tools" : "B:Back X:Refresh L+R:Tools"), MUTED);
    DisplayScreen(menu->screen.sc_Screens[menu->screen.sc_curScreen], 0);
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
                menu->covers_enabled = 0;
                return; /* virtual device list is not a filesystem root */
            }
    }
    usb_art_index_free(&menu->usb_art);
    menu->covers_enabled = 0;
    if (menu->skip_usb_art || strcmp(menu->ode.identity, "LOCODE")) return;
    message = menu->message;
    menu->message = "Reading cover index...";
    draw(menu);
    usb_art_index_load(&menu->ode, ode_bus_read_buffer, &menu->usb_art);
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

static int read_usb_cover(void *context)
{
    UsbCoverRead *read = (UsbCoverRead *)context;
    return usb_art_pixels(&read->menu->ode, ode_bus_read_buffer, read->filename, read->pixels);
}

static void update_cover(Menu *menu)
{
    const MenuEntry *entry;
    const char *file;
    char path[9218];
    const char *lookup;
    if (menu->tools || !menu->covers_enabled || menu->review || !menu->ready || menu->loading ||
        menu->cover_attempted || now_ms(menu) - menu->selection_since < 350UL) return;
    menu->cover_attempted = 1;
    if (!row_count(menu) || (menu->listing.has_parent && !menu->selected)) return;
    entry = browser_entry(menu, menu->selected - (menu->listing.has_parent ? 1 : 0));
    if (entry->is_directory || directory_path(menu, path, 8193) < 0) return;
    strcat(path, "/");
    strcat(path, entry->name);
    if (menu->demo) {
        file = menu_cover_lookup(path);
        if (!file) return;
        menu->cover = LoadCel((char *)file, MEMTYPE_CEL);
    } else {
        ArtRouteStep route[MAX_DEPTH];
        UsbCoverRead read;
        CCB *cover;
        const char *message;
        unsigned int i;
        int result;
        unsigned long started;
        lookup = path + 9; /* remove /Storage/ */
        if (menu->depth && !(menu->path_ids[0] & 0xffffffUL))
            lookup += strlen(menu->path[0]) + 1; /* device labels are not USB path components */
        file = menu_cover_find(menu->usb_art.entries, menu->usb_art.count, lookup);
        if (!file) return;
        cover = CreateCel(64, 80, 16, CREATECEL_UNCODED, NULL);
        if (!cover) return;
        for (i = 0; i < menu->depth; ++i) {
            route[i].id = menu->path_ids[i];
            route[i].name = menu->path[i];
        }
        read.menu = menu;
        read.filename = file;
        read.pixels = (unsigned char *)cover->ccb_SourcePtr;
        message = menu->message;
        menu->message = "Loading cover...";
        draw(menu);
        started = now_ms(menu);
        result = art_at_root(&menu->ode, route, menu->depth, &menu->listing,
                             menu->config.folder_workaround, read_usb_cover, &read);
        menu->selection_since += now_ms(menu) - started;
        menu->message = message;
        if (result < 0) UnloadCel(cover);
        else menu->cover = cover;
        if (result == -2 || menu->bus.timed_out) {
            menu->ode.navigation_failed = 1;
            menu->ready = menu->view_valid = 0;
            menu->message = "Cover I/O failed. Restart console.";
            return;
        }
    }
    if (menu->cover) {
        menu->cover->ccb_XPos = 236 << 16;
        menu->cover->ccb_YPos = 86 << 16;
        menu->cover->ccb_Flags |= CCB_LAST | CCB_BGND;
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
    menu->config_result = 3; /* not attempted */
    memset(menu->ode.identity, 0, sizeof(menu->ode.identity));
    memset(menu->ode.revision, 0, sizeof(menu->ode.revision));
    memset(menu->ode.identity_reply, 0, sizeof(menu->ode.identity_reply));
    menu_config_defaults(&menu->config);
    usb_art_index_free(&menu->usb_art);
    menu->covers_enabled = 0;
    menu->skip_usb_art = 0;
    menu->ode.navigation_known = 0;
    menu->ode.navigation_failed = 0;
    ode_listing_free(&menu->listing);
    menu->message = "Connecting to ODE...";
    draw(menu);
    if (menu->bus.request < 0 && ode_bus_open(&menu->bus) < 0) {
        menu->message = "Cannot open XBUS. C: View demo";
        return;
    }
    if (ode_identify(&menu->ode) < 0 || ode_change_directory(&menu->ode, 0) < 0) {
        menu->message = menu->bus.timed_out ? "No ODE reply. C: View demo" : "ODE not available. C: View demo";
        return;
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
    menu->saved_config = menu->config;
    if (menu->bus.timed_out) {
        menu->message = "Config I/O failed. Restart console.";
        return;
    }
    if (ode_playlist_clear(&menu->ode, &menu->playlist) < 0) {
        menu->message = menu->ode.error;
        return;
    }
    load_directory(menu, 0);
    if (menu->ready && config_result == 0 && menu->config.remember_position) load_position(menu);
    if (menu->ready && config_result < 0) menu->message = "Config not loaded. Compat OFF.";
    else if (menu->ready && config_result == 1) menu->message = "USB config unsupported. Compat OFF.";
    else if (menu->ready && config_result == 2) menu->message = "Config skipped. Compat OFF.";
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
    size_t i;
    int result = -1;
    current_route(menu,route);
    if (menu->bus.timed_out || menu->ode.navigation_failed || !menu->playlist.synchronized) return -1;
    menu->message = "Checking queued game paths..."; draw(menu);
    /* Validate all names/paths before clearing the existing firmware queue. */
    for (i = 0; i < menu->playlist.count; ++i)
        if (library_visit(&menu->ode,route,menu->depth,&menu->listing,menu->config.folder_workaround,
                          menu->playlist.paths[i],NULL,NULL) < 0) return -1;
    ode_playlist_dispose(&menu->group);
    menu->message = "Applying disc queue..."; draw(menu);
    if (ode_playlist_clear(&menu->ode,&menu->group) < 0) goto done;
    for (i = 0; i < menu->playlist.count; ++i)
        if (library_visit(&menu->ode,route,menu->depth,&menu->listing,menu->config.folder_workaround,
                          menu->playlist.paths[i],queue_file_action,menu) < 0) goto done;
    menu->playlist.dirty = 0; result = 0;
done:
    if (result < 0) menu->playlist.synchronized = 0;
    ode_playlist_dispose(&menu->group);
    return result;
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
/* Defer a plain C press until release. Holding C for letter navigation must
 * never also add the selected game to the queue. */
static u32 browser_shortcuts(Menu *menu, u32 buttons, u32 pressed, u32 released)
{
    if (menu->review || menu->demo || !menu->ready) { menu->c_pending = 0; return pressed; }
    if (pressed & (ControlA | ControlB | ControlX | ControlStart)) menu->c_pending = 0;
    if (pressed & ControlC) { menu->c_pending = 1; pressed &= ~ControlC; }
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
static void record_launch(Menu *menu, const char *path)
{
    char *data;
    size_t length;
    remember_position(menu,1);
    if (!menu->config.recent_games || menu->config_result != 0 || !menu->ready ||
        menu->bus.timed_out || menu->ode.navigation_failed || strncmp(path,"/Storage/",9)) return;
    data = (char *)malloc(FAVORITES_BYTES);
    if (!data) return;
    menu->recent_result = -1; menu->recent_valid = 0;
    if (!text_at_root(menu,"recent.txt",NULL,data,FAVORITES_BYTES,&length) &&
        !menu_favorites_parse(&menu->recent,data,length) && menu->recent.count <= 10 &&
        !menu_recent_push(&menu->recent,path+9) && !menu_favorites_format(&menu->recent,data,FAVORITES_BYTES) &&
        !text_at_root(menu,"recent.txt",data,NULL,0,NULL)) {
        menu->recent_result = 1; menu->recent_valid = 1;
    }
    free(data);
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
    format(line,"remember_position=%d\nrecent_games=%d\nunsaved_settings=%d\n",menu->config.remember_position,menu->config.recent_games,config_dirty(menu)); strcat(data,line);
    format(line,"queue_count=%lu\nqueue_synchronized=%d\nqueue_dirty=%d\n",(unsigned long)menu->playlist.count,menu->playlist.synchronized,menu->playlist.dirty); strcat(data,line);
    format(line,"usb_write_check=%d\nbackup_result=%d\nrestore_result=%d\nnvram_error=%08lx\n",menu->usb_tested,menu->backup_result,menu->restore_result,(unsigned long)menu->nvram_info.last_error & 0xffffffffUL); strcat(data,line);
    format(line,"position_result=%d\nrecent_result=%d\nfolder=%.64s\n",menu->position_result,menu->recent_result,menu->breadcrumb); strcat(data,line);
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
}
static int config_dirty(const Menu *menu)
{
    MenuConfig current;
    const MenuConfig *saved = &menu->saved_config;
    settings_values(menu,&current);
    return current.folder_workaround != saved->folder_workaround || current.usb_covers != saved->usb_covers ||
        current.screen_margin != saved->screen_margin || current.large_titles != saved->large_titles ||
        current.theme != saved->theme || current.remember_position != saved->remember_position || current.recent_games != saved->recent_games;
}
static void discard_config(Menu *menu)
{
    menu->config.screen_margin = menu->saved_config.screen_margin;
    menu->config.large_titles = menu->saved_config.large_titles; menu->config.theme = menu->saved_config.theme;
    menu->settings_folder_workaround = menu->saved_config.folder_workaround;
    menu->settings_usb_covers = menu->saved_config.usb_covers;
    menu->settings_remember_position = menu->saved_config.remember_position;
    menu->settings_recent_games = menu->saved_config.recent_games;
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
    if (menu->tool_selected == 9) { save_config(menu); return; }
    if (!menu->tool_selected) menu->config.screen_margin = (menu->config.screen_margin + direction + 13) % 13;
    else if (menu->tool_selected == 1) menu->config.large_titles = !menu->config.large_titles;
    else if (menu->tool_selected == 2) menu->config.theme = (menu->config.theme + direction + 3) % 3;
    else if (menu->tool_selected == 3) menu->settings_folder_workaround = !menu->settings_folder_workaround;
    else if (menu->tool_selected == 4) menu->settings_usb_covers = !menu->settings_usb_covers;
    else if (menu->tool_selected == 5) menu->settings_remember_position = !menu->settings_remember_position;
    else if (menu->tool_selected == 6) menu->settings_recent_games = !menu->settings_recent_games;
    else if (menu->tool_selected == 8) discard_config(menu);
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
        strcpy(menu->filter,menu->find_query); menu->selected = 0; menu->tools = 0;
        menu->message = NULL; selection_changed(menu);
    }
}
static void tool_line(Menu *menu, int row, const char *line, Color color)
{
    text(menu, 22, 82 + row * 11, line, color);
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
    const char *title = "TOOLS";
    size_t i, count = 0, start;
    if (menu->tools == TOOL_SAVES) title = "INTERNAL SAVES";
    if (menu->tools == TOOL_SLOTS) title = menu->backup_mode ? "CHOOSE BACKUP SLOT" : "USB BACKUPS";
    if (menu->tools == TOOL_ARCHIVE) title = "BACKUP CONTENTS";
    if (menu->tools == TOOL_HELP) title = "HELP";
    if (menu->tools == TOOL_DIAG) title = "DIAGNOSTICS";
    if (menu->tools == TOOL_CONFIRM_BACKUP || menu->tools == TOOL_CONFIRM_RESTORE) title = "CONFIRM ACTION";
    if (menu->tools == TOOL_GROUP) title = "MATCHING DISC SET";
    if (menu->tools == TOOL_FAVORITES) title = "USB FAVORITES";
    if (menu->tools == TOOL_CONFIG) title = config_dirty(menu) ? "CONFIGURATION * UNSAVED" : "CONFIGURATION";
    if (menu->tools == TOOL_RECENT) title = "RECENT LAUNCH SELECTIONS";
    if (menu->tools == TOOL_FIND) title = "FIND IN THIS FOLDER";
    if (menu->tools == TOOL_FAVORITES && menu->favorites_move) title = "MOVE FAVORITE";
    if (menu->tools == TOOL_JUMP) title = "JUMP TO LETTER";
    if (menu->tools == TOOL_BACKUP_DETAILS) title = menu->details_new ? "NEW BACKUP CONTENTS" : "EXISTING SLOT CONTENTS";
    menu->screen.sc_curScreen ^= 1;
    rectangle(menu, 0, 0, 320, 240, BG);
    if (menu->logo) draw_cel(menu, menu->logo);
    else text(menu, 16, 28, "OH MY 3DO", TEXT);
    text(menu, 184, 26, "SYSTEM TOOLS", ACCENT);
    rectangle(menu, 12, 60, 296, 144, PANEL);
    text(menu, 22, 64, title, TEXT);
    rectangle(menu, 20, 77, 280, 1, LINE);
    if (menu->tools == TOOL_HOME) {
        static const char *items[] = {"Internal saves / back up", "USB backups / restore", "Favorites", "Jump to letter", "Configuration", "Help / controls", "Diagnostics", "Recent games", "Find by name"};
        for (i = 0; i < sizeof(items) / sizeof(items[0]); ++i) {
            if (i == menu->tool_selected) rectangle(menu, 16, 82 + i * 12, 287, 11, LINE);
            text(menu,24,84+i*12,i == 4 && config_dirty(menu) ? "Configuration *" : items[i],i == menu->tool_selected ? TEXT : MUTED);
        }
    } else if (menu->tools == TOOL_SAVES || menu->tools == TOOL_ARCHIVE || menu->tools == TOOL_SLOTS || menu->tools == TOOL_BACKUP_DETAILS || menu->tools == TOOL_GROUP || menu->tools == TOOL_FAVORITES || menu->tools == TOOL_RECENT) {
        SaveArchive *archive = menu->tools == TOOL_SAVES ? &menu->nvram : (menu->tools == TOOL_BACKUP_DETAILS && menu->details_new ? &menu->pending : &menu->backup);
        count = menu->tools == TOOL_SLOTS ? (menu->backup_mode ? 8 : 10) : (menu->tools == TOOL_GROUP ? menu->group.count : (menu->tools == TOOL_FAVORITES ? menu->favorites.count : (menu->tools == TOOL_RECENT ? menu->recent.count : archive->count)));
        start = menu->tool_selected / 9 * 9;
        for (i = start; i < count && i < start + 9; ++i) {
            if (i == menu->tool_selected) rectangle(menu, 16, 81 + (i - start) * 11, 287, 10, LINE);
            if (menu->tools == TOOL_SLOTS) {
                slot_path(line, (unsigned int)i);
                tool_line(menu, (int)(i - start), line + 6, i == menu->tool_selected ? TEXT : MUTED);
            } else if (menu->tools == TOOL_GROUP || menu->tools == TOOL_FAVORITES || menu->tools == TOOL_RECENT) {
                const char *name = menu->tools == TOOL_GROUP ? menu->group.paths[i] : (menu->tools == TOOL_RECENT ? menu->recent.paths[i] : menu->favorites.paths[i]);
                char label[35];
                size_t length = strlen(name), offset = i == menu->tool_selected ? menu_scroll_offset(length,34,now_ms(menu)-menu->selection_since) : 0;
                if (!strncmp(name,"/Storage/",9)) { name += 9; length -= 9; offset = i == menu->tool_selected ? menu_scroll_offset(length,34,now_ms(menu)-menu->selection_since) : 0; }
                length -= offset; if (length > 34) length = 34;
                memcpy(label,name+offset,length); label[length] = 0;
                tool_line(menu,(int)(i-start),label,i == menu->tool_selected ? TEXT : MUTED);
            } else {
                const SaveEntry *e = &archive->entries[i];
                size_t n = strlen(e->name);
                if (n > 24) n = 24;
                memcpy(line, e->name, n); line[n] = 0;
                tool_line(menu, (int)(i - start), line, i == menu->tool_selected ? TEXT : MUTED);
                format(line, "%5lu", (unsigned long)e->size);
                text(menu, 260, 82 + (i - start) * 11, line, MUTED);
            }
        }
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
        for (i = 0; i < 10; ++i) {
            if (!i) format(line,"Screen margin: %d",menu->config.screen_margin);
            else if (i == 1) format(line,"Large game titles: %s",menu->config.large_titles ? "ON" : "OFF");
            else if (i == 2) format(line,"Theme: %s",menu->config.theme == 1 ? "High contrast" : (menu->config.theme == 2 ? "Amber" : "Teal"));
            else if (i == 3) format(line,"Folder workaround: %s",menu->settings_folder_workaround ? "ON" : "OFF");
            else if (i == 4) format(line,"USB covers: %s",menu->settings_usb_covers ? "ON" : "OFF");
            else if (i == 5) format(line,"Remember position: %s",menu->settings_remember_position ? "ON" : "OFF");
            else if (i == 6) format(line,"Recent games: %s",menu->settings_recent_games ? "ON" : "OFF");
            else if (i == 7) strcpy(line,"Restore display defaults");
            else if (i == 8) strcpy(line,"Discard unsaved changes");
            else strcpy(line,"Save changes to USB");
            if (i == menu->tool_selected) rectangle(menu,16,81+i*11,287,10,LINE);
            text(menu,24,82+i*11,line,i == menu->tool_selected ? TEXT : MUTED);
        }
        text(menu,24,194,"Non-display changes need reboot.",MUTED);
    } else if (menu->tools == TOOL_FIND) {
        format(line,"Find: %s_",menu->find_query); tool_line(menu,0,line,TEXT);
        tool_line(menu,1,"Filters names in this folder.",MUTED);
        for (i = 0; i < 40; ++i) {
            char key[3]; key[0] = find_keys[i]; key[1] = 0;
            if (key[0] == ' ') strcpy(key,"SP");
            if (i == (size_t)menu->find_key) rectangle(menu,22+(i%10)*27,111+(i/10)*17,19,13,LINE);
            text(menu,27+(i%10)*27,113+(i/10)*17,key,TEXT);
        }
        text(menu,24,189,"Start:Apply X:Erase C:Clear",MUTED);
    } else if (menu->tools == TOOL_JUMP) {
        char letter[2]; letter[0] = menu->jump_letter ? (char)('A'+menu->jump_letter-1) : '#'; letter[1] = 0;
        tool_line(menu,1,"Left/Right: Choose # or A-Z",TEXT);
        format(line,"Jump to: %s",letter); tool_line(menu,4,line,ACCENT);
        tool_line(menu,7,"A:Jump   B:Cancel",TEXT);
    } else if (menu->tools == TOOL_HELP) {
        static const char *help[] = {
            "A: Open folder / launch game", "B: Parent folder / cancel", "Up/Down: Select (hold to repeat)",
            "Left/Right or L/R: Page", "C: Add selected disc to queue", "Start: Review queued discs",
            "X: Refresh current folder", "L+R together: Open tools", "Hold X at boot: Skip USB extras", "Saves: A backs up one, C all"
        };
        static const char *more[] = {
            "Queue: X removes one disc", "Queue: C toggles move mode", "Move mode: Up/Down reorders",
            "Hold C, press X: Clear queue", "Hold C + Left/Right: Letter", "Disc 1: Offer matching disc set",
            "Favorites: A opens game folder", "Favorites: C adds, X removes", "Config: C saves, X discards", "Non-display changes need reboot"
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
            format(line,"Position:%d Recent:%d",menu->position_result,menu->recent_result); tool_line(menu,8,line,MUTED);
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
        else if (menu->tools == TOOL_FIND) controls = "Arrows:Choose A:Key B:Cancel";
        else if (menu->tools == TOOL_CONFIG) controls = "Left/Right:Change C:Save B:Back";
        else if (menu->tools == TOOL_JUMP) controls = "Left/Right:Letter A:Jump B:Back";
        else if (menu->tools == TOOL_BACKUP_DETAILS) controls = "A:Continue C:Old/New B:Back";
        else if (menu->tools == TOOL_SAVES) controls = "A:Back up one C:All X:Refresh";
        else if (menu->tools == TOOL_ARCHIVE) controls = "A:Restore selected B:Back";
        else if (menu->tools == TOOL_CONFIRM_BACKUP || menu->tools == TOOL_CONFIRM_RESTORE) controls = "A:Confirm B:Cancel";
        else if (menu->tools == TOOL_DIAG) controls = "C:Export USB B:Back";
        else if (menu->tools == TOOL_HELP) controls = "B:Back";
        else if (menu->tools == TOOL_RECENT) controls = "A:Open folder Start:Reload B:Back";
        text(menu,16,212,menu->tool_message ? menu->tool_message : controls,menu->tool_message ? AMBER : ACCENT);
    }
    text(menu, 16, 224, menu->tools == TOOL_FAVORITES ? "R:Move order   Start:Reload" :
         ((menu->tools == TOOL_DIAG || menu->tools == TOOL_HELP) ? "Left/Right: More pages" : "L+R:Tools   Up/Down:Select"), MUTED);
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
        else if (page == TOOL_GROUP) { menu->tools = 0; ode_playlist_dispose(&menu->group); selection_changed(menu); }
        else if (page == TOOL_BACKUP_DETAILS) { menu->tools = TOOL_SLOTS; menu->tool_selected = menu->backup_slot; }
        else if (page == TOOL_ARCHIVE) { menu->tools = TOOL_SLOTS; menu->backup_mode = 0; menu->tool_selected = menu->backup_slot; }
        else if (page == TOOL_CONFIRM_RESTORE) { menu->tools = TOOL_ARCHIVE; menu->tool_selected = menu->save_selected; }
        else if (page == TOOL_CONFIRM_BACKUP) { menu->tools = TOOL_SLOTS; menu->tool_selected = menu->backup_slot; }
        else { menu->tools = TOOL_HOME; menu->tool_selected = 0; save_archive_free(&menu->pending); }
        return;
    }
    if (page == TOOL_HOME) count = 9;
    if (page == TOOL_SAVES) count = menu->nvram.count;
    if (page == TOOL_ARCHIVE) count = menu->backup.count;
    if (page == TOOL_SLOTS) count = menu->backup_mode ? 8 : 10;
    if (page == TOOL_CONFIG) count = 10;
    if (page == TOOL_GROUP) count = menu->group.count;
    if (page == TOOL_FAVORITES) count = menu->favorites.count;
    if (page == TOOL_RECENT) count = menu->recent.count;
    if (page == TOOL_BACKUP_DETAILS) count = menu->details_new ? menu->pending.count : menu->backup.count;
    if (count) {
        if (pressed & ControlUp) menu->tool_selected = menu->tool_selected ? menu->tool_selected - 1 : count - 1;
        if (pressed & ControlDown) menu->tool_selected = (menu->tool_selected + 1) % count;
    }
    if (page == TOOL_HELP) {
        if (pressed & (ControlLeft | ControlRight)) menu->tool_selected = !menu->tool_selected;
        return;
    }
    if (page == TOOL_JUMP) {
        if (pressed & ControlLeft) menu->jump_letter = (menu->jump_letter + 26) % 27;
        if (pressed & ControlRight) menu->jump_letter = (menu->jump_letter + 1) % 27;
        if (pressed & ControlA) { menu->tools = 0; jump_letter(menu,menu->jump_letter); }
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
        else if (selected == 3) { menu->tools = TOOL_JUMP; menu->jump_letter = 1; }
        else if (selected == 4) menu->tools = TOOL_CONFIG;
        else if (selected == 5) menu->tools = TOOL_HELP;
        else if (selected == 6) menu->tools = TOOL_DIAG;
        else if (selected == 7) { menu->tools = TOOL_RECENT; open_recent(menu); }
        else { menu->tools = TOOL_FIND; strcpy(menu->find_query,menu->filter); menu->find_key = 0; }
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
        pressed = buttons & ~previous;
        released = previous & ~buttons;
        if (buttons & (ControlUp | ControlDown | ControlLeft | ControlRight)) {
            if (buttons == previous) ++held;
            else held = 0;
            if (held > 20 && (held % 5) == 0)
                pressed |= buttons & (ControlUp | ControlDown | ControlLeft | ControlRight);
        } else held = 0;
        previous = buttons;
        if ((buttons & (ControlLeftShift | ControlRightShift)) ==
                (ControlLeftShift | ControlRightShift) &&
                (pressed & (ControlLeftShift | ControlRightShift))) {
            if (menu.favorites_move) cancel_favorites_move(&menu);
            menu.tools = TOOL_HOME; menu.tool_selected = 0; menu.tool_message = NULL; menu.c_pending = 0;
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
        draw(&menu);
        WaitVBL(menu.vbl, 1);
    }
}
