#ifndef HAVOC_PIXFONT_H
#define HAVOC_PIXFONT_H

#include <stdbool.h>
#include <stdint.h>

/* Bitmap fonts: the X11 misc-fixed fonts, compiled in as const data
 * (pixfont-data.c, made by gen-pixfont.py). A font is a grid of equal cells,
 * w x h pixels; a glyph is h rows of bpr bytes (1 for w <= 8, else 2), the
 * leftmost pixel in the top bit. The data holds runs of consecutive code
 * points; the glyph of range r for code point c is glyph r.index + c - r.first.
 * Glyph 0 of every font is the one drawn for a code point it does not have. */
struct pixrange {
	uint16_t first, count;
	uint32_t index;
};

struct pixfont {
	const char *name;
	uint8_t w, h, bpr;
	const uint8_t *rows;
	const struct pixrange *range;
	uint16_t nrange;
};

extern const struct pixfont *const pixfonts[];

/* spec is "yes" (the font follows [font] size) or the name of a font, such as
 * "6x13" (that font, whatever the size). Returns 0, or -1 when there is no
 * such font, and then nothing is selected. */
int pixfont_select(const char *spec);

/* True once pixfont_select() has returned 0. */
bool pixfont_active(void);

/* The cell size for a font size: the largest font not higher than size, or
 * the smallest one, drawn in whole-number multiples from twice the highest
 * font's height. */
void pixfont_scale(int size, int *w, int *h);

/* The multiple pixfont_scale() chose: each font pixel is a k x k block. */
int pixfont_multiple(void);

/* The rows of the glyph for code point ch in cells cells (1 or 2) of the
 * selected font, and their bytes per row. A code point the font lacks, and
 * every wide cell, gives the font's missing glyph (for two cells a box). */
const uint8_t *pixfont_glyph(uint32_t ch, int cells, int *bpr);

#endif
