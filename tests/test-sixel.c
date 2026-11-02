/* Tests for sixel.c.
 *
 *   test-sixel                      the built-in cases
 *   test-sixel FILE OUT [MAX_W]     decode every DCS q string in FILE and write
 *                                   OUT (JSON) and OUT.<n>.rgba, the same
 *                                   files tools/sixel_ref.py --json writes
 *   test-sixel --time FILE [N]      decode the first string of FILE N times
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../sixel.h"

static int failed, checks;

#define CHECK(c) do { \
	checks++; \
	if (!(c)) { \
		failed++; \
		fprintf(stderr, "%s:%d: FAIL %s\n", __FILE__, __LINE__, #c); \
	} \
} while (0)

static unsigned int rng_state = 12345;

static unsigned int rnd(void)
{
	rng_state = rng_state * 1103515245u + 12345u;
	return rng_state >> 8;
}

/* Feeds body in pieces of random size, so that every state is resumed. */
static void feed_split(struct sixel *s, const char *body, size_t len)
{
	size_t at = 0;

	while (at < len) {
		size_t n = 1 + rnd() % (rnd() % 4 == 0 ? 2 : 40);

		if (n > len - at)
			n = len - at;
		sixel_put(s, body + at, n);
		at += n;
	}
}

static int decode_one(int argc, const int *argv, const struct sixel_limits *lim,
		      const char *body, size_t len, int split,
		      struct sixel_image *img)
{
	struct sixel *s = sixel_new(argc, argv, lim);
	int rc;

	if (!s) {
		fprintf(stderr, "out of memory\n");
		exit(2);
	}
	if (split)
		feed_split(s, body, len);
	else
		sixel_put(s, body, len);
	rc = sixel_finish(s, img);
	sixel_free(s);
	return rc;
}

static bool is_set(const struct sixel_image *im, unsigned int x, unsigned int y)
{
	size_t b = (size_t)y * im->w + x;

	return !im->mask || (im->mask[b >> 3] >> (b & 7) & 1);
}

static void free_img(struct sixel_image *im)
{
	free(im->pix);
	free(im->mask);
	im->pix = im->mask = NULL;
}

/* Decodes whole and in pieces and requires the same picture. */
static int dec(int argc, const int *argv, const struct sixel_limits *lim,
	       const char *body, struct sixel_image *img)
{
	struct sixel_image b;
	size_t len = strlen(body);
	int rc = decode_one(argc, argv, lim, body, len, 0, img);
	int rc2 = decode_one(argc, argv, lim, body, len, 1, &b);

	CHECK(rc == rc2);
	CHECK(img->w == b.w && img->h == b.h && img->gy == b.gy);
	CHECK(img->truncated == b.truncated);
	CHECK((img->mask == NULL) == (b.mask == NULL));
	if (rc == 0 && rc2 == 0 && img->w == b.w && img->h == b.h) {
		size_t n = (size_t)img->w * img->h;

		CHECK(!memcmp(img->pix, b.pix, n));
		if (img->mask && b.mask)
			CHECK(!memcmp(img->mask, b.mask, (n + 7) >> 3));
		CHECK(!memcmp(img->pal, b.pal, sizeof(img->pal)));
	}
	free_img(&b);
	if (rc)
		CHECK(!img->pix && !img->mask);
	return rc;
}

static const struct sixel_limits wide = { 4096, 4096u * 4096u };

/* HLS in doubles, the way colorsys does it, for the sweep */
static double hue_v(double m1, double m2, double h)
{
	h -= (int)h;
	if (h < 0)
		h += 1;
	if (h < 1.0 / 6)
		return m1 + (m2 - m1) * h * 6;
	if (h < 0.5)
		return m2;
	if (h < 2.0 / 3)
		return m1 + (m2 - m1) * (2.0 / 3 - h) * 6;
	return m1;
}

