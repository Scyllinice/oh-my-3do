#include "ui_font.h"
#include "ui_font_data.h"
#include <string.h>
static int glyph(unsigned char c) { return c>=32 && c<=126 ? c-32 : '?'-32; }
int ui_font_height(int style)
{
    static const int heights[4]={12,10,15,19};
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
            unsigned int bits=ui_glyphs[style][c][row];
            for (col=0;col<16 && x+col<max_width;++col)
                if (bits&(0x8000U>>col)) out[row*UI_FONT_STRIDE+(x+col)/8] |= (unsigned char)(0x80U>>((x+col)&7));
        }
        x+=advance;
    }
    return x;
}
