/*
T_CHKSTK.C

The 9x build's __chkstk (port/win9x/src/w9x_chkstk.s): a function with a
1 MB local array, another whose frames of 64 KB recurse, and a 512 KB
alloca, on the main thread (the image's 8 MB stack) and on a second one.
Each touches its memory from the far end first, which only works when
__chkstk has walked the guard page down through every page between.
*/

#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "w9x_test.h"

static int __attribute__((noinline)) big_frame(int seed)
{
	volatile unsigned char block[1 << 20];
	int i, sum = 0;

	/* (the lowest address first: the page __chkstk reached last) */
	block[0] = (unsigned char)seed;
	for (i = 0; i < (int)sizeof(block); i += 4096)
		block[i] = (unsigned char)(seed + i);
	for (i = 0; i < (int)sizeof(block); i += 4096)
		sum += block[i];
	return sum;
}

static int __attribute__((noinline)) recurse(int depth)
{
	volatile unsigned char block[65536];

	block[0] = (unsigned char)depth;
	block[sizeof(block) - 1] = (unsigned char)depth;
	if (depth == 0)
		return block[0];
	return recurse(depth - 1) + block[sizeof(block) - 1];
}

static int __attribute__((noinline)) dynamic(int size)
{
	volatile unsigned char *block = __builtin_alloca((size_t)size);

	block[0] = 1;
	block[size - 1] = 2;
	return block[0] + block[size - 1];
}

static int expected_sum(int seed)
{
	int i, sum = 0;

	for (i = 0; i < (1 << 20); i += 4096)
		sum += (unsigned char)(seed + i);
	return sum;
}

static DWORD WINAPI on_thread(void *parameter)
{
	int *result = parameter;

	result[0] = big_frame(9) == expected_sum(9);
	result[1] = recurse(40) == 820;
	result[2] = dynamic(512 * 1024) == 3;
	return 0;
}

int main(void)
{
	int results[3] = { 0, 0, 0 };
	HANDLE thread;
	DWORD id;

	CHECK(big_frame(3) == expected_sum(3));
	CHECK(recurse(60) == 1830);
	CHECK(dynamic(512 * 1024) == 3);
	thread = CreateThread(NULL, 0, on_thread, results, 0, &id);
	CHECK(thread != NULL);
	WaitForSingleObject(thread, INFINITE);
	CloseHandle(thread);
	CHECK(results[0] && results[1] && results[2]);
	return test_finish("t_chkstk");
}
