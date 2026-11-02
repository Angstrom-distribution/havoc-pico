/* Shared by the kitty tests: feeds APC strings to a struct kitty the way the
 * vte does and collects the results. */
#ifndef TEST_KHARNESS_H
#define TEST_KHARNESS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kitty.h"

#define MAXRES 64

static struct kitty_result res[MAXRES];
static int nres;
static unsigned int rng_state = 1;

static unsigned int rnd(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

static void clear_res(void)
{
	int i;

	for (i = 0; i < nres; i++)
		kitty_result_free(&res[i]);
	nres = 0;
}

/* One APC string (without ESC _ and ST) in pieces of up to piece bytes (0:
 * whole, -1: random). Returns the number of results it produced. */
static int feed_apc(struct kitty *k, const char *s, size_t len, int piece,
		    bool abort_it)
{
	struct kitty_result out[2];
	size_t pos = 0;
	bool ok = true;
	int n, i;

	while (pos < len && ok) {
		size_t c = len - pos;

		if (piece > 0 && c > (size_t)piece)
			c = piece;
		else if (piece < 0) {
			size_t r = 1 + rnd() % 300;

			if (r < c)
				c = r;
		}
		ok = kitty_put(k, s + pos, c);
		pos += c;
	}
	if (!len)
		kitty_put(k, s, 0);
	n = kitty_end(k, abort_it || !ok, out);
	for (i = 0; i < n; i++) {
		if (nres < MAXRES)
			res[nres++] = out[i];
		else
			kitty_result_free(&out[i]);
	}
	return n;
}

static __attribute__((unused)) const char *errname(enum kitty_err e)
{
	static const char *const n[] = { "OK", "EINVAL", "EBADF", "EBADF_SHM",
		"ENOMEM", "ENODATA", "ENOENT" };

	return n[e];
}

#endif
