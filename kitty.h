#ifndef HAVOC_KITTY_H
#define HAVOC_KITTY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum kitty_err {
	KITTY_OK,
	KITTY_EINVAL,
	KITTY_EBADF,		/* t=f, t=t, or t=s while off */
	KITTY_EBADF_SHM,	/* t=s that could not be used, any reason */
	KITTY_ENOMEM,
	KITTY_ENODATA,
	KITTY_ENOENT,
};

/* The keys of a command. Absent keys hold the protocol defaults: a 't',
 * t 'd', d 'a', f 32, o 0, everything else 0. */
struct kitty_cmd {
	uint64_t keys;		/* bit k - 'A': key k given */
	char a, t, d, o;
	uint32_t f, i, I, p, s, v, S, O, x, y, w, h, X, Y, c, r, C, q, m, U;
	int32_t z;
};
#define KITTY_HAS(cmd, k)	((cmd)->keys >> ((k) - 'A') & 1)

struct kitty_image {
	unsigned int w, h;	/* 0: none */
	uint16_t *rgb;		/* w * h RGB565 */
	uint8_t *alpha;		/* w * h, NULL when every pixel is opaque */
};

struct kitty_limits {
	size_t max_pixels;	/* the cap: a picture above it is ENOMEM */
	bool shm;		/* t=s allowed ([graphics] kitty shm) */
	/* Asked once the size is known, before any pixel buffer: may evict
	 * to make room; false gives ENOMEM. NULL: always true. */
	bool (*room)(void *data, size_t pixels);
	void *data;
};

struct kitty_result {
	struct kitty_cmd cmd;	/* keys of the first chunk, q of the last */
	enum kitty_err err;
	struct kitty_image img;	/* a=t, a=T with err OK; owned by the caller */
};

struct kitty;

struct kitty *kitty_new(const struct kitty_limits *lim);
void kitty_set_limits(struct kitty *k, const struct kitty_limits *lim);
void kitty_free(struct kitty *k);
/* Drop a transfer in progress (hard reset). */
void kitty_reset(struct kitty *k);

/* One APC string: kitty_put with its bytes in any split (the first byte
 * of the string is 'G'; anything else makes kitty_put return false and the
 * string is not kitty's), then kitty_end, which returns how many commands
 * finished with it, 0 to 2, in out[] in order: 0 after a chunk with m=1
 * (more to come; m counts only with t=d); 1 for the last chunk or a single command; 2 when this
 * APC was a new command that ended an unfinished chunked transfer (out[0]
 * is that transfer with ENODATA, out[1] the new command, unless it has
 * m=1 itself: then only out[0]). aborted: the APC was cut; a transfer in
 * progress is dropped without a result. */
bool kitty_put(struct kitty *k, const char *buf, size_t len);
int kitty_end(struct kitty *k, bool aborted, struct kitty_result out[2]);
void kitty_result_free(struct kitty_result *r);

/* The reply for cmd and err into buf (at least 128 bytes): 0 when no
 * reply is due (no i, q=2, or q=1 and err OK). */
size_t kitty_format_reply(const struct kitty_cmd *cmd, enum kitty_err err,
			  char *buf, size_t len);

#endif
