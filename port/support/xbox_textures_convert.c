/*
XBOX_TEXTURES_CONVERT.C

Texel conversion for the texture upload: see xbox_textures_convert.h.

A row is converted in one of three ways. Texels the destination takes as
they are (A8R8G8B8 as BGRA, R5G6B5 as 5:6:5, a rotated A1R5G5B5 or
A4R4G4B4) are copied. Everything else is fetched as 32-bit ARGB words,
which is the BGRA destination itself; the other destinations pack those
words, a piece of the row at a time so that they stay in the cache.

16-bit destinations round to the nearest value, so a texel that was 16-bit
on the Xbox arrives unchanged.

Plain C: the Windows 9x build runs this on a Pentium III, which has no
integer vectors worth the name.
*/

#include "xbox_textures_convert.h"

#include <stdlib.h>
#include <string.h>

/* ---------- constants */

enum
{
	/* texels fetched before they are packed */
	ROW_PIECE_TEXELS = 256,
	/* widths the offsets of a row are kept on the stack for */
	SWIZZLE_TABLE_ENTRIES = 1024
};

/* ---------- formats */

static const unsigned char kind_bytes[NUMBER_OF_TEXTURE_KINDS] =
{
	4,
	4, 4, 2, 2, 2, 2,
	1, 1, 1, 2, 1,
	2, 2, 2,
	2, 4, 4, 4, 4, 2,
	2, 2, 2, 4, 2,
	8, 16, 16
};

unsigned long texture_convert_kind_bytes(int kind)
{
	return kind >= 0 && kind < NUMBER_OF_TEXTURE_KINDS ? kind_bytes[kind] : 4;
}

unsigned long texture_convert_destination_bytes(int destination)
{
	switch (destination)
	{
	case _texture_destination_bgra8:
	case _texture_destination_rgba8:
		return 4;
	case _texture_destination_y8:
		return 1;
	default:
		return 2;
	}
}

int texture_convert_supported(int kind, int destination)
{
	switch (destination)
	{
	case _texture_destination_y8:
		return kind == _texture_kind_l8 || kind == _texture_kind_al8 || kind == _texture_kind_a8;
	case _texture_destination_y8a8:
		return kind == _texture_kind_a8l8;
	default:
		return destination >= 0 && destination < NUMBER_OF_TEXTURE_DESTINATIONS;
	}
}

unsigned long texture_convert_kind_alpha(int kind)
{
	switch (kind)
	{
	case _texture_kind_a1r5g5b5:
	case _texture_kind_r5g5b5a1:
	case _texture_kind_dxt1:
		return _texture_alpha_one_bit;
	case _texture_kind_x8r8g8b8:
	case _texture_kind_r5g6b5:
	case _texture_kind_x1r5g5b5:
	case _texture_kind_l8:
	case _texture_kind_g8b8:
	case _texture_kind_r8b8:
	case _texture_kind_r6g5b5:
	case _texture_kind_l16:
	case _texture_kind_v16u16:
	case _texture_kind_yuy2:
	case _texture_kind_uyvy:
	case _texture_kind_d24s8:
	case _texture_kind_d16:
		return _texture_alpha_opaque;
	default:
		return _texture_alpha_full;
	}
}

int texture_convert_destination_16bit(unsigned long alpha)
{
	return alpha == _texture_alpha_opaque ? _texture_destination_r5g6b5 :
		alpha == _texture_alpha_one_bit ? _texture_destination_rgb5a1 : _texture_destination_rgba4;
}

/* ---------- swizzling */

struct texture_convert_swizzle_masks texture_convert_swizzle_masks(unsigned long width, unsigned long height,
	unsigned long depth)
{
	struct texture_convert_swizzle_masks masks = { 0, 0, 0 };
	unsigned long bit = 1, mask_bit = 1;
	int done;

	/* bits of x, y and z alternate until each dimension runs out */
	do
	{
		done = 1;
		if (bit < width)
		{
			masks.x |= mask_bit;
			mask_bit <<= 1;
			done = 0;
		}
		if (bit < height)
		{
			masks.y |= mask_bit;
			mask_bit <<= 1;
			done = 0;
		}
		if (bit < depth)
		{
			masks.z |= mask_bit;
			mask_bit <<= 1;
			done = 0;
		}
		bit <<= 1;
	} while (!done);
	return masks;
}

unsigned long texture_convert_swizzle_offset(unsigned long mask, unsigned long value)
{
	unsigned long result = 0, bit = 1;

	while (value && bit)
	{
		if (mask & bit)
		{
			if (value & 1)
				result |= bit;
			value >>= 1;
		}
		bit <<= 1;
	}
	return result;
}

/* the offset after this one: the subtraction carries through the bits
that are not the mask's */
#define SWIZZLE_NEXT(offset, mask) (((offset) - (mask)) & (mask))

void texture_convert_swizzle_table(unsigned long mask, unsigned long bytes, unsigned long count,
	unsigned long *offsets)
{
	unsigned long offset = 0, index;

	for (index = 0; index < count; index++)
	{
		offsets[index] = offset * bytes;
		offset = SWIZZLE_NEXT(offset, mask);
	}
}

/* ---------- texels as 32-bit ARGB */

#define ARGB(a, r, g, b) (((unsigned int)(a) << 24) | ((unsigned int)(r) << 16) | ((unsigned int)(g) << 8) | (unsigned int)(b))
#define OPAQUE 0xff000000u

static inline unsigned int load16(const unsigned char *s)
{
	return s[0] | ((unsigned int)s[1] << 8);
}

static inline unsigned int load32(const unsigned char *s)
{
	return s[0] | ((unsigned int)s[1] << 8) | ((unsigned int)s[2] << 16) | ((unsigned int)s[3] << 24);
}

/* The 16-bit formats widen all their fields at once: each field is moved
to the top of its byte, then its top bits are copied below it (5 bits
become 8 as v << 3 | v >> 2, 6 as v << 2 | v >> 4, 4 as v * 17). */

