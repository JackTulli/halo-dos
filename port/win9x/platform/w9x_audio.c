/*
W9X_AUDIO.C

Sound output on Windows 98/ME (see w9x_backend.h): the game's mixer
(dsound_sdl.c, the console's integer one) writes 16-bit stereo frames into
a looping DirectSound buffer, or, without DirectSound, into the wave
mapper's buffers (audio.output).

dsound.dll is loaded at run time. Its cooperative level is the priority
one, which may set the primary buffer's format: otherwise 9x mixes
everything down to its default 22 kHz 8-bit. The rate comes from the
primary buffer, read back after asking: the first of 44100, 48000 and 22050
(or audio.rate) that it plays as asked, so DirectSound does not resample
what the mixer already resampled; the mixer then runs at that rate. The
secondary buffer has DirectX 3's description size, which every runtime
accepts, and asks for the accurate play position
(DSBCAPS_GETCURRENTPOSITION2, retried without it for runtimes older than
DirectX 5).

A thread of time-critical priority (above the game's) keeps the buffer
filled some way ahead of the play position: 2048 frames to begin with,
more for each time the play position caught up with it (a slow driver,
or the game holding the processor), up to 6144. The wave mapper's path
queues eight buffers of 512 frames and refills each as it comes back.
*/

#include "w9x_internal.h"

#include <mmsystem.h>
#include <dsound.h>
#include <stdio.h>
#include <string.h>

#define RING_FRAMES 8192
#define LATENCY_START_FRAMES 2048
#define LATENCY_STEP_FRAMES 512
#define LATENCY_MAXIMUM_FRAMES 6144
#define FRAME_BYTES 4
#define WAVE_BUFFERS 8
#define WAVE_BUFFER_FRAMES 512

typedef HRESULT (WINAPI *direct_sound_create_function)(LPGUID device, LPDIRECTSOUND *result, LPUNKNOWN outer);

static void (*audio_fill)(short *samples, unsigned long frames);
static unsigned long audio_rate;

static HMODULE dsound_library;
static LPDIRECTSOUND direct_sound;
static LPDIRECTSOUNDBUFFER primary_buffer;
static LPDIRECTSOUNDBUFFER sound_buffer;
static unsigned long latency_frames = LATENCY_START_FRAMES;
static unsigned long underruns;

static HWAVEOUT wave_out;
static HANDLE wave_event;
static WAVEHDR wave_headers[WAVE_BUFFERS];
static short wave_samples[WAVE_BUFFERS][WAVE_BUFFER_FRAMES * 2];

/* the game's floating point state, which the thread starts with */
static struct w9x_fpu thread_fpu;

static void format_describe(WAVEFORMATEX *format, unsigned long rate)
{
	memset(format, 0, sizeof(*format));
	format->wFormatTag = WAVE_FORMAT_PCM;
	format->nChannels = 2;
	format->nSamplesPerSec = rate;
	format->wBitsPerSample = 16;
	format->nBlockAlign = FRAME_BYTES;
	format->nAvgBytesPerSec = rate * FRAME_BYTES;
}

/* ---------- DirectSound */

static void direct_sound_close(void)
{
	if (sound_buffer)
		sound_buffer->lpVtbl->Release(sound_buffer);
	if (primary_buffer)
		primary_buffer->lpVtbl->Release(primary_buffer);
	if (direct_sound)
		direct_sound->lpVtbl->Release(direct_sound);
	sound_buffer = NULL;
	primary_buffer = NULL;
	direct_sound = NULL;
}

/* the priority level, on the game's window once it exists */
static BOOL direct_sound_cooperate(HWND window)
{
	struct w9x_fpu fpu;
	HRESULT result;

	w9x_fpu_save(&fpu);
	result = direct_sound->lpVtbl->SetCooperativeLevel(direct_sound, window, DSSCL_PRIORITY);
	w9x_fpu_check(&fpu, "IDirectSound::SetCooperativeLevel");
	return SUCCEEDED(result);
}

