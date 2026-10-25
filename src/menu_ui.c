/*
 * Text helpers for the game list. See menu_ui.h.
 *
 * Disc images are usually named the way the Redump and No-Intro projects
 * name them: "Title (Region) (Languages) (Other tags).iso". The menu shows
 * the title and any tags that tell versions apart, such as "(Rev 1)" or
 * "(Disc 2)", but hides region and language tags, since the folder
 * structure (Games/USA/...) already says which region a game is.
 */
#include "menu_ui.h"
#include <string.h>

/* The longest heading menu_breadcrumb writes, not counting the zero byte. */
#define BREADCRUMB_CHARS 34

/* Converts an ASCII capital letter to lower case; other bytes are unchanged. */
static unsigned char to_lower_ascii(unsigned char c)
{
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c;
}

int menu_path_compare(const char *a, const char *b)
{
    while (*a && to_lower_ascii((unsigned char)*a) == to_lower_ascii((unsigned char)*b)) {
        ++a;
        ++b;
    }
    return (int)to_lower_ascii((unsigned char)*a) - (int)to_lower_ascii((unsigned char)*b);
}

/* Whether `path` starts with `prefix`, ignoring case. */
static int starts_with_ignoring_case(const char *path, const char *prefix)
{
    while (*prefix &&
           to_lower_ascii((unsigned char)*path) == to_lower_ascii((unsigned char)*prefix)) {
        ++path;
        ++prefix;
    }
    return *prefix == 0;
}

/* Whether `name` ends with `extension` (four characters, like ".iso"), ignoring case. */
static int has_extension(const char *name, const char *extension)
{
    size_t length = strlen(name);
    return length >= 4 && !menu_path_compare(name + length - 4, extension);
}

const char *menu_image_format(const char *name, int directory)
{
    /* A name must have something before the extension to be a disc. */
    if (directory || strlen(name) <= 4)
        return NULL;
    if (has_extension(name, ".iso"))
        return "ISO";
    if (has_extension(name, ".cue"))
        return "CUE";
    return NULL;
}

size_t menu_title_length(const char *name, int directory)
{
    size_t length = strlen(name);
    if (menu_image_format(name, directory))
        length -= 4; /* drop ".iso" or ".cue" */
    return length;
}

/*
 * If the first `end` characters of `name` finish with a tag in round or
 * square brackets, such as " (USA)" or " [b]", returns the position of its
 * opening bracket; otherwise returns 0. The tag must be preceded by a space
 * and must not contain another closing bracket of the same kind.
 */
static size_t find_last_tag(const char *name, size_t end)
{
    size_t open;
    char closing, opening;

    if (end < 4)
        return 0;
    closing = name[end - 1];
    if (closing != ')' && closing != ']')
        return 0;
    opening = closing == ')' ? '(' : '[';

    /* Walk back to the matching opening bracket. */
    for (open = end - 2; open > 1 && name[open] != opening; --open) {
        if (name[open] == closing)
            return 0; /* nested or unbalanced brackets: not a simple tag */
    }

    if (name[open] == opening && name[open - 1] == ' ')
        return open;
    return 0;
}

/*
 * Whether a tag's text (the part inside the brackets, `length` characters)
 * names only regions or languages, as a comma-separated list such as
 * "USA, Europe" or "En,Fr,De". Matching ignores case.
 */
