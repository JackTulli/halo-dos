/*
W95_PROFILE.C

Testing (setting profile=1): a thread wakes every millisecond, stops the
game's thread for a moment and notes where it is: its instruction pointer
and the return addresses of its frame pointer chain (every unit keeps its
frame pointer). At exit the samples go to PROFILE.BIN beside the program,
each one PROFILE_DEPTH addresses (the first the instruction pointer, then
callers, zero after the last), for a script that names them from
HALO95.map: the functions the time is spent in, and under.

profile=2 samples from the start, not the first frame, and writes the
samples so far every five seconds as well (by the sampler, through Win32
alone: no lock the game's thread may hold), for a game that hangs before
it draws or never exits.
*/

#include <windows.h>
#include <mmsystem.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "halo_system.h"

long dos_setting_integer(const char *name, long default_value);

#define PROFILE_DEPTH 12
#define PROFILE_SAMPLES 200000

static HANDLE game_thread;
static unsigned long *samples;
static volatile long sample_count;
static volatile int stopping;
static volatile int sampling;
static unsigned long stack_low, stack_high;
static int snapshots;

static void write_snapshot(void)
{
	char path[MAX_PATH];
	HANDLE file;
	DWORD written;

	snprintf(path, sizeof(path), "%s\\PROFILE.BIN", system_program_directory());
	file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return;
	WriteFile(file, samples, (DWORD)(sample_count * PROFILE_DEPTH * sizeof(*samples)), &written, NULL);
	CloseHandle(file);
}

static DWORD WINAPI sampler(LPVOID parameter)
{
	(void)parameter;
	while (!stopping && sample_count < PROFILE_SAMPLES)
	{
		CONTEXT context;
		unsigned long *sample;

		Sleep(1);
		if (!sampling)
			continue;
		if (SuspendThread(game_thread) == (DWORD)-1)
			break;
		context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
		if (GetThreadContext(game_thread, &context))
		{
			unsigned long frame = context.Ebp;
			int depth = 1;

			sample = &samples[sample_count * PROFILE_DEPTH];
			memset(sample, 0, PROFILE_DEPTH * sizeof(*sample));
			sample[0] = context.Eip;
			/* (a frame: the caller's frame pointer, then the return address) */
			while (depth < PROFILE_DEPTH && frame >= stack_low && frame + 8 <= stack_high && !(frame & 3))
			{
				unsigned long const *words = (unsigned long const *)frame;

				sample[depth++] = words[1];
				if (words[0] <= frame)
					break;
				frame = words[0];
			}
			sample_count++;
		}
		ResumeThread(game_thread);
		if (snapshots && sample_count % 5000 == 0)
			write_snapshot();
	}
	return 0;
}

static void write_samples(void)
{
	char path[MAX_PATH];
	FILE *file;

	stopping = TRUE;
	if (!samples)
		return;
	snprintf(path, sizeof(path), "%s\\PROFILE.BIN", system_program_directory());
	file = fopen(path, "wb");
	if (file)
	{
		fwrite(samples, PROFILE_DEPTH * sizeof(*samples), (size_t)sample_count, file);
		fclose(file);
	}
	{
		char line[96];

		snprintf(line, sizeof(line), "profile: %ld samples in PROFILE.BIN", (long)sample_count);
		system_log(line);
	}
}

void system_profile_frame(void)
{
	sampling = samples != NULL;
}


void w95_profile_start(void)
{
	unsigned long top;
	HANDLE thread;
	DWORD identifier;

	long mode = dos_setting_integer("profile", 0);

	if (!mode)
		return;
	snapshots = mode >= 2;
	sampling = snapshots;
	samples = malloc((size_t)PROFILE_SAMPLES * PROFILE_DEPTH * sizeof(*samples));
	if (!samples || !DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &game_thread, 0,
		FALSE, DUPLICATE_SAME_ACCESS))
	{
		system_log("profile: could not start");
		return;
	}
	/* (the game thread's stack: from its top, the thread information
	block's second word, down as far as the program's reserve, 8 MB) */
	__asm__ volatile("movl %%fs:4, %0" : "=r"(top));
	stack_high = top;
	stack_low = top - 0x800000;
	timeBeginPeriod(1);
	atexit(write_samples);
	thread = CreateThread(NULL, 0, sampler, NULL, 0, &identifier);
	if (!thread)
	{
		system_log("profile: could not start");
		return;
	}
	SetThreadPriority(thread, THREAD_PRIORITY_TIME_CRITICAL);
	system_log("profile: sampling every millisecond");
}
