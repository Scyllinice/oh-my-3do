/*
 * Reading categories.json and switching categories on and off.
 * See menu_categories.h for the file and why it is edited in place.
 *
 * The menu has no JSON library, so this file contains a small one. It does
 * not build a tree of objects: it records where each JSON value starts and
 * ends in the text (a "token"), and looks up fields by walking those tokens.
 * That is enough to read the few fields the menu needs, and keeps memory use
 * fixed.
 */
#include "menu_categories.h"
#include "menu_ui.h"
#include <string.h>

/* The most JSON values (objects, arrays, strings, numbers...) a file may hold. */
#define MAX_TOKENS 512

/* Returned by find_field when there is no such field (or it is ambiguous). */
#define NO_TOKEN MAX_TOKENS

/* How deeply objects and arrays may nest, to keep the recursion bounded. */
#define MAX_NESTING 12

/* The longest category name, which is also a folder name on the drive. */
#define CATEGORY_NAME_CHARS 48

/*
 * One JSON value in the text: its kind and where it lies.
 *
 * Tokens are stored in the order their values start, so an object or array
 * is followed directly by the tokens of everything inside it. `next` is the
 * index just past the value's last inner token, which is where its next
 * sibling starts; following `next` skips over a whole value at once.
 */
typedef struct Token {
    size_t start; /* offset of the value's first character */
    size_t end;   /* offset just past its last character */
    size_t next;  /* index of the token after this value and everything in it */
    int type; /* the value's first character: '{', '[', '"', or another for true/false/null/numbers */
} Token;

typedef struct Category {
    char name[CATEGORY_NAME_CHARS + 1];
    /* Where this category's "hidden" value (true or false) starts in the
     * text, so menu_category_toggle can rewrite it in place. */
    size_t hidden_offset;
    int hidden;
} Category;

/* A complete set of categories, together with the text they were read from. */
typedef struct Categories {
    char text[MENU_CATEGORIES_BYTES];
    Category rows[MENU_CATEGORIES_MAX];
    size_t count;
    int configured; /* 1 when read from the drive, 0 for the defaults */
} Categories;

/*
 * The categories in use, and a scratch set that a new file is read into.
 * Only a file that reads completely is copied into `current`, so a damaged
 * file never leaves the menu with half its categories.
 */
static Categories current, pending;

/* The tokenizer's state while reading one file. */
static Token tokens[MAX_TOKENS];
static size_t token_count;
static const char *input; /* the text being read */
static size_t input_length;
static size_t cursor; /* the next character to read */

/* Whether `path` starts with `name`, ignoring ASCII case. */
static int starts_with_ignoring_case(const char *path, const char *name)
{
    while (*name) {
        unsigned char a = (unsigned char)*path++;
        unsigned char b = (unsigned char)*name++;
        if (a >= 'A' && a <= 'Z')
            a += (unsigned char)('a' - 'A');
        if (b >= 'A' && b <= 'Z')
            b += (unsigned char)('a' - 'A');
        if (a != b)
            return 0;
    }
    return 1;
}

/* Whether `path` starts with the folder `name`, as a whole folder name:
 * followed by a slash or the end of the path. */
static int starts_with_folder(const char *path, const char *name)
{
    size_t length = strlen(name);
    return starts_with_ignoring_case(path, name) && (path[length] == 0 || path[length] == '/');
}

/* Moves the cursor past spaces, tabs and line breaks. */
static void skip_whitespace(void)
{
    while (cursor < input_length && (input[cursor] == ' ' || input[cursor] == '\n' ||
                                     input[cursor] == '\r' || input[cursor] == '\t'))
        ++cursor;
}

/*
 * Moves the cursor past the rest of a string, whose opening quote has just
 * been read, and past its closing quote. Returns 0, or -1 if the string is
 * invalid JSON or never ends.
 */
static int skip_string(void)
{
    while (cursor < input_length && input[cursor] != '"') {
        unsigned char c = (unsigned char)input[cursor++];
        if (c < 32)
            return -1; /* JSON strings may not contain raw control characters */
        if (c == '\\') {
            /* An escape: \" \\ \/ \b \f \n \r \t, or \u followed by four hex digits. */
            if (cursor >= input_length)
                return -1;
            c = (unsigned char)input[cursor++];
            if (!strchr("\"\\/bfnrtu", c))
                return -1;
            if (c == 'u') {
                unsigned int i;
                for (i = 0; i < 4; ++i) {
                    if (cursor >= input_length ||
                        !strchr("0123456789abcdefABCDEF", input[cursor++]))
                        return -1;
                }
            }
        }
    }
    if (cursor >= input_length)
        return -1;
    ++cursor; /* the closing quote */
    return 0;
}

