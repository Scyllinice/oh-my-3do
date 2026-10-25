/*
 * Favorites, recent games, name search and multi-disc sets.
 * See library_features.h.
 */
#include "library_features.h"
#include "menu_ui.h"
#include <stdlib.h>
#include <string.h>

/* The UTF-8 "byte order mark" some Windows editors put at the start of a file. */
#define UTF8_BOM_LENGTH 3

/* Converts an ASCII capital letter to lower case; other bytes are unchanged. */
static unsigned char to_lower_ascii(unsigned char c)
{
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + 'a' - 'A') : c;
}

/* Converts an ASCII lower-case letter to a capital; other bytes are unchanged. */
static unsigned char to_upper_ascii(unsigned char c)
{
    return c >= 'a' && c <= 'z' ? (unsigned char)(c - ('a' - 'A')) : c;
}

/* Spaces, tabs and the carriage return of Windows line endings. */
static int is_blank(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

/* Whether a favorite should be listed: not in a hidden category, and not in
 * the Adult category when that is hidden. */
static int is_visible(const char *path, int hide_adult)
{
    return !menu_categories_hidden_path(path) && (!hide_adult || !menu_adult_path(path));
}

size_t menu_favorites_visible_index(const MenuFavorites *favorites, int hide_adult, size_t visible)
{
    size_t i;
    for (i = 0; i < favorites->count; ++i) {
        if (is_visible(favorites->paths[i], hide_adult)) {
            if (visible == 0)
                return i;
            --visible;
        }
    }
    return favorites->count;
}

size_t menu_favorites_visible_count(const MenuFavorites *favorites, int hide_adult)
{
    size_t i, count = 0;
    for (i = 0; i < favorites->count; ++i) {
        if (is_visible(favorites->paths[i], hide_adult))
            ++count;
    }
    return count;
}

int menu_relative_path(const char *path)
{
    const char *p = path;
    const char *name_start = path;

    if (*path == 0 || *path == '/' || strlen(path) >= FAVORITE_PATH)
        return 0;

    /* Walk the path, checking each name when its '/' or the end is reached. */
    for (;; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c != 0 && (c < 32 || c == 127 || c == '\\' || c == ':'))
            return 0;
        if (c == 0 || c == '/') {
            size_t name_length = (size_t)(p - name_start);
            if (name_length == 0 || (name_length == 1 && *name_start == '.') ||
                (name_length == 2 && !memcmp(name_start, "..", 2)))
                return 0;
            if (c == 0)
                return 1;
            name_start = p + 1;
        }
    }
}

void menu_favorites_free(MenuFavorites *favorites)
{
    free(favorites->storage);
    memset(favorites, 0, sizeof(*favorites));
}

int menu_favorites_parse(MenuFavorites *favorites, const char *data, size_t size)
{
    MenuFavorites loaded;
    char *text;
    size_t position = 0;

    memset(&loaded, 0, sizeof(loaded));
    if (size >= FAVORITES_BYTES || memchr(data, 0, size))
        return -1;

    /* Keep a zero-terminated copy; each line is cut out of it in place. */
    loaded.storage = (char *)malloc(size + 1);
    if (!loaded.storage)
        return -1;
    text = loaded.storage;
    memcpy(text, data, size);
    text[size] = 0;

    /* Skip a UTF-8 byte order mark left by a PC text editor. */
    if (size >= UTF8_BOM_LENGTH && (unsigned char)data[0] == 0xef &&
        (unsigned char)data[1] == 0xbb && (unsigned char)data[2] == 0xbf)
        position = UTF8_BOM_LENGTH;

    while (position < size) {
        size_t start = position, end, i;
        const char *path;

        /* Find the line's end, then trim blanks from both ends and end the
         * string there (overwriting the newline or a trailing blank). */
        while (position < size && text[position] != '\n')
            ++position;
        end = position;
        if (position < size)
            ++position;
        while (start < end && is_blank(text[start]))
            ++start;
        while (end > start && is_blank(text[end - 1]))
            --end;
        text[end] = 0;
        path = text + start;

        /* Blank lines and comments. */
        if (start == end || *path == '#' || *path == ';')
            continue;

        /* Every path must be safe, and each may appear only once. */
        if (loaded.count == FAVORITES_MAX || !menu_relative_path(path))
            goto invalid;
        for (i = 0; i < loaded.count; ++i) {
            if (!menu_path_compare(loaded.paths[i], path))
                goto invalid;
        }
        loaded.paths[loaded.count++] = path;
    }

    menu_favorites_free(favorites);
    *favorites = loaded;
    return 0;

invalid:
    menu_favorites_free(&loaded);
    return -1;
}

/*
 * Whether `path` can be written as a line of a list file: a safe relative
 * path that the reader will read back unchanged (it would skip a line
 * starting with '#' or ';', and trim blanks from either end).
 */
