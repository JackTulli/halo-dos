/*
XDK_DSOUND.C

Xbox DirectSound for the DOS and Windows 95 builds: halo-ce-universal's
integer mixer (its port/linux/src/dsound_sdl.c, the one its Xbox and
Windows 98/ME builds mix with, there on a thread of their own), played out
by the system's sound device (halo_system.h: a Sound Blaster on DOS,
Windows' wave mapper on Windows 95). ChupathingyCE's mixer has the same
DirectSound entry points but mixes in floats with a sinc resampler and a
reverb, more than a Pentium of these builds has to spare.

The game plays everything through DirectSound streams: 16-bit stereo PCM
(music and other uncompressed sounds) and Xbox ADPCM, mono or stereo, at 22
or 44 kHz. A packet is decoded to 16-bit PCM when the game submits it, since
the sound cache may reuse its memory once the packet completes. For every
voice the mixer resamples to the output rate (which is how SetFrequency
changes pitch) and applies:
	- the stream volume (millibels),
	- the front left and right mix bin volumes of 2D voices,
	- for 3D voices, DirectSound's inverse distance rolloff between the
	  minimum and maximum distance, an equal power pan from the source's
	  direction in listener space, and the low frequency part of the I3DL2
	  direct path, obstruction and occlusion levels.
Doppler, the high frequency filters, cones and I3DL2 reverb are not
modelled.

There is one thread: the mixer runs when the game asks for it
(dsound_pump: DirectSoundDoWork every frame, and the waits and loading
screen in between), and fills the device that far ahead of what it plays
(the lead: a little more than the longest recent gap between asks, so a
slow frame does not leave a hole, and no more, so a shot is heard when it
is fired). Packets the mixer has finished are completed from
DirectSoundDoWork and from Flush. Without a device the same mixer drains
the voices into nothing at their real rate.

The settings (dos_settings.c): sound (1 on, 0 off: DirectSoundCreate fails
and the game runs without its sound engine), sound_volume (1.0), sound_rate
(22050; 11025 is lighter, 44100 for fast machines).
*/

#include "platform.h"
/* the console's integer mixer; the C runtime's math is the double one
(PDCLib's, with musl's) */
#define HALO_FIXED_MIXER 1
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "dsound_mix_kernels.h"
#define sqrtf(x) __builtin_sqrtf(x)
#define fabsf(x) __builtin_fabsf(x)
#define powf(x, y) ((float)pow((double)(x), (double)(y)))
#define cosf(x) ((float)cos((double)(x)))
/* (the C runtime may have its own) */
#define tanhf dsound_tanhf
static float tanhf(float x)
{
	float e = (float)exp(2.0 * (double)x);

	return 1.0f - 2.0f / (e + 1.0f);
}

long dos_setting_integer(const char *name, long default_value);
float dos_setting_real(const char *name, float default_value);

/* the rate the device took (system_audio_open) */
static unsigned long output_rate = 22050;
#define OUTPUT_RATE output_rate
#define OUTPUT_CHANNELS 2
/* the game attaches four at most (sound_dsound_xbox.c,
MAXIMUM_SOUND_PACKETS); each slot costs memory in each of its streams */
#define MAXIMUM_STREAM_PACKETS 8
/* the mixer holds its lock for a chunk, which the game's sound calls wait
behind: a quarter of a 1024-frame buffer at a time (the same mixing, a
quarter of the wait) */
#define MIX_CHUNK_FRAMES 256

#define XBOX_ADPCM_BLOCK_BYTES 36
#define XBOX_ADPCM_BLOCK_SAMPLES 64

/* ---------- voices */

struct voice_packet
{
	XMEDIAPACKET packet;
	short *samples;           /* interleaved, source channel count */
	unsigned long frames;
	BOOL finished;            /* played out by the mixer, not yet completed */
};

struct sdl_stream
{
	/* must be first: in C an IDirectSoundStream is just { lpVtbl } */
	IDirectSoundStream object;
	struct sdl_stream *next;
	ULONG reference_count;
	LPFNXMEDIAOBJECTCALLBACK callback;
	LPVOID context;

	/* format */
	BOOL adpcm;
	unsigned long channels;
	DWORD sample_rate;
	DWORD frequency;

	BOOL paused;

	/* 2D gains */
	float volume;             /* SetVolume */
	float mix_left, mix_right;
	float headroom;

	/* 3D */
	BOOL has_3d;
	DWORD mode;
	float position[3];
	float minimum_distance, maximum_distance;
	float i3dl2_gain;

	struct voice_packet packets[MAXIMUM_STREAM_PACKETS];
	unsigned long packet_head;
	unsigned long packet_count;
	/* position inside the head packet, in source frames */
	double cursor;
	/* the console's mixer keeps it in whole frames and 1/65536ths */
	unsigned long cursor_frame, cursor_fraction;
	/* the last frame of the previous packet, for interpolating across packets */
	float previous[2];
	/* gains the mixer is ramping from, to avoid clicks */
	float current_left, current_right;
	BOOL gains_valid;
	/* the gains it ramps to, as voice_gains last worked them out: kept
	until a setter of this stream marks them dirty, or the listener moves
	(listener_generation) */
	float target_left, target_right;
	BOOL gains_dirty;
	unsigned long gains_generation;
};

static pthread_mutex_t mixer_lock = PTHREAD_MUTEX_INITIALIZER;
static struct sdl_stream *streams;
/* for the console's diagnostics: streams made, packets taken, and the
frames the mixer last produced with any voice playing */
static unsigned long streams_created, packets_taken, frames_with_voices;

/* the listener, in DirectSound's left-handed +y up space */
static struct
{
	float position[3];
	float front[3];
	float top[3];
	float rolloff_factor;
	float distance_factor;
} listener = { { 0, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 }, 1.0f, 1.0f };

static float master_volume = 1.0f;
/* moves on with every change to the listener or the master volume, which
every voice's gains depend on */
static unsigned long listener_generation = 1;

static float gain_from_millibels(LONG millibels)
{
	if (millibels <= DSBVOLUME_MIN)
		return 0.0f;
	return powf(10.0f, (float)millibels / 2000.0f);
}

/* ---------- decoding */

