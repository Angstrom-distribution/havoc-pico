#ifndef HAVOC_EMOJI_H
#define HAVOC_EMOJI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef EMOJI_DIR
#define EMOJI_DIR "/usr/share/wvkbd/emoji"
#endif

/* Code points below this always come from the font, so text in the Latin,
 * Greek and Cyrillic blocks never depends on a pack. */
#define EMOJI_FIRST 0x2000

/* Emoji packs, the format wvkbd's emoji layers read (wvkbd's emoji-pack.h):
 * emoji-10.pack, emoji-12.pack, emoji-16.pack and emoji-24.pack, each the
 * same emoji at one size, every number little-endian:
 *
 *   offset size  field
 *    0      8    magic "WVKEMOJI"
 *    8      2    version, 1
 *   10      2    header size, 64
 *   12      2    image size N in pixels; the N in the file name
 *   14      2    pixel format, 1: RGB565 + 4-bit alpha
 *   16      4    entry count, 1 to EMOJI_PACK_MAX_COUNT
 *   20      4    table offset, 64
 *   24      4    attribution offset, at or after the end of the table
 *   28      4    attribution length, 0 to EMOJI_PACK_MAX_ATTRIBUTION
 *   32      4    data offset, a multiple of 4, at or after the end of the
 *                attribution and at most EMOJI_PACK_MAX_META
 *   36      4    record size, N * N * 2 + N * emoji_alpha_stride(N)
 *   40     24    set name, NUL-terminated and NUL-padded, A-Z a-z 0-9 . _ + -
 *   64           the table: count entries of {u32 code point, u32 offset},
 *                code points strictly ascending, 1 to 0x10FFFF; an offset
 *                is a multiple of 4, at or after the data offset, and its
 *                record ends inside the file
 *                the attribution: text with no control character other than
 *                newline
 *                the records: N * N straight RGB565 pixels row by row, then
 *                N rows of emoji_alpha_stride(N) bytes of 4-bit alpha, the
 *                even pixel in the high nibble
 */
#define EMOJI_PACK_MAGIC "WVKEMOJI"
#define EMOJI_PACK_VERSION 1
#define EMOJI_PACK_HEADER 64
#define EMOJI_PACK_RGB565_A4 1
#define EMOJI_PACK_NAME 24
#define EMOJI_PACK_MAX_COUNT 4096
#define EMOJI_PACK_MAX_ATTRIBUTION 4096
#define EMOJI_PACK_MAX_META 65536

struct emoji_pack_view {
	unsigned px;
	uint32_t count;
	size_t record;
	const uint8_t *base;
	const uint8_t *table;
};

size_t emoji_alpha_stride(unsigned px);

/* Checks the first `have` bytes of a pack of file_size bytes that should hold
 * images of want_px pixels; `have` must reach the data offset. NULL when the
 * pack is good (v filled in), otherwise why not. */
const char *emoji_pack_check(const uint8_t *buf, size_t have,
			     uint64_t file_size, unsigned want_px,
			     struct emoji_pack_view *v);

/* The directory of the packs: -E, else HAVOC_EMOJI_DIR, else EMOJI_DIR. */
void emoji_set_dir(const char *dir);

/* Whether cp has a picture in the pack for cells of cw x ch pixels: the
 * largest pack that fits two cells wide and one high, or when that one is
 * missing or refused the next smaller one. Reads the pack's header and table
 * the first time, never its images. */
bool emoji_wide(uint32_t cp, int cw, int ch);

/* The record of cp for cells of cw x ch pixels, its size in *px, or NULL.
 * Maps the pack on the first call that needs it. */
const uint8_t *emoji_image(uint32_t cp, int cw, int ch, unsigned *px);

/* Paints a picture over two cells of cw x ch at dst (stride in pixels):
 * the left cell's background is bgl, the right one's bgr (0xRRGGBB), the
 * window opacity premultiplied like the cells around them. */
void emoji_paint(uint32_t *dst, int stride, int cw, int ch,
		 uint32_t bgl, uint32_t bgr, uint8_t opacity,
		 const uint8_t *rec, unsigned px);

#endif
