/*
XD3D_DEVICE.C

The Xbox's Direct3D device for the Direct3D build (xd3d.h): its state, its
vertex programs, draws, clears, presentation and vertical blank, over the
PC half (pcd3d.h). Its semantics follow ChupathingyCE's
port/linux/src/d3d8_gl.c, the reference for every entry point here.

A draw:
- runs the vertex program on the processor (nv2a_vsh_cpu.c) over the
  vertices it uses: clip-space positions, the four stages' coordinates,
  the two colours and the fog coordinate;
- turns the pixel state (texture modes, register combiners, final
  combiner, alpha test, blend) into passes of texture stages
  (nv2a_combiner_ff.c, halo-ce-universal's planner for OpenGL 1.1's
  combine environments, which a PC's Direct3D stages compute alike), each
  with its own vertex colours worked out from the program's;
- hands each pass to the PC half.
*/

#include "xd3d.h"
#include "nv2a_vsh_cpu.h"
#include "nv2a_combiner_ff.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the settings (port/dos/xdk/dos_settings.c) */
long dos_setting_integer(const char *name, long default_value);
/* the window (port/dos/win95/w95_system.c) */
void *system_display_window(void);
/* (xd3d_textures.c) */
extern unsigned long xd3d_back_buffer_changes;

DWORD D3D__RenderState[D3DRS_MAX];
DWORD D3D__TextureState[D3DTSS_MAXSTAGES][D3DTSS_MAX];
WORD *D3D__IndexData;

#define VERTEX_SHADER_SIGNATURE 0x76736864UL /* 'vshd' */
#define VERTEX_PROGRAM_SLOTS 136
#define SCREEN_WIDTH 640
#define SCREEN_HEIGHT 480

struct vertex_element
{
	unsigned char reg;
	unsigned char stream;
	unsigned char type;
	unsigned char bytes;
	unsigned short offset;
};

struct vertex_shader_object
{
	unsigned long signature;
	DWORD *instructions;
	unsigned long instruction_count;
	struct vertex_element elements[XGPU_VERTEX_ATTRIBUTE_COUNT];
	unsigned long element_count;
	/* the program for the processor, made at its first draw */
	struct nv2a_vsh_cpu_program *program;
	BOOL program_failed;
};

static struct
{
	BOOL created;
	/* the PC half is there to draw */
	BOOL drawing;
	struct pcd3d_capabilities capabilities;
	struct nv2a_ff_capabilities plan_capabilities;

	D3DSurface back_buffer, depth_buffer;
	D3DSurface *render_target, *depth_stencil;
	D3DVIEWPORT8 viewport;
	DWORD shader_constant_mode;
	float constants[XGPU_VERTEX_CONSTANT_COUNT][4];
	float viewport_scale[4], viewport_offset[4];

	struct vertex_shader_object *vertex_shader;
	struct vertex_shader_object *program_slots[VERTEX_PROGRAM_SLOTS];
	DWORD program_address;
	struct
	{
		DWORD data;
		UINT stride;
	} streams[16];
	UINT base_vertex_index;
	D3DBaseTexture *textures[D3DTSS_MAXSTAGES];
	D3DPalette *palettes[D3DTSS_MAXSTAGES];

	/* immediate mode: the registers' values, and the vertices so far */
	float attributes[XGPU_VERTEX_ATTRIBUTE_COUNT][4];
	BOOL immediate_active;
	D3DPRIMITIVETYPE immediate_type;
	float (*immediate_vertices)[XGPU_VERTEX_ATTRIBUTE_COUNT][4];
	unsigned long immediate_count, immediate_capacity;

	/* the target and viewport the PC half was last given */
	BOOL target_dirty;
	BOOL target_usable;
	BOOL target_is_back_buffer;

	unsigned long frame;
} device;

/* ---------- small things */

static float dword_to_float(DWORD value)
{
	float result;

	memcpy(&result, &value, sizeof(result));
	return result;
}

static void color_to_vec4(D3DCOLOR color, float *out)
{
	out[0] = ((color >> 16) & 0xff) / 255.0f;
	out[1] = ((color >> 8) & 0xff) / 255.0f;
	out[2] = (color & 0xff) / 255.0f;
	out[3] = ((color >> 24) & 0xff) / 255.0f;
}

static unsigned long channel_byte(float value)
{
	if (!(value > 0.0f))
		return 0;
	if (value >= 1.0f)
		return 255;
	return (unsigned long)(value * 255.0f + 0.5f);
}

static unsigned long vec4_to_argb(const float *color)
{
	return (channel_byte(color[3]) << 24) | (channel_byte(color[0]) << 16) | (channel_byte(color[1]) << 8) |
		channel_byte(color[2]);
}

/* an Xbox comparison (OpenGL's numbers) as the PC's */
static unsigned char compare_function(DWORD function)
{
	return function >= D3DCMP_NEVER && function <= D3DCMP_ALWAYS ? (unsigned char)(function - D3DCMP_NEVER + 1) : 8;
}

static unsigned char blend_factor(DWORD factor)
{
	switch (factor)
	{
	case D3DBLEND_ZERO: return 1;
	case D3DBLEND_ONE: return 2;
	case D3DBLEND_SRCCOLOR: return 3;
	case D3DBLEND_INVSRCCOLOR: return 4;
	case D3DBLEND_SRCALPHA: return 5;
	case D3DBLEND_INVSRCALPHA: return 6;
	case D3DBLEND_DESTALPHA: return 7;
	case D3DBLEND_INVDESTALPHA: return 8;
	case D3DBLEND_DESTCOLOR: return 9;
	case D3DBLEND_INVDESTCOLOR: return 10;
	case D3DBLEND_SRCALPHASAT: return 11;
	/* (the constant colour factors: Direct3D 8 has none) */
	case D3DBLEND_CONSTANTCOLOR:
	case D3DBLEND_CONSTANTALPHA: return 2;
	default: return 1;
	}
}

static unsigned char blend_operation(DWORD operation)
{
	switch (operation)
	{
	case D3DBLENDOP_SUBTRACT: return 2;
	case D3DBLENDOP_REVSUBTRACT:
	case D3DBLENDOP_REVSUBTRACTSIGNED: return 3;
	case D3DBLENDOP_MIN: return 4;
	case D3DBLENDOP_MAX: return 5;
	default: return 1;
	}
}

static unsigned long nv2a_blend_operation(DWORD operation)
{
	switch (operation)
	{
	case D3DBLENDOP_SUBTRACT: return _nv2a_ff_blend_operation_subtract;
	case D3DBLENDOP_REVSUBTRACT:
	case D3DBLENDOP_REVSUBTRACTSIGNED: return _nv2a_ff_blend_operation_reverse_subtract;
	case D3DBLENDOP_MIN: return _nv2a_ff_blend_operation_minimum;
	case D3DBLENDOP_MAX: return _nv2a_ff_blend_operation_maximum;
	default: return _nv2a_ff_blend_operation_add;
	}
}

static unsigned char stencil_operation(DWORD operation)
{
	switch (operation)
	{
	case D3DSTENCILOP_ZERO: return 2;
	case D3DSTENCILOP_REPLACE: return 3;
	case D3DSTENCILOP_INCRSAT: return 4;
	case D3DSTENCILOP_DECRSAT: return 5;
	case D3DSTENCILOP_INVERT: return 6;
	case D3DSTENCILOP_INCR: return 7;
	case D3DSTENCILOP_DECR: return 8;
	default: return 1;
	}
}

static void log_once(BOOL *done, const char *text)
{
	if (!*done)
	{
		*done = TRUE;
		platform_log("Direct3D: %s", text);
	}
}

const D3DSurface *xd3d_back_buffer(void)
{
	return &device.back_buffer;
}

/* ---------- the vertical blank, at 60 a second of the clock: looked at
from the game's waits (system_sleep), each draw and each presentation (the
builds of this port run one thread) */

static D3DCALLBACK vertical_blank_callback;
static unsigned long long vertical_blank_start;
static unsigned long vertical_blank_count, pending_flips;
static volatile unsigned int flip_count;

void d3d_vertical_blank_poll(void)
{
	unsigned long long now = system_microseconds();
	unsigned long target, calls = 0;

	if (!vertical_blank_start)
		vertical_blank_start = now;
	target = (unsigned long)((now - vertical_blank_start) * 3 / 50000);
	while (vertical_blank_count < target)
	{
		vertical_blank_count++;
		/* a presented frame becomes visible at the next vertical blank */
		if (pending_flips)
		{
			pending_flips--;
			flip_count++;
		}
		/* (a long stall: the blanks it missed are not all called) */
		if (vertical_blank_callback && calls++ < 4)
			vertical_blank_callback(0);
	}
}

/* replaces main/d3d_intimacy.cpp, which reads the counter out of the Xbox
Direct3D runtime's private device structure */
volatile unsigned int *d3d_find_flipcount(void)
{
	return &flip_count;
}

void WINAPI D3DDevice_SetVerticalBlankCallback(D3DCALLBACK callback)
{
	vertical_blank_callback = callback;
}

void WINAPI D3DDevice_BlockUntilVerticalBlank(void)
{
	unsigned long count = vertical_blank_count;

	while (vertical_blank_count == count)
	{
		system_sleep(1);
		d3d_vertical_blank_poll();
	}
}

/* ---------- creation */

Direct3D *WINAPI Direct3DCreate8(UINT sdk_version)
{
	(void)sdk_version;
	return (Direct3D *)1;
}

void WINAPI Direct3D_SetPushBufferSize(DWORD push_buffer_size, DWORD segment_count)
{
	(void)push_buffer_size;
	(void)segment_count;
}

static void viewport_update_constants(void)
{
	/* Direct3D's reserved constants c[-38] and c[-37] map clip space to
	the screen; zscale is the depth buffer's range */
	float zscale = 16777215.0f;

	if (device.depth_stencil)
	{
		struct xd3d_texture_description description;

		xd3d_texture_describe(device.depth_stencil->Format, device.depth_stencil->Size, &description);
		if (description.format == D3DFMT_D16 || description.format == D3DFMT_LIN_D16 ||
			description.format == D3DFMT_F16 || description.format == D3DFMT_LIN_F16)
		{
			zscale = 65535.0f;
		}
	}
	device.viewport_scale[0] = device.viewport.Width * 0.5f;
	device.viewport_scale[1] = -(float)device.viewport.Height * 0.5f;
	device.viewport_scale[2] = zscale * (device.viewport.MaxZ - device.viewport.MinZ);
	device.viewport_scale[3] = 0.0f;
	device.viewport_offset[0] = device.viewport.X + device.viewport.Width * 0.5f;
	device.viewport_offset[1] = device.viewport.Y + device.viewport.Height * 0.5f;
	device.viewport_offset[2] = zscale * device.viewport.MinZ;
	device.viewport_offset[3] = 0.0f;
	if (!(device.shader_constant_mode & D3DSCM_NORESERVEDCONSTANTS))
	{
		memcpy(device.constants[XGPU_VERTEX_CONSTANT_BIAS - 38], device.viewport_scale, sizeof(device.viewport_scale));
		memcpy(device.constants[XGPU_VERTEX_CONSTANT_BIAS - 37], device.viewport_offset, sizeof(device.viewport_offset));
	}
	device.target_dirty = TRUE;
}

