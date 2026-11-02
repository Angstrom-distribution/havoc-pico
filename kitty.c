/* Kitty graphics protocol, receive side: APC parser, chunk handling and
 * decoding to RGB565 (plus an alpha plane). Placement is not done here. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base64.h"
#include "kitty.h"
#include "kitty-shm.h"
#if HAVOC_PNG
#include "kitty-png.h"
#endif

#define CTRL_MAX	512
#define NAME_MAX_LEN	255
#define SIDE_MAX	4096

enum { ST_IDLE, ST_CTRL, ST_DATA, ST_NOTK };

/* raw pixels in, RGB565 and alpha out; a pixel split between two calls is
 * kept in pend */
struct kraw {
	uint16_t *rgb;
	uint8_t *alpha;		/* NULL for f=24 */
	size_t total, done;	/* pixels */
	unsigned int bpp;	/* 3 or 4 */
	unsigned int w, h;
	uint8_t pend[4];
	unsigned int npend;
	uint8_t aand;		/* AND of all alpha bytes */
};

struct kitty {
	struct kitty_limits lim;
	int state;
	char ctrl[CTRL_MAX];
	size_t ctrl_len;
	bool ctrl_over;

	bool pending;		/* a chunked transfer waits for its next chunk */
	struct kitty_cmd cmd;	/* of the transfer: keys of its first chunk */
	bool more;		/* this chunk has m=1 */
	bool chunkable;		/* the command carries a payload */
	bool transmit;		/* a=t, T or q with usable keys */
	enum kitty_err err;	/* sticky until the transfer ends */
	bool discard;		/* ignore the payload */
	struct b64 b64;

	bool by_name;		/* t=s: the payload is the object's name */
	char name[NAME_MAX_LEN + 1];
	size_t name_len;	/* NAME_MAX_LEN + 1: too long */

	struct kraw raw;
	bool sink_on;		/* a sink holds the transfer's data */
#if HAVOC_PNG
	struct kitty_z *z;	/* o=z */
	size_t zout;		/* bytes inflated so far */
	struct kitty_png *png;	/* f=100 */
#endif

	struct kitty_result prior;
	bool have_prior;
};

static inline uint16_t rgb565(unsigned int r, unsigned int g, unsigned int b)
{
	return (r >> 3) << 11 | (g >> 2) << 5 | b >> 3;
}

static void raw_free(struct kraw *r)
{
	free(r->rgb);
	free(r->alpha);
	memset(r, 0, sizeof *r);
}

static bool raw_init(struct kitty *k, unsigned int w, unsigned int h, bool alpha)
{
	struct kraw *r = &k->raw;
	size_t n = (size_t)w * h;

	memset(r, 0, sizeof *r);
	if (n > k->lim.max_pixels)
		return false;
	if (k->lim.room && !k->lim.room(k->lim.data, n))
		return false;
	r->rgb = malloc(n * sizeof *r->rgb);
	if (alpha)
		r->alpha = malloc(n);
	if (!r->rgb || (alpha && !r->alpha)) {
		raw_free(r);
		return false;
	}
	r->total = n;
	r->w = w;
	r->h = h;
	r->bpp = alpha ? 4 : 3;
	r->aand = 0xff;
	return true;
}

/* false: more bytes than the picture holds */
static bool raw_put(struct kraw *r, const uint8_t *b, size_t n)
{
	size_t done = r->done;
	uint8_t aand = r->aand;
	const size_t total = r->total;
	const unsigned int bpp = r->bpp;

	while (n) {
		if (done == total)
			return false;
		if (r->npend) {
			while (n && r->npend < bpp) {
				r->pend[r->npend++] = *b++;
				n--;
			}
			if (r->npend < bpp)
				break;
			r->rgb[done] = rgb565(r->pend[0], r->pend[1], r->pend[2]);
			if (bpp == 4) {
				r->alpha[done] = r->pend[3];
				aand &= r->pend[3];
			}
			done++;
			r->npend = 0;
			continue;
		}
		if (bpp == 3) {
			while (n >= 3 && done < total) {
				r->rgb[done++] = rgb565(b[0], b[1], b[2]);
				b += 3;
				n -= 3;
			}
		} else {
			while (n >= 4 && done < total) {
				r->rgb[done] = rgb565(b[0], b[1], b[2]);
				r->alpha[done++] = b[3];
				aand &= b[3];
				b += 4;
				n -= 4;
			}
		}
		if (n && done == total)
			return false;
		while (n) {
			r->pend[r->npend++] = *b++;
			n--;
		}
	}
	r->done = done;
	r->aand = aand;
	return true;
}