static void test_hls(void)
{
	static const int ls[] = { 0, 1, 25, 50, 75, 99, 100 };
	static const int ss[] = { 0, 1, 50, 100 };
	struct sixel_image im;
	char body[64];
	unsigned int h, i, j;
	int worst = 0;

	for (h = 0; h < 360; h += 3)
		for (i = 0; i < sizeof(ls) / sizeof(*ls); i++)
			for (j = 0; j < sizeof(ss) / sizeof(*ss); j++) {
				double l = ls[i] / 100.0, s = ss[j] / 100.0;
				double m2 = l <= 0.5 ? l * (1 + s) : l + s - l * s;
				double m1 = 2 * l - m2, hh = ((h + 240) % 360) / 360.0;
				double e[3];
				uint32_t c;
				int k;

				e[0] = s == 0 ? l : hue_v(m1, m2, hh + 1.0 / 3);
				e[1] = s == 0 ? l : hue_v(m1, m2, hh);
				e[2] = s == 0 ? l : hue_v(m1, m2, hh - 1.0 / 3);
				snprintf(body, sizeof(body), "#9;1;%u;%d;%d~", h, ls[i], ss[j]);
				CHECK(dec(0, NULL, &wide, body, &im) == 0);
				c = im.pal[9];
				free_img(&im);
				for (k = 0; k < 3; k++) {
					int got = c >> (16 - 8 * k) & 255;
					int want = (int)(e[k] * 255 + 0.5);
					int d = got > want ? got - want : want - got;

					if (d > worst)
						worst = d;
					CHECK(d <= 1);
				}
			}
	printf("hls: worst channel error %d\n", worst);
	/* hue 0 is blue, 120 red, 240 green */
	CHECK(dec(0, NULL, &wide, "#1;1;0;50;100#2;1;120;50;100#3;1;240;50;100~", &im) == 0);
	CHECK(im.pal[1] == 0x0000ff && im.pal[2] == 0xff0000 && im.pal[3] == 0x00ff00);
	free_img(&im);
	CHECK(dec(0, NULL, &wide, "#1;1;0;100;0#2;1;77;0;50#3;1;5;50;0~", &im) == 0);
	CHECK(im.pal[1] == 0xffffff && im.pal[2] == 0 && im.pal[3] == 0x808080);
	free_img(&im);
}

static void test_colours(void)
{
	struct sixel_image im;

	/* register 300 is register 44 */
	CHECK(dec(0, NULL, &wide, "#300;2;100;0;0~#44~", &im) == 0);
	CHECK(im.w == 2 && im.pal[44] == 0xff0000 && im.pix[0] == 44 && im.pix[1] == 44);
	free_img(&im);
	/* percent table: 0, 50 -> 128, 100, above 100 clamps */
	CHECK(dec(0, NULL, &wide, "#1;2;0;50;100#2;2;101;65535;7~", &im) == 0);
	CHECK(im.pal[1] == 0x0080ff && im.pal[2] == 0xffff12);
	free_img(&im);
	/* defaults: VT340 for 0 to 15, black above */
	CHECK(dec(0, NULL, &wide, "#15~#16~", &im) == 0);
	CHECK(im.pal[0] == 0 && im.pal[1] == 0x3333cc && im.pal[2] == 0xcc2121 &&
	      im.pal[15] == 0xcccccc && im.pal[16] == 0 && im.pal[255] == 0);
	free_img(&im);
	/* a definition cut short selects but does not define */
	CHECK(dec(0, NULL, &wide, "#25;2;100;100~", &im) == 0);
	CHECK(im.pal[25] == 0 && im.pix[0] == 25);
	free_img(&im);
	/* a definition at the end of the string still takes effect */
	CHECK(dec(0, NULL, &wide, "~#7;2;100;0;100", &im) == 0);
	CHECK(im.pal[7] == 0xff00ff);
	free_img(&im);
	/* a colour in every register: each pixel gets its own */
	{
		char body[4096], *p = body;
		int i;

		for (i = 0; i < 256; i++)
			p += sprintf(p, "#%d;2;%d;0;0~", i, i % 101);
		*p = 0;
		CHECK(dec(0, NULL, &wide, body, &im) == 0);
		CHECK(im.w == 256);
		for (i = 0; i < 256; i++)
			CHECK(im.pix[i] == i);
		free_img(&im);
	}
}