static void plan_capabilities_set(void)
{
	const struct pcd3d_capabilities *c = &device.capabilities;
	struct nv2a_ff_capabilities *p = &device.plan_capabilities;

	memset(p, 0, sizeof(*p));
	p->units = (unsigned char)c->units;
	p->combine = 1;
	p->combine_arb = (c->operations & PCD3D_OPERATION(PCD3D_TOP_LERP)) &&
		(c->operations & PCD3D_OPERATION(PCD3D_TOP_SUBTRACT));
	p->environment_add = (c->operations & PCD3D_OPERATION(PCD3D_TOP_ADD)) != 0;
	p->dot3 = (c->operations & PCD3D_OPERATION(PCD3D_TOP_DOTPRODUCT3)) != 0;
	p->blend_subtract = (unsigned char)c->blend_operations;
	p->blend_minmax = (unsigned char)c->blend_operations;
	p->blend_color = 0;
	p->fog_coordinate = 1;
	p->secondary_color = 1;
	p->cube_maps = (unsigned char)c->cube_maps;
	p->destination_alpha = (unsigned char)c->destination_alpha;
	p->bakes = _nv2a_ff_bakes_none;
	p->maximum_passes = 0;
}

HRESULT WINAPI Direct3D_CreateDevice(UINT adapter, D3DDEVTYPE device_type, void *unused, DWORD behavior_flags,
	D3DPRESENT_PARAMETERS *presentation_parameters, D3DDevice **returned_device)
{
	int index;

	(void)adapter;
	(void)device_type;
	(void)unused;
	(void)behavior_flags;
	(void)presentation_parameters;
	if (!device.created)
	{
		struct system_display display;

		memset(&device, 0, sizeof(device));
		xd3d_surface_initialize(&device.back_buffer, D3DFMT_LIN_A8R8G8B8, SCREEN_WIDTH, SCREEN_HEIGHT);
		xd3d_surface_initialize(&device.depth_buffer, D3DFMT_LIN_D24S8, SCREEN_WIDTH, SCREEN_HEIGHT);
		device.render_target = &device.back_buffer;
		device.depth_stencil = &device.depth_buffer;
		device.viewport.Width = SCREEN_WIDTH;
		device.viewport.Height = SCREEN_HEIGHT;
		device.viewport.MaxZ = 1.0f;
		D3D__RenderState[D3DRS_ZENABLE] = TRUE;
		D3D__RenderState[D3DRS_ZWRITEENABLE] = TRUE;
		D3D__RenderState[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
		D3D__RenderState[D3DRS_COLORWRITEENABLE] = D3DCOLORWRITEENABLE_ALL;
		D3D__RenderState[D3DRS_SRCBLEND] = D3DBLEND_ONE;
		D3D__RenderState[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
		D3D__RenderState[D3DRS_BLENDOP] = D3DBLENDOP_ADD;
		D3D__RenderState[D3DRS_CULLMODE] = D3DCULL_CCW;
		D3D__RenderState[D3DRS_FRONTFACE] = D3DFRONT_CW;
		D3D__RenderState[D3DRS_FILLMODE] = D3DFILL_SOLID;
		D3D__RenderState[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
		D3D__RenderState[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
		D3D__RenderState[D3DRS_STENCILMASK] = 0xff;
		D3D__RenderState[D3DRS_STENCILWRITEMASK] = 0xff;
		D3D__RenderState[D3DRS_STENCILFAIL] = D3DSTENCILOP_KEEP;
		D3D__RenderState[D3DRS_STENCILZFAIL] = D3DSTENCILOP_KEEP;
		D3D__RenderState[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
		for (index = 0; index < D3DTSS_MAXSTAGES; index++)
		{
			D3D__TextureState[index][D3DTSS_ADDRESSU] = D3DTADDRESS_WRAP;
			D3D__TextureState[index][D3DTSS_ADDRESSV] = D3DTADDRESS_WRAP;
			D3D__TextureState[index][D3DTSS_ADDRESSW] = D3DTADDRESS_WRAP;
			D3D__TextureState[index][D3DTSS_MAGFILTER] = D3DTEXF_POINT;
			D3D__TextureState[index][D3DTSS_MINFILTER] = D3DTEXF_POINT;
			D3D__TextureState[index][D3DTSS_MAXANISOTROPY] = 1;
		}
		viewport_update_constants();

		/* the window, and Direct3D on it at its size */
		memset(&display, 0, sizeof(display));
		if (system_display_open(SCREEN_WIDTH, SCREEN_HEIGHT, 32, &display) &&
			pcd3d_open(system_display_window(), (unsigned long)display.width, (unsigned long)display.height))
		{
			device.drawing = TRUE;
			pcd3d_capabilities(&device.capabilities);
			plan_capabilities_set();
		}
		else
		{
			platform_log("Direct3D: nothing will be drawn");
		}
		device.target_dirty = TRUE;
		device.created = TRUE;
	}
	*returned_device = (D3DDevice *)1;
	return S_OK;
}

ULONG WINAPI D3DDevice_Release(void)
{
	return 1;
}

void WINAPI D3DDevice_GetDeviceCaps(D3DCAPS8 *caps)
{
	memset(caps, 0, sizeof(*caps));
	caps->DeviceType = D3DDEVTYPE_HAL;
	caps->MaxTextureWidth = 4096;
	caps->MaxTextureHeight = 4096;
	caps->MaxVolumeExtent = 512;
	caps->MaxTextureRepeat = 8192;
	caps->MaxTextureAspectRatio = 4096;
	caps->MaxAnisotropy = 4;
	caps->MaxTextureBlendStages = 4;
	caps->MaxSimultaneousTextures = 4;
	caps->MaxActiveLights = 8;
	caps->MaxVertexBlendMatrices = 4;
	caps->MaxPointSize = 64.0f;
	caps->MaxPrimitiveCount = 0xfffff;
	caps->MaxVertexIndex = 0xffff;
	caps->MaxStreams = 16;
	caps->MaxStreamStride = 255;
	caps->VertexShaderVersion = D3DVS_VERSION(1, 1);
	caps->MaxVertexShaderConst = 192;
	caps->PixelShaderVersion = D3DPS_VERSION(1, 1);
	caps->MaxPixelShaderValue = 1.0f;
}

/* ---------- targets and the viewport */

void WINAPI D3DDevice_GetBackBuffer(INT back_buffer, D3DBACKBUFFER_TYPE type, D3DSurface **result)
{
	(void)back_buffer;
	(void)type;
	/* like Direct3D, the caller gets a reference it must release */
	device.back_buffer.Common++;
	*result = &device.back_buffer;
}

HRESULT WINAPI D3DDevice_GetDepthStencilSurface(D3DSurface **result)
{
	*result = device.depth_stencil;
	if (!*result)
		return D3DERR_NOTFOUND;
	(*result)->Common++;
	return S_OK;
}

void WINAPI D3DDevice_SetRenderTarget(D3DSurface *render_target, D3DSurface *depth_stencil)
{
	if (render_target)
		device.render_target = render_target;
	device.depth_stencil = depth_stencil;
	/* like Direct3D, the viewport becomes the whole new target */
	if (device.render_target)
	{
		struct xd3d_texture_description description;

		xd3d_texture_describe(device.render_target->Format, device.render_target->Size, &description);
		device.viewport.X = 0;
		device.viewport.Y = 0;
		device.viewport.Width = description.width;
		device.viewport.Height = description.height;
		device.viewport.MinZ = 0.0f;
		device.viewport.MaxZ = 1.0f;
	}
	viewport_update_constants();
}

void WINAPI D3DDevice_SetViewport(CONST D3DVIEWPORT8 *viewport)
{
	device.viewport = *viewport;
	viewport_update_constants();
}

void WINAPI D3DDevice_SetFlickerFilter(DWORD filter) { (void)filter; }
void WINAPI D3DDevice_SetSoftDisplayFilter(BOOL enable) { (void)enable; }

void WINAPI D3DDevice_SetShaderConstantMode(D3DSHADERCONSTANTMODE mode)
{
	device.shader_constant_mode = mode;
	viewport_update_constants();
}

/* the target and viewport to the PC half, if they changed; FALSE when
nothing can be drawn into the target */
static BOOL target_apply(void)
{
	if (!device.drawing)
		return FALSE;
	if (device.target_dirty)
	{
		struct xd3d_texture_description description;
		struct pcd3d_texture *texture;
		BOOL usable, depth;

		device.target_dirty = FALSE;
		texture = xd3d_target_get(device.render_target, &usable);
		device.target_usable = usable;
		device.target_is_back_buffer = !device.render_target || device.render_target->Data == device.back_buffer.Data;
		if (!usable)
			return FALSE;
		xd3d_texture_describe(device.render_target->Format, device.render_target->Size, &description);
		depth = device.depth_stencil != NULL;
		pcd3d_target(texture, depth, description.width, description.height);
		pcd3d_viewport(device.viewport.X, device.viewport.Y, device.viewport.Width, device.viewport.Height,
			device.viewport.MinZ, device.viewport.MaxZ);
	}
	if (device.target_usable && device.target_is_back_buffer)
		xd3d_back_buffer_changes++;
	return device.target_usable;
}

/* ---------- the GPU's timing: the PC's own */

BOOL WINAPI D3DDevice_IsBusy(void)
{
	return FALSE;
}

void WINAPI D3DDevice_KickPushBuffer(void)
{
}

void WINAPI D3DDevice_InsertCallback(D3DCALLBACKTYPE type, D3DCALLBACK callback, DWORD context)
{
	(void)type;
	/* the "GPU" reaches the callback at once */
	if (callback)
		callback(context);
}

/* ---------- visibility tests: Direct3D 8 cannot count pixels, so a test
answers that none were drawn (the lens flares it serves stay out) */

void WINAPI D3DDevice_BeginVisibilityTest(void)
{
}

HRESULT WINAPI D3DDevice_EndVisibilityTest(DWORD index)
{
	(void)index;
	return S_OK;
}

HRESULT WINAPI D3DDevice_GetVisibilityTestResult(DWORD index, UINT *result, ULONGLONG *time_stamp)
{
	(void)index;
	if (result)
		*result = 0;
	if (time_stamp)
		*time_stamp = 0;
	return S_OK;
}

/* ---------- render and texture stage state */

void D3DFASTCALL D3DDevice_SetRenderState_Simple(DWORD method, DWORD value)
{
	/* callers also store the value in D3D__RenderState themselves */
	(void)method;
	(void)value;
}

void D3DFASTCALL D3DDevice_SetRenderState_Deferred(D3DRENDERSTATETYPE state, DWORD value)
{
	if ((unsigned long)state < D3DRS_MAX)
		D3D__RenderState[state] = value;
}

/* As the Xbox's D3D8 does it: a z bias is a polygon offset of -bias depth
units plus -bias/4 times the polygon's depth slope */
void WINAPI D3DDevice_SetRenderState_ZBias(DWORD value)
{
	float offset = -(float)value;
	float slope = offset * 0.25f;
	DWORD enable = value != 0;

	memcpy(&D3D__RenderState[D3DRS_POLYGONOFFSETZSLOPESCALE], &slope, sizeof(slope));
	memcpy(&D3D__RenderState[D3DRS_POLYGONOFFSETZOFFSET], &offset, sizeof(offset));
	D3D__RenderState[D3DRS_POINTOFFSETENABLE] = enable;
	D3D__RenderState[D3DRS_WIREFRAMEOFFSETENABLE] = enable;
	D3D__RenderState[D3DRS_SOLIDOFFSETENABLE] = enable;
	D3D__RenderState[D3DRS_ZBIAS] = value;
}

void WINAPI D3DDevice_SetRenderStateNotInline(D3DRENDERSTATETYPE state, DWORD value)
{
	if (state == D3DRS_ZBIAS)
		D3DDevice_SetRenderState_ZBias(value);
	else if ((unsigned long)state < D3DRS_MAX)
		D3D__RenderState[state] = value;
}

#define COMPLEX_RENDER_STATE(name, state) \
	void WINAPI D3DDevice_SetRenderState_##name(DWORD value) { D3D__RenderState[state] = value; }

COMPLEX_RENDER_STATE(PSTextureModes, D3DRS_PSTEXTUREMODES)
COMPLEX_RENDER_STATE(VertexBlend, D3DRS_VERTEXBLEND)
COMPLEX_RENDER_STATE(FogColor, D3DRS_FOGCOLOR)
COMPLEX_RENDER_STATE(FillMode, D3DRS_FILLMODE)
COMPLEX_RENDER_STATE(BackFillMode, D3DRS_BACKFILLMODE)
COMPLEX_RENDER_STATE(TwoSidedLighting, D3DRS_TWOSIDEDLIGHTING)
COMPLEX_RENDER_STATE(NormalizeNormals, D3DRS_NORMALIZENORMALS)
COMPLEX_RENDER_STATE(ZEnable, D3DRS_ZENABLE)
COMPLEX_RENDER_STATE(StencilEnable, D3DRS_STENCILENABLE)
COMPLEX_RENDER_STATE(StencilFail, D3DRS_STENCILFAIL)
COMPLEX_RENDER_STATE(FrontFace, D3DRS_FRONTFACE)
COMPLEX_RENDER_STATE(CullMode, D3DRS_CULLMODE)
COMPLEX_RENDER_STATE(TextureFactor, D3DRS_TEXTUREFACTOR)
COMPLEX_RENDER_STATE(LogicOp, D3DRS_LOGICOP)
COMPLEX_RENDER_STATE(EdgeAntiAlias, D3DRS_EDGEANTIALIAS)
COMPLEX_RENDER_STATE(MultiSampleAntiAlias, D3DRS_MULTISAMPLEANTIALIAS)
COMPLEX_RENDER_STATE(MultiSampleMask, D3DRS_MULTISAMPLEMASK)
COMPLEX_RENDER_STATE(MultiSampleType, D3DRS_MULTISAMPLETYPE)
COMPLEX_RENDER_STATE(ShadowFunc, D3DRS_SHADOWFUNC)
COMPLEX_RENDER_STATE(LineWidth, D3DRS_LINEWIDTH)
COMPLEX_RENDER_STATE(Dxt1NoiseEnable, D3DRS_DXT1NOISEENABLE)
COMPLEX_RENDER_STATE(YuvEnable, D3DRS_YUVENABLE)
COMPLEX_RENDER_STATE(OcclusionCullEnable, D3DRS_OCCLUSIONCULLENABLE)
COMPLEX_RENDER_STATE(StencilCullEnable, D3DRS_STENCILCULLENABLE)
COMPLEX_RENDER_STATE(RopZCmpAlwaysRead, D3DRS_ROPZCMPALWAYSREAD)
COMPLEX_RENDER_STATE(RopZRead, D3DRS_ROPZREAD)
COMPLEX_RENDER_STATE(DoNotCullUncompressed, D3DRS_DONOTCULLUNCOMPRESSED)

void D3DFASTCALL D3DDevice_SetTextureState_Deferred(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES && (unsigned long)type < D3DTSS_MAX)
		D3D__TextureState[stage][type] = value;
}

void WINAPI D3DDevice_SetTextureState_TexCoordIndex(DWORD stage, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES)
		D3D__TextureState[stage][D3DTSS_TEXCOORDINDEX] = value;
}

void WINAPI D3DDevice_SetTextureState_BorderColor(DWORD stage, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES)
		D3D__TextureState[stage][D3DTSS_BORDERCOLOR] = value;
}

void WINAPI D3DDevice_SetTextureState_ColorKeyColor(DWORD stage, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES)
		D3D__TextureState[stage][D3DTSS_COLORKEYCOLOR] = value;
}

void WINAPI D3DDevice_SetTextureState_BumpEnv(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES && (unsigned long)type < D3DTSS_MAX)
		D3D__TextureState[stage][type] = value;
}

