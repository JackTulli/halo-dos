/*
XD3D_RESOURCES.C

The Xbox's Direct3D resources for the Direct3D build (xd3d.h): creation,
locking, registration and release of textures, surfaces, vertex and index
buffers and palettes, and how a texture lies in memory. After
ChupathingyCE's port/linux/src/d3d8_resources.c and xbox_textures.c, with
plain pointers in Data: the game's memory is the Xbox's own window at the
same addresses, so a resource's Data is where its bytes are.
*/

#include "xd3d.h"
#include "xbox_textures_convert.h"

#include <stdlib.h>
#include <string.h>

/* ---------- memory: 128-byte aligned, as the Xbox's library allocates it */

static void *aligned_allocate(unsigned long size)
{
	unsigned char *raw = malloc(size + D3DTEXTURE_ALIGNMENT + sizeof(void *));
	unsigned char *aligned;

	if (!raw)
	{
		platform_log("Direct3D: out of memory for a %lu byte resource", size);
		return NULL;
	}
	aligned = (unsigned char *)(((unsigned long)raw + sizeof(void *) + D3DTEXTURE_ALIGNMENT - 1) &
		~(unsigned long)(D3DTEXTURE_ALIGNMENT - 1));
	((void **)aligned)[-1] = raw;
	memset(aligned, 0, size);
	return aligned;
}

static void aligned_free(void *memory)
{
	if (memory)
		free(((void **)memory)[-1]);
}

static DWORD serial;

DWORD xd3d_resource_serial(void)
{
	/* (never 0: a header nobody registered has 0) */
	if (!++serial)
		serial = 1;
	return serial;
}

/* ---------- formats (ChupathingyCE's xbox_textures.c) */

struct format_information
{
	unsigned char kind;
	unsigned char bytes; /* per texel; per 4x4 block for DXT */
	unsigned char linear;
};

static struct format_information format_information(DWORD format)
{
	static const struct format_information table[0x42] =
	{
		[0x00] = { _texture_kind_l8, 1, 0 },
		[0x01] = { _texture_kind_al8, 1, 0 },
		[0x02] = { _texture_kind_a1r5g5b5, 2, 0 },
		[0x03] = { _texture_kind_x1r5g5b5, 2, 0 },
		[0x04] = { _texture_kind_a4r4g4b4, 2, 0 },
		[0x05] = { _texture_kind_r5g6b5, 2, 0 },
		[0x06] = { _texture_kind_a8r8g8b8, 4, 0 },
		[0x07] = { _texture_kind_x8r8g8b8, 4, 0 },
		[0x0b] = { _texture_kind_p8, 1, 0 },
		[0x0c] = { _texture_kind_dxt1, 8, 0 },
		[0x0e] = { _texture_kind_dxt3, 16, 0 },
		[0x0f] = { _texture_kind_dxt5, 16, 0 },
		[0x10] = { _texture_kind_a1r5g5b5, 2, 1 },
		[0x11] = { _texture_kind_r5g6b5, 2, 1 },
		[0x12] = { _texture_kind_a8r8g8b8, 4, 1 },
		[0x13] = { _texture_kind_l8, 1, 1 },
		[0x16] = { _texture_kind_r8b8, 2, 1 },
		[0x17] = { _texture_kind_g8b8, 2, 1 },
		[0x19] = { _texture_kind_a8, 1, 0 },
		[0x1a] = { _texture_kind_a8l8, 2, 0 },
		[0x1b] = { _texture_kind_al8, 1, 1 },
		[0x1c] = { _texture_kind_x1r5g5b5, 2, 1 },
		[0x1d] = { _texture_kind_a4r4g4b4, 2, 1 },
		[0x1e] = { _texture_kind_x8r8g8b8, 4, 1 },
		[0x1f] = { _texture_kind_a8, 1, 1 },
		[0x20] = { _texture_kind_a8l8, 2, 1 },
		[0x24] = { _texture_kind_yuy2, 2, 1 },
		[0x25] = { _texture_kind_uyvy, 2, 1 },
		[0x27] = { _texture_kind_r6g5b5, 2, 0 },
		[0x28] = { _texture_kind_g8b8, 2, 0 },
		[0x29] = { _texture_kind_r8b8, 2, 0 },
		[0x2a] = { _texture_kind_d24s8, 4, 0 },
		[0x2b] = { _texture_kind_d24s8, 4, 0 },
		[0x2c] = { _texture_kind_d16, 2, 0 },
		[0x2d] = { _texture_kind_d16, 2, 0 },
		[0x2e] = { _texture_kind_d24s8, 4, 1 },
		[0x2f] = { _texture_kind_d24s8, 4, 1 },
		[0x30] = { _texture_kind_d16, 2, 1 },
		[0x31] = { _texture_kind_d16, 2, 1 },
		[0x32] = { _texture_kind_l16, 2, 0 },
		[0x33] = { _texture_kind_v16u16, 4, 0 },
		[0x35] = { _texture_kind_l16, 2, 1 },
		[0x36] = { _texture_kind_v16u16, 4, 1 },
		[0x37] = { _texture_kind_r6g5b5, 2, 1 },
		[0x38] = { _texture_kind_r5g5b5a1, 2, 0 },
		[0x39] = { _texture_kind_r4g4b4a4, 2, 0 },
		[0x3a] = { _texture_kind_a8b8g8r8, 4, 0 },
		[0x3b] = { _texture_kind_b8g8r8a8, 4, 0 },
		[0x3c] = { _texture_kind_r8g8b8a8, 4, 0 },
		[0x3d] = { _texture_kind_r5g5b5a1, 2, 1 },
		[0x3e] = { _texture_kind_r4g4b4a4, 2, 1 },
		[0x3f] = { _texture_kind_a8b8g8r8, 4, 1 },
		[0x40] = { _texture_kind_b8g8r8a8, 4, 1 },
		[0x41] = { _texture_kind_r8g8b8a8, 4, 1 },
	};
	struct format_information unknown = { _texture_kind_a8r8g8b8, 4, 0 };

