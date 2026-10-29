/*
R_INTERNAL.H

What the renderer's units share: the picture, the textures' layout, and
the rasterizer's interface (the seam a hardware rasterizer would take: a
Voodoo's Glide or OpenGL get the same screen-space triangles).
*/

#ifndef __R_INTERNAL_H
#define __R_INTERNAL_H

#include "r_public.h"

/* ---------- the picture */

struct r_frame
{
	int width, height, bits;
	/* the colour buffer: width x height bytes (8 bits) or words (32 bits) */
	unsigned char *pixels;
	int pitch;
	/* the depth buffer: 1/z in 2.30 fixed point times z_near (larger is
	nearer; 0 is the clear value, nothing) */
	unsigned long *depth;
	struct r_settings settings;
};

extern struct r_frame r_frame;
extern struct r_statistics r_statistics;

/* a float to the nearest integer as the x87 stores it (fistp, rounding as
the control word says: to nearest, as the runtime leaves it). A C cast
truncates, which costs two control word loads around every conversion;
ceil is a call. Out of range gives 0x80000000, as a cast's does. */
static __inline__ long r_round(float value)
{
	long result;

	__asm__("fistpl %0" : "=m"(result) : "t"(value) : "st");
	return result;
}

/* ---------- textures

8-bit texels, indices into the texture's palette (index 0 is transparent
in an alpha-tested texture). Each level is a power of two on each side,
up to the settings' texture size; up to R_MAXIMUM_LEVELS levels, each half
the one before. */

#define R_MAXIMUM_LEVELS 6

struct r_texture
{
	int flags;
	int levels;
	/* log2 of the largest level's sides */
	int width_shift, height_shift;
	unsigned char *texels[R_MAXIMUM_LEVELS];
	/* 0x00RRGGBB (the alpha test is index 0's) */
	unsigned long palette[256];
	/* each palette entry as the 8-bit picture's nearest colour */
	unsigned char remap[256];
	unsigned long average;
	unsigned long bytes;
	/* the 8-bit picture's opaque textures: texels the picture's palette
	indices (baked), or still the texture's own, to be baked when the map's
	palette is set */
	int baked;
	/* its texture in the hardware rasterizer (r_gl.c's name, r_voodoo.c's
	slot), 0 until drawn there */
	unsigned long hardware;
	/* its largest level in full colour (0xAARRGGBB), for a hardware
	rasterizer that draws it so (r_backend's texture_created), until that
	takes it; NULL otherwise */
	unsigned long *argb;
	int argb_width, argb_height;
	struct r_texture *next;
};

/* the 8-bit picture's palette and its colour maps (r_palette.c) */
extern unsigned char r_palette_rgb[256 * 3];
/* r_color_map[light][index]: index lit at light / 16 (0 to 31: 16 is the
texel as it is, 31 nearly twice as bright) */
#define R_LIGHT_LEVELS 32
extern unsigned char r_color_map[R_LIGHT_LEVELS + 1][256];
/* r_fog_map[fog][index]: index faded toward the view's fog colour by fog /
16 (0 to 15); made again when the fog colour changes */
#define R_FOG_LEVELS 16
extern unsigned char r_fog_map[R_FOG_LEVELS][256];
/* the nearest palette colour of a 15-bit colour (5 bits red, green, blue) */
extern unsigned char r_inverse_palette[32768];
void r_palette_initialize(void);
/* the textures made with the palette before: their remaps again, and the
8-bit picture's opaque ones baked into it (r_texture.c) */
void r_textures_palette_changed(void);
void r_palette_set_fog(unsigned long fog_color);
unsigned char r_palette_nearest(unsigned long rgb);
/* additive and half blends of two 8-bit picture colours: built on first
use (64 KB each) */
const unsigned char *r_palette_add_table(void);
const unsigned char *r_palette_blend_table(void);

/* ---------- the rasterizer */

