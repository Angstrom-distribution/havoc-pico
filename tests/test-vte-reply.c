/* Replies that depend on tsm_vte_set_graphics(), and the ones that do not. */
#include "tsmtest.h"

static struct tterm T;

#define REPLY(in, out) do { \
	t_clear_reply(&T); \
	t_in(&T, in); \
	CHECK_STR(T.reply, out); \
} while (0)

static void row_is(int row, const char *s)
{
	char buf[T_MAXCOLS + 1];

	t_row(&T, row, buf, sizeof(buf));
	CHECK_STR(buf, s);
}

static const struct tsm_vte_graphics g_sixel = {
	.sixel = true, .cell_w = 6, .cell_h = 10, .registers = 256,
	.version = "0.7.0-git",
};

static void test_before(void)
{
	t_new(&T, 40, 32);
	REPLY("\033[c", "\033[?62;1;6;9;15c");
	t_in(&T, "a");
	REPLY("\033[?1;1;0S", "");
	/* it scrolled the screen */
	row_is(0, "");
	CHECK_INT(tsm_screen_get_sb_count(T.scr), 1);
	REPLY("\033[14t", "");
	REPLY("\033[16t", "");
	REPLY("\033[18t", "");
	REPLY("\033[>q", "");
	CHECK_INT(tsm_screen_get_sb_count(T.scr), 1);
	REPLY("\033[3J", "");
	CHECK_INT(tsm_screen_get_sb_count(T.scr), 1);
	/* 8452 is only a flag */
	t_in(&T, "\033[?8452h");
	CHECK_INT(tsm_vte_get_graphics_flags(T.vte), TSM_VTE_SIXEL_CURSOR_RIGHT);
	t_free(&T);
}

static void test_after(void)
{
	t_new(&T, 40, 32);
	tsm_vte_set_graphics(T.vte, &g_sixel);
	REPLY("\033[c", "\033[?62;1;4;6;9;15c");
	REPLY("\033[>c", "\033[>1;1;0c");
	t_in(&T, "a");
	REPLY("\033[?1;1;0S", "\033[?1;0;256S");
	row_is(0, "a");
	CHECK_INT(tsm_screen_get_sb_count(T.scr), 0);
	REPLY("\033[?1;3;0S", "\033[?1;3;256S");
	REPLY("\033[?1;2S", "\033[?1;0;256S");
	REPLY("\033[?1;4S", "\033[?1;0;256S");
	REPLY("\033[?1;9S", "\033[?1;2;256S");
	REPLY("\033[?2;1;0S", "\033[?2;0;240;320S");
	REPLY("\033[?2;3;0S", "\033[?2;3;240;320S");
	REPLY("\033[?3;1;0S", "\033[?3;1;0S");
	REPLY("\033[?5;1;0S", "\033[?5;1;0S");
	REPLY("\033[14t", "\033[4;320;240t");
	REPLY("\033[16t", "\033[6;10;6t");
	REPLY("\033[18t", "\033[8;32;40t");
	REPLY("\033[21t", "");
	REPLY("\033[?14t", "");
	REPLY("\033[>q", "\033P>|havoc(0.7.0-git)\033\\");
	REPLY("\033[>1q", "");
	REPLY("\033[0 q", "");

	/* the geometry follows the screen */
	tsm_screen_resize(T.scr, 50, 20);
	REPLY("\033[?2;1;0S", "\033[?2;0;300;200S");
	REPLY("\033[18t", "\033[8;20;50t");
	tsm_screen_resize(T.scr, 40, 32);

	/* CSI S without ? scrolls as before */
	t_clear_reply(&T);
	t_in(&T, "\033[S");
	row_is(0, "");
	CHECK_INT(tsm_screen_get_sb_count(T.scr), 1);

	/* ED 3 empties the scrollback */
	REPLY("\033[3J", "");
	CHECK_INT(tsm_screen_get_sb_count(T.scr), 0);

	/* 8452 */
	CHECK_INT(tsm_vte_get_graphics_flags(T.vte), 0);
	t_in(&T, "\033[?8452h");
	CHECK_INT(tsm_vte_get_graphics_flags(T.vte), TSM_VTE_SIXEL_CURSOR_RIGHT);
	t_in(&T, "\033[?8452l");
	CHECK_INT(tsm_vte_get_graphics_flags(T.vte), 0);
	t_in(&T, "\033[?8452h");
	tsm_vte_reset(T.vte);
	CHECK_INT(tsm_vte_get_graphics_flags(T.vte), 0);

	/* replies in the order of the requests */
	REPLY("\033[c\033[>q\033[5n",
	      "\033[?62;1;4;6;9;15c\033P>|havoc(0.7.0-git)\033\\\033[0n");

	/* back to the old replies */
	tsm_vte_set_graphics(T.vte, NULL);
	REPLY("\033[c", "\033[?62;1;6;9;15c");
	REPLY("\033[14t", "");
	REPLY("\033[>q", "");
	t_in(&T, "x");
	REPLY("\033[?1;1;0S", "");
	CHECK_INT(tsm_screen_get_sb_count(T.scr), 1);
	REPLY("\033[3J", "");
	CHECK_INT(tsm_screen_get_sb_count(T.scr), 1);
	t_free(&T);
}