static inline unsigned int argb_r5g6b5(unsigned int v)
{
	unsigned int x = ((v & 0xf800) << 8) | ((v & 0x07e0) << 5) | ((v & 0x001f) << 3);

	return x | ((x >> 5) & 0x00070007) | ((x >> 6) & 0x00000300) | OPAQUE;
}

static inline unsigned int rgb_x1r5g5b5(unsigned int v)
{
	unsigned int x = ((v & 0x7c00) << 9) | ((v & 0x03e0) << 6) | ((v & 0x001f) << 3);

	return x | ((x >> 5) & 0x00070707);
}

static inline unsigned int argb_a1r5g5b5(unsigned int v)
{
	return rgb_x1r5g5b5(v) | ((0u - (v >> 15)) << 24);
}

static inline unsigned int argb_a4r4g4b4(unsigned int v)
{
	unsigned int x = ((v & 0xf000) << 12) | ((v & 0x0f00) << 8) | ((v & 0x00f0) << 4) | (v & 0x000f);

	return x | (x << 4);
}

static inline unsigned int argb_r6g5b5(unsigned int v)
{
	unsigned int x = ((v & 0xfc00) << 8) | ((v & 0x03e0) << 6) | ((v & 0x001f) << 3);

	return x | ((x >> 6) & 0x00030000) | ((x >> 5) & 0x00000707) | OPAQUE;
}

static inline unsigned int argb_r5g5b5a1(unsigned int v)
{
	unsigned int x = ((v & 0xf800) << 8) | ((v & 0x07c0) << 5) | ((v & 0x003e) << 2);

	return x | ((x >> 5) & 0x00070707) | ((0u - (v & 1)) << 24);
}

static inline unsigned int argb_r4g4b4a4(unsigned int v)
{
	unsigned int x = ((v & 0x000f) << 24) | ((v & 0xf000) << 4) | (v & 0x0f00) | ((v & 0x00f0) >> 4);

	return x | (x << 4);
}

static inline unsigned int clamp_byte(int value)
{
	return (unsigned int)(value < 0 ? 0 : value > 255 ? 255 : value);
}

static inline unsigned int argb_yuv(int y, int u, int v)
{
	int c = y - 16, d = u - 128, e = v - 128;

	return ARGB(255, clamp_byte((298 * c + 409 * e + 128) >> 8),
		clamp_byte((298 * c - 100 * d - 208 * e + 128) >> 8),
		clamp_byte((298 * c + 516 * d + 128) >> 8));
}

/* fetch_<kind>: count texels as ARGB words. s is the texel; offsets is
the table of a swizzled row or NULL. */
typedef void fetch_function(unsigned int *destination, const unsigned char *source, const unsigned long *offsets,
	unsigned long count, const unsigned int *palette);

#define DEFINE_FETCH(name, bytes, expression) \
static void fetch_##name(unsigned int *destination, const unsigned char *source, const unsigned long *offsets, \
	unsigned long count, const unsigned int *palette) \
{ \
	unsigned long index; \
	\
	(void)palette; \
	if (offsets) \
	{ \
		for (index = 0; index < count; index++) \
		{ \
			const unsigned char *s = source + offsets[index]; \
			\
			destination[index] = (expression); \
		} \
	} \
	else \
	{ \
		for (index = 0; index < count; index++) \
		{ \
			const unsigned char *s = source + index * (bytes); \
			\
			destination[index] = (expression); \
		} \
	} \
}

DEFINE_FETCH(a8r8g8b8, 4, load32(s))
DEFINE_FETCH(x8r8g8b8, 4, load32(s) | OPAQUE)
DEFINE_FETCH(r5g6b5, 2, argb_r5g6b5(load16(s)))
DEFINE_FETCH(a1r5g5b5, 2, argb_a1r5g5b5(load16(s)))
DEFINE_FETCH(x1r5g5b5, 2, rgb_x1r5g5b5(load16(s)) | OPAQUE)
DEFINE_FETCH(a4r4g4b4, 2, argb_a4r4g4b4(load16(s)))
DEFINE_FETCH(l8, 1, s[0] * 0x010101u | OPAQUE)
DEFINE_FETCH(al8, 1, s[0] * 0x01010101u)
DEFINE_FETCH(a8, 1, ((unsigned int)s[0] << 24) | 0x00ffffffu)
DEFINE_FETCH(a8l8, 2, s[0] * 0x010101u | ((unsigned int)s[1] << 24))
DEFINE_FETCH(p8, 1, palette ? palette[s[0]] : s[0] * 0x010101u | OPAQUE)
/* V8U8 shares this format: U (the low byte) reads as red, V as green */
DEFINE_FETCH(g8b8, 2, ARGB(255, s[0], s[1], 0))
DEFINE_FETCH(r8b8, 2, ARGB(255, s[1], 0, s[0]))
DEFINE_FETCH(r6g5b5, 2, argb_r6g5b5(load16(s)))
DEFINE_FETCH(l16, 2, s[1] * 0x010101u | OPAQUE)
DEFINE_FETCH(v16u16, 4, ARGB(255, s[1], s[3], 0))
DEFINE_FETCH(a8b8g8r8, 4, ARGB(s[3], s[0], s[1], s[2]))
DEFINE_FETCH(b8g8r8a8, 4, ARGB(s[0], s[1], s[2], s[3]))
DEFINE_FETCH(r8g8b8a8, 4, ARGB(s[0], s[3], s[2], s[1]))
DEFINE_FETCH(r5g5b5a1, 2, argb_r5g5b5a1(load16(s)))
DEFINE_FETCH(r4g4b4a4, 2, argb_r4g4b4a4(load16(s)))
DEFINE_FETCH(d24s8, 4, s[3] * 0x010101u | OPAQUE)
DEFINE_FETCH(d16, 2, s[1] * 0x010101u | OPAQUE)

