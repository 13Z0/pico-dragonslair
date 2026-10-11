/*
 * Scoreboard overlay. See scoreboard.h.
 */
#include "scoreboard.h"

#include "dl_port.h"

/* 5x7 glyphs, one byte per row, bit 4 leftmost */
static const struct
{
    char c;
    uint8_t rows[7];
} glyphs[] = {
    {'0', {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e}}, {'1', {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e}},
    {'2', {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f}}, {'3', {0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e}},
    {'4', {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02}}, {'5', {0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e}},
    {'6', {0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e}}, {'7', {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e}}, {'9', {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c}},
    {'A', {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}}, {'B', {0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e}},
    {'C', {0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e}}, {'D', {0x1c, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1c}},
    {'E', {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f}}, {'I', {0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f}}, {'P', {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10}},
    {'R', {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11}}, {'S', {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e}},
    {'T', {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}}, {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}},
};

#define CELL 6     /* glyph width plus one column of space */
#define TEXT_Y 3   /* first line of the glyphs in the strip */
#define LEFT 28    /* x of cell 0: 44 cells, centred */

static const uint8_t *glyph(char c)
{
    for (unsigned i = 0; i < sizeof(glyphs) / sizeof(glyphs[0]); i++)
        if (glyphs[i].c == c)
            return glyphs[i].rows;
    return 0;
}

static uint16_t colour(int format, int r, int g, int b)
{
    if (format == SB_RGB444)
        return (uint16_t)(((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4));
    return (uint16_t)(((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
}

static void put(uint16_t *fb, int stride, int cell, char c, uint16_t fg)
{
    const uint8_t *g = glyph(c);
    if (!g)
        return;
    const int x0 = LEFT + cell * CELL;
    for (int y = 0; y < 7; y++)
    {
        uint16_t *d = fb + (TEXT_Y + y) * stride + x0;
        for (int x = 0; x < 5; x++)
            if (g[y] & (0x10 >> x))
                d[x] = fg;
    }
}

static void text(uint16_t *fb, int stride, int cell, const char *s, uint16_t fg)
{
    for (; *s; s++, cell++)
        put(fb, stride, cell, *s, fg);
}

/* LED digit value as a character: 0-9, A b C d E, blank */
static char led_char(uint8_t v)
{
    static const char map[16] = "0123456789ABCDE ";
    return map[v & 0x0f];
}

static void digits(uint16_t *fb, int stride, int cell, const uint8_t *d, int n, uint16_t fg)
{
    for (int i = 0; i < n; i++)
        put(fb, stride, cell + i, led_char(d[i]), fg);
}

void sb_draw(const dl_leds_t *leds, uint16_t *fb, int stride, int format)
{
    const uint16_t bg = colour(format, 16, 16, 24);
    const uint16_t label = colour(format, 240, 200, 64);
    const uint16_t led = colour(format, 255, 48, 32);

    for (int y = 0; y < SB_HEIGHT; y++)
    {
        uint16_t *d = fb + y * stride;
        for (int x = 0; x < 320; x++)
            d[x] = bg;
    }

    const uint8_t *dg = leds->digit;
    text(fb, stride, 0, "1UP", label);
    digits(fb, stride, 4, dg + 0, 6, led);
    text(fb, stride, 11, "L", label);
    digits(fb, stride, 12, dg + 6, 1, led);

    text(fb, stride, 17, "CREDITS", label);
    digits(fb, stride, 25, dg + 14, 2, led);

    text(fb, stride, 31, "2UP", label);
    digits(fb, stride, 35, dg + 8, 6, led);
    text(fb, stride, 42, "L", label);
    digits(fb, stride, 43, dg + 7, 1, led);
}
