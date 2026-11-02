/* Kitty t=s: read a picture from POSIX shared memory. The name comes from
 * the program on the other end of the pty, so it is checked hard: no
 * directories, no symbolic links, no pipes, only an object of our own user,
 * and only after those checks is it removed. */
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "kitty-shm.h"

#define NAME_MAX_LEN	255

int kitty_shm_open(const char *name, size_t namelen, size_t need, size_t max,
		   struct kitty_shm *out)
{
	char path[NAME_MAX_LEN + 2];
	struct stat st;
	void *p;
	size_t i;
	int fd;

	out->data = NULL;
	out->len = 0;
	if (namelen < 1 || namelen > NAME_MAX_LEN || need < 1)
		return -1;
	for (i = 0; i < namelen; i++) {
		char c = name[i];

		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		      (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
			return -1;
	}
	if (name[0] == '.' && (namelen == 1 || (namelen == 2 && name[1] == '.')))
		return -1;
	path[0] = '/';
	memcpy(path + 1, name, namelen);
	path[namelen + 1] = 0;

	/* O_NONBLOCK: a pipe in the shm directory must not block the open */
	fd = shm_open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_uid != getuid() ||
	    st.st_size < 0 || (size_t)st.st_size < need || (size_t)st.st_size > max) {
		close(fd);
		return -1;
	}
	shm_unlink(path);
	p = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (p == MAP_FAILED)
		return -1;
	out->data = p;
	out->len = st.st_size;
	return 0;
}

void kitty_shm_close(struct kitty_shm *m)
{
	if (m->data)
		munmap((void *)m->data, m->len);
	m->data = NULL;
	m->len = 0;
}
