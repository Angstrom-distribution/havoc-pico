/* kplace.c with synthetic libtsm events: every rule for a line move, an
 * erase and a clear, and the view. Needs no terminal. */
#include <stdlib.h>
#include <string.h>

#include "../kplace.h"
#include "check.h"

static struct kplace_list L;
static int gone_n;
static uint32_t gone_img[512];

static void gone(void *data, const struct kplace *p)
{
	(void)data;
	if (gone_n < 512)
		gone_img[gone_n] = p->image;
	gone_n++;
}

static void fresh(void)
{
	kplace_fini(&L, NULL, NULL);
	gone_n = 0;
}

static void add(uint32_t image, int row, int rows, int z)
{
	struct kplace p;
	static uint32_t order;

	memset(&p, 0, sizeof(p));
	p.image = image;
	p.row = row;
	p.rows = rows;
	p.cols = 2;
	p.z = z;
	p.order = ++order;
	CHECK(kplace_add(&L, &p) != NULL);
}

static struct kplace *find(uint32_t image)
{
	int i;

	for (i = 0; i < L.n; i++)
		if (L.v[i].image == image)
			return &L.v[i];
	return NULL;
}

/* the row and the skip of image, or -999 when it is gone */
#define ROW(img) (find(img) ? find(img)->row : -999)
#define SKIP(img) (find(img) ? find(img)->skip : -999)
#define ROWS(img) (find(img) ? find(img)->rows : -999)

static void ev(enum tsm_screen_event_type type, int top, int bottom, int n,
	       bool to_sb, int size_y, int sb_count)
{
	struct tsm_screen_event e = { .type = type, .top = top,
				      .bottom = bottom, .n = n,
				      .to_sb = to_sb };

	kplace_event(&L, &e, size_y, sb_count, gone, NULL);
}

#define UP(top, bottom, n, to_sb, sb) \
	ev(TSM_SCREEN_EV_SCROLL, top, bottom, n, to_sb, 24, sb)
#define DOWN(top, bottom, n) \
	ev(TSM_SCREEN_EV_SCROLL, top, bottom, -(n), false, 24, 10)

static void test_up_full_screen(void)
{
	fresh();
	add(1, 5, 3, 0);
	add(2, -3, 2, 0);	/* in the scrollback */
	add(3, -2, 4, 0);	/* straddles the top */
	UP(0, 23, 1, true, 10);
	CHECK(ROW(1) == 4);
	CHECK(ROW(2) == -4);
	CHECK(ROW(3) == -3 && SKIP(3) == 0);
	UP(0, 23, 2, true, 10);
	CHECK(ROW(1) == 2 && ROW(2) == -6 && ROW(3) == -5);
	CHECK(gone_n == 0);
	/* the prune: wholly beyond the lines the scrollback holds */
	UP(0, 23, 1, true, 5);
	CHECK(ROW(2) == -999);		/* -7 + 2 <= -5 */
	CHECK(ROW(3) == -6 && ROWS(3) == 4);
	UP(0, 23, 3, true, 5);
	CHECK(ROW(3) == -999);		/* -9 + 4 <= -5 */
	CHECK(gone_n == 2);
	fresh();
	add(1, 0, 3, 0);
	UP(0, 23, 1, false, 0);		/* no scrollback: the row is lost */
	CHECK(ROW(1) == -1);
	CHECK(SKIP(1) == 1);
	UP(0, 23, 2, false, 0);
	CHECK(ROW(1) == -999);
}

static void test_up_alternate(void)
{
	fresh();
	add(1, 1, 3, 0);
	add(2, 10, 2, 0);
	UP(0, 23, 2, false, 0);	/* rows are lost, not kept */
	CHECK(ROW(1) == -1 && SKIP(1) == 1);
	CHECK(ROW(2) == 8 && SKIP(2) == 0);
	UP(0, 23, 1, false, 0);
	CHECK(ROW(1) == -2 && SKIP(1) == 2);
	UP(0, 23, 1, false, 0);
	CHECK(ROW(1) == -999);
}

