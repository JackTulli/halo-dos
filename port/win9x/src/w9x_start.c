/*
W9X_START.C

The 9x build's start-up code: the executable's entry point (lld-link
-entry:halo9x_start), and the image directories the C runtime would
otherwise supply. In order:

 1. the processor check (w9x_cpucheck.c), before any code that may use SSE;
 2. the floating-point state the game computes in (w9x_fpu_reset);
 3. the system's version, for the log;
 4. the standard streams on the process's standard handles;
 5. the C initializers (.CRT$XI*) and the constructors (.CRT$XC*), in the
    linker's order of their section names: clang names a constructor of
    priority 101 .CRT$XCA00101, the others .CRT$XCC00200 and so on, and the
    default priority .CRT$XCU, so priority 101 (the platform layer's memory
    window, port/linux/src/xbox_memory.c) runs first;
 6. exit(main(argc, argv)), argv split from GetCommandLineA the way the
    Microsoft C runtime splits it.

This unit is built with -mno-sse and never with link-time optimisation:
nothing here may run an SSE instruction before step 1 has answered
(tools/win9x_check.py checks halo9x_start for it).
*/

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <threads.h>

#include "w9x_runtime.h"

int main(int argc, char **argv);

/* what the C library's start-up does elsewhere (w9x_crt.c) */
void w9x_crt_start(void);

/* ---------- image directories */

/* referenced by every unit that uses floating point (MSVC's convention;
clang's Microsoft target follows it) */
int _fltused = 0x9875;

/* The TLS directory: the template of the executable's __thread variables
(.tls, which the linker sorts between these two), and where the loader
writes the module's TLS index. No callbacks: the 9x loader never calls
them. */
unsigned long _tls_index = 0;
__attribute__((section(".tls"))) char _tls_start = 0;
__attribute__((section(".tls$ZZZ"))) char _tls_end = 0;
__attribute__((section(".rdata$T"), used)) const IMAGE_TLS_DIRECTORY32 _tls_used = {
	(DWORD)&_tls_start,
	(DWORD)&_tls_end,
	(DWORD)&_tls_index,
	0,
	0,
	0,
};

/* The load configuration, for its SafeSEH table only: Windows XP and later
refuse a hand-registered exception handler (the processor check's) that
the image does not list. lld-link -safeseh fills in the table. 9x reads
none of this. */
extern void *__safe_se_handler_table[];
extern char __safe_se_handler_count; /* an absolute symbol: its address is the count */
__attribute__((used)) const struct
{
	DWORD Size, TimeDateStamp;
	WORD MajorVersion, MinorVersion;
	DWORD GlobalFlagsClear, GlobalFlagsSet, CriticalSectionDefaultTimeout,
		DeCommitFreeBlockThreshold, DeCommitTotalFreeThreshold, LockPrefixTable,
		MaximumAllocationSize, VirtualMemoryThreshold, ProcessHeapFlags, ProcessAffinityMask;
	WORD CSDVersion, Reserved1;
	DWORD EditList, SecurityCookie, SEHandlerTable, SEHandlerCount;
} _load_config_used = {
	0x48, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	(DWORD)__safe_se_handler_table, (DWORD)&__safe_se_handler_count,
};

/* The initializer tables: the linker puts every .CRT$X?? contribution
between the A and Z markers, sorted by name, and merges them into .rdata
(as the Microsoft runtime does). The slots between may be zero (padding). */
typedef int(__cdecl *initializer_function)(void);
typedef void(__cdecl *constructor_function)(void);

#pragma comment(linker, "/merge:.CRT=.rdata")
__attribute__((section(".CRT$XIA"), used)) static const initializer_function initializers_begin = 0;
__attribute__((section(".CRT$XIZ"), used)) static const initializer_function initializers_end = 0;
__attribute__((section(".CRT$XCA"), used)) static const constructor_function constructors_begin = 0;
__attribute__((section(".CRT$XCZ"), used)) static const constructor_function constructors_end = 0;

static void run_initializers(void)
{
	const initializer_function *initializer = &initializers_begin + 1;
	const initializer_function *initializers_stop = &initializers_end;
	const constructor_function *constructor = &constructors_begin + 1;
	const constructor_function *constructors_stop = &constructors_end;

	/* (the bounds are separate objects to the compiler, which may not
	compare pointers between them: it is told nothing of where they are) */
	__asm__("" : "+r"(initializer), "+r"(initializers_stop));
	__asm__("" : "+r"(constructor), "+r"(constructors_stop));
	for (; initializer < initializers_stop; initializer++)
	{
		if (*initializer && (*initializer)() != 0)
			ExitProcess(255);
	}
	for (; constructor < constructors_stop; constructor++)
	{
		if (*constructor)
			(*constructor)();
	}
}

/* ---------- the system */

unsigned long w9x_platform_id;
unsigned long w9x_version_major;
unsigned long w9x_version_minor;
unsigned long w9x_build_number;

