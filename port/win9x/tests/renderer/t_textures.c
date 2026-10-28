/*
T_TEXTURES.C

The texel converters of the Windows 9x renderers (port/linux/src/
xbox_textures_convert.c) against the code they replace: xbox_textures.c
itself is compiled into this program, with stubs for what it takes from
the device, so that its decode_level and its DXT decoder are the
reference. The DXT decoders are also checked against a third one written
here from the S3TC description.

    t_textures                  random texels: every format, many sizes
    t_textures --maps <dump>... every bitmap of maps (bitmap_dump.py)
    t_textures --bench          cycles per texel, old and new, random texels
    t_textures --bench <dump>...                 the bitmaps of maps

The 32-bit destinations must match bit for bit; the 16-bit ones must be
the old result rounded to the nearest 16-bit value.
*/

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../w9x_test.h"

#include "xbox_textures_under_test.h"

#include "../../../linux/src/xbox_textures_convert.h"

/* ---------- the reference for the destinations the old code never made */

static unsigned int nearest(unsigned int value, unsigned int maximum)
{
	return (value * maximum + 127) / 255;
}

static unsigned int reference_pack(int destination, unsigned int argb)
{
	unsigned int a = argb >> 24, r = (argb >> 16) & 0xff, g = (argb >> 8) & 0xff, b = argb & 0xff;

	switch (destination)
	{
	case _texture_destination_rgba8: return r | (g << 8) | (b << 16) | (a << 24);
	case _texture_destination_r5g6b5: return (nearest(r, 31) << 11) | (nearest(g, 63) << 5) | nearest(b, 31);
	case _texture_destination_rgba4: return (nearest(r, 15) << 12) | (nearest(g, 15) << 8) | (nearest(b, 15) << 4) | nearest(a, 15);
	case _texture_destination_rgb5a1: return (nearest(r, 31) << 11) | (nearest(g, 31) << 6) | (nearest(b, 31) << 1) | (a >= 128);
	default: return argb;
	}
}

/* the y8 and y8a8 destinations hold the texel's own bytes: what the old
conversion spread over the channels */
static unsigned int reference_native(int kind, int destination, unsigned int argb)
{
	if (destination == _texture_destination_y8)
		return kind == _texel_a8 ? argb >> 24 : argb & 0xff;
	return (argb & 0xff) | ((argb >> 24) << 8);
}

static unsigned long reference_alpha(const unsigned long *argb, unsigned long count)
{
	unsigned long result = _texture_alpha_opaque, index;

	for (index = 0; index < count; index++)
	{
		unsigned long alpha = argb[index] >> 24;

		if (alpha != 255)
			result |= alpha ? _texture_alpha_full : _texture_alpha_one_bit;
	}
	return result;
}

/* ---------- a DXT decoder from the format's description, one texel at a
time (EXT_texture_compression_s3tc; colors widened by bit replication,
divisions rounded down) */

static void reference_color(unsigned int color565, unsigned int rgb[3])
{
	unsigned int r = color565 >> 11, g = (color565 >> 5) & 63, b = color565 & 31;

	rgb[0] = (r << 3) | (r >> 2);
	rgb[1] = (g << 2) | (g >> 4);
	rgb[2] = (b << 3) | (b >> 2);
}

static unsigned int reference_dxt_texel(int kind, const unsigned char *block, unsigned int x, unsigned int y)
{
	const unsigned char *color_block = kind == _texel_dxt1 ? block : block + 8;
	unsigned int color0 = color_block[0] + color_block[1] * 256, color1 = color_block[2] + color_block[3] * 256;
	unsigned int code = (color_block[4 + y] >> (2 * x)) & 3;
	unsigned int c0[3], c1[3], rgb[3], alpha = 255;
	int channel;

	reference_color(color0, c0);
	reference_color(color1, c1);
	for (channel = 0; channel < 3; channel++)
	{
		if (code == 0)
			rgb[channel] = c0[channel];
		else if (code == 1)
			rgb[channel] = c1[channel];
		else if (kind != _texel_dxt1 || color0 > color1)
			rgb[channel] = code == 2 ? (2 * c0[channel] + c1[channel]) / 3 : (c0[channel] + 2 * c1[channel]) / 3;
		else if (code == 2)
			rgb[channel] = (c0[channel] + c1[channel]) / 2;
		else
			rgb[channel] = 0;
	}
	if (kind == _texel_dxt1)
	{
		if (color0 <= color1 && code == 3)
			alpha = 0;
	}
	else if (kind == _texel_dxt3)
	{
		unsigned long long bits = 0;
		int index;

		for (index = 7; index >= 0; index--)
			bits = (bits << 8) | block[index];
		alpha = (unsigned int)((bits >> (4 * (4 * y + x))) & 15) * 255 / 15;
	}
	else
	{
		unsigned int alpha0 = block[0], alpha1 = block[1];
		unsigned long long bits = 0;
		unsigned int alpha_code;
		int index;

		for (index = 7; index >= 2; index--)
			bits = (bits << 8) | block[index];
		alpha_code = (unsigned int)((bits >> (3 * (4 * y + x))) & 7);
		if (alpha_code == 0)
			alpha = alpha0;
		else if (alpha_code == 1)
			alpha = alpha1;
		else if (alpha0 > alpha1)
			alpha = ((8 - alpha_code) * alpha0 + (alpha_code - 1) * alpha1) / 7;
		else if (alpha_code == 6)
			alpha = 0;
		else if (alpha_code == 7)
			alpha = 255;
		else
			alpha = ((6 - alpha_code) * alpha0 + (alpha_code - 1) * alpha1) / 5;
	}
	return (alpha << 24) | (rgb[0] << 16) | (rgb[1] << 8) | rgb[2];
}

static void reference_dxt_level(int kind, const unsigned char *source, unsigned long width, unsigned long height,
	unsigned long depth, unsigned long *destination)
{
	unsigned long blocks_x = (width + 3) / 4, blocks_y = (height + 3) / 4;
	unsigned long block_bytes = kind == _texel_dxt1 ? 8 : 16;
	unsigned long x, y, z;

	for (z = 0; z < depth; z++)
	{
		for (y = 0; y < height; y++)
		{
			for (x = 0; x < width; x++)
			{
				destination[(z * height + y) * width + x] = reference_dxt_texel(kind,
					source + ((z * blocks_y + y / 4) * blocks_x + x / 4) * block_bytes, x & 3, y & 3);
			}
		}
	}
}

