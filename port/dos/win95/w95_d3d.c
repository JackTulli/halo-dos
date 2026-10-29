/*
W95_D3D.C

Direct3D 8 on Windows 95 and 98 (the setting renderer = direct3d,
SETTINGS > RENDERER): d3d8.dll, loaded when asked for (DirectX 8.0a is the
last for Windows 95; nothing is imported, so the program starts without
it), a device on the game's window at the window's size, and the device
the renderer's Direct3D rasterizer draws through (port/dos/render/r_d3d.c),
handed to it. Without the library or a device, the software renderer stays.

The vertices come in clip space (r_d3d.h); a vertex shader of four moves
(vs.1.1, written below as its tokens: there is no assembler here) hands
them on, so that Direct3D clips them and maps them to the viewport. A card
without vertex shaders in hardware runs it in software, as Direct3D 8 does
for every card of the Windows 95 years.

The Xbox's texture pass (r_d3d.h's r_d3d_detail) takes up to four
texture stages at once, as Halo's own did: a second program hands each
stage the base map's coordinates times its scale (constants c0 to c3).

Textures are kept in the card's best format: 32-bit, or 16-bit on a card
without it (5:6:5 for an opaque texture, 1:5:5:5 for a cut-out, 4:4:4:4
for one with alpha), in the managed pool, so that a lost device (the
desktop's mode changed, the screen locked) needs only a Reset; a window of
a new size Resets the device at it.
*/

#include "halo_d3d8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "halo_system.h"
#include "r_d3d.h"

/* the settings (port/dos/xdk/dos_settings.c) */
long dos_setting_integer(const char *name, long default_value);

static void log_line(const char *format, ...)
{
	char line[512];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	system_log(line);
}

struct r_d3d_texture
{
	IDirect3DTexture8 *texture;
	/* (a cube map's instead: texture NULL) */
	IDirect3DCubeTexture8 *cube;
	D3DFORMAT format;
	int width, height, levels;
};

/* the render states, texture stage states, textures and vertex shader the
device was last given: a value it has already is not given again (each
call goes through the runtime to the driver, and the texture pass and the
ordinary draws ask for most of their stages' states at every switch: on
Blood Gulch about 5,500 calls a frame asked, 1,100 made) */
#define CACHED_RENDER_STATES 256
#define CACHED_STAGES 8
#define CACHED_STAGE_STATES 32

struct device_cache
{
	DWORD render[CACHED_RENDER_STATES];
	unsigned char render_known[CACHED_RENDER_STATES];
	DWORD stage[CACHED_STAGES][CACHED_STAGE_STATES];
	unsigned char stage_known[CACHED_STAGES][CACHED_STAGE_STATES];
	IDirect3DBaseTexture8 *textures[CACHED_STAGES];
	unsigned char texture_known[CACHED_STAGES];
	DWORD shader;
	int shader_known;
};

static struct
{
	HMODULE library;
	IDirect3D8 *d3d;
	IDirect3DDevice8 *device;
	HWND window;
	D3DPRESENT_PARAMETERS presentation;
	D3DCAPS8 caps;
	DWORD vertex_shader;
	DWORD behaviour;
	/* the texture formats, by what a texture's alpha holds */
	D3DFORMAT formats[3];
	int modulate_twice;
	DWORD anisotropy;
	int in_scene, lost;
	/* what was last given to Direct3D */
	struct r_d3d_state state;
	int state_known;
	/* the Xbox's texture pass: its program, whether its stages are set, and
	the stages last set */
	DWORD detail_shader;
	int in_detail;
	struct r_d3d_detail detail;
	/* the depth buffer has a stencil: the texture pass gives each pixel its
	pass once (a surface two clusters list is drawn twice) */
	int stencil;
	/* (testing: an older card's limits, test_limits) */
	int test_limits;
	struct device_cache cache;
	/* Halo's water: its vertex program, the pixel program it uses (water:
	r_d3d.h's 2 or 1, 0 none), and a white texel where it has no base map */
	DWORD water_vertex_shader, water_pixel_shader, pixel_shader;
	int water;
	struct r_d3d_texture *white;
	/* testing (setting debug_d3d=1): the draws and calls a frame, and the
	game thread's cycles outside Present and in it (Windows Vista's
	QueryThreadCycleTime, where there is one), logged every 300 frames */
	int debug;
	unsigned long frames, draws, vertices, indices, calls_asked, calls_made;
	BOOL (WINAPI *thread_cycles)(HANDLE thread, ULONG64 *cycles);
	ULONG64 cycles_last, cycles_present;
} d3d;

static int test_texture_taken(int width, int height);

/* ---------- the pass-through program and its vertex (struct r_d3d_vertex:
v0 the position, v1 the colour, v2 the fog factor, v3 the coordinates, v4
the lightmap's) */

static const DWORD vertex_declaration[] =
{
	D3DVSD_STREAM(0),
	D3DVSD_REG(0, D3DVSDT_FLOAT4),
	D3DVSD_REG(1, D3DVSDT_D3DCOLOR),
	D3DVSD_REG(2, D3DVSDT_FLOAT1),
	D3DVSD_REG(3, D3DVSDT_FLOAT2),
	D3DVSD_REG(4, D3DVSDT_FLOAT2),
	D3DVSD_END()
};

/* tokens: the version, then each instruction's opcode (mov is 1), its
destination (0x80000000 | register type << 28 | write mask << 16 |
number: oPos and oFog are rasterizer outputs 4 (0 and 1), oD0 attribute
output 5, oT0 and oT1 texture coordinate outputs 6) and its source (input
registers 1, swizzle at bit 16: 0xe4 is xyzw, 0 is xxxx) */
static const DWORD vertex_function[] =
{
	0xfffe0101,
	0x00000001, 0xc00f0000, 0x90e40000, /* mov oPos, v0 */
	0x00000001, 0xd00f0000, 0x90e40001, /* mov oD0, v1 */
	0x00000001, 0xc0010001, 0x90000002, /* mov oFog.x, v2.x */
	0x00000001, 0xe00f0000, 0x90e40003, /* mov oT0, v3 */
	0x00000001, 0xe00f0001, 0x90e40004, /* mov oT1, v4 */
	0x0000ffff
};

/* the Xbox's texture pass's: each stage's coordinates the base map's times
its scale plus its offset, a dp4 of (u, v, 0, 1) for each (dp4 is 9;
constant registers are type 2; vs.1.1 reads one constant an instruction):
c(2s) = (scale u, 0, 0, offset u), c(2s+1) = (0, scale v, 0, offset v) */
static const DWORD detail_function[] =
{
	0xfffe0101,
	0x00000001, 0xc00f0000, 0x90e40000, /* mov oPos, v0 */
	0x00000001, 0xd00f0000, 0x90e40001, /* mov oD0, v1 */
	0x00000001, 0xc0010001, 0x90000002, /* mov oFog.x, v2.x */
	0x00000009, 0xe0010000, 0x90e40003, 0xa0e40000, /* dp4 oT0.x, v3, c0 */
	0x00000009, 0xe0020000, 0x90e40003, 0xa0e40001, /* dp4 oT0.y, v3, c1 */
	0x00000009, 0xe0010001, 0x90e40003, 0xa0e40002, /* dp4 oT1.x, v3, c2 */
	0x00000009, 0xe0020001, 0x90e40003, 0xa0e40003, /* dp4 oT1.y, v3, c3 */
	0x00000009, 0xe0010002, 0x90e40003, 0xa0e40004, /* dp4 oT2.x, v3, c4 */
	0x00000009, 0xe0020002, 0x90e40003, 0xa0e40005, /* dp4 oT2.y, v3, c5 */
	0x00000009, 0xe0010003, 0x90e40003, 0xa0e40006, /* dp4 oT3.x, v3, c6 */
	0x00000009, 0xe0020003, 0x90e40003, 0xa0e40007, /* dp4 oT3.y, v3, c7 */
	0x0000ffff
};

/* ---------- Halo's water (r_d3d.h's r_d3d_water): its vertex (struct
r_d3d_water_vertex: v0 the position, v1 the colour, v2 to v7 the sets) and
its programs, assembled by tools/d3d8_shader_tokens.py */

static const DWORD water_declaration[] =
{
	D3DVSD_STREAM(0),
	D3DVSD_REG(0, D3DVSDT_FLOAT4),
	D3DVSD_REG(1, D3DVSDT_D3DCOLOR),
	D3DVSD_REG(2, D3DVSDT_FLOAT2),
	D3DVSD_REG(3, D3DVSDT_FLOAT4),
	D3DVSD_REG(4, D3DVSDT_FLOAT4),
	D3DVSD_REG(5, D3DVSDT_FLOAT4),
	D3DVSD_REG(6, D3DVSDT_FLOAT4),
	D3DVSD_REG(7, D3DVSDT_FLOAT2),
	D3DVSD_END()
};

