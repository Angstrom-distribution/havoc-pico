#ifndef HAVOC_C8_H
#define HAVOC_C8_H

#include <stdbool.h>
#include <stdint.h>

/* The colours of a C8 (8 bits per pixel, indexed) window.
 *
 * Indices 16 to 231 are the xterm 6x6x6 colour cube, fixed. The other 40
 * (0 to 15 and 232 to 255) are slots that hold colours the window was asked
 * to draw, in the order they come, and never change meaning while the
 * pictures that use them exist: a slot is only ever added. A colour is drawn
 * as, in this order: the cube entry when it is exactly a cube colour, its
 * slot, a new slot, the nearest cube entry (and then the table is "full").
 * 256-colour programs and the ANSI colours of a theme therefore cost slots
 * only for the colours that are not in the cube.
 *
 * Colours are 0x00RRGGBB. */

#define C8_SLOTS 40

struct c8_table {
	uint32_t rgb[C8_SLOTS];
	int n;
	/* the palette has changed since c8_clean() */
	bool dirty;
	/* a colour was drawn as the nearest cube entry because no slot was
	 * free; c8_reset() then makes room for the colours in use now */
	bool full;
	uint32_t last;
	uint8_t last_idx;
};

void c8_reset(struct c8_table *t);
/* nearest cube entry, always */
uint8_t c8_cube(uint32_t rgb);
/* the index for a colour without adding it: exact entry or nearest cube */
uint8_t c8_find(const struct c8_table *t, uint32_t rgb);
/* the index for a colour, adding a slot when it needs one */
uint8_t c8_get(struct c8_table *t, uint32_t rgb);
/* The cube index for a picture pixel (0x00RRGGBB) dithered with a 4x4
 * ordered pattern. phase is (y & 3) << 2 | (x & 3) with x and y counted from
 * the picture's own corner, so the pattern moves with the picture. Never
 * adds a slot. */
uint8_t c8_dither(uint32_t rgb, unsigned int phase);
/* the 256 entries of the palette, 0x00RRGGBB; unused slots are black */
void c8_palette(const struct c8_table *t, uint32_t out[256]);
/* the index of slot s (0 to C8_SLOTS - 1) */
uint8_t c8_slot_index(int s);

#endif
