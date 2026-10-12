/* Copyright (c) 2026 Koen Kooi. MIT licence, as in LICENSE.
 *
 * Emoji from wvkbd's pack files: a GPU-less handheld has neither a colour
 * emoji font nor the memory to render one, and the keyboard that types the
 * emoji already installs their pictures. A pack is untrusted input: every
 * number in it is checked before it is used, and a pack that fails a check is
 * not used at all. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "emoji.h"

#define NSIZES 4
static const unsigned sizes[NSIZES] = { 10, 12, 16, 24 };

static uint16_t le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}

static bool name_char(uint8_t c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
	       (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '+' ||
	       c == '-';
}

size_t emoji_alpha_stride(unsigned px)
{
	return (((size_t)px * 4 + 7) / 8 + 3) & ~(size_t)3;
}

const char *emoji_pack_check(const uint8_t *buf, size_t have,
			     uint64_t file_size, unsigned want_px,
			     struct emoji_pack_view *v)
{
	if (file_size == 0)
		return "empty file";
	if (have > file_size)
		have = file_size;
	if (file_size < EMOJI_PACK_HEADER || have < EMOJI_PACK_HEADER)
		return "truncated header";
	if (memcmp(buf, EMOJI_PACK_MAGIC, 8) != 0)
		return "not an emoji pack (bad magic)";
	if (le16(buf + 8) != EMOJI_PACK_VERSION)
		return "unsupported version";
	if (le16(buf + 10) != EMOJI_PACK_HEADER)
		return "bad header size";
	unsigned px = le16(buf + 12);
	if (px != want_px)
		return "image size does not match the file name";
	if (le16(buf + 14) != EMOJI_PACK_RGB565_A4)
		return "unsupported pixel format";

	uint32_t count = le32(buf + 16), table = le32(buf + 20);
	uint32_t attr = le32(buf + 24), attr_len = le32(buf + 28);
	uint32_t data = le32(buf + 32), record = le32(buf + 36);
	/* 64-bit sums below, so no count or offset can wrap a check */
	if (count == 0 || count > EMOJI_PACK_MAX_COUNT)
		return "bad entry count";
	if (table != EMOJI_PACK_HEADER)
		return "bad table offset";
	uint64_t table_end = (uint64_t)table + (uint64_t)count * 8;
	if (attr_len > EMOJI_PACK_MAX_ATTRIBUTION)
		return "attribution too long";
	if (attr < table_end || (uint64_t)attr + attr_len > data)
		return "attribution overlaps the table or the data";
	if (data % 4 || data > EMOJI_PACK_MAX_META)
		return "bad data offset";
	if (data > file_size)
		return "truncated before the data";
	if (have < data)
		return "header and table not read";
	if (record != px * px * 2 + px * emoji_alpha_stride(px))
		return "bad record size";

	const char *name = (const char *)buf + 40;
	size_t name_len = strnlen(name, EMOJI_PACK_NAME);
	if (name_len == 0 || name_len == EMOJI_PACK_NAME)
		return "bad set name";
	for (size_t i = 0; i < EMOJI_PACK_NAME; i++)
		if (i < name_len ? !name_char((uint8_t)name[i]) : name[i] != 0)
			return "bad set name";

	/* the same rule as wvkbd, which prints it, so a pack one of them
	 * takes the other takes too */
	for (uint32_t i = 0; i < attr_len; i++) {
		uint8_t c = buf[attr + i];
		if ((c < 0x20 && c != '\n') || c == 0x7f)
			return "bad attribution text";
	}

	uint32_t prev = 0;
	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *e = buf + table + (size_t)i * 8;
		uint32_t cp = le32(e), off = le32(e + 4);
		if (cp == 0 || cp > 0x10ffff)
			return "bad code point";
		if (i > 0 && cp == prev)
			return "duplicate code point";
		if (i > 0 && cp < prev)
			return "code points not sorted";
		prev = cp;
		if (off % 4)
			return "misaligned image";
		if (off < data)
			return "image inside the header";
		if ((uint64_t)off + record > file_size)
			return "image outside the file";
	}

	if (v) {
		v->px = px;
		v->count = count;
		v->record = record;
		v->base = buf;
		v->table = buf + table;
	}
	return NULL;
}

/* The offset of cp's record, 0 when the pack has none (no record can be at
 * 0, the header is there). */
static uint32_t find(const struct emoji_pack_view *v, uint32_t cp)
{
	uint32_t lo = 0, hi = v->count;

	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		uint32_t c = le32(v->table + (size_t)mid * 8);

		if (c == cp)
			return le32(v->table + (size_t)mid * 8 + 4);
		if (c < cp)
			lo = mid + 1;
		else
			hi = mid;
	}
	return 0;
}

enum pack_state { PACK_UNKNOWN, PACK_ABSENT, PACK_BAD, PACK_GOOD, PACK_MAPPED };