static int is_region_or_language_tag(const char *text, size_t length)
{
    static const char *const words[] = {
        /* Regions and countries */
        "USA", "Europe", "Japan", "Korea", "World", "Asia", "Australia", "Canada", "Germany",
        "France", "Spain", "Italy", "Netherlands", "Brazil", "China", "Taiwan", "UK",
        "United Kingdom", "United States", "Russia", "Sweden", "Norway", "Denmark", "Finland",
        "Portugal", "Scandinavia", "Latin America", "New Zealand", "Hong Kong",
        /* Two-letter language codes */
        "En", "Ja", "Fr", "De", "Es", "It", "Nl", "Pt", "Ko", "Zh", "Ru", "Ar", "Sv", "No", "Da",
        "Fi", "Pl", "Cs", "Sk", "Hu", "He", "El", "Tr",
        /* Language names */
        "English", "Japanese", "French", "German", "Spanish", "Italian", "Dutch", "Portuguese",
        "Korean", "Chinese", "Russian", "Arabic"};
    size_t word_start = 0;

    if (length == 0)
        return 0;

    /* Check each comma-separated word. */
    while (word_start < length) {
        size_t word_end, comma, i;
        int known = 0;

        /* Skip spaces before the word, find the comma after it, and trim
         * spaces before the comma. */
        while (word_start < length && text[word_start] == ' ')
            ++word_start;
        for (comma = word_start; comma < length && text[comma] != ','; ++comma) {
        }
        word_end = comma;
        while (word_end > word_start && text[word_end - 1] == ' ')
            --word_end;

        for (i = 0; i < sizeof(words) / sizeof(words[0]); ++i) {
            size_t word_length = word_end - word_start;
            size_t k;
            if (strlen(words[i]) != word_length)
                continue;
            for (k = 0; k < word_length && to_lower_ascii((unsigned char)text[word_start + k]) ==
                                               to_lower_ascii((unsigned char)words[i][k]);
                 ++k) {
            }
            if (k == word_length) {
                known = 1;
                break;
            }
        }

        /* Every word must be known, and the list must not end with a comma. */
        if (!known || (comma < length && comma + 1 == length))
            return 0;
        word_start = comma + 1;
    }
    return 1;
}

size_t menu_short_title_length(const char *name, int directory)
{
    size_t length = menu_title_length(name, directory);
    size_t open;

    if (directory)
        return length;

    /* Remove region and language tags from the end, one at a time. Each tag
     * runs from its opening bracket at `open` to the bracket at length - 1. */
    while ((open = find_last_tag(name, length)) != 0) {
        if (!is_region_or_language_tag(name + open + 1, length - open - 2))
            break;
        length = open - 1; /* also drop the space before the tag */
    }
    return length;
}

size_t menu_display_title(char *out, size_t capacity, const char *name, int directory)
{
    size_t length = menu_title_length(name, directory);
    size_t tags_start = length; /* where the trailing tags begin (at a space) */
    size_t position, used = 0, i;

    if (capacity == 0)
        return 0;

    /* Find where the run of tags at the end of the name begins. */
    if (!directory) {
        size_t open;
        while ((open = find_last_tag(name, tags_start)) != 0)
            tags_start = open - 1;
    }

    /* Copy the title itself, leaving room for the zero byte. */
    for (i = 0; i < tags_start && used + 1 < capacity; ++i)
        out[used++] = name[i];

    /*
     * Then each tag in turn, as " (Tag)", except region and language tags.
     * `position` is at the space before a tag, so the bracket follows it.
     */
    position = tags_start;
    while (position < length) {
        char closing = name[position + 1] == '(' ? ')' : ']';
        size_t tag_end = position + 2;

        while (tag_end < length && name[tag_end] != closing)
            ++tag_end;

        if (tag_end == length ||
            !is_region_or_language_tag(name + position + 2, tag_end - position - 2)) {
            for (i = position; i <= tag_end && i < length && used + 1 < capacity; ++i)
                out[used++] = name[i];
        }
        position = tag_end + 1;
    }

    out[used] = 0;
    return used;
}

size_t menu_scroll_offset(size_t length, size_t visible, unsigned long elapsed)
{
    /* Timing, in milliseconds. */
    const unsigned long pause_at_start = 1200UL;
    const unsigned long per_character = 180UL;
    const unsigned long pause_at_end = 1000UL;
    unsigned long distance, cycle, phase, scrolled;

    if (length <= visible)
        return 0; /* it fits: no scrolling */

    /* One cycle: pause, scroll `distance` characters, pause, repeat. */
    distance = (unsigned long)(length - visible);
    cycle = pause_at_start + distance * per_character + pause_at_end;
    phase = elapsed % cycle;
    if (phase < pause_at_start)
        return 0;

    scrolled = (phase - pause_at_start) / per_character;
    return scrolled > distance ? (size_t)distance : (size_t)scrolled;
}

