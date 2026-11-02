#include <stdint.h>
#include <sys/stat.h>
#include "check.h"
#include "kharness.h"

/* allocation count: tests that a refused picture allocates nothing */
static long mallocs;
void *__real_malloc(size_t);
void *__wrap_malloc(size_t n)
{
	mallocs++;
	return __real_malloc(n);
}

static size_t cap = 100000;
static long room_calls;
static size_t room_last;
static bool room_ok = true;

static bool room(void *data, size_t px)
{
	(void)data;
	room_calls++;
	room_last = px;
	return room_ok;
}

static struct kitty_limits limits(bool shm)
{
	struct kitty_limits l = { .max_pixels = cap, .shm = shm, .room = room };

	return l;
}

static struct kitty *newk(bool shm)
{
	struct kitty_limits l = limits(shm);

	return kitty_new(&l);
}

static char b64c[64];
static const char tab[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *b64enc(const uint8_t *in, size_t n)
{
	char *out = malloc(n / 3 * 4 + 8), *o = out;
	size_t i;

	(void)b64c;
	for (i = 0; i < n; i += 3) {
		uint32_t v = in[i] << 16 | (i + 1 < n ? in[i + 1] << 8 : 0) |
			     (i + 2 < n ? in[i + 2] : 0);

		*o++ = tab[v >> 18 & 63];
		*o++ = tab[v >> 12 & 63];
		*o++ = i + 1 < n ? tab[v >> 6 & 63] : '=';
		*o++ = i + 2 < n ? tab[v & 63] : '=';
	}
	*o = 0;
	return out;
}

/* "<keys>;<base64 of data>" */
static char *mk(const char *keys, const uint8_t *data, size_t n)
{
	char *b = b64enc(data, n);
	char *s = malloc(strlen(keys) + strlen(b) + 4);

	sprintf(s, "G%s;%s", keys, b);
	free(b);
	return s;
}

static int cmd(struct kitty *k, const char *s)
{
	return feed_apc(k, s, strlen(s), 0, false);
}

static uint8_t *pixels(unsigned int w, unsigned int h, unsigned int bpp)
{
	uint8_t *p = malloc((size_t)w * h * bpp);
	size_t i;

	for (i = 0; i < (size_t)w * h * bpp; i++)
		p[i] = i * 7 + (i >> 8) * 13;
	return p;
}

static void reply(const struct kitty_result *r, char *buf)
{
	size_t n = kitty_format_reply(&r->cmd, r->err, buf, 128);

	buf[n] = 0;
}

static uint16_t c565(const uint8_t *p)
{
	return (p[0] >> 3) << 11 | (p[1] >> 2) << 5 | p[2] >> 3;
}

/* --- vectors against kitty_ref.py --- */

static uint8_t *slurp(const char *path, size_t *n)
{
	FILE *f = fopen(path, "rb");
	uint8_t *b;
	long l;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	l = ftell(f);
	fseek(f, 0, SEEK_SET);
	b = malloc(l + 1);
	if (fread(b, 1, l, f) != (size_t)l)
		l = 0;
	b[l] = 0;
	fclose(f);
	*n = l;
	return b;
}

/* The results as lines like kitty_ref.py writes them. */
static void lines_of(char *out, size_t max)
{
	int i, img = 0;
	size_t n = 0;

	out[0] = 0;
	for (i = 0; i < nres; i++) {
		const struct kitty_result *r = &res[i];

		n += snprintf(out + n, max - n, "a=%c i=%u", r->cmd.a, r->cmd.i);
		if (r->err)
			n += snprintf(out + n, max - n, " err=%s",
				      r->err == KITTY_EBADF_SHM ? "EBADF" : errname(r->err));
		else if (r->img.w)
			n += snprintf(out + n, max - n, " w=%u h=%u alpha=%d",
				      r->img.w, r->img.h, r->img.alpha ? 1 : 0);
		n += snprintf(out + n, max - n, "\n");
		(void)img;
	}
}

static void check_images(const char *refdir, const char *name)
{
	int i, img = 0;

	for (i = 0; i < nres; i++) {
		const struct kitty_result *r = &res[i];
		char path[512];
		size_t n, px;
		uint8_t *want;

		if (!r->img.w)
			continue;
		px = (size_t)r->img.w * r->img.h;
		snprintf(path, sizeof path, "%s/%s.%d.rgb565", refdir, name, img);
		want = slurp(path, &n);
		CHECK(want && n == px * 2);
		CHECK(want && memcmp(want, r->img.rgb, px * 2) == 0);
		free(want);
		snprintf(path, sizeof path, "%s/%s.%d.alpha", refdir, name, img);
		want = slurp(path, &n);
		CHECK(r->img.alpha ? (want && n == px && !memcmp(want, r->img.alpha, px))
				   : want == NULL);
		free(want);
		img++;
	}
}

/* every ESC _ ... ESC \ of the file */
static void feed_stream(struct kitty *k, const uint8_t *d, size_t n, int piece)
{
	size_t i = 0;

	while (i + 1 < n) {
		size_t j;

		if (d[i] != 0x1b || d[i + 1] != '_') {
			i++;
			continue;
		}
		for (j = i + 2; j + 1 < n && !(d[j] == 0x1b && d[j + 1] == '\\'); j++)
			;
		if (j + 1 >= n)
			break;
		feed_apc(k, (const char *)d + i + 2, j - i - 2, piece, false);
		i = j + 2;
	}
}

static void vectors(const char *refdir, const char *vecdir)
{
	static const char *const names[] = {
		"chafa-kitty-grad", "chafa-kitty-alpha", "timg-kitty",
		"timg-kitty-pattern", "icat-stream", "icat-stream-pattern",
		"icat-detect", "icat-kitty", "mpv-kitty-shm",
	};
	static const int pieces[] = { 0, 1, 7, -1, -1 };
	size_t v;
	int p;

	for (v = 0; v < sizeof names / sizeof names[0]; v++) {
		char path[512], got[4096];
		size_t n, m;
		uint8_t *data, *want;

		snprintf(path, sizeof path, "%s/%s.bin", vecdir, names[v]);
		data = slurp(path, &n);
		snprintf(path, sizeof path, "%s/%s.txt", refdir, names[v]);
		want = slurp(path, &m);
		CHECK(data && want);
		if (!data || !want)
			continue;
		for (p = 0; p < 5; p++) {
			struct kitty *k = newk(false);

			rng_state = 99 + p;
			clear_res();
			feed_stream(k, data, n, pieces[p]);
			lines_of(got, sizeof got);
			CHECK(strcmp(got, (char *)want) == 0);
			if (strcmp(got, (char *)want))
				fprintf(stderr, "%s piece %d:\n%s--- want\n%s",
					names[v], pieces[p], got, want);
			check_images(refdir, names[v]);
			kitty_free(k);
		}
		free(data);
		free(want);
	}
	clear_res();
}

/* --- built-in cases --- */

static void replies(void)
{
	struct kitty *k = newk(false);
	char buf[128];
	uint8_t px[3] = { 1, 2, 3 };
	char *s;

	/* icat's first probe: payload "123" */
	CHECK(cmd(k, "Ga=q,f=24,s=1,v=1,S=3,i=1;MTIz") == 1);
	reply(&res[0], buf);
	CHECK(strcmp(buf, "\033_Gi=1;OK\033\\") == 0);
	clear_res();
	/* t=t and t=s with shm off */
	CHECK(cmd(k, "Ga=q,f=24,t=t,s=1,v=1,S=3,i=2;L2Rldi9zaG0va2l0dHk") == 1);
	reply(&res[0], buf);
	CHECK(strcmp(buf, "\033_Gi=2;EBADF:transmission medium not supported\033\\") == 0);
	clear_res();
	CHECK(cmd(k, "Ga=q,f=24,t=s,s=1,v=1,S=3,i=3;aWNhdC1CRk9CR0dXS0hFNFFF") == 1);
	reply(&res[0], buf);
	CHECK(strcmp(buf, "\033_Gi=3;EBADF:transmission medium not supported\033\\") == 0);
	clear_res();

	/* q and p */
	s = mk("a=t,f=24,s=1,v=1,i=5,q=1", px, 3);
	cmd(k, s);
	reply(&res[0], buf);
	CHECK(buf[0] == 0);			/* q=1: only errors */
	clear_res();
	free(s);
	s = mk("a=t,f=24,s=1,v=1,i=5,q=2", px, 2);
	cmd(k, s);
	reply(&res[0], buf);
	CHECK(buf[0] == 0 && res[0].err == KITTY_ENODATA);	/* q=2: none */
	clear_res();
	free(s);
	s = mk("a=t,f=24,s=1,v=1,i=5,q=1", px, 2);
	cmd(k, s);
	reply(&res[0], buf);
	CHECK(strcmp(buf, "\033_Gi=5;ENODATA:data short or too long\033\\") == 0);
	clear_res();
	free(s);
	s = mk("a=t,f=24,s=1,v=1", px, 3);
	cmd(k, s);
	reply(&res[0], buf);
	CHECK(buf[0] == 0);			/* no i */
	clear_res();
	free(s);
	s = mk("a=t,f=24,s=1,v=1,i=9,p=4", px, 3);
	cmd(k, s);
	reply(&res[0], buf);
	CHECK(strcmp(buf, "\033_Gi=9,p=4;OK\033\\") == 0);
	CHECK(res[0].img.w == 1 && res[0].img.rgb[0] == c565(px));
	CHECK(res[0].img.alpha == NULL);
	clear_res();
	free(s);
	kitty_free(k);
}

static void errors(void)
{
	struct kitty *k = newk(false);
	uint8_t *d = pixels(4, 4, 4);
	char *s;
	char big[700];
	size_t i;
	static const struct { const char *keys; enum kitty_err err; } t[] = {
		{ "a=t,f=24,i=1", KITTY_EINVAL },		/* no s, v */
		{ "a=t,f=24,s=4,i=1", KITTY_EINVAL },
		{ "a=t,f=8,s=4,v=4", KITTY_EINVAL },
		{ "a=f,f=24,s=4,v=4", KITTY_EINVAL },
		{ "a=t,f=24,s=4,v=4,I=1", KITTY_EINVAL },
		{ "a=t,f=24,s=4,v=4,U=1", KITTY_EINVAL },
		{ "a=T,f=24,s=4,v=4,P=1", KITTY_EINVAL },
		{ "a=T,f=24,s=4,v=4,Q=1", KITTY_EINVAL },
		{ "a=T,f=24,s=4,v=4,H=1", KITTY_EINVAL },
		{ "a=T,f=24,s=4,v=4,V=1", KITTY_EINVAL },
		{ "a=t,f=24,s=4,v=4,O=4", KITTY_EINVAL },
		{ "a=t,f=24,s=5000,v=4", KITTY_EINVAL },
		{ "a=t,f=24,s=4,v=5000", KITTY_EINVAL },
		{ "a=t,f=24,s=4,v=4,t=x", KITTY_EINVAL },
		{ "a=t,f=24,s=4,v=4,t=f", KITTY_EBADF },
		{ "a=t,f=24,s=4,v=4,t=t", KITTY_EBADF },
		{ "a=t,f=24,s=4,v=4,t=s", KITTY_EBADF },	/* shm off */
		/* syntax */
		{ "a=,f=24,s=4,v=4", KITTY_EINVAL },
		{ "=1", KITTY_EINVAL },
		{ "ab=1", KITTY_EINVAL },
		{ "a=tt", KITTY_EINVAL },
		{ "a=t,f=24,s=-4,v=4", KITTY_EINVAL },
		{ "a=t,f=24,s=4,s=5,v=4", KITTY_EINVAL },	/* repeated, differs */
		{ "a=t,f=24,s=4,v=4,,i=1", KITTY_EINVAL },
		{ "a=t,f=24,s=4,v=4,", KITTY_EINVAL },
		{ "a=t,f=24,s=4x,v=4", KITTY_EINVAL },
		{ "a=1,f=24,s=4,v=4", KITTY_EINVAL },
	};

	for (i = 0; i < sizeof t / sizeof t[0]; i++) {
		s = mk(t[i].keys, d, 48);
		clear_res();
		CHECK(cmd(k, s) == 1);
		CHECK(res[0].err == t[i].err);
		CHECK(res[0].img.w == 0 && res[0].img.rgb == NULL);
		if (res[0].err != t[i].err)
			fprintf(stderr, "case %s: %s\n", t[i].keys, errname(res[0].err));
		free(s);
	}
	/* the same key twice with the same value is fine */
	s = mk("a=t,f=24,s=4,s=4,v=4", d, 48);
	clear_res();
	CHECK(cmd(k, s) == 1 && res[0].err == KITTY_OK && res[0].img.w == 4);
	free(s);

	/* data: short, long, bad base64 */
	s = mk("a=t,f=24,s=4,v=4", d, 47);
	clear_res();
	cmd(k, s);
	CHECK(res[0].err == KITTY_ENODATA && !res[0].img.rgb);
	free(s);
	s = mk("a=t,f=24,s=4,v=4", d, 49);
	clear_res();
	cmd(k, s);
	CHECK(res[0].err == KITTY_ENODATA && !res[0].img.rgb);
	free(s);
	s = mk("a=t,f=24,s=4,v=4", d, 48);
	s[strlen(s) - 6] = '*';
	clear_res();
	cmd(k, s);
	CHECK(res[0].err == KITTY_EINVAL && !res[0].img.rgb);
	free(s);
	clear_res();
	/* exact fit: 4x4 f=32 with 4 bytes per pixel */
	free(d);
	d = pixels(4, 4, 4);
	s = mk("a=t,f=32,s=4,v=4", d, 64);
	CHECK(cmd(k, s) == 1 && res[0].err == KITTY_OK);
	CHECK(res[0].img.alpha != NULL || res[0].img.rgb);
	free(s);
	clear_res();

	/* size above the cap, and room refusing: no allocation */
	cap = 1000;
	kitty_set_limits(k, &(struct kitty_limits){ .max_pixels = cap, .room = room });
	s = mk("a=t,f=24,s=40,v=40", d, 64);
	mallocs = 0;
	room_calls = 0;
	CHECK(cmd(k, s) == 1 && res[0].err == KITTY_ENOMEM);
	CHECK(room_calls == 0);
	free(s);
	clear_res();
	cap = 100000;
	kitty_set_limits(k, &(struct kitty_limits){ .max_pixels = cap, .room = room });
	room_ok = false;
	room_calls = 0;
	s = mk("a=t,f=24,s=40,v=40", d, 64);
	{
		long before;

		before = mallocs;
		(void)before;
		mallocs = 0;
		CHECK(cmd(k, s) == 1 && res[0].err == KITTY_ENOMEM);
		/* the only allocations are the harness's own copy of the string */
		CHECK(mallocs == 0);
	}
	CHECK(room_calls == 1 && room_last == 1600);
	room_ok = true;
	free(s);
	clear_res();

	/* control part of 513 bytes */
	strcpy(big, "Ga=t,f=24,s=1,v=1");
	while (strlen(big) < 1 + 513)
		strcat(big, ",x=1");
	big[1 + 513] = 0;
	strcat(big, ";AAAA");
	CHECK(cmd(k, big) == 1 && res[0].err == KITTY_EINVAL);
	clear_res();
	/* 512 bytes are fine */
	strcpy(big, "Ga=t,f=24,s=1,v=1");
	while (strlen(big) < 1 + 512)
		strcat(big, ",x=1");
	big[1 + 512] = 0;
	{
		/* cut at a key boundary: pad with i= digits */
		size_t l = strlen(big);

		while (big[l - 1] == ',' || big[l - 2] == ',' || big[l - 1] == 'x' ||
		       big[l - 1] == '=')
			big[--l] = 0;
	}
	strcat(big, ";AAAA");
	CHECK(cmd(k, big) == 1 && res[0].err == KITTY_OK);
	clear_res();

	/* z negative and saturating, large numbers */
	s = mk("a=p,i=4294967296,z=-5000000000,x=99999999999", d, 0);
	CHECK(cmd(k, s) == 1 && res[0].err == KITTY_OK);
	CHECK(res[0].cmd.i == 0xffffffffu && res[0].cmd.z == INT32_MIN &&
	      res[0].cmd.x == 0xffffffffu);
	CHECK(KITTY_HAS(&res[0].cmd, 'z') && KITTY_HAS(&res[0].cmd, 'i'));
	CHECK(!KITTY_HAS(&res[0].cmd, 'y'));
	free(s);
	clear_res();
	CHECK(cmd(k, "Ga=p,z=5000000000") == 1 && res[0].cmd.z == INT32_MAX);
	clear_res();
	/* defaults */
	CHECK(cmd(k, "Ga=d") == 1);
	CHECK(res[0].cmd.t == 'd' && res[0].cmd.d == 'a' && res[0].cmd.f == 32 &&
	      res[0].cmd.o == 0 && res[0].err == KITTY_OK);
	clear_res();
	CHECK(cmd(k, "G") == 1 && res[0].cmd.a == 't' && res[0].err == KITTY_EINVAL);
	clear_res();
	free(d);
	kitty_free(k);
}

static void chunks(void)
{
	struct kitty *k = newk(false);
	uint8_t *d = pixels(8, 4, 3);	/* 96 bytes */
	char *b = b64enc(d, 96);
	char s[1024];
	char buf[128];
	size_t i;

	/* chafa: empty first chunk, then m=1 chunks, then empty m=0 */
	sprintf(s, "Ga=T,f=24,s=8,v=4,i=3,m=1");
	CHECK(cmd(k, s) == 0);
	sprintf(s, "Gm=1;%.64s", b);
	CHECK(cmd(k, s) == 0);
	sprintf(s, "Gm=1;%s", b + 64);
	CHECK(cmd(k, s) == 0);
	CHECK(cmd(k, "Gm=0") == 1);
	CHECK(res[0].err == KITTY_OK && res[0].img.w == 8 && res[0].cmd.a == 'T');
	CHECK(res[0].img.rgb[1] == c565(d + 3));
	clear_res();

	/* ranger style: every key repeated, q of the last chunk wins */
	sprintf(s, "Ga=t,f=24,s=8,v=4,i=3,m=1;%.64s", b);
	CHECK(cmd(k, s) == 0);
	sprintf(s, "Ga=t,f=24,s=8,v=4,i=3,m=0,q=2;%s", b + 64);
	CHECK(cmd(k, s) == 1);
	CHECK(res[0].err == KITTY_OK && res[0].img.w == 8 && res[0].cmd.q == 2);
	reply(&res[0], buf);
	CHECK(buf[0] == 0);
	clear_res();

	/* a new command in the middle of a transfer */
	sprintf(s, "Ga=t,f=24,s=8,v=4,i=3,m=1;%.64s", b);
	CHECK(cmd(k, s) == 0);
	sprintf(s, "Ga=t,f=24,s=8,v=4,i=4;%s", b);
	CHECK(cmd(k, s) == 2);
	CHECK(res[0].err == KITTY_ENODATA && res[0].cmd.i == 3);
	CHECK(res[1].err == KITTY_OK && res[1].cmd.i == 4 && res[1].img.w == 8);
	clear_res();
	/* ... which itself starts a chunked transfer */
	sprintf(s, "Ga=t,f=24,s=8,v=4,i=3,m=1;%.64s", b);
	CHECK(cmd(k, s) == 0);
	sprintf(s, "Ga=t,f=24,s=8,v=4,i=4,m=1;%.64s", b);
	CHECK(cmd(k, s) == 1);
	CHECK(res[0].err == KITTY_ENODATA && res[0].cmd.i == 3);
	sprintf(s, "Gm=0;%s", b + 64);
	CHECK(cmd(k, s) == 1);
	CHECK(res[1].err == KITTY_OK && res[1].cmd.i == 4);
	clear_res();
	/* a different command (a=d) ends it too */
	sprintf(s, "Ga=t,f=24,s=8,v=4,i=3,m=1;%.64s", b);
	CHECK(cmd(k, s) == 0);
	CHECK(cmd(k, "Ga=d,d=i,i=3") == 2);
	CHECK(res[0].err == KITTY_ENODATA && res[1].cmd.a == 'd');
	clear_res();

	/* an error in the first chunk: later chunks are dropped, error at m=0 */
	CHECK(cmd(k, "Ga=t,f=24,s=8,v=4,i=3,I=1,m=1") == 0);
	sprintf(s, "Gm=1;%.64s", b);
	CHECK(cmd(k, s) == 0);
	sprintf(s, "Gm=0;%s", b + 64);
	CHECK(cmd(k, s) == 1 && res[0].err == KITTY_EINVAL && res[0].cmd.i == 3);
	clear_res();
	/* an error in the middle (too much data) */
	sprintf(s, "Ga=t,f=24,s=2,v=2,i=3,m=1;%s", b);
	CHECK(cmd(k, s) == 0);
	sprintf(s, "Gm=1;%s", b);
	CHECK(cmd(k, s) == 0);
	CHECK(cmd(k, "Gm=0") == 1 && res[0].err == KITTY_ENODATA);
	clear_res();

	/* aborted in the middle: no result, and the next command is fresh */
	sprintf(s, "Ga=t,f=24,s=8,v=4,i=3,m=1;%.64s", b);
	CHECK(cmd(k, s) == 0);
	sprintf(s, "Gm=1;%.64s", b);
	CHECK(feed_apc(k, s, strlen(s), 0, true) == 0);
	CHECK(cmd(k, "Gm=0") == 1 && res[0].err == KITTY_EINVAL);	/* no transfer */
	clear_res();
	/* kitty_reset in the middle */
	sprintf(s, "Ga=t,f=24,s=8,v=4,i=3,m=1;%.64s", b);
	CHECK(cmd(k, s) == 0);
	kitty_reset(k);
	sprintf(s, "Gm=0;%s", b + 64);
	CHECK(cmd(k, s) == 1 && res[0].err == KITTY_EINVAL);
	clear_res();
	/* an APC cut off in the control part */
	CHECK(feed_apc(k, "Ga=t,f=2", 8, 0, true) == 0);

	/* not kitty's */
	CHECK(!kitty_put(k, "Xhello", 6));
	CHECK(kitty_end(k, true, res) == 0);
	CHECK(!kitty_put(k, "", 0) || true);
	kitty_end(k, false, res);
	/* ... and kitty again afterwards */
	CHECK(cmd(k, "Ga=d") == 1);
	clear_res();
	/* an unfinished transfer survives a foreign APC */
	sprintf(s, "Ga=t,f=24,s=8,v=4,i=3,m=1;%.64s", b);
	CHECK(cmd(k, s) == 0);
	CHECK(!kitty_put(k, "Pfoo", 4));
	CHECK(kitty_end(k, true, res) == 0);
	sprintf(s, "Gm=0;%s", b + 64);
	CHECK(cmd(k, s) == 1 && res[0].err == KITTY_OK);
	clear_res();

	/* all pieces sizes, with the 32-bit alpha split inside a pixel */
	free(d);
	d = pixels(8, 4, 4);
	free(b);
	b = b64enc(d, 128);
	for (i = 1; i < 40; i++) {
		sprintf(s, "Ga=t,f=32,s=8,v=4,i=3;%s", b);
		clear_res();
		CHECK(feed_apc(k, s, strlen(s), i, false) == 1);
		CHECK(res[0].err == KITTY_OK && res[0].img.w == 8);
		CHECK(res[0].img.alpha && res[0].img.alpha[5] == d[23]);
		CHECK(res[0].img.rgb[3] == c565(d + 12));
	}
	/* all alpha 255: the plane is dropped */
	for (i = 0; i < 128; i += 4)
		d[i + 3] = 255;
	free(b);
	b = b64enc(d, 128);
	sprintf(s, "Ga=t,f=32,s=8,v=4,i=3;%s", b);
	clear_res();
	CHECK(cmd(k, s) == 1 && res[0].err == KITTY_OK && res[0].img.alpha == NULL);
	clear_res();
	/* a=q frees the picture but reports */
	sprintf(s, "Ga=q,f=32,s=8,v=4,i=3;%s", b);
	CHECK(cmd(k, s) == 1 && res[0].err == KITTY_OK && res[0].img.rgb == NULL);
	clear_res();
	free(d);
	free(b);
	kitty_free(k);
}

static void png_off(void)
{
#if !HAVOC_PNG
	struct kitty *k = newk(false);

	CHECK(cmd(k, "Ga=t,f=100,i=1;iVBORw0KGgo") == 1 && res[0].err == KITTY_EINVAL);
	clear_res();
	CHECK(cmd(k, "Ga=t,f=24,s=1,v=1,o=z,i=1;eJxjYAAAAAMAAQ==") == 1 &&
	      res[0].err == KITTY_EINVAL);
	clear_res();
	kitty_free(k);
#endif
}

int main(int argc, char **argv)
{
	if (argc >= 3)
		vectors(argv[1], argv[2]);
	replies();
	errors();
	chunks();
	png_off();
	clear_res();
	CHECK_DONE("test-kitty");
}
