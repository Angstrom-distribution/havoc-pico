#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pixfont.h"

static const struct pixfont *cur;
static bool follow;
static int multiple = 1;
static uint8_t wide[3 * 40];

int pixfont_select(const char *spec)
{
	int i;

	if (strcmp(spec, "yes") == 0) {
		if (pixfonts[0] == NULL)
			return -1;
		cur = pixfonts[0];
		follow = true;
		return 0;
	}

	for (i = 0; pixfonts[i]; ++i) {
		if (strcmp(spec, pixfonts[i]->name) == 0) {
			cur = pixfonts[i];
			follow = false;
			return 0;
		}
	}

	return -1;
}

bool pixfont_active(void)
{
	return cur != NULL;
}

void pixfont_scale(int size, int *w, int *h)
{
	int i, top = 0;

	multiple = 1;
	if (follow) {
		while (pixfonts[top + 1])
			++top;
		/* the fonts are in order of height */
		if (size >= 2 * pixfonts[top]->h)
			multiple = size / pixfonts[top]->h;

		cur = pixfonts[0];
		for (i = 1; pixfonts[i]; ++i) {
			/* of two fonts of one height (6x13 and 7x13) the
			 * later is taken: it has the cells of the outline
			 * font at that size */
			if (pixfonts[i]->h * multiple <= size &&
			    pixfonts[i]->h >= cur->h)
				cur = pixfonts[i];
		}
	}

	*w = cur->w * multiple;
	*h = cur->h * multiple;
}

int pixfont_multiple(void)
{
	return multiple;
}

/* A box one pixel inside the edge of cells cells, for what the font has no
 * glyph for. */
static const uint8_t *wide_box(int cells, int *bpr)
{
	int w = cur->w * cells, h = cur->h, x, y;

	*bpr = (w + 7) / 8;
	memset(wide, 0, sizeof(wide));
	for (y = 0; y < h; ++y)
		for (x = 0; x < w; ++x)
			if (y == 0 || y == h - 1 || x == 0 || x == w - 1)
				wide[y * *bpr + x / 8] |= 0x80 >> (x & 7);

	return wide;
}

const uint8_t *pixfont_glyph(uint32_t ch, int cells, int *bpr)
{
	int lo = 0, hi = cur->nrange - 1;
	size_t size = (size_t)cur->h * cur->bpr;

	if (cells != 1)
		return wide_box(cells > 2 ? 2 : cells, bpr);

	*bpr = cur->bpr;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		const struct pixrange *r = &cur->range[mid];

		if (ch < r->first)
			hi = mid - 1;
		else if (ch >= (uint32_t)r->first + r->count)
			lo = mid + 1;
		else
			return cur->rows + (r->index + ch - r->first) * size;
	}

	return cur->rows;
}