static void test_repeat(void)
{
	struct sixel_image im;
	struct sixel_limits l100 = { 100, 100000 };
	struct sixel_limits l300 = { 300, 200000 };
	unsigned int x;

	CHECK(dec(0, NULL, &wide, "!0~", &im) == 0 && im.w == 1);
	free_img(&im);
	CHECK(dec(0, NULL, &wide, "!1~!2~", &im) == 0 && im.w == 3);
	free_img(&im);
	CHECK(dec(0, NULL, &l100, "!4000000000~", &im) == 0);
	CHECK(im.w == 100 && im.mask == NULL);
	free_img(&im);
	/* parameters saturate at 65535 rather than wrap: 65536 is not 0, and
	 * register 65580 is 65535 & 255 */
	CHECK(dec(0, NULL, &wide, "!65536~", &im) == 0 && im.w == 4096);
	free_img(&im);
	CHECK(dec(0, NULL, &wide, "#65580;2;100;0;0~", &im) == 0);
	CHECK(im.pix[0] == 255 && im.pal[255] == 0xff0000);
	free_img(&im);
	/* a repeat is dropped when no data byte follows it */
	CHECK(dec(0, NULL, &wide, "!5#2~", &im) == 0 && im.w == 1 && im.pix[0] == 2);
	free_img(&im);
	CHECK(dec(0, NULL, &wide, "!5;3~", &im) == 0 && im.w == 1);
	free_img(&im);
	/* x saturates: a second run after a huge one adds nothing */
	CHECK(dec(0, NULL, &l100, "!65535~!65535~!65535~$~", &im) == 0);
	CHECK(im.w == 100);
	free_img(&im);
	/* max_w clips; the pixels past it are not kept */
	{
		char body[600];

		memset(body, 0, sizeof(body));
		strcpy(body, "#1;2;100;0;0!240~");
		CHECK(dec(0, NULL, &l100, body, &im) == 0);
		CHECK(im.w == 100 && im.h == 2 * 6);
		for (x = 0; x < 100; x++)
			CHECK(im.pix[x] == 1);
		free_img(&im);
	}
	/* the size hint */
	CHECK(dec(0, NULL, &l300, "\"1;1;60000;60000;60000;60000~", &im) == 0);
	CHECK(im.w == 300 && im.h <= 200000 / 300 && (size_t)im.w * im.h <= 200000);
	free_img(&im);
	CHECK(dec(0, NULL, &l300, "\"60000;60000;7;9~", &im) == 0);
	CHECK(im.w == 7 && im.h == 9);
	free_img(&im);
	/* Pan / Pad: 3, clamped to 5, clamped to 1 */
	CHECK(dec(0, NULL, &wide, "\"3;1;;~", &im) == 0 && im.h == 18);
	free_img(&im);
	CHECK(dec(0, NULL, &wide, "\"60000;1;;~", &im) == 0 && im.h == 30);
	free_img(&im);
	CHECK(dec(0, NULL, &wide, "\"1;60000;;~", &im) == 0 && im.h == 6);
	free_img(&im);
	/* a zero Pan or Pad leaves the aspect of P1 */
	CHECK(dec(0, NULL, &wide, "\"0;1;;~", &im) == 0 && im.h == 12);
	free_img(&im);
	/* raster attributes after data are ignored */
	CHECK(dec(0, NULL, &wide, "~\"1;1;50;50~", &im) == 0);
	CHECK(im.w == 2 && im.h == 12);
	free_img(&im);
}