static int can_store(const char *path)
{
    return menu_relative_path(path) && *path != '#' && *path != ';' && *path != ' ' &&
           *path != '\t' && path[strlen(path) - 1] != ' ';
}

/* Appends `path` and a newline to `data` (FAVORITES_BYTES long), which holds
 * `*used` bytes. Returns 0, or -1 if it would not fit with room for a zero. */
static int append_line(char *data, size_t *used, const char *path)
{
    size_t length = strlen(path);
    if (*used + length + 1 >= FAVORITES_BYTES)
        return -1;
    memcpy(data + *used, path, length);
    *used += length;
    data[(*used)++] = '\n';
    return 0;
}

int menu_favorites_edit(MenuFavorites *favorites, const char *path, int remove)
{
    char *data;
    size_t i, found = favorites->count, used = 0;
    int result;

    if (!can_store(path))
        return -1;

    for (i = 0; i < favorites->count; ++i) {
        if (!menu_path_compare(favorites->paths[i], path)) {
            found = i;
            break;
        }
    }
    /* Nothing to do: removing a path that is not there, or adding one that is. */
    if (remove ? found == favorites->count : found != favorites->count)
        return 1;
    if (!remove && favorites->count == FAVORITES_MAX)
        return -1;

    /*
     * Write out the edited list as text and read it back in. That way the
     * result is checked exactly as a file from the drive would be, and the
     * list only changes if all of it is valid.
     */
    data = (char *)malloc(FAVORITES_BYTES);
    if (!data)
        return -1;
    for (i = 0; i < favorites->count; ++i) {
        if (remove && i == found)
            continue;
        if (append_line(data, &used, favorites->paths[i]) < 0)
            goto too_big;
    }
    if (!remove && append_line(data, &used, path) < 0)
        goto too_big;
    data[used] = 0;

    result = menu_favorites_parse(favorites, data, used);
    free(data);
    return result;

too_big:
    free(data);
    return -1;
}

int menu_favorites_format(const MenuFavorites *favorites, char *out, size_t capacity)
{
    size_t i, used = 0;

    if (capacity == 0)
        return -1;
    for (i = 0; i < favorites->count; ++i) {
        size_t length = strlen(favorites->paths[i]);
        if (used + length + 1 >= capacity)
            return -1;
        memcpy(out + used, favorites->paths[i], length);
        used += length;
        out[used++] = '\n';
    }
    out[used] = 0;
    return 0;
}

int menu_favorites_move(MenuFavorites *favorites, size_t from, size_t to)
{
    const char *moving;

    if (from >= favorites->count || to >= favorites->count)
        return -1;

    moving = favorites->paths[from];
    if (from < to) {
        /* Moving down: shift the entries after it up by one. */
        memmove(favorites->paths + from, favorites->paths + from + 1,
                (to - from) * sizeof(*favorites->paths));
    } else if (from > to) {
        /* Moving up: shift the entries before it down by one. */
        memmove(favorites->paths + to + 1, favorites->paths + to,
                (from - to) * sizeof(*favorites->paths));
    }
    favorites->paths[to] = moving;
    return 0;
}

int menu_recent_push(MenuFavorites *recent, const char *path)
{
    char *data;
    size_t i, count = 1, used;
    int result;

    /* (Unlike favorites, a leading tab is accepted here, as it always was.) */
    if (!menu_relative_path(path) || *path == '#' || *path == ';' || *path == ' ' ||
        path[strlen(path) - 1] == ' ')
        return -1;

    /* Build the new list as text: this game first, then the others in their
     * old order without this one, up to RECENT_MAX in all. */
    data = (char *)malloc(FAVORITES_BYTES);
    if (!data)
        return -1;
    used = strlen(path);
    memcpy(data, path, used);
    data[used++] = '\n';
    for (i = 0; i < recent->count && count < RECENT_MAX; ++i) {
        if (!menu_path_compare(path, recent->paths[i]))
            continue;
        if (append_line(data, &used, recent->paths[i]) < 0)
            break; /* out of room: keep the newer games that fit */
        ++count;
    }
    data[used] = 0;

    /* Read it back, as menu_favorites_edit does. */
    result = menu_favorites_parse(recent, data, used);
    free(data);
    return result;
}

int menu_name_matches(const char *name, const char *query)
{
    const char *start;

    if (*query == 0)
        return 1;

    /* Try the query at every position in the name. */
    for (start = name; *start; ++start) {
        const char *a = start;
        const char *b = query;
        while (*a && *b && to_upper_ascii((unsigned char)*a) == to_upper_ascii((unsigned char)*b)) {
            ++a;
            ++b;
        }
        if (*b == 0)
            return 1; /* reached the end of the query: it matched here */
    }
    return 0;
}

