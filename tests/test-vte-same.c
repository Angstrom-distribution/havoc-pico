/*
 * Feed streams to a vte with no string ops and no graphics set and print the
 * screen, the scrollback and the replies. run-tsm.sh builds this against the
 * libtsm of the branch and against the one of pico and compares the output.
 * It uses only the interface that pico has.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tsm/libtsm.h"

#define COLS 40
#define ROWS 12

struct sink {
	unsigned char *buf;
	size_t len, cap;
	uint64_t hash;
	int row;
	char text[COLS + 1];
};

static void sink_add(struct sink *s, const void *p, size_t n)
{
	if (s->len + n > s->cap) {
		s->cap = (s->len + n) * 2 + 64;
		s->buf = realloc(s->buf, s->cap);
	}
	memcpy(s->buf + s->len, p, n);
	s->len += n;
}

static void write_cb(struct tsm_vte *vte, const char *u8, size_t len,
		     void *data)
{
	sink_add(data, u8, len);
}

static void mix(uint64_t *h, uint64_t v)
{
	*h = (*h ^ v) * 0x100000001b3ULL;
}

static void draw_cb(struct tsm_screen *con, uint32_t id, const uint32_t *ch,
		    size_t len, int width, int x, int y,
		    const struct tsm_screen_attr *a, tsm_age_t age, void *data)
{
	uint64_t *h = data;
	size_t i;

	mix(h, x);
	mix(h, y);
	mix(h, width);
	mix(h, len);
	for (i = 0; i < len; i++)
		mix(h, ch[i]);
	mix(h, (uint16_t)a->fccode);
	mix(h, (uint16_t)a->bccode);
	mix(h, a->fr << 16 | a->fg << 8 | a->fb);
	mix(h, a->br << 16 | a->bg << 8 | a->bb);
	mix(h, a->bold | a->underline << 1 | a->inverse << 2 |
		a->protect << 3 | a->blink << 4);
}

static void dump_view(struct tsm_screen *scr, int page)
{
	uint64_t h = 0xcbf29ce484222325ULL;

	tsm_screen_draw(scr, draw_cb, &h);
	printf(" view%d %016llx cursor %d,%d\n", page, (unsigned long long)h,
	       tsm_screen_get_cursor_x(scr), tsm_screen_get_cursor_y(scr));
}

static void run(const char *name, const unsigned char *in, size_t len,
		size_t piece)
{
	struct tsm_screen *scr;
	struct tsm_vte *vte;
	struct sink sink = { 0 };
	size_t off, n, i;

	tsm_screen_new(&scr);
	tsm_vte_new(&vte, scr, write_cb, &sink);
	tsm_screen_resize(scr, COLS, ROWS);
	tsm_screen_set_max_sb(scr, 200);
	tsm_vte_hard_reset(vte);

	for (off = 0; off < len; off += n) {
		n = piece && piece < len - off ? piece : len - off;
		tsm_vte_input(vte, (const char *)in + off, n);
	}

	printf("%s piece %zu replies", name, piece);
	for (i = 0; i < sink.len; i++)
		printf(" %02x", sink.buf[i]);
	printf("\n");
	dump_view(scr, 0);
	for (i = 1; i < 40; i++) {
		tsm_screen_sb_up(scr, ROWS);
		dump_view(scr, i);
	}

	free(sink.buf);
	tsm_vte_unref(vte);
	tsm_screen_unref(scr);
}

static void run_all(const char *name, const unsigned char *in, size_t len)
{
	run(name, in, len, 0);
	run(name, in, len, 1);
	run(name, in, len, 7);
}

static uint32_t rnd_state = 12345;

static uint32_t rnd(void)
{
	rnd_state = rnd_state * 1103515245 + 12345;
	return rnd_state >> 8;
}

static void add(char **buf, size_t *len, const char *s)
{
	size_t n = strlen(s);

	*buf = realloc(*buf, *len + n + 1);
	memcpy(*buf + *len, s, n);
	*len += n;
}

static void generated(void)
{
	static const char *snippets[] = {
		"hello world\r\n", "\033[1;31mred\033[0m ", "\033[44mblue\033[m",
		"\033[38;2;1;2;3mtc\033[m", "\033P1;2;3qabc~~\033\\",
		"\033P>|x\033\\", "\033Pq#0;2;100;0;0~~-\033\\",
		"\033_Ga=T,f=24;AAAA\033\\", "\033^pm\033\\", "\033Xsos\033\\",
		"\xc2\x90qdata\xc2\x9c", "\xc2\x9f" "Gx\xc2\x9c",
		"\xc2\x98sos\xc2\x9c", "\xc2\x9ePM\xc2\x9c",
		"\033Pqab\030cd", "\033Pqab\032cd", "\033_Gab\030cd",
		"\033Pq\033Pqx\033\\\033\\", "\033_Ga\033[2J", "\033Pqa\033c",
		"\xc3\xa9\xe4\xb8\x96\xf0\x9f\x98\x80 ", "\033]0;title\007",
		"\033[?1049h", "\033[?1049l", "\033[?8452h", "\033[?8452l",
		"\033[3J", "\033[?1;1;0S", "\033[14t", "\033[>q", "\033[c",
		"\033[>c", "\033[5n", "\033[6n", "\033[2J", "\033[H",
		"\033[5;10r", "\033[r", "\033[3S", "\033[2T", "\033[4L",
		"\033[2M", "\033[3P", "\033[3@", "\033[2X", "\033[K",
		"\033M", "\033D", "\033E", "\033c", "\t", "\b", "\a", "\x7f",
		"\n\n\n\n\n\n\n\n\n\n\n\n\n\n", "\033[?7l", "\033[?7h",
	};
	char *buf = NULL;
	size_t len = 0, i;
	char name[64], tmp[32];
	int round, f;

	/* text with controls */
	for (round = 0; round < 3; round++) {
		buf = NULL;
		len = 0;
		for (i = 0; i < 300; i++)
			add(&buf, &len, snippets[rnd() % (sizeof(snippets) /
							  sizeof(snippets[0]))]);
		snprintf(name, sizeof(name), "snippets%d", round);
		run_all(name, (unsigned char *)buf, len);
		free(buf);
	}

	/* every final byte, with and without the markers */
	buf = NULL;
	len = 0;
	for (f = 0x40; f <= 0x7e; f++) {
		static const char *pre[] = { "", "?", ">", "=", "<", "!", "$",
					     "?1;", "1;2;3", "0", "14", "3" };
		size_t k;

		for (k = 0; k < sizeof(pre) / sizeof(pre[0]); k++) {
			snprintf(tmp, sizeof(tmp), "\033[%s%c", pre[k], f);
			add(&buf, &len, tmp);
			add(&buf, &len, "abc");
		}
	}
	run_all("csi-finals", (unsigned char *)buf, len);
	free(buf);

	/* every C1 control inside UTF-8, alone and inside a string */
	buf = NULL;
	len = 0;
	for (f = 0x80; f <= 0x9f; f++) {
		snprintf(tmp, sizeof(tmp), "a\xc2%cb", f);
		add(&buf, &len, tmp);
		snprintf(tmp, sizeof(tmp), "\033Pqxy\xc2%czw\033\\", f);
		add(&buf, &len, tmp);
		snprintf(tmp, sizeof(tmp), "\033_Gxy\xc2%czw\033\\", f);
		add(&buf, &len, tmp);
		add(&buf, &len, "\033\\ok\n");
	}
	run_all("c1", (unsigned char *)buf, len);
	free(buf);

	/* random bytes, and random bytes from the escape alphabet */
	for (round = 0; round < 6; round++) {
		static const char alpha[] = "\033\033\033[]PX^_\\qG;?>0123456789"
					    "abcdefgmnrsthJKLMSTc@ \030\032\n"
					    "\xc2\x9c\xc2\x9b\xc3\xa9";
		unsigned char *r = malloc(20000);

		for (i = 0; i < 20000; i++)
			r[i] = round < 3 ? rnd() : alpha[rnd() % (sizeof(alpha) - 1)];
		snprintf(name, sizeof(name), "random%d", round);
		run_all(name, r, 20000);
		free(r);
	}
}

