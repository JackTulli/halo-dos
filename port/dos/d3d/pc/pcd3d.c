/*
PCD3D.C

The PC half of the Direct3D build's device (pcd3d.h): a PC's Direct3D 8,
from d3d8.dll loaded when the device opens (DirectX 8.0a is the last for
Windows 95; nothing is imported, so the program starts without it).

The vertices come transformed: clip-space positions, colours, fog factors
and the coordinates of each unit, all worked out by the Xbox half on the
processor. A vertex shader of nine moves (vs.1.1, written below as its
tokens: there is no assembler here) hands them on, so that Direct3D still
clips them and maps them to the viewport; a device without vertex shaders
in hardware runs it in software, as Direct3D 8 does for every card of the
Windows 95 years.

The game's screen is 640 by 480 (its targets' pixels are its "logical"
ones); the back buffer is the window's size, and viewports and clears are
scaled to it.
*/

#include "halo_d3d8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "halo_system.h"
#include "pcd3d.h"

/* ---------- the log */

static void pcd3d_log(const char *format, ...)
{
	char line[320];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	system_log(line);
}

/* ---------- the device */

static struct
{
	HMODULE library;
	IDirect3D8 *d3d;
	IDirect3DDevice8 *device;
	D3DPRESENT_PARAMETERS presentation;
	D3DCAPS8 caps;
	struct pcd3d_capabilities capabilities;
	D3DFORMAT back_buffer_format, depth_format, target_format;
	BOOL stencil;
	DWORD vertex_shader;
	IDirect3DSurface8 *back_buffer, *depth_buffer;
	BOOL in_scene;
	BOOL lost;

	/* the target: its texture (NULL: the back buffer), its size and the
	scale from the game's pixels to its own */
	struct pcd3d_texture *target;
	unsigned long target_width, target_height;
	float scale[2];
	BOOL target_depth;

	/* what was last given to Direct3D (0xff..: not known) */
	DWORD render_states[256];
	DWORD stage_states[4][32];
	IDirect3DBaseTexture8 *stage_textures[4];
} pc;

struct pcd3d_texture
{
	IDirect3DBaseTexture8 *base;
	IDirect3DTexture8 *texture;
	IDirect3DCubeTexture8 *cube;
	unsigned long width, height, levels;
	BOOL target;
};

static void states_forget(void)
{
	memset(pc.render_states, 0xff, sizeof(pc.render_states));
	memset(pc.stage_states, 0xff, sizeof(pc.stage_states));
	memset(pc.stage_textures, 0, sizeof(pc.stage_textures));
}

static void render_state(D3DRENDERSTATETYPE state, DWORD value)
{
	if ((unsigned)state < 256 && pc.render_states[state] == value)
		return;
	if ((unsigned)state < 256)
		pc.render_states[state] = value;
	IDirect3DDevice8_SetRenderState(pc.device, state, value);
}

static void stage_state(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
	if (stage < 4 && (unsigned)type < 32)
	{
		if (pc.stage_states[stage][type] == value)
			return;
		pc.stage_states[stage][type] = value;
	}
	IDirect3DDevice8_SetTextureStageState(pc.device, stage, type, value);
}

static void stage_texture(DWORD stage, IDirect3DBaseTexture8 *texture)
{
	if (stage < 4 && pc.stage_textures[stage] == texture)
		return;
	if (stage < 4)
		pc.stage_textures[stage] = texture;
	IDirect3DDevice8_SetTexture(pc.device, stage, texture);
}

static void scene_begin(void)
{
	if (!pc.in_scene && pc.device && !pc.lost)
	{
		IDirect3DDevice8_BeginScene(pc.device);
		pc.in_scene = TRUE;
	}
}

static void scene_end(void)
{
	if (pc.in_scene)
	{
		IDirect3DDevice8_EndScene(pc.device);
		pc.in_scene = FALSE;
	}
}

/* the pass-through program and its vertex: v0 position, v1 diffuse, v2
specular, v3 fog, v4-v7 the units' coordinates (struct pcd3d_vertex) */
static const DWORD vertex_declaration[] =
{
	D3DVSD_STREAM(0),
	D3DVSD_REG(0, D3DVSDT_FLOAT4),
	D3DVSD_REG(1, D3DVSDT_D3DCOLOR),
	D3DVSD_REG(2, D3DVSDT_D3DCOLOR),
	D3DVSD_REG(3, D3DVSDT_FLOAT1),
	D3DVSD_REG(4, D3DVSDT_FLOAT4),
	D3DVSD_REG(5, D3DVSDT_FLOAT4),
	D3DVSD_REG(6, D3DVSDT_FLOAT4),
	D3DVSD_REG(7, D3DVSDT_FLOAT4),
	D3DVSD_END()
};

