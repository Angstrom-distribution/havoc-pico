/* The pictures of a terminal window: what the decoders produce is kept in
 * slots, cells of the screen refer to them (libtsm's pic field) and the
 * total stays within a pixel count. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "graphics.h"

#if HAVE_GRAPHICS

#if HAVOC_SIXEL
#include "sixel.h"
#endif

#define NSLOTS 256	/* slot 0 means no picture */

#if HAVOC_SIXEL
struct slot {
	unsigned int w, h;	/* picture pixels */
	unsigned int cw0, ch0;	/* cell size when it was placed */
	uint8_t *pix;		/* w * h register numbers */
	uint8_t *mask;		/* bit set: pixel set; NULL: all set */
	uint32_t pal[256];	/* 0x00RRGGBB */
	uint32_t gen;		/* differs for every picture */
	uint32_t last_use;
};
#endif

struct gfx {
	struct tsm_screen *scr;
	struct tsm_vte *vte;
	struct gfx_config cfg;
	bool debug;
	int cw, ch, cols, rows;
#if HAVOC_SIXEL
	struct slot *slot[NSLOTS];
	size_t pixels;		/* stored by all slots */
	uint32_t gen, use;
	struct sixel *dec;	/* the string in progress */
#endif
};

#if HAVOC_SIXEL

static bool in_set(const uint8_t *set, unsigned int slot)
{
	return set[slot / 8] >> (slot % 8) & 1;
}

static int slot_count(const struct gfx *g)
{
	int i, n = 0;

	for (i = 1; i < NSLOTS; i++)
		n += g->slot[i] != NULL;
	return n;
}

static void free_slot(struct gfx *g, unsigned int i)
{
	struct slot *s = g->slot[i];

	if (!s)
		return;
	g->pixels -= (size_t)s->w * s->h;
	free(s->pix);
	free(s->mask);
	free(s);
	g->slot[i] = NULL;
}

/* A slot no cell refers to anymore. */
static void sweep(struct gfx *g, uint8_t used[TSM_PIC_SETS][32])
{
	unsigned int i;

	tsm_screen_pictures_used(g->scr, used);
	for (i = 1; i < NSLOTS; i++)
		if (g->slot[i] && !in_set(used[TSM_PIC_MAIN], i) &&
		    !in_set(used[TSM_PIC_ALT], i) &&
		    !in_set(used[TSM_PIC_SB], i))
			free_slot(g, i);
}

static void drop_slot(struct gfx *g, unsigned int i)
{
	tsm_screen_drop_picture(g->scr, i);
	free_slot(g, i);
}

static unsigned int free_index(const struct gfx *g)
{
	unsigned int i;

	for (i = 1; i < NSLOTS; i++)
		if (!g->slot[i])
			return i;
	return 0;
}

/* Room for one more picture of n pixels and a slot for it: first the slots
 * no cell refers to, then the oldest ones that are not drawn now. A picture
 * in view is never given up. */
static bool make_room(struct gfx *g, size_t n)
{
	uint8_t used[TSM_PIC_SETS][32];
	size_t cap = g->cfg.max_pixels;

	if (n > cap)
		return false;
	if (g->pixels + n > cap || !free_index(g) || g->pixels > cap / 2)
		sweep(g, used);
	else
		return true;

	while (g->pixels + n > cap || !free_index(g)) {
		unsigned int i, old = 0;

		for (i = 1; i < NSLOTS; i++)
			if (g->slot[i] && !in_set(used[TSM_PIC_VIEW], i) &&
			    (!old || g->slot[i]->last_use <
				     g->slot[old]->last_use))
				old = i;
		if (!old)
			return false;
		drop_slot(g, old);
	}
	return true;
}

/* Pixels of the pictures that have a tile on the screen now. */
static size_t pixels_in_view(struct gfx *g)
{
	uint8_t used[TSM_PIC_SETS][32];
	size_t n = 0;
	unsigned int i;

	tsm_screen_pictures_used(g->scr, used);
	for (i = 1; i < NSLOTS; i++)
		if (g->slot[i] && in_set(used[TSM_PIC_VIEW], i))
			n += (size_t)g->slot[i]->w * g->slot[i]->h;
	return n;
}

