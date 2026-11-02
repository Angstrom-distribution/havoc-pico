#ifndef HAVOC_KITTY_PNG_H
#define HAVOC_KITTY_PNG_H

#include "kitty.h"

/* zlib (o=z): inflate, handing the bytes to out as they come */
struct kitty_z;
struct kitty_z *kitty_z_new(void);
/* false: zlib error, data after the end of the stream, or out false */
bool kitty_z_put(struct kitty_z *z, const uint8_t *buf, size_t len,
		 bool (*out)(void *data, const uint8_t *buf, size_t len),
		 void *data);
bool kitty_z_done(const struct kitty_z *z);	/* Z_STREAM_END seen */
void kitty_z_free(struct kitty_z *z);

/* PNG (f=100) through libpng's progressive reader */
struct kitty_png;
struct kitty_png *kitty_png_new(const struct kitty_limits *lim);
/* false: error, *err set (EINVAL bad or interlaced PNG, ENOMEM size) */
bool kitty_png_put(struct kitty_png *p, const uint8_t *buf, size_t len,
		   enum kitty_err *err);
/* true: the PNG ended and *out holds it; false: *err (ENODATA if short) */
bool kitty_png_finish(struct kitty_png *p, struct kitty_image *out,
		      enum kitty_err *err);
void kitty_png_free(struct kitty_png *p);

#endif
