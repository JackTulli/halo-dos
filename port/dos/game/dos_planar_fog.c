/*
DOS_PLANAR_FOG.C

Halo's planar fog on Direct3D 8 (the Xbox look: r_draws_lightmaps), from the
game's own data, as Halo's rasterizer_xbox_environment_fog.c draws it: the
fog a cluster's fog plane holds (the sea round b30's island, a50's and c10's
water) over every surface under the plane, by how far under it a pixel is
and how far it is from the eye, looked up at each pixel in the game's
planar fog density map (game globals > rasterizer data): its alpha where
the eye is over the plane, its blue where the eye is under it, mixed by how
deep the eye is. Without it a lit sea floor shows through the water's
added reflection as if the water were clear, and comes out white.

The surfaces are drawn again over themselves (dos_level.c), the fog's
colour over them as much as the map's density says, that colour fogged by
the view's atmospheric fog as far as Halo's pass lets it be (by how high
the eye is over the plane: from just over the water, hardly). The view's
fog is the eye's cluster's (structure_get_planar_fog), once a view; the
atmospheric fog is Halo's too here (dos_xbox_atmospheric_fog, set by
dos_render_game.c).

Setting d3d_fog 0: neither (the fade into the sky's colour instead).
*/

#include "cseries.h"

#include "game/game_globals.h"
#include "render/render.h"
#include "scenario/scenario.h"
#include "structures/structures.h"
#include "tag_files/tag_groups.h"

#include "halo_system.h"
#include "dos_render.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

long dos_setting_integer(const char *name, long default_value);
long dos_frame_number(void);
unsigned long *dos_bitmap_tag_decode(long bitmap_tag_index, short bitmap_index, int largest, int *width, int *height);

/* the eye's depth in the fog changes the map in steps of this many */
#define EYE_STEPS 32

/* render_fog's planar modes and its definition's flags (each of Halo's
files that reads them has them) */
enum
{
	_planar_fog_off = 0,
	_planar_fog_normal,
	_planar_fog_fully_fogged
};

#define FOG_SCREEN_EFFECT_ONLY_BIT 2

static struct
{
	/* the view's fog: whether there is one, its plane (a point p is under
	it by plane[3] - n . p), the map's coordinates' scales, the eye */
	int active;
	float plane[4];
	float inverse_distance, inverse_depth;
	float eye[3], forward[3];
	float color[3];
	/* the view's own fog (r_view's) over the fog's colour: how much of it
	Halo lets through (its atmospheric fog's share by how high the eye is
	over the plane, rasterizer_xbox_environment_fog.c's atmospheric eye
	density), and the rise to all of it at the far clip after */
	int air_fogged;
	float air_start, air_scale, air_maximum, air_far, air_far_scale, air_share;
	float air_color[3];
	/* surfaces over the plane take none (the map's nearest row clear) */
	int over_clear;
	/* the density map (0xAARRGGBB, decoded once a map) and its texture for
	this eye depth and maximum density */
	unsigned long *density;
	int width, height, tried;
	r_texture *texture;
	int texture_eye, texture_maximum;
	/* vertices with the fog's coordinates */
	struct r_vertex *vertices;
	int vertex_size;
	/* (testing: what the pass drew, this frame's and the last's) */
	long logged_frame, counted_frame;
	long draws, triangles, last_draws, last_triangles;
} fog;

static void *raw_malloc(size_t size) { return (malloc)(size); }
static void raw_free(void *pointer) { (free)(pointer); }

void dos_planar_fog_dispose(void)
{
	raw_free(fog.density);
	r_texture_delete(fog.texture);
	raw_free(fog.vertices);
	memset(&fog, 0, sizeof(fog));
}

