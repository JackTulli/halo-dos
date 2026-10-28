/*
XBOX_TEXTURES_UNDER_TEST.H

port/linux/src/xbox_textures.c compiled into a test program
(t_textures.c, t_texture_upload.c): the few names it takes from the device
as stubs, the file itself, and the texture header the game's texture cache
makes for a bitmap of a map (bitmap_dump.py writes the bitmaps).

The OpenGL stubs keep a running hash of every image they are given, which
is how two versions of the file's upload are compared.
*/

#ifndef __XBOX_TEXTURES_UNDER_TEST_H
#define __XBOX_TEXTURES_UNDER_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- xbox_textures.c, as it is

What it includes is replaced by the few names it uses: xgpu.h is kept out
by its guard. HALO_ANDROID is the variant of the file that needs the fewest
names, and has had the DXT decoder from the start. */

#define __HALO_LINUX_XGPU_H
#define HALO_ANDROID

typedef int BOOL;
typedef unsigned long DWORD;
typedef DWORD D3DCOLOR;
typedef unsigned int GLuint;
typedef unsigned int GLenum;
typedef int GLint;
typedef int GLsizei;
#define TRUE 1
#define FALSE 0

/* port/include/xdk/xdk_d3d8.h */
#define D3DFORMAT_CUBEMAP 0x00000004
#define D3DFORMAT_DIMENSION_MASK 0x000000F0
#define D3DFORMAT_DIMENSION_SHIFT 4
#define D3DFORMAT_FORMAT_MASK 0x0000FF00
#define D3DFORMAT_FORMAT_SHIFT 8
#define D3DFORMAT_MIPMAP_MASK 0x000F0000
#define D3DFORMAT_MIPMAP_SHIFT 16
#define D3DFORMAT_USIZE_MASK 0x00F00000
#define D3DFORMAT_USIZE_SHIFT 20
#define D3DFORMAT_VSIZE_MASK 0x0F000000
#define D3DFORMAT_VSIZE_SHIFT 24
#define D3DFORMAT_PSIZE_MASK 0xF0000000
#define D3DFORMAT_PSIZE_SHIFT 28
#define D3DSIZE_WIDTH_MASK 0x00000FFF
#define D3DSIZE_HEIGHT_MASK 0x00FFF000
#define D3DSIZE_HEIGHT_SHIFT 12
#define D3DSIZE_PITCH_MASK 0xFF000000
#define D3DSIZE_PITCH_SHIFT 24
#define D3DTEXTURE_CUBEFACE_ALIGNMENT 128
#define D3DTEXTURE_PITCH_ALIGNMENT 64

/* port/linux/src/xgpu.h */
struct xgpu_texture_description
{
	DWORD format;
	unsigned long width, height, depth, levels;
	BOOL cube_map;
	BOOL linear;
	BOOL compressed;
	unsigned long pitch;
};

static struct
{
	BOOL s3tc;
	BOOL s3tc_3d;
} xgpu_capabilities;

enum
{
	GL_RGBA, GL_RGBA8, GL_UNSIGNED_BYTE, GL_TEXTURE_2D, GL_TEXTURE_3D, GL_TEXTURE_CUBE_MAP,
	GL_TEXTURE_CUBE_MAP_POSITIVE_X, GL_TEXTURE_SWIZZLE_R, GL_TEXTURE_SWIZZLE_B, GL_RED, GL_BLUE,
	GL_UNPACK_ALIGNMENT, GL_TEXTURE_BASE_LEVEL, GL_TEXTURE_MAX_LEVEL,
};

/* the images the upload hands to OpenGL, hashed in the order they come */
static unsigned long long gl_image_hash = 1469598103934665603ULL;
static unsigned long gl_image_count;