static void test_no_sixel(void)
{
	struct tsm_vte_graphics g = g_sixel;

	g.sixel = false;
	t_new(&T, 40, 32);
	tsm_vte_set_graphics(T.vte, &g);
	REPLY("\033[c", "\033[?62;1;6;9;15c");
	t_in(&T, "a");
	/* no reply and no scroll either */
	REPLY("\033[?1;1;0S", "");
	REPLY("\033[?2;1;0S", "");
	row_is(0, "a");
	CHECK_INT(tsm_screen_get_sb_count(T.scr), 0);
	/* the size reports are for any picture protocol */
	REPLY("\033[14t", "\033[4;320;240t");
	REPLY("\033[16t", "\033[6;10;6t");
	REPLY("\033[>q", "\033P>|havoc(0.7.0-git)\033\\");
	t_free(&T);
}

static void test_version_copy(void)
{
	char v[64];
	struct tsm_vte_graphics g = g_sixel;

	/* the version is copied, and cut at 31 bytes */
	strcpy(v, "1234567890123456789012345678901234567890");
	g.version = v;
	t_new(&T, 40, 32);
	tsm_vte_set_graphics(T.vte, &g);
	v[0] = 'X';
	REPLY("\033[>q", "\033P>|havoc(1234567890123456789012345678901)\033\\");
	g.version = NULL;
	tsm_vte_set_graphics(T.vte, &g);
	REPLY("\033[>q", "\033P>|havoc()\033\\");
	t_free(&T);
}

static void test_reply_api(void)
{
	struct tsm_screen_attr a;

	t_new(&T, 40, 32);
	t_clear_reply(&T);
	tsm_vte_reply(T.vte, "\033_Gi=1;OK\033\\", 11);
	CHECK_STR(T.reply, "\033_Gi=1;OK\033\\");
	t_in(&T, "\033[5n");
	tsm_vte_reply(T.vte, "x", 1);
	CHECK_STR(T.reply, "\033_Gi=1;OK\033\\\033[0nx");

	t_in(&T, "\033[1;44m");
	tsm_vte_get_attr(T.vte, &a);
	CHECK_INT(a.bold, 1);
	CHECK_INT(a.bccode, TSM_COLOR_BLUE);
	t_in(&T, "\033[0m");
	tsm_vte_get_attr(T.vte, &a);
	CHECK_INT(a.bold, 0);
	CHECK_INT(a.bccode, TSM_COLOR_BACKGROUND);
	t_free(&T);
}

/* OSC numbers too large for an int are refused, not wrapped: 4294967297
 * would wrap to colour 1 */
static void test_osc_numbers(void)
{
	t_new(&T, 40, 32);
	REPLY("\033]4;1;?\a", "\033]4;1;rgb:CD/00/00\a");
	REPLY("\033]4;4294967297;?\a", "");
	REPLY("\033]4294967300;x\a", "");
	t_in(&T, "\033]4;1;rgb:100000001/100000002/100000003\a");
	REPLY("\033]4;1;?\a", "\033]4;1;rgb:CD/00/00\a");
	t_free(&T);
}

int main(void)
{
	test_before();
	test_after();
	test_no_sixel();
	test_version_copy();
	test_reply_api();
	test_osc_numbers();
	return t_summary("test-vte-reply");
}
