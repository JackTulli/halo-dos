/*
XBOX_TEXTURES_CONVERT.H

Texel conversion for the texture upload (xbox_textures.c): deswizzling,
every Xbox texel format into the layouts OpenGL takes, DXT decoding,
padding to a power of two, mip levels dropped or made. No OpenGL in here
and no state: every function works on the rows it is given, so an upload
can run them on a worker thread.

Pixels are 32-bit words (unsigned int) or 16-bit words in the processor's
byte order, which is little endian on everything this builds for.
*/

#ifndef __HALO_LINUX_XBOX_TEXTURES_CONVERT_H
#define __HALO_LINUX_XBOX_TEXTURES_CONVERT_H

/* ---------- formats */

/* what a source texel is. The values are those of xbox_textures.c's
texel kinds, in its order. */
enum
{
	_texture_kind_unknown,
	_texture_kind_a8r8g8b8, _texture_kind_x8r8g8b8, _texture_kind_r5g6b5, _texture_kind_a1r5g5b5,
	_texture_kind_x1r5g5b5, _texture_kind_a4r4g4b4,
	_texture_kind_l8, _texture_kind_al8, _texture_kind_a8, _texture_kind_a8l8, _texture_kind_p8,
	_texture_kind_g8b8, _texture_kind_r8b8, _texture_kind_r6g5b5,
	_texture_kind_l16, _texture_kind_v16u16, _texture_kind_a8b8g8r8, _texture_kind_b8g8r8a8,
	_texture_kind_r8g8b8a8, _texture_kind_r5g5b5a1,
	_texture_kind_r4g4b4a4, _texture_kind_yuy2, _texture_kind_uyvy, _texture_kind_d24s8, _texture_kind_d16,
	_texture_kind_dxt1, _texture_kind_dxt3, _texture_kind_dxt5,
	NUMBER_OF_TEXTURE_KINDS
};

/* what OpenGL is given */
enum
{
	/* GL_BGRA, GL_UNSIGNED_BYTE: 32-bit ARGB words (EXT_bgra, OpenGL 1.2) */
	_texture_destination_bgra8,
	/* GL_RGBA, GL_UNSIGNED_BYTE: OpenGL 1.1 without EXT_bgra, OpenGL ES; also
	what a 16-bit internal format is fed without packed pixels */
	_texture_destination_rgba8,
	/* GL_RGB, GL_UNSIGNED_SHORT_5_6_5 (OpenGL 1.2; EXT_packed_pixels has no
	such type: there, opaque textures go as _texture_destination_rgb5a1) */
	_texture_destination_r5g6b5,
	/* GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4 (EXT_packed_pixels, OpenGL 1.2) */
	_texture_destination_rgba4,
	/* GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1 (EXT_packed_pixels, OpenGL 1.2) */
	_texture_destination_rgb5a1,
	/* GL_LUMINANCE, GL_INTENSITY or GL_ALPHA, GL_UNSIGNED_BYTE: the byte of an
	L8, AL8 or A8 texel as it is */
	_texture_destination_y8,
	/* GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE: an A8L8 texel as it is */
	_texture_destination_y8a8,
	NUMBER_OF_TEXTURE_DESTINATIONS
};

/* what the alpha of a texture holds (the scans below): nothing but 255,
nothing but 0 and 255, or anything */
enum
{
	_texture_alpha_opaque = 0,
	_texture_alpha_one_bit = 1,
	_texture_alpha_full = 3
};

/* bytes of a texel in memory; of a 4x4 block for DXT */
unsigned long texture_convert_kind_bytes(int kind);
unsigned long texture_convert_destination_bytes(int destination);
/* every kind converts to the 32-bit and 16-bit destinations; y8 and y8a8
take only the kinds they are named for */
int texture_convert_supported(int kind, int destination);
/* the most a texel of this kind can hold: _texture_alpha_opaque means a
scan is not needed */
unsigned long texture_convert_kind_alpha(int kind);
/* the 16-bit destination for what a scan found: R5G6B5 without alpha,
5551 for one bit, 4444 otherwise. Without GL_UNSIGNED_SHORT_5_6_5 the
caller takes 5551 for the first too. */
int texture_convert_destination_16bit(unsigned long alpha);

/* ---------- swizzled textures

A swizzled level is in Morton order: the bits of x, y and z alternate in a
texel's index until each dimension runs out. A texel is at
source + ((y_offset | z_offset) * bytes) + x_offsets[x], where the y and z
offsets are texture_convert_swizzle_offset() of their masks and the table
of x holds byte offsets. */

struct texture_convert_swizzle_masks
{
	unsigned long x, y, z;
};