void WINAPI D3DDevice_SetTexture(DWORD stage, D3DBaseTexture *texture)
{
	if (stage < D3DTSS_MAXSTAGES)
		device.textures[stage] = texture;
}

void WINAPI D3DDevice_SetPalette(DWORD stage, D3DPalette *palette)
{
	if (stage < D3DTSS_MAXSTAGES)
		device.palettes[stage] = palette;
}

void WINAPI D3DDevice_SetPixelShaderProgram(D3DPIXELSHADERDEF *definition)
{
	/* the definition's members are the pixel shader render states */
	if (!definition)
		return;
	memcpy(&D3D__RenderState[D3DRS_PSALPHAINPUTS0], definition->PSAlphaInputs, sizeof(definition->PSAlphaInputs));
	D3D__RenderState[D3DRS_PSFINALCOMBINERINPUTSABCD] = definition->PSFinalCombinerInputsABCD;
	D3D__RenderState[D3DRS_PSFINALCOMBINERINPUTSEFG] = definition->PSFinalCombinerInputsEFG;
	memcpy(&D3D__RenderState[D3DRS_PSCONSTANT0_0], definition->PSConstant0, sizeof(definition->PSConstant0));
	memcpy(&D3D__RenderState[D3DRS_PSCONSTANT1_0], definition->PSConstant1, sizeof(definition->PSConstant1));
	memcpy(&D3D__RenderState[D3DRS_PSALPHAOUTPUTS0], definition->PSAlphaOutputs, sizeof(definition->PSAlphaOutputs));
	memcpy(&D3D__RenderState[D3DRS_PSRGBINPUTS0], definition->PSRGBInputs, sizeof(definition->PSRGBInputs));
	D3D__RenderState[D3DRS_PSCOMPAREMODE] = definition->PSCompareMode;
	D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT0] = definition->PSFinalCombinerConstant0;
	D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT1] = definition->PSFinalCombinerConstant1;
	memcpy(&D3D__RenderState[D3DRS_PSRGBOUTPUTS0], definition->PSRGBOutputs, sizeof(definition->PSRGBOutputs));
	D3D__RenderState[D3DRS_PSCOMBINERCOUNT] = definition->PSCombinerCount;
	D3D__RenderState[D3DRS_PSTEXTUREMODES] = definition->PSTextureModes;
	D3D__RenderState[D3DRS_PSDOTMAPPING] = definition->PSDotMapping;
	D3D__RenderState[D3DRS_PSINPUTTEXTURE] = definition->PSInputTexture;
}

/* ---------- vertex shaders */

static unsigned long vertex_type_bytes(unsigned long type)
{
	switch (type)
	{
	case D3DVSDT_FLOAT1: return 4;
	case D3DVSDT_FLOAT2: return 8;
	case D3DVSDT_FLOAT3: return 12;
	case D3DVSDT_FLOAT4: return 16;
	case D3DVSDT_D3DCOLOR: return 4;
	case D3DVSDT_SHORT1: return 2;
	case D3DVSDT_SHORT2: return 4;
	case D3DVSDT_SHORT3: return 6;
	case D3DVSDT_SHORT4: return 8;
	case D3DVSDT_NORMSHORT1: return 2;
	case D3DVSDT_NORMSHORT2: return 4;
	case D3DVSDT_NORMSHORT3: return 6;
	case D3DVSDT_NORMSHORT4: return 8;
	case D3DVSDT_NORMPACKED3: return 4;
	case D3DVSDT_PBYTE1: return 1;
	case D3DVSDT_PBYTE2: return 2;
	case D3DVSDT_PBYTE3: return 3;
	case D3DVSDT_PBYTE4: return 4;
	case D3DVSDT_FLOAT2H: return 12;
	default: return 0;
	}
}

static void parse_declaration(struct vertex_shader_object *object, const DWORD *declaration)
{
	unsigned long stream = 0;
	unsigned long offsets[16] = { 0 };

	for (; declaration && *declaration != D3DVSD_END(); declaration++)
	{
		DWORD token = *declaration;
		unsigned long token_type = (token & D3DVSD_TOKENTYPEMASK) >> D3DVSD_TOKENTYPESHIFT;

		switch (token_type)
		{
		case D3DVSD_TOKEN_STREAM:
			stream = token & D3DVSD_STREAMNUMBERMASK;
			break;
		case D3DVSD_TOKEN_STREAMDATA:
			if (token & D3DVSD_DATALOADTYPEMASK)
			{
				/* skip: the count is in dwords, or in bytes with bit 27 */
				unsigned long count = (token & D3DVSD_SKIPCOUNTMASK) >> D3DVSD_SKIPCOUNTSHIFT;

				offsets[stream] += (token & 0x08000000) ? count : count * 4;
			}
			else if (object->element_count < XGPU_VERTEX_ATTRIBUTE_COUNT)
			{
				struct vertex_element *element = &object->elements[object->element_count++];

				element->reg = (unsigned char)(token & D3DVSD_VERTEXREGMASK);
				element->stream = (unsigned char)stream;
				element->type = (unsigned char)((token & D3DVSD_DATATYPEMASK) >> D3DVSD_DATATYPESHIFT);
				element->bytes = (unsigned char)vertex_type_bytes(element->type);
				element->offset = (unsigned short)offsets[stream];
				offsets[stream] += element->bytes;
			}
			break;
		case D3DVSD_TOKEN_CONSTMEM:
			declaration += ((token & D3DVSD_CONSTCOUNTMASK) >> D3DVSD_CONSTCOUNTSHIFT) * 4;
			break;
		case D3DVSD_TOKEN_EXT:
			declaration += (token & D3DVSD_EXTCOUNTMASK) >> D3DVSD_EXTCOUNTSHIFT;
			break;
		default:
			break;
		}
	}
}

HRESULT WINAPI D3DDevice_CreateVertexShader(CONST DWORD *declaration, CONST DWORD *function, DWORD *handle, DWORD usage)
{
	struct vertex_shader_object *object = calloc(1, sizeof(*object));

	(void)usage;
	if (!object)
		return E_OUTOFMEMORY;
	object->signature = VERTEX_SHADER_SIGNATURE;
	if (function)
	{
		/* header: program type in the low word, instruction count in the high */
		object->instruction_count = function[0] >> 16;
		object->instructions = malloc(object->instruction_count * 4 * sizeof(DWORD) + 4);
		if (object->instructions)
			memcpy(object->instructions, function + 1, object->instruction_count * 4 * sizeof(DWORD));
	}
	parse_declaration(object, declaration);
	/* (odd values are FVF codes; a program's handle, its address, is even) */
	*handle = (DWORD)object;
	return S_OK;
}

static struct vertex_shader_object *vertex_shader_from_handle(DWORD handle)
{
	struct vertex_shader_object *object = (struct vertex_shader_object *)handle;

	if (!handle || (handle & 1) || object->signature != VERTEX_SHADER_SIGNATURE)
		return NULL;
	return object;
}

void halo_vertex_shader_lighting(unsigned long handle)
{
	(void)handle;
}

void WINAPI D3DDevice_DeleteVertexShader(DWORD handle)
{
	/* (programs stay: the object is small) */
	(void)handle;
}

void WINAPI D3DDevice_SetVertexShader(DWORD handle)
{
	struct vertex_shader_object *object = vertex_shader_from_handle(handle);

	if (object)
	{
		device.vertex_shader = object;
		device.program_address = 0;
		device.program_slots[0] = object;
	}
}

void WINAPI D3DDevice_LoadVertexShader(DWORD handle, DWORD address)
{
	if (address < VERTEX_PROGRAM_SLOTS)
		device.program_slots[address] = vertex_shader_from_handle(handle);
}

void WINAPI D3DDevice_SelectVertexShader(DWORD handle, DWORD address)
{
	struct vertex_shader_object *object = vertex_shader_from_handle(handle);

	if (object)
		device.vertex_shader = object;
	if (address < VERTEX_PROGRAM_SLOTS)
		device.program_address = address;
}

void WINAPI D3DDevice_GetVertexShaderSize(DWORD handle, UINT *size)
{
	struct vertex_shader_object *object = vertex_shader_from_handle(handle);

	*size = object ? object->instruction_count : 0;
}