/* YUY2 and UYVY hold two texels in four bytes, and are linear only: the
row must start at an even texel, which a piece of a row does. The last
texel of a row of odd width is half a pair, and the V of that pair is
past the row's bytes: it is taken as 128 (none) rather than read. */
static void fetch_yuy2(unsigned int *destination, const unsigned char *source, const unsigned long *offsets,
	unsigned long count, const unsigned int *palette)
{
	unsigned long pairs = count & ~1UL;
	unsigned long index;

	(void)offsets;
	(void)palette;
	for (index = 0; index < pairs; index++)
	{
		const unsigned char *pair = source + (index & ~1UL) * 2;

		destination[index] = argb_yuv(pair[(index & 1) ? 2 : 0], pair[1], pair[3]);
	}
	if (count & 1)
		destination[pairs] = argb_yuv(source[pairs * 2], source[pairs * 2 + 1], 128);
}

static void fetch_uyvy(unsigned int *destination, const unsigned char *source, const unsigned long *offsets,
	unsigned long count, const unsigned int *palette)
{
	unsigned long pairs = count & ~1UL;
	unsigned long index;

	(void)offsets;
	(void)palette;
	for (index = 0; index < pairs; index++)
	{
		const unsigned char *pair = source + (index & ~1UL) * 2;

		destination[index] = argb_yuv(pair[(index & 1) ? 3 : 1], pair[0], pair[2]);
	}
	if (count & 1)
		destination[pairs] = argb_yuv(source[pairs * 2 + 1], source[pairs * 2], 128);
}

static fetch_function *const fetch_functions[NUMBER_OF_TEXTURE_KINDS] =
{
	fetch_a8r8g8b8,
	fetch_a8r8g8b8, fetch_x8r8g8b8, fetch_r5g6b5, fetch_a1r5g5b5, fetch_x1r5g5b5, fetch_a4r4g4b4,
	fetch_l8, fetch_al8, fetch_a8, fetch_a8l8, fetch_p8,
	fetch_g8b8, fetch_r8b8, fetch_r6g5b5,
	fetch_l16, fetch_v16u16, fetch_a8b8g8r8, fetch_b8g8r8a8, fetch_r8g8b8a8, fetch_r5g5b5a1,
	fetch_r4g4b4a4, fetch_yuy2, fetch_uyvy, fetch_d24s8, fetch_d16,
	NULL, NULL, NULL
};

/* ---------- ARGB words into the other destinations */

/* 8 bits to 5, 6 and 4, to the nearest: (v * 31 + 127) / 255 and so on.
The results of two channels that are 16 bits apart fit their halves of a
word, so red and blue (and alpha and green) share a multiplication. */
static inline unsigned int quantize5(unsigned int v) { return (v * 249 + 1014) >> 11; }
static inline unsigned int quantize6(unsigned int v) { return (v * 253 + 505) >> 10; }
static inline unsigned int quantize4(unsigned int v) { return (v * 15 + 135) >> 8; }
#define QUANTIZE5_PAIR(pair) ((pair) * 249 + 0x03f603f6u)
#define QUANTIZE4_PAIR(pair) ((pair) * 15 + 0x00870087u)

static inline unsigned int pack_rgba8(unsigned int c)
{
	return (c & 0xff00ff00u) | ((c >> 16) & 0xff) | ((c & 0xff) << 16);
}

static inline unsigned int pack_r5g6b5(unsigned int c)
{
	unsigned int red_blue = QUANTIZE5_PAIR(c & 0x00ff00ff);

	return ((red_blue >> 16) & 0xf800) | (quantize6((c >> 8) & 0xff) << 5) | ((red_blue >> 11) & 0x001f);
}

static inline unsigned int pack_rgba4(unsigned int c)
{
	unsigned int red_blue = QUANTIZE4_PAIR(c & 0x00ff00ff);
	unsigned int alpha_green = QUANTIZE4_PAIR((c >> 8) & 0x00ff00ff);

	return ((red_blue >> 12) & 0xf000) | (alpha_green & 0x0f00) | ((red_blue >> 4) & 0x00f0) | (alpha_green >> 24);
}

static inline unsigned int pack_rgb5a1(unsigned int c)
{
	unsigned int red_blue = QUANTIZE5_PAIR(c & 0x00ff00ff);

	return ((red_blue >> 16) & 0xf800) | (quantize5((c >> 8) & 0xff) << 6) | ((red_blue >> 10) & 0x003e) | (c >> 31);
}

static inline unsigned int pack(int destination_format, unsigned int c)
{
	switch (destination_format)
	{
	case _texture_destination_rgba8: return pack_rgba8(c);
	case _texture_destination_r5g6b5: return pack_r5g6b5(c);
	case _texture_destination_rgba4: return pack_rgba4(c);
	case _texture_destination_rgb5a1: return pack_rgb5a1(c);
	default: return c;
	}
}

static void pack_row(int destination_format, void *destination, const unsigned int *source, unsigned long count)
{
	unsigned int *destination32 = destination;
	unsigned short *destination16 = destination;
	unsigned long index;

	switch (destination_format)
	{
	case _texture_destination_rgba8:
		for (index = 0; index < count; index++)
			destination32[index] = pack_rgba8(source[index]);
		break;
	case _texture_destination_r5g6b5:
		for (index = 0; index < count; index++)
			destination16[index] = (unsigned short)pack_r5g6b5(source[index]);
		break;
	case _texture_destination_rgba4:
		for (index = 0; index < count; index++)
			destination16[index] = (unsigned short)pack_rgba4(source[index]);
		break;
	case _texture_destination_rgb5a1:
		for (index = 0; index < count; index++)
			destination16[index] = (unsigned short)pack_rgb5a1(source[index]);
		break;
	}
}

/* ---------- texels the destination takes as they are, or nearly */

enum
{
	_direct_none,
	_direct_copy8,
	_direct_copy16,
	_direct_copy32,
	_direct_a1r5g5b5_rgb5a1,
	_direct_x1r5g5b5_rgb5a1,
	_direct_a4r4g4b4_rgba4,
	_direct_a8r8g8b8_rgba8,
	_direct_x8r8g8b8_rgba8
};