/* the game's planar fog density map, decoded once a map */
static int load_density(void)
{
	struct game_globals *globals;
	const struct game_globals_rasterizer_data *data;

	if (fog.tried)
		return fog.density != NULL;
	fog.tried = TRUE;
	globals = scenario_get_game_globals();
	if (!globals || globals->rasterizer_data.count < 1 || !globals->rasterizer_data.address)
		return FALSE;
	data = (const struct game_globals_rasterizer_data *)xbox_pointer(globals->rasterizer_data.address);
	if (data->planar_fog_density.index == NONE)
		return FALSE;
	fog.density = dos_bitmap_tag_decode(data->planar_fog_density.index, 0, 256, &fog.width, &fog.height);
	/* (testing: the atmospheric fog's map too, along its one row) */
	if (dos_setting_integer("debug_planar_fog", 0) && data->atmospheric_fog_density.index != NONE)
	{
		int width, height, length;
		unsigned long *texels = dos_bitmap_tag_decode(data->atmospheric_fog_density.index, 0, 256, &width, &height);
		char line[300];

		if (texels)
		{
			length = snprintf(line, sizeof(line), "atmospheric fog density map %dx%d:", width, height);
			for (int c = 0; c < 9 && length < (int)sizeof(line) - 16; c++)
			{
				unsigned long texel = texels[(width - 1) * c / 8];

				length += snprintf(line + length, sizeof(line) - (size_t)length, " %08lX", texel);
			}
			system_log(line);
			raw_free(texels);
		}
	}
	/* (testing, setting debug_planar_fog: the map at a few places, alpha and
	blue, its rows the depth, its columns the distance) */
	if (fog.density && dos_setting_integer("debug_planar_fog", 0))
	{
		char line[300];
		int length;

		snprintf(line, sizeof(line), "planar fog density map %s: %dx%d", tag_get_name(data->planar_fog_density.index),
			fog.width, fog.height);
		system_log(line);
		for (int r = 0; r < 5; r++)
		{
			int y = (fog.height - 1) * r / 4;

			length = snprintf(line, sizeof(line), "  depth %.2f:", (double)r / 4.0);
			for (int c = 0; c < 5 && length < (int)sizeof(line) - 16; c++)
			{
				unsigned long texel = fog.density[y * fog.width + (fog.width - 1) * c / 4];

				length += snprintf(line + length, sizeof(line) - (size_t)length, " %02lX/%02lX", texel >> 24, texel & 0xFF);
			}
			system_log(line);
		}
	}
	return fog.density != NULL;
}

/* its texture for the eye's depth in the fog (0 to EYE_STEPS) and the
fog's maximum density (0 to 255): white, the alpha the density */
static r_texture *density_texture(int eye, int maximum)
{
	unsigned long *texels;
	long count = (long)fog.width * fog.height;

	if (fog.texture && fog.texture_eye == eye && fog.texture_maximum == maximum)
		return fog.texture;
	r_texture_delete(fog.texture);
	fog.texture = NULL;
	texels = raw_malloc((size_t)count * sizeof(unsigned long));
	if (!texels)
		return NULL;
	fog.over_clear = TRUE;
	for (long i = 0; i < count; i++)
	{
		unsigned long above = fog.density[i] >> 24, below = fog.density[i] & 0xFF;
		unsigned long density = ((above * (unsigned long)(EYE_STEPS - eye) + below * (unsigned long)eye) / EYE_STEPS) *
			(unsigned long)maximum / 255;

		texels[i] = (density << 24) | 0xFFFFFFUL;
		if (i < fog.width && density)
			fog.over_clear = FALSE;
	}
	fog.texture = r_texture_new(texels, fog.width, fog.height, R_TEXTURE_ALPHA | R_TEXTURE_CLAMP | R_TEXTURE_ONE_LEVEL);
	fog.texture_eye = eye;
	fog.texture_maximum = maximum;
	raw_free(texels);
	return fog.texture;
}