void menu_breadcrumb(char *out, const char *parent, const char *leaf, unsigned int depth)
{
    size_t used;

    if (depth == 0) {
        strcpy(out, "/ STORAGE");
        return;
    }

    /* Deeper folders show "..." for the levels above, plus the parent's name
     * when parent and folder fit together. */
    strcpy(out, depth > 1 ? "... / " : "/ ");
    if (depth > 1 && strlen(parent) + strlen(leaf) + 9 <= BREADCRUMB_CHARS) {
        strcat(out, parent);
        strcat(out, " / ");
    }

    /* The folder's own name, cut short with "..." if it does not fit. */
    used = strlen(out);
    strncpy(out + used, leaf, BREADCRUMB_CHARS - used);
    out[BREADCRUMB_CHARS] = 0;
    if (strlen(leaf) > BREADCRUMB_CHARS - used)
        memcpy(out + BREADCRUMB_CHARS - 3, "...", 3);
}

const char *menu_cover_lookup(const char *path)
{
    return menu_cover_find(menu_covers, menu_cover_count, path);
}

const char *menu_cover_find(const MenuCover *covers, size_t count, const char *path)
{
    size_t low = 0, high = count;

    /* Cover paths are relative to the drive root. */
    if (!strncmp(path, "/Storage/", 9))
        path += 9;
    while (*path == '/')
        ++path;

    /* Binary search: halve the range [low, high) until the path is found. */
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        int order = menu_path_compare(path, covers[middle].path);
        if (order == 0)
            return covers[middle].file;
        if (order < 0)
            high = middle;
        else
            low = middle + 1;
    }
    return NULL;
}

/* Set from the "Hide adult" setting; see menu_set_hide_adult. */
static int hide_adult;

void menu_set_hide_adult(int hide)
{
    hide_adult = hide;
}

int menu_adult_path(const char *path)
{
    int had_storage_prefix = 0;

    if (!strncmp(path, "/Storage/", 9)) {
        path += 9;
        had_storage_prefix = 1;
    }
    if (starts_with_ignoring_case(path, "Adult/"))
        return 1;

    /* A full menu path may name a USB volume first: /Storage/VOLUME/Adult/... */
    if (had_storage_prefix) {
        const char *after_volume = strchr(path, '/');
        if (after_volume)
            return starts_with_ignoring_case(after_volume + 1, "Adult/");
    }
    return 0;
}

int menu_support_entry(const char *name, int directory, int root)
{
    /* The menu's own files in the drive root. */
    static const char *const root_files[] = {"boot.iso", "menu-demo.iso", "boot.iso.sha256",
                                             "menu-demo.iso.sha256"};
    size_t i;

    /*
     * The navigation helper is a tiny placeholder ISO the desktop app puts in
     * category folders. The FCare can enter it as a detour when its folder
     * bug would strike (see MENU_WORKAROUND_AUTO), so it must stay on the
     * drive, but nobody should see or launch it.
     */
    if (!directory && !menu_path_compare(name, "Oh My 3DO - Folder Navigation Helper (Keep).iso"))
        return 1;

    /* .bin files are the data tracks of CUE discs; the .cue file is shown instead. */
    if (!directory && has_extension(name, ".bin"))
        return 1;

    if (!root)
        return 0;

    if (directory)
        return !menu_path_compare(name, ".ohmy3do") || menu_categories_hidden_name(name) ||
               (hide_adult && !menu_path_compare(name, "Adult"));

    for (i = 0; i < sizeof(root_files) / sizeof(root_files[0]); ++i) {
        if (!menu_path_compare(name, root_files[i]))
            return 1;
    }
    return 0;
}
