/* Sixel decoder: a resumable state machine that fills an 8-bit register
 * canvas and a 1-bit mask. It knows nothing of the terminal; the caller
 * gives it the string between DCS q and ST and the limits it may use. */
#include <stdlib.h>
#include <string.h>

#include "sixel.h"

#define NPARAM 5
#define PARAM_MAX 65535

enum state { GROUND, RASTER, COLOUR, REPEAT };

struct sixel {
	struct sixel_limits lim;
	enum state state;

	/* numeric part of the command in progress */
	unsigned int p[NPARAM];
	unsigned int pi;	/* index of the parameter being read */
	bool anyp;		/* a digit or ; was seen */

	unsigned int aspect;	/* pixel rows per sixel row, 1 to 5 */
	bool transparent;
	bool started;		/* a data byte was seen: raster is over */
	unsigned int ph, pv;	/* size hint from the raster attributes */

	unsigned int reg;	/* selected register */
	unsigned int x, y;	/* graphics cursor; y is the band's top row */
	unsigned int maxx;
	unsigned int bottom;	/* last row with a set pixel + 1 */

	uint8_t *pix;		/* lim.max_w * cap_h */
	uint8_t *mask;
	unsigned int cap_h;
	bool truncated;

	uint32_t pal[256];
};

/* VT340 power-on colours in percent, registers 0 to 15 */
static const uint8_t vt340[16][3] = {
	{ 0, 0, 0 }, { 20, 20, 80 }, { 80, 13, 13 }, { 20, 80, 20 },
	{ 80, 20, 80 }, { 20, 80, 80 }, { 80, 80, 20 }, { 53, 53, 53 },
	{ 26, 26, 26 }, { 33, 33, 60 }, { 60, 26, 26 }, { 33, 60, 33 },
	{ 60, 33, 60 }, { 33, 60, 60 }, { 60, 60, 33 }, { 80, 80, 80 },
};

/* percent to 0..255, rounded: (v * 255 + 50) / 100 */
static uint8_t pct[101];

static void pct_init(void)
{
	unsigned int v;

	if (pct[100])
		return;
	for (v = 0; v <= 100; v++)
		pct[v] = (v * 255 + 50) / 100;
}

struct sixel *sixel_new(int argc, const int *argv,
			const struct sixel_limits *lim)
{
	struct sixel *s = calloc(1, sizeof(*s));
	int p1 = argc > 0 && argv[0] > 0 ? argv[0] : 0;
	unsigned int i;

	if (!s)
		return NULL;
	pct_init();
	s->lim = *lim;
	if (s->lim.max_w < 1)
		s->lim.max_w = 1;
	if (s->lim.max_w > SIXEL_MAX_SIDE)
		s->lim.max_w = SIXEL_MAX_SIDE;
	/* the DEC table; unknown values act as 0 */
	s->aspect = p1 == 2 ? 5 : p1 == 3 || p1 == 4 ? 3 :
		    p1 >= 7 && p1 <= 9 ? 1 : 2;
	s->transparent = argc > 1 && argv[1] == 1;
	for (i = 0; i < 16; i++)
		s->pal[i] = (uint32_t)pct[vt340[i][0]] << 16 |
			    pct[vt340[i][1]] << 8 | pct[vt340[i][2]];
	return s;
}

void sixel_free(struct sixel *s)
{
	if (!s)
		return;
	free(s->pix);
	free(s->mask);
	free(s);
}

/* Colours, once per definition (so / and % are fine here). */

static uint32_t hls_to_rgb(unsigned int hue, unsigned int l, unsigned int sat)
{
	/* hue 0 is blue; L and S in hundredths of a percent */
	int h = (hue % 360 + 240) % 360;
	int L = (l > 100 ? 100 : l) * 100;
	int S = (sat > 100 ? 100 : sat) * 100;
	int q, p, c[3], t, i;

	if (S == 0) {
		c[0] = c[1] = c[2] = (L * 255 + 5000) / 10000;
		return (uint32_t)c[0] << 16 | c[1] << 8 | c[2];
	}
	q = L <= 5000 ? L * (10000 + S) / 10000 : L + S - L * S / 10000;
	p = 2 * L - q;
	for (i = 0; i < 3; i++) {
		int v;

		t = (h + (i == 0 ? 120 : i == 1 ? 0 : 240)) % 360;
		if (t < 60)
			v = p + (q - p) * t / 60;
		else if (t < 180)
			v = q;
		else if (t < 240)
			v = p + (q - p) * (240 - t) / 60;
		else
			v = p;
		c[i] = (v * 255 + 5000) / 10000;
	}
	return (uint32_t)c[0] << 16 | c[1] << 8 | c[2];
}