/* asks the primary buffer for rate; the rate it really took */
static unsigned long primary_rate(unsigned long rate)
{
	WAVEFORMATEX format;
	DWORD written = 0;

	if (!primary_buffer)
		return 0;
	format_describe(&format, rate);
	primary_buffer->lpVtbl->SetFormat(primary_buffer, &format);
	memset(&format, 0, sizeof(format));
	if (FAILED(primary_buffer->lpVtbl->GetFormat(primary_buffer, &format, sizeof(format), &written)))
		return 0;
	return format.nSamplesPerSec;
}

static BOOL direct_sound_open(unsigned long rate_wanted, unsigned long *rate)
{
	static const unsigned long rates[] = { 44100, 48000, 22050 };
	direct_sound_create_function create;
	DSBUFFERDESC1 description;
	WAVEFORMATEX format;
	struct w9x_fpu fpu;
	unsigned long chosen = 0;
	HRESULT result;
	int index;

	w9x_fpu_save(&fpu);
	dsound_library = LoadLibraryA("dsound.dll");
	w9x_fpu_check(&fpu, "LoadLibraryA(dsound.dll)");
	create = dsound_library ? (direct_sound_create_function)GetProcAddress(dsound_library, "DirectSoundCreate") : NULL;
	if (!create)
	{
		platform_log("sound: no DirectSound");
		return FALSE;
	}
	w9x_fpu_save(&fpu);
	result = create(NULL, &direct_sound, NULL);
	w9x_fpu_check(&fpu, "DirectSoundCreate");
	if (FAILED(result))
	{
		platform_log("sound: DirectSoundCreate failed (%08lx)", (unsigned long)result);
		direct_sound = NULL;
		return FALSE;
	}
	/* before the game's window exists: the desktop's, until
	w9x_audio_window_opened */
	if (!direct_sound_cooperate(w9x_window() ? w9x_window() : GetDesktopWindow()))
	{
		platform_log("sound: DirectSound's priority level was refused; mixing at its default rate");
		direct_sound->lpVtbl->SetCooperativeLevel(direct_sound, GetDesktopWindow(), DSSCL_NORMAL);
	}
	else
	{
		memset(&description, 0, sizeof(description));
		description.dwSize = sizeof(description);
		description.dwFlags = DSBCAPS_PRIMARYBUFFER;
		if (FAILED(direct_sound->lpVtbl->CreateSoundBuffer(direct_sound, (LPCDSBUFFERDESC)&description,
			&primary_buffer, NULL)))
		{
			primary_buffer = NULL;
		}
	}
	if (rate_wanted)
	{
		/* (the mixer runs at the rate asked for, whatever the primary
		buffer took: DirectSound converts) */
		primary_rate(rate_wanted);
		chosen = rate_wanted;
	}
	else
	{
		for (index = 0; index < (int)(sizeof(rates) / sizeof(rates[0])) && !chosen; index++)
		{
			if (primary_rate(rates[index]) == rates[index])
				chosen = rates[index];
		}
		/* none as asked: whatever the primary buffer plays at */
		if (!chosen)
			chosen = primary_rate(rates[0]);
		if (!chosen)
			chosen = rates[0];
	}

	format_describe(&format, chosen);
	memset(&description, 0, sizeof(description));
	description.dwSize = sizeof(description);
	description.dwFlags = DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_GLOBALFOCUS;
	description.dwBufferBytes = RING_FRAMES * FRAME_BYTES;
	description.lpwfxFormat = &format;
	result = direct_sound->lpVtbl->CreateSoundBuffer(direct_sound, (LPCDSBUFFERDESC)&description, &sound_buffer, NULL);
	if (FAILED(result))
	{
		description.dwFlags = DSBCAPS_GLOBALFOCUS;
		result = direct_sound->lpVtbl->CreateSoundBuffer(direct_sound, (LPCDSBUFFERDESC)&description, &sound_buffer,
			NULL);
	}
	if (FAILED(result))
	{
		platform_log("sound: DirectSound would not make a %lu Hz buffer (%08lx)", chosen, (unsigned long)result);
		sound_buffer = NULL;
		direct_sound_close();
		return FALSE;
	}
	*rate = chosen;
	platform_log("sound: DirectSound, %lu Hz", chosen);
	return TRUE;
}

