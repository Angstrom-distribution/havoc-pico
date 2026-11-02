/* Needs a writable /dev/shm and, for the owner case, root; skips (exit 77)
 * where it cannot make its cases. */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#if HAVOC_PNG
#include <zlib.h>
#endif
#include "check.h"
#include "kharness.h"
#include "../kitty-shm.h"

static bool room(void *d, size_t n)
{
	(void)d;
	(void)n;
	return true;
}

static bool exists(const char *name)
{
	char p[400];

	snprintf(p, sizeof p, "/dev/shm/%s", name);
	return access(p, F_OK) == 0;
}

static int make(const char *name, const void *data, size_t n)
{
	char p[400];
	int fd;

	snprintf(p, sizeof p, "/%s", name);
	shm_unlink(p);
	fd = shm_open(p, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (fd < 0)
		return -1;
	if (write(fd, data, n) != (ssize_t)n)
		return -1;
	return fd;
}

static void drop(const char *name)
{
	char p[400];

	snprintf(p, sizeof p, "/%s", name);
	shm_unlink(p);
}

static const char tab[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64(const uint8_t *in, size_t n, char *o)
{
	size_t i;

	for (i = 0; i < n; i += 3) {
		uint32_t v = in[i] << 16 | (i + 1 < n ? in[i + 1] << 8 : 0) |
			     (i + 2 < n ? in[i + 2] : 0);

		*o++ = tab[v >> 18 & 63];
		*o++ = tab[v >> 12 & 63];
		*o++ = i + 1 < n ? tab[v >> 6 & 63] : '=';
		*o++ = i + 2 < n ? tab[v & 63] : '=';
	}
	*o = 0;
}

/* a=T, t=s, f=24 of w x h pixels from the object called name */
static int shm_cmd(struct kitty *k, const char *name, int w, int h, int S)
{
	char nb[600], line[800];

	b64((const uint8_t *)name, strlen(name), nb);
	if (S)
		snprintf(line, sizeof line, "Ga=T,t=s,f=24,s=%d,v=%d,S=%d,i=1;%s", w, h, S, nb);
	else
		snprintf(line, sizeof line, "Ga=T,t=s,f=24,s=%d,v=%d,i=1;%s", w, h, nb);
	return feed_apc(k, line, strlen(line), 0, false);
}

int main(void)
{
	struct kitty_limits lim = { .max_pixels = 100000, .shm = true, .room = room };
	struct kitty *k = kitty_new(&lim);
	struct kitty_shm m;
	uint8_t px[64 * 3];
	char name[200];
	int fd, i;
	pid_t me = getpid();

	for (i = 0; i < (int)sizeof px; i++)
		px[i] = i * 5;
	snprintf(name, sizeof name, "havoc-test-%d", (int)me);

	fd = make(name, px, sizeof px);
	if (fd < 0) {
		printf("test-kitty-shm: cannot make a shm object, skipped\n");
		return 77;
	}
	close(fd);

	/* the direct interface */
	CHECK(kitty_shm_open(name, strlen(name), 64 * 3, 4096, &m) == 0);
	CHECK(m.len == sizeof px && memcmp(m.data, px, sizeof px) == 0);
	CHECK(!exists(name));		/* removed after the read */
	kitty_shm_close(&m);
	CHECK(m.data == NULL);

	/* a missing name */
	CHECK(kitty_shm_open(name, strlen(name), 1, 4096, &m) == -1);
	/* names that are refused, nothing removed */
	fd = make(name, px, sizeof px);
	close(fd);
	CHECK(kitty_shm_open("", 0, 1, 4096, &m) == -1);
	CHECK(kitty_shm_open("a/b", 3, 1, 4096, &m) == -1);
	CHECK(kitty_shm_open("/tmp/x", 6, 1, 4096, &m) == -1);
	CHECK(kitty_shm_open("a b", 3, 1, 4096, &m) == -1);
	CHECK(kitty_shm_open("..", 2, 1, 4096, &m) == -1);
	CHECK(kitty_shm_open(".", 1, 1, 4096, &m) == -1);
	{
		/* the name with a NUL, a slash or a newline after it: the
		 * object of the part before must not be opened */
		char nm[256];
		const char tails[] = { 0, '/', '\n', '\\' };
		size_t l = strlen(name), t;

		for (t = 0; t < sizeof tails; t++) {
			memcpy(nm, name, l);
			nm[l] = tails[t];
			nm[l + 1] = 'x';
			CHECK(kitty_shm_open(nm, l + 2, 1, 4096, &m) == -1);
			CHECK(kitty_shm_open(nm, l + 1, 1, 4096, &m) == -1);
		}
		/* the bytes above 127 */
		memcpy(nm, name, l);
		nm[l] = (char)0xc3;
		CHECK(kitty_shm_open(nm, l + 1, 1, 4096, &m) == -1);
	}
	{
		char long_name[300];

		memset(long_name, 'a', sizeof long_name);
		CHECK(kitty_shm_open(long_name, 256, 1, 4096, &m) == -1);
		CHECK(kitty_shm_open(long_name, 255, 1, 4096, &m) == -1);	/* absent */
	}
	CHECK(exists(name));
	/* too small and too large: not removed */
	CHECK(kitty_shm_open(name, strlen(name), sizeof px + 1, 4096, &m) == -1);
	CHECK(exists(name));
	CHECK(kitty_shm_open(name, strlen(name), 1, sizeof px - 1, &m) == -1);
	CHECK(exists(name));
	CHECK(kitty_shm_open(name, strlen(name), 0, 4096, &m) == -1);
	CHECK(exists(name));

	/* an object of another user: refused and not removed */
	if (getuid() == 0) {
		char p[400];

		snprintf(p, sizeof p, "/dev/shm/%s", name);
		CHECK(chown(p, 65534, 65534) == 0);
		CHECK(kitty_shm_open(name, strlen(name), 1, 4096, &m) == -1);
		CHECK(exists(name));
		CHECK(shm_cmd(k, name, 8, 8, 0) == 1 && res[0].err == KITTY_EBADF_SHM);
		CHECK(exists(name));
		clear_res();
	} else {
		printf("test-kitty-shm: not root, owner case skipped\n");
	}
	drop(name);

	/* a FIFO and a symbolic link in the shm directory */
	{
		char p[400], q[100], fifo[100], tgt[400];

		snprintf(fifo, sizeof fifo, "havoc-fifo-%d", (int)me);
		snprintf(p, sizeof p, "/dev/shm/%s", fifo);
		unlink(p);
		if (mkfifo(p, 0600) == 0) {
			alarm(5);
			CHECK(kitty_shm_open(fifo, strlen(fifo), 1, 4096, &m) == -1);
			CHECK(shm_cmd(k, fifo, 8, 8, 0) == 1 &&
			      res[0].err == KITTY_EBADF_SHM);
			alarm(0);
			CHECK(exists(fifo));
			clear_res();
			unlink(p);
		} else {
			printf("test-kitty-shm: no fifo, skipped\n");
		}
		fd = make(name, px, sizeof px);
		close(fd);
		snprintf(q, sizeof q, "havoc-link-%d", (int)me);
		snprintf(p, sizeof p, "/dev/shm/%s", q);
		unlink(p);
		snprintf(tgt, sizeof tgt, "/dev/shm/%s", name);
		if (symlink(tgt, p) == 0) {
			CHECK(kitty_shm_open(q, strlen(q), 1, 4096, &m) == -1);
			CHECK(exists(q) && exists(name));
			unlink(p);
		} else {
			printf("test-kitty-shm: no symlink, skipped\n");
		}
		drop(name);
	}

	/* through the decoder: good object, picture shown, object gone */
	fd = make(name, px, sizeof px);
	close(fd);
	CHECK(shm_cmd(k, name, 8, 8, 0) == 1);
	CHECK(res[0].err == KITTY_OK && res[0].img.w == 8 && res[0].img.h == 8);
	CHECK(res[0].img.rgb[0] == ((px[0] >> 3) << 11 | (px[1] >> 2) << 5 | px[2] >> 3));
	CHECK(res[0].cmd.a == 'T');
	CHECK(!exists(name));
	clear_res();
	/* mpv sends m=1 with t=s; the command is complete all the same */
	fd = make(name, px, sizeof px);
	close(fd);
	{
		char nb[600], line[800];

		b64((const uint8_t *)name, strlen(name), nb);
		snprintf(line, sizeof line, "Ga=T,t=s,f=24,s=8,v=8,C=1,q=2,m=1;%s", nb);
		CHECK(feed_apc(k, line, strlen(line), 0, false) == 1);
		CHECK(res[0].err == KITTY_OK && res[0].img.w == 8 && !exists(name));
		clear_res();
	}
	/* a larger object than needed: the first s*v*3 bytes */
	{
		uint8_t big[1000];

		memset(big, 0x77, sizeof big);
		memcpy(big, px, sizeof px);
		fd = make(name, big, sizeof big);
		close(fd);
		CHECK(shm_cmd(k, name, 8, 8, 0) == 1 && res[0].err == KITTY_OK);
		CHECK(res[0].img.rgb[63] != 0);
		clear_res();
	}
#if HAVOC_PNG
	/* o=z from shared memory: S bounds what is read, so bytes after it in
	 * the object do not count as data after the stream */
	{
		uint8_t zz[400];
		uLongf zl = sizeof zz - 16;
		char nb[600], line[800];

		CHECK(compress(zz, &zl, px, sizeof px) == Z_OK);
		memcpy(zz + zl, "trailing bytes", 14);
		b64((const uint8_t *)name, strlen(name), nb);
		fd = make(name, zz, zl + 14);
		close(fd);
		snprintf(line, sizeof line, "Ga=T,t=s,f=24,s=8,v=8,o=z,S=%lu,i=1;%s",
			 (unsigned long)zl, nb);
		CHECK(feed_apc(k, line, strlen(line), 0, false) == 1);
		CHECK(res[0].err == KITTY_OK && res[0].img.w == 8 && !exists(name));
		CHECK(res[0].img.rgb && res[0].img.rgb[63] ==
		      ((px[189] >> 3) << 11 | (px[190] >> 2) << 5 | px[191] >> 3));
		clear_res();
		/* without S the whole object is read and the trailing bytes fail */
		fd = make(name, zz, zl + 14);
		close(fd);
		snprintf(line, sizeof line, "Ga=T,t=s,f=24,s=8,v=8,o=z,i=1;%s", nb);
		CHECK(feed_apc(k, line, strlen(line), 0, false) == 1);
		CHECK(res[0].err == KITTY_ENODATA && !exists(name));
		clear_res();
	}
#endif
	/* too small for s * v */
	fd = make(name, px, sizeof px);
	close(fd);
	CHECK(shm_cmd(k, name, 9, 8, 0) == 1 && res[0].err == KITTY_EBADF_SHM);
	CHECK(exists(name));
	clear_res();
	drop(name);
	/* absent, and a name with a slash */
	CHECK(shm_cmd(k, name, 8, 8, 0) == 1 && res[0].err == KITTY_EBADF_SHM);
	clear_res();
	CHECK(shm_cmd(k, "../etc/passwd", 8, 8, 0) == 1 && res[0].err == KITTY_EBADF_SHM);
	clear_res();
	/* a name of 300 bytes */
	{
		char n300[301];

		memset(n300, 'a', 300);
		n300[300] = 0;
		CHECK(shm_cmd(k, n300, 8, 8, 0) == 1 && res[0].err == KITTY_EBADF_SHM);
		clear_res();
	}
	/* above the cap: the object is larger than 4 * max_pixels */
	{
		static uint8_t huge[4 * 100000 + 8];

		fd = make(name, huge, sizeof huge);
		close(fd);
		CHECK(shm_cmd(k, name, 8, 8, 0) == 1 && res[0].err == KITTY_EBADF_SHM);
		CHECK(exists(name));
		clear_res();
		drop(name);
	}
	/* more pixels than the cap: ENOMEM as for t=d, the object stays */
	{
		static uint8_t over[3 * 101000];

		fd = make(name, over, sizeof over);
		close(fd);
		CHECK(shm_cmd(k, name, 100001, 1, 0) == 1 && res[0].err == KITTY_EINVAL);
		clear_res();
		CHECK(shm_cmd(k, name, 1000, 101, 0) == 1 && res[0].err == KITTY_ENOMEM);
		CHECK(exists(name));
		clear_res();
		drop(name);
	}
	/* shm off */
	lim.shm = false;
	kitty_set_limits(k, &lim);
	fd = make(name, px, sizeof px);
	close(fd);
	CHECK(shm_cmd(k, name, 8, 8, 0) == 1 && res[0].err == KITTY_EBADF);
	CHECK(exists(name));
	clear_res();
	drop(name);
	kitty_free(k);
	CHECK_DONE("test-kitty-shm");
}
