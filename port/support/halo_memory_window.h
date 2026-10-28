/*
HALO_MEMORY_WINDOW.H

Where the native builds put the Xbox memory window. The Xbox sees physical
memory at address P at virtual address 0x80000000 + P; the game asks for its
game state and tag cache at fixed addresses there (physical_memory_map.c),
and Direct3D resources carry physical addresses in their Data fields. The
desktop platform layer reserves a 128 MB window of its own and does the same
arithmetic over it (port/linux/src/platform.h, xbox_memory.c).

The window's base is chosen when the game is built (configure.py
--window-base). It is 0x80000000, the console's own, everywhere but Windows
9x, where 0x80000000 and up is the system's shared arena; there it is
0x40000000. Any multiple of the window's size works: physical offsets stay
below it, so a base ORed onto an offset is the same as one added to it.

The retail maps are linked to the console's window: their tag data and
structure BSPs hold absolute addresses in it. A build whose window is
elsewhere moves them after reading (port/linux/src/xbox_map_rebase.h,
cache/cache_files.c), and mixes its base into the game state's checksum so
that it never loads a saved game holding another window's addresses (saved
games/game_state.c).

Only plain definitions, so that the units built with the Windows SDK
instead of the Xbox's can include this file too.
*/

#ifndef __HALO_MEMORY_WINDOW_H
#define __HALO_MEMORY_WINDOW_H

#ifndef HALO_WINDOW_BASE
#ifdef HALO_WIN9X
#define HALO_WINDOW_BASE 0x40000000UL /* 0x80000000 and up is 9x's shared arena */
#else
#define HALO_WINDOW_BASE 0x80000000UL /* the console's own */
#endif
#endif

#define HALO_WINDOW_SIZE 0x08000000UL /* a 128 MB development kit */

/* the window the retail maps are linked to */
#define HALO_XBOX_WINDOW_BASE 0x80000000UL

/* the physical offset of an address in this build's window or in the
console's (where a map's addresses point until they are moved) */
#define HALO_WINDOW_PHYSICAL(address) ((unsigned long)(address) & (HALO_WINDOW_SIZE - 1))
/* the address in this build's window of a physical offset (or of an address
in either window) */
#define HALO_WINDOW_ADDRESS(physical) (HALO_WINDOW_BASE | HALO_WINDOW_PHYSICAL(physical))

/* the tag cache, where the maps' tag data is read (physical_memory_map.c) */
#define HALO_TAG_CACHE_ADDRESS (HALO_WINDOW_BASE + 0x003A6000UL)

/* the base is a nonzero multiple of the window's size, and the window ends
inside the 32-bit address space (on 9x, inside the process's own arena) */
typedef char halo_window_base_is_aligned[
	(HALO_WINDOW_BASE & (HALO_WINDOW_SIZE - 1)) == 0 ? 1 : -1];
typedef char halo_window_base_is_not_null[HALO_WINDOW_BASE != 0 ? 1 : -1];
typedef char halo_window_fits_the_address_space[
	HALO_WINDOW_BASE <= 0xFFFFFFFFUL - (HALO_WINDOW_SIZE - 1) ? 1 : -1];
#ifdef HALO_WIN9X
typedef char halo_window_is_below_the_shared_arena[
	HALO_WINDOW_BASE + HALO_WINDOW_SIZE <= 0x80000000UL ? 1 : -1];
#endif

#endif /* __HALO_MEMORY_WINDOW_H */
