/* libFuzzer target: the input is a series of APC strings separated by
 * ESC \, each fed in pieces. Build with HAVOC_PNG=1. */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../kitty.h"

static bool room(void *d, size_t n)
{
	(void)d;
	return n < 90000;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct kitty_limits lim = { .max_pixels = 100000, .shm = false, .room = room };
	struct kitty *k = kitty_new(&lim);
	unsigned int piece = 1 + (size ? data[0] : 0) * 3;
	size_t i = size ? 1 : 0, start = i;

	if (!k)
		return 0;
	while (i <= size) {
		bool st = i + 1 < size && data[i] == 0x1b && data[i + 1] == '\\';

		if (st || i == size) {
			size_t pos = start;
			bool ok = true;
			struct kitty_result out[2];
			int n, j;

			while (pos < i && ok) {
				size_t c = i - pos < piece ? i - pos : piece;

				ok = kitty_put(k, (const char *)data + pos, c);
				pos += c;
			}
			n = kitty_end(k, !ok || (i == size && size && data[size - 1] == 0x18), out);
			for (j = 0; j < n; j++) {
				char buf[128];

				kitty_format_reply(&out[j].cmd, out[j].err, buf, sizeof buf);
				kitty_result_free(&out[j]);
			}
			start = i + 2;
			i += 2;
			continue;
		}
		i++;
	}
	kitty_free(k);
	return 0;
}