static const DWORD water_vertex_function[] =
{
	0xfffe0101, /* vs.1.1 */
	0x00000001, 0xc00f0000, 0x90e40000, /* mov oPos, v0 */
	0x00000001, 0xd00f0000, 0x90e40001, /* mov oD0, v1 */
	0x00000001, 0xe00f0000, 0x90e40002, /* mov oT0, v2 */
	0x00000001, 0xe00f0001, 0x90e40003, /* mov oT1, v3 */
	0x00000001, 0xe00f0002, 0x90e40004, /* mov oT2, v4 */
	0x00000001, 0xe00f0003, 0x90e40005, /* mov oT3, v5 */
	0x00000001, 0xe00f0004, 0x90e40006, /* mov oT4, v6 */
	0x00000001, 0xe00f0005, 0x90e40007, /* mov oT5, v7 */
	0x0000ffff, /* end */
};

/* program 2, pixel shaders 1.4: ATI's ocean water program's way (the
ripple's normal into the world by the surface's frame, the eye reflected in
it into the cube map, the tint by how squarely it is seen from a texture),
with Halo's water's own mix: the reflection times the tint plus its eighth
power times one minus it (glints), times the tint's alpha and the base
map's, times what the fog leaves (the colour's alpha). Stages: 0 the
ripples' normal map, 1 the cube map, 2 the tint, 5 the base map. */
static const DWORD water_pixel_14[] =
{
	0xffff0104, /* ps.1.4 */
	0x00000042, 0x800f0000, 0xb0e40000, /* texld r0, t0 */
	0x00000040, 0x80070001, 0xb0e40001, /* texcrd r1.rgb, t1 */
	0x00000040, 0x80070002, 0xb0e40002, /* texcrd r2.rgb, t2 */
	0x00000040, 0x80070003, 0xb0e40003, /* texcrd r3.rgb, t3 */
	0x00000040, 0x80070004, 0xb0e40004, /* texcrd r4.rgb, t4 */
	0x00000005, 0x80070002, 0x80e40002, 0x84000000, /* mul r2.rgb, r2, r0_bx2.r */
	0x00000004, 0x80070002, 0x80e40003, 0x84550000, 0x80e40002, /* mad r2.rgb, r3, r0_bx2.g, r2 */
	0x00000004, 0x80070002, 0x80e40004, 0x84aa0000, 0x80e40002, /* mad r2.rgb, r4, r0_bx2.b, r2 */
	0x00000008, 0x80070003, 0x80e40002, 0x80e40001, /* dp3 r3.rgb, r2, r1 */
	0x00000004, 0x80070001, 0x80e40002, 0x87e40003, 0x81e40001, /* mad r1.rgb, r2, r3_x2, -r1 */
	0x00000001, 0x80170000, 0x80e40003, /* mov_sat r0.rgb, r3 */
	0x0000fffd, /* phase */
	0x00000042, 0x800f0001, 0x80a40001, /* texld r1, r1.xyz */
	0x00000042, 0x800f0002, 0x80a40000, /* texld r2, r0.xyz */
	0x00000042, 0x800f0005, 0xb0e40005, /* texld r5, t5 */
	0x00000005, 0x80070003, 0x80e40001, 0x80e40001, /* mul r3.rgb, r1, r1 */
	0x40000005, 0x80080005, 0x80ff0002, 0x80ff0005, /* +mul r5.a, r2.a, r5.a */
	0x00000005, 0x80070003, 0x80e40003, 0x80e40003, /* mul r3.rgb, r3, r3 */
	0x00000005, 0x80070003, 0x80e40003, 0x80e40003, /* mul r3.rgb, r3, r3 */
	0x00000012, 0x80070000, 0x80e40002, 0x80e40001, 0x80e40003, /* lrp r0.rgb, r2, r1, r3 */
	0x00000005, 0x80070000, 0x80e40000, 0x80ff0005, /* mul r0.rgb, r0, r5.a */
	0x00000005, 0x80070000, 0x80e40000, 0x90ff0000, /* mul r0.rgb, r0, v0.a */
	/* (r0's alpha is the last phase's to write: the phase leaves none) */
	0x00000001, 0x80080000, 0x80ff0005, /* mov r0.a, r5.a */
	0x0000ffff, /* end */
};

/* program 1, pixel shaders 1.1: Halo's own (rasterizer_xbox_water.c's
reflection pass): texm3x3vspec, the reflection times the draw's tint (c0)
plus its eighth power times one minus it, times what the fog leaves.
Stages: 0 the ripples' normal map, 3 the cube map. */
static const DWORD water_pixel_11[] =
{
	0xffff0101, /* ps.1.1 */
	0x00000042, 0xb00f0000, /* tex t0 */
	0x00000049, 0xb00f0001, 0xb4e40000, /* texm3x3pad t1, t0_bx2 */
	0x00000049, 0xb00f0002, 0xb4e40000, /* texm3x3pad t2, t0_bx2 */
	0x0000004d, 0xb00f0003, 0xb4e40000, /* texm3x3vspec t3, t0_bx2 */
	0x00000005, 0x800f0001, 0xb0e40003, 0xb0e40003, /* mul r1, t3, t3 */
	0x00000005, 0x800f0001, 0x80e40001, 0x80e40001, /* mul r1, r1, r1 */
	0x00000005, 0x800f0001, 0x80e40001, 0x80e40001, /* mul r1, r1, r1 */
	0x00000012, 0x800f0000, 0xa0e40000, 0xb0e40003, 0x80e40001, /* lrp r0, c0, t3, r1 */
	0x00000005, 0x800f0000, 0x80e40000, 0x90ff0000, /* mul r0, r0, v0.a */
	0x0000ffff, /* end */
};

/* ---------- what the device is given (through the cache) */

static void set_render_state(D3DRENDERSTATETYPE state, DWORD value)
{
	d3d.calls_asked++;
	if ((unsigned)state < CACHED_RENDER_STATES)
	{
		if (d3d.cache.render_known[state] && d3d.cache.render[state] == value)
			return;
		d3d.cache.render[state] = value;
		d3d.cache.render_known[state] = TRUE;
	}
	d3d.calls_made++;
	IDirect3DDevice8_SetRenderState(d3d.device, state, value);
}

static void set_stage_state(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
	d3d.calls_asked++;
	if (stage < CACHED_STAGES && (unsigned)type < CACHED_STAGE_STATES)
	{
		if (d3d.cache.stage_known[stage][type] && d3d.cache.stage[stage][type] == value)
			return;
		d3d.cache.stage[stage][type] = value;
		d3d.cache.stage_known[stage][type] = TRUE;
	}
	d3d.calls_made++;
	IDirect3DDevice8_SetTextureStageState(d3d.device, stage, type, value);
}

static void set_texture(DWORD stage, IDirect3DBaseTexture8 *texture)
{
	d3d.calls_asked++;
	if (stage < CACHED_STAGES)
	{
		if (d3d.cache.texture_known[stage] && d3d.cache.textures[stage] == texture)
			return;
		d3d.cache.textures[stage] = texture;
		d3d.cache.texture_known[stage] = TRUE;
	}
	d3d.calls_made++;
	IDirect3DDevice8_SetTexture(d3d.device, stage, texture);
}

static void set_vertex_shader(DWORD shader)
{
	d3d.calls_asked++;
	if (d3d.cache.shader_known && d3d.cache.shader == shader)
		return;
	d3d.cache.shader = shader;
	d3d.cache.shader_known = TRUE;
	d3d.calls_made++;
	IDirect3DDevice8_SetVertexShader(d3d.device, shader);
}

/* ---------- textures */

static int format_usable(D3DFORMAT format)
{
	return SUCCEEDED(IDirect3D8_CheckDeviceFormat(d3d.d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
		d3d.presentation.BackBufferFormat, 0, D3DRTYPE_TEXTURE, format));
}

static struct r_d3d_texture *texture_new(int width, int height, int levels, int alpha)
{
	struct r_d3d_texture *texture = calloc(1, sizeof(*texture));

	if (!texture || !d3d.device)
	{
		free(texture);
		return NULL;
	}
	texture->format = d3d.formats[alpha >= 0 && alpha <= 2 ? alpha : 2];
	texture->width = width;
	texture->height = height;
	texture->levels = levels;
	if (!test_texture_taken(width, height) ||
		FAILED(IDirect3DDevice8_CreateTexture(d3d.device, width, height, levels, 0, texture->format, D3DPOOL_MANAGED,
		&texture->texture)))
	{
		log_line("direct3d: a %dx%d texture could not be made", width, height);
		free(texture);
		return NULL;
	}
	return texture;
}

/* a locked level's rows from 0xAARRGGBB texels, in the texture's format */
static void write_rows(const struct r_d3d_texture *texture, const D3DLOCKED_RECT *locked_rect,
	const unsigned long *argb, int width, int height);

/* a level's texels from 0xAARRGGBB, in the texture's format */
static void texture_write(struct r_d3d_texture *texture, int level, const unsigned long *argb)
{
	int width, height;
	D3DLOCKED_RECT locked;

	if (!texture || !texture->texture ||
		FAILED(IDirect3DTexture8_LockRect(texture->texture, level, &locked, NULL, 0)))
	{
		return;
	}
	width = texture->width >> level ? texture->width >> level : 1;
	height = texture->height >> level ? texture->height >> level : 1;
	write_rows(texture, &locked, argb, width, height);
	IDirect3DTexture8_UnlockRect(texture->texture, level);
}

