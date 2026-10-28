/*
T_STDLIB.C

The rest of the 9x build's C library the game calls: qsort (PDCLib's, as
the Xbox's: sorted, the same elements, and how it orders equal keys),
bsearch, the MSVC string extras (port/win9x/src/w9x_crt.c), strtol and
family, getenv over GetEnvironmentVariableA, memcpy and friends
(port/xbox/src/nx_string.c) on every alignment, and the 64-bit
arithmetic helpers (nxdk's _alldiv and family).
*/

#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "w9x_test.h"

char *_strlwr(char *string);
char *_strupr(char *string);
char *_itoa(int value, char *string, int radix);
char *_ltoa(long value, char *string, int radix);
char *_ultoa(unsigned long value, char *string, int radix);
void _splitpath(const char *path, char *drive, char *directory, char *name, char *extension);
void _makepath(char *path, const char *drive, const char *directory, const char *name, const char *extension);

struct record
{
	int key;
	int order;
};

static int compare_records(const void *a, const void *b)
{
	const struct record *x = a, *y = b;

	return x->key < y->key ? -1 : x->key > y->key;
}

static int compare_ints(const void *a, const void *b)
{
	int x = *(const int *)a, y = *(const int *)b;

	return x < y ? -1 : x > y;
}

static void test_qsort(void)
{
	static struct record records[5000];
	static int values[20000], sorted_count[64];
	int n, i, stable = 1, ordered = 1, count;

	for (count = 0; count <= 4000; count += count < 40 ? 1 : 397)
	{
		long long sum = 0, sum_after = 0;

		for (i = 0; i < count; i++)
		{
			records[i].key = (int)(test_random() % 50);
			records[i].order = i;
			sum += records[i].key * 7919LL + records[i].order;
		}
		qsort(records, (size_t)count, sizeof(records[0]), compare_records);
		for (i = 0; i < count; i++)
		{
			sum_after += records[i].key * 7919LL + records[i].order;
			if (i && records[i - 1].key > records[i].key)
				ordered = 0;
			if (i && records[i - 1].key == records[i].key && records[i - 1].order > records[i].order)
				stable = 0;
		}
		CHECK(sum == sum_after);
	}
	CHECK(ordered);
	printf("# qsort keeps equal keys in their order: %s (PDCLib's, as the Xbox build's; the UCRT's and glibc's "
		"differ again)\n", stable ? "yes" : "no");
	for (n = 0; n < 20; n++)
	{
		for (i = 0; i < 20000; i++)
			values[i] = (int)test_random();
		qsort(values, 20000, sizeof(int), compare_ints);
		for (i = 1; i < 20000; i++)
		{
			if (values[i - 1] > values[i])
				break;
		}
		CHECK(i == 20000);
	}
	{
		int key = values[12345];

		CHECK(bsearch(&key, values, 20000, sizeof(int), compare_ints) != NULL);
	}
	(void)sorted_count;
}

static void test_strings(void)
{
	char buffer[260], drive[8], directory[260], name[260], extension[260];
	char *copy;

	CHECK(_stricmp("Halo.Map", "halo.MAP") == 0);
	CHECK(_stricmp("a", "B") < 0 && _stricmp("b", "A") > 0);
	CHECK(_strnicmp("LevelS\\a10", "levels\\B30", 7) == 0);
	CHECK(_strnicmp("abc", "abd", 3) < 0);
	strcpy(buffer, "MiXeD 123");
	CHECK(!strcmp(_strlwr(buffer), "mixed 123"));
	CHECK(!strcmp(_strupr(buffer), "MIXED 123"));
	CHECK(!strcmp(_itoa(-12345, buffer, 10), "-12345"));
	CHECK(!strcmp(_itoa(255, buffer, 16), "ff"));
	CHECK(!strcmp(_ltoa(-2147483647L - 1, buffer, 10), "-2147483648"));
	CHECK(!strcmp(_ultoa(4294967295UL, buffer, 10), "4294967295"));
	CHECK(!strcmp(_ultoa(5, buffer, 2), "101"));
	_splitpath("c:\\games\\halo\\maps\\a10.map", drive, directory, name, extension);
	CHECK(!strcmp(drive, "c:") && !strcmp(directory, "\\games\\halo\\maps\\") && !strcmp(name, "a10") &&
		!strcmp(extension, ".map"));
	_splitpath("saves/profile.bin", drive, directory, name, extension);
	CHECK(!strcmp(drive, "") && !strcmp(directory, "saves/") && !strcmp(name, "profile") &&
		!strcmp(extension, ".bin"));
	_makepath(buffer, "d", "maps", "ui", "map");
	CHECK(!strcmp(buffer, "d:maps\\ui.map"));
	copy = strdup("duplicated");
	CHECK(copy && !strcmp(copy, "duplicated"));
	free(copy);
	CHECK(strtol("  -0x1F", NULL, 0) == -31);
	CHECK(strtol("2147483648", NULL, 10) == 2147483647L);
	CHECK(strtoul("4294967295", NULL, 10) == 4294967295UL);
	CHECK(strtoll("-9223372036854775808", NULL, 10) == -9223372036854775807LL - 1);
	CHECK(strtoull("0xFFFFFFFFFFFFFFFF", NULL, 16) == 0xFFFFFFFFFFFFFFFFULL);
	CHECK(atoi("  77z") == 77);
}