static int direct_conversion(int kind, int destination_format)
{
	switch (destination_format)
	{
	case _texture_destination_bgra8:
		return kind == _texture_kind_a8r8g8b8 || kind == _texture_kind_unknown ? _direct_copy32 : _direct_none;
	case _texture_destination_rgba8:
		return kind == _texture_kind_a8r8g8b8 || kind == _texture_kind_unknown ? _direct_a8r8g8b8_rgba8 :
			kind == _texture_kind_x8r8g8b8 ? _direct_x8r8g8b8_rgba8 :
			kind == _texture_kind_a8b8g8r8 ? _direct_copy32 : _direct_none;
	case _texture_destination_r5g6b5:
		return kind == _texture_kind_r5g6b5 ? _direct_copy16 : _direct_none;
	case _texture_destination_rgba4:
		return kind == _texture_kind_a4r4g4b4 ? _direct_a4r4g4b4_rgba4 :
			kind == _texture_kind_r4g4b4a4 ? _direct_copy16 : _direct_none;
	case _texture_destination_rgb5a1:
		return kind == _texture_kind_a1r5g5b5 ? _direct_a1r5g5b5_rgb5a1 :
			kind == _texture_kind_x1r5g5b5 ? _direct_x1r5g5b5_rgb5a1 :
			kind == _texture_kind_r5g5b5a1 ? _direct_copy16 : _direct_none;
	case _texture_destination_y8:
		return _direct_copy8;
	case _texture_destination_y8a8:
		return _direct_copy16;
	default:
		return _direct_none;
	}
}

#define DIRECT_LOOP(type, bytes, load, expression) \
	{ \
		type *d = destination; \
		\
		if (offsets) \
		{ \
			for (index = 0; index < count; index++) \
			{ \
				unsigned int v = load(source + offsets[index]); \
				\
				d[index] = (type)(expression); \
			} \
		} \
		else \
		{ \
			for (index = 0; index < count; index++) \
			{ \
				unsigned int v = load(source + index * (bytes)); \
				\
				d[index] = (type)(expression); \
			} \
		} \
	}

#define LOAD8(s) (*(s))

static void direct_row(int direct, void *destination, const unsigned char *source, const unsigned long *offsets,
	unsigned long count)
{
	unsigned long index;

	switch (direct)
	{
	case _direct_copy8:
		if (!offsets)
			memcpy(destination, source, count);
		else
			DIRECT_LOOP(unsigned char, 1, LOAD8, v)
		break;
	case _direct_copy16:
		if (!offsets)
			memcpy(destination, source, count * 2);
		else
			DIRECT_LOOP(unsigned short, 2, load16, v)
		break;
	case _direct_copy32:
		if (!offsets)
			memcpy(destination, source, count * 4);
		else
			DIRECT_LOOP(unsigned int, 4, load32, v)
		break;
	case _direct_a1r5g5b5_rgb5a1:
		DIRECT_LOOP(unsigned short, 2, load16, (v << 1) | (v >> 15))
		break;
	case _direct_x1r5g5b5_rgb5a1:
		DIRECT_LOOP(unsigned short, 2, load16, (v << 1) | 1)
		break;
	case _direct_a4r4g4b4_rgba4:
		DIRECT_LOOP(unsigned short, 2, load16, (v << 4) | (v >> 12))
		break;
	case _direct_a8r8g8b8_rgba8:
		DIRECT_LOOP(unsigned int, 4, load32, pack_rgba8(v))
		break;
	case _direct_x8r8g8b8_rgba8:
		DIRECT_LOOP(unsigned int, 4, load32, pack_rgba8(v) | OPAQUE)
		break;
	}
}

/* ---------- rows and levels */

void texture_convert_row(int kind, int destination_format, void *destination, const unsigned char *source,
	const unsigned long *offsets, unsigned long count, const void *palette)
{
	int direct;
	fetch_function *fetch;

	if (kind < 0 || kind >= NUMBER_OF_TEXTURE_KINDS || !texture_convert_supported(kind, destination_format))
		return;
	direct = direct_conversion(kind, destination_format);
	if (direct != _direct_none)
	{
		direct_row(direct, destination, source, offsets, count);
		return;
	}
	fetch = fetch_functions[kind];
	if (!fetch)
		return;
	/* the two-texel formats have no swizzled layout: their texels follow
	each other from source on, in every piece of the row */
	if (kind == _texture_kind_yuy2 || kind == _texture_kind_uyvy)
		offsets = NULL;
	if (destination_format == _texture_destination_bgra8)
	{
		fetch(destination, source, offsets, count, palette);
	}
	else
	{
		unsigned int piece[ROW_PIECE_TEXELS];
		unsigned long bytes = kind_bytes[kind];
		unsigned long destination_bytes = texture_convert_destination_bytes(destination_format);

		while (count)
		{
			unsigned long piece_count = count < ROW_PIECE_TEXELS ? count : ROW_PIECE_TEXELS;

			fetch(piece, source, offsets, piece_count, palette);
			pack_row(destination_format, destination, piece, piece_count);
			if (offsets)
				offsets += piece_count;
			else
				source += piece_count * bytes;
			destination = (unsigned char *)destination + piece_count * destination_bytes;
			count -= piece_count;
		}
	}
}