	if (format < sizeof(table) / sizeof(table[0]) && table[format].kind != _texture_kind_unknown)
		return table[format];
	return unknown;
}

BOOL xd3d_format_is_depth(DWORD format)
{
	return format == D3DFMT_D24S8 || format == D3DFMT_F24S8 || format == D3DFMT_D16 || format == D3DFMT_F16 ||
		format == D3DFMT_LIN_D24S8 || format == D3DFMT_LIN_F24S8 || format == D3DFMT_LIN_D16 ||
		format == D3DFMT_LIN_F16;
}

static unsigned long floor_log2(unsigned long value)
{
	unsigned long result = 0;

	while (value > 1)
	{
		value >>= 1;
		result++;
	}
	return result;
}

static unsigned long level_dimension(unsigned long base, unsigned long level)
{
	unsigned long value = base >> level;

	return value ? value : 1;
}

void xd3d_texture_describe(DWORD format_word, DWORD size_word, struct xd3d_texture_description *description)
{
	struct format_information information;

	memset(description, 0, sizeof(*description));
	description->format = (format_word & D3DFORMAT_FORMAT_MASK) >> D3DFORMAT_FORMAT_SHIFT;
	information = format_information(description->format);
	description->kind = information.kind;
	description->bytes = information.bytes;
	description->cube_map = (format_word & D3DFORMAT_CUBEMAP) != 0;
	description->compressed = information.kind == _texture_kind_dxt1 || information.kind == _texture_kind_dxt3 ||
		information.kind == _texture_kind_dxt5;
	if (size_word)
	{
		description->width = (size_word & D3DSIZE_WIDTH_MASK) + 1;
		description->height = ((size_word & D3DSIZE_HEIGHT_MASK) >> D3DSIZE_HEIGHT_SHIFT) + 1;
		description->depth = 1;
		description->levels = 1;
		description->pitch = (((size_word & D3DSIZE_PITCH_MASK) >> D3DSIZE_PITCH_SHIFT) + 1) * D3DTEXTURE_PITCH_ALIGNMENT;
		description->linear = TRUE;
	}
	else
	{
		description->width = 1UL << ((format_word & D3DFORMAT_USIZE_MASK) >> D3DFORMAT_USIZE_SHIFT);
		description->height = 1UL << ((format_word & D3DFORMAT_VSIZE_MASK) >> D3DFORMAT_VSIZE_SHIFT);
		description->depth = 1UL << ((format_word & D3DFORMAT_PSIZE_MASK) >> D3DFORMAT_PSIZE_SHIFT);
		description->levels = (format_word & D3DFORMAT_MIPMAP_MASK) >> D3DFORMAT_MIPMAP_SHIFT;
		if (!description->levels)
			description->levels = 1;
		description->linear = information.linear;
		description->pitch = description->width * information.bytes;
	}
	if ((format_word & D3DFORMAT_DIMENSION_MASK) >> D3DFORMAT_DIMENSION_SHIFT != 3)
		description->depth = 1;
}

