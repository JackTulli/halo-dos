/*
PCD3D.H

Between the two halves of the Direct3D build's device (port/dos/DIRECT3D.md):
the Xbox half (port/dos/d3d/xbox) implements the Xbox's Direct3D for
Halo's own renderer, runs its vertex programs on the processor and turns
its pixel state into passes of texture stages; the PC half
(port/dos/d3d/pc) draws those passes with a PC's Direct3D 8. The Xbox's
and the PC's Direct3D headers declare the same names differently, so
neither half sees the other's: what crosses is in plain C here.

Numbers that name Direct3D states (stage operations and arguments,
comparisons, blend factors, cull modes...) are the PC's Direct3D 8 values,
listed below; the Xbox half translates the Xbox's into them.
*/

#ifndef HALO_PCD3D_H
#define HALO_PCD3D_H

/* ---------- the PC's Direct3D 8 values */

/* D3DTEXTUREOP */
enum
{
	PCD3D_TOP_DISABLE = 1,
	PCD3D_TOP_SELECTARG1 = 2,
	PCD3D_TOP_SELECTARG2 = 3,
	PCD3D_TOP_MODULATE = 4,
	PCD3D_TOP_MODULATE2X = 5,
	PCD3D_TOP_MODULATE4X = 6,
	PCD3D_TOP_ADD = 7,
	PCD3D_TOP_ADDSIGNED = 8,
	PCD3D_TOP_ADDSIGNED2X = 9,
	PCD3D_TOP_SUBTRACT = 10,
	PCD3D_TOP_BLENDDIFFUSEALPHA = 12,
	PCD3D_TOP_BLENDTEXTUREALPHA = 13,
	PCD3D_TOP_BLENDFACTORALPHA = 14,
	PCD3D_TOP_BLENDCURRENTALPHA = 16,
	PCD3D_TOP_DOTPRODUCT3 = 24,
	PCD3D_TOP_LERP = 26
};

/* D3DTA_*: an argument and its modifiers */
enum
{
	PCD3D_TA_DIFFUSE = 0,
	PCD3D_TA_CURRENT = 1,
	PCD3D_TA_TEXTURE = 2,
	PCD3D_TA_TFACTOR = 3,
	PCD3D_TA_SPECULAR = 4,
	PCD3D_TA_COMPLEMENT = 0x10,
	PCD3D_TA_ALPHAREPLICATE = 0x20
};

/* D3DCMPFUNC: NEVER 1 .. ALWAYS 8; D3DBLEND: ZERO 1 .. SRCALPHASAT 11;
D3DBLENDOP: ADD 1, SUBTRACT 2, REVSUBTRACT 3, MIN 4, MAX 5; D3DCULL: NONE
1, CW 2, CCW 3; D3DFILLMODE: POINT 1, WIREFRAME 2, SOLID 3; D3DSTENCILOP:
KEEP 1 .. DECR 8; D3DTEXTUREADDRESS: WRAP 1, MIRROR 2, CLAMP 3, BORDER 4;
D3DTEXTUREFILTERTYPE: NONE 0, POINT 1, LINEAR 2, ANISOTROPIC 3 */

/* D3DPRIMITIVETYPE */
enum
{
	PCD3D_POINTLIST = 1,
	PCD3D_LINELIST = 2,
	PCD3D_LINESTRIP = 3,
	PCD3D_TRIANGLELIST = 4,
	PCD3D_TRIANGLESTRIP = 5,
	PCD3D_TRIANGLEFAN = 6
};

/* ---------- what the device can do */

/* operations the stages have (D3DTEXOPCAPS), a bit for each PCD3D_TOP_ */
#define PCD3D_OPERATION(operation) (1UL << ((operation) - 1))

struct pcd3d_capabilities
{
	/* texture stages that can each sample a texture, up to 4 */
	unsigned long units;
	unsigned long operations;
	/* D3DRS_BLENDOP other than ADD */
	int blend_operations;
	int cube_maps;
	int volume_maps;
	/* textures' sides must be powers of two (and square) */
	int power_of_two;
	int square;
	unsigned long maximum_texture_size;
	/* the back buffer keeps alpha */
	int destination_alpha;
	/* D3DRS_COLORWRITEENABLE */
	int color_write;
	/* textures can be drawn into */
	int render_target_textures;
	/* the device's description, for the log */
	char description[160];
};

/* ---------- the device */

