/*
DOS_PROFILE.C

Testing (setting profile=1): where the DOS build spends its time, sampled
a thousand times a second by the timer's interrupt (dos_profile_isr.s). At exit
the samples go to PROFILE.BIN beside the program, as the Windows 95 build
writes them (port/dos/win95/w95_profile.c: 12 words a sample, the EIP then
its callers), after a first record that says where the code was loaded:
"PROF", the address of dos_profile_start, the samples a second.
tools/dos_profile.py names them from HALO.map.

The timer chip's channel 0 runs at 1000 Hz while it samples, and is put
back to the BIOS's 18.2 at exit (one interrupt in 55 still reaches the
BIOS's handler meanwhile, so DOS keeps its time).
*/

#include <stdio.h>
#include <stdlib.h>

#include "dos_dpmi.h"
#include "halo_system.h"

int dos_setting_boolean(const char *name, int default_value);
void dos_profile_start(void);
long dos_setting_integer(const char *name, long default_value);

#define PROFILE_DEPTH 12
#define PROFILE_SAMPLES 120000
#define PROFILE_RATE 1000
/* the timer chip's input clock (1.193182 MHz) over the rate */
#define TIMER_DIVISOR 1193

void dos_profile_isr(void);
extern char dos_profile_isr_end;
extern unsigned short dos_profile_data_selector;
extern unsigned char dos_profile_old_vector[6];
extern unsigned long *volatile dos_profile_samples;
extern volatile unsigned long dos_profile_count;
extern unsigned long dos_profile_capacity;
extern unsigned long dos_profile_stack_low, dos_profile_stack_high;
extern char dos_profile_data_end;
extern volatile unsigned long dos_profile_on;

static unsigned short old_selector;
static unsigned long old_offset;
static int installed;

static void set_timer_divisor(unsigned divisor)
{
	dos_outb(0x43, 0x36);
	dos_outb(0x40, (unsigned char)(divisor & 0xFF));
	dos_outb(0x40, (unsigned char)(divisor >> 8));
}

static void profile_stop(void)
{
	char path[260], line[96];
	unsigned long header[PROFILE_DEPTH] = { 0 };
	FILE *file;

	if (!installed)
		return;
	installed = 0;
	__asm__ __volatile__("cli");
	set_timer_divisor(0);
	dos_set_interrupt_vector(8, old_selector, old_offset);
	__asm__ __volatile__("sti");
	header[0] = 0x464F5250;
	header[1] = (unsigned long)dos_profile_start;
	header[2] = PROFILE_RATE;
	snprintf(path, sizeof(path), "%s\\PROFILE.BIN", system_program_directory());
	file = fopen(path, "wb");
	if (file)
	{
		fwrite(header, sizeof(header), 1, file);
		fwrite(dos_profile_samples, PROFILE_DEPTH * sizeof(unsigned long), (size_t)dos_profile_count, file);
		fclose(file);
	}
	snprintf(line, sizeof(line), "profile: %lu samples in PROFILE.BIN", dos_profile_count);
	system_log(line);
}

void system_profile_frame(void)
{
	dos_profile_on = installed;
}

void dos_profile_start(void)
{
	unsigned long esp;
	unsigned long *samples;

	if (installed || !dos_setting_integer("profile", 0))
		return;
	samples = malloc((size_t)PROFILE_SAMPLES * PROFILE_DEPTH * sizeof(unsigned long));
	if (!samples)
	{
		system_log("profile: no memory for the samples");
		return;
	}
	/* the game's stack: from a little above this frame (main's, and the
	start-up's, are about as deep: the game's frames are below), down by
	the program's 256 KB */
	__asm__ __volatile__("movl %%esp, %0" : "=r"(esp));
	dos_profile_stack_high = esp + 4096 - 8;
	dos_profile_stack_low = esp + 4096 - 0x40000 - 4096;
	dos_profile_samples = samples;
	dos_profile_capacity = PROFILE_SAMPLES;
	dos_profile_count = 0;
	dos_profile_data_selector = dos_data_selector();
	dos_dpmi_lock((const void *)dos_profile_isr, (unsigned long)(&dos_profile_isr_end - (char *)dos_profile_isr));
	dos_dpmi_lock((const void *)&dos_profile_samples,
		(unsigned long)(&dos_profile_data_end - (char *)&dos_profile_samples));
	dos_dpmi_lock(samples, (unsigned long)PROFILE_SAMPLES * PROFILE_DEPTH * sizeof(unsigned long));
	dos_get_interrupt_vector(8, &old_selector, &old_offset);
	*(unsigned long *)&dos_profile_old_vector[0] = old_offset;
	*(unsigned short *)&dos_profile_old_vector[4] = old_selector;
	__asm__ __volatile__("cli");
	dos_set_interrupt_vector(8, dos_code_selector(), (unsigned long)dos_profile_isr);
	set_timer_divisor(TIMER_DIVISOR);
	__asm__ __volatile__("sti");
	installed = 1;
	/* (profile=2: from the start, the loading too, not the first frame) */
	if (dos_setting_integer("profile", 0) >= 2)
		dos_profile_on = 1;
	dos_at_exit(profile_stop);
	system_log("profile: sampling a thousand times a second");
}
