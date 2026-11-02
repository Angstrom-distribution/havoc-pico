/* kplace.c wired to the events of a real libtsm screen and vte, the way
 * graphics.c wires it: one list per screen, the event's screen chooses.
 * Line feeds, scroll regions, insert and delete lines, the alternate
 * screen, a resize and a reset; the rows after each step are worked out by
 * hand. */
#include "tsmtest.h"
#include "../kplace.h"

static struct tterm T;
static struct kplace_list L[2];
static bool alt;
static int gone_n;

static void gone(void *data, const struct kplace *p)
{
	gone_n++;
}

static void ev_cb(struct tsm_screen *con, const struct tsm_screen_event *ev,
		  void *data)
{
	int h = tsm_screen_get_height(con);

	switch (ev->type) {
	case TSM_SCREEN_EV_ALTERNATE:
		alt = ev->alt;
		break;
	case TSM_SCREEN_EV_CLEAR_SB:
		kplace_event(&L[0], ev, h, tsm_screen_get_sb_count(con), gone,
			     NULL);
		break;
	case TSM_SCREEN_EV_RESET:
		kplace_clear(&L[0], gone, NULL);
		kplace_clear(&L[1], gone, NULL);
		break;
	default:
		kplace_event(&L[ev->alt], ev, h,
			     ev->alt ? 0 : tsm_screen_get_sb_count(con), gone,
			     NULL);
	}
}

static void setup(int cols, int rows)
{
	struct tsm_vte_graphics vg = { .cell_w = 6, .cell_h = 10, .registers = 256,
				       .version = "test" };

	t_new(&T, cols, rows);
	/* ED 3 clears the scrollback only once graphics are set */
	tsm_vte_set_graphics(T.vte, &vg);
	tsm_screen_set_event_cb(T.scr, ev_cb, NULL);
	kplace_fini(&L[0], NULL, NULL);
	kplace_fini(&L[1], NULL, NULL);
	alt = false;
	gone_n = 0;
}

/* a placement of rows rows at the cursor row of the active screen */
static void put(uint32_t image, int rows)
{
	struct kplace p;

	memset(&p, 0, sizeof(p));
	p.image = image;
	p.rows = rows;
	p.cols = 2;
	p.row = tsm_screen_get_cursor_y(T.scr);
	p.order = image;
	kplace_add(&L[alt], &p);
}

static struct kplace *get(int list, uint32_t image)
{
	int i;

	for (i = 0; i < L[list].n; i++)
		if (L[list].v[i].image == image)
			return &L[list].v[i];
	return NULL;
}

#define ROW(l, img) (get(l, img) ? get(l, img)->row : -999)
#define SKIP(l, img) (get(l, img) ? get(l, img)->skip : -999)
#define ROWS(l, img) (get(l, img) ? get(l, img)->rows : -999)

static void to(int row)	/* 1-based */
{
	char b[32];

	snprintf(b, sizeof(b), "\033[%d;1H", row);
	t_in(&T, b);
}

static void test_line_feeds(void)
{
	setup(40, 10);
	to(4);
	put(1, 2);
	to(10);
	t_in(&T, "\n\n\n\n");
	CHECK_INT(ROW(0, 1), -1);
	CHECK_INT(SKIP(0, 1), 0);
	CHECK_INT(tsm_screen_get_sb_count(T.scr), 4);
	t_in(&T, "\n\n\n\n\n\n");
	CHECK_INT(ROW(0, 1), -7);
	CHECK_INT(tsm_screen_get_sb_count(T.scr), 10);
	/* ED 3 empties the scrollback; the placement was in it */
	t_in(&T, "\033[3J");
	CHECK_INT(ROW(0, 1), -999);
	CHECK_INT(gone_n, 1);
	t_free(&T);
}

static void test_clear_sb_partial(void)
{
	setup(40, 10);
	to(10);
	put(1, 3);		/* rows 9..11: hangs, the screen scrolls it */
	t_in(&T, "\n\n");	/* in: row 7, ends at 9 */
	CHECK_INT(ROW(0, 1), 9 - 2);
	t_in(&T, "\n\n\n\n");	/* row 3 */
	CHECK_INT(ROW(0, 1), 3);
	t_in(&T, "\n\n\n\n\n");	/* row -2: rows -2..0 */
	CHECK_INT(ROW(0, 1), -2);
	t_in(&T, "\033[3J");
	CHECK_INT(ROW(0, 1), -2);
	CHECK_INT(SKIP(0, 1), 2);
	CHECK_INT(ROWS(0, 1), 3);
	t_in(&T, "\033[2J");	/* the screen row 0 is part of it */
	CHECK_INT(ROW(0, 1), -999);
	t_free(&T);
}

