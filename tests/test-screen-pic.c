/* Picture tiles in cells (built with HAVOC_SIXEL=1). */
#include <errno.h>
#include "tsmtest.h"

#if !HAVOC_SIXEL
#error test-screen-pic.c needs -DHAVOC_SIXEL=1
#endif

#define COLS 40
#define ROWS 12

static struct tterm T;
static struct tgrid G;
static int nscroll, nscroll_sb;

static void ev_cb(struct tsm_screen *con, const struct tsm_screen_event *ev,
		  void *data)
{
	if (ev->type == TSM_SCREEN_EV_SCROLL && ev->n > 0) {
		nscroll += ev->n;
		if (ev->to_sb)
			nscroll_sb += ev->n;
	}
}

static void setup(void)
{
	t_new(&T, COLS, ROWS);
	tsm_screen_set_event_cb(T.scr, ev_cb, NULL);
	nscroll = nscroll_sb = 0;
}

static void grab(void)
{
	t_grab(&T, &G);
}

static int place(int slot, int cols, int rows, int cursor_row,
		 unsigned int flags)
{
	struct tsm_screen_attr a;

	tsm_vte_get_attr(T.vte, &a);
	return tsm_screen_place_picture(T.scr, &a, slot, cols, rows,
					cursor_row, flags);
}

static void cursor_is(int x, int y)
{
	CHECK_INT(tsm_screen_get_cursor_x(T.scr), x);
	CHECK_INT(tsm_screen_get_cursor_y(T.scr), y);
}

/* the tiles of a picture at (x, y) are all there and blank */
static void tiles_are(int slot, int x, int y, int cols, int rows)
{
	int c, r;

	grab();
	for (r = 0; r < rows; r++)
		for (c = 0; c < cols; c++) {
			CHECK_INT(G.pic[y + r][x + c], TSM_PIC(slot, c, r));
			CHECK_INT(G.ch[y + r][x + c], 0);
			CHECK_INT(G.width[y + r][x + c], 1);
		}
}

static int count_pics(void)
{
	int x, y, n = 0;

	grab();
	for (y = 0; y < G.rows; y++)
		for (x = 0; x < G.cols; x++)
			if (G.pic[y][x])
				n++;
	return n;
}

static void test_basic(void)
{
	setup();
	t_in(&T, "\033[44m\033[4;6H");
	CHECK_INT(place(7, 3, 2, 1, 0), 0);
	tiles_are(7, 5, 3, 3, 2);
	CHECK_INT(count_pics(), 6);
	CHECK_INT(G.attr[3][5].bccode, TSM_COLOR_BLUE);
	CHECK_INT(G.attr[4][7].bccode, TSM_COLOR_BLUE);
	CHECK_INT(G.pic[3][4], 0);
	CHECK_INT(G.pic[3][8], 0);
	cursor_is(5, 4);

	t_in(&T, "\033[1;1H");
	place(8, 2, 3, 0, 0);
	cursor_is(0, 0);
	t_in(&T, "\033[10;1H");
	place(9, 2, 2, 1, 0);
	cursor_is(0, 10);

	/* cursor right of the picture */
	t_in(&T, "\033[4;6H");
	place(7, 3, 2, 1, TSM_PIC_CURSOR_RIGHT);
	cursor_is(8, 4);
	t_in(&T, "\033[4;6H");
	place(7, 3, 2, 0, TSM_PIC_CURSOR_RIGHT);
	cursor_is(8, 3);
	t_free(&T);
}