void WINAPI D3DDevice_SetVertexShaderConstant(INT reg, CONST void *constant_data, DWORD constant_count)
{
	long first = reg + XGPU_VERTEX_CONSTANT_BIAS;

	if (first < 0 || first >= XGPU_VERTEX_CONSTANT_COUNT)
		return;
	if (first + (long)constant_count > XGPU_VERTEX_CONSTANT_COUNT)
		constant_count = XGPU_VERTEX_CONSTANT_COUNT - first;
	memcpy(device.constants[first], constant_data, constant_count * sizeof(device.constants[0]));
}

/* the program that runs: the one loaded at the selected address, else the
current shader's own */
static struct vertex_shader_object *current_program(void)
{
	struct vertex_shader_object *program = device.program_slots[device.program_address];

	return program ? program : device.vertex_shader;
}

/* ---------- streams */

void WINAPI D3DDevice_SetStreamSource(UINT stream_number, D3DVertexBuffer *stream_data, UINT stride)
{
	if (stream_number >= 16)
		return;
	device.streams[stream_number].data = stream_data ? stream_data->Data : 0;
	device.streams[stream_number].stride = stride;
}

void WINAPI D3DDevice_SetIndices(D3DIndexBuffer *index_data, UINT base_vertex_index)
{
	device.base_vertex_index = base_vertex_index;
	D3D__IndexData = index_data ? (WORD *)index_data->Data : NULL;
}

/* ---------- the pixel state's plans */

struct plan_key
{
	struct nv2a_ff_state state;
	struct nv2a_ff_draw draw;
};

struct plan_entry
{
	struct plan_entry *next;
	unsigned long hash;
	struct plan_key key;
	struct nv2a_ff_plan plan;
	BOOL fallback;
};

#define PLAN_BUCKETS 1024

static struct plan_entry *plan_buckets[PLAN_BUCKETS];

static unsigned long hash_bytes(const void *data, unsigned long size)
{
	const unsigned char *bytes = data;
	unsigned long hash = 2166136261UL;

	for (; size; size--)
		hash = (hash ^ *bytes++) * 16777619UL;
	return hash;
}

/* the built-in plan: the first texture times the vertex colour, with the
draw's alpha test and blend */
static void fallback_plan(const struct plan_key *key, struct nv2a_ff_plan *plan)
{
	struct nv2a_ff_pass *pass = &plan->passes[0];
	unsigned long mode = key->state.texture_modes & 0x1f;

	memset(plan, 0, sizeof(*plan));
	plan->status = _nv2a_ff_plan_approximate;
	plan->pass_count = 1;
	if (key->state.sampler_type[0] != _xgpu_sampler_none && mode != 0)
	{
		pass->unit_count = 1;
		pass->units[0].stage = 0;
		pass->units[0].bake = NV2A_FF_BAKE_NONE;
		pass->units[0].environment = _nv2a_ff_environment_modulate;
		pass->units[0].constant_rgb = pass->units[0].constant_alpha = NV2A_FF_EXPRESSION_NONE;
	}
	/* (the expressions: none; the primary colour is the diffuse colour) */
	pass->primary_rgb = pass->primary_alpha = NV2A_FF_EXPRESSION_NONE;
	pass->secondary_rgb = NV2A_FF_EXPRESSION_NONE;
	pass->fog_factor = pass->fog_color = NV2A_FF_EXPRESSION_NONE;
	pass->alpha_test_function = (unsigned short)key->state.alpha_test_function;
	pass->alpha_reference = NV2A_FF_EXPRESSION_NONE;
	pass->blend_enable = key->draw.blend_enable;
	pass->source_blend = (unsigned short)key->draw.source_blend;
	pass->destination_blend = (unsigned short)key->draw.destination_blend;
	pass->blend_operation = (unsigned short)key->draw.blend_operation;
	pass->write_color = key->draw.write_color;
	pass->write_alpha = key->draw.write_alpha;
}

/* text to the log a line at a time */
static void log_lines(const char *prefix, const char *text)
{
	char line[300];

	while (*text)
	{
		unsigned long length = 0;

		while (text[length] && text[length] != '\n')
			length++;
		snprintf(line, sizeof(line), "%s%.*s", prefix, (int)(length < 250 ? length : 250), text);
		platform_log("%s", line);
		text += length;
		if (*text == '\n')
			text++;
	}
}

/* testing (setting d3d_log_plans=1): each new pixel state, what it computes
and the passes it is drawn with */
static void plan_log(const struct plan_entry *entry)
{
	static long enabled = -1;
	static char text[4096];

	if (enabled < 0)
		enabled = dos_setting_integer("d3d_log_plans", 0);
	if (!enabled)
		return;
	platform_log("plan %08lx: status %d, approximations %04lx, dropped %d %d %d %d, modes %05lx, blend %d %lu/%lu",
		nv2a_ff_state_hash(&entry->key.state), entry->plan.status, entry->plan.approximations,
		entry->plan.dropped_stage[0], entry->plan.dropped_stage[1], entry->plan.dropped_stage[2],
		entry->plan.dropped_stage[3], entry->key.state.texture_modes, entry->key.draw.blend_enable,
		entry->key.draw.source_blend, entry->key.draw.destination_blend);
	nv2a_ff_state_describe(&entry->key.state, NULL, &device.plan_capabilities, text, sizeof(text));
	log_lines("  state: ", text);
	nv2a_ff_plan_describe(&entry->plan, text, sizeof(text));
	log_lines("  plan: ", text);
}

static struct plan_entry *plan_get(const struct plan_key *key)
{
	static struct plan_entry *last;
	unsigned long hash;
	struct plan_entry **bucket, *entry;

	if (last && !memcmp(&last->key, key, sizeof(*key)))
		return last;
	hash = hash_bytes(key, sizeof(*key));
	bucket = &plan_buckets[hash % PLAN_BUCKETS];
	for (entry = *bucket; entry; entry = entry->next)
	{
		if (entry->hash == hash && !memcmp(&entry->key, key, sizeof(*key)))
		{
			last = entry;
			return entry;
		}
	}
	entry = calloc(1, sizeof(*entry));
	if (!entry)
		return NULL;
	entry->hash = hash;
	entry->key = *key;
	nv2a_ff_compile(&key->state, NULL, &device.plan_capabilities, &key->draw, &entry->plan);
	if (entry->plan.status == _nv2a_ff_plan_impossible || !entry->plan.pass_count)
	{
		platform_log("Direct3D: no stage form for pixel state %08lx: drawn as its first texture times the "
			"vertex colour", nv2a_ff_state_hash(&key->state));
		fallback_plan(key, &entry->plan);
		entry->fallback = TRUE;
	}
	entry->next = *bucket;
	*bucket = entry;
	last = entry;
	plan_log(entry);
	return entry;
}

/* the fog factor from the program's fog coordinate, as nv2a_psh.c's
shaders compute it (halo-ce-universal's xgpu_fixed.c) */
static float fog_factor_from_coordinate(const struct nv2a_ff_state *ff_state,
	const struct nv2a_ff_constants *constants, float coordinate)
{
	float factor = 1.0f;

	if (ff_state->fog_enable)
	{
		float span;

		switch (ff_state->fog_table_mode)
		{
		case D3DFOG_EXP:
			factor = expf(-constants->fog_parameters[2] * coordinate);
			break;
		case D3DFOG_EXP2:
			factor = constants->fog_parameters[2] * coordinate;
			factor = expf(-factor * factor);
			break;
		case D3DFOG_LINEAR:
			span = constants->fog_parameters[1] - constants->fog_parameters[0];
			factor = (constants->fog_parameters[1] - coordinate) / (span > 1.0e-6f ? span : 1.0e-6f);
			break;
		default:
			factor = coordinate;
			break;
		}
	}
	return factor < 0.0f ? 0.0f : factor > 1.0f ? 1.0f : factor;
}

/* ---------- a plan's units as the PC's stages */

static BOOL scale_reported, constant_reported, interpolate_reported;

static unsigned char stage_argument(unsigned char source, unsigned char operand)
{
	unsigned char argument;

	switch (source)
	{
	case _nv2a_ff_source_texture: argument = PCD3D_TA_TEXTURE; break;
	case _nv2a_ff_source_constant: argument = PCD3D_TA_TFACTOR; break;
	case _nv2a_ff_source_primary: argument = PCD3D_TA_DIFFUSE; break;
	default: argument = PCD3D_TA_CURRENT; break;
	}
	if (operand == _nv2a_ff_operand_inverse_color || operand == _nv2a_ff_operand_inverse_alpha)
		argument |= PCD3D_TA_COMPLEMENT;
	if (operand == _nv2a_ff_operand_alpha || operand == _nv2a_ff_operand_inverse_alpha)
		argument |= PCD3D_TA_ALPHAREPLICATE;
	return argument;
}

static BOOL has_operation(int operation)
{
	return (device.capabilities.operations & PCD3D_OPERATION(operation)) != 0;
}

/* one side of a combine environment */
static void stage_combine(const struct nv2a_ff_combine *combine, unsigned char *operation, unsigned char *arguments)
{
	unsigned char a0 = stage_argument(combine->source[0], combine->operand[0]);
	unsigned char a1 = stage_argument(combine->source[1], combine->operand[1]);
	unsigned char a2 = stage_argument(combine->source[2], combine->operand[2]);

	arguments[0] = PCD3D_TA_CURRENT;
	arguments[1] = a0;
	arguments[2] = a1;
	switch (combine->function)
	{
	case _nv2a_ff_combine_replace:
		*operation = PCD3D_TOP_SELECTARG1;
		break;
	case _nv2a_ff_combine_modulate:
		*operation = combine->scale == 4 ? PCD3D_TOP_MODULATE4X : combine->scale == 2 ? PCD3D_TOP_MODULATE2X :
			PCD3D_TOP_MODULATE;
		break;
	case _nv2a_ff_combine_add:
		*operation = PCD3D_TOP_ADD;
		if (combine->scale != 1)
			log_once(&scale_reported, "a stage's ADD has a scale the PC's stages do not (left out)");
		break;
	case _nv2a_ff_combine_add_signed:
		*operation = combine->scale >= 2 ? PCD3D_TOP_ADDSIGNED2X : PCD3D_TOP_ADDSIGNED;
		if (combine->scale == 4)
			log_once(&scale_reported, "a stage's ADDSIGNED has a scale of 4 (drawn with 2)");
		break;
	case _nv2a_ff_combine_subtract:
		*operation = PCD3D_TOP_SUBTRACT;
		break;
	case _nv2a_ff_combine_dot3:
		*operation = PCD3D_TOP_DOTPRODUCT3;
		break;
	case _nv2a_ff_combine_interpolate:
		/* OpenGL: a0 * a2 + a1 * (1 - a2); Direct3D's LERP: ARG0 * ARG1 +
		(1 - ARG0) * ARG2 */
		if (has_operation(PCD3D_TOP_LERP))
		{
			*operation = PCD3D_TOP_LERP;
			arguments[0] = a2;
		}
		else if (combine->operand[2] == _nv2a_ff_operand_alpha)
		{
			*operation = combine->source[2] == _nv2a_ff_source_texture ? PCD3D_TOP_BLENDTEXTUREALPHA :
				combine->source[2] == _nv2a_ff_source_primary ? PCD3D_TOP_BLENDDIFFUSEALPHA :
				combine->source[2] == _nv2a_ff_source_constant ? PCD3D_TOP_BLENDFACTORALPHA :
				PCD3D_TOP_BLENDCURRENTALPHA;
		}
		else
		{
			*operation = PCD3D_TOP_SELECTARG1;
			log_once(&interpolate_reported, "an interpolation the stages cannot do (its first argument kept)");
		}
		break;
	default:
		*operation = PCD3D_TOP_SELECTARG1;
		break;
	}
}