/* the buffer kept latency_frames ahead of the play position */
static void direct_sound_service(DWORD *written)
{
	const DWORD ring = RING_FRAMES * FRAME_BYTES;
	DWORD play, write, queued, wanted;
	void *first, *second;
	DWORD first_size, second_size;
	HRESULT result = sound_buffer->lpVtbl->GetCurrentPosition(sound_buffer, &play, &write);

	if (result == DSERR_BUFFERLOST)
	{
		sound_buffer->lpVtbl->Restore(sound_buffer);
		sound_buffer->lpVtbl->Play(sound_buffer, 0, 0, DSBPLAY_LOOPING);
		return;
	}
	if (FAILED(result))
		return;
	queued = (*written + ring - play) % ring;
	/* the play position went past what was written: on from just ahead of
	where it may be written now, with more ahead from now on. Nothing is
	ever written more than latency_frames ahead of the play position, so
	more queued than that is the play position a lap ahead (a fixed bound
	above the largest latency missed every stall longer than 35 ms past it,
	and played most of the ring's old sound before writing again) */
	if (queued > latency_frames * FRAME_BYTES)
	{
		*written = write;
		queued = (*written + ring - play) % ring;
		underruns++;
		if (latency_frames < LATENCY_MAXIMUM_FRAMES)
		{
			latency_frames += LATENCY_STEP_FRAMES;
			platform_log("sound: the play position caught up %lu times; %lu frames ahead now", underruns,
				latency_frames);
		}
	}
	if (queued >= latency_frames * FRAME_BYTES)
		return;
	wanted = (latency_frames * FRAME_BYTES - queued) & ~(DWORD)(FRAME_BYTES - 1);
	result = sound_buffer->lpVtbl->Lock(sound_buffer, *written, wanted, &first, &first_size, &second, &second_size, 0);
	if (result == DSERR_BUFFERLOST)
	{
		sound_buffer->lpVtbl->Restore(sound_buffer);
		return;
	}
	if (FAILED(result))
		return;
	audio_fill((short *)first, first_size / FRAME_BYTES);
	if (second && second_size)
		audio_fill((short *)second, second_size / FRAME_BYTES);
	sound_buffer->lpVtbl->Unlock(sound_buffer, first, first_size, second, second_size);
	*written = (*written + first_size + second_size) % ring;
}

/* ---------- the wave mapper */

static BOOL wave_out_open(unsigned long rate_wanted, unsigned long *rate)
{
	static const unsigned long rates[] = { 44100, 48000, 22050 };
	WAVEFORMATEX format;
	struct w9x_fpu fpu;
	MMRESULT result = MMSYSERR_ERROR;
	int index;

	wave_event = CreateEventA(NULL, FALSE, FALSE, NULL);
	if (!wave_event)
		return FALSE;
	for (index = 0; index < (int)(sizeof(rates) / sizeof(rates[0])); index++)
	{
		unsigned long candidate = rate_wanted ? rate_wanted : rates[index];

		format_describe(&format, candidate);
		w9x_fpu_save(&fpu);
		result = waveOutOpen(&wave_out, WAVE_MAPPER, &format, (DWORD_PTR)wave_event, 0, CALLBACK_EVENT);
		w9x_fpu_check(&fpu, "waveOutOpen");
		if (result == MMSYSERR_NOERROR)
		{
			*rate = candidate;
			platform_log("sound: the wave mapper, %lu Hz", candidate);
			return TRUE;
		}
		if (rate_wanted)
			break;
	}
	platform_log("sound: the wave mapper would not open (%u)", (unsigned)result);
	CloseHandle(wave_event);
	wave_event = NULL;
	return FALSE;
}

