#include "ui_font.h"
#include "ui_font_data.h"
#include <string.h>
static int glyph(unsigned char c) { return c>=32 && c<=126 ? c-32 : '?'-32; }
/* Glyph data for a style, and the extra advance its strokes need. */
static int source(int style) { return style==UI_FONT_SMALL_BOLD ? UI_FONT_SMALL : (style>=0 && style<4 ? style : 0); }
static int advance(int style,int c)
{
    return ui_advances[source(style)][c]+(style==UI_FONT_SMALL_BOLD && c ? 1 : 0);
}
int ui_font_height(int style)
{
    static const int heights[4]={15,12,17,20};
    return heights[source(style)];
}
int ui_font_width(const char *text,int style)
{
    int width=0;
    while (*text) width+=advance(style,glyph((unsigned char)*text++));
    return width;
}
size_t ui_font_fit(const char *text,size_t length,int pixels,int style)
{
    size_t i=0; int width=0;
    while (i<length && text[i]) {
        int n=advance(style,glyph((unsigned char)text[i]));
        if (width+n>pixels) break;
        width+=n; ++i;
    }
    return i;
}
size_t ui_font_tail_start(const char *text,size_t length,int pixels,int style)
{
    size_t start=length;
    int width=0;
    while (start) {
        int step=advance(style,glyph((unsigned char)text[start-1]));
        if (width+step>pixels) break;
        width+=step;--start;
    }
    return start;
}
void ui_font_measure(UiFontMeasure *measure,const char *text,size_t length,int pixels,int style)
{
    if (measure->valid && measure->length==length && measure->pixels==pixels &&
        measure->style==style && !memcmp(measure->value,text,length)) return;
    measure->length=length;measure->pixels=pixels;measure->style=style;
    measure->fit_valid=measure->tail_valid=measure->ellipsis_valid=measure->offset_valid=0;
    measure->valid=length<UI_FONT_MEASURE_BYTES;
    if (measure->valid) { memcpy(measure->value,text,length);measure->value[length]=0; }
}
size_t ui_font_measure_tail(UiFontMeasure *measure,const char *text)
{
    if (!measure->tail_valid) {
        measure->tail=ui_font_tail_start(text,measure->length,measure->pixels,measure->style);
        measure->tail_valid=1;
    }
    return measure->tail;
}
size_t ui_font_measure_ellipsis(UiFontMeasure *measure,const char *text)
{
    if (!measure->ellipsis_valid) {
        /* A line can fit in pixels yet exceed the caller's 64-character cap. */
        measure->ellipsis=ui_font_fit(text,measure->length,measure->pixels-ui_font_width("...",measure->style),measure->style);
        measure->ellipsis_valid=1;
    }
    return measure->ellipsis;
}
size_t ui_font_measure_fit(UiFontMeasure *measure,const char *text,size_t offset)
{
    if (offset>=measure->length) return 0;
    if (!offset) {
        if (!measure->fit_valid) {
            measure->fit=ui_font_fit(text,measure->length,measure->pixels,measure->style);
            measure->fit_valid=1;
        }
        return measure->fit;
    }
    if (measure->offset_valid && measure->offset==offset) return measure->offset_fit;
    measure->offset=offset;measure->offset_valid=1;
    measure->offset_fit=ui_font_fit(text+offset,measure->length-offset,measure->pixels,measure->style);
    return measure->offset_fit;
}
/* Widen each stroke a pixel rightwards, except where that would close a
 * one-pixel gap before the next stroke, so letters like m and B stay open. */
static void embolden(unsigned char *out,int width,int height)
{
    unsigned char lit[UI_FONT_WIDTH+2];
    int row, x;
    for (row=0;row<height;++row) {
        unsigned char *line=out+row*UI_FONT_STRIDE;
        memset(lit,0,sizeof(lit));
        for (x=0;x<width;++x) lit[x]=(unsigned char)((x&1) ? line[x/2]&15U : line[x/2]>>4);
        for (x=0;x+1<width;++x)
            if (lit[x] && !lit[x+1] && !lit[x+2])
                line[(x+1)/2]|=(unsigned char)(((x+1)&1) ? lit[x] : lit[x]<<4);
    }
}
int ui_font_render(unsigned char *out,const char *text,int style,int max_width)
{
    int x=0, data=source(style);
    if (max_width>UI_FONT_WIDTH) max_width=UI_FONT_WIDTH;
    memset(out,0,UI_FONT_STRIDE*UI_FONT_HEIGHT);
    while (*text) {
        int c=glyph((unsigned char)*text++), row, col, step=advance(style,c);
        if (x+step>max_width) break;
        for (row=0;row<ui_font_height(style);++row) {
            for (col=0;col<16 && x+col<max_width;++col) {
                unsigned int packed=ui_glyphs[data][c][row*8+col/2];
                unsigned int coverage=(col&1) ? packed&15U : packed>>4;
                int dx=x+col;
                if (coverage) {
                    unsigned char *pixel=out+row*UI_FONT_STRIDE+dx/2;
                    unsigned int old=(dx&1) ? *pixel&15U : *pixel>>4;
                    if (coverage>old) *pixel=(unsigned char)((dx&1) ? ((*pixel&0xf0U)|coverage) : ((*pixel&15U)|(coverage<<4)));
                }
            }
        }
        x+=step;
    }
    if (style==UI_FONT_SMALL_BOLD) embolden(out,x,ui_font_height(style));
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