static void stage_environment(const struct nv2a_ff_unit *unit, struct pcd3d_unit *stage)
{
	switch (unit->environment)
	{
	case _nv2a_ff_environment_replace:
		stage->color_operation = stage->alpha_operation = PCD3D_TOP_SELECTARG1;
		stage->color_arguments[1] = stage->alpha_arguments[1] = PCD3D_TA_TEXTURE;
		break;
	case _nv2a_ff_environment_decal:
		stage->color_operation = PCD3D_TOP_BLENDTEXTUREALPHA;
		stage->color_arguments[1] = PCD3D_TA_TEXTURE;
		stage->color_arguments[2] = PCD3D_TA_CURRENT;
		stage->alpha_operation = PCD3D_TOP_SELECTARG1;
		stage->alpha_arguments[1] = PCD3D_TA_CURRENT;
		break;
	case _nv2a_ff_environment_blend:
		stage->color_operation = PCD3D_TOP_LERP;
		stage->color_arguments[0] = PCD3D_TA_TEXTURE;
		stage->color_arguments[1] = PCD3D_TA_TFACTOR;
		stage->color_arguments[2] = PCD3D_TA_CURRENT;
		stage->alpha_operation = PCD3D_TOP_MODULATE;
		stage->alpha_arguments[1] = PCD3D_TA_TEXTURE;
		stage->alpha_arguments[2] = PCD3D_TA_CURRENT;
		break;
	case _nv2a_ff_environment_add:
		stage->color_operation = PCD3D_TOP_ADD;
		stage->color_arguments[1] = PCD3D_TA_TEXTURE;
		stage->color_arguments[2] = PCD3D_TA_CURRENT;
		stage->alpha_operation = PCD3D_TOP_MODULATE;
		stage->alpha_arguments[1] = PCD3D_TA_TEXTURE;
		stage->alpha_arguments[2] = PCD3D_TA_CURRENT;
		break;
	case _nv2a_ff_environment_combine:
		stage_combine(&unit->rgb, &stage->color_operation, stage->color_arguments);
		stage_combine(&unit->alpha, &stage->alpha_operation, stage->alpha_arguments);
		break;
	default:
		stage->color_operation = stage->alpha_operation = PCD3D_TOP_MODULATE;
		stage->color_arguments[1] = stage->alpha_arguments[1] = PCD3D_TA_TEXTURE;
		stage->color_arguments[2] = stage->alpha_arguments[2] = PCD3D_TA_CURRENT;
		break;
	}
}

static unsigned char address_mode(DWORD mode)
{
	return mode >= D3DTADDRESS_WRAP && mode <= D3DTADDRESS_BORDER ? (unsigned char)mode : 3;
}

static unsigned char filter_mode(DWORD filter)
{
	return filter <= D3DTEXF_ANISOTROPIC ? (unsigned char)filter : 2;
}

/* ---------- draws */

/* scratch arrays for a draw's vertices, grown as needed */
static struct
{
	unsigned long capacity;
	float (*positions)[4];
	float (*texcoords[4])[4];
	unsigned char (*diffuse)[4];
	unsigned char (*specular)[4];
	float *fog;
	float *fog_factors;
	struct pcd3d_vertex *vertices;
	unsigned long index_capacity;
	unsigned short *indices;
} scratch;

static BOOL scratch_reserve(unsigned long count)
{
	int index;

	if (count <= scratch.capacity)
		return TRUE;
	count += count / 2 + 64;
	free(scratch.positions);
	for (index = 0; index < 4; index++)
		free(scratch.texcoords[index]);
	free(scratch.diffuse);
	free(scratch.specular);
	free(scratch.fog);
	free(scratch.fog_factors);
	free(scratch.vertices);
	scratch.positions = malloc(count * sizeof(*scratch.positions));
	for (index = 0; index < 4; index++)
		scratch.texcoords[index] = malloc(count * sizeof(*scratch.texcoords[index]));
	scratch.diffuse = malloc(count * sizeof(*scratch.diffuse));
	scratch.specular = malloc(count * sizeof(*scratch.specular));
	scratch.fog = malloc(count * sizeof(*scratch.fog));
	scratch.fog_factors = malloc(count * sizeof(*scratch.fog_factors));
	scratch.vertices = malloc(count * sizeof(*scratch.vertices));
	scratch.capacity = count;
	if (!scratch.positions || !scratch.texcoords[0] || !scratch.texcoords[1] || !scratch.texcoords[2] ||
		!scratch.texcoords[3] || !scratch.diffuse || !scratch.specular || !scratch.fog || !scratch.fog_factors ||
		!scratch.vertices)
	{
		scratch.capacity = 0;
		return FALSE;
	}
	return TRUE;
}

static BOOL indices_reserve(unsigned long count)
{
	if (count <= scratch.index_capacity)
		return TRUE;
	count += count / 2 + 64;
	free(scratch.indices);
	scratch.indices = malloc(count * sizeof(*scratch.indices));
	scratch.index_capacity = scratch.indices ? count : 0;
	return scratch.indices != NULL;
}

static int pc_primitive(D3DPRIMITIVETYPE type)
{
	switch (type)
	{
	case D3DPT_POINTLIST: return PCD3D_POINTLIST;
	case D3DPT_LINELIST: return PCD3D_LINELIST;
	case D3DPT_LINELOOP:
	case D3DPT_LINESTRIP: return PCD3D_LINESTRIP;
	case D3DPT_TRIANGLESTRIP:
	case D3DPT_QUADSTRIP: return PCD3D_TRIANGLESTRIP;
	case D3DPT_TRIANGLEFAN:
	case D3DPT_POLYGON: return PCD3D_TRIANGLEFAN;
	default: return PCD3D_TRIANGLELIST;
	}
}

struct draw_statistics
{
	unsigned long draws, passes, vertices, skipped_target, skipped_program, fallback_plans;
};

static struct draw_statistics statistics;

/* testing (setting d3d_trace_frame=<n>): every draw of frame n in the log */
static BOOL trace_frame(void)
{
	static long frame = -1;

	if (frame < 0)
		frame = dos_setting_integer("d3d_trace_frame", 0);
	return frame && (long)device.frame == frame;
}

/* the pixel state as the planner reads it, its draw state and constants */
static void pixel_state(struct plan_key *key, struct nv2a_ff_constants *constants, struct xd3d_bound_texture *bound,
	BOOL *bound_valid)
{
	DWORD *rs = D3D__RenderState;
	int stage;

	memset(key, 0, sizeof(*key));
	memcpy(key->state.combiner_state, rs, sizeof(key->state.combiner_state));
	memset(&key->state.combiner_state[D3DRS_PSCONSTANT0_0], 0, 16 * sizeof(DWORD));
	key->state.combiner_state[D3DRS_PSFINALCOMBINERCONSTANT0] = 0;
	key->state.combiner_state[D3DRS_PSFINALCOMBINERCONSTANT1] = 0;
	key->state.texture_modes = rs[D3DRS_PSTEXTUREMODES];
	for (stage = 0; stage < NV2A_FF_STAGE_COUNT; stage++)
	{
		unsigned long texture_mode = (rs[D3DRS_PSTEXTUREMODES] >> (5 * stage)) & 0x1f;

		bound_valid[stage] = FALSE;
		/* (modes that sample no texture: none, clip plane, pass-through) */
		if (texture_mode != 0 && texture_mode != 0x04 && texture_mode != 0x05 && texture_mode != 0x11)
		{
			bound_valid[stage] = xd3d_texture_get(device.textures[stage], device.palettes[stage], &bound[stage]);
			key->state.sampler_type[stage] = !bound_valid[stage] ? _xgpu_sampler_none :
				bound[stage].cube_map ? _xgpu_sampler_cube : _xgpu_sampler_2d;
		}
		else if (texture_mode == 0x11)
		{
			key->state.sampler_type[stage] = _xgpu_sampler_2d;
		}
		key->state.alpha_kill[stage] = D3D__TextureState[stage][D3DTSS_ALPHAKILL] == D3DTALPHAKILL_ENABLE;
		key->state.color_sign[stage] = (unsigned char)((D3D__TextureState[stage][D3DTSS_COLORSIGN] >> 28) & 0xf);
	}
	key->state.alpha_test_function = rs[D3DRS_ALPHATESTENABLE] ? rs[D3DRS_ALPHAFUNC] : 0;
	key->state.fog_enable = rs[D3DRS_FOGENABLE] != 0;
	key->state.fog_table_mode = (unsigned char)rs[D3DRS_FOGTABLEMODE];

	key->draw.blend_enable = rs[D3DRS_ALPHABLENDENABLE] != 0;
	key->draw.source_blend = key->draw.blend_enable ? rs[D3DRS_SRCBLEND] : _nv2a_ff_blend_one;
	key->draw.destination_blend = key->draw.blend_enable ? rs[D3DRS_DESTBLEND] : _nv2a_ff_blend_zero;
	key->draw.blend_operation = key->draw.blend_enable ? nv2a_blend_operation(rs[D3DRS_BLENDOP]) :
		_nv2a_ff_blend_operation_add;
	key->draw.write_color = (rs[D3DRS_COLORWRITEENABLE] &
		(D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE)) != 0;
	key->draw.write_alpha = (rs[D3DRS_COLORWRITEENABLE] & D3DCOLORWRITEENABLE_ALPHA) != 0;
	key->draw.depth_write = rs[D3DRS_ZENABLE] && rs[D3DRS_ZWRITEENABLE] && rs[D3DRS_ZFUNC] != D3DCMP_NEVER;

	memset(constants, 0, sizeof(*constants));
	for (stage = 0; stage < 8; stage++)
	{
		color_to_vec4(rs[D3DRS_PSCONSTANT0_0 + stage], constants->c0[stage]);
		color_to_vec4(rs[D3DRS_PSCONSTANT1_0 + stage], constants->c1[stage]);
	}
	color_to_vec4(rs[D3DRS_PSFINALCOMBINERCONSTANT0], constants->final_c0);
	color_to_vec4(rs[D3DRS_PSFINALCOMBINERCONSTANT1], constants->final_c1);
	color_to_vec4(rs[D3DRS_FOGCOLOR], constants->fog_color);
	constants->fog_parameters[0] = dword_to_float(rs[D3DRS_FOGSTART]);
	constants->fog_parameters[1] = dword_to_float(rs[D3DRS_FOGEND]);
	constants->fog_parameters[2] = dword_to_float(rs[D3DRS_FOGDENSITY]);
	constants->alpha_reference = (float)(rs[D3DRS_ALPHAREF] & 0xff);
	color_to_vec4(rs[D3DRS_BLENDCOLOR], constants->blend_color);
	for (stage = 0; stage < NV2A_FF_STAGE_COUNT; stage++)
	{
		constants->bump_luminance[stage][0] = dword_to_float(D3D__TextureState[stage][D3DTSS_BUMPENVLSCALE]);
		constants->bump_luminance[stage][1] = dword_to_float(D3D__TextureState[stage][D3DTSS_BUMPENVLOFFSET]);
	}
}

