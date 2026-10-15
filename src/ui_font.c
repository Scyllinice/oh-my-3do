#include "ui_font.h"
#include "ui_font_data.h"
#include <string.h>
static int glyph(unsigned char c) { return c>=32 && c<=126 ? c-32 : '?'-32; }
int ui_font_height(int style)
{
    static const int heights[4]={15,12,17,19};
    return heights[style>=0 && style<4 ? style : 0];
}
int ui_font_width(const char *text,int style)
{
    int width=0;
    if (style<0 || style>3) style=0;
    while (*text) width+=ui_advances[style][glyph((unsigned char)*text++)];
    return width;
}
size_t ui_font_fit(const char *text,size_t length,int pixels,int style)
{
    size_t i=0; int width=0;
    if (style<0 || style>3) style=0;
    while (i<length && text[i]) {
        int n=ui_advances[style][glyph((unsigned char)text[i])];
        if (width+n>pixels) break;
        width+=n; ++i;
    }
    return i;
}
int ui_font_render(unsigned char *out,const char *text,int style,int max_width)
{
    int x=0;
    if (style<0 || style>3) style=0;
    if (max_width>UI_FONT_WIDTH) max_width=UI_FONT_WIDTH;
    memset(out,0,UI_FONT_STRIDE*UI_FONT_HEIGHT);
    while (*text) {
        int c=glyph((unsigned char)*text++), row, col, advance=ui_advances[style][c];
        if (x+advance>max_width) break;
        for (row=0;row<ui_font_height(style);++row) {
            for (col=0;col<16 && x+col<max_width;++col) {
                unsigned int packed=ui_glyphs[style][c][row*8+col/2];
                unsigned int coverage=(col&1) ? packed&15U : packed>>4;
                int dx=x+col;
                if (coverage) {
                    unsigned char *pixel=out+row*UI_FONT_STRIDE+dx/2;
                    unsigned int old=(dx&1) ? *pixel&15U : *pixel>>4;
                    if (coverage>old) *pixel=(unsigned char)((dx&1) ? ((*pixel&0xf0U)|coverage) : ((*pixel&15U)|(coverage<<4)));
                }
            }
        }
        x+=advance;
    }
    return x;
}

void ui_font_palette(unsigned short *out,unsigned int ink,unsigned int background)
{
    int i;
    memset(out,0,32*sizeof(*out));
    for (i=1;i<16;++i) {
        unsigned int r=(((ink>>10)&31U)*i+((background>>10)&31U)*(15-i)+7)/15;
        unsigned int g=(((ink>>5)&31U)*i+((background>>5)&31U)*(15-i)+7)/15;
        unsigned int b=((ink&31U)*i+(background&31U)*(15-i)+7)/15;
        out[i]=(unsigned short)((r<<10)|(g<<5)|b);
    }
}
