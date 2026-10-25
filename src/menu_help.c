/*
 * The help text for every screen, and the logic that decides when to show
 * it. See menu_help.h.
 *
 * Each screen has a title and up to MENU_HELP_ROWS lines; lines left out at
 * the end of a list are shown blank. The tables are indexed by
 * MenuHelpTopic, so their order must match the enum in menu_help.h.
 */
#include "menu_help.h"

static const char *const titles[HELP_TOPIC_COUNT] = {
    "Game library",            /* HELP_LIBRARY */
    "Disc queue",              /* HELP_QUEUE */
    "Alphabet strip",          /* HELP_ALPHABET */
    "Tools",                   /* HELP_TOOLS */
    "Console saves",           /* HELP_SAVES */
    "USB backup slots",        /* HELP_SLOTS */
    "Choose destination slot", /* HELP_DESTINATION */
    "Backup contents",         /* HELP_ARCHIVE */
    "Hex editor",              /* HELP_HEX */
    "Editing a byte",          /* HELP_HEX_BYTE */
    "Known values & cheats",   /* HELP_CHEATS */
    "Settings",                /* HELP_SETTINGS */
    "Category visibility",     /* HELP_CATEGORIES */
    "Text keyboard",           /* HELP_KEYBOARD */
    "Favorites",               /* HELP_FAVORITES */
    "Recent games",            /* HELP_RECENT */
    "Search results",          /* HELP_RESULTS */
    "Compare backups",         /* HELP_COMPARE */
    "Byte differences",        /* HELP_COMPARE_BYTES */
    "Confirm action",          /* HELP_CONFIRM */
    "Diagnostics",             /* HELP_DIAGNOSTICS */
    "Game details",            /* HELP_DETAILS */
    "Reorder favorites",       /* HELP_FAVORITE_MOVE */
    "Jump to slot",            /* HELP_SLOT_JUMP */
    "General controls",        /* HELP_GENERAL */
    "Find backup slots",       /* HELP_SLOT_FIND */
    "Review backup contents",  /* HELP_BACKUP_DETAILS */
    "Choose comparison slot",  /* HELP_COMPARE_PICK */
};