static void test_aspect_and_p2(void)
{
	static const int rows[] = { 2, 2, 5, 3, 3, 2, 2, 1, 1, 1, 2, 2 };
	struct sixel_image im;
	int p, argv[2];

	for (p = 0; p < 12; p++) {
		argv[0] = p;
		CHECK(dec(1, argv, &wide, "~", &im) == 0);
		CHECK((int)im.h == 6 * rows[p]);
		CHECK(im.gy == 0);
		free_img(&im);
	}
	argv[0] = -1;
	CHECK(dec(1, argv, &wide, "~", &im) == 0 && im.h == 12);
	free_img(&im);
	CHECK(dec(0, NULL, &wide, "~", &im) == 0 && im.h == 12);
	free_img(&im);

	/* P2 = 1: the height is that of the last set pixel; otherwise at
	 * least Pv, the rest unset */
	argv[0] = 7;
	argv[1] = 1;
	CHECK(dec(2, argv, &wide, "\"1;1;10;20~", &im) == 0);
	CHECK(im.transparent && im.w == 10 && im.h == 6);
	free_img(&im);
	argv[1] = 0;
	CHECK(dec(2, argv, &wide, "\"1;1;10;20~", &im) == 0);
	CHECK(!im.transparent && im.w == 10 && im.h == 20 && im.mask);
	CHECK(is_set(&im, 0, 5) && !is_set(&im, 0, 6) && !is_set(&im, 9, 19) &&
	      !is_set(&im, 1, 0) && im.pix[19 * 10] == 0);
	free_img(&im);
	argv[1] = 2;
	CHECK(dec(2, argv, &wide, "~", &im) == 0 && !im.transparent);
	free_img(&im);
	argv[1] = -1;
	CHECK(dec(2, argv, &wide, "~", &im) == 0 && !im.transparent);
	free_img(&im);
}

static void test_edges(void)
{
	struct sixel_image im;
	struct sixel_limits small = { 100, 1000 };
	char body[1024], *p;
	int i, c;

	CHECK(dec(0, NULL, &wide, "", &im) == -1 && im.w == 0 && im.h == 0);
	/* two graphics new lines, aspect 2: 12 rows each */
	CHECK(dec(0, NULL, &wide, "$-$-", &im) == -1 && im.gy == 24);
	CHECK(dec(0, NULL, &wide, "?", &im) == -1 || im.h == 0);
	/* only a raster size: nothing was drawn, the hint alone is a picture */
	CHECK(dec(0, NULL, &wide, "\"1;1;4;4", &im) == 0 && im.w == 4 && im.h == 4);
	CHECK(im.mask && !is_set(&im, 0, 0));
	free_img(&im);

	/* bytes outside the alphabet are ignored */
	CHECK(dec(0, NULL, &wide, "~\001 \177\377~", &im) == 0 && im.w == 2);
	free_img(&im);

	/* a canvas past the pixel limit: 100 x 10 rows, the second band of 6
	 * does not fit after the first */
	p = body;
	for (i = 0; i < 5; i++) {
		memcpy(p, "!100~-", 6);
		p += 6;
	}
	*p = 0;
	CHECK(dec(0, NULL, &small, "\"1;1;;;" , &im) == -1);
	CHECK(dec(1, (int[]){ 7 }, &small, body, &im) == 0);
	CHECK(im.truncated && im.h == 6 && im.w == 100 && im.gy == 30);
	CHECK((size_t)im.w * im.h <= small.max_pixels);
	free_img(&im);
	CHECK(dec(1, (int[]){ 7 }, &wide, body, &im) == 0);
	CHECK(!im.truncated && im.h == 30 && im.mask == NULL);
	free_img(&im);
	/* the height limit of the side */
	{
		struct sixel_limits tall = { 1, (size_t)1 << 30 };
		char *big = malloc(2 * 1000 + 8), *q = big;
		struct sixel_image t;

		for (i = 0; i < 1000; i++) {
			*q++ = '~';
			*q++ = '-';
		}
		*q = 0;
		CHECK(dec(1, (int[]){ 7 }, &tall, big, &t) == 0);
		CHECK(t.truncated && t.h <= SIXEL_MAX_SIDE && t.h >= SIXEL_MAX_SIDE - 6);
		free_img(&t);
		free(big);
	}
	/* a limit of nothing draws nothing */
	{
		struct sixel_limits none = { 100, 0 };

		CHECK(dec(0, NULL, &none, "!5~", &im) == -1);
	}
	/* max_w is clamped into 1 to SIXEL_MAX_SIDE */
	{
		struct sixel_limits z = { 0, 1000 }, huge = { 100000, 1u << 30 };

		CHECK(dec(0, NULL, &z, "!5~", &im) == 0 && im.w == 1);
		free_img(&im);
		CHECK(dec(0, NULL, &huge, "!5000~", &im) == 0 && im.w == 4096);
		free_img(&im);
	}

	/* every byte after the introducers: no crash, nothing past the limits */
	for (c = 0; c < 256; c++) {
		static const char *lead[] = { "#", "!", "\"", "#1;", "!7", "\"1;1;", "#1;1;" };
		unsigned int k;

		for (k = 0; k < sizeof(lead) / sizeof(*lead); k++) {
			snprintf(body, sizeof(body), "%s%c~~%c;5#2;2;10;10;10!3~", lead[k],
				 c ? c : 1, c ? c : 1);
			if (dec(0, NULL, &small, body, &im) == 0) {
				CHECK(im.w >= 1 && im.w <= small.max_w);
				CHECK((size_t)im.w * im.h <= small.max_pixels);
				free_img(&im);
			}
		}
	}
}