static const int ima_index_table[16] =
{
	-1, -1, -1, -1, 2, 4, 6, 8,
	-1, -1, -1, -1, 2, 4, 6, 8,
};

static const int ima_step_table[89] =
{
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
	50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
	253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
	1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
	3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
	11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
	32767,
};

/* a nibble's step, as tables: for each step index and nibble, the
difference it adds to the predictor and the step index it leads to,
clamped (built once, in audio_start). The decoder then does a table look-up
for each, where it tested the nibble's four bits and clamped the index:
the same sums and clamps, at a third of the cost, on the game's thread,
which decodes every packet it submits */
#define ADPCM_STEP_INDICES 89
static int adpcm_difference[ADPCM_STEP_INDICES * 16];
static unsigned char adpcm_next_index[ADPCM_STEP_INDICES * 16];

static void adpcm_tables_build(void)
{
	int index, nibble;

	for (index = 0; index < ADPCM_STEP_INDICES; index++)
	{
		for (nibble = 0; nibble < 16; nibble++)
		{
			int step = ima_step_table[index];
			int difference = step >> 3;
			int next = index + ima_index_table[nibble];

			if (nibble & 1) difference += step >> 2;
			if (nibble & 2) difference += step >> 1;
			if (nibble & 4) difference += step;
			if (nibble & 8) difference = -difference;
			if (next < 0) next = 0;
			if (next > 88) next = 88;
			adpcm_difference[index * 16 + nibble] = difference;
			adpcm_next_index[index * 16 + nibble] = (unsigned char)next;
		}
	}
}

/* one nibble: the predictor moves and is clamped, and the step index (as
its row in the tables, index * 16) moves on */
static inline short adpcm_expand(unsigned long nibble, int *predictor, unsigned long *row)
{
	unsigned long entry = *row + nibble;
	int value = *predictor + adpcm_difference[entry];

	if (value > 32767) value = 32767;
	if (value < -32768) value = -32768;
	*predictor = value;
	*row = (unsigned long)adpcm_next_index[entry] * 16;
	return (short)value;
}

/* one channel of a block: its header, then eight groups of four bytes
(eight nibbles, low first), group_stride bytes apart, into every
output_stride'th sample */
static inline void decode_adpcm_channel(const unsigned char *header, const unsigned char *nibbles,
	unsigned long group_stride, short *output, unsigned long output_stride)
{
	int predictor = (short)(header[0] | (header[1] << 8));
	unsigned long row = (unsigned long)(header[2] > 88 ? 88 : header[2]) * 16;
	unsigned long group, byte;

	for (group = 0; group < 8; group++, nibbles += group_stride)
	{
		for (byte = 0; byte < 4; byte++)
		{
			output[0] = adpcm_expand(nibbles[byte] & 0xf, &predictor, &row);
			output[output_stride] = adpcm_expand(nibbles[byte] >> 4, &predictor, &row);
			output += 2 * output_stride;
		}
	}
}

/* Xbox ADPCM: per block, a 4-byte header per channel (predictor, step
index), then 4-byte groups of eight nibbles, low nibble first, alternating
between channels; 64 samples per channel. Streams have one channel or
two */
static short *decode_adpcm(const unsigned char *source, unsigned long size, unsigned long channels,
	unsigned long *frame_count)
{
	unsigned long block_bytes = XBOX_ADPCM_BLOCK_BYTES * channels;
	unsigned long blocks = size / block_bytes;
	short *samples = malloc((blocks ? blocks : 1) * XBOX_ADPCM_BLOCK_SAMPLES * channels * sizeof(short));
	unsigned long block;

	if (!samples)
	{
		*frame_count = 0;
		return NULL;
	}
	for (block = 0; block < blocks; block++)
	{
		const unsigned char *data = source + block * block_bytes;
		short *output = samples + block * XBOX_ADPCM_BLOCK_SAMPLES * channels;

		if (channels == 1)
		{
			decode_adpcm_channel(data, data + 4, 4, output, 1);
		}
		else
		{
			/* both headers, then the channels' groups in turn */
			decode_adpcm_channel(data, data + 8, 8, output, 2);
			decode_adpcm_channel(data + 4, data + 12, 8, output + 1, 2);
		}
	}
	*frame_count = blocks * XBOX_ADPCM_BLOCK_SAMPLES;
	return samples;
}

static short *decode_pcm(const unsigned char *source, unsigned long size, unsigned long channels,
	unsigned long *frame_count)
{
	unsigned long frames = size / (2 * channels);
	short *samples = malloc((frames ? frames : 1) * channels * sizeof(short));

	if (samples)
		memcpy(samples, source, frames * channels * sizeof(short));
	*frame_count = samples ? frames : 0;
	return samples;
}

/* ---------- 3D */

