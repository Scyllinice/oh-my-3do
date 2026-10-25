#include "menu_help.h"
static const char *const titles[HELP_TOPIC_COUNT] = {"Game library",
                                                     "Disc queue",
                                                     "Alphabet strip",
                                                     "Tools",
                                                     "Console saves",
                                                     "USB backup slots",
                                                     "Choose destination slot",
                                                     "Backup contents",
                                                     "Hex editor",
                                                     "Editing a byte",
                                                     "Known values & cheats",
                                                     "Settings",
                                                     "Category visibility",
                                                     "Text keyboard",
                                                     "Favorites",
                                                     "Recent games",
                                                     "Search results",
                                                     "Compare backups",
                                                     "Byte differences",
                                                     "Confirm action",
                                                     "Diagnostics",
                                                     "Game details",
                                                     "Reorder favorites",
                                                     "Jump to slot",
                                                     "General controls",
                                                     "Find backup slots",
                                                     "Review backup contents",
                                                     "Choose comparison slot"};
static const char *const lines[HELP_TOPIC_COUNT][MENU_HELP_ROWS] = {
    {"Up/Down: Select a game", "Left/Right or L/R: Page", "A: Open folder / play game",
     "B: Parent folder", "C: Add selected disc to queue", "Tap P: Queue; Hold C + P: Details",
     "Tap X: Cover/video or refresh", "Hold X: Full-screen media", "Hold C + Up: Alphabet strip",
     "L+R together: Tools"},
    {"Up/Down: Select a queued disc", "A or tap P: Play queue", "B: Back (or finish moving)",
     "C: Toggle disc move mode", "Move mode: Up/Down reorders", "X: Remove selected disc",
     "Hold C + X: Clear queue", "The queue is for one multi-disc game.", "L+R together: Tools", ""},
    {"Left/Right: Choose a letter", "A: Jump to that letter", "Down: Return to game list",
     "B: Cancel letter navigation", "Only available letters can be chosen.", "", "", "", "", ""},
    {"Arrows: Choose a tool", "A: Open selected tool", "Hold C + P in library: Game details",
     "B: Return to library", "L+R: Return to library", "Hold P on any menu: Screen help",
     "Tap P keeps its normal action.", "", "", ""},
    {"Up/Down: Select an NVRAM save", "A: Back up only selected save",
     "C: Back up the entire NVRAM", "X: Refresh console save list", "B: Back",
     "A single-save backup merges into slot.", "A full backup replaces the slot.",
     "Left/Right: Switch to USB backups", "", ""},
    {"Up/Down: Select a slot", "L/R: Slot pages; Left/Right: View", "A: Open selected slot",
     "C: Name the slot", "X: Compare with another slot", "Tap P: Find slots / jump by number",
     "B: Back", "Slot search uses the drive catalog.", "", ""},
    {"Up/Down: Choose destination slot", "L/R: Page through destination slots",
     "A: Review and confirm destination", "Tap P: Find slots / jump by number",
     "B: Cancel without writing", "Single saves merge by filename.",
     "Full backups replace the whole slot.", "Review the confirmation before A.", "", ""},
    {"Up/Down: Select a save", "A: Restore selected save to NVRAM", "C: Open raw hex editor",
     "P: Copy selected save to another slot", "X: Open known cheats for this save",
     "B: Return to slot list", "Restore keeps a recovery backup.", "", "", ""},
    {"Arrows: Move between bytes", "L/R: Page through bytes", "A: Edit selected byte",
     "C: Save edited save into a slot", "B: Back / review unsaved edits",
     "Edits stay in the USB draft.", "Raw edits may need game checksums.", "X: Open known cheats",
     "", ""},
    {"Left/Right: Choose high/low nibble", "Up/Down: Change nibble value", "A: Apply byte to draft",
     "B: Cancel this byte edit", "Applying does not write the slot.",
     "Use C outside byte edit to save.", "", "", "", ""},
    {"Up/Down: Select preset or field", "A: Apply selected preset",
     "Left/Right: Change field value", "C: Save edited save into a slot",
     "X: Switch to raw hex editor", "B: Back", "Changes remain in the USB draft.",
     "Verified profiles maintain checksums.", "", ""},
    {"Up/Down: Choose a setting", "A: Toggle / open selected setting",
     "Left/Right: Adjust selected value", "C: Save settings to USB", "X: Discard unsaved settings",
     "B: Back", "Categories can be shown or hidden.", "", "", ""},
    {"Up/Down: Choose a category", "A: Toggle shown / hidden", "B: Return to Settings",
     "Visibility saves immediately.", "Hidden games remain on the drive.", "", "", "", "", ""},
    {"Arrows: Choose a character", "A: Add selected character", "X: Erase last character",
     "C: Clear text", "Tap P: Search or save this text", "B: Cancel / back",
     "Hold P: Help without submitting.", "", "", ""},
    {"Up/Down: Select favorite", "A: Open its containing game folder",
     "C: Add current browser game", "X: Remove selected favorite",
     "R: Reorder favorites; P: Reload", "B: Back", "", "", "", ""},
    {"Up/Down: Select a recent game", "A: Open its containing folder", "P: Reload recent list",
     "B: Back", "", "", "", "", "", ""},
    {"Up/Down: Select a result", "A: Open its containing folder", "B: Return to search", "", "", "",
     "", "", "", ""},
    {"Up/Down: Select a save difference", "A: Show changed byte values", "B: Return to slot list",
     "Added/removed are relative to first slot.", "Comparing does not change saves.", "", "", "",
     "", ""},
    {"Left/Right: Page byte differences", "B: Return to comparison list",
     "-- means the byte is absent.", "Comparing does not change saves.", "", "", "", "", "", ""},
    {"A: Confirm the displayed operation", "B: Cancel without writing",
     "Read the destination and scope.", "Help does not confirm the operation.", "", "", "", "", "",
     ""},
    {"Left/Right: More diagnostic pages", "C: Export diagnostics to USB",
     "X: Test return to the library", "B: Back", "Export needs a working connection.",
     "A transport timeout needs a reboot.", "", "", "", ""},
    {"Left/Right: More detail pages", "B: Return to previous menu",
     "Details do not change game files.", "", "", "", "", "", "", ""},
    {"Up/Down: Move selected favorite", "A or R: Save new order", "B: Cancel reordering", "", "",
     "", "", "", "", ""},
    {"Left/Right: Choose a digit", "A: Add digit to slot number", "X: Erase last digit",
     "C: Clear number", "Tap P: Go to that slot", "B: Cancel", "Hold P: Help without jumping.", "",
     "", ""},
    {"Left/Right: More help pages", "B: Return to Tools", "Hold P: Help for the current screen",
     "Tap P keeps its normal action.", "", "", "", "", "", ""},
    {"Up/Down: Choose a search option", "A: Open selected option", "B: Return to slots",
     "Search uses the drive save catalog.", "", "", "", "", "", ""},
    {"Up/Down: Review saves", "C: Switch existing / new contents", "A: Continue to confirmation",
     "B: Return to slots", "Nothing is written on this screen.", "", "", "", "", ""},
    {"Up/Down: Select comparison slot", "L/R: Page through slots", "A: Compare with first slot",
     "B: Cancel comparison", "Comparing does not change saves.", "", "", "", "", ""}};
const char *menu_help_title(MenuHelpTopic topic)
{
    return (unsigned int)topic < HELP_TOPIC_COUNT ? titles[topic] : "Controls";
}
const char *menu_help_line(MenuHelpTopic topic, unsigned int row)
{
    return (unsigned int)topic < HELP_TOPIC_COUNT && row < MENU_HELP_ROWS ? lines[topic][row] : "";
}
int menu_help_poll(MenuHelpHold *h, unsigned long now, int down, int pressed, int released,
                   int other)
{
    if (h->active)
        return 3;
    if (pressed && !other) {
        h->pending = 1;
        h->since = now;
    }
    if (h->pending && other) {
        h->pending = 0;
        return 0;
    }
    if (h->pending && (down || released) && now - h->since >= MENU_HELP_HOLD_MS) {
        h->pending = 0;
        h->active = 1;
        return 2;
    }
    if (h->pending && released) {
        h->pending = 0;
        return 1;
    }
    if (!down && !released)
        h->pending = 0;
    return 0;
}