void texture_convert_level(int kind, int destination_format, void *destination, unsigned long destination_pitch,
	const unsigned char *source, unsigned long width, unsigned long height, unsigned long depth,
	unsigned long source_pitch, const void *palette)
{
	unsigned char *row = destination;
	unsigned long y, z;

	if (kind < 0 || kind >= NUMBER_OF_TEXTURE_KINDS)
		return;
	if (source_pitch)
	{
		for (y = 0; y < height * depth; y++)
		{
			texture_convert_row(kind, destination_format, row, source + y * source_pitch, NULL, width, palette);
			row += destination_pitch;
		}
	}
	else
	{
		struct texture_convert_swizzle_masks masks = texture_convert_swizzle_masks(width, height, depth);
		unsigned long bytes = kind_bytes[kind];
		unsigned long stack_offsets[SWIZZLE_TABLE_ENTRIES];
		unsigned long *offsets = width <= SWIZZLE_TABLE_ENTRIES ? stack_offsets : malloc(width * sizeof(unsigned long));
		unsigned long z_offset = 0;

		if (!offsets)
			return;
		texture_convert_swizzle_table(masks.x, bytes, width, offsets);
		for (z = 0; z < depth; z++)
		{
			unsigned long y_offset = 0;

			for (y = 0; y < height; y++)
			{
				texture_convert_row(kind, destination_format, row, source + (y_offset | z_offset) * bytes, offsets,
					width, palette);
				row += destination_pitch;
				y_offset = SWIZZLE_NEXT(y_offset, masks.y);
			}
			z_offset = SWIZZLE_NEXT(z_offset, masks.z);
		}
		if (offsets != stack_offsets)
			free(offsets);
	}
}

/* ---------- alpha scan */

/* what one alpha value adds to a scan: nothing for 255, one bit for 0,
both for anything between */
#define ALPHA_OF_BYTE(a) (((a) != 255) | ((((a) + 1) & 0xfe) ? _texture_alpha_full : 0))
#define ALPHA_OF_NIBBLE(a) (((a) != 15) | ((((a) + 1) & 0xe) ? _texture_alpha_full : 0))

#define ALPHA_SCAN(bytes, expression) \
	while (count && result != _texture_alpha_full) \
	{ \
		unsigned long piece_count = count < ROW_PIECE_TEXELS ? count : ROW_PIECE_TEXELS; \
		\
		for (index = 0; index < piece_count; index++) \
		{ \
			const unsigned char *s = source + index * (bytes); \
			\
			result |= (expression); \
		} \
		source += piece_count * (bytes); \
		count -= piece_count; \
	}

unsigned long texture_convert_alpha(int kind, const unsigned char *source, unsigned long count,
	const void *palette)
{
	const unsigned int *colors = palette;
	unsigned long result = _texture_alpha_opaque;
	unsigned long index;

	switch (kind)
	{
	case _texture_kind_unknown:
	case _texture_kind_a8r8g8b8:
	case _texture_kind_a8b8g8r8:
		ALPHA_SCAN(4, ALPHA_OF_BYTE(s[3]))
		break;
	case _texture_kind_b8g8r8a8:
	case _texture_kind_r8g8b8a8:
		ALPHA_SCAN(4, ALPHA_OF_BYTE(s[0]))
		break;
	case _texture_kind_al8:
	case _texture_kind_a8:
		ALPHA_SCAN(1, ALPHA_OF_BYTE(s[0]))
		break;
	case _texture_kind_a8l8:
		ALPHA_SCAN(2, ALPHA_OF_BYTE(s[1]))
		break;
	case _texture_kind_p8:
		if (colors)
			ALPHA_SCAN(1, ALPHA_OF_BYTE(colors[s[0]] >> 24))
		break;
	case _texture_kind_a1r5g5b5:
		ALPHA_SCAN(2, !(s[1] & 0x80))
		break;
	case _texture_kind_r5g5b5a1:
		ALPHA_SCAN(2, !(s[0] & 1))
		break;
	case _texture_kind_a4r4g4b4:
		ALPHA_SCAN(2, ALPHA_OF_NIBBLE(s[1] >> 4))
		break;
	case _texture_kind_r4g4b4a4:
		ALPHA_SCAN(2, ALPHA_OF_NIBBLE(s[0] & 15))
		break;
	}
	return result;
}

/* ---------- DXT */

/* a third of a sum of three bytes, rounded down */
#define THIRD(sum) (((sum) * 683) >> 11)

/* the four colors of a block as ARGB; punch_through is DXT1 */
static inline void dxt_colors(const unsigned char *block, int punch_through, unsigned int colors[4])
{
	unsigned int c0 = load16(block), c1 = load16(block + 2);
	unsigned int rgb0 = argb_r5g6b5(c0), rgb1 = argb_r5g6b5(c1);
	/* red and blue in the halves of a word, where their sums have room */
	unsigned int rb0 = rgb0 & 0x00ff00ff, g0 = (rgb0 >> 8) & 0xff;
	unsigned int rb1 = rgb1 & 0x00ff00ff, g1 = (rgb1 >> 8) & 0xff;

	colors[0] = rgb0;
	colors[1] = rgb1;
	if (c0 > c1 || !punch_through)
	{
		unsigned int rb2 = 2 * rb0 + rb1, rb3 = rb0 + 2 * rb1;

		colors[2] = ARGB(255, THIRD(rb2 >> 16), THIRD(2 * g0 + g1), THIRD(rb2 & 0xffff));
		colors[3] = ARGB(255, THIRD(rb3 >> 16), THIRD(g0 + 2 * g1), THIRD(rb3 & 0xffff));
	}
	else
	{
		colors[2] = (((rb0 + rb1) >> 1) & 0x00ff00ff) | (((g0 + g1) >> 1) << 8) | OPAQUE;
		colors[3] = 0;
	}
}

/* the eight alpha values of a DXT5 block */
static inline void dxt5_alphas(const unsigned char *block, unsigned int values[8])
{
	unsigned int a0 = block[0], a1 = block[1];
	int index;

	values[0] = a0;
	values[1] = a1;
	if (a0 > a1)
	{
		for (index = 2; index < 8; index++)
			values[index] = ((8 - index) * a0 + (index - 1) * a1) / 7;
	}
	else
	{
		for (index = 2; index < 6; index++)
			values[index] = ((6 - index) * a0 + (index - 1) * a1) / 5;
		values[6] = 0;
		values[7] = 255;
	}
}

/* an alpha value where the destination keeps it, to be or'ed with a
color packed with alpha 0 */
static inline unsigned int pack_alpha(int destination_format, unsigned int alpha)
{
	switch (destination_format)
	{
	case _texture_destination_bgra8:
	case _texture_destination_rgba8:
		return alpha << 24;
	case _texture_destination_rgba4:
		return quantize4(alpha);
	case _texture_destination_rgb5a1:
		return alpha >> 7;
	default:
		return 0;
	}
}

