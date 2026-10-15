#include "ui_font.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    unsigned char guarded[UI_FONT_STRIDE*UI_FONT_HEIGHT+2];
    unsigned char uppercase[UI_FONT_STRIDE*UI_FONT_HEIGHT];
    unsigned short palette[34];
    int style;
    memset(palette,0xa5,sizeof(palette));
    ui_font_palette(palette+1,0x7fff,0);
    assert(palette[0]==0xa5a5 && palette[33]==0xa5a5);
    assert(palette[1]==0 && palette[16]==0x7fff);
    assert(palette[8]>0 && palette[8]<0x7fff);
    for (style=17;style<33;++style) assert(palette[style]==0);
    for (style=0;style<4;++style) {
        size_t fit, pixel;
        int smooth=0, opaque=0;
        assert(ui_font_width("iii",style)<ui_font_width("WWW",style));
        fit=ui_font_fit("Dragon's Lair",13,ui_font_width("Dragon",style),style);
        assert(fit==6);
        memset(guarded,0xa5,sizeof(guarded));
        ui_font_render(guarded+1,"Dragon's Lair",style,UI_FONT_WIDTH);
        assert(guarded[0]==0xa5 && guarded[sizeof(guarded)-1]==0xa5);
        for (pixel=1;pixel<sizeof(guarded)-1;++pixel) {
            unsigned int hi=guarded[pixel]>>4, lo=guarded[pixel]&15;
            if ((hi>0 && hi<15) || (lo>0 && lo<15)) smooth=1;
            if (hi==15 || lo==15) opaque=1;
        }
        assert(smooth && opaque);
        ui_font_render(uppercase,"DRAGON'S LAIR",style,UI_FONT_WIDTH);
        assert(memcmp(guarded+1,uppercase,sizeof(uppercase)));
        ui_font_render(guarded+1,"WWWW",style,1);
        memset(uppercase,0,sizeof(uppercase));
        assert(!memcmp(guarded+1,uppercase,sizeof(uppercase)));
        assert(ui_font_height(style)<=UI_FONT_HEIGHT);
    }
    puts("UI font: antialiased coverage, palette, proportional width, fitting and buffer bounds passed");
    return 0;
}
