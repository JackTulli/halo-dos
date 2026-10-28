/*
DOS_RENDER.H

The game-side units of the DOS and Windows 95 builds' drawing
(port/dos/game/dos_*.c): what they share. They read the game's tags and
hand the renderer (port/dos/render/r_public.h) textures and triangles.
*/

#ifndef __DOS_RENDER_H
#define __DOS_RENDER_H

#include "r_public.h"

/* ---------- textures and materials (dos_textures.c) */

/* how a material draws */
struct dos_material
{
	r_texture *texture;
	/* R_DRAW_* */
	unsigned flags;
	/* the light is multiplied by this (0x80 per channel: as it is) */
	unsigned long tint;
	/* drawn after the opaque ones (blended, added) */
	int transparent;
	/* not drawn at all (shaders this renderer has no way to show) */
	int skip;
	/* the model shader takes the object's change colour (armour) */
	int change_color;
	/* the texture coordinates' scale */
	float u_scale, v_scale;
	/* a model shader's translucency: the share of the first distant light
	that reaches the faces turned away from it (lit at each vertex) */
	float translucency;
	/* a sky's backdrop: a cube map's faces (+x -x +y -y +z -z), looked up
	by the view's direction (the material is skipped as a surface) */
	r_texture *cube[6];
	/* the bitmap its texture came from and how (a model shader's: its
	tinted copies, dos_material_tinted) */
	long bitmap_tag_index;
	int texture_flags;
	/* an environment's detail maps, drawn over its surfaces by a hardware
	rasterizer (dos_detail_maps; dos_level.c): the primary and the
	secondary, each a channel's average 0x80; their scale over the base
	map's coordinates; blended: the base map's alpha chooses between them
	(the primary where it is 1), else the first alone */
	r_texture *detail[2];
	float detail_scale[2][2];
	int detail_blended;
	struct bitmap_data *detail_base;
	/* or the Xbox's texture pass over its surfaces (Direct3D 8,
	dos_xbox_detail: its texture the base map as it is), if stage_count */
	struct r_detail_pass xbox_detail;
	/* a layer added over it at its own tiling (a chicago shader's map tiled
	many times more than the rest, which a stack of them would average
	away: a space sky's stars), in its tint (0x80 as it is); NULL none */
	r_texture *layer;
	float layer_scale[2];
	unsigned long layer_tint;
};

/* whether the level's surfaces have their detail maps drawn over them:
TEXTURE QUALITY HIGH or ORIGINAL where the renderer can (OpenGL), unless
the setting detail_maps = 0 */
int dos_detail_maps(void);
/* whether environment surfaces are drawn as the Xbox's texture pass draws
them (Direct3D 8 with three texture stages, unless the setting d3d_textures
= 0): the base map, then the detail maps mixed over it on the card, nothing
baked (no terrain tiles) */
int dos_xbox_detail(void);
/* whether every map's sky is its own sky model (Direct3D 8, unless the
setting d3d_sky = 0), not a stand-in gradient */
int dos_xbox_sky(void);

void dos_textures_initialize_for_new_map(void);
void dos_textures_dispose_from_old_map(void);
/* a bitmap of a bitmap tag as a texture (cached); NULL if it cannot be */
r_texture *dos_bitmap_texture(long bitmap_tag_index, short bitmap_index, int texture_flags);
/* a particle's sprite sheet, its alpha kept (multiply: its alpha how dark
each texel is, for the darkening shaders; tint: its colour times that over
0x80, 0x808080 none, for 256 colours) */
r_texture *dos_particle_texture(long bitmap_tag_index, short bitmap_index, int multiply, unsigned long tint);
/* dos_particles.c: the game's particles drawn (first_person: those on the
viewer's own first-person weapon, else the rest; none wider than most of a
view view_height pixels high); the setting particles,
and whether one more may be made (effects/particles.c's particle_new) */
void dos_particles_draw(short local_player_index, int first_person, int view_height);
int dos_particles_enabled(void);
int dos_particles_allowed(void);
/* a shader tag's material (cached) */
const struct dos_material *dos_shader_material(long shader_tag_index);
/* its texture times a change colour (0x00RRGGBB, 0xFF as it is): the 8-bit
picture's armour colours, which its monochrome light cannot tint (cached) */
r_texture *dos_material_tinted(const struct dos_material *material, unsigned long change_color);
/* a bitmap's texels decoded to 32-bit (0xAARRGGBB): its level of at most
largest texels a side; the caller frees them */
unsigned long *dos_bitmap_decode(struct bitmap_data *bitmap, int largest, int *width, int *height);
/* a terrain tile's own texture (dos_level.c): a blended environment
shader's base map over [uv_low, uv_high] of its coordinates, times its
detail maps, size texels a side; NULL for a shader of another kind. The
maps decoded for one shader stay until another's are asked for, or
dos_terrain_tiles_done; the texture takes its colours from palette (NULL:
a palette of its own) */
r_texture *dos_terrain_tile(long shader_tag_index, const float *uv_low, const float *uv_high, int size,
	r_shared_palette *palette);
