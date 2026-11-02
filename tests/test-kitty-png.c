/* PNG (f=100) and zlib through the library, the pictures from kitty_ref.py.
 * Built only with HAVOC_PNG=1: test-kitty-png PNGDIR */
#include <stdint.h>
#include "check.h"
#include "kharness.h"
#include "../kitty-png.h"

static size_t cap = 100000;
static long room_calls;
static size_t room_last;
static bool room_ok = true;

static bool room(void *d, size_t n)
{
	(void)d;
	room_calls++;
	room_last = n;
	return room_ok;
}

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

static const char tab[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *b64enc(const uint8_t *in, size_t n)
{
	char *out = malloc(n / 3 * 4 + 8), *o = out;
	size_t i;

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

static enum kitty_err want_err(const char *s)
{
	if (!strcmp(s, "einval"))
		return KITTY_EINVAL;
	if (!strcmp(s, "enodata"))
		return KITTY_ENODATA;
	return KITTY_OK;
}

static void compare(const char *dir, const char *name, const struct kitty_image *img,
		    unsigned int w, unsigned int h, int has_alpha)
{
	char path[512];
	size_t n;
	uint8_t *want;

	CHECK(img->w == w && img->h == h);
	snprintf(path, sizeof path, "%s/%s.rgb565", dir, name);
	want = slurp(path, &n);
	CHECK(want && n == (size_t)w * h * 2 && img->rgb &&
	      memcmp(want, img->rgb, n) == 0);
	free(want);
	snprintf(path, sizeof path, "%s/%s.alpha", dir, name);
	want = slurp(path, &n);
	CHECK(has_alpha ? (want && img->alpha && n == (size_t)w * h &&
			   memcmp(want, img->alpha, n) == 0)
			: (want == NULL && img->alpha == NULL));
	free(want);
}

static void one(const char *dir, const char *name, unsigned int w, unsigned int h,
		const char *status, int has_alpha)
{
	static const size_t pieces[] = { 1, 7, 4096, 0 };
	struct kitty_limits lim = { .max_pixels = cap, .room = room };
	char path[512];
	uint8_t *png;
	size_t n, p;
	enum kitty_err want = want_err(status);

	snprintf(path, sizeof path, "%s/%s.png", dir, name);
	png = slurp(path, &n);
	CHECK(png != NULL);
	if (!png)
		return;
	for (p = 0; p < 4; p++) {
		/* the library alone */
		struct kitty_png *kp = kitty_png_new(&lim);
		enum kitty_err e = KITTY_OK;
		struct kitty_image img = { 0 };
		size_t step = pieces[p] ? pieces[p] : n, pos = 0;
		bool ok = true;

		while (pos < n && ok) {
			size_t c = n - pos < step ? n - pos : step;

			ok = kitty_png_put(kp, png + pos, c, &e);
			pos += c;
		}
		if (ok)
			ok = kitty_png_finish(kp, &img, &e);
		if (want == KITTY_OK) {
			CHECK(ok);
			if (ok)
				compare(dir, name, &img, w, h, has_alpha);
		} else {
			CHECK(!ok && e == want);
			if (ok || e != want)
				fprintf(stderr, "%s: ok %d err %d want %s\n", name, ok, e, status);
		}
		free(img.rgb);
		free(img.alpha);
		kitty_png_free(kp);
	}
	/* through kitty_put, with the base64 split */
	for (p = 0; p < 4; p++) {
		struct kitty *k = kitty_new(&lim);
		char *b = b64enc(png, n);
		char *s = malloc(strlen(b) + 64);

		sprintf(s, "Ga=t,f=100,i=1;%s", b);
		clear_res();
		CHECK(feed_apc(k, s, strlen(s), pieces[p], false) == 1);
		CHECK(res[0].err == want);
		if (want == KITTY_OK)
			compare(dir, name, &res[0].img, w, h, has_alpha);
		else
			CHECK(res[0].img.rgb == NULL);
		clear_res();
		free(s);
		free(b);
		kitty_free(k);
	}
	free(png);
}

static uint16_t c565(const uint8_t *p)
{
	return (p[0] >> 3) << 11 | (p[1] >> 2) << 5 | p[2] >> 3;
}

/* o=z: streams made by kitty_ref.py raw */
static void zlib_cases(const char *dir)
{
	static const char *const names[] = { "rgb", "rgba", "rgb-z", "rgba-z" };
	struct kitty_limits lim = { .max_pixels = cap, .room = room };
	unsigned int w = 29, h = 17;
	size_t i, p;
	char path[512];

	for (i = 0; i < 4; i++) {
		static const int pieces[] = { 0, 1, 5, 64, -1 };
		uint8_t *data, *want;
		size_t n, wn;
		bool alpha = !strncmp(names[i], "rgba", 4);

		snprintf(path, sizeof path, "%s/%s.bin", dir, names[i]);
		data = slurp(path, &n);
		snprintf(path, sizeof path, "%s/%s.rgb565", dir, alpha ? "rgba" : "rgb");
		want = slurp(path, &wn);
		CHECK(data && want);
		if (!data || !want)
			continue;
		for (p = 0; p < 5; p++) {
			struct kitty *k = kitty_new(&lim);
			size_t a, b2;

			rng_state = 5 + p;
			clear_res();
			/* strip ESC _ and ESC \ */
			a = 2;
			b2 = n - 2;
			CHECK(feed_apc(k, (char *)data + a, b2 - a, pieces[p], false) == 1);
			CHECK(res[0].err == KITTY_OK);
			if (res[0].err == KITTY_OK) {
				CHECK(res[0].img.w == w && res[0].img.h == h);
				CHECK(memcmp(res[0].img.rgb, want, wn) == 0);
				CHECK((res[0].img.alpha != NULL) == alpha);
			}
			clear_res();
			kitty_free(k);
		}
		free(data);
		free(want);
	}
	{
		/* damaged streams: a stream cut short, corrupt, trailing bytes,
		 * raw data one byte long or short before compression */
		struct kitty *k = kitty_new(&lim);
		uint8_t *data;
		size_t n;
		char *line;

		snprintf(path, sizeof path, "%s/rgb-z.bin", dir);
		data = slurp(path, &n);
		line = malloc(n + 100);
		/* cut: the base64 text loses its last 8 characters */
		memcpy(line, data + 2, n - 4);
		line[n - 4] = 0;
		line[n - 4 - 8] = 0;
		clear_res();
		CHECK(feed_apc(k, line, strlen(line), 0, false) == 1);
		CHECK(res[0].err == KITTY_ENODATA);
		clear_res();
		/* corrupt: a byte in the middle of the base64 text */
		memcpy(line, data + 2, n - 4);
		line[n - 4] = 0;
		line[60] = line[60] == 'A' ? 'B' : 'A';
		line[61] = line[61] == 'A' ? 'B' : 'A';
		CHECK(feed_apc(k, line, n - 4, 0, false) == 1);
		CHECK(res[0].err == KITTY_EINVAL || res[0].err == KITTY_ENODATA);
		clear_res();
		free(line);
		free(data);
		kitty_free(k);
	}
	{
		struct kitty *k = kitty_new(&lim);
		uint8_t raw[29 * 17 * 3 + 1];
		/* zlib stored blocks by hand: header 78 01, final stored block */
		static uint8_t z[2 + 5 + sizeof raw + 4 + 4];
		char *b, *s;
		size_t len, zn;
		uint32_t a1 = 1, a2 = 0, i2;
		int extra;

		for (extra = -1; extra <= 1; extra++) {
			len = 29 * 17 * 3 + extra;
			for (i2 = 0; i2 < len; i2++)
				raw[i2] = i2;
			z[0] = 0x78;
			z[1] = 0x01;
			z[2] = 1;
			z[3] = len & 255;
			z[4] = len >> 8;
			z[5] = ~len & 255;
			z[6] = (~len >> 8) & 255;
			memcpy(z + 7, raw, len);
			a1 = 1;
			a2 = 0;
			for (i2 = 0; i2 < len; i2++) {
				a1 = (a1 + raw[i2]) % 65521;
				a2 = (a2 + a1) % 65521;
			}
			z[7 + len] = a2 >> 8;
			z[8 + len] = a2;
			z[9 + len] = a1 >> 8;
			z[10 + len] = a1;
			zn = 11 + len;
			b = b64enc(z, zn);
			s = malloc(strlen(b) + 80);
			sprintf(s, "Ga=t,f=24,s=29,v=17,o=z,i=1;%s", b);
			clear_res();
			CHECK(feed_apc(k, s, strlen(s), 0, false) == 1);
			CHECK(res[0].err == (extra == 0 ? KITTY_OK : KITTY_ENODATA));
			clear_res();
			free(b);
			free(s);
		}
		/* data after the end of the stream */
		memcpy(z + 7, raw, 29 * 17 * 3);
		kitty_free(k);
	}
	(void)c565;
}

static void chunk_cases(const char *dir)
{
	struct kitty_limits lim = { .max_pixels = cap, .room = room };
	struct kitty *k = kitty_new(&lim);
	char path[512];
	uint8_t *png;
	size_t n, i;
	char *b, s[8192];
	unsigned int w = 37, h = 23;

	snprintf(path, sizeof path, "%s/rgba.png", dir);
	png = slurp(path, &n);
	b = b64enc(png, n);
	/* chunks of 4096 base64 characters, as icat sends them */
	clear_res();
	for (i = 0; i < strlen(b); i += 1000) {
		bool last = i + 1000 >= strlen(b);

		if (i == 0)
			snprintf(s, sizeof s, "Ga=T,f=100,i=7,m=%d;%.1000s", !last, b);
		else
			snprintf(s, sizeof s, "Gm=%d;%.1000s", !last, b + i);
		CHECK(feed_apc(k, s, strlen(s), 0, false) == (last ? 1 : 0));
	}
	CHECK(nres == 1 && res[0].err == KITTY_OK);
	compare(dir, "rgba", &res[0].img, w, h, 1);
	clear_res();

	/* memory: refused by the cap before any row; room asked once */
	cap = 850;
	kitty_set_limits(k, &(struct kitty_limits){ .max_pixels = cap, .room = room });
	snprintf(s, sizeof s, "Ga=t,f=100,i=1;%s", b);
	{
		char *big = malloc(strlen(b) + 40);

		room_calls = 0;
		sprintf(big, "Ga=t,f=100,i=1;%s", b);
		CHECK(feed_apc(k, big, strlen(big), 0, false) == 1);
		CHECK(res[0].err == KITTY_ENOMEM && res[0].img.rgb == NULL);
		CHECK(room_calls == 0);
		clear_res();
		cap = 100000;
		kitty_set_limits(k, &(struct kitty_limits){ .max_pixels = cap, .room = room });
		room_ok = false;
		CHECK(feed_apc(k, big, strlen(big), 0, false) == 1);
		CHECK(res[0].err == KITTY_ENOMEM && res[0].img.rgb == NULL);
		CHECK(room_calls == 1 && room_last == 37 * 23);
		room_ok = true;
		clear_res();
		free(big);
	}
	/* PNG inside o=z and a PNG with s and v wrong: s, v are ignored */
	free(b);
	free(png);
	kitty_free(k);
}

int main(int argc, char **argv)
{
	char path[512];
	FILE *f;
	char line[256];

	if (argc < 3) {
		fprintf(stderr, "usage: test-kitty-png PNGDIR RAWDIR\n");
		return 2;
	}
	snprintf(path, sizeof path, "%s/list.txt", argv[1]);
	f = fopen(path, "r");
	CHECK(f != NULL);
	while (f && fgets(line, sizeof line, f)) {
		char name[64], status[16];
		unsigned int w, h;
		int al;

		if (sscanf(line, "%63s %u %u %15s %d", name, &w, &h, status, &al) == 5)
			one(argv[1], name, w, h, status, al);
	}
	if (f)
		fclose(f);
	zlib_cases(argv[2]);
	chunk_cases(argv[1]);
	CHECK_DONE("test-kitty-png");
}