/* the picture, when every byte arrived */
static bool raw_finish(struct kraw *r, struct kitty_image *img)
{
	if (r->done != r->total || r->npend)
		return false;
	if (r->alpha && r->aand == 0xff) {
		free(r->alpha);
		r->alpha = NULL;
	}
	img->w = r->w;
	img->h = r->h;
	img->rgb = r->rgb;
	img->alpha = r->alpha;
	r->rgb = NULL;
	r->alpha = NULL;
	return true;
}

/* --- control part --- */

static uint32_t *numfield(struct kitty_cmd *c, char key)
{
	switch (key) {
	case 'f': return &c->f;
	case 'i': return &c->i;
	case 'I': return &c->I;
	case 'p': return &c->p;
	case 's': return &c->s;
	case 'v': return &c->v;
	case 'S': return &c->S;
	case 'O': return &c->O;
	case 'x': return &c->x;
	case 'y': return &c->y;
	case 'w': return &c->w;
	case 'h': return &c->h;
	case 'X': return &c->X;
	case 'Y': return &c->Y;
	case 'c': return &c->c;
	case 'r': return &c->r;
	case 'C': return &c->C;
	case 'q': return &c->q;
	case 'm': return &c->m;
	case 'U': return &c->U;
	}
	return NULL;
}

static char *charfield(struct kitty_cmd *c, char key)
{
	switch (key) {
	case 'a': return &c->a;
	case 't': return &c->t;
	case 'd': return &c->d;
	case 'o': return &c->o;
	}
	return NULL;
}

#define KEYBIT(key)	((uint64_t)1 << ((key) - 'A'))

/* not isalpha: a locale could pass bytes above 0x7f, and KEYBIT needs a
 * letter */