/* a vertex in the picture: pixel coordinates (centres at .5), its depth as
w = 1/z, texture coordinates over z, and its colour */
struct r_raster_vertex
{
	float x, y;
	float w;
	float s, t;
	/* the texture coordinates themselves (s and t are them times w), for
	triangles drawn without perspective (r_soft.c's affine ones) */
	float u, v;
	/* the texel's multiplier (0x80 as it is, per channel 0-255) and, with
	fog, what is added after (the fog's colour times its share), each 0-255 */
	float r, g, b;
	float fog_r, fog_g, fog_b;
	/* the 8-bit picture's light level (0-31) and fog level (0-15) */
	float light, fog;
	/* the lightmap's coordinates over z (R_RASTER_LIGHTMAP's) */
	float lightmap_s, lightmap_t;
};

/* a vertex of Halo's water for a card (r_backend's water): the picture's x
and y times the view's depth (centre x z + x focal: no division, so that
the card clips them), the depth; the base map's coordinates; how much of it
the fog leaves (1 all, to 0); and in the world, the way to the eye (unit)
and the surface's normal, binormal and tangent */
struct r_water_vertex
{
	float xz, yz, z;
	float u, v;
	float fade;
	float eye[3];
	float frame[9];
};

#define R_RASTER_FOG 0x100 /* the fog terms are not all zero */
#define R_RASTER_AFFINE 0x200 /* no perspective (the screen's quads) */
#define R_RASTER_SCREEN 0x400 /* a screen quad: blend as r_screen_blend says */
/* the texture times r_lightmap's at the vertices' lightmap coordinates (a
card's that draws lightmaps: r_backend's lightmap_new) */
#define R_RASTER_LIGHTMAP 0x800
extern unsigned long r_lightmap;
/* the Xbox's texture pass over what is there (r_set_detail_pass) */
#define R_RASTER_XBOX_DETAIL 0x1000
extern const struct r_detail_pass *r_detail_pass;

/* draws a triangle (any winding) clipped to [x0, x1) x [y0, y1) */
void r_raster_triangle(const struct r_raster_vertex *a, const struct r_raster_vertex *b,
	const struct r_raster_vertex *c, const struct r_texture *texture, int level, unsigned flags, int x0, int y0,
	int x1, int y1);
/* the blend of the next screen triangles (R_RASTER_SCREEN) */
void r_raster_screen_blend(enum r_screen_blend blend);
void r_raster_clear(unsigned long color);
/* r_span8.s: the 8-bit picture's opaque pixels, each the texel at u and v
(16.16, v times the texture's width), stepped by r_span8's du and dv, depth
tested and written (z, stepped by dz; the depth buffer's word for a pixel at
depth_offset + 4 * its address, depth_offset the row's); the texel itself,
or shade[texel], or fog[shade[texel]], or a cut-out's. Two ways:
- a triangle's rows without perspective, all of them in one call: r_span8's
rows rows from pixel_row, each between its edges x_long and x_short (16.16,
a pixel in where its centre is, stepped a row), inside x0 and x1, its u, v,
z, light and fog at its first pixel the rows' (at x = 0) plus x times their
steps, the light a row of r_color_map (or constant_shade), the fog one of
r_fog_map;
- a triangle's rows with perspective, all of them in one call: the rows'
edges as above; each row's start values from the planes (p_*: as
r_soft.c's raster8_opaque works them out, the same operations on the x87 in
the same order and precision, so the same pixels), its segments of up to 16
pixels between divisions, each from where the last ended (segment_*).
r_span8 holds what they share (the assembly reads it by offset: keep the
order) */
struct r_span8_constants
{
	const unsigned char *texels;
	const unsigned char *shade;
	const unsigned char *fog;
	unsigned long du, dv, dz;
	/* the texture's u mask shifted up 16, and its texel indices' mask (u's
	and v's, v's times the width) */
	unsigned long umask16, index_mask;
	/* a cut-out texture's own palette to the picture's (its texel 0 a hole) */
	const unsigned char *remap;
	unsigned long depth_offset;
	/* the rows' */
	unsigned char *pixel_row;
	long rows;
	long x_long, long_step, x_short, short_step;
	long x0, x1;
	unsigned long u_row, u_dy, v_row, v_dy, z_row, z_dy;
	long light_row, light_dy, dlight;
	long fog_row, fog_dy, dfog;
	long pitch;
	const unsigned char *constant_shade;
	/* (the assembly's own: a run's end) */
	unsigned char *end;
	/* the segments' */
	unsigned char *segment_p;
	unsigned long segment_u, segment_v, segment_z;
	long segment_light, segment_fog;
	/* dlight and dfog times a segment's 16 */
	long dlight16, dfog16;
	long width_shift;
	/* a perspective triangle's planes (r_span8_perspective_*): at x = 0 on
	the row's centre and stepped a row (floats, as r_soft.c keeps them), and
	across; the constants; the rows' floats in between; a segment's */
	float p_w_row, p_s_row, p_t_row, p_l_row, p_f_row;
	float p_s_dy, p_t_dy, p_l_dy, p_f_dy;
	float p_s_dx, p_t_dx, p_l_dx, p_f_dx;
	/* 65536 times the depth's scale (texels times z over w), 0.5, 65536 */
	float p_k, p_half, p_fixed;
	unsigned long p_depth_bias;
	float p_s, p_t;
	long p_left, p_count, p_u_end, p_v_end, p_tmp;
	/* w's steps across and down, as the x87 holds them (more than a float's
	precision: r_soft.c's never pass through a float) */
	double p_w_dx, p_w_dy;
	/* depth_offset's step a row (the depth buffer's row is the width in
	words, the picture's its pitch in bytes, rounded up to whole words) */
	unsigned long depth_step;
};

