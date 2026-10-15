#include "ui_font.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    unsigned char guarded[UI_FONT_STRIDE*UI_FONT_HEIGHT+2];
    unsigned char uppercase[UI_FONT_STRIDE*UI_FONT_HEIGHT];
    int style;
    for (style=0;style<4;++style) {
        size_t fit;
        assert(ui_font_width("iii",style)<ui_font_width("WWW",style));
        fit=ui_font_fit("Dragon's Lair",13,ui_font_width("Dragon",style),style);
        assert(fit==6);
        memset(guarded,0xa5,sizeof(guarded));
        ui_font_render(guarded+1,"Dragon's Lair",style,UI_FONT_WIDTH);
        assert(guarded[0]==0xa5 && guarded[sizeof(guarded)-1]==0xa5);
        ui_font_render(uppercase,"DRAGON'S LAIR",style,UI_FONT_WIDTH);
        assert(memcmp(guarded+1,uppercase,sizeof(uppercase)));
        ui_font_render(guarded+1,"WWWW",style,1);
        memset(uppercase,0,sizeof(uppercase));
        assert(!memcmp(guarded+1,uppercase,sizeof(uppercase)));
        assert(ui_font_height(style)<=UI_FONT_HEIGHT);
    }
    puts("UI font: proportional width, mixed case, fitting and buffer bounds passed");
    return 0;
}
