#ifndef OH3DO_UI_FONT_H
#define OH3DO_UI_FONT_H
#include <stddef.h>
#define UI_FONT_NORMAL 0
#define UI_FONT_SMALL 1
#define UI_FONT_LARGE 2
#define UI_FONT_HEADING 3
#define UI_FONT_WIDTH 288
#define UI_FONT_STRIDE 36
#define UI_FONT_HEIGHT 20
int ui_font_height(int style);
int ui_font_width(const char *text, int style);
size_t ui_font_fit(const char *text,size_t length,int pixels,int style);
/* Rasterize into a 1-bit, 36-byte-stride CEL buffer; returns the used width. */
int ui_font_render(unsigned char *out,const char *text,int style,int max_width);
#endif