struct texture_convert_swizzle_masks texture_convert_swizzle_masks(unsigned long width, unsigned long height,
	unsigned long depth);
/* the bits of value, laid on the bits of mask */
unsigned long texture_convert_swizzle_offset(unsigned long mask, unsigned long value);
/* offsets[n] = texture_convert_swizzle_offset(mask, n) * bytes, n < count */
void texture_convert_swizzle_table(unsigned long mask, unsigned long bytes, unsigned long count,
	unsigned long *offsets);

/* ---------- texels that are not compressed */

/* count texels of one row. offsets is the table of x of a swizzled level,
with source at the row's own offset; NULL for texels that follow each other
(a linear texture's row). palette is 256 ARGB words for P8, or NULL.
A row of texels that follow each other may be converted in place
(destination == source) when the destination's texel is not larger than
the source's; a swizzled row may not. YUY2 and UYVY rows are never
swizzled, and the last texel of an odd width takes its V as 128. */
void texture_convert_row(int kind, int destination_format, void *destination, const unsigned char *source,
	const unsigned long *offsets, unsigned long count, const void *palette);

/* a whole level, slices of a volume one after the other. source_pitch is
the bytes of a row of a linear texture, 0 for a swizzled one;
destination_pitch is in bytes, and may be wider than the level
(texture_convert_pad_row). */
void texture_convert_level(int kind, int destination_format, void *destination, unsigned long destination_pitch,
	const unsigned char *source, unsigned long width, unsigned long height, unsigned long depth,
	unsigned long source_pitch, const void *palette);

/* the alpha of count texels that follow each other (a whole swizzled
level, or a row of a linear one): _texture_alpha_*, to be or'ed over rows
and levels */
unsigned long texture_convert_alpha(int kind, const unsigned char *source, unsigned long count,
	const void *palette);

/* ---------- DXT

A level is rows of 4x4 blocks, of 8 bytes (DXT1) or 16; a volume's slices
follow each other. Colors are interpolated as the decoder this replaces
did: (2a + b) / 3 and (a + b) / 2, rounded down, on the 8-bit expansions
of the two 5:6:5 colors. A DXT1 block whose first color is not above its
second has three colors and a transparent black one. */

/* one row of blocks: up to four rows of width texels (rows is less than 4
at the bottom of a level whose height is not a multiple of 4, and for the
levels smaller than a block) */
void texture_convert_dxt_rows(int kind, int destination_format, void *destination, unsigned long destination_pitch,
	const unsigned char *blocks, unsigned long width, unsigned long rows);
void texture_convert_dxt_level(int kind, int destination_format, void *destination, unsigned long destination_pitch,
	const unsigned char *source, unsigned long width, unsigned long height, unsigned long depth);
/* the alpha of the texels of one row of blocks that are inside the
level: _texture_alpha_*. It decides the 16-bit destination before the
decoding. */
unsigned long texture_convert_dxt_alpha(int kind, const unsigned char *blocks, unsigned long width,
	unsigned long rows);

/* ---------- sizes OpenGL 1.1 takes */

/* the power of two that is not below value */
unsigned long texture_convert_power_of_two(unsigned long value);
/* fills texels count..padded_count - 1 of a row with texel count - 1 */
void texture_convert_pad_row(void *row, unsigned long count, unsigned long padded_count, unsigned long bytes);
/* fills rows rows..padded_rows - 1 of an image with row rows - 1 */
void texture_convert_pad_rows(void *image, unsigned long pitch, unsigned long rows, unsigned long padded_rows);
/* what the texture matrix multiplies coordinates by to address the image
inside its padding: 1 / padded size for coordinates in texels (linear
textures, render targets), size / padded size for coordinates in 0..1 */
void texture_convert_pad_scale(unsigned long width, unsigned long height, unsigned long padded_width,
	unsigned long padded_height, int coordinates_in_texels, float scale[2]);

/* the number of top levels to leave out for no dimension to be above
maximum_size. When it is not below the texture's level count, the levels
below the last are made with texture_convert_halve_row. */
unsigned long texture_convert_levels_to_drop(unsigned long width, unsigned long height, unsigned long depth,
	unsigned long maximum_size);
/* one row of the next smaller level, by box filter (rounded to nearest):
source_width / 2 texels, or 1. row1 is the row below row0, or row0 again
for a level one texel high; row2 and row3 are the same two rows of the
next slice of a volume, NULL otherwise. */
void texture_convert_halve_row(int destination_format, void *destination, const void *row0, const void *row1,
	const void *row2, const void *row3, unsigned long source_width);

#endif