/* ---------- one level through both paths */

static unsigned long levels_checked, texels_checked;
static unsigned long first_failures;

static void fail(const char *format, ...)
{
	va_list arguments;

	test_checks++;
	test_failures++;
	if (first_failures++ < 20)
	{
		printf("# FAIL ");
		va_start(arguments, format);
		vprintf(format, arguments);
		va_end(arguments);
		printf("\n");
	}
}

#define GUARD_TEXELS 3
#define GUARD_BYTE 0xa5

/* the level by the old code, as ARGB words */
static void old_level(const struct xgpu_texture_description *description, unsigned long level,
	const unsigned char *source, const D3DCOLOR *palette, unsigned long *destination)
{
	struct format_information information = format_information(description->format);

	if (description->compressed)
	{
		dxt_decode_level(information.kind, source, level_dimension(description->width, level),
			level_dimension(description->height, level), level_dimension(description->depth, level), destination);
	}
	else
	{
		decode_level(description, level, source, palette, destination);
	}
}

/* the level by the new code. Rows are GUARD_TEXELS wider than the level,
which must stay untouched. */
static void new_level(int kind, int destination_format, unsigned char *destination, unsigned long pitch,
	const unsigned char *source, unsigned long width, unsigned long height, unsigned long depth,
	unsigned long source_pitch, const D3DCOLOR *palette)
{
	if (kind_compressed((unsigned char)kind))
		texture_convert_dxt_level(kind, destination_format, destination, pitch, source, width, height, depth);
	else
		texture_convert_level(kind, destination_format, destination, pitch, source, width, height, depth,
			source_pitch, palette);
}

static unsigned long new_alpha(int kind, const unsigned char *source, unsigned long width, unsigned long height,
	unsigned long depth, unsigned long source_pitch, const D3DCOLOR *palette)
{
	unsigned long result = _texture_alpha_opaque;
	unsigned long y, z;

	if (kind_compressed((unsigned char)kind))
	{
		unsigned long row_bytes = ((width + 3) / 4) * (kind == _texel_dxt1 ? 8 : 16);

		for (z = 0; z < depth; z++)
		{
			for (y = 0; y < height; y += 4)
			{
				result |= texture_convert_dxt_alpha(kind, source, width, height - y < 4 ? height - y : 4);
				source += row_bytes;
			}
		}
	}
	else if (source_pitch)
	{
		for (y = 0; y < height; y++)
			result |= texture_convert_alpha(kind, source + y * source_pitch, width, palette);
	}
	else
	{
		result = texture_convert_alpha(kind, source, width * height * depth, palette);
	}
	return result;
}

static unsigned int read_texel(const unsigned char *texel, unsigned long bytes)
{
	return bytes == 4 ? *(const unsigned int *)texel : bytes == 2 ? *(const unsigned short *)texel : *texel;
}

/* compares the level in every destination; returns the alpha the scan found */
static unsigned long check_level(const struct xgpu_texture_description *description, unsigned long level,
	const unsigned char *source, const D3DCOLOR *palette, const unsigned long *expected, const char *what)
{
	struct format_information information = format_information(description->format);
	int kind = information.kind;
	unsigned long width = level_dimension(description->width, level);
	unsigned long height = level_dimension(description->height, level);
	unsigned long depth = level_dimension(description->depth, level);
	unsigned long source_pitch = !description->compressed && description->linear ? description->pitch : 0;
	unsigned long count = width * height * depth;
	unsigned long *old = malloc(count * sizeof(unsigned long));
	unsigned char *converted = malloc((width + GUARD_TEXELS) * height * depth * 4);
	unsigned long alpha, expected_alpha;
	int destination;

	old_level(description, level, source, palette, old);
	if (expected && memcmp(old, expected, count * sizeof(unsigned long)))
		fail("%s format %02lx %lux%lux%lu level %lu: the old decoder and the reference differ", what,
			(unsigned long)description->format, width, height, depth, level);

	for (destination = 0; destination < NUMBER_OF_TEXTURE_DESTINATIONS; destination++)
	{
		unsigned long bytes = texture_convert_destination_bytes(destination);
		unsigned long pitch = (width + GUARD_TEXELS) * bytes;
		unsigned long row, x, bad = 0;

		if (!texture_convert_supported(kind, destination))
			continue;
		memset(converted, GUARD_BYTE, pitch * height * depth);
		new_level(kind, destination, converted, pitch, source, width, height, depth, source_pitch, palette);
		for (row = 0; row < height * depth && !bad; row++)
		{
			const unsigned char *texels = converted + row * pitch;

			for (x = 0; x < width; x++)
			{
				unsigned int argb = (unsigned int)old[row * width + x];
				unsigned int want = destination == _texture_destination_y8 || destination == _texture_destination_y8a8 ?
					reference_native(kind, destination, argb) : reference_pack(destination, argb);
				unsigned int got = read_texel(texels + x * bytes, bytes);

				if (got != want)
				{
					fail("%s format %02lx %lux%lux%lu level %lu destination %d: texel %lu,%lu is %08x, not %08x (old %08x)",
						what, (unsigned long)description->format, width, height, depth, level, destination, x, row, got,
						want, argb);
					bad = 1;
					break;
				}
			}
			for (x = width * bytes; x < pitch; x++)
			{
				if (texels[x] != GUARD_BYTE)
				{
					fail("%s format %02lx %lux%lux%lu level %lu destination %d: wrote past row %lu", what,
						(unsigned long)description->format, width, height, depth, level, destination, row);
					bad = 1;
					break;
				}
			}
		}
		test_checks++;
	}

	alpha = new_alpha(kind, source, width, height, depth, source_pitch, palette);
	expected_alpha = reference_alpha(old, count);
	if (alpha != expected_alpha)
		fail("%s format %02lx %lux%lux%lu level %lu: alpha scan %lu, texels say %lu", what,
			(unsigned long)description->format, width, height, depth, level, alpha, expected_alpha);
	if ((alpha | texture_convert_kind_alpha(kind)) != texture_convert_kind_alpha(kind))
		fail("%s format %02lx: alpha %lu is more than its kind's", what, (unsigned long)description->format, alpha);
	test_checks++;
	levels_checked++;
	texels_checked += count;
	free(converted);
	free(old);
	return alpha;
}

/* ---------- random data */

static D3DCOLOR random_palette[256];