/* a cube map (Halo's water's reflection), in the formats the textures take */
static struct r_d3d_texture *cube_new(int size, int levels)
{
	struct r_d3d_texture *cube = calloc(1, sizeof(*cube));

	if (!cube || !d3d.device || !(d3d.caps.TextureCaps & D3DPTEXTURECAPS_CUBEMAP))
	{
		free(cube);
		return NULL;
	}
	cube->format = d3d.formats[_r_d3d_alpha_none];
	cube->width = cube->height = size;
	cube->levels = levels;
	if (FAILED(IDirect3DDevice8_CreateCubeTexture(d3d.device, (UINT)size, (UINT)levels, 0, cube->format, D3DPOOL_MANAGED,
		&cube->cube)))
	{
		log_line("direct3d: a cube map of %d texels a side could not be made", size);
		free(cube);
		return NULL;
	}
	return cube;
}

static void cube_write(struct r_d3d_texture *cube, int face, int level, const unsigned long *argb)
{
	D3DLOCKED_RECT locked;
	int size;

	if (!cube || !cube->cube || face < 0 || face > 5 || level >= cube->levels ||
		FAILED(IDirect3DCubeTexture8_LockRect(cube->cube, (D3DCUBEMAP_FACES)face, (UINT)level, &locked, NULL, 0)))
	{
		return;
	}
	size = cube->width >> level ? cube->width >> level : 1;
	write_rows(cube, &locked, argb, size, size);
	IDirect3DCubeTexture8_UnlockRect(cube->cube, (D3DCUBEMAP_FACES)face, (UINT)level);
}

static void write_rows(const struct r_d3d_texture *texture, const D3DLOCKED_RECT *locked_rect,
	const unsigned long *argb, int width, int height)
{
	D3DLOCKED_RECT locked = *locked_rect;

	for (int y = 0; y < height; y++)
	{
		const unsigned long *in = argb + (size_t)y * width;
		unsigned char *row = (unsigned char *)locked.pBits + (size_t)y * locked.Pitch;

		switch (texture->format)
		{
		case D3DFMT_A8R8G8B8:
		case D3DFMT_X8R8G8B8:
			memcpy(row, in, (size_t)width * 4);
			break;
		case D3DFMT_R5G6B5:
			for (int x = 0; x < width; x++)
				((unsigned short *)row)[x] = (unsigned short)(((in[x] >> 8) & 0xF800) | ((in[x] >> 5) & 0x07E0) |
					((in[x] >> 3) & 0x001F));
			break;
		case D3DFMT_A1R5G5B5:
			for (int x = 0; x < width; x++)
				((unsigned short *)row)[x] = (unsigned short)(((in[x] >> 31) << 15) | ((in[x] >> 9) & 0x7C00) |
					((in[x] >> 6) & 0x03E0) | ((in[x] >> 3) & 0x001F));
			break;
		default: /* D3DFMT_A4R4G4B4 */
			for (int x = 0; x < width; x++)
				((unsigned short *)row)[x] = (unsigned short)(((in[x] >> 16) & 0xF000) | ((in[x] >> 12) & 0x0F00) |
					((in[x] >> 8) & 0x00F0) | ((in[x] >> 4) & 0x000F));
			break;
		}
	}
}

/* the card's texture of one of ours, flat or cube */
static IDirect3DBaseTexture8 *base_texture(const struct r_d3d_texture *texture)
{
	return !texture ? NULL : texture->cube ? (IDirect3DBaseTexture8 *)texture->cube :
		(IDirect3DBaseTexture8 *)texture->texture;
}

static void texture_delete(struct r_d3d_texture *texture)
{
	if (!texture)
		return;
	/* (not left on a stage: the cache would take a new texture at its
	address for it) */
	for (DWORD stage = 0; d3d.device && stage < CACHED_STAGES; stage++)
	{
		if (d3d.cache.texture_known[stage] && d3d.cache.textures[stage] == base_texture(texture))
			set_texture(stage, NULL);
	}
	/* (the texture pass's stages are set again: one may be this) */
	d3d.detail.stage_count = 0;
	if (texture->texture)
		IDirect3DTexture8_Release(texture->texture);
	if (texture->cube)
		IDirect3DCubeTexture8_Release(texture->cube);
	free(texture);
}

/* ---------- drawing */

static void scene_begin(void)
{
	if (!d3d.in_scene && d3d.device && !d3d.lost)
	{
		IDirect3DDevice8_BeginScene(d3d.device);
		d3d.in_scene = TRUE;
	}
}

static void scene_end(void)
{
	if (d3d.in_scene)
	{
		IDirect3DDevice8_EndScene(d3d.device);
		d3d.in_scene = FALSE;
	}
}

static void viewport(int x, int y, int width, int height, float minimum_z, float maximum_z)
{
	D3DVIEWPORT8 view;
	int right = x + width, bottom = y + height;

	if (!d3d.device || d3d.lost)
		return;
	/* (inside the back buffer) */
	x = x < 0 ? 0 : x;
	y = y < 0 ? 0 : y;
	right = right > (int)d3d.presentation.BackBufferWidth ? (int)d3d.presentation.BackBufferWidth : right;
	bottom = bottom > (int)d3d.presentation.BackBufferHeight ? (int)d3d.presentation.BackBufferHeight : bottom;
	if (right <= x || bottom <= y)
		return;
	view.X = (DWORD)x;
	view.Y = (DWORD)y;
	view.Width = (DWORD)(right - x);
	view.Height = (DWORD)(bottom - y);
	view.MinZ = minimum_z;
	view.MaxZ = maximum_z;
	IDirect3DDevice8_SetViewport(d3d.device, &view);
}

static void clear(int x0, int y0, int x1, int y1, int color, int depth, unsigned long argb)
{
	D3DRECT box;
	D3DVIEWPORT8 saved, whole;

	if (!d3d.device || d3d.lost || (!color && !depth))
		return;
	box.x1 = x0 < 0 ? 0 : x0;
	box.y1 = y0 < 0 ? 0 : y0;
	box.x2 = x1 > (int)d3d.presentation.BackBufferWidth ? (int)d3d.presentation.BackBufferWidth : x1;
	box.y2 = y1 > (int)d3d.presentation.BackBufferHeight ? (int)d3d.presentation.BackBufferHeight : y1;
	if (box.x2 <= box.x1 || box.y2 <= box.y1)
		return;
	/* (a clear is clipped to the viewport: the whole back buffer's for it) */
	IDirect3DDevice8_GetViewport(d3d.device, &saved);
	whole.X = 0;
	whole.Y = 0;
	whole.Width = d3d.presentation.BackBufferWidth;
	whole.Height = d3d.presentation.BackBufferHeight;
	whole.MinZ = 0.0f;
	whole.MaxZ = 1.0f;
	IDirect3DDevice8_SetViewport(d3d.device, &whole);
	IDirect3DDevice8_Clear(d3d.device, 1, &box, (color ? D3DCLEAR_TARGET : 0) | (depth ? D3DCLEAR_ZBUFFER : 0) |
		(depth && d3d.stencil ? D3DCLEAR_STENCIL : 0), (D3DCOLOR)argb, 1.0f, 0);
	IDirect3DDevice8_SetViewport(d3d.device, &saved);
}

static void render_state(D3DRENDERSTATETYPE state, DWORD value)
{
	set_render_state(state, value);
}

static void stage_state(D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
	set_stage_state(0, type, value);
}

