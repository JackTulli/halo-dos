/*
R_PUBLIC.H

The renderer of the DOS and Windows 95 builds, as the game-side glue sees
it (port/dos/game/dos_render_game.c and dos_hud_game.c). Plain C: it knows
nothing of the game's tags; the glue turns the game's level, models and
bitmaps into textures and triangles of this interface.

How it is made (port/dos/README.md, "The renderer"): this frontend culls,
transforms, lights, fogs and clips; the triangles that survive go to a
rasterizer (r_raster.h) in screen space, each vertex with its 1/z, its
texture coordinates over z and its colour. The software rasterizer
(r_soft.c) is the one there is now; a Voodoo's Glide and OpenGL take the
same triangles, as Quake's renderers took the same scene.

Two kinds of picture: 8-bit (palette indices; the lighting through a
colour map of 32 levels, monochrome) and 32-bit (0x00RRGGBB; light in
colour). Textures are 8-bit indices into a palette of their own in both,
so a texture costs a quarter of a true-colour one.
*/

#ifndef __R_PUBLIC_H
#define __R_PUBLIC_H

/* ---------- start-up */

struct r_settings
{
	/* the picture's size and kind (8 or 32 bits a pixel) */
	int width, height, bits;
	/* the largest side a texture keeps (16 to 256) */
	int texture_size;
	/* 0 the least drawn, 3 the most */
	int detail;
	/* the texture's error in pixels a triangle may have and still be
	drawn without perspective (0: every one with it; below 0: the
	detail's) */
	float affine_tolerance;
	/* magnified textures' texels dithered into each other (the 32-bit
	picture: r_soft.c) rather than drawn as squares */
	int texture_dither;
	/* magnified textures filtered (bilinear: four texels a pixel, the
	32-bit picture's, over the dither), for a fast machine */
	int texture_smooth;
	/* the 8-bit picture's light a pixel at a time, dithered between its
	levels, across large triangles whose light changes (else a level each
	16 pixels) */
	int light_dither;
};

/* FALSE if there is not the memory for the picture */
int r_initialize(const struct r_settings *settings);
/* whether a hardware rasterizer draws (OpenGL on Windows 95, a Voodoo on
DOS: r_gl.c, r_voodoo.c), and the frame it has drawn so far read back into
the picture's pixels (a screenshot's, before it is shown) */
int r_hardware_active(void);
void r_hardware_read_back(void);
/* whether R_DRAW_DETAIL draws (OpenGL with the texture environment's
combine extension) */
int r_draws_detail(void);
/* whether the card multiplies the texture by a lightmap of its own (the
Xbox's look: Direct3D 8, r_d3d.c): r_lightmap_new and r_set_lightmap draw,
else they do nothing */
int r_draws_lightmaps(void);
/* the settings that change as the game runs (the detail, the dither, the
affine tolerance): the picture's size and kind stay */
void r_settings_changed(const struct r_settings *settings);
void r_dispose(void);
/* the picture to show (system_display_present's) */
const void *r_frame_pixels(int *width, int *height, int *pitch, int *bits);
/* the 8-bit picture's colour of an index, 0x00RRGGBB */
unsigned long r_palette_color(unsigned char index);
/* the 8-bit picture's colours: 256 RGB triples */
const unsigned char *r_palette(void);
/* a map's own 8-bit palette (256 RGB triples), set while its textures wait
to be baked into the picture's colours (r_textures_defer_baking) */
void r_palette_set(const unsigned char *rgb);
/* the palette of a weighted histogram of 15-bit colours (RGB 5:5:5): at
most wanted colours (0x00RRGGBB); how many */
int r_palette_median_cut(const unsigned long *histogram, int wanted, unsigned long *colors);

/* ---------- textures */

typedef struct r_texture r_texture;