static void test_mask(void)
{
	struct sixel_image im;

	/* all set: no mask */
	CHECK(dec(0, NULL, &wide, "~~~", &im) == 0 && im.mask == NULL);
	free_img(&im);
	/* one pixel unset, in a full byte of the mask and in the last,
	 * partial one: the mask stays */
	CHECK(dec(1, (int[]){ 7 }, &wide, "~}", &im) == 0 && im.mask != NULL);
	CHECK(im.w == 2 && im.h == 6 && !is_set(&im, 1, 0) && is_set(&im, 1, 1) &&
	      is_set(&im, 0, 0));
	free_img(&im);
	CHECK(dec(1, (int[]){ 7 }, &wide, "~^", &im) == 0 && im.mask != NULL);
	CHECK(im.w == 2 && im.h == 6 && !is_set(&im, 1, 5) && is_set(&im, 0, 5));
	free_img(&im);
	CHECK(dec(0, NULL, &wide, "?~", &im) == 0 && im.mask != NULL);
	CHECK(!is_set(&im, 0, 0) && is_set(&im, 1, 0));
	free_img(&im);
	/* a stride of 300 compacted to 2: (0,0) and (1,1) */
	CHECK(dec(1, (int[]){ 7 }, &(struct sixel_limits){ 300, 90000 }, "@A", &im) == 0);
	CHECK(im.w == 2 && im.h == 2 && im.mask && im.mask[0] == 0x09);
	CHECK(is_set(&im, 0, 0) && is_set(&im, 1, 1) && !is_set(&im, 1, 0) && !is_set(&im, 0, 1));
	free_img(&im);
	/* P2 = 1: unset pixels stay unset under a later register */
	CHECK(dec(2, (int[]){ 7, 1 }, &wide, "#1@#2A", &im) == 0);
	CHECK(im.pix[0] == 1 && im.pix[3] == 2 && im.mask[0] == 0x09);
	free_img(&im);
	/* overdrawing with $ replaces the register of a pixel */
	CHECK(dec(1, (int[]){ 7 }, &wide, "#1~~$#2~", &im) == 0);
	CHECK(im.pix[0] == 2 && im.pix[1] == 1 && im.mask == NULL);
	free_img(&im);
}

/* A model of the picture for random strings: a grid of register or -1. */
#define MW 40
#define MH 1024

