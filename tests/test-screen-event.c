/* Screen events and the scrollback view. */
#include "tsmtest.h"

static struct tterm T;
static char evlog[4096];

static void ev_cb(struct tsm_screen *con, const struct tsm_screen_event *ev,
		  void *data)
{
	char buf[64];

	switch (ev->type) {
	case TSM_SCREEN_EV_SCROLL:
		snprintf(buf, sizeof(buf), "S%d-%d:%d:%c:%c", ev->top,
			 ev->bottom, ev->n, ev->alt ? 'a' : 'm',
			 ev->to_sb ? 's' : 'd');
		break;
	case TSM_SCREEN_EV_ERASE_SCREEN:
		snprintf(buf, sizeof(buf), "E%d", ev->alt);
		break;
	case TSM_SCREEN_EV_CLEAR_SB:
		snprintf(buf, sizeof(buf), "C");
		break;
	case TSM_SCREEN_EV_ALTERNATE:
		snprintf(buf, sizeof(buf), "A%d", ev->alt);
		break;
	case TSM_SCREEN_EV_RESET:
		snprintf(buf, sizeof(buf), "R");
		break;
	default:
		snprintf(buf, sizeof(buf), "?");
	}
	if (*evlog)
		strcat(evlog, " ");
	strcat(evlog, buf);
}

static void setup(void)
{
	t_new(&T, 40, 32);
	tsm_screen_set_event_cb(T.scr, ev_cb, NULL);
	evlog[0] = 0;
}

/* the events since the last call */
#define EVENTS(s) do { CHECK_STR(evlog, s); evlog[0] = 0; } while (0)

static void test_scroll(void)
{
	setup();
	t_in(&T, "\033[32;1H");
	EVENTS("");
	t_in(&T, "\n");
	EVENTS("S0-31:1:m:s");
	t_in(&T, "\033[3S");
	EVENTS("S0-31:3:m:s");
	t_in(&T, "\033[2T");
	EVENTS("S0-31:-2:m:d");
	t_in(&T, "\033[1;1H\033M");
	EVENTS("S0-31:-1:m:d");
	t_in(&T, "\033[1;1H\033[M");
	EVENTS("S0-31:1:m:d");

	/* a region */
	t_in(&T, "\033[5;20r\033[20;1H\n");
	EVENTS("S4-19:1:m:s");
	t_in(&T, "\033[5;1H\033M");
	EVENTS("S4-19:-1:m:d");
	t_in(&T, "\033[8;1H\033[2L");
	EVENTS("S7-19:-2:m:d");
	t_in(&T, "\033[2M");
	EVENTS("S7-19:2:m:d");
	/* outside the region: nothing moves */
	t_in(&T, "\033[2;1H\033[2L");
	EVENTS("");
	t_in(&T, "\033[r\033[8;1H\033[2L");
	EVENTS("S7-31:-2:m:d");
	t_in(&T, "\033[2M");
	EVENTS("S7-31:2:m:d");

	/* more lines than the screen has */
	t_in(&T, "\033[300S");
	EVENTS("S0-31:32:m:s");
	t_in(&T, "\033[300T");
	EVENTS("S0-31:-32:m:d");
	t_free(&T);
}

static void test_alt(void)
{
	setup();
	t_in(&T, "\033[?1049h");
	EVENTS("A1 E1");
	t_in(&T, "\033[32;1H\n");
	EVENTS("S0-31:1:a:d");
	t_in(&T, "\033[?1049l");
	EVENTS("A0");

	t_in(&T, "\033[?47h");
	EVENTS("A1");
	t_in(&T, "\033[?47l");
	EVENTS("A0");

	t_in(&T, "\033[?1047h");
	EVENTS("A1");
	t_in(&T, "\033[?1047l");
	EVENTS("E1 A0");

	/* a second switch to the same screen is no event */
	t_in(&T, "\033[?1049l");
	t_in(&T, "\033[?47l");
	EVENTS("");
	t_free(&T);
}

