#include "base64.h"

/* 0 to 63, or -1; a table keeps the byte loop free of range tests */
static const int8_t val[128] = {
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63,
	52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1,
	-1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
	15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1,
	-1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
	41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1,
};

void b64_init(struct b64 *b)
{
	b->acc = 0;
	b->n = 0;
	b->pad = false;
}

long b64_decode(struct b64 *b, const char *in, size_t len, uint8_t *out)
{
	uint8_t *o = out;
	uint32_t acc = b->acc;
	unsigned int n = b->n;
	bool pad = b->pad;

	while (len--) {
		unsigned char c = (unsigned char)*in++;
		int v;

		if (c == '=') {
			if (n == 0 && pad) {
				pad = false;
			} else if (n == 2) {
				*o++ = acc >> 4;
				n = 0;
				acc = 0;
				pad = true;
			} else if (n == 3) {
				*o++ = acc >> 10;
				*o++ = acc >> 2;
				n = 0;
				acc = 0;
				pad = false;
			} else {
				return -1;
			}
			continue;
		}
		if (c >= 128 || (v = val[c]) < 0)
			return -1;
		pad = false;
		acc = acc << 6 | v;
		if (++n == 4) {
			*o++ = acc >> 16;
			*o++ = acc >> 8;
			*o++ = acc;
			n = 0;
			acc = 0;
		}
	}
	b->acc = acc;
	b->n = n;
	b->pad = pad;
	return o - out;
}

int b64_finish(struct b64 *b, uint8_t *out)
{
	unsigned int n = b->n;
	uint32_t acc = b->acc;

	b->n = 0;
	b->acc = 0;
	b->pad = false;
	if (n == 2) {
		out[0] = acc >> 4;
		return 1;
	}
	if (n == 3) {
		out[0] = acc >> 10;
		out[1] = acc >> 2;
		return 2;
	}
	return n ? -1 : 0;
}
