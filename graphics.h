#ifndef HAVOC_GRAPHICS_H
#define HAVOC_GRAPHICS_H

#define HAVE_GRAPHICS (HAVOC_SIXEL || HAVOC_KITTY)

#if HAVE_GRAPHICS
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "tsm/libtsm.h"

struct gfx_config {
	bool sixel;		/* [graphics] sixel */
	bool shm;		/* [graphics] shm (kitty t=s) */
	size_t max_pixels;	/* [graphics] max_pixels, all pictures together */
};

struct gfx;

/* Takes over the vte's string ops and the screen's event callback. NULL
 * when no protocol is on (or built in); every call below accepts NULL and
 * then does nothing. */
struct gfx *gfx_new(struct tsm_screen *scr, struct tsm_vte *vte,
		    const struct gfx_config *cfg);
/* Sets the vte's string ops and the screen's event callback back to none. */
void gfx_free(struct gfx *g);
bool gfx_enabled(const struct gfx *g);

/* At every configure: cell size in buffer pixels and the grid. Updates the
 * vte's graphics replies. */
void gfx_set_geometry(struct gfx *g, int cw, int ch, int cols, int rows);

/* HAVOC_GFX_DEBUG set: a line to stderr after each picture,
 * "gfx: sixel=<slots> kitty=<images> places=<main>+<alt> pixels=<n>/<cap>". */
void gfx_debug(const struct gfx *g);

#if HAVOC_SIXEL
/* A number that changes whenever the picture a sixel tile refers to is
 * another one (its slot was reused), for the cell key. 0 for no picture. */
uint32_t gfx_pic_gen(const struct gfx *g, uint32_t pic);

enum gfx_layer {
	GFX_TILE,		/* the sixel tile pic */
};

struct gfx_target {
	uint32_t *px;		/* the cell's top left pixel */
	int stride;		/* pixels from one row to the next */
	int w, h;		/* pixels to paint: the cell, or what fits */
};

/* Composite one layer of the cell over what t holds: picture pixels
 * replace, unset ones leave the pixel. The picture is scaled to the
 * current cell size, nearest neighbour. */
void gfx_paint(struct gfx *g, enum gfx_layer layer, uint32_t pic,
	       const struct gfx_target *t);
#endif

#endif
#endif
