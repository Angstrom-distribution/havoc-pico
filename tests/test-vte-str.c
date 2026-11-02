/* DCS and APC strings handed to a consumer. */
#include "tsmtest.h"

#define DATA_MAX 65536

struct rec {
	int starts, puts, ends, done, aborts;
	bool start_ret;
	int fail_put_at;	/* put call that returns false, 0: never */
	struct tsm_vte_str_info info;
	int argv[16];
	char data[DATA_MAX];
	size_t dlen;
	size_t minput, maxput;
	enum tsm_vte_str_end last_end;
	char log[256];
};

static bool r_start(void *data, const struct tsm_vte_str_info *info)
{
	struct rec *r = data;
	int i;

	r->starts++;
	r->info = *info;
	for (i = 0; i < info->argc && i < 16; i++)
		r->argv[i] = info->argv[i];
	r->info.argv = r->argv;
	if (strlen(r->log) < sizeof(r->log) - 2)
		strcat(r->log, "S");
	return r->start_ret;
}

static bool r_put(void *data, const char *buf, size_t len)
{
	struct rec *r = data;

	r->puts++;
	if (!r->minput || len < r->minput)
		r->minput = len;
	if (len > r->maxput)
		r->maxput = len;
	if (r->dlen + len <= DATA_MAX) {
		memcpy(r->data + r->dlen, buf, len);
		r->dlen += len;
	}
	return !(r->fail_put_at && r->puts == r->fail_put_at);
}

static void r_end(void *data, enum tsm_vte_str_end how)
{
	struct rec *r = data;

	r->ends++;
	if (how == TSM_VTE_STR_DONE)
		r->done++;
	else
		r->aborts++;
	r->last_end = how;
	if (strlen(r->log) < sizeof(r->log) - 2)
		strcat(r->log, how == TSM_VTE_STR_DONE ? "D" : "A");
}

static const struct tsm_vte_str_ops ops = { r_start, r_put, r_end };

static struct tterm T;
static struct rec R;

static void setup(void)
{
	t_new(&T, 40, 10);
	memset(&R, 0, sizeof(R));
	R.start_ret = true;
	tsm_vte_set_str_ops(T.vte, &ops, &R);
}

static void teardown(void)
{
	t_free(&T);
}

#define data_is(s) CHECK(R.dlen == strlen(s) && !memcmp(R.data, s, R.dlen))

static void row_is(int row, const char *s)
{
	char buf[T_MAXCOLS + 1];

	t_row(&T, row, buf, sizeof(buf));
	CHECK_STR(buf, s);
}

static void test_dcs_basic(void)
{
	setup();
	t_in(&T, "\033Pq#0;2;100;0;0~~\033\\");
	CHECK_INT(R.starts, 1);
	CHECK_INT(R.info.kind, TSM_VTE_STR_DCS);
	CHECK_INT(R.info.final, 'q');
	CHECK_INT(R.info.flags, 0);
	CHECK_INT(R.info.argc, 1);
	CHECK_INT(R.argv[0], -1);
	data_is("#0;2;100;0;0~~");
	CHECK_INT(R.ends, 1);
	CHECK_INT(R.done, 1);
	t_in(&T, "ok");
	row_is(0, "ok");
	teardown();

	setup();
	t_in(&T, "\033P0;1;0qx\033\\");
	CHECK_INT(R.info.argc, 3);
	CHECK_INT(R.argv[0], 0);
	CHECK_INT(R.argv[1], 1);
	CHECK_INT(R.argv[2], 0);
	data_is("x");
	teardown();

	setup();
	t_in(&T, "\033P9;;3qx\033\\");
	CHECK_INT(R.info.argc, 3);
	CHECK_INT(R.argv[0], 9);
	CHECK_INT(R.argv[1], -1);
	CHECK_INT(R.argv[2], 3);
	teardown();

	setup();
	t_in(&T, "\033P>|x\033\\");
	CHECK_INT(R.info.flags, TSM_VTE_STR_PRIVATE);
	CHECK_INT(R.info.final, '|');
	data_is("x");
	CHECK_INT(R.done, 1);
	teardown();

	setup();
	t_in(&T, "\033P$qm\033\\");
	CHECK_INT(R.info.flags, TSM_VTE_STR_INTERMEDIATE);
	CHECK_INT(R.info.final, 'q');
	data_is("m");
	teardown();

	setup();
	t_in(&T, "\033P?1$qm\033\\");
	CHECK_INT(R.info.flags,
		  TSM_VTE_STR_INTERMEDIATE | TSM_VTE_STR_PRIVATE);
	CHECK_INT(R.argv[0], 1);
	teardown();

	/* flags do not leak into the next string */
	setup();
	t_in(&T, "\033P>|x\033\\\033Pqy\033\\");
	CHECK_INT(R.starts, 2);
	CHECK_INT(R.info.flags, 0);
	CHECK_INT(R.info.final, 'q');
	teardown();
}

