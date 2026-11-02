/* PNG (f=100) and zlib (o=z) for the kitty graphics protocol. Only built
 * with HAVOC_PNG=1. libpng reads progressively, one row at a time, so the
 * decoded picture is the only large buffer. */
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

#include <png.h>
#include <zlib.h>

#include "kitty-png.h"

#define PNG_SIDE_MAX		4096
#define PNG_CHUNK_MAX		65536

/* --- zlib --- */

struct kitty_z {
	z_stream s;
	bool done;
};

struct kitty_z *kitty_z_new(void)
{
	struct kitty_z *z = calloc(1, sizeof *z);

	if (!z)
		return NULL;
	if (inflateInit(&z->s) != Z_OK) {
		free(z);
		return NULL;
	}
	return z;
}

bool kitty_z_put(struct kitty_z *z, const uint8_t *buf, size_t len,
		 bool (*out)(void *data, const uint8_t *buf, size_t len),
		 void *data)
{
	uint8_t tmp[4096];

	if (z->done)
		return len == 0;
	z->s.next_in = (Bytef *)buf;
	z->s.avail_in = len;
	do {
		size_t got;
		int r;

		z->s.next_out = tmp;
		z->s.avail_out = sizeof tmp;
		r = inflate(&z->s, Z_NO_FLUSH);
		got = sizeof tmp - z->s.avail_out;
		if (r != Z_OK && r != Z_STREAM_END && r != Z_BUF_ERROR)
			return false;
		if (got && !out(data, tmp, got))
			return false;
		if (r == Z_STREAM_END) {
			z->done = true;
			return z->s.avail_in == 0;
		}
		if (r == Z_BUF_ERROR && !got)
			return z->s.avail_in == 0;
	} while (z->s.avail_in || z->s.avail_out == 0);
	return true;
}

bool kitty_z_done(const struct kitty_z *z)
{
	return z->done;
}

void kitty_z_free(struct kitty_z *z)
{
	if (!z)
		return;
	inflateEnd(&z->s);
	free(z);
}

/* --- PNG --- */

struct kitty_png {
	png_structp png;
	png_infop info;
	struct kitty_limits lim;
	enum kitty_err err;
	bool done, got_info;
	unsigned int w, h;
	uint16_t *rgb;
	uint8_t *alpha;
	uint8_t aand;
};

static void png_fail(png_structp png, png_const_charp msg)
{
	(void)msg;
	png_longjmp(png, 1);
}

static void png_quiet(png_structp png, png_const_charp msg)
{
	(void)png;
	(void)msg;
}

static void info_cb(png_structp png, png_infop info)
{
	struct kitty_png *p = png_get_progressive_ptr(png);
	png_uint_32 w, h;
	int bd, ct, il;
	size_t n;

	png_get_IHDR(png, info, &w, &h, &bd, &ct, &il, NULL, NULL);
	if (il != PNG_INTERLACE_NONE) {
		p->err = KITTY_EINVAL;
		png_error(png, "interlaced");
	}
	n = (size_t)w * h;
	if (n > p->lim.max_pixels ||
	    (p->lim.room && !p->lim.room(p->lim.data, n))) {
		p->err = KITTY_ENOMEM;
		png_error(png, "too large");
	}
	p->rgb = malloc(n * sizeof *p->rgb);
	p->alpha = malloc(n);
	if (!p->rgb || !p->alpha) {
		p->err = KITTY_ENOMEM;
		png_error(png, "memory");
	}
	p->w = w;
	p->h = h;
	p->aand = 0xff;
	/* every row comes out as RGBA, 8 bits */
	png_set_expand(png);
	if (bd == 16)
		png_set_strip_16(png);
	if (ct == PNG_COLOR_TYPE_GRAY || ct == PNG_COLOR_TYPE_GRAY_ALPHA)
		png_set_gray_to_rgb(png);
	png_set_add_alpha(png, 0xff, PNG_FILLER_AFTER);
	png_read_update_info(png, info);
	p->got_info = true;
}

static void row_cb(png_structp png, png_bytep row, png_uint_32 num, int pass)
{
	struct kitty_png *p = png_get_progressive_ptr(png);
	uint16_t *rgb;
	uint8_t *alpha, aand;
	unsigned int x;

	(void)pass;
	if (!row || num >= p->h)
		return;
	rgb = p->rgb + (size_t)num * p->w;
	alpha = p->alpha + (size_t)num * p->w;
	aand = p->aand;
	for (x = 0; x < p->w; x++, row += 4) {
		rgb[x] = (row[0] >> 3) << 11 | (row[1] >> 2) << 5 | row[2] >> 3;
		alpha[x] = row[3];
		aand &= row[3];
	}
	p->aand = aand;
}

static void end_cb(png_structp png, png_infop info)
{
	struct kitty_png *p = png_get_progressive_ptr(png);

	(void)info;
	p->done = true;
}

struct kitty_png *kitty_png_new(const struct kitty_limits *lim)
{
	struct kitty_png *p = calloc(1, sizeof *p);

	if (!p)
		return NULL;
	p->lim = *lim;
	p->png = png_create_read_struct(PNG_LIBPNG_VER_STRING, p, png_fail,
					png_quiet);
	if (p->png)
		p->info = png_create_info_struct(p->png);
	if (!p->png || !p->info) {
		kitty_png_free(p);
		return NULL;
	}
	/* none of these can fail, so no jump buffer is needed yet */
	png_set_user_limits(p->png, PNG_SIDE_MAX, PNG_SIDE_MAX);
	png_set_chunk_malloc_max(p->png, PNG_CHUNK_MAX);
	png_set_progressive_read_fn(p->png, p, info_cb, row_cb, end_cb);
	return p;
}

bool kitty_png_put(struct kitty_png *p, const uint8_t *buf, size_t len,
		   enum kitty_err *err)
{
	if (p->err) {
		*err = p->err;
		return false;
	}
	if (p->done)
		return true;		/* whatever follows IEND is ignored */
	if (setjmp(png_jmpbuf(p->png))) {
		if (!p->err)
			p->err = KITTY_EINVAL;
		*err = p->err;
		return false;
	}
	png_process_data(p->png, p->info, (png_bytep)buf, len);
	return true;
}

bool kitty_png_finish(struct kitty_png *p, struct kitty_image *out,
		      enum kitty_err *err)
{
	if (p->err || !p->done || !p->got_info) {
		*err = p->err ? p->err : KITTY_ENODATA;
		return false;
	}
	if (p->aand == 0xff) {
		free(p->alpha);
		p->alpha = NULL;
	}
	out->w = p->w;
	out->h = p->h;
	out->rgb = p->rgb;
	out->alpha = p->alpha;
	p->rgb = NULL;
	p->alpha = NULL;
	return true;
}

void kitty_png_free(struct kitty_png *p)
{
	if (!p)
		return;
	if (p->png)
		png_destroy_read_struct(&p->png, p->info ? &p->info : NULL, NULL);
	free(p->rgb);
	free(p->alpha);
	free(p);
}