/* The block writers, for 32-bit and for 16-bit destinations: a 4x4 block
from its packed colors, the two index bits of each texel, and for DXT3 and
DXT5 the alpha of each texel. dxt_rows_<bits> is one row of blocks; it is
expanded for each kind and destination (texture_convert_dxt_rows), so that
neither is asked about block after block. */
#define DEFINE_DXT_WRITERS(suffix, type) \
static inline void dxt1_write_##suffix(type *destination, unsigned long pitch, const type colors[4], \
	unsigned int bits) \
{ \
	int row; \
	\
	for (row = 0; row < 4; row++) \
	{ \
		destination[0] = colors[bits & 3]; \
		destination[1] = colors[(bits >> 2) & 3]; \
		destination[2] = colors[(bits >> 4) & 3]; \
		destination[3] = colors[(bits >> 6) & 3]; \
		bits >>= 8; \
		destination = (type *)((unsigned char *)destination + pitch); \
	} \
} \
\
static inline void dxt3_write_##suffix(type *destination, unsigned long pitch, const type colors[4], \
	unsigned int bits, const unsigned char *block, const type alphas[16]) \
{ \
	int row; \
	\
	for (row = 0; row < 4; row++) \
	{ \
		unsigned int low = block[row * 2], high = block[row * 2 + 1]; \
		\
		destination[0] = colors[bits & 3] | alphas[low & 15]; \
		destination[1] = colors[(bits >> 2) & 3] | alphas[low >> 4]; \
		destination[2] = colors[(bits >> 4) & 3] | alphas[high & 15]; \
		destination[3] = colors[(bits >> 6) & 3] | alphas[high >> 4]; \
		bits >>= 8; \
		destination = (type *)((unsigned char *)destination + pitch); \
	} \
} \
\
static inline void dxt5_write_##suffix(type *destination, unsigned long pitch, const type colors[4], \
	unsigned int bits, const unsigned char *block, const type alphas[8]) \
{ \
	int half, row; \
	\
	/* three bits a texel: 24 bits for each two rows */ \
	for (half = 0; half < 2; half++) \
	{ \
		unsigned int alpha_bits = block[2 + half * 3] | ((unsigned int)block[3 + half * 3] << 8) | \
			((unsigned int)block[4 + half * 3] << 16); \
		\
		for (row = 0; row < 2; row++) \
		{ \
			destination[0] = colors[bits & 3] | alphas[alpha_bits & 7]; \
			destination[1] = colors[(bits >> 2) & 3] | alphas[(alpha_bits >> 3) & 7]; \
			destination[2] = colors[(bits >> 4) & 3] | alphas[(alpha_bits >> 6) & 7]; \
			destination[3] = colors[(bits >> 6) & 3] | alphas[(alpha_bits >> 9) & 7]; \
			bits >>= 8; \
			alpha_bits >>= 12; \
			destination = (type *)((unsigned char *)destination + pitch); \
		} \
	} \
} \
\
static inline __attribute__((always_inline)) void dxt_rows_##suffix(int kind, int destination_format, \
	type *destination, unsigned long pitch, const unsigned char *blocks, unsigned long width, unsigned long rows) \
{ \
	unsigned long block_count = (width + 3) / 4; \
	unsigned long block_index; \
	type dxt3_alphas[16]; \
	int index; \
	\
	if (kind == _texture_kind_dxt3) \
	{ \
		for (index = 0; index < 16; index++) \
			dxt3_alphas[index] = (type)pack_alpha(destination_format, (unsigned int)index * 0x11); \
	} \
	for (block_index = 0; block_index < block_count; block_index++) \
	{ \
		unsigned long columns = width - block_index * 4 < 4 ? width - block_index * 4 : 4; \
		int whole = columns == 4 && rows == 4; \
		const unsigned char *color_block = kind == _texture_kind_dxt1 ? blocks : blocks + 8; \
		unsigned int argb[4]; \
		type colors[4]; \
		type scratch[16]; \
		type *block_destination = whole ? destination + block_index * 4 : scratch; \
		unsigned long block_pitch = whole ? pitch : 4 * sizeof(type); \
		unsigned int bits = load32(color_block + 4); \
		\
		dxt_colors(color_block, kind == _texture_kind_dxt1, argb); \
		/* DXT3 and DXT5 colors are packed with alpha 0, for the texel's to be or'ed in */ \
		for (index = 0; index < 4; index++) \
			colors[index] = (type)pack(destination_format, kind == _texture_kind_dxt1 ? argb[index] : \
				argb[index] & 0x00ffffffu); \
		if (destination_format == _texture_destination_r5g6b5) \
		{ \
			/* the block's own two colors are what they would be packed to */ \
			colors[0] = (type)load16(color_block); \
			colors[1] = (type)load16(color_block + 2); \
		} \
		if (kind == _texture_kind_dxt1) \
		{ \
			dxt1_write_##suffix(block_destination, block_pitch, colors, bits); \
		} \
		else \
		{ \
			if (kind == _texture_kind_dxt3) \
			{ \
				dxt3_write_##suffix(block_destination, block_pitch, colors, bits, blocks, dxt3_alphas); \
			} \
			else \
			{ \
				unsigned int values[8]; \
				type alphas[8]; \
				\
				dxt5_alphas(blocks, values); \
				for (index = 0; index < 8; index++) \
					alphas[index] = (type)pack_alpha(destination_format, values[index]); \
				dxt5_write_##suffix(block_destination, block_pitch, colors, bits, blocks, alphas); \
			} \
		} \
		if (!whole) \
		{ \
			/* a block that reaches past the level: only the texels inside */ \
			unsigned long row; \
			\
			for (row = 0; row < rows; row++) \
			{ \
				memcpy((unsigned char *)(destination + block_index * 4) + row * pitch, scratch + row * 4, \
					columns * sizeof(type)); \
			} \
		} \
		blocks += kind == _texture_kind_dxt1 ? 8 : 16; \
	} \
}

