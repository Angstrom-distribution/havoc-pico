/* c8_dither against a formula written another way (with divides, by
 * comparing instead of tabulating), for every level and every phase. */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "../c8.h"

static const int level[6] = { 0, 95, 135, 175, 215, 255 };
static const int bayer[4][4] = {
	{ 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 },
};

/* the channel as a level index: the next level is taken when the distance
 * from the lower one, in sixteenths of the gap, is above the threshold */
static int ref_channel(int v, int th)
{
	int lo = 0, i;

	for (i = 0; i < 6; i++)
		if (level[i] <= v)
			lo = i;
	if (lo == 5)
		return 5;
	return lo + ((v - level[lo]) * 16 / (level[lo + 1] - level[lo]) > th);
}

static int ref_index(int r, int g, int b, int x, int y)
{
	int th = bayer[y & 3][x & 3];

	return 16 + 36 * ref_channel(r, th) + 6 * ref_channel(g, th) +
	       ref_channel(b, th);
}

int main(void)
{
	int v, ph, i, x, y;
	unsigned int seed = 12345;

	/* every value in each channel, the other two at other values */
	for (v = 0; v < 256; v++)
		for (ph = 0; ph < 16; ph++) {
			int x = ph & 3, y = ph >> 2;
			int o = (v * 7 + 3) & 255, p = (v * 13 + 100) & 255;

			CHECK(c8_dither(v << 16 | o << 8 | p, ph) ==
			      ref_index(v, o, p, x, y));
			CHECK(c8_dither(o << 16 | v << 8 | p, ph) ==
			      ref_index(o, v, p, x, y));
			CHECK(c8_dither(p << 16 | o << 8 | v, ph) ==
			      ref_index(p, o, v, x, y));
		}

	/* the phase argument is the low 4 bits; the flag bits of the window
	 * code are ignored */
	CHECK(c8_dither(0x7f7f7f, 0x80 | 5) == c8_dither(0x7f7f7f, 5));

	/* the exact cube colours stay what they are in every phase */
	for (i = 0; i < 216; i++) {
		uint32_t c = (uint32_t)level[i / 36] << 16 |
			     level[i / 6 % 6] << 8 | level[i % 6];

		for (ph = 0; ph < 16; ph++)
			CHECK(c8_dither(c, ph) == 16 + i);
	}

	/* just below a level, and at the midpoint of each gap */
	for (i = 0; i < 5; i++) {
		int mid = (level[i] + level[i + 1]) / 2;
		int n = 0;

		for (ph = 0; ph < 16; ph++) {
			int idx = c8_dither(mid * 0x010101, ph) - 16;

			/* grey: the three channels agree */
			CHECK(idx / 36 == idx / 6 % 6 && idx / 6 % 6 == idx % 6);
			n += idx % 6 == i + 1;
		}
		/* about half of the phases take the upper level */
		CHECK(n >= 7 && n <= 9);
	}

	/* a grey ramp: the mean level of the 16 phases rises with the input
	 * and is within a sixteenth of the gap of it */
	{
		int prev = -1;

		for (v = 0; v < 256; v++) {
			int sum = 0;

			for (ph = 0; ph < 16; ph++)
				sum += level[(c8_dither(v * 0x010101, ph) - 16) % 6];
			CHECK(sum >= prev);
			prev = sum;
			/* 16 v against the sum: less than one gap (95 at most) */
			CHECK(sum <= 16 * v && 16 * v - sum < 95);
		}
	}

	/* every result is a cube index, for random colours */
	for (i = 0; i < 100000; i++) {
		unsigned int c;

		seed = seed * 1103515245 + 12345;
		c = seed >> 4 & 0xffffff;
		x = seed >> 28 & 3;
		y = seed >> 26 & 3;
		CHECK(c8_dither(c, y << 2 | x) >= 16 &&
		      c8_dither(c, y << 2 | x) < 232);
	}
	CHECK_DONE("test-c8-dither");
}