#define R_TEXTURE_ALPHA_TEST 0x01 /* texels of alpha below half are not drawn */
#define R_TEXTURE_CLAMP 0x02 /* coordinates outside 0..1 take the edge */
#define R_TEXTURE_ALPHA 0x04 /* its alpha blends (the screen's quads): kept in the palette */
#define R_TEXTURE_LARGE 0x08 /* up to 512 texels a side, over the settings' texture size (terrain tiles) */
/* on a card, its largest level alone (a lookup table, the planar fog's
density: coordinates far past its edge take the edge, not an average of
it all from a level the card picks for them) */
#define R_TEXTURE_ONE_LEVEL 0x10

/* a texture from 32-bit texels (0xAARRGGBB, rows top down), the largest
level: the renderer shrinks it to its texture size, makes its smaller
levels and its palette. NULL if there is no memory */
r_texture *r_texture_new(const unsigned long *argb, int width, int height, int flags);
void r_texture_delete(r_texture *texture);
/* a palette several opaque textures share (a map's terrain tiles): a
histogram of 15-bit colours (32768 counts) of them all, then the palette
of it, then each texture made with it -- no palette of its own to work out */
typedef struct r_shared_palette r_shared_palette;
unsigned long *r_histogram_new(void);
void r_histogram_add(unsigned long *histogram, const unsigned long *argb, long count);
r_shared_palette *r_shared_palette_new(const unsigned long *histogram);
void r_shared_palette_delete(r_shared_palette *shared);
r_texture *r_texture_new_shared(const unsigned long *argb, int width, int height, int flags,
	r_shared_palette *shared);
/* its average colour, 0xAARRGGBB (for drawing far things untextured) */
unsigned long r_texture_average(const r_texture *texture);
/* the bytes all textures take */
unsigned long r_texture_memory(void);
/* while a map loads in 8 bits: its textures keep their own colours until
its palette is set (r_palette_set), then are baked into it */
void r_textures_defer_baking(int defer);
/* colours a texture shows (its smallest level of 8 or more a side, as its
own palette has them, 0x00RRGGBB): how many, at most maximum */
int r_texture_colors(const r_texture *texture, unsigned long *colors, int maximum);

/* ---------- frames and views */

/* the start of a frame: the picture cleared to a colour (0x00RRGGBB) */
void r_frame_begin(unsigned long clear_color);
void r_frame_end(void);

struct r_view
{
	/* the eye, where it looks and which way is up, in world units */
	float position[3], forward[3], up[3];
	/* radians from the bottom of the view to its top */
	float vertical_field_of_view;
	/* a pixel's width over its height as the picture is seen (0: square) */
	float pixel_aspect;
	float z_near, z_far;
	/* the view's pixels in the picture: [x0, x1) x [y0, y1) */
	int x0, y0, x1, y1;
	/* fog: none with fog_end 0; else from fog_start to fog_end it goes to
	fog_color (0x00RRGGBB), solid past fog_end (where nothing is drawn) */
	float fog_start, fog_end;
	unsigned long fog_color;
	/* or, fog_maximum between 0 and 1 (Halo's atmospheric fog on a card's
	Xbox look: its maximum density), the fog goes only that far by fog_end,
	stays so to fog_far, and from there reaches all of it at z_far (where
	nothing is drawn); other values: as above */
	float fog_maximum, fog_far;
	/* the sky behind the view: its colour (0x00RRGGBB) at each of
	R_SKY_STOPS heights above the horizon (0, 15, 30, 50 and 90 degrees),
	or NULL for none (the frame's clear colour there) */
	const unsigned long *sky_colors;
};

#define R_SKY_STOPS 5

void r_view_begin(const struct r_view *view);
void r_view_end(void);
/* the view's axes (right, up, ahead) and its eye, in the world: a point p
is (p - eye) . right, . up, . ahead in its space */
void r_view_basis(float right[3], float up[3], float ahead[3], float eye[3]);

