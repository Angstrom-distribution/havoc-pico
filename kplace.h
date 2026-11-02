#ifndef HAVOC_KPLACE_H
#define HAVOC_KPLACE_H

#include <stdint.h>
#include "tsm/libtsm.h"

/* Where kitty pictures sit on one screen. They are not cells: text written
 * or erased over a placement leaves it, and it moves only with the lines
 * (libtsm's scroll events). Rows count from the first row of the screen;
 * a row above 0 is in the scrollback. */
struct kplace {
	uint32_t image;		/* store handle */
	uint32_t pid;		/* p=, 0 none */
	int32_t z;
	int row, col;		/* top left cell of the box */
	int rows, cols;		/* box in cells */
	int skip;		/* rows cut off the top of the box */
	uint16_t cw0, ch0;	/* cell size when placed */
	uint32_t sx, sy, sw, sh;	/* source rectangle, image pixels */
	uint32_t dx, dy, dw, dh;	/* where it goes in the box, placement pixels */
	uint32_t serial;	/* new for every placement */
	uint32_t order;		/* placement order, for equal z */
};

#define KPLACE_MAX 256

struct kplace_list {
	struct kplace *v;
	int n, cap;
};

/* called for every placement the list deletes */
typedef void (*kplace_gone)(void *data, const struct kplace *p);

void kplace_init(struct kplace_list *l);
/* deletes every placement (with gone) and frees the array */
void kplace_fini(struct kplace_list *l, kplace_gone gone, void *data);
/* NULL when the list holds KPLACE_MAX, or on no memory */
struct kplace *kplace_add(struct kplace_list *l, const struct kplace *p);
void kplace_del(struct kplace_list *l, int i, kplace_gone gone, void *data);
void kplace_clear(struct kplace_list *l, kplace_gone gone, void *data);
/* The rules for one libtsm event, then the prune of entries wholly beyond
 * the scrollback. size_y: rows of the screen, sb_count: scrollback lines
 * (0 for the alternate screen). */
void kplace_event(struct kplace_list *l, const struct tsm_screen_event *ev,
		  int size_y, int sb_count, kplace_gone gone, void *data);
/* Indices of the entries with a shown row in view rows 0 .. size_y - 1
 * (view row = placement row + sb_offset, sb_offset the scrollback lines
 * shown above the screen), sorted by z, equal z by order; returns how many
 * (at most max). */
int kplace_in_view(const struct kplace_list *l, int sb_offset, int size_y,
		   int *out, int max);

#endif