static void test_region(void)
{
	setup(40, 10);
	to(1);
	put(3, 2);		/* rows 0..1, above the region */
	to(5);
	put(1, 2);		/* rows 4..5 inside */
	to(3);
	put(2, 2);		/* rows 2..3 inside, at the top */
	to(9);
	put(4, 2);		/* rows 8..9, below */
	t_in(&T, "\033[3;8r");	/* rows 2..7 */
	to(8);
	t_in(&T, "\n");
	CHECK_INT(ROW(0, 1), 3);
	CHECK_INT(ROW(0, 2), 1);
	CHECK_INT(SKIP(0, 2), 1);
	CHECK_INT(ROW(0, 3), 0);
	CHECK_INT(ROW(0, 4), 8);
	to(8);
	t_in(&T, "\n");
	CHECK_INT(ROW(0, 1), 2);
	CHECK_INT(ROW(0, 2), -999);	/* both rows cut */
	CHECK_INT(ROW(0, 3), 0);
	CHECK_INT(ROW(0, 4), 8);
	/* reverse index at the region top moves them down */
	to(3);
	t_in(&T, "\033M");
	CHECK_INT(ROW(0, 1), 3);
	CHECK_INT(ROW(0, 3), 0);
	t_in(&T, "\033M\033M\033M\033M");	/* 4 more: row 7 rows 2 */
	CHECK_INT(ROW(0, 1), 7);
	CHECK_INT(ROWS(0, 1), 1);	/* the bottom row is cut at 7 */
	t_free(&T);
}

static void test_insert_delete(void)
{
	setup(40, 10);
	to(3);
	put(1, 2);		/* rows 2..3 */
	to(7);
	put(2, 2);		/* rows 6..7 */
	to(9);
	put(3, 2);		/* rows 8..9 */
	to(5);
	t_in(&T, "\033[2L");	/* insert 2 lines at row 4: rows 4..9 move down */
	CHECK_INT(ROW(0, 1), 2);
	CHECK_INT(ROW(0, 2), 8);
	CHECK_INT(ROW(0, 3), -999);	/* pushed off the bottom */
	t_in(&T, "\033[1M");	/* delete 1 line at row 4 */
	CHECK_INT(ROW(0, 2), 7);
	CHECK_INT(ROW(0, 1), 2);
	t_free(&T);
}

static void test_alternate(void)
{
	setup(40, 10);
	to(3);
	put(1, 2);
	t_in(&T, "\033[?1049h");
	CHECK_INT(alt, 1);
	CHECK_INT(ROW(0, 1), 3 - 1);	/* the main list is kept */
	to(3);
	put(2, 2);
	to(10);
	t_in(&T, "\n");		/* a scroll on the alternate screen */
	CHECK_INT(ROW(1, 2), 1);
	CHECK_INT(ROW(0, 1), 2);	/* untouched by it */
	t_in(&T, "\n\n");
	CHECK_INT(ROW(1, 2), -1);
	CHECK_INT(SKIP(1, 2), 1);
	t_in(&T, "\n");
	CHECK_INT(ROW(1, 2), -999);
	t_in(&T, "\033[?1049l");
	CHECK_INT(alt, 0);
	CHECK_INT(ROW(0, 1), 2);
	/* the next entry erases the alternate screen and its list */
	to(3);
	t_in(&T, "\033[?1049h");
	put(5, 2);
	t_in(&T, "\033[?1049l\033[?1049h");
	CHECK_INT(ROW(1, 5), -999);
	t_free(&T);
}

static void test_resize(void)
{
	setup(40, 10);
	to(9);
	put(1, 2);		/* rows 8..9 */
	to(10);
	t_in(&T, "x");		/* text on the last row: the lines must go up */
	tsm_screen_resize(T.scr, 40, 6);
	/* the cursor was on the last row of ten: four lines went up */
	CHECK_INT(ROW(0, 1), 4);
	CHECK_INT(ROWS(0, 1), 2);
	tsm_screen_resize(T.scr, 40, 12);
	CHECK_INT(ROW(0, 1), 4);	/* more rows move nothing */
	t_free(&T);
}

static void test_reset(void)
{
	setup(40, 10);
	put(1, 2);
	to(10);
	t_in(&T, "\n\n\n");
	put(2, 1);
	t_in(&T, "\033[?1049h");
	put(3, 1);
	CHECK(L[0].n == 2 && L[1].n == 1);
	t_in(&T, "\033c");
	CHECK(L[0].n == 0 && L[1].n == 0);
	t_free(&T);
}

int main(void)
{
	test_line_feeds();
	test_clear_sb_partial();
	test_region();
	test_insert_delete();
	test_alternate();
	test_resize();
	test_reset();
	kplace_fini(&L[0], NULL, NULL);
	kplace_fini(&L[1], NULL, NULL);
	return t_summary("test-kplace-tsm");
}