/* whether a sphere is in the view (inside the frustum and nearer than the
fog's end) */
int r_sphere_visible(const float center[3], float radius);
/* its distance from the eye, and how many pixels a unit is there across the
view: for choosing a model's level of detail */
float r_distance(const float point[3]);
float r_pixels_per_unit_at(float distance);

/* ---------- triangles */

struct r_vertex
{
	float position[3];
	float u, v;
	/* the light at the vertex, 0x00RRGGBB, 0x80 being as the texture is (a
	texel lit at 0xFF is twice as bright) */
	unsigned long color;
};

/* how triangles are drawn */
#define R_DRAW_TWO_SIDED 0x01 /* back faces too */
#define R_DRAW_ADDITIVE 0x02 /* added to the picture (glows), no depth written */
#define R_DRAW_BLEND 0x04 /* half over the picture (glass, water), no depth written */
#define R_DRAW_NO_DEPTH_TEST 0x08
#define R_DRAW_NO_FOG 0x10
/* with R_DRAW_BLEND: over the picture as much as each texel's alpha says,
not half (Halo's alpha-blended shaders: cloud layers, nebulae, smoke; the
texture made with R_TEXTURE_ALPHA). The 8-bit picture: none under a
quarter, half to three quarters, whole above */
#define R_DRAW_ALPHA 0x40
/* a detail map over the same triangles drawn just before (a hardware
rasterizer's, r_draws_detail; the software one draws nothing): what is
there times twice the texel, the texel moved toward 0x80 (no change) as
the vertices' grey is below 0xFF, their strength there */
#define R_DRAW_DETAIL 0x80
/* the vertices are in the view's own space already (x right, y up, z
ahead of the eye: r_view_basis), not the world's */
#define R_DRAW_VIEW_SPACE 0x20
/* with R_DRAW_ADDITIVE: its texels premultiplied, the picture kept by one
minus their alpha (Halo's meter shaders: the plasma weapons' gauges); the
software and Voodoo rasterizers add them (the picture kept whole) */
#define R_DRAW_PREMULTIPLIED 0x10000

/* triangles of vertices (three indices each), transformed by matrix (4x3,
column vectors forward, left, up and the position, as Halo's
real_matrix4x3 after its scale) or in the world with matrix NULL. Halo's
front faces wind clockwise as the eye sees them. texture NULL is the
vertices' colour alone */
void r_draw_triangles(const struct r_vertex *vertices, int vertex_count, const unsigned short *indices,
	int triangle_count, const float *matrix, r_texture *texture, unsigned flags);

/* A lightmap page on the card (r_draws_lightmaps): 0xAARRGGBB texels, white
the texture as it is; its handle, 0 if it could not be made. The next
r_draw_triangles multiplies its texture by the lightmap at the vertices'
lightmap coordinates (two floats a vertex, beside the vertices): set before
it, forgotten after. */
unsigned long r_lightmap_new(const unsigned long *argb, int width, int height);
void r_lightmap_delete(unsigned long lightmap);
void r_set_lightmap(unsigned long lightmap, const float *coordinates);

/* The Xbox's texture pass (Direct3D 8 on a card of three texture stages or
more: r_draws_xbox_detail says how many): an environment's detail maps
over the triangles drawn just before, in one pass as Halo's own (its
rasterizer_xbox_environment.c): the primary and the secondary detail map
mixed at each pixel by an alpha (a blended shader's base map's, a normal
one's secondary's), times the micro detail map, and the picture times twice
that (or once: Halo's plain multiply). Its textures are the card's own
(r_hardware_texture_new), each level as the map stores it: Halo's detail
maps fade toward grey down their levels, so the grain goes in the distance
as it did. Set before the next r_draw_triangles (its texture the base map,
whose coordinates each stage scales), forgotten after. */
#define R_DETAIL_STAGES 4