void dos_planar_fog_view(short cluster_index, const struct r_view *view, short sky_index, float air_distance)
{
	struct render_fog render_fog;
	const float *eye = view->position, *forward = view->forward;
	float z_far = view->z_far, distance, eye_density, maximum;

	fog.active = FALSE;
	if (!dos_xbox_fog() || cluster_index == NONE || !global_structure_bsp_get())
	{
		return;
	}
	memset(&render_fog, 0, sizeof(render_fog));
	structure_get_planar_fog(cluster_index, &render_fog);
	/* (Halo's own checks: rasterizer_xbox.c _rasterizer_window_set_fog) */
	if (render_fog.planar_mode == _planar_fog_off ||
		TEST_FLAG(render_fog.fog_definition_flags, FOG_SCREEN_EFFECT_ONLY_BIT) ||
		!load_density())
	{
		return;
	}
	maximum = render_fog.planar_maximum_density > 0.0f ? render_fog.planar_maximum_density : 1.0f;
	if (render_fog.planar_mode == _planar_fog_fully_fogged)
	{
		/* (the plane the eye's, facing it, at the far clip) */
		render_fog.planar_maximum_depth = 1.0f;
		render_fog.plane.n.i = forward[0];
		render_fog.plane.n.j = forward[1];
		render_fog.plane.n.k = forward[2];
		render_fog.plane.d = forward[0] * eye[0] + forward[1] * eye[1] + forward[2] * eye[2] + z_far;
	}
	/* (indoors no farther than the atmospheric fog: render.c) */
	if (air_distance > 0.0f && sky_index == NONE && render_fog.planar_maximum_distance > air_distance)
		render_fog.planar_maximum_distance = air_distance;
	if (render_fog.planar_maximum_distance <= 0.0f || render_fog.planar_maximum_depth <= 0.0f)
		return;
	fog.plane[0] = render_fog.plane.n.i;
	fog.plane[1] = render_fog.plane.n.j;
	fog.plane[2] = render_fog.plane.n.k;
	fog.plane[3] = render_fog.plane.d;
	fog.inverse_distance = 1.0f / render_fog.planar_maximum_distance;
	fog.inverse_depth = 1.0f / render_fog.planar_maximum_depth;
	memcpy(fog.eye, eye, sizeof(fog.eye));
	memcpy(fog.forward, forward, sizeof(fog.forward));
	distance = fog.plane[0] * eye[0] + fog.plane[1] * eye[1] + fog.plane[2] * eye[2] - fog.plane[3];
	eye_density = -distance * fog.inverse_depth;
	eye_density = eye_density < 0.0f ? 0.0f : eye_density > 1.0f ? 1.0f : eye_density;
	maximum = maximum > 1.0f ? 1.0f : maximum;
	if (!density_texture((int)(eye_density * (float)EYE_STEPS + 0.5f), (int)(maximum * 255.0f + 0.5f)))
		return;
	fog.color[0] = render_fog.planar_color.red;
	fog.color[1] = render_fog.planar_color.green;
	fog.color[2] = render_fog.planar_color.blue;
	/* the view's fog over it: Halo's atmospheric fog as far as the eye is
	over the plane (all of it where Halo has none: the view's fog is then
	this renderer's own, at the draw distance) */
	fog.air_fogged = view->fog_end > view->fog_start && view->fog_end > 0.0f;
	if (fog.air_fogged)
	{
		fog.air_start = view->fog_start;
		fog.air_scale = 1.0f / (view->fog_end - view->fog_start);
		fog.air_maximum = view->fog_maximum > 0.0f && view->fog_maximum < 1.0f ? view->fog_maximum : 1.0f;
		fog.air_far = view->fog_far > view->fog_end ? view->fog_far : view->fog_end;
		fog.air_far_scale = fog.air_maximum < 1.0f && z_far > fog.air_far ? 1.0f / (z_far - fog.air_far) : 0.0f;
		fog.air_share = air_distance > 0.0f ? distance / air_distance : 1.0f;
		fog.air_share = fog.air_share < 0.0f ? 0.0f : fog.air_share > 1.0f ? 1.0f : fog.air_share;
		for (int k = 0; k < 3; k++)
			fog.air_color[k] = (float)((view->fog_color >> (16 - 8 * k)) & 0xFF) / 255.0f;
	}
	/* (in the fog's own clear water the surfaces over the plane take none;
	seen from under it, they do) */
	if (eye_density > 0.0f)
		fog.over_clear = FALSE;
	fog.active = TRUE;
	if (dos_setting_integer("debug_planar_fog", 0) && fog.logged_frame + 150 <= dos_frame_number())
	{
		char line[300];

		fog.logged_frame = dos_frame_number();
		snprintf(line, sizeof(line), "planar fog: cluster %d, mode %d, flags %04X, plane %.3f %.3f %.3f %.2f, colour %.2f "
			"%.2f %.2f, density %.2f, distance %.1f, depth %.1f, eye %.2f under (density %.2f); last frame %ld draws, "
			"%ld triangles", cluster_index, render_fog.planar_mode, render_fog.fog_definition_flags, (double)fog.plane[0],
			(double)fog.plane[1], (double)fog.plane[2], (double)fog.plane[3], (double)render_fog.planar_color.red,
			(double)render_fog.planar_color.green, (double)render_fog.planar_color.blue, (double)maximum,
			(double)render_fog.planar_maximum_distance, (double)render_fog.planar_maximum_depth, (double)-distance,
			(double)eye_density, fog.last_draws, fog.last_triangles);
		system_log(line);
		snprintf(line, sizeof(line), "  air fog over it: %s, from %.1f by %.4f, at most %.2f, far %.1f by %.4f, share %.2f",
			fog.air_fogged ? "yes" : "no", (double)fog.air_start, (double)fog.air_scale, (double)fog.air_maximum,
			(double)fog.air_far, (double)fog.air_far_scale, (double)fog.air_share);
		system_log(line);
	}
}

