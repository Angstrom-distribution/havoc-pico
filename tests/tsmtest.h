/* Shared helpers for the libtsm tests. */
#ifndef TSMTEST_H
#define TSMTEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "tsm/libtsm.h"
#include "tsm/libtsm-int.h"

static int t_checks, t_failed;

#define CHECK(c) do { \
	t_checks++; \
	if (!(c)) { \
		t_failed++; \
		fprintf(stderr, "%s:%d: FAIL %s\n", __FILE__, __LINE__, #c); \
	} \
} while (0)

#define CHECK_INT(a, b) do { \
	long long t_a = (a), t_b = (b); \
	t_checks++; \
	if (t_a != t_b) { \
		t_failed++; \
		fprintf(stderr, "%s:%d: FAIL %s == %s (%lld != %lld)\n", \
			__FILE__, __LINE__, #a, #b, t_a, t_b); \
	} \
} while (0)

#define CHECK_STR(a, b) do { \
	const char *t_a = (a), *t_b = (b); \
	t_checks++; \
	if (strcmp(t_a, t_b)) { \
		t_failed++; \
		fprintf(stderr, "%s:%d: FAIL %s: \"%s\" != \"%s\"\n", \
			__FILE__, __LINE__, #a, t_a, t_b); \
	} \
} while (0)

#define T_MAXCOLS 256
#define T_MAXROWS 128

struct tterm {
	struct tsm_screen *scr;
	struct tsm_vte *vte;
	char reply[8192];
	size_t rlen;
};

static __attribute__((unused)) void t_write(struct tsm_vte *vte, const char *u8, size_t len,
		    void *data)
{
	struct tterm *t = data;

	if (t->rlen + len < sizeof(t->reply)) {
		memcpy(t->reply + t->rlen, u8, len);
		t->rlen += len;
		t->reply[t->rlen] = 0;
	}
}

static __attribute__((unused)) void t_new(struct tterm *t, int cols, int rows)
{
	memset(t, 0, sizeof(*t));
	if (tsm_screen_new(&t->scr) < 0 ||
	    tsm_vte_new(&t->vte, t->scr, t_write, t) < 0) {
		fprintf(stderr, "cannot create the terminal\n");
		exit(2);
	}
	tsm_screen_resize(t->scr, cols, rows);
	tsm_screen_set_max_sb(t->scr, 1000);
	tsm_vte_hard_reset(t->vte);
}

static __attribute__((unused)) void t_free(struct tterm *t)
{
	tsm_vte_unref(t->vte);
	tsm_screen_unref(t->scr);
}

static __attribute__((unused)) void t_inl(struct tterm *t, const void *s, size_t len)
{
	tsm_vte_input(t->vte, s, len);
}

static __attribute__((unused)) void t_in(struct tterm *t, const char *s)
{
	tsm_vte_input(t->vte, s, strlen(s));
}

static __attribute__((unused)) void t_clear_reply(struct tterm *t)
{
	t->rlen = 0;
	t->reply[0] = 0;
}

/* the cells drawn now */
struct tgrid {
	uint32_t ch[T_MAXROWS][T_MAXCOLS];
	int width[T_MAXROWS][T_MAXCOLS];
	uint32_t pic[T_MAXROWS][T_MAXCOLS];
	struct tsm_screen_attr attr[T_MAXROWS][T_MAXCOLS];
	int cols, rows;
};

static __attribute__((unused)) void t_draw_cb(struct tsm_screen *con, uint32_t id, const uint32_t *ch,
		      size_t len, int width, int x, int y,
		      const struct tsm_screen_attr *attr, tsm_age_t age,
		      void *data)
{
	struct tgrid *g = data;

	if (x >= T_MAXCOLS || y >= T_MAXROWS)
		return;
	g->ch[y][x] = len ? ch[0] : 0;
	g->width[y][x] = width;
	g->attr[y][x] = *attr;
#if HAVOC_SIXEL
	g->pic[y][x] = attr->pic;
#else
	g->pic[y][x] = 0;
#endif
	if (x + 1 > g->cols)
		g->cols = x + 1;
	if (y + 1 > g->rows)
		g->rows = y + 1;
}

static __attribute__((unused)) void t_grab(struct tterm *t, struct tgrid *g)
{
	memset(g, 0, sizeof(*g));
	tsm_screen_draw(t->scr, t_draw_cb, g);
}

/* the text of a row, blanks as spaces, trailing blanks cut, others as '?' */
static __attribute__((unused)) void t_row(struct tterm *t, int row, char *out, size_t n)
{
	static struct tgrid g;
	int x, last = -1;

	t_grab(t, &g);
	for (x = 0; x < g.cols && (size_t)x + 1 < n; x++) {
		uint32_t c = g.ch[row][x];

		out[x] = !c ? ' ' : c < 0x7f && c >= 0x20 ? c : '?';
		if (c)
			last = x;
	}
	out[last + 1] = 0;
}

static __attribute__((unused)) int t_summary(const char *name)
{
	printf("%s: %d checks, %d failed\n", name, t_checks, t_failed);
	return t_failed ? 1 : 0;
}

#endif