/* tokens: the version, then each instruction's opcode (mov is 1), its
destination (0x80000000 | register type << 28 | write mask << 16 |
number: oPos and oFog are rasterizer outputs 4 (0 and 1), oDn attribute
outputs 5, oTn texture coordinate outputs 6) and its source (input
registers 1, swizzle at bit 16: 0xe4 is xyzw, 0 is xxxx) */
static const DWORD vertex_function[] =
{
	0xfffe0101,
	0x00000001, 0xc00f0000, 0x90e40000, /* mov oPos, v0 */
	0x00000001, 0xd00f0000, 0x90e40001, /* mov oD0, v1 */
	0x00000001, 0xd00f0001, 0x90e40002, /* mov oD1, v2 */
	0x00000001, 0xc0010001, 0x90000003, /* mov oFog.x, v3.x */
	0x00000001, 0xe00f0000, 0x90e40004, /* mov oT0, v4 */
	0x00000001, 0xe00f0001, 0x90e40005, /* mov oT1, v5 */
	0x00000001, 0xe00f0002, 0x90e40006, /* mov oT2, v6 */
	0x00000001, 0xe00f0003, 0x90e40007, /* mov oT3, v7 */
	0x0000ffff
};

typedef IDirect3D8 *(WINAPI *direct3d_create_function)(UINT version);

static void capabilities_read(void)
{
	struct pcd3d_capabilities *c = &pc.capabilities;
	D3DADAPTER_IDENTIFIER8 identifier;
	DWORD operations = pc.caps.TextureOpCaps;
	static const struct
	{
		int operation;
		DWORD cap;
	} table[] =
	{
		{ PCD3D_TOP_DISABLE, D3DTEXOPCAPS_DISABLE },
		{ PCD3D_TOP_SELECTARG1, D3DTEXOPCAPS_SELECTARG1 },
		{ PCD3D_TOP_SELECTARG2, D3DTEXOPCAPS_SELECTARG2 },
		{ PCD3D_TOP_MODULATE, D3DTEXOPCAPS_MODULATE },
		{ PCD3D_TOP_MODULATE2X, D3DTEXOPCAPS_MODULATE2X },
		{ PCD3D_TOP_MODULATE4X, D3DTEXOPCAPS_MODULATE4X },
		{ PCD3D_TOP_ADD, D3DTEXOPCAPS_ADD },
		{ PCD3D_TOP_ADDSIGNED, D3DTEXOPCAPS_ADDSIGNED },
		{ PCD3D_TOP_ADDSIGNED2X, D3DTEXOPCAPS_ADDSIGNED2X },
		{ PCD3D_TOP_SUBTRACT, D3DTEXOPCAPS_SUBTRACT },
		{ PCD3D_TOP_BLENDDIFFUSEALPHA, D3DTEXOPCAPS_BLENDDIFFUSEALPHA },
		{ PCD3D_TOP_BLENDTEXTUREALPHA, D3DTEXOPCAPS_BLENDTEXTUREALPHA },
		{ PCD3D_TOP_BLENDFACTORALPHA, D3DTEXOPCAPS_BLENDFACTORALPHA },
		{ PCD3D_TOP_BLENDCURRENTALPHA, D3DTEXOPCAPS_BLENDCURRENTALPHA },
		{ PCD3D_TOP_DOTPRODUCT3, D3DTEXOPCAPS_DOTPRODUCT3 },
		{ PCD3D_TOP_LERP, D3DTEXOPCAPS_LERP },
	};
	unsigned i;

	memset(c, 0, sizeof(*c));
	c->units = pc.caps.MaxSimultaneousTextures;
	if (c->units > pc.caps.MaxTextureBlendStages)
		c->units = pc.caps.MaxTextureBlendStages;
	if (c->units > 4)
		c->units = 4;
	if (!c->units)
		c->units = 1;
	for (i = 0; i < sizeof(table) / sizeof(table[0]); i++)
		if (operations & table[i].cap)
			c->operations |= PCD3D_OPERATION(table[i].operation);
	c->blend_operations = (pc.caps.PrimitiveMiscCaps & D3DPMISCCAPS_BLENDOP) != 0;
	c->cube_maps = (pc.caps.TextureCaps & D3DPTEXTURECAPS_CUBEMAP) != 0;
	c->volume_maps = (pc.caps.TextureCaps & D3DPTEXTURECAPS_VOLUMEMAP) != 0;
	c->power_of_two = (pc.caps.TextureCaps & D3DPTEXTURECAPS_POW2) &&
		!(pc.caps.TextureCaps & D3DPTEXTURECAPS_NONPOW2CONDITIONAL);
	c->square = (pc.caps.TextureCaps & D3DPTEXTURECAPS_SQUAREONLY) != 0;
	c->maximum_texture_size = pc.caps.MaxTextureWidth < pc.caps.MaxTextureHeight ? pc.caps.MaxTextureWidth :
		pc.caps.MaxTextureHeight;
	c->destination_alpha = pc.back_buffer_format == D3DFMT_A8R8G8B8;
	c->color_write = (pc.caps.PrimitiveMiscCaps & D3DPMISCCAPS_COLORWRITEENABLE) != 0;
	c->render_target_textures = pc.target_format != D3DFMT_UNKNOWN;
	memset(&identifier, 0, sizeof(identifier));
	IDirect3D8_GetAdapterIdentifier(pc.d3d, D3DADAPTER_DEFAULT, D3DENUM_NO_WHQL_LEVEL, &identifier);
	snprintf(c->description, sizeof(c->description), "%s (%s)", identifier.Description, identifier.Driver);
}