static void gl_image(GLenum target, GLint level, GLint internal, GLsizei width, GLsizei height, GLsizei depth,
	const void *data, unsigned long bytes)
{
	const unsigned char *source = data;
	unsigned long long hash = gl_image_hash;
	unsigned long index;
	unsigned int header[6];

	header[0] = target; header[1] = (unsigned int)level; header[2] = (unsigned int)internal;
	header[3] = (unsigned int)width; header[4] = (unsigned int)height; header[5] = (unsigned int)depth;
	for (index = 0; index < 6; index++)
		hash = (hash ^ header[index]) * 1099511628211ULL;
	for (index = 0; index + 4 <= bytes; index += 4)
	{
		unsigned int word;

		memcpy(&word, source + index, 4);
		hash = (hash ^ word) * 1099511628211ULL;
	}
	for (; index < bytes; index++)
		hash = (hash ^ source[index]) * 1099511628211ULL;
	gl_image_hash = hash;
	gl_image_count++;
}

static void glBindTexture(GLenum target, GLuint texture) { (void)target; (void)texture; }
static void glTexParameteri(GLenum target, GLenum name, GLint value) { (void)target; (void)name; (void)value; }
static void glPixelStorei(GLenum name, GLint value) { (void)name; (void)value; }
static void glGenTextures(GLsizei count, GLuint *textures) { (void)count; *textures = 1; }
static void glDeleteTextures(GLsizei count, const GLuint *textures) { (void)count; (void)textures; }
static void glCompressedTexImage2D(GLenum target, GLint level, GLenum format, GLsizei width, GLsizei height,
	GLint border, GLsizei size, const void *data)
{
	(void)border;
	gl_image(target, level, (GLint)format, width, height, 1, data, (unsigned long)size);
}
static void glCompressedTexImage3D(GLenum target, GLint level, GLenum format, GLsizei width, GLsizei height,
	GLsizei depth, GLint border, GLsizei size, const void *data)
{
	(void)border;
	gl_image(target, level, (GLint)format, width, height, depth, data, (unsigned long)size);
}
static void glTexImage2D(GLenum target, GLint level, GLint internal, GLsizei width, GLsizei height, GLint border,
	GLenum format, GLenum type, const void *data)
{
	(void)border; (void)format; (void)type;
	gl_image(target, level, internal, width, height, 1, data, (unsigned long)width * (unsigned long)height * 4);
}
static void glTexImage3D(GLenum target, GLint level, GLint internal, GLsizei width, GLsizei height, GLsizei depth,
	GLint border, GLenum format, GLenum type, const void *data)
{
	(void)border; (void)format; (void)type;
	gl_image(target, level, internal, width, height, depth, data,
		(unsigned long)width * (unsigned long)height * (unsigned long)depth * 4);
}

static void xgpu_gl_state_invalidate(void) {}
static void xgpu_gl_texture_deleted(GLuint texture) { (void)texture; }
static unsigned long memory_watch_serial(void) { return 1; }
static unsigned long memory_watch_generation(unsigned long address, unsigned long size) { (void)address; (void)size; return 1; }
static void memory_watch_protect(unsigned long address, unsigned long size) { (void)address; (void)size; }
static BOOL platform_is_contiguous(const void *address) { (void)address; return TRUE; }
static void platform_log(const char *format, ...) { (void)format; }
#define PLATFORM_PHYSICAL_TO_VIRTUAL(physical) ((void *)(physical))

int config_boolean(const char *name) { (void)name; return 0; }
const char *config_string(const char *name) { (void)name; return ""; }

void xgpu_texture_describe(DWORD format_word, DWORD size_word, struct xgpu_texture_description *description);
unsigned long xgpu_texture_face_size(const struct xgpu_texture_description *description);
unsigned long xgpu_texture_level_offset(const struct xgpu_texture_description *description, unsigned long level);
unsigned long xgpu_texture_level_pitch(const struct xgpu_texture_description *description, unsigned long level);
GLuint xgpu_texture_get(const DWORD *resource, const D3DCOLOR *palette, GLenum *target,
	struct xgpu_texture_description *description);
void xgpu_texture_cache_begin_frame(void);

#ifndef XBOX_TEXTURES_C
#define XBOX_TEXTURES_C "../../../linux/src/xbox_textures.c"
#endif
#include XBOX_TEXTURES_C

/* ---------- the bitmaps of a map */

#pragma pack(push, 1)
struct dump_header
{
	char signature[4];
	unsigned short width, height, depth, type, format, flags, mipmap_count, pad;
	unsigned int pixels_offset, pixels_size, unused;
};
#pragma pack(pop)