static void test_edges(void)
{
	/* clipped at the right margin */
	setup();
	t_in(&T, "\033[1;39H");
	CHECK_INT(place(2, 3, 1, 0, 0), 0);
	grab();
	CHECK_INT(G.pic[0][38], TSM_PIC(2, 0, 0));
	CHECK_INT(G.pic[0][39], TSM_PIC(2, 1, 0));
	CHECK_INT(count_pics(), 2);
	cursor_is(38, 0);
	t_in(&T, "\033[1;39H");
	place(2, 3, 1, 0, TSM_PIC_CURSOR_RIGHT);
	cursor_is(0, 1);
	t_free(&T);

	/* the cursor right of the picture wraps at the bottom and scrolls */
	setup();
	t_in(&T, "\033[12;39H");
	place(2, 5, 1, 0, TSM_PIC_CURSOR_RIGHT);
	cursor_is(0, 11);
	CHECK_INT(nscroll, 1);
	t_free(&T);

	/* a pending wrap starts on the next line */
	setup();
	t_in(&T, "0123456789012345678901234567890123456789");
	cursor_is(40, 0);
	place(3, 2, 1, 0, 0);
	grab();
	CHECK_INT(G.pic[1][0], TSM_PIC(3, 0, 0));
	CHECK_INT(G.pic[1][1], TSM_PIC(3, 1, 0));
	CHECK_INT(G.ch[0][39], '9');
	cursor_is(0, 1);
	t_free(&T);

	/* without auto-wrap it stays in the last column */
	setup();
	t_in(&T, "\033[?7l0123456789012345678901234567890123456789");
	place(3, 2, 1, 0, 0);
	grab();
	CHECK_INT(G.pic[0][39], TSM_PIC(3, 0, 0));
	CHECK_INT(G.pic[1][0], 0);
	CHECK_INT(count_pics(), 1);
	t_free(&T);

	/* a pending wrap on the last line scrolls */
	setup();
	t_in(&T, "\033[12;1H0123456789012345678901234567890123456789");
	cursor_is(40, 11);
	place(3, 2, 1, 0, 0);
	CHECK_INT(nscroll, 1);
	grab();
	CHECK_INT(G.pic[11][0], TSM_PIC(3, 0, 0));
	t_free(&T);
}

static void test_scrolling(void)
{
	uint8_t used[TSM_PIC_SETS][32];

	/* at the bottom the screen scrolls for the rows */
	setup();
	t_in(&T, "\033[12;1H");
	place(4, 2, 4, 3, 0);
	CHECK_INT(nscroll, 3);
	tiles_are(4, 0, 8, 2, 4);
	cursor_is(0, 11);
	t_free(&T);

	/* taller than the screen: the first rows end up in the scrollback */
	setup();
	t_in(&T, "\033[1;1H");
	place(5, 2, 14, 0, 0);
	CHECK_INT(nscroll, 2);
	CHECK_INT(nscroll_sb, 2);
	CHECK_INT(tsm_screen_get_sb_count(T.scr), 2);
	grab();
	CHECK_INT(G.pic[0][0], TSM_PIC(5, 0, 2));
	CHECK_INT(G.pic[11][1], TSM_PIC(5, 1, 13));
	/* the row asked for scrolled off: the cursor goes to the top */
	cursor_is(0, 0);
	tsm_screen_pictures_used(T.scr, used);
	CHECK(used[TSM_PIC_MAIN][0] & (1 << 5));
	CHECK(used[TSM_PIC_SB][0] & (1 << 5));
	CHECK(used[TSM_PIC_VIEW][0] & (1 << 5));
	CHECK(!used[TSM_PIC_ALT][0]);
	tsm_screen_sb_up(T.scr, 2);
	grab();
	CHECK_INT(G.pic[0][0], TSM_PIC(5, 0, 0));
	CHECK_INT(G.pic[1][1], TSM_PIC(5, 1, 1));
	CHECK_INT(G.pic[2][0], TSM_PIC(5, 0, 2));
	t_free(&T);

	/* the cursor row is the last one */
	setup();
	t_in(&T, "\033[3;1H");
	place(5, 2, 3, 2, 0);
	cursor_is(0, 4);
	t_free(&T);

	/* out of range cursor rows are clamped */
	setup();
	t_in(&T, "\033[3;1H");
	place(5, 2, 3, 9, 0);
	cursor_is(0, 4);
	place(5, 2, 3, -4, 0);
	t_free(&T);

	/* in a region the cursor stops at the region top, not at row 0 */
	setup();
	t_in(&T, "\033[4;9r\033[8;1H");
	place(6, 2, 8, 0, 0);
	CHECK_INT(nscroll, 6);
	grab();
	CHECK_INT(G.pic[3][0], TSM_PIC(6, 0, 2));
	CHECK_INT(G.pic[8][1], TSM_PIC(6, 1, 7));
	CHECK_INT(G.pic[2][0], 0);
	CHECK_INT(G.pic[9][0], 0);
	cursor_is(0, 3);
	t_free(&T);

	/* a line the alternate screen reuses after a scroll holds no tile */
	setup();
	t_in(&T, "\033[?1049h\033[1;1H");
	place(6, 2, 1, 0, 0);
	CHECK_INT(T.scr->lines[0]->has_pic, 1);
	t_in(&T, "\033[12;1H\n");
	CHECK_INT(T.scr->lines[11]->has_pic, 0);
	CHECK_INT(count_pics(), 0);
	t_free(&T);
}