/* shape 0: any bytes; 1: bytes that are mostly 0 or 255 (one-bit alpha
and its neighbours); 2: all 255 */
static void random_bytes(unsigned char *bytes, unsigned long count, int shape)
{
	unsigned long index;

	for (index = 0; index < count; index++)
	{
		unsigned long long value = test_random();

		bytes[index] = shape == 2 ? 255 : shape == 1 && (value & 0x700) ? ((value & 0x800) ? 255 : 0) :
			(unsigned char)value;
	}
}

static void random_palettes(int shape)
{
	unsigned char bytes[4];
	int index;

	for (index = 0; index < 256; index++)
	{
		random_bytes(bytes, 4, shape);
		random_palette[index] = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | ((unsigned long)bytes[3] << 24);
	}
}

static void test_formats(void)
{
	CHECK(_texture_kind_unknown == _texel_unknown);
	CHECK(_texture_kind_a8r8g8b8 == _texel_a8r8g8b8);
	CHECK(_texture_kind_x8r8g8b8 == _texel_x8r8g8b8);
	CHECK(_texture_kind_r5g6b5 == _texel_r5g6b5);
	CHECK(_texture_kind_a1r5g5b5 == _texel_a1r5g5b5);
	CHECK(_texture_kind_x1r5g5b5 == _texel_x1r5g5b5);
	CHECK(_texture_kind_a4r4g4b4 == _texel_a4r4g4b4);
	CHECK(_texture_kind_l8 == _texel_l8);
	CHECK(_texture_kind_al8 == _texel_al8);
	CHECK(_texture_kind_a8 == _texel_a8);
	CHECK(_texture_kind_a8l8 == _texel_a8l8);
	CHECK(_texture_kind_p8 == _texel_p8);
	CHECK(_texture_kind_g8b8 == _texel_g8b8);
	CHECK(_texture_kind_r8b8 == _texel_r8b8);
	CHECK(_texture_kind_r6g5b5 == _texel_r6g5b5);
	CHECK(_texture_kind_l16 == _texel_l16);
	CHECK(_texture_kind_v16u16 == _texel_v16u16);
	CHECK(_texture_kind_a8b8g8r8 == _texel_a8b8g8r8);
	CHECK(_texture_kind_b8g8r8a8 == _texel_b8g8r8a8);
	CHECK(_texture_kind_r8g8b8a8 == _texel_r8g8b8a8);
	CHECK(_texture_kind_r5g5b5a1 == _texel_r5g5b5a1);
	CHECK(_texture_kind_r4g4b4a4 == _texel_r4g4b4a4);
	CHECK(_texture_kind_yuy2 == _texel_yuy2);
	CHECK(_texture_kind_uyvy == _texel_uyvy);
	CHECK(_texture_kind_d24s8 == _texel_d24s8);
	CHECK(_texture_kind_d16 == _texel_d16);
	CHECK(_texture_kind_dxt1 == _texel_dxt1);
	CHECK(_texture_kind_dxt3 == _texel_dxt3);
	CHECK(_texture_kind_dxt5 == _texel_dxt5);
	{
		DWORD format;

		for (format = 0; format < 0x42; format++)
		{
			struct format_information information = format_information(format);

			CHECK(texture_convert_kind_bytes(information.kind) == information.bytes);
		}
	}
	CHECK(texture_convert_destination_16bit(_texture_alpha_opaque) == _texture_destination_r5g6b5);
	CHECK(texture_convert_destination_16bit(_texture_alpha_one_bit) == _texture_destination_rgb5a1);
	CHECK(texture_convert_destination_16bit(_texture_alpha_full) == _texture_destination_rgba4);
}

static void test_swizzle(void)
{
	static unsigned long table[4096];
	unsigned long width, height, depth, index, bytes;
	unsigned long combinations = 0;

	for (width = 1; width <= 4096; width <<= 1)
	{
		for (height = 1; height <= 4096; height <<= 1)
		{
			for (depth = 1; depth <= 512; depth <<= 1)
			{
				struct swizzle_masks old = swizzle_masks(width, height, depth);
				struct texture_convert_swizzle_masks masks = texture_convert_swizzle_masks(width, height, depth);
				int same = old.x == masks.x && old.y == masks.y && old.z == masks.z;

				if (!same)
					fail("swizzle masks of %lux%lux%lu differ", width, height, depth);
				for (bytes = 1; bytes <= 4 && same; bytes <<= 1)
				{
					texture_convert_swizzle_table(masks.x, bytes, width, table);
					for (index = 0; index < width; index++)
						same &= table[index] == spread(old.x, index) * bytes;
					texture_convert_swizzle_table(masks.y, bytes, height, table);
					for (index = 0; index < height; index++)
						same &= table[index] == spread(old.y, index) * bytes;
					texture_convert_swizzle_table(masks.z, bytes, depth, table);
					for (index = 0; index < depth; index++)
						same &= table[index] == spread(old.z, index) * bytes;
				}
				for (index = 0; index < 64 && same; index++)
				{
					unsigned long value = (unsigned long)test_random() & (height - 1);

					same &= texture_convert_swizzle_offset(masks.y, value) == spread(old.y, value);
				}
				if (!same)
					fail("swizzle offsets of %lux%lux%lu differ", width, height, depth);
				test_checks++;
				combinations++;
			}
		}
	}
	printf("# swizzle: %lu sizes, masks and tables match spread()\n", combinations);
}

static DWORD swizzled_format_word(DWORD format, unsigned long width, unsigned long height, unsigned long depth,
	unsigned long levels)
{
	return (floor_log2(depth) << D3DFORMAT_PSIZE_SHIFT) | (floor_log2(height) << D3DFORMAT_VSIZE_SHIFT) |
		(floor_log2(width) << D3DFORMAT_USIZE_SHIFT) | (format << D3DFORMAT_FORMAT_SHIFT) |
		((depth > 1 ? 3UL : 2UL) << D3DFORMAT_DIMENSION_SHIFT) | (levels << D3DFORMAT_MIPMAP_SHIFT);
}