extern struct r_span8_constants r_span8;
void r_span8_perspective_unlit(void);
void r_span8_perspective_shaded(void);
void r_span8_perspective_fogged(void);
void r_span8_perspective_cut_shaded(void);
void r_span8_perspective_cut_fogged(void);
/* r_span8.s: r_main.c's vertex loop for a textured draw in the 8-bit
picture (r_draw_triangles; neither added light nor lightmaps): each vertex
of vertices into view space (or as it is), its light and fog levels, its
outcode, projected where in front of the near plane; the same operations
in the same order and precision as the C loop (r_vertices8 holds what it
reads: keep the order) */
struct r_vertices8_constants
{
	float turn[3][3], offset[3];
	float fog_start, fog_scale;
	float z_near, far, tangent_x, tangent_y;
	float focal_x, focal, centre_x, centre_y;
	const struct r_vertex *vertices;
	/* r_main.c's clip vertices, its projected ones, the outcodes */
	void *transformed, *projected;
	unsigned char *codes;
	long count, view_space, fogged;
	/* the last vertex's colour and its light level (light_vertex's too) */
	unsigned long last_color;
	float last_light;
	/* the constants: the light's weights, 16 / 128, the fog's levels - 1, 1 */
	float red, green, blue, eighth, fifteen, one;
	long scratch;
};

extern struct r_vertices8_constants r_vertices8;
void r_vertices8_draw(void);
void r_span8_rows_unlit(void);
void r_span8_rows_shaded(void);
void r_span8_rows_fogged(void);
void r_span8_rows_cut_shaded(void);
void r_span8_rows_cut_fogged(void);

/* a screen quad that is a rectangle on the screen and in its texture, of
one colour (0xAARRGGBB): a row at a time (the blend r_raster_screen_blend's) */
void r_raster_screen_rect(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
	const struct r_texture *texture, unsigned long color);

/* added to the depth of what is drawn from now (r_set_in_front) */
void r_raster_set_depth_bias(unsigned long bias);
/* R_DRAW_ALPHA's alpha times scale over 256 (r_set_alpha_scale) */
void r_raster_set_alpha_scale(unsigned long scale);
/* a row of the picture in one colour, its depth untouched */
void r_raster_fill_row(int x0, int x1, int y, unsigned long color);