DEFINE_DXT_WRITERS(32, unsigned int)
DEFINE_DXT_WRITERS(16, unsigned short)

static int kind_is_dxt(int kind)
{
	return kind == _texture_kind_dxt1 || kind == _texture_kind_dxt3 || kind == _texture_kind_dxt5;
}

#define DXT_ROWS(suffix, format) \
	switch (kind) \
	{ \
	case _texture_kind_dxt1: \
		dxt_rows_##suffix(_texture_kind_dxt1, format, destination, destination_pitch, blocks, width, rows); \
		break; \
	case _texture_kind_dxt3: \
		dxt_rows_##suffix(_texture_kind_dxt3, format, destination, destination_pitch, blocks, width, rows); \
		break; \
	default: \
		dxt_rows_##suffix(_texture_kind_dxt5, format, destination, destination_pitch, blocks, width, rows); \
		break; \
	}

void texture_convert_dxt_rows(int kind, int destination_format, void *destination, unsigned long destination_pitch,
	const unsigned char *blocks, unsigned long width, unsigned long rows)
{
	if (!kind_is_dxt(kind) || rows > 4)
		return;
	switch (destination_format)
	{
	case _texture_destination_bgra8: DXT_ROWS(32, _texture_destination_bgra8) break;
	case _texture_destination_rgba8: DXT_ROWS(32, _texture_destination_rgba8) break;
	case _texture_destination_r5g6b5: DXT_ROWS(16, _texture_destination_r5g6b5) break;
	case _texture_destination_rgba4: DXT_ROWS(16, _texture_destination_rgba4) break;
	case _texture_destination_rgb5a1: DXT_ROWS(16, _texture_destination_rgb5a1) break;
	}
}

void texture_convert_dxt_level(int kind, int destination_format, void *destination, unsigned long destination_pitch,
	const unsigned char *source, unsigned long width, unsigned long height, unsigned long depth)
{
	unsigned long block_row_bytes = ((width + 3) / 4) * (kind == _texture_kind_dxt1 ? 8 : 16);
	unsigned char *row = destination;
	unsigned long y, z;

	for (z = 0; z < depth; z++)
	{
		for (y = 0; y < height; y += 4)
		{
			unsigned long rows = height - y < 4 ? height - y : 4;

			texture_convert_dxt_rows(kind, destination_format, row, destination_pitch, source, width, rows);
			source += block_row_bytes;
			row += rows * destination_pitch;
		}
	}
}

unsigned long texture_convert_dxt_alpha(int kind, const unsigned char *blocks, unsigned long width,
	unsigned long rows)
{
	unsigned long block_count = (width + 3) / 4;
	unsigned long result = _texture_alpha_opaque;
	/* nothing more to find once this is reached */
	unsigned long most = kind == _texture_kind_dxt1 ? _texture_alpha_one_bit : _texture_alpha_full;
	unsigned long block_index;

	if (!kind_is_dxt(kind) || rows > 4)
		return result;
	if (!rows)
		return result;
	for (block_index = 0; block_index < block_count && result != most; block_index++)
	{
		unsigned long columns = width - block_index * 4 < 4 ? width - block_index * 4 : 4;
		/* bit 4 * row + column: the texels inside the level */
		unsigned int inside = ((0xfu >> (4 - columns)) * 0x1111u) & (0xffffu >> (16 - rows * 4));
		int index;

		if (kind == _texture_kind_dxt1)
		{
			if (load16(blocks) <= load16(blocks + 2))
			{
				/* a texel is transparent when both its index bits are set; the
				texels inside the level, as the low bit of each pair */
				unsigned int bits = load32(blocks + 4);
				unsigned int inside_pairs = ((0xffu >> (8 - columns * 2)) * 0x01010101u) & (0xffffffffu >> (32 - rows * 8)) &
					0x55555555u;

				if (bits & (bits >> 1) & inside_pairs)
					result |= _texture_alpha_one_bit;
			}
			blocks += 8;
		}
		else if (kind == _texture_kind_dxt3)
		{
			unsigned int low = load32(blocks), high = load32(blocks + 4);

			if (inside != 0xffff)
			{
				for (index = 0; index < 16; index++)
				{
					unsigned int alpha = (blocks[index / 2] >> ((index & 1) * 4)) & 15;

					if ((inside >> index) & 1)
						result |= ALPHA_OF_NIBBLE(alpha);
				}
			}
			else if (low != 0xffffffffu || high != 0xffffffffu)
			{
				/* a 4-bit alpha is 0 or 15 when each of its bits equals the next */
				result |= _texture_alpha_one_bit;
				if (((low ^ (low >> 1)) | (high ^ (high >> 1))) & 0x77777777u)
					result |= _texture_alpha_full;
			}
			blocks += 16;
		}
		else
		{
			unsigned int values[8];
			unsigned int low = blocks[2] | ((unsigned int)blocks[3] << 8) | ((unsigned int)blocks[4] << 16);
			unsigned int high = blocks[5] | ((unsigned int)blocks[6] << 8) | ((unsigned int)blocks[7] << 16);

			dxt5_alphas(blocks, values);
			for (index = 0; index < 8; index++)
			{
				unsigned int alpha = values[(low >> (index * 3)) & 7];

				if ((inside >> index) & 1)
					result |= ALPHA_OF_BYTE(alpha);
				alpha = values[(high >> (index * 3)) & 7];
				if ((inside >> (index + 8)) & 1)
					result |= ALPHA_OF_BYTE(alpha);
			}
			blocks += 16;
		}
	}
	return result;
}

/* ---------- sizes OpenGL 1.1 takes */

unsigned long texture_convert_power_of_two(unsigned long value)
{
	unsigned long result = 1;

	while (result < value && result < 0x80000000UL)
		result <<= 1;
	return result;
}