/* creates the device on the window (a HWND), its back buffer the window's
size; 0 when it cannot */
int pcd3d_open(void *window, unsigned long width, unsigned long height);
void pcd3d_close(void);
void pcd3d_capabilities(struct pcd3d_capabilities *capabilities);
/* the back buffer's size */
void pcd3d_size(unsigned long *width, unsigned long *height);

/* the frame to the window */
void pcd3d_present(void);
/* the back buffer as 32-bit ARGB words, top row first (screenshots); 0
when it cannot be read */
int pcd3d_read_back(unsigned long *argb, unsigned long width, unsigned long height);

/* ---------- textures */

struct pcd3d_texture;

/* a texture of 32-bit ARGB texels (in faces of six for a cube map);
target: one the device draws into (no levels below the first) */
struct pcd3d_texture *pcd3d_texture_new(unsigned long width, unsigned long height, unsigned long levels,
	int cube_map, int target);
void pcd3d_texture_delete(struct pcd3d_texture *texture);
/* a level's texels for writing; NULL when it cannot be locked */
void *pcd3d_texture_lock(struct pcd3d_texture *texture, int face, int level, unsigned long *pitch);
void pcd3d_texture_unlock(struct pcd3d_texture *texture, int face, int level);
void pcd3d_texture_size(const struct pcd3d_texture *texture, unsigned long *width, unsigned long *height);
/* the back buffer's pixels copied into a target texture of its size (the
game samples the screen it is drawing: screen effects) */
void pcd3d_texture_copy_back_buffer(struct pcd3d_texture *texture);

/* ---------- where draws go */

/* a target texture, or NULL for the back buffer; depth: with the depth
buffer (the back buffer's own, for a target no larger). The game's size
of the target: viewports and clears come in its pixels. */
void pcd3d_target(struct pcd3d_texture *texture, int depth, unsigned long logical_width, unsigned long logical_height);
/* in the game's pixels of the target */
void pcd3d_viewport(unsigned long x, unsigned long y, unsigned long width, unsigned long height,
	float minimum_z, float maximum_z);

#define PCD3D_CLEAR_TARGET 1
#define PCD3D_CLEAR_ZBUFFER 2
#define PCD3D_CLEAR_STENCIL 4

/* rectangles: x1, y1, x2, y2 each, in the game's pixels; none: the
viewport */
void pcd3d_clear(unsigned long count, const long *rectangles, unsigned long flags, unsigned long argb, float z,
	unsigned long stencil);

/* ---------- drawing */

struct pcd3d_sampler
{
	/* NULL: no texture */
	struct pcd3d_texture *texture;
	unsigned char address[3];
	unsigned char magnification, minification, mip;
	unsigned char maximum_anisotropy;
	unsigned long border;
	float lod_bias;
	/* the coordinates' components, 2 to 4, and whether the last divides
	the others */
	unsigned char components;
	unsigned char projected;
};

struct pcd3d_unit
{
	struct pcd3d_sampler sampler;
	/* PCD3D_TOP_*; arguments PCD3D_TA_*, [0] being Direct3D's ARG0 (the
	third argument of LERP) */
	unsigned char color_operation, color_arguments[3];
	unsigned char alpha_operation, alpha_arguments[3];
};

struct pcd3d_state
{
	unsigned long unit_count;
	struct pcd3d_unit units[4];
	unsigned long texture_factor;

	unsigned char z_enable, z_write, z_function;
	unsigned char cull, fill;
	unsigned char blend_enable, source_blend, destination_blend, blend_operation;
	unsigned char alpha_test, alpha_function, alpha_reference;
	/* red 1, green 2, blue 4, alpha 8 */
	unsigned char color_write;
	unsigned char stencil_enable, stencil_function, stencil_fail, stencil_z_fail, stencil_pass;
	unsigned long stencil_reference, stencil_mask, stencil_write_mask;
	/* the vertices' fog factors blend towards fog_color */
	unsigned char fog_enable;
	unsigned long fog_color;
	/* the vertices' specular colour is added after the stages */
	unsigned char specular;
	/* D3DRS_ZBIAS, 0 to 16 */
	unsigned char depth_bias;
};

struct pcd3d_vertex
{
	/* clip space: x and y from -w to w, z from 0 to w */
	float position[4];
	/* ARGB */
	unsigned long diffuse, specular;
	/* 1: no fog, 0: the fog colour */
	float fog;
	/* unit n's coordinates */
	float texcoord[4][4];
};

/* draws vertices (with indices into them, or in order without) */
void pcd3d_draw(const struct pcd3d_state *state, int primitive, const struct pcd3d_vertex *vertices,
	unsigned long vertex_count, const unsigned short *indices, unsigned long index_count);

#endif