/* a swizzled or compressed texture of random texels, all its levels */
static void random_texture(DWORD format, unsigned long width, unsigned long height, unsigned long depth, int shape)
{
	unsigned long largest = width > height ? width : height;
	unsigned long levels = floor_log2(depth > largest ? depth : largest) + 1;
	struct xgpu_texture_description description;
	unsigned long size, level;
	unsigned char *data;

	xgpu_texture_describe(swizzled_format_word(format, width, height, depth, levels), 0, &description);
	size = xgpu_texture_face_size(&description);
	data = malloc(size + 16);
	random_bytes(data, size + 16, shape);
	for (level = 0; level < description.levels; level++)
	{
		const unsigned char *source = data + xgpu_texture_level_offset(&description, level);

		if (description.compressed)
		{
			struct format_information information = format_information(format);
			unsigned long w = level_dimension(width, level), h = level_dimension(height, level);
			unsigned long d = level_dimension(depth, level);
			unsigned long *reference = malloc(w * h * d * sizeof(unsigned long));

			reference_dxt_level(information.kind, source, w, h, d, reference);
			check_level(&description, level, source, random_palette, reference, "random");
			free(reference);
		}
		else
		{
			check_level(&description, level, source, random_palette, NULL, "random");
		}
	}
	free(data);
}

static void random_linear_texture(DWORD format, unsigned long width, unsigned long height, int shape)
{
	struct format_information information = format_information(format);
	unsigned long pitch = (width * information.bytes + D3DTEXTURE_PITCH_ALIGNMENT - 1) / D3DTEXTURE_PITCH_ALIGNMENT *
		D3DTEXTURE_PITCH_ALIGNMENT;
	DWORD size_word = (width - 1) | ((height - 1) << D3DSIZE_HEIGHT_SHIFT) |
		((pitch / D3DTEXTURE_PITCH_ALIGNMENT - 1) << D3DSIZE_PITCH_SHIFT);
	struct xgpu_texture_description description;
	unsigned char *data = malloc(pitch * height + 16);

	xgpu_texture_describe((format << D3DFORMAT_FORMAT_SHIFT) | (2 << D3DFORMAT_DIMENSION_SHIFT) |
		(1 << D3DFORMAT_MIPMAP_SHIFT), size_word, &description);
	random_bytes(data, pitch * height + 16, shape);
	if ((information.kind == _texel_yuy2 || information.kind == _texel_uyvy) && (width & 1))
	{
		/* the last texel of an odd row is half a pair. The old code read the
		pair's V from past the row's bytes, the converter takes 128: put that
		there for the two to agree. */
		unsigned long row;

		for (row = 0; row < height; row++)
			data[row * pitch + (width - 1) * 2 + (information.kind == _texel_yuy2 ? 3 : 2)] = 128;
	}
	check_level(&description, 0, data, random_palette, NULL, "random linear");
	free(data);
}

/* DXT of any size, which only the decoders themselves can be asked for
(a texture header holds powers of two) */
static void random_dxt(int kind, unsigned long width, unsigned long height, unsigned long depth, int shape)
{
	unsigned long block_bytes = kind == _texel_dxt1 ? 8 : 16;
	unsigned long blocks = ((width + 3) / 4) * ((height + 3) / 4) * depth;
	unsigned long count = width * height * depth;
	unsigned char *data = malloc(blocks * block_bytes);
	unsigned long *old = malloc(count * sizeof(unsigned long));
	unsigned long *reference = malloc(count * sizeof(unsigned long));
	unsigned char *converted = malloc((width + GUARD_TEXELS) * height * depth * 4);
	unsigned long index, alpha;
	int destination;

	random_bytes(data, blocks * block_bytes, shape == 2 ? 0 : shape);
	for (index = 0; index < blocks; index++)
	{
		unsigned char *block = data + index * block_bytes;
		unsigned char *color_block = kind == _texel_dxt1 ? block : block + 8;
		unsigned long long choice = test_random();

		/* equal colors, ordered either way; DXT5 alpha ends equal, 255 and
		ordered either way; opaque DXT3 blocks */
		if ((choice & 7) == 0)
			memcpy(color_block + 2, color_block, 2);
		else if ((choice & 7) == 1 || shape == 2)
		{
			unsigned int c0 = color_block[0] | (color_block[1] << 8), c1 = color_block[2] | (color_block[3] << 8);

			if (c0 < c1 || (shape == 2 && c0 == c1))
			{
				c0 = c0 == c1 ? c0 ^ 1 : c0;
				color_block[0] = (unsigned char)(c1 > c0 ? c1 : c0);
				color_block[1] = (unsigned char)((c1 > c0 ? c1 : c0) >> 8);
				color_block[2] = (unsigned char)(c1 > c0 ? c0 : c1);
				color_block[3] = (unsigned char)((c1 > c0 ? c0 : c1) >> 8);
			}
		}
		if (kind == _texel_dxt5)
		{
			if (((choice >> 8) & 7) == 0)
				block[1] = block[0];
			else if (((choice >> 8) & 7) == 1 || shape == 2)
				block[0] = block[1] = 255;
			if (shape == 2)
				memset(block + 2, 0, 6);
		}
		if (kind == _texel_dxt3 && ((((choice >> 8) & 3) == 0) || shape == 2))
			memset(block, 255, 8);
	}

	dxt_decode_level((unsigned char)kind, data, width, height, depth, old);
	reference_dxt_level(kind, data, width, height, depth, reference);
	if (memcmp(old, reference, count * sizeof(unsigned long)))
		fail("dxt kind %d %lux%lux%lu: the old decoder and the reference differ", kind, width, height, depth);
	for (destination = _texture_destination_bgra8; destination <= _texture_destination_rgb5a1; destination++)
	{
		unsigned long bytes = texture_convert_destination_bytes(destination);
		unsigned long pitch = (width + GUARD_TEXELS) * bytes;
		unsigned long row, x, bad = 0;

		memset(converted, GUARD_BYTE, pitch * height * depth);
		texture_convert_dxt_level(kind, destination, converted, pitch, data, width, height, depth);
		for (row = 0; row < height * depth && !bad; row++)
		{
			const unsigned char *texels = converted + row * pitch;

			for (x = 0; x < width && !bad; x++)
			{
				unsigned int want = reference_pack(destination, (unsigned int)reference[row * width + x]);
				unsigned int got = read_texel(texels + x * bytes, bytes);

				if (got != want)
				{
					fail("dxt kind %d %lux%lux%lu destination %d: texel %lu,%lu is %08x, not %08x", kind, width, height,
						depth, destination, x, row, got, want);
					bad = 1;
				}
			}
			for (x = width * bytes; x < pitch && !bad; x++)
			{
				if (texels[x] != GUARD_BYTE)
				{
					fail("dxt kind %d %lux%lux%lu destination %d: wrote past row %lu", kind, width, height, depth,
						destination, row);
					bad = 1;
				}
			}
		}
		test_checks++;
	}
	alpha = new_alpha(kind, data, width, height, depth, 0, NULL);
	if (alpha != reference_alpha(reference, count))
		fail("dxt kind %d %lux%lux%lu: alpha scan %lu, texels say %lu", kind, width, height, depth, alpha,
			reference_alpha(reference, count));
	test_checks++;
	levels_checked++;
	texels_checked += count;
	free(converted);
	free(reference);
	free(old);
	free(data);
}

