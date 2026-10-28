/*
DOS_START.C

The DOS build's start-up, after dos_entry.s: the machine (the program's
segment base, the transfer buffer, the clock, the heap), the C runtime as
the 9x build's start-up readies it (port/win9x/src/w9x_start.c: the
floating-point state, the standard streams, argv), the platform, then the
game's main. And the way out: dos_exit undoes what the platform changed
(the video mode, interrupt handlers) before DOS ends the program, whether
the game ends or crashes.
*/

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include "dos_dpmi.h"
#include "dos_kernel32.h"
#include "w9x_runtime.h"

int main(int argc, char **argv);
void dos_p2p_state_initialize(void);
void w9x_crt_start(void);
void system_initialize(void);
void system_log(const char *text);
extern int system_argc;
extern char **system_argv;
unsigned long dos_selector_base(unsigned short selector);
void dos_heap_initialize(void);
void dos_exceptions_install(void);

extern unsigned short dos_psp_selector;

/* referenced by every unit that uses floating point (MSVC's convention) */
int _fltused = 0x9875;

/* ---------- leaving */

static void (*exit_functions[16])(void);
static int exit_function_count;

void dos_at_exit(void (*function)(void))
{
	if (exit_function_count < (int)(sizeof(exit_functions) / sizeof(exit_functions[0])))
		exit_functions[exit_function_count++] = function;
}

void dos_run_exit_functions(void)
{
	/* last registered, first undone; each runs once even if one of them
	crashes and comes back here */
	while (exit_function_count > 0)
	{
		void (*function)(void) = exit_functions[--exit_function_count];

		function();
	}
}

/* the platform's changes undone (the text screen back), then a message on
it and in the log, and the end */
void dos_exit_with_message(const char *text)
{
	dos_run_exit_functions();
	dos_console_set_visible(1);
	dos_log_text(text);
	dos_exit(1);
}

void dos_exit(int code)
{
	dos_run_exit_functions();
	__asm__ __volatile__("int $0x21" : : "a"(0x4C00 | (code & 0xFF)));
	for (;;)
		;
}

/* ---------- the clock */

unsigned long long dos_tsc_frequency;
static unsigned long long tsc_start;

unsigned long long dos_microseconds(void)
{
	unsigned long long elapsed = dos_rdtsc() - tsc_start;

	return elapsed / dos_tsc_frequency * 1000000 + elapsed % dos_tsc_frequency * 1000000 / dos_tsc_frequency;
}

/* the time stamp counter against the BIOS's 18.2 Hz tick (0040:006C):
three ticks, from the start of one */
static void measure_clock(void)
{
	volatile unsigned long *bios_ticks = dos_real_pointer(0x0040, 0x006C);
	unsigned long first, now;
	unsigned long long start, end;

	__asm__ __volatile__("sti");
	first = *bios_ticks;
	while ((now = *bios_ticks) == first)
		;
	start = dos_rdtsc();
	while (*bios_ticks - now < 3)
		;
	end = dos_rdtsc();
	/* 3 ticks of 65536 / 1193182 seconds */
	dos_tsc_frequency = (end - start) * 1193182ULL / (3ULL * 65536ULL);
	if (dos_tsc_frequency < 1000000)
		dos_tsc_frequency = 1000000;
	tsc_start = dos_rdtsc();
}

/* ---------- the machine */

void dos_machine_initialize(void)
{
	unsigned short selector;

	dos_segment_base = dos_selector_base(dos_data_selector());
	if (!dos_dos_allocate(DOS_TRANSFER_SIZE / 16, &dos_transfer_segment, &selector))
	{
		static const char message[] = "Halo needs 64 KB of conventional memory.\r\n$";

		/* (DOS's print-string, which DOS/32A translates) */
		__asm__ __volatile__("int $0x21" : : "a"(0x0900), "d"(message) : "memory");
		dos_exit(1);
	}
	dos_transfer = dos_real_pointer(dos_transfer_segment, 0);
	measure_clock();
}

/* ---------- the program's surroundings: the PSP's command line and the
environment (DOS strings, then a count and the program's path) */

static char command_line[300];
static const char *environment;
static char program_path[260];
/* DOS keeps 126 characters of a command line: one that long may have lost
its end (Windows 95's COMMAND.COM then puts it whole in CMDLINE, taken
instead when there) */
static int command_line_cut;

