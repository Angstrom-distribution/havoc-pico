/* sixel.c when an allocation fails: each allocation of a decode is made
 * to fail in turn, and the decode must still end in a picture within the
 * limits or -1, without a bad access or a leak (ASan and LSan check that).
 * Linked with -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc so that only
 * the calls from sixel.o go through the wrappers. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../sixel.h"

void *__real_malloc(size_t n);
void *__real_calloc(size_t n, size_t m);
void *__real_realloc(void *p, size_t n);

static int calls, fail_at;

static int fail(void)
{
	return ++calls == fail_at;
}

void *__wrap_malloc(size_t n)
{
	return fail() ? NULL : __real_malloc(n);
}

void *__wrap_calloc(size_t n, size_t m)
{
	return fail() ? NULL : __real_calloc(n, m);
}

void *__wrap_realloc(void *p, size_t n)
{
	return fail() ? NULL : __real_realloc(p, n);
}

static int checks, failed;

#define CHECK(c) do { \
	checks++; \
	if (!(c)) { \
		failed++; \
		fprintf(stderr, "%s:%d: FAIL %s (case %d, allocation %d)\n", \
			__FILE__, __LINE__, #c, k, fail_at); \
	} \
} while (0)

static void run(int k, int argc, const int *argv, const struct sixel_limits *lim,
		const char *body)
{
	struct sixel_image im;
	struct sixel *s;
	size_t i, n, len = strlen(body);
	unsigned int sum = 0;

	calls = 0;
	s = sixel_new(argc, argv, lim);
	if (!s)
		return;
	for (i = 0; i < len; i += 3)
		sixel_put(s, body + i, len - i < 3 ? len - i : 3);
	if (sixel_finish(s, &im) == 0) {
		n = (size_t)im.w * im.h;
		CHECK(im.w >= 1 && im.w <= lim->max_w && n <= lim->max_pixels);
		/* touch every byte, so that ASan sees a short buffer */
		for (i = 0; i < n; i++)
			sum += im.pix[i];
		if (im.mask)
			for (i = 0; i < (n + 7) >> 3; i++)
				sum += im.mask[i];
		free(im.pix);
		free(im.mask);
	}
	sixel_free(s);
	(void)sum;
}

int main(void)
{
	static const struct {
		int argc, argv[2];
		struct sixel_limits lim;
		const char *body;
	} cases[] = {
		/* bands grown one after the other, full width */
		{ 1, { 7 }, { 8, 1000 }, "!8~-!8~-!8~-!8~" },
		/* compacted from a stride of 300, with a mask */
		{ 0, { 0 }, { 300, 100000 }, "#1~?~-#2@A" },
		/* the size hint makes the picture taller than the canvas */
		{ 2, { 0, 0 }, { 50, 100000 }, "\"1;1;20;200~" },
		{ 2, { 0, 0 }, { 20, 100000 }, "\"1;1;20;200!20~" },
		/* nothing drawn, the hint alone */
		{ 0, { 0 }, { 50, 100000 }, "\"1;1;10;10" },
		/* only zero bits: a width but no canvas */
		{ 0, { 0 }, { 50, 100000 }, "\"1;1;1;30???" },
	};
	int k, total;

	for (k = 0; k < (int)(sizeof(cases) / sizeof(*cases)); k++) {
		fail_at = 0;
		run(k, cases[k].argc, cases[k].argv, &cases[k].lim, cases[k].body);
		total = calls;
		for (fail_at = 1; fail_at <= total; fail_at++)
			run(k, cases[k].argc, cases[k].argv, &cases[k].lim, cases[k].body);
	}
	printf("%s: %d checks, %d failed\n", failed ? "FAILED" : "PASSED", checks, failed);
	return failed ? 1 : 0;
}