static void test_random_textures(void)
{
	static const unsigned long linear_widths[] = { 1, 2, 3, 5, 17, 63, 64, 65, 100, 255, 256, 257, 320, 640 };
	static const unsigned long linear_heights[] = { 1, 2, 3, 7, 30 };
	static const unsigned long dxt_sizes[][3] =
	{
		{ 32, 32, 1 }, { 64, 16, 1 }, { 13, 64, 1 }, { 128, 128, 1 }, { 256, 256, 1 }, { 100, 60, 1 }, { 16, 16, 4 },
		{ 9, 7, 3 }, { 1, 1, 2 },
	};
	DWORD format;
	unsigned long width, height, depth, index;
	int shape;

	for (shape = 0; shape < 3; shape++)
	{
		unsigned long before = texels_checked, levels_before = levels_checked;

		random_palettes(shape);
		for (format = 0; format < 0x42; format++)
		{

			struct format_information information = format_information(format);

			/* the table's own entries only */
			if (format != 0x06 && information.kind == _texel_a8r8g8b8 && format != 0x12)
				continue;

			if (kind_compressed(information.kind))
			{
				for (width = 1; width <= 64; width <<= 1)
				{
					for (height = 1; height <= 64; height <<= 1)
						random_texture(format, width, height, 1, shape);
				}
				random_texture(format, 256, 256, 1, shape);
				random_texture(format, 16, 8, 4, shape);
				random_texture(format, 4, 4, 4, shape);
			}
			else if (information.linear)
			{
				for (width = 0; width < sizeof(linear_widths) / sizeof(linear_widths[0]); width++)
				{
					for (height = 0; height < sizeof(linear_heights) / sizeof(linear_heights[0]); height++)
						random_linear_texture(format, linear_widths[width], linear_heights[height], shape);
				}
				random_linear_texture(format, 640, 480, shape);
			}
			else if (information.kind != _texel_yuy2 && information.kind != _texel_uyvy)
			{
				for (width = 1; width <= 256; width <<= 1)
				{
					for (height = 1; height <= 256; height <<= 1)
						random_texture(format, width, height, 1, shape);
				}
				for (depth = 2; depth <= 16; depth <<= 1)
				{
					for (width = 1; width <= 32; width <<= 1)
					{
						for (height = 1; height <= 32; height <<= 1)
							random_texture(format, width, height, depth, shape);
					}
				}
				random_texture(format, 2048, 2, 1, shape);
				random_texture(format, 2, 2048, 1, shape);
				random_texture(format, 1024, 512, 1, shape);
				random_texture(format, 64, 64, 64, shape);
				random_texture(format, 2, 4, 128, shape);
			}
		}
		for (index = _texel_dxt1; index <= _texel_dxt5; index++)
		{
			for (width = 1; width <= 21; width++)
			{
				for (height = 1; height <= 21; height++)
					random_dxt((int)index, width, height, 1, shape);
			}
			for (width = 0; width < sizeof(dxt_sizes) / sizeof(dxt_sizes[0]); width++)
				random_dxt((int)index, dxt_sizes[width][0], dxt_sizes[width][1], dxt_sizes[width][2], shape);
		}
		printf("# random texels, shape %d: %lu levels, %lu texels\n", shape, levels_checked - levels_before,
			texels_checked - before);
	}
}

/* every 16-bit value and every byte through the converters, which random
data does not promise */
static void test_exhaustive(void)
{
	static const DWORD formats16[] = { 0x02, 0x03, 0x04, 0x05, 0x1a, 0x27, 0x28, 0x29, 0x2c, 0x32, 0x38, 0x39 };
	static unsigned char data[65536 * 2];
	unsigned long index, value;

	for (index = 0; index < sizeof(formats16) / sizeof(formats16[0]); index++)
	{
		struct xgpu_texture_description description;
		struct swizzle_masks masks = swizzle_masks(256, 256, 1);

		/* texel x,y holds the value y * 256 + x */
		for (value = 0; value < 65536; value++)
		{
			unsigned long offset = (spread(masks.x, value & 255) | spread(masks.y, value >> 8)) * 2;

			data[offset] = (unsigned char)value;
			data[offset + 1] = (unsigned char)(value >> 8);
		}
		xgpu_texture_describe(swizzled_format_word(formats16[index], 256, 256, 1, 1), 0, &description);
		check_level(&description, 0, data, NULL, NULL, "every value");
	}
	for (value = 0; value < 256; value++)
	{
		CHECK(reference_pack(_texture_destination_r5g6b5, (unsigned int)value * 0x010101) ==
			((nearest((unsigned int)value, 31) * 0x0801) | (nearest((unsigned int)value, 63) << 5)));
	}
	printf("# every 16-bit value: %lu formats\n", (unsigned long)(sizeof(formats16) / sizeof(formats16[0])));
}

/* ---------- padding, dropped levels, the box filter */

static unsigned int channel_of(int destination, unsigned int texel, int channel)
{
	switch (destination)
	{
	case _texture_destination_bgra8:
	case _texture_destination_rgba8:
		return (texel >> (channel * 8)) & 0xff;
	case _texture_destination_r5g6b5:
		return channel == 0 ? texel & 31 : channel == 1 ? (texel >> 5) & 63 : channel == 2 ? texel >> 11 : 0;
	case _texture_destination_rgba4:
		return (texel >> (channel * 4)) & 15;
	case _texture_destination_rgb5a1:
		return channel == 0 ? texel & 1 : (texel >> (1 + (channel - 1) * 5)) & 31;
	case _texture_destination_y8:
		return channel == 0 ? texel : 0;
	default:
		return channel < 2 ? (texel >> (channel * 8)) & 0xff : 0;
	}
}

