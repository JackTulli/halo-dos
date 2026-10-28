/*
W9X_CPUCHECK.C

The first thing the 9x build's start-up code does (w9x_start.c): make sure
the processor can run the game before any of its code runs. The game does
its single-precision arithmetic in SSE registers, as the Xbox build does on
the console's Pentium III (the same results bit for bit as the other ports,
which cross-play needs); a processor without SSE, or a system that does not
save the SSE registers (Windows 95, Windows 98 before the Second Edition),
would stop at its first float with an invalid-opcode fault.

Two questions, then:
 - does the processor have SSE? CPUID, which a processor without the
   instruction refuses to have its EFLAGS.ID bit toggled;
 - does the system let a program use it? One SSE instruction (xorps), run
   under an exception handler registered by hand: a system that has not set
   CR4.OSFXSR makes it an invalid-opcode fault, which the handler turns
   into an answer.

This unit is built with -mno-sse and never with link-time optimisation, so
no SSE instruction of the compiler's own can run before the answer; the
probe is written as bytes. tools/win9x_check.py checks the linked image for
exactly that: no SSE in these functions but the probe.

Windows XP and later check an exception handler against the image's SafeSEH
table (the load configuration in w9x_start.c), so the handler is registered
there too; 9x ignores the table.
*/

#include <windows.h>

#include "w9x_runtime.h"

static volatile LONG probe_faulted;
/* the length of the instruction the handler skips; 0 outside the probe */
static volatile DWORD probe_length;

EXCEPTION_DISPOSITION __cdecl w9x_cpu_check_handler(EXCEPTION_RECORD *record, void *frame, CONTEXT *context,
	void *dispatcher)
{
	(void)frame;
	(void)dispatcher;
	if (record->ExceptionCode == STATUS_ILLEGAL_INSTRUCTION && probe_length)
	{
		probe_faulted = 1;
		context->Eip += probe_length;
		return ExceptionContinueExecution;
	}
	return ExceptionContinueSearch;
}
__asm__(".safeseh _w9x_cpu_check_handler");

/* an exception registration record (the frame chain at FS:[0]) */
struct seh_record
{
	struct seh_record *next;
	void *handler;
};

/* whether xorps faults; runs it with the handler registered */
static int __attribute__((noinline)) sse_faults(void)
{
	struct seh_record record;

	probe_faulted = 0;
	__asm__ __volatile__("movl %%fs:0, %0" : "=r"(record.next));
	record.handler = (void *)w9x_cpu_check_handler;
	__asm__ __volatile__("movl %0, %%fs:0" : : "r"(&record) : "memory");
	probe_length = 3;
	/* xorps %xmm0, %xmm0 (the probe: the one SSE instruction here) */
	__asm__ __volatile__(".byte 0x0f, 0x57, 0xc0" : : : "memory");
	probe_length = 0;
	__asm__ __volatile__("movl %0, %%fs:0" : : "r"(record.next) : "memory");
	return probe_faulted;
}

static int cpuid_has_sse(void)
{
	unsigned long before, after, a, b, c, d;

	/* CPUID exists when EFLAGS.ID (bit 21) can be changed */
	__asm__ __volatile__(
		"pushfl\n\t"
		"popl %0\n\t"
		"movl %0, %1\n\t"
		"xorl $0x200000, %1\n\t"
		"pushl %1\n\t"
		"popfl\n\t"
		"pushfl\n\t"
		"popl %1\n\t"
		"pushl %0\n\t"
		"popfl"
		: "=&r"(before), "=&r"(after));
	if (!((before ^ after) & 0x200000))
		return 0;
	__asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0), "c"(0));
	if (a < 1)
		return 0;
	__asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
	/* EDX bit 25: SSE (bit 24, FXSR, comes with it on every SSE processor) */
	return (d >> 25) & 1;
}

void w9x_cpu_check(void)
{
	if (cpuid_has_sse() && !sse_faults())
		return;
	MessageBoxA(NULL,
		"Halo needs a Pentium III or later processor with SSE, and Windows 98 Second Edition or Windows Me.",
		"Halo", MB_OK | MB_ICONSTOP | MB_SETFOREGROUND);
	ExitProcess(3);
}
