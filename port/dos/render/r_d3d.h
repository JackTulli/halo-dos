/*
R_D3D.H

The renderer's Direct3D 8 rasterizer (r_d3d.c): the screen-space triangles,
quads, text and pictures the software rasterizer draws (r_internal.h's
seam) drawn by the graphics card through Direct3D 8, as the OpenGL one
(r_gl.c) does through OpenGL. The renderer is one set of objects for DOS
and Windows 95, so Direct3D is reached only through the device the
Windows 95 build hands it (port/dos/win95/w95_d3d.c, over d3d8.dll loaded
at run time); on DOS it is never active. Nothing here is Direct3D's own:
the table below is plain C, so another Direct3D (9) can fill it too.
*/

#ifndef __R_D3D_H
#define __R_D3D_H

/* a texture on the card */
struct r_d3d_texture;

/* a vertex: clip space (x and y from -w to w, y up; z from 0 to w), the
colour (0xAARRGGBB) the stage multiplies the texel by, the fog factor (1
none, 0 all fog) and the texture coordinates */
struct r_d3d_vertex
{
	float x, y, z, w;
	unsigned long color;
	float fog;
	float u, v;
	/* the lightmap's (r_d3d_state.lightmap) */
	float lightmap_u, lightmap_v;
};

/* what the stage does with the texel (r_d3d_state.texture) */
enum
{
	/* no texture: the colour alone */
	_r_d3d_colour,
	/* the texel times the colour */
	_r_d3d_modulate,
	/* the texel times twice the colour (0x80 the texel as it is) */
	_r_d3d_modulate_twice,
	/* a detail map's: the texel toward grey (0x80) as the colour's alpha
	is below one (texel times alpha, grey times one minus it) */
	_r_d3d_detail,
	/* the Xbox's texture pass: the stages of the draw's r_d3d_detail */
	_r_d3d_xbox_detail
};

/* how the triangle's colour meets the picture's (r_d3d_state.blend) */
enum
{
	_r_d3d_blend_none,
	_r_d3d_blend_alpha,        /* source alpha, one minus it */
	_r_d3d_blend_add,          /* one, one */
	_r_d3d_blend_add_alpha,    /* source alpha, one */
	_r_d3d_blend_premultiplied, /* one, one minus source alpha */
	_r_d3d_blend_detail,       /* destination colour, source colour: the picture times twice the texel */
	_r_d3d_blend_multiply      /* destination colour, zero: the picture times the texel */
};

/* how a batch of triangles is drawn */
struct r_d3d_state
{
	unsigned char texture;
	unsigned char blend;
	/* texels of alpha below half are not drawn */
	unsigned char alpha_test;
	/* (the depth test passes equal depths too, as Halo's: a detail map over
	its surface) */
	unsigned char depth_test, depth_write;
	unsigned char fog;
	/* filtered between texels and levels (trilinear, anisotropic where the
	card can), else nearest; coordinates outside 0..1 take the edge */
	unsigned char smooth, clamp;
	/* the stage's result times a lightmap (a second stage, always filtered),
	white the colour as it is: Halo's level as the Xbox draws it */
	unsigned char lightmap;
	/* drawn only where the stencil is 0, which it then raises (the Xbox's
	texture pass over the level: a pixel's once) */
	unsigned char stencil_once;
	unsigned long fog_color;
};

/* the Xbox's texture pass (_r_d3d_xbox_detail): up to four stages, each a
texture at the vertices' coordinates times its scale plus its offset, its
colour and its alpha each doing one of these (r_public.h's _r_stage_*, in
the same order) with the texel and what is so far. Wrapped, filtered
between texels and levels. */
#define R_D3D_DETAIL_STAGES 4

enum
{
	_r_d3d_stage_current,
	_r_d3d_stage_texture,
	_r_d3d_stage_modulate,
	_r_d3d_stage_modulate_twice,
	_r_d3d_stage_add,
	_r_d3d_stage_add_signed,
	_r_d3d_stage_subtract, /* so far less the texel */
	_r_d3d_stage_subtract_reverse, /* the texel less so far */
	_r_d3d_stage_blend_current_alpha, /* so far where its alpha is 1 */
	_r_d3d_stage_blend_current_alpha_inverse, /* the texel where the alpha so far is 1 */
	_r_d3d_stage_blend_texture_alpha, /* the texel where its alpha is 1 */
	_r_d3d_stage_blend_texture_alpha_inverse, /* so far where the texel's alpha is 1 */
	_r_d3d_stage_square /* so far times itself */
};

struct r_d3d_detail
{
	int stage_count;
	struct r_d3d_texture *textures[R_D3D_DETAIL_STAGES];
	float scales[R_D3D_DETAIL_STAGES][2];
	float offsets[R_D3D_DETAIL_STAGES][2];
	unsigned char operations[R_D3D_DETAIL_STAGES];
	unsigned char alpha_operations[R_D3D_DETAIL_STAGES];
};