static BOOL depth_format_usable(D3DFORMAT format)
{
	return SUCCEEDED(IDirect3D8_CheckDeviceFormat(pc.d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, pc.back_buffer_format,
		D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, format)) &&
		SUCCEEDED(IDirect3D8_CheckDepthStencilMatch(pc.d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
			pc.back_buffer_format, pc.back_buffer_format, format));
}

int pcd3d_open(void *window, unsigned long width, unsigned long height)
{
	direct3d_create_function create;
	D3DDISPLAYMODE mode;
	DWORD behaviour;
	HRESULT result;

	memset(&pc, 0, sizeof(pc));
	pc.library = LoadLibraryA("d3d8.dll");
	if (!pc.library)
	{
		pcd3d_log("Direct3D: no d3d8.dll (DirectX 8 is not installed)");
		return 0;
	}
	create = (direct3d_create_function)GetProcAddress(pc.library, "Direct3DCreate8");
	pc.d3d = create ? create(D3D_SDK_VERSION) : NULL;
	if (!pc.d3d)
	{
		pcd3d_log("Direct3D: Direct3DCreate8 failed");
		return 0;
	}
	if (FAILED(IDirect3D8_GetAdapterDisplayMode(pc.d3d, D3DADAPTER_DEFAULT, &mode)) ||
		FAILED(IDirect3D8_GetDeviceCaps(pc.d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &pc.caps)))
	{
		pcd3d_log("Direct3D: no hardware device");
		return 0;
	}
	/* (a window's back buffer is in the desktop's format) */
	pc.back_buffer_format = mode.Format;
	pc.depth_format = depth_format_usable(D3DFMT_D24S8) ? D3DFMT_D24S8 :
		depth_format_usable(D3DFMT_D24X4S4) ? D3DFMT_D24X4S4 :
		depth_format_usable(D3DFMT_D15S1) ? D3DFMT_D15S1 :
		depth_format_usable(D3DFMT_D24X8) ? D3DFMT_D24X8 : D3DFMT_D16;
	pc.stencil = pc.depth_format == D3DFMT_D24S8 || pc.depth_format == D3DFMT_D24X4S4 ||
		pc.depth_format == D3DFMT_D15S1;
	pc.target_format = SUCCEEDED(IDirect3D8_CheckDeviceFormat(pc.d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
		pc.back_buffer_format, D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8)) ? D3DFMT_A8R8G8B8 :
		SUCCEEDED(IDirect3D8_CheckDeviceFormat(pc.d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, pc.back_buffer_format,
			D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, pc.back_buffer_format)) ? pc.back_buffer_format : D3DFMT_UNKNOWN;

	memset(&pc.presentation, 0, sizeof(pc.presentation));
	pc.presentation.BackBufferWidth = width;
	pc.presentation.BackBufferHeight = height;
	pc.presentation.BackBufferFormat = pc.back_buffer_format;
	pc.presentation.BackBufferCount = 1;
	pc.presentation.SwapEffect = D3DSWAPEFFECT_DISCARD;
	pc.presentation.hDeviceWindow = (HWND)window;
	pc.presentation.Windowed = TRUE;
	pc.presentation.EnableAutoDepthStencil = TRUE;
	pc.presentation.AutoDepthStencilFormat = pc.depth_format;
	/* (screenshots read it) */
	pc.presentation.Flags = D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
	/* the vertices are transformed already: a card's own vertex shaders
	help only a little, and software runs the program on any card */
	behaviour = pc.caps.VertexShaderVersion >= D3DVS_VERSION(1, 1) &&
		(pc.caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT) ? D3DCREATE_HARDWARE_VERTEXPROCESSING :
		D3DCREATE_SOFTWARE_VERTEXPROCESSING;
	result = IDirect3D8_CreateDevice(pc.d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, (HWND)window,
		behaviour | D3DCREATE_FPU_PRESERVE, &pc.presentation, &pc.device);
	if (FAILED(result) && behaviour != D3DCREATE_SOFTWARE_VERTEXPROCESSING)
	{
		behaviour = D3DCREATE_SOFTWARE_VERTEXPROCESSING;
		result = IDirect3D8_CreateDevice(pc.d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, (HWND)window,
			behaviour | D3DCREATE_FPU_PRESERVE, &pc.presentation, &pc.device);
	}
	if (FAILED(result))
	{
		pcd3d_log("Direct3D: CreateDevice failed (%08lx)", (unsigned long)result);
		return 0;
	}
	result = IDirect3DDevice8_CreateVertexShader(pc.device, vertex_declaration, vertex_function, &pc.vertex_shader, 0);
	if (FAILED(result))
	{
		pcd3d_log("Direct3D: the vertex shader failed (%08lx)", (unsigned long)result);
		return 0;
	}
	IDirect3DDevice8_GetBackBuffer(pc.device, 0, D3DBACKBUFFER_TYPE_MONO, &pc.back_buffer);
	IDirect3DDevice8_GetDepthStencilSurface(pc.device, &pc.depth_buffer);
	capabilities_read();
	states_forget();
	IDirect3DDevice8_SetVertexShader(pc.device, pc.vertex_shader);
	render_state(D3DRS_LIGHTING, FALSE);
	render_state(D3DRS_CLIPPING, TRUE);
	render_state(D3DRS_FOGTABLEMODE, D3DFOG_NONE);
	render_state(D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
	render_state(D3DRS_DITHERENABLE, TRUE);
	pcd3d_target(NULL, TRUE, 640, 480);
	pcd3d_log("Direct3D: %s, %lux%lu, back buffer format %lu, depth %lu, targets %lu, %s vertex processing; "
		"%lu units, operations %08lx, vertex shaders %lu.%lu, pixel shaders %lu.%lu",
		pc.capabilities.description, width, height, (unsigned long)pc.back_buffer_format,
		(unsigned long)pc.depth_format, (unsigned long)pc.target_format,
		behaviour == D3DCREATE_SOFTWARE_VERTEXPROCESSING ? "software" : "hardware", pc.capabilities.units,
		pc.capabilities.operations, (unsigned long)(pc.caps.VertexShaderVersion >> 8) & 0xff,
		(unsigned long)pc.caps.VertexShaderVersion & 0xff, (unsigned long)(pc.caps.PixelShaderVersion >> 8) & 0xff,
		(unsigned long)pc.caps.PixelShaderVersion & 0xff);
	return 1;
}

void pcd3d_close(void)
{
	scene_end();
	if (pc.back_buffer)
		IDirect3DSurface8_Release(pc.back_buffer);
	if (pc.depth_buffer)
		IDirect3DSurface8_Release(pc.depth_buffer);
	if (pc.device)
		IDirect3DDevice8_Release(pc.device);
	if (pc.d3d)
		IDirect3D8_Release(pc.d3d);
	memset(&pc, 0, sizeof(pc));
}

void pcd3d_capabilities(struct pcd3d_capabilities *capabilities)
{
	*capabilities = pc.capabilities;
}

void pcd3d_size(unsigned long *width, unsigned long *height)
{
	*width = pc.presentation.BackBufferWidth;
	*height = pc.presentation.BackBufferHeight;
}

void pcd3d_present(void)
{
	HRESULT result;

	if (!pc.device)
		return;
	scene_end();
	if (pc.lost)
	{
		/* (a window loses its device when the desktop's mode changes;
		nothing that would need making again is kept in video memory
		but the targets, which are left as they are for now) */
		if (IDirect3DDevice8_TestCooperativeLevel(pc.device) == D3DERR_DEVICENOTRESET &&
			SUCCEEDED(IDirect3DDevice8_Reset(pc.device, &pc.presentation)))
		{
			pc.lost = FALSE;
			states_forget();
			IDirect3DDevice8_SetVertexShader(pc.device, pc.vertex_shader);
			pcd3d_log("Direct3D: the device is back");
		}
		return;
	}
	result = IDirect3DDevice8_Present(pc.device, NULL, NULL, NULL, NULL);
	if (result == D3DERR_DEVICELOST)
	{
		pc.lost = TRUE;
		pcd3d_log("Direct3D: the device was lost");
	}
}

/* a pixel of the back buffer's format as ARGB */
static unsigned long pixel_argb(const unsigned char *row, unsigned long x)
{
	unsigned long value;

	switch (pc.back_buffer_format)
	{
	case D3DFMT_R5G6B5:
		value = ((const unsigned short *)row)[x];
		return 0xff000000UL | ((value & 0xf800) << 8) | ((value & 0xe000) << 3) | ((value & 0x07e0) << 5) |
			((value & 0x0600) >> 1) | ((value & 0x001f) << 3) | ((value & 0x001c) >> 2);
	case D3DFMT_X1R5G5B5:
	case D3DFMT_A1R5G5B5:
		value = ((const unsigned short *)row)[x];
		return 0xff000000UL | ((value & 0x7c00) << 9) | ((value & 0x7000) << 4) | ((value & 0x03e0) << 6) |
			((value & 0x0380) << 1) | ((value & 0x001f) << 3) | ((value & 0x001c) >> 2);
	default:
		return ((const unsigned long *)row)[x] | 0xff000000UL;
	}
}

int pcd3d_read_back(unsigned long *argb, unsigned long width, unsigned long height)
{
	D3DLOCKED_RECT locked;
	unsigned long x, y;

	if (!pc.back_buffer || pc.lost)
		return 0;
	scene_end();
	if (FAILED(IDirect3DSurface8_LockRect(pc.back_buffer, &locked, NULL, D3DLOCK_READONLY)))
		return 0;
	for (y = 0; y < height && y < pc.presentation.BackBufferHeight; y++)
	{
		const unsigned char *row = (const unsigned char *)locked.pBits + y * locked.Pitch;

		for (x = 0; x < width && x < pc.presentation.BackBufferWidth; x++)
			argb[y * width + x] = pixel_argb(row, x);
	}
	IDirect3DSurface8_UnlockRect(pc.back_buffer);
	return 1;
}

/* ---------- textures */

struct pcd3d_texture *pcd3d_texture_new(unsigned long width, unsigned long height, unsigned long levels,
	int cube_map, int target)
{
	struct pcd3d_texture *texture;
	HRESULT result;

	if (!pc.device || (target && pc.target_format == D3DFMT_UNKNOWN))
		return NULL;
	texture = calloc(1, sizeof(*texture));
	if (!texture)
		return NULL;
	texture->width = width;
	texture->height = height;
	texture->levels = target ? 1 : levels;
	texture->target = target;
	if (cube_map)
	{
		result = IDirect3DDevice8_CreateCubeTexture(pc.device, width, texture->levels,
			target ? D3DUSAGE_RENDERTARGET : 0, target ? pc.target_format : D3DFMT_A8R8G8B8,
			target ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED, &texture->cube);
		texture->base = (IDirect3DBaseTexture8 *)texture->cube;
	}
	else
	{
		result = IDirect3DDevice8_CreateTexture(pc.device, width, height, texture->levels,
			target ? D3DUSAGE_RENDERTARGET : 0, target ? pc.target_format : D3DFMT_A8R8G8B8,
			target ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED, &texture->texture);
		texture->base = (IDirect3DBaseTexture8 *)texture->texture;
	}
	if (FAILED(result))
	{
		pcd3d_log("Direct3D: a %lux%lu%s texture failed (%08lx)", width, height,
			target ? " target" : cube_map ? " cube" : "", (unsigned long)result);
		free(texture);
		return NULL;
	}
	return texture;
}

void pcd3d_texture_delete(struct pcd3d_texture *texture)
{
	int stage;

	if (!texture)
		return;
	for (stage = 0; stage < 4; stage++)
	{
		if (pc.stage_textures[stage] == texture->base)
			stage_texture(stage, NULL);
	}
	if (pc.target == texture)
		pcd3d_target(NULL, TRUE, 640, 480);
	if (texture->base)
		IDirect3DBaseTexture8_Release(texture->base);
	free(texture);
}

void *pcd3d_texture_lock(struct pcd3d_texture *texture, int face, int level, unsigned long *pitch)
{
	D3DLOCKED_RECT locked;
	HRESULT result;

	if (!texture || texture->target)
		return NULL;
	result = texture->cube ?
		IDirect3DCubeTexture8_LockRect(texture->cube, (D3DCUBEMAP_FACES)face, level, &locked, NULL, 0) :
		IDirect3DTexture8_LockRect(texture->texture, level, &locked, NULL, 0);
	if (FAILED(result))
		return NULL;
	*pitch = (unsigned long)locked.Pitch;
	return locked.pBits;
}

void pcd3d_texture_unlock(struct pcd3d_texture *texture, int face, int level)
{
	if (texture->cube)
		IDirect3DCubeTexture8_UnlockRect(texture->cube, (D3DCUBEMAP_FACES)face, level);
	else
		IDirect3DTexture8_UnlockRect(texture->texture, level);
}

void pcd3d_texture_size(const struct pcd3d_texture *texture, unsigned long *width, unsigned long *height)
{
	*width = texture->width;
	*height = texture->height;
}

void pcd3d_texture_copy_back_buffer(struct pcd3d_texture *texture)
{
	IDirect3DSurface8 *surface = NULL;

	if (!texture || !texture->texture || !pc.back_buffer)
		return;
	if (SUCCEEDED(IDirect3DTexture8_GetSurfaceLevel(texture->texture, 0, &surface)))
	{
		/* (the formats must agree: a copy of another format stays as it was) */
		IDirect3DDevice8_CopyRects(pc.device, pc.back_buffer, NULL, 0, surface, NULL);
		IDirect3DSurface8_Release(surface);
	}
}

/* ---------- targets */

void pcd3d_target(struct pcd3d_texture *texture, int depth, unsigned long logical_width, unsigned long logical_height)
{
	IDirect3DSurface8 *surface = NULL;

	if (!pc.device)
		return;
	if (texture && texture->texture && texture->target &&
		SUCCEEDED(IDirect3DTexture8_GetSurfaceLevel(texture->texture, 0, &surface)))
	{
		/* (the depth buffer goes with a target no larger than it) */
		depth = depth && texture->width <= pc.presentation.BackBufferWidth &&
			texture->height <= pc.presentation.BackBufferHeight;
		IDirect3DDevice8_SetRenderTarget(pc.device, surface, depth ? pc.depth_buffer : NULL);
		IDirect3DSurface8_Release(surface);
		pc.target = texture;
		pc.target_width = texture->width;
		pc.target_height = texture->height;
	}
	else
	{
		IDirect3DDevice8_SetRenderTarget(pc.device, pc.back_buffer, pc.depth_buffer);
		pc.target = NULL;
		pc.target_width = pc.presentation.BackBufferWidth;
		pc.target_height = pc.presentation.BackBufferHeight;
		depth = TRUE;
	}
	pc.target_depth = depth;
	pc.scale[0] = logical_width ? (float)pc.target_width / (float)logical_width : 1.0f;
	pc.scale[1] = logical_height ? (float)pc.target_height / (float)logical_height : 1.0f;
}

static DWORD scaled(float value, int axis, unsigned long limit)
{
	long pixel = (long)(value * pc.scale[axis] + 0.5f);

	if (pixel < 0)
		pixel = 0;
	if ((unsigned long)pixel > limit)
		pixel = (long)limit;
	return (DWORD)pixel;
}

void pcd3d_viewport(unsigned long x, unsigned long y, unsigned long width, unsigned long height,
	float minimum_z, float maximum_z)
{
	D3DVIEWPORT8 viewport;

	if (!pc.device)
		return;
	viewport.X = scaled((float)x, 0, pc.target_width);
	viewport.Y = scaled((float)y, 1, pc.target_height);
	viewport.Width = scaled((float)(x + width), 0, pc.target_width) - viewport.X;
	viewport.Height = scaled((float)(y + height), 1, pc.target_height) - viewport.Y;
	viewport.MinZ = minimum_z;
	viewport.MaxZ = maximum_z;
	if (!viewport.Width || !viewport.Height)
		return;
	IDirect3DDevice8_SetViewport(pc.device, &viewport);
}

void pcd3d_clear(unsigned long count, const long *rectangles, unsigned long flags, unsigned long argb, float z,
	unsigned long stencil)
{
	D3DRECT boxes[16];
	DWORD d3d_flags = 0;
	unsigned long index;

	if (!pc.device || pc.lost)
		return;
	if (flags & PCD3D_CLEAR_TARGET)
		d3d_flags |= D3DCLEAR_TARGET;
	if ((flags & PCD3D_CLEAR_ZBUFFER) && pc.target_depth)
		d3d_flags |= D3DCLEAR_ZBUFFER;
	if ((flags & PCD3D_CLEAR_STENCIL) && pc.target_depth && pc.stencil)
		d3d_flags |= D3DCLEAR_STENCIL;
	if (!d3d_flags)
		return;
	if (count > 16)
		count = 16;
	for (index = 0; index < count; index++)
	{
		boxes[index].x1 = scaled((float)rectangles[index * 4 + 0], 0, pc.target_width);
		boxes[index].y1 = scaled((float)rectangles[index * 4 + 1], 1, pc.target_height);
		boxes[index].x2 = scaled((float)rectangles[index * 4 + 2], 0, pc.target_width);
		boxes[index].y2 = scaled((float)rectangles[index * 4 + 3], 1, pc.target_height);
	}
	scene_begin();
	IDirect3DDevice8_Clear(pc.device, count, count ? boxes : NULL, d3d_flags, (D3DCOLOR)argb, z, stencil);
}

/* ---------- drawing */

static void apply_state(const struct pcd3d_state *state)
{
	unsigned long unit;

	render_state(D3DRS_ZENABLE, state->z_enable ? D3DZB_TRUE : D3DZB_FALSE);
	render_state(D3DRS_ZWRITEENABLE, state->z_write);
	render_state(D3DRS_ZFUNC, state->z_function);
	render_state(D3DRS_CULLMODE, state->cull);
	render_state(D3DRS_FILLMODE, state->fill);
	render_state(D3DRS_ALPHABLENDENABLE, state->blend_enable);
	if (state->blend_enable)
	{
		render_state(D3DRS_SRCBLEND, state->source_blend);
		render_state(D3DRS_DESTBLEND, state->destination_blend);
		if (pc.capabilities.blend_operations)
			render_state(D3DRS_BLENDOP, state->blend_operation);
	}
	render_state(D3DRS_ALPHATESTENABLE, state->alpha_test);
	if (state->alpha_test)
	{
		render_state(D3DRS_ALPHAFUNC, state->alpha_function);
		render_state(D3DRS_ALPHAREF, state->alpha_reference);
	}
	if (pc.capabilities.color_write)
		render_state(D3DRS_COLORWRITEENABLE, state->color_write);
	render_state(D3DRS_STENCILENABLE, state->stencil_enable && pc.stencil && pc.target_depth);
	if (state->stencil_enable && pc.stencil)
	{
		render_state(D3DRS_STENCILFUNC, state->stencil_function);
		render_state(D3DRS_STENCILFAIL, state->stencil_fail);
		render_state(D3DRS_STENCILZFAIL, state->stencil_z_fail);
		render_state(D3DRS_STENCILPASS, state->stencil_pass);
		render_state(D3DRS_STENCILREF, state->stencil_reference);
		render_state(D3DRS_STENCILMASK, state->stencil_mask);
		render_state(D3DRS_STENCILWRITEMASK, state->stencil_write_mask);
	}
	render_state(D3DRS_FOGENABLE, state->fog_enable);
	if (state->fog_enable)
		render_state(D3DRS_FOGCOLOR, state->fog_color);
	render_state(D3DRS_SPECULARENABLE, state->specular);
	render_state(D3DRS_ZBIAS, state->depth_bias);
	render_state(D3DRS_TEXTUREFACTOR, state->texture_factor);

	for (unit = 0; unit < 4; unit++)
	{
		const struct pcd3d_unit *u = &state->units[unit];

		if (unit >= state->unit_count)
		{
			stage_state(unit, D3DTSS_COLOROP, D3DTOP_DISABLE);
			stage_state(unit, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
			stage_texture(unit, NULL);
			continue;
		}
		stage_texture(unit, u->sampler.texture ? u->sampler.texture->base : NULL);
		stage_state(unit, D3DTSS_COLOROP, u->color_operation);
		stage_state(unit, D3DTSS_COLORARG1, u->color_arguments[1]);
		stage_state(unit, D3DTSS_COLORARG2, u->color_arguments[2]);
		stage_state(unit, D3DTSS_COLORARG0, u->color_arguments[0]);
		stage_state(unit, D3DTSS_ALPHAOP, u->alpha_operation);
		stage_state(unit, D3DTSS_ALPHAARG1, u->alpha_arguments[1]);
		stage_state(unit, D3DTSS_ALPHAARG2, u->alpha_arguments[2]);
		stage_state(unit, D3DTSS_ALPHAARG0, u->alpha_arguments[0]);
		stage_state(unit, D3DTSS_TEXCOORDINDEX, unit);
		if (u->sampler.texture)
		{
			stage_state(unit, D3DTSS_ADDRESSU, u->sampler.address[0]);
			stage_state(unit, D3DTSS_ADDRESSV, u->sampler.address[1]);
			stage_state(unit, D3DTSS_ADDRESSW, u->sampler.address[2]);
			stage_state(unit, D3DTSS_MAGFILTER, u->sampler.magnification);
			stage_state(unit, D3DTSS_MINFILTER, u->sampler.minification);
			stage_state(unit, D3DTSS_MIPFILTER, u->sampler.texture->levels > 1 ? u->sampler.mip : D3DTEXF_NONE);
			stage_state(unit, D3DTSS_MAXANISOTROPY, u->sampler.maximum_anisotropy ? u->sampler.maximum_anisotropy : 1);
			stage_state(unit, D3DTSS_BORDERCOLOR, u->sampler.border);
			{
				DWORD bias;

				memcpy(&bias, &u->sampler.lod_bias, sizeof(bias));
				stage_state(unit, D3DTSS_MIPMAPLODBIAS, bias);
			}
			stage_state(unit, D3DTSS_TEXTURETRANSFORMFLAGS, u->sampler.projected ?
				D3DTTFF_PROJECTED | (u->sampler.components > 3 ? D3DTTFF_COUNT4 : D3DTTFF_COUNT3) : D3DTTFF_DISABLE);
		}
	}
}

void pcd3d_draw(const struct pcd3d_state *state, int primitive, const struct pcd3d_vertex *vertices,
	unsigned long vertex_count, const unsigned short *indices, unsigned long index_count)
{
	unsigned long primitives, count = indices ? index_count : vertex_count;

	if (!pc.device || pc.lost || !vertex_count)
		return;
	switch (primitive)
	{
	case PCD3D_POINTLIST: primitives = count; break;
	case PCD3D_LINELIST: primitives = count / 2; break;
	case PCD3D_LINESTRIP: primitives = count > 1 ? count - 1 : 0; break;
	case PCD3D_TRIANGLELIST: primitives = count / 3; break;
	default: primitives = count > 2 ? count - 2 : 0; break;
	}
	if (!primitives)
		return;
	scene_begin();
	apply_state(state);
	if (indices)
		IDirect3DDevice8_DrawIndexedPrimitiveUP(pc.device, (D3DPRIMITIVETYPE)primitive, 0, vertex_count, primitives,
			indices, D3DFMT_INDEX16, vertices, sizeof(*vertices));
	else
		IDirect3DDevice8_DrawPrimitiveUP(pc.device, (D3DPRIMITIVETYPE)primitive, primitives, vertices,
			sizeof(*vertices));
}