static const char *const lines[HELP_TOPIC_COUNT][MENU_HELP_ROWS] = {
    /* HELP_LIBRARY: Game library */
    {
        "Up/Down: Select a game",
        "Left/Right or L/R: Page",
        "A: Open folder / play game",
        "B: Parent folder",
        "C: Add selected disc to queue",
        "Tap P: Queue; Hold C + P: Details",
        "Tap X: Cover/video or refresh",
        "Hold X: Full-screen media",
        "Hold C + Up: Alphabet strip",
        "L+R together: Tools",
    },
    /* HELP_QUEUE: Disc queue */
    {
        "Up/Down: Select a queued disc",
        "A or tap P: Play queue",
        "B: Back (or finish moving)",
        "C: Toggle disc move mode",
        "Move mode: Up/Down reorders",
        "X: Remove selected disc",
        "Hold C + X: Clear queue",
        "The queue is for one multi-disc game.",
        "L+R together: Tools",
    },
    /* HELP_ALPHABET: Alphabet strip */
    {
        "Left/Right: Choose a letter",
        "A: Jump to that letter",
        "Down: Return to game list",
        "B: Cancel letter navigation",
        "Only available letters can be chosen.",
    },
    /* HELP_TOOLS: Tools */
    {
        "Arrows: Choose a tool",
        "A: Open selected tool",
        "Hold C + P in library: Game details",
        "B: Return to library",
        "L+R: Return to library",
        "Hold P on any menu: Screen help",
        "Tap P keeps its normal action.",
    },
    /* HELP_SAVES: Console saves */
    {
        "Up/Down: Select an NVRAM save",
        "A: Back up only selected save",
        "C: Back up the entire NVRAM",
        "X: Refresh console save list",
        "B: Back",
        "A single-save backup merges into slot.",
        "A full backup replaces the slot.",
        "Left/Right: Switch to USB backups",
    },
    /* HELP_SLOTS: USB backup slots */
    {
        "Up/Down: Select a slot",
        "L/R: Slot pages; Left/Right: View",
        "A: Open selected slot",
        "C: Name the slot",
        "X: Compare with another slot",
        "Tap P: Find slots / jump by number",
        "B: Back",
        "Slot search uses the drive catalog.",
    },
    /* HELP_DESTINATION: Choose destination slot */
    {
        "Up/Down: Choose destination slot",
        "L/R: Page through destination slots",
        "A: Review and confirm destination",
        "Tap P: Find slots / jump by number",
        "B: Cancel without writing",
        "Single saves merge by filename.",
        "Full backups replace the whole slot.",
        "Review the confirmation before A.",
    },
    /* HELP_ARCHIVE: Backup contents */
    {
        "Up/Down: Select a save",
        "A: Restore selected save to NVRAM",
        "C: Open raw hex editor",
        "P: Copy selected save to another slot",
        "X: Open known cheats for this save",
        "B: Return to slot list",
        "Restore keeps a recovery backup.",
    },
    /* HELP_HEX: Hex editor */
    {
        "Arrows: Move between bytes",
        "L/R: Page through bytes",
        "A: Edit selected byte",
        "C: Save edited save into a slot",
        "B: Back / review unsaved edits",
        "Edits stay in the USB draft.",
        "Raw edits may need game checksums.",
        "X: Open known cheats",
    },
    /* HELP_HEX_BYTE: Editing a byte */
    {
        "Left/Right: Choose high/low nibble",
        "Up/Down: Change nibble value",
        "A: Apply byte to draft",
        "B: Cancel this byte edit",
        "Applying does not write the slot.",
        "Use C outside byte edit to save.",
    },
    /* HELP_CHEATS: Known values & cheats */
    {
        "Up/Down: Select preset or field",
        "A: Apply selected preset",
        "Left/Right: Change field value",
        "C: Save edited save into a slot",
        "X: Switch to raw hex editor",
        "B: Back",
        "Changes remain in the USB draft.",
        "Verified profiles maintain checksums.",
    },
    /* HELP_SETTINGS: Settings */
    {
        "Up/Down: Choose a setting",
        "A: Toggle / open selected setting",
        "Left/Right: Adjust selected value",
        "C: Save settings to USB",
        "X: Discard unsaved settings",
        "B: Back",
        "Categories can be shown or hidden.",
    },
    /* HELP_CATEGORIES: Category visibility */
    {
        "Up/Down: Choose a category",
        "A: Toggle shown / hidden",
        "B: Return to Settings",
        "Visibility saves immediately.",
        "Hidden games remain on the drive.",
    },
    /* HELP_KEYBOARD: Text keyboard */
    {
        "Arrows: Choose a character",
        "A: Add selected character",
        "X: Erase last character",
        "C: Clear text",
        "Tap P: Search or save this text",
        "B: Cancel / back",
        "Hold P: Help without submitting.",
    },
    /* HELP_FAVORITES: Favorites */
    {
        "Up/Down: Select favorite",
        "A: Open its containing game folder",
        "C: Add current browser game",
        "X: Remove selected favorite",
        "R: Reorder favorites; P: Reload",
        "B: Back",
    },
    /* HELP_RECENT: Recent games */
    {
        "Up/Down: Select a recent game",
        "A: Open its containing folder",
        "P: Reload recent list",
        "B: Back",
    },
    /* HELP_RESULTS: Search results */
    {
        "Up/Down: Select a result",
        "A: Open its containing folder",
        "B: Return to search",
    },
    /* HELP_COMPARE: Compare backups */
    {
        "Up/Down: Select a save difference",
        "A: Show changed byte values",
        "B: Return to slot list",
        "Added/removed are relative to first slot.",
        "Comparing does not change saves.",
    },
    /* HELP_COMPARE_BYTES: Byte differences */
    {
        "Left/Right: Page byte differences",
        "B: Return to comparison list",
        "-- means the byte is absent.",
        "Comparing does not change saves.",
    },
    /* HELP_CONFIRM: Confirm action */
    {
        "A: Confirm the displayed operation",
        "B: Cancel without writing",
        "Read the destination and scope.",
        "Help does not confirm the operation.",
    },
    /* HELP_DIAGNOSTICS: Diagnostics */
    {
        "Left/Right: More diagnostic pages",
        "C: Export diagnostics to USB",
        "X: Test return to the library",
        "B: Back",
        "Export needs a working connection.",
        "A transport timeout needs a reboot.",
    },
    /* HELP_DETAILS: Game details */
    {
        "Left/Right: More detail pages",
        "B: Return to previous menu",
        "Details do not change game files.",
    },
    /* HELP_FAVORITE_MOVE: Reorder favorites */
    {
        "Up/Down: Move selected favorite",
        "A or R: Save new order",
        "B: Cancel reordering",
    },
    /* HELP_SLOT_JUMP: Jump to slot */
    {
        "Left/Right: Choose a digit",
        "A: Add digit to slot number",
        "X: Erase last digit",
        "C: Clear number",
        "Tap P: Go to that slot",
        "B: Cancel",
        "Hold P: Help without jumping.",
    },
    /* HELP_GENERAL: General controls */
    {
        "Left/Right: More help pages",
        "B: Return to Tools",
        "Hold P: Help for the current screen",
        "Tap P keeps its normal action.",
    },
    /* HELP_SLOT_FIND: Find backup slots */
    {
        "Up/Down: Choose a search option",
        "A: Open selected option",
        "B: Return to slots",
        "Search uses the drive save catalog.",
    },
    /* HELP_BACKUP_DETAILS: Review backup contents */
    {
        "Up/Down: Review saves",
        "C: Switch existing / new contents",
        "A: Continue to confirmation",
        "B: Return to slots",
        "Nothing is written on this screen.",
    },
    /* HELP_COMPARE_PICK: Choose comparison slot */
    {
        "Up/Down: Select comparison slot",
        "L/R: Page through slots",
        "A: Compare with first slot",
        "B: Cancel comparison",
        "Comparing does not change saves.",
    },
};