static float dot3(const float *a, const float *b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void spatialize(const struct sdl_stream *stream, float *left, float *right)
{
	float offset[3], right_axis[3], distance, attenuation, pan, side, ahead;
	int axis;

	if (stream->mode == DS3DMODE_HEADRELATIVE)
	{
		for (axis = 0; axis < 3; axis++)
			offset[axis] = stream->position[axis];
		side = offset[0];
		ahead = offset[2];
	}
	else
	{
		for (axis = 0; axis < 3; axis++)
			offset[axis] = stream->position[axis] - listener.position[axis];
		/* left-handed: right = top x front */
		right_axis[0] = listener.top[1] * listener.front[2] - listener.top[2] * listener.front[1];
		right_axis[1] = listener.top[2] * listener.front[0] - listener.top[0] * listener.front[2];
		right_axis[2] = listener.top[0] * listener.front[1] - listener.top[1] * listener.front[0];
		side = dot3(offset, right_axis);
		ahead = dot3(offset, listener.front);
	}
	distance = sqrtf(dot3(offset, offset)) * listener.distance_factor;

	/* DirectSound's inverse distance law, held beyond the maximum distance */
	attenuation = 1.0f;
	if (distance > stream->minimum_distance && stream->minimum_distance > 0.0f)
	{
		float clamped = distance < stream->maximum_distance ? distance : stream->maximum_distance;

		attenuation = stream->minimum_distance /
			(stream->minimum_distance + listener.rolloff_factor * (clamped - stream->minimum_distance));
	}

	/* equal power pan; close sources and sources straight ahead or behind
	stay centred, and neither ear drops below a quarter */
	{
		float horizontal = sqrtf(side * side + ahead * ahead);
		float angle;

		pan = horizontal > 1.0e-4f ? side / horizontal : 0.0f;
		if (distance < stream->minimum_distance && stream->minimum_distance > 0.0f)
			pan *= distance / stream->minimum_distance;
		pan *= 0.75f;
		angle = (pan + 1.0f) * 0.25f * 3.14159265f;
		*left = cosf(angle) * 1.41421356f * 0.70710678f;
		*right = sinf(angle) * 1.41421356f * 0.70710678f;
	}
	*left *= attenuation * stream->i3dl2_gain;
	*right *= attenuation * stream->i3dl2_gain;
}

static void voice_gains(const struct sdl_stream *stream, float *left, float *right)
{
	if (stream->has_3d && stream->mode != DS3DMODE_DISABLE)
	{
		spatialize(stream, left, right);
	}
	else
	{
		*left = stream->mix_left;
		*right = stream->mix_right;
	}
	*left *= stream->volume * master_volume;
	*right *= stream->volume * master_volume;
}

/* voice_gains, worked out again only once one of its inputs changed: the
mixer asks for every voice's in every chunk (on the console 187 times a
second, the 3D ones a square root, divisions, a cosine and a sine each),
and the same inputs give the same gains */
static void voice_target_gains(struct sdl_stream *stream, float *left, float *right)
{
	if (stream->gains_dirty || stream->gains_generation != listener_generation)
	{
		voice_gains(stream, &stream->target_left, &stream->target_right);
		stream->gains_dirty = FALSE;
		stream->gains_generation = listener_generation;
	}
	*left = stream->target_left;
	*right = stream->target_right;
}

/* ---------- mixing */


/* The console's mixer: the same one in integer arithmetic.

The console's floating point is the x87's, which the emulator runs in
software, a helper call for every operation; the float mixer spent some
thirty of them on every frame of every voice (and a double-to-integer
conversion with two control-word loads), enough to take half of the
emulated processor in a firefight, ahead of the game, since the output
thread runs above it. Here the position is whole frames plus a 16-bit
fraction, the gains are fixed point (1.0 is 1 << 20 while they ramp,
1 << 12 when applied), the voices sum in 32-bit integers at the 16-bit
output's scale, and the soft limiter's curve is a table. The gains
themselves (distance, pan, volume) are still worked out in floating point,
once per voice per chunk, and only when they changed. Interpolation, ramps
and the limiter are the float mixer's, to within the fixed point's
rounding. The loop over a packet's frames is dsound_mix_kernels.c's, which
also has GAIN_RAMP_SHIFT and GAIN_SHIFT. */

/* the limiter: above 0.8 of full scale the excess is eased to
0.2 tanh(excess / 0.2), looked up in steps of four ranges / table size */
#define LIMIT_KNEE 26214
#define LIMIT_RANGE 6553
#define LIMIT_SPAN (4 * LIMIT_RANGE)
#define LIMIT_TABLE_SIZE 1024
static short limit_table[LIMIT_TABLE_SIZE];

static void limit_table_build(void)
{
	int index;

	for (index = 0; index < LIMIT_TABLE_SIZE; index++)
	{
		float excess = (float)index * (float)LIMIT_SPAN / (float)LIMIT_TABLE_SIZE;

		limit_table[index] = (short)((float)LIMIT_RANGE * tanhf(excess / (float)LIMIT_RANGE) + 0.5f);
	}
}

static long gain_to_fixed(float gain)
{
	if (gain <= 0.0f)
		return 0;
	if (gain >= 8.0f)
		return 8L << GAIN_RAMP_SHIFT;
	return (long)(gain * (float)(1L << GAIN_RAMP_SHIFT));
}

static struct voice_packet *first_unfinished_packet(struct sdl_stream *stream)
{
	unsigned long position;

	for (position = 0; position < stream->packet_count; position++)
	{
		struct voice_packet *candidate = &stream->packets[(stream->packet_head + position) % MAXIMUM_STREAM_PACKETS];

		if (!candidate->finished)
			return candidate;
	}
	return NULL;
}

/* mixes one voice into output (frames of stereo, 16-bit scale) */
static void mix_voice_fixed(struct sdl_stream *stream, long *output, unsigned long frames)
{
	unsigned long rate, step_frames, step_fraction, step, frame = 0;
	long left, right, target_fixed_left, target_fixed_right, ramp_left, ramp_right;
	float target_left, target_right;
	BOOL kernels, held, silent;
	struct voice_packet *packet;

	if (stream->paused || !stream->packet_count || !stream->sample_rate || !frames)
		return;
	rate = stream->frequency ? stream->frequency : stream->sample_rate;
	step_frames = rate / OUTPUT_RATE;
	step_fraction = ((rate % OUTPUT_RATE) << 16) / OUTPUT_RATE;
	voice_target_gains(stream, &target_left, &target_right);
	if (!stream->gains_valid)
	{
		stream->current_left = target_left;
		stream->current_right = target_right;
		stream->gains_valid = TRUE;
	}
	left = gain_to_fixed(stream->current_left);
	right = gain_to_fixed(stream->current_right);
	target_fixed_left = gain_to_fixed(target_left);
	target_fixed_right = gain_to_fixed(target_right);
	ramp_left = (target_fixed_left - left) / (long)frames;
	ramp_right = (target_fixed_right - right) / (long)frames;

	/* the kernels keep the position as one 16.16 number, which holds it
	exactly for packets of up to 32768 frames and steps of less than 32768
	frames (the game's packets have at most 8192). Gains from 0 to below
	the applied scale's 1 at both ends of the chunk (the ramp stays between
	them) add 0 to every frame: such a voice only moves through its
	packets. (Compared unsigned: a gain that is not a number converts to a
	negative one, which does add.) */
	step = (step_frames << 16) | step_fraction;
	kernels = step_frames < 0x8000 && step != 0;
	held = !ramp_left && !ramp_right;
	silent = (unsigned long)left < (1UL << (GAIN_RAMP_SHIFT - GAIN_SHIFT)) &&
		(unsigned long)right < (1UL << (GAIN_RAMP_SHIFT - GAIN_SHIFT)) &&
		(unsigned long)target_fixed_left < (1UL << (GAIN_RAMP_SHIFT - GAIN_SHIFT)) &&
		(unsigned long)target_fixed_right < (1UL << (GAIN_RAMP_SHIFT - GAIN_SHIFT));

	packet = first_unfinished_packet(stream);
	while (packet && frame < frames)
	{
		const short *samples = packet->samples;
		unsigned long count = packet->frames;
		unsigned long channels = stream->channels;
		unsigned long last = channels - 1;
		unsigned long position = stream->cursor_frame;
		unsigned long fraction = stream->cursor_fraction;

		if (position >= count)
		{
			/* played out: on into the next packet */
			stream->cursor_frame = position - count;
			packet->finished = TRUE;
			packet = first_unfinished_packet(stream);
			continue;
		}
		if (kernels && count <= 0x8000)
		{
			/* the frames whose next frame is in the packet (a silent
			voice: all of them), as far as the chunk goes */
			unsigned long cursor = (position << 16) | fraction;
			unsigned long limit = (silent ? count : count - 1) << 16;

			if (cursor < limit)
			{
				unsigned long run = (limit - cursor - 1) / step + 1;

				if (run > frames - frame)
					run = frames - frame;
				if (!silent)
				{
					long *destination = output + frame * 2;

					if (channels == 1 && held)
						dsound_mix_mono(samples, destination, run, cursor, step, left, right);
					else if (channels == 1)
						dsound_mix_mono_ramp(samples, destination, run, cursor, step, left, right, ramp_left, ramp_right);
					else if (held)
						dsound_mix_stereo(samples, destination, run, cursor, step, left, right);
					else
						dsound_mix_stereo_ramp(samples, destination, run, cursor, step, left, right, ramp_left,
							ramp_right);
				}
				frame += run;
				left += (long)run * ramp_left;
				right += (long)run * ramp_right;
				cursor += run * step;
				position = cursor >> 16;
				fraction = cursor & 0xFFFF;
			}
		}
		/* the packet's last frame, which interpolates towards itself (or,
		past the kernels' limits, every frame) */
		for (; frame < frames && position < count; frame++)
		{
			const short *a = samples + position * channels;
			long a0 = a[0], a1 = a[last], b0 = a0, b1 = a1;
			long blend = (long)(fraction >> 1);
			long sample_left, sample_right;

			if (position + 1 < count)
			{
				b0 = a[channels];
				b1 = a[channels + last];
			}
			sample_left = a0 + (((b0 - a0) * blend) >> 15);
			sample_right = a1 + (((b1 - a1) * blend) >> 15);
			output[frame * 2] += (sample_left * (left >> (GAIN_RAMP_SHIFT - GAIN_SHIFT))) >> GAIN_SHIFT;
			output[frame * 2 + 1] += (sample_right * (right >> (GAIN_RAMP_SHIFT - GAIN_SHIFT))) >> GAIN_SHIFT;
			left += ramp_left;
			right += ramp_right;
			fraction += step_fraction;
			position += step_frames + (fraction >> 16);
			fraction &= 0xFFFF;
		}
		stream->cursor_frame = position;
		stream->cursor_fraction = fraction;
	}
	stream->current_left = target_left;
	stream->current_right = target_right;
}

static void mix_fixed(long *output, unsigned long frames)
{
	struct sdl_stream *stream;
	unsigned long voices = 0;

	memset(output, 0, frames * OUTPUT_CHANNELS * sizeof(long));
	pthread_mutex_lock(&mixer_lock);
	for (stream = streams; stream; stream = stream->next)
	{
		if (!stream->paused && stream->packet_count)
		{
			frames_with_voices += frames;
			voices++;
		}
		mix_voice_fixed(stream, output, frames);
	}
	/* (the console's diagnostics) */
	(void)voices;
	pthread_mutex_unlock(&mixer_lock);
}


/* ---------- output */

static BOOL audio_started = FALSE;


/* the loudest sample since the last look, for the diagnostics */
static unsigned long output_peak;


/* the console's output thread asks for 16-bit frames (nx_audio.c, nx_apu.c;
on Windows 98/ME w9x_audio.c's) */
static void console_fill(short *samples, unsigned long frames)
{
	long buffer[MIX_CHUNK_FRAMES * OUTPUT_CHANNELS];
	unsigned long sample;

	while (frames > 0)
	{
		unsigned long count = frames > MIX_CHUNK_FRAMES ? MIX_CHUNK_FRAMES : frames;

		mix_fixed(buffer, count);
		for (sample = 0; sample < count * OUTPUT_CHANNELS; sample++)
		{
			long value = buffer[sample];
			long magnitude = value < 0 ? -value : value;

			if (magnitude > LIMIT_KNEE)
			{
				/* soft limit rather than wrap or hard clip when many
				voices pile up */
				unsigned long excess = (unsigned long)(magnitude - LIMIT_KNEE);
				unsigned long index = excess >= LIMIT_SPAN ? LIMIT_TABLE_SIZE - 1 :
					excess * LIMIT_TABLE_SIZE / LIMIT_SPAN;

				magnitude = LIMIT_KNEE + limit_table[index];
				if (magnitude > 32767)
					magnitude = 32767;
				value = value < 0 ? -magnitude : magnitude;
			}
			samples[sample] = (short)value;
			if ((unsigned long)magnitude > output_peak)
				output_peak = (unsigned long)magnitude;
		}
		samples += count * OUTPUT_CHANNELS;
		frames -= count;
	}
}

/* whether a device plays the mix; without one, when the voices were last
drained */
static BOOL audio_device;
static unsigned long long silent_clock;
/* when the mixer was last asked, and the longest gap between asks lately
(microseconds), for the lead */
static unsigned long long last_pump;
static float longest_gap;

/* the frames to have in the device ahead of what it plays: a little more
than the longest gap between asks of late (each ask's gap counted up to a
quarter second: a map loading is not a frame), at least 40 ms */
static unsigned long lead_frames(void)
{
	unsigned long long now = system_microseconds();
	float gap = last_pump ? (float)(now - last_pump) : 0.0f, lead;

	last_pump = now;
	if (gap > 250000.0f)
		gap = 250000.0f;
	longest_gap *= 0.98f;
	if (gap > longest_gap)
		longest_gap = gap;
	lead = (longest_gap * 1.25f + 15000.0f) * (float)output_rate / 1000000.0f;
	if (lead < 0.04f * (float)output_rate)
		lead = 0.04f * (float)output_rate;
	return (unsigned long)lead;
}

/* the game's turn to mix: the device filled as far ahead as the lead, or
the voices drained into nothing for the time gone by */
void dsound_pump(void)
{
	unsigned long dos_settings_generation(void);
	static unsigned long settings_generation;

	if (!audio_started)
		return;
	/* (the SETTINGS page's VOLUME as it changes) */
	if (settings_generation != dos_settings_generation())
	{
		settings_generation = dos_settings_generation();
		master_volume = dos_setting_real("sound_volume", 1.0f);
		if (master_volume < 0.0f)
			master_volume = 0.0f;
		listener_generation++;
	}
	if (audio_device)
	{
		system_audio_update(console_fill, lead_frames());
		return;
	}
	{
		unsigned long long now = system_microseconds();
		unsigned long frames = (unsigned long)((now - silent_clock) * output_rate / 1000000ULL);
		long buffer[MIX_CHUNK_FRAMES * OUTPUT_CHANNELS];

		if (!frames)
			return;
		silent_clock += (unsigned long long)frames * 1000000ULL / output_rate;
		/* (no more than a second's worth after a long wait) */
		if (frames > output_rate)
			frames = output_rate;
		while (frames)
		{
			unsigned long count = frames > MIX_CHUNK_FRAMES ? MIX_CHUNK_FRAMES : frames;

			mix_fixed(buffer, count);
			frames -= count;
		}
	}
}

static void audio_start(void)
{
	long rate = dos_setting_integer("sound_rate", 22050);

	if (audio_started)
		return;
	audio_started = TRUE;
	master_volume = dos_setting_real("sound_volume", 1.0f);
	if (master_volume < 0.0f)
		master_volume = 0.0f;
	listener_generation++;
	adpcm_tables_build();
	limit_table_build();
	if (rate < 8000 || rate > 48000)
		rate = 22050;
	output_rate = (unsigned long)rate;
	audio_device = system_audio_open(output_rate, &output_rate);
	if (!audio_device)
	{
		platform_log("sound: no device, the sounds play silently");
		silent_clock = system_microseconds();
	}
	else
	{
		platform_log("sound: %lu Hz", output_rate);
	}
}

/* ---------- completion */

static void packet_release(struct voice_packet *entry)
{
	free(entry->samples);
	entry->samples = NULL;
}

/* completes the head packet; called with the lock held, which the game's
callback runs without */
static void stream_complete_head(struct sdl_stream *stream, DWORD status, DWORD completed_size)
{
	struct voice_packet *entry = &stream->packets[stream->packet_head];
	XMEDIAPACKET packet = entry->packet;

	packet_release(entry);
	entry->finished = FALSE;
	stream->packet_head = (stream->packet_head + 1) % MAXIMUM_STREAM_PACKETS;
	stream->packet_count--;
	if (packet.pdwCompletedSize)
		*packet.pdwCompletedSize = completed_size;
	if (packet.pdwStatus)
		*packet.pdwStatus = status;
	if (stream->callback)
	{
		pthread_mutex_unlock(&mixer_lock);
		stream->callback(stream->context, packet.pContext, status);
		pthread_mutex_lock(&mixer_lock);
	}
	else if (packet.hCompletionEvent)
	{
		SetEvent(packet.hCompletionEvent);
	}
}

static void streams_complete_finished(void)
{
	struct sdl_stream *stream;

	pthread_mutex_lock(&mixer_lock);
	for (stream = streams; stream; stream = stream->next)
	{
		while (stream->packet_count && stream->packets[stream->packet_head].finished)
			stream_complete_head(stream, XMEDIAPACKET_STATUS_SUCCESS, stream->packets[stream->packet_head].packet.dwMaxSize);
	}
	pthread_mutex_unlock(&mixer_lock);
}

/* ---------- stream interface */

static struct sdl_stream *stream_from_interface(void *stream)
{
	return (struct sdl_stream *)stream;
}

static ULONG STDMETHODCALLTYPE stream_add_reference(IDirectSoundStream *object)
{
	struct sdl_stream *stream = stream_from_interface(object);
	ULONG count;

	pthread_mutex_lock(&mixer_lock);
	count = ++stream->reference_count;
	pthread_mutex_unlock(&mixer_lock);
	return count;
}

static HRESULT STDMETHODCALLTYPE stream_flush(IDirectSoundStream *object);

static ULONG STDMETHODCALLTYPE stream_release(IDirectSoundStream *object)
{
	struct sdl_stream *stream = stream_from_interface(object);
	struct sdl_stream **link;
	ULONG count;

	pthread_mutex_lock(&mixer_lock);
	count = --stream->reference_count;
	pthread_mutex_unlock(&mixer_lock);
	if (count)
		return count;

	stream_flush(object);
	pthread_mutex_lock(&mixer_lock);
	for (link = &streams; *link; link = &(*link)->next)
	{
		if (*link == stream)
		{
			*link = stream->next;
			break;
		}
	}
	pthread_mutex_unlock(&mixer_lock);
	free(stream);
	return 0;
}

static HRESULT STDMETHODCALLTYPE stream_get_info(IDirectSoundStream *object, LPXMEDIAINFO information)
{
	struct sdl_stream *stream = stream_from_interface(object);

	memset(information, 0, sizeof(*information));
	information->dwFlags = XMO_STREAMF_FIXED_SAMPLE_SIZE | XMO_STREAMF_INPUT_ASYNC;
	information->dwInputSize = stream->adpcm ? XBOX_ADPCM_BLOCK_BYTES * stream->channels : 2 * stream->channels;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE stream_get_status(IDirectSoundStream *object, LPDWORD status)
{
	struct sdl_stream *stream = stream_from_interface(object);

	pthread_mutex_lock(&mixer_lock);
	*status = stream->packet_count < MAXIMUM_STREAM_PACKETS ? XMO_STATUSF_ACCEPT_INPUT_DATA : 0;
	pthread_mutex_unlock(&mixer_lock);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE stream_process(IDirectSoundStream *object, LPCXMEDIAPACKET input, LPCXMEDIAPACKET output)
{
	struct sdl_stream *stream = stream_from_interface(object);
	struct voice_packet *entry;
	unsigned long frames = 0;
	short *samples;

	(void)output;
	if (!input)
		return E_INVALIDARG;
	/* decode outside the lock */
	samples = stream->adpcm ?
		decode_adpcm(input->pvBuffer, input->dwMaxSize, stream->channels, &frames) :
		decode_pcm(input->pvBuffer, input->dwMaxSize, stream->channels, &frames);
	pthread_mutex_lock(&mixer_lock);
	if (stream->packet_count == MAXIMUM_STREAM_PACKETS)
	{
		pthread_mutex_unlock(&mixer_lock);
		free(samples);
		return E_OUTOFMEMORY;
	}
	entry = &stream->packets[(stream->packet_head + stream->packet_count) % MAXIMUM_STREAM_PACKETS];
	packets_taken++;
	entry->packet = *input;
	entry->samples = samples;
	entry->frames = samples ? frames : 0;
	entry->finished = FALSE;
	if (input->pdwStatus)
		*input->pdwStatus = XMEDIAPACKET_STATUS_PENDING;
	if (input->pdwCompletedSize)
		*input->pdwCompletedSize = 0;
	if (!stream->packet_count)
	{
		/* a stream that ran dry starts over */
		stream->cursor = 0.0;
		stream->cursor_frame = 0;
		stream->cursor_fraction = 0;
		stream->gains_valid = FALSE;
	}
	stream->packet_count++;
	pthread_mutex_unlock(&mixer_lock);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE stream_discontinuity(IDirectSoundStream *object)
{
	(void)object;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE stream_flush(IDirectSoundStream *object)
{
	struct sdl_stream *stream = stream_from_interface(object);

	pthread_mutex_lock(&mixer_lock);
	while (stream->packet_count)
	{
		struct voice_packet *head = &stream->packets[stream->packet_head];

		stream_complete_head(stream, head->finished ? XMEDIAPACKET_STATUS_SUCCESS : XMEDIAPACKET_STATUS_FLUSHED,
			head->finished ? head->packet.dwMaxSize : 0);
	}
	stream->cursor = 0.0;
	stream->cursor_frame = 0;
	stream->cursor_fraction = 0;
	pthread_mutex_unlock(&mixer_lock);
	return S_OK;
}

static IDirectSoundStreamVtbl stream_vtable =
{
	stream_add_reference,
	stream_release,
	stream_get_info,
	stream_get_status,
	stream_process,
	stream_discontinuity,
	stream_flush,
};

/* ---------- the DirectSound object */

struct sdl_direct_sound
{
	ULONG reference_count;
};

static struct sdl_direct_sound direct_sound = { 0 };

HRESULT WINAPI DirectSoundCreate(LPGUID device_id, LPDIRECTSOUND *result, LPUNKNOWN outer)
{
	(void)device_id;
	(void)outer;
	/* (sound off: the game runs without its sound engine) */
	if (!dos_setting_integer("sound", 1))
	{
		platform_log("sound: off (the setting sound)");
		*result = NULL;
		return E_FAIL;
	}
	audio_start();
	direct_sound.reference_count++;
	*result = (LPDIRECTSOUND)&direct_sound;
	return DS_OK;
}

ULONG WINAPI IDirectSound_Release(LPDIRECTSOUND sound)
{
	(void)sound;
	return direct_sound.reference_count ? --direct_sound.reference_count : 0;
}

VOID WINAPI DirectSoundDoWork(void)
{
	dsound_pump();
	streams_complete_finished();
}

VOID WINAPI DirectSoundUseFullHRTF(void)
{
}

HRESULT WINAPI IDirectSound_GetCaps(LPDIRECTSOUND sound, LPDSCAPS caps)
{
	(void)sound;
	memset(caps, 0, sizeof(*caps));
	caps->dwFree2DBuffers = 64;
	caps->dwFree3DBuffers = 64;
	caps->dwFreeBufferSGEs = 2047;
	caps->dwMemoryAllocated = 0;
	return DS_OK;
}

HRESULT WINAPI IDirectSound_GetSpeakerConfig(LPDIRECTSOUND sound, LPDWORD speaker_config)
{
	(void)sound;
	*speaker_config = DSSPEAKER_STEREO;
	return DS_OK;
}

HRESULT WINAPI IDirectSound_DownloadEffectsImage(LPDIRECTSOUND sound, LPCVOID image, DWORD image_size,
	LPCDSEFFECTIMAGELOC image_location, LPDSEFFECTIMAGEDESC *image_description)
{
	(void)sound;
	(void)image;
	(void)image_size;
	(void)image_location;
	if (image_description)
		*image_description = NULL;
	return DS_OK;
}

HRESULT WINAPI IDirectSound_CommitDeferredSettings(LPDIRECTSOUND sound) { (void)sound; return DS_OK; }
HRESULT WINAPI IDirectSound_SetMixBinHeadroom(LPDIRECTSOUND sound, DWORD mix_bin_mask, DWORD headroom) { (void)sound; (void)mix_bin_mask; (void)headroom; return DS_OK; }
HRESULT WINAPI IDirectSound_SetI3DL2Listener(LPDIRECTSOUND sound, LPCDSI3DL2LISTENER listener_properties, DWORD apply) { (void)sound; (void)listener_properties; (void)apply; return DS_OK; }

HRESULT WINAPI IDirectSound_SetDistanceFactor(LPDIRECTSOUND sound, FLOAT factor, DWORD apply)
{
	(void)sound;
	(void)apply;
	pthread_mutex_lock(&mixer_lock);
	listener.distance_factor = factor > 0.0f ? factor : 1.0f;
	listener_generation++;
	pthread_mutex_unlock(&mixer_lock);
	return DS_OK;
}

HRESULT WINAPI IDirectSound_SetRolloffFactor(LPDIRECTSOUND sound, FLOAT factor, DWORD apply)
{
	(void)sound;
	(void)apply;
	pthread_mutex_lock(&mixer_lock);
	listener.rolloff_factor = factor >= 0.0f ? factor : 1.0f;
	listener_generation++;
	pthread_mutex_unlock(&mixer_lock);
	return DS_OK;
}

HRESULT WINAPI IDirectSound_SetPosition(LPDIRECTSOUND sound, FLOAT x, FLOAT y, FLOAT z, DWORD apply)
{
	(void)sound;
	(void)apply;
	pthread_mutex_lock(&mixer_lock);
	listener.position[0] = x;
	listener.position[1] = y;
	listener.position[2] = z;
	listener_generation++;
	pthread_mutex_unlock(&mixer_lock);
	return DS_OK;
}

HRESULT WINAPI IDirectSound_SetVelocity(LPDIRECTSOUND sound, FLOAT x, FLOAT y, FLOAT z, DWORD apply) { (void)sound; (void)x; (void)y; (void)z; (void)apply; return DS_OK; }

static void normalize3(float *vector)
{
	float length = sqrtf(dot3(vector, vector));

	if (length > 1.0e-6f)
	{
		vector[0] /= length;
		vector[1] /= length;
		vector[2] /= length;
	}
}

HRESULT WINAPI IDirectSound_SetOrientation(LPDIRECTSOUND sound, FLOAT x_front, FLOAT y_front, FLOAT z_front,
	FLOAT x_top, FLOAT y_top, FLOAT z_top, DWORD apply)
{
	(void)sound;
	(void)apply;
	pthread_mutex_lock(&mixer_lock);
	listener.front[0] = x_front;
	listener.front[1] = y_front;
	listener.front[2] = z_front;
	listener.top[0] = x_top;
	listener.top[1] = y_top;
	listener.top[2] = z_top;
	normalize3(listener.front);
	normalize3(listener.top);
	listener_generation++;
	pthread_mutex_unlock(&mixer_lock);
	return DS_OK;
}

HRESULT WINAPI IDirectSound_CreateSoundStream(LPDIRECTSOUND sound, LPCDSSTREAMDESC description,
	LPDIRECTSOUNDSTREAM *result, LPUNKNOWN outer)
{
	struct sdl_stream *stream = calloc(1, sizeof(*stream));
	const WAVEFORMATEX *format = description->lpwfxFormat;

	(void)sound;
	(void)outer;
	if (!stream)
		return E_OUTOFMEMORY;
	stream->object.lpVtbl = &stream_vtable;
	stream->reference_count = 1;
	stream->callback = description->lpfnCallback;
	stream->context = description->lpvContext;
	stream->adpcm = format && format->wFormatTag == WAVE_FORMAT_XBOX_ADPCM;
	stream->channels = format && format->nChannels == 2 ? 2 : 1;
	stream->sample_rate = format ? format->nSamplesPerSec : 0;
	stream->frequency = stream->sample_rate;
	stream->volume = 1.0f;
	/* DirectSound's default mix bins: a mono voice to both fronts, a
	stereo voice's channels to the front left and right */
	stream->mix_left = 1.0f;
	stream->mix_right = 1.0f;
	stream->has_3d = (description->dwFlags & DSSTREAMCAPS_CTRL3D) != 0;
	stream->mode = DS3DMODE_NORMAL;
	stream->minimum_distance = DS3D_DEFAULTMINDISTANCE;
	stream->maximum_distance = DS3D_DEFAULTMAXDISTANCE;
	stream->i3dl2_gain = 1.0f;
	stream->gains_dirty = TRUE;
	pthread_mutex_lock(&mixer_lock);
	stream->next = streams;
	streams_created++;
	streams = stream;
	pthread_mutex_unlock(&mixer_lock);
	*result = &stream->object;
	return DS_OK;
}

/* January-era DirectSound exports the game declares itself
(sound_dsound_xbox.c); the XDK 3911 headers no longer carry them */

void __stdcall DirectSoundStopStream(LPDIRECTSOUNDSTREAM stream)
{
	stream_flush(stream);
}

unsigned long __stdcall DirectSoundGetStreamVoiceStatus(LPDIRECTSOUNDSTREAM stream)
{
	struct sdl_stream *record = stream_from_interface(stream);
	unsigned long active;

	pthread_mutex_lock(&mixer_lock);
	active = record->packet_count != 0;
	pthread_mutex_unlock(&mixer_lock);
	return active;
}

/* (every one marks the stream's gains to be worked out again: most feed
them) */
#define STREAM_SETTER(body) \
	struct sdl_stream *record = stream_from_interface(stream); \
	pthread_mutex_lock(&mixer_lock); \
	body; \
	record->gains_dirty = TRUE; \
	pthread_mutex_unlock(&mixer_lock); \
	return DS_OK;

HRESULT WINAPI IDirectSoundStream_SetFrequency(LPDIRECTSOUNDSTREAM stream, DWORD frequency)
{
	STREAM_SETTER(record->frequency = frequency ? frequency : record->sample_rate)
}

HRESULT WINAPI IDirectSoundStream_SetVolume(LPDIRECTSOUNDSTREAM stream, LONG volume)
{
	STREAM_SETTER(record->volume = gain_from_millibels(volume))
}

HRESULT WINAPI IDirectSoundStream_SetMixBins(LPDIRECTSOUNDSTREAM stream, DWORD mix_bin_mask)
{
	STREAM_SETTER(
		record->mix_left = (mix_bin_mask & DSMIXBIN_FRONT_LEFT) ? 1.0f : 0.0f;
		record->mix_right = (mix_bin_mask & DSMIXBIN_FRONT_RIGHT) ? 1.0f : 0.0f)
}

/* volumes come in the order of the set bits of the mask */
HRESULT WINAPI IDirectSoundStream_SetMixBinVolumes(LPDIRECTSOUNDSTREAM stream, DWORD mix_bin_mask, const LONG *volumes)
{
	struct sdl_stream *record = stream_from_interface(stream);
	unsigned long bit, index = 0;

	pthread_mutex_lock(&mixer_lock);
	for (bit = 0; bit < 32; bit++)
	{
		if (!(mix_bin_mask & (1UL << bit)))
			continue;
		if ((1UL << bit) == DSMIXBIN_FRONT_LEFT)
			record->mix_left = gain_from_millibels(volumes[index]);
		else if ((1UL << bit) == DSMIXBIN_FRONT_RIGHT)
			record->mix_right = gain_from_millibels(volumes[index]);
		index++;
	}
	record->gains_dirty = TRUE;
	pthread_mutex_unlock(&mixer_lock);
	return DS_OK;
}

HRESULT WINAPI IDirectSoundStream_SetMode(LPDIRECTSOUNDSTREAM stream, DWORD mode, DWORD apply)
{
	(void)apply;
	STREAM_SETTER(record->mode = mode)
}

HRESULT WINAPI IDirectSoundStream_SetPosition(LPDIRECTSOUNDSTREAM stream, FLOAT x, FLOAT y, FLOAT z, DWORD apply)
{
	(void)apply;
	STREAM_SETTER(record->position[0] = x; record->position[1] = y; record->position[2] = z)
}

HRESULT WINAPI IDirectSoundStream_SetMinDistance(LPDIRECTSOUNDSTREAM stream, FLOAT distance, DWORD apply)
{
	(void)apply;
	STREAM_SETTER(record->minimum_distance = distance)
}

HRESULT WINAPI IDirectSoundStream_SetMaxDistance(LPDIRECTSOUNDSTREAM stream, FLOAT distance, DWORD apply)
{
	(void)apply;
	STREAM_SETTER(record->maximum_distance = distance)
}

HRESULT WINAPI IDirectSoundStream_SetI3DL2Source(LPDIRECTSOUNDSTREAM stream, LPCDSI3DL2BUFFER source, DWORD apply)
{
	LONG direct;

	(void)apply;
	/* the low frequency part of the direct path */
	direct = source->lDirect +
		(LONG)(source->Obstruction.lHFLevel * source->Obstruction.flLFRatio) +
		(LONG)(source->Occlusion.lHFLevel * source->Occlusion.flLFRatio);
	if (direct > 0)
		direct = 0;
	{
		STREAM_SETTER(record->i3dl2_gain = gain_from_millibels(direct))
	}
}

HRESULT WINAPI IDirectSoundStream_Pause(LPDIRECTSOUNDSTREAM stream, DWORD pause)
{
	STREAM_SETTER(record->paused = pause == DSSTREAMPAUSE_PAUSE)
}

HRESULT WINAPI IDirectSoundStream_SetVelocity(LPDIRECTSOUNDSTREAM stream, FLOAT x, FLOAT y, FLOAT z, DWORD apply) { (void)stream; (void)x; (void)y; (void)z; (void)apply; return DS_OK; }
HRESULT WINAPI IDirectSoundStream_SetConeAngles(LPDIRECTSOUNDSTREAM stream, DWORD inside, DWORD outside, DWORD apply) { (void)stream; (void)inside; (void)outside; (void)apply; return DS_OK; }
HRESULT WINAPI IDirectSoundStream_SetConeOrientation(LPDIRECTSOUNDSTREAM stream, FLOAT x, FLOAT y, FLOAT z, DWORD apply) { (void)stream; (void)x; (void)y; (void)z; (void)apply; return DS_OK; }
HRESULT WINAPI IDirectSoundStream_SetConeOutsideVolume(LPDIRECTSOUNDSTREAM stream, LONG volume, DWORD apply) { (void)stream; (void)volume; (void)apply; return DS_OK; }

/* ---------- buffers

The game's only buffer is a silent looping one that keeps the voice
processor busy; it needs no mixing. */

struct null_buffer
{
	ULONG reference_count;
	LPVOID data;
	DWORD size;
	BOOL playing;
};

HRESULT WINAPI DirectSoundCreateBuffer(LPCDSBUFFERDESC description, LPDIRECTSOUNDBUFFER *result)
{
	struct null_buffer *buffer = calloc(1, sizeof(*buffer));

	(void)description;
	if (!buffer)
		return E_OUTOFMEMORY;
	buffer->reference_count = 1;
	*result = (LPDIRECTSOUNDBUFFER)buffer;
	return DS_OK;
}

HRESULT WINAPI IDirectSound_CreateSoundBuffer(LPDIRECTSOUND sound, LPCDSBUFFERDESC description,
	LPDIRECTSOUNDBUFFER *result, LPUNKNOWN outer)
{
	(void)sound;
	(void)outer;
	return DirectSoundCreateBuffer(description, result);
}

ULONG WINAPI IDirectSoundBuffer_Release(LPDIRECTSOUNDBUFFER buffer)
{
	struct null_buffer *record = (struct null_buffer *)buffer;
	ULONG count = --record->reference_count;

	if (!count)
		free(record);
	return count;
}

HRESULT WINAPI IDirectSoundBuffer_SetBufferData(LPDIRECTSOUNDBUFFER buffer, LPVOID data, DWORD size)
{
	struct null_buffer *record = (struct null_buffer *)buffer;

	record->data = data;
	record->size = size;
	return DS_OK;
}

HRESULT WINAPI IDirectSoundBuffer_Play(LPDIRECTSOUNDBUFFER buffer, DWORD reserved1, DWORD reserved2, DWORD flags)
{
	(void)reserved1;
	(void)reserved2;
	(void)flags;
	((struct null_buffer *)buffer)->playing = TRUE;
	return DS_OK;
}

HRESULT WINAPI IDirectSoundBuffer_Stop(LPDIRECTSOUNDBUFFER buffer)
{
	((struct null_buffer *)buffer)->playing = FALSE;
	return DS_OK;
}

HRESULT WINAPI IDirectSoundBuffer_SetCurrentPosition(LPDIRECTSOUNDBUFFER buffer, DWORD play_cursor) { (void)buffer; (void)play_cursor; return DS_OK; }
HRESULT WINAPI IDirectSoundBuffer_SetLoopRegion(LPDIRECTSOUNDBUFFER buffer, DWORD loop_start, DWORD loop_length) { (void)buffer; (void)loop_start; (void)loop_length; return DS_OK; }
HRESULT WINAPI IDirectSoundBuffer_SetPitch(LPDIRECTSOUNDBUFFER buffer, LONG pitch) { (void)buffer; (void)pitch; return DS_OK; }
HRESULT WINAPI IDirectSoundBuffer_SetVolume(LPDIRECTSOUNDBUFFER buffer, LONG volume) { (void)buffer; (void)volume; return DS_OK; }