/* the raster state the passes share */
static void raster_state(struct pcd3d_state *state)
{
	DWORD *rs = D3D__RenderState;
	DWORD write = rs[D3DRS_COLORWRITEENABLE];

	state->z_enable = rs[D3DRS_ZENABLE] != 0 && device.depth_stencil != NULL;
	state->z_write = rs[D3DRS_ZWRITEENABLE] != 0;
	state->z_function = compare_function(rs[D3DRS_ZFUNC]);
	state->cull = rs[D3DRS_CULLMODE] == D3DCULL_CW ? 2 : rs[D3DRS_CULLMODE] == D3DCULL_CCW ? 3 : 1;
	state->fill = rs[D3DRS_FILLMODE] == D3DFILL_POINT ? 1 : rs[D3DRS_FILLMODE] == D3DFILL_WIREFRAME ? 2 : 3;
	state->color_write = (unsigned char)(((write & D3DCOLORWRITEENABLE_RED) ? 1 : 0) |
		((write & D3DCOLORWRITEENABLE_GREEN) ? 2 : 0) | ((write & D3DCOLORWRITEENABLE_BLUE) ? 4 : 0) |
		((write & D3DCOLORWRITEENABLE_ALPHA) ? 8 : 0));
	state->stencil_enable = rs[D3DRS_STENCILENABLE] != 0;
	state->stencil_function = compare_function(rs[D3DRS_STENCILFUNC]);
	state->stencil_fail = stencil_operation(rs[D3DRS_STENCILFAIL]);
	state->stencil_z_fail = stencil_operation(rs[D3DRS_STENCILZFAIL]);
	state->stencil_pass = stencil_operation(rs[D3DRS_STENCILPASS]);
	state->stencil_reference = rs[D3DRS_STENCILREF];
	state->stencil_mask = rs[D3DRS_STENCILMASK];
	state->stencil_write_mask = rs[D3DRS_STENCILWRITEMASK];
	state->depth_bias = 0;
	if (rs[D3DRS_SOLIDOFFSETENABLE])
	{
		float offset = -dword_to_float(rs[D3DRS_POLYGONOFFSETZOFFSET]);

		state->depth_bias = offset <= 0.0f ? 0 : offset >= 16.0f ? 16 : (unsigned char)(offset + 0.5f);
	}
}

/* runs the vertex program over vertices first..first + count - 1; FALSE
when it cannot */
static BOOL vertices_evaluate(struct vertex_shader_object *program, BOOL immediate, unsigned long first,
	unsigned long count)
{
	struct nv2a_vsh_cpu_draw run;
	unsigned long index;
	float zscale;

	if (!program->program && !program->program_failed && program->instructions)
	{
		program->program = nv2a_vsh_cpu_program_new(program->instructions, program->instruction_count);
		program->program_failed = !program->program;
	}
	if (!program->program || !scratch_reserve(count))
		return FALSE;
	memset(&run, 0, sizeof(run));
	run.constants = (const float (*)[4])device.constants;
	for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
	{
		memcpy(run.inputs[index].value, device.attributes[index], sizeof(run.inputs[index].value));
		run.inputs[index].type = D3DVSDT_FLOAT4;
		if (immediate)
		{
			run.inputs[index].data = &device.immediate_vertices[0][index][0];
			run.inputs[index].stride = sizeof(device.immediate_vertices[0]);
		}
	}
	if (!immediate)
	{
		/* (the declaration is the current shader's, whichever program runs) */
		const struct vertex_shader_object *declaration = device.vertex_shader ? device.vertex_shader : program;

		for (index = 0; index < declaration->element_count; index++)
		{
			const struct vertex_element *element = &declaration->elements[index];

			if (!device.streams[element->stream].data || !element->bytes)
				continue;
			run.inputs[element->reg].data = (const unsigned char *)device.streams[element->stream].data + element->offset;
			run.inputs[element->reg].stride = device.streams[element->stream].stride;
			run.inputs[element->reg].type = element->type;
		}
	}
	/* The clip-space position, mapped by the PC's viewport as the Xbox's
	maps it: the evaluator works out clip space for a viewport of this
	scale and offset, which are the Xbox's (in its depth units); its own
	half-pixel bias for OpenGL is taken back in the offsets, Direct3D's
	pixel centres being the Xbox's */
	zscale = device.viewport_scale[2] != 0.0f ? device.viewport_scale[2] : 1.0f;
	run.viewport_scale[0] = device.viewport.Width * 0.5f;
	run.viewport_scale[1] = -(float)device.viewport.Height * 0.5f;
	run.viewport_scale[2] = zscale;
	run.viewport_offset[0] = device.viewport.X + device.viewport.Width * 0.5f + 0.5f;
	run.viewport_offset[1] = device.viewport.Y + device.viewport.Height * 0.5f + 0.5f;
	run.viewport_offset[2] = device.viewport_offset[2];
	run.screen_offset = 0.0f;
	run.flags = NV2A_VSH_CPU_POSITION_CAPTURED;
	run.position.data = scratch.positions;
	run.position.stride = sizeof(*scratch.positions);
	for (index = 0; index < 4; index++)
	{
		run.texcoords[index].array.data = scratch.texcoords[index];
		run.texcoords[index].array.stride = sizeof(*scratch.texcoords[index]);
		run.texcoords[index].source = (unsigned char)(_nv2a_vsh_output_texcoord0 + index);
		run.texcoords[index].components = 4;
	}
	run.colors[0].array.data = scratch.diffuse;
	run.colors[0].array.stride = sizeof(*scratch.diffuse);
	run.colors[0].color_source = _nv2a_vsh_output_diffuse;
	run.colors[0].alpha_source = _nv2a_vsh_output_diffuse;
	run.colors[0].alpha_component = 3;
	run.colors[1].array.data = scratch.specular;
	run.colors[1].array.stride = sizeof(*scratch.specular);
	run.colors[1].color_source = _nv2a_vsh_output_specular;
	run.colors[1].alpha_source = _nv2a_vsh_output_specular;
	run.colors[1].alpha_component = 3;
	run.fog.data = scratch.fog;
	run.fog.stride = sizeof(*scratch.fog);
	return nv2a_vsh_cpu_run(program->program, &run, first, count);
}

/* draws vertices first..first + count - 1 (as evaluated), in order or
by indices into them */
static void draw_passes(D3DPRIMITIVETYPE type, unsigned long count, const unsigned short *indices,
	unsigned long index_count)
{
	struct plan_key key;
	struct nv2a_ff_constants constants;
	struct xd3d_bound_texture bound[NV2A_FF_STAGE_COUNT];
	BOOL bound_valid[NV2A_FF_STAGE_COUNT];
	struct plan_entry *entry;
	const struct nv2a_ff_plan *plan;
	struct pcd3d_state state;
	unsigned long vertex, index;
	int pass_index, unit, stage;
	BOOL projected[NV2A_FF_STAGE_COUNT] = { FALSE };

	pixel_state(&key, &constants, bound, bound_valid);
	entry = plan_get(&key);
	if (!entry)
		return;
	plan = &entry->plan;
	if (entry->fallback)
		statistics.fallback_plans++;

	/* the fog factors, and which stages' coordinates divide by their last */
	for (vertex = 0; vertex < count; vertex++)
		scratch.fog_factors[vertex] = fog_factor_from_coordinate(&key.state, &constants, scratch.fog[vertex]);
	for (stage = 0; stage < NV2A_FF_STAGE_COUNT; stage++)
	{
		if (!bound_valid[stage] || bound[stage].cube_map)
			continue;
		for (vertex = 0; vertex < count; vertex++)
		{
			if (scratch.texcoords[stage][vertex][3] != 1.0f)
			{
				projected[stage] = TRUE;
				break;
			}
		}
	}

	memset(&state, 0, sizeof(state));
	raster_state(&state);
	for (pass_index = 0; pass_index < plan->pass_count; pass_index++)
	{
		const struct nv2a_ff_pass *pass = &plan->passes[pass_index];
		struct nv2a_ff_values values;
		BOOL constant_set = FALSE;
		float fog_color[3];

		state.unit_count = pass->unit_count < device.capabilities.units ? pass->unit_count : device.capabilities.units;
		state.texture_factor = 0xffffffffUL;
		for (unit = 0; unit < (int)state.unit_count; unit++)
		{
			const struct nv2a_ff_unit *plan_unit = &pass->units[unit];
			struct pcd3d_unit *pc_unit = &state.units[unit];

			memset(pc_unit, 0, sizeof(*pc_unit));
			stage_environment(plan_unit, pc_unit);
			stage = plan_unit->stage;
			if (stage < NV2A_FF_STAGE_COUNT && bound_valid[stage])
			{
				pc_unit->sampler.texture = bound[stage].texture;
				pc_unit->sampler.address[0] = address_mode(D3D__TextureState[stage][D3DTSS_ADDRESSU]);
				pc_unit->sampler.address[1] = address_mode(D3D__TextureState[stage][D3DTSS_ADDRESSV]);
				pc_unit->sampler.address[2] = address_mode(D3D__TextureState[stage][D3DTSS_ADDRESSW]);
				pc_unit->sampler.magnification = filter_mode(D3D__TextureState[stage][D3DTSS_MAGFILTER]);
				pc_unit->sampler.minification = filter_mode(D3D__TextureState[stage][D3DTSS_MINFILTER]);
				pc_unit->sampler.mip = filter_mode(D3D__TextureState[stage][D3DTSS_MIPFILTER]);
				pc_unit->sampler.maximum_anisotropy = (unsigned char)D3D__TextureState[stage][D3DTSS_MAXANISOTROPY];
				pc_unit->sampler.border = D3D__TextureState[stage][D3DTSS_BORDERCOLOR];
				pc_unit->sampler.lod_bias = dword_to_float(D3D__TextureState[stage][D3DTSS_MIPMAPLODBIAS]);
				pc_unit->sampler.components = bound[stage].cube_map ? 3 : projected[stage] ? 3 : 2;
				pc_unit->sampler.projected = (unsigned char)projected[stage];
			}
			if (plan_unit->constant_rgb != NV2A_FF_EXPRESSION_NONE || plan_unit->constant_alpha != NV2A_FF_EXPRESSION_NONE)
			{
				float color[4];
				unsigned long argb;

				nv2a_ff_plan_constant(plan, pass_index, unit, &constants, color);
				argb = vec4_to_argb(color);
				if (!constant_set)
				{
					state.texture_factor = argb;
					constant_set = TRUE;
				}
				else if (state.texture_factor != argb)
				{
					log_once(&constant_reported, "a pass's units want different constant colours (the first kept)");
				}
			}
		}
		if (!state.unit_count)
		{
			/* the colour alone */
			state.unit_count = 1;
			memset(&state.units[0], 0, sizeof(state.units[0]));
			state.units[0].color_operation = state.units[0].alpha_operation = PCD3D_TOP_SELECTARG1;
			state.units[0].color_arguments[1] = state.units[0].alpha_arguments[1] = PCD3D_TA_DIFFUSE;
		}

		/* the alpha test and blend */
		state.alpha_test = pass->alpha_test_function != 0;
		state.alpha_function = compare_function(pass->alpha_test_function);
		if (pass->alpha_reference != NV2A_FF_EXPRESSION_NONE)
		{
			float reference[3];

			memset(&values, 0, sizeof(values));
			values.constants = &constants;
			nv2a_ff_plan_evaluate(plan, pass->alpha_reference, &values, reference);
			state.alpha_reference = (unsigned char)channel_byte(reference[0]);
		}
		else
		{
			state.alpha_reference = (unsigned char)(D3D__RenderState[D3DRS_ALPHAREF] & 0xff);
		}
		state.blend_enable = pass->blend_enable;
		state.source_blend = blend_factor(pass->source_blend);
		state.destination_blend = blend_factor(pass->destination_blend);
		state.blend_operation = blend_operation(pass->blend_operation == _nv2a_ff_blend_operation_subtract ?
			D3DBLENDOP_SUBTRACT : pass->blend_operation == _nv2a_ff_blend_operation_reverse_subtract ?
			D3DBLENDOP_REVSUBTRACT : pass->blend_operation == _nv2a_ff_blend_operation_minimum ? D3DBLENDOP_MIN :
			pass->blend_operation == _nv2a_ff_blend_operation_maximum ? D3DBLENDOP_MAX : D3DBLENDOP_ADD);
		{
			DWORD write = D3D__RenderState[D3DRS_COLORWRITEENABLE];
			unsigned char mask = 0;

			if (pass->write_color)
				mask |= (unsigned char)(((write & D3DCOLORWRITEENABLE_RED) ? 1 : 0) |
					((write & D3DCOLORWRITEENABLE_GREEN) ? 2 : 0) | ((write & D3DCOLORWRITEENABLE_BLUE) ? 4 : 0));
			if (pass->write_alpha)
				mask |= 8;
			state.color_write = mask;
		}
		if (pass->depth_equal)
		{
			state.z_function = 3;
			state.z_write = FALSE;
		}
		else
		{
			state.z_function = compare_function(D3D__RenderState[D3DRS_ZFUNC]);
			state.z_write = D3D__RenderState[D3DRS_ZWRITEENABLE] != 0;
		}

		/* fog */
		state.fog_enable = pass->fog_enable;
		if (pass->fog_enable)
		{
			memset(&values, 0, sizeof(values));
			values.constants = &constants;
			nv2a_ff_plan_evaluate(plan, pass->fog_color, &values, fog_color);
			state.fog_color = 0xff000000UL | (channel_byte(fog_color[0]) << 16) | (channel_byte(fog_color[1]) << 8) |
				channel_byte(fog_color[2]);
		}
		state.specular = pass->secondary_rgb != NV2A_FF_EXPRESSION_NONE;

		/* the vertices, with this pass's colours */
		for (vertex = 0; vertex < count; vertex++)
		{
			struct pcd3d_vertex *out = &scratch.vertices[vertex];
			float v0[4], v1[4], color[4];

			memcpy(out->position, scratch.positions[vertex], sizeof(out->position));
			v0[0] = scratch.diffuse[vertex][0] / 255.0f;
			v0[1] = scratch.diffuse[vertex][1] / 255.0f;
			v0[2] = scratch.diffuse[vertex][2] / 255.0f;
			v0[3] = scratch.diffuse[vertex][3] / 255.0f;
			v1[0] = scratch.specular[vertex][0] / 255.0f;
			v1[1] = scratch.specular[vertex][1] / 255.0f;
			v1[2] = scratch.specular[vertex][2] / 255.0f;
			v1[3] = scratch.specular[vertex][3] / 255.0f;
			memset(&values, 0, sizeof(values));
			values.constants = &constants;
			values.v0 = v0;
			values.v1 = v1;
			values.fog_factor = scratch.fog_factors[vertex];
			if (entry->fallback)
				memcpy(color, v0, sizeof(color));
			else
				nv2a_ff_plan_primary(plan, pass_index, &values, color);
			out->diffuse = vec4_to_argb(color);
			out->specular = 0;
			if (state.specular)
			{
				float secondary[3];

				nv2a_ff_plan_secondary(plan, pass_index, &values, secondary);
				out->specular = (channel_byte(secondary[0]) << 16) | (channel_byte(secondary[1]) << 8) |
					channel_byte(secondary[2]);
			}
			out->fog = 1.0f;
			if (pass->fog_enable)
			{
				float factor[3];

				nv2a_ff_plan_evaluate(plan, pass->fog_factor, &values, factor);
				out->fog = factor[0];
			}
			for (unit = 0; unit < (int)state.unit_count; unit++)
			{
				const float *in;

				stage = pass->unit_count ? pass->units[unit].stage : NV2A_FF_STAGE_NONE;
				if (stage >= NV2A_FF_STAGE_COUNT || !bound_valid[stage])
				{
					memset(out->texcoord[unit], 0, sizeof(out->texcoord[unit]));
					continue;
				}
				in = scratch.texcoords[stage][vertex];
				if (bound[stage].cube_map)
				{
					out->texcoord[unit][0] = in[0];
					out->texcoord[unit][1] = in[1];
					out->texcoord[unit][2] = in[2];
					out->texcoord[unit][3] = 1.0f;
				}
				else
				{
					/* (s, t, q: divided by q when projected) */
					out->texcoord[unit][0] = in[0] * bound[stage].coordinate_scale[0];
					out->texcoord[unit][1] = in[1] * bound[stage].coordinate_scale[1];
					out->texcoord[unit][2] = projected[stage] ? in[3] : 0.0f;
					out->texcoord[unit][3] = 1.0f;
				}
			}
		}
		pcd3d_draw(&state, pc_primitive(type), scratch.vertices, count, indices, index_count);
		statistics.passes++;
		if (trace_frame())
		{
			const float *p = scratch.vertices[0].position;

			platform_log("  pass %d/%d: units %lu (stage %d tex %p), z %d/%d/%d, cull %d, blend %d %d/%d, alpha %d, "
				"write %x, fog %d, v0 %.2f %.2f %.2f %.2f diffuse %08lx", pass_index, plan->pass_count, state.unit_count,
				pass->unit_count ? pass->units[0].stage : -1, (void *)state.units[0].sampler.texture, state.z_enable,
				state.z_write, state.z_function, state.cull, state.blend_enable, state.source_blend,
				state.destination_blend, state.alpha_test, state.color_write, state.fog_enable, (double)p[0],
				(double)p[1], (double)p[2], (double)p[3], scratch.vertices[0].diffuse);
		}
	}
	statistics.draws++;
	statistics.vertices += count;
	(void)index;
}

