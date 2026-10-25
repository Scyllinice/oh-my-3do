/*
 * Reading and writing the remembered folder position. See menu_position.h
 * for the file format.
 */
#include "menu_position.h"
#include <string.h>

/* The first line of a position file, and its length in bytes. */
#define POSITION_HEADER        "OH3DO_POSITION1\n"
#define POSITION_HEADER_LENGTH 16

/* The menu's name for the root of the drive. */
#define STORAGE_ROOT        "/Storage"
#define STORAGE_ROOT_LENGTH 8

/*
 * Whether `name` (`length` bytes, not zero-terminated) is safe as one folder
 * or file name in a path: not empty, not too long, not "." or "..", and free
 * of control characters and path separators. A position file can be edited
 * by hand, so it is never trusted to point somewhere unexpected.
 */
static int is_safe_name(const char *name, size_t length)
{
    size_t i;

    if (length == 0 || length >= ODE_MAX_NAME)
        return 0;
    if ((length == 1 && name[0] == '.') || (length == 2 && !memcmp(name, "..", 2)))
        return 0;

    for (i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (c < 32 || c == 127 || c == '/' || c == '\\' || c == ':')
            return 0;
    }
    return 1;
}

/*
 * Whether a position is safe to use: the folder is /Storage or a path below
 * it made only of safe names, and the selection is empty or a safe name.
 */
static int is_valid_position(const MenuPosition *position)
{
    int at_root = strcmp(position->folder, STORAGE_ROOT) == 0;

    if (!at_root) {
        const char *part;

        if (strncmp(position->folder, STORAGE_ROOT "/", STORAGE_ROOT_LENGTH + 1) != 0)
            return 0;

        /* Check each name between the slashes after "/Storage/". */
        part = position->folder + STORAGE_ROOT_LENGTH + 1;
        for (;;) {
            const char *slash = strchr(part, '/');
            size_t part_length = slash ? (size_t)(slash - part) : strlen(part);
            if (!is_safe_name(part, part_length))
                return 0;
            if (!slash)
                break;
            part = slash + 1;
        }
    }

    return position->selected[0] == 0 ||
           is_safe_name(position->selected, strlen(position->selected));
}

int menu_position_parse(MenuPosition *position, const char *text, size_t length)
{
    const char *folder, *folder_end;
    const char *selected, *selected_end;
    size_t folder_length, selected_length;

    memset(position, 0, sizeof(*position));
    if (length == 0)
        return 0; /* no position saved yet */

    /* The whole file must be text, start with the header and hold at least
     * one more character. */
    if (length >= POSITION_BYTES || memchr(text, 0, length) ||
        length < POSITION_HEADER_LENGTH + 1 ||
        memcmp(text, POSITION_HEADER, POSITION_HEADER_LENGTH) != 0)
        return -1;

    /* Line 2: the folder. */
    folder = text + POSITION_HEADER_LENGTH;
    folder_end = (const char *)memchr(folder, '\n', length - POSITION_HEADER_LENGTH);
    if (!folder_end)
        return -1;
    folder_length = (size_t)(folder_end - folder);
    if (folder_length >= sizeof(position->folder))
        return -1;
    memcpy(position->folder, folder, folder_length);

    /* Line 3: the selected name, which must end exactly at the end of the file. */
    selected = folder_end + 1;
    selected_end = (const char *)memchr(selected, '\n', length - (size_t)(selected - text));
    if (!selected_end || selected_end != text + length - 1)
        goto invalid;
    selected_length = (size_t)(selected_end - selected);
    if (selected_length >= sizeof(position->selected))
        goto invalid;
    memcpy(position->selected, selected, selected_length);

    if (is_valid_position(position))
        return 0;

invalid:
    /* Never leave half a position behind. */
    memset(position, 0, sizeof(*position));
    return -1;
}

int menu_position_format(const MenuPosition *position, char *out, size_t capacity)
{
    /* The header, two newlines and the terminating zero need 19 bytes. */
    size_t needed =
        POSITION_HEADER_LENGTH + strlen(position->folder) + 1 + strlen(position->selected) + 1 + 1;

    if (!is_valid_position(position) || needed > capacity)
        return -1;

    strcpy(out, POSITION_HEADER);
    strcat(out, position->folder);
    strcat(out, "\n");
    strcat(out, position->selected);
    strcat(out, "\n");
    return 0;
}