static void test_terminators(void)
{
	setup();
	t_in(&T, "\033Pqab\xc2\x9c" "Z");
	CHECK_INT(R.done, 1);
	data_is("ab");
	row_is(0, "Z");
	teardown();

	/* CAN aborts, and the text after it prints */
	setup();
	t_in(&T, "\033Pqab\030Y");
	CHECK_INT(R.starts, 1);
	CHECK_INT(R.aborts, 1);
	CHECK_INT(R.done, 0);
	/* data still held back when the string is cut is dropped */
	data_is("");
	row_is(0, "Y");
	t_in(&T, "\033\\");
	CHECK_INT(R.ends, 1);
	teardown();

	/* SUB aborts and still prints the error symbol */
	setup();
	t_in(&T, "\033Pqab\032Y");
	CHECK_INT(R.aborts, 1);
	row_is(0, "?Y");
	{
		static struct tgrid g;

		t_grab(&T, &g);
		CHECK_INT(g.ch[0][0], 0xbf);
	}
	teardown();

	/* ESC [ aborts and the CSI is parsed */
	setup();
	t_in(&T, "hello\033Pqab\033[2J");
	CHECK_INT(R.aborts, 1);
	row_is(0, "");
	t_in(&T, "x");
	teardown();

	setup();
	t_in(&T, "\033Pqab\033\033\\ok");
	CHECK_INT(R.starts, 1);
	CHECK_INT(R.aborts, 1);
	row_is(0, "ok");
	teardown();

	/* ESC P and ESC _ inside a string start the next one */
	setup();
	t_in(&T, "\033Pqa\033Pqb\033\\\033\\");
	CHECK_INT(R.starts, 2);
	CHECK_STR(R.log, "SASD");
	data_is("b");
	row_is(0, "");
	teardown();

	setup();
	t_in(&T, "\033_Ga\033_Gb\033\\");
	CHECK_INT(R.starts, 2);
	CHECK_STR(R.log, "SASD");
	teardown();

	/* what timg sends */
	setup();
	t_in(&T, "\033Pq\033Pq\"1;1;64;48~~\033\\\033\\");
	CHECK_INT(R.starts, 2);
	CHECK_STR(R.log, "SASD");
	CHECK_INT(R.info.argc, 1);
	row_is(0, "");
	t_in(&T, "ok");
	row_is(0, "ok");
	teardown();

	/* another C1 aborts: here OSC and CSI */
	setup();
	t_in(&T, "\033Pqab\xc2\x9d" "0;t\007");
	CHECK_STR(R.log, "SA");
	teardown();

	setup();
	t_in(&T, "\033Pqab\xc2\x9b" "2J");
	CHECK_STR(R.log, "SA");
	teardown();
}