/* stages 1 to 3 as the ordinary draws have them: the lightmap's stage (off
until a lightmap is drawn), the others off */
static void stages_for_drawing(void)
{
	set_vertex_shader(d3d.vertex_shader);
	stage_state(D3DTSS_TEXCOORDINDEX, 0);
	stage_state(D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
	set_stage_state(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
	set_stage_state(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
	/* (the lightmap's stage: the colour so far times the lightmap's texel,
	filtered, its coordinates the second set; the alpha as it is) */
	set_stage_state(1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	set_stage_state(1, D3DTSS_COLORARG2, D3DTA_CURRENT);
	set_stage_state(1, D3DTSS_ALPHAARG1, D3DTA_CURRENT);
	set_stage_state(1, D3DTSS_TEXCOORDINDEX, 1);
	set_stage_state(1, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
	set_stage_state(1, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
	set_stage_state(1, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
	set_stage_state(1, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
	set_stage_state(1, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
	set_stage_state(1, D3DTSS_MIPFILTER, D3DTEXF_LINEAR);
	set_stage_state(1, D3DTSS_MAXANISOTROPY, 1);
	for (DWORD stage = 2; stage < R_D3D_DETAIL_STAGES && stage < d3d.caps.MaxTextureBlendStages; stage++)
	{
		set_stage_state(stage, D3DTSS_COLOROP, D3DTOP_DISABLE);
		set_stage_state(stage, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
		set_texture(stage, NULL);
	}
	d3d.state_known = FALSE;
	d3d.in_detail = FALSE;
}

/* a stage's colour or alpha operation (r_d3d.h's _r_d3d_stage_*) with its
texel and what is so far, the arguments in the order the operation needs */
static void stage_operation(DWORD stage, int operation, int alpha)
{
	DWORD op = D3DTOP_SELECTARG1, first = D3DTA_TEXTURE, second = D3DTA_CURRENT;

	switch (operation)
	{
	case _r_d3d_stage_current: op = D3DTOP_SELECTARG2; break;
	case _r_d3d_stage_texture: op = D3DTOP_SELECTARG1; break;
	case _r_d3d_stage_modulate: op = D3DTOP_MODULATE; break;
	case _r_d3d_stage_modulate_twice: op = d3d.modulate_twice ? D3DTOP_MODULATE2X : D3DTOP_MODULATE; break;
	case _r_d3d_stage_add: op = D3DTOP_ADD; break;
	case _r_d3d_stage_add_signed: op = D3DTOP_ADDSIGNED; break;
	/* (SUBTRACT is the first less the second) */
	case _r_d3d_stage_subtract: op = D3DTOP_SUBTRACT; first = D3DTA_CURRENT; second = D3DTA_TEXTURE; break;
	case _r_d3d_stage_subtract_reverse: op = D3DTOP_SUBTRACT; break;
	/* (the blends: the first where the alpha is 1) */
	case _r_d3d_stage_blend_current_alpha: op = D3DTOP_BLENDCURRENTALPHA; first = D3DTA_CURRENT; second = D3DTA_TEXTURE; break;
	case _r_d3d_stage_blend_current_alpha_inverse: op = D3DTOP_BLENDCURRENTALPHA; break;
	case _r_d3d_stage_blend_texture_alpha: op = D3DTOP_BLENDTEXTUREALPHA; break;
	case _r_d3d_stage_blend_texture_alpha_inverse:
		op = D3DTOP_BLENDTEXTUREALPHA;
		first = D3DTA_CURRENT;
		second = D3DTA_TEXTURE;
		break;
	case _r_d3d_stage_square: op = D3DTOP_MODULATE; first = D3DTA_CURRENT; second = D3DTA_CURRENT; break;
	default: break;
	}
	set_stage_state(stage, alpha ? D3DTSS_ALPHAOP : D3DTSS_COLOROP, op);
	set_stage_state(stage, alpha ? D3DTSS_ALPHAARG1 : D3DTSS_COLORARG1, first);
	set_stage_state(stage, alpha ? D3DTSS_ALPHAARG2 : D3DTSS_COLORARG2, second);
}

/* the Xbox's texture pass's stages (its program, wrapped and filtered
textures, each stage's operations, its scale and offset constants), and the
stage after them off */
static void stages_for_detail(const struct r_d3d_detail *detail)
{
	float constants[2 * R_D3D_DETAIL_STAGES][4];
	DWORD minification = d3d.anisotropy > 1 ? D3DTEXF_ANISOTROPIC : D3DTEXF_LINEAR;

	if (d3d.in_detail && !memcmp(detail, &d3d.detail, sizeof(*detail)))
		return;
	if (!d3d.in_detail)
		set_vertex_shader(d3d.detail_shader);
	for (DWORD stage = 0; stage < R_D3D_DETAIL_STAGES; stage++)
	{
		int used = stage < (DWORD)detail->stage_count;

		constants[2 * stage][0] = used ? detail->scales[stage][0] : 1.0f;
		constants[2 * stage][1] = 0.0f;
		constants[2 * stage][2] = 0.0f;
		constants[2 * stage][3] = used ? detail->offsets[stage][0] : 0.0f;
		constants[2 * stage + 1][0] = 0.0f;
		constants[2 * stage + 1][1] = used ? detail->scales[stage][1] : 1.0f;
		constants[2 * stage + 1][2] = 0.0f;
		constants[2 * stage + 1][3] = used ? detail->offsets[stage][1] : 0.0f;
		if (stage >= (DWORD)detail->stage_count)
		{
			if (stage < d3d.caps.MaxTextureBlendStages)
			{
				set_stage_state(stage, D3DTSS_COLOROP, D3DTOP_DISABLE);
				set_stage_state(stage, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
				set_texture(stage, NULL);
			}
			continue;
		}
		stage_operation(stage, detail->operations[stage], FALSE);
		stage_operation(stage, detail->alpha_operations[stage], TRUE);
		set_stage_state(stage, D3DTSS_TEXCOORDINDEX, stage);
		set_stage_state(stage, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
		set_stage_state(stage, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
		set_stage_state(stage, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
		set_stage_state(stage, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
		set_stage_state(stage, D3DTSS_MINFILTER, minification);
		set_stage_state(stage, D3DTSS_MIPFILTER, D3DTEXF_LINEAR);
		set_stage_state(stage, D3DTSS_MAXANISOTROPY, d3d.anisotropy);
		set_texture(stage, (IDirect3DBaseTexture8 *)detail->textures[stage]->texture);
	}
	IDirect3DDevice8_SetVertexShaderConstant(d3d.device, 0, constants, 2 * R_D3D_DETAIL_STAGES);
	d3d.detail = *detail;
	d3d.in_detail = TRUE;
}

/* the fixed states, set once and after a Reset */
static void states_initialize(void)
{
	render_state(D3DRS_LIGHTING, FALSE);
	render_state(D3DRS_CLIPPING, TRUE);
	render_state(D3DRS_CULLMODE, D3DCULL_NONE);
	render_state(D3DRS_DITHERENABLE, TRUE);
	render_state(D3DRS_SPECULARENABLE, FALSE);
	render_state(D3DRS_FOGTABLEMODE, D3DFOG_NONE);
	render_state(D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
	render_state(D3DRS_ALPHAREF, 0x7F);
	render_state(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
	render_state(D3DRS_TEXTUREFACTOR, 0xFF808080UL);
	render_state(D3DRS_ZENABLE, D3DZB_TRUE);
	/* (equal depths pass, as Halo's own depth test: a pass over a surface
	drawn already, a surface two clusters list drawn again over itself, not
	its passes twice over the first; and with a 16-bit depth buffer the sky,
	whose depth there rounds to the cleared farthest, still drawn) */
	render_state(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
	render_state(D3DRS_STENCILENABLE, FALSE);
	render_state(D3DRS_STENCILFUNC, D3DCMP_EQUAL);
	render_state(D3DRS_STENCILREF, 0);
	render_state(D3DRS_STENCILMASK, 0xFF);
	render_state(D3DRS_STENCILWRITEMASK, 0xFF);
	render_state(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
	render_state(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
	render_state(D3DRS_STENCILPASS, D3DSTENCILOP_INCRSAT);
	stages_for_drawing();
}

static void apply_state(const struct r_d3d_state *state)
{
	const struct r_d3d_state *old = d3d.state_known ? &d3d.state : NULL;

	if (!old || old->texture != state->texture)
	{
		switch (state->texture)
		{
		case _r_d3d_colour:
			stage_state(D3DTSS_COLOROP, D3DTOP_SELECTARG1);
			stage_state(D3DTSS_COLORARG1, D3DTA_DIFFUSE);
			stage_state(D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
			stage_state(D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
			break;
		case _r_d3d_xbox_detail:
			/* (its stages: stages_for_detail) */
			break;
		case _r_d3d_detail:
			/* (texel times the colour's alpha, the colour (grey) times one
			minus it) */
			stage_state(D3DTSS_COLOROP, D3DTOP_BLENDDIFFUSEALPHA);
			stage_state(D3DTSS_COLORARG1, D3DTA_TEXTURE);
			stage_state(D3DTSS_COLORARG2, D3DTA_DIFFUSE);
			stage_state(D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
			stage_state(D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
			break;
		default:
			stage_state(D3DTSS_COLOROP, state->texture == _r_d3d_modulate_twice && d3d.modulate_twice ?
				D3DTOP_MODULATE2X : D3DTOP_MODULATE);
			stage_state(D3DTSS_COLORARG1, D3DTA_TEXTURE);
			stage_state(D3DTSS_COLORARG2, D3DTA_DIFFUSE);
			stage_state(D3DTSS_ALPHAOP, D3DTOP_MODULATE);
			stage_state(D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
			stage_state(D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
			break;
		}
	}
	if (!old || old->smooth != state->smooth)
	{
		DWORD minification = state->smooth ? (d3d.anisotropy > 1 ? D3DTEXF_ANISOTROPIC : D3DTEXF_LINEAR) : D3DTEXF_POINT;

		stage_state(D3DTSS_MAGFILTER, state->smooth ? D3DTEXF_LINEAR : D3DTEXF_POINT);
		stage_state(D3DTSS_MINFILTER, minification);
		stage_state(D3DTSS_MIPFILTER, state->smooth ? D3DTEXF_LINEAR : D3DTEXF_POINT);
		if (d3d.anisotropy > 1)
			stage_state(D3DTSS_MAXANISOTROPY, state->smooth ? d3d.anisotropy : 1);
	}
	if (!old || old->clamp != state->clamp)
	{
		stage_state(D3DTSS_ADDRESSU, state->clamp ? D3DTADDRESS_CLAMP : D3DTADDRESS_WRAP);
		stage_state(D3DTSS_ADDRESSV, state->clamp ? D3DTADDRESS_CLAMP : D3DTADDRESS_WRAP);
	}
	if (!old || old->blend != state->blend)
	{
		render_state(D3DRS_ALPHABLENDENABLE, state->blend != _r_d3d_blend_none);
		switch (state->blend)
		{
		case _r_d3d_blend_alpha:
			render_state(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
			render_state(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
			break;
		case _r_d3d_blend_add:
			render_state(D3DRS_SRCBLEND, D3DBLEND_ONE);
			render_state(D3DRS_DESTBLEND, D3DBLEND_ONE);
			break;
		case _r_d3d_blend_add_alpha:
			render_state(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
			render_state(D3DRS_DESTBLEND, D3DBLEND_ONE);
			break;
		case _r_d3d_blend_premultiplied:
			render_state(D3DRS_SRCBLEND, D3DBLEND_ONE);
			render_state(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
			break;
		case _r_d3d_blend_detail:
			render_state(D3DRS_SRCBLEND, D3DBLEND_DESTCOLOR);
			render_state(D3DRS_DESTBLEND, D3DBLEND_SRCCOLOR);
			break;
		case _r_d3d_blend_multiply:
			render_state(D3DRS_SRCBLEND, D3DBLEND_DESTCOLOR);
			render_state(D3DRS_DESTBLEND, D3DBLEND_ZERO);
			break;
		default:
			break;
		}
	}
	if (!old || old->lightmap != state->lightmap)
	{
		set_stage_state(1, D3DTSS_COLOROP,
			state->lightmap ? D3DTOP_MODULATE : D3DTOP_DISABLE);
		set_stage_state(1, D3DTSS_ALPHAOP,
			state->lightmap ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE);
	}
	if (!old || old->alpha_test != state->alpha_test)
		render_state(D3DRS_ALPHATESTENABLE, state->alpha_test);
	/* (the texture pass: where the stencil is 0, which it then raises) */
	if (d3d.stencil && (!old || old->stencil_once != state->stencil_once))
		render_state(D3DRS_STENCILENABLE, state->stencil_once);
	if (!old || old->depth_test != state->depth_test)
		render_state(D3DRS_ZENABLE, state->depth_test ? D3DZB_TRUE : D3DZB_FALSE);
	if (!old || old->depth_write != state->depth_write)
		render_state(D3DRS_ZWRITEENABLE, state->depth_write);
	if (!old || old->fog != state->fog)
		render_state(D3DRS_FOGENABLE, state->fog);
	if (state->fog && (!old || old->fog_color != state->fog_color))
		render_state(D3DRS_FOGCOLOR, state->fog_color);
	d3d.state = *state;
	d3d.state_known = TRUE;
}

/* the triangles: indexed, or, on a card whose hardware takes fewer
vertices a draw than a batch has, the indices' vertices one after the
other */
static void draw_triangles(const struct r_d3d_vertex *vertices, unsigned long vertex_count,
	const unsigned short *indices, unsigned long index_count)
{
	d3d.draws++;
	d3d.vertices += vertex_count;
	d3d.indices += index_count;
	if (d3d.behaviour == D3DCREATE_SOFTWARE_VERTEXPROCESSING || vertex_count <= d3d.caps.MaxVertexIndex + 1UL)
	{
		IDirect3DDevice8_DrawIndexedPrimitiveUP(d3d.device, D3DPT_TRIANGLELIST, 0, vertex_count, index_count / 3, indices,
			D3DFMT_INDEX16, vertices, sizeof(*vertices));
		return;
	}
	{
		static struct r_d3d_vertex spread[3 * 256];

		for (unsigned long first = 0; first < index_count; first += 3 * 256)
		{
			unsigned long count = index_count - first < 3 * 256 ? index_count - first : 3 * 256;

			for (unsigned long i = 0; i < count; i++)
				spread[i] = vertices[indices[first + i]];
			IDirect3DDevice8_DrawPrimitiveUP(d3d.device, D3DPT_TRIANGLELIST, count / 3, spread, sizeof(*spread));
		}
	}
}

static void draw(const struct r_d3d_state *state, struct r_d3d_texture *texture, struct r_d3d_texture *lightmap,
	const struct r_d3d_detail *detail, const struct r_d3d_vertex *vertices, unsigned long vertex_count,
	const unsigned short *indices, unsigned long index_count)
{
	IDirect3DTexture8 *card = texture ? texture->texture : NULL;
	IDirect3DTexture8 *lightmap_card = lightmap && state->lightmap ? lightmap->texture : NULL;

	if (!d3d.device || d3d.lost || index_count < 3 || !vertex_count)
		return;
	scene_begin();
	if (state->texture == _r_d3d_xbox_detail)
	{
		if (!detail || !d3d.detail_shader)
			return;
		apply_state(state);
		stages_for_detail(detail);
		draw_triangles(vertices, vertex_count, indices, index_count);
		return;
	}
	if (d3d.in_detail)
		stages_for_drawing();
	apply_state(state);
	set_texture(0, (IDirect3DBaseTexture8 *)card);
	set_texture(1, (IDirect3DBaseTexture8 *)lightmap_card);
	draw_triangles(vertices, vertex_count, indices, index_count);
}

static void set_pixel_shader(DWORD shader)
{
	if (shader != d3d.pixel_shader)
	{
		IDirect3DDevice8_SetPixelShader(d3d.device, shader);
		d3d.pixel_shader = shader;
	}
}

/* a water stage: its texture, its coordinates the stage's own set, filtered,
wrapped or clamped, with or without its levels */
static void water_stage(DWORD stage, const struct r_d3d_texture *texture, int wrap, int levels)
{
	set_texture(stage, base_texture(texture));
	set_stage_state(stage, D3DTSS_TEXCOORDINDEX, stage);
	set_stage_state(stage, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
	set_stage_state(stage, D3DTSS_ADDRESSU, wrap ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP);
	set_stage_state(stage, D3DTSS_ADDRESSV, wrap ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP);
	set_stage_state(stage, D3DTSS_ADDRESSW, wrap ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP);
	set_stage_state(stage, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
	set_stage_state(stage, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
	set_stage_state(stage, D3DTSS_MIPFILTER, levels ? D3DTEXF_LINEAR : D3DTEXF_NONE);
	set_stage_state(stage, D3DTSS_MAXANISOTROPY, 1);
}

static void water_triangles(const struct r_d3d_water_vertex *vertices, unsigned long vertex_count,
	const unsigned short *indices, unsigned long index_count)
{
	d3d.draws++;
	if (d3d.behaviour == D3DCREATE_SOFTWARE_VERTEXPROCESSING || vertex_count <= d3d.caps.MaxVertexIndex + 1UL)
	{
		IDirect3DDevice8_DrawIndexedPrimitiveUP(d3d.device, D3DPT_TRIANGLELIST, 0, vertex_count, index_count / 3, indices,
			D3DFMT_INDEX16, vertices, sizeof(*vertices));
		return;
	}
	{
		static struct r_d3d_water_vertex spread[3 * 128];

		for (unsigned long first = 0; first < index_count; first += 3 * 128)
		{
			unsigned long count = index_count - first < 3 * 128 ? index_count - first : 3 * 128;

			for (unsigned long i = 0; i < count; i++)
				spread[i] = vertices[indices[first + i]];
			IDirect3DDevice8_DrawPrimitiveUP(d3d.device, D3DPT_TRIANGLELIST, count / 3, spread, sizeof(*spread));
		}
	}
}

/* Halo's water: the base map multiplying what is under it (where the shader
says), then the reflection added, by the water's program; the ordinary
draws' states put back after */
static void draw_water(const struct r_d3d_water *water, const struct r_d3d_water_vertex *vertices,
	unsigned long vertex_count, const unsigned short *indices, unsigned long index_count)
{
	union
	{
		float value;
		DWORD bits;
	} bias;

	if (!d3d.device || d3d.lost || !d3d.water || index_count < 3 || !vertex_count || !water->bump || !water->reflection ||
		!water->tint)
	{
		return;
	}
	scene_begin();
	set_vertex_shader(d3d.water_vertex_shader);
	render_state(D3DRS_FOGENABLE, FALSE);
	render_state(D3DRS_ALPHATESTENABLE, FALSE);
	render_state(D3DRS_STENCILENABLE, FALSE);
	render_state(D3DRS_ZENABLE, D3DZB_TRUE);
	render_state(D3DRS_ALPHABLENDENABLE, TRUE);
	for (DWORD stage = 1; stage < 6 && stage < d3d.caps.MaxTextureBlendStages; stage++)
		set_texture(stage, NULL);
	/* (what is under the water times the base map, toward no change in the
	fog: the texel where the colour's alpha is 1, white where 0) */
	if (water->base_multiply && water->base)
	{
		set_pixel_shader(0);
		water_stage(0, water->base, FALSE, TRUE);
		set_stage_state(0, D3DTSS_TEXCOORDINDEX, 5);
		set_stage_state(0, D3DTSS_COLOROP, D3DTOP_BLENDDIFFUSEALPHA);
		set_stage_state(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		set_stage_state(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
		set_stage_state(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
		set_stage_state(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
		set_stage_state(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
		set_stage_state(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
		render_state(D3DRS_SRCBLEND, D3DBLEND_ZERO);
		render_state(D3DRS_DESTBLEND, D3DBLEND_SRCCOLOR);
		render_state(D3DRS_ZWRITEENABLE, TRUE);
		water_triangles(vertices, vertex_count, indices, index_count);
	}
	/* the reflection, added */
	bias.value = -water->bump_lod_bias;
	water_stage(0, water->bump, TRUE, TRUE);
	set_stage_state(0, D3DTSS_MIPMAPLODBIAS, bias.bits);
	if (d3d.water >= 2)
	{
		water_stage(1, water->reflection, FALSE, TRUE);
		water_stage(2, water->tint, FALSE, FALSE);
		water_stage(5, water->base_alpha && water->base ? water->base : d3d.white, FALSE, TRUE);
	}
	else
	{
		float tint[4] = { (float)((water->tint_color >> 16) & 0xFF) / 255.0f,
			(float)((water->tint_color >> 8) & 0xFF) / 255.0f, (float)(water->tint_color & 0xFF) / 255.0f, 1.0f };

		set_stage_state(1, D3DTSS_TEXCOORDINDEX, 1);
		set_stage_state(2, D3DTSS_TEXCOORDINDEX, 2);
		water_stage(3, water->reflection, FALSE, TRUE);
		IDirect3DDevice8_SetPixelShaderConstant(d3d.device, 0, tint, 1);
	}
	set_pixel_shader(d3d.water_pixel_shader);
	render_state(D3DRS_SRCBLEND, D3DBLEND_ONE);
	render_state(D3DRS_DESTBLEND, D3DBLEND_ONE);
	render_state(D3DRS_ZWRITEENABLE, FALSE);
	water_triangles(vertices, vertex_count, indices, index_count);
	/* (the ordinary draws' states again) */
	set_pixel_shader(0);
	set_stage_state(0, D3DTSS_MIPMAPLODBIAS, 0);
	for (DWORD stage = 1; stage < 6 && stage < d3d.caps.MaxTextureBlendStages; stage++)
	{
		set_texture(stage, NULL);
		set_stage_state(stage, D3DTSS_TEXCOORDINDEX, stage);
		set_stage_state(stage, D3DTSS_COLOROP, D3DTOP_DISABLE);
		set_stage_state(stage, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
	}
	set_texture(0, NULL);
	stages_for_drawing();
}

/* a pixel of the back buffer's format as 0x00RRGGBB */
static unsigned long pixel_rgb(const unsigned char *row, int x)
{
	unsigned long value;

	switch (d3d.presentation.BackBufferFormat)
	{
	case D3DFMT_R5G6B5:
		value = ((const unsigned short *)row)[x];
		return ((value & 0xF800) << 8) | ((value & 0xE000) << 3) | ((value & 0x07E0) << 5) | ((value & 0x0600) >> 1) |
			((value & 0x001F) << 3) | ((value & 0x001C) >> 2);
	case D3DFMT_X1R5G5B5:
	case D3DFMT_A1R5G5B5:
		value = ((const unsigned short *)row)[x];
		return ((value & 0x7C00) << 9) | ((value & 0x7000) << 4) | ((value & 0x03E0) << 6) | ((value & 0x0380) << 1) |
			((value & 0x001F) << 3) | ((value & 0x001C) >> 2);
	default:
		return ((const unsigned long *)row)[x] & 0x00FFFFFFUL;
	}
}

static int read_back(int x, int y, int width, int height, unsigned long *rgb)
{
	IDirect3DSurface8 *back = NULL;
	D3DLOCKED_RECT locked;

	if (!d3d.device || d3d.lost)
		return FALSE;
	scene_end();
	if (FAILED(IDirect3DDevice8_GetBackBuffer(d3d.device, 0, D3DBACKBUFFER_TYPE_MONO, &back)))
		return FALSE;
	if (FAILED(IDirect3DSurface8_LockRect(back, &locked, NULL, D3DLOCK_READONLY)))
	{
		IDirect3DSurface8_Release(back);
		return FALSE;
	}
	for (int row = 0; row < height; row++)
	{
		int source_y = y + row < (int)d3d.presentation.BackBufferHeight ? y + row :
			(int)d3d.presentation.BackBufferHeight - 1;
		const unsigned char *line = (const unsigned char *)locked.pBits + (size_t)source_y * locked.Pitch;

		for (int column = 0; column < width; column++)
		{
			int source_x = x + column < (int)d3d.presentation.BackBufferWidth ? x + column :
				(int)d3d.presentation.BackBufferWidth - 1;

			rgb[(size_t)row * width + column] = pixel_rgb(line, source_x);
		}
	}
	IDirect3DSurface8_UnlockRect(back);
	IDirect3DSurface8_Release(back);
	return TRUE;
}


/* (testing: the game thread's cycles so far, 0 without the call) */
static ULONG64 thread_cycles(void)
{
	ULONG64 cycles = 0;

	if (d3d.thread_cycles)
		d3d.thread_cycles(GetCurrentThread(), &cycles);
	return cycles;
}

/* ---------- the device */

typedef IDirect3D8 *(WINAPI *direct3d_create_function)(UINT version);

static int depth_format_usable(D3DFORMAT format)
{
	return SUCCEEDED(IDirect3D8_CheckDeviceFormat(d3d.d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
		d3d.presentation.BackBufferFormat, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, format)) &&
		SUCCEEDED(IDirect3D8_CheckDepthStencilMatch(d3d.d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
			d3d.presentation.BackBufferFormat, d3d.presentation.BackBufferFormat, format));
}

/* the depth buffer for the back buffer's format: 24 bits where the card
has them, with a stencil where it can; on a desktop of thousands of colours
most cards of the years have 16 bits alone, some a 1-bit stencil with them,
which is enough */
static void choose_depth_format(void)
{
	D3DFORMAT format = dos_setting_integer("d3d_test_depth", 0) == 16 ? D3DFMT_D16 :
		depth_format_usable(D3DFMT_D24S8) ? D3DFMT_D24S8 : depth_format_usable(D3DFMT_D24X4S4) ? D3DFMT_D24X4S4 :
		depth_format_usable(D3DFMT_D24X8) ? D3DFMT_D24X8 : depth_format_usable(D3DFMT_D32) ? D3DFMT_D32 :
		depth_format_usable(D3DFMT_D15S1) ? D3DFMT_D15S1 : D3DFMT_D16;

	d3d.presentation.AutoDepthStencilFormat = format;
	d3d.stencil = format == D3DFMT_D24S8 || format == D3DFMT_D24X4S4 || format == D3DFMT_D15S1;
}

static void client_size(HWND window, UINT *width, UINT *height)
{
	RECT client;

	GetClientRect(window, &client);
	*width = client.right > 0 ? (UINT)client.right : 1;
	*height = client.bottom > 0 ? (UINT)client.bottom : 1;
}

static void close_device(void)
{
	scene_end();
	if (d3d.white)
	{
		if (d3d.white->texture)
			IDirect3DTexture8_Release(d3d.white->texture);
		free(d3d.white);
	}
	if (d3d.device)
		IDirect3DDevice8_Release(d3d.device);
	if (d3d.d3d)
		IDirect3D8_Release(d3d.d3d);
	if (d3d.library)
		FreeLibrary(d3d.library);
	memset(&d3d, 0, sizeof(d3d));
}

/* (testing, settings d3d_test_*: an older card's limits on this one, so
that its fallbacks can be seen drawn -- d3d_test_stages n texture stages,
d3d_test_size n the largest texture side, d3d_test_aspect n the widest
texture shape, d3d_test_pow2 1 sides powers of two alone, d3d_test_square 1
square textures alone, d3d_test_16 1 16-bit textures, d3d_test_depth 16 a
16-bit depth buffer without a stencil: a Voodoo 3 is stages 2, size 256,
aspect 8, pow2, 16-bit textures and depth) */
static void test_limits(void)
{
	long stages = dos_setting_integer("d3d_test_stages", 0), size = dos_setting_integer("d3d_test_size", 0);
	long aspect = dos_setting_integer("d3d_test_aspect", 0);

	if (stages > 0 && (DWORD)stages < d3d.caps.MaxSimultaneousTextures)
		d3d.caps.MaxSimultaneousTextures = (DWORD)stages;
	if (stages > 0 && (DWORD)stages < d3d.caps.MaxTextureBlendStages)
		d3d.caps.MaxTextureBlendStages = (DWORD)stages;
	if (size > 0 && (DWORD)size < d3d.caps.MaxTextureWidth)
		d3d.caps.MaxTextureWidth = (DWORD)size;
	if (size > 0 && (DWORD)size < d3d.caps.MaxTextureHeight)
		d3d.caps.MaxTextureHeight = (DWORD)size;
	if (aspect > 0)
		d3d.caps.MaxTextureAspectRatio = (DWORD)aspect;
	if (dos_setting_integer("d3d_test_pow2", 0))
		d3d.caps.TextureCaps = (d3d.caps.TextureCaps | D3DPTEXTURECAPS_POW2) & ~(DWORD)D3DPTEXTURECAPS_NONPOW2CONDITIONAL;
	if (dos_setting_integer("d3d_test_square", 0))
		d3d.caps.TextureCaps |= D3DPTEXTURECAPS_SQUAREONLY;
	/* (and textures it would not take refused, as it would) */
	d3d.test_limits = size > 0 || aspect > 0 || dos_setting_integer("d3d_test_pow2", 0) ||
		dos_setting_integer("d3d_test_square", 0);
}

/* (testing: whether the card of test_limits takes a texture of that size) */
static int test_texture_taken(int width, int height)
{
	DWORD aspect = d3d.caps.MaxTextureAspectRatio;

	if (!d3d.test_limits)
		return TRUE;
	if ((DWORD)width > d3d.caps.MaxTextureWidth || (DWORD)height > d3d.caps.MaxTextureHeight)
		return FALSE;
	if (aspect && ((DWORD)width > (DWORD)height * aspect || (DWORD)height > (DWORD)width * aspect))
		return FALSE;
	if ((d3d.caps.TextureCaps & D3DPTEXTURECAPS_POW2) && ((width & (width - 1)) || (height & (height - 1))))
		return FALSE;
	return !(d3d.caps.TextureCaps & D3DPTEXTURECAPS_SQUAREONLY) || width == height;
}

int w95_d3d_open(HWND window)
{
	direct3d_create_function create;
	D3DDISPLAYMODE mode;
	D3DADAPTER_IDENTIFIER8 identifier;
	struct r_d3d_device device;
	HRESULT result;

	memset(&d3d, 0, sizeof(d3d));
	d3d.library = LoadLibraryA("d3d8.dll");
	if (!d3d.library)
	{
		system_log("direct3d: Windows has no d3d8.dll (DirectX 8): the software renderer");
		return FALSE;
	}
	create = (direct3d_create_function)GetProcAddress(d3d.library, "Direct3DCreate8");
	d3d.d3d = create ? create(D3D_SDK_VERSION) : NULL;
	if (!d3d.d3d || FAILED(IDirect3D8_GetAdapterDisplayMode(d3d.d3d, D3DADAPTER_DEFAULT, &mode)) ||
		FAILED(IDirect3D8_GetDeviceCaps(d3d.d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &d3d.caps)))
	{
		system_log("direct3d: no Direct3D 8 hardware device: the software renderer");
		close_device();
		return FALSE;
	}
	test_limits();
	d3d.window = window;
	d3d.debug = (int)dos_setting_integer("debug_d3d", 0);
	if (d3d.debug)
	{
		d3d.thread_cycles = (BOOL (WINAPI *)(HANDLE, ULONG64 *))GetProcAddress(GetModuleHandleA("kernel32.dll"),
			"QueryThreadCycleTime");
		d3d.cycles_last = thread_cycles();
	}
	memset(&d3d.presentation, 0, sizeof(d3d.presentation));
	client_size(window, &d3d.presentation.BackBufferWidth, &d3d.presentation.BackBufferHeight);
	/* (a window's back buffer is in the desktop's format) */
	d3d.presentation.BackBufferFormat = mode.Format;
	d3d.presentation.BackBufferCount = 1;
	d3d.presentation.SwapEffect = D3DSWAPEFFECT_DISCARD;
	d3d.presentation.hDeviceWindow = window;
	d3d.presentation.Windowed = TRUE;
	d3d.presentation.EnableAutoDepthStencil = TRUE;
	choose_depth_format();
	/* (screenshots read it) */
	d3d.presentation.Flags = D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
	/* the vertices are transformed already: a card's own vertex shaders
	help only a little, and software runs the program on any card */
	d3d.behaviour = d3d.caps.VertexShaderVersion >= D3DVS_VERSION(1, 1) &&
		(d3d.caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT) ? D3DCREATE_HARDWARE_VERTEXPROCESSING :
		D3DCREATE_SOFTWARE_VERTEXPROCESSING;
	result = IDirect3D8_CreateDevice(d3d.d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
		d3d.behaviour | D3DCREATE_FPU_PRESERVE, &d3d.presentation, &d3d.device);
	if (FAILED(result) && d3d.behaviour != D3DCREATE_SOFTWARE_VERTEXPROCESSING)
	{
		d3d.behaviour = D3DCREATE_SOFTWARE_VERTEXPROCESSING;
		result = IDirect3D8_CreateDevice(d3d.d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
			d3d.behaviour | D3DCREATE_FPU_PRESERVE, &d3d.presentation, &d3d.device);
	}
	if (FAILED(result))
	{
		log_line("direct3d: no device (%08lx): the software renderer", (unsigned long)result);
		close_device();
		return FALSE;
	}
	if (FAILED(IDirect3DDevice8_CreateVertexShader(d3d.device, vertex_declaration, vertex_function, &d3d.vertex_shader,
		0)))
	{
		system_log("direct3d: the vertex shader could not be made: the software renderer");
		close_device();
		return FALSE;
	}
	/* (the Xbox's texture pass's: without it, that pass is not drawn) */
	if (FAILED(IDirect3DDevice8_CreateVertexShader(d3d.device, vertex_declaration, detail_function, &d3d.detail_shader,
		0)))
	{
		system_log("direct3d: the texture pass's vertex shader could not be made");
		d3d.detail_shader = 0;
	}
	/* the texture formats: 32-bit where the card has it */
	if (format_usable(D3DFMT_A8R8G8B8) && !dos_setting_integer("d3d_test_16", 0))
	{
		d3d.formats[_r_d3d_alpha_none] = d3d.formats[_r_d3d_alpha_test] = d3d.formats[_r_d3d_alpha_full] =
			D3DFMT_A8R8G8B8;
	}
	else
	{
		d3d.formats[_r_d3d_alpha_none] = format_usable(D3DFMT_R5G6B5) ? D3DFMT_R5G6B5 : D3DFMT_A1R5G5B5;
		d3d.formats[_r_d3d_alpha_test] = format_usable(D3DFMT_A1R5G5B5) ? D3DFMT_A1R5G5B5 : D3DFMT_A4R4G4B4;
		d3d.formats[_r_d3d_alpha_full] = D3DFMT_A4R4G4B4;
	}
	d3d.modulate_twice = (d3d.caps.TextureOpCaps & D3DTEXOPCAPS_MODULATE2X) != 0;
	d3d.anisotropy = (d3d.caps.TextureFilterCaps & D3DPTFILTERCAPS_MINFANISOTROPIC) && d3d.caps.MaxAnisotropy > 1 ?
		(d3d.caps.MaxAnisotropy > 8 ? 8 : d3d.caps.MaxAnisotropy) : 1;
	states_initialize();
	/* Halo's water (setting d3d_water: 2, or not given, the best program the
	card runs; 1 Halo's own, pixel shaders 1.1; 0 an ordinary surface): its
	vertex program, its pixel program, a white texel where it has no base
	map; a card without cube maps or pixel shaders draws it as before */
	{
		long wanted = dos_setting_integer("d3d_water", 2);
		int program = 0;

		if (wanted >= 2 && d3d.caps.PixelShaderVersion >= D3DPS_VERSION(1, 4) && d3d.caps.MaxSimultaneousTextures >= 6)
			program = 2;
		else if (wanted >= 1 && d3d.caps.PixelShaderVersion >= D3DPS_VERSION(1, 1) &&
			d3d.caps.MaxSimultaneousTextures >= 4)
		{
			program = 1;
		}
		if (!(d3d.caps.TextureCaps & D3DPTEXTURECAPS_CUBEMAP))
			program = 0;
		if (program && FAILED(IDirect3DDevice8_CreateVertexShader(d3d.device, water_declaration, water_vertex_function,
			&d3d.water_vertex_shader, 0)))
		{
			system_log("direct3d: the water's vertex shader could not be made");
			program = 0;
		}
		if (program == 2 && FAILED(IDirect3DDevice8_CreatePixelShader(d3d.device, water_pixel_14,
			&d3d.water_pixel_shader)))
		{
			system_log("direct3d: the water's pixel shader 1.4 could not be made: Halo's 1.1 one");
			program = 1;
		}
		if (program == 1 && FAILED(IDirect3DDevice8_CreatePixelShader(d3d.device, water_pixel_11,
			&d3d.water_pixel_shader)))
		{
			system_log("direct3d: the water's pixel shader 1.1 could not be made: water as before");
			program = 0;
		}
		if (program)
		{
			static const unsigned long white = 0xFFFFFFFFUL;

			d3d.white = texture_new(1, 1, 1, _r_d3d_alpha_full);
			if (d3d.white)
				texture_write(d3d.white, 0, &white);
			else
				program = 0;
		}
		d3d.water = program;
	}

	memset(&device, 0, sizeof(device));
	device.maximum_texture_size = (int)(d3d.caps.MaxTextureWidth < d3d.caps.MaxTextureHeight ? d3d.caps.MaxTextureWidth :
		d3d.caps.MaxTextureHeight);
	/* (the shapes it takes: a card that takes other sides only on
	conditions takes them without levels, and every texture here has its
	levels) */
	device.maximum_aspect = (int)d3d.caps.MaxTextureAspectRatio;
	device.power_of_two = (d3d.caps.TextureCaps & D3DPTEXTURECAPS_POW2) != 0;
	device.square = (d3d.caps.TextureCaps & D3DPTEXTURECAPS_SQUAREONLY) != 0;
	device.draws_detail = (d3d.caps.TextureOpCaps & D3DTEXOPCAPS_BLENDDIFFUSEALPHA) &&
		(d3d.caps.SrcBlendCaps & D3DPBLENDCAPS_DESTCOLOR) && (d3d.caps.DestBlendCaps & D3DPBLENDCAPS_SRCCOLOR);
	device.draws_lightmaps = d3d.caps.MaxSimultaneousTextures >= 2 && d3d.caps.MaxTextureBlendStages >= 2;
	/* (the Xbox's texture pass: a stage mixing by the alpha so far, the
	picture times the result, and three stages at least) */
	if (d3d.detail_shader && (d3d.caps.TextureOpCaps & D3DTEXOPCAPS_BLENDCURRENTALPHA) &&
		(d3d.caps.SrcBlendCaps & D3DPBLENDCAPS_DESTCOLOR) && (d3d.caps.DestBlendCaps & D3DPBLENDCAPS_SRCCOLOR) &&
		(d3d.caps.DestBlendCaps & D3DPBLENDCAPS_ZERO))
	{
		device.detail_stages = (int)(d3d.caps.MaxSimultaneousTextures < d3d.caps.MaxTextureBlendStages ?
			d3d.caps.MaxSimultaneousTextures : d3d.caps.MaxTextureBlendStages);
		device.detail_stages = device.detail_stages > R_D3D_DETAIL_STAGES ? R_D3D_DETAIL_STAGES : device.detail_stages;
	}
	device.width = (int)d3d.presentation.BackBufferWidth;
	device.height = (int)d3d.presentation.BackBufferHeight;
	device.texture_new = texture_new;
	device.texture_write = texture_write;
	device.texture_delete = texture_delete;
	device.viewport = viewport;
	device.clear = clear;
	device.draw = draw;
	device.read_back = read_back;
	device.water = d3d.water;
	device.cube_new = cube_new;
	device.cube_write = cube_write;
	device.draw_water = draw_water;
	if (!r_d3d_use(&device))
	{
		close_device();
		return FALSE;
	}
	memset(&identifier, 0, sizeof(identifier));
	IDirect3D8_GetAdapterIdentifier(d3d.d3d, D3DADAPTER_DEFAULT, D3DENUM_NO_WHQL_LEVEL, &identifier);
	log_line("direct3d: %s (%s), %ux%u, %s vertex processing, textures %s, largest %d%s%s, shape up to %d:1, "
		"anisotropy %lu, depth %d bits%s, Xbox texture pass %d stages%s%s%s, water program %d",
		identifier.Description, identifier.Driver, d3d.presentation.BackBufferWidth, d3d.presentation.BackBufferHeight,
		d3d.behaviour == D3DCREATE_SOFTWARE_VERTEXPROCESSING ? "software" : "hardware",
		d3d.formats[0] == D3DFMT_A8R8G8B8 ? "32-bit" : "16-bit", device.maximum_texture_size,
		device.power_of_two ? ", powers of two" : "", device.square ? ", square" : "", device.maximum_aspect,
		(unsigned long)d3d.anisotropy, d3d.presentation.AutoDepthStencilFormat == D3DFMT_D16 ||
		d3d.presentation.AutoDepthStencilFormat == D3DFMT_D15S1 ? 16 :
		d3d.presentation.AutoDepthStencilFormat == D3DFMT_D32 ? 32 : 24, d3d.stencil ? " and a stencil" : "",
		device.detail_stages, device.draws_detail ? ", detail maps" : "",
		device.draws_lightmaps ? ", lightmaps" : "",
		d3d.modulate_twice ? "" : ", no MODULATE2X (the light is halved)", d3d.water);
	return TRUE;
}

int w95_d3d_active(void)
{
	return d3d.device != NULL;
}

/* Reset at the window's size (a lost device, or a window of a new size):
TRUE if it is drawable again */
static int reset(void)
{
	D3DDISPLAYMODE mode;

	scene_end();
	client_size(d3d.window, &d3d.presentation.BackBufferWidth, &d3d.presentation.BackBufferHeight);
	/* (a window's back buffer is in the desktop's format, which a mode of
	the screen's own -- the setting fullscreen 2, or another program's --
	may have changed: what lost the device; the depth buffer to match) */
	if (SUCCEEDED(IDirect3D8_GetAdapterDisplayMode(d3d.d3d, D3DADAPTER_DEFAULT, &mode)) &&
		mode.Format != d3d.presentation.BackBufferFormat)
	{
		d3d.presentation.BackBufferFormat = mode.Format;
		choose_depth_format();
		log_line("direct3d: the desktop's format is now %d, the depth buffer's %d", (int)mode.Format,
			(int)d3d.presentation.AutoDepthStencilFormat);
	}
	if (FAILED(IDirect3DDevice8_Reset(d3d.device, &d3d.presentation)))
		return FALSE;
	/* (a Reset puts every state back as it was at the start) */
	memset(&d3d.cache, 0, sizeof(d3d.cache));
	d3d.pixel_shader = 0;
	states_initialize();
	log_line("direct3d: the device made again at %ux%u", d3d.presentation.BackBufferWidth,
		d3d.presentation.BackBufferHeight);
	return TRUE;
}

/* the frame shown; a lost device or a resized window dealt with before the
next */
void w95_d3d_present(void)
{
	UINT width, height;
	HRESULT result;

	if (!d3d.device)
		return;
	r_d3d_flush();
	scene_end();
	if (d3d.lost)
	{
		if (IDirect3DDevice8_TestCooperativeLevel(d3d.device) == D3DERR_DEVICENOTRESET && reset())
		{
			d3d.lost = FALSE;
			system_log("direct3d: the device is back");
		}
		return;
	}
	if (d3d.debug)
	{
		ULONG64 start = thread_cycles(), end;

		result = IDirect3DDevice8_Present(d3d.device, NULL, NULL, NULL, NULL);
		end = thread_cycles();
		d3d.cycles_present += end - start;
		if (++d3d.frames % 300 == 0)
		{
			log_line("direct3d: a frame %lu draws, %lu vertices, %lu indices, %lu of %lu state calls made; "
				"%lu k cycles of the game's thread outside Present, %lu k in it", d3d.draws / 300, d3d.vertices / 300,
				d3d.indices / 300, d3d.calls_made / 300, d3d.calls_asked / 300,
				(unsigned long)((end - d3d.cycles_last - d3d.cycles_present) / 300000),
				(unsigned long)(d3d.cycles_present / 300000));
			d3d.draws = d3d.vertices = d3d.indices = d3d.calls_made = d3d.calls_asked = 0;
			d3d.cycles_last = end;
			d3d.cycles_present = 0;
		}
	}
	else
	{
		result = IDirect3DDevice8_Present(d3d.device, NULL, NULL, NULL, NULL);
	}
	if (result == D3DERR_DEVICELOST)
	{
		d3d.lost = TRUE;
		system_log("direct3d: the device was lost");
		return;
	}
	client_size(d3d.window, &width, &height);
	if (!IsIconic(d3d.window) && (width != d3d.presentation.BackBufferWidth || height != d3d.presentation.BackBufferHeight))
	{
		if (!reset())
			d3d.lost = TRUE;
	}
	/* (testing, setting d3d_test_reset n: the window a quarter larger at
	the n-th frame shown, so that the device is Reset at its new size, as a
	resized window or a lost device has it) */
	{
		static long frames, reset_frame = -1;

		if (reset_frame < 0)
			reset_frame = dos_setting_integer("d3d_test_reset", 0);
		if (reset_frame > 0 && ++frames == reset_frame)
		{
			RECT outer;

			GetWindowRect(d3d.window, &outer);
			SetWindowPos(d3d.window, NULL, 0, 0, (outer.right - outer.left) + (int)width / 4,
				(outer.bottom - outer.top) + (int)height / 4, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
			log_line("direct3d: test: the window made larger at frame %ld", frames);
		}
	}
}

/* the back buffer's size (the picture's rectangle is placed in it) */
void w95_d3d_size(int *width, int *height)
{
	*width = d3d.device ? (int)d3d.presentation.BackBufferWidth : 1;
	*height = d3d.device ? (int)d3d.presentation.BackBufferHeight : 1;
}

void w95_d3d_close(void)
{
	close_device();
}
