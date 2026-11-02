#ifndef HAVOC_KITTY_SHM_H
#define HAVOC_KITTY_SHM_H

#include <stddef.h>
#include <stdint.h>

struct kitty_shm {
	const uint8_t *data;
	size_t len;		/* the object's size */
};

/* name, namelen: as decoded from the payload, not NUL terminated. Checks
 * the name (1 to 255 bytes of A-Z a-z 0-9 . _ -), opens "/" name with
 * O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC, fstat: regular file,
 * owner getuid(), size within need..max; then shm_unlink, mmap read-only
 * MAP_PRIVATE, close. Returns 0, or -1 for every failure alike; nothing
 * is unlinked on a failure before the fstat checks passed. */
int kitty_shm_open(const char *name, size_t namelen, size_t need, size_t max,
		   struct kitty_shm *out);
void kitty_shm_close(struct kitty_shm *m);

#endif