/* what a stage makes of its texel and what is so far (its colour, and
separately its alpha); the first stage's "so far" is nothing */
enum
{
	_r_stage_current, /* what is so far */
	_r_stage_texture, /* the texel */
	_r_stage_modulate, /* the texel times what is so far */
	_r_stage_modulate_twice, /* twice that */
	_r_stage_add,
	_r_stage_add_signed, /* added, less a half */
	_r_stage_subtract, /* what is so far less the texel */
	_r_stage_subtract_reverse, /* the texel less what is so far */
	_r_stage_blend_current_alpha, /* what is so far where its alpha is 1, the texel where 0 */
	_r_stage_blend_current_alpha_inverse, /* the texel where the alpha so far is 1 */
	_r_stage_blend_texture_alpha, /* the texel where its own alpha is 1 */
	_r_stage_blend_texture_alpha_inverse, /* what is so far where the texel's alpha is 1 */
	_r_stage_square /* what is so far times itself (the stage's texel not read) */
};

struct r_detail_pass
{
	int stage_count;
	unsigned long textures[R_DETAIL_STAGES];
	/* the base map's coordinates times the scale, plus the offset, a
	stage's */
	float scales[R_DETAIL_STAGES][2];
	float offsets[R_DETAIL_STAGES][2];
	unsigned char operations[R_DETAIL_STAGES];
	unsigned char alpha_operations[R_DETAIL_STAGES];
	/* the picture times what the stages give, not twice that */
	int multiply;
	/* the stages drawn instead of the draw's texture, blended as its flags
	say (a chicago shader's stack of maps), not over what is there */
	int replace;
};

int r_draws_xbox_detail(void);
/* a texture on the card from its levels (0xAARRGGBB, count of them, each
half the one before; the card makes any smaller ones), alpha kept or not;
its handle, 0 if it could not be made */
unsigned long r_hardware_texture_new(unsigned long *const *levels, const int *widths, const int *heights, int count,
	int alpha);
void r_hardware_texture_delete(unsigned long texture);
void r_set_detail_pass(const struct r_detail_pass *pass);
/* a level of a card texture written again (0xAARRGGBB: an animated one,
the water's ripples) */
void r_hardware_texture_write(unsigned long texture, int level, const unsigned long *argb);
/* a cube map on the card: six faces (+x -x +y -y +z -z, Direct3D's
order and coordinates, the world's axes), each size texels a side, levels
of them each (0xAARRGGBB, each level half the one before); 0 if it could
not be made. Deleted as a texture. */
unsigned long r_hardware_cube_new(unsigned long *const *faces[6], int size, int levels);

/* ---------- Halo's water on the card (r_draws_water)

Its reflection, as Halo's water shader (shader_transparent_water) makes
it: a cube map seen in the surface, the surface's normal at each pixel
from its ripples (a normal map, made again each frame from the shader's
ripple layers), the reflection times a tint that goes from the shader's
parallel tint (looking along the water) to its perpendicular one (looking
straight down), mixed with the reflection to its eighth power (glints);
added to what is under it, faded by the fog. Before it, where the shader
says, the base map multiplies what is under the water. */
struct r_water
{
	/* the ripples' normal map (r_hardware_texture_new, rewritten each frame)
	and its coordinates: the vertices' times the scale plus the offset */
	unsigned long bump;
	float bump_scale[2], bump_offset[2];
	/* its levels of detail moved by this (Halo's ripple mipmap bias) */
	float bump_lod_bias;
	/* the cube map reflected (r_hardware_cube_new) */
	unsigned long reflection;
	/* the tint by how squarely the surface is seen: 256 texels a row, from
	along the water (0) to straight down (255); alpha the share of the
	reflection added */
	unsigned long tint;
	/* the same for this draw as a whole (a card that works out the tint
	once a draw: 0x00RRGGBB) */
	unsigned long tint_color;
	/* the base map (0: none), whose alpha the reflection is times (alpha)
	and whose colour multiplies what is under the water first (multiply) */
	unsigned long base;
	int base_alpha, base_multiply;
	/* with base_alpha, the reflection's share by how squarely the surface
	is seen: from share_parallel along it to share_perpendicular straight
	down, half of each along it (Halo's; the tint's row holds it for a card
	that reads it there); and the base map's alpha on average, for a card
	that cannot read the map there (pixel shaders 1.1) */
	float share_parallel, share_perpendicular, base_alpha_mean;
};