static void read_version(void)
{
	OSVERSIONINFOA version;

	version.dwOSVersionInfoSize = sizeof(version);
	if (!GetVersionExA(&version))
		return;
	w9x_platform_id = version.dwPlatformId;
	w9x_version_major = version.dwMajorVersion;
	w9x_version_minor = version.dwMinorVersion;
	/* (9x keeps the version in the high word of the build number) */
	w9x_build_number = version.dwPlatformId == VER_PLATFORM_WIN32_WINDOWS ? version.dwBuildNumber & 0xFFFF :
		version.dwBuildNumber;
}

/* ---------- the standard streams */

extern mtx_t _PDCLIB_filelist_mtx;

static void open_standard_streams(void)
{
	HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
	HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
	HANDLE error = GetStdHandle(STD_ERROR_HANDLE);

	mtx_init(&_PDCLIB_filelist_mtx, mtx_plain);
	mtx_init(&stdin->mtx, mtx_recursive);
	mtx_init(&stdout->mtx, mtx_recursive);
	mtx_init(&stderr->mtx, mtx_recursive);
	/* (a windowed program without a console has none: its writes fail
	quietly, as the Microsoft runtime's do) */
	stdin->handle = input ? input : INVALID_HANDLE_VALUE;
	stdout->handle = output ? output : INVALID_HANDLE_VALUE;
	stderr->handle = error ? error : INVALID_HANDLE_VALUE;
}

/* ---------- the command line */

/* Splits the command line as the Microsoft runtime does: the program's
name up to the first blank outside quotes, without its quotes; then
arguments separated
by blanks, where quotes group, 2n backslashes and a quote are n backslashes
and the quote's grouping, 2n + 1 backslashes and a quote are n backslashes
and a literal quote, and other backslashes are themselves. Returns the
count; with argv NULL, only counts, and *size is the text's size. */
static int split_command_line(const char *line, char **argv, char *text, size_t *size)
{
	const char *cursor = line;
	char *out = text;
	size_t length = 0;
	int count = 0;

#define PUT(c) do { if (out) *out++ = (c); length++; } while (0)

	/* the program's name: up to the first blank outside quotes, the quotes
	dropped wherever they are (C:\"Program Files"\halo9x.exe, "a b"c) */
	if (argv)
		argv[count] = out;
	{
		int quoted = 0;

		for (; *cursor && (quoted || (*cursor != ' ' && *cursor != '\t')); cursor++)
		{
			if (*cursor == '"')
				quoted = !quoted;
			else
				PUT(*cursor);
		}
	}
	PUT('\0');
	count++;

	for (;;)
	{
		int quoted = 0;

		while (*cursor == ' ' || *cursor == '\t')
			cursor++;
		if (!*cursor)
			break;
		if (argv)
			argv[count] = out;
		for (; *cursor; )
		{
			if (*cursor == '\\')
			{
				size_t backslashes = 0;

				while (*cursor == '\\')
				{
					backslashes++;
					cursor++;
				}
				if (*cursor == '"')
				{
					for (; backslashes >= 2; backslashes -= 2)
						PUT('\\');
					if (backslashes)
					{
						PUT('"');
						cursor++;
					}
				}
				else
				{
					for (; backslashes; backslashes--)
						PUT('\\');
				}
				continue;
			}
			if (*cursor == '"')
			{
				/* "" inside quotes is a literal quote (the runtime of
				Visual C++ 6 onwards) */
				if (quoted && cursor[1] == '"')
				{
					PUT('"');
					cursor += 2;
					continue;
				}
				quoted = !quoted;
				cursor++;
				continue;
			}
			if (!quoted && (*cursor == ' ' || *cursor == '\t'))
				break;
			PUT(*cursor);
			cursor++;
		}
		PUT('\0');
		count++;
	}
#undef PUT
	if (argv)
		argv[count] = NULL;
	*size = length;
	return count;
}

static char **make_argv(int *argc)
{
	static char *no_arguments[2] = { "halo", NULL };
	const char *line = GetCommandLineA();
	size_t size = 0;
	int count;
	char **argv;

	if (!line)
	{
		*argc = 1;
		return no_arguments;
	}
	count = split_command_line(line, NULL, NULL, &size);
	argv = malloc((count + 1) * sizeof(char *) + size);
	if (!argv)
	{
		*argc = 1;
		return no_arguments;
	}
	split_command_line(line, argv, (char *)(argv + count + 1), &size);
	*argc = count;
	return argv;
}

/* ---------- the entry point */

void __cdecl halo9x_start(void)
{
	int argc;
	char **argv;

	w9x_cpu_check();
	w9x_fpu_reset();
	read_version();
	w9x_crt_start();
	open_standard_streams();
	run_initializers();
	argv = make_argv(&argc);
	exit(main(argc, argv));
}
