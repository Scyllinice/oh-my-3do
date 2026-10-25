/*
 * Reading the menu's settings file. See menu_config.h for the format and
 * menu_settings.c for writing it.
 */
#include "menu_config.h"
#include <string.h>

/* The UTF-8 "byte order mark" some Windows editors put at the start of a file. */
#define UTF8_BOM_LENGTH 3

/* FCare commands used here (usb_file.h describes the file protocol). */
#define FCARE_OPEN_FILE         0xe1
#define FCARE_READ_FILE         0xe3
#define FCARE_CLOSE_FILE        0xe5
#define FCARE_WRITE_BUFFER_WORD 0xe6

/* Status bit the FCare sets in its last reply byte when a command failed. */
#define FCARE_STATUS_ERROR 0x10

/*
 * Every setting, in the order of the `values` arrays below. A setting's
 * position is also its bit in the "seen" mask.
 */
enum {
    SETTING_FOLDER_WORKAROUND,
    SETTING_USB_COVERS,
    SETTING_SCREEN_MARGIN,
    SETTING_LARGE_TITLES,
    SETTING_REMEMBER_POSITION,
    SETTING_RECENT_GAMES,
    SETTING_VIDEO_PREVIEWS,
    SETTING_PREVIEW_SOUND,
    SETTING_HIDE_ADULT,
    SETTING_COUNT
};

static const char *const setting_names[SETTING_COUNT] = {
    "folder_workaround", "usb_covers",     "screen_margin", "large_titles", "remember_position",
    "recent_games",      "video_previews", "preview_sound", "hide_adult"};

/*
 * The settings every file the menu writes includes before its end line. The
 * newest setting, hide_adult, is optional so files from older menus still
 * load.
 */
#define REQUIRED_SETTINGS 0xffU

/* The section headings the file may contain. */
static const char *const section_names[] = {"[compatibility]", "[artwork]", "[display]",
                                            "[library]", "[audio]"};