static void test_apc(void)
{
	setup();
	t_in(&T, "\033_Ga=q;AAAA\033\\");
	CHECK_INT(R.starts, 1);
	CHECK_INT(R.info.kind, TSM_VTE_STR_APC);
	CHECK_INT(R.info.final, 0);
	CHECK_INT(R.info.argc, 0);
	CHECK_INT(R.info.flags, 0);
	data_is("Ga=q;AAAA");
	CHECK_INT(R.done, 1);
	teardown();

	/* PM and SOS are not delivered */
	setup();
	t_in(&T, "\033^x\033\\\033Xy\033\\ok");
	CHECK_INT(R.starts, 0);
	CHECK_INT(R.puts, 0);
	row_is(0, "ok");
	teardown();

	setup();
	t_in(&T, "\xc2\x9f" "Gx\xc2\x9c");
	CHECK_INT(R.info.kind, TSM_VTE_STR_APC);
	data_is("Gx");
	CHECK_INT(R.done, 1);
	teardown();

	setup();
	t_in(&T, "\xc2\x98" "x\xc2\x9c" "ok");
	CHECK_INT(R.starts, 0);
	row_is(0, "ok");
	teardown();

	/* an APC does not inherit DCS parameters */
	setup();
	t_in(&T, "\033P1;2;3qx\033\\\033_Gy\033\\");
	CHECK_INT(R.info.kind, TSM_VTE_STR_APC);
	CHECK_INT(R.info.argc, 0);
	CHECK_INT(R.info.flags, 0);
	teardown();
}

static char *long_apc(size_t body, size_t *len)
{
	char *s = malloc(body + 8);
	size_t i, n = 0;

	s[n++] = 033;
	s[n++] = '_';
	for (i = 0; i < body; i++)
		s[n++] = 'A' + i % 26;
	s[n++] = 033;
	s[n++] = '\\';
	*len = n;
	return s;
}

static void check_long(const char *s, size_t len, size_t body,
		       const size_t *pieces, int npieces, size_t step)
{
	size_t i, off = 0, n;

	setup();
	for (i = 0; i < (size_t)npieces; i++) {
		n = pieces[i] < len - off ? pieces[i] : len - off;
		t_inl(&T, s + off, n);
		off += n;
	}
	while (off < len) {
		n = step < len - off ? step : len - off;
		t_inl(&T, s + off, n);
		off += n;
	}
	CHECK_INT(R.starts, 1);
	CHECK_INT(R.done, 1);
	CHECK_INT(R.ends, 1);
	CHECK_INT(R.dlen, body);
	CHECK(R.dlen == body && !memcmp(R.data, s + 2, body));
	CHECK(R.minput >= 1 && R.maxput <= 256);
	/* every batch but the last is full */
	CHECK_INT(R.puts, (body + 255) / 256);
	teardown();
}

static void test_long(void)
{
	static const size_t splits[] = { 1, 255, 256, 257, 684, 2048, 4096 };
	size_t len, body = 27168, i, one[1];
	char *s = long_apc(body, &len);

	check_long(s, len, body, NULL, 0, len);
	for (i = 0; i < sizeof(splits) / sizeof(splits[0]); i++) {
		one[0] = splits[i];
		check_long(s, len, body, one, 1, len);
	}
	check_long(s, len, body, NULL, 0, 1);
	check_long(s, len, body, NULL, 0, 7);
	check_long(s, len, body, NULL, 0, 256);
	free(s);

	/* the same around the batch size */
	for (body = 254; body <= 258; body++) {
		s = long_apc(body, &len);
		check_long(s, len, body, NULL, 0, len);
		check_long(s, len, body, NULL, 0, 1);
		free(s);
	}
	s = long_apc(512, &len);
	check_long(s, len, 512, NULL, 0, len);
	free(s);
}

static void test_put_false(void)
{
	size_t len;
	char *s = long_apc(600, &len);

	setup();
	R.fail_put_at = 2;
	t_inl(&T, s, len - 2);
	CHECK_INT(R.starts, 1);
	CHECK_INT(R.puts, 2);
	CHECK_INT(R.ends, 1);
	CHECK_INT(R.aborts, 1);
	t_in(&T, "\033\\");
	t_in(&T, "after");
	CHECK_INT(R.puts, 2);
	CHECK_INT(R.ends, 1);
	row_is(0, "after");
	teardown();

	/* the failing batch is the last one, at ST */
	setup();
	R.fail_put_at = 1;
	t_in(&T, "\033_Gabc\033\\");
	CHECK_STR(R.log, "SA");
	t_in(&T, "ok");
	row_is(0, "ok");
	teardown();

	/* DCS: same, byte by byte */
	setup();
	R.fail_put_at = 2;
	{
		size_t i;

		t_in(&T, "\033Pq");
		for (i = 0; i < 520; i++)
			t_in(&T, "~");
		t_in(&T, "\033\\more");
	}
	CHECK_INT(R.puts, 2);
	CHECK_INT(R.aborts, 1);
	CHECK_INT(R.ends, 1);
	row_is(0, "more");
	teardown();
	free(s);
}

