#ifndef OH3DO_UI_FONT_H
#define OH3DO_UI_FONT_H
#include <stddef.h>
#define UI_FONT_NORMAL 0
#define UI_FONT_SMALL 1
#define UI_FONT_LARGE 2
#define UI_FONT_HEADING 3
/* The small glyphs with each stroke a pixel wider, for button letters. */
#define UI_FONT_SMALL_BOLD 4
#define UI_FONT_WIDTH 288
#define UI_FONT_STRIDE 144
#define UI_FONT_HEIGHT 20
int ui_font_height(int style);
int ui_font_width(const char *text, int style);
size_t ui_font_fit(const char *text,size_t length,int pixels,int style);
/* Start of the longest suffix that fits, for proportional-text scrolling. */
size_t ui_font_tail_start(const char *text,size_t length,int pixels,int style);
/* Rasterize 4-bit coverage into a 144-byte-stride CEL buffer. */
void ui_font_palette(unsigned short *out,unsigned int ink,unsigned int background);
int ui_font_render(unsigned char *out,const char *text,int style,int max_width);
#endif