/* indices for a draw's primitives, from the game's (or none: in order)
less first; quads as triangles. Returns the count, 0 when there is
nothing to draw. */
static unsigned long indices_make(D3DPRIMITIVETYPE type, const WORD *source, unsigned long count, unsigned long first)
{
	unsigned long index, quad;

	if (type == D3DPT_QUADLIST)
	{
		unsigned long quads = count / 4;

		if (!indices_reserve(quads * 6))
			return 0;
		for (quad = 0; quad < quads; quad++)
		{
			unsigned long v[4];

			for (index = 0; index < 4; index++)
				v[index] = source ? source[quad * 4 + index] - first : quad * 4 + index;
			scratch.indices[quad * 6 + 0] = (unsigned short)v[0];
			scratch.indices[quad * 6 + 1] = (unsigned short)v[1];
			scratch.indices[quad * 6 + 2] = (unsigned short)v[2];
			scratch.indices[quad * 6 + 3] = (unsigned short)v[0];
			scratch.indices[quad * 6 + 4] = (unsigned short)v[2];
			scratch.indices[quad * 6 + 5] = (unsigned short)v[3];
		}
		return quads * 6;
	}
	if (!source)
		return 0;
	if (!indices_reserve(count))
		return 0;
	for (index = 0; index < count; index++)
		scratch.indices[index] = (unsigned short)(source[index] - first);
	return count;
}

static BOOL draw_begin(BOOL immediate, struct vertex_shader_object **program)
{
	if (!target_apply())
	{
		if (device.drawing)
			statistics.skipped_target++;
		if (trace_frame())
			platform_log("draw skipped: target %08lx", device.render_target ? (unsigned long)device.render_target->Data : 0);
		return FALSE;
	}
	*program = current_program();
	if (!*program || !(*program)->instructions)
	{
		statistics.skipped_program++;
		if (trace_frame())
			platform_log("draw skipped: no program");
		return FALSE;
	}
	if (trace_frame())
		platform_log("draw%s: program %p (%lu instructions, %lu elements), target %08lx%s, viewport %lu %lu %lu %lu",
			immediate ? " (immediate)" : "", (void *)*program, (*program)->instruction_count,
			device.vertex_shader ? device.vertex_shader->element_count : 0,
			device.render_target ? (unsigned long)device.render_target->Data : 0,
			device.target_is_back_buffer ? " (back buffer)" : "", device.viewport.X, device.viewport.Y,
			device.viewport.Width, device.viewport.Height);
	return TRUE;
}

void WINAPI D3DDevice_DrawVertices(D3DPRIMITIVETYPE primitive_type, UINT start_vertex, UINT vertex_count)
{
	struct vertex_shader_object *program;
	unsigned long index_count;

	if (!vertex_count || !draw_begin(FALSE, &program))
		return;
	if (!vertices_evaluate(program, FALSE, start_vertex, vertex_count))
		return;
	index_count = indices_make(primitive_type, NULL, vertex_count, 0);
	if (primitive_type == D3DPT_QUADLIST)
	{
		if (index_count)
			draw_passes(primitive_type, vertex_count, scratch.indices, index_count);
		return;
	}
	draw_passes(primitive_type, vertex_count, NULL, 0);
}

void WINAPI D3DDevice_DrawIndexedVertices(D3DPRIMITIVETYPE primitive_type, UINT vertex_count, CONST WORD *index_data)
{
	struct vertex_shader_object *program;
	unsigned long minimum = 0xffff, maximum = 0, index, index_count;

	if (!vertex_count || !index_data || !draw_begin(FALSE, &program))
		return;
	for (index = 0; index < vertex_count; index++)
	{
		if (index_data[index] < minimum)
			minimum = index_data[index];
		if (index_data[index] > maximum)
			maximum = index_data[index];
	}
	/* (index i is vertex base + i) */
	if (!vertices_evaluate(program, FALSE, device.base_vertex_index + minimum, maximum - minimum + 1))
		return;
	index_count = indices_make(primitive_type, index_data, vertex_count, minimum);
	if (index_count)
		draw_passes(primitive_type, maximum - minimum + 1, scratch.indices, index_count);
}

/* ---------- immediate mode */

void WINAPI D3DDevice_Begin(D3DPRIMITIVETYPE primitive_type)
{
	device.immediate_active = TRUE;
	device.immediate_type = primitive_type;
	device.immediate_count = 0;
}

static void immediate_emit(void)
{
	if (device.immediate_count == device.immediate_capacity)
	{
		unsigned long capacity = device.immediate_capacity ? device.immediate_capacity * 2 : 256;
		void *vertices = realloc(device.immediate_vertices, capacity * sizeof(device.immediate_vertices[0]));

		if (!vertices)
			return;
		device.immediate_vertices = vertices;
		device.immediate_capacity = capacity;
	}
	memcpy(device.immediate_vertices[device.immediate_count], device.attributes, sizeof(device.attributes));
	device.immediate_count++;
}

void WINAPI D3DDevice_End(void)
{
	struct vertex_shader_object *program;
	unsigned long count = device.immediate_count, index_count;
	D3DPRIMITIVETYPE type = device.immediate_type;

	device.immediate_active = FALSE;
	if (!count || !draw_begin(TRUE, &program))
		return;
	if (!vertices_evaluate(program, TRUE, 0, count))
		return;
	if (type == D3DPT_QUADLIST)
	{
		index_count = indices_make(type, NULL, count, 0);
		if (index_count)
			draw_passes(type, count, scratch.indices, index_count);
		return;
	}
	draw_passes(type, count, NULL, 0);
}

static void set_attribute(INT reg, float a, float b, float c, float d)
{
	BOOL emit = FALSE;

	if (reg == D3DVSDE_VERTEX)
	{
		reg = 0;
		emit = TRUE;
	}
	if (reg < 0 || reg >= XGPU_VERTEX_ATTRIBUTE_COUNT)
		return;
	device.attributes[reg][0] = a;
	device.attributes[reg][1] = b;
	device.attributes[reg][2] = c;
	device.attributes[reg][3] = d;
	/* like the hardware, writing register 0 completes a vertex */
	if (device.immediate_active && (emit || reg == 0))
		immediate_emit();
}

void WINAPI D3DDevice_SetVertexData2f(INT reg, FLOAT a, FLOAT b)
{
	set_attribute(reg, a, b, 0.0f, 1.0f);
}

void WINAPI D3DDevice_SetVertexData4f(INT reg, FLOAT a, FLOAT b, FLOAT c, FLOAT d)
{
	set_attribute(reg, a, b, c, d);
}

void WINAPI D3DDevice_SetVertexData2s(INT reg, SHORT a, SHORT b)
{
	set_attribute(reg, (float)a, (float)b, 0.0f, 1.0f);
}

void WINAPI D3DDevice_SetVertexData4ub(INT reg, BYTE a, BYTE b, BYTE c, BYTE d)
{
	set_attribute(reg, a / 255.0f, b / 255.0f, c / 255.0f, d / 255.0f);
}

