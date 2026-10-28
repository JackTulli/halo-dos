/*
W9X_TEST.H

The checks of the 9x build's C runtime tests (port/win9x/tests, built by
python3 tools/win9x_build.py --dev). A test prints what it finds on
standard output; lines starting with '#' are notes, the rest is compared
with the same program built against the UCRT where there is such a
reference (tools/win9x_build.py, REFERENCE_TESTS). A failed check prints a
FAIL line; the program's exit code is the number of failures.
*/

#ifndef __W9X_TEST_H
#define __W9X_TEST_H

#include <stdio.h>

static int test_failures;
static int test_checks;

#define CHECK(condition) test_check((condition) != 0, #condition, __FILE__, __LINE__)

static int test_check(int passed, const char *text, const char *file, int line)
{
	test_checks++;
	if (!passed)
	{
		test_failures++;
		printf("# FAIL %s:%d: %s\n", file, line, text);
	}
	return passed;
}

static int test_finish(const char *name)
{
	printf("# %s: %d checks, %d failed\n", name, test_checks, test_failures);
	fflush(stdout);
	return test_failures;
}

/* a deterministic random sequence (xorshift64), the same in both builds */
static unsigned long long test_random_state = 88172645463325252ULL;

static unsigned long long test_random(void)
{
	test_random_state ^= test_random_state << 13;
	test_random_state ^= test_random_state >> 7;
	test_random_state ^= test_random_state << 17;
	return test_random_state;
}

#endif