static struct pack {
	enum pack_state state;
	/* the header and table as probed: the width of a code point is needed
	 * when it is written, its picture only when it is drawn */
	uint8_t *meta;
	struct emoji_pack_view probed;
	void *map;
	size_t len;
	struct emoji_pack_view mapped;
} packs[NSIZES];

static const char *dir_opt;
static bool said;

void emoji_set_dir(const char *dir)
{
	dir_opt = dir;
}

static const char *pack_dir(void)
{
	const char *env;

	if (dir_opt)
		return dir_opt;
	env = getenv("HAVOC_EMOJI_DIR");
	return env && *env ? env : EMOJI_DIR;
}

static bool pack_path(int i, char *path, size_t size)
{
	int n = snprintf(path, size, "%s/emoji-%u.pack", pack_dir(), sizes[i]);

	return n > 0 && (size_t)n < size;
}

/* One line for each pack that is refused, since a refused pack is a broken
 * installation and nothing else would say why the emoji are missing. */
static void pack_reject(int i, const char *why)
{
	char path[PATH_MAX];

	packs[i].state = PACK_BAD;
	free(packs[i].meta);
	packs[i].meta = NULL;
	if (!pack_path(i, path, sizeof path))
		snprintf(path, sizeof path, "emoji-%u.pack", sizes[i]);
	fprintf(stderr, "havoc: emoji pack %s not used: %s\n", path, why);
	said = true;
}

/* O_NONBLOCK keeps a FIFO put in the pack's place from blocking. */
static int pack_open(int i, size_t *size)
{
	char path[PATH_MAX];
	struct stat st;
	const char *why = NULL;
	int fd;

	if (!pack_path(i, path, sizeof path)) {
		pack_reject(i, "path too long");
		return -1;
	}
	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
	if (fd < 0) {
		if (errno == ENOENT || errno == ENOTDIR)
			packs[i].state = PACK_ABSENT;
		else
			pack_reject(i, strerror(errno));
		return -1;
	}
	if (fstat(fd, &st) < 0)
		why = strerror(errno);
	else if (!S_ISREG(st.st_mode))
		why = "not a regular file";
	else if (st.st_size == 0)
		why = "empty file";
	else if ((uint64_t)st.st_size > UINT32_MAX)
		why = "too large";
	if (why) {
		close(fd);
		pack_reject(i, why);
		return -1;
	}
	*size = (size_t)st.st_size;
	return fd;
}

static const char *read_at(int fd, uint8_t *buf, size_t from, size_t to)
{
	while (from < to) {
		ssize_t n = pread(fd, buf + from, to - from, (off_t)from);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return n < 0 ? strerror(errno) : "truncated while reading";
		from += (size_t)n;
	}
	return NULL;
}

/* Reads only the bytes before the data offset, so a terminal that never
 * shows an emoji reads no image off the flash. */
static enum pack_state pack_probe(int i)
{
	struct pack *p = &packs[i];
	uint8_t head[EMOJI_PACK_HEADER];
	uint8_t *buf = head;
	size_t size, have;
	const char *why;
	int fd;

	if (p->state != PACK_UNKNOWN)
		return p->state;
	fd = pack_open(i, &size);
	if (fd < 0)
		return p->state;
	have = size < EMOJI_PACK_HEADER ? size : EMOJI_PACK_HEADER;
	why = read_at(fd, head, 0, have);
	if (!why && have == EMOJI_PACK_HEADER) {
		/* a data offset the check refuses is not read; the header alone
		 * gets the check its reason */
		uint32_t data = le32(head + 32);

		if (data > EMOJI_PACK_HEADER && data <= EMOJI_PACK_MAX_META &&
		    data <= size) {
			buf = malloc(data);
			if (!buf) {
				buf = head;
				why = "out of memory";
			} else {
				memcpy(buf, head, EMOJI_PACK_HEADER);
				have = data;
				why = read_at(fd, buf, EMOJI_PACK_HEADER, have);
			}
		}
	}
	close(fd);
	if (!why)
		why = emoji_pack_check(buf, have, size, sizes[i], &p->probed);
	if (why) {
		if (buf != head)
			free(buf);
		pack_reject(i, why);
		return p->state;
	}
	/* a good pack always has a data offset past the header, so buf is
	 * the heap copy */
	p->meta = buf;
	p->state = PACK_GOOD;
	return p->state;
}

/* Checked again on the mapping, because the file may have been replaced
 * since the probe; kept for the life of the process. */
static bool pack_map(int i)
{
	struct pack *p = &packs[i];
	const char *why;
	size_t size;
	void *map;
	int fd, err;

	if (p->state == PACK_MAPPED)
		return true;
	if (pack_probe(i) != PACK_GOOD)
		return false;
	fd = pack_open(i, &size);
	if (fd < 0) {
		if (p->state == PACK_ABSENT)
			pack_reject(i, "removed since it was read");
		return false;
	}
	map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	err = errno;
	close(fd);
	if (map == MAP_FAILED) {
		pack_reject(i, strerror(err));
		return false;
	}
	why = emoji_pack_check(map, size, size, sizes[i], &p->mapped);
	if (why) {
		munmap(map, size);
		pack_reject(i, why);
		return false;
	}
	p->map = map;
	p->len = size;
	p->state = PACK_MAPPED;
	free(p->meta);
	p->meta = NULL;
	return true;
}

