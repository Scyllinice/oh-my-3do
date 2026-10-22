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
/* Zero-initialize before first use. Bounded measurements for repeated labels. Text
 * longer than this still works, but is measured again rather than retained. */
#define UI_FONT_MEASURE_BYTES 256
typedef struct UiFontMeasure {
    char value[UI_FONT_MEASURE_BYTES];
    size_t length,fit,tail,ellipsis,offset,offset_fit;
    int pixels,style,valid,fit_valid,tail_valid,ellipsis_valid,offset_valid;
} UiFontMeasure;
void ui_font_measure(UiFontMeasure *measure,const char *text,size_t length,int pixels,int style);
/* Call after measure with the same text. Only requested measurements run;
 * ordinary short labels do not pay for suffix/ellipsis calculations. */
size_t ui_font_measure_tail(UiFontMeasure *measure,const char *text);
size_t ui_font_measure_ellipsis(UiFontMeasure *measure,const char *text);
/* Remembers the current scroll window as well as the initial fit. */
size_t ui_font_measure_fit(UiFontMeasure *measure,const char *text,size_t offset);
/* Rasterize 4-bit coverage into a 144-byte-stride CEL buffer. */
void ui_font_palette(unsigned short *out,unsigned int ink,unsigned int background);
int ui_font_render(unsigned char *out,const char *text,int style,int max_width);
#endif
