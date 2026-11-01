#include <string.h>

#include "c8.h"

static const uint8_t level[6] = { 0, 95, 135, 175, 215, 255 };

uint8_t c8_slot_index(int s)
{
	return s < 16 ? s : 232 + (s - 16);
}

static int near_level(unsigned v)
{
	/* the midpoints of the levels */
	return v < 48 ? 0 : v < 115 ? 1 : v < 155 ? 2 : v < 195 ? 3 :
	       v < 235 ? 4 : 5;
}

static uint32_t cube_rgb(unsigned i)
{
	i -= 16;
	return (uint32_t)level[i / 36] << 16 | level[i / 6 % 6] << 8 |
	       level[i % 6];
}

void c8_reset(struct c8_table *t)
{
	memset(t, 0, sizeof(*t));
	t->dirty = true;
	t->last_idx = 0;
	t->last = 0xffffffff;
}

uint8_t c8_cube(uint32_t rgb)
{
	return 16 + 36 * near_level(rgb >> 16 & 255) +
	       6 * near_level(rgb >> 8 & 255) + near_level(rgb & 255);
}

uint8_t c8_find(const struct c8_table *t, uint32_t rgb)
{
	uint8_t c = c8_cube(rgb);
	int i;

	if (cube_rgb(c) == rgb)
		return c;
	for (i = 0; i < t->n; ++i)
		if (t->rgb[i] == rgb)
			return c8_slot_index(i);
	return c;
}

uint8_t c8_get(struct c8_table *t, uint32_t rgb)
{
	uint8_t c;
	int i;

	rgb &= 0xffffff;
	if (rgb == t->last)
		return t->last_idx;
	c = c8_cube(rgb);
	if (cube_rgb(c) != rgb) {
		for (i = 0; i < t->n && t->rgb[i] != rgb; ++i)
			;
		if (i < t->n) {
			c = c8_slot_index(i);
		} else if (t->n < C8_SLOTS) {
			t->rgb[t->n] = rgb;
			c = c8_slot_index(t->n++);
			t->dirty = true;
		} else {
			t->full = true;
		}
	}
	t->last = rgb;
	t->last_idx = c;
	return c;
}

void c8_palette(const struct c8_table *t, uint32_t out[256])
{
	int i;

	memset(out, 0, 256 * sizeof(*out));
	for (i = 16; i < 232; ++i)
		out[i] = cube_rgb(i);
	for (i = 0; i < t->n; ++i)
		out[c8_slot_index(i)] = t->rgb[i];
}
