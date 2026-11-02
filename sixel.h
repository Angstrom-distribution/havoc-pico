#ifndef HAVOC_SIXEL_H
#define HAVOC_SIXEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SIXEL_MAX_SIDE 4096

struct sixel_limits {
	unsigned int max_w;	/* pixels, 1 to SIXEL_MAX_SIDE: pixels at and
				 * past it are dropped (the text area right of
				 * the cursor) */
	size_t max_pixels;	/* max_w * stored height stays within this */
};

struct sixel_image {
	unsigned int w, h;	/* 0 x 0: nothing was drawn */
	uint8_t *pix;		/* w * h register numbers, row after row */
	uint8_t *mask;		/* bit y * w + x (LSB first) set: pixel set;
				 * NULL: every pixel is set */
	uint32_t pal[256];	/* 0x00RRGGBB */
	unsigned int gy;	/* graphics cursor y at the end */
	bool transparent;	/* P2 = 1 */
	bool truncated;		/* the height limit cut it */
};

struct sixel;

/* argc, argv: the DCS parameters as libtsm gives them (-1 = empty).
 * NULL when out of memory. */
struct sixel *sixel_new(int argc, const int *argv,
			const struct sixel_limits *lim);
/* Bytes 0x20 to 0x7e of the string, any split. */
void sixel_put(struct sixel *s, const char *buf, size_t len);
/* The picture: pix and mask pass to the caller (free them); the decoder
 * still needs sixel_free. Returns 0, or -1 when w or h is 0 or memory ran
 * out (nothing to free then); gy, transparent and truncated are set
 * either way. */
int sixel_finish(struct sixel *s, struct sixel_image *out);
void sixel_free(struct sixel *s);

#endif