void texture_convert_pad_row(void *row, unsigned long count, unsigned long padded_count, unsigned long bytes)
{
	unsigned long index;

	if (!count)
		return;
	switch (bytes)
	{
	case 4:
	{
		unsigned int *texels = row;
		unsigned int last = texels[count - 1];

		for (index = count; index < padded_count; index++)
			texels[index] = last;
		break;
	}
	case 2:
	{
		unsigned short *texels = row;
		unsigned short last = texels[count - 1];

		for (index = count; index < padded_count; index++)
			texels[index] = last;
		break;
	}
	case 1:
		if (padded_count > count)
			memset((unsigned char *)row + count, ((unsigned char *)row)[count - 1], padded_count - count);
		break;
	}
}

void texture_convert_pad_rows(void *image, unsigned long pitch, unsigned long rows, unsigned long padded_rows)
{
	unsigned char *last;
	unsigned long index;

	if (!rows)
		return;
	last = (unsigned char *)image + (rows - 1) * pitch;
	for (index = rows; index < padded_rows; index++)
		memcpy((unsigned char *)image + index * pitch, last, pitch);
}

void texture_convert_pad_scale(unsigned long width, unsigned long height, unsigned long padded_width,
	unsigned long padded_height, int coordinates_in_texels, float scale[2])
{
	scale[0] = (coordinates_in_texels ? 1.0f : (float)width) / (float)padded_width;
	scale[1] = (coordinates_in_texels ? 1.0f : (float)height) / (float)padded_height;
}

unsigned long texture_convert_levels_to_drop(unsigned long width, unsigned long height, unsigned long depth,
	unsigned long maximum_size)
{
	unsigned long largest = width > height ? width : height;
	unsigned long result = 0;

	if (depth > largest)
		largest = depth;
	if (!maximum_size)
		maximum_size = 1;
	while (largest > maximum_size)
	{
		largest >>= 1;
		result++;
	}
	return result;
}

/* the bits of each channel of a destination: shift and width, four
channels at most */
struct channel_layout
{
	unsigned char count;
	unsigned char shift[4];
	unsigned char bits[4];
};

static const struct channel_layout channel_layouts[NUMBER_OF_TEXTURE_DESTINATIONS] =
{
	{ 4, { 0, 8, 16, 24 }, { 8, 8, 8, 8 } },
	{ 4, { 0, 8, 16, 24 }, { 8, 8, 8, 8 } },
	{ 3, { 0, 5, 11, 0 }, { 5, 6, 5, 0 } },
	{ 4, { 0, 4, 8, 12 }, { 4, 4, 4, 4 } },
	{ 4, { 0, 1, 6, 11 }, { 1, 5, 5, 5 } },
	{ 1, { 0, 0, 0, 0 }, { 8, 0, 0, 0 } },
	{ 2, { 0, 8, 0, 0 }, { 8, 8, 0, 0 } },
};

static inline unsigned int load_texel(const void *row, unsigned long index, unsigned long bytes)
{
	switch (bytes)
	{
	case 4: return ((const unsigned int *)row)[index];
	case 2: return ((const unsigned short *)row)[index];
	default: return ((const unsigned char *)row)[index];
	}
}

void texture_convert_halve_row(int destination_format, void *destination, const void *row0, const void *row1,
	const void *row2, const void *row3, unsigned long source_width)
{
	const struct channel_layout *layout;
	unsigned long bytes, count, columns, index;
	const void *rows[4];
	unsigned long row_count = row2 && row3 ? 4 : 2;
	unsigned int shift;

	if (destination_format < 0 || destination_format >= NUMBER_OF_TEXTURE_DESTINATIONS)
		return;
	layout = &channel_layouts[destination_format];
	bytes = texture_convert_destination_bytes(destination_format);
	count = source_width > 1 ? source_width / 2 : 1;
	columns = source_width > 1 ? 2 : 1;
	rows[0] = row0;
	rows[1] = row1;
	rows[2] = row2;
	rows[3] = row3;
	/* the texels averaged are 2, 4 or 8 */
	shift = (columns == 2 ? 1 : 0) + (row_count == 4 ? 2 : 1);

	if (bytes == 4 && columns == 2 && row_count == 2)
	{
		/* the common case, two channels at a time */
		const unsigned int *a = row0, *b = row1;
		unsigned int *d = destination;

		for (index = 0; index < count; index++)
		{
			unsigned int p0 = a[index * 2], p1 = a[index * 2 + 1], p2 = b[index * 2], p3 = b[index * 2 + 1];
			unsigned int even = (p0 & 0x00ff00ff) + (p1 & 0x00ff00ff) + (p2 & 0x00ff00ff) + (p3 & 0x00ff00ff) + 0x00020002;
			unsigned int odd = ((p0 >> 8) & 0x00ff00ff) + ((p1 >> 8) & 0x00ff00ff) + ((p2 >> 8) & 0x00ff00ff) +
				((p3 >> 8) & 0x00ff00ff) + 0x00020002;

			d[index] = ((even >> 2) & 0x00ff00ff) | (((odd >> 2) & 0x00ff00ff) << 8);
		}
		return;
	}

	for (index = 0; index < count; index++)
	{
		unsigned int result = 0;
		unsigned int channel;

		for (channel = 0; channel < layout->count; channel++)
		{
			unsigned int mask = (1u << layout->bits[channel]) - 1;
			unsigned int sum = 0;
			unsigned long row, column;

			for (row = 0; row < row_count; row++)
			{
				for (column = 0; column < columns; column++)
					sum += (load_texel(rows[row], index * columns + column, bytes) >> layout->shift[channel]) & mask;
			}
			result |= ((sum + (1u << (shift - 1))) >> shift) << layout->shift[channel];
		}
		switch (bytes)
		{
		case 4: ((unsigned int *)destination)[index] = result; break;
		case 2: ((unsigned short *)destination)[index] = (unsigned short)result; break;
		default: ((unsigned char *)destination)[index] = (unsigned char)result; break;
		}
	}
}