static void wave_out_start(void)
{
	int index;

	for (index = 0; index < WAVE_BUFFERS; index++)
	{
		wave_headers[index].lpData = (LPSTR)wave_samples[index];
		wave_headers[index].dwBufferLength = sizeof(wave_samples[index]);
		waveOutPrepareHeader(wave_out, &wave_headers[index], sizeof(WAVEHDR));
		audio_fill(wave_samples[index], WAVE_BUFFER_FRAMES);
		waveOutWrite(wave_out, &wave_headers[index], sizeof(WAVEHDR));
	}
}

static void wave_out_service(void)
{
	int index;

	for (index = 0; index < WAVE_BUFFERS; index++)
	{
		if (wave_headers[index].dwFlags & WHDR_DONE)
		{
			audio_fill(wave_samples[index], WAVE_BUFFER_FRAMES);
			waveOutWrite(wave_out, &wave_headers[index], sizeof(WAVEHDR));
		}
	}
}

/* ---------- the thread */

static DWORD WINAPI audio_thread(LPVOID parameter)
{
	DWORD written = 0;

	(void)parameter;
	/* the game's precision and rounding here too (the mixer works its
	gains out in floating point) */
	w9x_fpu_check(&thread_fpu, NULL);
	if (sound_buffer)
	{
		void *first, *second;
		DWORD first_size, second_size;

		/* the first latency_frames written before it plays (written from 0
		while already playing, the play position was ahead of what was
		written: counted as a stall, and a longer delay from the start) */
		if (SUCCEEDED(sound_buffer->lpVtbl->Lock(sound_buffer, 0, latency_frames * FRAME_BYTES, &first, &first_size,
			&second, &second_size, 0)))
		{
			audio_fill((short *)first, first_size / FRAME_BYTES);
			if (second && second_size)
				audio_fill((short *)second, second_size / FRAME_BYTES);
			sound_buffer->lpVtbl->Unlock(sound_buffer, first, first_size, second, second_size);
			written = (first_size + second_size) % (RING_FRAMES * FRAME_BYTES);
		}
		sound_buffer->lpVtbl->Play(sound_buffer, 0, 0, DSBPLAY_LOOPING);
		for (;;)
		{
			direct_sound_service(&written);
			Sleep(5);
		}
	}
	wave_out_start();
	for (;;)
	{
		WaitForSingleObject(wave_event, 50);
		wave_out_service();
	}
	return 0;
}

/* ---------- public code */

int w9x_audio_open(const char *backend, unsigned long rate, unsigned long *opened_rate)
{
	BOOL wave_only = backend && !strcmp(backend, "waveout");
	BOOL direct_sound_only = backend && !strcmp(backend, "dsound");

	if (rate && (rate < 8000 || rate > 48000))
	{
		platform_log("sound: audio.rate %lu is out of range; choosing one", rate);
		rate = 0;
	}
	if (!wave_only && direct_sound_open(rate, opened_rate))
	{
		audio_rate = *opened_rate;
		return TRUE;
	}
	if (!direct_sound_only && wave_out_open(rate, opened_rate))
	{
		audio_rate = *opened_rate;
		return TRUE;
	}
	return FALSE;
}

void w9x_audio_run(void (*fill)(short *samples, unsigned long frames))
{
	HANDLE thread;
	DWORD identifier;

	audio_fill = fill;
	w9x_fpu_save(&thread_fpu);
	thread = CreateThread(NULL, 64 * 1024, audio_thread, NULL, 0, &identifier);
	if (!thread)
	{
		platform_log("sound: could not start the output thread");
		return;
	}
	SetThreadPriority(thread, THREAD_PRIORITY_TIME_CRITICAL);
	CloseHandle(thread);
}

void w9x_audio_window_opened(HWND window)
{
	unsigned long rate;

	if (!direct_sound)
		return;
	if (!direct_sound_cooperate(window))
		return;
	/* the primary format again: it is the cooperating window's */
	rate = primary_rate(audio_rate);
	if (rate && rate != audio_rate)
		platform_log("sound: the primary buffer plays %lu Hz now, the mixer %lu", rate, audio_rate);
}