static void test_memory(void)
{
	static unsigned char source[600], target[700], expected[700];
	int offset, from, size;

	for (offset = 0; offset < 600; offset++)
		source[offset] = (unsigned char)(offset * 7 + 3);
	for (from = 0; from < 8; from++)
	{
		for (offset = 0; offset < 8; offset++)
		{
			for (size = 0; size < 300; size += size < 40 ? 1 : 37)
			{
				memset(target, 0xAA, sizeof(target));
				memset(expected, 0xAA, sizeof(expected));
				memcpy(target + offset, source + from, (size_t)size);
				for (int i = 0; i < size; i++)
					expected[offset + i] = source[from + i];
				CHECK(!memcmp(target, expected, sizeof(target)));
				memset(target + offset, from, (size_t)size);
				for (int i = 0; i < size; i++)
					expected[offset + i] = (unsigned char)from;
				CHECK(!memcmp(target, expected, sizeof(target)));
			}
		}
	}
	/* overlapping moves, both ways */
	for (offset = -8; offset <= 8; offset++)
	{
		for (size = 0; size < 200; size += 13)
		{
			for (int i = 0; i < 700; i++)
				target[i] = expected[i] = (unsigned char)i;
			memmove(target + 300 + offset, target + 300, (size_t)size);
			for (int i = size - 1; offset > 0 && i >= 0; i--)
				expected[300 + offset + i] = expected[300 + i];
			for (int i = 0; offset <= 0 && i < size; i++)
				expected[300 + offset + i] = expected[300 + i];
			CHECK(!memcmp(target, expected, sizeof(target)));
		}
	}
	CHECK(memcmp("abcd", "abce", 4) < 0 && memcmp("abce", "abcd", 4) > 0 && memcmp("ab", "ab", 2) == 0);
}

static void test_arithmetic(void)
{
	volatile long long a = 1234567890123456789LL, b = -987654321LL;
	volatile unsigned long long c = 0xFEDCBA9876543210ULL, d = 0x12345ULL;

	CHECK(a / b == -1249999988LL);
	CHECK(a % b == 1234567890123456789LL - (-1249999988LL) * -987654321LL);
	CHECK(c / d == 0xFEDCBA9876543210ULL / 0x12345ULL);
	CHECK(c % d == 0xFEDCBA9876543210ULL % 0x12345ULL);
	CHECK((a << 7) == (long long)((unsigned long long)a << 7));
	CHECK((b >> 9) == -1929013LL);
	CHECK((c >> 33) == 0x7F6E5D4CULL);
	CHECK(a * b == 1234567890123456789LL * -987654321LL);
}

static void test_environment(void)
{
	char *value;

	SetEnvironmentVariableA("HALO_W9X_TEST", "first value");
	value = getenv("HALO_W9X_TEST");
	CHECK(value && !strcmp(value, "first value"));
	CHECK(getenv("halo_w9x_test") == value);
	SetEnvironmentVariableA("HALO_W9X_TEST", "second");
	CHECK(value && !strcmp(value, "first value")); /* (an earlier answer stays valid) */
	value = getenv("HALO_W9X_TEST");
	CHECK(value && !strcmp(value, "second"));
	CHECK(getenv("HALO_W9X_TEST_UNSET_NAME") == NULL);
	value = getenv("PATH");
	printf("# PATH is %u bytes\n", value ? (unsigned)strlen(value) : 0u);
	CHECK(value != NULL);
}

int main(void)
{
	test_qsort();
	test_strings();
	test_memory();
	test_arithmetic();
	test_environment();
	return test_finish("t_stdlib");
}