const char *menu_help_title(MenuHelpTopic topic)
{
    if ((unsigned int)topic >= HELP_TOPIC_COUNT)
        return "Controls";
    return titles[topic];
}

const char *menu_help_line(MenuHelpTopic topic, unsigned int row)
{
    if ((unsigned int)topic >= HELP_TOPIC_COUNT || row >= MENU_HELP_ROWS)
        return "";
    /* Rows left out of a topic's list above are null pointers, shown as blank. */
    return lines[topic][row] ? lines[topic][row] : "";
}

int menu_help_poll(MenuHelpHold *hold, unsigned long now, int down, int pressed, int released,
                   int other)
{
    /* Help stays open until the screen closes it and clears `active`. */
    if (hold->active)
        return MENU_HELP_SHOWING;

    /* P went down on its own: start timing, it may become a hold. */
    if (pressed && !other) {
        hold->pending = 1;
        hold->since = now;
    }

    /* Another button joined in: this is a combination such as C + P, which
     * the screen handles itself, so it is neither a tap nor a hold. */
    if (hold->pending && other) {
        hold->pending = 0;
        return MENU_HELP_NOTHING;
    }

    /* Held long enough (whether still down or just released): open help. */
    if (hold->pending && (down || released) && now - hold->since >= MENU_HELP_HOLD_MS) {
        hold->pending = 0;
        hold->active = 1;
        return MENU_HELP_OPEN;
    }

    /* Released before the hold time: an ordinary tap. */
    if (hold->pending && released) {
        hold->pending = 0;
        return MENU_HELP_TAP;
    }

    /* P is not involved this frame: forget any half-finished press. */
    if (!down && !released)
        hold->pending = 0;
    return MENU_HELP_NOTHING;
}