/* Moves the cursor past true, false, null or an integer. Returns 0, or -1. */
static int skip_literal(void)
{
    size_t remaining = input_length - cursor;
    const char *here = input + cursor;

    if (remaining >= 4 && !memcmp(here, "true", 4)) {
        cursor += 4;
    } else if (remaining >= 5 && !memcmp(here, "false", 5)) {
        cursor += 5;
    } else if (remaining >= 4 && !memcmp(here, "null", 4)) {
        cursor += 4;
    } else {
        /* An integer, possibly negative. (The file never needs fractions.) */
        if (input[cursor] == '-')
            ++cursor;
        if (cursor >= input_length || input[cursor] < '0' || input[cursor] > '9')
            return -1;
        while (cursor < input_length && input[cursor] >= '0' && input[cursor] <= '9')
            ++cursor;
    }
    return 0;
}

static int read_value(unsigned int depth);

/*
 * Reads the members of an object or the elements of an array, whose opening
 * bracket has just been read, through its closing bracket. In an object each
 * member is a string key, a colon and a value. Returns 0, or -1.
 */
static int read_container(char opening, unsigned int depth)
{
    char closing = opening == '{' ? '}' : ']';

    skip_whitespace();
    if (cursor < input_length && input[cursor] == closing) {
        ++cursor; /* empty: {} or [] */
        return 0;
    }

    for (;;) {
        if (opening == '{') {
            /* The key, which must be a string, then a colon. */
            skip_whitespace();
            if (cursor >= input_length || input[cursor] != '"' || read_value(depth + 1) < 0)
                return -1;
            skip_whitespace();
            if (cursor >= input_length || input[cursor++] != ':')
                return -1;
        }
        if (read_value(depth + 1) < 0)
            return -1;

        /* After each item: a comma and another item, or the closing bracket. */
        skip_whitespace();
        if (cursor >= input_length)
            return -1;
        if (input[cursor] == closing) {
            ++cursor;
            return 0;
        }
        if (input[cursor++] != ',')
            return -1;
    }
}

/*
 * Reads one JSON value at the cursor, recording a token for it (and, through
 * recursion, for everything inside it). Returns the value's token index, or
 * -1 if the text is not valid JSON or is too large or deep for the menu.
 */
static int read_value(unsigned int depth)
{
    size_t slot, start;
    char first;
    int result;

    if (depth > MAX_NESTING || token_count >= MAX_TOKENS)
        return -1;
    skip_whitespace();
    if (cursor >= input_length)
        return -1;

    /* Claim this value's token before reading anything inside it, so the
     * tokens end up in the order the values start. */
    slot = token_count++;
    start = cursor;
    first = input[cursor++];
    tokens[slot].type = first;

    if (first == '"') {
        result = skip_string();
    } else if (first == '{' || first == '[') {
        result = read_container(first, depth);
    } else {
        cursor = start; /* literals are read from their first character */
        result = skip_literal();
    }
    if (result < 0)
        return -1;

    tokens[slot].start = start;
    tokens[slot].end = cursor;
    tokens[slot].next = token_count;
    return (int)slot;
}

/* Whether token `index` is the string `text` (with no escapes). */
static int token_is_string(size_t index, const char *text)
{
    size_t length = strlen(text);
    return tokens[index].type == '"' && tokens[index].end - tokens[index].start == length + 2 &&
           !memcmp(input + tokens[index].start + 1, text, length);
}

/*
 * Finds the value of field `key` in the object at token `object`. Returns
 * the value's token, or NO_TOKEN if the object lacks the field, has it more
 * than once, or is not an object.
 */
