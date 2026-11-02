/* The pictures of a terminal window. Sixel pictures are kept in slots and
 * cells of the screen refer to them (libtsm's pic field). Kitty pictures
 * are images found by id and placements on a list per screen (kplace.c);
 * a placement is not made of cells, so text does not remove it. Both count
 * against one pixel cap. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "graphics.h"

#if HAVE_GRAPHICS

#if HAVOC_SIXEL
#include "sixel.h"
#endif
#if HAVOC_KITTY
#include "kitty.h"
#include "kplace.h"
#endif

#define NSLOTS 256	/* slot 0 means no picture */
#define KPLACE_BOX_MAX 4096	/* columns or rows of one placement */
#define KIMG_MAX 1024	/* kitty images; the handle is the index + 1 */

/* After a=T or a=p the cursor goes down by the box's rows minus this */
#define KITTY_CURSOR_ROWS_BACK 0

#define Z_UNDER_BG (-1073741824)	/* z below this: under cell backgrounds too */

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

#if HAVOC_KITTY
struct kimg {
	uint32_t id;		/* i=, 0: anonymous, freed with its last placement */
	unsigned int w, h;
	uint16_t *rgb;		/* RGB565 */
	uint8_t *alpha;		/* NULL: opaque */
	int nplace;		/* placements, both screens */
	uint32_t last_use;
};
#endif