static void test_up_region(void)
{
	fresh();
	/* region 5..15 */
	add(1, 8, 3, 0);	/* inside */
	add(2, 3, 3, 0);	/* above, partly: 3..5 */
	add(3, 14, 3, 0);	/* partly below: 14..16 */
	add(4, 0, 2, 0);	/* above */
	add(5, 20, 2, 0);	/* below */
	UP(5, 15, 1, true, 10);
	CHECK(ROW(1) == 7);
	CHECK(ROW(2) == 3 && ROW(3) == 14 && ROW(4) == 0 && ROW(5) == 20);
	/* a placement in the scrollback follows the grown scrollback */
	fresh();
	add(1, -4, 2, 0);
	add(2, 6, 4, 0);
	UP(5, 15, 3, true, 10);
	CHECK(ROW(1) == -7);
	/* inside, cut at the region top */
	CHECK(ROW(2) == 3 && SKIP(2) == 2);
	UP(5, 15, 1, true, 10);
	CHECK(ROW(2) == 2 && SKIP(2) == 3);
	UP(5, 15, 1, true, 10);
	CHECK(ROW(2) == -999);
	/* insert/delete lines: top is the cursor row, rows are dropped */
	fresh();
	add(1, 8, 2, 0);
	add(2, 2, 2, 0);
	UP(6, 23, 2, false, 10);	/* delete 2 lines at row 6 */
	CHECK(ROW(1) == 6 && SKIP(1) == 0 && ROW(2) == 2);
	UP(7, 23, 1, false, 10);	/* at row 7: the placement now starts at 6 */
	CHECK(ROW(1) == 6 && SKIP(1) == 0);
}

static void test_down(void)
{
	fresh();
	add(1, 8, 3, 0);
	add(2, 14, 2, 0);	/* 14..15 */
	add(3, 3, 3, 0);	/* above the region */
	add(4, 14, 3, 0);	/* 14..16: partly below */
	DOWN(5, 15, 1);
	CHECK(ROW(1) == 9);
	CHECK(ROW(2) == 15 && ROWS(2) == 1);	/* last row cut */
	CHECK(ROW(3) == 3 && ROW(4) == 14);
	DOWN(5, 15, 1);
	CHECK(ROW(2) == -999);		/* nothing left */
	CHECK(ROW(1) == 10);
	/* a scroll down does not bring back what the top cut */
	fresh();
	add(1, 3, 4, 0);
	UP(5, 15, 1, true, 10);	/* partly above: not inside, unchanged */
	CHECK(ROW(1) == 3 && SKIP(1) == 0);
	/* a placement in the scrollback is not moved by a scroll down */
	fresh();
	add(1, -3, 2, 0);
	DOWN(0, 23, 2);
	CHECK(ROW(1) == -3);
}

static void test_hang(void)
{
	fresh();
	/* placed at row 30 of 32 with 5 rows, then the cursor goes down */
	add(1, 30, 5, 0);
	ev(TSM_SCREEN_EV_SCROLL, 0, 31, 4, true, 32, 10);
	CHECK(ROW(1) == 26 && SKIP(1) == 0);
	fresh();
	add(1, 30, 40, 0);	/* taller than the screen */
	ev(TSM_SCREEN_EV_SCROLL, 0, 31, 38, true, 32, 100);
	CHECK(ROW(1) == -8 && SKIP(1) == 0);
}

static void test_erase(void)
{
	fresh();
	add(1, 5, 3, 0);
	add(2, -4, 2, 0);	/* wholly in the scrollback */
	add(3, -2, 4, 0);	/* straddles the top */
	add(4, 30, 5, 0);	/* hangs below */
	add(5, 40, 2, 0);	/* wholly below the screen */
	ev(TSM_SCREEN_EV_ERASE_SCREEN, 0, 0, 0, false, 32, 10);
	CHECK(ROW(1) == -999 && ROW(3) == -999 && ROW(4) == -999);
	CHECK(ROW(2) == -4);
	CHECK(ROW(5) == 40);	/* does not touch the screen rows */
	CHECK(gone_n == 3);
}