/* Spaces, tabs and the carriage return of Windows line endings. */
static int is_blank(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

/* Whether the text from `start` to `end` is exactly `word`. */
static int text_is(const char *start, const char *end, const char *word)
{
    size_t length = strlen(word);
    return (size_t)(end - start) == length && !memcmp(start, word, length);
}

/* The largest value a setting accepts. */
static int largest_value(int setting)
{
    if (setting == SETTING_SCREEN_MARGIN)
        return MENU_MARGIN_MAX;
    if (setting == SETTING_FOLDER_WORKAROUND)
        return MENU_WORKAROUND_OFF;
    return 1; /* everything else is off (0) or on (1) */
}

void menu_config_defaults(MenuConfig *config)
{
    memset(config, 0, sizeof(*config));
    config->usb_covers = 1;
    config->remember_position = 1;
    config->recent_games = 1;
    config->video_previews = 1;
    config->preview_sound = 1;
}

/*
 * Reads one "key = value" line (with blanks trimmed from both ends, from
 * `start` to `end`). On success stores the value in `values`, marks the key in
 * `seen` and returns 0; returns -1 for anything invalid.
 */
static int parse_setting_line(const char *start, const char *end, int *values, unsigned int *seen)
{
    const char *equals = start;
    const char *key_end, *position;
    int setting, value = 0, digits = 0;

    /* Split the line at "=", and trim blanks before it. */
    while (equals < end && *equals != '=')
        ++equals;
    if (equals == end)
        return -1;
    key_end = equals;
    while (key_end > start && is_blank(key_end[-1]))
        --key_end;

    /* The key must be a known setting not given before. */
    for (setting = 0; setting < SETTING_COUNT; ++setting) {
        if (text_is(start, key_end, setting_names[setting]))
            break;
    }
    if (setting == SETTING_COUNT || (*seen & (1U << setting)))
        return -1;

    /* The value: one or two digits, after optional blanks. */
    position = equals + 1;
    while (position < end && is_blank(*position))
        ++position;
    while (position < end && *position >= '0' && *position <= '9') {
        if (++digits > 2)
            return -1;
        value = value * 10 + (*position - '0');
        ++position;
    }
    if (digits == 0 || value > largest_value(setting))
        return -1;

    /* Only blanks or a comment may follow. */
    while (position < end && is_blank(*position))
        ++position;
    if (position < end && *position != '#' && *position != ';')
        return -1;

    values[setting] = value;
    *seen |= 1U << setting;
    return 0;
}

int menu_config_parse(MenuConfig *config, const char *data, size_t length)
{
    MenuConfig defaults;
    int values[SETTING_COUNT];
    unsigned int seen = 0; /* bit N set once setting N has been read */
    int found_end_line = 0;
    size_t position = 0;

    /* Start from the defaults; the file only changes what it mentions. */
    menu_config_defaults(&defaults);
    values[SETTING_FOLDER_WORKAROUND] = defaults.folder_workaround;
    values[SETTING_USB_COVERS] = defaults.usb_covers;
    values[SETTING_SCREEN_MARGIN] = defaults.screen_margin;
    values[SETTING_LARGE_TITLES] = defaults.large_titles;
    values[SETTING_REMEMBER_POSITION] = defaults.remember_position;
    values[SETTING_RECENT_GAMES] = defaults.recent_games;
    values[SETTING_VIDEO_PREVIEWS] = defaults.video_previews;
    values[SETTING_PREVIEW_SOUND] = defaults.preview_sound;
    values[SETTING_HIDE_ADULT] = defaults.hide_adult;
    menu_config_defaults(config);

    if (length > MENU_CONFIG_BYTES)
        return -1;

    /* Skip a UTF-8 byte order mark left by a PC text editor. */
    if (length >= UTF8_BOM_LENGTH && (unsigned char)data[0] == 0xef &&
        (unsigned char)data[1] == 0xbb && (unsigned char)data[2] == 0xbf)
        position = UTF8_BOM_LENGTH;

    while (position < length) {
        const char *line = data + position;
        const char *line_end;
        size_t i;

        /* Find the end of the line. A zero byte means the file is damaged. */
        while (position < length && data[position] != '\n') {
            if (data[position] == 0)
                return -1;
            ++position;
        }
        line_end = data + position;
        if (position < length)
            ++position; /* step over the newline */

        /* Trim blanks from both ends. */
        while (line < line_end && is_blank(*line))
            ++line;
        while (line_end > line && is_blank(line_end[-1]))
            --line_end;

        if (text_is(line, line_end, MENU_CONFIG_END)) {
            /*
             * Files the menu writes list every setting before this line,
             * then pad the rest of the file. Stop here, so neither the
             * padding nor the tail of an older, longer file can add settings.
             */
            if ((seen & REQUIRED_SETTINGS) != REQUIRED_SETTINGS)
                return -1;
            found_end_line = 1;
            break;
        }

        /* Blank lines and comments. */
        if (line == line_end || *line == '#' || *line == ';')
            continue;

        /* Section headings only organize the file; any known one is fine. */
        if (*line == '[') {
            int known = 0;
            for (i = 0; i < sizeof(section_names) / sizeof(section_names[0]); ++i) {
                if (text_is(line, line_end, section_names[i]))
                    known = 1;
            }
            if (!known)
                return -1;
            continue;
        }

        if (parse_setting_line(line, line_end, values, &seen) < 0)
            return -1;
    }

    /*
     * A file that fills the whole read but has no end line may be a longer
     * file edited on a PC whose rest was cut off, so it is not trusted.
     */
    if (length == MENU_CONFIG_BYTES && !found_end_line)
        return -1;

    config->folder_workaround = values[SETTING_FOLDER_WORKAROUND];
    config->usb_covers = values[SETTING_USB_COVERS];
    config->screen_margin = values[SETTING_SCREEN_MARGIN];
    config->large_titles = values[SETTING_LARGE_TITLES];
    config->remember_position = values[SETTING_REMEMBER_POSITION];
    config->recent_games = values[SETTING_RECENT_GAMES];
    config->video_previews = values[SETTING_VIDEO_PREVIEWS];
    config->preview_sound = values[SETTING_PREVIEW_SOUND];
    config->hide_adult = values[SETTING_HIDE_ADULT];
    return 0;
}

/*
 * Sends one seven-byte command and checks the reply: it must be `expected`
 * bytes long, echo the command's code, and not have the error bit set.
 */
static int send_command(Ode *ode, const unsigned char *packet, unsigned char *reply,
                        size_t expected)
{
    size_t length = 0;
    if (ode->transfer(ode->context, packet, reply, 32, &length) < 0 || length != expected ||
        reply[0] != packet[0] || (reply[length - 1] & FCARE_STATUS_ERROR)) {
        ode->error = "USB config command failed";
        return -1;
    }
    return 0;
}

/* Stores four bytes at word `offset` of the FCare's data buffer. */
static int write_buffer_word(Ode *ode, unsigned int offset, const unsigned char *four_bytes)
{
    unsigned char packet[7], reply[32];
    packet[0] = FCARE_WRITE_BUFFER_WORD;
    packet[1] = (unsigned char)(offset >> 8);
    packet[2] = (unsigned char)offset;
    memcpy(packet + 3, four_bytes, 4);
    return send_command(ode, packet, reply, 2);
}

/*
 * Reads and parses the settings file at `filename` (relative to the drive
 * root). Returns 0, 1 for a drive that is not an FCare, -1 for an invalid
 * file or a failed command, or -2 when the file could not be opened.
 */
static int load_config_file(Ode *ode, OdeReadBuffer read_buffer, MenuConfig *config,
                            const char *filename)
{
    static const unsigned char zero_word[4] = {0, 0, 0, 0};
    unsigned char data[ODE_TOC_BYTES], packet[7], reply[32];
    size_t name_length = strlen(filename) + 1; /* including the terminating zero */
    size_t i, length;
    int result = -1;

    menu_config_defaults(config);

    /*
     * These commands were checked against the FCare (which identifies as
     * "LOCODE") only. Sending them to another drive emulator could leave its
     * command queue stuck, so other drives get the defaults.
     */
    if (strcmp(ode->identity, "LOCODE"))
        return 1;

    /* Put the file name into the FCare's buffer, four bytes at a time. */
    memset(data, 0, sizeof(data));
    memcpy(data, filename, name_length);
    for (i = 0; i < name_length; i += 4) {
        if (write_buffer_word(ode, (unsigned int)(i / 4), data + i) < 0)
            return -1;
    }

    /* Open it. The reply's second byte is nonzero on success. */
    memset(packet, 0, sizeof(packet));
    packet[0] = FCARE_OPEN_FILE;
    packet[3] = (unsigned char)name_length;
    if (send_command(ode, packet, reply, 3) < 0)
        return -1;
    if (!reply[1]) {
        ode->error = "Cannot open USB config";
        return -2;
    }

    /*
     * The FCare reports reading the full amount asked for even when the file
     * is shorter, and leaves the rest of its buffer as it was. Zero the
     * buffer first, so a short or empty file reads as zeros rather than
     * leftovers from an earlier read.
     */
    for (i = 0; i < MENU_CONFIG_BYTES / 4; ++i) {
        if (write_buffer_word(ode, (unsigned int)i, zero_word) < 0)
            goto close_file;
    }

    /* Read up to MENU_CONFIG_BYTES from the file into the FCare's buffer. */
    memset(packet, 0, sizeof(packet));
    packet[0] = FCARE_READ_FILE;
    packet[1] = MENU_CONFIG_BYTES >> 8;
    packet[2] = MENU_CONFIG_BYTES & 255;
    if (send_command(ode, packet, reply, 4) < 0)
        goto close_file;
    if (reply[1] != packet[1] || reply[2] != packet[2]) {
        ode->error = "Cannot read USB config";
        goto close_file;
    }

    /* Copy the buffer to the console. */
    if (read_buffer(ode->context, data, sizeof(data)) < 0) {
        ode->error = "USB config data read failed";
        goto close_file;
    }

    /* The text ends at the first zero byte (or the end of the read). */
    for (length = 0; length < MENU_CONFIG_BYTES && data[length]; ++length) {
    }
    result = menu_config_parse(config, (const char *)data, length);
    if (result < 0)
        ode->error = "Invalid USB config; defaults used";

close_file:
    memset(packet, 0, sizeof(packet));
    packet[0] = FCARE_CLOSE_FILE;
    if (send_command(ode, packet, reply, 2) < 0)
        result = -1;
    if (result < 0)
        config->folder_workaround = MENU_WORKAROUND_AUTO;
    return result;
}

int menu_config_load(Ode *ode, OdeReadBuffer read_buffer, MenuConfig *config)
{
    int result;

    if (strcmp(ode->identity, "LOCODE")) {
        menu_config_defaults(config);
        return 1;
    }

    result = load_config_file(ode, read_buffer, config, ".ohmy3do/" MENU_CONFIG_NAME);
    if (result == -2)
        result = -1; /* a missing file is reported like any other failure */
    if (result == 0)
        ode->error = NULL;
    return result;
}
