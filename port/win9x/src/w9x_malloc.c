/*
W9X_MALLOC.C

malloc, calloc, realloc, aligned_alloc and free: nxdk's copy of Doug Lea's
malloc 2.8.6 (CC0, with PDCLib), in its Win32 configuration: memory from
VirtualAlloc in 64 KB regions, spin locks on interlocked instructions.

Two changes for 9x, made from outside so that nxdk's file stays as it is:
 - a waiting thread sleeps 1 ms (Sleep(1)) instead of dlmalloc's 50 ms
   (SleepEx(50)): long enough for the lock's holder, whatever its priority,
   to run (9x has one processor, and its Sleep(0) only yields to threads of
   the caller's own priority), and short enough not to stall the game when
   its mixer thread and its main thread meet in malloc;
 - no MEM_TOP_DOWN for the large blocks: an NT placement hint which 9x does
   not document for VirtualAlloc.
*/

#include <stddef.h>
#include <errno.h>
#include <string.h>

/* <windows.h> first, so that the renames below do not reach its
declarations (malloc.c includes it again, to no effect) */
#include <windows.h>

#define w9x_malloc_yield(duration, alertable) Sleep(1)
#define SleepEx w9x_malloc_yield
#undef MEM_TOP_DOWN
#define MEM_TOP_DOWN 0

/* (only skips malloc.c's sbrk declaration: memory comes from VirtualAlloc) */
#define __FreeBSD__ 1
#define LACKS_FCNTL_H 1

/* (its spin locks are ints handed to the interlocked functions, which take
LONGs: the same 32 bits here) */
#pragma clang diagnostic ignored "-Wincompatible-pointer-types"

#include <_dlmalloc/malloc.c>