/* 0: this renderer draws Halo's water as an ordinary transparent surface;
1: as above, the tint once a draw (pixel shaders 1.1); 2: as above, the
tint at each pixel by its ripple (pixel shaders 1.4) */
int r_draws_water(void);
/* water's triangles: world vertices (their u, v the base map's), and each
vertex's normal, binormal and tangent (nine floats a vertex, the world's) */
void r_draw_water(const struct r_water *water, const struct r_vertex *vertices, const float *frames, int vertex_count,
	const unsigned short *indices, int triangle_count);

/* ---------- the screen (the HUD, text, menus): in the picture's pixels */

struct r_screen_vertex
{
	float x, y;
	float u, v;
	/* 0xAARRGGBB: the texel times this (alpha: how much of it covers) */
	unsigned long color;
};

/* how a quad covers the picture */
enum r_screen_blend
{
	_r_screen_opaque,
	/* alpha over: texel alpha times the colour's */
	_r_screen_alpha,
	/* added (Halo's HUD, which is premultiplied: colour + picture * (1 - alpha)) */
	_r_screen_add,
	_r_screen_premultiplied,
};

/* a quad, its corners in order round it; texture NULL is the colour */
void r_draw_screen_quad(const struct r_screen_vertex corners[4], r_texture *texture, enum r_screen_blend blend);
/* what is drawn from now is in front of everything drawn without (its
depth raised above the world's, still tested among itself): the
first-person weapon, drawn first, so the world's pixels behind it stop at
the depth test */
void r_set_in_front(int in_front);
/* R_DRAW_ALPHA's texels cover this much of what they did (0 to 1) for what
is drawn from now: a fading particle's; 1 again after */
void r_set_alpha_scale(float scale);
/* the view's near plane moved, and the depth's scale with it, for what is
drawn from now: the first-person weapon's (Halo draws it with a near clip of
its own, about a fifth of the world's), then the world's again */
void r_view_set_near(float z_near);
/* a square facing the eye in one colour (0x00RRGGBB), radius world units
from its middle to each side: a distant object's dot */
void r_draw_billboard(const float center[3], float radius, unsigned long color);
/* a rectangle of one colour, [x0, x1) x [y0, y1), alpha over */
void r_fill_rectangle(int x0, int y0, int x1, int y1, unsigned long argb);
/* an 8-bit coverage image (a glyph: 0 none, 255 all) of width x height
(rows stride bytes apart) at (x, y), each of its pixels scale_x by scale_y
of the picture's, in a colour alpha over, clipped to [x0, x1) x [y0, y1):
filtered to that size once and kept (r_coverage_cache_flush forgets them:
when the coverage's memory goes) */
void r_draw_coverage(float x, float y, const unsigned char *coverage, int width, int height, int stride,
	float scale_x, float scale_y, unsigned long argb, int x0, int y0, int x1, int y1);
void r_coverage_cache_flush(void);
/* argb texels already at the picture's size (width x height, alpha over)
with their top left at (x, y) */
void r_draw_picture(int x, int y, const unsigned long *argb, int width, int height);
/* the depth buffer cleared in a rectangle (what is drawn next is in front) */
void r_clear_depth(int x0, int y0, int x1, int y1);

/* ---------- measuring */

struct r_statistics
{
	long triangles_drawn, triangles_culled, pixels_drawn;
	long views;
	/* (the rasterizer's: rows it stepped through, triangles off the numbers) */
	long rows_scanned, triangles_not_finite, triangles_affine;
};
const struct r_statistics *r_frame_statistics(void);

#endif /* __R_PUBLIC_H */
