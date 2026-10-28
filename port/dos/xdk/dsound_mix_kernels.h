/*
DSOUND_MIX_KERNELS.H

The inner loops of the console's integer mixer (dsound_mix_kernels.c),
which dsound_sdl.c runs over the frames of a packet that have their next
frame in the same packet.

Each mixes frames output frames of one voice into output (stereo pairs at
the 16-bit output's scale): the source position is cursor, in 16.16 source
frames, advanced by step every frame; the sample is interpolated between
the frame at the position and the next one; the gains are fixed point
(GAIN_RAMP_SHIFT), held or ramped by ramp_left and ramp_right every frame.
frames is at least 1, and every frame's position plus one lies in samples.
*/

#ifndef __HALO_LINUX_DSOUND_MIX_KERNELS_H
#define __HALO_LINUX_DSOUND_MIX_KERNELS_H

/* the gains: 1.0 is 1 << GAIN_RAMP_SHIFT while they ramp, 1 << GAIN_SHIFT
when applied */
#define GAIN_RAMP_SHIFT 20
#define GAIN_SHIFT 12

/* a mono voice, its one sample to both sides */
void dsound_mix_mono(const short *samples, long *output, unsigned long frames, unsigned long cursor,
	unsigned long step, long left, long right);
void dsound_mix_mono_ramp(const short *samples, long *output, unsigned long frames, unsigned long cursor,
	unsigned long step, long left, long right, long ramp_left, long ramp_right);

/* a stereo voice, interleaved frames */
void dsound_mix_stereo(const short *samples, long *output, unsigned long frames, unsigned long cursor,
	unsigned long step, long left, long right);
void dsound_mix_stereo_ramp(const short *samples, long *output, unsigned long frames, unsigned long cursor,
	unsigned long step, long left, long right, long ramp_left, long ramp_right);

#endif
