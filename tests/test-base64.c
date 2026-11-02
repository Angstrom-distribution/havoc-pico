#include <stdlib.h>
#include <string.h>
#include "check.h"
#include "../base64.h"

static const char enc_tab[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* independent encoder, padded */
static size_t encode(const uint8_t *in, size_t n, char *out, bool pad)
{
	char *o = out;
	size_t i;

	for (i = 0; i + 2 < n; i += 3) {
		uint32_t v = in[i] << 16 | in[i + 1] << 8 | in[i + 2];
		*o++ = enc_tab[v >> 18 & 63];
		*o++ = enc_tab[v >> 12 & 63];
		*o++ = enc_tab[v >> 6 & 63];
		*o++ = enc_tab[v & 63];
	}
	if (n - i == 1) {
		uint32_t v = in[i] << 16;
		*o++ = enc_tab[v >> 18 & 63];
		*o++ = enc_tab[v >> 12 & 63];
		if (pad) {
			*o++ = '=';
			*o++ = '=';
		}
	} else if (n - i == 2) {
		uint32_t v = in[i] << 16 | in[i + 1] << 8;
		*o++ = enc_tab[v >> 18 & 63];
		*o++ = enc_tab[v >> 12 & 63];
		*o++ = enc_tab[v >> 6 & 63];
		if (pad)
			*o++ = '=';
	}
	return o - out;
}

/* Decode s in pieces of step characters. Returns the byte count, -1 on an
 * error; the bytes go to out. */
static long dec(const char *s, size_t step, uint8_t *out)
{
	struct b64 b;
	size_t len = strlen(s), i, got = 0;
	int f;

	b64_init(&b);
	if (!step)
		step = len ? len : 1;
	for (i = 0; i < len; i += step) {
		size_t n = len - i < step ? len - i : step;
		long r = b64_decode(&b, s + i, n, out + got);
		if (r < 0)
			return -1;
		got += r;
	}
	f = b64_finish(&b, out + got);
	if (f < 0)
		return -1;
	return got + f;
}

static void expect(const char *s, const char *want)
{
	size_t step;

	for (step = 0; step < 6; step++) {
		uint8_t out[64];
		long n = dec(s, step, out);

		CHECK(n == (long)strlen(want));
		CHECK(n > 0 ? memcmp(out, want, n) == 0 : true);
	}
}

static void bad(const char *s)
{
	uint8_t out[64];
	size_t step;

	for (step = 0; step < 4; step++)
		CHECK(dec(s, step, out) == -1);
}

int main(void)
{
	static const char *rfc[][2] = {
		{"", ""}, {"Zg==", "f"}, {"Zm8=", "fo"}, {"Zm9v", "foo"},
		{"Zm9vYg==", "foob"}, {"Zm9vYmE=", "fooba"},
		{"Zm9vYmFy", "foobar"},
		/* unpadded ends */
		{"Zg", "f"}, {"Zm8", "fo"}, {"Zm9vYg", "foob"},
		{"Zm9vYmE", "fooba"},
		/* padding of every chunk */
		{"TQ==TWE=", "MMa"}, {"TQ==TQ==", "MM"}, {"TWE=TWE=", "MaMa"},
		{"TWFu", "Man"}, {"TWE=", "Ma"}, {"TQ==", "M"},
		{"TWFuTWFu", "ManMan"},
		/* a single '=' after one sextet pair is accepted */
		{"TQ=", "M"},
	};
	uint8_t *buf, *out;
	char *text;
	size_t i, n;
	unsigned int seed = 12345;

	for (i = 0; i < sizeof rfc / sizeof rfc[0]; i++) {
		if (!rfc[i][1][0]) {
			uint8_t o[4];
			CHECK(dec(rfc[i][0], 0, o) == 0);
		} else {
			expect(rfc[i][0], rfc[i][1]);
		}
	}

	bad("TW*u");
	bad("TW u");
	bad("TWFu\n");
	bad("TW\x80u");
	bad("T");		/* one lone sextet */
	bad("TWFuT");
	bad("=");		/* '=' with no sextets */
	bad("T=");		/* with one */
	bad("TWFu=");		/* after a full quantum */
	bad("TQ==TQ===");	/* three '=' */
	bad("TQ===");

	/* 1 MB random round trip, in odd pieces */
	n = 1000003;
	buf = malloc(n);
	out = malloc(n + 8);
	text = malloc(n / 3 * 4 + 16);
	for (i = 0; i < n; i++) {
		seed = seed * 1103515245 + 12345;
		buf[i] = seed >> 16;
	}
	for (i = 0; i < 2; i++) {
		size_t tl = encode(buf, n, text, i == 0);
		struct b64 b;
		size_t got = 0, p = 0, step = 0;
		long r;
		int f;

		text[tl] = 0;
		b64_init(&b);
		while (p < tl) {
			size_t c = 1 + (step++ * 37) % 4099;

			if (c > tl - p)
				c = tl - p;
			r = b64_decode(&b, text + p, c, out + got);
			CHECK(r >= 0);
			if (r < 0)
				break;
			got += r;
			p += c;
		}
		f = b64_finish(&b, out + got);
		CHECK(f >= 0);
		got += f;
		CHECK(got == n);
		CHECK(memcmp(out, buf, n) == 0);
	}
	free(buf);
	free(out);
	free(text);
	CHECK_DONE("test-base64");
}