/* ---------- hardware rasterizers

OpenGL on Windows 95 (r_gl.c) and a 3dfx Voodoo on DOS (r_voodoo.c) take
what the software rasterizer draws when one is in use: r_soft.c and
r_main.c hand over through this table */

struct r_backend
{
	void (*triangle)(const struct r_raster_vertex *a, const struct r_raster_vertex *b,
		const struct r_raster_vertex *c, const struct r_texture *texture, unsigned flags);
	void (*screen_blend)(int blend);
	void (*clear)(unsigned long color);
	void (*fill_row)(int x0, int x1, int y, unsigned long color);
	void (*screen_rect)(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
		const struct r_texture *texture, unsigned long color);
	void (*set_near)(float z_near);
	void (*set_in_front)(int in_front);
	void (*set_alpha_scale)(unsigned long scale);
	void (*view_fog)(int fogged, float fog_start, float fog_end, unsigned long fog_color);
	void (*clear_depth)(int x0, int y0, int x1, int y1);
	/* a glyph's coverage (width x height bytes) at (x, y) in a colour; the
	backend's handle for it kept in *handle (0 the first time) */
	void (*coverage)(int x, int y, const unsigned char *cover, int width, int height, unsigned long argb,
		unsigned long *handle, int clip_x0, int clip_y0, int clip_x1, int clip_y1);
	void (*coverage_forget)(unsigned long handle);
	void (*picture)(int x, int y, const unsigned long *argb, int width, int height);
	void (*fill_rectangle)(int x0, int y0, int x1, int y1, unsigned long argb);
	void (*texture_delete)(struct r_texture *texture);
	/* the frame drawn so far into r_frame's pixels (a screenshot's) */
	void (*read_back)(void);
	/* a texture just made, its full colours in argb, to the card now
	(NULL: the backend takes textures at their first drawing, from their
	8-bit levels) */
	void (*texture_created)(struct r_texture *texture);
	/* whether it draws R_DRAW_DETAIL (NULL: it does not) */
	int (*draws_detail)(void);
	/* a view's rectangle of the picture, which its triangles stay inside
	(the software rasterizer clips each to it; a card draws them whole, and
	split screen's views would spill into each other), or none (active 0:
	the whole picture again) */
	void (*view_rect)(int x0, int y0, int x1, int y1, int active);
	/* lightmaps (NULL: it draws none; Direct3D 8's alone, r_d3d.c): a page
	from 0xAARRGGBB texels, its handle (0: none), and its end */
	unsigned long (*lightmap_new)(const unsigned long *argb, int width, int height);
	void (*lightmap_delete)(unsigned long lightmap);
	/* the Xbox's texture pass (NULL: it draws none; Direct3D 8's): the
	texture stages it has for it (under 3: none), and its textures */
	int (*detail_stages)(void);
	unsigned long (*hardware_texture_new)(unsigned long *const *levels, const int *widths, const int *heights,
		int count, int alpha);
	void (*hardware_texture_delete)(unsigned long texture);
	/* a card texture's level written again, and cube maps (NULL: none) */
	void (*hardware_texture_write)(unsigned long texture, int level, const unsigned long *argb);
	unsigned long (*hardware_cube_new)(unsigned long *const *faces[6], int size, int levels);
	/* Halo's water (NULL: none; r_draws_water's 1 or 2), its vertices
	unclipped (the card clips them) */
	int (*draws_water)(void);
	void (*water)(const struct r_water *water, const struct r_water_vertex *vertices, int vertex_count,
		const unsigned short *indices, int triangle_count);
	/* the view's fog held at a maximum from its end, then all of it at the
	far clip (r_view's fog_maximum and fog_far; maximum 1: none of that),
	told after view_fog (NULL: the backend's fog is all at its end) */
	void (*view_fog_curve)(float maximum, float far, float z_far);
};

/* the one in use, NULL for the software rasterizer */
extern const struct r_backend *r_backend;

#endif /* __R_INTERNAL_H */