static void test_random(void)
{
	int iter;

	for (iter = 0; iter < 400; iter++) {
		struct sixel_limits lim = { 1 + rnd() % 40, 100000 };
		static int grid[MH][MW];
		char body[2048], *p = body;
		unsigned int aspect = 1 + rnd() % 5, x = 0, y = 0, maxx = 0, n, k, bottom = 0;
		int reg = 0, argv[1], row, col;
		struct sixel_image im;
		int ops = 1 + rnd() % 60;

		for (row = 0; row < MH; row++)
			for (col = 0; col < MW; col++)
				grid[row][col] = -1;
		argv[0] = aspect == 1 ? 7 : aspect == 2 ? 0 : aspect == 3 ? 3 : aspect == 5 ? 2 : 0;
		if (aspect == 4) {
			aspect = 2;
			argv[0] = 0;
		}
		while (ops--) {
			unsigned int op = rnd() % 10;

			if (op == 0) {
				reg = rnd() % 256;
				p += sprintf(p, "#%d", reg);
			} else if (op == 1) {
				x = 0;
				*p++ = '$';
			} else if (op == 2) {
				x = 0;
				y += 6 * aspect;
				*p++ = '-';
			} else {
				unsigned int bits = rnd() % 64;
				int rep = op >= 6;

				n = rep ? 1 + rnd() % 30 : 1;
				if (rep)
					p += sprintf(p, "!%u", n);
				*p++ = 0x3f + bits;
				for (k = 0; k < 6 && y + 6 * aspect <= MH; k++) {
					unsigned int r, xx;

					if (!(bits >> k & 1))
						continue;
					for (r = 0; r < aspect; r++)
						for (xx = x; xx < x + n && xx < lim.max_w; xx++)
							grid[y + k * aspect + r][xx] = reg;
					if (x < lim.max_w && y + (k + 1) * aspect > bottom)
						bottom = y + (k + 1) * aspect;
				}
				x += n;
				if (x > maxx)
					maxx = x;
				continue;
			}
		}
		*p = 0;
		/* the model skipped data past MH rows, so stay below it */
		if (y + 6 * aspect > MH)
			continue;
		if (dec(1, argv, &lim, body, &im) != 0) {
			CHECK(bottom == 0 || (maxx == 0));
			continue;
		}
		CHECK(im.w == (maxx < lim.max_w ? maxx : lim.max_w));
		CHECK(im.h == bottom);
		CHECK(im.gy == y);
		for (row = 0; row < (int)im.h; row++)
			for (col = 0; col < (int)im.w; col++) {
				int g = grid[row][col];

				CHECK(is_set(&im, col, row) == (g >= 0));
				if (g >= 0)
					CHECK(im.pix[(size_t)row * im.w + col] == g);
			}
		free_img(&im);
	}
}

/* the file mode */

static void put_json(FILE *f, int *first, int n, const int *p, int np, int w, int h,
		     unsigned int gy, int aborted)
{
	int i;

	fprintf(f, "%s{\"p\": [", *first ? "" : ", ");
	*first = 0;
	for (i = 0; i < np; i++)
		fprintf(f, "%s%d", i ? ", " : "", p[i] < 0 ? 0 : p[i]);
	fprintf(f, "], \"w\": %d, \"h\": %d, \"gy\": %u, \"aborted\": %s, \"n\": %d}",
		w, h, gy, aborted ? "true" : "false", n);
}