static void test_wide(void)
{
	/* a wide character cut at the left edge */
	setup();
	t_in(&T, "\033[1;5H\xe4\xb8\x96");
	grab();
	CHECK_INT(G.ch[0][4], 0x4e16);
	CHECK_INT(G.width[0][4], 2);
	CHECK_INT(G.width[0][5], 0);
	t_in(&T, "\033[1;6H");
	place(6, 2, 1, 0, 0);
	grab();
	CHECK_INT(G.ch[0][4], 0);
	CHECK_INT(G.width[0][4], 1);
	CHECK_INT(G.pic[0][5], TSM_PIC(6, 0, 0));
	CHECK_INT(G.pic[0][6], TSM_PIC(6, 1, 0));
	t_free(&T);

	/* and at the right edge */
	setup();
	t_in(&T, "\033[1;7H\xe4\xb8\x96");
	t_in(&T, "\033[1;5H");
	place(6, 3, 1, 0, 0);
	grab();
	CHECK_INT(G.pic[0][4], TSM_PIC(6, 0, 0));
	CHECK_INT(G.pic[0][6], TSM_PIC(6, 2, 0));
	CHECK_INT(G.ch[0][7], 0);
	CHECK_INT(G.width[0][7], 1);
	CHECK_INT(G.pic[0][7], 0);
	t_free(&T);

	/* a wide character inside the tiles goes whole */
	setup();
	t_in(&T, "\033[1;6H\xe4\xb8\x96");
	t_in(&T, "\033[1;5H");
	place(6, 5, 1, 0, 0);
	tiles_are(6, 4, 0, 5, 1);
	t_free(&T);

	/* a wide character written over a tile: both cells are text */
	setup();
	t_in(&T, "\033[4;6H");
	place(7, 3, 1, 0, 0);
	t_in(&T, "\033[4;6H\xe4\xb8\x96");
	grab();
	CHECK_INT(G.ch[3][5], 0x4e16);
	CHECK_INT(G.pic[3][5], 0);
	CHECK_INT(G.pic[3][6], 0);
	CHECK_INT(G.pic[3][7], TSM_PIC(7, 2, 0));
	t_free(&T);

	/* the same with the second cell a tile: the lead is a tile too */
	setup();
	t_in(&T, "\033[4;5H");
	place(7, 3, 1, 0, 0);
	t_in(&T, "\033[4;4H\xe4\xb8\x96");
	grab();
	CHECK_INT(G.pic[3][3], 0);
	CHECK_INT(G.pic[3][4], 0);
	CHECK_INT(G.pic[3][5], TSM_PIC(7, 1, 0));
	t_free(&T);
}