static void test_clear_sb(void)
{
	fresh();
	add(1, 5, 3, 0);
	add(2, -4, 2, 0);
	add(3, -2, 4, 0);
	add(4, -1, 1, 0);	/* the last scrollback line only */
	ev(TSM_SCREEN_EV_CLEAR_SB, 0, 0, 0, false, 24, 0);
	CHECK(ROW(1) == 5 && SKIP(1) == 0);
	CHECK(ROW(2) == -999 && ROW(4) == -999);
	CHECK(ROW(3) == -2 && SKIP(3) == 2);
	/* it still moves with the screen: first shown row is 0 */
	UP(0, 23, 1, true, 1);
	CHECK(ROW(3) == -3 && SKIP(3) == 2);
	fresh();
	add(1, -4, 2, 0);
	ev(TSM_SCREEN_EV_CLEAR_SB, 0, 0, 0, false, 24, 0);
	CHECK(L.n == 0);
}

static void test_reset(void)
{
	fresh();
	add(1, 5, 3, 0);
	add(2, -4, 2, 0);
	ev(TSM_SCREEN_EV_RESET, 0, 0, 0, false, 24, 10);
	CHECK(L.n == 0 && gone_n == 2);
}

static void test_view(void)
{
	int out[8];
	int n;

	fresh();
	add(1, 5, 3, 0);
	add(2, -4, 2, -3);	/* -4..-3 */
	add(3, -2, 4, 7);	/* -2..1 */
	add(4, 20, 2, 0);	/* 20..21 */
	add(5, 5, 3, 0);	/* same z as 1, later */
	add(6, 40, 2, 0);	/* below the screen */
	n = kplace_in_view(&L, 0, 24, out, 8);
	CHECK(n == 4);
	/* by z, equal z by order of placement */
	CHECK(L.v[out[0]].image == 1 && L.v[out[1]].image == 4 &&
	      L.v[out[2]].image == 5 && L.v[out[3]].image == 3);
	n = kplace_in_view(&L, 5, 24, out, 8);	/* 2 shows at 1..2, 4 is below */
	CHECK(n == 4);
	CHECK(L.v[out[0]].image == 2);
	n = kplace_in_view(&L, 3, 24, out, 8);	/* 2 at -1..0: one row */
	CHECK(n == 5);
	n = kplace_in_view(&L, 2, 24, out, 8);	/* 2 at -2..-1 */
	CHECK(n == 4);
	n = kplace_in_view(&L, 100, 24, out, 8);	/* past the top */
	CHECK(n == 0);
	n = kplace_in_view(&L, 0, 24, out, 2);	/* at most max */
	CHECK(n == 2);
	/* skipped rows are not in view */
	find(1)->skip = 3;
	n = kplace_in_view(&L, 0, 24, out, 8);
	CHECK(n == 3);
	find(4)->skip = 1;	/* shows row 21 */
	n = kplace_in_view(&L, -21, 24, out, 8);
	CHECK(n == 2 && L.v[out[0]].image == 4 && L.v[out[1]].image == 6);
}

static void test_max(void)
{
	int i;
	struct kplace p;

	fresh();
	memset(&p, 0, sizeof(p));
	p.rows = 1;
	p.cols = 1;
	for (i = 0; i < KPLACE_MAX; i++) {
		p.image = i + 1;
		CHECK(kplace_add(&L, &p) != NULL);
	}
	CHECK(kplace_add(&L, &p) == NULL && L.n == KPLACE_MAX);
	kplace_del(&L, 10, gone, NULL);
	CHECK(L.n == KPLACE_MAX - 1 && L.v[10].image == 12);
	CHECK(kplace_add(&L, &p) != NULL);
	kplace_clear(&L, gone, NULL);
	CHECK(L.n == 0 && gone_n == KPLACE_MAX + 1);
}

int main(void)
{
	test_up_full_screen();
	test_up_alternate();
	test_up_region();
	test_down();
	test_hang();
	test_erase();
	test_clear_sb();
	test_reset();
	test_view();
	test_max();
	fresh();
	CHECK_DONE("test-kplace");
}
