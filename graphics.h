#ifndef HAVOC_GRAPHICS_H
#define HAVOC_GRAPHICS_H

#define HAVE_GRAPHICS (HAVOC_SIXEL || HAVOC_KITTY)

#if HAVE_GRAPHICS
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "tsm/libtsm.h"
#include "c8.h"

struct gfx_config {
	bool sixel;		/* [graphics] sixel */
	bool kitty;		/* [graphics] kitty */
	bool kitty_shm;		/* [graphics] kitty shm */
	size_t memory;		/* [graphics] memory: pixels, all pictures */
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

/* Once per redraw, before tsm_screen_draw: works out the kitty placements
 * in view and, for each of the cols * rows cells, writes into klayer a
 * hash of the placements over it (0: none) and into lmask the layers that
 * have any (GFX_HAS_*). With no kitty placement both are cleared. */
#define GFX_HAS_UNDER_BG	0x01
#define GFX_HAS_UNDER_TEXT	0x02
#define GFX_HAS_OVER_TEXT	0x04
void gfx_frame(struct gfx *g, uint32_t *klayer, uint8_t *lmask);

#if HAVOC_SIXEL
/* A number that changes whenever the picture a sixel tile refers to is
 * another one (its slot was reused), for the cell key. 0 for no picture. */
uint32_t gfx_pic_gen(const struct gfx *g, uint32_t pic);
#endif

enum gfx_layer {
	GFX_UNDER_BG,		/* kitty z < -2^30; call on default-background cells only */
	GFX_UNDER_TEXT,		/* kitty -2^30 <= z < 0 */
	GFX_TILE,		/* the sixel tile pic */
	GFX_OVER_TEXT,		/* kitty z >= 0 */
};

struct gfx_target {
	uint32_t *px;		/* the cell's top left pixel */
	int stride;		/* pixels from one row to the next */
	int w, h;		/* pixels to paint: the cell, or what fits */
	/* C8 drawing: one byte for each pixel that is written for the pixels
	 * the picture sets, GFX_PHASE_PICTURE | (y & 3) << 2 | (x & 3) with x
	 * and y counted from the picture's corner. NULL in XRGB drawing. */
	uint8_t *phase;
	int phase_stride;
};

#define GFX_PHASE_PICTURE	0x80	/* | phase: dither */
#define GFX_PHASE_NEAREST	0x40	/* nearest cube entry or known slot, no new slot */

/* Composite one layer of the view cell (x, y) over what t holds: opaque
 * picture pixels replace, ones with alpha blend over (the colours are
 * premultiplied, as for text), unset ones and alpha 0 leave the pixel.
 * The picture is scaled to the current cell size, nearest neighbour.
 * pic: the cell's sixel tile, for GFX_TILE. */
void gfx_paint(struct gfx *g, enum gfx_layer layer, int x, int y,
	       uint32_t pic, const struct gfx_target *t);

/* An XRGB scratch to C8, pixel by pixel: phase 0 through c8_get (text,
 * backgrounds), GFX_PHASE_NEAREST through c8_find, GFX_PHASE_PICTURE through
 * c8_dither (no slot). Divide-free. */
void gfx_to_c8(const uint32_t *src, int sstride, const uint8_t *phase,
	       int pstride, int w, int h, uint8_t *dst, int dstride,
	       struct c8_table *t);

#endif
#endif
