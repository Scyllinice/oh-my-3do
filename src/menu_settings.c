#include "menu_settings.h"
#include <string.h>
static void setting(char *data, const char *key, int value)
{
    char number[5]; size_t n = 0;
    if (value >= 10) number[n++] = (char)('0' + value/10);
    number[n++] = (char)('0' + value%10); number[n++] = '\r'; number[n++] = '\n'; number[n] = 0;
    strcat(data,key); strcat(data,"="); strcat(data,number);
}
int menu_settings_save(Ode *ode, OdeReadBuffer reader, const MenuConfig *c)
{
    unsigned char data[MENU_CONFIG_BYTES];
    MenuConfig check;
    int result;
    size_t used;
    memset(data,0,sizeof(data)); strcpy((char *)data,"; Saved by Oh My 3DO.\r\n[compatibility]\r\n");
    if (c->folder_workaround < 0 || c->folder_workaround > 1 || c->usb_covers < 0 || c->usb_covers > 1 ||
        c->screen_margin < 0 || c->screen_margin > 12 || c->large_titles < 0 || c->large_titles > 1 || c->theme < 0 || c->theme >= MENU_THEME_COUNT ||
        c->remember_position < 0 || c->remember_position > 1 || c->recent_games < 0 || c->recent_games > 1 || c->video_previews < 0 || c->video_previews > 1 || c->preview_sound < 0 || c->preview_sound > 1) return -1;
    setting((char *)data,"folder_workaround",c->folder_workaround);
    strcat((char *)data,"\r\n[artwork]\r\n"); setting((char *)data,"usb_covers",c->usb_covers); setting((char *)data,"video_previews",c->video_previews);
    strcat((char *)data,"\r\n[display]\r\n"); setting((char *)data,"screen_margin",c->screen_margin);
    setting((char *)data,"large_titles",c->large_titles); setting((char *)data,"theme",c->theme);
    strcat((char *)data,"\r\n[library]\r\n"); setting((char *)data,"remember_position",c->remember_position); setting((char *)data,"recent_games",c->recent_games);
    strcat((char *)data,"\r\n[audio]\r\n"); setting((char *)data,"preview_sound",c->preview_sound);
    strcat((char *)data,"\r\n" MENU_CONFIG_END "\r\n");
    used=strlen((char *)data);
    if (used>sizeof(data)-2) return -1;
    /* Overwrite every byte of the former NUL-padded file without truncation.
     * The explicit end marker lets the loader ignore the printable blank tail. */
    memset(data+used,' ',sizeof(data)-used);
    data[sizeof(data)-2]='\r'; data[sizeof(data)-1]='\n';
    if (menu_config_parse(&check,(char *)data,sizeof(data)) < 0) return -1;
    if (usb_file_open(ode,MENU_CONFIG_NAME) < 0) return -1;
    result = usb_file_write(ode,data,sizeof(data));
    if (usb_file_close(ode) < 0) result = -1;
    if (result < 0 || usb_file_open(ode,MENU_CONFIG_NAME) < 0) return -1;
    result = usb_file_verify(ode,reader,data,sizeof(data));
    if (usb_file_close(ode) < 0) result = -1;
    return result;
}
