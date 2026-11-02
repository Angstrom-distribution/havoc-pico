/*
 * libFuzzer harness for the picture store with a real libtsm screen, vte
 * and sixel decoder (HAVOC_SIXEL=1). graphics.c is included so that the
 * harness can check the store: the pixel count is the sum of the slots and
 * within the cap, and every tile drawn names a slot that exists.
 *
 * Input: byte 0 picks the cap, byte 1 the cell size; then terminal data, in
 * which 0x01 followed by one byte is an action of the window instead (draw,
 * scroll the view, resize, zoom, reset).
 */
#include <assert.h>
#include "../graphics.c"

static uint32_t cellbuf[32 * 32];

static void write_cb(struct tsm_vte *vte, const char *u8, size_t len,
		     void *data)
{
}

static void check_store(struct gfx *g)
{
	size_t sum = 0;
	unsigned int i;

	for (i = 1; i < NSLOTS; i++)
		if (g->slot[i])
			sum += (size_t)g->slot[i]->w * g->slot[i]->h;
	if (sum != g->pixels || g->pixels > g->cfg.memory) {
		fprintf(stderr, "fuzz-gfx: pixels %zu, slots %zu, cap %zu\n",
			g->pixels, sum, g->cfg.memory);
		abort();
	}
}

static void draw_cb(struct tsm_screen *con, uint32_t id, const uint32_t *ch,
		    size_t len, int width, int x, int y,
		    const struct tsm_screen_attr *a, tsm_age_t age, void *data)
{
	struct gfx *g = data;
	struct gfx_target t;

	if (!a->pic)
		return;
	if (!g->slot[TSM_PIC_SLOT(a->pic)]) {
		fprintf(stderr, "fuzz-gfx: tile of slot %u, which is free\n",
			TSM_PIC_SLOT(a->pic));
		abort();
	}
	if (!gfx_pic_gen(g, a->pic))
		abort();
	t.px = cellbuf;
	t.stride = 32;
	t.w = g->cw;
	t.h = g->ch;
	gfx_paint(g, GFX_TILE, a->pic, &t);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct gfx_config cfg = { 0 };
	struct tsm_screen *scr;
	struct tsm_vte *vte;
	struct gfx *g;
	size_t i, run;
	int cw, ch, cols = 40, rows = 12;

	if (size < 2)
		return 0;
	cfg.sixel = true;
	cfg.memory = (size_t)data[0] * 2048;
	cw = 1 + data[1] % 16;
	ch = 1 + (data[1] >> 4) * 2;
	data += 2;
	size -= 2;

	tsm_screen_new(&scr);
	tsm_vte_new(&vte, scr, write_cb, NULL);
	tsm_screen_resize(scr, cols, rows);
	tsm_screen_set_max_sb(scr, 30);
	g = gfx_new(scr, vte, &cfg);
	assert(g);
	gfx_set_geometry(g, cw, ch, cols, rows);

	for (i = 0; i < size; ) {
		for (run = 0; i + run < size && data[i + run] != 0x01; run++)
			;
		while (run) {
			size_t n = run > 4096 ? 4096 : run;

			tsm_vte_input(vte, (const char *)data + i, n);
			i += n;
			run -= n;
		}
		check_store(g);
		if (i + 1 >= size)
			break;
		switch (data[i + 1] & 7) {
		case 0:
			tsm_screen_draw(scr, draw_cb, g);
			break;
		case 1:
			tsm_screen_sb_up(scr, 1 + (data[i + 1] >> 3));
			break;
		case 2:
			tsm_screen_sb_reset(scr);
			break;
		case 3:
			cols = 1 + (data[i + 1] >> 3) * 3;
			tsm_screen_resize(scr, cols, rows);
			gfx_set_geometry(g, cw, ch, cols, rows);
			break;
		case 4:
			rows = 1 + (data[i + 1] >> 3);
			tsm_screen_resize(scr, cols, rows);
			gfx_set_geometry(g, cw, ch, cols, rows);
			break;
		case 5:
			cw = 1 + (data[i + 1] >> 3);
			ch = 1 + (data[i + 1] >> 3);
			gfx_set_geometry(g, cw, ch, cols, rows);
			break;
		case 6:
			tsm_vte_hard_reset(vte);
			break;
		case 7:
			tsm_vte_reset(vte);
			break;
		}
		i += 2;
		tsm_screen_draw(scr, draw_cb, g);
		check_store(g);
	}

	gfx_free(g);
	tsm_vte_unref(vte);
	tsm_screen_unref(scr);
	return 0;
}