static bool str_start(void *data, const struct tsm_vte_str_info *info)
{
	struct gfx *g = data;
	struct sixel_limits lim;
	size_t view;
	int x, avail;

	if (info->kind != TSM_VTE_STR_DCS || info->final != 'q' ||
	    info->flags || !g->cfg.sixel || g->cw <= 0 || g->ch <= 0)
		return false;

	/* the columns the picture may take: the text area right of the
	 * cursor, all of it when a wrap is pending */
	x = tsm_screen_get_cursor_x(g->scr);
	avail = tsm_screen_get_width(g->scr);
	if (x >= avail)
		avail = tsm_screen_get_flags(g->scr) & TSM_SCREEN_AUTO_WRAP ?
			avail : 1;
	else
		avail -= x;
	lim.max_w = (unsigned int)avail * g->cw;
	if (lim.max_w > SIXEL_MAX_SIDE)
		lim.max_w = SIXEL_MAX_SIDE;

	/* whatever is not in view can be given up for this picture */
	view = pixels_in_view(g);
	if (view >= g->cfg.max_pixels)
		return false;
	lim.max_pixels = g->cfg.max_pixels - view;
	if (lim.max_pixels < lim.max_w)
		return false;

	sixel_free(g->dec);
	g->dec = sixel_new(info->argc, info->argv, &lim);
	return g->dec != NULL;
}

static bool str_put(void *data, const char *buf, size_t len)
{
	struct gfx *g = data;

	if (!g->dec)
		return false;
	sixel_put(g->dec, buf, len);
	return true;
}

static void place(struct gfx *g, struct sixel_image *img)
{
	struct tsm_screen_attr attr;
	struct slot *s;
	unsigned int i, cols, rows;
	unsigned int flags = 0;
	size_t n = (size_t)img->w * img->h;

	if (!make_room(g, n) || !(s = malloc(sizeof(*s)))) {
		free(img->pix);
		free(img->mask);
		return;
	}
	i = free_index(g);
	s->w = img->w;
	s->h = img->h;
	s->cw0 = g->cw;
	s->ch0 = g->ch;
	s->pix = img->pix;
	s->mask = img->mask;
	memcpy(s->pal, img->pal, sizeof(s->pal));
	s->gen = ++g->gen;
	s->last_use = ++g->use;
	g->slot[i] = s;
	g->pixels += n;

	cols = (img->w + g->cw - 1) / g->cw;
	rows = (img->h + g->ch - 1) / g->ch;
	if (tsm_vte_get_graphics_flags(g->vte) & TSM_VTE_SIXEL_CURSOR_RIGHT)
		flags |= TSM_PIC_CURSOR_RIGHT;
	tsm_vte_get_attr(g->vte, &attr);
	/* the cursor ends on the last row the picture covers */
	if (tsm_screen_place_picture(g->scr, &attr, i, cols, rows, rows - 1,
				     flags) < 0)
		free_slot(g, i);
}

static void str_end(void *data, enum tsm_vte_str_end how)
{
	struct gfx *g = data;
	struct sixel_image img;
	int ok;

	if (!g->dec)
		return;
	ok = how == TSM_VTE_STR_DONE && sixel_finish(g->dec, &img) == 0;
	sixel_free(g->dec);
	g->dec = NULL;
	if (!ok)
		return;
	/* the height limit cut it: the picture is larger than the room there
	 * is, and half a picture says nothing */
	if (img.truncated) {
		free(img.pix);
		free(img.mask);
	} else {
		place(g, &img);
	}
	gfx_debug(g);
}

static void on_event(struct tsm_screen *scr, const struct tsm_screen_event *ev,
		     void *data)
{
	struct gfx *g = data;
	uint8_t used[TSM_PIC_SETS][32];
	size_t before = g->pixels;
	unsigned int i;

	switch (ev->type) {
	case TSM_SCREEN_EV_ALTERNATE:
		/* the last frame of a video must not stay behind the main
		 * screen */
		if (ev->alt)
			return;
		tsm_screen_pictures_used(scr, used);
		for (i = 1; i < NSLOTS; i++)
			if (g->slot[i] && in_set(used[TSM_PIC_ALT], i) &&
			    !in_set(used[TSM_PIC_MAIN], i) &&
			    !in_set(used[TSM_PIC_SB], i))
				drop_slot(g, i);
		break;
	case TSM_SCREEN_EV_ERASE_SCREEN:
	case TSM_SCREEN_EV_CLEAR_SB:
	case TSM_SCREEN_EV_RESET:
		sweep(g, used);
		break;
	default:
		return;
	}
	if (g->pixels != before)
		gfx_debug(g);
}

