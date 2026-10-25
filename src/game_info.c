/*
 * Game descriptions: naming and checking the per-game text files the desktop
 * app writes. See game_info.h for the file format.
 */
#include "game_info.h"
#include "usb_art.h"
#include <string.h>

/* The first line of every description file, including its newline. */
#define INFO_HEADER        "OM3DINFO1\n"
#define INFO_HEADER_LENGTH 10

/* The checksum line: eight hex digits and a newline. */
#define CHECKSUM_LINE_LENGTH 9

/*
 * Anything shorter cannot hold all four lines. This is only a quick first
 * check; the line-by-line checks below catch every other bad length.
 */
#define SHORTEST_INFO_FILE 21

/* Converts an ASCII capital letter to lower case; other bytes are unchanged. */
static unsigned char to_lower_ascii(unsigned char c)
{
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c;
}

void game_info_filename(const char *path, char *name)
{
    static const char digits[] = "0123456789abcdef";
    unsigned long crc = 0xffffffffUL; /* the standard CRC32 starting value */
    size_t i;

    /*
     * Checksum the path in lower case, so "Games/Doom.iso" and
     * "games/doom.iso" (the same file on a FAT drive) share one name. The
     * desktop app folds case the same way.
     */
    for (i = 0; path[i]; ++i) {
        unsigned char c = to_lower_ascii((unsigned char)path[i]);
        crc = usb_art_crc32_update(crc, &c, 1);
    }
    crc ^= 0xffffffffUL; /* the standard CRC32 final step */

    /* "info/" + eight hex digits, most significant first + ".txt" */
    strcpy(name, "info/");
    for (i = 0; i < 8; ++i)
        name[5 + i] = digits[(crc >> (28 - i * 4)) & 15];
    strcpy(name + 13, ".txt");
}

/*
 * Reads eight hexadecimal digits (either case) as a number. Returns 0, or -1
 * if any character is not a hex digit.
 */
static int parse_hex8(const char *text, unsigned long *value)
{
    size_t i;
    *value = 0;
    for (i = 0; i < 8; ++i) {
        unsigned char c = to_lower_ascii((unsigned char)text[i]);
        unsigned char digit;
        if (c >= '0' && c <= '9')
            digit = (unsigned char)(c - '0');
        else if (c >= 'a' && c <= 'f')
            digit = (unsigned char)(c - 'a' + 10);
        else
            return -1;
        *value = (*value << 4) | digit;
    }
    return 0;
}

int game_info_parse(const char *bytes, size_t length, const char *path, char *description,
                    size_t capacity)
{
    const char *file_end = bytes + length;
    const char *path_line, *path_end;
    const char *text, *text_end;
    const char *checksum_line;
    size_t path_length = strlen(path);
    size_t text_length, i;
    unsigned long expected_crc;

    if (capacity == 0)
        return -1;
    description[0] = 0; /* stays empty unless everything checks out */

    /* Line 1: the header. */
    if (length < SHORTEST_INFO_FILE || memcmp(bytes, INFO_HEADER, INFO_HEADER_LENGTH) != 0)
        return -1;

    /* Line 2: the disc's path, which must be the disc we were asked about. */
    path_line = bytes + INFO_HEADER_LENGTH;
    path_end = (const char *)memchr(path_line, '\n', length - INFO_HEADER_LENGTH);
    if (!path_end || (size_t)(path_end - path_line) != path_length)
        return -1;
    for (i = 0; i < path_length; ++i) {
        if (to_lower_ascii((unsigned char)path_line[i]) != to_lower_ascii((unsigned char)path[i]))
            return -1;
    }

    /* Line 3: the description itself. */
    text = path_end + 1;
    text_end = (const char *)memchr(text, '\n', (size_t)(file_end - text));
    if (!text_end)
        return -1;
    text_length = (size_t)(text_end - text);
    if (text_length >= capacity || text_length >= GAME_DESCRIPTION_BYTES)
        return -1;

    /* Line 4: the checksum, which must be the last thing in the file. */
    checksum_line = text_end + 1;
    if (file_end - checksum_line != CHECKSUM_LINE_LENGTH || checksum_line[8] != '\n')
        return -1;
    if (parse_hex8(checksum_line, &expected_crc) < 0)
        return -1;

    /* The checksum covers lines 2 and 3, including their newlines. */
    if (usb_art_crc32((const unsigned char *)path_line, (size_t)(checksum_line - path_line)) !=
        expected_crc)
        return -1;

    /* The menu's font only has printable ASCII; the desktop app converts
     * everything else before writing the file. */
    for (i = 0; i < text_length; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c < 32 || c > 126)
            return -1;
    }

    memcpy(description, text, text_length);
    description[text_length] = 0;
    return 0;
}