int menu_initial(const char *name)
{
    unsigned char c = to_upper_ascii((unsigned char)*name);
    return c >= 'A' && c <= 'Z' ? c - 'A' + 1 : 0;
}

/* Characters that separate words in a disc name, and the end of the name. */
static int is_word_separator(char c)
{
    return c == 0 || c == ' ' || c == '_' || c == '-' || c == '(' || c == ')' || c == '[' ||
           c == ']' || c == '.';
}

/*
 * Looks for a disc number in a disc image's name: "Disc 2", "Disk 2" or
 * "CD2" (any case, with an optional space, underscore or hyphen before the
 * number), starting at the beginning of a word and ending at the end of one.
 *
 * If there is exactly one, returns the number (1 to ODE_MAX_DISCS) and
 * writes the name without its extension and without the number into `key`
 * (ODE_MAX_NAME bytes): discs of one set share the same key. Otherwise
 * returns 0.
 */
static int find_disc_number(const char *name, char *key)
{
    size_t length = strlen(name);
    size_t i, number_start = 0, number_end = 0;
    int disc = 0, numbers_found = 0;

    if (length >= ODE_MAX_NAME || length < 5)
        return 0;
    if (menu_path_compare(name + length - 4, ".iso") &&
        menu_path_compare(name + length - 4, ".cue"))
        return 0;
    length -= 4; /* ignore the extension */

    for (i = 0; i < length; ++i) {
        size_t word_length = 0, j;
        int value = 0;

        /* Only look at the start of a word. */
        if (i > 0 && !is_word_separator(name[i - 1]))
            continue;

        if (i + 4 <= length && to_lower_ascii(name[i]) == 'd' &&
            to_lower_ascii(name[i + 1]) == 'i' && to_lower_ascii(name[i + 2]) == 's' &&
            (to_lower_ascii(name[i + 3]) == 'c' || to_lower_ascii(name[i + 3]) == 'k'))
            word_length = 4; /* "disc" or "disk" */
        else if (i + 2 <= length && to_lower_ascii(name[i]) == 'c' &&
                 to_lower_ascii(name[i + 1]) == 'd')
            word_length = 2; /* "cd" */
        if (word_length == 0)
            continue;

        /* An optional separator, then the digits. */
        j = i + word_length;
        while (j < length && (name[j] == ' ' || name[j] == '_' || name[j] == '-'))
            ++j;
        if (j == length || name[j] < '0' || name[j] > '9')
            continue;
        number_start = j;
        while (j < length && name[j] >= '0' && name[j] <= '9') {
            if (value > ODE_MAX_DISCS)
                return 0; /* stop before a long number could overflow */
            value = value * 10 + (name[j] - '0');
            ++j;
        }

        /* The number must end a word, be in range, and be the only one. */
        ++numbers_found;
        if ((j < length && !is_word_separator(name[j])) || value < 1 || value > ODE_MAX_DISCS ||
            numbers_found > 1)
            return 0;
        disc = value;
        number_end = j;
    }

    if (numbers_found != 1)
        return 0;

    /* The key: everything before the number, then everything after it. */
    memcpy(key, name, number_start);
    memcpy(key + number_start, name + number_end, length - number_end);
    key[number_start + length - number_end] = 0;
    return disc;
}

size_t menu_disc_group(const OdeListing *listing, const MenuEntry *selected, const MenuEntry **out,
                       size_t capacity, size_t *position)
{
    char selected_key[ODE_MAX_NAME], key[ODE_MAX_NAME];
    const MenuEntry *by_number[ODE_MAX_DISCS]; /* disc N goes in by_number[N - 1] */
    size_t i, count = 0, highest = 0;
    int selected_number;

    *position = 0;
    if (selected->is_directory)
        return 0;
    selected_number = find_disc_number(selected->name, selected_key);
    if (!selected_number)
        return 0;

    /* Gather every disc in the folder with the same key, by number. */
    memset(by_number, 0, sizeof(by_number));
    for (i = 0; i < listing->count; ++i) {
        const MenuEntry *entry = listing->entries[i];
        int number;

        if (entry->is_directory)
            continue;
        number = find_disc_number(entry->name, key);
        if (!number || menu_path_compare(selected_key, key))
            continue;
        if (by_number[number - 1])
            return 0; /* two discs with the same number: not a clean set */
        by_number[number - 1] = entry;
        ++count;
        if ((size_t)number > highest)
            highest = (size_t)number;
    }

    /* Complete from 1 to the highest number, with no gaps (count == highest). */
    if (count < 2 || count != highest || count > capacity)
        return 0;
    for (i = 0; i < count; ++i)
        out[i] = by_number[i];
    *position = (size_t)selected_number - 1;
    return count;
}
