/*
SYS_AUDIO_RECORD.C

Testing, for DOS and Windows 95 alike: with the setting sound_record=<file>
the sound device is a .WAV file (16-bit stereo at the asked rate) that the
mix is written to at its real rate, as a device would play it, instead of
a Sound Blaster or the wave mapper: the mixer can be heard afterwards, and
checked without a sound coming out of the machine running the test.
*/

#include <stdio.h>
#include <string.h>

#include "halo_system.h"

long dos_setting_integer(const char *name, long default_value);
const char *dos_setting(const char *name, const char *default_value);

static struct
{
	FILE *file;
	unsigned long rate;
	unsigned long long start;
	unsigned long long written;
} record;

static void put32(unsigned char *at, unsigned long value)
{
	at[0] = (unsigned char)value;
	at[1] = (unsigned char)(value >> 8);
	at[2] = (unsigned char)(value >> 16);
	at[3] = (unsigned char)(value >> 24);
}

static void write_header(void)
{
	unsigned char header[44];
	unsigned long bytes = (unsigned long)record.written * 4;

	memcpy(header, "RIFF", 4);
	put32(header + 4, 36 + bytes);
	memcpy(header + 8, "WAVEfmt ", 8);
	put32(header + 16, 16);
	header[20] = 1, header[21] = 0;
	header[22] = 2, header[23] = 0;
	put32(header + 24, record.rate);
	put32(header + 28, record.rate * 4);
	header[32] = 4, header[33] = 0;
	header[34] = 16, header[35] = 0;
	memcpy(header + 36, "data", 4);
	put32(header + 40, bytes);
	fseek(record.file, 0, SEEK_SET);
	fwrite(header, 1, sizeof(header), record.file);
	fseek(record.file, 0, SEEK_END);
}

int sys_audio_record_open(unsigned long rate)
{
	const char *name = dos_setting("sound_record", "");
	char line[160];

	if (!name || !name[0] || (name[0] == '0' && !name[1]))
		return 0;
	record.file = fopen(name, "wb");
	if (!record.file)
		return 0;
	record.rate = rate;
	record.written = 0;
	record.start = system_microseconds();
	write_header();
	snprintf(line, sizeof(line), "sound: recorded to %s", name);
	system_log(line);
	return 1;
}

int sys_audio_recording(void)
{
	return record.file != NULL;
}

/* the frames that have played since the start, as a device would */
void sys_audio_record_update(void (*fill)(short *samples, unsigned long frames))
{
	short samples[512 * 2];
	unsigned long long due;

	if (!record.file)
		return;
	due = (system_microseconds() - record.start) * record.rate / 1000000ULL;
	while (record.written < due)
	{
		unsigned long count = due - record.written > 512 ? 512 : (unsigned long)(due - record.written);

		fill(samples, count);
		fwrite(samples, 4, count, record.file);
		record.written += count;
	}
}

void sys_audio_record_close(void)
{
	if (!record.file)
		return;
	write_header();
	fclose(record.file);
	record.file = NULL;
}
