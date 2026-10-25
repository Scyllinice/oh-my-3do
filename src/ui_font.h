/*
 * The menu's pixel font: measuring text and drawing it into a picture the
 * console can show.
 *
 * Text is drawn into a 4-bit image: each pixel is a number from 0 (no ink)
 * to 15 (full ink), two pixels per byte. Paired with a 16-color palette from
 * ui_font_palette, that image becomes a CEL (the 3DO's drawing object) in any
 * ink and background color.
 *
 * The glyphs come from ui_font_data.h, which tools/build-ui-font.py
 * generates from the Ark Pixel font. They cover printable ASCII only; any
 * other byte is drawn as '?'.
 */
#ifndef OH3DO_UI_FONT_H
#define OH3DO_UI_FONT_H
#include <stddef.h>

/* Text styles (sizes), used to index the glyph tables. */
#define UI_FONT_NORMAL  0
#define UI_FONT_SMALL   1
#define UI_FONT_LARGE   2
#define UI_FONT_HEADING 3
/* The small glyphs with each stroke a pixel wider, for button letters. */
#define UI_FONT_SMALL_BOLD 4

/*
 * The size of the image ui_font_render draws into: UI_FONT_WIDTH pixels
 * wide (two per byte, so UI_FONT_STRIDE bytes per row) and UI_FONT_HEIGHT
 * rows, enough for one line in the tallest style.
 */
#define UI_FONT_WIDTH  288
#define UI_FONT_STRIDE 144
#define UI_FONT_HEIGHT 20

/* The height of a line in `style`, in pixels. */
int ui_font_height(int style);

/* The width of `text` in `style`, in pixels. */
int ui_font_width(const char *text, int style);

/* How many of the first `length` characters of `text` fit in `pixels`. */
size_t ui_font_fit(const char *text, size_t length, int pixels, int style);

/*
 * Where the longest ending of `text` that fits in `pixels` starts: the first
 * character to show when a scrolling title has reached its end.
 */
size_t ui_font_tail_start(const char *text, size_t length, int pixels, int style);

/*
 * Remembered measurements for one label that is drawn again every frame.
 *
 * Measuring proportional text means adding up every character's width, which
 * adds up across a screen full of labels redrawn 30 times a second. A
 * UiFontMeasure keeps the results for one label until its text, width or
 * style changes. Each measurement is only worked out when first asked for, so
 * ordinary short labels never pay for the scrolling or "..." ones.
 *
 * Start with a zeroed structure. Text of UI_FONT_MEASURE_BYTES or more still
 * works, but is not remembered and so is measured again each time.
 */
#define UI_FONT_MEASURE_BYTES 256
typedef struct UiFontMeasure {
    char value[UI_FONT_MEASURE_BYTES]; /* the label's text, to notice changes */
    size_t length;                     /* its length */
    int pixels, style;                 /* the space and style it is measured for */
    int valid;                         /* the fields above describe the current label */
    size_t fit;                        /* characters that fit from the start */
    size_t tail;                       /* see ui_font_tail_start */
    size_t ellipsis;                   /* characters that fit before a "..." */
    size_t offset, offset_fit;         /* characters that fit from `offset` on */
    int fit_valid, tail_valid, ellipsis_valid, offset_valid; /* which results are known */
} UiFontMeasure;

/*
 * Points `measure` at a label. If it already holds the same text, width and
 * style, its results are kept; otherwise they are forgotten.
 */
void ui_font_measure(UiFontMeasure *measure, const char *text, size_t length, int pixels,
                     int style);

/* The label's tail start (see ui_font_tail_start). Call after ui_font_measure
 * with the same text, as for the next two functions. */
size_t ui_font_measure_tail(UiFontMeasure *measure, const char *text);

/* How many characters fit with a "..." after them. */
size_t ui_font_measure_ellipsis(UiFontMeasure *measure, const char *text);

/*
 * How many characters fit starting `offset` characters in, for a scrolling
 * label. Remembers both the unscrolled fit and the current scroll position.
 */
size_t ui_font_measure_fit(UiFontMeasure *measure, const char *text, size_t offset);

/*
 * Fills a CEL palette (32 entries; the first 16 are used) for drawing in
 * `ink` over `background`, both 15-bit RGB colors (5 bits each of red, green
 * and blue). Entry N blends N/15 of the ink with the rest background.
 */
void ui_font_palette(unsigned short *out, unsigned int ink, unsigned int background);

/*
 * Draws `text` into `out` (UI_FONT_STRIDE * UI_FONT_HEIGHT bytes, cleared
 * first), stopping before the first character that would pass `max_width`
 * pixels. Returns the width drawn, in pixels.
 */
int ui_font_render(unsigned char *out, const char *text, int style, int max_width);
#endif
