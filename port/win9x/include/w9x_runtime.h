/*
W9X_RUNTIME.H

What the 9x build's C runtime (port/win9x/src) offers its platform layer
beyond the C and MSVC interfaces: the calls the Windows build makes to
Windows Vista and later, done the 9x way.
*/

#ifndef __W9X_RUNTIME_H
#define __W9X_RUNTIME_H

/* pread and pwrite (port/windows/src/win32_posix.c): 9x file handles take
no offset in an OVERLAPPED, so the descriptor's lock is held across the
seek and the transfer (w9x_crt_io.c). Each returns the bytes moved, or -1
with errno set. */
int w9x_pread(int descriptor, void *buffer, unsigned long size, long long offset);
int w9x_pwrite(int descriptor, const void *buffer, unsigned long size, long long offset);

/* The floating-point state every thread of the game runs with (the UCRT's
default, which the PC and Xbox builds compute in): x87 at 53 bits, round
to nearest, exceptions masked; MXCSR 0x1F80, which is the same for SSE
(never DAZ, which a Pentium III without it refuses with a fault, nor FTZ).
The start-up code sets it for the first thread; a thread the platform
layer starts sets it first thing (w9x_crt.c). */
void w9x_fpu_reset(void);

/* The processor check of the start-up code (w9x_cpucheck.c): returns when
the processor has SSE and the system saves its registers, and otherwise
tells the user and ends the process. */
void w9x_cpu_check(void);

/* GetVersionExA's answer, read at start-up: the platform (1 = 9x, 2 = NT)
and the version (4.10 = 98, 4.90 = Me) */
extern unsigned long w9x_platform_id;
extern unsigned long w9x_version_major;
extern unsigned long w9x_version_minor;
extern unsigned long w9x_build_number;

#endif
