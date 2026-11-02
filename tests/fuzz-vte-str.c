/*
 * libFuzzer harness for the vte string interface and, with HAVOC_SIXEL, the
 * picture tiles. The input is cut into chunks by its first byte and fed to a
 * vte whose consumer gives up after a number of puts chosen by the second
 * byte. Every start that returned true must get exactly one end.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tsm/libtsm.h"

struct ctx {
	int open;
	int fail_after;	/* puts until put returns false, -1: never */
	int puts;
	int refuse;	/* start returns false */
};

static void die(const char *msg)
{
	fprintf(stderr, "fuzz-vte-str: %s\n", msg);
	abort();
}

static bool f_start(void *data, const struct tsm_vte_str_info *info)
{
	struct ctx *c = data;

	if (c->open)
		die("start inside an open string");
	if (info->kind == TSM_VTE_STR_DCS && (info->argc < 1 || info->argc > 16))
		die("bad argc");
	if (info->kind == TSM_VTE_STR_APC && info->argc != 0)
		die("APC with parameters");
	c->puts = 0;
	if (c->refuse)
		return false;
	c->open = 1;
	return true;
}

static bool f_put(void *data, const char *buf, size_t len)
{
	struct ctx *c = data;
	size_t i;

	if (!c->open)
		die("put outside a string");
	if (!len || len > 256)
		die("bad put size");
	for (i = 0; i < len; i++)
		if ((unsigned char)buf[i] < 0x20 || (unsigned char)buf[i] > 0x7e)
			die("put with a byte outside 0x20 to 0x7e");
	c->puts++;
	return c->fail_after < 0 || c->puts <= c->fail_after;
}

static void f_end(void *data, enum tsm_vte_str_end how)
{
	struct ctx *c = data;

	if (!c->open)
		die("end without start");
	c->open = 0;
}

static const struct tsm_vte_str_ops ops = { f_start, f_put, f_end };

static void write_cb(struct tsm_vte *vte, const char *u8, size_t len,
		     void *data)
{
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct tsm_screen *scr;
	struct tsm_vte *vte;
	struct ctx c = { 0 };
	struct tsm_vte_graphics g = { true, 6, 10, 256, "0.7.0-git" };
	size_t chunk, off;
	unsigned int mode;
	int step = 0;

	if (size < 3)
		return 0;
	chunk = data[0] ? data[0] : size;
	c.fail_after = data[1] < 128 ? -1 : data[1] - 128;
	mode = data[2];
	c.refuse = !(mode & 0x40) && (mode & 0x80);
	data += 3;
	size -= 3;

	tsm_screen_new(&scr);
	tsm_vte_new(&vte, scr, write_cb, NULL);
	tsm_screen_resize(scr, 40, 12);
	tsm_screen_set_max_sb(scr, 20);
	tsm_vte_set_str_ops(vte, &ops, &c);
	if (mode & 1)
		tsm_vte_set_graphics(vte, &g);

	for (off = 0; off < size; off += chunk, step++) {
		size_t n = size - off < chunk ? size - off : chunk;

		tsm_vte_input(vte, (const char *)data + off, n);
		if (mode & 2 && step % 5 == 4)
			tsm_vte_reset(vte);
		if (mode & 4 && step % 7 == 6)
			tsm_screen_resize(scr, 20 + step % 30, 6 + step % 10);
#if HAVOC_SIXEL
		if (mode & 8) {
			struct tsm_screen_attr a;
			uint8_t used[TSM_PIC_SETS][32];
			uint8_t b = data[off];

			tsm_vte_get_attr(vte, &a);
			tsm_screen_place_picture(scr, &a, 1 + b % 255,
						 1 + b % 60, 1 + b % 20,
						 b % 25 - 2, b & 1);
			tsm_screen_pictures_used(scr, used);
			if (b & 2)
				tsm_screen_drop_picture(scr, 1 + b % 255);
		}
#endif
		if (mode & 0x10 && step % 3 == 2)
			tsm_vte_set_str_ops(vte, step % 2 ? &ops : NULL, &c);
	}

	tsm_vte_unref(vte);
	if (c.open)
		die("string still open after the vte is freed");
	tsm_screen_unref(scr);
	return 0;
}