static int file_mode(const char *in, const char *out, unsigned int max_w)
{
	FILE *f = fopen(in, "rb"), *j;
	unsigned char *d;
	long len, i;
	int n = 0, first = 1;
	struct sixel_limits lim = { max_w, (size_t)SIXEL_MAX_SIDE * SIXEL_MAX_SIDE };

	if (!f) {
		perror(in);
		return 2;
	}
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	d = malloc(len + 1);
	if (fread(d, 1, len, f) != (size_t)len) {
		perror("read");
		return 2;
	}
	fclose(f);
	j = fopen(out, "w");
	if (!j) {
		perror(out);
		return 2;
	}
	fputc('[', j);
	for (i = 0; i + 1 < len; i++) {
		int p[8], np = 0, cur = -1;
		long k, body;
		char name[4096];
		struct sixel_image im;
		struct sixel *s;
		FILE *r;

		if (d[i] != 0x1b || d[i + 1] != 'P')
			continue;
		/* the ESC P [0-9;]* q of the reference's pattern */
		k = i + 2;
		while (k < len && ((d[k] >= '0' && d[k] <= '9') || d[k] == ';')) {
			if (d[k] == ';') {
				if (np < 8)
					p[np++] = cur;
				cur = -1;
			} else {
				cur = (cur < 0 ? 0 : cur) * 10 + (d[k] - '0');
				if (cur > 65535)
					cur = 65535;
			}
			k++;
		}
		if (k >= len || d[k] != 'q')
			continue;
		if (cur >= 0 || (np && k > i + 2)) {
			if (np < 8)
				p[np++] = cur;
		}
		body = ++k;
		while (k < len && d[k] != 0x1b)
			k++;
		if (k + 1 >= len)
			break;
		snprintf(name, sizeof(name), "%s.%d.rgba", out, n);
		r = fopen(name, "wb");
		if (d[k + 1] != '\\') {
			put_json(j, &first, n, p, 0, 0, 0, 0, 1);
			fclose(r);
			n++;
			i = k - 1;
			continue;
		}
		s = sixel_new(np, p, &lim);
		{
			long at = body;

			while (at < k) {
				long c = 1 + rnd() % (rnd() % 4 == 0 ? 2 : 40);

				if (c > k - at)
					c = k - at;
				sixel_put(s, (const char *)d + at, c);
				at += c;
			}
		}
		if (sixel_finish(s, &im) == 0) {
			unsigned int x, y;

			for (y = 0; y < im.h; y++)
				for (x = 0; x < im.w; x++) {
					uint32_t c = im.pal[im.pix[(size_t)y * im.w + x]];
					unsigned char px[4] = { c >> 16, c >> 8, c, 255 };

					if (!is_set(&im, x, y))
						memset(px, 0, 4);
					fwrite(px, 1, 4, r);
				}
			free_img(&im);
		}
		put_json(j, &first, n, p, np, im.w, im.h, im.gy, 0);
		fclose(r);
		sixel_free(s);
		n++;
		i = k - 1;
	}
	fputs("]\n", j);
	fclose(j);
	free(d);
	return 0;
}

static int time_mode(const char *in, int reps)
{
	FILE *f = fopen(in, "rb");
	unsigned char *d;
	long len, a, b;
	struct sixel_limits lim = { 320, 320 * 400 };
	struct timespec t0, t1;
	int i, p1 = -1;
	struct sixel_image im;

	if (!f) {
		perror(in);
		return 2;
	}
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	d = malloc(len);
	if (fread(d, 1, len, f) != (size_t)len)
		return 2;
	fclose(f);
	for (a = 0; a + 1 < len && !(d[a] == 0x1b && d[a + 1] == 'P'); a++)
		;
	while (a < len && d[a] != 'q')
		a++;
	a++;
	for (b = a; b < len && d[b] != 0x1b; b++)
		;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (i = 0; i < reps; i++) {
		struct sixel *s = sixel_new(0, &p1, &lim);

		sixel_put(s, (const char *)d + a, b - a);
		if (sixel_finish(s, &im) == 0) {
			if (!i)
				printf("%ux%u\n", im.w, im.h);
			free_img(&im);
		}
		sixel_free(s);
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	printf("%ld bytes: %.3f ms per decode\n", b - a,
	       ((t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6) / reps);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc >= 3 && !strcmp(argv[1], "--time"))
		return time_mode(argv[2], argc > 3 ? atoi(argv[3]) : 100);
	if (argc >= 3)
		return file_mode(argv[1], argv[2], argc > 3 ? atoi(argv[3]) : 4096);

	test_hls();
	test_colours();
	test_repeat();
	test_aspect_and_p2();
	test_edges();
	test_mask();
	test_random();
	printf("%s: %d checks, %d failed\n", failed ? "FAILED" : "PASSED", checks, failed);
	return failed ? 1 : 0;
}
