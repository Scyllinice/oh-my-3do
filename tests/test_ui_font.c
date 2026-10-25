#include "ui_font.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    unsigned char guarded[UI_FONT_STRIDE * UI_FONT_HEIGHT + 2];
    unsigned char uppercase[UI_FONT_STRIDE * UI_FONT_HEIGHT];
    unsigned short palette[34];
    int style;
    memset(palette, 0xa5, sizeof(palette));
    ui_font_palette(palette + 1, 0x7fff, 0);
    assert(palette[0] == 0xa5a5 && palette[33] == 0xa5a5);
    assert(palette[1] == 0 && palette[16] == 0x7fff);
    assert(palette[8] > 0 && palette[8] < 0x7fff);
    for (style = 17; style < 33; ++style)
        assert(palette[style] == 0);
    for (style = 0; style < 4; ++style) {
        size_t fit, pixel;
        int smooth = 0, opaque = 0;
        assert(ui_font_width("iii", style) < ui_font_width("WWW", style));
        fit = ui_font_fit("Dragon's Lair", 13, ui_font_width("Dragon", style), style);
        assert(fit == 6);
        memset(guarded, 0xa5, sizeof(guarded));
        ui_font_render(guarded + 1, "Dragon's Lair", style, UI_FONT_WIDTH);
        assert(guarded[0] == 0xa5 && guarded[sizeof(guarded) - 1] == 0xa5);
        for (pixel = 1; pixel < sizeof(guarded) - 1; ++pixel) {
            unsigned int hi = guarded[pixel] >> 4, lo = guarded[pixel] & 15;
            if ((hi > 0 && hi < 15) || (lo > 0 && lo < 15))
                smooth = 1;
            if (hi == 15 || lo == 15)
                opaque = 1;
        }
        assert(!smooth && opaque); /* crisp: every pixel fully on or off */
        ui_font_render(uppercase, "DRAGON'S LAIR", style, UI_FONT_WIDTH);
        assert(memcmp(guarded + 1, uppercase, sizeof(uppercase)));
        ui_font_render(guarded + 1, "WWWW", style, 1);
        memset(uppercase, 0, sizeof(uppercase));
        assert(!memcmp(guarded + 1, uppercase, sizeof(uppercase)));
        assert(ui_font_height(style) <= UI_FONT_HEIGHT);
    }
    {
        const char *titles[] = {"Dennis Miller - That's News To Me", "iiiiiiiiiiiiWWWWWWWW",
                                "WWWWWWWWiiiiiiiiiiii"};
        int title, pixels;
        for (title = 0; title < 3; ++title)
            for (style = 0; style < 5; ++style)
                for (pixels = 1; pixels <= 152; ++pixels) {
                    const char *name = titles[title];
                    size_t length = strlen(name),
                           start = ui_font_tail_start(name, length, pixels, style);
                    assert(start <= length && ui_font_width(name + start, style) <= pixels);
                    assert(!start || ui_font_width(name + start - 1, style) > pixels);
                    assert(ui_font_fit(name + start, length - start, pixels, style) ==
                           length - start);
                }
    }
    {
        UiFontMeasure measure;
        char scratch[320];
        int pixels, title;
        const char *names[] = {"Dennis Miller - That's News To Me", "iiiiiiiiiiiiWWWWWWWW",
                               "WWWWWWWWiiiiiiiiiiii"};
        memset(&measure, 0, sizeof(measure));
        for (title = 0; title < 3; ++title)
            for (style = 0; style < 5; ++style)
                for (pixels = 0; pixels <= 288; pixels += 3) {
                    size_t length = strlen(names[title]), offset;
                    strcpy(scratch, names[title]);
                    ui_font_measure(&measure, scratch, length, pixels, style);
                    assert(!measure.tail_valid && !measure.ellipsis_valid);
                    assert(ui_font_measure_fit(&measure, scratch, 0) ==
                           ui_font_fit(scratch, length, pixels, style));
                    assert(ui_font_measure_tail(&measure, scratch) ==
                           ui_font_tail_start(scratch, length, pixels, style));
                    for (offset = 0; offset <= length; ++offset) {
                        size_t fit = ui_font_fit(scratch + offset, length - offset, pixels, style);
                        assert(ui_font_measure_fit(&measure, scratch, offset) == fit);
                        assert(ui_font_measure_fit(&measure, scratch, offset) == fit);
                    }
                    ui_font_measure_fit(&measure, scratch, 1);
                    ui_font_measure(&measure, scratch, length, pixels, style);
                    assert(measure.valid && measure.offset_valid); /* repeated layout kept */
                    assert(
                        ui_font_measure_ellipsis(&measure, scratch) ==
                        ui_font_fit(scratch, length, pixels - ui_font_width("...", style), style));
                }
        /* A 65-character line can fit in pixels but still need the renderer's
         * 64-character truncation; reserve the original ellipsis width. */
        memset(scratch, 'i', 65);
        scratch[65] = 0;
        pixels = ui_font_width(scratch, UI_FONT_SMALL);
        ui_font_measure(&measure, scratch, 65, pixels, UI_FONT_SMALL);
        assert(ui_font_measure_fit(&measure, scratch, 0) == 65);
        assert(
            ui_font_measure_ellipsis(&measure, scratch) ==
            ui_font_fit(scratch, 65, pixels - ui_font_width("...", UI_FONT_SMALL), UI_FONT_SMALL));
        /* Shared scratch with a different value, and uncacheably long text. */
        strcpy(scratch, "WWWW");
        ui_font_measure(&measure, scratch, 4, 24, UI_FONT_SMALL);
        strcpy(scratch, "iiii");
        ui_font_measure(&measure, scratch, 4, 24, UI_FONT_SMALL);
        assert(ui_font_measure_fit(&measure, scratch, 0) ==
               ui_font_fit(scratch, 4, 24, UI_FONT_SMALL));
        memset(scratch, 'W', sizeof(scratch) - 1);
        scratch[sizeof(scratch) - 1] = 0;
        ui_font_measure(&measure, scratch, strlen(scratch), 47, UI_FONT_SMALL);
        assert(!measure.valid);
        assert(ui_font_measure_fit(&measure, scratch, 300) ==
               ui_font_fit(scratch + 300, 19, 47, UI_FONT_SMALL));
        memset(scratch, 'i', sizeof(scratch) - 1);
        ui_font_measure(&measure, scratch, strlen(scratch), 47, UI_FONT_SMALL);
        assert(!measure.valid && !measure.offset_valid);
        assert(ui_font_measure_fit(&measure, scratch, 300) ==
               ui_font_fit(scratch + 300, 19, 47, UI_FONT_SMALL));
    }
    {
        /* Bold button letters: one pixel wider per glyph, a superset of the
         * small glyph, and "m" keeps three separate stems. */
        unsigned char small[UI_FONT_STRIDE * UI_FONT_HEIGHT], bold[UI_FONT_STRIDE * UI_FONT_HEIGHT];
        int row, x, lit_small = 0, lit_bold = 0, stems = 0;
        assert(ui_font_width("A B", UI_FONT_SMALL_BOLD) == ui_font_width("A B", UI_FONT_SMALL) + 2);
        assert(ui_font_height(UI_FONT_SMALL_BOLD) == ui_font_height(UI_FONT_SMALL));
        ui_font_render(small, "B", UI_FONT_SMALL, UI_FONT_WIDTH);
        ui_font_render(bold, "B", UI_FONT_SMALL_BOLD, UI_FONT_WIDTH);
        for (x = 0; x < UI_FONT_STRIDE * UI_FONT_HEIGHT; ++x) {
            assert((small[x] & bold[x]) == small[x]);
            lit_small += (small[x] >> 4 != 0) + ((small[x] & 15) != 0);
            lit_bold += (bold[x] >> 4 != 0) + ((bold[x] & 15) != 0);
        }
        assert(lit_bold > lit_small);
        ui_font_render(bold, "m", UI_FONT_SMALL_BOLD, UI_FONT_WIDTH);
        for (row = 0; row < UI_FONT_HEIGHT; ++row) {
            int runs = 0, was = 0;
            for (x = 0; x < 16; ++x) {
                int on = ((x & 1) ? bold[row * UI_FONT_STRIDE + x / 2] & 15
                                  : bold[row * UI_FONT_STRIDE + x / 2] >> 4) != 0;
                if (on && !was)
                    ++runs;
                was = on;
            }
            if (runs > stems)
                stems = runs;
        }
        assert(stems == 3);
    }
    puts("UI font: crisp (1-bit) coverage, bold button letters, palette, proportional width, "
         "fitting and buffer bounds passed");
    return 0;
}