static size_t find_field(size_t object, const char *key)
{
    size_t key_token = object + 1; /* the first member's key follows the object */
    size_t found = NO_TOKEN;

    if (tokens[object].type != '{')
        return NO_TOKEN;

    while (key_token < tokens[object].next) {
        size_t value_token = key_token + 1;
        if (token_is_string(key_token, key)) {
            if (found != NO_TOKEN)
                return NO_TOKEN; /* a repeated key is ambiguous */
            found = value_token;
        }
        /* Skip the whole value to reach the next member's key. */
        key_token = tokens[value_token].next;
    }
    return found;
}

/*
 * Whether a category name is usable as a folder name on a FAT drive: printable
 * ASCII without the characters FAT forbids, not starting with a space or
 * ending with a space or dot, and not the menu's own .ohmy3do folder.
 */
static int is_valid_category_name(const char *name, size_t length)
{
    size_t i;
    for (i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (c < 32 || c >= 127 || strchr("<>:\"/\\|?*", c))
            return 0;
    }
    if (name[0] == ' ' || name[length - 1] == ' ' || name[length - 1] == '.')
        return 0;
    return menu_path_compare(name, ".ohmy3do") != 0;
}

/*
 * Reads one category object (token `object`) into the next row of `pending`.
 * Returns 0, or -1 if it is invalid or repeats another category's name.
 */
static int read_category(size_t object)
{
    size_t name_token, hidden_token, id_token, name_length, i;
    Category *row;

    if (pending.count >= MENU_CATEGORIES_MAX || tokens[object].type != '{')
        return -1;

    /* Each category needs a string name, a string id and a hidden flag.
     * (The id is used by the desktop app; the menu only checks it exists.) */
    name_token = find_field(object, "name");
    hidden_token = find_field(object, "hidden");
    id_token = find_field(object, "id");
    if (name_token == NO_TOKEN || hidden_token == NO_TOKEN || id_token == NO_TOKEN ||
        tokens[name_token].type != '"' || tokens[id_token].type != '"')
        return -1;

    /* The name, without its quotes. */
    name_length = tokens[name_token].end - tokens[name_token].start - 2;
    if (name_length == 0 || name_length > CATEGORY_NAME_CHARS)
        return -1;
    row = &pending.rows[pending.count];
    memcpy(row->name, input + tokens[name_token].start + 1, name_length);
    if (!is_valid_category_name(row->name, name_length))
        return -1;
    for (i = 0; i < pending.count; ++i) {
        if (!menu_path_compare(row->name, pending.rows[i].name))
            return -1; /* two categories may not share a folder */
    }

    /* "hidden" must be exactly true or false. */
    row->hidden_offset = tokens[hidden_token].start;
    if (tokens[hidden_token].end - row->hidden_offset == 4 &&
        !memcmp(input + row->hidden_offset, "true", 4))
        row->hidden = 1;
    else if (tokens[hidden_token].end - row->hidden_offset != 5 ||
             memcmp(input + row->hidden_offset, "false", 5))
        return -1;

    ++pending.count;
    return 0;
}

int menu_categories_parse(const char *text, size_t length)
{
    size_t categories_token, schema_token, i;

    if (!text || length == 0 || length >= MENU_CATEGORIES_BYTES)
        return -1;

    /* Tokenize the whole file: it must be one object and nothing more. */
    input = text;
    input_length = length;
    cursor = 0;
    token_count = 0;
    if (read_value(0) != 0 || tokens[0].type != '{')
        return -1;
    skip_whitespace();
    if (cursor != length)
        return -1;

    /* Only version 1 of the format is understood. */
    schema_token = find_field(0, "schema");
    categories_token = find_field(0, "categories");
    if (schema_token == NO_TOKEN || tokens[schema_token].end - tokens[schema_token].start != 1 ||
        input[tokens[schema_token].start] != '1')
        return -1;
    if (categories_token == NO_TOKEN || tokens[categories_token].type != '[')
        return -1;

    /* Read each element of the "categories" array into `pending`. */
    memset(&pending, 0, sizeof(pending));
    for (i = categories_token + 1; i < tokens[categories_token].next; i = tokens[i].next) {
        if (read_category(i) < 0)
            return -1;
    }
    if (pending.count == 0)
        return -1;

    /* Everything checked out: keep the text (for writing back) and use it. */
    memcpy(pending.text, text, length);
    pending.configured = 1;
    current = pending;
    return 0;
}

/* Appends `text` to the default categories text being built. */
static void append(const char *text)
{
    strcat(current.text, text);
}

