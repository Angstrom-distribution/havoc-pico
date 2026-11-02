/* The list of kitty placements of one screen and how libtsm's line moves
 * change it. Needs nothing but the C library. */
#include <stdlib.h>
#include <string.h>

#include "kplace.h"

void kplace_init(struct kplace_list *l)
{
	memset(l, 0, sizeof(*l));
}

void kplace_del(struct kplace_list *l, int i, kplace_gone gone, void *data)
{
	struct kplace p = l->v[i];

	memmove(&l->v[i], &l->v[i + 1], (l->n - i - 1) * sizeof(*l->v));
	l->n--;
	if (gone)
		gone(data, &p);
}

void kplace_clear(struct kplace_list *l, kplace_gone gone, void *data)
{
	while (l->n)
		kplace_del(l, l->n - 1, gone, data);
}

void kplace_fini(struct kplace_list *l, kplace_gone gone, void *data)
{
	kplace_clear(l, gone, data);
	free(l->v);
	kplace_init(l);
}

struct kplace *kplace_add(struct kplace_list *l, const struct kplace *p)
{
	if (l->n >= KPLACE_MAX)
		return NULL;
	if (l->n == l->cap) {
		int cap = l->cap ? l->cap * 2 : 16;
		struct kplace *v = realloc(l->v, cap * sizeof(*v));

		if (!v)
			return NULL;
		l->v = v;
		l->cap = cap;
	}
	l->v[l->n] = *p;
	return &l->v[l->n++];
}

/* The last row of the box that is on the screen. A box taller than what
 * is left of the screen exists while the cursor is moved down past it;
 * the lines it will scroll are counted as the box's own. */
static int last_row(const struct kplace *p, int size_y)
{
	int last = p->row + p->rows - 1;

	return last > size_y - 1 ? size_y - 1 : last;
}

static void scroll_up(struct kplace_list *l, const struct tsm_screen_event *ev,
		      int size_y, int sb_count, kplace_gone gone, void *data)
{
	int i, n = ev->n, top = ev->top, bottom = ev->bottom;

	for (i = 0; i < l->n; i++) {
		struct kplace *p = &l->v[i];
		int first = p->row + p->skip;
		bool inside;

		/* the scrollback moves with the screen when the rows went
		 * into it, and does not exist when they were lost */
		inside = (top == 0 || first >= top) &&
			 last_row(p, size_y) <= bottom &&
			 (first >= 0 ||
			  (top == 0 && (ev->to_sb || sb_count == 0)));
		if (inside) {
			p->row -= n;
			first = p->row + p->skip;
			if (first < top && !(top == 0 && ev->to_sb)) {
				p->skip += top - first;
				if (p->skip >= p->rows) {
					kplace_del(l, i--, gone, data);
					continue;
				}
			}
		} else if (ev->to_sb && first < 0) {
			p->row -= n;
		}
	}
}

static void scroll_down(struct kplace_list *l,
			const struct tsm_screen_event *ev, int size_y,
			kplace_gone gone, void *data)
{
	int i, m = -ev->n, top = ev->top, bottom = ev->bottom;

	for (i = 0; i < l->n; i++) {
		struct kplace *p = &l->v[i];

		if (p->row + p->skip < top || last_row(p, size_y) > bottom)
			continue;
		p->row += m;
		if (p->row + p->rows - 1 > bottom) {
			p->rows = bottom - p->row + 1;
			if (p->rows <= p->skip)
				kplace_del(l, i--, gone, data);
		}
	}
}

void kplace_event(struct kplace_list *l, const struct tsm_screen_event *ev,
		  int size_y, int sb_count, kplace_gone gone, void *data)
{
	int i;

	switch (ev->type) {
	case TSM_SCREEN_EV_SCROLL:
		if (ev->n > 0)
			scroll_up(l, ev, size_y, sb_count, gone, data);
		else if (ev->n < 0)
			scroll_down(l, ev, size_y, gone, data);
		break;
	case TSM_SCREEN_EV_ERASE_SCREEN:
		/* the placements that touch the screen; those wholly in the
		 * scrollback stay */
		for (i = 0; i < l->n; i++) {
			struct kplace *p = &l->v[i];

			if (p->row + p->skip <= size_y - 1 &&
			    p->row + p->rows - 1 >= 0)
				kplace_del(l, i--, gone, data);
		}
		break;
	case TSM_SCREEN_EV_CLEAR_SB:
		for (i = 0; i < l->n; i++) {
			struct kplace *p = &l->v[i];

			if (p->row + p->rows <= 0)
				kplace_del(l, i--, gone, data);
			else if (p->row + p->skip < 0)
				/* what shows from the screen on stays where
				 * it is in the picture */
				p->skip = -p->row;
		}
		break;
	case TSM_SCREEN_EV_RESET:
		kplace_clear(l, gone, data);
		return;
	default:
		return;
	}

	for (i = 0; i < l->n; i++)
		if (l->v[i].row + l->v[i].rows <= -sb_count ||
		    l->v[i].skip >= l->v[i].rows)
			kplace_del(l, i--, gone, data);
}

static bool before(const struct kplace *a, const struct kplace *b)
{
	return a->z < b->z || (a->z == b->z && a->order < b->order);
}

int kplace_in_view(const struct kplace_list *l, int sb_offset, int size_y,
		   int *out, int max)
{
	int i, n = 0;

	for (i = 0; i < l->n; i++) {
		const struct kplace *p = &l->v[i];
		int j;

		if (p->row + p->skip + sb_offset > size_y - 1 ||
		    p->row + p->rows - 1 + sb_offset < 0 || p->skip >= p->rows)
			continue;
		if (n == max)
			break;
		for (j = n; j > 0 && before(p, &l->v[out[j - 1]]); j--)
			out[j] = out[j - 1];
		out[j] = i;
		n++;
	}
	return n;
}