void WINAPI D3DDevice_SetVertexDataColor(INT reg, D3DCOLOR color)
{
	float value[4];

	color_to_vec4(color, value);
	set_attribute(reg, value[0], value[1], value[2], value[3]);
}

/* ---------- clearing */

void WINAPI D3DDevice_Clear(DWORD count, CONST D3DRECT *rectangles, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
	static BOOL partial_reported;
	long boxes[16 * 4];
	unsigned long pc_flags = 0;
	DWORD index;

	if (!target_apply())
		return;
	/* (a clear of alpha alone is left out: the PC's back buffer has none,
	and a clear of all four would wipe the picture) */
	if ((flags & (D3DCLEAR_TARGET_R | D3DCLEAR_TARGET_G | D3DCLEAR_TARGET_B)) ==
		(D3DCLEAR_TARGET_R | D3DCLEAR_TARGET_G | D3DCLEAR_TARGET_B))
	{
		pc_flags |= PCD3D_CLEAR_TARGET;
	}
	else if (flags & (D3DCLEAR_TARGET_R | D3DCLEAR_TARGET_G | D3DCLEAR_TARGET_B))
	{
		log_once(&partial_reported, "a clear of some of the colour channels (left out)");
	}
	if ((flags & D3DCLEAR_ZBUFFER) && device.depth_stencil)
		pc_flags |= PCD3D_CLEAR_ZBUFFER;
	if ((flags & D3DCLEAR_STENCIL) && device.depth_stencil)
		pc_flags |= PCD3D_CLEAR_STENCIL;
	if (count > 16)
		count = 16;
	for (index = 0; index < count && rectangles; index++)
	{
		boxes[index * 4 + 0] = rectangles[index].x1;
		boxes[index * 4 + 1] = rectangles[index].y1;
		boxes[index * 4 + 2] = rectangles[index].x2;
		boxes[index * 4 + 3] = rectangles[index].y2;
	}
	pcd3d_clear(rectangles ? count : 0, boxes, pc_flags, color, z, stencil);
}

/* ---------- presentation */

/* testing (settings screenshot_frame=<n>, screenshot_every=<n>,
quit_frame=<n>): the picture of a frame as SHOTnnnn.BMP beside the
program, and the end of the program at a frame */
static void screenshot_write(unsigned long frame)
{
	unsigned long width, height, x, y;
	unsigned long *pixels;
	unsigned char header[54], *row;
	unsigned long row_bytes, padded;
	char name[64], path[300];
	FILE *file;

	pcd3d_size(&width, &height);
	pixels = malloc(width * height * sizeof(*pixels));
	row_bytes = width * 3;
	padded = (row_bytes + 3) & ~3UL;
	row = calloc(1, padded);
	if (!pixels || !row || !pcd3d_read_back(pixels, width, height))
	{
		free(pixels);
		free(row);
		return;
	}
	snprintf(name, sizeof(name), "d:\\SHOT%04lu.BMP", frame % 10000);
	platform_translate_path(name, path, sizeof(path));
	file = fopen(path, "wb");
	if (file)
	{
		memset(header, 0, sizeof(header));
		header[0] = 'B';
		header[1] = 'M';
		*(unsigned long *)(header + 2) = 54 + padded * height;
		*(unsigned long *)(header + 10) = 54;
		*(unsigned long *)(header + 14) = 40;
		*(long *)(header + 18) = (long)width;
		*(long *)(header + 22) = (long)height;
		*(unsigned short *)(header + 26) = 1;
		*(unsigned short *)(header + 28) = 24;
		*(unsigned long *)(header + 34) = padded * height;
		fwrite(header, 1, sizeof(header), file);
		for (y = height; y-- > 0;)
		{
			for (x = 0; x < width; x++)
			{
				unsigned long c = pixels[y * width + x];

				row[x * 3 + 0] = (unsigned char)c;
				row[x * 3 + 1] = (unsigned char)(c >> 8);
				row[x * 3 + 2] = (unsigned char)(c >> 16);
			}
			fwrite(row, 1, padded, file);
		}
		fclose(file);
		platform_log("screenshot SHOT%04lu.BMP (%lux%lu)", frame % 10000, width, height);
	}
	free(pixels);
	free(row);
}

static void testing_frame(void)
{
	static long screenshot_frame = -1, screenshot_every, quit_frame;
	static unsigned long long last_time;
	static unsigned long timed_frames;
	unsigned long long now = system_microseconds();

	if (screenshot_frame < 0)
	{
		screenshot_frame = dos_setting_integer("screenshot_frame", 0);
		screenshot_every = dos_setting_integer("screenshot_every", 0);
		quit_frame = dos_setting_integer("quit_frame", 0);
		last_time = now;
	}
	timed_frames++;
	if (now - last_time >= 5000000)
	{
		platform_log("frame %lu: %.1f ms a frame; last frame %lu draws, %lu passes, %lu vertices, %lu skipped "
			"(target), %lu skipped (program), %lu with the built-in plan", device.frame,
			(double)(now - last_time) / 1000.0 / (double)timed_frames, statistics.draws, statistics.passes,
			statistics.vertices, statistics.skipped_target, statistics.skipped_program, statistics.fallback_plans);
		last_time = now;
		timed_frames = 0;
	}
	if ((screenshot_frame && (long)device.frame == screenshot_frame) ||
		(screenshot_every && device.frame % (unsigned long)screenshot_every == 0))
	{
		screenshot_write(device.frame);
	}
	if (quit_frame && (long)device.frame >= quit_frame)
	{
		platform_log("quit_frame reached");
		exit(0);
	}
}

void WINAPI D3DDevice_Present(CONST RECT *source_rectangle, CONST RECT *destination_rectangle, void *unused,
	void *unused2)
{
	(void)source_rectangle;
	(void)destination_rectangle;
	(void)unused;
	(void)unused2;
	if (trace_frame())
	{
		extern void d3d_trace_visibility(void);

		d3d_trace_visibility();
	}
	device.frame++;
	if (device.drawing)
	{
		testing_frame();
		pcd3d_present();
		xd3d_textures_frame_end();
	}
	memset(&statistics, 0, sizeof(statistics));
	pending_flips++;
	d3d_vertical_blank_poll();
}

unsigned long xd3d_frame_number(void)
{
	return device.frame;
}

/* ---------- the port's own drawing over the game's (its menu: dos_menu.c
through port/dos/d3d/game/d3d_glue.c), in the screen's 640x480 pixels */

#define OVERLAY_PICTURES 8

static struct
{
	const unsigned long *pixels;
	int width, height;
	struct pcd3d_texture *texture;
	float u, v;
	unsigned long used;
} overlay_pictures[OVERLAY_PICTURES];

void xd3d_overlay_quad(float x0, float y0, float x1, float y1, unsigned long argb, struct pcd3d_texture *texture,
	float u0, float v0, float u1, float v1)
{
	struct pcd3d_state state;
	struct pcd3d_vertex vertices[4];
	int index;

	if (!device.drawing)
		return;
	pcd3d_target(NULL, TRUE, SCREEN_WIDTH, SCREEN_HEIGHT);
	pcd3d_viewport(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 0.0f, 1.0f);
	/* (the game's next draw sets its own target and viewport again) */
	device.target_dirty = TRUE;
	xd3d_back_buffer_changes++;
	memset(&state, 0, sizeof(state));
	state.unit_count = 1;
	state.units[0].sampler.texture = texture;
	state.units[0].sampler.address[0] = state.units[0].sampler.address[1] = state.units[0].sampler.address[2] = 3;
	state.units[0].sampler.magnification = state.units[0].sampler.minification = 2;
	state.units[0].sampler.components = 2;
	state.units[0].color_operation = state.units[0].alpha_operation = texture ? PCD3D_TOP_MODULATE :
		PCD3D_TOP_SELECTARG1;
	state.units[0].color_arguments[1] = state.units[0].alpha_arguments[1] = texture ? PCD3D_TA_TEXTURE :
		PCD3D_TA_DIFFUSE;
	state.units[0].color_arguments[2] = state.units[0].alpha_arguments[2] = PCD3D_TA_DIFFUSE;
	state.z_function = 8;
	state.cull = 1;
	state.fill = 3;
	state.blend_enable = TRUE;
	state.source_blend = 5;
	state.destination_blend = 6;
	state.blend_operation = 1;
	state.color_write = 15;
	memset(vertices, 0, sizeof(vertices));
	for (index = 0; index < 4; index++)
	{
		float x = index == 1 || index == 2 ? x1 : x0;
		float y = index >= 2 ? y1 : y0;

		vertices[index].position[0] = x / (SCREEN_WIDTH * 0.5f) - 1.0f;
		vertices[index].position[1] = 1.0f - y / (SCREEN_HEIGHT * 0.5f);
		vertices[index].position[2] = 0.0f;
		vertices[index].position[3] = 1.0f;
		vertices[index].diffuse = argb;
		vertices[index].fog = 1.0f;
		vertices[index].texcoord[0][0] = index == 1 || index == 2 ? u1 : u0;
		vertices[index].texcoord[0][1] = index >= 2 ? v1 : v0;
		vertices[index].texcoord[0][3] = 1.0f;
	}
	pcd3d_draw(&state, PCD3D_TRIANGLEFAN, vertices, 4, NULL, 0);
}

/* a picture of 32-bit ARGB pixels at x, y (kept by its address for the
next frames: the menu draws the same few each frame) */
void xd3d_overlay_picture(float x, float y, const unsigned long *pixels, int width, int height)
{
	static unsigned long uses;
	int index, chosen = 0;
	unsigned long texture_width = 1, texture_height = 1;

	if (!device.drawing || !pixels || width <= 0 || height <= 0)
		return;
	for (index = 0; index < OVERLAY_PICTURES; index++)
	{
		if (overlay_pictures[index].pixels == pixels && overlay_pictures[index].width == width &&
			overlay_pictures[index].height == height && overlay_pictures[index].texture)
		{
			break;
		}
		if (overlay_pictures[index].used < overlay_pictures[chosen].used)
			chosen = index;
	}
	if (index == OVERLAY_PICTURES)
	{
		unsigned long pitch, row;
		unsigned char *texels;

		index = chosen;
		if (overlay_pictures[index].texture)
			pcd3d_texture_delete(overlay_pictures[index].texture);
		memset(&overlay_pictures[index], 0, sizeof(overlay_pictures[index]));
		while (texture_width < (unsigned long)width)
			texture_width <<= 1;
		while (texture_height < (unsigned long)height)
			texture_height <<= 1;
		overlay_pictures[index].texture = pcd3d_texture_new(texture_width, texture_height, 1, FALSE, FALSE);
		if (!overlay_pictures[index].texture)
			return;
		texels = pcd3d_texture_lock(overlay_pictures[index].texture, 0, 0, &pitch);
		if (texels)
		{
			for (row = 0; row < texture_height; row++)
			{
				unsigned long source_row = row < (unsigned long)height ? row : (unsigned long)height - 1;
				unsigned long *out = (unsigned long *)(texels + row * pitch);
				unsigned long column;

				for (column = 0; column < texture_width; column++)
					out[column] = pixels[source_row * width + (column < (unsigned long)width ? column : (unsigned long)width - 1)];
			}
			pcd3d_texture_unlock(overlay_pictures[index].texture, 0, 0);
		}
		overlay_pictures[index].pixels = pixels;
		overlay_pictures[index].width = width;
		overlay_pictures[index].height = height;
		overlay_pictures[index].u = (float)width / (float)texture_width;
		overlay_pictures[index].v = (float)height / (float)texture_height;
	}
	overlay_pictures[index].used = ++uses;
	xd3d_overlay_quad(x, y, x + width, y + height, 0xffffffffUL, overlay_pictures[index].texture, 0.0f, 0.0f,
		overlay_pictures[index].u, overlay_pictures[index].v);
}
