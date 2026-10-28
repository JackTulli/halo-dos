/*
   HALO_MTCP.H

   Force-included ahead of each mTCP unit of Halo's DOS build: mTCP, written
   for 16-bit real mode (Open Watcom, small and large models), compiled by
   clang for the build's flat 32-bit protected mode under DOS/32A. Its
   segmented-memory words (far, near, cdecl, __interrupt) mean nothing
   there; its own types come from <stdint.h>; interrupts are turned off and
   on through the DPMI host (packet.cpp), since the packet driver's
   receiver runs in a hardware interrupt in real mode.

   Its 16-bit parts are rewritten for protected mode: the packet driver's
   interface (packet.cpp: a receiver in conventional memory, the driver
   called through DPMI), the checksums (ipasm.cpp, for IPASM.ASM), the
   clock (timer.h, timer.cpp: the game's) and the stack's start
   (utils.cpp).
*/

#ifndef HALO_MTCP_H
#define HALO_MTCP_H

#define HALO_MTCP 1

#define far
#define __far
#define near
#define __near
#define cdecl
#define _cdecl
#define __interrupt

#define CFG_H "halo_mtcp.cfg"

/* (the C library's headers, as C: not all of them say so to C++) */
extern "C" {
#include <ctype.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
}

#define stricmp _stricmp
#define strnicmp _strnicmp

/* the BIOS's ticks (55 ms), counted from the game's clock (timer.cpp) */
extern "C" uint32_t mtcp_timer_ticks( void );

/* mTCP is built with Open Watcom's -zp2: structures packed to 2 bytes. Its
packets are structures (an Ethernet header of 14 bytes, then IP's, then
TCP's, whose 32-bit sequence numbers natural alignment would move 2 bytes
on) */
#pragma pack(2)

#endif