static unsigned long level_bytes(const struct xd3d_texture_description *description, unsigned long level)
{
	unsigned long width = level_dimension(description->width, level);
	unsigned long height = level_dimension(description->height, level);
	unsigned long depth = level_dimension(description->depth, level);

	if (description->compressed)
		return ((width + 3) / 4) * ((height + 3) / 4) * description->bytes * depth;
	if (description->linear)
		return description->pitch * height;
	return width * height * depth * description->bytes;
}

unsigned long xd3d_texture_level_offset(const struct xd3d_texture_description *description, unsigned long level)
{
	unsigned long offset = 0;
	unsigned long index;

	for (index = 0; index < level && index < description->levels; index++)
		offset += level_bytes(description, index);
	return offset;
}

unsigned long xd3d_texture_face_size(const struct xd3d_texture_description *description)
{
	unsigned long size = xd3d_texture_level_offset(description, description->levels);

	if (description->cube_map)
		size = (size + D3DTEXTURE_CUBEFACE_ALIGNMENT - 1) & ~(unsigned long)(D3DTEXTURE_CUBEFACE_ALIGNMENT - 1);
	return size;
}

unsigned long xd3d_texture_level_pitch(const struct xd3d_texture_description *description, unsigned long level)
{
	if (description->linear)
		return description->pitch;
	if (description->compressed)
		return ((level_dimension(description->width, level) + 3) / 4) * description->bytes;
	return level_dimension(description->width, level) * description->bytes;
}

static BOOL format_is_linear(D3DFORMAT format)
{
	return format_information((DWORD)format).linear;
}

static unsigned long bytes_per_texel(D3DFORMAT format)
{
	return format_information((DWORD)format).bytes;
}

/* ---------- the device's own surfaces */

void xd3d_surface_initialize(D3DSurface *surface, D3DFORMAT format, unsigned long width, unsigned long height)
{
	unsigned long pitch = (width * bytes_per_texel(format) + D3DTEXTURE_PITCH_ALIGNMENT - 1) &
		~(unsigned long)(D3DTEXTURE_PITCH_ALIGNMENT - 1);
	void *memory = aligned_allocate(pitch * height);

	memset(surface, 0, sizeof(*surface));
	/* owned by the device: without D3DCOMMON_D3DCREATED, Release never frees it */
	surface->Common = D3DCOMMON_TYPE_SURFACE | 1;
	surface->Data = (DWORD)memory;
	surface->Format = ((DWORD)format << D3DFORMAT_FORMAT_SHIFT) | (2 << D3DFORMAT_DIMENSION_SHIFT) | D3DFORMAT_DMACHANNEL_A;
	surface->Size = ((pitch / D3DTEXTURE_PITCH_ALIGNMENT - 1) << D3DSIZE_PITCH_SHIFT) |
		((height - 1) << D3DSIZE_HEIGHT_SHIFT) | (width - 1);
}

/* ---------- registration and release */

void WINAPI D3DResource_Register(D3DResource *resource, void *base)
{
	/* every resource starts Common, Data, Lock: Data is an offset from base */
	DWORD *fields = (DWORD *)resource;

	fields[1] = (DWORD)base + fields[1];
	fields[2] = xd3d_resource_serial();
}

ULONG WINAPI D3DResource_Release(D3DResource *resource)
{
	DWORD *fields = (DWORD *)resource;
	ULONG count = fields[0] & D3DCOMMON_REFCOUNT_MASK;

	if (count)
	{
		count--;
		fields[0] = (fields[0] & ~D3DCOMMON_REFCOUNT_MASK) | count;
	}
	/* resources the game built itself (not D3DCOMMON_D3DCREATED) are never freed here */
	if (!count && (fields[0] & D3DCOMMON_D3DCREATED))
	{
		if ((fields[0] & D3DCOMMON_TYPE_MASK) == D3DCOMMON_TYPE_INDEXBUFFER)
			free((void *)fields[1]);
		else
			aligned_free((void *)fields[1]);
		free(resource);
	}
	return count;
}

