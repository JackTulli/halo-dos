/*
DOS_CRASH.C

What happens when the game faults (a page fault, a protection fault, a
division by zero...): the DPMI host calls a handler of dos_exceptions.s,
which brings the program here on a stack of its own. Here the platform's
changes are undone (the text mode back, the keyboard DOS's again), the
fault is written to the screen and to HALO.LOG with the registers and the
return addresses up the frame pointers' chain, and the program ends.

The addresses are linear; HALO.MAP (wlink's map) names them, counted from
the code's start, which the report gives as well.
*/

#include <stdio.h>
#include <string.h>

#include "dos_dpmi.h"
#include "dos_kernel32.h"

void dos_run_exit_functions(void);
void dos_start(void);

/* filled by dos_exceptions.s: the exception's number, its error code,
EIP, EFLAGS, ESP, then EAX EBX ECX EDX ESI EDI EBP */
unsigned long dos_crash_record[12];
__attribute__((aligned(16))) unsigned char dos_crash_stack[32768];
extern unsigned short dos_crash_data_selector;

#define HANDLER(number) extern void dos_exception_##number(void)
HANDLER(0); HANDLER(1); HANDLER(3); HANDLER(4); HANDLER(5); HANDLER(6); HANDLER(7); HANDLER(8);
HANDLER(10); HANDLER(11); HANDLER(12); HANDLER(13); HANDLER(14); HANDLER(16); HANDLER(17);
#undef HANDLER

static const struct
{
	int number;
	void (*handler)(void);
	const char *name;
} exceptions[] = {
	{ 0, dos_exception_0, "division by zero" },
	{ 1, dos_exception_1, "debug trap" },
	{ 3, dos_exception_3, "breakpoint" },
	{ 4, dos_exception_4, "overflow" },
	{ 5, dos_exception_5, "bound range" },
	{ 6, dos_exception_6, "invalid instruction" },
	{ 7, dos_exception_7, "no floating-point unit" },
	{ 8, dos_exception_8, "double fault" },
	{ 10, dos_exception_10, "invalid TSS" },
	{ 11, dos_exception_11, "segment not present" },
	{ 12, dos_exception_12, "stack fault" },
	{ 13, dos_exception_13, "general protection fault" },
	{ 14, dos_exception_14, "page fault" },
	{ 16, dos_exception_16, "floating-point error" },
	{ 17, dos_exception_17, "alignment check" },
};

void dos_exceptions_install(void)
{
	dos_crash_data_selector = dos_data_selector();
	for (unsigned i = 0; i < sizeof(exceptions) / sizeof(exceptions[0]); i++)
	{
		unsigned long eax = 0x0203, ebx = (unsigned long)exceptions[i].number, ecx = dos_code_selector();
		unsigned long edx = (unsigned long)exceptions[i].handler;

		__asm__ __volatile__("int $0x31" : "+a"(eax), "+b"(ebx), "+c"(ecx), "+d"(edx) : : "esi", "edi", "memory",
			"cc");
	}
}

static int crashing;

void dos_crash_entry(void)
{
	char line[200];
	unsigned long number = dos_crash_record[0];
	const char *name = "exception";
	unsigned long frame;

	__asm__ __volatile__("sti");
	if (crashing++)
		dos_exit(3);
	for (unsigned i = 0; i < sizeof(exceptions) / sizeof(exceptions[0]); i++)
	{
		if ((unsigned long)exceptions[i].number == number)
			name = exceptions[i].name;
	}
	dos_run_exit_functions();
	dos_console_set_visible(1);
	snprintf(line, sizeof(line), "Halo crashed: %s (exception %lu, error code %lu) at %08lX", name, number,
		dos_crash_record[1], dos_crash_record[2]);
	dos_log_text(line);
	snprintf(line, sizeof(line), "the code starts at %08lX (dos_start, in HALO.MAP)", (unsigned long)dos_start);
	dos_log_text(line);
	snprintf(line, sizeof(line), "EAX %08lX EBX %08lX ECX %08lX EDX %08lX ESI %08lX EDI %08lX",
		dos_crash_record[5], dos_crash_record[6], dos_crash_record[7], dos_crash_record[8], dos_crash_record[9],
		dos_crash_record[10]);
	dos_log_text(line);
	snprintf(line, sizeof(line), "EBP %08lX ESP %08lX EFLAGS %08lX", dos_crash_record[11], dos_crash_record[4],
		dos_crash_record[3]);
	dos_log_text(line);
	/* the return addresses up the chain of saved frame pointers, while it
	stays inside the heap or the stack and climbs */
	frame = dos_crash_record[11];
	for (int depth = 0; depth < 24 && frame && !(frame & 3); depth++)
	{
		const unsigned long *words = (const unsigned long *)frame;
		unsigned long next = words[0];

		snprintf(line, sizeof(line), "  called from %08lX", words[1]);
		dos_log_text(line);
		if (next <= frame)
			break;
		frame = next;
	}
	dos_exit(3);
}
