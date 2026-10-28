/*
XD3D.H

Inside the Xbox half of the Direct3D build's device (port/dos/DIRECT3D.md):
the Xbox's Direct3D, as Halo's own renderer (source/rasterizer/xbox) calls
it, over the PC half (pcd3d.h).

- xd3d_resources.c: textures, surfaces, buffers and palettes, kept as the
  Xbox keeps them: headers whose Data is the address of their memory
  (here a plain pointer: Windows 95 builds are 32-bit), texels in the
  Xbox's layouts;
- xd3d_textures.c: the PC's copy of each texture the game draws with,
  made from the Xbox's texels (every format as 32-bit ARGB), and the
  targets the game draws into;
- xd3d_device.c: the device's state, vertex programs, draws, clears,
  presentation and the vertical blank.
*/

#ifndef HALO_XD3D_H
#define HALO_XD3D_H

#include "xgpu.h"
#include "pcd3d.h"

/* ---------- the layout of a texture in memory (xd3d_resources.c) */

struct xd3d_texture_description
{
	DWORD format; /* D3DFMT_* */
	unsigned long width, height, depth, levels;
	BOOL cube_map;
	/* not swizzled: addressed with texel coordinates */
	BOOL linear;
	BOOL compressed;
	/* a linear texture's bytes from one row to the next */
	unsigned long pitch;
	/* the texel's kind (xbox_textures_convert.h) and bytes, of a 4x4
	block for a compressed one */
	int kind;
	unsigned long bytes;
};

void xd3d_texture_describe(DWORD format_word, DWORD size_word, struct xd3d_texture_description *description);
/* bytes of one face, its levels included (cube faces padded) */
unsigned long xd3d_texture_face_size(const struct xd3d_texture_description *description);
unsigned long xd3d_texture_level_offset(const struct xd3d_texture_description *description, unsigned long level);
unsigned long xd3d_texture_level_pitch(const struct xd3d_texture_description *description, unsigned long level);
BOOL xd3d_format_is_depth(DWORD format);

/* the surfaces the device owns: its back buffer and depth buffer */
void xd3d_surface_initialize(D3DSurface *surface, D3DFORMAT format, unsigned long width, unsigned long height);

/* Every texture header carries in Lock a number that changes when its
texels may have: at registration (a map's bitmaps, loaded into the
texture cache's memory) and at every lock. The PC's copy is remade when
the number differs from the one it was made from. */
DWORD xd3d_resource_serial(void);

/* ---------- the PC's copies (xd3d_textures.c) */

/* what a stage samples: the texture and what its coordinates are
multiplied by (linear textures are addressed in texels) */
struct xd3d_bound_texture
{
	struct pcd3d_texture *texture;
	float coordinate_scale[2];
	BOOL cube_map, volume;
};

/* the PC's texture for an Xbox texture header (and its palette); FALSE
when there is none to draw with */
BOOL xd3d_texture_get(const D3DBaseTexture *texture, const D3DPalette *palette, struct xd3d_bound_texture *bound);
/* the target texture for a surface the game draws into; NULL for the back
buffer, or when the PC cannot draw into textures (*usable FALSE) */
struct pcd3d_texture *xd3d_target_get(const D3DSurface *surface, BOOL *usable);
/* the frame is over: copies unused for long are let go */
void xd3d_textures_frame_end(void);

/* the back buffer's Data and size (xd3d_device.c) */
const D3DSurface *xd3d_back_buffer(void);

#endif