/* the PC draws from copies: nothing waits for it */
BOOL WINAPI D3DResource_IsBusy(D3DResource *resource)
{
	(void)resource;
	return FALSE;
}

void WINAPI D3DResource_BlockUntilNotBusy(D3DResource *resource)
{
	(void)resource;
}

/* ---------- textures */

static HRESULT create_texture(unsigned long width, unsigned long height, unsigned long depth, unsigned long levels,
	D3DFORMAT format, BOOL cube_map, D3DBaseTexture **result)
{
	D3DBaseTexture *texture = calloc(1, sizeof(*texture));
	struct xd3d_texture_description description;
	unsigned long maximum_levels = floor_log2(width > height ? width : height) + 1;
	void *memory;

	if (!texture)
		return E_OUTOFMEMORY;
	if (!levels || levels > maximum_levels)
		levels = maximum_levels;
	texture->Common = D3DCOMMON_TYPE_TEXTURE | D3DCOMMON_D3DCREATED | 1;
	if (format_is_linear(format))
	{
		unsigned long pitch = (width * bytes_per_texel(format) + D3DTEXTURE_PITCH_ALIGNMENT - 1) &
			~(unsigned long)(D3DTEXTURE_PITCH_ALIGNMENT - 1);

		texture->Format = ((DWORD)format << D3DFORMAT_FORMAT_SHIFT) | (1 << D3DFORMAT_MIPMAP_SHIFT) |
			(2 << D3DFORMAT_DIMENSION_SHIFT) | D3DFORMAT_DMACHANNEL_A;
		texture->Size = ((pitch / D3DTEXTURE_PITCH_ALIGNMENT - 1) << D3DSIZE_PITCH_SHIFT) |
			((height - 1) << D3DSIZE_HEIGHT_SHIFT) | (width - 1);
	}
	else
	{
		texture->Format = (floor_log2(depth) << D3DFORMAT_PSIZE_SHIFT) |
			(floor_log2(height) << D3DFORMAT_VSIZE_SHIFT) |
			(floor_log2(width) << D3DFORMAT_USIZE_SHIFT) |
			((DWORD)format << D3DFORMAT_FORMAT_SHIFT) |
			(levels << D3DFORMAT_MIPMAP_SHIFT) |
			((depth > 1 ? 3 : 2) << D3DFORMAT_DIMENSION_SHIFT) |
			(cube_map ? D3DFORMAT_CUBEMAP : 0) |
			D3DFORMAT_DMACHANNEL_A;
		texture->Size = 0;
	}
	xd3d_texture_describe(texture->Format, texture->Size, &description);
	memory = aligned_allocate(xd3d_texture_face_size(&description) * (cube_map ? 6 : 1));
	if (!memory)
	{
		free(texture);
		return E_OUTOFMEMORY;
	}
	texture->Data = (DWORD)memory;
	texture->Lock = xd3d_resource_serial();
	*result = texture;
	return S_OK;
}

HRESULT WINAPI D3DDevice_CreateTexture(UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format,
	D3DPOOL pool, D3DTexture **texture)
{
	(void)usage;
	(void)pool;
	return create_texture(width, height, 1, levels, format, FALSE, (D3DBaseTexture **)texture);
}

HRESULT WINAPI D3DDevice_CreateVolumeTexture(UINT width, UINT height, UINT depth, UINT levels, DWORD usage,
	D3DFORMAT format, D3DPOOL pool, D3DVolumeTexture **texture)
{
	(void)usage;
	(void)pool;
	return create_texture(width, height, depth, levels, format, FALSE, (D3DBaseTexture **)texture);
}

HRESULT WINAPI D3DDevice_CreateCubeTexture(UINT edge_length, UINT levels, DWORD usage, D3DFORMAT format,
	D3DPOOL pool, D3DCubeTexture **texture)
{
	(void)usage;
	(void)pool;
	return create_texture(edge_length, edge_length, 1, levels, format, TRUE, (D3DBaseTexture **)texture);
}

