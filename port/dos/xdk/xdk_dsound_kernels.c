/*
XDK_DSOUND_KERNELS.C (halo-ce-universal's dsound_mix_kernels.c)

The inner loops of the console's integer mixer (dsound_sdl.c, the
console's part; the other ports mix in floating point there and leave
this unit empty).

They run once for every voice and every frame of the 48 kHz output, the
mixer's whole cost. Inside mix_voice_fixed they shared a frame with its
floating point gain math, whose SSE spills made the compiler realign the
stack and keep a frame pointer, and they tested the end of the packet,
interpolated a mono voice's one channel twice and shifted a held gain again
on every frame. Here each is a function of its own, in integers only (this
unit builds with -O2 and without a frame pointer on the console: a seventh
register), for the frames that have their next frame in the packet; the
packet's last frames, and the step into the next packet, stay with
mix_voice_fixed. The arithmetic is its loop's, operation for operation, so
the output is the same to the bit (compared on the host with the loop
they replace, over random streams of packets, rates, pitch changes and
gains). No MMX: the emulator runs it in helper calls.
*/

#include "dsound_mix_kernels.h"

/* (the console, Windows 98/ME, and the DOS and Windows 95 builds, which mix
as it does: xdk_dsound.c) */
#if defined(HALO_XBOX) || defined(HALO_WIN9X) || defined(HALO_DOS)

/* the position's whole frames and the interpolation's weight, 0 to 32767 */
#define CURSOR_FRAME(cursor) ((cursor) >> 16)
#define CURSOR_BLEND(cursor) ((long)(((cursor) & 0xFFFF) >> 1))

__attribute__((noinline))
void dsound_mix_mono(const short *samples, long *output, unsigned long frames, unsigned long cursor,
	unsigned long step, long left, long right)
{
	long gain_left = left >> (GAIN_RAMP_SHIFT - GAIN_SHIFT);
	long gain_right = right >> (GAIN_RAMP_SHIFT - GAIN_SHIFT);
	long *end = output + frames * 2;

	for (; output != end; output += 2)
	{
		const short *a = samples + CURSOR_FRAME(cursor);
		long a0 = a[0];
		long sample = a0 + (((a[1] - a0) * CURSOR_BLEND(cursor)) >> 15);

		output[0] += (sample * gain_left) >> GAIN_SHIFT;
		output[1] += (sample * gain_right) >> GAIN_SHIFT;
		cursor += step;
	}
}

__attribute__((noinline))
void dsound_mix_mono_ramp(const short *samples, long *output, unsigned long frames, unsigned long cursor,
	unsigned long step, long left, long right, long ramp_left, long ramp_right)
{
	long *end = output + frames * 2;

	for (; output != end; output += 2)
	{
		const short *a = samples + CURSOR_FRAME(cursor);
		long a0 = a[0];
		long sample = a0 + (((a[1] - a0) * CURSOR_BLEND(cursor)) >> 15);

		output[0] += (sample * (left >> (GAIN_RAMP_SHIFT - GAIN_SHIFT))) >> GAIN_SHIFT;
		output[1] += (sample * (right >> (GAIN_RAMP_SHIFT - GAIN_SHIFT))) >> GAIN_SHIFT;
		left += ramp_left;
		right += ramp_right;
		cursor += step;
	}
}

__attribute__((noinline))
void dsound_mix_stereo(const short *samples, long *output, unsigned long frames, unsigned long cursor,
	unsigned long step, long left, long right)
{
	long gain_left = left >> (GAIN_RAMP_SHIFT - GAIN_SHIFT);
	long gain_right = right >> (GAIN_RAMP_SHIFT - GAIN_SHIFT);
	long *end = output + frames * 2;

	for (; output != end; output += 2)
	{
		const short *a = samples + CURSOR_FRAME(cursor) * 2;
		long blend = CURSOR_BLEND(cursor);
		long a0 = a[0], a1 = a[1];
		long sample_left = a0 + (((a[2] - a0) * blend) >> 15);
		long sample_right = a1 + (((a[3] - a1) * blend) >> 15);

		output[0] += (sample_left * gain_left) >> GAIN_SHIFT;
		output[1] += (sample_right * gain_right) >> GAIN_SHIFT;
		cursor += step;
	}
}

__attribute__((noinline))
void dsound_mix_stereo_ramp(const short *samples, long *output, unsigned long frames, unsigned long cursor,
	unsigned long step, long left, long right, long ramp_left, long ramp_right)
{
	long *end = output + frames * 2;

	for (; output != end; output += 2)
	{
		const short *a = samples + CURSOR_FRAME(cursor) * 2;
		long blend = CURSOR_BLEND(cursor);
		long a0 = a[0], a1 = a[1];
		long sample_left = a0 + (((a[2] - a0) * blend) >> 15);
		long sample_right = a1 + (((a[3] - a1) * blend) >> 15);

		output[0] += (sample_left * (left >> (GAIN_RAMP_SHIFT - GAIN_SHIFT))) >> GAIN_SHIFT;
		output[1] += (sample_right * (right >> (GAIN_RAMP_SHIFT - GAIN_SHIFT))) >> GAIN_SHIFT;
		left += ramp_left;
		right += ramp_right;
		cursor += step;
	}
}

#endif