static void read_psp(void)
{
	const unsigned char *psp = dos_pointer(dos_selector_base(dos_psp_selector));
	unsigned length = psp[0x80];
	unsigned short environment_selector = *(const unsigned short *)(psp + 0x2C);
	const char *cursor;
	size_t used;

	if (environment_selector)
	{
		environment = dos_pointer(dos_selector_base(environment_selector));
		for (cursor = environment; *cursor; cursor += strlen(cursor) + 1)
			;
		/* after the double zero: a word count, then the program's path */
		if (*(const unsigned short *)(cursor + 1) >= 1)
		{
			strncpy(program_path, cursor + 3, sizeof(program_path) - 1);
			program_path[sizeof(program_path) - 1] = 0;
		}
	}
	if (!program_path[0])
		strcpy(program_path, "HALO.EXE");
	/* the command line as Windows gives it: the program, then the rest */
	used = (size_t)snprintf(command_line, sizeof(command_line), "%s", program_path);
	if (length >= 126 && environment)
	{
		for (cursor = environment; *cursor; cursor += strlen(cursor) + 1)
		{
			if (!strncmp(cursor, "CMDLINE=", 8) && strlen(cursor + 8) < sizeof(command_line))
			{
				strcpy(command_line, cursor + 8);
				return;
			}
		}
	}
	if (length >= 126)
		command_line_cut = 1;
	if (length > 126)
		length = 126;
	if (used + 1 + length < sizeof(command_line))
	{
		command_line[used] = ' ';
		memcpy(command_line + used + 1, psp + 0x81, length);
		command_line[used + 1 + length] = 0;
		/* (DOS ends it with a carriage return) */
		for (char *c = command_line; *c; c++)
		{
			if (*c == '\r')
				*c = 0;
		}
	}
}

const char *dos_command_line(void)
{
	return command_line;
}

const char *dos_program_path(void)
{
	return program_path;
}

const char *dos_environment(void)
{
	return environment;
}

/* ---------- the C runtime (as port/win9x/src/w9x_start.c readies it) */

extern mtx_t _PDCLIB_filelist_mtx;

static void open_standard_streams(void)
{
	mtx_init(&_PDCLIB_filelist_mtx, mtx_plain);
	mtx_init(&stdin->mtx, mtx_recursive);
	mtx_init(&stdout->mtx, mtx_recursive);
	mtx_init(&stderr->mtx, mtx_recursive);
	stdin->handle = GetStdHandle(STD_INPUT_HANDLE);
	stdout->handle = GetStdHandle(STD_OUTPUT_HANDLE);
	stderr->handle = GetStdHandle(STD_ERROR_HANDLE);
}

/* argv as the Microsoft runtime splits a command line, simply: blanks
separate, quotes group */
static char **make_argv(int *argc)
{
	static char *arguments[32];
	static char text[sizeof(command_line)];
	char *cursor = text;
	int count = 0;

	strcpy(text, command_line);
	while (*cursor && count < (int)(sizeof(arguments) / sizeof(arguments[0])) - 1)
	{
		char *out;
		int quoted = 0;

		while (*cursor == ' ' || *cursor == '\t')
			cursor++;
		if (!*cursor)
			break;
		arguments[count++] = out = cursor;
		for (; *cursor && (quoted || (*cursor != ' ' && *cursor != '\t')); cursor++)
		{
			if (*cursor == '"')
				quoted = !quoted;
			else
				*out++ = *cursor;
		}
		if (*cursor)
			cursor++;
		*out = 0;
	}
	arguments[count] = NULL;
	*argc = count;
	return arguments;
}

void dos_start_c(void)
{
	int argc;
	char **argv;

	dos_machine_initialize();
	read_psp();
	dos_heap_initialize();
	dos_exceptions_install();
	w9x_fpu_reset();
	w9x_crt_start();
	open_standard_streams();
	argv = make_argv(&argc);
	system_argc = argc;
	system_argv = argv;
	system_initialize();
	if (command_line_cut)
	{
		system_log("start: the command line is 126 characters, all DOS keeps: settings at its end may be "
			"lost (put them in HALODOS.CFG)");
	}
	/* (internet play's state: its sockets none, p2p.c) */
	dos_p2p_state_initialize();
	exit(main(argc, argv));
}