static void test_not_taken(void)
{
	/* start returns false: no put, no end, data skipped */
	setup();
	R.start_ret = false;
	t_in(&T, "\033Pqab\033\\ok");
	CHECK_INT(R.starts, 1);
	CHECK_INT(R.puts, 0);
	CHECK_INT(R.ends, 0);
	row_is(0, "ok");
	t_in(&T, "\033_Gab\033\\X");
	CHECK_INT(R.starts, 2);
	CHECK_INT(R.puts, 0);
	CHECK_INT(R.ends, 0);
	row_is(0, "okX");
	t_in(&T, "\033Pqab\xc2\x9c" "Y");
	row_is(0, "okXY");
	teardown();

	/* a refused string ended by CAN: text goes on */
	setup();
	R.start_ret = false;
	t_in(&T, "\033Pqab\030Y");
	row_is(0, "Y");
	CHECK_INT(R.ends, 0);
	teardown();

	/* no ops */
	setup();
	tsm_vte_set_str_ops(T.vte, NULL, NULL);
	t_in(&T, "\033Pqab\033\\ok\033_Gx\033\\!");
	CHECK_INT(R.starts, 0);
	row_is(0, "ok!");
	teardown();

	/* a UTF-8 sequence cut by a broken one does not leak into the data */
	setup();
	t_in(&T, "\033Pq\xc3");
	t_in(&T, "ab\033\\");
	data_is("b");
	teardown();

	/* control characters and non-ASCII data inside the string */
	setup();
	t_in(&T, "\033Pqa\nb\r\007\177\xc3\xa9" "c\033\\");
	data_is("abc");
	CHECK_INT(R.done, 1);
	CHECK_INT(tsm_screen_get_cursor_y(T.scr), 0);
	CHECK_INT(tsm_screen_get_cursor_x(T.scr), 0);
	teardown();
}

static void test_reset(void)
{
	setup();
	t_in(&T, "\033Pqab");
	tsm_vte_reset(T.vte);
	CHECK_STR(R.log, "SA");
	t_in(&T, "xyz");
	row_is(0, "xyz");
	CHECK_INT(R.puts, 0);
	teardown();

	setup();
	t_in(&T, "\033_Gab");
	tsm_vte_hard_reset(T.vte);
	CHECK_STR(R.log, "SA");
	t_in(&T, "xyz");
	row_is(0, "xyz");
	teardown();

	/* ESC c inside a string: aborted by the ESC, then RIS */
	setup();
	t_in(&T, "\033_Gab\033c");
	CHECK_STR(R.log, "SA");
	teardown();

	/* reset after the ESC of the terminator */
	setup();
	t_in(&T, "\033_Gab\033");
	tsm_vte_reset(T.vte);
	CHECK_STR(R.log, "SA");
	t_in(&T, "\\x");
	row_is(0, "\\x");
	teardown();

	/* freeing the vte ends an open string */
	setup();
	t_in(&T, "\033_Gab");
	teardown();
	CHECK_STR(R.log, "SA");

	/* replacing the ops ends the string for the old ones */
	setup();
	t_in(&T, "\033_Gab");
	tsm_vte_set_str_ops(T.vte, NULL, NULL);
	CHECK_STR(R.log, "SA");
	t_in(&T, "cd\033\\ok");
	row_is(0, "ok");
	CHECK_INT(R.ends, 1);
	teardown();
}

int main(void)
{
	test_dcs_basic();
	test_terminators();
	test_apc();
	test_long();
	test_put_false();
	test_not_taken();
	test_reset();
	return t_summary("test-vte-str");
}
