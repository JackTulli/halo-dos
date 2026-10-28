/*
T_CRT.C

The DOS build's C runtime on DOS: printf with floating point, files
(write, read back, seek, delete), malloc of large blocks, the clock, the
command line and the environment. Prints what it found and "ok" or the
first failure; run it in DOS (or DOSBox) with its output redirected.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "dos_dpmi.h"

unsigned long dos_heap_size(void);

static int failures;

#define CHECK(condition) do { if (!(condition)) { printf("FAILED: %s (line %d)\n", #condition, __LINE__); \
	failures++; } } while (0)

int main(int argc, char **argv)
{
	char buffer[256];
	FILE *file;
	unsigned long long start;
	void *blocks[8];

	printf("argc %d, argv[0] %s\n", argc, argv[0]);
	for (int i = 1; i < argc; i++)
		printf("argv[%d] %s\n", i, argv[i]);
	printf("heap %lu KB, clock %llu Hz\n", dos_heap_size() / 1024, dos_tsc_frequency);
	printf("PATH=%s\n", getenv("PATH") ? getenv("PATH") : "(none)");
	snprintf(buffer, sizeof(buffer), "%.3f %g %e %d %s", 3.14159, 0.000125, 12345.678, -42, "x");
	printf("%s\n", buffer);
	CHECK(!strcmp(buffer, "3.142 0.000125 1.234568e+04 -42 x"));

	file = fopen("T_RUN.TMP", "wb");
	CHECK(file != NULL);
	if (file)
	{
		for (int i = 0; i < 100000; i++)
			fputc(i & 0xFF, file);
		fclose(file);
	}
	file = fopen("T_RUN.TMP", "rb");
	CHECK(file != NULL);
	if (file)
	{
		long size;

		fseek(file, 0, SEEK_END);
		size = ftell(file);
		CHECK(size == 100000);
		fseek(file, 70000, SEEK_SET);
		CHECK(fgetc(file) == (70000 & 0xFF));
		fclose(file);
	}
	CHECK(remove("T_RUN.TMP") == 0);
	CHECK(fopen("T_RUN.TMP", "rb") == NULL);

	for (int i = 0; i < 8; i++)
	{
		blocks[i] = malloc(2 * 1024 * 1024);
		CHECK(blocks[i] != NULL);
		if (blocks[i])
			memset(blocks[i], i, 2 * 1024 * 1024);
	}
	for (int i = 0; i < 8; i++)
		free(blocks[i]);

	start = dos_microseconds();
	while (dos_microseconds() - start < 100000)
		;
	printf("time %ld, clock() %ld\n", (long)time(NULL), (long)clock());

	printf(failures ? "%d FAILED\n" : "ok\n", failures);
	return failures ? 1 : 0;
}