static bool letter(char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static void cmd_defaults(struct kitty_cmd *c)
{
	memset(c, 0, sizeof *c);
	c->a = 't';
	c->t = 'd';
	c->d = 'a';
	c->f = 32;
}

/* false: syntax error */
static bool parse_ctrl(const char *s, size_t n, bool over, struct kitty_cmd *c)
{
	size_t i = 0;

	cmd_defaults(c);
	if (over)
		return false;
	while (i < n) {
		char key = s[i];
		uint32_t *nf;
		char *cf;
		size_t start;

		if (!letter(key) || i + 1 >= n || s[i + 1] != '=')
			return false;
		i += 2;
		start = i;
		while (i < n && s[i] != ',')
			i++;
		if (i == start)
			return false;
		cf = charfield(c, key);
		if (cf) {
			if (i - start != 1 || !letter(s[start]))
				return false;
			if (c->keys & KEYBIT(key) && *cf != s[start])
				return false;
			*cf = s[start];
		} else {
			bool neg = false;
			uint64_t v = 0;
			size_t j = start;

			if (key == 'z' && s[j] == '-') {
				neg = true;
				j++;
				if (j == i)
					return false;
			}
			for (; j < i; j++) {
				if (s[j] < '0' || s[j] > '9')
					return false;
				v = v * 10 + (s[j] - '0');
				if (v > 0xffffffffu)
					v = 0xffffffffu;
			}
			if (key == 'z') {
				int32_t z;

				if (neg)
					z = v > 0x80000000u ? INT32_MIN : -(int64_t)v;
				else
					z = v > INT32_MAX ? INT32_MAX : v;
				if (c->keys & KEYBIT(key) && c->z != z)
					return false;
				c->z = z;
			} else if ((nf = numfield(c, key))) {
				if (c->keys & KEYBIT(key) && *nf != v)
					return false;
				*nf = v;
			}
		}
		c->keys |= KEYBIT(key);
		if (i < n) {
			i++;
			if (i == n)
				return false;
		}
	}
	return true;
}

/* A chunk after one with m=1 belongs to the transfer when it gives nothing
 * but m and q, or only keys the first chunk gave with the same value. */
static bool is_continuation(struct kitty *k, const struct kitty_cmd *c)
{
	uint64_t keys = c->keys & ~(KEYBIT('m') | KEYBIT('q'));
	struct kitty_cmd first = k->cmd, cc = *c;
	char key;

	for (key = 'A'; key <= 'z'; key++) {
		uint32_t *a, *b;
		char *ca, *cb;

		if (!(keys & KEYBIT(key)))
			continue;
		a = numfield(&first, key);
		b = numfield(&cc, key);
		if (a && *a != *b)
			return false;
		ca = charfield(&first, key);
		cb = charfield(&cc, key);
		if (ca && *ca != *cb)
			return false;
		if (key == 'z' && first.z != cc.z)
			return false;
	}
	return true;
}

static enum kitty_err validate(struct kitty *k, struct kitty_cmd *c)
{
	if (strchr("tTpdq", c->a) == NULL)
		return KITTY_EINVAL;
	if (c->keys & (KEYBIT('I') | KEYBIT('U') | KEYBIT('P') | KEYBIT('Q') |
		       KEYBIT('H') | KEYBIT('V')))
		return KITTY_EINVAL;
	if (c->a == 'p' || c->a == 'd')
		return KITTY_OK;
	if (c->f != 24 && c->f != 32 && c->f != 100)
		return KITTY_EINVAL;
	if (c->o != 0 && c->o != 'z')
		return KITTY_EINVAL;
	if (c->O != 0 || c->s > SIDE_MAX || c->v > SIDE_MAX)
		return KITTY_EINVAL;
#if !HAVOC_PNG
	if (c->f == 100 || c->o == 'z')
		return KITTY_EINVAL;
#endif
	if (c->t != 'd' && c->t != 's' && c->t != 'f' && c->t != 't')
		return KITTY_EINVAL;
	if (c->f != 100 && (!c->s || !c->v))
		return KITTY_EINVAL;
	if (c->t == 'f' || c->t == 't')
		return KITTY_EBADF;
	if (c->t == 's' && !k->lim.shm)
		return KITTY_EBADF;
	return KITTY_OK;
}

/* --- transfer --- */

static void sink_free(struct kitty *k)
{
	raw_free(&k->raw);
#if HAVOC_PNG
	kitty_z_free(k->z);
	k->z = NULL;
	kitty_png_free(k->png);
	k->png = NULL;
#endif
	k->sink_on = false;
}

/* Start decoding into a fresh sink. */
static enum kitty_err sink_start(struct kitty *k)
{
	const struct kitty_cmd *c = &k->cmd;

#if HAVOC_PNG
	k->zout = 0;
	if (c->o == 'z') {
		k->z = kitty_z_new();
		if (!k->z)
			return KITTY_ENOMEM;
	}
	if (c->f == 100) {
		k->png = kitty_png_new(&k->lim);
		if (!k->png)
			return KITTY_ENOMEM;
		k->sink_on = true;
		return KITTY_OK;
	}
#endif
	if (!raw_init(k, c->s, c->v, c->f == 32))
		return KITTY_ENOMEM;
	k->sink_on = true;
	return KITTY_OK;
}

static void seterr(struct kitty *k, enum kitty_err err)
{
	if (!k->err)
		k->err = err;
}

static void fail(struct kitty *k, enum kitty_err err)
{
	seterr(k, err);
	sink_free(k);
}

/* Decoded bytes into the picture; must not free the sink (zlib calls it). */
static bool sink_data(void *data, const uint8_t *b, size_t n)
{
	struct kitty *k = data;

#if HAVOC_PNG
	/* libpng ignores what follows IEND, so without a bound a small zlib
	 * stream could keep havoc inflating for a long time; 4 bytes per pixel
	 * of the cap is also the bound for t=s */
	if (k->z && (k->zout += n) > 4 * k->lim.max_pixels) {
		seterr(k, KITTY_ENODATA);
		return false;
	}
	if (k->png) {
		enum kitty_err e = KITTY_OK;

		if (!kitty_png_put(k->png, b, n, &e)) {
			seterr(k, e);
			return false;
		}
		return true;
	}
#endif
	if (!raw_put(&k->raw, b, n)) {
		seterr(k, KITTY_ENODATA);
		return false;
	}
	return true;
}

static bool sink_put(struct kitty *k, const uint8_t *b, size_t n)
{
	bool ok;

	if (!k->sink_on)
		return true;
#if HAVOC_PNG
	if (k->z) {
		ok = kitty_z_put(k->z, b, n, sink_data, k);
		if (!ok)
			seterr(k, kitty_z_done(k->z) ? KITTY_ENODATA : KITTY_EINVAL);
	} else
#endif
		ok = sink_data(k, b, n);
	if (!ok)
		sink_free(k);
	return ok;
}

static void name_put(struct kitty *k, const uint8_t *b, size_t n)
{
	if (k->name_len + n > NAME_MAX_LEN) {
		k->name_len = NAME_MAX_LEN + 1;
		return;
	}
	memcpy(k->name + k->name_len, b, n);
	k->name_len += n;
}

static void payload(struct kitty *k, const char *buf, size_t len)
{
	uint8_t tmp[1024 / 4 * 3 + 3];

	while (len && !k->err && !k->discard) {
		size_t n = len > 1024 ? 1024 : len;
		long r = b64_decode(&k->b64, buf, n, tmp);

		if (r < 0) {
			fail(k, KITTY_EINVAL);
			return;
		}
		if (k->by_name)
			name_put(k, tmp, r);
		else if (!sink_put(k, tmp, r))
			return;
		buf += n;
		len -= n;
	}
}

static void end_prior(struct kitty *k)
{
	k->prior.cmd = k->cmd;
	k->prior.err = k->err ? k->err : KITTY_ENODATA;
	memset(&k->prior.img, 0, sizeof k->prior.img);
	k->have_prior = true;
	sink_free(k);
	k->pending = false;
	k->err = KITTY_OK;
}

static void begin_cmd(struct kitty *k)
{
	struct kitty_cmd c;
	bool ok = parse_ctrl(k->ctrl, k->ctrl_len, k->ctrl_over, &c);

	k->state = ST_DATA;
	if (ok && k->pending && is_continuation(k, &c)) {
		k->more = c.m != 0;
		if (KITTY_HAS(&c, 'q')) {
			k->cmd.q = c.q;
			k->cmd.keys |= KEYBIT('q');
		}
		return;
	}
	if (k->pending)
		end_prior(k);
	sink_free(k);
	k->pending = false;
	k->err = KITTY_OK;
	k->discard = false;
	k->by_name = false;
	k->name_len = 0;
	k->transmit = false;
	k->cmd = c;
	b64_init(&k->b64);
	k->more = false;
	k->chunkable = false;
	if (!ok) {
		k->err = KITTY_EINVAL;
		k->discard = true;
		return;
	}
	k->chunkable = c.a != 'p' && c.a != 'd';
	/* only direct data comes in chunks: mpv sends t=s with m=1 on every
	 * frame and relies on each being complete */
	k->more = k->chunkable && c.t == 'd' && c.m != 0;
	k->err = validate(k, &k->cmd);
	if (k->err || !k->chunkable) {
		k->discard = true;
		return;
	}
	k->transmit = true;
	if (c.t == 's') {
		k->by_name = true;
		return;
	}
	k->err = sink_start(k);
	if (k->err)
		k->discard = true;
}

/* t=s: the payload was the object's name; read the object into the sink.
 * Every failure to use the object is the same reply. */
static bool shm_load(struct kitty *k)
{
	const struct kitty_cmd *c = &k->cmd;
	struct kitty_shm m;
	size_t need, take;
	enum kitty_err err;

	if (k->name_len > NAME_MAX_LEN) {
		fail(k, KITTY_EBADF_SHM);
		return false;
	}
	if (c->f == 100 || c->o == 'z')
		need = 1;
	else
		need = (size_t)c->s * c->v * (c->f == 32 ? 4 : 3);
	if (kitty_shm_open(k->name, k->name_len, need, 4 * k->lim.max_pixels, &m) < 0) {
		fail(k, KITTY_EBADF_SHM);
		return false;
	}
	err = sink_start(k);
	if (err) {
		kitty_shm_close(&m);
		fail(k, err);
		return false;
	}
	take = need;
	if (c->f == 100 || c->o == 'z')
		take = c->S && c->S < m.len ? c->S : m.len;
	if (!sink_put(k, m.data, take)) {
		kitty_shm_close(&m);
		return false;
	}
	kitty_shm_close(&m);
	return true;
}

/* the data of a finished transfer into the result */
static void finish_data(struct kitty *k, struct kitty_image *img)
{
	uint8_t tmp[2];
	int f;

	if (k->err || !k->transmit)
		return;
	f = b64_finish(&k->b64, tmp);
	if (f < 0) {
		fail(k, KITTY_EINVAL);
		return;
	}
	if (k->by_name)
		name_put(k, tmp, f);
	else if (f && !sink_put(k, tmp, f))
		return;
	if (k->by_name && !shm_load(k))
		return;
	if (!k->sink_on) {
		fail(k, KITTY_ENODATA);
		return;
	}
#if HAVOC_PNG
	if (k->z && !kitty_z_done(k->z)) {
		fail(k, KITTY_ENODATA);
		return;
	}
	if (k->png) {
		enum kitty_err e = KITTY_ENODATA;

		if (!kitty_png_finish(k->png, img, &e))
			fail(k, e);
		return;
	}
#endif
	if (!raw_finish(&k->raw, img))
		fail(k, KITTY_ENODATA);
}

static void complete(struct kitty *k, struct kitty_result *r)
{
	memset(r, 0, sizeof *r);
	r->cmd = k->cmd;
	finish_data(k, &r->img);
	r->err = k->err;
	if (r->err || k->cmd.a == 'q') {
		kitty_result_free(r);
		r->img.w = r->img.h = 0;
	}
	sink_free(k);
	k->pending = false;
	k->err = KITTY_OK;
}

/* --- API --- */

struct kitty *kitty_new(const struct kitty_limits *lim)
{
	struct kitty *k = calloc(1, sizeof *k);

	if (k)
		k->lim = *lim;
	return k;
}

void kitty_set_limits(struct kitty *k, const struct kitty_limits *lim)
{
	k->lim = *lim;
}

void kitty_reset(struct kitty *k)
{
	sink_free(k);
	k->state = ST_IDLE;
	k->pending = false;
	k->have_prior = false;
	k->err = KITTY_OK;
}

void kitty_free(struct kitty *k)
{
	if (!k)
		return;
	kitty_reset(k);
	free(k);
}

bool kitty_put(struct kitty *k, const char *buf, size_t len)
{
	if (k->state == ST_NOTK)
		return false;
	if (k->state == ST_IDLE) {
		if (!len)
			return true;
		if (buf[0] != 'G') {
			k->state = ST_NOTK;
			return false;
		}
		k->state = ST_CTRL;
		k->ctrl_len = 0;
		k->ctrl_over = false;
		buf++;
		len--;
	}
	if (k->state == ST_CTRL) {
		const char *semi = memchr(buf, ';', len);
		size_t n = semi ? (size_t)(semi - buf) : len;

		if (k->ctrl_len + n > CTRL_MAX) {
			k->ctrl_over = true;
			n = CTRL_MAX - k->ctrl_len;
			/* the rest of the control part is skipped */
		}
		memcpy(k->ctrl + k->ctrl_len, buf, n);
		k->ctrl_len += n;
		if (semi) {
			begin_cmd(k);
			len -= semi + 1 - buf;
			buf = semi + 1;
		} else {
			return true;
		}
	}
	if (len)
		payload(k, buf, len);
	return true;
}

int kitty_end(struct kitty *k, bool aborted, struct kitty_result out[2])
{
	int n = 0;

	if (k->state == ST_IDLE || k->state == ST_NOTK) {
		k->state = ST_IDLE;
		return 0;
	}
	if (k->state == ST_CTRL && !aborted)
		begin_cmd(k);
	if (k->have_prior) {
		out[n++] = k->prior;
		k->have_prior = false;
	}
	if (aborted || k->state == ST_CTRL) {
		sink_free(k);
		k->pending = false;
		k->err = KITTY_OK;
		k->state = ST_IDLE;
		return n;
	}
	k->state = ST_IDLE;
	if (k->more) {
		k->pending = true;
		return n;
	}
	complete(k, &out[n++]);
	return n;
}

void kitty_result_free(struct kitty_result *r)
{
	free(r->img.rgb);
	free(r->img.alpha);
	memset(&r->img, 0, sizeof r->img);
}

size_t kitty_format_reply(const struct kitty_cmd *cmd, enum kitty_err err,
			  char *buf, size_t len)
{
	static const char *const text[] = {
		[KITTY_OK] = "OK",
		[KITTY_EINVAL] = "EINVAL:bad or unsupported key",
		[KITTY_EBADF] = "EBADF:transmission medium not supported",
		[KITTY_EBADF_SHM] = "EBADF:shared memory not usable",
		[KITTY_ENOMEM] = "ENOMEM:picture too large",
		[KITTY_ENODATA] = "ENODATA:data short or too long",
		[KITTY_ENOENT] = "ENOENT:no such image",
	};
	int n;

	if (!cmd->i || cmd->q == 2 || (cmd->q == 1 && err == KITTY_OK))
		return 0;
	if (KITTY_HAS(cmd, 'p'))
		n = snprintf(buf, len, "\033_Gi=%u,p=%u;%s\033\\", cmd->i, cmd->p,
			     text[err]);
	else
		n = snprintf(buf, len, "\033_Gi=%u;%s\033\\", cmd->i, text[err]);
	return n < 0 || (size_t)n >= len ? 0 : n;
}