static unsigned int pct_of(unsigned int v)
{
	return pct[v > 100 ? 100 : v];
}

/* The numeric part of a command in progress ends at a byte that is not a
 * digit or ;. */

static unsigned int nparams(const struct sixel *s)
{
	return !s->anyp ? 0 : s->pi + 1 > NPARAM ? NPARAM : s->pi + 1;
}

static void end_raster(struct sixel *s)
{
	unsigned int n = nparams(s);

	/* only before the first data byte: later it would change the
	 * geometry of what is drawn */
	if (s->started)
		return;
	if (n >= 2 && s->p[0] && s->p[1]) {
		unsigned int a = s->p[0] / s->p[1];

		s->aspect = a < 1 ? 1 : a > 5 ? 5 : a;
	}
	if (n >= 3)
		s->ph = s->p[2] > SIXEL_MAX_SIDE ? SIXEL_MAX_SIDE : s->p[2];
	if (n >= 4)
		s->pv = s->p[3] > SIXEL_MAX_SIDE ? SIXEL_MAX_SIDE : s->p[3];
}

static void end_colour(struct sixel *s)
{
	unsigned int n = nparams(s);
	unsigned int reg = s->p[0] & 255;	/* the DEC terminals wrapped */

	s->reg = reg;
	if (n < 5)
		return;
	if (s->p[1] == 2)
		s->pal[reg] = pct_of(s->p[2]) << 16 | pct_of(s->p[3]) << 8 |
			      pct_of(s->p[4]);
	else if (s->p[1] == 1)
		s->pal[reg] = hls_to_rgb(s->p[2], s->p[3], s->p[4]);
}

static void start_command(struct sixel *s, enum state st)
{
	memset(s->p, 0, sizeof(s->p));
	s->pi = 0;
	s->anyp = false;
	s->state = st;
}

/* Canvas. It is max_w wide from the start and grows a band at a time. */

static bool grow(struct sixel *s, unsigned int need)
{
	size_t w = s->lim.max_w;
	uint8_t *np, *nm;

	if (need <= s->cap_h)
		return true;
	if (need > SIXEL_MAX_SIDE || w * need > s->lim.max_pixels)
		return false;
	np = realloc(s->pix, w * need);
	if (!np)
		return false;
	s->pix = np;
	nm = realloc(s->mask, (w * need + 7) >> 3);
	if (!nm)
		return false;
	s->mask = nm;
	memset(s->pix + w * s->cap_h, 0, w * (need - s->cap_h));
	/* a byte shared by the old and the new rows keeps its bits: the new
	 * ones were never set */
	memset(s->mask + ((w * s->cap_h + 7) >> 3), 0,
	       ((w * need + 7) >> 3) - ((w * s->cap_h + 7) >> 3));
	s->cap_h = need;
	return true;
}

static void mask_run(uint8_t *m, size_t b, unsigned int n)
{
	size_t i = b >> 3;
	unsigned int sh = b & 7;

	if (sh) {
		unsigned int nb = 8 - sh;

		if (n < nb) {
			m[i] |= ((1u << n) - 1) << sh;
			return;
		}
		m[i++] |= 0xff << sh;
		n -= nb;
	}
	memset(m + i, 0xff, n >> 3);
	i += n >> 3;
	if (n & 7)
		m[i] |= (1u << (n & 7)) - 1;
}

static void draw(struct sixel *s, unsigned int bits, unsigned int n)
{
	unsigned int w = s->lim.max_w, x0 = s->x, x1, run, k, r;
	unsigned int top, last = 0;

	s->started = true;
	s->x = s->x + n > SIXEL_MAX_SIDE + 1 ? SIXEL_MAX_SIDE + 1 : s->x + n;
	if (s->x > s->maxx)
		s->maxx = s->x;
	if (!bits || x0 >= w)
		return;
	if (!grow(s, s->y + 6 * s->aspect)) {
		s->truncated = true;
		return;
	}
	x1 = x0 + n > w ? w : x0 + n;
	run = x1 - x0;
	for (k = 0; k < 6; k++) {
		if (!(bits >> k & 1))
			continue;
		top = s->y + k * s->aspect;
		for (r = 0; r < s->aspect; r++) {
			size_t at = (size_t)(top + r) * w + x0;

			memset(s->pix + at, s->reg, run);
			mask_run(s->mask, at, run);
		}
		last = top + s->aspect;
	}
	if (last > s->bottom)
		s->bottom = last;
}