static void lock_level(DWORD *resource, unsigned long face, unsigned long level, D3DLOCKED_RECT *locked,
	CONST RECT *rectangle)
{
	struct xd3d_texture_description description;
	unsigned long pitch;
	char *bits;

	xd3d_texture_describe(resource[3], resource[4], &description);
	pitch = xd3d_texture_level_pitch(&description, level);
	bits = (char *)resource[1];
	if (bits)
		bits += face * xd3d_texture_face_size(&description) + xd3d_texture_level_offset(&description, level);
	if (rectangle && bits)
	{
		/* (swizzled textures cannot be addressed by rectangle: only linear
		and compressed layouts have a row pitch) */
		unsigned long bytes = pitch / level_dimension(description.width, level);

		if (description.compressed)
			bits += (rectangle->top / 4) * pitch + (rectangle->left / 4) * description.bytes;
		else
			bits += rectangle->top * pitch + rectangle->left * bytes;
	}
	locked->Pitch = (INT)pitch;
	locked->pBits = bits;
	/* (the game is about to write: the PC's copy is made again) */
	resource[2] = xd3d_resource_serial();
}

void WINAPI D3DTexture_LockRect(D3DTexture *texture, UINT level, D3DLOCKED_RECT *locked, CONST RECT *rectangle,
	DWORD flags)
{
	(void)flags;
	lock_level((DWORD *)texture, 0, level, locked, rectangle);
}

void WINAPI D3DCubeTexture_LockRect(D3DCubeTexture *texture, D3DCUBEMAP_FACES face, UINT level,
	D3DLOCKED_RECT *locked, CONST RECT *rectangle, DWORD flags)
{
	(void)flags;
	lock_level((DWORD *)texture, (unsigned long)face, level, locked, rectangle);
}

void WINAPI D3DVolumeTexture_LockBox(D3DVolumeTexture *texture, UINT level, D3DLOCKED_BOX *locked,
	CONST D3DBOX *box, DWORD flags)
{
	DWORD *resource = (DWORD *)texture;
	struct xd3d_texture_description description;
	unsigned long row_pitch, slice;
	char *bits;

	(void)flags;
	xd3d_texture_describe(resource[3], resource[4], &description);
	row_pitch = xd3d_texture_level_pitch(&description, level);
	slice = row_pitch * level_dimension(description.height, level);
	bits = (char *)resource[1];
	if (bits)
		bits += xd3d_texture_level_offset(&description, level);
	if (box && bits)
		bits += box->Front * slice + box->Top * row_pitch + box->Left * (row_pitch / level_dimension(description.width, level));
	locked->RowPitch = (INT)row_pitch;
	locked->SlicePitch = (INT)slice;
	locked->pBits = bits;
	resource[2] = xd3d_resource_serial();
}

static void describe_level(const DWORD *resource, unsigned long level, D3DSURFACE_DESC *description)
{
	struct xd3d_texture_description texture;
	unsigned long width, height;

	xd3d_texture_describe(resource[3], resource[4], &texture);
	width = level_dimension(texture.width, level);
	height = level_dimension(texture.height, level);
	memset(description, 0, sizeof(*description));
	description->Format = (D3DFORMAT)texture.format;
	description->Type = D3DRTYPE_SURFACE;
	description->Width = width;
	description->Height = height;
	description->Size = xd3d_texture_level_pitch(&texture, level) * (texture.compressed ? (height + 3) / 4 : height);
	description->MultiSampleType = D3DMULTISAMPLE_NONE;
}

void WINAPI D3DTexture_GetLevelDesc(D3DTexture *texture, UINT level, D3DSURFACE_DESC *description)
{
	describe_level((const DWORD *)texture, level, description);
}

HRESULT WINAPI D3DTexture_GetSurfaceLevel(D3DTexture *texture, UINT level, D3DSurface **result)
{
	D3DBaseTexture *base = (D3DBaseTexture *)texture;
	struct xd3d_texture_description description;
	D3DSurface *surface = calloc(1, sizeof(*surface));
	unsigned long width, height;

	if (!surface)
		return E_OUTOFMEMORY;
	xd3d_texture_describe(base->Format, base->Size, &description);
	width = level_dimension(description.width, level);
	height = level_dimension(description.height, level);
	surface->Common = D3DCOMMON_TYPE_SURFACE | 1;
	surface->Data = base->Data + xd3d_texture_level_offset(&description, level);
	surface->Format = (base->Format & ~(D3DFORMAT_USIZE_MASK | D3DFORMAT_VSIZE_MASK | D3DFORMAT_PSIZE_MASK |
		D3DFORMAT_MIPMAP_MASK)) |
		(floor_log2(width) << D3DFORMAT_USIZE_SHIFT) |
		(floor_log2(height) << D3DFORMAT_VSIZE_SHIFT) |
		(1 << D3DFORMAT_MIPMAP_SHIFT);
	surface->Size = base->Size;
	surface->Parent = base;
	*result = surface;
	return S_OK;
}

