/*
W95_AUDIO.C

Windows 95's sound device (halo_system.h): the wave mapper, 16-bit stereo,
fed from the game's thread. The mixer (port/dos/xdk/xdk_dsound.c) asks
every frame and in the game's waits; each time, the blocks the device has
played come back and are filled again until lead frames are queued
(BLOCKS of BLOCK_FRAMES: 11.6 ms each at 22.05 kHz, 370 ms in all). Should
the game stall past them the device simply runs dry and starts again with
the next block. No thread: these builds have one (the wave mapper's
callbacks would run beside the mixer), and Windows 95 has neither
DirectSound in every installation nor a mapper that likes being driven
from another thread on every driver.

The setting sound_record=<file> writes the mix to a .WAV file instead
(sys_audio_record.c).
*/

#include <windows.h>
#include <mmsystem.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "halo_system.h"

int sys_audio_record_open(unsigned long rate);
int sys_audio_recording(void);
void sys_audio_record_update(void (*fill)(short *samples, unsigned long frames));
void sys_audio_record_close(void);

#define BLOCK_FRAMES 256
#define BLOCKS 32

static struct
{
	HWAVEOUT device;
	WAVEHDR headers[BLOCKS];
	short samples[BLOCKS][BLOCK_FRAMES * 2];
	/* the oldest block in the device, and how many are */
	int oldest, queued;
	int open;
} audio;

static void audio_close_at_exit(void)
{
	system_audio_close();
}

int system_audio_open(unsigned long rate, unsigned long *opened_rate)
{
	static const unsigned long rates[] = { 0, 22050, 11025 };
	char line[96];

	if (sys_audio_record_open(rate))
	{
		*opened_rate = rate;
		atexit(audio_close_at_exit);
		return TRUE;
	}
	if (!waveOutGetNumDevs())
	{
		system_log("sound: Windows has no wave device");
		return FALSE;
	}
	/* (the rate asked, else 22.05 or 11.025 kHz, which every card plays) */
	for (int i = 0; i < (int)(sizeof(rates) / sizeof(rates[0])); i++)
	{
		WAVEFORMATEX format;
		MMRESULT result;

		memset(&format, 0, sizeof(format));
		format.wFormatTag = WAVE_FORMAT_PCM;
		format.nChannels = 2;
		format.nSamplesPerSec = i ? rates[i] : rate;
		format.wBitsPerSample = 16;
		format.nBlockAlign = 4;
		format.nAvgBytesPerSec = format.nSamplesPerSec * 4;
		result = waveOutOpen(&audio.device, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL);
		if (result == MMSYSERR_NOERROR)
		{
			*opened_rate = format.nSamplesPerSec;
			break;
		}
		snprintf(line, sizeof(line), "sound: the wave mapper refused %lu Hz (%u)",
			(unsigned long)format.nSamplesPerSec, (unsigned)result);
		system_log(line);
		audio.device = NULL;
	}
	if (!audio.device)
		return FALSE;
	for (int i = 0; i < BLOCKS; i++)
	{
		WAVEHDR *header = &audio.headers[i];

		memset(header, 0, sizeof(*header));
		header->lpData = (LPSTR)audio.samples[i];
		header->dwBufferLength = BLOCK_FRAMES * 4;
		waveOutPrepareHeader(audio.device, header, sizeof(*header));
	}
	audio.oldest = 0;
	audio.queued = 0;
	audio.open = TRUE;
	atexit(audio_close_at_exit);
	snprintf(line, sizeof(line), "sound: the wave mapper at %lu Hz", *opened_rate);
	system_log(line);
	return TRUE;
}

void system_audio_update(void (*fill)(short *samples, unsigned long frames), unsigned long lead)
{
	if (sys_audio_recording())
	{
		sys_audio_record_update(fill);
		return;
	}
	if (!audio.open)
		return;
	/* (the blocks it has played, oldest first: the device sets their
	flag) */
	while (audio.queued && (((volatile WAVEHDR *)&audio.headers[audio.oldest])->dwFlags & WHDR_DONE))
	{
		audio.oldest = (audio.oldest + 1) % BLOCKS;
		audio.queued--;
	}
	while (audio.queued < BLOCKS && (unsigned long)audio.queued * BLOCK_FRAMES < lead)
	{
		int index = (audio.oldest + audio.queued) % BLOCKS;
		WAVEHDR *header = &audio.headers[index];

		fill(audio.samples[index], BLOCK_FRAMES);
		header->dwFlags &= ~WHDR_DONE;
		if (waveOutWrite(audio.device, header, sizeof(*header)) != MMSYSERR_NOERROR)
			break;
		audio.queued++;
	}
}

void system_audio_close(void)
{
	sys_audio_record_close();
	if (!audio.open)
		return;
	audio.open = FALSE;
	waveOutReset(audio.device);
	for (int i = 0; i < BLOCKS; i++)
		waveOutUnprepareHeader(audio.device, &audio.headers[i], sizeof(audio.headers[i]));
	waveOutClose(audio.device);
	audio.device = NULL;
}