static int pack_pick(int cw, int ch)
{
	int i, want = -1;

	for (i = 0; i < NSIZES; i++)
		if ((int)sizes[i] <= 2 * cw && (int)sizes[i] <= ch)
			want = i;
	/* a smaller picture leaves a margin; a larger one would be cut off */
	for (i = want; i >= 0; i--)
		if (pack_probe(i) >= PACK_GOOD)
			return i;
	if (!said) {
		if (want < 0)
			fprintf(stderr, "havoc: no emoji: cells of %dx%d px "
				"are too small for the smallest pack (%u px)\n",
				cw, ch, sizes[0]);
		else
			fprintf(stderr, "havoc: no emoji: no usable pack of "
				"at most %u px in %s\n", sizes[want],
				pack_dir());
		said = true;
	}
	return -1;
}

bool emoji_wide(uint32_t cp, int cw, int ch)
{
	int i;

	if (cp < EMOJI_FIRST)
		return false;
	i = pack_pick(cw, ch);
	if (i < 0)
		return false;
	return find(packs[i].state == PACK_MAPPED ? &packs[i].mapped
						  : &packs[i].probed, cp) != 0;
}

const uint8_t *emoji_image(uint32_t cp, int cw, int ch, unsigned *px)
{
	uint32_t off;
	int i;

	if (cp < EMOJI_FIRST)
		return NULL;
	/* a pack that fails now is marked bad, and the next pick is the
	 * nearest smaller size */
	do {
		i = pack_pick(cw, ch);
		if (i < 0)
			return NULL;
		/* text that is not in the pack, CJK say, maps nothing */
		if (!find(packs[i].state == PACK_MAPPED ? &packs[i].mapped
							: &packs[i].probed, cp))
			return NULL;
	} while (!pack_map(i));

	off = find(&packs[i].mapped, cp);
	if (!off)
		return NULL;
	*px = sizes[i];
	return packs[i].mapped.base + off;
}

#define mul(a, b) (((uint32_t)(a) * (uint32_t)(b) + 255) >> 8)
#define join(a, r, g, b) ((uint32_t)(a) << 24 | (uint32_t)(r) << 16 | \
			  (uint32_t)(g) << 8 | (uint32_t)(b))

/* the same premultiplied pixel main.c's blank() paints, so the margin of the
 * picture cannot be told from an empty cell next to it */
static uint32_t cell_bg(uint32_t rgb, uint8_t a)
{
	return join(a, mul(rgb >> 16 & 0xff, a), mul(rgb >> 8 & 0xff, a),
		    mul(rgb & 0xff, a));
}

void emoji_paint(uint32_t *dst, int stride, int cw, int ch,
		 uint32_t bgl, uint32_t bgr, uint8_t opacity,
		 const uint8_t *rec, unsigned px)
{
	const uint8_t *alpha = rec + (size_t)px * px * 2;
	size_t astride = emoji_alpha_stride(px);
	uint32_t l = cell_bg(bgl, opacity), r = cell_bg(bgr, opacity);
	int w = 2 * cw;
	int ox = (2 * cw - (int)px) / 2, oy = (ch - (int)px) / 2;
	int x, y;

	for (y = 0; y < ch; ++y)
		for (x = 0; x < w; ++x)
			dst[y * stride + x] = x < cw ? l : r;

	for (y = 0; y < (int)px; ++y) {
		int dy = oy + y;

		if (dy < 0 || dy >= ch)
			continue;
		for (x = 0; x < (int)px; ++x) {
			int dx = ox + x;
			uint8_t a4 = alpha[y * astride + x / 2] >>
				     (x & 1 ? 0 : 4) & 0xf;
			const uint8_t *c = rec + ((size_t)y * px + x) * 2;
			uint16_t v = le16(c);
			uint8_t fr, fg, fb, fa, ca;
			uint32_t *d, b;

			if (dx < 0 || dx >= w || a4 == 0)
				continue;
			/* bit replication, so white stays 0xff */
			fr = (v >> 11) << 3 | (v >> 13);
			fg = (v >> 5 & 0x3f) << 2 | (v >> 9 & 0x3);
			fb = (v & 0x1f) << 3 | (v >> 2 & 0x7);
			d = &dst[dy * stride + dx];
			if (a4 == 0xf) {
				*d = join(0xff, fr, fg, fb);
				continue;
			}
			/* the blend of main.c's print(), with the picture's
			 * alpha for the glyph's coverage */
			fa = a4 * 17;
			ca = 255 - fa;
			b = *d;
			*d = join(fa + mul(b >> 24, ca),
				  mul(fr, fa) + mul(b >> 16 & 0xff, ca),
				  mul(fg, fa) + mul(b >> 8 & 0xff, ca),
				  mul(fb, fa) + mul(b & 0xff, ca));
		}
	}
}