struct gfx {
	struct tsm_screen *scr;
	struct tsm_vte *vte;
	struct gfx_config cfg;
	bool debug;
	int cw, ch, cols, rows;
	size_t pixels;		/* stored by all pictures */
	uint32_t use;
#if HAVOC_SIXEL
	struct slot *slot[NSLOTS];
	uint32_t gen;
	struct sixel *dec;	/* the string in progress */
#endif
#if HAVOC_KITTY
	struct kitty *kit;
	bool apc;		/* the string in progress is kitty's */
	struct kimg *img[KIMG_MAX];
	int nimg;
	struct kplace_list pl[2];	/* main, alternate */
	bool alt;		/* the alternate screen is active */
	uint32_t serial, order;
	/* the placements in view, from gfx_frame */
	int view[KPLACE_MAX];
	int nview;
	int sb_off;
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

#endif /* HAVOC_SIXEL */

#if HAVOC_KITTY

static void kimg_free(struct gfx *g, uint32_t h)
{
	struct kimg *m = h && h <= KIMG_MAX ? g->img[h - 1] : NULL;

	if (!m)
		return;
	g->pixels -= (size_t)m->w * m->h;
	g->nimg--;
	free(m->rgb);
	free(m->alpha);
	free(m);
	g->img[h - 1] = NULL;
}

/* Called for every placement a list drops: an anonymous image goes with
 * its last placement. */
static void place_gone(void *data, const struct kplace *p)
{
	struct gfx *g = data;
	struct kimg *m = g->img[p->image - 1];

	if (m && --m->nplace <= 0 && m->id == 0)
		kimg_free(g, p->image);
}

/* Remove every placement of an image, then the image. */
static void kimg_drop(struct gfx *g, uint32_t h)
{
	int s, i;

	for (s = 0; s < 2; s++)
		for (i = 0; i < g->pl[s].n; i++)
			if (g->pl[s].v[i].image == h)
				kplace_del(&g->pl[s], i--, place_gone, g);
	kimg_free(g, h);
}

static uint32_t kimg_find(const struct gfx *g, uint32_t id)
{
	uint32_t i;

	if (!id)
		return 0;
	for (i = 0; i < KIMG_MAX; i++)
		if (g->img[i] && g->img[i]->id == id)
			return i + 1;
	return 0;
}

static uint32_t kimg_free_index(const struct gfx *g)
{
	uint32_t i;

	for (i = 0; i < KIMG_MAX; i++)
		if (!g->img[i])
			return i + 1;
	return 0;
}

/* Which images have a placement in view now. */
static void kimg_in_view(const struct gfx *g, bool used[KIMG_MAX])
{
	int v[KPLACE_MAX];
	int n, i;
	int off = g->alt ? 0 : tsm_screen_get_sb_offset(g->scr);

	memset(used, 0, KIMG_MAX * sizeof(*used));
	n = kplace_in_view(&g->pl[g->alt], off, tsm_screen_get_height(g->scr),
			   v, KPLACE_MAX);
	for (i = 0; i < n; i++)
		used[g->pl[g->alt].v[v[i]].image - 1] = true;
}

#endif /* HAVOC_KITTY */

enum { NEED_NONE, NEED_SLOT, NEED_KIMG };

static bool fits(const struct gfx *g, size_t n, int need)
{
	if (g->pixels + n > g->cfg.memory)
		return false;
#if HAVOC_SIXEL
	if (need == NEED_SLOT && !free_index(g))
		return false;
#endif
#if HAVOC_KITTY
	if (need == NEED_KIMG && !kimg_free_index(g))
		return false;
#endif
	return true;
}

/* Room for one more picture of n pixels, and a slot or image index for
 * it. In this order until it fits: sixel slots no cell refers to; kitty
 * images with an id and no placement, the oldest first; pictures that are
 * not in view, the oldest first. A picture in view is never given up. */
static bool make_room(struct gfx *g, size_t n, int need)
{
#if HAVOC_SIXEL
	uint8_t used[TSM_PIC_SETS][32];
#endif
#if HAVOC_KITTY
	bool kview[KIMG_MAX];
#endif

	if (n > g->cfg.memory)
		return false;
	/* with room to spare the sweep waits until the store is half full,
	 * so that a cell scan is not done for every picture */
	if (fits(g, n, need) && g->pixels <= g->cfg.memory / 2)
		return true;

#if HAVOC_SIXEL
	sweep(g, used);
#endif
	if (fits(g, n, need))
		return true;

#if HAVOC_KITTY
	while (!fits(g, n, need)) {
		uint32_t i, old = 0;

		for (i = 0; i < KIMG_MAX; i++)
			if (g->img[i] && g->img[i]->nplace <= 0 &&
			    (!old || g->img[i]->last_use <
				     g->img[old - 1]->last_use))
				old = i + 1;
		if (!old)
			break;
		kimg_drop(g, old);
	}
	kimg_in_view(g, kview);
#endif

	while (!fits(g, n, need)) {
		/* the oldest of both kinds that is not in view */
		uint32_t oldest = 0;
		unsigned int pick = 0;
		int kind = 0;

#if HAVOC_SIXEL
		{
			unsigned int s;

			for (s = 1; s < NSLOTS; s++)
				if (g->slot[s] &&
				    !in_set(used[TSM_PIC_VIEW], s) &&
				    (!oldest || g->slot[s]->last_use < oldest)) {
					oldest = g->slot[s]->last_use;
					kind = 1;
					pick = s;
				}
		}
#endif
#if HAVOC_KITTY
		{
			unsigned int k;

			for (k = 0; k < KIMG_MAX; k++)
				if (g->img[k] && !kview[k] &&
				    (!oldest || g->img[k]->last_use < oldest)) {
					oldest = g->img[k]->last_use;
					kind = 2;
					pick = k + 1;
				}
		}
#endif
		if (!kind)
			return false;
#if HAVOC_SIXEL
		if (kind == 1)
			drop_slot(g, pick);
#endif
#if HAVOC_KITTY
		if (kind == 2)
			kimg_drop(g, pick);
#endif
	}
	return true;
}

#if HAVOC_SIXEL

/* Pixels of the pictures that are in view now. */
static size_t pixels_in_view(struct gfx *g)
{
	size_t n = 0;
	uint8_t used[TSM_PIC_SETS][32];
	unsigned int i;

	tsm_screen_pictures_used(g->scr, used);
	for (i = 1; i < NSLOTS; i++)
		if (g->slot[i] && in_set(used[TSM_PIC_VIEW], i))
			n += (size_t)g->slot[i]->w * g->slot[i]->h;
#if HAVOC_KITTY
	{
		bool kview[KIMG_MAX];

		kimg_in_view(g, kview);
		for (i = 0; i < KIMG_MAX; i++)
			if (g->img[i] && kview[i])
				n += (size_t)g->img[i]->w * g->img[i]->h;
	}
#endif
	return n;
}

static bool sixel_start(struct gfx *g, const struct tsm_vte_str_info *info)
{
	struct sixel_limits lim;
	size_t view;
	int x, avail;

	if (info->final != 'q' || info->flags || !g->cfg.sixel ||
	    g->cw <= 0 || g->ch <= 0)
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
	if (view >= g->cfg.memory)
		return false;
	lim.max_pixels = g->cfg.memory - view;
	if (lim.max_pixels < lim.max_w)
		return false;

	sixel_free(g->dec);
	g->dec = sixel_new(info->argc, info->argv, &lim);
	return g->dec != NULL;
}

static void place(struct gfx *g, struct sixel_image *img)
{
	struct tsm_screen_attr attr;
	struct slot *s;
	unsigned int i, cols, rows;
	unsigned int flags = 0;
	size_t n = (size_t)img->w * img->h;

	if (!make_room(g, n, NEED_SLOT) || !(s = malloc(sizeof(*s)))) {
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
	/* the tiles keep the cell's inverse bit, which the cursor and a
	 * selection use; an SGR 7 in force must not invert the picture */
	attr.inverse = 0;
	/* the cursor ends on the last row the picture covers */
	if (tsm_screen_place_picture(g->scr, &attr, i, cols, rows, rows - 1,
				     flags) < 0)
		free_slot(g, i);
}

static void sixel_end(struct gfx *g, enum tsm_vte_str_end how)
{
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

static void sixel_event(struct gfx *g, const struct tsm_screen_event *ev)
{
	uint8_t used[TSM_PIC_SETS][32];
	unsigned int i;

	switch (ev->type) {
	case TSM_SCREEN_EV_ALTERNATE:
		/* the last frame of a video must not stay behind the main
		 * screen */
		if (ev->alt)
			return;
		tsm_screen_pictures_used(g->scr, used);
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
		break;
	}
}

#endif /* HAVOC_SIXEL */

#if HAVOC_KITTY

static bool kitty_room(void *data, size_t pixels)
{
	return make_room(data, pixels, NEED_KIMG);
}

/* Where the box of a placement goes and how the source is cut and scaled
 * into it. Sizes are in pixels of the cell as it is now; they stay with
 * the placement when the cell size changes. */
static enum kitty_err geometry(const struct gfx *g, const struct kimg *m,
			       const struct kitty_cmd *c, struct kplace *p)
{
	uint64_t cw = g->cw > 0 ? g->cw : 0, ch = g->ch > 0 ? g->ch : 0;
	uint64_t dw, dh;
	uint32_t sw, sh, dx, dy;

	if (!cw || !ch || cw > 0xffff || ch > 0xffff || c->x >= m->w ||
	    c->y >= m->h || c->c > KPLACE_BOX_MAX || c->r > KPLACE_BOX_MAX)
		return KITTY_EINVAL;
	sw = c->w && c->w < m->w - c->x ? c->w : m->w - c->x;
	sh = c->h && c->h < m->h - c->y ? c->h : m->h - c->y;
	/* the offset inside the first cell */
	dx = c->X < cw ? c->X : (uint32_t)cw - 1;
	dy = c->Y < ch ? c->Y : (uint32_t)ch - 1;
	if (c->c)
		dw = c->c * cw - dx;
	else if (c->r)
		dw = (uint64_t)sw * (c->r * ch - dy) / sh;
	else
		dw = sw;
	if (c->r)
		dh = c->r * ch - dy;
	else if (c->c)
		dh = (uint64_t)sh * (c->c * cw - dx) / sw;
	else
		dh = sh;
	if (!dw)
		dw = 1;
	if (!dh)
		dh = 1;
	if ((dx + dw + cw - 1) / cw > KPLACE_BOX_MAX ||
	    (dy + dh + ch - 1) / ch > KPLACE_BOX_MAX)
		return KITTY_EINVAL;
	p->cols = (dx + dw + cw - 1) / cw;
	p->rows = (dy + dh + ch - 1) / ch;
	p->cw0 = cw;
	p->ch0 = ch;
	p->sx = c->x;
	p->sy = c->y;
	p->sw = sw;
	p->sh = sh;
	p->dx = dx;
	p->dy = dy;
	p->dw = dw;
	p->dh = dh;
	return KITTY_OK;
}

/* A video sends each frame as a new anonymous picture on the same box and
 * never deletes the last one: the earlier frame goes. */
static void drop_same_box(struct gfx *g, const struct kplace *np)
{
	struct kplace_list *l = &g->pl[g->alt];
	int i;

	for (i = 0; i < l->n; i++) {
		const struct kplace *p = &l->v[i];

		if (g->img[p->image - 1]->id == 0 && p->row == np->row &&
		    p->col == np->col && p->rows == np->rows &&
		    p->cols == np->cols && p->z == np->z)
			kplace_del(l, i--, place_gone, g);
	}
}

/* The oldest placement of the active list that is not in view, to make
 * room in the list. */
static bool list_evict(struct gfx *g)
{
	struct kplace_list *l = &g->pl[g->alt];
	int off = g->alt ? 0 : tsm_screen_get_sb_offset(g->scr);
	int h = tsm_screen_get_height(g->scr);
	int i, old = -1;

	for (i = 0; i < l->n; i++) {
		const struct kplace *p = &l->v[i];

		if (p->row + p->skip + off <= h - 1 &&
		    p->row + p->rows - 1 + off >= 0)
			continue;
		if (old < 0 || p->order < l->v[old].order)
			old = i;
	}
	if (old < 0)
		return false;
	kplace_del(l, old, place_gone, g);
	return true;
}

static enum kitty_err put_placement(struct gfx *g, uint32_t h,
				    const struct kitty_cmd *c)
{
	struct kimg *m = g->img[h - 1];
	struct kplace_list *l = &g->pl[g->alt];
	struct kplace p, *np;
	enum kitty_err err;
	int x, i;

	memset(&p, 0, sizeof(p));
	err = geometry(g, m, c, &p);
	if (err != KITTY_OK)
		return err;
	x = tsm_screen_get_cursor_x(g->scr);
	if (x >= tsm_screen_get_width(g->scr))
		x = tsm_screen_get_width(g->scr) - 1;
	p.image = h;
	p.pid = c->p;
	p.z = c->z;
	p.row = tsm_screen_get_cursor_y(g->scr);
	p.col = x;
	p.serial = ++g->serial;
	p.order = ++g->order;

	/* the same placement id again replaces the placement */
	if (p.pid)
		for (i = 0; i < l->n; i++)
			if (l->v[i].image == h && l->v[i].pid == p.pid)
				kplace_del(l, i--, place_gone, g);
	if (m->id == 0)
		drop_same_box(g, &p);
	if (l->n >= KPLACE_MAX && !list_evict(g))
		return KITTY_ENOMEM;
	np = kplace_add(l, &p);
	if (!np)
		return KITTY_ENOMEM;
	m->nplace++;
	m->last_use = ++g->use;

	if (c->C != 1) {
		int down = np->rows - KITTY_CURSOR_ROWS_BACK;

		/* the scrolls move the placement with the text */
		if (down > 0)
			tsm_screen_move_down(g->scr, down, true);
		tsm_screen_move_right(g->scr, np->cols);
	}
	return KITTY_OK;
}

/* Take the picture of a result into the store. On success r->img is
 * handed over (rgb and alpha are NULL after) and *h is its handle. */
static enum kitty_err store(struct gfx *g, struct kitty_result *r, uint32_t *h)
{
	const struct kitty_cmd *c = &r->cmd;
	size_t n = (size_t)r->img.w * r->img.h;
	struct kimg *m;
	uint32_t old;

	if (!r->img.rgb || !n)
		return KITTY_ENODATA;
	/* a new picture with an id replaces the old one and its placements */
	old = kimg_find(g, c->i);
	if (old)
		kimg_drop(g, old);
	if (!make_room(g, n, NEED_KIMG))
		return KITTY_ENOMEM;
	m = calloc(1, sizeof(*m));
	if (!m)
		return KITTY_ENOMEM;
	*h = kimg_free_index(g);
	m->id = c->i;
	m->w = r->img.w;
	m->h = r->img.h;
	m->rgb = r->img.rgb;
	m->alpha = r->img.alpha;
	m->last_use = ++g->use;
	r->img.rgb = NULL;
	r->img.alpha = NULL;
	g->img[*h - 1] = m;
	g->nimg++;
	g->pixels += n;
	return KITTY_OK;
}

static bool covers(const struct kplace *p, int x, int y, bool use_x,
		   bool use_y)
{
	if (use_x && (x < p->col || x >= p->col + p->cols))
		return false;
	if (use_y && (y < p->row + p->skip || y >= p->row + p->rows))
		return false;
	return true;
}

/* a=d. The lowercase selectors delete placements, the uppercase ones also
 * the pictures that are left without. */
static enum kitty_err delete_cmd(struct gfx *g, const struct kitty_cmd *c)
{
	struct kplace_list *act = &g->pl[g->alt];
	bool free_img = c->d >= 'A' && c->d <= 'Z';
	char d = free_img ? c->d + ('a' - 'A') : c->d;
	bool touched[KIMG_MAX];
	uint32_t i, h = 0;
	int s, j, rows = tsm_screen_get_height(g->scr);
	int cx = (int)c->x - 1, cy = (int)c->y - 1;

	memset(touched, 0, sizeof(touched));
	switch (d) {
	case 'a': case 'c': case 'p': case 'x': case 'y': case 'z':
		break;
	case 'i':
		h = kimg_find(g, c->i);
		if (!h)
			return KITTY_ENOENT;
		break;
	default:
		return KITTY_EINVAL;
	}

	if (d == 'c') {
		cx = tsm_screen_get_cursor_x(g->scr);
		cy = tsm_screen_get_cursor_y(g->scr);
	}
	for (s = 0; s < 2; s++) {
		struct kplace_list *l = &g->pl[s];

		if (d != 'i' && l != act)
			continue;
		for (j = 0; j < l->n; j++) {
			const struct kplace *p = &l->v[j];
			bool hit;

			switch (d) {
			case 'a':
				hit = p->row + p->skip <= rows - 1 &&
				      p->row + p->rows - 1 >= 0;
				break;
			case 'i':
				hit = p->image == h &&
				      (!KITTY_HAS(c, 'p') || p->pid == c->p);
				break;
			case 'c': case 'p':
				hit = covers(p, cx, cy, true, true);
				break;
			case 'x':
				hit = covers(p, cx, 0, true, false);
				break;
			case 'y':
				hit = covers(p, 0, cy, false, true);
				break;
			default:
				hit = p->z == c->z;
				break;
			}
			if (!hit)
				continue;
			touched[p->image - 1] = true;
			kplace_del(l, j--, place_gone, g);
		}
	}
	if (d == 'i')
		touched[h - 1] = true;
	if (free_img)
		for (i = 0; i < KIMG_MAX; i++)
			if (touched[i] && g->img[i] && g->img[i]->nplace <= 0)
				kimg_free(g, i + 1);
	return KITTY_OK;
}

static void kitty_run(struct gfx *g, struct kitty_result *r)
{
	const struct kitty_cmd *c = &r->cmd;
	enum kitty_err err = r->err;
	uint32_t h = 0;
	char buf[128];
	size_t n;

	if (err == KITTY_OK) {
		switch (c->a) {
		case 't': case 'T':
			err = store(g, r, &h);
			if (err == KITTY_OK && c->a == 'T')
				err = put_placement(g, h, c);
			/* nothing can refer to an anonymous picture that is
			 * not placed */
			if (h && g->img[h - 1] && g->img[h - 1]->id == 0 &&
			    g->img[h - 1]->nplace <= 0)
				kimg_free(g, h);
			break;
		case 'p':
			h = kimg_find(g, c->i);
			err = h ? put_placement(g, h, c) : KITTY_ENOENT;
			break;
		case 'd':
			err = delete_cmd(g, c);
			break;
		default:	/* q: the decode told */
			break;
		}
		if (c->a != 'q')
			gfx_debug(g);
	}
	kitty_result_free(r);

	/* a delete has no answer when it worked: a client that does not
	 * read replies must not get a stray "OK" for it */
	if (c->a == 'd' && err == KITTY_OK)
		return;
	n = kitty_format_reply(c, err, buf, sizeof(buf));
	if (n)
		tsm_vte_reply(g->vte, buf, n);
}

static void kitty_event(struct gfx *g, const struct tsm_screen_event *ev)
{
	int h = tsm_screen_get_height(g->scr);

	switch (ev->type) {
	case TSM_SCREEN_EV_ALTERNATE:
		g->alt = ev->alt;
		break;
	case TSM_SCREEN_EV_SCROLL:
	case TSM_SCREEN_EV_ERASE_SCREEN:
		kplace_event(&g->pl[ev->alt], ev, h,
			     ev->alt ? 0 : tsm_screen_get_sb_count(g->scr),
			     place_gone, g);
		break;
	case TSM_SCREEN_EV_CLEAR_SB:
		/* the scrollback is the main screen's, whichever is active */
		kplace_event(&g->pl[0], ev, h, tsm_screen_get_sb_count(g->scr),
			     place_gone, g);
		break;
	case TSM_SCREEN_EV_RESET: {
		uint32_t i;

		kplace_clear(&g->pl[0], place_gone, g);
		kplace_clear(&g->pl[1], place_gone, g);
		for (i = 1; i <= KIMG_MAX; i++)
			kimg_free(g, i);
		if (g->kit)
			kitty_reset(g->kit);
		break;
	}
	default:
		break;
	}
}

/* A kitty string ended, with the commands it finished. */
static void kitty_str_end(struct gfx *g, enum tsm_vte_str_end how)
{
	struct kitty_result res[2];
	int n, i;

	g->apc = false;
	n = kitty_end(g->kit, how != TSM_VTE_STR_DONE, res);
	for (i = 0; i < n; i++)
		kitty_run(g, &res[i]);
}

#endif /* HAVOC_KITTY */

static bool str_start(void *data, const struct tsm_vte_str_info *info)
{
	struct gfx *g = data;

#if HAVOC_SIXEL
	if (info->kind == TSM_VTE_STR_DCS)
		return sixel_start(g, info);
#endif
#if HAVOC_KITTY
	if (info->kind == TSM_VTE_STR_APC && g->kit) {
		g->apc = true;
		return true;
	}
#endif
	(void)g;
	(void)info;
	return false;
}

static bool str_put(void *data, const char *buf, size_t len)
{
	struct gfx *g = data;

#if HAVOC_KITTY
	if (g->apc)
		return kitty_put(g->kit, buf, len);
#endif
#if HAVOC_SIXEL
	if (!g->dec)
		return false;
	sixel_put(g->dec, buf, len);
	return true;
#else
	(void)g;
	(void)buf;
	(void)len;
	return false;
#endif
}

static void str_end(void *data, enum tsm_vte_str_end how)
{
	struct gfx *g = data;

#if HAVOC_KITTY
	if (g->apc) {
		kitty_str_end(g, how);
		return;
	}
#endif
#if HAVOC_SIXEL
	sixel_end(g, how);
#else
	(void)g;
	(void)how;
#endif
}

static void on_event(struct tsm_screen *scr, const struct tsm_screen_event *ev,
		     void *data)
{
	struct gfx *g = data;
	size_t before = g->pixels;
	int counts = 0;

#if HAVOC_KITTY
	counts = g->nimg + g->pl[0].n * KIMG_MAX + g->pl[1].n * KIMG_MAX * 512;
#endif
#if HAVOC_SIXEL
	sixel_event(g, ev);
#endif
#if HAVOC_KITTY
	kitty_event(g, ev);
	if (counts != g->nimg + g->pl[0].n * KIMG_MAX +
	    g->pl[1].n * KIMG_MAX * 512)
		before = ~g->pixels;
#endif
	if (g->pixels != before)
		gfx_debug(g);
}

#if HAVOC_SIXEL
uint32_t gfx_pic_gen(const struct gfx *g, uint32_t pic)
{
	const struct slot *s = g ? g->slot[TSM_PIC_SLOT(pic)] : NULL;

	return s ? s->gen : 0;
}

static void paint_tile(struct gfx *g, uint32_t pic,
		       const struct gfx_target *t)
{
	const struct slot *s = g->slot[TSM_PIC_SLOT(pic)];
	uint32_t fx, fy, ay = 0;
	unsigned int x0, y0;
	int px, py;

	if (!s)
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

#if HAVOC_KITTY

/* RGB565 channels to 8 bits, the top bits repeated into the low ones */
static uint8_t c5[32], c6[64];

static void tables(void)
{
	int i;

	if (c5[31])
		return;
	for (i = 0; i < 32; i++)
		c5[i] = i << 3 | i >> 2;
	for (i = 0; i < 64; i++)
		c6[i] = i << 2 | i >> 4;
}

static int64_t floor_div(int64_t a, int64_t b)
{
	int64_t q = a / b;

	return q - (a % b != 0 && (a < 0) != (b < 0));
}

#define MUL(a, b) (((uint32_t)(a) * (uint32_t)(b) + 255) >> 8)
#define SAT(v) ((v) > 255 ? 255 : (v))

/* One cell's share of one placement. The cell is tx, ty cells from the
 * box's top left. The source is stepped through in 16.16 per target
 * pixel, from a start worked out here (64 bit, divides once per cell);
 * the pixel loops only add and compare. */
static void paint_kplace(const struct gfx *g, const struct kplace *p,
			 const struct kimg *m, int tx, int ty,
			 const struct gfx_target *t)
{
	int64_t bx = (int64_t)tx * p->cw0 - p->dx;
	int64_t by = (int64_t)ty * p->ch0 - p->dy;
	int64_t ax0 = floor_div(bx * p->sw * 65536, p->dw);
	int64_t ay0 = floor_div(by * p->sh * 65536, p->dh);
	int64_t fx = ((uint64_t)p->sw * p->cw0 << 16) /
		     ((uint64_t)p->dw * g->cw);
	int64_t fy = ((uint64_t)p->sh * p->ch0 << 16) /
		     ((uint64_t)p->dh * g->ch);
	int64_t xend = (int64_t)p->sw << 16, yend = (int64_t)p->sh << 16;
	int64_t ay = ay0;
	int px, py;

	for (py = 0; py < t->h; py++, ay += fy) {
		const uint16_t *rgb;
		const uint8_t *al;
		uint32_t *dst = t->px + (size_t)py * t->stride;
		int64_t ax = ax0;
		size_t row;

		if (ay < 0)
			continue;
		if (ay >= yend)
			break;
		row = (size_t)(p->sy + (ay >> 16)) * m->w + p->sx;
		rgb = m->rgb + row;
		al = m->alpha ? m->alpha + row : NULL;
		for (px = 0; px < t->w; px++, ax += fx) {
			uint32_t v, a, r, gr, b;
			size_t sx;

			if (ax < 0)
				continue;
			if (ax >= xend)
				break;
			sx = ax >> 16;
			a = al ? al[sx] : 255;
			if (!a)
				continue;
			v = rgb[sx];
			r = c5[v >> 11];
			gr = c6[v >> 5 & 63];
			b = c5[v & 31];
			if (a == 255) {
				dst[px] = 0xff000000 | r << 16 | gr << 8 | b;
			} else {
				uint32_t d = dst[px], ca = 255 - a;
				uint32_t oa = a + MUL(d >> 24, ca);
				uint32_t orr = MUL(r, a) + MUL(d >> 16 & 255, ca);
				uint32_t og = MUL(gr, a) + MUL(d >> 8 & 255, ca);
				uint32_t ob = MUL(b, a) + MUL(d & 255, ca);

				dst[px] = SAT(oa) << 24 | SAT(orr) << 16 |
					  SAT(og) << 8 | SAT(ob);
			}
		}
	}
}

static uint8_t layer_bit(int32_t z)
{
	return z < Z_UNDER_BG ? GFX_HAS_UNDER_BG :
	       z < 0 ? GFX_HAS_UNDER_TEXT : GFX_HAS_OVER_TEXT;
}

void gfx_frame(struct gfx *g, uint32_t *klayer, uint8_t *lmask)
{
	int i, c, r;

	if (!g || !klayer || !lmask || g->cols <= 0 || g->rows <= 0)
		return;
	g->nview = 0;
	memset(klayer, 0, (size_t)g->cols * g->rows * sizeof(*klayer));
	memset(lmask, 0, (size_t)g->cols * g->rows);
	if (!g->kit)
		return;
	g->sb_off = g->alt ? 0 : tsm_screen_get_sb_offset(g->scr);
	g->nview = kplace_in_view(&g->pl[g->alt], g->sb_off, g->rows, g->view,
				  KPLACE_MAX);
	for (i = 0; i < g->nview; i++) {
		const struct kplace *p = &g->pl[g->alt].v[g->view[i]];
		int c0 = p->col < 0 ? 0 : p->col;
		int c1 = p->col + p->cols > g->cols ? g->cols : p->col + p->cols;
		int r0 = p->row + p->skip + g->sb_off;
		int r1 = p->row + p->rows + g->sb_off;
		uint8_t bit = layer_bit(p->z);

		if (r0 < 0)
			r0 = 0;
		if (r1 > g->rows)
			r1 = g->rows;
		for (r = r0; r < r1; r++)
			for (c = c0; c < c1; c++) {
				size_t k = (size_t)r * g->cols + c;
				uint32_t h = klayer[k];

				/* which placement, and which part of it the
				 * cell shows: a moved or replaced one changes
				 * the cells it covers, and only those */
				h = (h ^ p->serial) * 16777619u;
				h = (h ^ ((uint32_t)(c - p->col) << 16) ^
				     (uint32_t)(r - g->sb_off - p->row)) *
				    16777619u;
				klayer[k] = h ? h : 1;
				lmask[k] |= bit;
			}
	}
}

static void paint_layer(struct gfx *g, uint8_t bit, int x, int y,
			const struct gfx_target *t)
{
	int i;

	tables();
	for (i = 0; i < g->nview; i++) {
		const struct kplace *p = &g->pl[g->alt].v[g->view[i]];
		int ty = y - g->sb_off - p->row;

		if (layer_bit(p->z) != bit || x < p->col ||
		    x >= p->col + p->cols || ty < p->skip || ty >= p->rows)
			continue;
		paint_kplace(g, p, g->img[p->image - 1], x - p->col, ty, t);
	}
}

#else /* !HAVOC_KITTY */

void gfx_frame(struct gfx *g, uint32_t *klayer, uint8_t *lmask)
{
	(void)g;
	(void)klayer;
	(void)lmask;
}

#endif /* HAVOC_KITTY */

void gfx_paint(struct gfx *g, enum gfx_layer layer, int x, int y,
	       uint32_t pic, const struct gfx_target *t)
{
	if (!g || g->cw <= 0 || g->ch <= 0)
		return;
#if HAVOC_SIXEL
	if (layer == GFX_TILE) {
		paint_tile(g, pic, t);
		return;
	}
#endif
#if HAVOC_KITTY
	if (layer == GFX_UNDER_BG)
		paint_layer(g, GFX_HAS_UNDER_BG, x, y, t);
	else if (layer == GFX_UNDER_TEXT)
		paint_layer(g, GFX_HAS_UNDER_TEXT, x, y, t);
	else if (layer == GFX_OVER_TEXT)
		paint_layer(g, GFX_HAS_OVER_TEXT, x, y, t);
#endif
	(void)x;
	(void)y;
	(void)pic;
	(void)t;
}

void gfx_debug(const struct gfx *g)
{
	int sixel = 0, kitty = 0, pm = 0, pa = 0;

	if (!g || !g->debug)
		return;
#if HAVOC_SIXEL
	sixel = slot_count(g);
#endif
#if HAVOC_KITTY
	kitty = g->nimg;
	pm = g->pl[0].n;
	pa = g->pl[1].n;
#endif
	fprintf(stderr, "gfx: sixel=%d kitty=%d places=%d+%d pixels=%zu/%zu\n",
		sixel, kitty, pm, pa, g->pixels, g->cfg.memory);
}

struct gfx *gfx_new(struct tsm_screen *scr, struct tsm_vte *vte,
		    const struct gfx_config *cfg)
{
	static const struct tsm_vte_str_ops ops = {
		.start = str_start,
		.put = str_put,
		.end = str_end,
	};
	struct gfx *g;
	bool sixel = HAVOC_SIXEL && cfg->sixel;
	bool kitty = HAVOC_KITTY && cfg->kitty;

	if (!sixel && !kitty)
		return NULL;
	g = calloc(1, sizeof(*g));
	if (!g)
		return NULL;
	g->scr = scr;
	g->vte = vte;
	g->cfg = *cfg;
	g->cfg.sixel = sixel;
	g->cfg.kitty = kitty;
	g->debug = getenv("HAVOC_GFX_DEBUG") != NULL;
#if HAVOC_KITTY
	if (kitty) {
		struct kitty_limits lim;

		memset(&lim, 0, sizeof(lim));
		lim.max_pixels = cfg->memory;
		lim.shm = cfg->kitty_shm;
		lim.room = kitty_room;
		lim.data = g;
		g->kit = kitty_new(&lim);
		if (!g->kit && !sixel) {
			free(g);
			return NULL;
		}
	}
#endif
	tsm_vte_set_str_ops(vte, &ops, g);
	tsm_screen_set_event_cb(scr, on_event, g);
	return g;
}

void gfx_free(struct gfx *g)
{
#if HAVOC_SIXEL
	unsigned int i;
#endif

	if (!g)
		return;
	/* ends a string in progress while the decoders are still there */
	tsm_vte_set_str_ops(g->vte, NULL, NULL);
	tsm_screen_set_event_cb(g->scr, NULL, NULL);
#if HAVOC_SIXEL
	sixel_free(g->dec);
	for (i = 1; i < NSLOTS; i++)
		free_slot(g, i);
#endif
#if HAVOC_KITTY
	kplace_fini(&g->pl[0], place_gone, g);
	kplace_fini(&g->pl[1], place_gone, g);
	{
		uint32_t k;

		for (k = 1; k <= KIMG_MAX; k++)
			kimg_free(g, k);
	}
	if (g->kit)
		kitty_free(g->kit);
#endif
	free(g);
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