/* the view's atmospheric fog as Halo works it out (scenario.c: the sky's
outdoor fog, its indoor fog where the eye's cluster has no sky, eased
toward a new one as the eye moves): FALSE where there is none */
int dos_xbox_atmospheric_fog(short local_player_index, short sky_index, const float eye[3], float *start, float *end,
	float *maximum, unsigned long *color)
{
	struct render_fog render_fog;
	real_point3d point;

	point.x = eye[0];
	point.y = eye[1];
	point.z = eye[2];
	memset(&render_fog, 0, sizeof(render_fog));
	scenario_get_atmospheric_fog(local_player_index, sky_index, &point, &render_fog);
	if (dos_setting_integer("debug_planar_fog", 0) && fog.logged_frame == dos_frame_number())
	{
		char line[200];

		snprintf(line, sizeof(line), "atmospheric fog: sky %d, colour %.2f %.2f %.2f, density %.2f, from %.1f to %.1f",
			sky_index, (double)render_fog.atmospheric_color.red, (double)render_fog.atmospheric_color.green,
			(double)render_fog.atmospheric_color.blue, (double)render_fog.atmospheric_maximum_density,
			(double)render_fog.atmospheric_minimum_distance, (double)render_fog.atmospheric_maximum_distance);
		system_log(line);
	}
	if (render_fog.atmospheric_maximum_distance <= 0.0f)
		return FALSE;
	*start = render_fog.atmospheric_minimum_distance;
	*end = render_fog.atmospheric_maximum_distance;
	*maximum = render_fog.atmospheric_maximum_density > 0.0f ? render_fog.atmospheric_maximum_density : 1.0f;
	*maximum = *maximum > 1.0f ? 1.0f : *maximum;
	*color = 0;
	{
		const float channels[3] = { render_fog.atmospheric_color.red, render_fog.atmospheric_color.green,
			render_fog.atmospheric_color.blue };

		for (int k = 0; k < 3; k++)
		{
			float value = channels[k] * 255.0f + 0.5f;

			*color |= (unsigned long)(value < 0.0f ? 0.0f : value > 255.0f ? 255.0f : value) << (16 - 8 * k);
		}
	}
	return TRUE;
}

int dos_planar_fog_reaches(const float center[3], float radius)
{
	if (!fog.active)
		return FALSE;
	if (!fog.over_clear)
		return TRUE;
	/* (some of it under the plane) */
	return fog.plane[0] * center[0] + fog.plane[1] * center[1] + fog.plane[2] * center[2] - fog.plane[3] < radius;
}