static void test_sizes(void)
{
	unsigned char rows[4][64 * 4], halved[64 * 4 + 8];
	unsigned long width, padded, bytes, index;
	int destination, volume, single_row;
	float scale[2];

	CHECK(texture_convert_power_of_two(1) == 1);
	CHECK(texture_convert_power_of_two(2) == 2);
	CHECK(texture_convert_power_of_two(3) == 4);
	CHECK(texture_convert_power_of_two(640) == 1024);
	CHECK(texture_convert_power_of_two(480) == 512);
	CHECK(texture_convert_power_of_two(1024) == 1024);
	CHECK(texture_convert_power_of_two(1025) == 2048);

	texture_convert_pad_scale(640, 480, 1024, 512, 1, scale);
	CHECK(scale[0] == 1.0f / 1024.0f && scale[1] == 1.0f / 512.0f);
	texture_convert_pad_scale(640, 480, 1024, 512, 0, scale);
	CHECK(scale[0] == 0.625f && scale[1] == 0.9375f);

	CHECK(texture_convert_levels_to_drop(256, 256, 1, 1024) == 0);
	CHECK(texture_convert_levels_to_drop(1024, 1024, 1, 1024) == 0);
	CHECK(texture_convert_levels_to_drop(2048, 16, 1, 1024) == 1);
	CHECK(texture_convert_levels_to_drop(16, 4096, 1, 1024) == 2);
	CHECK(texture_convert_levels_to_drop(64, 64, 128, 32) == 2);
	CHECK(texture_convert_levels_to_drop(640, 480, 1, 256) == 2);

	for (bytes = 1; bytes <= 4; bytes <<= 1)
	{
		for (width = 1; width <= 9; width++)
		{
			for (padded = width; padded <= 16; padded++)
			{
				unsigned char image[4][16 * 4 + 4];
				int good = 1;

				memset(image, GUARD_BYTE, sizeof(image));
				random_bytes(image[0], width * bytes, 0);
				random_bytes(image[1], width * bytes, 0);
				texture_convert_pad_row(image[0], width, padded, bytes);
				texture_convert_pad_row(image[1], width, padded, bytes);
				texture_convert_pad_rows(image, sizeof(image[0]), 2, 4);
				for (index = width; index < padded; index++)
				{
					good &= !memcmp(image[0] + index * bytes, image[0] + (width - 1) * bytes, bytes);
					good &= !memcmp(image[1] + index * bytes, image[1] + (width - 1) * bytes, bytes);
				}
				good &= image[0][padded * bytes] == GUARD_BYTE && image[1][padded * bytes] == GUARD_BYTE;
				good &= !memcmp(image[2], image[1], sizeof(image[0])) && !memcmp(image[3], image[1], sizeof(image[0]));
				CHECK(good);
			}
		}
	}

	for (destination = 0; destination < NUMBER_OF_TEXTURE_DESTINATIONS; destination++)
	{
		bytes = texture_convert_destination_bytes(destination);
		for (volume = 0; volume < 2; volume++)
		{
			for (single_row = 0; single_row < 2; single_row++)
			{
				for (width = 1; width <= 64; width++)
				{
					unsigned long count = width > 1 ? width / 2 : 1, columns = width > 1 ? 2 : 1;
					unsigned long row_count = volume ? 4 : 2;
					const unsigned char *used[4];
					int good = 1, row;

					for (row = 0; row < 4; row++)
						random_bytes(rows[row], width * bytes, (int)(width % 3));
					used[0] = rows[0];
					used[1] = single_row ? rows[0] : rows[1];
					used[2] = rows[2];
					used[3] = single_row ? rows[2] : rows[3];
					memset(halved, GUARD_BYTE, sizeof(halved));
					texture_convert_halve_row(destination, halved, used[0], used[1], volume ? used[2] : NULL,
						volume ? used[3] : NULL, width);
					for (index = 0; index < count; index++)
					{
						unsigned int got = read_texel(halved + index * bytes, bytes);
						int channel;

						for (channel = 0; channel < 4; channel++)
						{
							unsigned long sum = 0, samples = row_count * columns, column;

							for (row = 0; row < (int)row_count; row++)
							{
								for (column = 0; column < columns; column++)
									sum += channel_of(destination, read_texel(used[row] + (index * columns + column) * bytes,
										bytes), channel);
							}
							good &= channel_of(destination, got, channel) == (sum * 2 + samples) / (samples * 2);
						}
					}
					good &= halved[count * bytes] == GUARD_BYTE;
					if (!good)
						fail("box filter: destination %d width %lu volume %d single row %d", destination, width, volume,
							single_row);
					test_checks++;
				}
			}
		}
	}
	printf("# padding, dropped levels and the box filter checked\n");
}

/* ---------- maps */

static void test_map(const char *path)
{
	static const char *const alpha_names[4] = { "opaque", "one bit", "", "full" };
	FILE *file = fopen(path, "rb");
	unsigned long bitmaps = 0, skipped = 0, short_data = 0, levels_before = levels_checked;
	unsigned long texels_before = texels_checked;
	unsigned long by_alpha[0x42][4];
	unsigned long by_format[0x42];
	unsigned char *data;
	long size, position = 0;
	DWORD format;

	memset(by_alpha, 0, sizeof(by_alpha));
	memset(by_format, 0, sizeof(by_format));
	if (!file)
	{
		fail("%s: cannot open", path);
		return;
	}
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	data = malloc((size_t)size + 64);
	if (fread(data, 1, (size_t)size, file) != (size_t)size)
		fail("%s: cannot read", path);
	fclose(file);
	memset(data + size, 0, 64);

	while (position + (long)sizeof(struct dump_header) <= size)
	{
		const struct dump_header *bitmap = (const struct dump_header *)(data + position);
		const unsigned char *pixels = data + position + sizeof(struct dump_header);
		struct xgpu_texture_description description;
		DWORD format_word, size_word;
		unsigned long faces, face, level, alpha = _texture_alpha_opaque;

		if (memcmp(bitmap->signature, "bmp1", 4))
		{
			fail("%s: not a bitmap dump at %ld", path, position);
			break;
		}
		position += (long)sizeof(struct dump_header) + (long)bitmap->pixels_size;
		if (!bitmap_header(bitmap, &format_word, &size_word))
		{
			skipped++;
			continue;
		}
		xgpu_texture_describe(format_word, size_word, &description);
		faces = description.cube_map ? 6 : 1;
		if (xgpu_texture_face_size(&description) * (faces - 1) + xgpu_texture_level_offset(&description,
			description.levels) > bitmap->pixels_size)
		{
			short_data++;
			continue;
		}
		for (face = 0; face < faces; face++)
		{
			for (level = 0; level < description.levels; level++)
			{
				alpha |= check_level(&description, level, pixels + face * xgpu_texture_face_size(&description) +
					xgpu_texture_level_offset(&description, level), random_palette, NULL, path);
			}
		}
		by_format[description.format]++;
		by_alpha[description.format][alpha]++;
		bitmaps++;
	}
	free(data);
	printf("# %s: %lu bitmaps, %lu levels, %lu texels through both paths; %lu without a header, %lu short of data\n",
		path, bitmaps, levels_checked - levels_before, texels_checked - texels_before, skipped, short_data);
	for (format = 0; format < 0x42; format++)
	{
		if (by_format[format])
		{
			printf("#     format %02lx: %4lu bitmaps; alpha %s %lu, %s %lu, %s %lu\n", (unsigned long)format,
				by_format[format], alpha_names[0], by_alpha[format][0], alpha_names[1], by_alpha[format][1],
				alpha_names[3], by_alpha[format][3]);
		}
	}
}