/* a tile's texels added to a histogram (r_histogram_new): the shader's
tiles' shared palette is made from their previews */
void dos_terrain_preview(long shader_tag_index, const float *uv_low, const float *uv_high, int size,
	unsigned long *histogram);
void dos_terrain_tiles_done(void);
/* whether a shader's batches can be terrain tiles (decodes nothing) */
int dos_terrain_shader(long shader_tag_index);

/* the lighting setting: 0 none at all (textures as they are), 1 the
level's baked light, objects taking the level's under them */
extern int dos_lighting;
/* the objects lit as the Xbox lights them (Direct3D 8 with the setting
d3d_object_light, its default there: dos_render_game.c): Halo's own light
for each, its models lit at each vertex (dos_models.c) */
int dos_object_light(void);

/* ---------- models (dos_models.c): which parts are drawn */

enum
{
	DOS_MODELS_ALL,
	DOS_MODELS_OPAQUE,
	DOS_MODELS_TRANSPARENT,
};

void dos_models_pass(int pass);
/* whether the last opaque pass left transparent parts out */
int dos_models_transparent_left_out(void);

/* ---------- the level (dos_level.c) */

void dos_level_load(void);
void dos_level_unload(void);
/* a rebuild when the scripts switch the structure BSP */
void dos_level_follow_bsp(void);
/* the clusters visible from the view's eye (its cluster's potentially
visible set; NULL: all), as the views' objects need them */
void dos_level_draw(unsigned long const *visible_clusters, short cluster_count);
void dos_level_draw_transparent(unsigned long const *visible_clusters, short cluster_count);
/* the colour the frame clears to, and the fog */
unsigned long dos_level_sky_color(void);
/* the sky of the eye's cluster, its model's average colour (has_model: it
has one), each frame */
void dos_level_set_sky(short sky_index, unsigned long model_color, int has_model);
/* the sky's gradient: the map's own (dos_level_sky_colors), or one made
from its sky's colours; NULL for its colour alone */
const unsigned long *dos_level_sky_gradient(void);
/* the sky behind the views (R_SKY_STOPS colours from the horizon up, or NULL)
and the share of the draw distance the fog starts at */
const unsigned long *dos_level_sky_colors(void);
float dos_level_fog_start(void);
/* the level's baked light under a point (0x00RRGGBB, 0x80 as the texture
is), for the objects; FALSE where nothing is below */
int dos_level_light_at(const real_point3d *point, unsigned long *color);

/* ---------- models (dos_models.c) */

void dos_models_dispose_from_old_map(void);
/* the next models drawn are the first-person ones (their own depth) */
void dos_models_first_person(int first_person);

/* ---------- the frame (dos_render_game.c) */

/* the logical screen to the picture's pixels: dos_screen_width x 480, the
width the display's shape gives (640 at 4:3, 854 at 16:9: the game's own
wide screen, halo_screen_width), one scale both ways where the shape is
wider than 4:3 */
extern float dos_screen_scale_x, dos_screen_scale_y;
extern long dos_screen_width;
/* where a 640-wide part of the logical screen centred starts, in the
picture's pixels, while one is drawn (halo_screen_ui_offset: the game's
menus and post-game screens; the port's menus and loading screens); 0
otherwise */
extern float dos_screen_offset_x;
#define DOS_SCREEN_X(x) ((x) * dos_screen_scale_x + dos_screen_offset_x)
float dos_screen_centred_offset(void);
/* VIEW MODEL: 0 low, 1 medium, 2 high, 3 off (dos_models.c) */
int dos_view_model_quality(void);
/* dos_simplify.c: a mesh's triangles (indices into vertices whose positions
are stride floats apart; nodes, where not NULL, a byte a vertex) collapsed
toward target triangles, no vertex moving farther than max_distance from
its triangles' planes, the outer edge and seams kept closed: the triangles
kept (the mesh's own vertex indices) into kept (triangle_count * 3), their
count returned (-1 without memory) */
int dos_simplify(const float *positions, int stride, const unsigned char *nodes, int vertex_count,
	const unsigned short *indices, int triangle_count, int target, double max_distance, unsigned short *kept);
long dos_frame_number(void);

#endif /* __DOS_RENDER_H */