static void test_text(void)
{
	setup();
	t_in(&T, "\033[4;6H");
	place(7, 3, 2, 1, 0);

	t_in(&T, "\033[4;7HX");
	grab();
	CHECK_INT(G.ch[3][6], 'X');
	CHECK_INT(G.pic[3][6], 0);
	CHECK_INT(G.pic[3][5], TSM_PIC(7, 0, 0));
	CHECK_INT(G.pic[3][7], TSM_PIC(7, 2, 0));

	/* EL, ECH */
	t_in(&T, "\033[4;7H\033[K");
	grab();
	CHECK_INT(G.pic[3][5], TSM_PIC(7, 0, 0));
	CHECK_INT(G.pic[3][6], 0);
	CHECK_INT(G.pic[3][7], 0);
	t_in(&T, "\033[5;6H\033[2X");
	grab();
	CHECK_INT(G.pic[4][5], 0);
	CHECK_INT(G.pic[4][6], 0);
	CHECK_INT(G.pic[4][7], TSM_PIC(7, 2, 1));
	t_free(&T);

	/* DCH and ICH move tiles like text */
	setup();
	t_in(&T, "\033[4;6H");
	place(7, 3, 1, 0, 0);
	t_in(&T, "\033[4;6H\033[P");
	grab();
	CHECK_INT(G.pic[3][5], TSM_PIC(7, 1, 0));
	CHECK_INT(G.pic[3][6], TSM_PIC(7, 2, 0));
	CHECK_INT(G.pic[3][7], 0);
	t_in(&T, "\033[4;6H\033[@");
	grab();
	CHECK_INT(G.pic[3][5], 0);
	CHECK_INT(G.pic[3][6], TSM_PIC(7, 1, 0));
	CHECK_INT(G.pic[3][7], TSM_PIC(7, 2, 0));
	t_free(&T);

	/* IL and DL move whole lines */
	setup();
	t_in(&T, "\033[4;6H");
	place(7, 3, 1, 0, 0);
	t_in(&T, "\033[4;1H\033[L");
	grab();
	CHECK_INT(G.pic[3][5], 0);
	CHECK_INT(G.pic[4][5], TSM_PIC(7, 0, 0));
	t_in(&T, "\033[4;1H\033[M");
	grab();
	CHECK_INT(G.pic[3][5], TSM_PIC(7, 0, 0));
	CHECK_INT(G.pic[4][5], 0);
	t_free(&T);
}