static void vectors(void)
{
	const char *dir = getenv("VECTORS");
	DIR *d;
	struct dirent *e;
	char path[1024];
	char *names[512];
	int n = 0, i, j;

	if (!dir)
		dir = "/s/overnight-20261101/havoc-graphics/vectors";
	d = opendir(dir);
	if (!d) {
		fprintf(stderr, "test-vte-same: no vectors in %s, skipped\n",
			dir);
		return;
	}
	while ((e = readdir(d)) && n < 512) {
		size_t l = strlen(e->d_name);

		if (l > 4 && !strcmp(e->d_name + l - 4, ".bin"))
			names[n++] = strdup(e->d_name);
	}
	closedir(d);
	/* directory order differs between runs of the two builds */
	for (i = 0; i < n; i++)
		for (j = i + 1; j < n; j++)
			if (strcmp(names[i], names[j]) > 0) {
				char *t = names[i];

				names[i] = names[j];
				names[j] = t;
			}
	for (i = 0; i < n; i++) {
		FILE *f;
		unsigned char *buf;
		long len;

		snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
		f = fopen(path, "rb");
		if (!f)
			continue;
		fseek(f, 0, SEEK_END);
		len = ftell(f);
		fseek(f, 0, SEEK_SET);
		buf = malloc(len + 1);
		if (fread(buf, 1, len, f) == (size_t)len)
			run_all(names[i], buf, len);
		fclose(f);
		free(buf);
		free(names[i]);
	}
	fprintf(stderr, "test-vte-same: %d vector files\n", n);
}

int main(void)
{
	generated();
	vectors();
	return 0;
}