/* ---------- speed */

static unsigned long long cycles(void)
{
	return __builtin_ia32_rdtsc();
}

static double cycles_per_second(void)
{
	clock_t start = clock(), now;
	unsigned long long first;

	while ((now = clock()) == start)
		;
	start = now;
	first = cycles();
	while ((now = clock()) - start < CLOCKS_PER_SEC / 4)
		;
	return (double)(cycles() - first) * CLOCKS_PER_SEC / (double)(now - start);
}

struct bench_case
{
	const char *name;
	DWORD format;
	unsigned long width, height, depth;
	BOOL linear;
};

#define BENCH_RUNS 9

static double bench_frequency;

static void bench_report(const char *name, const char *path, unsigned long long best, unsigned long texels)
{
	double per_texel = (double)best / (double)texels;

	printf("# %-34s %-22s %7.2f cycles/texel %8.1f million texels/s\n", name, path, per_texel,
		bench_frequency / per_texel / 1e6);
}

static void bench(const struct bench_case *bench_case)
{
	struct format_information information = format_information(bench_case->format);
	struct xgpu_texture_description description;
	unsigned long texels = bench_case->width * bench_case->height * bench_case->depth;
	unsigned long *old = malloc(texels * sizeof(unsigned long));
	unsigned char *converted = malloc(texels * 4);
	unsigned long long best, start, elapsed;
	unsigned char *data;
	unsigned long size;
	volatile unsigned long alpha = 0;
	int destination, run;

	if (bench_case->linear)
	{
		unsigned long pitch = bench_case->width * information.bytes;

		xgpu_texture_describe((bench_case->format << D3DFORMAT_FORMAT_SHIFT) | (2 << D3DFORMAT_DIMENSION_SHIFT) |
			(1 << D3DFORMAT_MIPMAP_SHIFT), (bench_case->width - 1) | ((bench_case->height - 1) << D3DSIZE_HEIGHT_SHIFT) |
			((pitch / D3DTEXTURE_PITCH_ALIGNMENT - 1) << D3DSIZE_PITCH_SHIFT), &description);
	}
	else
	{
		xgpu_texture_describe(swizzled_format_word(bench_case->format, bench_case->width, bench_case->height,
			bench_case->depth, 1), 0, &description);
	}
	size = xgpu_texture_face_size(&description);
	data = malloc(size + 16);
	random_bytes(data, size + 16, 0);

	best = ~0ULL;
	for (run = 0; run < BENCH_RUNS; run++)
	{
		start = cycles();
		old_level(&description, 0, data, random_palette, old);
		elapsed = cycles() - start;
		if (elapsed < best)
			best = elapsed;
	}
	bench_report(bench_case->name, "old, to BGRA8", best, texels);

	for (destination = _texture_destination_bgra8; destination <= _texture_destination_rgb5a1; destination++)
	{
		static const char *const names[] = { "new, to BGRA8", "new, to RGBA8", "new, to R5G6B5", "new, to RGBA4",
			"new, to RGB5A1" };
		unsigned long pitch = bench_case->width * texture_convert_destination_bytes(destination);

		best = ~0ULL;
		for (run = 0; run < BENCH_RUNS; run++)
		{
			start = cycles();
			new_level(information.kind, destination, converted, pitch, data, bench_case->width, bench_case->height,
				bench_case->depth, bench_case->linear ? description.pitch : 0, random_palette);
			elapsed = cycles() - start;
			if (elapsed < best)
				best = elapsed;
		}
		bench_report(bench_case->name, names[destination], best, texels);
	}

	best = ~0ULL;
	for (run = 0; run < BENCH_RUNS; run++)
	{
		/* opaque texels, so that the scan reads them all */
		start = cycles();
		alpha |= new_alpha(information.kind, data, bench_case->width, bench_case->height, bench_case->depth,
			bench_case->linear ? description.pitch : 0, random_palette);
		elapsed = cycles() - start;
		if (elapsed < best)
			best = elapsed;
	}
	bench_report(bench_case->name, "new, alpha scan", best, texels);
	free(data);
	free(converted);
	free(old);
}

static void test_speed(void)
{
	static const struct bench_case cases[] =
	{
		{ "A8R8G8B8 512x512 swizzled", 0x06, 512, 512, 1, FALSE },
		{ "X8R8G8B8 512x512 swizzled", 0x07, 512, 512, 1, FALSE },
		{ "R5G6B5 512x512 swizzled", 0x05, 512, 512, 1, FALSE },
		{ "A1R5G5B5 256x256 swizzled", 0x02, 256, 256, 1, FALSE },
		{ "A4R4G4B4 256x256 swizzled", 0x04, 256, 256, 1, FALSE },
		{ "AL8 256x256 swizzled", 0x01, 256, 256, 1, FALSE },
		{ "A8L8 256x256 swizzled", 0x1a, 256, 256, 1, FALSE },
		{ "P8 256x256 swizzled", 0x0b, 256, 256, 1, FALSE },
		{ "A8R8G8B8 64x64 swizzled", 0x06, 64, 64, 1, FALSE },
		{ "X8R8G8B8 64x64x64 swizzled", 0x07, 64, 64, 64, FALSE },
		{ "A8R8G8B8 640x480 linear", 0x12, 640, 480, 1, TRUE },
		{ "A4R4G4B4 640x480 linear", 0x1d, 640, 480, 1, TRUE },
		{ "DXT1 512x512", 0x0c, 512, 512, 1, FALSE },
		{ "DXT3 512x512", 0x0e, 512, 512, 1, FALSE },
		{ "DXT5 512x512", 0x0f, 512, 512, 1, FALSE },
		{ "DXT1 64x64", 0x0c, 64, 64, 1, FALSE },
	};
	unsigned long index;

	bench_frequency = cycles_per_second();
	printf("# time stamp counter: %.0f MHz; best of %d runs each\n", bench_frequency / 1e6, BENCH_RUNS);
	random_palettes(0);
	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++)
		bench(&cases[index]);
}

