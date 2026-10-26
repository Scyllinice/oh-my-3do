/*
 * Oh My 3DO: a game browser and launcher for the 3DO with an FCare optical
 * drive emulator, which plays disc images from a USB drive.
 *
 * This file holds the menu's state, its drawing and its controls; the
 * modules it uses do the rest (see the includes below). In outline:
 *
 *   - main() at the bottom connects to the drive emulator (connect_ode),
 *     then runs the main loop: read the controller, react, draw, wait for
 *     the next video frame.
 *   - The browser shows one folder of the drive at a time. Moving the
 *     selection loads the game's cover and, after a pause, its gameplay
 *     preview. A launches a game (or opens a folder); C queues discs for
 *     multi-disc games.
 *   - L+R together opens Tools: favorites, recent games, Find, settings,
 *     the save manager (backing up and restoring NVRAM saves), diagnostics.
 *   - Holding P on any screen shows its controls (menu_help.h).
 *
 * Drawing uses two screen buffers: one is shown while the other is drawn,
 * then they swap. Much of the drawing code avoids redrawing what both
 * buffers already show, because the 3DO's processor (a 12.5 MHz ARM60) has
 * little time to spare while a preview plays.
 *
 * The screen is 320x240 pixels. Every position below is in that space;
 * rectangle() and the text functions shrink everything toward the middle by
 * the "screen margin" setting, for TVs that cut off the picture's edges.
 */
#include "ode.h"
#include "ode_bus.h"
#include "menu_config.h"
#include "menu_ui.h"
#include "menu_help.h"
#include "game_info.h"
#include "ui_font.h"
#include "preview_audio.h"
#include "usb_art.h"
#include "cover_cache.h"
#include "usb_media.h"
#include "preview_stream.h"
#include "art_navigation.h"
#include "save_usb.h"
#include "nvram_io.h"
#include "save_names.h"
#include "save_catalog.h"
#include "save_cheats.h"
#include "save_cheats_usb.h"
#include "library_features.h"
#include "library_navigation.h"
#include "library_index.h"
#include "menu_settings.h"
#include "menu_position.h"
#include "channel_gecko_data.h"

/* The 3DO SDK (Portfolio) headers. */
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

#define MENU_VERSION "1.0.3"

/* Rows in the game list with normal titles (large titles fit fewer). */
#define ROWS 10

/*
 * The Tools screens. menu->tools holds the one showing, or 0 for the
 * browser. (Some numbers are unused: screens that were removed.)
 */
#define TOOL_HOME            1 /* the Tools menu itself */
#define TOOL_SAVES           2 /* saves in the console's NVRAM */
#define TOOL_SLOTS           3 /* backup slots on the USB drive */
#define TOOL_ARCHIVE         4 /* choosing a slot to write a backup to */
#define TOOL_HELP            5 /* general controls */
#define TOOL_DIAG            6 /* diagnostics */
#define TOOL_CONFIRM_BACKUP  7 /* confirming a backup */
#define TOOL_CONFIRM_RESTORE 8 /* confirming a restore */
#define TOOL_FAVORITES       10
#define TOOL_CONFIG          11 /* settings */
#define TOOL_BACKUP_DETAILS  13 /* the saves inside one backup */
#define TOOL_RECENT          14 /* recent games */
#define TOOL_FIND            15 /* typing a search */
#define TOOL_RESULTS         16 /* search results */
#define TOOL_GAME_DETAILS    17 /* a game's genre, publisher and description */
#define TOOL_HEX             18 /* the raw save editor */
#define TOOL_HEX_DISCARD     19 /* confirming unsaved edits are to be dropped */
#define TOOL_CHEATS          20 /* known values in a save (save_cheats.h) */
#define TOOL_SLOT_NAME       21 /* typing a slot label */
#define TOOL_COMPARE         22 /* comparing two backups: the list of saves */
#define TOOL_COMPARE_BYTES   23 /* comparing two backups: one save's bytes */
#define TOOL_SLOT_JUMP       24 /* typing a slot number */
#define TOOL_SLOT_FIND       25 /* slot search options */
#define TOOL_SLOT_QUERY      26 /* typing a slot search */
#define TOOL_CATEGORIES      27 /* which categories are shown */

/* What save_root_operation does at the drive's root. */
#define SAVE_LOAD           0 /* read a backup slot */
#define SAVE_STORE          1 /* write a backup slot */
#define SAVE_TEST           2 /* check the drive can be written to */
#define SAVE_SETTINGS       3 /* write ohmy3do.ini */
#define SAVE_LOAD_TARGET    4 /* read a slot about to be written (may be empty) */
#define SAVE_STORE_CHECKED  5 /* write a slot, if it still holds what was read */
#define SAVE_READ_LABELS    6 /* read the slot page's labels */
#define SAVE_WRITE_LABELS   7 /* write the slot page's labels */
#define SAVE_SEARCH_CATALOG 8 /* search the slot catalog */

/* The deepest folder the browser follows. */
#define MAX_DEPTH 32

/* Room for a menu path: a folder ("/Storage/Games/USA"), and a folder plus
 * one entry's name. The USB media indexes and saved records use the part
 * after "/Storage/", STORAGE_PREFIX characters in. */
#define FOLDER_PATH_BYTES 8193
#define ENTRY_PATH_BYTES  9218
#define STORAGE_PREFIX    9

/* The four directions on the controller's pad. */
#define DIRECTIONS (ControlUp | ControlDown | ControlLeft | ControlRight)

/* How long the selection must rest on a game before its cover loads, and
 * before its preview starts, in milliseconds. Scrolling past does neither. */
#define COVER_IDLE_MS   700UL
#define PREVIEW_IDLE_MS 1600UL

/* The "Gecko TV" palette: the menu's only theme. MakeRGB15 takes 5-bit red,
 * green and blue. */
#define BG    MakeRGB15(2, 2, 3)
#define TEXT  MakeRGB15(31, 31, 29)
#define MUTED MakeRGB15(24, 25, 27)
/* Greener than it looks on a PC: the console's video output pushes lime
 * toward yellow. */
#define ACCENT     MakeRGB15(16, 31, 4)
#define PANEL      MakeRGB15(6, 6, 8)
#define LINE       MakeRGB15(10, 10, 13)
#define SELECT     MakeRGB15(8, 8, 11)
#define DIM        MakeRGB15(13, 13, 16)
#define HEADER     MakeRGB15(3, 3, 5)
#define TV_ORANGE  MakeRGB15(31, 15, 3)
#define OSD_GREEN  MakeRGB15(12, 31, 8)
#define REMOTE_RED MakeRGB15(28, 6, 4)
#define INK        MakeRGB15(1, 1, 2)
#define AMBER      MakeRGB15(30, 22, 9)

/*
 * What the browser knows about one entry of the current folder: for a folder
 * that acts as a game (it holds one game's discs), those discs; and the
 * game's details from metadata.3di. Kept for a few entries at a time.
 */
#define GAME_LOOKUP_SLOTS 16
typedef struct GameLookup {
    const MenuEntry *entry;
    const char *discs[ODE_MAX_DISCS];
    size_t count;
    const LibraryGameInfo *info;
    int info_valid;
} GameLookup;
/* Remembered text measurements for names drawn at a given screen position
 * (see scroll_label), so scrolling names are not measured every frame. */
#define SCROLL_MEASURE_SLOTS 8
typedef struct ScrollMeasure {
    int valid, x, y;
    UiFontMeasure measure;
} ScrollMeasure;

/* Text drawn recently, kept as ready-made CELs (see text()). */
#define UI_TEXT_CACHE 64

/* Startup steps, timed for the diagnostics screen, in order. */
enum {
    BOOT_SPLASH,
    BOOT_ART,
    BOOT_IDENTIFY,
    BOOT_CONFIG,
    BOOT_SAVES,
    BOOT_QUEUE,
    BOOT_FOLDER,
    BOOT_CATEGORIES,
    BOOT_COVERS,
    BOOT_PREVIEWS,
    BOOT_METADATA,
    BOOT_POSITION,
    BOOT_READY,
    BOOT_STEPS
};
static const char *const boot_step_names[BOOT_STEPS] = {
    "splash",     "art",    "identify", "config",   "saves",    "queue", "folder",
    "categories", "covers", "previews", "metadata", "position", "ready"};

/* One cached piece of text: its picture, and what it was drawn with. */
typedef struct UiText {
    CCB *cel;
    int style, width, palette_valid, capacity_width, glyph_width;
    Color ink, background;
    char value[97];
} UiText;

/*
 * What one screen buffer currently shows of the browser, so the next draw
 * into it can skip the parts that are already right. Each back buffer can
 * hold a different selection while the pad is moving.
 */
typedef struct BrowserFrame {
    unsigned long shell, picture, overlay; /* hashes of the three layers */
    size_t selected, start;
    size_t scroll_length[4], scroll_visible[4];
    int scroll_count, controls, gecko, letter;
    int valid;
} BrowserFrame;

/* A game title's hash, for telling apart entries with the same title. */
typedef struct TitleHash {
    unsigned long hash, title;
    size_t index;
} TitleHash;

/*
 * Everything the menu knows. There is one Menu, in main(); every function
 * receives it. Fields are grouped by what they are for.
 */
typedef struct Menu {
    /* ---- Screen and drawing ---- */
    ScreenContext screen; /* the two screen buffers */
    Item vbl;             /* waits for the vertical blank */
    Item clock;           /* the millisecond timer */
    CCB font_ccb;
    int have_font;
    int large_drawing; /* draw text in the large style */
    /* Recently drawn text, kept as CELs (see text()). */
    UiText text_cache[UI_TEXT_CACHE];
    unsigned int text_cursor;
    int text_style, text_failed;
    int text_background_set;
    Color text_background;
    /* Screens still to clear (one per buffer) before drawing on a clean slate. */
    int clear_frames;

    /* ---- Startup ---- */
    CCB *splash;
    int booting, boot_stage;
    int splash_frames; /* full splash draws so far: both buffers need one */
    /* Startup timing for diagnostics: the clock when main began (time since
     * power-on), then each step's end, in ms after that. */
    unsigned long boot_at, boot_ms[BOOT_STEPS], boot_commands[BOOT_STEPS];
    unsigned long boot_draws, boot_draw_ms;              /* splash redraws while starting */
    unsigned long boot_kind_count[17], boot_kind_ms[17]; /* the bus's command times when ready */

    /* ---- The drive emulator ---- */
    Ode ode;
    OdeBus bus;
    int ready;      /* connected and showing a folder */
    int demo;       /* running from the demo ISO, without a drive */
    int view_valid; /* the listing on screen matches the drive */
    /* After a USB failure the menu can climb back to the drive's top
     * folder (see ode_recover_root): what that folder looked like at startup. */
    int root_known, recovery_media_off, recovery_result;
    unsigned long root_fingerprint, root_volume;

    /* ---- Browsing ---- */
    OdeListing listing;                   /* the current folder */
    unsigned int depth;                   /* folders below the drive's top */
    char path[MAX_DEPTH][ODE_MAX_NAME];   /* the folder names down to here... */
    unsigned long path_ids[MAX_DEPTH];    /* ...and their ids */
    size_t previous_selection[MAX_DEPTH]; /* the selection in each parent, for Back */
    size_t selected;                      /* the selected row */
    char breadcrumb[35];                  /* the folder heading */
    char filter[25];                      /* hide names not containing this */
    unsigned long selection_since;        /* when the selection last changed */
    size_t read_count;                    /* entries read so far, while a folder loads */
    int loading, loading_notice;
    /* Visible listing entries (not hidden, matching the filter), rebuilt only
     * when the listing or what hides entries changes. */
    const MenuEntry **visible;
    size_t visible_count, visible_capacity;
    unsigned long listing_generation, visible_key;
    int visible_valid;
    /* The alphabet strip (hold C + Up): which letters have games. */
    int jump_letter;
    int alphabet_active;
    unsigned char alphabet_letters[27];
    unsigned long alphabet_key;
    int alphabet_valid;
    /* Game lookups (see GameLookup) and the metadata they were made with. */
    GameLookup game_lookups[GAME_LOOKUP_SLOTS];
    const LibraryGameInfo *lookup_metadata;
    size_t lookup_metadata_count;
    unsigned long lookup_listing_generation;
    unsigned int lookup_depth, lookup_next;
    int game_lookups_valid;
    ScrollMeasure scroll_measures[SCROLL_MEASURE_SLOTS];
    unsigned int scroll_measure_next;
    /* Which entries share a title and need a version shown (see title_variant). */
    unsigned short *title_variants;
    size_t title_variants_count, title_variants_capacity;
    unsigned long title_variants_key;
    int title_variants_valid;
    /* Long names scroll; their clock pauses while media loads. One scroll
     * position per rendered frame. */
    unsigned long scroll_elapsed, scroll_paused_at;
    int scroll_paused;

    /* ---- The disc queue (multi-disc games) ---- */
    OdePlaylist playlist; /* the FCare's queue */
    OdePlaylist group;    /* a matching multi-disc set being offered */
    int review;           /* the queue screen is showing */
    size_t queue_selected;
    int queue_move;  /* moving the selected disc with Up/Down */
    int single_disc; /* hold C + A: just this disc, not its set */
    int launching;
    int c_pending; /* C is held: waiting to see whether it is a combination */

    /* ---- Favorites, recent games, the remembered position and Find ---- */
    MenuFavorites favorites;
    MenuFavorites recent;
    int recent_valid;
    int favorites_valid;
    int favorites_move;       /* reordering favorites */
    char *favorites_original; /* the order before reordering, to cancel back to */
    char favorites_prefix[ODE_MAX_NAME + 10];
    MenuPosition last_position;
    int position_result;
    int position_dirty; /* the position changed and should be saved */
    int recent_result;
    LibraryCatalog catalog; /* every disc on the drive, for Find */
    unsigned long catalog_volume;
    size_t search_matches[LIBRARY_CATALOG_MAX], search_count;
    unsigned long search_redraw;
    char find_query[25];
    int find_key; /* the selected key of the on-screen keyboard */

    /* ---- Settings ---- */
    MenuConfig config;       /* in effect now */
    MenuConfig saved_config; /* as saved on the drive */
    /* Navigation and artwork changes only take effect after a restart, so
     * these keep the values in effect until then. */
    int settings_folder_workaround;
    int settings_usb_covers;
    int settings_remember_position;
    int settings_recent_games;
    int settings_video_previews;
    int config_result;
    int categories_valid;

    /* ---- Covers ---- */
    CoverCache cover_cache;
    unsigned long cover_load_ms, cover_commands;
    int cover_cached;
    UsbArtIndex usb_art;      /* covers/index.3di */
    UsbArtIndex usb_previews; /* previews/index.3di */
    int art_indexes_valid;
    unsigned long art_index_volume;
    LibraryMetadata metadata;
    CCB *cover;
    int cover_attempted;
    int covers_enabled;
    int skip_usb_art;
    const char *cover_file, *cover_folder;

    /* ---- Previews ---- */
    PreviewAudio preview_audio;
    UsbPreview preview;
    /* An OM3DVID7 preview streams instead (stream.open). Full screen draws it
     * straight into the back screen; sport then copies that screen into the
     * other buffer during the vertical blank that shows it. */
    PreviewStream stream;
    Item sport;
    int sport_pending;
    unsigned long stream_shown; /* stream frames decoded when last shown */
    CCB *preview_cel;
    int preview_attempted, media_cancelled, preview_priming;
    /* Directions that canceled a media read; the main loop still moves by
     * them, even when the button was let go before the read ended. */
    u32 cancel_directions;
    /* Tap X on a game with a preview: preview_off keeps its cover up until the
     * selection changes; preview_has caches whether it has one (-1 unknown). */
    int preview_off, preview_has;
    /* Hold X: the current video or box art full-screen until any button. full_cover is
     * the 192x240 file when the library has one; else the cover is doubled. */
    int full_view; /* 1 = cover, 2 = playing preview */
    /* Leaving full-screen video fades out, redraws the browser in the dark
     * and fades back in (see fade_step): 1 out, 2 in, since fade_since. */
    int fade;
    unsigned long fade_since;
    int full_cleared; /* screen buffers cleared since the view opened */
    unsigned long full_drawn_decode;
    int x_pending; /* X is held: a tap or, after a while, the full-screen view */
    unsigned long x_since;
    CCB *full_cover;
    unsigned long preview_since;
    unsigned int preview_frame;
    int preview_result;
    /* Preview timing and memory, for diagnostics. */
    unsigned long preview_load_ms;
    unsigned long preview_read_ms, preview_check_ms, preview_navigation_ms, preview_setup_ms;
    unsigned long preview_bytes, preview_commands;
    unsigned int preview_frames, preview_fps;
    unsigned long preview_free_bytes, preview_largest_bytes;
    unsigned long preview_vram_free_bytes, preview_vram_largest_bytes;
    unsigned int preview_chunks;
    unsigned long preview_source_frames, preview_vram_bytes;
    unsigned long preview_decodes, preview_decode_ms, preview_decode_max_ms, preview_skipped;
    /* Playback passes: light (picture side only) and full redraws, the time
     * they spend after decoding, and the longest gap between passes. */
    unsigned long preview_full_view_decodes,
        preview_full_view_skipped; /* while X shows it full screen */
    unsigned long playback_static_key;
    unsigned int playback_static_frames;
    unsigned long preview_video_draws,
        preview_video_ms; /* time the cel engine spends drawing the video */
    unsigned long preview_light_draws, preview_full_draws, preview_draw_ms, preview_pass_max_ms,
        preview_pass_at;

    /* ---- The Gecko TV look ---- */
    CCB *logo;
    CCB *channel_gecko; /* the animated gecko on the channel card */
    unsigned long gecko_since;
    int gecko_active, gecko_frame, gecko_uploaded;
    /* TV static, the channel digits and bar, and the screen's rounded corners. */
    CCB *noise, *osd_digits, *osd_bar, *osd_ch, *screen_corner;
    unsigned short *noise_pixels;
    int crt_failed, osd_hidden;
    /* "Tuning": static and a load bar while a preview loads. */
    int tuning, tuning_full;
    long osd_number, osd_lit;
    unsigned long tuning_bytes, tuning_draws, progress_drawn;
    /* When tuning began. A long selected name holds still until it ends:
     * each screen buffer keeps the list from its last full draw, so a
     * scrolling name would flick between two positions. */
    unsigned long tuning_since;

    /* ---- What the screen buffers already show (to skip redrawing it) ---- */
    unsigned long panel_hash; /* list panel shown in both screen buffers */
    int panel_frames;
    BrowserFrame browser_frames[2];
    /* Playback redraws the list row, OSD and title card only until both
     * screen buffers hold their current look (overlay_frames counts them).
     * Their look depends on the panel, the message and the scroll offsets of
     * the names drawn with scroll_label, whose lengths are kept here. */
    int overlay_frames, scroll_count;
    unsigned long overlay_key;
    size_t scroll_length[4], scroll_visible[4];
    unsigned long preview_drawn_decode;
    CCB *badge; /* the X badge as drawn on screen; see badge_save */
    int badge_saved;

    /* ---- Messages and controller input ---- */
    const char *message; /* the status line, if any */
    char status[80];
    char notice[80]; /* a short notice, shown for a few seconds */
    int notice_active;
    unsigned long notice_since;
    u32 suppress_buttons; /* buttons to ignore until they are let go */
    u32 shoulder_pending; /* L or R held: waiting to see if both are pressed */
    int tools_chord_down, tools_requested;

    /* ---- Tools ---- */
    int tools; /* the Tools screen showing (TOOL_...), or 0 */
    size_t tool_selected;
    size_t home_selected; /* the selection on the Tools menu */
    const char *tool_message;
    int diagnostics_result;
    MenuHelpHold help_hold;
    int help_drawn, help_topic, help_margin;
    unsigned long tools_home_hash;
    int tools_home_drawn;
    /* Game details (hold C + tap P). */
    size_t game_details_page, game_details_pages;
    char game_description[GAME_DESCRIPTION_BYTES];

    /* ---- The save manager ---- */
    SaveArchive nvram; /* the console's saves, as last read */
    NvramInfo nvram_info;
    SaveArchive backup;  /* the backup slot being looked at */
    SaveArchive pending; /* a backup about to be written */
    size_t save_selected;
    int backup_mode;  /* backing up: choosing the destination slot */
    int backup_merge; /* the backup adds one save to a slot, not the whole NVRAM */
    unsigned int backup_slot;
    int backup_slot_valid;
    int usb_tested;
    int backup_result;
    int restore_result;
    SaveNames save_names;
    int save_names_loaded;
    /* The slot list: pages of 8 slots, labels, search and jumping. */
    unsigned long slot_page, slot_labels_page;
    int slot_filtered, slot_catalog_stale, slot_find_option;
    unsigned int slot_name_slot;
    char slot_query[25];
    SaveCatalogResults slot_results;
    char slot_jump[11];
    SaveSlotLabels slot_labels, slot_labels_before;
    int slot_labels_valid;
    char slot_label_edit[SAVE_SLOT_LABEL_BYTES];
    /* Copying a save to another slot. */
    SaveArchive copy_original;
    unsigned int copy_source_slot;
    /* Comparing two backups. */
    SaveArchive compare_archive;
    int compare_choosing; /* choosing the second slot */
    unsigned int compare_slot;
    size_t compare_offset;
    int details_new; /* showing the new contents, not the slot's current ones */
    /* The raw save editor and the cheat editor. */
    SaveArchive hex_archive;  /* the edited copy */
    SaveArchive hex_original; /* as it was, to tell whether it changed */
    unsigned int hex_source_slot;
    size_t hex_offset;
    size_t cheat_selected;
    int hex_view; /* TOOL_HEX or TOOL_CHEATS: which editor is open */
    int hex_editing, hex_nibble, hex_dirty;
    unsigned int hex_value;
} Menu;

/* Functions used before their definitions further down. */
static void refresh_usb_art(Menu *menu);
static void stop_preview(Menu *menu);
static void draw_tools(Menu *menu);
static void draw_context_help(Menu *menu);
static void open_game_details(Menu *menu);
static void draw_full_cover(Menu *menu);
static void close_full_cover(Menu *menu);
static void current_route(Menu *menu, ArtRouteStep *route);
static void tools_input(Menu *menu, u32 pressed);
static int queue_set(Menu *menu, const MenuEntry *entry);
static void start_queue(Menu *menu);
static void queue_selected_disc(Menu *menu);
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
static void learn_saves(Menu *menu);
static int config_dirty(const Menu *menu);

/* Rows in the game list: fewer with large titles. */
static size_t menu_rows(const Menu *menu)
{
    return menu->config.large_titles ? 7 : ROWS;
}

/* ==== Small helpers ==== */

/*
 * sprintf into `buffer`. All callers use fixed formats whose output is known
 * to fit.
 *
 * Why not sprintf itself: Norcroft (the 3DO compiler) rewrites sprintf calls
 * to a private runtime function the console does not have. And this file
 * uses the SDK's quoted "stdarg.h", not the compiler's built-in <stdarg.h>,
 * because the built-in one lays out va_list differently from Portfolio's
 * vsprintf, which would then read a stack address as the first value. A PC
 * preview cannot catch that, so the SDK build checks the type below.
 */
static void format(char *buffer, const char *pattern, ...)
{
    va_list args;
    va_start(args, pattern);
#ifdef OH3DO_SDK
    {
        /* Fails to compile if the built-in header ever comes back. */
        char *portfolio_args = args;
        vsprintf(buffer, pattern, portfolio_args);
    }
#else
    vsprintf(buffer, pattern, args);
#endif
    va_end(args);
}

/* Milliseconds since the console started. */
static unsigned long now_ms(Menu *menu)
{
    return (unsigned long)GetMSecTime(menu->clock);
}

/* format, but appending to the string already in `buffer`. */
static void append_format(char *buffer, const char *pattern, ...)
{
    va_list args;
    va_start(args, pattern);
    buffer += strlen(buffer);
#ifdef OH3DO_SDK
    {
        char *portfolio_args = args;
        vsprintf(buffer, pattern, portfolio_args);
    }
#else
    vsprintf(buffer, pattern, args);
#endif
    va_end(args);
}

/* Records that a startup step finished, for diagnostics. */
static void boot_step(Menu *menu, int step)
{
    menu->boot_ms[step] = now_ms(menu) - menu->boot_at;
    menu->boot_commands[step] = menu->bus.commands;
    if (step == BOOT_READY) {
        memcpy(menu->boot_kind_count, menu->bus.kind_count, sizeof(menu->boot_kind_count));
        memcpy(menu->boot_kind_ms, menu->bus.kind_ms, sizeof(menu->boot_kind_ms));
    }
}

/* Freezes scrolling names while media loads (the screen is not redrawn), so
 * they do not jump ahead afterwards. */
static void pause_scroll(Menu *menu)
{
    if (!menu->scroll_paused) {
        menu->scroll_paused_at = now_ms(menu);
        menu->scroll_paused = 1;
    }
}

/* Lets scrolling names move again, from where they stopped. */
static void resume_scroll(Menu *menu)
{
    if (menu->scroll_paused) {
        menu->selection_since += now_ms(menu) - menu->scroll_paused_at;
        menu->scroll_paused = 0;
    }
}

/* Whether navigation detours around the FCare folder-change bug (see menu_config.h). */
static int folder_workaround(const Menu *menu)
{
    if (menu->config.folder_workaround == MENU_WORKAROUND_AUTO)
        return !strcmp(menu->ode.identity, "LOCODE");
    return menu->config.folder_workaround == MENU_WORKAROUND_ON;
}

/* The folder workaround setting as Settings and Diagnostics show it. */
static const char *workaround_name(int value)
{
    if (value == MENU_WORKAROUND_AUTO)
        return "AUTO";
    return value == MENU_WORKAROUND_ON ? "ON" : "OFF";
}

/* Waits until the screen copy queued after the last full-screen video frame
 * is done (see Menu.sport). */
static void sport_wait(Menu *menu)
{
#ifdef OH3DO_SDK
    if (menu->sport_pending) {
        WaitIO(menu->sport);
        menu->sport_pending = 0;
    }
#else
    menu->sport_pending = 0;
#endif
}

/* Stops and frees any preview, loaded or streaming, and its sound. */
static void stop_preview(Menu *menu)
{
    menu->preview_priming = 0;
    sport_wait(menu);
    preview_stream_close(&menu->stream);
    preview_audio_close(&menu->preview_audio);
    if (menu->preview_cel)
        UnloadCel(menu->preview_cel);
    menu->preview_cel = NULL;
    usb_preview_free(&menu->preview);
    /* Memory freed: text pictures may be created again. */
    menu->text_failed = 0;
}

/*
 * Notices L+R pressed together. Media reads also poll the controller, so a
 * chord seen there is kept until the main loop can act on it, even if both
 * buttons have been let go by then.
 */
static void observe_tools_chord(Menu *menu, u32 buttons)
{
    u32 shoulders = buttons & (ControlLeftShift | ControlRightShift);
    if (!shoulders) {
        menu->tools_chord_down = 0;
    } else if (shoulders == (ControlLeftShift | ControlRightShift) && !menu->tools_chord_down) {
        menu->tools_chord_down = 1;
        menu->tools_requested = 1;
    }
}

/*
 * L or R alone pages the list, but only when let go: until then, it may be
 * the first half of L+R. So the first shoulder of a chord never moves the
 * list or starts anything. Returns `pressed` with shoulder presses replaced
 * by their delayed releases.
 */
static u32 shoulder_input(Menu *menu, u32 buttons, u32 pressed, u32 released)
{
    const u32 shoulders = ControlLeftShift | ControlRightShift;
    u32 shoulder_presses = pressed & shoulders;

    pressed &= ~shoulders;
    if (menu->tools_chord_down || menu->tools_requested || (buttons & shoulders) == shoulders ||
        pressed) {
        /* Part of L+R, or with another button: the shoulders do nothing alone. */
        menu->shoulder_pending = 0;
        return pressed;
    }
    menu->shoulder_pending |= shoulder_presses;
    pressed |= menu->shoulder_pending & released;
    menu->shoulder_pending &= ~released;
    return pressed;
}

/* Forgets everything tied to the old selection (cover, preview, full-screen
 * view...) after the selection moves. */
static void selection_changed(Menu *menu)
{
    stop_preview(menu);
    menu->preview_attempted = 0;
    menu->preview_off = 0;
    menu->preview_has = -1;
    menu->x_pending = 0;
    menu->selection_since = now_ms(menu);

    /*
     * Start the channel gecko's animation, but while the pad is held and
     * the selection repeats, keep an animation already playing moving rather
     * than restarting it at every step.
     */
    if (menu->channel_gecko && !menu->tools && !menu->review) {
        if (!menu->gecko_active || menu->selection_since - menu->gecko_since >=
                                       CHANNEL_GECKO_FRAMES * CHANNEL_GECKO_FRAME_MS)
            menu->gecko_since = menu->selection_since;
        menu->gecko_active = 1;
    }

    menu->scroll_paused = 0;
    menu->position_dirty = 1;
    menu->alphabet_active = 0;
    close_full_cover(menu);
    if (menu->cover)
        UnloadCel(menu->cover);
    menu->cover = NULL;
    menu->cover_file = menu->cover_folder = NULL;
    menu->cover_attempted = 0;
}

/* Shows a short notice on the status line for a few seconds. */
static void notify(Menu *menu, const char *message)
{
    strncpy(menu->notice, message, sizeof(menu->notice) - 1);
    menu->notice[sizeof(menu->notice) - 1] = 0;
    menu->notice_since = now_ms(menu);
    menu->notice_active = 1;
    menu->message = NULL;
}

/* Rebuilds the folder heading from the current path. */
static void update_breadcrumb(Menu *menu)
{
    const char *parent = menu->depth > 1 ? menu->path[menu->depth - 2] : "";
    const char *folder = menu->depth ? menu->path[menu->depth - 1] : "";
    menu_breadcrumb(menu->breadcrumb, parent, folder, menu->depth);
}

/* ==== Drawing primitives ==== */

/*
 * Fills a rectangle in the screen buffer being drawn. Positions are in the
 * 320x240 layout space and are scaled in by the screen margin; only a
 * full-screen fill ignores the margin.
 */
static void rectangle(Menu *menu, int x, int y, int w, int h, Color color)
{
    GrafCon gc;
    Rect rect;
    int margin = menu->config.screen_margin;

    if (x == 0 && y == 0 && w == 320 && h == 240)
        margin = 0;
    rect.rect_XLeft = margin + x * (320 - 2 * margin) / 320;
    rect.rect_YTop = margin + y * (240 - 2 * margin) / 240;
    rect.rect_XRight = margin + (x + w) * (320 - 2 * margin) / 320 - 1;
    rect.rect_YBottom = margin + (y + h) * (240 - 2 * margin) / 240 - 1;
    /* Never less than one pixel. */
    if (rect.rect_XRight < rect.rect_XLeft)
        rect.rect_XRight = rect.rect_XLeft;
    if (rect.rect_YBottom < rect.rect_YTop)
        rect.rect_YBottom = rect.rect_YTop;
    SetFGPen(&gc, color);
    FillRect(menu->screen.sc_BitmapItems[menu->screen.sc_curScreen], &gc, &rect);
}

/*
 * Draws a CEL, scaled in by the screen margin like everything else. A CEL's
 * position and its HDX/HDY/VDX/VDY step vectors are 16.16 fixed point, so
 * each is scaled by (320 - 2 * margin) / 320 across and the same with 240
 * down.
 */
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
    copy.ccb_Flags |= CCB_LAST; /* draw just this one, not a chain */
    DrawCels(menu->screen.sc_BitmapItems[menu->screen.sc_curScreen], &copy);
}

/*
 * The background behind text at height y, for antialiasing the font's edges
 * against: what label_on set, or else what the screen normally has there
 * (the header band, the list panel, or the page).
 */
static Color text_background_at(const Menu *menu, int y)
{
    if (menu->text_background_set)
        return menu->text_background;
    if (y < 52)
        return HEADER;
    if (y < 186)
        return PANEL;
    return BG;
}

/*
 * Makes sure a cached text CEL can hold `width` pixels, creating a bigger one
 * if needed (rounded up to 16 pixels, as CEL rows must be whole words).
 * When memory runs out, text_failed is set and the caller falls back to the
 * SDK's plain font until memory is freed.
 */
static void make_room_for_text(Menu *menu, UiText *cached, int width)
{
    int capacity = (width + 15) & ~15;
    CCB *larger;

    if (capacity < 16)
        capacity = 16;
    if ((cached->cel && cached->capacity_width >= capacity) || menu->text_failed)
        return;

    larger = CreateCel(capacity, UI_FONT_HEIGHT, 4, 0, NULL);
    if (!larger) {
        menu->text_failed = 1;
        return;
    }
    if (cached->cel)
        UnloadCel(cached->cel);
    cached->cel = larger;
    cached->capacity_width = capacity;
    cached->width = -1; /* nothing drawn into it yet */
    cached->palette_valid = 0;
    memset(larger->ccb_PLUTPtr, 0, 64);
}

/*
 * Draws `string` at (x, y) in `color`, in the current text style.
 *
 * Text is drawn as a CEL: a small 4-bit picture of the text (from
 * ui_font_render) with a 16-color palette blending the ink into the
 * background. Making that picture is the slow part, so recently drawn text
 * is kept in a small cache of CELs (text_cache) and only redrawn when its
 * text, style or width changes. Text is drawn at its native size on whole
 * pixels even with a screen margin, so it stays sharp.
 *
 * Characters outside printable ASCII show as '?'. Text is cut off at x = 308.
 */
static void text(Menu *menu, int x, int y, const char *string, Color color)
{
    /* static: too big for the console's small stack. */
    static unsigned char bitmap[UI_FONT_STRIDE * UI_FONT_HEIGHT];
    UiText *cached;
    char safe[97];
    size_t i;
    int style = menu->large_drawing ? UI_FONT_LARGE : menu->text_style;
    int margin = menu->config.screen_margin;
    int glyph_width;
    int px = margin + x * (320 - 2 * margin) / 320;
    int py = margin + y * (240 - 2 * margin) / 240;
    int max_width = (308 - x) * (320 - 2 * margin) / 320;
    Color background = text_background_at(menu, y);

    if (max_width > UI_FONT_WIDTH)
        max_width = UI_FONT_WIDTH;
    if (max_width <= 0 || !*string)
        return;

    for (i = 0; i < 96 && string[i]; ++i) {
        unsigned char c = (unsigned char)string[i];
        safe[i] = c >= 32 && c <= 126 ? (char)c : '?';
    }
    safe[i] = 0;
#ifdef UI_PREVIEW_TEXT
    UI_PREVIEW_TEXT(safe); /* the PC preview records every label */
#endif

    /*
     * Each call takes the next cache slot in order. Every screen's drawing
     * code sets text_cursor back to 0 before it starts, so a screen that
     * draws the same labels in the same order finds each one already made.
     * (Playback's few labels use the last 16 slots instead; see draw().)
     * Past the cache's end, the last slot is reused.
     */
    cached = &menu->text_cache[menu->text_cursor < UI_TEXT_CACHE ? menu->text_cursor++
                                                                 : UI_TEXT_CACHE - 1];

    /* The picture and its full width depend only on the text and style. The
     * full width is kept so a later, wider space can grow the CEL. */
    if (cached->cel && cached->style == style && !strcmp(cached->value, safe))
        glyph_width = cached->glyph_width;
    else
        glyph_width = ui_font_width(safe, style);
    make_room_for_text(menu, cached, glyph_width < max_width ? glyph_width : max_width);

    if (cached->cel) {
        CCB *cel = cached->cel;
        int width;

        if (max_width > cached->capacity_width)
            max_width = cached->capacity_width;

        /* Redraw the picture only if what it shows changed. */
        if (cached->style != style || strcmp(cached->value, safe) || cached->width != max_width) {
            int row, stride = cached->capacity_width / 2; /* bytes: two pixels each */
            ui_font_render(bitmap, safe, style, max_width);
            for (row = 0; row < ui_font_height(style); ++row)
                memcpy((unsigned char *)cel->ccb_SourcePtr + row * stride,
                       bitmap + row * UI_FONT_STRIDE, (size_t)stride);
            strcpy(cached->value, safe);
            cached->style = style;
            cached->width = max_width;
        }
        cached->glyph_width = glyph_width;
        width = glyph_width < max_width ? glyph_width : max_width;
        if (!width)
            return;

        /* The palette, if the colors changed. */
        if (!cached->palette_valid || cached->ink != color || cached->background != background) {
            ui_font_palette((unsigned short *)cel->ccb_PLUTPtr, color, background);
            cached->ink = color;
            cached->background = background;
            cached->palette_valid = 1;
        }

        /* PRE0 and PRE1 are the CEL's "preamble" words: they hold its
         * height and width (each minus one). */
        cel->ccb_PRE0 = (cel->ccb_PRE0 & ~0x0000ffc0UL) | ((ui_font_height(style) - 1) << 6);
        cel->ccb_PRE1 = (cel->ccb_PRE1 & ~0x000007ffUL) | (width - 1);
        cel->ccb_Width = width;
        cel->ccb_Height = ui_font_height(style);
        /* At native size on whole pixels (16.16 fixed point). */
        cel->ccb_XPos = px << 16;
        cel->ccb_YPos = py << 16;
        cel->ccb_HDX = 1 << 20;
        cel->ccb_HDY = 0;
        cel->ccb_VDX = 0;
        cel->ccb_VDY = 1 << 16;
        cel->ccb_Flags |= CCB_LAST;
        DrawCels(menu->screen.sc_BitmapItems[menu->screen.sc_curScreen], cel);
    } else {
        /* Out of memory for CELs: the SDK's plain 8-pixel font, cut to fit. */
        GrafCon gc;
        char fallback[37];
        for (i = 0; i < 36 && i < (size_t)(max_width / 8) && safe[i]; ++i)
            fallback[i] = safe[i];
        fallback[i] = 0;
        SetFGPen(&gc, color);
        MoveTo(&gc, px, py);
        DrawText8(&gc, menu->screen.sc_BitmapItems[menu->screen.sc_curScreen],
                  (const u8 *)fallback);
    }
}

/* text() in a given style. */
static void label(Menu *menu, int x, int y, const char *value, Color color, int style)
{
    int previous = menu->text_style;
    menu->text_style = style;
    text(menu, x, y, value, color);
    menu->text_style = previous;
}

/* label() over a given background color (for text on buttons and badges). */
static void label_on(Menu *menu, int x, int y, const char *value, Color ink, int style,
                     Color background)
{
    int previous = menu->text_background_set;
    Color old = menu->text_background;
    menu->text_background_set = 1;
    menu->text_background = background;
    label(menu, x, y, value, ink, style);
    menu->text_background_set = previous;
    menu->text_background = old;
}

/* A rectangle with its corners cut off: three overlapping rectangles. */
static void rounded(Menu *menu, int x, int y, int w, int h, Color color)
{
    rectangle(menu, x + 3, y, w - 6, h, color);
    rectangle(menu, x + 1, y + 1, w - 2, h - 2, color);
    rectangle(menu, x, y + 3, w, h - 6, color);
}

/* ==== Names, titles and buttons ==== */

/*
 * The remembered measurements for a name drawn at (x, y) (see
 * ScrollMeasure), taking over the oldest slot for a new position. Names
 * that scroll are measured again every frame otherwise. The measurement
 * keeps its own copy of the text, since most names are built in shared
 * scratch buffers.
 */
static UiFontMeasure *scroll_measure(Menu *menu, int x, int y)
{
    unsigned int i;
    ScrollMeasure *slot;
    for (i = 0; i < SCROLL_MEASURE_SLOTS; ++i) {
        slot = menu->scroll_measures + i;
        if (slot->valid && slot->x == x && slot->y == y)
            return &slot->measure;
    }
    slot = menu->scroll_measures + menu->scroll_measure_next;
    menu->scroll_measure_next = (menu->scroll_measure_next + 1) % SCROLL_MEASURE_SLOTS;
    memset(slot, 0, sizeof(*slot));
    slot->valid = 1;
    slot->x = x;
    slot->y = y;
    return &slot->measure;
}
/*
 * Draws one line of a name in `room` layout pixels. The selected name
 * scrolls if it is too long (see menu_scroll_offset); any other name that
 * does not fit is cut short with "...". At most 64 characters are drawn.
 */
static void scroll_label(Menu *menu, int x, int y, int room, const char *name, size_t length,
                         int style, Color ink, Color background, int selected)
{
    char title[68];
    size_t visible, offset = 0;
    UiFontMeasure *measure = scroll_measure(menu, x, y);
    int limit = (308 - x) * (320 - 2 * menu->config.screen_margin) / 320;
    /* From layout pixels to screen pixels, and no further than text() draws. */
    room = room * (320 - 2 * menu->config.screen_margin) / 320;
    if (room > limit)
        room = limit;
    if (room > UI_FONT_WIDTH)
        room = UI_FONT_WIDTH;
    ui_font_measure(measure, name, length, room, style);
    visible = selected ? 0 : ui_font_measure_fit(measure, name, 0);
    if (visible > 64)
        visible = 64;
    if (selected) {
        /* Scroll until the name's end shows. The characters shown at the
         * end are what decides how far to go: the end can be wider than the
         * beginning in a proportional font. */
        size_t tail = ui_font_measure_tail(measure, name);
        if (length - tail > 64)
            tail = length - 64;
        visible = length - tail;
        offset = menu_scroll_offset(length, visible, menu->scroll_elapsed);
        /* Remember scrolling names: the screen must keep being redrawn while
         * they move (see overlay_key). */
        if (menu->scroll_count < 4) {
            menu->scroll_length[menu->scroll_count] = length;
            menu->scroll_visible[menu->scroll_count++] = visible;
        }
    } else if (visible < length) {
        /* Cut short: as much as fits with "...", without trailing spaces. */
        visible = ui_font_measure_ellipsis(measure, name);
        if (visible > 64)
            visible = 64;
        while (visible && name[visible - 1] == ' ')
            --visible;
        memcpy(title, name, visible);
        strcpy(title + visible, "...");
        label_on(menu, x, y, title, ink, style, background);
        return;
    }
    visible = ui_font_measure_fit(measure, name, offset);
    if (visible > 64)
        visible = 64;
    memcpy(title, name + offset, visible);
    title[visible] = 0;
    label_on(menu, x, y, title, ink, style, background);
}
/*
 * A game's title as shown (see menu_display_title), with " [N]" added when
 * several entries share the title (see title_variant). Returns a shared
 * buffer, valid until the next call.
 */
static const char *display_game_variant(const char *name, int directory, unsigned int variant)
{
    /* Big enough for full paths from favorites and the queue, not just one
     * folder entry's name. */
    static char title[ENTRY_PATH_BYTES];
    size_t n = menu_display_title(title, sizeof(title), name, directory);
    if (variant && n + 16 < sizeof(title))
        format(title + n, " [%u]", variant);
    return title;
}
/* A file or folder name as the list shows it (display_game_variant with no
 * number). */
static const char *display_game_title(const char *name, int directory)
{
    return display_game_variant(name, directory, 0);
}
/*
 * Draws a game's name in a list row: its title, plus a small "ISO" or "CUE"
 * badge at the right when the row is wide enough.
 */
static void game_label(Menu *menu, int x, int y, int room, const char *name, int directory,
                       int style, Color ink, Color background, int selected)
{
    const char *kind = menu_image_format(name, directory);
    int scale = 320 - 2 * menu->config.screen_margin;
    if (kind && room > 140) {
        /* The badge's width in layout pixels, rounded up; centered vertically. */
        int badge_width = (ui_font_width(kind, UI_FONT_SMALL) * 320 + scale - 1) / scale;
        int badge_y = y + (ui_font_height(style) - ui_font_height(UI_FONT_SMALL)) / 2;
        label_on(menu, x + room - badge_width, badge_y, kind, ink, UI_FONT_SMALL, background);
        room -= badge_width + 8;
    }
    name = display_game_title(name, directory);
    scroll_label(menu, x, y, room, name, strlen(name), style, ink, background, selected);
}
/* Blends two colors: num/den of a and the rest b, channel by channel, rounded. */
static Color mix(unsigned int a, unsigned int b, unsigned int num, unsigned int den)
{
    unsigned int r = (((a >> 10) & 31U) * num + ((b >> 10) & 31U) * (den - num) + den / 2) / den;
    unsigned int g = (((a >> 5) & 31U) * num + ((b >> 5) & 31U) * (den - num) + den / 2) / den;
    unsigned int c = ((a & 31U) * num + (b & 31U) * (den - num) + den / 2) / den;
    return MakeRGB15(r, g, c);
}
/* A button cap, filled row by row (the cel engine has no curves): a w x h
 * shape whose ends are halves of an ends x h ellipse (ends == w gives a
 * circle or oval, ends == h a capsule). Each row's edge pixels are blended
 * into the ground by how much of them the curve covers, so small caps read
 * round instead of stepped. */
static void cap_shape(Menu *menu, int x, int y, int w, int h, int ends, Color color, Color ground)
{
    int row;
    for (row = 0; row < h; ++row) {
        /*
         * How wide the ellipse is at this row's middle: with dy the distance
         * from the center (in half pixels), the half-width is proportional
         * to sqrt(h*h - dy*dy). `root` is that square root times 8, found
         * by counting up (no floating point or square root routine).
         */
        long dy = 2L * row + 1 - h, inside = (long)h * h - dy * dy, root = 0, half, left, right,
             first, last;
        while ((root + 1) * (root + 1) <= 64L * inside)
            ++root;
        /* The row's edges, in sixteenths of a pixel from x. */
        half = 8L * (long)(w - ends) + (long)ends * root / h;
        left = 8L * w - half;
        right = 8L * w + half;
        /* Whole pixels inside, then the partly covered pixel at each edge,
         * blended by how much of it is inside. */
        first = (left + 15) / 16;
        last = right / 16;
        if (last > first)
            rectangle(menu, x + (int)first, y + row, (int)(last - first), 1, color);
        if (left % 16)
            rectangle(menu, x + (int)(left / 16), y + row, 1, 1,
                      mix((unsigned int)color, (unsigned int)ground, (unsigned int)(16 - left % 16),
                          16U));
        if (right % 16 && right / 16 < w)
            rectangle(
                menu, x + (int)(right / 16), y + row, 1, 1,
                mix((unsigned int)color, (unsigned int)ground, (unsigned int)(right % 16), 16U));
    }
}
/* A controller prompt shaped like the pad's buttons: round caps for A, B and
 * C, small ovals for P and X, capsules for shoulders and arrows. Each button
 * has its own color so prompts read at a glance, and sits on a darker lip so
 * it looks raised. Returns the width used, including the action's label. */
static int button(Menu *menu, int x, int y, const char *key, const char *action, int lit)
{
    Color cap = !strcmp(key, "A")   ? ACCENT
                : !strcmp(key, "B") ? REMOTE_RED
                : !strcmp(key, "C") ? AMBER
                : (!strcmp(key, "P") || !strcmp(key, "X") || !strcmp(key, "Hold P"))
                    ? MakeRGB15(22, 23, 26)
                    : TV_ORANGE;
    Color lip = mix((unsigned int)cap, (unsigned int)INK, 9U, 16U);
    /* The cap's brightness, weighting green most as the eye does. */
    unsigned int rgb = (unsigned int)cap,
                 luma = ((rgb >> 10) & 31U) * 30U + ((rgb >> 5) & 31U) * 59U + (rgb & 31U) * 11U;
    /* Dark letters on light caps, white on the red B. */
    Color ink = luma > 1500U ? INK : TEXT;
    int scale = 320 - 2 * menu->config.screen_margin;
    int key_width = ui_font_width(key, UI_FONT_SMALL_BOLD) * 320 / scale, width, round = !key[1];
    (void)lit; /* accepted for callers, but currently unused */
    /* Each shape is drawn twice: the darker lip one pixel lower, then the cap. */
    if (round && (key[0] == 'A' || key[0] == 'B' || key[0] == 'C')) {
        width = 14;
        cap_shape(menu, x, y + 1, 14, 13, 14, lip, BG);
        cap_shape(menu, x, y, 14, 13, 14, cap, BG);
    } else if (round) {
        width = 18;
        cap_shape(menu, x, y + 3, 18, 10, 18, lip, BG);
        cap_shape(menu, x, y + 2, 18, 10, 18, cap, BG);
    } else {
        width = key_width + 12;
        cap_shape(menu, x, y + 2, width, 11, 11, lip, BG);
        cap_shape(menu, x, y + 1, width, 11, 11, cap, BG);
    }
    label_on(menu, x + (width - key_width) / 2, y + 1, key, ink, UI_FONT_SMALL_BOLD, cap);
    label(menu, x + width + 4, y + 1, action, TEXT, UI_FONT_SMALL);
    return width + 4 + ui_font_width(action, UI_FONT_SMALL) * 320 / scale + 10;
}
/* The plain background behind the Tools screens: the header band and page. */
static void backdrop(Menu *menu)
{
    int i;
    rectangle(menu, 0, 0, 320, 240, BG);
    for (i = 0; i < 8; ++i)
        rectangle(menu, 0, i * 24, 320, 24, HEADER);
    rectangle(menu, 0, 223, 320, 17, BG);
}

/* ==== Which entries the browser shows ==== */

/* Whether the current folder is the drive's top (or a volume's top, when
 * the top level lists volumes), where the menu's support files live. */
static int storage_root(const Menu *menu)
{
    return menu->depth == 0 || (menu->depth == 1 && !(menu->path_ids[0] & 0xffffffUL));
}
/* Whether an entry is hidden from the list (see menu_support_entry). */
static int hidden_entry(const Menu *menu, const MenuEntry *entry)
{
    /* Entries with no entry bits are volumes, never hidden. */
    if (entry->is_directory && !(entry->toc_id & 0xffffffUL))
        return 0;
    return menu_support_entry(entry->name, entry->is_directory, storage_root(menu));
}
/* Frees the current listing and everything derived from it. Every listing
 * replacement goes through here, so bumping listing_generation tells the
 * caches below that they are out of date. */
static void free_listing(Menu *menu)
{
    free(menu->title_variants);
    menu->title_variants = NULL;
    menu->title_variants_count = menu->title_variants_capacity = 0;
    menu->title_variants_valid = 0;
    menu->game_lookups_valid = 0;
    ode_listing_free(&menu->listing);
    ++menu->listing_generation;
}
/*
 * A hash (FNV-1a) of everything that decides which entries show: the
 * listing, hidden_entry's inputs and the filter. If it is unchanged, the
 * cached visible list is still right.
 */
static unsigned long visible_key(const Menu *menu)
{
    unsigned long key = 2166136261UL;
    const char *c;
    key = (key ^ menu->listing_generation) * 16777619UL;
    key = (key ^ (unsigned long)menu->listing.entries) * 16777619UL;
    key = (key ^ (unsigned long)menu->listing.count) * 16777619UL;
    key = (key ^ (unsigned long)storage_root(menu)) * 16777619UL;
    key = (key ^ (unsigned long)menu->config.hide_adult) * 16777619UL;
    for (c = menu->filter; *c; ++c)
        key = (key ^ (unsigned char)*c) * 16777619UL;
    return key;
}
/*
 * The entries shown in the list, in order, and how many; or NULL if memory
 * ran out (callers then scan the listing themselves). Cached, because
 * scanning a big folder several times per frame used to take most of the
 * frame time during 30 fps preview playback.
 */
static const MenuEntry **visible_entries(const Menu *menu, size_t *count)
{
    Menu *cache = (Menu *)menu; /* filling the cache changes nothing shown */
    unsigned long key = visible_key(menu);
    if (!cache->visible_valid || key != cache->visible_key) {
        size_t i, n = 0;
        cache->visible_valid = 0;
        if (menu->listing.count > cache->visible_capacity) {
            const MenuEntry **grown = (const MenuEntry **)realloc(
                (void *)cache->visible, menu->listing.count * sizeof(*grown));
            if (!grown)
                return NULL;
            cache->visible = grown;
            cache->visible_capacity = menu->listing.count;
        }
        for (i = 0; i < menu->listing.count; ++i) {
            const MenuEntry *entry = menu->listing.entries[i];
            if (!hidden_entry(menu, entry) && menu_name_matches(entry->name, menu->filter))
                cache->visible[n++] = entry;
        }
        cache->visible_count = n;
        cache->visible_key = key;
        cache->visible_valid = 1;
    }
    *count = cache->visible_count;
    return cache->visible;
}
/* The index-th shown entry (not counting the ".." row), or NULL. */
static const MenuEntry *browser_entry(const Menu *menu, size_t index)
{
    size_t i, count;
    const MenuEntry **visible = visible_entries(menu, &count);
    if (visible)
        return index < count ? visible[index] : NULL;
    /* No cache: count through the listing. */
    for (i = 0; i < menu->listing.count; ++i) {
        const MenuEntry *entry = menu->listing.entries[i];
        if (hidden_entry(menu, entry) || !menu_name_matches(entry->name, menu->filter))
            continue;
        if (!index--)
            return entry;
    }
    return NULL;
}
/* Rows in the list: the shown entries plus the ".." row, or the queue's discs. */
static size_t row_count(const Menu *menu)
{
    size_t i, count = menu->listing.has_parent ? 1 : 0, visible;
    if (menu->review)
        return menu->playlist.count;
    if (visible_entries(menu, &visible))
        return count + visible;
    for (i = 0; i < menu->listing.count; ++i)
        if (!hidden_entry(menu, menu->listing.entries[i]) &&
            menu_name_matches(menu->listing.entries[i]->name, menu->filter))
            ++count;
    return count;
}

/*
 * Fills available[0..26]: 1 for each initial (0 '#', 1-26 A-Z) some shown
 * entry starts with. Cached like the visible list; it never allocates or
 * talks to the drive, since it runs while drawing.
 */
static void alphabet_available(const Menu *menu, unsigned char *available)
{
    Menu *cache = (Menu *)menu;
    unsigned long key = visible_key(menu);
    size_t i;
    if (cache->alphabet_valid && cache->alphabet_key == key) {
        memcpy(available, cache->alphabet_letters, 27);
        return;
    }
    memset(available, 0, 27);
    for (i = 0; i < menu->listing.count; ++i) {
        const MenuEntry *entry = menu->listing.entries[i];
        if (!hidden_entry(menu, entry) && menu_name_matches(entry->name, menu->filter))
            available[menu_initial(entry->name)] = 1;
    }
    memcpy(cache->alphabet_letters, available, 27);
    cache->alphabet_key = key;
    cache->alphabet_valid = 1;
}
/*
 * Draws the alphabet strip, just above the title card. Letters with games
 * are brighter; the current one is highlighted. With `previous` >= 0 only
 * the old and new current letters are redrawn (moving along the strip);
 * with -1 the whole strip is.
 */
static void draw_alphabet_change(Menu *menu, int previous)
{
    unsigned char available[27];
    int i, current = menu->jump_letter, strip = 156;
    int scale = 320 - 2 * menu->config.screen_margin, x, total = 0;
    alphabet_available(menu, available);
    if (!menu->alphabet_active) {
        const MenuEntry *entry = NULL;
        if (!(menu->listing.has_parent && !menu->selected))
            entry = browser_entry(menu, menu->selected - (menu->listing.has_parent ? 1 : 0));
        current = entry ? menu_initial(entry->name) : -1;
    }
    for (i = 0; i < 27; ++i) {
        char letter[2];
        letter[0] = i ? (char)('A' + i - 1) : '#';
        letter[1] = 0;
        total += (ui_font_width(letter, UI_FONT_SMALL) * 320 + scale - 1) / scale + 2;
    }
    x = 14 + (292 - total) / 2;
    if (previous < 0 && menu->alphabet_active)
        rectangle(menu, 14, strip, 292, 16, PANEL);
    for (i = 0; i < 27; ++i) {
        char letter[2];
        int cell;
        Color ink = available[i] ? MUTED : DIM;
        letter[0] = i ? (char)('A' + i - 1) : '#';
        letter[1] = 0;
        cell = (ui_font_width(letter, UI_FONT_SMALL) * 320 + scale - 1) / scale + 2;
        if (previous >= 0 && i != previous && i != current) {
            x += cell;
            continue;
        }
        if (previous >= 0)
            rectangle(menu, x - 1, strip + 1, cell, 14, PANEL);
        if (i == current) {
            rectangle(menu, x - 1, strip + 1, cell, 14, menu->alphabet_active ? ACCENT : SELECT);
            ink = menu->alphabet_active ? MakeRGB15(1, 2, 4) : TEXT;
        }
        label_on(menu, x, strip + 2, letter, ink, UI_FONT_SMALL,
                 i == current ? (menu->alphabet_active ? ACCENT : SELECT)
                              : (menu->alphabet_active ? PANEL : BG));
        x += cell;
    }
}
/* Draws the whole alphabet strip. */
static void draw_alphabet(Menu *menu)
{
    draw_alphabet_change(menu, -1);
}
/* The page heading: the logo, two short colored bars and the title. */
static void brand(Menu *menu, const char *title)
{
    if (menu->logo) {
        CCB logo = *menu->logo;
        logo.ccb_XPos = 12 << 16;
        logo.ccb_YPos = 8 << 16;
        logo.ccb_HDX = (1 << 20) / 2; /* half size */
        logo.ccb_VDY = (1 << 16) / 2;
        draw_cel(menu, &logo);
    } else
        label(menu, 12, 12, "Oh My 3DO", TEXT, UI_FONT_NORMAL);
    rectangle(menu, 88, 6, 39, 2, TV_ORANGE);
    rectangle(menu, 129, 6, 17, 2, ACCENT);
    label(menu, 88, 9, title, TEXT, UI_FONT_HEADING);
}
/* ==== The Gecko TV: the portable TV on the right of the browser ==== */

/* The picture side is a portable TV: previews, covers and static all play on
 * its 160x120 screen, the preview's native size. (VIDEO_X, VIDEO_Y) is the
 * screen's top-left corner. */
#define VIDEO_X 152
#define VIDEO_Y 45
#define CABINET MakeRGB15(6, 6, 7)
/* Gecko TV draws its TV furniture from a few small cached cels rather than
 * many rectangles: TV static, the on-screen-display channel digits and its
 * bar. Without them it falls back to plain fills. */
/* The channel digits 0-9, each 3 dots wide and 5 tall, row by row. */
static const char *const osd_font[10] = {
    "111101101101111", "010110010010111", "111001111100111", "111001011001111", "101101111001001",
    "111100111001111", "111100111101111", "111001010010010", "111101111101111", "111101111001111"};
/* The static is one 86x120 cel drawn at 2x over the picture area. Each frame
 * starts it at a random place in a slightly taller block of noise, so the
 * snow jumps like a real TV's and never spills outside the picture. */
#define NOISE_W    86
#define NOISE_ROWS 136
/*
 * Creates a blank w x h CEL of 16-bit pixels for drawing into, or NULL
 * (setting crt_failed, so the plain fallbacks are used) if memory is short.
 * The cel engine pads rows to whole words: an odd width gets one more pixel,
 * and rows are at least 4 pixels.
 */
static CCB *ui_cel(Menu *menu, int w, int h)
{
    int stride = (w + 1) & ~1;
    CCB *cel = CreateCel(w, h, 16, CREATECEL_UNCODED, NULL);
    if (stride < 4)
        stride = 4;
    if (!cel)
        menu->crt_failed = 1;
    else
        memset(cel->ccb_SourcePtr, 0, (size_t)stride * (size_t)h * 2);
    return cel;
}
/* Creates the TV's CELs once: the static, the channel digits and bar, the
 * "CH" label and the screen's corner. */
static void crt_cels(Menu *menu)
{
    static const unsigned short grays[6] = {
        (unsigned short)MakeRGB15(3, 3, 4),    (unsigned short)MakeRGB15(6, 6, 7),
        (unsigned short)MakeRGB15(9, 9, 10),   (unsigned short)MakeRGB15(13, 13, 14),
        (unsigned short)MakeRGB15(17, 17, 18), (unsigned short)MakeRGB15(22, 22, 23)};
    unsigned short *pixels;
    unsigned long seed = 12345UL;
    int x, y;
    if (menu->noise || menu->crt_failed)
        return;
    menu->noise_pixels = (unsigned short *)malloc(NOISE_W * NOISE_ROWS * 2);
    if (menu->noise_pixels) {
        menu->noise = CreateCel(NOISE_W, 120, 16, CREATECEL_UNCODED, menu->noise_pixels);
        if (!menu->noise) {
            free(menu->noise_pixels);
            menu->noise_pixels = NULL;
        }
    }
    if (!menu->noise)
        menu->crt_failed = 1;
    menu->osd_digits = ui_cel(menu, 38, 16);
    menu->osd_bar = ui_cel(menu, 80, 8);
    menu->osd_ch = ui_cel(menu, 16, 12);
    menu->screen_corner = ui_cel(menu, 9, 9);
    if (menu->screen_corner) {
        /* Top-left corner of the tube, radius 9, in the recess color; flipped
         * for the others. The cel engine pads each 9-pixel row to 10 pixels. */
        pixels = (unsigned short *)menu->screen_corner->ccb_SourcePtr;
        for (y = 0; y < 9; ++y)
            for (x = 0; x < 9; ++x) {
                int dx = 2 * (9 - x) - 1, dy = 2 * (9 - y) - 1;
                if (dx * dx + dy * dy > 18 * 18)
                    pixels[y * 10 + x] = (unsigned short)INK;
            }
    }
    if (menu->osd_ch) {
        /* "CH" in the channel digits' blocky style, at 2x2 pixels per dot: font
         * text over bright video showed a dark fringe once averaging was off.
         * Drawn twice, first as a dark shadow one pixel down and right. */
        static const char *const ch[2] = {"111100100100111", "101101111101101"};
        int shadow, g, r, k, a, b;
        pixels = (unsigned short *)menu->osd_ch->ccb_SourcePtr;
        for (shadow = 1; shadow >= 0; --shadow)
            for (g = 0; g < 2; ++g)
                for (r = 0; r < 5; ++r)
                    for (k = 0; k < 3; ++k)
                        if (ch[g][r * 3 + k] == '1')
                            for (a = 0; a < 2; ++a)
                                for (b = 0; b < 2; ++b)
                                    pixels[(r * 2 + a + shadow) * 16 + g * 8 + k * 2 + b + shadow] =
                                        (unsigned short)(shadow ? INK : OSD_GREEN);
    }
    menu->osd_number = menu->osd_lit = -1; /* nothing drawn into them yet */
    /* Fill the static with random grays (a simple linear congruential
     * generator; it only needs to look random). */
    if (menu->noise) {
        pixels = menu->noise_pixels;
        for (x = 0; x < NOISE_W * NOISE_ROWS; ++x) {
            seed = seed * 1103515245UL + 12345UL;
            pixels[x] = grays[(seed >> 16) % 6U];
        }
    }
}
/* Draws a CEL at (x, y) at `scale` times its size, upside down if `flip`. */
static void draw_ui_cel(Menu *menu, CCB *cel, int x, int y, int scale, int flip)
{
    CCB copy;
    memcpy(&copy, cel, sizeof(copy));
    copy.ccb_XPos = (long)x << 16;
    copy.ccb_YPos = (long)y << 16;
    copy.ccb_HDX = (long)scale << 20;
    copy.ccb_HDY = 0;
    copy.ccb_VDX = 0;
    copy.ccb_VDY = flip ? -((long)scale << 16) : (long)scale << 16;
    draw_cel(menu, &copy);
}
/* The gecko on the channel card plays a short animation (from
 * channel_gecko_data.h) each time the selection changes. */
static void init_channel_gecko(Menu *menu)
{
    menu->channel_gecko = CreateCel(CHANNEL_GECKO_SIZE, CHANNEL_GECKO_SIZE, 4, 0, NULL);
    if (!menu->channel_gecko)
        return;
    memcpy(menu->channel_gecko->ccb_PLUTPtr, channel_gecko_palette, sizeof(channel_gecko_palette));
    menu->gecko_uploaded = -1;
}
/* The gecko's animation frame at time `now`; 0 (its resting pose) when it
 * is not animating. */
static int channel_gecko_frame(Menu *menu, unsigned long now)
{
    unsigned long frame;
    if (!menu->channel_gecko || !menu->gecko_active)
        return 0;
    frame = (now - menu->gecko_since) / CHANNEL_GECKO_FRAME_MS;
    if (frame >= CHANNEL_GECKO_FRAMES) {
        menu->gecko_active = 0;
        return 0;
    }
    return (int)frame;
}
/* Draws the gecko's current frame, if it is showing. */
static void draw_channel_gecko(Menu *menu)
{
    if (!menu->channel_gecko)
        return;
    /* Copy the frame's pixels into the CEL only when the frame changes. */
    if (menu->gecko_uploaded != menu->gecko_frame) {
        memcpy(menu->channel_gecko->ccb_SourcePtr, channel_gecko_pixels[menu->gecko_frame], 512);
        menu->gecko_uploaded = menu->gecko_frame;
    }
    draw_ui_cel(menu, menu->channel_gecko, 148, 1, 1, 0);
}
/* The static shows through the splash's screen opening (x 10-309, y 10-229)
 * around its solid logo panel (x 52-267, y 62-173); assets/startup.cel. Once
 * both buffers hold the panel, a frame repaints only these strips, in pieces
 * no wider than the static's source at double size. */
/* The startup static changes about 8 times a second: each frame costs
 * about 18 ms on the console, while the menu is busy starting. */
#define BOOT_STATIC_MS 125UL
static const short splash_static[6][4] = {{10, 10, 150, 52},  {160, 10, 150, 52},
                                          {10, 174, 150, 56}, {160, 174, 150, 56},
                                          {10, 62, 42, 112},  {268, 62, 42, 112}};
/* An uncoded 16-bit cel cut down to w x h pixels starting at source. */
static void cel_part(CCB *cel, const unsigned short *source, int w, int h)
{
    cel->ccb_SourcePtr = (CelData *)(void *)source;
    cel->ccb_PRE0 = (cel->ccb_PRE0 & ~0x0000ffc0UL) | ((unsigned long)(h - 1) << 6);
    cel->ccb_PRE1 = (cel->ccb_PRE1 & ~0x000007ffUL) | (unsigned long)(w - 1);
    cel->ccb_Width = w;
    cel->ccb_Height = h;
}
/* Static over one strip, from a random place in its source. */
static void noise_part(Menu *menu, const short *strip, unsigned long seed)
{
    CCB copy;
    unsigned long rows = (unsigned long)strip[3] / 2, cols = (unsigned long)strip[2] / 2, row, col;
    /* Scramble the seed's bits (a standard integer hash finisher), so
     * consecutive seeds land far apart. */
    seed ^= seed >> 15;
    seed *= 2246822519UL;
    seed ^= seed >> 13;
    row = seed % (NOISE_ROWS - rows + 1);
    /* Even columns keep the source word-aligned for the cel engine. */
    col = ((seed >> 16) % ((NOISE_W - cols) / 2 + 1)) * 2;
    memcpy(&copy, menu->noise, sizeof(copy));
    cel_part(&copy, menu->noise_pixels + row * NOISE_W + col, (int)cols, (int)rows);
    draw_ui_cel(menu, &copy, strip[0], strip[1], 2, 0);
}
/* The splash's own pixels over one strip: its corners and channel number. */
static void splash_part(Menu *menu, const short *strip)
{
    CCB copy;
    memcpy(&copy, menu->splash, sizeof(copy));
    cel_part(&copy, (const unsigned short *)menu->splash->ccb_SourcePtr + strip[1] * 320 + strip[0],
             strip[2], strip[3]);
    copy.ccb_XPos = (long)strip[0] << 16;
    copy.ccb_YPos = (long)strip[1] << 16;
    DrawCels(menu->screen.sc_BitmapItems[menu->screen.sc_curScreen], &copy);
}
/* A 172x240 strip of TV static at x, drawn at 2x; a new seed picks a new
 * frame. */
static void noise_at(Menu *menu, int x, unsigned long seed)
{
    CCB copy;
    seed ^= seed >> 15;
    seed *= 2246822519UL;
    seed ^= seed >> 13;
    memcpy(&copy, menu->noise, sizeof(copy));
    /* Even pixel offsets keep the source word-aligned for the cel engine. */
    copy.ccb_SourcePtr =
        (CelData *)(void *)(menu->noise_pixels + (seed % ((NOISE_ROWS - 120) * NOISE_W / 2)) * 2);
    draw_ui_cel(menu, &copy, x, 0, 2, 0);
}
/* The tube's rounded corners over whatever the screen just showed. A negative
 * direction draws the cel mirrored back from its position, so mirrored
 * corners start one past the screen's last pixel. */
static void tv_corners(Menu *menu)
{
    CCB copy;
    int i;
    if (!menu->screen_corner)
        return;
    for (i = 0; i < 4; ++i) {
        int right = i & 1, bottom = i >> 1;
        memcpy(&copy, menu->screen_corner, sizeof(copy));
        copy.ccb_XPos = ((long)VIDEO_X + (right ? (long)USB_PREVIEW_WIDTH : 0L)) << 16;
        copy.ccb_YPos = ((long)VIDEO_Y + (bottom ? (long)USB_PREVIEW_HEIGHT : 0L)) << 16;
        copy.ccb_HDX = right ? -(1L << 20) : 1L << 20;
        copy.ccb_HDY = 0;
        copy.ccb_VDX = 0;
        copy.ccb_VDY = bottom ? -(1L << 16) : 1L << 16;
        draw_cel(menu, &copy);
    }
}
/* TV static on the screen while a preview loads: an 80x60 window of the noise
 * cel drawn at 2x, so the snow keeps its chunky grain. */
static void tv_static(Menu *menu, unsigned long now)
{
    CCB copy;
    /* A new frame of static about 15 times a second. */
    unsigned long seed = (now / 67UL + 1UL) * 2654435761UL;
    if (!menu->noise) {
        rectangle(menu, VIDEO_X, VIDEO_Y, USB_PREVIEW_WIDTH, USB_PREVIEW_HEIGHT, DIM);
        tv_corners(menu);
        return;
    }
    seed ^= seed >> 15;
    seed *= 2246822519UL;
    seed ^= seed >> 13;
    memcpy(&copy, menu->noise, sizeof(copy));
    copy.ccb_SourcePtr =
        (CelData *)(void *)(menu->noise_pixels + (seed % ((NOISE_ROWS - 60) * NOISE_W / 2)) * 2);
    /* Cut the CEL to 80x60 (the preamble words hold the size minus one). */
    copy.ccb_PRE0 = (copy.ccb_PRE0 & ~0x0000ffc0UL) | (59UL << 6);
    copy.ccb_PRE1 = (copy.ccb_PRE1 & ~0x000007ffUL) | 79UL;
    copy.ccb_Width = 80;
    copy.ccb_Height = 60;
    draw_ui_cel(menu, &copy, VIDEO_X, VIDEO_Y, 2, 0);
    tv_corners(menu);
}
/* A rounded rectangle whose corner pixels are blended into the ground by how
 * much of them the curve covers, like the button caps. */
static void rounded_box(Menu *menu, int x, int y, int w, int h, int r, Color color, Color ground)
{
    int row;
    for (row = 0; row < h; ++row) {
        long dy = row < r ? 2L * (long)(r - row) - 1
                          : (row >= h - r ? 2L * (long)(row - (h - r)) + 1 : 0),
             root = 0, inset, first, last;
        if (!dy) {
            /* The straight rows in one fill (each costs a graphics call),
             * and the last alone, which then still covers the screen line
             * where overscan scaling makes it share the next row's. */
            if (h - r - row > 1)
                rectangle(menu, x, y + row, w, h - r - row - 1, color);
            rectangle(menu, x, y + h - r - 1, w, 1, color);
            row = h - r - 1;
            continue;
        }
        /* The corner circle's half-width at this row, times 8 (as in cap_shape). */
        while ((root + 1) * (root + 1) <= 64L * (4L * r * r - dy * dy))
            ++root;
        /* How far in the edge is, in sixteenths of a pixel. */
        inset = 16L * r - root;
        first = (inset + 15) / 16;
        last = (16L * w - inset) / 16;
        if (last > first)
            rectangle(menu, x + (int)first, y + row, (int)(last - first), 1, color);
        if (inset % 16) {
            Color edge = mix((unsigned int)color, (unsigned int)ground,
                             (unsigned int)(16 - inset % 16), 16U);
            rectangle(menu, x + (int)(inset / 16), y + row, 1, 1, edge);
            rectangle(menu, x + w - 1 - (int)(inset / 16), y + row, 1, 1, edge);
        }
    }
}
/* The portable TV: a charcoal cabinet with the screen set in a dark recess,
 * and speaker slots, a power light and two knobs along the bottom. */
static void tv_set(Menu *menu)
{
    int i;
    rounded_box(menu, 146, 34, 172, 143, 9, CABINET, BG);    /* the cabinet */
    rectangle(menu, 152, 35, 160, 2, MakeRGB15(10, 10, 12)); /* a highlight along its top */
    rounded_box(menu, 149, 41, 166, 128, 8, INK, CABINET);   /* the screen's recess */
    for (i = 0; i < 9; ++i)
        rectangle(menu, 152 + i * 5, 170, 3, 5, MakeRGB15(4, 4, 4)); /* speaker slots */
    rectangle(menu, 246, 170, 22, 5, MakeRGB15(3, 1, 1));
    rectangle(menu, 262, 171, 4, 3, REMOTE_RED); /* the power light */
    for (i = 0; i < 2; ++i) {
        Color knob = MakeRGB15(23, 23, 24);
        int x = 278 + i * 16;
        cap_shape(menu, x, 169, 9, 9, 9, mix((unsigned int)knob, (unsigned int)INK, 9U, 16U),
                  CABINET);
        cap_shape(menu, x, 168, 9, 9, 9, knob, CABINET);
        rectangle(menu, x + 4, 170, 1, 3, INK);
    }
}
/* A cover on the screen: at its own size, centered over a dimmed copy
 * stretched across the screen. A preview between plays fills it exactly. */
static void tv_picture(Menu *menu, CCB *source)
{
    CCB copy;
    long scale = 1L << 16; /* 1.0 in 16.16 fixed point */
    if (!source->ccb_Width || !source->ccb_Height)
        return;
    memcpy(&copy, source, sizeof(copy));
    if (source->ccb_Width > USB_PREVIEW_WIDTH)
        scale = ((long)USB_PREVIEW_WIDTH << 16) / source->ccb_Width;
    if (source->ccb_Height * scale > ((long)USB_PREVIEW_HEIGHT << 16))
        scale = ((long)USB_PREVIEW_HEIGHT << 16) / source->ccb_Height;
    copy.ccb_HDY = 0;
    copy.ccb_VDX = 0;
    rectangle(menu, VIDEO_X, VIDEO_Y, USB_PREVIEW_WIDTH, USB_PREVIEW_HEIGHT, BG);
    copy.ccb_XPos =
        ((long)VIDEO_X << 16) + (((long)USB_PREVIEW_WIDTH << 16) - source->ccb_Width * scale) / 2;
    copy.ccb_YPos =
        ((long)VIDEO_Y << 16) + (((long)USB_PREVIEW_HEIGHT << 16) - source->ccb_Height * scale) / 2;
    copy.ccb_HDX = scale << 4;
    copy.ccb_VDY = scale;
    draw_cel(menu, &copy);
    tv_corners(menu);
}
static void osd(Menu *menu, size_t selected);
static void control_badges(Menu *menu, int x, int y, const char *controls, int lit);
static const char *selected_preview(Menu *menu);

/* The tuning notice over the static, with its loading bar. */
static void tuning_overlay(Menu *menu, size_t selected)
{
    osd(menu, selected);
    rectangle(menu, 166, 82, 132, 46, BG);
    label_on(menu, 176, 86, "TUNING...", OSD_GREEN, UI_FONT_HEADING, BG);
    label_on(menu, 176, 109, "Any button cancels", MUTED, UI_FONT_SMALL, BG);
}
/* On-screen display: channel number (list position) and a 16-cell bar. */
static void osd(Menu *menu, size_t selected)
{
    long number = (long)selected;
    draw_channel_gecko(menu);
    if (++number > 999)
        number = 999;
    if (!menu->osd_hidden) {
        char status[16];
        if (menu->osd_ch)
            draw_ui_cel(menu, menu->osd_ch, 244, 14, 1, 0);
        else
            label(menu, 238, 12, "CH", OSD_GREEN, UI_FONT_SMALL);
        /* Status readout under the channel number, as a TV shows its mode. */
        if (menu->recovery_media_off)
            strcpy(status, "MEDIA OFF");
        else if (config_dirty(menu))
            strcpy(status, "UNSAVED");
        else if (menu->playlist.count)
            format(status, "QUEUE %lu", (unsigned long)menu->playlist.count);
        else
            status[0] = 0;
        if (*status) {
            /* On a dark tag, so the letters never blend into bright video. */
            int width =
                ui_font_width(status, UI_FONT_SMALL) * 320 / (320 - 2 * menu->config.screen_margin);
            rectangle(menu, 232 - width, 12, width + 8, 15, INK);
            label_on(menu, 236 - width, 14, status, config_dirty(menu) ? AMBER : OSD_GREEN,
                     UI_FONT_SMALL, INK);
        }
        /* Redraw the three digits' CEL only when the number changes: each dot
         * 3x3 pixels, with a dark shadow one pixel down and right. */
        if (menu->osd_digits && number != menu->osd_number) {
            unsigned short *pixels = (unsigned short *)menu->osd_digits->ccb_SourcePtr;
            int d, shadow, r, k, a, b;
            memset(pixels, 0, 38 * 16 * 2);
            for (shadow = 1; shadow >= 0; --shadow)
                for (d = 0; d < 3; ++d) {
                    const char *glyph = osd_font[(number / (d == 0 ? 100 : d == 1 ? 10 : 1)) % 10];
                    for (r = 0; r < 5; ++r)
                        for (k = 0; k < 3; ++k)
                            if (glyph[r * 3 + k] == '1')
                                for (a = 0; a < 3; ++a)
                                    for (b = 0; b < 3; ++b)
                                        pixels[(r * 3 + a + shadow) * 38 + d * 12 + k * 3 + b +
                                               shadow] = (unsigned short)(shadow ? INK : OSD_GREEN);
                }
            menu->osd_number = number;
        }
        if (menu->osd_digits)
            draw_ui_cel(menu, menu->osd_digits, 268, 10, 1, 0);
        else {
            char line[8];
            format(line, "%03ld", number);
            label(menu, 268, 8, line, OSD_GREEN, UI_FONT_HEADING);
        }
    }
    if (menu->tuning) {
        /* Only a preview that is loading has a bar: across the foot of the
         * screen, filling toward the largest clip, about 1.4 MiB. */
        long lit = (long)(menu->tuning_bytes / (1400UL * 1024UL / 16UL));
        if (lit > 15)
            lit = 15;
        /* Redraw the bar's 16 cells only when the lit count changes. */
        if (menu->osd_bar && lit != menu->osd_lit) {
            unsigned short *pixels = (unsigned short *)menu->osd_bar->ccb_SourcePtr;
            int cell, row, col;
            memset(pixels, 0, 80 * 8 * 2);
            for (cell = 0; cell < 16; ++cell)
                for (row = 0; row < 7; ++row)
                    for (col = 0; col < 3; ++col)
                        pixels[row * 80 + cell * 5 + col] =
                            (unsigned short)(cell < lit ? OSD_GREEN : MakeRGB15(3, 8, 3));
            menu->osd_lit = lit;
        }
        rectangle(menu, 156, 150, 88, 11, INK);
        if (menu->osd_bar)
            draw_ui_cel(menu, menu->osd_bar, 160, 152, 1, 0);
    } else if (!menu->osd_hidden) {
        /* Tap swaps video/cover; hold expands the current picture. */
        const char *swap = NULL;
        if (menu->preview_off && menu->preview_has < 0)
            menu->preview_has = selected_preview(menu) != NULL;
        if (menu->preview_cel)
            swap = "X:Cover";
        else if (menu->preview_off && menu->preview_has)
            swap = "X:Video";
        else if (menu->cover)
            swap = "X:Hold";
        if (swap) {
            rectangle(menu, 254, 148, 54, 15, INK);
            control_badges(menu, 256, 149, swap, 0);
        }
    }
}

/* Drawn on the TV when there is no cover: a little picture and a caption. */
static void media_placeholder(Menu *menu, int folder)
{
    rounded(menu, 194, 79, 59, 46, LINE);
    rounded(menu, 198, 83, 48, 34, SELECT);
    if (folder) {
        rounded(menu, 205, 92, 16, 6, ACCENT);
        rounded(menu, 204, 96, 33, 15, ACCENT);
        rectangle(menu, 208, 101, 25, 6, SELECT);
    } else {
        /* Original little lizard silhouette: no game sprites or logos. */
        rounded(menu, 208, 90, 23, 23, ACCENT);
        rounded(menu, 222, 97, 20, 12, ACCENT);
        rectangle(menu, 210, 94, 10, 5, BG);
        rectangle(menu, 223, 94, 10, 5, BG);
        rectangle(menu, 219, 95, 5, 2, BG);
        rectangle(menu, 227, 104, 12, 2, SELECT);
    }
    rectangle(menu, 247, 88, 3, 3, TV_ORANGE);
    rectangle(menu, 247, 95, 3, 3, ACCENT);
    rectangle(menu, 204, 125, 11, 3, LINE);
    rectangle(menu, 233, 125, 11, 3, LINE);
    label(menu, folder ? 194 : 199, 134, folder ? "Browse folder" : "No artwork", MUTED,
          UI_FONT_SMALL);
}

/* ==== Playing previews ==== */

/* Counts a streamed frame decoded for display, and frames passed over, for
 * diagnostics. */
static void stream_shown(Menu *menu, unsigned long spent)
{
    unsigned long frames = menu->stream.frame;
    if (frames == menu->stream_shown)
        return;
    if (frames > menu->stream_shown + 1) {
        menu->preview_skipped += frames - menu->stream_shown - 1;
        if (menu->full_view)
            menu->preview_full_view_skipped += frames - menu->stream_shown - 1;
    }
    if (menu->full_view)
        ++menu->preview_full_view_decodes;
    menu->stream_shown = frames;
    ++menu->preview_decodes;
    menu->preview_decode_ms += spent;
    if (spent > menu->preview_decode_max_ms)
        menu->preview_decode_max_ms = spent;
}
/* Decodes the streamed preview up to the frame due, timing it per frame
 * decoded: overall, apart for passes when the reader was idle throughout,
 * and apart for full-screen frames. */
static int decode_stream(Menu *menu, unsigned long now, int picture, unsigned char *screen)
{
    PreviewStream *s = &menu->stream;
    unsigned long started = now_ms(menu), before = s->frame, chunks = s->chunks_read, spent, frames;
    int idle = !s->reading, result;
    result = preview_stream_decode(s, preview_stream_due(s, now), picture, screen);
    spent = now_ms(menu) - started;
    frames = s->frame - before;
    if (frames) {
        s->decode_ms += spent;
        s->decode_frames += frames;
        if (idle && !s->reading && chunks == s->chunks_read) {
            s->idle_ms += spent;
            s->idle_frames += frames;
        }
        if (picture == OM3DVID7_SCREEN) {
            s->full_ms += spent;
            s->full_frames += frames;
        }
    }
    return result;
}
/* Advances the streamed preview in the browser's TV picture; full screen
 * decodes in draw_full_stream instead. */
static void advance_stream(Menu *menu, unsigned long now)
{
    unsigned long started;
    int result;
    if (menu->preview_priming || menu->full_view)
        return;
    /* Not due yet: just keep the sound fed. */
    if (preview_stream_due(&menu->stream, now) < menu->stream.frame) {
        preview_stream_audio(&menu->stream, now, 0);
        return;
    }
    started = now_ms(menu);
    result = decode_stream(menu, now, OM3DVID7_TV, NULL);
    /* A missing chunk pauses the sound too, so they stay in step. */
    preview_stream_audio(&menu->stream, now, result == PREVIEW_STREAM_NOT_READY);
    if (result < 0) {
        stop_preview(menu);
        menu->preview_result = -3;
        return;
    }
    stream_shown(menu, now_ms(menu) - started);
}
/* Advances the playing preview to the frame due now, looping the sound
 * with it. */
static void advance_preview(Menu *menu, unsigned long now)
{
    unsigned long elapsed, tick;
    unsigned int frame;
    if (menu->stream.open) {
        advance_stream(menu, now);
        return;
    }
    if (menu->preview_priming || !menu->preview_cel || !menu->preview.table)
        return;
    /* The frame due: elapsed ms * fps / 1000, without overflowing. */
    elapsed = now - menu->preview_since;
    tick = (elapsed / 1000) * menu->preview.fps + ((elapsed % 1000) * menu->preview.fps) / 1000;
    if (tick >= menu->preview.frames) {
        /* Restart both timelines at the same VBL; audio never drifts across loops. */
        menu->preview_since = now;
        tick = 0;
        if (menu->preview_audio.status == 1)
            preview_audio_restart(&menu->preview_audio);
    }
    frame = (unsigned int)tick;
    /* Decode it if it is a new frame, counting any passed over. */
    if (frame != menu->preview_frame) {
        unsigned long decode_started = now_ms(menu), spent;
        int decoded;
        if (frame > menu->preview_frame + 1) {
            menu->preview_skipped += (unsigned long)frame - menu->preview_frame - 1;
            if (menu->full_view)
                menu->preview_full_view_skipped += (unsigned long)frame - menu->preview_frame - 1;
        }
        if (menu->full_view)
            ++menu->preview_full_view_decodes;
        decoded = usb_preview_frame(&menu->preview, frame,
                                    (unsigned char *)menu->preview_cel->ccb_SourcePtr);
        if (decoded < 0) {
            stop_preview(menu);
            menu->preview_result = -3;
        } else
            menu->preview_frame = frame;
        spent = now_ms(menu) - decode_started;
        ++menu->preview_decodes;
        menu->preview_decode_ms += spent;
        if (spent > menu->preview_decode_max_ms)
            menu->preview_decode_max_ms = spent;
    }
}
/* ==== Folders that act as games, and game details ==== */

/* The current folder below the volume, as the metadata index spells paths,
 * with a trailing '/' (empty at the top). Returns its length, or -1 when it
 * and room more bytes do not fit. */
static long relative_folder(Menu *menu, char *path, size_t size, size_t room)
{
    unsigned int part, first = menu->depth && !(menu->path_ids[0] & 0xffffffUL) ? 1 : 0;
    size_t used = 0;
    for (part = first; part < menu->depth; ++part) {
        size_t length = strlen(menu->path[part]);
        if (used + length + 1 + room >= size)
            return -1;
        memcpy(path + used, menu->path[part], length);
        used += length;
        path[used++] = '/';
    }
    if (used + room >= size)
        return -1;
    path[used] = 0;
    return (long)used;
}
/* A folder that the metadata index shows holding exactly one game, either a
 * single disc or a complete same-folder disc set, acts as that game. Fills
 * discs with the disc names in play order and returns how many (0 for an
 * ordinary folder, including any with subfolders of games). The names point
 * into the index, which a volume change reloads. */
static size_t metadata_folder_game(Menu *menu, const char *prefix, const char **discs)
{
    static char probe[9217];
    MenuEntry candidates[ODE_MAX_DISCS];
    const MenuEntry *pointers[ODE_MAX_DISCS], *set[ODE_MAX_DISCS];
    OdeListing listing;
    size_t used = strlen(prefix), low = 0, high = menu->metadata.count, count = 0, position, i;
    if (used >= sizeof(probe))
        return 0;
    /* The index is sorted, so everything inside the folder is one run:
     * binary search for where the run starts. */
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (menu_path_compare(menu->metadata.entries[mid].path, prefix) < 0)
            low = mid + 1;
        else
            high = mid;
    }
    /* Collect the discs directly in the folder; any deeper path means the
     * folder holds subfolders of games, so it is an ordinary folder. */
    for (i = low; i < menu->metadata.count; ++i) {
        const char *path = menu->metadata.entries[i].path;
        if (strlen(path) <= used)
            break;
        memcpy(probe, path, used);
        probe[used] = 0;
        if (menu_path_compare(probe, prefix))
            break;
        /* The disc's name becomes part of the path the menu launches. */
        if (!library_path_safe(path))
            continue;
        if (strchr(path + used, '/') || count == ODE_MAX_DISCS)
            return 0;
        candidates[count].toc_id = 0;
        candidates[count].is_directory = 0;
        candidates[count].name = path + used;
        pointers[count] = candidates + count;
        ++count;
    }
    if (count == 1) {
        discs[0] = candidates[0].name;
        return 1;
    }
    if (!count)
        return 0;
    /* Several discs: they must form one complete set (Disc 1, 2, ...). */
    memset(&listing, 0, sizeof(listing));
    listing.entries = pointers;
    listing.count = count;
    if (menu_disc_group(&listing, pointers[0], set, ODE_MAX_DISCS, &position) != count)
        return 0;
    for (i = 0; i < count; ++i)
        discs[i] = set[i]->name;
    return count;
}
/* metadata_folder_game for a folder in the current listing. */
static size_t resolve_folder_game(Menu *menu, const MenuEntry *entry, const char **discs)
{
    static char prefix[9217];
    long folder;
    if (!entry || !entry->is_directory || !menu->metadata.count)
        return 0;
    folder = relative_folder(menu, prefix, sizeof(prefix), strlen(entry->name) + 1);
    if (folder < 0)
        return 0;
    strcpy(prefix + folder, entry->name);
    strcat(prefix, "/");
    return metadata_folder_game(menu, prefix, discs);
}
/*
 * The cached GameLookup for an entry, making one (and working out whether a
 * folder acts as a game) if needed. The cached pointers point into the
 * listing and the metadata index, so the whole cache is dropped whenever
 * either might have changed: a new listing, a reloaded index (even if
 * malloc happened to reuse its address, since the count is compared too) or
 * another folder.
 */
static GameLookup *game_lookup(Menu *menu, const MenuEntry *entry)
{
    GameLookup *lookup;
    unsigned int i;
    if (!menu->game_lookups_valid || menu->lookup_listing_generation != menu->listing_generation ||
        menu->lookup_metadata != menu->metadata.entries ||
        menu->lookup_metadata_count != menu->metadata.count || menu->lookup_depth != menu->depth) {
        memset(menu->game_lookups, 0, sizeof(menu->game_lookups));
        menu->lookup_listing_generation = menu->listing_generation;
        menu->lookup_metadata = menu->metadata.entries;
        menu->lookup_metadata_count = menu->metadata.count;
        menu->lookup_depth = menu->depth;
        menu->lookup_next = 0;
        menu->game_lookups_valid = 1;
    }
    for (i = 0; i < GAME_LOOKUP_SLOTS; ++i)
        if (menu->game_lookups[i].entry == entry)
            return menu->game_lookups + i;
    lookup = menu->game_lookups + menu->lookup_next;
    menu->lookup_next = (menu->lookup_next + 1) % GAME_LOOKUP_SLOTS;
    memset(lookup, 0, sizeof(*lookup));
    lookup->entry = entry;
    if (entry->is_directory)
        lookup->count = resolve_folder_game(menu, entry, lookup->discs);
    return lookup;
}
/* For a folder acting as a game, fills `discs` with its discs in play order
 * and returns how many; 0 for an ordinary folder or a file. */
static size_t folder_game(Menu *menu, const MenuEntry *entry, const char **discs)
{
    GameLookup *lookup;
    if (!entry || !entry->is_directory)
        return 0;
    lookup = game_lookup(menu, entry);
    memcpy(discs, lookup->discs, lookup->count * sizeof(*discs));
    return lookup->count;
}
/* Resolve the selected game's metadata once, including absent metadata. */
static const LibraryGameInfo *game_info(Menu *menu, const MenuEntry *chosen)
{
    static char path[9217];
    GameLookup *lookup;
    long used;
    if (!chosen)
        return NULL;
    lookup = game_lookup(menu, chosen);
    if (lookup->info_valid)
        return lookup->info;
    lookup->info_valid = 1;
    if (chosen->is_directory && !lookup->count)
        return NULL;
    used =
        relative_folder(menu, path, sizeof(path),
                        strlen(chosen->name) + (lookup->count ? strlen(lookup->discs[0]) + 1 : 0));
    if (used < 0)
        return NULL;
    strcpy(path + used, chosen->name);
    if (lookup->count) {
        strcat(path, "/");
        strcat(path, lookup->discs[0]);
    }
    lookup->info = library_metadata_find(&menu->metadata, path);
    return lookup->info;
}
/* Shows and plays as a game: a disc image, or a folder acting as its game. */
static int game_entry(Menu *menu, const MenuEntry *entry)
{
    return entry && (!entry->is_directory || game_lookup(menu, entry)->count != 0);
}
/* Orders TitleHashes by hash, then by position in the list. */
static int compare_title_hash(const void *a, const void *b)
{
    const TitleHash *left = (const TitleHash *)a, *right = (const TitleHash *)b;
    if (left->hash != right->hash)
        return left->hash < right->hash ? -1 : 1;
    return left->index < right->index ? -1 : left->index != right->index;
}
/* Sorts TitleHashes with a Shell sort; the C library's qsort took about a
 * millisecond per entry on the console (see menu_sort.c). */
static void sort_title_hashes(TitleHash *hashes, size_t count)
{
    static const size_t gaps[] = {701, 301, 132, 57, 23, 10, 4, 1};
    size_t g, k, j;
    for (g = 0; g < sizeof(gaps) / sizeof(gaps[0]); ++g) {
        for (k = gaps[g]; k < count; ++k) {
            TitleHash item = hashes[k];
            for (j = k; j >= gaps[g] && compare_title_hash(&hashes[j - gaps[g]], &item) > 0;
                 j -= gaps[g])
                hashes[j] = hashes[j - gaps[g]];
            hashes[j] = item;
        }
    }
}

/*
 * The number shown after a game's title when other games in the list share
 * it ("Doom [1]", "Doom [2]"), or 0 when its title is unique. Index is the
 * entry's position among the shown entries.
 *
 * Worked out once per listing (not on every move) and kept as a 2-byte rank
 * per shown entry.
 */
static unsigned int title_variant(Menu *menu, size_t index)
{
    static char first[ODE_MAX_NAME], other[ODE_MAX_NAME];
    const MenuEntry **entries;
    TitleHash *hashes;
    unsigned long key = visible_key(menu);
    size_t count, i, j, end, n = 0;
    key = (key ^ (unsigned long)menu->metadata.entries) * 16777619UL;
    key = (key ^ (unsigned long)menu->metadata.count) * 16777619UL;
    if (!menu->title_variants_valid || key != menu->title_variants_key) {
        menu->title_variants_valid = 1;
        menu->title_variants_key = key;
        menu->title_variants_count = 0;
        entries = visible_entries(menu, &count);
        if (!entries || !count)
            return 0;
        if (count > menu->title_variants_capacity) {
            unsigned short *grown =
                (unsigned short *)realloc(menu->title_variants, count * sizeof(*grown));
            if (!grown)
                return 0;
            menu->title_variants = grown;
            menu->title_variants_capacity = count;
        }
        memset(menu->title_variants, 0, count * sizeof(*menu->title_variants));
        hashes = (TitleHash *)malloc(count * sizeof(*hashes));
        if (!hashes)
            return 0;
        /* Every entry is grouped first by the name before its first tag
         * group and extension: display titles drop only tags, so equal
         * titles share it. Titles, and whether a folder acts as a game (an
         * index search), are then worked out only for shared ones, keeping
         * their games. Doing both for every entry of a large folder took
         * half a second on the console. */
        for (i = 0; i < count; ++i) {
            const char *name = entries[i]->name;
            size_t length = menu_title_length(name, 0), k;
            unsigned long hash = 2166136261UL;
            for (k = 0;
                 k < length && !(name[k] == ' ' && (name[k + 1] == '(' || name[k + 1] == '[')); ++k)
                hash = (hash ^ (unsigned char)name[k]) * 16777619UL;
            hashes[n].hash = hash;
            hashes[n++].index = i;
        }
        sort_title_hashes(hashes, n);

        /* For each group of entries sharing a name hash... */
        for (i = 0; i < n; i = end) {
            size_t games = i;
            for (end = i + 1; end < n && hashes[end].hash == hashes[i].hash; ++end) {
            }
            if (end - i < 2)
                continue;
            /* Each title once, as a hash; a title takes a millisecond. */
            for (j = i; j < end; ++j) {
                const char *c;
                unsigned long hash = 2166136261UL;
                menu_display_title(first, sizeof(first), entries[hashes[j].index]->name, 0);
                for (c = first; *c; ++c)
                    hash = (hash ^ (unsigned char)*c) * 16777619UL;
                hashes[j].title = hash;
            }
            /* Keep, in order, the games whose title another entry shares
             * (moving them to the start of the group). */
            for (j = i; j < end; ++j) {
                size_t k;
                for (k = i; k < end && (k == j || hashes[k].title != hashes[j].title); ++k) {
                }
                if (k < end && game_entry(menu, entries[hashes[j].index]))
                    hashes[games++] = hashes[j];
            }
            /* Number each game among those with exactly its title (the
             * hashes could collide, so the titles are compared too). */
            for (j = i; j < games; ++j) {
                size_t k, matches = 0, rank = 0;
                menu_display_title(first, sizeof(first), entries[hashes[j].index]->name, 0);
                for (k = i; k < games; ++k)
                    if (hashes[k].title == hashes[j].title) {
                        menu_display_title(other, sizeof(other), entries[hashes[k].index]->name, 0);
                        if (!strcmp(first, other)) {
                            ++matches;
                            if (k <= j)
                                ++rank;
                        }
                    }
                if (matches > 1)
                    menu->title_variants[hashes[j].index] = (unsigned short)rank;
            }
        }
        free(hashes);
        menu->title_variants_count = count;
    }
    return index < menu->title_variants_count ? menu->title_variants[index] : 0;
}
/* ==== Drawing the browser ==== */

/* Splits text into two title card lines at a space; returns the first
 * line's length (the whole text if it fits, or a cut mid-word if no space). */
static size_t card_split(const char *text, int room)
{
    size_t length = strlen(text), fit = ui_font_fit(text, length, room, UI_FONT_SMALL), split = fit;
    if (fit == length)
        return length;
    while (split && text[split] != ' ')
        --split;
    return split ? split : fit;
}
/*
 * Draws list row i (rows start..start+visible are on screen). The selected
 * row is highlighted like a lit remote control key, and its long name
 * scrolls. Rows are queued discs while the queue screen shows, otherwise
 * "< Back" and the folder's entries; folders get a ">" at the right.
 */
static void draw_row(Menu *menu, size_t i, size_t start, size_t selected, int style, int row_height)
{
    char line[24];
    const char *name;
    size_t length;
    int y = 56 + (int)(i - start) * row_height, x = 16, room = 122, directory = 0;
    unsigned int variant = 0;
    Color ink = MUTED, background = BG;
    if (i == selected) {
        /* A lit remote key: gecko-green, dark text. */
        background = menu->alphabet_active ? MUTED : ACCENT;
        ink = INK;
        rectangle(menu, 8, y - 1, 134, row_height, background);
    }
    if (menu->review) {
        name = menu->playlist.paths[i];
        if (!strncmp(name, "/Storage/", 9))
            name += 9;
        format(line, "%lu", (unsigned long)i + 1);
        label_on(menu, x, y, line, ACCENT, style, background);
        x += 16;
        room -= 16;
    } else if (menu->listing.has_parent && !i) {
        name = "< Back";
        length = 6;
    } else {
        const MenuEntry *entry = browser_entry(menu, i - (menu->listing.has_parent ? 1 : 0));
        name = entry->name;
        directory = !game_entry(menu, entry);
        if (!directory)
            variant = title_variant(menu, i - (menu->listing.has_parent ? 1 : 0));
    }
    if (directory) {
        label_on(menu, 134, y, ">", i == selected ? INK : DIM, style, background);
        room -= 10;
    }
    name = display_game_variant(name, directory, variant);
    length = strlen(name);
    scroll_label(menu, x, y, room, name, length, style, ink, background, i == selected);
}
/* Draws the current preview frame on the TV's 160x120 screen. Playback
 * redraws only this, so the solid list panel is never touched. */
static void gecko_video(Menu *menu)
{
    CCB copy;
    unsigned long started = now_ms(menu);
    memcpy(&copy, menu->preview_cel, sizeof(copy));
    copy.ccb_XPos = (long)VIDEO_X << 16;
    copy.ccb_YPos = (long)VIDEO_Y << 16;
    copy.ccb_HDX = ((long)USB_PREVIEW_WIDTH << 20) / menu->preview_cel->ccb_Width;
    copy.ccb_HDY = 0;
    copy.ccb_VDX = 0;
    copy.ccb_VDY = ((long)USB_PREVIEW_HEIGHT << 16) / menu->preview_cel->ccb_Height;
    draw_cel(menu, &copy);
    menu->preview_drawn_decode = menu->preview_decodes;
    menu->preview_video_ms += now_ms(menu) - started;
    ++menu->preview_video_draws;
    tv_corners(menu);
}
/* The OSD's X badge (254,148 54x15) sits on the picture. Drawing it costs
 * many fills, so after it is drawn its pixels are copied from the screen and
 * playback puts the copy back over each new frame. Screen bitmaps hold two
 * lines per 32-bit word: the upper line in the high half. Each row is copied
 * separately, including odd first/last rows, to preserve the full badge.
 *
 * badge_rect gives the badge's area in screen pixels (after the margin). */
static void badge_rect(Menu *menu, int *x, int *y, int *w, int *h)
{
    int margin = menu->config.screen_margin, top, bottom;
    *x = margin + 254 * (320 - 2 * margin) / 320;
    *w = margin + 308 * (320 - 2 * margin) / 320 - *x;
    top = margin + 148 * (240 - 2 * margin) / 240;
    bottom = margin + 163 * (240 - 2 * margin) / 240;
    *y = top;
    *h = bottom - top;
}
/* Copies the badge's pixels from the screen into the `badge` CEL. */
static void badge_save(Menu *menu)
{
#ifndef UI_PREVIEW_GLASS /* the host renderer has no screen bitmap */
    Bitmap *bitmap = menu->screen.sc_Bitmaps[menu->screen.sc_curScreen];
    int x, y, w, h, row, col, stride;
    unsigned short *pixels;
    badge_rect(menu, &x, &y, &w, &h);
    menu->badge_saved = 0;
    if (w <= 0 || h <= 0 || (long)x + w > (long)bitmap->bm_Width ||
        (long)y + h > (long)bitmap->bm_Height)
        return;
    if (menu->badge && (menu->badge->ccb_Width != w || menu->badge->ccb_Height != h)) {
        UnloadCel(menu->badge);
        menu->badge = NULL;
    }
    if (!menu->badge)
        menu->badge = CreateCel(w, h, 16, CREATECEL_UNCODED, NULL);
    if (!menu->badge)
        return;
    pixels = (unsigned short *)menu->badge->ccb_SourcePtr;
    /* The CEL's row length, from its preamble: (words per row - 2) is
     * stored, and each word holds two pixels. */
    stride = (int)((((menu->badge->ccb_PRE1 >> PRE1_WOFFSET10_SHIFT) & 0x3ff) + 2) *
                   2); /* halfwords per row */
    for (row = 0; row < h; ++row) {
        const unsigned short *line =
            (const unsigned short *)(void *)(bitmap->bm_Buffer +
                                             ((long)(y + row) >> 1) * bitmap->bm_Width * 4) +
            ((y + row) & 1);
        for (col = 0; col < w; ++col)
            pixels[row * stride + col] = line[(x + col) * 2];
    }
    menu->badge->ccb_XPos = (long)x << 16;
    menu->badge->ccb_YPos = (long)y << 16;
    menu->badge->ccb_HDX = 1L << 20;
    menu->badge->ccb_HDY = 0;
    menu->badge->ccb_VDX = 0;
    menu->badge->ccb_VDY = 1L << 16;
    menu->badge->ccb_Flags |= CCB_LAST;
    menu->badge_saved = 1;
#else
    (void)menu;
#endif
}
/* Puts the saved badge back over a new frame (or draws the OSD if none). */
static void badge_restore(Menu *menu, size_t selected)
{
    if (menu->osd_hidden)
        return;
#ifndef UI_PREVIEW_GLASS
    if (menu->badge_saved) {
        /* Screen pixels already: no margin scaling. */
        DrawCels(menu->screen.sc_BitmapItems[menu->screen.sc_curScreen], menu->badge);
        return;
    }
#endif
    osd(menu, selected);
}
/*
 * A hash of what the playback overlays (the status message, notice, OSD and
 * scrolling names) look like now, short of drawing them. While it stays the
 * same, playback need not redraw them. With `scrolling`, the scroll offsets
 * of names drawn by scroll_label count too.
 */
static unsigned long playback_overlay_key(Menu *menu, const char *message, int scrolling)
{
    unsigned long key = 2166136261UL, elapsed = menu->scroll_elapsed;
    int i;
    const char *c;
    if (message)
        for (c = message; *c; ++c)
            key = (key ^ (unsigned char)*c) * 16777619UL;
    key = (key ^ (unsigned long)menu->notice_active) * 16777619UL;
    key = (key ^ (unsigned long)menu->osd_hidden) * 16777619UL;
    for (i = 0; scrolling && i < menu->scroll_count; ++i)
        key = (key ^ (unsigned long)menu_scroll_offset(menu->scroll_length[i],
                                                       menu->scroll_visible[i], elapsed)) *
              16777619UL;
    return key;
}
/* playback_overlay_key, including the scrolling names. */
static unsigned long overlay_key(Menu *menu, const char *message)
{
    return playback_overlay_key(menu, message, 1);
}
/* A hash of everything the list panel shows, so playback can tell when it
 * is unchanged (and need not be redrawn). */
static unsigned long panel_hash(const Menu *menu, size_t selected, size_t total)
{
    unsigned long hash = 2166136261UL, values[15];
    const char *text[2];
    size_t i;
    values[0] = (unsigned long)selected;
    values[1] = (unsigned long)total;
    values[2] = (unsigned long)menu->view_valid;
    values[3] = (unsigned long)menu->ready;
    values[4] = (unsigned long)menu->loading;
    values[5] = (unsigned long)menu->listing.entries;
    values[6] = (unsigned long)menu->listing.count;
    values[7] = (unsigned long)menu->listing.has_parent;
    values[8] = (unsigned long)menu->depth;
    values[9] = (unsigned long)menu->config.screen_margin;
    values[10] = (unsigned long)menu->config.large_titles;
    values[11] = (unsigned long)config_dirty(menu);
    values[12] = (unsigned long)menu->playlist.count;
    values[13] = (unsigned long)menu->logo;
    values[14] = (unsigned long)menu->alphabet_active;
    for (i = 0; i < 15; ++i)
        hash = (hash ^ values[i]) * 16777619UL;
    text[0] = menu->breadcrumb;
    text[1] = menu->filter;
    for (i = 0; i < 2; ++i) {
        const char *c;
        for (c = text[i]; *c; ++c)
            hash = (hash ^ (unsigned char)*c) * 16777619UL;
        hash *= 31UL;
    }
    return hash;
}
/*
 * Draws the title card under the TV: the selected game's title (scrolling if
 * long), then either a status message in amber or two lines of details
 * (genre and year, publisher). Folders, "Back" and the queue get their own
 * lines. With `title_only`, just the title line. `scale` is the screen width
 * after the margin, for measuring text.
 */
static void draw_title_card(Menu *menu, const MenuEntry *chosen, size_t total, const char *message,
                            int scale, int title_only)
{
    /* Title card: a lower-third band, flush with the picture's left, bottom
     * and right edges so no sliver of video shows around it. */
    rectangle(menu, 152, 178, 168, title_only ? 24 : 62, BG);
    {
        const LibraryGameInfo *info = game_info(menu, chosen);
        const char *title = NULL, *second = NULL, *third = NULL;
        char details[96], more[96];
        size_t title_length = 0;
        details[0] = more[0] = 0;
        if (menu->review) {
            title = "Disc queue";
            second = "A plays from the first disc";
            third = "C moves, C + X clears";
        } else if (chosen && !game_entry(menu, chosen)) {
            title = chosen->name;
            second = "Folder";
            third = "A opens it";
        } else if (chosen) {
            title = display_game_variant(
                chosen->name, 0,
                title_variant(menu, menu->selected - (menu->listing.has_parent ? 1 : 0)));
            title_length = strlen(title);
            if (info) {
                format(details, "%.60s", *info->genre ? info->genre : "Unknown genre");
                if (*info->released) {
                    strcat(details, "  -  ");
                    strncat(details, info->released, 4);
                }
                format(more, "%.50s", *info->publisher ? info->publisher : "");
            } else
                strcpy(details, "No game information");
            second = details;
            third = more;
        } else if (total) {
            title = "Back";
            second = "Return to the parent folder";
        } else
            title = menu->review ? "Disc queue" : "Library";
        if (title) {
            if (!title_length)
                title_length = menu_title_length(title, chosen ? chosen->is_directory : 1);
            scroll_label(menu, 160, 181, 152, title, title_length, UI_FONT_HEADING, TEXT, BG, 1);
        }
        if (title_only)
            return;
        if (message && *message) {
            /* A message over two lines, broken at a space. */
            size_t split = card_split(message, 152 * scale / 320);
            memcpy(details, message, split < 95 ? split : 95);
            details[split < 95 ? split : 95] = 0;
            label_on(menu, 160, 203, details, AMBER, UI_FONT_SMALL, BG);
            while (message[split] == ' ')
                ++split;
            if (message[split]) {
                strncpy(more, message + split, 95);
                more[95] = 0;
                more[ui_font_fit(more, strlen(more), 152 * scale / 320, UI_FONT_SMALL)] = 0;
                label_on(menu, 160, 217, more, AMBER, UI_FONT_SMALL, BG);
            }
        } else {
            if (second && *second) {
                if (second != details) {
                    strncpy(details, second, 95);
                    details[95] = 0;
                }
                details[ui_font_fit(details, strlen(details), 152 * scale / 320, UI_FONT_SMALL)] =
                    0;
                label_on(menu, 160, 203, details, ACCENT, UI_FONT_SMALL, BG);
            }
            if (third && *third) {
                if (third != more) {
                    strncpy(more, third, 95);
                    more[95] = 0;
                }
                more[ui_font_fit(more, strlen(more), 152 * scale / 320, UI_FONT_SMALL)] = 0;
                label_on(menu, 160, 217, more, MUTED, UI_FONT_SMALL, BG);
            }
        }
    }
}
/* Draws the TV picture when no preview plays: the cover, or a placeholder. */
static void browser_picture(Menu *menu, const MenuEntry *chosen, size_t selected)
{
    if (menu->cover)
        tv_picture(menu, menu->cover);
    else {
        rectangle(menu, VIDEO_X, VIDEO_Y, USB_PREVIEW_WIDTH, USB_PREVIEW_HEIGHT, HEADER);
        media_placeholder(menu, chosen ? !game_entry(menu, chosen)
                                       : (menu->listing.has_parent && !selected));
        tv_corners(menu);
    }
}
/* Which set of control prompts applies (see browser_controls), as a number
 * that changes when the prompts would. */
static int browser_controls_key(Menu *menu, const MenuEntry *chosen, size_t total)
{
    if (!menu->ready || !total)
        return 0;
    if (!chosen)
        return 1;
    return game_entry(menu, chosen) ? 3 : 2;
}
/* Draws the control prompts under the list. */
static void browser_controls(Menu *menu, const MenuEntry *chosen, size_t total)
{
    /* Three prompt rows below the shorter list; help is always discoverable. */
    rectangle(menu, 8, 196, 138, 44, BG);
    if (menu->review) {
        if (total)
            control_badges(menu, 12, 197, "A:Play", 1);
    } else if (menu->ready && total) {
        if (!chosen)
            control_badges(menu, 12, 197, "A:Back", 1);
        else if (!game_entry(menu, chosen))
            control_badges(menu, 12, 197, "A:Open", 1);
        else
            control_badges(menu, 12, 197, menu->playlist.count ? "P:Queue C:Add" : "A:Play C:Queue",
                           1);
    }
    control_badges(menu, 12, 212,
                   menu->review || menu->depth || menu->listing.has_parent ? "B:Back L+R:Tools"
                                                                           : "L+R:Tools",
                   0);
    button(menu, 12, 226, "Hold P", "Help", 0);
}
/*
 * The browser is drawn in three layers, each with a hash saying what it
 * shows (see BrowserFrame): the "shell" (the panel and list, for this
 * listing and settings), the picture on the TV, and the overlays. A layer
 * is redrawn into a buffer only when its hash differs from what that buffer
 * last showed.
 */
static unsigned long browser_shell(Menu *menu, size_t total)
{
    unsigned long hash = panel_hash(menu, 0, total);
    hash = (hash ^ menu->listing_generation) * 16777619UL;
    hash = (hash ^ (unsigned long)menu->metadata.entries) * 16777619UL;
    hash = (hash ^ (unsigned long)menu->metadata.count) * 16777619UL;
    return (hash ^ (unsigned long)menu->config.hide_adult) * 16777619UL;
}
/* The picture layer's hash (see browser_shell). */
static unsigned long browser_picture_key(Menu *menu, const MenuEntry *chosen, size_t selected)
{
    unsigned long folder =
        chosen ? !game_entry(menu, chosen) : (menu->listing.has_parent && !selected);
    unsigned long hash = (2166136261UL ^ folder) * 16777619UL;
    hash = (hash ^ (unsigned long)menu->cover) * 16777619UL;
    hash = (hash ^ (unsigned long)menu->osd_hidden) * 16777619UL;
    hash = (hash ^ (unsigned long)menu->preview_off) * 16777619UL;
    return hash;
}
/* The overlay layer's hash for a buffer (see browser_shell). */
static unsigned long browser_overlay_key(Menu *menu, const BrowserFrame *frame, const char *message)
{
    unsigned long key;
    int i, count = menu->scroll_count;
    /* The other buffer may have drawn a different game's scrolling names. */
    menu->scroll_count = 0;
    key = overlay_key(menu, message);
    menu->scroll_count = count;
    for (i = 0; i < frame->scroll_count; ++i)
        key = (key ^ (unsigned long)menu_scroll_offset(
                         frame->scroll_length[i], frame->scroll_visible[i], menu->scroll_elapsed)) *
              16777619UL;
    return key;
}
/* Records what a screen buffer now shows (see BrowserFrame). */
static void browser_save(Menu *menu, BrowserFrame *frame, unsigned long shell,
                         unsigned long picture, int controls, size_t selected, size_t start,
                         const char *message)
{
    frame->valid = 1;
    frame->shell = shell;
    frame->picture = picture;
    frame->selected = selected;
    frame->start = start;
    frame->overlay = overlay_key(menu, message);
    frame->scroll_count = menu->scroll_count;
    frame->controls = controls;
    frame->gecko = menu->gecko_frame;
    frame->letter = menu->alphabet_active ? menu->jump_letter : -1;
    memcpy(frame->scroll_length, menu->scroll_length, sizeof(frame->scroll_length));
    memcpy(frame->scroll_visible, menu->scroll_visible, sizeof(frame->scroll_visible));
}
/* Startup: live static shows through the splash's transparent tube, and the
 * tuning bar fills a quarter per startup stage. Drawn without screen margins,
 * like the boot banner it replaces. */
static void boot_screen(Menu *menu, unsigned long now)
{
    int margin = menu->config.screen_margin, i;
    unsigned long started = now_ms(menu);
    menu->config.screen_margin = 0;
    menu->screen.sc_curScreen ^= 1;
    if (menu->splash) {
        crt_cels(menu);
        if (menu->noise && menu->splash_frames >= 2) {
            for (i = 0; i < 6; ++i)
                noise_part(menu, splash_static[i],
                           (now / BOOT_STATIC_MS + 1UL) * 2654435761UL +
                               (unsigned long)i * 40503UL);
            for (i = 0; i < 6; ++i)
                splash_part(menu, splash_static[i]);
        } else {
            if (menu->noise) {
                noise_at(menu, 0, (now / BOOT_STATIC_MS + 1UL) * 2654435761UL);
                noise_at(menu, 160, (now / BOOT_STATIC_MS + 1UL) * 40503UL);
            } else
                rectangle(menu, 0, 0, 320, 240, DIM);
            DrawCels(menu->screen.sc_BitmapItems[menu->screen.sc_curScreen], menu->splash);
            ++menu->splash_frames;
        }
        for (i = 0; i < 16; ++i)
            rectangle(menu, 120 + i * 5, 206, 3, 7,
                      i < menu->boot_stage * 4 ? OSD_GREEN : MakeRGB15(3, 8, 3));
    } else
        rectangle(menu, 0, 0, 320, 240, BG);
    DisplayScreen(menu->screen.sc_Screens[menu->screen.sc_curScreen], 0);
    menu->config.screen_margin = margin;
    ++menu->boot_draws;
    menu->boot_draw_ms += now_ms(menu) - started;
}
static unsigned char *screen_pixels(Menu *menu, int screen);
/* Copy the screen just drawn into the other buffer; 0 when it cannot. A
 * full browser draw takes several video frames (hundreds of fills), so
 * playback after one draws only its picture and captions over the copy. */
static int copy_screen(Menu *menu)
{
#ifdef OH3DO_SDK
    int shown = menu->screen.sc_curScreen;
    if (menu->sport < 0)
        return 0;
    sport_wait(menu);
    return CopyVRAMPages(menu->sport, screen_pixels(menu, shown ^ 1), screen_pixels(menu, shown),
                         OM3DVID7_FRAME_BYTES / GetPageSize(MEMTYPE_VRAM), 0xffffffffUL) >= 0;
#else
    (void)menu;
    return 0;
#endif
}
/*
 * What draw() works out once and its helpers share: the list's layout, the
 * selection and the message to show.
 */
typedef struct DrawState {
    size_t total;            /* rows in the list */
    size_t selected;         /* the selected row (in the list or the queue) */
    size_t rows;             /* rows per page */
    size_t start;            /* the first row on this page */
    const MenuEntry *chosen; /* the selected entry, or NULL for "< Back" or the queue */
    const char *message;     /* the status line to show, if any */
    int style, row_height;
    int scale; /* the screen width after the margin, for measuring text */
    unsigned long now;
} DrawState;

/*
 * Shows "Reading folder..." while a folder loads. The current path already
 * names the new folder, but the listing still belongs to the old one, so the
 * old picture is kept rather than redrawn with the wrong entries.
 */
static void draw_loading_notice(Menu *menu)
{
    menu->browser_frames[0].valid = menu->browser_frames[1].valid = 0;
    if (menu->loading_notice)
        return;
    menu->text_cursor = 0;
    menu->text_style = UI_FONT_NORMAL;
    rectangle(menu, 152, 178, 168, 62, BG);
    label_on(menu, 160, 181, "Reading folder...", AMBER, UI_FONT_NORMAL, BG);
    label_on(menu, 160, 203, "Please wait", MUTED, UI_FONT_SMALL, BG);
    DisplayScreen(menu->screen.sc_Screens[menu->screen.sc_curScreen], 0);
    menu->loading_notice = 1;
}

/* Works out which message the status line shows, in order of priority. */
static const char *browser_message(Menu *menu, char *line, unsigned long now)
{
    const char *message = menu->message;

    if (menu->notice_active && now - menu->notice_since >= 2500UL)
        menu->notice_active = 0; /* notices last two and a half seconds */
    if (!message && menu->notice_active)
        message = menu->notice;
    if (menu->loading) {
        strcpy(line, "Reading folder...");
        message = line;
    }
    if (menu->alphabet_active && !menu->notice_active)
        message = "Choose a letter. A to jump, B to cancel.";
    if (!menu->ready && menu->bus.last_error < 0) {
        format(line, "Connection error: %08lx", (unsigned long)menu->bus.last_error);
        message = line;
    }
    return message;
}

/*
 * Redraws only what changed in a buffer that already shows this folder's
 * browser (its shell hash matches; see browser_shell). Returns 1 if it
 * handled the frame, 0 if a full draw is needed.
 */
static int draw_browser_changes(Menu *menu, const DrawState *d, BrowserFrame *frame,
                                unsigned long shell, unsigned long picture, int controls)
{
    unsigned long key = browser_overlay_key(menu, frame, d->message);
    int moved;
    size_t i;

    if (!frame->valid || frame->shell != shell || menu->clear_frames)
        return 0;
    moved = frame->selected != d->selected;

    if (!moved && frame->picture == picture && frame->overlay == key) {
        /* Nothing but perhaps the gecko or the alphabet strip changed. */
        BrowserFrame *front = &menu->browser_frames[menu->screen.sc_curScreen ^ 1];
        int letter = menu->alphabet_active ? menu->jump_letter : -1;
        if (frame->gecko != menu->gecko_frame || frame->letter != letter) {
            if (frame->gecko != menu->gecko_frame)
                draw_channel_gecko(menu);
            if (frame->letter != letter)
                draw_alphabet_change(menu, frame->letter);
            frame->gecko = menu->gecko_frame;
            frame->letter = letter;
            DisplayScreen(menu->screen.sc_Screens[menu->screen.sc_curScreen], 0);
            return 1;
        }
        /* Show this ready back buffer if the front still shows an old
         * selection. Otherwise leave the display alone and poll again. */
        if (front->valid && front->shell == shell && front->selected == d->selected &&
            front->picture == picture && front->overlay == key &&
            front->gecko == menu->gecko_frame && front->letter == letter)
            menu->screen.sc_curScreen ^= 1;
        else
            DisplayScreen(menu->screen.sc_Screens[menu->screen.sc_curScreen], 0);
        return 1;
    }

    /* The list: a new page means every row; otherwise the old and new
     * selected rows. */
    menu->scroll_count = 0;
    if (frame->start != d->start) {
        rectangle(menu, 8, 55, 134, (int)d->rows * d->row_height, BG);
        for (i = d->start; i < d->total && i < d->start + d->rows; ++i)
            draw_row(menu, i, d->start, d->selected, d->style, d->row_height);
    } else {
        if (moved) {
            rectangle(menu, 8, 55 + (int)(frame->selected - d->start) * d->row_height, 134,
                      d->row_height, BG);
            draw_row(menu, frame->selected, d->start, d->selected, d->style, d->row_height);
        }
        if (d->total) {
            rectangle(menu, 8, 55 + (int)(d->selected - d->start) * d->row_height, 134,
                      d->row_height, BG);
            draw_row(menu, d->selected, d->start, d->selected, d->style, d->row_height);
        }
    }

    /* The TV picture, the channel display, the prompts and the title card. */
    if (frame->picture != picture)
        browser_picture(menu, d->chosen, d->selected);
    if (moved || frame->picture != picture) {
        rectangle(menu, 152, 0, 168, 34, BG);
        osd(menu, d->selected);
    } else if (frame->gecko != menu->gecko_frame) {
        draw_channel_gecko(menu);
    }
    if (frame->controls != controls)
        browser_controls(menu, d->chosen, d->total);
    draw_title_card(menu, d->chosen, d->total, d->message, d->scale, 0);
    if (menu->alphabet_active)
        draw_alphabet(menu);

    browser_save(menu, frame, shell, picture, controls, d->selected, d->start, d->message);
    DisplayScreen(menu->screen.sc_Screens[menu->screen.sc_curScreen], 0);
    menu->panel_frames = 0;
    return 1;
}

/*
 * The light redraw during preview playback, once both buffers hold this
 * list panel: the new video frame, and the selected row (its long name may
 * be scrolling), the OSD and the caption only until both buffers hold their
 * current look. Drawing those every frame would take most of a 30 fps
 * frame's time.
 */
static void draw_playback_frame(Menu *menu, const DrawState *d)
{
    unsigned long key = overlay_key(menu, d->message);
    unsigned long fixed = playback_overlay_key(menu, d->message, 0);

    /* Text CELs are cached by drawing order: keep these few labels in their
     * own slots, so the next full draw finds the list's labels still made. */
    menu->text_cursor = UI_TEXT_CACHE - 16;
    if (fixed != menu->playback_static_key) {
        menu->playback_static_key = fixed;
        menu->playback_static_frames = 0;
    }
    if (key != menu->overlay_key) {
        menu->overlay_key = key;
        menu->overlay_frames = 0;
    }
    if (menu->overlay_frames >= 2 && menu->preview_drawn_decode == menu->preview_decodes) {
        /* The front already holds this video frame and both buffers have
         * the current caption. Keep polling without another CEL pass. */
        menu->screen.sc_curScreen ^= 1;
        return;
    }

    gecko_video(menu);
    if (menu->overlay_frames < 2) {
        ++menu->overlay_frames;
        menu->scroll_count = 0;
        if (menu->view_valid && d->total)
            draw_row(menu, d->selected, d->start, d->selected, d->style, d->row_height);
        if (menu->playback_static_frames < 2) {
            osd(menu, d->selected);
            draw_title_card(menu, d->chosen, d->total, d->message, d->scale, 0);
            badge_save(menu);
            ++menu->playback_static_frames;
        } else {
            draw_title_card(menu, d->chosen, d->total, d->message, d->scale, 1);
            badge_restore(menu, d->selected);
        }
        menu->overlay_key = overlay_key(menu, d->message);
    } else {
        badge_restore(menu, d->selected);
    }
    DisplayScreen(menu->screen.sc_Screens[menu->screen.sc_curScreen], 0);
    ++menu->preview_light_draws;
    menu->preview_draw_ms += now_ms(menu) - d->now;
}

/* The TV set and what plays on it: static, the preview, a cover or a placeholder. */
static void draw_tv(Menu *menu, const DrawState *d, int playing)
{
    rectangle(menu, 0, 0, 320, 240, BG);
    tv_set(menu);
    if (menu->tuning) {
        tv_static(menu, d->now);
        ++menu->tuning_draws;
    } else if (playing) {
        gecko_video(menu);
    } else if (menu->preview_cel || menu->cover) {
        tv_picture(menu, menu->preview_cel ? menu->preview_cel : menu->cover);
    } else {
        rectangle(menu, VIDEO_X, VIDEO_Y, USB_PREVIEW_WIDTH, USB_PREVIEW_HEIGHT, HEADER);
        media_placeholder(menu, d->chosen ? !game_entry(menu, d->chosen)
                                          : (menu->listing.has_parent && !d->selected));
        tv_corners(menu);
    }
}

/* The logo and the folder heading (or "Disc queue", or the search). */
static void draw_heading(Menu *menu, const DrawState *d)
{
    char line[96];

    if (menu->logo) {
        CCB logo = *menu->logo;
        logo.ccb_XPos = 12 << 16;
        logo.ccb_YPos = 2 << 16;
        logo.ccb_HDX = 1 << 20;
        logo.ccb_VDY = 1 << 16;
        draw_cel(menu, &logo);
    } else {
        label_on(menu, 12, 12, "Oh My 3DO", TEXT, UI_FONT_NORMAL, BG);
    }

    if (menu->review)
        strcpy(line, "Disc queue");
    else if (*menu->filter)
        format(line, "Search: %s", menu->filter);
    else if (!menu->depth)
        strcpy(line, "All games");
    else /* the breadcrumb without its leading "/ " */
        strcpy(line, menu->breadcrumb[0] == '/' && menu->breadcrumb[1] == ' ' ? menu->breadcrumb + 2
                                                                              : menu->breadcrumb);
    line[ui_font_fit(line, strlen(line), 104 * d->scale / 320, UI_FONT_SMALL)] = 0;
    rectangle(menu, 12, 42, 20, 6, REMOTE_RED);
    label_on(menu, 38, 38, line, TV_ORANGE, UI_FONT_SMALL, BG);
}

/* Draws the whole browser into the back buffer and shows it. */
static void draw_browser_full(Menu *menu, const DrawState *d, BrowserFrame *frame,
                              unsigned long shell, unsigned long picture, int controls, int playing,
                              unsigned long hash)
{
    size_t i;

    menu->playback_static_frames = 0;
    menu->scroll_count = 0;
    crt_cels(menu);
    /* After loading, clear both screen buffers once, including the margin
     * border that full-screen video never covers. */
    if (menu->clear_frames) {
        rectangle(menu, 0, 0, 320, 240, BG);
        --menu->clear_frames;
    }
    if (menu->review)
        backdrop(menu);
    else
        draw_tv(menu, d, playing);

    /* A solid ground lets changed rows be erased without blending old text. */
    draw_heading(menu, d);
    if (menu->view_valid || menu->review) {
        for (i = d->start; i < d->total && i < d->start + d->rows; ++i)
            draw_row(menu, i, d->start, d->selected, d->style, d->row_height);
        if (!d->total) {
            label_on(menu, 16, 62, menu->review ? "No discs queued" : "Nothing here yet", TEXT,
                     UI_FONT_NORMAL, BG);
            label_on(menu, 16, 80, *menu->filter ? "Try another name" : "Choose another folder",
                     MUTED, UI_FONT_SMALL, BG);
        }
    }
    /* Button prompts under the list: what A and C do right now, then the
     * ways out (B to the parent folder or the library, L+R into Tools). */
    browser_controls(menu, d->chosen, d->total);

    if (menu->tuning) {
        /* Changing channel: no caption, just the tuning notice over the static. */
        tuning_overlay(menu, d->selected);
        if (menu->alphabet_active)
            draw_alphabet(menu);
        DisplayScreen(menu->screen.sc_Screens[menu->screen.sc_curScreen], 0);
        return;
    }

    if (!menu->review)
        osd(menu, d->selected);
    draw_title_card(menu, d->chosen, d->total, d->message, d->scale, 0);
    if (menu->alphabet_active)
        draw_alphabet(menu);
    if (frame)
        browser_save(menu, frame, shell, picture, controls, d->selected, d->start, d->message);
    DisplayScreen(menu->screen.sc_Screens[menu->screen.sc_curScreen], 0);

    if (playing) {
        ++menu->preview_full_draws;
        menu->preview_draw_ms += now_ms(menu) - d->now;
    }

    /*
     * Count how many buffers now hold this list panel, so playback can
     * switch to light redraws (draw_playback_frame) once both do. Copying
     * this screen into the other buffer gets there at once.
     */
    menu->overlay_frames = 0;
    if (!playing) {
        menu->panel_frames = 0;
    } else if (copy_screen(menu)) {
        menu->panel_hash = hash;
        menu->panel_frames = 2;
        menu->clear_frames = 0;
    } else if (hash == menu->panel_hash) {
        ++menu->panel_frames;
    } else {
        menu->panel_hash = hash;
        menu->panel_frames = 1;
    }
}

/*
 * Draws the current screen into the back buffer and shows it: the startup
 * screen, help, Tools, the full-screen view or the browser. The browser is
 * the hard case, and draws as little as it can (see browser_shell and
 * draw_playback_frame).
 */
static void draw(Menu *menu)
{
    char line[96];
    DrawState d;
    unsigned long hash = 0;
    unsigned long shell = 0, picture = 0;
    BrowserFrame *frame = NULL;
    int playing, browsing, controls = 0;

    d.selected = menu->review ? menu->queue_selected : menu->selected;
    d.rows = menu_rows(menu);
    d.start = (d.selected / d.rows) * d.rows;
    d.now = now_ms(menu);
    d.chosen = NULL;
    d.style = menu->config.large_titles ? UI_FONT_NORMAL : UI_FONT_SMALL;
    d.row_height = menu->config.large_titles ? 18 : 14;
    d.scale = 320 - 2 * menu->config.screen_margin;

    if (menu->tools != TOOL_HOME || menu->help_hold.active || menu->full_view || menu->loading ||
        menu->booting)
        menu->tools_home_drawn = 0;
    /* A successful mount releases the browser listing. Keep the last launch
     * frame displayed until the game takes over, including this input pass. */
    if (menu->launching)
        return;
    if (menu->loading && menu->view_valid && !menu->booting) {
        draw_loading_notice(menu);
        return;
    }

    d.total = (menu->view_valid || menu->review) ? row_count(menu) : 0;
    menu->scroll_elapsed =
        (menu->scroll_paused ? menu->scroll_paused_at : d.now) - menu->selection_since;
    menu->gecko_frame = channel_gecko_frame(menu, d.now);
    browsing = !menu->booting && !menu->tools && !menu->full_view && !menu->review &&
               !menu->loading && !menu->tuning && !menu->preview_cel && menu->view_valid;
    if (!browsing)
        menu->browser_frames[0].valid = menu->browser_frames[1].valid = 0;

    /* The other screens draw themselves. */
    if (menu->booting) {
        boot_screen(menu, d.now);
        return;
    }
    if (menu->help_hold.active) {
        draw_context_help(menu);
        return;
    }
    menu->help_drawn = 0;
    if (menu->tools) {
        draw_tools(menu);
        return;
    }
    if (menu->full_view) {
        draw_full_cover(menu);
        return;
    }

    menu->text_cursor = 0;
    menu->text_style = UI_FONT_NORMAL;
    d.message = browser_message(menu, line, d.now);
    if (menu->view_valid && !menu->review && d.total && !(menu->listing.has_parent && !d.selected))
        d.chosen = browser_entry(menu, d.selected - (menu->listing.has_parent ? 1 : 0));

    /* Before deciding what to draw: a frame that fails to decode stops the preview. */
    if (!menu->review)
        advance_preview(menu, d.now);
    /* A preview is playing when its picture has the TV's shape (a cover
     * shown in the preview's place does not). */
    playing = !menu->review && !menu->tuning && menu->preview_cel &&
              menu->preview_cel->ccb_Width * USB_PREVIEW_HEIGHT ==
                  menu->preview_cel->ccb_Height * USB_PREVIEW_WIDTH;
    if (playing) {
        unsigned long pass = now_ms(menu);
        if (menu->preview_pass_at && pass - menu->preview_pass_at > menu->preview_pass_max_ms)
            menu->preview_pass_max_ms = pass - menu->preview_pass_at;
        menu->preview_pass_at = pass;
        d.now = pass;
        hash = panel_hash(menu, d.selected, d.total);
    }

    /* Draw into the other buffer from here on. */
    menu->screen.sc_curScreen ^= 1;
    if (browsing) {
        frame = &menu->browser_frames[menu->screen.sc_curScreen];
        shell = browser_shell(menu, d.total);
        picture = browser_picture_key(menu, d.chosen, d.selected);
        controls = browser_controls_key(menu, d.chosen, d.total);
        if (draw_browser_changes(menu, &d, frame, shell, picture, controls))
            return;
    }
    if (playing && !menu->clear_frames && menu->panel_frames >= 2 && hash == menu->panel_hash) {
        draw_playback_frame(menu, &d);
        return;
    }
    draw_browser_full(menu, &d, frame, shell, picture, controls, playing, hash);
}

/* ==== Loading folders, recovering and the media indexes ==== */

/* Called after every 2 KiB read and every directory batch, to keep the
 * screen moving during long reads; 15 redraws a second is enough. Preview
 * loads run with this hook disabled (art_at_root). */
static void progress(void *context, size_t count)
{
    Menu *menu = (Menu *)context;
    unsigned long now = now_ms(menu);
    menu->read_count = count;
    if (now - menu->progress_drawn < (menu->booting ? BOOT_STATIC_MS : 66UL))
        return;
    menu->progress_drawn = now;
    draw(menu);
}
/* The menu while it starts up, for menu_transfer (which is not given it). */
static Menu *startup_menu;

/* The OdeTransfer the menu uses: every ODE command passes through here, so
 * during startup it can keep the static moving (BOOT_STATIC_MS). */
static int menu_transfer(void *context, const unsigned char *command, unsigned char *reply,
                         size_t capacity, size_t *length)
{
    int result = ode_bus_transfer(context, command, reply, capacity, length);
    Menu *menu = startup_menu;
    if (menu && menu->booting && now_ms(menu) - menu->progress_drawn >= BOOT_STATIC_MS) {
        menu->progress_drawn = now_ms(menu);
        draw(menu);
    }
    return result;
}

/*
 * Reads the folder the drive emulator is now in, selecting row `selected`.
 * At the drive's top, also remembers what it looks like (for
 * recover_library). On failure the old listing stays but is marked out of
 * date (view_valid 0) with the error on the status line.
 */
static void load_directory(Menu *menu, size_t selected)
{
    OdeListing next;
    menu->filter[0] = 0;
    ode_listing_init(&next);
    menu->ready = 0;
    menu->loading = 1;
    menu->loading_notice = 0;
    menu->read_count = 0;
    menu->notice_active = 0;
    draw(menu);
    if (ode_read_directory(&menu->ode, &next) == 0) {
        free_listing(menu);
        menu->listing = next;
        /* The drive's top folder (not a list of volumes): remember it. */
        if (storage_root(menu)) {
            size_t i;
            for (i = 0; i < menu->listing.count; ++i)
                if (menu->listing.entries[i]->is_directory &&
                    !(menu->listing.entries[i]->toc_id & 0xffffffUL))
                    break;
            if (i == menu->listing.count) {
                menu->root_fingerprint = ode_listing_fingerprint(&menu->listing);
                menu->root_volume = menu->depth ? menu->path_ids[0] : 0;
                menu->root_known = 1;
            }
        }
        menu->ready = menu->view_valid = 1;
        menu->selected = selected;
        menu->message = NULL;
        if (menu->selected >= row_count(menu))
            menu->selected = 0;
    } else {
        menu->view_valid = 0;
        menu->message = menu->bus.timed_out ? "ODE timeout. Restart console." : menu->ode.error;
    }
    menu->loading = 0;
    update_breadcrumb(menu);
    selection_changed(menu);
    if (menu->booting)
        boot_step(menu, BOOT_FOLDER);
    if (menu->ready)
        refresh_usb_art(menu);
}
/*
 * Whether "Return to library" can be offered after browsing failed: the top
 * folder is known, the FCare answered sensibly so far, and an earlier
 * recovery did not fail.
 */
static int can_recover_library(const Menu *menu)
{
    return menu->root_known && !menu->demo && !menu->bus.timed_out &&
           !menu->ode.transport_uncertain && !strcmp(menu->ode.identity, "LOCODE") &&
           menu->recovery_result >= 0;
}
/*
 * Tries to get back to the drive's top folder after a USB failure (see
 * ode_recover_root). On success browsing works again, but covers and
 * previews stay off until a restart, since the media reads were what was
 * in doubt. Returns 0, or -1.
 */
static int recover_library(Menu *menu)
{
    OdeListing root;
    int result;
    if (!can_recover_library(menu)) {
        menu->tool_message = "Recovery unavailable. Power cycle console.";
        return -1;
    }
    ode_listing_init(&root);
    stop_preview(menu);
    close_full_cover(menu);
    menu->ready = menu->view_valid = 0;
    menu->tools = menu->review = menu->alphabet_active = 0;
    menu->message = "Returning to library...";
    draw(menu);
    result = ode_recover_root(&menu->ode, menu->root_fingerprint, menu->root_volume, &root);
    if (result) {
        menu->recovery_result = -1;
        menu->ode.navigation_failed = 1;
        menu->message = "Recovery failed. Power cycle console.";
        return -1;
    }
    free_listing(menu);
    menu->listing = root;
    menu->depth = menu->root_volume ? 1 : 0;
    menu->filter[0] = 0;
    menu->selected = 0;
    menu->ready = menu->view_valid = 1;
    menu->recovery_result = 1;
    menu->recovery_media_off = 1;
    menu->covers_enabled = 0;
    menu->ode.media_beside = 0;
    menu->ode.media_folder = NULL;
    cover_cache_clear(&menu->cover_cache);
    menu->clear_frames = 2;
    update_breadcrumb(menu);
    selection_changed(menu);
    menu->position_dirty = 0;
    notify(menu, "Recovered. Media off until restart.");
    return 0;
}

/*
 * At the drive's top folder, (re)loads what the menu keeps from the
 * .ohmy3do folder there: the categories, the cover and preview indexes and
 * the game metadata. Done once per USB volume; elsewhere it does nothing.
 */
static void refresh_usb_art(Menu *menu)
{
    unsigned int root = menu->depth && !(menu->path_ids[0] & 0xffffffUL) ? 1 : 0;
    size_t i;
    const char *message;
    unsigned long volume;
    if (menu->demo || menu->recovery_media_off || menu->depth != root)
        return;
    if (!menu->depth) {
        for (i = 0; i < menu->listing.count; ++i)
            if (!(menu->listing.entries[i]->toc_id & 0xffffffUL)) {
                menu->covers_enabled = 0;
                return; /* a list of volumes, not a folder with files in it */
            }
    }
    volume = root ? menu->path_ids[0] & 0xff000000UL
                  : (menu->listing.count ? menu->listing.entries[0]->toc_id & 0xff000000UL : 0);
    if (menu->art_indexes_valid && menu->art_index_volume == volume) {
        menu->covers_enabled = menu->config.usb_covers && menu->usb_art.count != 0;
        return;
    }
    /* A new volume (or the first time): forget the old indexes. */
    usb_art_index_free(&menu->usb_art);
    usb_art_index_free(&menu->usb_previews);
    menu->game_lookups_valid = 0;
    menu->title_variants_valid = 0;
    library_metadata_free(&menu->metadata);
    menu->covers_enabled = 0;
    if (menu->config_result == 2 || strcmp(menu->ode.identity, "LOCODE"))
        return;
    menu->art_index_volume = volume;
    menu->art_indexes_valid = 1; /* Missing indexes also wait for explicit refresh. */
    /*
     * The categories file. Without one, the defaults apply, with Adult
     * hidden if the old hide_adult setting asked for it. Once a categories
     * file is in charge, that setting is cleared: the file decides.
     */
    {
        char *text = (char *)malloc(MENU_CATEGORIES_BYTES);
        size_t length = 0;
        int beside = menu->ode.media_beside;
        const char *folder = menu->ode.media_folder;
        menu_categories_reset();
        menu->categories_valid = 0;
        menu->ode.media_beside = 0;
        menu->ode.media_folder = NULL;
        if (text) {
            int result = usb_file_text(&menu->ode, ode_bus_read_buffer, "categories.json", text,
                                       MENU_CATEGORIES_BYTES, &length);
            if (!result && length)
                menu->categories_valid = menu_categories_parse(text, length) == 0;
            else if (!menu->bus.timed_out && !menu->ode.navigation_failed) {
                menu->categories_valid = 1;
                if (menu->config.hide_adult)
                    menu_category_toggle(7);
            }
            free(text);
        }
        menu->ode.media_beside = beside;
        menu->ode.media_folder = folder;
        if (menu->booting)
            boot_step(menu, BOOT_CATEGORIES);
        if (menu->categories_valid) {
            menu->config.hide_adult = menu->saved_config.hide_adult = 0;
            menu_set_hide_adult(0);
        }
    }
    /* The indexes and metadata, each only if still connected. */
    message = menu->message;
    menu->message = "Reading media indexes...";
    draw(menu);
    if (menu->config.usb_covers)
        usb_art_index_load(&menu->ode, ode_bus_read_buffer, &menu->usb_art);
    if (menu->booting)
        boot_step(menu, BOOT_COVERS);
    if (menu->config.video_previews && !menu->bus.timed_out && !menu->ode.navigation_failed)
        usb_media_index_load(&menu->ode, ode_bus_read_buffer, &menu->usb_previews, "previews");
    if (menu->booting)
        boot_step(menu, BOOT_PREVIEWS);
    if (!menu->bus.timed_out && !menu->ode.navigation_failed)
        library_metadata_load(&menu->ode, ode_bus_read_buffer, &menu->metadata);
    if (menu->booting)
        boot_step(menu, BOOT_METADATA);
    menu->game_lookups_valid = 0;
    menu->message = message;
    menu->covers_enabled = menu->usb_art.count != 0;
    if (menu->bus.timed_out || menu->ode.navigation_failed) {
        menu->ode.navigation_failed = 1;
        menu->ready = menu->view_valid = 0;
        menu->message = "Cover I/O failed. Restart console.";
    }
}

/* Writes the current folder's full menu path ("/Storage/Games/USA") into
 * `buffer`. Returns 0, or -1 if it does not fit in `capacity`. */
static int directory_path(Menu *menu, char *buffer, size_t capacity)
{
    unsigned int i;
    size_t used = 8;
    strcpy(buffer, "/Storage");
    for (i = 0; i < menu->depth; ++i) {
        size_t n = strlen(menu->path[i]);
        if (used + n + 2 > capacity)
            return -1;
        buffer[used++] = '/';
        memcpy(buffer + used, menu->path[i], n + 1);
        used += n;
    }
    return 0;
}

/* ==== Covers and the full-screen view ==== */

/* What a cover read needs, passed through media_read to read_usb_cover. */
typedef struct UsbCoverRead {
    Menu *menu;
    const char *filename;
    unsigned char *pixels;
} UsbCoverRead;

/* While a preview loads (Gecko TV), the loader calls media_cancel before
 * every 2 KiB block. That paces the static at about 15 frames a second. A full
 * redraw costs about 28 ms, so after both screen buffers hold the list, each
 * frame redraws only the screen: static, its corners and the notice. */
static void tuning_tick(Menu *menu)
{
    unsigned long now = now_ms(menu);
    size_t selected;
    menu->tuning_bytes += 2048;
    if (now - menu->progress_drawn < 66UL)
        return;
    menu->progress_drawn = now;
    if (menu->tuning_full < 2 || menu->alphabet_active || menu->tools) {
        draw(menu);
        ++menu->tuning_full;
        return;
    }
    selected = menu->selected;
    menu->screen.sc_curScreen ^= 1;
    tv_static(menu, now);
    ++menu->tuning_draws;
    tuning_overlay(menu, selected);
    DisplayScreen(menu->screen.sc_Screens[menu->screen.sc_curScreen], 0);
}
/*
 * The MediaCancel the menu gives every media read: polls the controller
 * (and keeps the tuning static moving) between blocks. Any button but a
 * lone shoulder cancels the read, so scrolling past a game never waits for
 * its cover or preview; the directions pressed are remembered so the list
 * still moves by them afterwards.
 */
static int media_cancel(void *context)
{
    Menu *menu = (Menu *)context;
    ControlPadEventData pad;
    if (menu->tuning)
        tuning_tick(menu);
    if (GetControlPad(1, 0, &pad) >= 0) {
        u32 buttons = pad.cped_ButtonBits;
        u32 shoulders;
        /* Consumed buttons (including the X hold opening a large cover)
         * cannot cancel the read until released and pressed again. */
        menu->suppress_buttons &= buttons;
        buttons &= ~menu->suppress_buttons;
        shoulders = buttons & (ControlLeftShift | ControlRightShift);
        observe_tools_chord(menu, buttons);
        /* The first shoulder is often followed by the second a few polls
         * later. Keep polling the read until the complete chord is seen. */
        if ((buttons & ~(ControlLeftShift | ControlRightShift)) ||
            shoulders == (ControlLeftShift | ControlRightShift))
            menu->media_cancelled = 1;
        menu->cancel_directions |= buttons & DIRECTIONS;
        /* An action used to cancel is consumed until release, so it cannot
         * open/play the newly selected game on the next main-loop pass. */
        menu->suppress_buttons |= buttons & ~(DIRECTIONS | ControlLeftShift | ControlRightShift);
    }
    return menu->media_cancelled;
}

/* Runs `read` where the media is: beside the disc (without a trip to the
 * volume root) or, for drives laid out the old way, at the root (see
 * art_at_root). For a collapsed game folder, enters it temporarily so the
 * open command gets the short local media path, and verifies the return
 * without changing the browser or remembered position. */
static int media_read(Menu *menu, ArtRouteStep *route, int beside, ArtRootRead read, void *context)
{
    OdeProgress progress = menu->ode.progress;
    int result;
    if (!beside)
        return art_at_root(&menu->ode, route, menu->depth, &menu->listing, folder_workaround(menu),
                           read, context);
    menu->ode.progress = NULL;
    menu->ode.media_beside = 1;
    if (menu->ode.media_folder) {
        const MenuEntry *folder = NULL;
        size_t i;
        for (i = 0; i < menu->listing.count; ++i)
            if (menu->listing.entries[i]->is_directory &&
                !strcmp(menu->listing.entries[i]->name, menu->ode.media_folder))
                folder = menu->listing.entries[i];
        result = art_in_folder(&menu->ode, &menu->listing, folder, folder_workaround(menu), read,
                               context);
    } else
        result = read(context);
    menu->ode.progress = progress;
    menu->ode.media_beside = 0;
    return menu->ode.navigation_failed ? -2 : result;
}
/* Reads the cover in a UsbCoverRead: a PNG, or a prepared .3dc. */
static int read_usb_cover(void *context)
{
    UsbCoverRead *read = (UsbCoverRead *)context;
    if (!menu_path_compare(read->filename + strlen(read->filename) - 4, ".png"))
        return usb_png_cover(&read->menu->ode, ode_bus_read_buffer, read->filename, read->pixels,
                             media_cancel, read->menu);
    return usb_fast_cover(&read->menu->ode, ode_bus_read_buffer, read->filename, read->pixels,
                          media_cancel, read->menu);
}

/* Reads the full-screen NAME-large.3dc for the cover in a UsbCoverRead. */
static int read_large_cover(void *context)
{
    UsbCoverRead *read = (UsbCoverRead *)context;
    return usb_large_cover(&read->menu->ode, ode_bus_read_buffer, read->filename, read->pixels,
                           media_cancel, read->menu);
}
/* The display hardware's blending of each pixel with its neighbors: off for
 * the menu's crisp text, on for full-screen video, where it softens the
 * codec's block edges at no cost. */
static void set_smoothing(Menu *menu, int on)
{
#ifndef UI_PREVIEW_GLASS
    int screen;
    for (screen = 0; screen < 2; ++screen) {
        if (on) {
            EnableHAVG(menu->screen.sc_Screens[screen]);
            EnableVAVG(menu->screen.sc_Screens[screen]);
        } else {
            DisableHAVG(menu->screen.sc_Screens[screen]);
            DisableVAVG(menu->screen.sc_Screens[screen]);
        }
    }
#else
    (void)menu;
    (void)on;
#endif
}
/* Fading out of full-screen video (the PC preview has no color tables). */
#ifndef UI_PREVIEW_GLASS
#define FADES 1
#else
#define FADES 0
#endif
#define FADE_OUT_MS 100UL
#define FADE_IN_MS  133UL

/* Scales both screens' color tables to level/256 of full brightness. Each
 * 5-bit level i becomes an 8-bit value (i << 3 | i >> 2) before scaling;
 * 256 restores the normal tables. */
static void set_brightness(Menu *menu, unsigned long level)
{
#if FADES
    u32 entries[32];
    int i, screen;
    for (i = 0; i < 32; ++i) {
        u32 value = (u32)((((unsigned long)i << 3) | ((unsigned long)i >> 2)) * level / 256UL);
        entries[i] = MakeCLUTColorEntry(i, value, value, value);
    }
    for (screen = 0; screen < 2; ++screen) {
        if (level >= 256UL)
            ResetScreenColors(menu->screen.sc_Screens[screen]);
        else
            SetScreenColors(menu->screen.sc_Screens[screen], entries, 32);
    }
#else
    (void)menu;
    (void)level;
#endif
}
static void close_full_cover(Menu *menu);
/*
 * One step of leaving full-screen video: fade out, draw the browser in the
 * dark, fade back in. The browser takes several video frames to draw, which
 * showed as a stall; in the dark it is not seen, along with the frames
 * playback must catch up on. Call every main-loop pass while `fade` is set.
 */
static void fade_step(Menu *menu)
{
    unsigned long elapsed;
    if (!menu->fade)
        return;
    elapsed = now_ms(menu) - menu->fade_since;
    if (menu->fade == 1) {
        if (elapsed < FADE_OUT_MS) {
            set_brightness(menu, 256UL - elapsed * 256UL / FADE_OUT_MS);
            return;
        }
        set_brightness(menu, 0);
        close_full_cover(menu);
        draw(menu);
        draw(menu);
        menu->fade = 2;
        menu->fade_since = now_ms(menu);
    } else if (elapsed < FADE_IN_MS)
        set_brightness(menu, elapsed * 256UL / FADE_IN_MS);
    else {
        set_brightness(menu, 256UL);
        menu->fade = 0;
    }
}
/* Leaves the full-screen view (cover or video) and frees the large cover. */
static void close_full_cover(Menu *menu)
{
    if (!menu->full_view)
        return;
    if (menu->full_view == 2)
        set_smoothing(menu, 0);
    sport_wait(menu);
    /* The TV picture missed the full-screen frames: redraw it from the map. */
    if (menu->stream.open && menu->stream.frame && menu->full_view == 2 &&
        preview_stream_decode(&menu->stream, menu->stream.frame - 1, OM3DVID7_TV, NULL) < 0)
        stop_preview(menu);
    if (menu->full_cover)
        UnloadCel(menu->full_cover);
    menu->full_cover = NULL;
    menu->full_view = 0;
    menu->full_cleared = 0;
    menu->clear_frames = 2;
    menu->panel_frames = 0;
}
/*
 * Opens the full-screen view of the selected game's cover (hold X): the
 * 192x240 large cover if the library has one, otherwise the normal cover
 * doubled. Pressing a button while it loads cancels.
 */
static void open_full_cover(Menu *menu)
{
    ArtRouteStep route[MAX_DEPTH];
    UsbCoverRead read;
    const char *message;
    CCB *cel = NULL;
    int result;
    if (!menu->cover || menu->full_view)
        return;
    if (menu->cover_file && !menu->demo &&
        (cel = CreateCel(USB_LARGE_COVER_WIDTH, USB_LARGE_COVER_HEIGHT, 16, CREATECEL_UNCODED,
                         NULL)) != NULL) {
        current_route(menu, route);
        read.menu = menu;
        read.filename = menu->cover_file;
        read.pixels = (unsigned char *)cel->ccb_SourcePtr;
        pause_scroll(menu);
        message = menu->message;
        menu->message = "Loading cover...";
        draw(menu);
        menu->media_cancelled = 0;
        menu->ode.media_folder = menu->cover_folder;
        result = media_read(menu, route, menu->usb_art.beside, read_large_cover, &read);
        menu->ode.media_folder = NULL;
        resume_scroll(menu);
        menu->message = message;
        if (result == -2 || menu->bus.timed_out) {
            UnloadCel(cel);
            menu->ode.navigation_failed = 1;
            menu->ready = menu->view_valid = 0;
            menu->message = "Cover I/O failed. Restart console.";
            return;
        }
        if (menu->media_cancelled) {
            UnloadCel(cel);
            return;
        }
        /* No large cover in the library: the normal one, doubled. */
        if (result < 0) {
            UnloadCel(cel);
            cel = NULL;
        }
    }
    if (cel) {
        /* Centered; CCB_BGND draws its black pixels instead of leaving
         * them transparent. */
        cel->ccb_XPos = (long)((320 - USB_LARGE_COVER_WIDTH) / 2) << 16;
        cel->ccb_YPos = 0;
        cel->ccb_Flags |= CCB_LAST | CCB_BGND;
    }
    menu->full_cover = cel;
    menu->full_view = 1;
}
/* A screen buffer's pixels (the host renderer has no bitmaps). */
static unsigned char *screen_pixels(Menu *menu, int screen)
{
#ifndef UI_PREVIEW_GLASS
    return (unsigned char *)menu->screen.sc_Bitmaps[screen]->bm_Buffer;
#else
    static unsigned char pixels[2][OM3DVID7_FRAME_BYTES];
    (void)menu;
    return pixels[screen];
#endif
}
/* Full screen for a streamed preview: decoded 1:1 straight into the back
 * screen, whatever the margin, which the video fills. Only a new frame is
 * drawn. After the flip, the SPORT hardware copies the new screen into the
 * other buffer in the same vertical blank, so the next frame starts from it
 * and draws only what changed. Without the copy, each frame redraws whole. */
static void draw_full_stream(Menu *menu)
{
    unsigned long now = now_ms(menu), started;
    int back = menu->screen.sc_curScreen ^ 1, result;
    if (menu->full_cleared && preview_stream_due(&menu->stream, now) < menu->stream.frame) {
        preview_stream_audio(&menu->stream, now, 0);
        return;
    }
    sport_wait(menu);
    if (!menu->full_cleared)
        preview_stream_invalidate(&menu->stream, OM3DVID7_SCREEN);
    started = now_ms(menu);
    result = decode_stream(menu, now, OM3DVID7_SCREEN, screen_pixels(menu, back));
    preview_stream_audio(&menu->stream, now, result == PREVIEW_STREAM_NOT_READY);
    if (result < 0) {
        close_full_cover(menu);
        stop_preview(menu);
        menu->preview_result = -3;
        return;
    }
    stream_shown(menu, now_ms(menu) - started);
    menu->screen.sc_curScreen = back;
    DisplayScreen(menu->screen.sc_Screens[back], 0);
#ifdef OH3DO_SDK
    if (menu->sport >= 0 &&
        CopyVRAMPagesDefer(menu->sport, screen_pixels(menu, back ^ 1), screen_pixels(menu, back),
                           OM3DVID7_FRAME_BYTES / GetPageSize(MEMTYPE_VRAM), 0xffffffffUL) >= 0)
        menu->sport_pending = 1;
    else
#endif
        preview_stream_invalidate(&menu->stream, OM3DVID7_SCREEN);
    menu->full_cleared = 1;
}
/* Draws the full-screen view: video (streamed or loaded) or a cover. */
static void draw_full_cover(Menu *menu)
{
    CCB copy;
    if (menu->full_view == 2 && menu->stream.open) {
        draw_full_stream(menu);
        return;
    }
    if (menu->full_view == 2) {
        advance_preview(menu, now_ms(menu));
        if (!menu->preview_cel) {
            close_full_cover(menu);
            return;
        }
        /* Input polling and audio continue even when no video frame is due.
         * Leave the displayed image alone rather than drawing it twice. */
        if (menu->full_cleared >= 2 && menu->full_drawn_decode == menu->preview_decodes)
            return;
    } else if (menu->full_cleared >= 2)
        return;
    menu->text_cursor = 0;
    menu->text_style = UI_FONT_SMALL;
    menu->screen.sc_curScreen ^= 1;
    /* A playing preview covers all but the margin, which keeps the
     * background once both buffers have it; filling the screen every frame
     * would cost the cel engine time a 30 fps frame cannot spare. */
    if (menu->full_cleared < 2) {
        rectangle(menu, 0, 0, 320, 240, BG);
        ++menu->full_cleared;
    }
    if (menu->full_view == 2) {
        memcpy(&copy, menu->preview_cel, sizeof(copy));
        /* Doubled to fill the screen. */
        copy.ccb_XPos = copy.ccb_YPos = 0;
        copy.ccb_HDX = (320L << 20) / menu->preview_cel->ccb_Width;
        copy.ccb_HDY = 0;
        copy.ccb_VDX = 0;
        copy.ccb_VDY = (240L << 16) / menu->preview_cel->ccb_Height;
        {
            unsigned long started = now_ms(menu);
            draw_cel(menu, &copy);
            menu->preview_video_ms += now_ms(menu) - started;
            ++menu->preview_video_draws;
        }
        menu->full_drawn_decode = menu->preview_decodes;
    } else if (menu->full_cover)
        draw_cel(menu, menu->full_cover);
    else if (menu->cover) {
        memcpy(&copy, menu->cover, sizeof(copy));
        copy.ccb_XPos = (160L - (long)menu->cover->ccb_Width) << 16;
        copy.ccb_YPos = (120L - (long)menu->cover->ccb_Height) << 16;
        copy.ccb_HDX = 2L << 20;
        copy.ccb_HDY = 0;
        copy.ccb_VDX = 0;
        copy.ccb_VDY = 2L << 16;
        draw_cel(menu, &copy);
    }
    /* The side margins beside the 192-pixel art hold the prompts. */
    if (menu->full_view == 1)
        control_badges(menu, 6, 224, "B:Back", 0);
    DisplayScreen(menu->screen.sc_Screens[menu->screen.sc_curScreen], 0);
}

/*
 * Writes the selected game's menu path into `path` (ENTRY_PATH_BYTES): its
 * disc, or for a folder acting as its game, that folder's first disc, whose
 * cover and preview stand for the game. Returns the entry, and through
 * `game` what folder_game says of it; NULL when the selection is not a game
 * (the ".." row or an ordinary folder) or the path is too long.
 */
static const MenuEntry *selected_game_path(Menu *menu, char *path, size_t *game)
{
    const MenuEntry *entry;
    const char *discs[ODE_MAX_DISCS];
    if (!row_count(menu) || (menu->listing.has_parent && !menu->selected))
        return NULL;
    entry = browser_entry(menu, menu->selected - (menu->listing.has_parent ? 1 : 0));
    *game = folder_game(menu, entry, discs);
    if ((entry->is_directory && !*game) || directory_path(menu, path, FOLDER_PATH_BYTES) < 0)
        return NULL;
    if (strlen(path) + strlen(entry->name) + (*game ? strlen(discs[0]) + 1 : 0) + 2 >
        ENTRY_PATH_BYTES)
        return NULL;
    strcat(path, "/");
    strcat(path, entry->name);
    if (*game) {
        strcat(path, "/");
        strcat(path, discs[0]);
    }
    return entry;
}

/* The part of a menu path the USB media indexes are keyed by: below
 * "/Storage/", and below the device label when the first folder is a
 * device rather than a folder on the drive. */
static const char *usb_media_path(Menu *menu, const char *path)
{
    const char *lookup = path + STORAGE_PREFIX;
    if (menu->depth && !(menu->path_ids[0] & 0xffffffUL))
        lookup += strlen(menu->path[0]) + 1;
    return lookup;
}

/*
 * Loads the cover `file` (named in the USB index) for the selected `entry`
 * into menu->cover: from the cover cache if it is there, otherwise from the
 * drive, showing "Loading cover..." while it reads. A button press cancels
 * the read. Leaves menu->cover NULL if there is no memory, the read fails or
 * it was canceled; a transport failure stops the browser.
 */
static void load_usb_cover(Menu *menu, const MenuEntry *entry, size_t game, const char *file)
{
    ArtRouteStep route[MAX_DEPTH];
    UsbCoverRead read;
    CCB *cover;
    const char *message;
    unsigned int i;
    int result;
    size_t bytes = (size_t)USB_COVER_WIDTH * USB_COVER_HEIGHT * 2;
    unsigned long started, commands, volume = entry->toc_id & 0xff000000UL;

    /* Covers the cache already holds show at once; others wait until the
     * selection has rested, so scrolling never stops to read one. */
    if (!cover_cache_contains(&menu->cover_cache, volume, file) &&
        now_ms(menu) - menu->selection_since < COVER_IDLE_MS)
        return;
    menu->cover_attempted = 1;

    /* The cel the cover is decoded into. If memory is short, the cache gives
     * way, but then the cover waits for the usual rest like a new one. */
    cover = CreateCel(USB_COVER_WIDTH, USB_COVER_HEIGHT, 16, CREATECEL_UNCODED, NULL);
    if (!cover && menu->cover_cache.clock) {
        cover_cache_clear(&menu->cover_cache);
        cover = CreateCel(USB_COVER_WIDTH, USB_COVER_HEIGHT, 16, CREATECEL_UNCODED, NULL);
        if (cover && now_ms(menu) - menu->selection_since < COVER_IDLE_MS) {
            UnloadCel(cover);
            menu->cover_attempted = 0;
            return;
        }
    }
    if (!cover)
        return;
    menu->cover_file = file;
    menu->cover_folder = game ? entry->name : NULL;

    if (cover_cache_get(&menu->cover_cache, volume, file, (unsigned char *)cover->ccb_SourcePtr,
                        bytes)) {
        menu->cover_cached = 1;
        menu->cover_load_ms = menu->cover_commands = 0;
        menu->cover = cover;
        return;
    }

    /* Read it from the drive, timing the read for the diagnostics. */
    for (i = 0; i < menu->depth; ++i) {
        route[i].id = menu->path_ids[i];
        route[i].name = menu->path[i];
    }
    read.menu = menu;
    read.filename = file;
    read.pixels = (unsigned char *)cover->ccb_SourcePtr;
    message = menu->message;
    pause_scroll(menu);
    menu->message = "Loading cover...";
    draw(menu);
    started = now_ms(menu);
    commands = menu->bus.commands;
    menu->cover_cached = 0;
    menu->media_cancelled = 0;
    menu->ode.media_folder = game ? entry->name : NULL;
    result = media_read(menu, route, menu->usb_art.beside, read_usb_cover, &read);
    menu->ode.media_folder = NULL;
    menu->cover_load_ms = now_ms(menu) - started;
    menu->cover_commands = menu->bus.commands - commands;
    resume_scroll(menu);
    menu->message = message;

    if (result < 0 || menu->media_cancelled) {
        UnloadCel(cover);
        menu->cover_file = menu->cover_folder = NULL;
    } else {
        menu->cover = cover;
        cover_cache_put(&menu->cover_cache, volume, file,
                        (const unsigned char *)cover->ccb_SourcePtr, bytes);
    }
    if (result == -2 || menu->bus.timed_out) {
        menu->ode.navigation_failed = 1;
        menu->ready = menu->view_valid = 0;
        menu->message = "Cover I/O failed. Restart console.";
    }
}

/*
 * Shows the selected game's cover beside the list, if it has one and the
 * browser is idle. Called every main-loop pass; it does its work once per
 * selection (cover_attempted), and only after the selection has rested for
 * COVER_IDLE_MS unless the cover is cached. The demo reads covers from the
 * menu's own disc instead of the drive.
 */
static void update_cover(Menu *menu)
{
    const MenuEntry *entry;
    const char *file;
    char path[ENTRY_PATH_BYTES];
    size_t game;
    if (menu->recovery_media_off || menu->tools || menu->tools_requested || !menu->covers_enabled ||
        menu->review || !menu->ready || menu->loading || menu->cover_attempted ||
        menu->bus.timed_out || menu->ode.navigation_failed)
        return;
    entry = selected_game_path(menu, path, &game);
    if (!entry)
        return;
    if (menu->demo) {
        if (now_ms(menu) - menu->selection_since < COVER_IDLE_MS)
            return;
        menu->cover_attempted = 1;
        file = menu_cover_lookup(path);
        if (file)
            menu->cover = LoadCel((char *)file, MEMTYPE_CEL);
    } else {
        file = usb_media_find(&menu->usb_art, usb_media_path(menu, path));
        if (file)
            load_usb_cover(menu, entry, game, file);
        else
            menu->cover_attempted = 1;
    }
    if (menu->cover && !menu->ode.navigation_failed) {
        /* Beside the list; CCB_BGND draws its black pixels. */
        menu->cover->ccb_XPos = 236 << 16;
        menu->cover->ccb_YPos = 86 << 16;
        menu->cover->ccb_Flags |= CCB_LAST | CCB_BGND;
    }
}

/* ==== Previews ==== */

/* A preview read in progress, passed through media_read to read_usb_preview.
 * The times split the load for the diagnostics: reading the clip, then
 * checking it (phase 1 onward). */
typedef struct PreviewRead {
    Menu *menu;
    const char *file;
    unsigned long started, checked, finished;
    int checking;
} PreviewRead;

/* The loader's progress callback: records when reading ended and checking
 * began (phase 1), and how long each has taken so far. */
static void preview_phase(void *context, int phase)
{
    PreviewRead *read = (PreviewRead *)context;
    unsigned long now = now_ms(read->menu);
    if (phase == 1) {
        read->checked = now;
        read->checking = 1;
        read->menu->preview_read_ms = now - read->started;
    } else if (read->checking)
        read->menu->preview_check_ms = now - read->checked;
    else
        read->menu->preview_read_ms = now - read->started;
}
/* Cache pixels are expendable; release them only when preview allocations
 * need space. This hook never sends an ODE command. */
static int reclaim_preview_covers(void *context)
{
    Menu *menu = (Menu *)context;
    if (!menu->cover_cache.clock)
        return 0;
    cover_cache_clear(&menu->cover_cache);
    return 1;
}
/* Play a streamed clip whose header the loader found: beside the discs only,
 * since the reader reopens the file from the current folder on every loop. */
static int start_stream(Menu *menu, const char *file, const char *folder)
{
    int result;
    if (!menu->usb_previews.beside)
        return -1;
    result = preview_stream_open(&menu->stream, &menu->ode, ode_bus_read_buffer, folder, file,
                                 menu->preview.stream_block, menu->config.preview_sound,
                                 media_cancel, menu, reclaim_preview_covers, menu);
    usb_preview_free(&menu->preview);
    if (result == -5)
        menu->media_cancelled = 1;
    return result == -4 ? USB_PREVIEW_NO_MEMORY : result;
}
/* Loads the clip in a PreviewRead whole (or, for a streamed clip, just its
 * header: then it returns USB_PREVIEW_STREAM). */
static int read_usb_preview(void *context)
{
    PreviewRead *read = (PreviewRead *)context;
    int result;
    read->started = now_ms(read->menu);
    result = usb_preview_load_reclaim(&read->menu->ode, ode_bus_read_buffer, read->file,
                                      &read->menu->preview, media_cancel, read->menu, preview_phase,
                                      read, read->menu->config.preview_sound,
                                      reclaim_preview_covers, read->menu);
    read->finished = now_ms(read->menu);
    if (!read->checking)
        read->menu->preview_read_ms = read->finished - read->started;
    return result;
}
/* Pool 0 is DRAM, 1 is VRAM. Hosts report zero. */
static void memory_available(int pool, unsigned long *free_bytes, unsigned long *largest)
{
    *free_bytes = *largest = 0;
#ifdef OH3DO_SDK
    {
        MemInfo memory;
        AvailMem(&memory, pool ? MEMTYPE_VRAM : MEMTYPE_DRAM);
        *free_bytes = memory.minfo_SysFree + memory.minfo_TaskFree;
        *largest = memory.minfo_SysLargest > memory.minfo_TaskLargest ? memory.minfo_SysLargest
                                                                      : memory.minfo_TaskLargest;
    }
#else
    (void)pool;
#endif
}
/* The selected game's preview clip in the USB index, or NULL. */
static const char *selected_preview(Menu *menu)
{
    static char path[ENTRY_PATH_BYTES];
    size_t game;
    if (!menu->usb_previews.count || !selected_game_path(menu, path, &game))
        return NULL;
    return usb_media_find(&menu->usb_previews, usb_media_path(menu, path));
}
/* X swaps the selected game between its preview and its cover; 0 when it has
 * no preview, so X keeps its other job. */
static int toggle_preview(Menu *menu)
{
    if (menu->review || menu->demo || !menu->ready || !menu->config.video_previews ||
        !selected_preview(menu))
        return 0;
    if (menu->preview_off) {
        /* Play it again now, without the usual settling delay. */
        menu->preview_off = 0;
        menu->preview_attempted = 0;
        menu->selection_since = now_ms(menu) - PREVIEW_IDLE_MS;
    } else {
        stop_preview(menu);
        menu->preview_off = 1;
        menu->preview_attempted = 1;
        menu->tuning = 0;
        menu->clear_frames = 2;
        menu->panel_frames = 0;
    }
    return 1;
}
/*
 * X does two things with media showing: a tap swaps preview and cover
 * (toggle_preview), a 500 ms hold opens the full-screen view. So a media X
 * press is held back until it is released (passed on as a press then) or
 * held long enough (the full-screen view opens and the press is dropped).
 * Other buttons or the browser being busy cancel the wait. The preview keeps
 * playing meanwhile. Returns `pressed`, less any X held back.
 */
static u32 media_x_input(Menu *menu, u32 buttons, u32 pressed, u32 released)
{
    if (menu->tools || menu->review || *menu->filter || !menu->ready ||
        menu->ode.navigation_failed || menu->alphabet_active || (buttons & ~ControlX)) {
        menu->x_pending = 0;
        return pressed;
    }
    if ((pressed & ControlX) && (menu->cover || menu->preview_cel)) {
        menu->x_pending = 1;
        menu->x_since = now_ms(menu);
        pressed &= ~ControlX;
    }
    if (!menu->x_pending)
        return pressed;
    if (buttons & ControlX) {
        if (now_ms(menu) - menu->x_since >= 500UL) {
            menu->x_pending = 0;
            menu->suppress_buttons |= ControlX;
            if (menu->preview_cel) {
                menu->full_view = 2;
                set_smoothing(menu, 1);
            } else {
                stop_preview(menu);
                open_full_cover(menu);
            }
        }
    } else {
        menu->x_pending = 0;
        if (released & ControlX)
            pressed |= ControlX;
    }
    return pressed;
}
/*
 * Starts the selected game's preview, if it has one, once the selection has
 * rested for PREVIEW_IDLE_MS. Like changing channel on the TV: the channel
 * number blinks, static plays while the clip loads, then the clip plays in
 * the TV. A button press at any point cancels and leaves the browser
 * responsive. Called every main-loop pass; it does its work once per
 * selection (preview_attempted). Records timings for the diagnostics, and
 * reports failures in a notice.
 */
static void update_preview(Menu *menu)
{
    const MenuEntry *entry;
    const char *file, *message, *discs[ODE_MAX_DISCS];
    size_t game;
    ArtRouteStep route[MAX_DEPTH];
    PreviewRead read;
    unsigned int i;
    int result;
    unsigned long started, commands, setup_started;
    if (menu->recovery_media_off || menu->full_view || menu->x_pending || menu->cancel_directions ||
        menu->config_result == 2 || strcmp(menu->ode.identity, "LOCODE") ||
        !menu->config.video_previews || menu->preview_attempted || menu->preview_off ||
        menu->demo || menu->tools || menu->tools_requested || menu->review ||
        menu->alphabet_active || !menu->ready || menu->loading || menu->bus.timed_out ||
        menu->ode.navigation_failed || !menu->usb_previews.count ||
        now_ms(menu) - menu->selection_since < PREVIEW_IDLE_MS)
        return;
    menu->preview_attempted = 1;
    file = selected_preview(menu);
    if (!file)
        return;
    entry = browser_entry(menu, menu->selected - (menu->listing.has_parent ? 1 : 0));
    game = folder_game(menu, entry, discs);

    /* Changing channel: the number blinks twice, then the static starts.
     * A press during the blink cancels like one during the read. */
    pause_scroll(menu);
    menu->media_cancelled = 0;
    for (i = 0; i < 16 && !media_cancel(menu); ++i) {
        if (!(i & 3)) {
            menu->osd_hidden = !(i & 4);
            draw(menu);
        }
        WaitVBL(menu->vbl, 1);
    }
    menu->osd_hidden = 0;
    if (menu->media_cancelled) {
        menu->preview_result = -5;
        resume_scroll(menu);
        return;
    }
    /* Playback holds off timed position saves (a USB write stalls the video),
     * so save it now, while loading pauses the menu anyway. */
    if (menu->position_dirty)
        remember_position(menu, 1);
    /* Keep decoded covers if the full clip fits. The loader releases them
     * at allocation boundaries if memory or fragmentation requires it. */
    memory_available(0, &menu->preview_free_bytes, &menu->preview_largest_bytes);
    memory_available(1, &menu->preview_vram_free_bytes, &menu->preview_vram_largest_bytes);

    /* Load it, with static on the TV, and reset the diagnostics. */
    for (i = 0; i < menu->depth; ++i) {
        route[i].id = menu->path_ids[i];
        route[i].name = menu->path[i];
    }
    memset(&read, 0, sizeof(read));
    read.menu = menu;
    read.file = file;
    message = menu->message;
    menu->message = "Loading preview... Press to cancel.";
    menu->tuning = 1;
    menu->tuning_since = now_ms(menu);
    menu->tuning_bytes = menu->tuning_draws = 0;
    menu->tuning_full = 0;
    draw(menu);
    menu->media_cancelled = 0;
    started = now_ms(menu);
    commands = menu->bus.commands;
    menu->preview_read_ms = menu->preview_check_ms = menu->preview_bytes = 0;
    menu->preview_frames = menu->preview_fps = 0;
    menu->preview_chunks = 0;
    menu->preview_source_frames = menu->preview_vram_bytes = 0;
    menu->preview_decodes = menu->preview_decode_ms = menu->preview_decode_max_ms =
        menu->preview_skipped = 0;
    menu->preview_full_view_decodes = menu->preview_full_view_skipped = 0;
    menu->preview_video_draws = menu->preview_video_ms = 0;
    menu->preview_light_draws = menu->preview_full_draws = menu->preview_draw_ms =
        menu->preview_pass_max_ms = menu->preview_pass_at = 0;
    menu->ode.media_folder = game ? entry->name : NULL;
    result = media_read(menu, route, menu->usb_previews.beside, read_usb_preview, &read);
    menu->ode.media_folder = NULL;
    if (result == USB_PREVIEW_STREAM)
        result = start_stream(menu, file, game ? entry->name : "");
    menu->tuning = 0;
    menu->clear_frames = 2;
    menu->message = message;
    setup_started = now_ms(menu);
    menu->preview_navigation_ms = setup_started - started - (read.finished - read.started);
    menu->preview_result = result == USB_PREVIEW_NO_MEMORY ? -4 : (result ? -1 : 1);
    if (menu->media_cancelled && result != -2) {
        usb_preview_free(&menu->preview);
        result = -1;
        menu->preview_result = -5;
    }

    /* Loaded: wrap the first frame in a cel for the TV and start playing. */
    if (!result) {
        /* A stream shows its 160x120 TV picture in the browser. */
        unsigned char *pixels = menu->stream.open ? menu->stream.thumb : menu->preview.frame_buffer;
        unsigned int width = menu->stream.open ? 160 : menu->preview.width,
                     height = menu->stream.open ? 120 : menu->preview.height;
        menu->preview_cel = CreateCel(width, height, 16, CREATECEL_UNCODED, pixels);
        if (!menu->preview_cel && reclaim_preview_covers(menu))
            menu->preview_cel = CreateCel(width, height, 16, CREATECEL_UNCODED, pixels);
        if (menu->preview_cel) {
            /* The loader has already decoded frame 0 into the cel's pixels. */
            menu->preview_cel->ccb_Flags |= CCB_LAST | CCB_BGND;
            menu->preview_frame = 0;
            menu->preview_bytes = menu->preview.video_bytes + menu->preview.audio_bytes;
            menu->preview_frames = menu->preview.frames;
            menu->preview_fps = menu->preview.fps;
            menu->preview_chunks = menu->preview.chunk_count;
            menu->preview_source_frames = menu->preview.source_frames;
            menu->preview_vram_bytes = menu->preview.vram_bytes;
            if (menu->stream.open) {
                unsigned int i;
                menu->preview_frames = menu->preview_source_frames = menu->stream.header.frames;
                menu->preview_fps = OM3DVID7_FPS;
                menu->preview_chunks = menu->stream.slots;
                for (i = 0; i < menu->stream.slots; ++i)
                    if (menu->stream.slot_vram[i])
                        menu->preview_vram_bytes += menu->stream.slot_bytes;
            }
            /* Prepare both list panels with frame zero before starting either
             * timeline. Playback then begins on the light picture-only path. */
            menu->preview_priming = 1;
            menu->panel_frames = 0;
            draw(menu);
            draw(menu);
            menu->preview_priming = 0;
            if (menu->config.preview_sound && menu->preview.audio && !menu->stream.open)
                preview_audio_start(&menu->preview_audio, menu->preview.audio,
                                    menu->preview.audio_frames, menu->preview.audio_rate);
            menu->stream_shown = menu->stream.frame;
            menu->preview_since = now_ms(menu);
            preview_stream_play(&menu->stream, menu->preview_since);
        } else {
            preview_stream_close(&menu->stream);
            usb_preview_free(&menu->preview);
            menu->preview_result = -4;
        }
    }

    menu->preview_setup_ms = now_ms(menu) - setup_started;
    menu->preview_load_ms = now_ms(menu) - started;
    menu->preview_commands = menu->bus.commands - commands;
    if (menu->preview_result == -4)
        notify(menu, "Preview needs more free memory.");
    else if (menu->preview_result == -1)
        notify(menu, "Preview failed. Check diagnostics.");
    if (result == -2 || menu->bus.timed_out || menu->ode.navigation_failed) {
        stop_preview(menu);
        menu->ode.navigation_failed = 1;
        menu->ready = menu->view_valid = 0;
        menu->message = "Preview I/O failed. Restart console.";
    }
    /* Resume only after both buffers and audio are ready, including failures. */
    resume_scroll(menu);
}

/* ==== Starting up, the demo and moving between folders ==== */

/*
 * Shows a made-up folder of games with covers from the menu's own disc, for
 * trying the menu without an ODE (C when the ODE is not found). Drops the
 * USB indexes; a queued disc must be cleared first.
 */
static void show_demo(Menu *menu)
{
    static const MenuEntry examples[] = {{5, 0, "Road Rash.iso"},
                                         {2, 1, "Multi Disc"},
                                         {9, 0, "Disc 10.iso"},
                                         {3, 1, "Favorites"},
                                         {1, 0, "Alone in the Dark.iso"},
                                         {7, 0, "Disc 2.iso"},
                                         {4, 0, "Disc 1.iso"},
                                         {6, 0, "The Need for Speed.iso"},
                                         {8, 0, "Super Street Fighter II Turbo.iso"}};
    size_t i;
    if (menu->playlist.count) {
        menu->message = "P: Review queue before demo";
        return;
    }
    menu->review = 0;
    menu->art_indexes_valid = 0;
    usb_art_index_free(&menu->usb_art);
    usb_art_index_free(&menu->usb_previews);
    library_metadata_free(&menu->metadata);
    menu->covers_enabled = menu_cover_count != 0;
    free_listing(menu);
    menu->listing.entries = (const MenuEntry **)malloc(9 * sizeof(MenuEntry *));
    if (!menu->listing.entries) {
        menu->message = "Not enough memory";
        return;
    }
    for (i = 0; i < 9; ++i) {
        size_t n = strlen(examples[i].name) + 1;
        MenuEntry *entry = (MenuEntry *)malloc(sizeof(*entry) + n);
        if (!entry) {
            free_listing(menu);
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

/*
 * Starts (or restarts) the menu's connection to the ODE: forgets everything
 * known about the drive, identifies the ODE, reads the settings (skipped
 * while X is held, for a drive whose files trouble the menu), learns the
 * saves, clears the ODE's disc queue, opens the drive's root (loading the
 * media indexes) and returns to the remembered position. Each step draws
 * its progress on the splash screen (boot_step). On failure the browser
 * shows why and offers the demo.
 */
static void connect_ode(Menu *menu)
{
    int config_result;
    ControlPadEventData pad;
    /* Forget the last connection. */
    menu->booting = 1;
    menu->boot_stage = 1;
    startup_menu = menu;
    menu->art_indexes_valid = 0;
    library_catalog_free(&menu->catalog);
    ode_playlist_dispose(&menu->playlist);
    menu->review = 0;
    menu->demo = menu->ready = menu->view_valid = 0;
    menu->selected = menu->depth = 0;
    menu->root_known = 0;
    menu->notice_active = 0;
    selection_changed(menu);
    update_breadcrumb(menu);
    menu->usb_tested = menu->backup_result = menu->restore_result = 0;
    menu->position_result = menu->recent_result = menu->diagnostics_result = 0;
    memset(&menu->last_position, 0, sizeof(menu->last_position));
    menu_favorites_free(&menu->recent);
    menu->recent_valid = 0;
    usb_art_index_free(&menu->usb_previews);
    menu->config_result = 3; /* not attempted */
    memset(menu->ode.identity, 0, sizeof(menu->ode.identity));
    memset(menu->ode.revision, 0, sizeof(menu->ode.revision));
    memset(menu->ode.identity_reply, 0, sizeof(menu->ode.identity_reply));
    menu_categories_reset();
    menu_config_defaults(&menu->config);
    usb_art_index_free(&menu->usb_art);
    cover_cache_clear(&menu->cover_cache);
    menu->covers_enabled = 0;
    menu->skip_usb_art = 0;
    menu->ode.navigation_known = 0;
    menu->ode.navigation_failed = 0;
    menu->ode.file_path[0] = 0;
    free_listing(menu);

    menu->message = "Connecting to ODE...";
    draw(menu);
    if (menu->bus.request < 0 && ode_bus_open(&menu->bus) < 0) {
        menu->message = "Cannot open XBUS. C: View demo";
        goto done;
    }
    if (ode_identify(&menu->ode) < 0 || ode_change_directory(&menu->ode, 0) < 0) {
        menu->message =
            menu->bus.timed_out ? "No ODE reply. C: View demo" : "ODE not available. C: View demo";
        goto done;
    }
    boot_step(menu, BOOT_IDENTIFY);

    /* The settings, and the copies the Settings screen edits. */
    menu->message = "Reading USB config...";
    menu->boot_stage = 2;
    draw(menu);
    if (GetControlPad(1, 0, &pad) >= 0 && (pad.cped_ButtonBits & ControlX))
        config_result = 2; /* Recovery path: do not send any file commands. */
    else
        config_result = menu_config_load(&menu->ode, ode_bus_read_buffer, &menu->config);
    menu->config_result = config_result;
    menu_set_hide_adult(menu->config.hide_adult);
    menu->skip_usb_art = config_result != 0 || !menu->config.usb_covers;
    menu->settings_folder_workaround = menu->config.folder_workaround;
    menu->settings_usb_covers = menu->config.usb_covers;
    menu->settings_remember_position = menu->config.remember_position;
    menu->settings_recent_games = menu->config.recent_games;
    menu->settings_video_previews = menu->config.video_previews;
    menu->saved_config = menu->config;
    boot_step(menu, BOOT_CONFIG);
    if (menu->bus.timed_out) {
        menu->message = "Config I/O failed. Restart console.";
        goto done;
    }

    /* The saves (and below, the remembered position) are read only if the
     * settings were, so a drive the menu has trouble with gets fewer file
     * commands. */
    if (config_result == 0)
        learn_saves(menu);
    boot_step(menu, BOOT_SAVES);
    if (ode_playlist_clear(&menu->ode, &menu->playlist) < 0) {
        menu->message = menu->ode.error;
        goto done;
    }
    boot_step(menu, BOOT_QUEUE);
    menu->boot_stage = 3;
    load_directory(menu, 0);
    menu->boot_stage = 4;
    if (menu->ready && config_result == 0 && menu->config.remember_position)
        load_position(menu);
    boot_step(menu, BOOT_POSITION);
    if (menu->ready && config_result < 0)
        menu->message = "Config not loaded. Defaults used.";
    else if (menu->ready && config_result == 1)
        menu->message = "USB config unsupported. Defaults used.";
    else if (menu->ready && config_result == 2)
        menu->message = "Config skipped. Defaults used.";
done:
    menu->booting = 0;
    startup_menu = NULL;
    if (menu->splash) {
        UnloadCel(menu->splash);
        menu->splash = NULL;
    }
    draw(menu);
    boot_step(menu, BOOT_READY);
}

/* Goes up to the parent folder (B, or the ".." row), reselecting the
 * folder we came out of. */
static void go_back(Menu *menu)
{
    unsigned long parent_id;
    size_t selected = 0;
    if (!menu->ready || (!menu->listing.has_parent && !menu->depth) || menu->demo)
        return;
    parent_id = menu->listing.has_parent ? menu->listing.parent_id : 0xffffffffUL;
    if (ode_change_directory(&menu->ode, parent_id) < 0) {
        menu->ready = 0;
        menu->message = "Folder I/O failed. Restart console.";
        return;
    }
    if (menu->depth)
        selected = menu->previous_selection[--menu->depth];
    load_directory(menu, selected);
}

/*
 * A on the selected row: goes up for "..", plays a folder acting as a game
 * (queuing its whole disc set), enters any other folder, or plays a disc
 * (with the rest of its set, if it is one). Starting a game records it in
 * the recent list; the ODE then mounts it and the console reboots into it.
 * menu->single_disc (hold C) plays just the selected disc, or enters a
 * game's folder.
 */
static void activate(Menu *menu)
{
    const MenuEntry *entry;
    if (!menu->ready || menu->demo || !row_count(menu))
        return;
    if (menu->listing.has_parent && menu->selected == 0) {
        go_back(menu);
        return;
    }
    entry = browser_entry(menu, menu->selected - (menu->listing.has_parent ? 1 : 0));
    if (entry->is_directory) {
        /* Queue the game's discs from the browser location. Queue resolution
         * restores that location before launch history is saved, so neither
         * the browser nor the remembered position enters the game folder.
         * Hold C to explicitly browse inside instead. */
        const char *discs[ODE_MAX_DISCS];
        size_t game = menu->single_disc ? 0 : folder_game(menu, entry, discs);
        menu->single_disc = 0;
        if (game) {
            if (menu->playlist.count || !menu->playlist.synchronized) {
                menu->message = "P: Review or clear disc queue";
                return;
            }
            queue_selected_disc(menu);
            if (menu->playlist.count == game)
                start_queue(menu);
            else {
                ode_playlist_dispose(&menu->playlist);
                menu->playlist.synchronized = 1;
            }
            return;
        }

        /* An ordinary folder: enter it, remembering where its row was so
         * going back reselects it. */
        if (menu->depth == MAX_DEPTH) {
            menu->message = "Maximum folder depth reached";
            return;
        }
        if (ode_enter_directory(&menu->ode, &menu->listing, entry, folder_workaround(menu)) < 0) {
            if (menu->ode.navigation_failed) {
                menu->ready = 0;
                menu->message = "Folder I/O failed. Restart console.";
            } else
                menu->message = menu->ode.error;
            return;
        }
        menu->previous_selection[menu->depth] = menu->listing.has_parent ? 1 : 0;
        {
            size_t i;
            for (i = 0; i < menu->listing.count && menu->listing.entries[i] != entry; ++i)
                if (!hidden_entry(menu, menu->listing.entries[i]))
                    ++menu->previous_selection[menu->depth];
        }
        menu->path_ids[menu->depth] = entry->toc_id;
        strcpy(menu->path[menu->depth], entry->name);
        ++menu->depth;
        load_directory(menu, 0);
    } else {
        /* A disc: one of a set queues the set from here on (queue_set). */
        if (!menu->single_disc && !menu->playlist.count && menu->playlist.synchronized &&
            queue_set(menu, entry)) {
            start_queue(menu);
            return;
        }
        menu->single_disc = 0;
        if (menu->playlist.count || !menu->playlist.synchronized) {
            menu->message = "P: Review or clear disc queue";
            return;
        }
        {
            /* activate also calls the disc-group helper, which owns large
             * path buffers. Keep this buffer off the 32 KiB task stack. */
            char *full = (char *)malloc(ENTRY_PATH_BYTES);
            if (full) {
                if (!directory_path(menu, full, ENTRY_PATH_BYTES) &&
                    strlen(full) + strlen(entry->name) + 2 <= ENTRY_PATH_BYTES) {
                    strcat(full, "/");
                    strcat(full, entry->name);
                    record_launch(menu, full);
                }
                free(full);
            }
            if (!menu->ready || menu->ode.navigation_failed || menu->bus.timed_out)
                return;
        }

        /* Launch: past this point the ODE's queue is no longer what the
         * menu believes it is. */
        menu->message = "Starting game...";
        draw(menu);
        menu->playlist.synchronized = 0;
        stop_preview(menu);
        if (ode_launch(&menu->ode, entry) < 0)
            menu->message = menu->ode.error;
        else {
            menu->launching = 1;
            menu->ready = menu->view_valid = 0;
            menu->message = "Image mounted. Waiting for boot...";
            free_listing(menu);
        }
    }
}

/* Adds the selected disc to the menu's queue (Y), or for a folder acting
 * as its game, all of its discs. P reviews the queue. */
static void queue_selected_disc(Menu *menu)
{
    char directory[FOLDER_PATH_BYTES], full[ENTRY_PATH_BYTES];
    const MenuEntry *entry;
    const char *discs[ODE_MAX_DISCS];
    size_t game, i;
    if (!menu->ready || !row_count(menu))
        return;
    if (menu->listing.has_parent && menu->selected == 0) {
        menu->message = "Select a disc image to queue";
        return;
    }
    entry = browser_entry(menu, menu->selected - (menu->listing.has_parent ? 1 : 0));
    if (directory_path(menu, directory, sizeof(directory)) < 0) {
        menu->message = "Folder path too long for disc queue";
        return;
    }
    game = folder_game(menu, entry, discs);
    if (entry->is_directory && !game) {
        menu->message = "Select a disc image to queue";
        return;
    }
    /* A folder acting as its game queues its disc, or its whole disc set. */
    for (i = 0; i < (game ? game : 1); ++i) {
        if (strlen(directory) + strlen(entry->name) + (game ? strlen(discs[i]) + 1 : 0) + 2 >
            sizeof(full)) {
            menu->message = "Folder path too long for disc queue";
            return;
        }
        strcpy(full, directory);
        strcat(full, "/");
        strcat(full, entry->name);
        if (game) {
            strcat(full, "/");
            strcat(full, discs[i]);
        }
        if (ode_playlist_append_local(&menu->ode, &menu->playlist, full) < 0) {
            menu->message = menu->ode.error;
            return;
        }
    }
    format(menu->status, "Queued disc %lu. P: Review", (unsigned long)menu->playlist.count);
    notify(menu, menu->status);
}

/* Opens the queue review (P), where discs are reordered or removed. */
static void review_queue(Menu *menu)
{
    menu->review = 1;
    menu->queue_selected = 0;
    menu->message = menu->playlist.synchronized ? NULL : "Queue uncertain. X:Clear to retry";
    menu->notice_active = 0;
    selection_changed(menu);
}

/* Plays the queue: sends it to the ODE if it changed, records the first
 * disc in the recent list, and starts the first disc. */
static void start_queue(Menu *menu)
{
    if (menu->playlist.dirty && apply_queue(menu) < 0) {
        menu->message = menu->ode.error;
        return;
    }
    if (menu->playlist.count)
        record_launch(menu, menu->playlist.paths[0]);
    if (!menu->ready || menu->ode.navigation_failed || menu->bus.timed_out)
        return;
    if (menu->playlist.count && menu->playlist.synchronized)
        stop_preview(menu);
    menu->message = "Starting first queued disc...";
    draw(menu);
    if (ode_playlist_start(&menu->ode, &menu->playlist) < 0) {
        menu->message = menu->ode.error;
        return;
    }
    menu->review = menu->ready = menu->view_valid = 0;
    menu->launching = 1;
    menu->message = "Image mounted. Waiting for boot...";
    free_listing(menu);
}

/* ==== Applying the disc queue ==== */

/* The current folder as a route of ids and names, for library_visit to
 * walk back to. */
static void current_route(Menu *menu, ArtRouteStep *route)
{
    unsigned int i;
    for (i = 0; i < menu->depth; ++i) {
        route[i].id = menu->path_ids[i];
        route[i].name = menu->path[i];
    }
}
/* library_visit's action for a queued disc: adds it to the ODE's queue. */
static int queue_file_action(void *context, const MenuEntry *entry, const char *directory)
{
    Menu *menu = (Menu *)context;
    return ode_playlist_add(&menu->ode, &menu->group, entry, directory);
}
/*
 * Sends the menu's queue to the ODE, replacing the ODE's own. The ODE
 * queues by directory entry, so each queued path is found first: from the
 * current folder's listing when every disc is in it (the usual case: one
 * re-read, no trips to the root), otherwise by walking to each one
 * (library_visit). Nothing is sent until every path is found. Returns 0, or
 * -1 with menu->ode.error saying why; if it failed partway through sending,
 * the queue is marked unsynchronized, as the ODE's queue is then unknown.
 */
static int apply_queue(Menu *menu)
{
    ArtRouteStep route[MAX_DEPTH];
    const MenuEntry *entries[ODE_MAX_DISCS];
    OdeListing fresh;
    char *directory;
    size_t i;
    int local = 0, result = -1;
    current_route(menu, route);
    ode_listing_init(&fresh);
    if (menu->bus.timed_out || menu->ode.navigation_failed || !menu->playlist.synchronized)
        return -1;
    directory = (char *)malloc(FOLDER_PATH_BYTES);
    if (!directory) {
        menu->ode.error = "Not enough queue memory";
        return -1;
    }

    /* Check every queued disc is still there. */
    menu->message = "Checking queued game paths...";
    draw(menu);
    if (!directory_path(menu, directory, FOLDER_PATH_BYTES))
        local = library_current_files(&menu->listing, directory, menu->playlist.paths,
                                      menu->playlist.count, entries);
    if (local < 0) {
        menu->ode.error = "Queued path is missing";
        goto checked;
    }
    if (local) {
        /* Re-read once for fresh TOC IDs. Same-folder sets need no root trips. */
        if (ode_read_directory(&menu->ode, &fresh) < 0)
            goto checked;
        if (library_current_files(&fresh, directory, menu->playlist.paths, menu->playlist.count,
                                  entries) != 1) {
            menu->ode.error = "Queued path is missing";
            goto checked;
        }
    } else {
        for (i = 0; i < menu->playlist.count; ++i)
            if (library_visit(&menu->ode, route, menu->depth, &menu->listing,
                              folder_workaround(menu), menu->playlist.paths[i], NULL, NULL) < 0)
                goto checked;
    }

    /* Replace the ODE's queue. */
    ode_playlist_dispose(&menu->group);
    menu->message = "Applying disc queue...";
    draw(menu);
    if (ode_playlist_clear(&menu->ode, &menu->group) < 0)
        goto uncertain;
    for (i = 0; i < menu->playlist.count; ++i) {
        if (local) {
            if (ode_playlist_add(&menu->ode, &menu->group, entries[i], directory) < 0)
                goto uncertain;
        } else if (library_visit(&menu->ode, route, menu->depth, &menu->listing,
                                 folder_workaround(menu), menu->playlist.paths[i],
                                 queue_file_action, menu) < 0)
            goto uncertain;
    }
    menu->playlist.dirty = 0;
    result = 0;
    goto applied;
uncertain:
    menu->playlist.synchronized = 0;
applied:
    ode_playlist_dispose(&menu->group);
checked:
    ode_listing_free(&fresh);
    free(directory);
    return result;
}
/* A disc from a recognized same-folder set is queued with the discs after it;
 * the firmware mounts the next one each time the game asks for a disc. The
 * caller launches the queue, after this frame's large buffers are released. */
static int queue_set(Menu *menu, const MenuEntry *entry)
{
    const MenuEntry *entries[ODE_MAX_DISCS];
    char directory[FOLDER_PATH_BYTES], path[ENTRY_PATH_BYTES];
    size_t position,
        count = menu_disc_group(&menu->listing, entry, entries, ODE_MAX_DISCS, &position), i;
    if (count < position + 2 || directory_path(menu, directory, sizeof(directory)) < 0)
        return 0;
    for (i = position; i < count; ++i) {
        strcpy(path, directory);
        strcat(path, "/");
        strcat(path, entries[i]->name);
        if (ode_playlist_append_local(&menu->ode, &menu->playlist, path) < 0) {
            ode_playlist_dispose(&menu->playlist);
            menu->playlist.synchronized = 1;
            return 0;
        }
    }
    return 1;
}
/* ==== Letter jumps and the C button ==== */

/* Selects the first row starting with `letter` (0 for digits and symbols,
 * 1-26 for A-Z, as menu_initial counts). */
static void jump_letter(Menu *menu, int letter)
{
    size_t i, count = row_count(menu), parent = menu->listing.has_parent ? 1 : 0;
    if (menu->review || !menu->ready)
        return;
    for (i = parent; i < count; ++i) {
        const MenuEntry *entry = browser_entry(menu, i - parent);
        if (menu_initial(entry->name) == letter) {
            menu->selected = i;
            selection_changed(menu);
            menu->message = NULL;
            return;
        }
    }
    menu->message = "No entries for that letter.";
}
/* Moves the focus to the alphabet strip, starting at the selected row's
 * letter. */
static void focus_alphabet(Menu *menu)
{
    const MenuEntry *entry = NULL;
    if (!(menu->listing.has_parent && !menu->selected))
        entry = browser_entry(menu, menu->selected - (menu->listing.has_parent ? 1 : 0));
    menu->jump_letter = entry ? menu_initial(entry->name) : 1;
    pause_scroll(menu); /* Letter navigation keeps the background names still. */
    menu->alphabet_active = 1;
    menu->c_pending = 0;
    menu->notice_active = 0;
    menu->message = NULL;
}

/*
 * The alphabet strip under the list. Up from the top row or down from the
 * bottom row focuses it; there, left and right pick a letter, A jumps to
 * it, and B or down returns to the list. Returns 1 if it used the press.
 */
static int alphabet_input(Menu *menu, u32 pressed)
{
    size_t count;
    if (menu->review || menu->tools || !menu->ready || menu->loading ||
        menu->ode.navigation_failed) {
        if (menu->alphabet_active)
            resume_scroll(menu);
        menu->alphabet_active = 0;
        return 0;
    }
    count = row_count(menu);
    if (!menu->alphabet_active) {
        if (!count || !(((pressed & ControlUp) && !menu->selected) ||
                        ((pressed & ControlDown) && menu->selected + 1 == count)))
            return 0;
        focus_alphabet(menu);
        return 1;
    }
    menu->c_pending = 0;
    if (pressed & ControlLeft)
        menu->jump_letter = (menu->jump_letter + 26) % 27;
    else if (pressed & ControlRight)
        menu->jump_letter = (menu->jump_letter + 1) % 27;
    else if (pressed & ControlB) {
        menu->alphabet_active = 0;
        resume_scroll(menu);
    } else if (pressed & ControlDown) {
        menu->selected = 0;
        selection_changed(menu);
    } else if (pressed & ControlA) {
        unsigned char available[27];
        alphabet_available(menu, available);
        if (available[menu->jump_letter]) {
            menu->alphabet_active = 0;
            resume_scroll(menu);
            jump_letter(menu, menu->jump_letter);
        } else
            notify(menu, "No entries for that letter.");
    }
    return 1;
}

/*
 * The browser's C chords: C+Start shows the game's details, C+Up focuses
 * the alphabet, C+Left/Right jump to the previous or next letter that has
 * entries. A plain C tap queues the selected disc, so a C press is held
 * back until it is released; using it in a chord drops it. Returns
 * `pressed` less the buttons used here.
 */
static u32 browser_shortcuts(Menu *menu, u32 buttons, u32 pressed, u32 released)
{
    if (menu->alphabet_active) {
        menu->c_pending = 0;
        return 0;
    }
    if (menu->review || menu->demo || !menu->ready) {
        menu->c_pending = 0;
        return pressed;
    }
    if (pressed & (ControlA | ControlB | ControlX | ControlStart))
        menu->c_pending = 0;
    if (pressed & ControlC) {
        menu->c_pending = 1;
        pressed &= ~ControlC;
    }
    if ((buttons & ControlC) && (pressed & ControlStart)) {
        menu->c_pending = 0;
        open_game_details(menu);
        return 0;
    }
    if ((buttons & ControlC) && (pressed & ControlUp)) {
        focus_alphabet(menu);
        return 0;
    }
    if ((buttons & ControlC) && (pressed & (ControlLeft | ControlRight))) {
        const MenuEntry *entry = NULL;
        int letter = 0, direction = (pressed & ControlRight) ? 1 : -1, attempt;
        menu->c_pending = 0;
        if (row_count(menu) > (menu->listing.has_parent ? 1U : 0U) &&
            !(menu->listing.has_parent && !menu->selected))
            entry = browser_entry(menu, menu->selected - (menu->listing.has_parent ? 1 : 0));
        if (entry)
            letter = menu_initial(entry->name);
        for (attempt = 0; attempt < 27; ++attempt) {
            size_t before = menu->selected;
            letter = (letter + direction + 27) % 27;
            jump_letter(menu, letter);
            if (menu->selected != before)
                break;
        }
        pressed &= ~(ControlLeft | ControlRight);
    }
    if ((released & ControlC) && menu->c_pending) {
        menu->c_pending = 0;
        queue_selected_disc(menu);
    }
    return pressed;
}
/* ==== Text files: at the drive's root, beside a disc ==== */

/* A text file read or write, passed through art_at_root or media_read to
 * text_at_root_callback. `write` set: write it; otherwise read up to
 * `capacity` bytes into `read`, setting `length`. */
typedef struct TextOperation {
    Menu *menu;
    const char *name;
    const char *write;
    char *read;
    size_t capacity;
    size_t length;
} TextOperation;
/* Does the TextOperation, in whatever folder the caller moved to. */
static int text_at_root_callback(void *context)
{
    TextOperation *op = (TextOperation *)context;
    if (op->write)
        return usb_file_text_write(&op->menu->ode, ode_bus_read_buffer, op->name, op->write);
    return usb_file_text(&op->menu->ode, ode_bus_read_buffer, op->name, op->read, op->capacity,
                         &op->length);
}
/*
 * Reads or writes the text file `name` in .ohmy3do at the drive's root (the
 * settings, position, recent and favorites files), going there and back
 * from the current folder. To write, pass the text as `write`; to read,
 * pass a buffer as `read` and `capacity`, and the length read comes back in
 * `length`. Returns 0, or -1 if it failed or the ODE can't do it (the demo,
 * the X-held recovery start, a different ODE, or the device list, where
 * there is no single root). A transport failure stops the browser.
 */
static int text_at_root(Menu *menu, const char *name, const char *write, char *read,
                        size_t capacity, size_t *length)
{
    TextOperation op;
    ArtRouteStep route[MAX_DEPTH];
    int result;
    size_t i;
    if (menu->demo || !menu->ready || menu->bus.timed_out || menu->ode.navigation_failed ||
        menu->config_result == 2 || strcmp(menu->ode.identity, "LOCODE"))
        return -1;
    if (!menu->depth)
        for (i = 0; i < menu->listing.count; ++i)
            if (!(menu->listing.entries[i]->toc_id & 0xffffffUL))
                return -1;
    current_route(menu, route);
    op.menu = menu;
    op.name = name;
    op.write = write;
    op.read = read;
    op.capacity = capacity;
    op.length = 0;
    result = art_at_root(&menu->ode, route, menu->depth, &menu->listing, folder_workaround(menu),
                         text_at_root_callback, &op);
    if (result == -2 || menu->bus.timed_out || menu->ode.navigation_failed) {
        menu->ready = menu->view_valid = 0;
        menu->message = "USB I/O failed. Restart console.";
        result = -1;
    }
    if (length)
        *length = op.length;
    return result;
}
/* Reads the text file `name` from the .ohmy3do folder beside the entry's
 * disc (inside the entry when it is a folder acting as its game), without a
 * trip to the root. Otherwise as text_at_root. */
static int text_beside(Menu *menu, const MenuEntry *entry, const char *name, char *read,
                       size_t capacity, size_t *length)
{
    TextOperation op;
    ArtRouteStep route[MAX_DEPTH];
    int result;
    if (menu->demo || !menu->ready || menu->bus.timed_out || menu->ode.navigation_failed ||
        menu->config_result == 2 || strcmp(menu->ode.identity, "LOCODE"))
        return -1;
    current_route(menu, route);
    op.menu = menu;
    op.name = name;
    op.write = NULL;
    op.read = read;
    op.capacity = capacity;
    op.length = 0;
    menu->ode.media_folder = entry->is_directory ? entry->name : NULL;
    result = media_read(menu, route, 1, text_at_root_callback, &op);
    menu->ode.media_folder = NULL;
    if (result == -2 || menu->bus.timed_out || menu->ode.navigation_failed) {
        menu->ready = menu->view_valid = 0;
        menu->message = "USB I/O failed. Restart console.";
        result = -1;
    }
    if (length)
        *length = op.length;
    return result;
}
/* ==== The remembered position ==== */

/* Makes `location` (found by library_locate) the browser's folder and
 * selection, taking over its listing. */
static void adopt_location(Menu *menu, LibraryLocation *location)
{
    size_t i;
    menu->filter[0] = 0;
    free_listing(menu);
    menu->listing = location->listing;
    menu->depth = location->depth;
    menu->selected = location->selected;
    for (i = 0; i < menu->depth; ++i) {
        strcpy(menu->path[i], location->names[i]);
        menu->path_ids[i] = location->ids[i];
        menu->previous_selection[i] = location->previous[i];
    }
    menu->tools = menu->review = 0;
    menu->message = NULL;
    menu->ready = menu->view_valid = 1;
    update_breadcrumb(menu);
    selection_changed(menu);
}
/*
 * At startup, returns to the folder and game the menu was last left on
 * (last-position.txt), if that folder still exists and is not hidden.
 * Called at the drive's root, before anything else moves away from it.
 */
static void load_position(Menu *menu)
{
    char *data = (char *)malloc(POSITION_BYTES);
    LibraryLocation *location;
    size_t length, i;
    int result = -1;
    if (!data)
        return;
    /* Same boot filesystem context used by the INI loader, before leaving root. */
    if (!usb_file_text(&menu->ode, ode_bus_read_buffer, "last-position.txt", data, POSITION_BYTES,
                       &length))
        result = menu_position_parse(&menu->last_position, data, length);
    free(data);
    if (result < 0 || menu->bus.timed_out || menu->ode.navigation_failed) {
        menu->position_result = -1;
        if (menu->bus.timed_out || menu->ode.navigation_failed)
            menu->ready = menu->view_valid = 0;
        return;
    }
    if (!*menu->last_position.folder || menu_categories_hidden_path(menu->last_position.folder))
        return;
    location = (LibraryLocation *)malloc(sizeof(*location));
    if (!location)
        return;
    result = library_open_directory(&menu->ode, NULL, 0, &menu->listing, folder_workaround(menu),
                                    menu->last_position.folder, location);
    if (!result) {
        adopt_location(menu, location);
        for (i = 0; i < row_count(menu); ++i) {
            const MenuEntry *entry;
            if (menu->listing.has_parent && !i)
                continue;
            entry = browser_entry(menu, i - (menu->listing.has_parent ? 1 : 0));
            if (entry && !menu_path_compare(entry->name, menu->last_position.selected)) {
                menu->selected = i;
                break;
            }
        }
    } else if (result == -2)
        menu->ready = menu->view_valid = 0;
    /* A renamed/missing folder simply leaves the boot listing in place. */
    free(location);
}

/*
 * Saves the browser's folder and selected entry to last-position.txt, if
 * they changed since the last save. Unless `force`, waits until the
 * selection has rested for two seconds with no tool, queue review or
 * preview showing (a USB write stalls video). Not saved in the device list
 * or when the settings weren't read; a failed write is reported once and
 * stops further attempts.
 */
static void remember_position(Menu *menu, int force)
{
    MenuPosition *position;
    char *data;
    const MenuEntry *entry = NULL;
    if (menu->recovery_media_off || !menu->config.remember_position || menu->config_result != 0 ||
        menu->position_result < 0 || menu->demo || !menu->ready || menu->loading ||
        (!force && (!menu->position_dirty || menu->tools || menu->review || menu->preview_cel ||
                    now_ms(menu) - menu->selection_since < 2000UL)))
        return;
    if (!menu->depth) {
        size_t i;
        for (i = 0; i < menu->listing.count; ++i)
            if (!(menu->listing.entries[i]->toc_id & 0xffffffUL))
                return;
    }
    menu->position_dirty = 0;
    position = (MenuPosition *)malloc(sizeof(*position));
    data = (char *)malloc(POSITION_BYTES);
    if (!position || !data) {
        free(position);
        free(data);
        return;
    }
    memset(position, 0, sizeof(*position));
    if (row_count(menu) && !(menu->listing.has_parent && !menu->selected))
        entry = browser_entry(menu, menu->selected - (menu->listing.has_parent ? 1 : 0));
    if (entry)
        strcpy(position->selected, entry->name);
    if (!directory_path(menu, position->folder, sizeof(position->folder)) &&
        (strcmp(position->folder, menu->last_position.folder) ||
         strcmp(position->selected, menu->last_position.selected))) {
        if (menu_position_format(position, data, POSITION_BYTES) < 0 ||
            text_at_root(menu, "last-position.txt", data, NULL, 0, NULL) < 0) {
            menu->position_result = -1;
            notify(menu, "Position not saved. See diagnostics.");
        } else {
            menu->last_position = *position;
            menu->position_result = 1;
        }
    }
    free(position);
    free(data);
}
/* ==== Launch history and diagnostics ==== */

/* What record_launch writes at the root, through art_at_root. */
typedef struct RecentLaunch {
    Menu *menu;
    const char *path;
    const char *snapshot;
    int recent;
} RecentLaunch;
/* At the root: writes the save snapshot, then puts the launched game at
 * the top of recent.txt. */
static int record_recent_at_root(void *context)
{
    RecentLaunch *op = (RecentLaunch *)context;
    Menu *menu = op->menu;
    char *data;
    size_t length;
    int result = -1;
    /* A lost snapshot only means this launch's saves stay unnamed. */
    if (op->snapshot)
        usb_file_text_write(&menu->ode, ode_bus_read_buffer, "save-snapshot.txt", op->snapshot);
    if (!op->recent)
        return 0;
    data = (char *)malloc(FAVORITES_BYTES);
    if (!data)
        return -1;
    if (!menu->recent_valid) {
        if (usb_file_text(&menu->ode, ode_bus_read_buffer, "recent.txt", data, FAVORITES_BYTES,
                          &length) < 0 ||
            menu_favorites_parse(&menu->recent, data, length) < 0 || menu->recent.count > 10)
            goto done;
        menu->recent_valid = 1;
    }
    if (menu->recent.count && !menu_path_compare(menu->recent.paths[0], op->path)) {
        result = 0;
        goto done;
    }
    if (menu_recent_push(&menu->recent, op->path) < 0 ||
        menu_favorites_format(&menu->recent, data, FAVORITES_BYTES) < 0)
        goto done;
    result = usb_file_text_write(&menu->ode, ode_bus_read_buffer, "recent.txt", data);
done:
    if (result)
        menu->recent_valid = 0;
    free(data);
    return result;
}
/*
 * Just before a game starts: saves the position, and at the root, the
 * launched disc's menu path at the top of the recent list (unless it is
 * already there) and a snapshot of NVRAM (save-snapshot.txt) labeled with
 * the game's title. The next startup compares NVRAM with the snapshot to
 * learn which saves the game wrote, and names them after it (learn_saves).
 */
static void record_launch(Menu *menu, const char *path)
{
    ArtRouteStep route[MAX_DEPTH];
    RecentLaunch op;
    SaveArchive *nvram = (SaveArchive *)malloc(sizeof(SaveArchive));
    NvramInfo info;
    char *snapshot = NULL;
    const char *base;
    int result;
    menu->message = "Saving launch history...";
    draw(menu);
    remember_position(menu, 1);
    if (menu->config_result != 0 || !menu->ready || menu->demo || menu->bus.timed_out ||
        menu->ode.navigation_failed || strncmp(path, "/Storage/", 9)) {
        free(nvram);
        return;
    }
    op.recent = menu->config.recent_games &&
                !(menu->recent_valid && menu->recent.count &&
                  !menu_path_compare(menu->recent.paths[0], path + STORAGE_PREFIX));
    /* NVRAM as the game finds it, so the next startup can tell which saves it wrote. */
    base = strrchr(path, '/') + 1;
    base = display_game_title(base, 0);
    if (nvram) {
        memset(nvram, 0, sizeof(*nvram));
        if (nvram_snapshot(nvram, &info) == 0 &&
            (snapshot = (char *)malloc(SAVE_SNAPSHOT_BYTES)) != NULL) {
            size_t n = strlen(base);
            if (n >= SAVE_TITLE_BYTES)
                n = SAVE_TITLE_BYTES - 1;
            if (save_snapshot_format(snapshot, SAVE_SNAPSHOT_BYTES, base, n, nvram) < 0) {
                free(snapshot);
                snapshot = NULL;
            }
        }
        save_archive_free(nvram);
        free(nvram);
    }
    op.snapshot = snapshot;
    if (!op.recent && !snapshot)
        return;
    current_route(menu, route);
    op.menu = menu;
    op.path = path + STORAGE_PREFIX;
    result = art_at_root(&menu->ode, route, menu->depth, &menu->listing, folder_workaround(menu),
                         record_recent_at_root, &op);
    free(snapshot);
    if (op.recent)
        menu->recent_result = result ? -1 : 1;
    if (result == -2 || menu->bus.timed_out || menu->ode.navigation_failed) {
        menu->ready = menu->view_valid = 0;
        menu->message = "History I/O failed. Restart console.";
    }
}
/* Opens the Recent screen: reads recent.txt, leaving out games in hidden
 * categories (and adult games when they are hidden). */
static void open_recent(Menu *menu)
{
    char *data = (char *)malloc(FAVORITES_BYTES);
    size_t length;
    menu->recent_valid = 0;
    menu_favorites_free(&menu->recent);
    if (data && !text_at_root(menu, "recent.txt", NULL, data, FAVORITES_BYTES, &length) &&
        !menu_favorites_parse(&menu->recent, data, length) && menu->recent.count <= 10) {
        menu->recent_valid = 1;
        {
            size_t i, kept = 0;
            for (i = 0; i < menu->recent.count; ++i)
                if (!menu_categories_hidden_path(menu->recent.paths[i]) &&
                    (!menu->config.hide_adult || !menu_adult_path(menu->recent.paths[i])))
                    menu->recent.paths[kept++] = menu->recent.paths[i];
            /* The file still lists them: re-read it before the next launch rewrites it. */
            if (kept != menu->recent.count) {
                menu->recent.count = kept;
                menu->recent_valid = 0;
            }
        }
        menu->tool_message = menu->recent.count ? NULL : "No recent launch selections.";
    } else
        menu->tool_message = "Recent list unavailable or invalid.";
    free(data);
    menu->tool_selected = 0;
}
/*
 * Writes diagnostics.txt at the drive's root: the ODE's identity, the last
 * command and reply, the settings, and timings and results from startup,
 * covers, previews and streaming, for bug reports. The values are taken
 * before the write, which changes some of them.
 */
static void export_diagnostics(Menu *menu)
{
    OdeBus snapshot = menu->bus;
    char data[4096];
    unsigned long free_bytes, largest;
    size_t i;
    strcpy(data, "Oh My 3DO diagnostics\n");
    append_format(data, "menu=%s\nidentity=%.6s\nrevision=%u.%u.%u\n", MENU_VERSION,
                  menu->ode.identity, menu->ode.revision[0], menu->ode.revision[1],
                  menu->ode.revision[2]);
    strcat(data, "identity_reply=");
    for (i = 0; i < 12; ++i)
        append_format(data, "%02x ", menu->ode.identity_reply[i]);
    append_format(data, "\ncommands=%lu\nlast_command=%02x\nreply_length=%lu\nlast_reply=",
                  snapshot.commands, snapshot.last_command, (unsigned long)snapshot.last_length);
    for (i = 0; i < snapshot.last_length && i < 32; ++i)
        append_format(data, "%02x ", snapshot.last_reply[i]);
    append_format(
        data,
        "\nlast_error=%08lx\nlast_fault_command=%02x\nlast_fault_status=%02x\nfault_error=%08lx\n",
        (unsigned long)snapshot.last_error & 0xffffffffUL, snapshot.failed_command,
        snapshot.failed_status, (unsigned long)snapshot.failure_error & 0xffffffffUL);
    append_format(data, "timeout=%d\nnavigation_blocked=%d\nconfig_result=%d\n", snapshot.timed_out,
                  menu->ode.navigation_failed, menu->config_result);
    append_format(data, "transport_uncertain=%d\nrecovery_result=%d\nrecovery_media_off=%d\n",
                  menu->ode.transport_uncertain, menu->recovery_result, menu->recovery_media_off);
    append_format(data, "folder_workaround=%d\nusb_covers=%d\nscreen_margin=%d\nlarge_titles=%d\n",
                  folder_workaround(menu), menu->config.usb_covers, menu->config.screen_margin,
                  menu->config.large_titles);
    append_format(data, "preview_sound=%d\npreview_audio_status=%d\n", menu->config.preview_sound,
                  menu->preview_audio.status);
    append_format(data, "cover_index_count=%lu\npreview_index_count=%lu\nmetadata_count=%lu\n",
                  (unsigned long)menu->usb_art.count, (unsigned long)menu->usb_previews.count,
                  (unsigned long)menu->metadata.count);
    strcat(data, "last_usb_file=");
    strcat(data, menu->ode.file_path);
    strcat(data, "\n");
    append_format(data, "cover_load_ms=%lu\ncover_commands=%lu\ncover_cached=%d\n",
                  menu->cover_load_ms, menu->cover_commands, menu->cover_cached);
    append_format(data,
                  "remember_position=%d\nrecent_games=%d\nvideo_previews=%d\nunsaved_settings=%d\n",
                  menu->config.remember_position, menu->config.recent_games,
                  menu->config.video_previews, config_dirty(menu));
    append_format(data, "queue_count=%lu\nqueue_synchronized=%d\nqueue_dirty=%d\n",
                  (unsigned long)menu->playlist.count, menu->playlist.synchronized,
                  menu->playlist.dirty);
    append_format(data,
                  "usb_write_check=%d\nbackup_result=%d\nrestore_result=%d\nnvram_error=%08lx\n",
                  menu->usb_tested, menu->backup_result, menu->restore_result,
                  (unsigned long)menu->nvram_info.last_error & 0xffffffffUL);
    append_format(data, "position_result=%d\nrecent_result=%d\nfolder=%.64s\n",
                  menu->position_result, menu->recent_result, menu->breadcrumb);
    append_format(data, "boot_at_ms=%lu\nboot_draws=%lu\nboot_draw_ms=%lu\n", menu->boot_at,
                  menu->boot_draws, menu->boot_draw_ms);
    /* Commands sent during startup, by kind (see ode_bus_kind). */
    for (i = 0; i < 17; ++i) {
        if (!menu->boot_kind_count[i])
            continue;
        if (i < 16)
            append_format(data, "boot_ode_%s%u=%lu/%lu\n", i < 8 ? "e" : "c", (unsigned int)(i % 8),
                          menu->boot_kind_count[i], menu->boot_kind_ms[i]);
        else
            append_format(data, "boot_ode_other=%lu/%lu\n", menu->boot_kind_count[i],
                          menu->boot_kind_ms[i]);
    }
    for (i = 0; i < BOOT_STEPS; ++i)
        append_format(data, "boot_%s_ms=%lu\nboot_%s_commands=%lu\n", boot_step_names[i],
                      menu->boot_ms[i], boot_step_names[i], menu->boot_commands[i]);
    append_format(data, "preview_result=%d\npreview_load_ms=%lu\n", menu->preview_result,
                  menu->preview_load_ms);
    append_format(data,
                  "preview_read_ms=%lu\n"
                  "preview_check_ms=%lu\n"
                  "preview_navigation_ms=%lu\n"
                  "preview_setup_ms=%lu\n",
                  menu->preview_read_ms, menu->preview_check_ms, menu->preview_navigation_ms,
                  menu->preview_setup_ms);
    append_format(
        data, "preview_bytes=%lu\npreview_commands=%lu\npreview_frames=%u\npreview_fps=%u\n",
        menu->preview_bytes, menu->preview_commands, menu->preview_frames, menu->preview_fps);
    append_format(data, "preview_source_frames=%lu\npreview_chunks=%u\npreview_vram_bytes=%lu\n",
                  menu->preview_source_frames, menu->preview_chunks, menu->preview_vram_bytes);
    append_format(data,
                  "preview_decodes=%lu\n"
                  "preview_decode_ms=%lu\n"
                  "preview_decode_max_ms=%lu\n"
                  "preview_skipped_frames=%lu\n",
                  menu->preview_decodes, menu->preview_decode_ms, menu->preview_decode_max_ms,
                  menu->preview_skipped);
    append_format(data, "preview_full_view_decodes=%lu\npreview_full_view_skipped=%lu\n",
                  menu->preview_full_view_decodes, menu->preview_full_view_skipped);
    append_format(data, "preview_video_draws=%lu\npreview_video_ms=%lu\n",
                  menu->preview_video_draws, menu->preview_video_ms);
    append_format(data,
                  "preview_light_draws=%lu\n"
                  "preview_full_draws=%lu\n"
                  "preview_draw_ms=%lu\n"
                  "preview_pass_max_ms=%lu\n",
                  menu->preview_light_draws, menu->preview_full_draws, menu->preview_draw_ms,
                  menu->preview_pass_max_ms);
    append_format(data, "preview_tuning_draws=%lu\n", menu->tuning_draws);
    append_format(data,
                  "stream_chunks=%lu\n"
                  "stream_read_ms=%lu\n"
                  "stream_read_max_ms=%lu\n"
                  "stream_stalls=%lu\n"
                  "stream_stall_ms=%lu\n",
                  menu->stream.chunks_read, menu->stream.read_ms, menu->stream.read_max_ms,
                  menu->stream.stalls, menu->stream.stall_ms);
    append_format(data, "stream_slots=%u\nstream_error=%d\nstream_reader_error=%08lx\n",
                  menu->stream.slots, menu->stream.error,
                  (unsigned long)menu->stream.reader_error_code & 0xffffffffUL);
    append_format(data,
                  "stream_decode_ms=%lu\n"
                  "stream_decode_frames=%lu\n"
                  "stream_idle_ms=%lu\n"
                  "stream_idle_frames=%lu\n",
                  menu->stream.decode_ms, menu->stream.decode_frames, menu->stream.idle_ms,
                  menu->stream.idle_frames);
    append_format(data, "stream_full_ms=%lu\nstream_full_frames=%lu\n", menu->stream.full_ms,
                  menu->stream.full_frames);
    append_format(data, "stream_loop_holds=%lu\nstream_loop_hold_ms=%lu\nstream_spare_blocks=%lu\n",
                  menu->stream.loop_holds, menu->stream.loop_hold_ms, menu->stream.spare_blocks);
    for (i = 0; i < 4 && i < menu->stream.stalls; ++i)
        append_format(data, "stream_stall%d=%lu %lu %08lx %lu\n", (int)i,
                      menu->stream.stall_log[i][0], menu->stream.stall_log[i][1],
                      menu->stream.stall_log[i][2], menu->stream.stall_log[i][3]);
    append_format(data, "preview_free_bytes=%lu\npreview_largest_bytes=%lu\n",
                  menu->preview_free_bytes, menu->preview_largest_bytes);
    append_format(data, "preview_vram_free_bytes=%lu\npreview_vram_largest_bytes=%lu\n",
                  menu->preview_vram_free_bytes, menu->preview_vram_largest_bytes);
    memory_available(0, &free_bytes, &largest);
    append_format(data, "export_dram_free_bytes=%lu\nexport_dram_largest_bytes=%lu\n", free_bytes,
                  largest);
    memory_available(1, &free_bytes, &largest);
    append_format(data, "export_vram_free_bytes=%lu\nexport_vram_largest_bytes=%lu\n", free_bytes,
                  largest);
    strcat(data, "Identity/revision may be hard-coded; exact firmware build is unknown.\n"
                 "Snapshot taken before export file I/O.\n");
    /* Padded to a fixed size with blank lines: the ODE cannot shorten a
     * file, so a shorter report would leave the end of the last one. */
    i = strlen(data);
    memset(data + i, '\n', sizeof(data) - 1 - i);
    data[sizeof(data) - 1] = 0;
    menu->tool_message = "Writing / verifying diagnostics...";
    draw(menu);
    menu->diagnostics_result =
        text_at_root(menu, "diagnostics.txt", data, NULL, 0, NULL) < 0 ? -1 : 1;
    menu->tool_message = menu->diagnostics_result > 0 ? "Saved USB diagnostics.txt."
                                                      : "USB export unavailable or failed.";
}
/* ==== Favorites and opening saved games ==== */

/* At the root: reads favorites.txt into menu->favorites. */
static int favorites_read(void *context)
{
    Menu *menu = (Menu *)context;
    char *data = (char *)malloc(FAVORITES_BYTES);
    size_t length;
    int result = -1;
    if (data && !usb_file_text(&menu->ode, ode_bus_read_buffer, "favorites.txt", data,
                               FAVORITES_BYTES, &length))
        result = menu_favorites_parse(&menu->favorites, data, length);
    menu->favorites_valid = result == 0;
    free(data);
    return result;
}
/*
 * Opens the Favorites screen: reads favorites.txt from the drive's root.
 * Favorites are saved relative to the volume, so when browsing a device in
 * the device list, its label becomes the prefix for opening them.
 */
static void open_favorites(Menu *menu)
{
    ArtRouteStep route[MAX_DEPTH];
    int result;
    menu_favorites_free(&menu->favorites);
    menu->favorites_valid = 0;
    strcpy(menu->favorites_prefix, "/Storage/");
    if (menu->depth && !(menu->path_ids[0] & 0xffffffUL)) {
        strcat(menu->favorites_prefix, menu->path[0]);
        strcat(menu->favorites_prefix, "/");
    }
    if (menu->demo || !menu->ready || menu->ode.navigation_failed || menu->bus.timed_out ||
        menu->config_result == 2 || strcmp(menu->ode.identity, "LOCODE")) {
        menu->tool_message = "USB favorites unavailable.";
        return;
    }
    if (!menu->depth) {
        size_t i;
        for (i = 0; i < menu->listing.count; ++i)
            if (!(menu->listing.entries[i]->toc_id & 0xffffffUL)) {
                menu->tool_message = "Enter a USB volume first.";
                return;
            }
    }
    current_route(menu, route);
    menu->tool_message = "Reading USB favorites...";
    draw(menu);
    result = art_at_root(&menu->ode, route, menu->depth, &menu->listing, folder_workaround(menu),
                         favorites_read, menu);
    menu->tool_selected = 0;
    if (result == -2 || menu->bus.timed_out || menu->ode.navigation_failed) {
        menu->ready = menu->view_valid = 0;
        menu->tool_message = "USB I/O failed. Restart console.";
    } else
        menu->tool_message =
            result ? "Invalid favorites.txt on USB."
                   : (menu_favorites_visible_count(&menu->favorites, menu->config.hide_adult)
                          ? NULL
                          : "C adds the selected browser game.");
}
/* An addition to or removal from favorites.txt, through art_at_root. */
typedef struct FavoritesEdit {
    Menu *menu;
    const char *path;
    int remove;
} FavoritesEdit;
/* At the root: re-reads favorites.txt, applies the edit and writes it back.
 * Returns 0, 1 if there was nothing to change, or -1. */
static int favorites_edit_at_root(void *context)
{
    FavoritesEdit *edit = (FavoritesEdit *)context;
    char *data;
    size_t i, used = 0;
    int result;
    /* Re-read before every edit: never overwrite a malformed/unreadable list. */
    if (favorites_read(edit->menu) < 0)
        return -1;
    result = menu_favorites_edit(&edit->menu->favorites, edit->path, edit->remove);
    if (result)
        return result;
    data = (char *)malloc(FAVORITES_BYTES);
    if (!data)
        return -1;
    for (i = 0; i < edit->menu->favorites.count; ++i) {
        size_t n = strlen(edit->menu->favorites.paths[i]);
        memcpy(data + used, edit->menu->favorites.paths[i], n);
        used += n;
        data[used++] = '\n';
    }
    data[used] = 0;
    result = usb_file_text_write(&edit->menu->ode, ode_bus_read_buffer, "favorites.txt", data);
    free(data);
    if (result < 0)
        edit->menu->favorites_valid = 0;
    return result;
}
/*
 * Adds the game selected in the browser to favorites.txt (C in Favorites),
 * or with `remove`, removes the favorite selected on screen (X). Saved as
 * the disc's path below the volume, for a folder acting as its game its
 * first disc's.
 */
static void edit_favorites(Menu *menu, int remove)
{
    ArtRouteStep route[MAX_DEPTH];
    FavoritesEdit edit;
    char path[FAVORITE_PATH];
    size_t used = 0, visible;
    unsigned int i, first = menu->depth && !(menu->path_ids[0] & 0xffffffUL) ? 1 : 0;
    int result;
    if (menu->demo || !menu->ready || menu->ode.navigation_failed || menu->bus.timed_out ||
        menu->config_result == 2 || strcmp(menu->ode.identity, "LOCODE")) {
        menu->tool_message = "USB favorites unavailable.";
        return;
    }
    if (remove) {
        if (!menu->favorites_valid ||
            menu->tool_selected >=
                menu_favorites_visible_count(&menu->favorites, menu->config.hide_adult)) {
            menu->tool_message = "Select a favorite to remove.";
            return;
        }
        strcpy(path, menu->favorites.paths[menu_favorites_visible_index(
                         &menu->favorites, menu->config.hide_adult, menu->tool_selected)]);
    } else {
        const MenuEntry *entry;
        const char *discs[ODE_MAX_DISCS];
        size_t game;
        if (menu->listing.has_parent && !menu->selected)
            goto select_game;
        if (!row_count(menu))
            goto select_game;
        entry = browser_entry(menu, menu->selected - (menu->listing.has_parent ? 1 : 0));
        game = folder_game(menu, entry, discs);
        if (!entry || (entry->is_directory && !game))
            goto select_game;
        /* The folders below the volume (skipping a device label)... */
        for (i = first; i < menu->depth; ++i) {
            size_t n = strlen(menu->path[i]);
            if (used + n + 1 >= sizeof(path))
                goto too_long;
            memcpy(path + used, menu->path[i], n);
            used += n;
            path[used++] = '/';
        }
        /* A folder acting as its game is saved as its (first) disc. */
        if (used + strlen(entry->name) + (game ? strlen(discs[0]) + 1 : 0) >= sizeof(path))
            goto too_long;
        strcpy(path + used, entry->name);
        if (game) {
            strcat(path, "/");
            strcat(path, discs[0]);
        }
    }
    current_route(menu, route);
    edit.menu = menu;
    edit.path = path;
    edit.remove = remove;
    menu->tool_message = "Saving / verifying favorites...";
    draw(menu);
    result = art_at_root(&menu->ode, route, menu->depth, &menu->listing, folder_workaround(menu),
                         favorites_edit_at_root, &edit);
    if (result == -2 || menu->bus.timed_out || menu->ode.navigation_failed) {
        menu->ready = menu->view_valid = 0;
        menu->tool_message = "USB I/O failed. Restart console.";
    } else if (result < 0)
        menu->tool_message = "Favorites not saved. Check USB file.";
    else if (result == 1)
        menu->tool_message = remove ? "Favorite already removed." : "Game is already a favorite.";
    else
        menu->tool_message = remove ? "Favorite removed and saved." : "Favorite added and saved.";
    visible = menu_favorites_visible_count(&menu->favorites, menu->config.hide_adult);
    if (menu->tool_selected >= visible)
        menu->tool_selected = visible ? visible - 1 : 0;
    return;
select_game:
    menu->tool_message = "Select a game in the browser first.";
    return;
too_long:
    menu->tool_message = "Favorite path is too long.";
}
/* Whether a saved /Storage disc path lies in a folder that acts as its game;
 * if so, `folder` receives that folder's path. The metadata's paths start
 * below the volume, so when the first part may be a volume label, the path
 * is tried without it too. */
static int saved_game_folder(Menu *menu, const char *path, char *folder, size_t size)
{
    static char prefix[9217];
    const char *discs[ODE_MAX_DISCS], *name = strrchr(path, '/'), *relative = path + STORAGE_PREFIX;
    size_t count, i, n;
    int attempt;
    if (!name || strncmp(path, "/Storage/", 9) || (size_t)(name - path) >= size)
        return 0;
    for (attempt = 0; attempt < 2 && relative && relative < name; ++attempt) {
        n = (size_t)(name + 1 - relative);
        if (n >= sizeof(prefix))
            return 0;
        memcpy(prefix, relative, n);
        prefix[n] = 0;
        count = metadata_folder_game(menu, prefix, discs);
        for (i = 0; i < count; ++i)
            if (!menu_path_compare(discs[i], name + 1)) {
                memcpy(folder, path, (size_t)(name - path));
                folder[name - path] = 0;
                return 1;
            }
        relative = strchr(relative, '/');
        if (relative)
            ++relative;
    }
    return 0;
}
/* Opens a disc's folder in the browser with the disc selected, for Find,
 * Favorites and Recent. A disc in a folder that acts as its game shows that
 * folder selected instead, as browsing to the game would. */
static void open_saved_path(Menu *menu, const char *path)
{
    static char folder[ENTRY_PATH_BYTES];
    ArtRouteStep route[MAX_DEPTH];
    LibraryLocation *location;
    int result;
    if (menu->ode.navigation_failed || menu->bus.timed_out || !menu->ready)
        return;
    location = (LibraryLocation *)malloc(sizeof(*location));
    if (!location) {
        menu->tool_message = "Not enough memory.";
        return;
    }
    current_route(menu, route);
    menu->tool_message = "Opening saved game folder...";
    draw(menu);
    if (saved_game_folder(menu, path, folder, sizeof(folder)))
        result = library_select_directory(&menu->ode, route, menu->depth, &menu->listing,
                                          folder_workaround(menu), folder, location);
    else
        result = library_open(&menu->ode, route, menu->depth, &menu->listing,
                              folder_workaround(menu), path, location);
    if (!result)
        adopt_location(menu, location);
    else {
        menu->tool_message =
            result == -2 ? "Folder I/O failed. Restart console." : "Saved path not found on USB.";
        if (result == -2)
            menu->ready = menu->view_valid = 0;
    }
    free(location);
}
/* A on a row of Favorites or Recent: opens that game in the browser. */
static void open_favorite(Menu *menu)
{
    char path[FAVORITE_PATH + ODE_MAX_NAME + 10];
    const char *selected;
    if (menu->tools == TOOL_RECENT) {
        if (!menu->recent_valid || menu->tool_selected >= menu->recent.count)
            return;
        selected = menu->recent.paths[menu->tool_selected];
        strcpy(path, "/Storage/");
    } else {
        if (!menu->favorites_valid ||
            menu->tool_selected >=
                menu_favorites_visible_count(&menu->favorites, menu->config.hide_adult))
            return;
        selected = menu->favorites.paths[menu_favorites_visible_index(
            &menu->favorites, menu->config.hide_adult, menu->tool_selected)];
        strcpy(path, menu->favorites_prefix);
    }
    strcat(path, selected);
    open_saved_path(menu, path);
}
/* ==== Settings ==== */

/* The rows of Tools > Settings, in order. */
enum {
    SETTING_MARGIN,
    SETTING_LARGE_TITLES,
    SETTING_FOLDER_WORKAROUND,
    SETTING_COVERS,
    SETTING_VIDEO_PREVIEWS,
    SETTING_REMEMBER_POSITION,
    SETTING_RECENT_GAMES,
    SETTING_PREVIEW_SOUND,
    SETTING_CATEGORIES,
    SETTING_DISPLAY_DEFAULTS,
    SETTING_DISCARD,
    SETTING_SAVE,
    SETTING_ROWS
};

/*
 * The settings as the Settings screen shows them. Display settings and
 * preview sound change menu->config directly, as they take effect at once;
 * the rest take effect at the next startup, so their edits are kept in the
 * menu's settings_* fields until then.
 */
static void settings_values(const Menu *menu, MenuConfig *settings)
{
    *settings = menu->config;
    settings->folder_workaround = menu->settings_folder_workaround;
    settings->usb_covers = menu->settings_usb_covers;
    settings->remember_position = menu->settings_remember_position;
    settings->recent_games = menu->settings_recent_games;
    settings->video_previews = menu->settings_video_previews;
}
/* Whether the Settings screen has changes not yet saved to the drive. */
static int config_dirty(const Menu *menu)
{
    MenuConfig current;
    const MenuConfig *saved = &menu->saved_config;
    settings_values(menu, &current);
    return current.folder_workaround != saved->folder_workaround ||
           current.usb_covers != saved->usb_covers ||
           current.screen_margin != saved->screen_margin ||
           current.large_titles != saved->large_titles ||
           current.remember_position != saved->remember_position ||
           current.recent_games != saved->recent_games ||
           current.video_previews != saved->video_previews ||
           current.preview_sound != saved->preview_sound || current.hide_adult != saved->hide_adult;
}
/* Puts every setting back as last saved (X in Settings). */
static void discard_config(Menu *menu)
{
    menu->config.preview_sound = menu->saved_config.preview_sound;
    menu->config.hide_adult = menu->saved_config.hide_adult;
    menu_set_hide_adult(menu->config.hide_adult);
    menu->config.screen_margin = menu->saved_config.screen_margin;
    menu->config.large_titles = menu->saved_config.large_titles;
    menu->settings_folder_workaround = menu->saved_config.folder_workaround;
    menu->settings_usb_covers = menu->saved_config.usb_covers;
    menu->settings_remember_position = menu->saved_config.remember_position;
    menu->settings_recent_games = menu->saved_config.recent_games;
    menu->settings_video_previews = menu->saved_config.video_previews;
    menu->tool_message = "Unsaved settings discarded.";
    selection_changed(menu);
}
/* Saves the settings to ohmy3do.ini (C in Settings). Refused if the file
 * did not load, so a file the menu misread is never overwritten. */
static void save_config(Menu *menu)
{
    if (menu->config_result != 0) {
        menu->tool_message = "Load a valid INI before saving.";
        return;
    }
    if (!save_root_operation(menu, SAVE_SETTINGS, MENU_CONFIG_NAME, NULL)) {
        settings_values(menu, &menu->saved_config);
        menu->tool_message = "Saved. Non-display: after reboot.";
    }
}
/*
 * A, Left or Right on a Settings row: changes the setting (`direction` -1
 * or 1 steps the screen margin), or does what the row says. Display
 * changes redraw the browser underneath, via selection_changed.
 */
static void change_config(Menu *menu, int direction)
{
    menu->tool_message = "Changed. C:Save to USB";
    switch (menu->tool_selected) {
    case SETTING_SAVE:
        save_config(menu);
        return;
    case SETTING_PREVIEW_SOUND:
        menu->config.preview_sound = !menu->config.preview_sound;
        return;
    case SETTING_CATEGORIES:
        menu->tools = TOOL_CATEGORIES;
        menu->tool_selected = 0;
        menu->tool_message =
            menu->categories_valid ? NULL : "Invalid categories.json. Repair on PC.";
        return;
    case SETTING_VIDEO_PREVIEWS:
        menu->settings_video_previews = !menu->settings_video_previews;
        return;
    case SETTING_MARGIN:
        menu->config.screen_margin =
            (menu->config.screen_margin + direction + MENU_MARGIN_MAX + 1) % (MENU_MARGIN_MAX + 1);
        break;
    case SETTING_LARGE_TITLES:
        menu->config.large_titles = !menu->config.large_titles;
        break;
    case SETTING_FOLDER_WORKAROUND:
        menu->settings_folder_workaround = (menu->settings_folder_workaround + 1) % 3;
        break;
    case SETTING_COVERS:
        menu->settings_usb_covers = !menu->settings_usb_covers;
        break;
    case SETTING_REMEMBER_POSITION:
        menu->settings_remember_position = !menu->settings_remember_position;
        break;
    case SETTING_RECENT_GAMES:
        menu->settings_recent_games = !menu->settings_recent_games;
        break;
    case SETTING_DISCARD:
        discard_config(menu);
        break;
    default: /* SETTING_DISPLAY_DEFAULTS */
        menu->config.screen_margin = menu->config.large_titles = 0;
        break;
    }
    selection_changed(menu);
}

/* ==== Reordering favorites ==== */

/* Leaves reordering (B), putting the list back as it was before. */
static void cancel_favorites_move(Menu *menu)
{
    int result = 0;
    if (menu->favorites_original)
        result = menu_favorites_parse(&menu->favorites, menu->favorites_original,
                                      strlen(menu->favorites_original));
    free(menu->favorites_original);
    menu->favorites_original = NULL;
    menu->favorites_move = 0;
    if (result < 0)
        menu->favorites_valid = 0;
    menu->tool_message =
        result < 0 ? "Reload favorites before opening." : "Local ordering edits discarded.";
}
/* Starts reordering favorites (R), keeping the list as it is now so the
 * save can check nobody changed the file meanwhile, and so B can undo. */
static void begin_favorites_move(Menu *menu)
{
    if (!menu->favorites_valid ||
        !menu_favorites_visible_count(&menu->favorites, menu->config.hide_adult)) {
        menu->tool_message = "No favorites to move.";
        return;
    }
    menu->favorites_original = (char *)malloc(FAVORITES_BYTES);
    if (!menu->favorites_original ||
        menu_favorites_format(&menu->favorites, menu->favorites_original, FAVORITES_BYTES) < 0) {
        free(menu->favorites_original);
        menu->favorites_original = NULL;
        menu->tool_message = "Not enough memory.";
        return;
    }
    menu->favorites_move = 1;
    menu->tool_message = NULL;
}
/* Saves the new order (A while reordering), if favorites.txt on the drive
 * still holds what it did when reordering began. */
static void save_favorites_order(Menu *menu)
{
    MenuFavorites current;
    char *latest = (char *)malloc(FAVORITES_BYTES), *desired = (char *)malloc(FAVORITES_BYTES);
    size_t length;
    int result = -1;
    memset(&current, 0, sizeof(current));
    if (!latest || !desired || !menu->favorites_original)
        goto done;
    if (menu_favorites_format(&menu->favorites, desired, FAVORITES_BYTES) < 0 ||
        text_at_root(menu, "favorites.txt", NULL, latest, FAVORITES_BYTES, &length) < 0 ||
        menu_favorites_parse(&current, latest, length) < 0 ||
        menu_favorites_format(&current, latest, FAVORITES_BYTES) < 0)
        goto done;
    if (strcmp(latest, menu->favorites_original)) {
        menu->tool_message = "USB list changed. Cancel and reload.";
        result = 1;
        goto done;
    }
    if (text_at_root(menu, "favorites.txt", desired, NULL, 0, NULL) < 0) {
        menu->favorites_valid = 0;
        goto done;
    }
    free(menu->favorites_original);
    menu->favorites_original = NULL;
    menu->favorites_move = 0;
    menu->tool_message = "Favorite order saved and verified.";
    result = 0;
done:
    if (result < 0)
        menu->tool_message = "Favorite order not saved.";
    menu_favorites_free(&current);
    free(latest);
    free(desired);
}
/* ==== Find ==== */

/* The on-screen keyboard: four rows of ten keys. */
static const char find_keys[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_.";
#define FIND_KEY_COLUMNS 10
#define FIND_KEYS        40

/* Called while the search index loads: B cancels (returns 1); otherwise
 * redraws the "Loading" message now and then. */
static int search_progress(void *context, size_t games)
{
    Menu *menu = (Menu *)context;
    ControlPadEventData pad;
    if (GetControlPad(1, 0, &pad) >= 0 && (pad.cped_ButtonBits & ControlB)) {
        menu->suppress_buttons |= ControlB;
        return 1;
    }
    if (now_ms(menu) - menu->search_redraw >= 150UL) {
        (void)games;
        strcpy(menu->status, "Loading search index... B cancels");
        menu->tool_message = menu->status;
        draw(menu);
        menu->search_redraw = now_ms(menu);
    }
    return 0;
}
/* Loading the search index at the root, through art_at_root. `prefix` is
 * the menu path the index's paths are below ("/Storage", plus the device
 * label in the device list). */
typedef struct SearchIndexRead {
    Menu *menu;
    char prefix[ODE_MAX_NAME + 10];
} SearchIndexRead;
/* The search index loader's cancel check: B cancels. */
static int index_cancel(void *context)
{
    return search_progress(context, 0);
}
/* Loads the search index; if memory runs short, drops the cover cache and
 * tries once more. */
static int read_search_index(void *context)
{
    SearchIndexRead *read = (SearchIndexRead *)context;
    int result = library_index_load(&read->menu->ode, ode_bus_read_buffer, read->prefix,
                                    &read->menu->catalog, index_cancel, read->menu);
    if (result == -3) {
        cover_cache_clear(&read->menu->cover_cache);
        result = library_index_load(&read->menu->ode, ode_bus_read_buffer, read->prefix,
                                    &read->menu->catalog, index_cancel, read->menu);
    }
    return result;
}
/*
 * Searches the library for menu->find_query and shows the results. The
 * library's index (library.3di, written by the desktop app) is loaded
 * on the first search on a volume and kept; `rebuild` reloads it. Games in
 * hidden categories (and adult games when hidden) are left out.
 */
static void search_library(Menu *menu, int rebuild)
{
    ArtRouteStep route[MAX_DEPTH];
    SearchIndexRead *read;
    unsigned long volume =
        menu->depth ? menu->path_ids[0] & 0xff000000UL
                    : (menu->listing.count ? menu->listing.entries[0]->toc_id & 0xff000000UL : 0);
    int result = 0;
    size_t i;
    if (menu->demo) {
        menu->tool_message = "Library search needs an ODE.";
        return;
    }
    if (menu->config_result != 0 || strcmp(menu->ode.identity, "LOCODE")) {
        menu->tool_message = "Search needs a valid USB config.";
        return;
    }
    if (!menu->ready || menu->bus.timed_out || menu->ode.navigation_failed) {
        menu->tool_message = "ODE unavailable. Restart console.";
        return;
    }
    if (!menu->depth)
        for (i = 0; i < menu->listing.count; ++i)
            if (menu->listing.entries[i]->is_directory &&
                !(menu->listing.entries[i]->toc_id & 0xffffffUL)) {
                menu->tool_message = "Open a USB drive before searching.";
                return;
            }
    if (!menu->catalog.valid || rebuild || menu->catalog_volume != volume) {
        read = (SearchIndexRead *)malloc(sizeof(*read));
        if (!read) {
            menu->tool_message = "Not enough search memory.";
            return;
        }
        read->menu = menu;
        strcpy(read->prefix, "/Storage");
        if (menu->depth && !(menu->path_ids[0] & 0xffffffUL)) {
            strcat(read->prefix, "/");
            strcat(read->prefix, menu->path[0]);
        }
        stop_preview(menu);
        current_route(menu, route);
        menu->search_redraw = 0;
        menu->tool_message = "Loading search index... B cancels";
        draw(menu);
        result = art_at_root(&menu->ode, route, menu->depth, &menu->listing,
                             folder_workaround(menu), read_search_index, read);
        free(read);
        if (result) {
            menu->tool_message =
                result == 1
                    ? "Search canceled."
                    : (result == -2 ? "Folder I/O failed. Restart console."
                                    : (result == -3 ? "Not enough search memory."
                                                    : "Run index-media.py on the USB drive."));
            if (result == -2 || menu->bus.timed_out || menu->ode.navigation_failed)
                menu->ready = menu->view_valid = 0;
            return;
        }
        menu->catalog_volume = volume;
    }

    /* Match the names, not their folders. */
    menu->search_count = 0;
    for (i = 0; i < menu->catalog.count; ++i) {
        const char *path = menu->catalog.paths[i], *name = strrchr(path, '/');
        if (menu_categories_hidden_path(path) || (menu->config.hide_adult && menu_adult_path(path)))
            continue;
        if (menu_name_matches(name ? name + 1 : path, menu->find_query))
            menu->search_matches[menu->search_count++] = i;
    }
    menu->tool_message = NULL;
    menu->tools = TOOL_RESULTS;
    menu->tool_selected = 0;
    menu->selection_since = now_ms(menu);
}
/* Moves the on-screen keyboard's cursor, wrapping at the edges. */
static void keyboard_move(Menu *menu, u32 pressed)
{
    unsigned int row = menu->find_key / FIND_KEY_COLUMNS * FIND_KEY_COLUMNS;
    if (pressed & ControlLeft)
        menu->find_key = row + (menu->find_key + FIND_KEY_COLUMNS - 1) % FIND_KEY_COLUMNS;
    if (pressed & ControlRight)
        menu->find_key = row + (menu->find_key + 1) % FIND_KEY_COLUMNS;
    if (pressed & ControlUp)
        menu->find_key = (menu->find_key + FIND_KEYS - FIND_KEY_COLUMNS) % FIND_KEYS;
    if (pressed & ControlDown)
        menu->find_key = (menu->find_key + FIND_KEY_COLUMNS) % FIND_KEYS;
}

/* Typing into `buffer` (`capacity` bytes) with the on-screen keyboard: A
 * types the key, X deletes the last character and C clears. */
static void keyboard_type(Menu *menu, u32 pressed, char *buffer, size_t capacity)
{
    size_t length = strlen(buffer);
    keyboard_move(menu, pressed);
    if ((pressed & ControlA) && length < capacity - 1) {
        buffer[length++] = find_keys[menu->find_key];
        buffer[length] = 0;
    }
    if ((pressed & ControlX) && length)
        buffer[length - 1] = 0;
    if (pressed & ControlC)
        buffer[0] = 0;
}

/* The Find keyboard: the pad moves around the keys, A types, X deletes, C
 * clears and Start searches. */
static void find_input(Menu *menu, u32 pressed)
{
    keyboard_type(menu, pressed, menu->find_query, sizeof(menu->find_query));
    if (pressed & ControlStart)
        search_library(menu, 0);
}

/* ==== Tools screen helpers ==== */

/* A line of text in the Tools panel, `row` lines down. */
static void tool_line(Menu *menu, int row, const char *line, Color color)
{
    text(menu, 22, 82 + row * 12, line, color);
}
/* A Tools list row; the selected one is a lit remote key, as in the library.
 * Returns the ink for the row's text, which is drawn on the matching ground. */
static Color tool_row(Menu *menu, int y, int selected)
{
    if (selected)
        rectangle(menu, 14, y - 1, 292, 13, ACCENT);
    menu->text_background_set = 1;
    menu->text_background = selected ? ACCENT : PANEL;
    return selected ? INK : MUTED;
}
/* "A:Open C:Add X:Remove" as controller badges; with lit, the first key is
 * green. Words without a colon continue the previous action ("A:Back up one"). */
static void control_badges(Menu *menu, int x, int y, const char *controls, int lit)
{
    char key[16], action[40];
    int first = lit;
    while (*controls) {
        const char *colon = strchr(controls, ':'), *next;
        size_t n;
        if (!colon)
            break;
        n = (size_t)(colon - controls);
        if (n >= sizeof(key))
            n = sizeof(key) - 1;
        memcpy(key, controls, n);
        key[n] = 0;
        /* The action runs to the start of the next "Key:" word. */
        for (next = colon + 1; *next; ++next) {
            const char *word = next, *end;
            if (*next != ' ')
                continue;
            for (end = word + 1; *end && *end != ' ' && *end != ':'; ++end) {
            }
            if (*end == ':')
                break;
        }
        n = (size_t)(next - colon - 1);
        if (n >= sizeof(action))
            n = sizeof(action) - 1;
        memcpy(action, colon + 1, n);
        action[n] = 0;
        x += button(menu, x, y, key, action, first);
        first = 0;
        controls = *next ? next + 1 : next;
    }
}
/*
 * Backup slots. The Slots screen shows slots a page of 8 at a time; on the
 * first page (when not choosing where to back up to), rows 8 and 9 are the
 * two recovery slots, which the menu writes automatically before a restore.
 * With a slot search, the rows are its results instead.
 */

/* The slot's file, below .ohmy3do. */
static void slot_path(char *path, unsigned int slot)
{
    if (slot == SAVE_RECOVERY_SLOT)
        strcpy(path, "saves/recovery.3ds");
    else if (slot == SAVE_RECOVERY2_SLOT)
        strcpy(path, "saves/recovery2.3ds");
    else
        format(path, "saves/slot%02lu.3ds", (unsigned long)slot + 1);
}
/* How many rows the Slots screen shows. */
static size_t slot_count(const Menu *menu)
{
    unsigned long remaining = SAVE_SLOT_LIMIT - menu->slot_page * 8;
    if (menu->slot_filtered)
        return menu->slot_results.count;
    return remaining < 8 ? (size_t)remaining : !menu->backup_mode && !menu->slot_page ? 10 : 8;
}
/* The slot shown on `row`. */
static unsigned int slot_at(const Menu *menu, size_t row)
{
    if (menu->slot_filtered)
        return menu->slot_results.rows[row].slot;
    return row == 8   ? SAVE_RECOVERY_SLOT
           : row == 9 ? SAVE_RECOVERY2_SLOT
                      : (unsigned int)(menu->slot_page * 8 + row);
}
/* Turns to the page showing `slot` and selects it (in the search results
 * if it is there, otherwise leaving the search). */
static void slot_select(Menu *menu, unsigned int slot)
{
    size_t i;
    if (menu->slot_filtered) {
        for (i = 0; i < menu->slot_results.count; ++i)
            if (menu->slot_results.rows[i].slot == slot) {
                menu->tool_selected = i;
                return;
            }
        menu->slot_filtered = 0;
    }
    if (slot == SAVE_RECOVERY_SLOT || slot == SAVE_RECOVERY2_SLOT) {
        menu->slot_page = 0;
        menu->tool_selected = slot == SAVE_RECOVERY_SLOT ? 8 : 9;
    } else {
        menu->slot_page = slot / 8;
        menu->tool_selected = slot % 8;
    }
}
/* The labels file for a page of slots (8 a page). */
static void labels_path(char *path, unsigned long page)
{
    if (!page)
        strcpy(path, "saves/slot-labels.txt");
    else
        format(path, "saves/slot-labels-%lu.txt", page);
}

/* The labels file for the current slot page. */
static void slot_labels_path(const Menu *menu, char *path)
{
    labels_path(path, menu->slot_page);
}
/* Searches the slot catalog for menu->slot_query, showing the matching
 * slots, or back to the search options if the catalog can't be read. */
static void slot_search(Menu *menu)
{
    menu->tool_selected = 0;
    if (!save_root_operation(menu, SAVE_SEARCH_CATALOG, NULL, NULL)) {
        menu->slot_filtered = 1;
        menu->tools = TOOL_SLOTS;
        menu->tool_message = menu->slot_results.count ? NULL : "No matching indexed slots.";
    } else {
        menu->tools = TOOL_SLOT_FIND;
    }
}
/* The save selected in a backup's list: in the hex editor's copy, the copy
 * being made, or NVRAM, whichever is open. */
static const SaveEntry *backup_entry(const Menu *menu)
{
    return menu->hex_archive.bytes     ? &menu->hex_archive.entries[menu->save_selected]
           : menu->copy_original.bytes ? &menu->copy_original.entries[menu->save_selected]
                                       : &menu->nvram.entries[menu->save_selected];
}
/* Ends copying a backup to another slot, back on the source backup. */
static void finish_copy(Menu *menu)
{
    save_archive_free(&menu->backup);
    menu->backup = menu->copy_original;
    memset(&menu->copy_original, 0, sizeof(menu->copy_original));
    menu->backup_slot = menu->copy_source_slot;
    menu->tools = TOOL_ARCHIVE;
    menu->tool_selected = menu->save_selected;
    menu->backup_mode = 0;
    save_archive_free(&menu->pending);
}

/* ==== Game details (C+Start) ==== */

/* Adds `value` to the details, word-wrapped (rather than cut off, which
 * could hide a region or revision at the end of a name). Rows count on
 * across calls; only those on the current page (10 rows) are drawn. */
static void game_details_text(Menu *menu, const char *value, size_t *row, Color ink)
{
    char line[97];
    size_t length, take;
    if (!value || !*value)
        return;
    length = strlen(value);
    while (length) {
        take = ui_font_fit(value, length, 274, UI_FONT_SMALL);
        if (!take)
            take = 1;
        if (take > sizeof(line) - 1)
            take = sizeof(line) - 1;
        if (take < length && value[take] != ' ') {
            size_t word = take;
            while (word && value[word] != ' ')
                --word;
            if (word)
                take = word;
        }
        if (*row / 10 == menu->game_details_page) {
            memcpy(line, value, take);
            line[take] = 0;
            tool_line(menu, (int)(*row % 10), line, ink);
        }
        ++*row;
        value += take;
        length -= take;
        while (length && *value == ' ') {
            ++value;
            --length;
        }
    }
}
/* Draws the details of the selected game: genre, publisher, release,
 * description, its file name, location and discs. Sends no ODE commands. */
static void draw_game_details(Menu *menu)
{
    const MenuEntry *entry = NULL;
    const LibraryGameInfo *info;
    const char *discs[ODE_MAX_DISCS];
    size_t row = 0, count, i;
    char line[80];
    static char path[ENTRY_PATH_BYTES];
    if (row_count(menu) && !(menu->listing.has_parent && !menu->selected) && !menu->review)
        entry = browser_entry(menu, menu->selected - (menu->listing.has_parent ? 1 : 0));
    if (!entry) {
        tool_line(menu, 0, "Select a game or folder first.", MUTED);
        menu->game_details_pages = 1;
        return;
    }
    info = game_info(menu, entry);
    if (info) {
        game_details_text(menu, info->genre, &row, ACCENT);
        game_details_text(menu, info->publisher, &row, TEXT);
        game_details_text(menu, info->released, &row, TEXT);
    }
    game_details_text(
        menu,
        *menu->game_description
            ? menu->game_description
            : "No description available. Prepare game information in the desktop app.",
        &row, TEXT);
    game_details_text(menu, "Original name:", &row, ACCENT);
    game_details_text(menu, entry->name, &row, TEXT);
    game_details_text(menu, "Location:", &row, ACCENT);
    if (!directory_path(menu, path, sizeof(path)))
        game_details_text(menu, path, &row, TEXT);
    else
        for (i = 0; i < menu->depth; ++i)
            game_details_text(menu, menu->path[i], &row, TEXT);
    count = folder_game(menu, entry, discs);
    if (count) {
        format(line, "Discs: %lu", (unsigned long)count);
        game_details_text(menu, line, &row, ACCENT);
        for (i = 0; i < count; ++i)
            game_details_text(menu, discs[i], &row, TEXT);
    } else
        game_details_text(menu, entry->is_directory ? "Folder" : "Disc image", &row, MUTED);
    menu->game_details_pages = (row + 9) / 10;
    format(line, "%lu / %lu", (unsigned long)(menu->game_details_page + 1),
           (unsigned long)menu->game_details_pages);
    label_on(menu, 254, 64, line, MUTED, UI_FONT_SMALL, PANEL);
}
/* Opens the details of the selected game, reading its description from the
 * drive. Only opening the details reads it: browsing never does. */
static void open_game_details(Menu *menu)
{
    const MenuEntry *entry = NULL;
    const LibraryGameInfo *info;
    char name[32], *bytes;
    size_t length;
    stop_preview(menu);
    menu->game_description[0] = 0;
    if (row_count(menu) && !(menu->listing.has_parent && !menu->selected) && !menu->review)
        entry = browser_entry(menu, menu->selected - (menu->listing.has_parent ? 1 : 0));
    info = entry ? game_info(menu, entry) : NULL;
    menu->tools = TOOL_GAME_DETAILS;
    menu->tool_message = NULL;
    menu->game_details_page = 0;
    menu->game_details_pages = 1;
    if (info && !menu->demo) {
        bytes = (char *)malloc(12288);
        if (bytes) {
            int result;
            game_info_filename(info->path, name);
            /* Beside the disc, as the desktop prepares drives, unless the
             * media indexes say this drive keeps its media at the root. */
            if ((menu->usb_art.count && !menu->usb_art.beside) ||
                (menu->usb_previews.count && !menu->usb_previews.beside))
                result = text_at_root(menu, name, NULL, bytes, 12288, &length);
            else
                result = text_beside(menu, entry, name + strlen("info/"), bytes, 12288, &length);
            if (!result)
                game_info_parse(bytes, length, info->path, menu->game_description,
                                sizeof(menu->game_description));
            free(bytes);
        }
    }
}
/* ==== The hex editor and cheats ==== */

/*
 * Opens the hex editor on the selected save of the open backup (C in a
 * backup). Edits are made to a copy; the backup as loaded is kept in
 * hex_original. Saving goes through the usual choice of slot and
 * confirmation, and never touches NVRAM: restoring is separate.
 */
static int open_hex(Menu *menu)
{
    save_archive_free(&menu->hex_archive);
    if (save_archive_build(&menu->hex_archive, menu->backup.entries, menu->backup.count) < 0) {
        menu->tool_message = "Cannot prepare editor copy.";
        return -1;
    }
    menu->hex_original = menu->backup;
    memset(&menu->backup, 0, sizeof(menu->backup));
    menu->hex_source_slot = menu->backup_slot;
    menu->hex_offset = 0;
    menu->hex_editing = menu->hex_nibble = menu->hex_dirty = 0;
    menu->hex_view = TOOL_HEX;
    menu->tools = TOOL_HEX;
    menu->tool_message = NULL;
    return 0;
}
/* Leaves the hex editor, dropping the copy, back on the backup's list. */
static void close_hex(Menu *menu)
{
    save_archive_free(&menu->backup);
    menu->backup = menu->hex_original;
    memset(&menu->hex_original, 0, sizeof(menu->hex_original));
    menu->backup_slot = menu->hex_source_slot;
    save_archive_free(&menu->hex_archive);
    save_archive_free(&menu->pending);
    menu->hex_dirty = 0;
    menu->tools = TOOL_ARCHIVE;
    menu->tool_selected = menu->save_selected;
}
/* The hex editor: the save's name, then the 32 bytes (4 rows of 8) on the
 * cursor's page, with the byte being edited showing its new value. */
static void draw_hex(Menu *menu)
{
    const SaveEntry *entry = &menu->hex_archive.entries[menu->save_selected];
    size_t start = menu->hex_offset / 32 * 32, i;
    char line[80];
    format(line, "%.31s%s", entry->name, menu->hex_dirty ? " *" : "");
    tool_line(menu, 0, line, TEXT);
    format(line, "%lu bytes / offset %04lx", (unsigned long)entry->size,
           (unsigned long)menu->hex_offset);
    tool_line(menu, 1, line, MUTED);
    for (i = start; i < entry->size && i < start + 32; ++i) {
        int x = 76 + (int)(i % 8) * 27, y = 112 + (int)((i - start) / 8) * 15;
        int selected = i == menu->hex_offset;
        if (i % 8 == 0) {
            format(line, "%04lx", (unsigned long)i);
            label(menu, 22, y, line, MUTED, UI_FONT_SMALL);
        }
        rectangle(menu, x - 2, y - 2, 24, 13, selected ? ACCENT : SELECT);
        format(line, "%02x",
               selected && menu->hex_editing ? menu->hex_value : (unsigned int)entry->data[i]);
        label_on(menu, x, y, line, selected ? INK : TEXT, UI_FONT_SMALL,
                 selected ? ACCENT : SELECT);
    }
    if (!entry->size)
        tool_line(menu, 3, "This save is empty.", MUTED);
    tool_line(menu, 8,
              menu->hex_editing ? (menu->hex_nibble ? "Editing low nibble" : "Editing high nibble")
                                : "Edits stay in this copy until saved.",
              TEXT);
    tool_line(menu, 9, "Raw edits need game checksums.", AMBER);
}
/*
 * The hex editor's buttons. Moving: the pad moves the cursor by a byte or a
 * row, L and R by a page; A edits the byte under it. Editing: Up/Down
 * change the selected digit, Left/Right switch digits, A keeps the value
 * and B drops it. C saves the edited save to a slot; B leaves (asking
 * first if there are unsaved edits).
 */
static void hex_input(Menu *menu, u32 pressed)
{
    SaveEntry *entry = &menu->hex_archive.entries[menu->save_selected];
    if (menu->hex_editing) {
        unsigned int shift = menu->hex_nibble ? 0 : 4, digit;
        if (pressed & ControlB) {
            menu->hex_editing = 0;
            return;
        }
        if (pressed & (ControlLeft | ControlRight))
            menu->hex_nibble = !menu->hex_nibble;
        shift = menu->hex_nibble ? 0 : 4;
        digit = (menu->hex_value >> shift) & 15;
        if (pressed & ControlUp)
            digit = (digit + 1) & 15;
        if (pressed & ControlDown)
            digit = (digit + 15) & 15;
        menu->hex_value = (menu->hex_value & ~(15U << shift)) | (digit << shift);
        if (pressed & ControlA) {
            ((unsigned char *)entry->data)[menu->hex_offset] = (unsigned char)menu->hex_value;
            menu->hex_dirty =
                memcmp(entry->data, menu->hex_original.entries[menu->save_selected].data,
                       entry->size) != 0;
            menu->hex_editing = 0;
        }
        return;
    }
    if (pressed & ControlB) {
        if (menu->hex_dirty)
            menu->tools = TOOL_HEX_DISCARD;
        else
            close_hex(menu);
        return;
    }
    if (pressed & ControlC) {
        save_archive_free(&menu->pending);
        if (save_archive_build(&menu->pending, entry, 1) < 0) {
            menu->tool_message = "Cannot prepare edited backup.";
            return;
        }
        menu->backup_merge = 1;
        menu->backup_mode = 1;
        menu->slot_filtered = 0;
        menu->tools = TOOL_SLOTS;
        menu->tool_selected = 0;
        return;
    }
    if (!entry->size)
        return;
    if (pressed & ControlLeft) {
        if (menu->hex_offset)
            --menu->hex_offset;
    }
    if (pressed & ControlRight) {
        if (menu->hex_offset + 1 < entry->size)
            ++menu->hex_offset;
    }
    if (pressed & ControlUp) {
        if (menu->hex_offset >= 8)
            menu->hex_offset -= 8;
    }
    if (pressed & ControlDown) {
        if (menu->hex_offset + 8 < entry->size)
            menu->hex_offset += 8;
    }
    if (pressed & ControlLeftShift)
        menu->hex_offset = menu->hex_offset >= 32 ? menu->hex_offset - 32 : 0;
    if (pressed & ControlRightShift) {
        menu->hex_offset += 32;
        if (menu->hex_offset >= entry->size)
            menu->hex_offset = entry->size - 1;
    }
    if (pressed & ControlA) {
        menu->hex_editing = 1;
        menu->hex_nibble = 0;
        menu->hex_value = entry->data[menu->hex_offset];
    }
}
/*
 * Known values (X in the hex editor), for saves of games in the cheat
 * database: presets that set several values at once, then each known
 * value, six rows a page. Below: where the selected one lives in the save.
 */
static void draw_cheats(Menu *menu)
{
    const SaveEntry *entry = &menu->hex_archive.entries[menu->save_selected];
    const SaveCheatProfile *p = save_cheats_find(entry);
    size_t i, count, start;
    char line[80];
    if (!p) {
        tool_line(menu, 2, "No verified profile matches this save.", AMBER);
        return;
    }
    format(line, "%.22s / %s%s", p->game, save_cheats_external() ? "USB database" : "bundled",
           menu->hex_dirty ? " *" : "");
    tool_line(menu, 0, line, TEXT);
    count = p->preset_count + p->field_count;
    start = menu->cheat_selected / 6 * 6;
    for (i = start; i < count && i < start + 6; ++i) {
        int selected = i == menu->cheat_selected;
        Color ink = tool_row(menu, 100 + (int)(i - start) * 12, selected);
        if (i < p->preset_count)
            format(line, "%s", p->presets[i].label);
        else {
            const SaveCheatField *field = &p->fields[i - p->preset_count];
            format(line, "%s: %lu", field->label, save_cheats_value(entry, field));
        }
        label_on(menu, 22, 100 + (int)(i - start) * 12, line, ink, UI_FONT_SMALL,
                 selected ? ACCENT : PANEL);
    }
    menu->text_background_set = 0;
    if (menu->cheat_selected < p->preset_count) {
        const SaveCheatPreset *preset = &p->presets[menu->cheat_selected];
        if (preset->additional_count)
            format(line, "%lu fields / first offset %04lx",
                   (unsigned long)preset->additional_count + 1,
                   (unsigned long)p->fields[preset->field].offset);
        else
            format(line, "Offset %04lx -> %lu", (unsigned long)p->fields[preset->field].offset,
                   preset->value);
    } else {
        const SaveCheatField *field = &p->fields[menu->cheat_selected - p->preset_count];
        format(line, "Offset %04lx: %lu-%lu", (unsigned long)field->offset, field->min, field->max);
    }
    tool_line(menu, 8, line, MUTED);
    tool_line(menu, 9,
              p->checksum_kind ? "Game checksum updated automatically."
                               : "Other save bits stay unchanged.",
              ACCENT);
}
/* Known values' buttons: Up/Down choose, A applies a preset, Left/Right
 * step a value within its range. B and C work as in the hex editor; X
 * returns to it. */
static void cheats_input(Menu *menu, u32 pressed)
{
    SaveEntry *entry = &menu->hex_archive.entries[menu->save_selected];
    const SaveCheatProfile *p = save_cheats_find(entry);
    size_t count;
    int result = -1;
    if (pressed & (ControlB | ControlC)) {
        hex_input(menu, pressed);
        return;
    }
    if (pressed & ControlX) {
        menu->tools = menu->hex_view = TOOL_HEX;
        return;
    }
    if (!p)
        return;
    count = p->preset_count + p->field_count;
    if (pressed & ControlUp)
        menu->cheat_selected = menu->cheat_selected ? menu->cheat_selected - 1 : count - 1;
    if (pressed & ControlDown)
        menu->cheat_selected = (menu->cheat_selected + 1) % count;
    if (menu->cheat_selected < p->preset_count) {
        if (pressed & ControlA)
            result = save_cheats_preset(entry, p, menu->cheat_selected);
    } else if (pressed & (ControlLeft | ControlRight)) {
        size_t field = menu->cheat_selected - p->preset_count;
        unsigned long value = save_cheats_value(entry, &p->fields[field]);
        if (pressed & ControlLeft) {
            if (value > p->fields[field].min)
                --value;
        } else if (value < p->fields[field].max)
            ++value;
        result = save_cheats_set(entry, p, field, value);
    }
    if (!result)
        menu->hex_dirty = memcmp(entry->data, menu->hex_original.entries[menu->save_selected].data,
                                 entry->size) != 0;
}
/* ==== Drawing the Tools screens ==== */

/* The heading of the current Tools screen. */
static const char *tools_title(Menu *menu)
{
    switch (menu->tools) {
    case TOOL_CHEATS:
        return "Known values & cheats";
    case TOOL_HEX:
        return "Hex editor / USB copy";
    case TOOL_HEX_DISCARD:
        return "Discard edits?";
    case TOOL_SAVES:
        return "Save Manager";
    case TOOL_SLOTS:
        return menu->compare_choosing ? "Compare: choose second slot"
               : menu->backup_mode    ? "Choose backup slot"
                                      : "Save Manager";
    case TOOL_SLOT_NAME:
        return "Name backup slot";
    case TOOL_SLOT_FIND:
    case TOOL_SLOT_QUERY:
        return "Find backup slots";
    case TOOL_COMPARE:
    case TOOL_COMPARE_BYTES:
        return "Compare backups";
    case TOOL_ARCHIVE:
        return "Backup contents";
    case TOOL_HELP:
        return "Controls & help";
    case TOOL_DIAG:
        return "Diagnostics";
    case TOOL_CONFIRM_BACKUP:
    case TOOL_CONFIRM_RESTORE:
        return "Confirm action";
    case TOOL_FAVORITES:
        return menu->favorites_move ? "Move favorite" : "Favorites";
    case TOOL_CATEGORIES:
        return "Show / hide categories";
    case TOOL_CONFIG:
        return config_dirty(menu) ? "Settings / unsaved" : "Settings";
    case TOOL_RECENT:
        return "Recent games";
    case TOOL_FIND:
        return "Find a game";
    case TOOL_RESULTS:
        return "Search results";
    case TOOL_GAME_DETAILS:
        return "Game details";
    case TOOL_BACKUP_DETAILS:
        return menu->details_new ? "New backup contents" : "Existing slot contents";
    default:
        return "Menu";
    }
}

/* The Tools menu itself: seven buttons in two columns. */
static void draw_tools_home(Menu *menu)
{
    static const char *items[] = {"Save Manager", "Favorites",    "Settings",   "Help & controls",
                                  "Diagnostics",  "Recent games", "Find a game"};
    size_t i;
    for (i = 0; i < 7; ++i) {
        int x = 18 + (i / 4) * 144, y = 84 + (i % 4) * 28;
        int active = i == menu->tool_selected;
        rectangle(menu, x, y, 138, 24, active ? ACCENT : SELECT);
        label_on(menu, x + 9, y + 5, i == 2 && config_dirty(menu) ? "Settings *" : items[i],
                 active ? INK : TEXT, UI_FONT_NORMAL, active ? ACCENT : SELECT);
    }
}

/* Whether the screen is one of the Save Manager's two tabs: console saves,
 * or USB backups (unless choosing where to back up to). */
static int save_manager_tab(Menu *menu)
{
    return menu->tools == TOOL_SAVES || (menu->tools == TOOL_SLOTS && !menu->backup_mode);
}

/*
 * The screens that are a paged list: console saves, backup slots, the
 * saves in a backup (or a new one), favorites and recent games. The Save
 * Manager's tabs show above the list; a position or summary line below.
 */
static void draw_tool_list(Menu *menu)
{
    char line[80];
    int tabs = save_manager_tab(menu);
    SaveArchive *archive =
        menu->tools == TOOL_SAVES
            ? &menu->nvram
            : (menu->tools == TOOL_BACKUP_DETAILS && menu->details_new ? &menu->pending
                                                                       : &menu->backup);
    size_t rows = menu->tools == TOOL_SLOTS ? 8 : tabs ? 7 : 8, count, start, i;
    int gap = menu->tools == TOOL_SLOTS ? 11 : 12;
    int top = tabs ? 100 : 82;
    count = menu->tools == TOOL_SLOTS
                ? (slot_count(menu))
                : (menu->tools == TOOL_FAVORITES
                       ? menu_favorites_visible_count(&menu->favorites, menu->config.hide_adult)
                       : (menu->tools == TOOL_RECENT ? menu->recent.count : archive->count));
    if (tabs) {
        int usb = menu->tools == TOOL_SLOTS;
        rectangle(menu, 18, 82, 138, 14, usb ? SELECT : ACCENT);
        rectangle(menu, 162, 82, 138, 14, usb ? ACCENT : SELECT);
        label_on(menu, 24, 85, "Console saves", usb ? TEXT : INK, UI_FONT_SMALL,
                 usb ? SELECT : ACCENT);
        label_on(menu, 168, 85, "USB backups", usb ? INK : TEXT, UI_FONT_SMALL,
                 usb ? ACCENT : SELECT);
    }

    /* The rows on the selected row's page. */
    start = menu->tool_selected / rows * rows;
    for (i = start; i < count && i < start + rows; ++i) {
        int selected = i == menu->tool_selected;
        Color ink = tool_row(menu, top + (int)(i - start) * gap, selected);
        if (menu->tools == TOOL_SLOTS) {
            /* A slot by number and label, if it has one. */
            if (menu->slot_filtered && menu->slot_results.rows[i].label[0])
                format(line, "saves/%lu %s", (unsigned long)slot_at(menu, i) + 1,
                       menu->slot_results.rows[i].label);
            else if (!menu->slot_filtered && i < 8 && menu->slot_labels_page == menu->slot_page &&
                     menu->slot_labels.labels[i][0])
                format(line, "saves/%lu %s", (unsigned long)slot_at(menu, i) + 1,
                       menu->slot_labels.labels[i]);
            else
                slot_path(line, slot_at(menu, i));
            label_on(menu, 22, top + (int)(i - start) * gap, line + strlen("saves/"),
                     selected ? INK : TEXT, UI_FONT_SMALL, selected ? ACCENT : PANEL);
        } else if (menu->tools == TOOL_FAVORITES || menu->tools == TOOL_RECENT) {
            const char *name = menu->tools == TOOL_RECENT
                                   ? menu->recent.paths[i]
                                   : menu->favorites.paths[menu_favorites_visible_index(
                                         &menu->favorites, menu->config.hide_adult, i)];
            if (!strncmp(name, "/Storage/", STORAGE_PREFIX))
                name += STORAGE_PREFIX;
            game_label(menu, 22, 82 + (int)(i - start) * gap, 276, name, 0, UI_FONT_SMALL,
                       selected ? INK : TEXT, selected ? ACCENT : PANEL, selected);
        } else {
            /* A save: its game's name if known, otherwise its file name,
             * and its size. */
            const SaveEntry *e = &archive->entries[i];
            const char *game = save_names_find(&menu->save_names, e->name);
            size_t n = strlen(game ? game : e->name);
            if (n > 36)
                n = 36;
            memcpy(line, game ? game : e->name, n);
            line[n] = 0;
            label_on(menu, 22, top + (int)(i - start) * gap, line, selected ? INK : TEXT,
                     UI_FONT_SMALL, selected ? ACCENT : PANEL);
            format(line, "%5lu", (unsigned long)e->size);
            text(menu, 260, top + (i - start) * gap, line, ink);
        }
    }
    menu->text_background_set = 0;
    if (!count)
        tool_line(menu, tabs ? 3 : 2,
                  (menu->tools == TOOL_FAVORITES || menu->tools == TOOL_RECENT)
                      ? "No games listed."
                      : (menu->tools == TOOL_BACKUP_DETAILS && !menu->backup_slot_valid
                             ? "Empty or invalid backup slot."
                             : "No saves to display."),
                  MUTED);

    /* Below the list: NVRAM use, or where in the list we are. */
    if (menu->tools == TOOL_SAVES) {
        format(line, "%lu used %lu free", menu->nvram_info.used_bytes, menu->nvram_info.free_bytes);
        text(menu, 298 - ui_font_width(line, UI_FONT_SMALL), 66, line, MUTED);
        if (count)
            format(line, "%lu files  File: %.31s", (unsigned long)count,
                   archive->entries[menu->tool_selected].name);
        else
            format(line, "0 files");
        text(menu, 22, 188, line, MUTED);
        return;
    }
    if (menu->tools == TOOL_SLOTS && menu->slot_filtered) {
        format(line, "%lu matches / %lu-%lu", menu->slot_results.total,
               menu->slot_results.count ? menu->slot_results.offset + 1 : 0,
               menu->slot_results.offset + (unsigned long)menu->slot_results.count);
        if (menu->slot_results.count) {
            /* Why the selected slot matched. */
            const char *hint = menu->slot_results.rows[menu->tool_selected].hint;
            scroll_label(menu, 22, 198, 276, hint, strlen(hint), UI_FONT_SMALL, MUTED, PANEL, 0);
        }
    } else if (menu->tools == TOOL_SLOTS)
        format(line, "Slots %lu-%lu / L R:page", menu->slot_page * 8 + 1,
               menu->slot_page * 8 + (slot_count(menu) > 8 ? 8 : (unsigned long)slot_count(menu)));
    else
        format(line, "%lu / %lu", (unsigned long)(count ? menu->tool_selected + 1 : 0),
               (unsigned long)count);
    label(menu, 22, 188, line, OSD_GREEN, UI_FONT_SMALL);
}

/* Show / hide categories: each category and whether it is shown. */
static void draw_categories(Menu *menu)
{
    size_t count = menu_categories_count(), start = menu->tool_selected / 8 * 8, i;
    for (i = start; i < count && i < start + 8; ++i) {
        int y = 82 + (int)(i - start) * 13, selected = i == menu->tool_selected;
        Color ink = tool_row(menu, y, selected);
        scroll_label(menu, 24, y, 222, menu_category_name(i), strlen(menu_category_name(i)),
                     UI_FONT_SMALL, ink, selected ? ACCENT : PANEL, selected);
        label_on(menu, 256, y, menu_category_hidden(i) ? "HIDE" : "SHOW", ink, UI_FONT_SMALL,
                 selected ? ACCENT : PANEL);
    }
    label(menu, 24, 194, "A toggles and saves immediately.", MUTED, UI_FONT_SMALL);
}

/* Settings: two pages of rows, each with its value at the right. */
static void draw_settings(Menu *menu)
{
    static const char *names[SETTING_ROWS] = {"Screen margin",
                                              "Large game titles",
                                              "Folder workaround",
                                              "Show covers",
                                              "Video previews",
                                              "Remember position",
                                              "Recent games",
                                              "Preview sound",
                                              "Categories...",
                                              "Restore display defaults",
                                              "Discard unsaved changes",
                                              "Save changes to USB"};
    char line[80];
    size_t start = menu->tool_selected / 9 * 9, i;
    for (i = start; i < SETTING_ROWS && i < start + 9; ++i) {
        const char *value = NULL;
        int selected = i == menu->tool_selected, y = 82 + (int)(i - start) * 12;
        int flag = -1; /* for ON/OFF settings, the setting */
        Color ink;
        switch (i) {
        case SETTING_MARGIN:
            format(line, "%d", menu->config.screen_margin);
            value = line;
            break;
        case SETTING_LARGE_TITLES:
            flag = menu->config.large_titles;
            break;
        case SETTING_FOLDER_WORKAROUND:
            value = workaround_name(menu->settings_folder_workaround);
            break;
        case SETTING_COVERS:
            flag = menu->settings_usb_covers;
            break;
        case SETTING_VIDEO_PREVIEWS:
            flag = menu->settings_video_previews;
            break;
        case SETTING_REMEMBER_POSITION:
            flag = menu->settings_remember_position;
            break;
        case SETTING_RECENT_GAMES:
            flag = menu->settings_recent_games;
            break;
        case SETTING_PREVIEW_SOUND:
            flag = menu->config.preview_sound;
            break;
        }
        if (flag >= 0)
            value = flag ? "ON" : "OFF";
        ink = tool_row(menu, y, selected);
        label(menu, 24, y, names[i], selected ? INK : TEXT, UI_FONT_SMALL);
        /* Values: ON lit green, OFF dim, anything else TV orange.
         * Right-aligned in screen pixels, whatever the margin. */
        if (value)
            label(menu,
                  298 - ui_font_width(value, UI_FONT_SMALL) * 320 /
                            (320 - 2 * menu->config.screen_margin),
                  y, value, selected ? ink : (flag > 0 ? ACCENT : (flag == 0 ? DIM : TV_ORANGE)),
                  UI_FONT_SMALL);
    }
    menu->text_background_set = 0;
    label(menu, 24, 194, "Sound, display, hiding apply now. C saves.", MUTED, UI_FONT_SMALL);
    label(menu, 280, 64, start ? "2/2" : "1/2", OSD_GREEN, UI_FONT_SMALL);
}

/* Search results: six names a page, and the selected one's folder below. */
static void draw_search_results(Menu *menu)
{
    char line[80];
    size_t count = menu->search_count, start = menu->tool_selected / 6 * 6, i;
    format(line, "%lu matches for: %.24s", (unsigned long)count,
           *menu->find_query ? menu->find_query : "All games");
    label(menu, 22, 82, line, MUTED, UI_FONT_SMALL);
    for (i = start; i < count && i < start + 6; ++i) {
        const char *path = menu->catalog.paths[menu->search_matches[i]];
        const char *name = strrchr(path, '/') + 1;
        int y = 99 + (int)(i - start) * 13, selected = i == menu->tool_selected;
        tool_row(menu, y, selected);
        game_label(menu, 22, y, 276, name, 0, UI_FONT_SMALL, selected ? INK : TEXT,
                   selected ? ACCENT : PANEL, selected);
    }
    menu->text_background_set = 0;
    if (!count)
        label(menu, 22, 112, "No games matched. P to edit.", TEXT, UI_FONT_SMALL);
    else {
        /* The folder, cut from the left so its end (nearest the game)
         * shows. */
        const char *path = menu->catalog.paths[menu->search_matches[menu->tool_selected]];
        size_t length = (size_t)(strrchr(path, '/') - path), skip = 0;
        int room = 280 * (320 - 2 * menu->config.screen_margin) / 320;
        while (skip < length &&
               ui_font_fit(path + skip, length - skip, room, UI_FONT_SMALL) < length - skip)
            ++skip;
        length -= skip;
        if (length > 70)
            length = 70;
        memcpy(line, path + skip, length);
        line[length] = 0;
        label(menu, 22, 185, line, MUTED, UI_FONT_SMALL);
    }
}

/* Find backup slots: the ways to look. */
static void draw_slot_find(Menu *menu)
{
    static const char *options[4] = {"Show occupied slots", "Search games / slot names",
                                     "Go to slot number", "Show all slot numbers"};
    size_t i;
    for (i = 0; i < 4; ++i) {
        int y = 90 + (int)i * 19, selected = (int)i == menu->slot_find_option;
        tool_row(menu, y, selected);
        label_on(menu, 22, y, options[i], selected ? INK : TEXT, UI_FONT_SMALL,
                 selected ? ACCENT : PANEL);
    }
    tool_line(menu, 8, "Older copies: refresh in desktop app.", MUTED);
}

/* Go to slot: the number typed so far, and a row of digit keys. */
static void draw_slot_jump(Menu *menu)
{
    char line[80];
    size_t i;
    format(line, "Go to slot: %s_", menu->slot_jump);
    tool_line(menu, 0, line, TEXT);
    tool_line(menu, 1, "1-999999999 / allocated when used", MUTED);
    for (i = 0; i < 10; ++i) {
        char key[2];
        key[0] = (char)('0' + i);
        key[1] = 0;
        if (i == (size_t)menu->find_key)
            rectangle(menu, 22 + i * 27, 111, 19, 13, ACCENT);
        label_on(menu, 27 + i * 27, 113, key, i == (size_t)menu->find_key ? INK : TEXT,
                 UI_FONT_SMALL, i == (size_t)menu->find_key ? ACCENT : PANEL);
    }
    tool_line(menu, 7, "P:Go X:Erase C:Clear B:Cancel", MUTED);
}

/* The screens with the on-screen keyboard: Find, a slot search and naming
 * a slot. */
static void draw_keyboard(Menu *menu)
{
    char line[80];
    size_t i;
    if (menu->tools == TOOL_SLOT_NAME)
        format(line, "Name: %s_", menu->slot_label_edit);
    else
        format(line, "Find: %s_",
               menu->tools == TOOL_SLOT_QUERY ? menu->slot_query : menu->find_query);
    tool_line(menu, 0, line, TEXT);
    tool_line(menu, 1,
              menu->tools == TOOL_SLOT_NAME    ? "Blank name restores slot filename."
              : menu->tools == TOOL_SLOT_QUERY ? "Game, save filename or slot label."
                                               : "Drive-wide search / PC index.",
              MUTED);
    for (i = 0; i < FIND_KEYS; ++i) {
        int x = 27 + (int)(i % FIND_KEY_COLUMNS) * 27, y = 113 + (int)(i / FIND_KEY_COLUMNS) * 17;
        char key[3];
        key[0] = find_keys[i];
        key[1] = 0;
        if (key[0] == ' ')
            strcpy(key, "SP");
        if (i == (size_t)menu->find_key) {
            rectangle(menu, x - 5, y - 2, 19, 13, ACCENT);
            label_on(menu, x, y, key, INK, UI_FONT_SMALL, ACCENT);
        } else
            text(menu, x, y, key, TEXT);
    }
    text(menu, 24, 189,
         menu->tools == TOOL_SLOT_NAME ? "P:Save X:Erase C:Clear" : "P:Search X:Erase C:Clear",
         MUTED);
}

/* Help & controls: two pages. */
static void draw_help(Menu *menu)
{
    static const char *help[] = {"A: Open folder / launch game",     "B: Parent folder / cancel",
                                 "Up/Down: Select (hold to repeat)", "Left/Right or L/R: Page",
                                 "C: Add selected disc to queue",    "Tap P:Queue / Hold P:Help",
                                 "X: Cover/video or refresh",        "L+R together: Open tools",
                                 "Hold X at boot: Skip USB extras",  "C+Up or list ends: Alphabet"};
    static const char *more[] = {
        "Queue: X removes one disc",      "Queue: C toggles move mode",
        "Move mode: Up/Down reorders",    "Hold C, press X: Clear queue",
        "Hold C + Left/Right: Letter",    "Hold C + A: Folder / one disc",
        "Favorites: A opens game folder", "Favorites: C adds, X removes",
        "Config: C saves, X discards",    "ABC: Left/Right A:Jump B:Cancel"};
    size_t i;
    for (i = 0; i < 10; ++i)
        tool_line(menu, (int)i, menu->tool_selected ? more[i] : help[i], i < 8 ? TEXT : MUTED);
}

/* Diagnostics page 1: the ODE's identity. */
static void draw_diagnostics_ode(Menu *menu)
{
    char line[80];
    const unsigned char *reply = menu->ode.identity_reply;
    format(line, "Menu %s / XBUS unit 0", MENU_VERSION);
    tool_line(menu, 0, line, TEXT);
    format(line, "ODE ID: %.6s", menu->ode.identity[0] ? menu->ode.identity : "------");
    tool_line(menu, 1, line, TEXT);
    format(line, "Reported revision: %u.%u.%u", menu->ode.revision[0], menu->ode.revision[1],
           menu->ode.revision[2]);
    tool_line(menu, 2, line, TEXT);
    format(line, "Boot flags: %02x Status: %02x", reply[1], reply[11]);
    tool_line(menu, 3, line, MUTED);
    tool_line(menu, 4, "Identity bytes (93 reply):", MUTED);
    format(line, "%02x %02x %02x %02x %02x %02x", reply[0], reply[1], reply[2], reply[3], reply[4],
           reply[5]);
    tool_line(menu, 5, line, TEXT);
    format(line, "%02x %02x %02x %02x %02x %02x", reply[6], reply[7], reply[8], reply[9], reply[10],
           reply[11]);
    tool_line(menu, 6, line, TEXT);
    tool_line(menu, 8, "ID/revision can be hard-coded.", AMBER);
    tool_line(menu, 9, "Exact firmware build is unknown.", AMBER);
}

/* Diagnostics page 2: settings in use and this session's results. */
static void draw_diagnostics_session(Menu *menu)
{
    char line[80];
    int row = 4;
    format(line, "Folder workaround: %s (%s)", workaround_name(menu->config.folder_workaround),
           folder_workaround(menu) ? "in use" : "not used");
    tool_line(menu, 0, line, TEXT);
    format(line, "Config result: %d (0 = loaded)", menu->config_result);
    tool_line(menu, 1, line, TEXT);
    format(line, "USB covers: %s", menu->skip_usb_art ? "disabled" : "enabled");
    tool_line(menu, 2, line, TEXT);
    format(line, "Cover index: %lu entries", (unsigned long)menu->usb_art.count);
    tool_line(menu, 3, line, MUTED);
    if (menu->usb_tested) {
        format(line, "USB write check: %s", menu->usb_tested > 0 ? "PASS" : "FAIL");
        tool_line(menu, row++, line, TEXT);
    }
    format(line, "Backup: %s",
           menu->backup_result > 0 ? "verified" : (menu->backup_result < 0 ? "failed" : "not run"));
    tool_line(menu, row++, line, TEXT);
    format(line, "Restore: %s",
           menu->restore_result > 0 ? "verified"
                                    : (menu->restore_result < 0 ? "failed" : "not run"));
    tool_line(menu, row++, line, TEXT);
    format(line, "Queued discs: %lu / 16", (unsigned long)menu->playlist.count);
    tool_line(menu, row++, line, MUTED);
    format(line, "NVRAM error: %08lx", (unsigned long)menu->nvram_info.last_error & 0xffffffffUL);
    tool_line(menu, row++, line, MUTED);
    tool_line(menu, row, "Results apply to this session.", MUTED);
}

/* Diagnostics page 3: the last ODE command and any fault. */
static void draw_diagnostics_transport(Menu *menu)
{
    char line[80];
    format(line, "Commands sent: %lu", menu->bus.commands);
    tool_line(menu, 0, line, TEXT);
    format(line, "Last command: %02x (%lu bytes)", menu->bus.last_command,
           (unsigned long)menu->bus.last_length);
    tool_line(menu, 1, line, TEXT);
    format(line, "Reply: %02x %02x %02x %02x", menu->bus.last_reply[0], menu->bus.last_reply[1],
           menu->bus.last_reply[2], menu->bus.last_reply[3]);
    tool_line(menu, 2, line, TEXT);
    format(line, "XBUS error: %08lx", (unsigned long)menu->bus.last_error & 0xffffffffUL);
    tool_line(menu, 3, line, MUTED);
    format(line, "Transport timed out: %s", menu->bus.timed_out ? "YES" : "no");
    tool_line(menu, 4, line, MUTED);
    format(line, "Navigation blocked: %s", menu->ode.navigation_failed ? "YES" : "no");
    tool_line(menu, 5, line, MUTED);
    format(line, "Last fault: cmd %02x status %02x", menu->bus.failed_command,
           menu->bus.failed_status);
    tool_line(menu, 6, line, MUTED);
    format(line, "Fault error: %08lx", (unsigned long)menu->bus.failure_error & 0xffffffffUL);
    tool_line(menu, 7, line, MUTED);
    format(line, "Cover: %lums / %lu cmds%s", menu->cover_load_ms, menu->cover_commands,
           menu->cover_cached ? " RAM" : "");
    tool_line(menu, 8, line, MUTED);
    format(line, "Diagnostic export: %d", menu->diagnostics_result);
    tool_line(menu, 9, line, MUTED);
}

/* Diagnostics page 4: how the last preview load went. */
static void draw_diagnostics_preview(Menu *menu)
{
    char line[80];
    tool_line(menu, 0, "Last preview load", TEXT);
    format(line, "Result: %d  Total: %lums", menu->preview_result, menu->preview_load_ms);
    tool_line(menu, 1, line, TEXT);
    format(line, "Read + CRC: %lums", menu->preview_read_ms);
    tool_line(menu, 2, line, MUTED);
    format(line, "Checks / first frame: %lums", menu->preview_check_ms);
    tool_line(menu, 3, line, MUTED);
    format(line, "Folder navigation: %lums", menu->preview_navigation_ms);
    tool_line(menu, 4, line, MUTED);
    format(line, "Audio / display setup: %lums", menu->preview_setup_ms);
    tool_line(menu, 5, line, MUTED);
    format(line, "Payload: %lu KiB", (menu->preview_bytes + 1023) / 1024);
    tool_line(menu, 6, line, MUTED);
    format(line, "Frames: %u at %u fps", menu->preview_frames, menu->preview_fps);
    tool_line(menu, 7, line, MUTED);
    format(line, "Commands: %lu", menu->preview_commands);
    tool_line(menu, 8, line, MUTED);
    if (menu->preview_result == -4)
        tool_line(menu, 9, "Not enough preview memory.", AMBER);
    else if (menu->preview_result == -5)
        tool_line(menu, 9, "Canceled by controller input.", MUTED);
    else if (menu->preview_result < 0)
        tool_line(menu, 9, "Preview file / read failed.", AMBER);
    else
        tool_line(menu, 9, "Playback checks frames as shown.", MUTED);
}

/* Diagnostics page 5: the last file asked for and the media indexes. */
static void draw_diagnostics_files(Menu *menu)
{
    char line[80];
    size_t i;
    tool_line(menu, 0, "Last requested USB file:", TEXT);
    for (i = 0; i < 4; ++i) {
        format(line, "%.32s",
               strlen(menu->ode.file_path) > i * 32 ? menu->ode.file_path + i * 32 : "");
        tool_line(menu, (int)i + 1, line, TEXT);
    }
    tool_line(menu, 5, "Browser folder:", MUTED);
    format(line, "%.40s", menu->breadcrumb);
    tool_line(menu, 6, line, TEXT);
    format(line, "Indexes: %lu covers / %lu clips", (unsigned long)menu->usb_art.count,
           (unsigned long)menu->usb_previews.count);
    tool_line(menu, 7, line, MUTED);
    format(line, "Game metadata: %lu entries", (unsigned long)menu->metadata.count);
    tool_line(menu, 8, line, MUTED);
    tool_line(menu, 9, "Retained after a USB timeout.", MUTED);
}

/* Diagnostics: the page chosen with Left/Right (menu->tool_selected). */
static void draw_diagnostics(Menu *menu)
{
    switch (menu->tool_selected) {
    case 0:
        draw_diagnostics_ode(menu);
        break;
    case 1:
        draw_diagnostics_session(menu);
        break;
    case 2:
        draw_diagnostics_transport(menu);
        break;
    case 3:
        draw_diagnostics_preview(menu);
        break;
    default:
        draw_diagnostics_files(menu);
        break;
    }
}

/*
 * Comparing two backups: the list of saves and how each differs, or with
 * TOOL_COMPARE_BYTES, the bytes that differ in the selected save, a page at
 * a time from menu->compare_offset.
 */
static void draw_compare(Menu *menu)
{
    char line[80];
    SaveDifference diff;
    size_t total = save_archive_diff_count(&menu->compare_archive, &menu->backup), i;
    {
        /* "slot01 > slot02": the slot names without "saves/" or ".3ds". */
        char a[40], b[40];
        slot_path(a, menu->compare_slot);
        slot_path(b, menu->backup_slot);
        a[strlen(a) - 4] = 0;
        b[strlen(b) - 4] = 0;
        format(line, "%.13s > %.13s", a + 6, b + 6);
        tool_line(menu, 0, line, MUTED);
    }
    if (menu->tools == TOOL_COMPARE) {
        static const char *states[] = {"SAME", "CHANGED", "ADDED", "REMOVED"};
        size_t start = menu->tool_selected / 8 * 8;
        for (i = start; i < total && i < start + 8; ++i)
            if (!save_archive_diff(&menu->compare_archive, &menu->backup, i, &diff)) {
                const SaveEntry *e = diff.after < menu->backup.count
                                         ? &menu->backup.entries[diff.after]
                                         : &menu->compare_archive.entries[diff.before];
                Color ink = tool_row(menu, 100 + (int)(i - start) * 11, i == menu->tool_selected);
                format(line, "%-7s %.25s", states[diff.state], e->name);
                text(menu, 22, 100 + (i - start) * 11, line, ink);
            }
        if (!total)
            tool_line(menu, 3, "Both slots are empty.", MUTED);
        else if (!save_archive_diff(&menu->compare_archive, &menu->backup, menu->tool_selected,
                                    &diff)) {
            format(line, "%lu byte(s) differ%s", (unsigned long)diff.changed_bytes,
                   diff.metadata_changed ? " / metadata" : "");
            tool_line(menu, 9, line, MUTED);
        }
    } else if (!save_archive_diff(&menu->compare_archive, &menu->backup, menu->tool_selected,
                                  &diff)) {
        /* Each differing byte; a save missing on one side shows "--". */
        const SaveEntry *a = diff.before < menu->compare_archive.count
                                 ? &menu->compare_archive.entries[diff.before]
                                 : NULL;
        const SaveEntry *b =
            diff.after < menu->backup.count ? &menu->backup.entries[diff.after] : NULL;
        size_t at, n = 0, row = 0;
        tool_line(menu, 1, a ? a->name : b->name, TEXT);
        tool_line(menu, 2, "Offset     Before  After", MUTED);
        for (at = 0; at < diff.length && row < 7; ++at)
            if (!a || !b || at >= a->size || at >= b->size || a->data[at] != b->data[at]) {
                char old[3], next[3];
                if (n++ < menu->compare_offset)
                    continue;
                if (a && at < a->size)
                    format(old, "%02x", a->data[at]);
                else
                    strcpy(old, "--");
                if (b && at < b->size)
                    format(next, "%02x", b->data[at]);
                else
                    strcpy(next, "--");
                format(line, "%04lx       %s      %s", (unsigned long)at, old, next);
                tool_line(menu, 3 + (int)row++, line, TEXT);
            }
        if (!diff.changed_bytes)
            tool_line(menu, 4,
                      diff.metadata_changed ? "Only filename/type differs."
                                            : "Save bytes are identical.",
                      MUTED);
    }
}

/* Confirming a backup: of all of NVRAM, or of one save (backing it up,
 * copying it, or saving it after hex edits) merged into the slot. */
static void draw_confirm_backup(Menu *menu)
{
    char line[80];
    slot_path(line, menu->backup_slot);
    tool_line(menu, 0, line, TEXT);
    if (menu->backup_merge) {
        const SaveEntry *entry = backup_entry(menu);
        tool_line(menu, 2,
                  menu->hex_archive.bytes     ? "Save edited file:"
                  : menu->copy_original.bytes ? "Copy this save:"
                                              : "Back up this save:",
                  TEXT);
        tool_line(menu, 3, entry->name, TEXT);
        tool_line(menu, 5, "Other slot entries stay unchanged.", ACCENT);
        format(line, "Slot will contain %lu save(s).", (unsigned long)menu->pending.count);
        tool_line(menu, 7, line, MUTED);
    } else {
        format(line, "Back up %lu save file(s)?", (unsigned long)menu->pending.count);
        tool_line(menu, 2, line, TEXT);
        if (menu->backup_slot_valid)
            format(line, "Replaces %lu existing save(s).", (unsigned long)menu->backup.count);
        else
            strcpy(line, "Replaces this USB backup slot.");
        tool_line(menu, 4, line, AMBER);
        tool_line(menu, 6, "Internal saves stay unchanged.", ACCENT);
    }
}

/* Confirming a restore of one save to NVRAM. */
static void draw_confirm_restore(Menu *menu)
{
    char line[80];
    const SaveEntry *e = &menu->backup.entries[menu->save_selected];
    tool_line(menu, 0, e->name, TEXT);
    format(line, "%lu bytes / type %08lx", (unsigned long)e->size, e->type);
    tool_line(menu, 1, line, MUTED);
    tool_line(menu, 3, "Restore this save to NVRAM?", AMBER);
    tool_line(menu, 4, "Replaces the matching save.", AMBER);
    tool_line(menu, 6, "First: verified recovery backup.", TEXT);
    tool_line(menu, 7,
              menu->backup_slot == SAVE_RECOVERY_SLOT ? "Replaces USB recovery2.3ds."
                                                      : "Replaces USB recovery.3ds.",
              MUTED);
    tool_line(menu, 9, "Keep power on until finished.", AMBER);
}

/* The current screen's controls, shown under the panel unless there is a
 * message to show instead. */
static const char *tools_controls(Menu *menu)
{
    switch (menu->tools) {
    case TOOL_GAME_DETAILS:
        return "Left/Right:Page B:Back";
    case TOOL_FAVORITES:
        return menu->favorites_move ? "Up/Down:Move A/R:Save B:Cancel"
                                    : "A:Open C:Add X:Remove B:Back";
    case TOOL_RESULTS:
        return "A:Open P:Edit X:Reload B:Back";
    case TOOL_SLOT_JUMP:
        return "Left/Right:Digit A:Add P:Go";
    case TOOL_SLOT_NAME:
    case TOOL_SLOT_QUERY:
    case TOOL_FIND:
        return "Arrows:Choose A:Key B:Cancel";
    case TOOL_COMPARE:
        return "A:Byte differences B:Back";
    case TOOL_COMPARE_BYTES:
        return "Left/Right:Page B:Back";
    case TOOL_SLOTS:
        if (menu->backup_mode)
            break;
        return menu->compare_choosing ? "A:Compare with this slot B:Cancel"
                                      : "A:Open C:Name X:Compare";
    case TOOL_CATEGORIES:
        return "A:Show/Hide B:Settings";
    case TOOL_CONFIG:
        return "Left/Right:Change C:Save B:Back";
    case TOOL_BACKUP_DETAILS:
        return "A:Continue C:Old/New B:Back";
    case TOOL_SAVES:
        return "A:Back up one C:All X:Refresh";
    case TOOL_ARCHIVE:
        return "A:Restore C:Hex X:Cheats B:Back";
    case TOOL_CHEATS: {
        const SaveCheatProfile *cheats =
            save_cheats_find(&menu->hex_archive.entries[menu->save_selected]);
        return cheats && menu->cheat_selected < cheats->preset_count ? "A:Apply preset"
                                                                     : "Left/Right:Change value";
    }
    case TOOL_HEX:
        return menu->hex_editing ? "Arrows:Value A:Apply B:Cancel" : "Arrows:Move A:Edit C:Save as";
    case TOOL_HEX_DISCARD:
        return "A:Discard B:Keep editing";
    case TOOL_CONFIRM_BACKUP:
    case TOOL_CONFIRM_RESTORE:
        return "A:Confirm B:Cancel";
    case TOOL_DIAG:
        return "C:Export X:Test return B:Back";
    case TOOL_HELP:
        return "B:Back";
    case TOOL_RECENT:
        return "A:Open folder P:Reload B:Back";
    }
    return "A:Select B:Back";
}

/* The second row of controls: the screen's extra buttons. */
static const char *tools_more_controls(Menu *menu)
{
    switch (menu->tools) {
    case TOOL_SLOTS:
        return "L/R:Slot page P:Find slots";
    case TOOL_ARCHIVE:
        return "P:Copy to slot L+R:Library";
    case TOOL_CHEATS:
        return "C:Save as X:Hex B:Back";
    case TOOL_HEX:
        return "L/R:Page X:Cheats B:Back";
    case TOOL_FAVORITES:
        return "R:Move order P:Reload";
    case TOOL_DIAG:
    case TOOL_HELP:
        return "Left/Right:More pages";
    }
    return save_manager_tab(menu) ? "Left/Right:View B:Back" : "Hold P:Help L+R:Library";
}

/*
 * Draws the Tools screen menu->tools: the TV's on-screen menu, with the
 * screen's title and content in a panel and its controls below.
 */
static void draw_tools(Menu *menu)
{
    /* The home screen has no scrolling or live diagnostics. Retain its two
     * buffers until a selection, settings marker, message or layout changes. */
    if (menu->tools == TOOL_HOME) {
        const char *c = menu->tool_message;
        unsigned long key = (2166136261UL ^ (unsigned long)menu->tool_selected) * 16777619UL;
        key = (key ^ (unsigned long)menu->config.screen_margin) * 16777619UL;
        key = (key ^ (unsigned long)config_dirty(menu)) * 16777619UL;
        key = (key ^ (unsigned long)menu->logo) * 16777619UL;
        key = (key ^ (unsigned long)(c != NULL)) * 16777619UL;
        if (c)
            for (; *c; ++c)
                key = (key ^ (unsigned char)*c) * 16777619UL;
        if (!menu->tools_home_drawn || menu->tools_home_hash != key) {
            menu->tools_home_hash = key;
            menu->tools_home_drawn = 0;
        }
        if (menu->tools_home_drawn >= 2)
            return;
        ++menu->tools_home_drawn;
    }

    /* The frame: heading, panel and title. */
    menu->text_cursor = 0;
    menu->text_style = UI_FONT_SMALL;
    menu->screen.sc_curScreen ^= 1;
    rectangle(menu, 0, 0, 320, 240, BG);
    brand(menu, menu->tools == TOOL_GAME_DETAILS ? "Game info" : "Tools");
    /* The TV's own setup menu: an on-screen-display tag, as CH is in the library. */
    label(menu,
          308 - (ui_font_width("MENU", UI_FONT_HEADING) * 320 + 319 -
                 2 * menu->config.screen_margin) /
                    (320 - 2 * menu->config.screen_margin),
          9, "MENU", OSD_GREEN, UI_FONT_HEADING);
    rectangle(menu, 12, 58, 296, 148, PANEL);
    label(menu, 22, 62, tools_title(menu), TEXT, UI_FONT_NORMAL);
    rectangle(menu, 22, 77, 39, 2, TV_ORANGE);
    rectangle(menu, 63, 77, 17, 2, ACCENT);

    /* The screen's content. */
    switch (menu->tools) {
    case TOOL_HOME:
        draw_tools_home(menu);
        break;
    case TOOL_CHEATS:
        draw_cheats(menu);
        break;
    case TOOL_HEX:
        draw_hex(menu);
        break;
    case TOOL_HEX_DISCARD:
        tool_line(menu, 2, "Discard unsaved hex edits?", AMBER);
        tool_line(menu, 4, "The USB backup is unchanged.", TEXT);
        break;
    case TOOL_GAME_DETAILS:
        draw_game_details(menu);
        break;
    case TOOL_SAVES:
    case TOOL_ARCHIVE:
    case TOOL_SLOTS:
    case TOOL_BACKUP_DETAILS:
    case TOOL_FAVORITES:
    case TOOL_RECENT:
        draw_tool_list(menu);
        break;
    case TOOL_CATEGORIES:
        draw_categories(menu);
        break;
    case TOOL_CONFIG:
        draw_settings(menu);
        break;
    case TOOL_RESULTS:
        draw_search_results(menu);
        break;
    case TOOL_SLOT_FIND:
        draw_slot_find(menu);
        break;
    case TOOL_SLOT_JUMP:
        draw_slot_jump(menu);
        break;
    case TOOL_FIND:
    case TOOL_SLOT_NAME:
    case TOOL_SLOT_QUERY:
        draw_keyboard(menu);
        break;
    case TOOL_HELP:
        draw_help(menu);
        break;
    case TOOL_DIAG:
        draw_diagnostics(menu);
        break;
    case TOOL_COMPARE:
    case TOOL_COMPARE_BYTES:
        draw_compare(menu);
        break;
    case TOOL_CONFIRM_BACKUP:
        draw_confirm_backup(menu);
        break;
    case TOOL_CONFIRM_RESTORE:
        draw_confirm_restore(menu);
        break;
    }

    /* The controls, or a message in their place. */
    if (menu->tool_message)
        label(menu, 16, 211, menu->tool_message, AMBER, UI_FONT_SMALL);
    else
        control_badges(menu, 16, 210, tools_controls(menu), 1);
    control_badges(menu, 16, 225, tools_more_controls(menu), 0);
    DisplayScreen(menu->screen.sc_Screens[menu->screen.sc_curScreen], 0);
}

/* ==== Saves on the drive ==== */

/* A save operation at the drive's root, through art_at_root. */
typedef struct SaveRootOperation {
    Menu *menu;
    int action; /* SAVE_LOAD, SAVE_STORE, ... */
    const char *path;
    SaveArchive *archive;
} SaveRootOperation;

/* Shows how much a save operation has moved so far. */
static void save_progress(void *context, size_t bytes)
{
    Menu *menu = (Menu *)context;
    menu->read_count += bytes;
    format(menu->status, "USB transfer / verify: %lu KiB",
           (unsigned long)((menu->read_count + 1023) / 1024));
    menu->tool_message = menu->status;
    draw(menu);
}

/*
 * SAVE_READ_LABELS and SAVE_WRITE_LABELS: reads the current slot page's
 * labels into the menu, or writes the edited labels, if the file still
 * holds what it did when editing began. A written label also goes into
 * the slot catalog.
 */
static int slot_labels_at_root(SaveRootOperation *op)
{
    Menu *menu = op->menu;
    Ode *ode = &menu->ode;
    char text[SAVE_SLOT_LABEL_FILE_BYTES], path[48];
    size_t length;
    SaveSlotLabels current;
    int result;
    slot_labels_path(menu, path);
    result = usb_file_text(ode, ode_bus_read_buffer, path, text, sizeof(text), &length);
    if (!result)
        result = save_slot_labels_parse(&current, text, length);
    if (result)
        return result;
    if (op->action == SAVE_READ_LABELS) {
        menu->slot_labels = current;
        menu->slot_labels_valid = 1;
        menu->slot_labels_page = menu->slot_page;
        return 0;
    }
    if (memcmp(&current, &menu->slot_labels_before, sizeof(current)) ||
        save_slot_labels_format(&menu->slot_labels, text, sizeof(text)) < 0)
        return -1;
    result = usb_file_text_write(ode, ode_bus_read_buffer, path, text);
    if (!result &&
        save_catalog_relabel(ode, ode_bus_read_buffer, menu->slot_name_slot, menu->slot_label_edit))
        menu->slot_catalog_stale = 1;
    return result;
}

/* After a slot is written: records its saves (and its label) in the slot
 * catalog, which slot searches use. If that fails, the catalog is marked
 * stale, for the desktop app to rebuild. */
static void update_slot_catalog(SaveRootOperation *op)
{
    Ode *ode = &op->menu->ode;
    unsigned int slot;
    char path[48], text[SAVE_SLOT_LABEL_FILE_BYTES];
    size_t length;
    SaveSlotLabels labels;
    const char *label = "";
    if (save_catalog_slot_from_path(op->path, &slot) < 0) {
        op->menu->slot_catalog_stale = 1;
        return;
    }
    if (slot < SAVE_SLOT_LIMIT) {
        labels_path(path, (unsigned long)slot / 8);
        if (!usb_file_text(ode, ode_bus_read_buffer, path, text, sizeof(text), &length) &&
            !save_slot_labels_parse(&labels, text, length))
            label = labels.labels[slot % 8];
    }
    if (save_catalog_update(ode, ode_bus_read_buffer, slot, op->archive, label))
        op->menu->slot_catalog_stale = 1;
}

/* Does a SaveRootOperation, at the root, showing progress as it goes. */
static int save_root_callback(void *context)
{
    SaveRootOperation *op = (SaveRootOperation *)context;
    Menu *menu = op->menu;
    Ode *ode = &menu->ode;
    OdeProgress previous_progress = ode->progress;
    void *previous_context = ode->progress_context;
    int result;
    menu->read_count = 0;
    if (op->action == SAVE_STORE || op->action == SAVE_STORE_CHECKED ||
        op->action == SAVE_WRITE_LABELS)
        menu->slot_catalog_stale = 0;
    ode->progress = save_progress;
    ode->progress_context = menu;
    switch (op->action) {
    case SAVE_SEARCH_CATALOG:
        result = save_catalog_search(ode, ode_bus_read_buffer, &menu->save_names, menu->slot_query,
                                     !menu->backup_mode, &menu->slot_results);
        break;
    case SAVE_READ_LABELS:
    case SAVE_WRITE_LABELS:
        result = slot_labels_at_root(op);
        break;
    case SAVE_SETTINGS: {
        MenuConfig settings;
        settings_values(menu, &settings);
        result = menu_settings_save(ode, ode_bus_read_buffer, &settings);
        break;
    }
    case SAVE_STORE_CHECKED:
        result = save_usb_store_checked(ode, ode_bus_read_buffer, op->path, op->archive,
                                        menu->backup_slot_valid ? &menu->backup : NULL);
        break;
    case SAVE_LOAD_TARGET:
        result = save_usb_load_target(ode, ode_bus_read_buffer, op->path, op->archive);
        break;
    case SAVE_TEST:
        result = save_usb_test(ode, ode_bus_read_buffer);
        break;
    case SAVE_STORE:
        result = save_usb_store(ode, ode_bus_read_buffer, op->path, op->archive);
        break;
    default: /* SAVE_LOAD */
        result = save_usb_load(ode, ode_bus_read_buffer, op->path, op->archive);
        break;
    }
    if (!result && (op->action == SAVE_STORE || op->action == SAVE_STORE_CHECKED))
        update_slot_catalog(op);
    ode->progress = previous_progress;
    ode->progress_context = previous_context;
    return result;
}

/*
 * Does a save operation (SAVE_LOAD, SAVE_STORE, ...) at the drive's root,
 * going there and back from the current folder. `path` names the slot file
 * and `archive` holds or receives the saves, for the operations that use
 * them. Returns the operation's result (0 for success), or -1 if it could
 * not be done; menu->tool_message says what went wrong. A transport failure
 * stops the browser.
 */
static int save_root_operation(Menu *menu, int action, const char *path, SaveArchive *archive)
{
    ArtRouteStep route[MAX_DEPTH];
    SaveRootOperation op;
    unsigned int i;
    int result;
    if (menu->demo || !menu->ready || menu->bus.timed_out || menu->ode.navigation_failed ||
        strcmp(menu->ode.identity, "LOCODE") || menu->config_result == 2) {
        menu->tool_message = "USB save access unavailable.";
        return -1;
    }
    /* A virtual device list has no current filesystem for relative file I/O. */
    if (!menu->depth)
        for (i = 0; i < menu->listing.count; ++i)
            if (!(menu->listing.entries[i]->toc_id & 0xffffffUL)) {
                menu->tool_message = "Enter a USB volume first.";
                return -1;
            }
    for (i = 0; i < menu->depth; ++i) {
        route[i].id = menu->path_ids[i];
        route[i].name = menu->path[i];
    }
    op.menu = menu;
    op.action = action;
    op.path = path;
    op.archive = archive;
    menu->tool_message = action != SAVE_LOAD && action != SAVE_LOAD_TARGET &&
                                 action != SAVE_READ_LABELS && action != SAVE_SEARCH_CATALOG
                             ? "Writing / verifying USB..."
                             : "Reading / validating backup...";
    draw(menu);
    result = art_at_root(&menu->ode, route, menu->depth, &menu->listing, folder_workaround(menu),
                         save_root_callback, &op);
    if (result == -2 || menu->bus.timed_out || menu->ode.navigation_failed) {
        menu->ready = menu->view_valid = 0;
        menu->tool_message = "USB I/O failed. Restart console.";
        return -1;
    }
    menu->tool_message = result < 0 ? action == SAVE_SEARCH_CATALOG
                                          ? "Refresh slot catalog in desktop app."
                                          : "USB failed. Check .ohmy3do/saves."
                                    : NULL;
    return result;
}
/* Reading the files that name saves, through art_at_root. `disc` holds
 * save-names.txt (each game's save file names, from the desktop app) and
 * `learned` saves-learned.txt (names the menu learned itself). */
typedef struct SaveNamesRead {
    Menu *menu;
    char *disc, *learned;
    size_t disc_length, learned_length;
} SaveNamesRead;
/* At the root: loads the cheat database and the current slot page's labels
 * too, as the Save Manager is opening. Missing files are not an error. */
static int save_names_at_root(void *context)
{
    SaveNamesRead *op = (SaveNamesRead *)context;
    Ode *ode = &op->menu->ode;
    save_cheats_usb_load(ode, ode_bus_read_buffer);
    {
        size_t length;
        char path[48];
        slot_labels_path(op->menu, path);
        if (!usb_file_text(ode, ode_bus_read_buffer, path, op->disc, SAVE_SLOT_LABEL_FILE_BYTES,
                           &length) &&
            !save_slot_labels_parse(&op->menu->slot_labels, op->disc, length)) {
            op->menu->slot_labels_valid = 1;
            op->menu->slot_labels_page = op->menu->slot_page;
        }
    }
    if (usb_file_text(ode, ode_bus_read_buffer, "save-names.txt", op->disc, SAVE_NAMES_BYTES,
                      &op->disc_length) < 0)
        op->disc_length = 0;
    if (usb_file_text(ode, ode_bus_read_buffer, "saves-learned.txt", op->learned, SAVE_NAMES_BYTES,
                      &op->learned_length) < 0)
        op->learned_length = 0;
    return 0;
}
/* Game names for NVRAM files, read once per session. Missing names only
 * leave the raw filenames showing. */
static void load_save_names(Menu *menu)
{
    ArtRouteStep route[MAX_DEPTH];
    SaveNamesRead op;
    size_t i;
    int result;
    if (menu->save_names_loaded || menu->demo || !menu->ready || menu->bus.timed_out ||
        menu->ode.navigation_failed || menu->config_result == 2 ||
        strcmp(menu->ode.identity, "LOCODE"))
        return;
    if (!menu->depth)
        for (i = 0; i < menu->listing.count; ++i)
            if (!(menu->listing.entries[i]->toc_id & 0xffffffUL))
                return;
    op.menu = menu;
    op.disc_length = op.learned_length = 0;
    op.disc = (char *)malloc(SAVE_NAMES_BYTES);
    op.learned = (char *)malloc(SAVE_NAMES_BYTES);
    if (op.disc && op.learned) {
        menu->tool_message = "Reading save names...";
        draw(menu);
        current_route(menu, route);
        result = art_at_root(&menu->ode, route, menu->depth, &menu->listing,
                             folder_workaround(menu), save_names_at_root, &op);
        if (result == -2 || menu->bus.timed_out || menu->ode.navigation_failed) {
            menu->ready = menu->view_valid = 0;
            menu->message = "USB I/O failed. Restart console.";
        } else if (!save_names_parse(&menu->save_names, op.disc, op.disc_length, op.learned,
                                     op.learned_length))
            menu->save_names_loaded = 1;
    }
    free(op.disc);
    free(op.learned);
}
/*
 * At startup, after a game was played: compares NVRAM with the snapshot
 * taken at launch (save-snapshot.txt) to name the files that game created
 * or changed, adds them to saves-learned.txt and clears the snapshot.
 * Runs at the drive's root, before any folder is entered. Failures only
 * mean those saves stay unnamed.
 */
static void learn_saves(Menu *menu)
{
    char *snapshot = (char *)malloc(SAVE_SNAPSHOT_BYTES), *disc = (char *)malloc(SAVE_NAMES_BYTES),
         *learned = (char *)malloc(SAVE_NAMES_BYTES);
    size_t length, disc_length, learned_length;
    SaveArchive *nvram = (SaveArchive *)malloc(sizeof(SaveArchive));
    NvramInfo info;
    SaveNames names;
    Ode *ode = &menu->ode;
    if (nvram)
        memset(nvram, 0, sizeof(*nvram));
    if (!nvram || !snapshot || !disc || !learned ||
        usb_file_text(ode, ode_bus_read_buffer, "save-snapshot.txt", snapshot, SAVE_SNAPSHOT_BYTES,
                      &length) < 0 ||
        length < 16)
        goto done;
    if (nvram_snapshot(nvram, &info) < 0 ||
        usb_file_text(ode, ode_bus_read_buffer, "saves-learned.txt", learned, SAVE_NAMES_BYTES,
                      &learned_length) < 0)
        goto done;
    save_cheats_usb_load(ode, ode_bus_read_buffer);
    if (usb_file_text(ode, ode_bus_read_buffer, "save-names.txt", disc, SAVE_NAMES_BYTES,
                      &disc_length) < 0)
        disc_length = 0;
    if (!save_names_parse(&names, disc, disc_length, learned, learned_length)) {
        if (save_snapshot_learn(snapshot, length, nvram, &names, learned, SAVE_NAMES_BYTES) > 0)
            usb_file_text_write(ode, ode_bus_read_buffer, "saves-learned.txt", learned);
        save_names_free(&names);
    }
    usb_file_text_write(ode, ode_bus_read_buffer, "save-snapshot.txt", "");
done:
    if (nvram) {
        save_archive_free(nvram);
        free(nvram);
    }
    free(snapshot);
    free(disc);
    free(learned);
}
/* Reads the saves in NVRAM for the Save Manager. */
static void scan_nvram(Menu *menu)
{
    load_save_names(menu);
    menu->tool_message = "Reading internal saves...";
    draw(menu);
    save_archive_free(&menu->nvram);
    if (nvram_snapshot(&menu->nvram, &menu->nvram_info) < 0)
        menu->tool_message = menu->nvram_info.error;
    else
        menu->tool_message = NULL;
    menu->tool_selected = 0;
}
/* Checks the drive can be written to and read back, recording the result
 * for the Diagnostics screen. */
static int test_save_usb(Menu *menu)
{
    int result = save_root_operation(menu, SAVE_TEST, NULL, NULL);
    menu->usb_tested = result < 0 ? -1 : 1;
    return result;
}
/*
 * Restores the selected save of the open backup into NVRAM (A, then
 * confirm). First, all of NVRAM is backed up to a recovery slot (the other
 * one when restoring from a recovery slot, so it is never overwritten by
 * itself); then the save is written, and NVRAM is read again to check the
 * save arrived and nothing else changed.
 */
static void restore_selected(Menu *menu)
{
    SaveArchive current;
    SaveArchive verified;
    const SaveEntry *entry = &menu->backup.entries[menu->save_selected];
    NvramInfo info;
    const char *recovery =
        menu->backup_slot == SAVE_RECOVERY_SLOT ? "saves/recovery2.3ds" : "saves/recovery.3ds";
    int result = -1;
    memset(&current, 0, sizeof(current));
    memset(&verified, 0, sizeof(verified));
    menu->restore_result = -1;
    if (menu->usb_tested != 1 && test_save_usb(menu) < 0)
        return;
    if (nvram_preflight(entry, &menu->nvram_info) < 0) {
        menu->tool_message = menu->nvram_info.error;
        return;
    }
    if (nvram_snapshot(&current, &menu->nvram_info) < 0) {
        menu->tool_message = menu->nvram_info.error;
        return;
    }
    /* The complete current filesystem must reach USB and be reread before the
     * first write to NVRAM. A directory-restore failure also cancels restore. */
    if (save_root_operation(menu, SAVE_STORE, recovery, &current) < 0)
        goto done;
    menu->tool_message = "Restoring internal save...";
    draw(menu);
    if (nvram_restore(entry, &menu->nvram_info) < 0) {
        menu->tool_message = menu->nvram_info.error;
        goto done;
    }
    /* Confirm filename, type, length and payload in a fresh complete snapshot. */
    if (nvram_snapshot(&verified, &info) == 0 && save_restore_matches(&current, &verified, entry))
        result = 0;
    menu->restore_result = result ? -1 : 1;
    menu->tool_message =
        result ? "Verify failed. Keep recovery backup." : "Save restored and verified.";
done:
    save_archive_free(&current);
    save_archive_free(&verified);
}
/* ==== Tools screens: the buttons ==== */

/* The hex editor: X opens known values if the cheat database knows this
 * save; everything else is hex_input's. */
static void hex_screen_input(Menu *menu, u32 pressed)
{
    if (!menu->hex_editing && (pressed & ControlX)) {
        if (save_cheats_find(&menu->hex_archive.entries[menu->save_selected])) {
            menu->tools = menu->hex_view = TOOL_CHEATS;
            menu->cheat_selected = 0;
            menu->tool_message = NULL;
        } else
            menu->tool_message = "No verified cheats match this save.";
        return;
    }
    hex_input(menu, pressed);
}

/* Find backup slots: choose how to look for slots. */
static void slot_find_input(Menu *menu, u32 pressed)
{
    if (pressed & ControlB) {
        menu->tools = TOOL_SLOTS;
        menu->tool_message = NULL;
        return;
    }
    if (pressed & ControlUp)
        menu->slot_find_option = (menu->slot_find_option + 3) % 4;
    if (pressed & ControlDown)
        menu->slot_find_option = (menu->slot_find_option + 1) % 4;
    if (!(pressed & ControlA))
        return;
    if (menu->slot_find_option == 0) {
        /* Occupied slots: a search that matches everything. */
        menu->slot_query[0] = 0;
        menu->slot_results.offset = 0;
        slot_search(menu);
    } else if (menu->slot_find_option == 1) {
        menu->tools = TOOL_SLOT_QUERY;
        menu->find_key = 0;
        menu->tool_message = NULL;
    } else if (menu->slot_find_option == 2) {
        menu->slot_jump[0] = 0;
        menu->find_key = 1;
        menu->tools = TOOL_SLOT_JUMP;
        menu->tool_message = NULL;
    } else {
        menu->slot_filtered = 0;
        menu->tools = TOOL_SLOTS;
        menu->tool_selected = 0;
        save_root_operation(menu, SAVE_READ_LABELS, NULL, NULL);
    }
}

/* Typing a slot search; Start searches. */
static void slot_query_input(Menu *menu, u32 pressed)
{
    if (pressed & ControlB) {
        menu->tools = TOOL_SLOT_FIND;
        menu->tool_message = NULL;
        return;
    }
    keyboard_type(menu, pressed, menu->slot_query, sizeof(menu->slot_query));
    if (pressed & ControlStart) {
        menu->slot_results.offset = 0;
        slot_search(menu);
    }
}

/* Typing a slot number on a row of digits; Start goes to that slot. */
static void slot_jump_input(Menu *menu, u32 pressed)
{
    size_t length = strlen(menu->slot_jump);
    if (pressed & ControlB) {
        menu->tools = TOOL_SLOTS;
        return;
    }
    if (pressed & ControlLeft)
        menu->find_key = (menu->find_key + 9) % 10;
    if (pressed & ControlRight)
        menu->find_key = (menu->find_key + 1) % 10;
    if ((pressed & ControlA) && length < 9) {
        menu->slot_jump[length++] = (char)('0' + menu->find_key);
        menu->slot_jump[length] = 0;
    }
    if ((pressed & ControlX) && length)
        menu->slot_jump[length - 1] = 0;
    if (pressed & ControlC)
        menu->slot_jump[0] = 0;
    if (pressed & ControlStart) {
        unsigned long number = 0;
        size_t i;
        for (i = 0; menu->slot_jump[i]; ++i)
            number = number * 10 + (unsigned long)(menu->slot_jump[i] - '0');
        if (!number || number > SAVE_SLOT_LIMIT)
            menu->tool_message = "Enter a slot number from 1-999999999.";
        else {
            menu->slot_filtered = 0;
            slot_select(menu, (unsigned int)(number - 1));
            menu->tools = TOOL_SLOTS;
            memset(&menu->slot_labels, 0, sizeof(menu->slot_labels));
            save_root_operation(menu, SAVE_READ_LABELS, NULL, NULL);
        }
    }
}

/* Typing a slot's label; Start saves it. A blank label removes it. */
static void slot_name_input(Menu *menu, u32 pressed)
{
    if (pressed & ControlB) {
        menu->tools = TOOL_SLOTS;
        menu->tool_message = NULL;
        return;
    }
    keyboard_type(menu, pressed, menu->slot_label_edit, SAVE_SLOT_LABEL_BYTES);
    if (!(pressed & ControlStart))
        return;
    menu->slot_labels_before = menu->slot_labels;
    strcpy(menu->slot_labels.labels[menu->slot_name_slot % 8], menu->slot_label_edit);
    if (!save_root_operation(menu, SAVE_WRITE_LABELS, NULL, NULL)) {
        menu->tools = TOOL_SLOTS;
        menu->tool_message = "Slot name saved and verified.";
        if (menu->slot_filtered) {
            menu->slot_results.offset = 0;
            slot_search(menu);
        }
    } else {
        menu->slot_labels = menu->slot_labels_before;
        menu->tool_message = "Name failed. Reload before retrying.";
        menu->tools = TOOL_SLOTS;
    }
}

/* Comparing backups: choose a save and A to see its bytes, which page with
 * Left/Right (7 a page). B goes back a step. */
static void compare_input(Menu *menu, u32 pressed)
{
    size_t count = save_archive_diff_count(&menu->compare_archive, &menu->backup);
    if (pressed & ControlB) {
        if (menu->tools == TOOL_COMPARE_BYTES)
            menu->tools = TOOL_COMPARE;
        else {
            save_archive_free(&menu->compare_archive);
            menu->tools = TOOL_SLOTS;
            slot_select(menu, menu->backup_slot);
        }
    } else if (menu->tools == TOOL_COMPARE) {
        if ((pressed & ControlUp) && count)
            menu->tool_selected = menu->tool_selected ? menu->tool_selected - 1 : count - 1;
        if ((pressed & ControlDown) && count)
            menu->tool_selected = (menu->tool_selected + 1) % count;
        if ((pressed & ControlA) && count) {
            menu->tools = TOOL_COMPARE_BYTES;
            menu->compare_offset = 0;
        }
    } else {
        SaveDifference diff;
        if (!save_archive_diff(&menu->compare_archive, &menu->backup, menu->tool_selected, &diff)) {
            if ((pressed & ControlLeft) && menu->compare_offset)
                menu->compare_offset -= 7;
            if ((pressed & ControlRight) && menu->compare_offset + 7 < diff.changed_bytes)
                menu->compare_offset += 7;
        }
    }
}

/*
 * A backup's list of saves: Start copies the selected save to another slot,
 * X opens its known values and C the hex editor. Returns 1 if the press was
 * used.
 */
static int archive_input(Menu *menu, u32 pressed)
{
    if (!menu->backup.count)
        return 0;
    if (pressed & ControlStart) {
        menu->save_selected = menu->tool_selected;
        save_archive_free(&menu->pending);
        if (save_archive_build(&menu->pending, &menu->backup.entries[menu->save_selected], 1) < 0) {
            menu->tool_message = "Cannot prepare copy.";
            return 1;
        }
        /* Keep the source backup aside while choosing where to copy to. */
        save_archive_free(&menu->copy_original);
        menu->copy_original = menu->backup;
        memset(&menu->backup, 0, sizeof(menu->backup));
        menu->copy_source_slot = menu->backup_slot;
        menu->backup_merge = menu->backup_mode = 1;
        menu->slot_filtered = 0;
        menu->tools = TOOL_SLOTS;
        menu->tool_selected = 0;
        return 1;
    }
    if (pressed & ControlX) {
        menu->save_selected = menu->tool_selected;
        if (!save_cheats_find(&menu->backup.entries[menu->save_selected])) {
            menu->tool_message = "No verified cheats match this save.";
            return 1;
        }
        if (!open_hex(menu)) {
            menu->tools = menu->hex_view = TOOL_CHEATS;
            menu->cheat_selected = 0;
        }
        return 1;
    }
    if (pressed & ControlC) {
        menu->save_selected = menu->tool_selected;
        open_hex(menu);
        return 1;
    }
    return 0;
}

/* Left/Right on the Save Manager's tabs: between console saves and USB
 * backups. */
static void switch_save_tab(Menu *menu)
{
    menu->tool_selected = 0;
    menu->tool_message = NULL;
    save_archive_free(&menu->pending);
    if (menu->tools == TOOL_SAVES) {
        menu->tools = TOOL_SLOTS;
        menu->backup_mode = 0;
        load_save_names(menu);
    } else {
        menu->tools = TOOL_SAVES;
        scan_nvram(menu);
    }
}

/*
 * The Slots screen's own buttons: Start finds slots, L and R turn the page
 * (of slots, or of search results), Left/Right switch tabs, C names the
 * selected slot and X picks it to compare with another. Returns 1 if the
 * press was used.
 */
static int slots_input(Menu *menu, u32 pressed)
{
    int browsing = !menu->backup_mode && !menu->compare_choosing;
    if (pressed & ControlStart) {
        menu->slot_find_option = 0;
        menu->tools = TOOL_SLOT_FIND;
        menu->tool_message = NULL;
        return 1;
    }
    if (pressed & (ControlLeftShift | ControlRightShift)) {
        if (menu->slot_filtered) {
            if ((pressed & ControlLeftShift) && menu->slot_results.offset)
                menu->slot_results.offset -= 8;
            else if ((pressed & ControlRightShift) &&
                     menu->slot_results.offset + 8 < menu->slot_results.total)
                menu->slot_results.offset += 8;
            else
                return 1;
            slot_search(menu);
            return 1;
        }
        if ((pressed & ControlLeftShift) && menu->slot_page)
            --menu->slot_page;
        if ((pressed & ControlRightShift) && menu->slot_page < (SAVE_SLOT_LIMIT - 1) / 8)
            ++menu->slot_page;
        menu->tool_selected = 0;
        memset(&menu->slot_labels, 0, sizeof(menu->slot_labels));
        save_root_operation(menu, SAVE_READ_LABELS, NULL, NULL);
        return 1;
    }
    if (!browsing)
        return 0;
    if (pressed & (ControlLeft | ControlRight)) {
        switch_save_tab(menu);
        return 1;
    }
    if (pressed & ControlC) {
        if (!slot_count(menu))
            return 1;
        menu->slot_name_slot = slot_at(menu, menu->tool_selected);
        if (menu->slot_name_slot >= SAVE_SLOT_LIMIT) {
            menu->tool_message = "Recovery backups keep their names.";
            return 1;
        }
        menu->slot_page = menu->slot_name_slot / 8;
        if (!save_root_operation(menu, SAVE_READ_LABELS, NULL, NULL)) {
            strcpy(menu->slot_label_edit, menu->slot_labels.labels[menu->slot_name_slot % 8]);
            menu->find_key = 0;
            menu->tools = TOOL_SLOT_NAME;
        }
        return 1;
    }
    if (pressed & ControlX) {
        char path[40];
        if (!slot_count(menu))
            return 1;
        slot_path(path, slot_at(menu, menu->tool_selected));
        save_archive_free(&menu->compare_archive);
        if (save_root_operation(menu, SAVE_LOAD_TARGET, path, &menu->compare_archive) >= 0) {
            menu->compare_slot = slot_at(menu, menu->tool_selected);
            menu->compare_choosing = 1;
            menu->tool_message = "Choose the second slot, then A.";
        }
        return 1;
    }
    return 0;
}

/* Reordering favorites: Up/Down move the selected favorite, A or R saves
 * the order and B puts it back. */
static void favorites_move_input(Menu *menu, u32 pressed)
{
    size_t to = menu->tool_selected, from_index, to_index;
    if (pressed & ControlB) {
        cancel_favorites_move(menu);
        return;
    }
    if (pressed & (ControlRightShift | ControlA)) {
        save_favorites_order(menu);
        return;
    }
    if (!(pressed & (ControlUp | ControlDown)))
        return;
    if ((pressed & ControlUp) && to)
        --to;
    if ((pressed & ControlDown) &&
        to + 1 < menu_favorites_visible_count(&menu->favorites, menu->config.hide_adult))
        ++to;
    /* Swap it with its neighbor in the whole list (some may be hidden). */
    from_index = menu_favorites_visible_index(&menu->favorites, menu->config.hide_adult,
                                              menu->tool_selected);
    to_index = menu_favorites_visible_index(&menu->favorites, menu->config.hide_adult, to);
    if (from_index < menu->favorites.count && to_index < menu->favorites.count) {
        const char *path = menu->favorites.paths[from_index];
        menu->favorites.paths[from_index] = menu->favorites.paths[to_index];
        menu->favorites.paths[to_index] = path;
        menu->tool_selected = to;
    }
}

/* B on a Tools screen: back one step, to the screen this one was opened
 * from (or out of Tools from the Tools menu). */
static void tools_back(Menu *menu)
{
    int page = menu->tools;
    menu->tool_message = NULL;
    if (page == TOOL_HOME || page == TOOL_GAME_DETAILS) {
        menu->tools = 0;
        selection_changed(menu);
    } else if (page == TOOL_SLOTS && menu->compare_choosing) {
        menu->compare_choosing = 0;
        save_archive_free(&menu->compare_archive);
    } else if (page == TOOL_SLOTS && menu->backup_mode) {
        /* Stop choosing a slot to back up to: back to what was backed up. */
        save_archive_free(&menu->pending);
        menu->backup_mode = 0;
        if (menu->copy_original.bytes)
            finish_copy(menu);
        else if (menu->hex_archive.bytes)
            menu->tools = menu->hex_view;
        else {
            menu->tools = TOOL_SAVES;
            scan_nvram(menu);
        }
    } else if (page == TOOL_RESULTS) {
        menu->tools = TOOL_FIND;
        menu->tool_selected = 0;
    } else if (page == TOOL_BACKUP_DETAILS) {
        menu->tools = TOOL_SLOTS;
        slot_select(menu, menu->backup_slot);
    } else if (page == TOOL_ARCHIVE) {
        menu->tools = TOOL_SLOTS;
        menu->backup_mode = 0;
        slot_select(menu, menu->backup_slot);
    } else if (page == TOOL_CONFIRM_RESTORE) {
        menu->tools = TOOL_ARCHIVE;
        menu->tool_selected = menu->save_selected;
    } else if (page == TOOL_CONFIRM_BACKUP) {
        menu->tools = TOOL_SLOTS;
        slot_select(menu, menu->backup_slot);
    } else {
        menu->tools = TOOL_HOME;
        menu->tool_selected = menu->home_selected;
        save_archive_free(&menu->pending);
    }
    if (menu->tools == TOOL_SLOTS && menu->slot_labels_page != menu->slot_page)
        save_root_operation(menu, SAVE_READ_LABELS, NULL, NULL);
}

/* How many rows the current Tools screen's list has, for moving through it
 * (0 for screens without a list). */
static size_t tool_list_length(Menu *menu)
{
    switch (menu->tools) {
    case TOOL_HOME:
        return 7;
    case TOOL_SAVES:
        return menu->nvram.count;
    case TOOL_ARCHIVE:
        return menu->backup.count;
    case TOOL_SLOTS:
        return slot_count(menu);
    case TOOL_CONFIG:
        return SETTING_ROWS;
    case TOOL_CATEGORIES:
        return menu_categories_count();
    case TOOL_FAVORITES:
        return menu_favorites_visible_count(&menu->favorites, menu->config.hide_adult);
    case TOOL_RECENT:
        return menu->recent.count;
    case TOOL_BACKUP_DETAILS:
        return menu->details_new ? menu->pending.count : menu->backup.count;
    case TOOL_RESULTS:
        return menu->search_count;
    }
    return 0;
}

/* Up/Down through a list (wrapping). The Tools menu is a grid of two
 * columns of four (the last missing): Left/Right switch columns. */
static void tool_list_move(Menu *menu, u32 pressed)
{
    size_t count = tool_list_length(menu);
    if (menu->tools == TOOL_HOME) {
        size_t column = menu->tool_selected / 4, row = menu->tool_selected % 4;
        if (pressed & (ControlLeft | ControlRight))
            column = !column;
        if (pressed & ControlUp)
            row = (row + 3) % 4;
        if (pressed & ControlDown)
            row = (row + 1) % 4;
        menu->tool_selected = column * 4 + row;
        if (menu->tool_selected >= 7)
            menu->tool_selected = column * 4 + (pressed & ControlUp ? 2 : 0);
        menu->home_selected = menu->tool_selected;
    } else if (count) {
        if (pressed & ControlUp)
            menu->tool_selected = menu->tool_selected ? menu->tool_selected - 1 : count - 1;
        if (pressed & ControlDown)
            menu->tool_selected = (menu->tool_selected + 1) % count;
    }
}

/*
 * A on a category: shows or hides it and saves categories.json at once.
 * If the browser is inside a category that is now hidden, it goes back out
 * of it; at the top, the folder is reloaded to show or hide it.
 */
static void toggle_category(Menu *menu)
{
    unsigned int root = menu->depth && !(menu->path_ids[0] & 0xffffffUL) ? 1 : 0;
    if (!menu->categories_valid) {
        menu->tool_message = "Invalid categories.json. Repair on PC.";
        return;
    }
    if (menu_category_toggle(menu->tool_selected) < 0) {
        menu->tool_message = "Invalid category data.";
        return;
    }
    if (text_at_root(menu, "categories.json", menu_categories_text(), NULL, 0, NULL) < 0) {
        menu_category_toggle(menu->tool_selected);
        menu->tool_message = "Visibility save failed. Check USB.";
        return;
    }
    menu->tool_message = "Visibility saved and verified.";
    if (menu->depth > root && menu_categories_hidden_name(menu->path[root])) {
        while (menu->ready && menu->depth > root) {
            unsigned int before = menu->depth;
            go_back(menu);
            if (menu->depth == before)
                break;
        }
    } else if (menu->depth == root)
        load_directory(menu, menu->selected);
}

/*
 * A or C on console saves: backs up the selected save (A) or all of them
 * (C). NVRAM is read again first, so the backup is of the saves as they
 * are now; then the person chooses a slot.
 */
static void begin_backup(Menu *menu, u32 pressed)
{
    size_t selected = menu->tool_selected;
    int all = (pressed & ControlC) != 0;
    char selected_name[SAVE_NAME_BYTES];
    selected_name[0] = 0;
    if (selected < menu->nvram.count)
        strcpy(selected_name, menu->nvram.entries[selected].name);
    scan_nvram(menu);
    if (menu->tool_message || !menu->nvram.count)
        return;
    if (!all) {
        /* Find the selected save again in the fresh list. */
        for (selected = 0; selected < menu->nvram.count; ++selected)
            if (!strcmp(selected_name, menu->nvram.entries[selected].name))
                break;
        if (selected == menu->nvram.count) {
            menu->tool_message = "Save list changed. Select again.";
            return;
        }
    }
    save_archive_free(&menu->pending);
    if (save_archive_build(&menu->pending,
                           all ? menu->nvram.entries : &menu->nvram.entries[selected],
                           all ? menu->nvram.count : 1) < 0) {
        menu->tool_message = "Cannot prepare backup.";
        return;
    }
    menu->backup_merge = !all;
    menu->save_selected = selected;
    menu->backup_mode = 1;
    menu->slot_filtered = 0;
    menu->tools = TOOL_SLOTS;
    menu->tool_selected = 0;
}

/* A on the Tools menu: opens the chosen screen. */
static void open_tool(Menu *menu)
{
    size_t selected = menu->tool_selected;
    menu->tool_selected = 0;
    switch (selected) {
    case 0:
        menu->tools = TOOL_SAVES;
        scan_nvram(menu);
        break;
    case 1:
        menu->tools = TOOL_FAVORITES;
        open_favorites(menu);
        break;
    case 2:
        menu->tools = TOOL_CONFIG;
        break;
    case 3:
        menu->tools = TOOL_HELP;
        break;
    case 4:
        menu->tools = TOOL_DIAG;
        break;
    case 5:
        menu->tools = TOOL_RECENT;
        open_recent(menu);
        break;
    default:
        menu->tools = TOOL_FIND;
        menu->find_key = 0;
        break;
    }
}

/*
 * A on a slot. Choosing where to back up to: reads the slot (merging the
 * one save into it, if backing up one) and shows what it will hold.
 * Comparing: reads it as the second backup and shows the comparison.
 * Otherwise: opens the backup in it.
 */
static void open_slot(Menu *menu)
{
    char path[40];
    if (!slot_count(menu))
        return;
    menu->backup_slot = slot_at(menu, menu->tool_selected);
    slot_path(path, menu->backup_slot);
    save_archive_free(&menu->backup);
    if (menu->backup_mode) {
        if (menu->backup_merge) {
            const SaveEntry *entry = backup_entry(menu);
            int loaded = save_root_operation(menu, SAVE_LOAD_TARGET, path, &menu->backup);
            if (menu->ode.navigation_failed || menu->bus.timed_out || !menu->ready)
                return;
            if (loaded < 0) {
                menu->tool_message = "Slot unreadable. Choose another.";
                return;
            }
            menu->backup_slot_valid = loaded == 0;
            if (save_archive_merge(&menu->pending, &menu->backup, entry) < 0) {
                menu->tool_message = "Merged slot is full. Choose another.";
                return;
            }
        } else
            menu->backup_slot_valid = !save_root_operation(menu, SAVE_LOAD, path, &menu->backup);
        if (menu->ode.navigation_failed || menu->bus.timed_out || !menu->ready)
            return;
        menu->tools = TOOL_BACKUP_DETAILS;
        menu->tool_selected = 0;
        menu->details_new = 0;
        menu->tool_message = NULL;
    } else if (menu->compare_choosing) {
        if (save_root_operation(menu, SAVE_LOAD_TARGET, path, &menu->backup) >= 0) {
            menu->compare_choosing = 0;
            menu->tools = TOOL_COMPARE;
            menu->tool_selected = 0;
        }
    } else {
        if (!save_root_operation(menu, SAVE_LOAD, path, &menu->backup)) {
            menu->tools = TOOL_ARCHIVE;
            menu->tool_selected = 0;
        } else if (menu->tool_message &&
                   !strcmp(menu->tool_message, "USB failed. Check .ohmy3do/saves."))
            menu->tool_message = "No valid backup in this slot.";
    }
}

/*
 * A on the backup confirmation: checks the drive can be written (once a
 * session), then writes the slot: the whole new backup, or the slot with
 * one save merged in, if it still holds what it did when read. Then back
 * to the slots, or after copying a save, to the backup it came from.
 */
static void confirm_backup(Menu *menu)
{
    char path[40];
    slot_path(path, menu->backup_slot);
    menu->backup_result = -1;
    if ((menu->usb_tested == 1 || !test_save_usb(menu)) &&
        !save_root_operation(menu, menu->backup_merge ? SAVE_STORE_CHECKED : SAVE_STORE, path,
                             &menu->pending)) {
        menu->backup_result = 1;
        menu->tool_message = menu->slot_catalog_stale ? "Saved. Refresh catalog in desktop app."
                             : menu->backup_merge     ? "Save merged and verified."
                                                      : "Backup saved and verified.";
        if (menu->copy_original.bytes) {
            finish_copy(menu);
            return;
        }
        /* Saved from the hex editor: the edits are now in the slot. */
        if (menu->hex_archive.bytes)
            menu->backup_mode = 0;
        save_archive_free(&menu->hex_archive);
        save_archive_free(&menu->hex_original);
        menu->hex_dirty = 0;
    }
    menu->tools = TOOL_SLOTS;
    slot_select(menu, menu->backup_slot);
}

/* A on a Tools screen: does what the selected row offers. */
static void tools_activate(Menu *menu)
{
    menu->tool_message = NULL;
    switch (menu->tools) {
    case TOOL_HOME:
        open_tool(menu);
        break;
    case TOOL_SLOTS:
        open_slot(menu);
        break;
    case TOOL_RESULTS:
        if (menu->search_count) {
            const char *path = menu->catalog.paths[menu->search_matches[menu->tool_selected]];
            if (path)
                open_saved_path(menu, path);
        }
        break;
    case TOOL_FAVORITES:
    case TOOL_RECENT:
        open_favorite(menu);
        break;
    case TOOL_BACKUP_DETAILS:
        menu->tools = TOOL_CONFIRM_BACKUP;
        break;
    case TOOL_ARCHIVE:
        if (menu->backup.count) {
            menu->save_selected = menu->tool_selected;
            menu->tools = TOOL_CONFIRM_RESTORE;
        }
        break;
    case TOOL_CONFIRM_BACKUP:
        confirm_backup(menu);
        break;
    case TOOL_CONFIRM_RESTORE:
        restore_selected(menu);
        menu->tools = TOOL_ARCHIVE;
        menu->tool_selected = menu->save_selected;
        break;
    }
}

/*
 * The buttons on a Tools screen (menu->tools). Screens that take over the
 * controller (the editors, keyboards and slot searches) have their own
 * functions. The others share B (back), moving through a list, and A (do
 * what the row offers), plus a few buttons of their own.
 */
static void tools_input(Menu *menu, u32 pressed)
{
    int page = menu->tools;
    switch (page) {
    case TOOL_CHEATS:
        cheats_input(menu, pressed);
        return;
    case TOOL_HEX:
        hex_screen_input(menu, pressed);
        return;
    case TOOL_HEX_DISCARD:
        if (pressed & ControlB)
            menu->tools = menu->hex_view;
        else if (pressed & ControlA)
            close_hex(menu);
        return;
    case TOOL_SLOT_FIND:
        slot_find_input(menu, pressed);
        return;
    case TOOL_SLOT_QUERY:
        slot_query_input(menu, pressed);
        return;
    case TOOL_SLOT_JUMP:
        slot_jump_input(menu, pressed);
        return;
    case TOOL_SLOT_NAME:
        slot_name_input(menu, pressed);
        return;
    case TOOL_COMPARE:
    case TOOL_COMPARE_BYTES:
        compare_input(menu, pressed);
        return;
    }

    /* Buttons particular to a screen, before the shared ones. */
    if (page == TOOL_ARCHIVE && archive_input(menu, pressed))
        return;
    if (page == TOOL_GAME_DETAILS && !(pressed & ControlB)) {
        if ((pressed & ControlRight) && menu->game_details_page + 1 < menu->game_details_pages)
            ++menu->game_details_page;
        if ((pressed & ControlLeft) && menu->game_details_page)
            --menu->game_details_page;
        return;
    }
    if (page == TOOL_SLOTS && slots_input(menu, pressed))
        return;
    if (page == TOOL_SAVES && (pressed & (ControlLeft | ControlRight))) {
        switch_save_tab(menu);
        return;
    }
    if (page == TOOL_FIND && !(pressed & ControlB)) {
        find_input(menu, pressed);
        return;
    }
    if (page == TOOL_FAVORITES && menu->favorites_move) {
        favorites_move_input(menu, pressed);
        return;
    }
    if (page == TOOL_FAVORITES && (pressed & ControlRightShift)) {
        begin_favorites_move(menu);
        return;
    }
    if (page == TOOL_CATEGORIES && (pressed & ControlB)) {
        menu->tools = TOOL_CONFIG;
        menu->tool_selected = SETTING_CATEGORIES;
        menu->tool_message = NULL;
        return;
    }

    if (pressed & ControlB) {
        tools_back(menu);
        return;
    }

    /* Search results: Start edits the search, X reloads the index, and
     * Left/Right (or L/R) turn the page. */
    if (page == TOOL_RESULTS) {
        if (pressed & ControlStart) {
            menu->tools = TOOL_FIND;
            menu->tool_message = NULL;
            return;
        }
        if (pressed & ControlX) {
            search_library(menu, 1);
            return;
        }
        if (menu->search_count) {
            if (pressed & (ControlRight | ControlRightShift)) {
                menu->tool_selected += 6;
                if (menu->tool_selected >= menu->search_count)
                    menu->tool_selected = menu->search_count - 1;
            } else if (pressed & (ControlLeft | ControlLeftShift))
                menu->tool_selected = menu->tool_selected >= 6 ? menu->tool_selected - 6 : 0;
        }
    }

    tool_list_move(menu, pressed);

    switch (page) {
    case TOOL_HELP:
        if (pressed & (ControlLeft | ControlRight))
            menu->tool_selected = !menu->tool_selected;
        return;
    case TOOL_CATEGORIES:
        if (pressed & ControlA) {
            toggle_category(menu);
            return;
        }
        break;
    case TOOL_CONFIG:
        if (pressed & ControlX)
            discard_config(menu);
        else if (pressed & ControlC)
            save_config(menu);
        else if (pressed & (ControlA | ControlLeft | ControlRight))
            change_config(menu, (pressed & ControlLeft) ? -1 : 1);
        else
            break;
        return;
    case TOOL_RECENT:
        if (pressed & ControlStart) {
            open_recent(menu);
            return;
        }
        break;
    case TOOL_FAVORITES:
        if (pressed & ControlStart)
            open_favorites(menu);
        else if (pressed & (ControlC | ControlX))
            edit_favorites(menu, (pressed & ControlX) != 0);
        else
            break;
        return;
    case TOOL_BACKUP_DETAILS:
        if (pressed & ControlC) {
            /* Switch between the slot as it is and as it will be. */
            menu->details_new = !menu->details_new;
            menu->tool_selected = 0;
            return;
        }
        break;
    case TOOL_DIAG:
        if (pressed & ControlX)
            recover_library(menu);
        else if (pressed & ControlC)
            export_diagnostics(menu);
        else {
            if (pressed & ControlRight)
                menu->tool_selected = (menu->tool_selected + 1) % 5;
            if (pressed & ControlLeft)
                menu->tool_selected = (menu->tool_selected + 4) % 5;
        }
        return;
    case TOOL_SAVES:
        if (pressed & ControlX) {
            scan_nvram(menu);
            return;
        }
        if (pressed & (ControlA | ControlC)) {
            begin_backup(menu, pressed);
            return;
        }
        break;
    }

    if (pressed & ControlA)
        tools_activate(menu);
}

/* ==== Help and opening Tools ==== */

/* Which help a held P shows: the current Tools screen's, or the browser's
 * (the alphabet strip, queue review or list). */
static MenuHelpTopic current_help_topic(const Menu *menu)
{
    switch (menu->tools) {
    case TOOL_HOME:
        return HELP_TOOLS;
    case TOOL_SAVES:
        return HELP_SAVES;
    case TOOL_SLOTS:
        return menu->backup_mode        ? HELP_DESTINATION
               : menu->compare_choosing ? HELP_COMPARE_PICK
                                        : HELP_SLOTS;
    case TOOL_ARCHIVE:
        return HELP_ARCHIVE;
    case TOOL_HEX:
        return menu->hex_editing ? HELP_HEX_BYTE : HELP_HEX;
    case TOOL_CHEATS:
        return HELP_CHEATS;
    case TOOL_CONFIG:
        return HELP_SETTINGS;
    case TOOL_CATEGORIES:
        return HELP_CATEGORIES;
    case TOOL_FIND:
    case TOOL_SLOT_NAME:
    case TOOL_SLOT_QUERY:
        return HELP_KEYBOARD;
    case TOOL_FAVORITES:
        return menu->favorites_move ? HELP_FAVORITE_MOVE : HELP_FAVORITES;
    case TOOL_RECENT:
        return HELP_RECENT;
    case TOOL_RESULTS:
        return HELP_RESULTS;
    case TOOL_COMPARE:
        return HELP_COMPARE;
    case TOOL_COMPARE_BYTES:
        return HELP_COMPARE_BYTES;
    case TOOL_CONFIRM_BACKUP:
    case TOOL_CONFIRM_RESTORE:
    case TOOL_HEX_DISCARD:
        return HELP_CONFIRM;
    case TOOL_DIAG:
        return HELP_DIAGNOSTICS;
    case TOOL_GAME_DETAILS:
        return HELP_DETAILS;
    case TOOL_BACKUP_DETAILS:
        return HELP_BACKUP_DETAILS;
    case TOOL_SLOT_JUMP:
        return HELP_SLOT_JUMP;
    case TOOL_HELP:
        return HELP_GENERAL;
    case TOOL_SLOT_FIND:
        return HELP_SLOT_FIND;
    default:
        return menu->alphabet_active ? HELP_ALPHABET : menu->review ? HELP_QUEUE : HELP_LIBRARY;
    }
}
/* Draws the help for the current screen (held P), drawing it into both
 * screen buffers once and then leaving it. */
static void draw_context_help(Menu *menu)
{
    MenuHelpTopic topic = current_help_topic(menu);
    unsigned int row;
    if (menu->help_topic != (int)topic || menu->help_margin != menu->config.screen_margin) {
        menu->help_drawn = 0;
        menu->help_topic = (int)topic;
        menu->help_margin = menu->config.screen_margin;
    }
    if (menu->help_drawn >= 2)
        return;
    ++menu->help_drawn;
    menu->text_cursor = 0;
    menu->text_style = UI_FONT_SMALL;
    menu->screen.sc_curScreen ^= 1;
    menu->browser_frames[0].valid = menu->browser_frames[1].valid = 0;
    rectangle(menu, 0, 0, 320, 240, BG);
    brand(menu, "Controls");
    rectangle(menu, 12, 58, 296, 148, PANEL);
    label(menu, 22, 62, menu_help_title(topic), TEXT, UI_FONT_NORMAL);
    rectangle(menu, 22, 77, 39, 2, TV_ORANGE);
    rectangle(menu, 63, 77, 17, 2, ACCENT);
    for (row = 0; row < MENU_HELP_ROWS; ++row)
        tool_line(menu, (int)row, menu_help_line(topic, row), TEXT);
    control_badges(menu, 16, 211, "B/P:Return to this screen", 0);
    label(menu, 16, 228, "Tap P: Normal action / Hold P: Help", MUTED, UI_FONT_SMALL);
    DisplayScreen(menu->screen.sc_Screens[menu->screen.sc_curScreen], 0);
}

/*
 * L+R: opens the Tools menu (with the item last chosen selected), or
 * closes Tools, putting away whatever was open (a copy, a comparison, an
 * unsaved backup). With unsaved hex edits, asks first.
 */
static void toggle_tools(Menu *menu)
{
    if (menu->tools == TOOL_HOME)
        menu->home_selected = menu->tool_selected;
    if (menu->favorites_move)
        cancel_favorites_move(menu);
    if (menu->tools && menu->hex_archive.bytes && (menu->hex_dirty || menu->hex_editing)) {
        menu->tools = TOOL_HEX_DISCARD;
        menu->tool_message = NULL;
        return;
    }
    if (menu->tools) {
        if (menu->hex_archive.bytes)
            close_hex(menu);
        if (menu->copy_original.bytes)
            finish_copy(menu);
        save_archive_free(&menu->compare_archive);
        menu->compare_choosing = 0;
        save_archive_free(&menu->pending);
        menu->tools = menu->review = 0;
        selection_changed(menu);
    } else {
        menu->tools = TOOL_HOME;
        menu->tool_selected = menu->home_selected;
    }
    menu->tool_message = NULL;
    menu->c_pending = menu->alphabet_active = 0;
}
/* Opens or closes Tools if L+R asked to (see observe_tools_chord).
 * Returns 1 if it did. */
static int dispatch_tools_request(Menu *menu)
{
    if (!menu->tools_requested)
        return 0;
    menu->tools_requested = 0;
    toggle_tools(menu);
    return 1;
}

/* ==== Starting the menu and the main loop ==== */

/* Sets up the console: the controller, two screens, a VBL request for
 * waiting on the display and a timer for the clock. Returns 0, or -1
 * (having undone what was set up) if any of them fails. */
static int open_console(Menu *menu)
{
    int screen;
    if (InitControlPad(1) < 0)
        return -1;
    if (CreateBasicDisplay(&menu->screen, DI_TYPE_DEFAULT, 2) < 0) {
        KillControlPad();
        return -1;
    }
    /* The display hardware blends each pixel with its neighbors by default,
     * which softens text and edges; the menu is drawn for crisp pixels. */
    for (screen = 0; screen < 2; ++screen) {
        DisableHAVG(menu->screen.sc_Screens[screen]);
        DisableVAVG(menu->screen.sc_Screens[screen]);
    }
    menu->vbl = CreateVBLIOReq();
    if (menu->vbl < 0) {
        DeleteBasicDisplay(&menu->screen);
        KillControlPad();
        return -1;
    }
    menu->clock = GetTimerIOReq();
    if (menu->clock < 0) {
        DeleteIOReq(menu->vbl);
        DeleteBasicDisplay(&menu->screen);
        KillControlPad();
        return -1;
    }
    return 0;
}

/* Loads the splash screen (drawn at once) and the logo from the menu's
 * disc, and the system font for drawing text. */
static void load_menu_art(Menu *menu)
{
    Font *font = GetCurrentFont();
    if (font && font->font_CCB) {
        menu->font_ccb = *font->font_CCB;
        menu->have_font = 1;
    }
    menu->covers_enabled = menu_cover_count != 0;
#ifndef MENU_DEMO
    menu->splash = LoadCel("$boot/startup.cel", MEMTYPE_CEL);
    /* Black pixels in the splash are transparent: the live static shows there. */
    if (menu->splash) {
        menu->splash->ccb_Flags |= CCB_LAST;
        menu->splash->ccb_Flags &= ~CCB_BGND;
    }
    menu->booting = 1;
    draw(menu);
    boot_step(menu, BOOT_SPLASH);
#endif
    menu->logo = LoadCel("$boot/logo.cel", MEMTYPE_CEL);
    init_channel_gecko(menu);
    if (menu->logo) {
        menu->logo->ccb_XPos = 16 << 16;
        menu->logo->ccb_YPos = 8 << 16;
        menu->logo->ccb_Flags |= CCB_LAST;
    }
    boot_step(menu, BOOT_ART);
}

/* The controller, as read on one pass of the main loop. */
typedef struct Pad {
    u32 buttons;  /* held */
    u32 pressed;  /* newly pressed, or repeating */
    u32 released; /* let go since the last pass */
    u32 repeated; /* directions repeating because they are held */
    /* Between passes: */
    u32 previous;            /* the buttons held on the last pass */
    unsigned long repeat_at; /* when a held direction next repeats */
} Pad;

/*
 * Reads the controller into `pad`. Buttons in menu->suppress_buttons (used
 * by something already) count as up until they are let go. A held
 * direction repeats after 250 ms, then every 50 ms, by the clock, so the
 * speed does not change with how long drawing takes. Returns -1 if the
 * controller could not be read.
 */
static int read_pad(Menu *menu, Pad *pad)
{
    ControlPadEventData event;
    if (GetControlPad(1, 0, &event) < 0)
        return -1;
    pad->buttons = event.cped_ButtonBits;
    menu->suppress_buttons &= pad->buttons;
    pad->buttons &= ~menu->suppress_buttons;
    /* Directions pressed during a canceled media read count as presses. */
    pad->pressed =
        (pad->buttons & ~pad->previous) | (menu->cancel_directions & ~menu->suppress_buttons);
    menu->cancel_directions = 0;
    pad->released = pad->previous & ~pad->buttons;
    pad->repeated = 0;
    if (pad->buttons & DIRECTIONS) {
        unsigned long now = now_ms(menu);
        if (pad->pressed & DIRECTIONS)
            pad->repeat_at = now + 250UL;
        else if (now >= pad->repeat_at) {
            pad->repeated = pad->buttons & DIRECTIONS;
            pad->pressed |= pad->repeated;
            pad->repeat_at = now + 50UL;
        }
    }
    pad->previous = pad->buttons;
    return 0;
}

/* Draws the screen and waits for the next video frame: the end of a pass
 * of the main loop. */
static void next_frame(Menu *menu)
{
    draw(menu);
    WaitVBL(menu->vbl, 1);
}

/*
 * Holding P (Start) shows help for the current screen; a tap does P's usual
 * job. While help shows, B or P closes it. Returns 1 if this pass is used
 * up (help is showing or just opened); otherwise may change pad->pressed:
 * P is held back while it may yet become a hold, and passed on after a tap.
 */
static int help_input(Menu *menu, Pad *pad)
{
    int action;
    if (menu->help_hold.active) {
        if (pad->pressed & (ControlB | ControlStart)) {
            menu->help_hold.active = menu->help_hold.pending = 0;
            menu->suppress_buttons |= pad->buttons;
            menu->clear_frames = 2;
            menu->preview_attempted = 0;
            menu->selection_since = now_ms(menu);
        }
        return 1;
    }
    action = menu_help_poll(&menu->help_hold, now_ms(menu), !!(pad->buttons & ControlStart),
                            !!(pad->pressed & ControlStart), !!(pad->released & ControlStart),
                            !!(pad->buttons & ~ControlStart));
    if (menu->help_hold.pending || action == MENU_HELP_OPEN)
        pad->pressed &= ~ControlStart;
    if (action == MENU_HELP_TAP)
        pad->pressed |= ControlStart;
    if (action == MENU_HELP_OPEN) {
        stop_preview(menu);
        menu->browser_frames[0].valid = menu->browser_frames[1].valid = 0;
        return 1;
    }
    return 0;
}

/* In the full-screen view, any button leaves it: video fades out first
 * (fade_step finishes the job), a cover closes at once. */
static void full_view_input(Menu *menu, Pad *pad)
{
    if (!pad->pressed)
        return;
    if (menu->full_view == 2 && FADES) {
        if (!menu->fade) {
            menu->fade = 1;
            menu->fade_since = now_ms(menu);
        }
    } else
        close_full_cover(menu);
    menu->suppress_buttons |= pad->buttons;
}

/* Moves the selection in the list (or the queue review) with the pad, L
 * and R: Up/Down by one (wrapping), Left/Right and L/R by a page. */
static void move_selection(Menu *menu, u32 pressed, size_t *selection, size_t total)
{
    if (pressed & ControlUp)
        *selection = *selection ? *selection - 1 : total - 1;
    else if (pressed & ControlDown)
        *selection = (*selection + 1) % total;
    else if (pressed & (ControlRight | ControlRightShift)) {
        *selection += menu_rows(menu);
        if (*selection >= total)
            *selection = total - 1;
    } else if (pressed & (ControlLeft | ControlLeftShift))
        *selection = *selection >= menu_rows(menu) ? *selection - menu_rows(menu) : 0;
}

/*
 * The queue review's buttons: X removes the selected disc (C+X clears the
 * queue), C starts or ends moving it, A or P plays the queue (or ends
 * moving), and B ends moving or closes the review.
 */
static void queue_review_input(Menu *menu, u32 pressed, u32 buttons)
{
    if (pressed & ControlB) {
        if (menu->queue_move)
            menu->queue_move = 0;
        else
            menu->review = 0;
        menu->message = NULL;
    } else if ((pressed & ControlX) && (buttons & ControlC)) {
        if (ode_playlist_clear(&menu->ode, &menu->playlist) < 0)
            menu->message = menu->ode.error;
        else {
            menu->queue_selected = 0;
            menu->queue_move = 0;
            notify(menu, "Queue cleared. B:Back");
        }
    } else if (pressed & ControlX) {
        if (!ode_playlist_remove_local(&menu->playlist, menu->queue_selected)) {
            if (menu->queue_selected >= menu->playlist.count)
                menu->queue_selected = menu->playlist.count ? menu->playlist.count - 1 : 0;
            notify(menu, "Disc removed. A:Play B:Back");
        } else
            menu->message = "Queue uncertain. C+X:Clear";
    } else if (pressed & ControlC) {
        menu->queue_move = !menu->queue_move;
        menu->message = menu->queue_move ? "Up/Down:Move C/B:Done" : NULL;
    } else if (pressed & (ControlA | ControlStart)) {
        if (menu->queue_move) {
            menu->queue_move = 0;
            menu->message = NULL;
        } else
            start_queue(menu);
    }
}

/*
 * The browser's buttons, after moving the selection: P reviews the queue,
 * C queues the selected disc (in the demo, connects to the ODE; with no
 * ODE, opens the demo), X swaps cover and preview or reloads the folder,
 * B goes up a folder and A opens or plays the selection.
 */
static void browser_input(Menu *menu, u32 pressed, u32 buttons)
{
    if ((pressed & ControlStart) && !menu->demo &&
        (menu->ready || menu->playlist.count || menu->ode.identity[0])) {
        review_queue(menu);
    } else if (pressed & ControlC) {
        if (menu->demo)
            connect_ode(menu);
        else if (!menu->ready)
            show_demo(menu);
        else
            queue_selected_disc(menu);
    } else if ((pressed & ControlX) && !*menu->filter && toggle_preview(menu)) {
        /* A tap swaps video and cover. Holding X expands either one. */
    } else if (pressed & ControlX) {
        /* Reload: clears a filter, or reads the folder (and the media
         * indexes) again, or retries connecting. */
        cover_cache_clear(&menu->cover_cache);
        menu->art_indexes_valid = 0;
        if (*menu->filter) {
            menu->filter[0] = 0;
            menu->selected = 0;
            menu->message = NULL;
            selection_changed(menu);
        } else if (menu->demo)
            show_demo(menu);
        else if (menu->ode.identity[0])
            load_directory(menu, menu->selected);
        else
            connect_ode(menu);
    } else if (pressed & ControlB)
        go_back(menu);
    else if (pressed & ControlA) {
        /* Hold C + A: launch only this disc, not the rest of its set. */
        menu->single_disc = (buttons & ControlC) != 0;
        activate(menu);
    }
}

/*
 * One pass of the main loop: reads the controller, hands the buttons to
 * whatever has the focus (help, the full-screen view, Tools, the alphabet
 * strip, the queue review or the browser), draws, and when the browser is
 * idle loads the selected game's cover and preview. Each pass ends with
 * one wait for the next video frame.
 */
static void main_loop_pass(Menu *menu, Pad *pad)
{
    size_t total, *selection, old_selection;
    int old_review;
    u32 pressed;
    if (menu->launching) {
        /* The ODE is rebooting the console into a game. */
        WaitVBL(menu->vbl, 1);
        return;
    }
    fade_step(menu);
    if (read_pad(menu, pad) < 0) {
        menu->message = "Controller read failed";
        next_frame(menu);
        return;
    }
    if (!menu->full_view && help_input(menu, pad)) {
        next_frame(menu);
        return;
    }
    if (menu->full_view) {
        full_view_input(menu, pad);
        next_frame(menu);
        return;
    }
    pressed = media_x_input(menu, pad->buttons, pad->pressed, pad->released);
    if (menu->full_view) {
        next_frame(menu);
        return;
    }

    /* L+R opens and closes Tools. */
    observe_tools_chord(menu, pad->buttons);
    pressed = shoulder_input(menu, pad->buttons, pressed, pad->released);
    if (pressed & ~ControlX)
        stop_preview(menu);
    if (dispatch_tools_request(menu))
        pressed = 0;
    if (menu->tools) {
        tools_input(menu, pressed);
        next_frame(menu);
        return;
    }

    /* After a transport failure, only B (to try returning to the library)
     * does anything. */
    if (menu->ode.navigation_failed) {
        if ((pressed & ControlB) && can_recover_library(menu))
            recover_library(menu);
        else
            menu->message = can_recover_library(menu) ? "Folder I/O failed. B:Return to library."
                                                      : "Connection lost. Power cycle console.";
        next_frame(menu);
        return;
    }

    /* A held Up/Down stops at the ends of the list: only a fresh press
     * moves on to the alphabet strip (or wraps the queue). */
    if (pad->repeated & (ControlUp | ControlDown)) {
        size_t rows = row_count(menu), at = menu->review ? menu->queue_selected : menu->selected;
        if (menu->alphabet_active || ((pad->repeated & ControlUp) && !at))
            pressed &= ~(pad->repeated & ControlUp);
        if (menu->alphabet_active || ((pad->repeated & ControlDown) && at + 1 >= rows))
            pressed &= ~(pad->repeated & ControlDown);
    }
    if (alphabet_input(menu, pressed)) {
        next_frame(menu);
        return;
    }
    pressed = browser_shortcuts(menu, pad->buttons, pressed, pad->released);
    if (menu->tools) {
        next_frame(menu);
        return;
    }

    /* The list, or the queue review. */
    total = row_count(menu);
    selection = menu->review ? &menu->queue_selected : &menu->selected;
    old_selection = *selection;
    old_review = menu->review;
    if (menu->review && menu->queue_move && total && (pressed & (ControlUp | ControlDown))) {
        /* Moving a queued disc up or down the queue. */
        size_t to = menu->queue_selected;
        if ((pressed & ControlUp) && to)
            --to;
        if ((pressed & ControlDown) && to + 1 < total)
            ++to;
        if (!ode_playlist_move_local(&menu->playlist, menu->queue_selected, to))
            menu->queue_selected = to;
        pressed &= ~(ControlUp | ControlDown);
    }
    if ((menu->ready || menu->review) && total)
        move_selection(menu, pressed, selection, total);
    if (menu->review)
        queue_review_input(menu, pressed, pad->buttons);
    else
        browser_input(menu, pressed, pad->buttons);
    if (menu->launching) {
        WaitVBL(menu->vbl, 1);
        return;
    }
    if (old_review != menu->review ||
        old_selection != (menu->review ? menu->queue_selected : menu->selected))
        selection_changed(menu);

    /* Show the new selection before allocating artwork or touching USB. */
    draw(menu);
    remember_position(menu, 0);
    /* A shoulder may be the first half of L+R. Do not start blocking
     * media I/O while it is held and the other shoulder is on its way, nor
     * while a held direction is still moving through the list. */
    if (!(pad->buttons & (ControlLeftShift | ControlRightShift | DIRECTIONS)) &&
        !menu->tools_requested && !menu->help_hold.pending) {
        update_cover(menu);
        update_preview(menu);
    }
    WaitVBL(menu->vbl, 1);
}

/* Starts the menu: settings at their defaults until the drive's are read,
 * the console set up, the splash shown while connecting to the ODE (or the
 * demo, in the demo build), then the main loop, which never ends: the menu
 * stops when the console reboots into a game. */
int main(int argc, char **argv)
{
    static Menu menu;
    Pad pad;
    (void)argc;
    (void)argv;
    memset(&pad, 0, sizeof(pad));
    menu_categories_reset();
    menu_config_defaults(&menu.config);
    menu.settings_usb_covers = menu.config.usb_covers;
    menu.settings_remember_position = menu.config.remember_position;
    menu.settings_recent_games = menu.config.recent_games;
    menu.settings_video_previews = menu.config.video_previews;
    menu.saved_config = menu.config;
    menu.config_result = 3; /* not attempted */
    menu.bus.device = menu.bus.request = menu.bus.timer = -1;
    menu.ode.transfer = menu_transfer;
    menu.ode.context = &menu.bus;
    menu.ode.progress = progress;
    menu.ode.progress_context = &menu;
    if (open_console(&menu) < 0)
        return 1;
    menu.boot_at = now_ms(&menu);
    /* Copies between the screen buffers for full-screen streamed video;
     * without one, each such frame is drawn whole. */
    menu.sport = GetVRAMIOReq();
    load_menu_art(&menu);
#ifdef MENU_DEMO
    show_demo(&menu);
#else
    connect_ode(&menu);
#endif
    for (;;)
        main_loop_pass(&menu, &pad);
}
