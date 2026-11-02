#ifndef HAVOC_BASE64_H
#define HAVOC_BASE64_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct b64 {
	uint32_t acc;
	unsigned int n;		/* sextets in acc, 0 to 3 */
	bool pad;		/* one more '=' may follow */
};

void b64_init(struct b64 *b);
/* Decode len characters into out, which has room for len / 4 * 3 + 3
 * bytes. '=' completes the current quantum; a new quantum may start after
 * it (clients that pad every chunk). Returns the bytes written, or -1 for
 * a character outside A-Z a-z 0-9 + / =, or '=' with fewer than 2 sextets
 * pending. */
long b64_decode(struct b64 *b, const char *in, size_t len, uint8_t *out);
/* End of the data: 2 or 3 pending sextets give 1 or 2 bytes (unpadded
 * end). Returns 0 to 2, or -1 for a single pending sextet. */
int b64_finish(struct b64 *b, uint8_t *out);

#endif
