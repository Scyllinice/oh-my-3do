/*
 * Writing the settings file. See menu_settings.h for why it is always the
 * same size, and menu_config.h for the format.
 */
#include "menu_settings.h"
#include <string.h>

/* Whether `value` is between 0 and `largest`, inclusive. */
static int in_range(int value, int largest)
{
    return value >= 0 && value <= largest;
}

/* Whether every setting holds a value the reader accepts. */
static int is_valid_config(const MenuConfig *config)
{
    return in_range(config->folder_workaround, MENU_WORKAROUND_OFF) &&
           in_range(config->usb_covers, 1) && in_range(config->screen_margin, MENU_MARGIN_MAX) &&
           in_range(config->large_titles, 1) && in_range(config->remember_position, 1) &&
           in_range(config->recent_games, 1) && in_range(config->video_previews, 1) &&
           in_range(config->preview_sound, 1) && in_range(config->hide_adult, 1);
}

/*
 * Appends "key=value" and a Windows line ending to the text in `data`.
 * Values are at most two digits (see is_valid_config).
 */
static void append_setting(char *data, const char *key, int value)
{
    char line_end[5]; /* up to two digits, "\r\n" and a terminating zero */
    size_t length = 0;

    if (value >= 10)
        line_end[length++] = (char)('0' + value / 10);
    line_end[length++] = (char)('0' + value % 10);
    line_end[length++] = '\r';
    line_end[length++] = '\n';
    line_end[length] = 0;

    strcat(data, key);
    strcat(data, "=");
    strcat(data, line_end);
}

int menu_settings_save(Ode *ode, OdeReadBuffer reader, const MenuConfig *config)
{
    unsigned char data[MENU_CONFIG_BYTES];
    char *text = (char *)data;
    MenuConfig check;
    size_t used;
    int result;

    if (!is_valid_config(config))
        return -1;

    /*
     * Build the file. Windows line endings ("\r\n") keep it readable in any
     * PC text editor.
     */
    memset(data, 0, sizeof(data));
    strcpy(text, "; Saved by Oh My 3DO.\r\n[compatibility]\r\n");
    append_setting(text, "folder_workaround", config->folder_workaround);
    strcat(text, "\r\n[artwork]\r\n");
    append_setting(text, "usb_covers", config->usb_covers);
    append_setting(text, "video_previews", config->video_previews);
    strcat(text, "\r\n[display]\r\n");
    append_setting(text, "screen_margin", config->screen_margin);
    append_setting(text, "large_titles", config->large_titles);
    strcat(text, "\r\n[library]\r\n");
    append_setting(text, "remember_position", config->remember_position);
    append_setting(text, "recent_games", config->recent_games);
    append_setting(text, "hide_adult", config->hide_adult);
    strcat(text, "\r\n[audio]\r\n");
    append_setting(text, "preview_sound", config->preview_sound);
    strcat(text, "\r\n" MENU_CONFIG_END "\r\n");

    used = strlen(text);
    if (used > sizeof(data) - 2)
        return -1;

    /*
     * Fill the rest with spaces and end with a line break, so the file is
     * always MENU_CONFIG_BYTES long and every byte of an older file is
     * overwritten. The reader stops at the end line, so the padding is never
     * read as settings, and spaces (unlike zero bytes) keep the file plain
     * text for PC editors.
     */
    memset(data + used, ' ', sizeof(data) - used);
    data[sizeof(data) - 2] = '\r';
    data[sizeof(data) - 1] = '\n';

    /* Never write a file the menu itself would refuse to load. */
    if (menu_config_parse(&check, text, sizeof(data)) < 0)
        return -1;

    /* Write it... */
    if (usb_file_open(ode, MENU_CONFIG_NAME) < 0)
        return -1;
    result = usb_file_write(ode, data, sizeof(data));
    if (usb_file_close(ode) < 0)
        result = -1;

    /* ...then read it back and compare. */
    if (result < 0 || usb_file_open(ode, MENU_CONFIG_NAME) < 0)
        return -1;
    result = usb_file_verify(ode, reader, data, sizeof(data));
    if (usb_file_close(ode) < 0)
        result = -1;
    return result;
}