static void test_used_drop(void)
{
	uint8_t used[TSM_PIC_SETS][32];
	uint8_t none[32] = { 0 };

	setup();
	t_in(&T, "\033[44m\033[4;6H");
	place(3, 3, 2, 1, 0);
	tsm_screen_pictures_used(T.scr, used);
	CHECK(used[TSM_PIC_MAIN][0] == (1 << 3));
	CHECK(used[TSM_PIC_VIEW][0] == (1 << 3));
	CHECK(!memcmp(used[TSM_PIC_ALT], none, 32));
	CHECK(!memcmp(used[TSM_PIC_SB], none, 32));

	/* the alternate screen, with a slot above 8 */
	t_in(&T, "\033[?1049h\033[1;1H");
	place(200, 2, 1, 0, 0);
	tsm_screen_pictures_used(T.scr, used);
	CHECK(used[TSM_PIC_MAIN][0] == (1 << 3));
	CHECK(used[TSM_PIC_ALT][200 / 8] == (1 << (200 % 8)));
	CHECK(used[TSM_PIC_VIEW][200 / 8] == (1 << (200 % 8)));
	CHECK(used[TSM_PIC_VIEW][0] == 0);
	t_in(&T, "\033[?1049l");

	/* into the scrollback */
	t_in(&T, "\033[12;1H");
	place(5, 1, 1, 0, 0);
	t_in(&T, "\033[1;12r\033[12;1H");
	{
		int i;

		for (i = 0; i < 14; i++)
			t_in(&T, "\n");
	}
	tsm_screen_pictures_used(T.scr, used);
	CHECK_INT(used[TSM_PIC_SB][0], (1 << 3) | (1 << 5));
	CHECK_INT(used[TSM_PIC_VIEW][0], 0);
	CHECK_INT(used[TSM_PIC_MAIN][0], 0);
	tsm_screen_sb_up(T.scr, 100);
	tsm_screen_pictures_used(T.scr, used);
	CHECK_INT(used[TSM_PIC_VIEW][0], (1 << 3) | (1 << 5));
	tsm_screen_sb_reset(T.scr);
	t_free(&T);

	/* has_pic is cleared once the tiles are gone */
	setup();
	t_in(&T, "\033[4;6H");
	place(3, 2, 1, 0, 0);
	CHECK_INT(T.scr->lines[3]->has_pic, 1);
	CHECK_INT(T.scr->lines[2]->has_pic, 0);
	tsm_screen_pictures_used(T.scr, used);
	CHECK_INT(T.scr->lines[3]->has_pic, 1);
	t_in(&T, "\033[2J");
	tsm_screen_pictures_used(T.scr, used);
	CHECK_INT(T.scr->lines[3]->has_pic, 0);
	CHECK(!memcmp(used[TSM_PIC_MAIN], none, 32));
	t_free(&T);

	/* drop: the colours stay, other slots stay */
	setup();
	t_in(&T, "\033[44m\033[4;6H");
	place(3, 3, 2, 0, 0);
	t_in(&T, "\033[1;1H");
	place(3, 2, 1, 0, 0);
	t_in(&T, "\033[41m\033[8;6H");
	place(4, 2, 1, 0, 0);
	t_in(&T, "\033[12;1H\n\n");
	CHECK_INT(tsm_screen_get_sb_count(T.scr), 2);
	tsm_screen_pictures_used(T.scr, used);
	CHECK(used[TSM_PIC_SB][0] == (1 << 3));
	CHECK(used[TSM_PIC_MAIN][0] == ((1 << 3) | (1 << 4)));
	tsm_screen_drop_picture(T.scr, 3);
	grab();
	CHECK_INT(G.pic[1][5], 0);
	CHECK_INT(G.attr[1][5].bccode, TSM_COLOR_BLUE);
	CHECK_INT(G.attr[2][7].bccode, TSM_COLOR_BLUE);
	CHECK_INT(G.pic[5][5], TSM_PIC(4, 0, 0));
	tsm_screen_pictures_used(T.scr, used);
	CHECK(used[TSM_PIC_MAIN][0] == (1 << 4));
	CHECK(used[TSM_PIC_SB][0] == 0);
	tsm_screen_sb_up(T.scr, 2);
	grab();
	CHECK_INT(G.pic[0][0], 0);
	CHECK_INT(G.attr[0][0].bccode, TSM_COLOR_BLUE);
	t_free(&T);
}

static void test_invalid(void)
{
	setup();
	CHECK_INT(place(0, 1, 1, 0, 0), -EINVAL);
	CHECK_INT(place(256, 1, 1, 0, 0), -EINVAL);
	CHECK_INT(place(1, 0, 1, 0, 0), -EINVAL);
	CHECK_INT(place(1, TSM_PIC_MAX + 1, 1, 0, 0), -EINVAL);
	CHECK_INT(place(1, 1, 0, 0, 0), -EINVAL);
	CHECK_INT(place(1, 1, TSM_PIC_MAX + 1, 0, 0), -EINVAL);
	CHECK_INT(count_pics(), 0);
	cursor_is(0, 0);
	CHECK_INT(place(255, 1, 1, 0, 0), 0);
	CHECK_INT(place(1, TSM_PIC_MAX, 1, 0, 0), 0);
	CHECK_INT(place(1, 1, TSM_PIC_MAX, 0, 0), 0);
	t_free(&T);
}

int main(void)
{
	test_basic();
	test_edges();
	test_scrolling();
	test_wide();
	test_text();
	test_used_drop();
	test_invalid();
	return t_summary("test-screen-pic");
}