static void ground(struct sixel *s, unsigned char c)
{
	switch (c) {
	case '"':
		start_command(s, RASTER);
		break;
	case '#':
		start_command(s, COLOUR);
		break;
	case '!':
		start_command(s, REPEAT);
		break;
	case '$':
		s->x = 0;
		break;
	case '-':
		s->x = 0;
		/* far past anything that fits; keeps y from wrapping */
		if (s->y < (1u << 24))
			s->y += 6 * s->aspect;
		break;
	default:
		if (c >= 0x3f && c <= 0x7e)
			draw(s, c - 0x3f, 1);
		break;
	}
}

static void put_byte(struct sixel *s, unsigned char c)
{
	if (s->state == GROUND) {
		ground(s, c);
		return;
	}
	if (c >= '0' && c <= '9') {
		if (s->pi < NPARAM) {
			unsigned int v = s->p[s->pi] * 10 + (c - '0');

			s->p[s->pi] = v > PARAM_MAX ? PARAM_MAX : v;
		}
		s->anyp = true;
		return;
	}
	if (c == ';' && s->state != REPEAT) {
		if (s->pi < NPARAM)
			s->pi++;
		s->anyp = true;
		return;
	}
	if (s->state == REPEAT) {
		s->state = GROUND;
		if (c >= 0x3f && c <= 0x7e) {
			/* !0 repeats once */
			draw(s, c - 0x3f, s->p[0] ? s->p[0] : 1);
			return;
		}
	} else {
		if (s->state == RASTER)
			end_raster(s);
		else
			end_colour(s);
		s->state = GROUND;
	}
	ground(s, c);
}

void sixel_put(struct sixel *s, const char *buf, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++)
		put_byte(s, (unsigned char)buf[i]);
}

/* Bits past the end of the last byte are clear. */
static bool all_set(const uint8_t *m, size_t bits)
{
	size_t i, n = bits >> 3;

	for (i = 0; i < n; i++)
		if (m[i] != 0xff)
			return false;
	return !(bits & 7) || m[n] == (1u << (bits & 7)) - 1;
}

int sixel_finish(struct sixel *s, struct sixel_image *out)
{
	size_t maxw = s->lim.max_w, w, h, hmax, have, y;
	size_t total, nbytes;
	uint8_t *pix, *mask;

	memset(out, 0, sizeof(*out));
	out->gy = s->y;
	out->transparent = s->transparent;
	out->truncated = s->truncated;

	/* an unfinished command still counts */
	if (s->state == RASTER)
		end_raster(s);
	else if (s->state == COLOUR)
		end_colour(s);
	s->state = GROUND;
	memcpy(out->pal, s->pal, sizeof(out->pal));

	w = s->maxx > s->ph ? s->maxx : s->ph;
	if (w > maxw)
		w = maxw;
	h = s->bottom;
	if (!s->transparent && s->pv > h)
		h = s->pv;
	hmax = s->lim.max_pixels / maxw;
	if (hmax > SIXEL_MAX_SIDE)
		hmax = SIXEL_MAX_SIDE;
	if (h > hmax)
		h = hmax;
	if (!w || !h)
		return -1;

	/* rows past what was drawn are unset, rows past h are dropped */
	have = h < s->cap_h ? h : s->cap_h;
	if (w != maxw)
		for (y = 1; y < have; y++)
			memmove(s->pix + y * w, s->pix + y * maxw, w);
	pix = realloc(s->pix, w * h);
	if (!pix) {
		/* a failed shrink keeps the larger buffer; a failed growth
		 * (the size hint taller than the canvas) has no room */
		if (h > have)
			return -1;
		pix = s->pix;
	}
	s->pix = pix;
	if (h > have)
		memset(pix + have * w, 0, (h - have) * w);

	total = w * h;
	nbytes = (total + 7) >> 3;
	mask = calloc(nbytes, 1);
	if (!mask)
		return -1;
	if (!have) {
		/* nothing drawn: the picture is all unset */
	} else if (w == maxw) {
		memcpy(mask, s->mask, (have * w + 7) >> 3);
		/* the last byte may hold bits of rows that were dropped */
		if (total & 7)
			mask[nbytes - 1] &= (1u << (total & 7)) - 1;
	} else {
		const uint8_t *src = s->mask;
		size_t sb = 0, db = 0, x;

		for (y = 0; y < have; y++) {
			for (x = 0; x < w; x++, sb++, db++)
				if (src[sb >> 3] >> (sb & 7) & 1)
					mask[db >> 3] |= 1u << (db & 7);
			sb += maxw - w;
		}
	}
	if (all_set(mask, total)) {
		free(mask);
		mask = NULL;
	}

	out->w = w;
	out->h = h;
	out->pix = pix;
	out->mask = mask;
	s->pix = NULL;
	free(s->mask);
	s->mask = NULL;
	s->cap_h = 0;
	return 0;
}
