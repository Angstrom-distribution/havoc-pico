#ifndef TEST_CHECK_H
#define TEST_CHECK_H

#include <stdio.h>

static int checks, failed;

#define CHECK(cond) do { \
	checks++; \
	if (!(cond)) { \
		failed++; \
		fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #cond); \
	} \
} while (0)

#define CHECK_DONE(name) do { \
	printf("%s: %d checks, %d failed\n", name, checks, failed); \
	return failed ? 1 : 0; \
} while (0)

#endif