void WINAPI D3DSurface_GetDesc(D3DSurface *surface, D3DSURFACE_DESC *description)
{
	describe_level((const DWORD *)surface, 0, description);
}

void WINAPI D3DSurface_LockRect(D3DSurface *surface, D3DLOCKED_RECT *locked, CONST RECT *rectangle, DWORD flags)
{
	(void)flags;
	lock_level((DWORD *)surface, 0, 0, locked, rectangle);
	if (surface->Parent)
		surface->Parent->Lock = surface->Lock;
}

/* ---------- vertex and index buffers */

HRESULT WINAPI D3DDevice_CreateVertexBuffer(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool, D3DVertexBuffer **result)
{
	D3DVertexBuffer *buffer = calloc(1, sizeof(*buffer));
	void *memory;

	(void)usage;
	(void)fvf;
	(void)pool;
	if (!buffer)
		return E_OUTOFMEMORY;
	memory = aligned_allocate(length ? length : 1);
	if (!memory)
	{
		free(buffer);
		return E_OUTOFMEMORY;
	}
	buffer->Common = D3DCOMMON_TYPE_VERTEXBUFFER | D3DCOMMON_D3DCREATED | 1;
	buffer->Data = (DWORD)memory;
	*result = buffer;
	return S_OK;
}

void WINAPI D3DVertexBuffer_Lock(D3DVertexBuffer *buffer, UINT offset, UINT size, BYTE **data, DWORD flags)
{
	(void)size;
	(void)flags;
	*data = buffer->Data ? (BYTE *)buffer->Data + offset : NULL;
}

HRESULT WINAPI D3DDevice_CreateIndexBuffer(UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool,
	D3DIndexBuffer **result)
{
	D3DIndexBuffer *buffer = calloc(1, sizeof(*buffer));
	void *memory;

	(void)usage;
	(void)format;
	(void)pool;
	if (!buffer)
		return E_OUTOFMEMORY;
	/* (index data is ordinary memory on the Xbox too) */
	memory = calloc(1, length ? length : 1);
	if (!memory)
	{
		free(buffer);
		return E_OUTOFMEMORY;
	}
	buffer->Common = D3DCOMMON_TYPE_INDEXBUFFER | D3DCOMMON_D3DCREATED | 1;
	buffer->Data = (DWORD)memory;
	*result = buffer;
	return S_OK;
}

/* ---------- palettes */

HRESULT WINAPI D3DDevice_CreatePalette(D3DPALETTESIZE size, D3DPalette **result)
{
	D3DPalette *palette = calloc(1, sizeof(*palette));
	void *memory;

	if (!palette)
		return E_OUTOFMEMORY;
	/* (always room for 256 entries, which the texture decoder may read) */
	memory = aligned_allocate(256 * sizeof(D3DCOLOR));
	if (!memory)
	{
		free(palette);
		return E_OUTOFMEMORY;
	}
	palette->Common = D3DCOMMON_TYPE_PALETTE | D3DCOMMON_D3DCREATED | 1 | ((DWORD)size << D3DPALETTE_COMMON_PALETTESIZE_SHIFT);
	palette->Data = (DWORD)memory;
	palette->Lock = xd3d_resource_serial();
	*result = palette;
	return S_OK;
}

void WINAPI D3DPalette_Lock(D3DPalette *palette, D3DCOLOR **colors, DWORD flags)
{
	(void)flags;
	*colors = (D3DCOLOR *)palette->Data;
	palette->Lock = xd3d_resource_serial();
}

/* ---------- D3DX */

HRESULT WINAPI D3DXGetErrorStringA(HRESULT error, LPSTR buffer, UINT buffer_length)
{
	if (buffer && buffer_length)
	{
		const char *text = error == S_OK ? "S_OK" :
			error == E_OUTOFMEMORY ? "E_OUTOFMEMORY" :
			error == E_FAIL ? "E_FAIL" : "Direct3D error";

		strncpy(buffer, text, buffer_length - 1);
		buffer[buffer_length - 1] = '\0';
	}
	return S_OK;
}