uint32_t gfx_pic_gen(const struct gfx *g, uint32_t pic)
{
	const struct slot *s = g ? g->slot[TSM_PIC_SLOT(pic)] : NULL;

	return s ? s->gen : 0;
}

void gfx_paint(struct gfx *g, enum gfx_layer layer, uint32_t pic,
	       const struct gfx_target *t)
{
	const struct slot *s = g->slot[TSM_PIC_SLOT(pic)];
	uint32_t fx, fy, ay = 0;
	unsigned int x0, y0;
	int px, py;

	if (!s || g->cw <= 0 || g->ch <= 0)
		return;

	/* source pixels per target pixel, 16.16; once per cell */
	fx = (s->cw0 << 16) / g->cw;
	fy = (s->ch0 << 16) / g->ch;
	x0 = TSM_PIC_COL(pic) * s->cw0;
	y0 = TSM_PIC_ROW(pic) * s->ch0;

	for (py = 0; py < t->h; py++, ay += fy) {
		unsigned int sy = y0 + (ay >> 16);
		const uint8_t *row;
		size_t base;
		uint32_t ax = 0;
		uint32_t *dst = t->px + (size_t)py * t->stride;

		/* below the picture: the last tile is partly empty */
		if (sy >= s->h)
			break;
		base = (size_t)sy * s->w;
		row = s->pix + base;
		for (px = 0; px < t->w; px++, ax += fx) {
			unsigned int sx = x0 + (ax >> 16);
			size_t bit = base + sx;

			if (sx >= s->w)
				break;
			if (s->mask && !(s->mask[bit >> 3] >> (bit & 7) & 1))
				continue;
			dst[px] = 0xff000000 | s->pal[row[sx]];
		}
	}
}

#endif /* HAVOC_SIXEL */

void gfx_debug(const struct gfx *g)
{
#if HAVOC_SIXEL
	if (g && g->debug)
		fprintf(stderr, "gfx: sixel=%d kitty=0 places=0+0 pixels=%zu/%zu\n",
			slot_count(g), g->pixels, g->cfg.max_pixels);
#else
	(void)g;
#endif
}

struct gfx *gfx_new(struct tsm_screen *scr, struct tsm_vte *vte,
		    const struct gfx_config *cfg)
{
#if HAVOC_SIXEL
	static const struct tsm_vte_str_ops ops = {
		.start = str_start,
		.put = str_put,
		.end = str_end,
	};
	struct gfx *g;

	if (!cfg->sixel)
		return NULL;
	g = calloc(1, sizeof(*g));
	if (!g)
		return NULL;
	g->scr = scr;
	g->vte = vte;
	g->cfg = *cfg;
	g->debug = getenv("HAVOC_GFX_DEBUG") != NULL;
	tsm_vte_set_str_ops(vte, &ops, g);
	tsm_screen_set_event_cb(scr, on_event, g);
	return g;
#else
	(void)scr;
	(void)vte;
	(void)cfg;
	return NULL;
#endif
}

void gfx_free(struct gfx *g)
{
#if HAVOC_SIXEL
	unsigned int i;

	if (!g)
		return;
	/* ends a string in progress while g->dec is still there */
	tsm_vte_set_str_ops(g->vte, NULL, NULL);
	tsm_screen_set_event_cb(g->scr, NULL, NULL);
	sixel_free(g->dec);
	for (i = 1; i < NSLOTS; i++)
		free_slot(g, i);
	free(g);
#else
	(void)g;
#endif
}

bool gfx_enabled(const struct gfx *g)
{
	return g != NULL;
}

void gfx_set_geometry(struct gfx *g, int cw, int ch, int cols, int rows)
{
	struct tsm_vte_graphics vg;

	if (!g)
		return;
	g->cw = cw;
	g->ch = ch;
	g->cols = cols;
	g->rows = rows;
	memset(&vg, 0, sizeof(vg));
	vg.sixel = g->cfg.sixel;
	vg.cell_w = cw;
	vg.cell_h = ch;
	vg.registers = 256;
	vg.version = VERSION;
	tsm_vte_set_graphics(g->vte, &vg);
}

#endif /* HAVE_GRAPHICS */