/* source/bitmaps/bitmaps.c */
enum { _bitmap_type_2d, _bitmap_type_3d, _bitmap_type_cube_map };
enum
{
	_bitmap_has_power_of_two_dimensions_bit, _bitmap_compressed_bit, _bitmap_palettized_bit, _bitmap_swizzled_bit,
	_bitmap_linear_bit, _bitmap_v16u16_bit,
};
#define NONE (-1)

/* source/cache/xbox_texture_cache.c bitmap_d3d_format_tables */
static const int bitmap_formats[2][18] =
{
	{ 0x19, 0x00, 0x01, 0x1a, NONE, NONE, 0x05, NONE, 0x02, 0x04, 0x07, 0x06, NONE, NONE, 0x0c, 0x0e, 0x0f, 0x0b },
	{ 0x1f, 0x13, 0x1b, 0x20, NONE, NONE, 0x11, NONE, 0x10, 0x1d, 0x1e, 0x12, NONE, NONE, NONE, NONE, NONE, NONE },
};
static const unsigned char bitmap_format_bytes[18] = { 1, 1, 1, 2, 0, 0, 2, 0, 2, 2, 4, 4, 0, 0, 0, 0, 0, 1 };

static unsigned long maximum(unsigned long a, unsigned long b) { return a > b ? a : b; }

/* the texture header the game's texture cache makes for a bitmap
(texture_cache_initialize_hardware_format, with
rasterizer_xbox_bitmap_get_max_mipmap_count); FALSE if it would not */
static BOOL bitmap_header(const struct dump_header *bitmap, DWORD *format_word, DWORD *size_word)
{
	BOOL linear = (bitmap->flags >> _bitmap_linear_bit) & 1;
	int format;

	if (bitmap->format >= 18 || bitmap_formats[linear][bitmap->format] == NONE)
		return FALSE;
	format = bitmap_formats[linear][bitmap->format];
	if (((bitmap->flags >> _bitmap_v16u16_bit) & 1) && (bitmap->format == 10 || bitmap->format == 11))
		format = linear ? 0x36 : 0x33;
	if (linear)
	{
		unsigned long pitch = bitmap->width * bitmap_format_bytes[bitmap->format];

		if (pitch < D3DTEXTURE_PITCH_ALIGNMENT)
			return FALSE;
		*format_word = ((DWORD)format << D3DFORMAT_FORMAT_SHIFT) | (1 << D3DFORMAT_MIPMAP_SHIFT) |
			(2 << D3DFORMAT_DIMENSION_SHIFT);
		*size_word = ((pitch / D3DTEXTURE_PITCH_ALIGNMENT - 1) << D3DSIZE_PITCH_SHIFT) |
			((DWORD)(bitmap->height - 1) << D3DSIZE_HEIGHT_SHIFT) | (DWORD)(bitmap->width - 1);
	}
	else
	{
		BOOL compressed = (bitmap->flags >> _bitmap_compressed_bit) & 1;
		unsigned long levels = 0;

		if (bitmap->flags & (1 << _bitmap_has_power_of_two_dimensions_bit))
		{
			levels = floor_log2(compressed ? maximum(bitmap->width / 4, maximum(bitmap->height / 4, bitmap->depth)) :
				maximum(bitmap->width, maximum(bitmap->height, bitmap->depth)));
			if (levels > bitmap->mipmap_count)
				levels = bitmap->mipmap_count;
		}
		*format_word = (floor_log2(bitmap->depth) << D3DFORMAT_PSIZE_SHIFT) |
			(floor_log2(bitmap->height) << D3DFORMAT_VSIZE_SHIFT) | (floor_log2(bitmap->width) << D3DFORMAT_USIZE_SHIFT) |
			((DWORD)format << D3DFORMAT_FORMAT_SHIFT) |
			((bitmap->type == _bitmap_type_3d ? 3UL : 2UL) << D3DFORMAT_DIMENSION_SHIFT) |
			((levels + 1) << D3DFORMAT_MIPMAP_SHIFT) | (bitmap->type == _bitmap_type_cube_map ? D3DFORMAT_CUBEMAP : 0);
		*size_word = 0;
	}
	return TRUE;
}

#endif
