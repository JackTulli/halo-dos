/*
W95_START.C

The Windows 95 build's start-up (lld-link -entry:w95_start): the 9x
build's (port/win9x/src/w9x_start.c) for any Pentium. The floating-point
state the game computes in, the C runtime's start (the standard streams on
the process's handles), argv split from the command line, the system
layer, then the game's main. One thread, as on DOS: no TLS directory, no
initializers (the build compiles thread-locals as plain storage).
*/

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <threads.h>

#include "w9x_runtime.h"

int main(int argc, char **argv);
void dos_p2p_state_initialize(void);
void w9x_crt_start(void);
void system_initialize(void);
extern int system_argc;
extern char **system_argv;

/* referenced by every unit that uses floating point (MSVC's convention) */
int _fltused = 0x9875;

/* GetVersionExA's answer, which the 9x runtime reads */
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
	w9x_build_number = version.dwPlatformId == VER_PLATFORM_WIN32_WINDOWS ? version.dwBuildNumber & 0xFFFF :
		version.dwBuildNumber;
}

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

/* argv as the Microsoft runtime splits a command line, simply: blanks
separate, quotes group */
static char **make_argv(int *argc)
{
	static char *arguments[128];
	const char *line = GetCommandLineA();
	size_t length = line ? strlen(line) : 0;
	char *text = malloc(length + 1), *cursor = text;
	int count = 0;

	if (!text)
	{
		static char *none[2] = { "halo95", NULL };

		*argc = 1;
		return none;
	}
	memcpy(text, line ? line : "", length + 1);
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

void __cdecl w95_start(void)
{
	int argc;
	char **argv;

	w9x_fpu_reset();
	read_version();
	w9x_crt_start();
	open_standard_streams();
	argv = make_argv(&argc);
	system_argc = argc;
	system_argv = argv;
	system_initialize();
	/* (internet play's state: its sockets none, p2p.c) */
	dos_p2p_state_initialize();
	exit(main(argc, argv));
}
