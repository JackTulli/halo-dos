/*
DOS_MALLOC.C

malloc and the rest for the DOS build: nxdk's copy of Doug Lea's malloc
2.8.6 (as the 9x build's port/win9x/src/w9x_malloc.c), growing by sbrk
through one block of the DPMI host's memory taken at start-up. DOS has one
thread; the spin locks dlmalloc keeps are never contended.

The block is all the memory the host has but a margin for the extender
(its buffers, page tables for the video card's frame buffer). The game
then sees one heap, and tells the player how large it is (dos_heap_size).
*/

#include <stddef.h>
#include <errno.h>
#include <string.h>

#include "dos_dpmi.h"

/* <windows.h> first, for what dlmalloc's code for Microsoft compilers
uses (its spin locks' interlocked exchange, _BitScanReverse) even when it
is not configured for Win32 */
#include <windows.h>

void *dos_sbrk(ptrdiff_t increment);

/* not Win32 here: memory by sbrk (MORECORE), never released to the host */
#undef _WIN32
#undef WIN32
#define HAVE_MORECORE 1
#define HAVE_MMAP 0
#define MORECORE dos_sbrk
#define MORECORE_CONTIGUOUS 1
#define MORECORE_CANNOT_TRIM 1
#define USE_SPIN_LOCKS 1
#define LACKS_UNISTD_H 1
#define LACKS_FCNTL_H 1
#define LACKS_SYS_TYPES_H 1
#define LACKS_SYS_MMAN_H 1
#define LACKS_SCHED_H 1
#define LACKS_SYS_PARAM_H 1
#define sched_yield() ((void)0)
#define interlockedexchange(target, value) InterlockedExchange((volatile LONG *)(target), (LONG)(value))
#define interlockedcompareexchange InterlockedCompareExchange
/* (only skips malloc.c's own sbrk declaration) */
#define __FreeBSD__ 1
#define MALLOC_FAILURE_ACTION errno = ENOMEM;

#include <_dlmalloc/malloc.c>

/* ---------- the arena */

/* what is left to the extender */
#define HOST_MARGIN (512UL * 1024UL)
/* never more than this (an address space for the frame buffer's mapping
must remain) */
#define MAXIMUM_ARENA (1024UL * 1024UL * 1024UL)

static unsigned char *arena;
static unsigned long arena_size;
static unsigned long arena_used;

void dos_heap_initialize(void)
{
	unsigned long largest = dos_dpmi_largest_free_block();
	unsigned long size, linear, handle;

	size = largest > HOST_MARGIN ? largest - HOST_MARGIN : 0;
	if (size > MAXIMUM_ARENA)
		size = MAXIMUM_ARENA;
	size &= ~0xFFFUL;
	while (size >= 1024 * 1024 && !dos_dpmi_allocate(size, &linear, &handle))
		size -= 256 * 1024;
	if (size < 1024 * 1024)
	{
		static const char message[] = "Halo could not get its memory from the DOS extender.\r\n$";

		__asm__ __volatile__("int $0x21" : : "a"(0x0900), "d"(message) : "memory");
		dos_exit(1);
	}
	arena = dos_pointer(linear);
	arena_size = size;
	arena_used = 0;
}

unsigned long dos_heap_size(void)
{
	return arena_size;
}

unsigned long dos_heap_unused(void)
{
	return arena_size - arena_used;
}

void *dos_sbrk(ptrdiff_t increment)
{
	unsigned char *previous = arena + arena_used;

	if (increment < 0)
		return (void *)MFAIL;
	if ((unsigned long)increment > arena_size - arena_used)
	{
		errno = ENOMEM;
		return (void *)MFAIL;
	}
	arena_used += (unsigned long)increment;
	if ((unsigned long)increment >= 1024UL * 1024UL)
	{
		/* (testing: who grows the heap by a megabyte or more, its callers'
		return addresses up the frame pointers' chain: HALO.map names them) */
		extern void system_log(const char *text);
		extern int snprintf(char *, unsigned int, const char *, ...);
		char line[160];
		unsigned long *frame = (unsigned long *)__builtin_frame_address(0);
		int length = snprintf(line, sizeof(line), "heap: +%lu KB from", (unsigned long)increment / 1024);

		for (int depth = 0; depth < 8 && frame && length < (int)sizeof(line) - 12; depth++)
		{
			length += snprintf(line + length, sizeof(line) - length, " %08lX", frame[1]);
			if ((unsigned long *)frame[0] <= frame)
				break;
			frame = (unsigned long *)frame[0];
		}
		system_log(line);
	}
	return previous;
}

/* ---------- what the game is told of the heap (halo_system.h) */

unsigned long system_memory_total(void)
{
	return arena_size;
}

unsigned long system_memory_free(void)
{
	struct mallinfo info = dlmallinfo();

	return dos_heap_unused() + (unsigned long)info.fordblks;
}
