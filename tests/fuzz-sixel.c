/* libFuzzer target for sixel.c. The first two bytes pick P1 and P2 and the
 * size of the pieces the rest is fed in; the limits are small so that the
 * limit paths run. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../sixel.h"

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
	static const struct sixel_limits lim = { 300, 200000 };
	struct sixel_image im;
	int argv[2];
	struct sixel *s;
	size_t step, at = 0, i;
	unsigned int sum = 0;

	if (n < 2)
		return 0;
	argv[0] = d[0] & 15;
	argv[1] = d[1] & 3;
	step = 1 + (d[1] >> 2);
	d += 2;
	n -= 2;
	s = sixel_new(2, argv, &lim);
	if (!s)
		return 0;
	while (at < n) {
		size_t c = n - at < step ? n - at : step;

		sixel_put(s, (const char *)d + at, c);
		at += c;
	}
	if (sixel_finish(s, &im) == 0) {
		size_t px = (size_t)im.w * im.h;

		if (im.w > lim.max_w || px > lim.max_pixels || im.h > SIXEL_MAX_SIDE)
			abort();
		/* read all of it, so that the sanitizer sees a short buffer */
		for (i = 0; i < px; i++)
			sum += im.pix[i];
		if (im.mask)
			for (i = 0; i < (px + 7) / 8; i++)
				sum += im.mask[i];
		for (i = 0; i < 256; i++)
			sum += im.pal[i] & 1;
		if (sum == 0xdeadbeef)
			abort();
		free(im.pix);
		free(im.mask);
	}
	sixel_free(s);
	return 0;
}