/* Halo's water (r_public.h's r_water): a vertex in clip space; its colour
(white, its alpha how much of it the fog leaves); the ripples' coordinates
(t0); four sets of four (t1 to t4) as the water's program reads them; the
base map's coordinates (t5). The programs (w95_d3d.c):
	- 2, pixel shaders 1.4 (after ATI's ocean water program, as an MIT
	  repository of it has it: github.com/mahmoudgalal/Irrlicht_Water_Shader):
	  t1 the way to the eye, t2 to t4 the tangent, binormal and normal, all
	  in the world; the ripple's normal turned into the world by them, the
	  eye reflected in it into the cube map, the tint by how squarely it is
	  seen (the tint texture), the reflection and its eighth power mixed by
	  it, times its alpha and the base map's;
	- 1, pixel shaders 1.1 (Halo's own: texm3x3vspec): t1 to t3 the rows of
	  the matrix from the ripple's space to the world (tangent, binormal and
	  normal's x, y, z) with the way to the eye in w; the tint once a draw. */
struct r_d3d_water_vertex
{
	float x, y, z, w;
	unsigned long color;
	float t0[2];
	float t1[4], t2[4], t3[4], t4[4];
	float t5[2];
};

struct r_d3d_water
{
	int program;
	struct r_d3d_texture *bump, *reflection, *tint, *base;
	/* the ripples' level of detail bias (Halo's ripple mipmap bias) */
	float bump_lod_bias;
	/* program 1's tint (0x00RRGGBB) */
	unsigned long tint_color;
	/* the base map's alpha is the reflection's share (program 2), and its
	colour multiplies what is under the water first */
	int base_alpha, base_multiply;
};

/* what a texture's alpha holds: none, a cut-out's (0 or 255), anything */
enum
{
	_r_d3d_alpha_none,
	_r_d3d_alpha_test,
	_r_d3d_alpha_full
};

struct r_d3d_device
{
	/* the card: its largest texture side, the widest shape it takes (a side
	at most this many times the other; 0 any), whether its sides must be
	powers of two, or equal, and whether it does what the detail maps and the
	lightmaps (two textures at once) need */
	int maximum_texture_size;
	int maximum_aspect;
	int power_of_two, square;
	int draws_detail;
	int draws_lightmaps;
	/* the texture stages the Xbox's texture pass can take (under 3: it is
	not drawn) */
	int detail_stages;
	/* the back buffer's size */
	int width, height;

	/* a texture of levels levels, each half the one before (to 1x1);
	written a level at a time from 0xAARRGGBB texels */
	struct r_d3d_texture *(*texture_new)(int width, int height, int levels, int alpha);
	void (*texture_write)(struct r_d3d_texture *texture, int level, const unsigned long *argb);
	void (*texture_delete)(struct r_d3d_texture *texture);
	/* where the next triangles' clip space goes in the back buffer, and the
	depth range they take */
	void (*viewport)(int x, int y, int width, int height, float minimum_z, float maximum_z);
	/* a rectangle of the back buffer cleared to a colour, its depth to the
	farthest, or both */
	void (*clear)(int x0, int y0, int x1, int y1, int color, int depth, unsigned long argb);
	/* triangles, three indices each into the vertices (lightmap: the second
	stage's texture, with state->lightmap; detail: the stages of the Xbox's
	texture pass, with _r_d3d_xbox_detail) */
	void (*draw)(const struct r_d3d_state *state, struct r_d3d_texture *texture, struct r_d3d_texture *lightmap,
		const struct r_d3d_detail *detail, const struct r_d3d_vertex *vertices, unsigned long vertex_count,
		const unsigned short *indices, unsigned long index_count);
	/* a rectangle of the back buffer as 0x00RRGGBB words (screenshots);
	FALSE if it cannot be read */
	int (*read_back)(int x, int y, int width, int height, unsigned long *rgb);
	/* Halo's water: the program the card runs for it (0 none: drawn as an
	ordinary surface), cube maps (deleted as textures), and its draw */
	int water;
	struct r_d3d_texture *(*cube_new)(int size, int levels);
	void (*cube_write)(struct r_d3d_texture *cube, int face, int level, const unsigned long *argb);
	void (*draw_water)(const struct r_d3d_water *water, const struct r_d3d_water_vertex *vertices,
		unsigned long vertex_count, const unsigned short *indices, unsigned long index_count);
};

/* the card draws from now (the picture the r_frame's size, shown in the
back buffer's rectangle r_d3d_viewport gives): FALSE if the device lacks
something */
int r_d3d_use(const struct r_d3d_device *device);
/* where the picture goes in the back buffer from the next frame (from its
top left), and the back buffer's size */
void r_d3d_viewport(int x, int y, int width, int height, int window_width, int window_height);
/* the triangles gathered so far drawn (before the frame is shown) */
void r_d3d_flush(void);

#endif /* __R_D3D_H */
