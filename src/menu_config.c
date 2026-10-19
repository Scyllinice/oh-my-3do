#include "menu_config.h"
#include <string.h>

static int space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

void menu_config_defaults(MenuConfig *config)
{
    memset(config, 0, sizeof(*config)); config->usb_covers = config->remember_position = config->recent_games = config->preview_sound = 1;
}
int menu_config_parse(MenuConfig *config, const char *data, size_t length)
{
    static const char *keys[] = {"folder_workaround", "usb_covers", "screen_margin", "large_titles", "remember_position", "recent_games", "video_previews", "preview_sound", "hide_adult"};
    int values[9] = {0, 1, 0, 0, 1, 1, 0, 1, 0};
    unsigned int seen = 0;
    size_t pos = 0, i;
    int ended = 0;
    menu_config_defaults(config);
    if (length > MENU_CONFIG_BYTES) return -1;
    if (length >= 3 && (unsigned char)data[0] == 0xef && (unsigned char)data[1] == 0xbb && (unsigned char)data[2] == 0xbf) pos = 3;
    while (pos < length) {
        size_t start = pos, end, eq, key_end;
        int value = 0, digits = 0;
        while (pos < length && data[pos] != '\n') { if (!data[pos]) return -1; ++pos; }
        end = pos; if (pos < length) ++pos;
        while (start < end && space(data[start])) ++start;
        while (end > start && space(data[end-1])) --end;
        if (end-start == sizeof(MENU_CONFIG_END)-1 && !memcmp(data+start,MENU_CONFIG_END,sizeof(MENU_CONFIG_END)-1)) {
            /* Generated files have all keys before this marker (hide_adult
             * is newer and optional). Stop here so printable padding or an
             * older on-disk tail cannot add settings. */
            if ((seen & 0xffU) != 0xffU) return -1;
            ended = 1; break;
        }
        if (start == end || data[start] == '#' || data[start] == ';') continue;
        if (data[start] == '[') {
            if (!((end-start == 7 && !memcmp(data+start,"[audio]",7)) ||
                (end-start == 15 && !memcmp(data+start,"[compatibility]",15)) ||
                (end-start == 9 && (!memcmp(data+start,"[artwork]",9) || !memcmp(data+start,"[display]",9) || !memcmp(data+start,"[library]",9))))) return -1;
            continue;
        }
        eq = start; while (eq < end && data[eq] != '=') ++eq;
        if (eq == end) return -1;
        key_end = eq; while (key_end > start && space(data[key_end-1])) --key_end;
        for (i = 0; i < 9; ++i) if (strlen(keys[i]) == key_end-start && !memcmp(data+start,keys[i],key_end-start)) break;
        if (i == 9 || (seen & (1U << i))) return -1;
        ++eq; while (eq < end && space(data[eq])) ++eq;
        while (eq < end && data[eq] >= '0' && data[eq] <= '9') {
            if (++digits > 2) return -1;
            value = value * 10 + data[eq++] - '0';
        }
        if (!digits || value > (i == 2 ? 12 : (i == 0 ? MENU_WORKAROUND_OFF : 1))) return -1;
        while (eq < end && space(data[eq])) ++eq;
        if (eq < end && data[eq] != '#' && data[eq] != ';') return -1;
        values[i] = value; seen |= 1U << i;
    }
    /* An unmarked full buffer may be a truncated longer PC-edited file. */
    if (length == MENU_CONFIG_BYTES && !ended) return -1;
    config->folder_workaround = values[0]; config->usb_covers = values[1];
    config->screen_margin = values[2]; config->large_titles = values[3];
    config->remember_position = values[4]; config->recent_games = values[5];
    config->video_previews = values[6]; config->preview_sound = values[7];
    config->hide_adult = values[8];
    return 0;
}

static int exchange(Ode *ode, const unsigned char *packet,
                    unsigned char *reply, size_t expected)
{
    size_t length = 0;
    if (ode->transfer(ode->context, packet, reply, 32, &length) < 0 ||
        length != expected || reply[0] != packet[0] || (reply[length - 1] & 0x10)) {
        ode->error = "USB config command failed";
        return -1;
    }
    return 0;
}

static int buffer_word(Ode *ode, unsigned int offset, const unsigned char *data)
{
    unsigned char packet[7], reply[32];
    packet[0] = 0xe6;
    packet[1] = (unsigned char)(offset >> 8);
    packet[2] = (unsigned char)offset;
    memcpy(packet + 3, data, 4);
    return exchange(ode, packet, reply, 2);
}

static int config_at(Ode *ode, OdeReadBuffer read_buffer, MenuConfig *config,
                     const char *filename)
{
    unsigned char data[ODE_TOC_BYTES], packet[7], reply[32];
    static const unsigned char zero[4] = {0, 0, 0, 0};
    size_t i, length, name_length = strlen(filename)+1;
    int result = -1;
    menu_config_defaults(config);
    /* The implementation was checked against FCare's LOCODE commands only.
     * Probing unsupported commands could leave an unknown ODE's FIFO hung. */
    if (strcmp(ode->identity, "LOCODE")) return 1;
    memset(data, 0, sizeof(data));
    memcpy(data, filename, name_length);
    for (i = 0; i < name_length; i += 4)
        if (buffer_word(ode, (unsigned int)(i / 4), data + i) < 0) return -1;
    memset(packet, 0, sizeof(packet));
    packet[0] = 0xe1;
    packet[3] = (unsigned char)name_length;
    if (exchange(ode, packet, reply, 3) < 0) return -1;
    if (!reply[1]) { ode->error = "Cannot open USB config"; return -2; }
    /* FCare reports the requested count even on EOF. Zero the destination
     * before reading so short and missing files cannot expose stale bytes. */
    for (i = 0; i < MENU_CONFIG_BYTES / 4; ++i)
        if (buffer_word(ode, (unsigned int)i, zero) < 0) goto close_file;
    memset(packet, 0, sizeof(packet));
    packet[0] = 0xe3;
    packet[1] = MENU_CONFIG_BYTES >> 8;
    packet[2] = MENU_CONFIG_BYTES & 255;
    if (exchange(ode, packet, reply, 4) < 0) goto close_file;
    if (reply[1] != packet[1] || reply[2] != packet[2]) {
        ode->error = "Cannot read USB config";
        goto close_file;
    }
    if (read_buffer(ode->context, data, sizeof(data)) < 0) {
        ode->error = "USB config data read failed";
        goto close_file;
    }
    for (length = 0; length < MENU_CONFIG_BYTES && data[length]; ++length) {}
    result = menu_config_parse(config, (const char *)data, length);
    if (result < 0) ode->error = "Invalid USB config; defaults used";
close_file:
    memset(packet, 0, sizeof(packet));
    packet[0] = 0xe5;
    if (exchange(ode, packet, reply, 2) < 0) result = -1;
    if (result < 0) config->folder_workaround = MENU_WORKAROUND_AUTO;
    return result;
}

int menu_config_load(Ode *ode, OdeReadBuffer read_buffer, MenuConfig *config)
{
    int result;
    if (strcmp(ode->identity,"LOCODE")) { menu_config_defaults(config); return 1; }
    result = config_at(ode,read_buffer,config,".ohmy3do/" MENU_CONFIG_NAME);
    if (result == -2) result = -1;
    if (!result) ode->error = NULL;
    return result;
}
