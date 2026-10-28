/*
T_MALLOC.C

The 9x build's malloc (nxdk's dlmalloc, port/win9x/src/w9x_malloc.c):
random allocations, reallocations and frees whose contents are checked,
from one thread and then from four at once (with frees on other threads
than the allocating one); calloc's zeros; aligned_alloc; _msize; blocks
large enough for dlmalloc's direct VirtualAlloc.
*/

#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "w9x_test.h"

size_t _msize(void *pointer);

#define SLOTS 512

struct slot
{
	unsigned char *block;
	size_t size;
	unsigned char fill;
};

/* a size: mostly small, some pages, a few above dlmalloc's 256 KB mmap
threshold */
static size_t random_size(unsigned long long (*random)(void))
{
	unsigned long long r = random();

	switch (r % 16)
	{
	case 0: return (size_t)(r >> 8) % (1u << 20) + 256 * 1024;
	case 1: case 2: return (size_t)(r >> 8) % 65536;
	default: return (size_t)(r >> 8) % 512;
	}
}

static int intact(const struct slot *slot)
{
	size_t i;

	for (i = 0; i < slot->size; i++)
	{
		if (slot->block[i] != (unsigned char)(slot->fill + i))
			return 0;
	}
	return 1;
}

static void fill(struct slot *slot, size_t from)
{
	size_t i;

	for (i = from; i < slot->size; i++)
		slot->block[i] = (unsigned char)(slot->fill + i);
}

/* random operations on a set of slots; returns the number of corruptions */
static long exercise(struct slot *slots, int count, int operations, unsigned long long (*random)(void))
{
	long corrupt = 0;
	int i;

	for (i = 0; i < operations; i++)
	{
		struct slot *slot = &slots[random() % (unsigned)count];
		unsigned long long what = random() % 8;

		if (slot->block && !intact(slot))
			corrupt++;
		if (!slot->block)
		{
			slot->size = random_size(random);
			slot->fill = (unsigned char)random();
			if (what == 0)
			{
				size_t j;

				slot->block = calloc(1, slot->size + 1);
				for (j = 0; slot->block && j <= slot->size; j++)
				{
					if (slot->block[j])
					{
						corrupt++;
						break;
					}
				}
			}
			else
				slot->block = malloc(slot->size + 1);
			if (!slot->block)
				return corrupt + 1000000;
			if (_msize(slot->block) < slot->size + 1)
				corrupt++;
			fill(slot, 0);
		}
		else if (what < 3)
		{
			size_t size = random_size(random), kept = size < slot->size ? size : slot->size;
			unsigned char *grown = realloc(slot->block, size + 1);

			if (!grown)
				return corrupt + 1000000;
			slot->block = grown;
			slot->size = kept;
			if (!intact(slot))
				corrupt++;
			slot->size = size;
			fill(slot, kept);
		}
		else
		{
			free(slot->block);
			slot->block = NULL;
		}
	}
	for (i = 0; i < count; i++)
	{
		if (slots[i].block && !intact(&slots[i]))
			corrupt++;
	}
	return corrupt;
}

/* each thread its own random sequence */
static __thread unsigned long long thread_state;

static unsigned long long thread_random(void)
{
	thread_state ^= thread_state << 13;
	thread_state ^= thread_state >> 7;
	thread_state ^= thread_state << 17;
	return thread_state;
}

static struct slot shared[4][SLOTS / 4];
static long thread_corruptions[4];

static DWORD WINAPI worker(void *parameter)
{
	int index = (int)(INT_PTR)parameter;

	thread_state = 0x9E3779B97F4A7C15ULL * (unsigned long long)(index + 1);
	thread_corruptions[index] = exercise(shared[index], SLOTS / 4, 40000, thread_random);
	return 0;
}

int main(void)
{
	static struct slot slots[SLOTS];
	HANDLE threads[4];
	long corrupt;
	int i;

	corrupt = exercise(slots, SLOTS, 200000, test_random);
	printf("# one thread: 200000 operations, %ld corruptions\n", corrupt);
	CHECK(corrupt == 0);
	for (i = 0; i < SLOTS; i++)
		free(slots[i].block);

	for (i = 0; i < 4; i++)
	{
		DWORD id;

		threads[i] = CreateThread(NULL, 0, worker, (void *)(INT_PTR)i, 0, &id);
	}
	WaitForMultipleObjects(4, threads, TRUE, INFINITE);
	for (i = 0; i < 4; i++)
	{
		CloseHandle(threads[i]);
		printf("# thread %d: 40000 operations, %ld corruptions\n", i, thread_corruptions[i]);
		CHECK(thread_corruptions[i] == 0);
	}
	/* the blocks of every thread freed by this one */
	for (i = 0; i < SLOTS; i++)
		free(shared[i / (SLOTS / 4)][i % (SLOTS / 4)].block);

	/* alignment */
	for (i = 3; i <= 16; i++)
	{
		size_t alignment = (size_t)1 << i;
		void *block = aligned_alloc(alignment, alignment * 3);

		CHECK(block && ((uintptr_t)block & (alignment - 1)) == 0);
		free(block);
	}
	/* malloc's own alignment: 8 bytes (doubles) */
	for (i = 0; i < 100; i++)
	{
		void *block = malloc((size_t)i);

		CHECK(block && ((uintptr_t)block & 7) == 0);
		free(block);
	}
	CHECK(realloc(NULL, 10) != NULL);
	{
		void *huge = malloc((size_t)0x7FFFFFF0);

		printf("# malloc(2 GB): %s\n", huge ? "given" : "refused");
		free(huge);
	}
	free(NULL);
	return test_finish("t_malloc");
}