void menu_categories_reset(void)
{
    static const char *const default_names[] = {"Games",        "Demos",      "Samplers",
                                                "Applications", "Multimedia", "Educational",
                                                "Bonus Discs",  "Adult"};
    const size_t default_count = sizeof(default_names) / sizeof(default_names[0]);
    char digit[2]; /* the category number as a one-character string */
    size_t i;

    /*
     * Build the same text the desktop app writes for a new drive, so the
     * defaults go through the same reader (and toggling works the same way):
     *   {"schema":1,"categories":[{"id":"category-0","name":"Games",
     *    "hidden":false},...],"defaults":{"Games":"category-0",...}}
     */
    memset(&current, 0, sizeof(current));
    digit[1] = 0;
    append("{\"schema\":1,\"categories\":[");
    for (i = 0; i < default_count; ++i) {
        digit[0] = (char)('0' + i);
        if (i > 0)
            append(",");
        append("{\"id\":\"category-");
        append(digit);
        append("\",\"name\":\"");
        append(default_names[i]);
        append("\",\"hidden\":false}");
    }
    append("],\"defaults\":{");
    for (i = 0; i < default_count; ++i) {
        digit[0] = (char)('0' + i);
        if (i > 0)
            append(",");
        append("\"");
        append(default_names[i]);
        append("\":\"category-");
        append(digit);
        append("\"");
    }
    append("}}");

    menu_categories_parse(current.text, strlen(current.text));
    current.configured = 0; /* these are defaults, not the drive's file */
}

size_t menu_categories_count(void)
{
    return current.count;
}

const char *menu_category_name(size_t index)
{
    return index < current.count ? current.rows[index].name : "";
}

int menu_category_hidden(size_t index)
{
    return index < current.count && current.rows[index].hidden;
}

int menu_categories_configured(void)
{
    return current.configured;
}

const char *menu_categories_text(void)
{
    return current.text;
}

int menu_category_toggle(size_t index)
{
    /* Static: too big for the console's small stack. */
    static char edited[MENU_CATEGORIES_BYTES];
    const Category *row;
    size_t offset, length, old_length, new_length;
    const char *new_value;

    if (index >= current.count)
        return -1;
    row = &current.rows[index];

    /* Swap "true" (4 characters) for "false" (5), or the other way round. */
    offset = row->hidden_offset;
    old_length = row->hidden ? 4 : 5;
    new_value = row->hidden ? "false" : "true";
    new_length = row->hidden ? 5 : 4;
    length = strlen(current.text);
    if (length - old_length + new_length >= sizeof(current.text))
        return -1;

    memcpy(edited, current.text, offset);
    memcpy(edited + offset, new_value, new_length);
    strcpy(edited + offset + new_length, current.text + offset + old_length);

    /* Reading the edited text back updates every row (and every offset). */
    return menu_categories_parse(edited, length - old_length + new_length);
}

int menu_categories_hidden_name(const char *name)
{
    size_t i;
    for (i = 0; i < current.count; ++i) {
        if (current.rows[i].hidden && !menu_path_compare(name, current.rows[i].name))
            return 1;
    }
    return 0;
}

int menu_categories_hidden_path(const char *path)
{
    int starts_with_category = 0;
    size_t i;

    /* Accept "Category/..." or "/Storage/..." but no other absolute path. */
    if (!strncmp(path, "/Storage/", 9))
        path += 9;
    else if (*path == '/')
        return 0;

    /* Does the path start with any category at all, hidden or not? */
    for (i = 0; i < current.count; ++i) {
        if (starts_with_folder(path, current.rows[i].name))
            starts_with_category = 1;
    }

    for (i = 0; i < current.count; ++i) {
        const char *after_first_folder;

        if (!current.rows[i].hidden)
            continue;
        if (starts_with_folder(path, current.rows[i].name))
            return 1;

        /*
         * The FCare may show a volume folder before the categories
         * (/Storage/VOLUME/Games/...). If the path does not start with a
         * category, try again after its first folder.
         */
        after_first_folder = strchr(path, '/');
        if (!starts_with_category && after_first_folder &&
            starts_with_folder(after_first_folder + 1, current.rows[i].name))
            return 1;
    }
    return 0;
}