static void test_erase_reset(void)
{
	setup();
	t_in(&T, "\033[2J");
	EVENTS("E0");
	t_in(&T, "\033[J");
	EVENTS("");
	t_in(&T, "\033[3J");
	EVENTS("");		/* no graphics set: ignored as before */

	{
		struct tsm_vte_graphics g = { .sixel = true, .cell_w = 6,
					      .cell_h = 10, .registers = 256 };

		tsm_vte_set_graphics(T.vte, &g);
	}
	t_in(&T, "\033[3J");
	EVENTS("C");

	t_in(&T, "\033c");
	EVENTS("E0 C R");

	t_in(&T, "\033[?1049h");
	EVENTS("A1 E1");
	t_in(&T, "\033c");
	EVENTS("A0 E0 C R");

	t_in(&T, "\033[?1049h");
	EVENTS("A1 E1");
	tsm_vte_reset(T.vte);
	EVENTS("A0");
	tsm_vte_hard_reset(T.vte);
	EVENTS("E0 C R");
	t_free(&T);
}

static void test_resize(void)
{
	setup();
	t_in(&T, "\033[32;1Hx");
	tsm_screen_resize(T.scr, 40, 20);
	EVENTS("S0-31:12:m:s");
	CHECK_INT(tsm_screen_get_sb_count(T.scr), 12);
	t_free(&T);

	/* the cursor higher up: the rows below are empty and stay */
	setup();
	t_in(&T, "\033[5;1Hx");
	tsm_screen_resize(T.scr, 40, 20);
	EVENTS("");
	t_free(&T);
}

static void test_no_scrollback(void)
{
	setup();
	tsm_screen_set_max_sb(T.scr, 0);
	t_in(&T, "\033[32;1H\n");
	/* the line is dropped, not kept */
	EVENTS("S0-31:1:m:d");
	t_free(&T);
}

static void test_sb_view(void)
{
	int i;

	setup();
	tsm_screen_set_max_sb(T.scr, 10);
	for (i = 0; i < 40; i++)
		t_in(&T, "\033[32;1H\n");
	CHECK_INT(tsm_screen_get_sb_count(T.scr), 10);
	CHECK_INT(tsm_screen_get_sb_offset(T.scr), 0);
	tsm_screen_sb_up(T.scr, 5);
	CHECK_INT(tsm_screen_get_sb_offset(T.scr), 5);
	tsm_screen_sb_down(T.scr, 2);
	CHECK_INT(tsm_screen_get_sb_offset(T.scr), 3);
	tsm_screen_sb_reset(T.scr);
	CHECK_INT(tsm_screen_get_sb_offset(T.scr), 0);
	tsm_screen_sb_page_up(T.scr, 1);
	CHECK_INT(tsm_screen_get_sb_offset(T.scr), 10);
	tsm_screen_sb_page_down(T.scr, 1);
	CHECK_INT(tsm_screen_get_sb_offset(T.scr), 0);

	/* new lines while scrolled back */
	tsm_screen_sb_up(T.scr, 4);
	CHECK_INT(tsm_screen_get_sb_offset(T.scr), 4);
	tsm_screen_sb_reset(T.scr);
	tsm_screen_sb_up(T.scr, 2);
	t_in(&T, "\033[32;1H\n");
	CHECK_INT(tsm_screen_get_sb_count(T.scr), 10);
	CHECK(tsm_screen_get_sb_offset(T.scr) >= 1);
	tsm_screen_clear_sb(T.scr);
	CHECK_INT(tsm_screen_get_sb_count(T.scr), 0);
	CHECK_INT(tsm_screen_get_sb_offset(T.scr), 0);
	t_free(&T);
}

int main(void)
{
	test_scroll();
	test_alt();
	test_erase_reset();
	test_resize();
	test_no_scrollback();
	test_sb_view();
	return t_summary("test-screen-event");
}