/* triangles drawn already, again with the fog over them: each vertex's
place in the map (its distance along the view, its depth under the plane) */
void dos_planar_fog_draw(const struct r_vertex *vertices, int vertex_count, const unsigned short *indices,
	int triangle_count, unsigned flags)
{
	float view_distance;

	if (!fog.active || !fog.texture || vertex_count <= 0 || triangle_count <= 0)
		return;
	if (fog.counted_frame != dos_frame_number())
	{
		fog.counted_frame = dos_frame_number();
		fog.last_draws = fog.draws;
		fog.last_triangles = fog.triangles;
		fog.draws = fog.triangles = 0;
	}
	fog.draws++;
	fog.triangles += triangle_count;
	if (vertex_count > fog.vertex_size)
	{
		raw_free(fog.vertices);
		fog.vertices = raw_malloc((size_t)vertex_count * sizeof(*fog.vertices));
		fog.vertex_size = fog.vertices ? vertex_count : 0;
		if (!fog.vertices)
			return;
	}
	view_distance = fog.forward[0] * fog.eye[0] + fog.forward[1] * fog.eye[1] + fog.forward[2] * fog.eye[2];
	for (int i = 0; i < vertex_count; i++)
	{
		const float *p = vertices[i].position;
		struct r_vertex *out = &fog.vertices[i];
		float depth = fog.forward[0] * p[0] + fog.forward[1] * p[1] + fog.forward[2] * p[2] - view_distance;
		float share = 0.0f;

		out->position[0] = p[0];
		out->position[1] = p[1];
		out->position[2] = p[2];
		out->u = depth * fog.inverse_distance;
		out->v = (fog.plane[3] - (fog.plane[0] * p[0] + fog.plane[1] * p[1] + fog.plane[2] * p[2])) * fog.inverse_depth;
		/* its colour, the view's fog over it as Halo's pass mixes them (the
		vertex's: the card's fog is off, the light being the fog here) */
		if (fog.air_fogged)
		{
			float air = (depth - fog.air_start) * fog.air_scale;

			share = (air < 0.0f ? 0.0f : air > 1.0f ? 1.0f : air) * fog.air_maximum * fog.air_share;
			if (fog.air_far_scale > 0.0f && depth > fog.air_far)
			{
				float rise = (depth - fog.air_far) * fog.air_far_scale;

				share += (1.0f - share) * (rise > 1.0f ? 1.0f : rise);
			}
		}
		out->color = 0;
		for (int k = 0; k < 3; k++)
		{
			/* (as light: 0x80 the texel, which is white, as it is) */
			float value = (fog.color[k] + (fog.air_color[k] - fog.color[k]) * share) * 128.0f + 0.5f;

			out->color |= (unsigned long)(value < 0.0f ? 0.0f : value > 255.0f ? 255.0f : value) << (16 - 8 * k);
		}
	}
	/* (testing, setting debug_planar_fog 2: the fog magenta; 3: what the
	pass covers, half magenta, whatever the density) */
	switch (dos_setting_integer("debug_planar_fog", 0))
	{
	case 2:
	case 3:
		for (int i = 0; i < vertex_count; i++)
			fog.vertices[i].color = 0x800080UL;
		if (dos_setting_integer("debug_planar_fog", 0) == 3)
		{
			r_draw_triangles(fog.vertices, vertex_count, indices, triangle_count, NULL, NULL,
				R_DRAW_BLEND | R_DRAW_NO_FOG | (flags & R_DRAW_TWO_SIDED));
			return;
		}
		break;
	default:
		break;
	}
	r_draw_triangles(fog.vertices, vertex_count, indices, triangle_count, NULL, fog.texture,
		R_DRAW_BLEND | R_DRAW_ALPHA | R_DRAW_NO_FOG | (flags & R_DRAW_TWO_SIDED));
}