/* the bitmaps of maps, which are not random: every level of every bitmap
by the old code, by the new code to BGRA, and by the new code as the light
renderer would upload it (the alpha scan, then the 16-bit destination it
chooses). The best of three passes of each bitmap. */
static void bench_maps(int count, char **paths)
{
	static const char *const names[3] = { "old, to BGRA8", "new, to BGRA8", "new, scan and to 16 bits" };
	static unsigned long long format_cycles[0x42][3];
	static unsigned long long format_texels[0x42];
	unsigned long long total_cycles[3] = { 0, 0, 0 }, total_texels = 0;
	DWORD format;
	int index, path;

	bench_frequency = cycles_per_second();
	random_palettes(0);
	for (index = 0; index < count; index++)
	{
		FILE *file = fopen(paths[index], "rb");
		unsigned char *data;
		long size, position = 0;

		if (!file)
			continue;
		fseek(file, 0, SEEK_END);
		size = ftell(file);
		fseek(file, 0, SEEK_SET);
		data = malloc((size_t)size + 64);
		size = (long)fread(data, 1, (size_t)size, file);
		fclose(file);
		while (position + (long)sizeof(struct dump_header) <= size)
		{
			const struct dump_header *bitmap = (const struct dump_header *)(data + position);
			const unsigned char *pixels = data + position + sizeof(struct dump_header);
			struct xgpu_texture_description description;
			struct format_information information;
			unsigned long long best[3] = { ~0ULL, ~0ULL, ~0ULL };
			DWORD format_word, size_word;
			unsigned long faces, face, level, texels = 0, source_pitch;
			unsigned long *old;
			int run;

			position += (long)sizeof(struct dump_header) + (long)bitmap->pixels_size;
			if (!bitmap_header(bitmap, &format_word, &size_word))
				continue;
			xgpu_texture_describe(format_word, size_word, &description);
			information = format_information(description.format);
			faces = description.cube_map ? 6 : 1;
			source_pitch = !description.compressed && description.linear ? description.pitch : 0;
			old = malloc(description.width * description.height * description.depth * sizeof(unsigned long));
			for (run = 0; run < 3; run++)
			{
				for (path = 0; path < 3; path++)
				{
					unsigned long long start = cycles();
					unsigned long alpha = _texture_alpha_opaque;
					int destination = _texture_destination_bgra8;

					if (path == 2)
					{
						for (face = 0; face < faces; face++)
						{
							for (level = 0; level < description.levels; level++)
							{
								alpha |= new_alpha(information.kind, pixels + face * xgpu_texture_face_size(&description) +
									xgpu_texture_level_offset(&description, level), level_dimension(description.width, level),
									level_dimension(description.height, level), level_dimension(description.depth, level),
									source_pitch, random_palette);
							}
						}
						destination = texture_convert_destination_16bit(alpha);
					}
					texels = 0;
					for (face = 0; face < faces; face++)
					{
						for (level = 0; level < description.levels; level++)
						{
							const unsigned char *source = pixels + face * xgpu_texture_face_size(&description) +
								xgpu_texture_level_offset(&description, level);
							unsigned long width = level_dimension(description.width, level);
							unsigned long height = level_dimension(description.height, level);
							unsigned long depth = level_dimension(description.depth, level);

							if (path == 0)
								old_level(&description, level, source, random_palette, old);
							else
								new_level(information.kind, destination, (unsigned char *)old,
									width * texture_convert_destination_bytes(destination), source, width, height, depth,
									source_pitch, random_palette);
							texels += width * height * depth;
						}
					}
					start = cycles() - start;
					if (start < best[path])
						best[path] = start;
				}
			}
			for (path = 0; path < 3; path++)
				format_cycles[description.format][path] += best[path];
			format_texels[description.format] += texels;
			free(old);
		}
		free(data);
	}
	printf("# time stamp counter: %.0f MHz; %d maps\n", bench_frequency / 1e6, count);
	for (format = 0; format < 0x42; format++)
	{
		char name[32];

		if (!format_texels[format])
			continue;
		sprintf(name, "format %02lx, %llu texels", (unsigned long)format, format_texels[format]);
		for (path = 0; path < 3; path++)
		{
			printf("# %-34s %-26s %7.2f cycles/texel\n", name, names[path],
				(double)format_cycles[format][path] / (double)format_texels[format]);
			total_cycles[path] += format_cycles[format][path];
		}
		total_texels += format_texels[format];
	}
	for (path = 0; path < 3; path++)
	{
		printf("# all formats, %llu texels: %-26s %7.2f cycles/texel, %.1f million texels/s, %.2f s\n", total_texels,
			names[path], (double)total_cycles[path] / (double)total_texels,
			bench_frequency / ((double)total_cycles[path] / (double)total_texels) / 1e6,
			(double)total_cycles[path] / bench_frequency);
	}
}

/* ---------- main */

int main(int argc, char **argv)
{
	int index;

	if (argc > 1 && !strcmp(argv[1], "--bench"))
	{
		if (argc > 2)
			bench_maps(argc - 2, argv + 2);
		else
			test_speed();
		return 0;
	}
	if (argc > 1 && !strcmp(argv[1], "--maps"))
	{
		random_palettes(0);
		for (index = 2; index < argc; index++)
			test_map(argv[index]);
		return test_finish("t_textures --maps");
	}
	test_formats();
	test_swizzle();
	test_exhaustive();
	test_random_textures();
	test_sizes();
	printf("# %lu levels, %lu texels in all\n", levels_checked, texels_checked);
	return test_finish("t_textures");
}
