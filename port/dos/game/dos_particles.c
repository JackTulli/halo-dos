/*
DOS_PARTICLES.C

The game's particles (effects/particles.c: the sparks, dust and blood of
impacts, muzzle flashes, the bits of explosions) drawn by the software
renderer. Halo's own particle renderer (source/render/render_particles.c and
render_sprite.c) is left out of these builds with the rest of source/render;
this one draws what it would have: each particle its sprite as two
triangles, placed as render_sprite.c places it (facing the eye, along the
particle's direction or across it), sized, held to its minimum pixels and
faded in and out as render_particles.c does, with its shader's blend:
added (glows: muzzle flashes, sparks), over by the texels' alpha (dust,
blood), or darkening for the multiplying ones.

The setting particles (SETTINGS > PARTICLES): 0 none made at all (so the
game's tick moves and collides none either: effects/particles.c's
particle_new asks dos_particles_allowed), 1 (the default) up to
particle_limit (96) alive at once. Particle systems (smoke columns, the
weather) and contrails stay off.
*/

#include "cseries.h"

#include "bitmaps/bitmap_group.h"
#include "camera/director.h"
#include "effects/particles.h"
#include "game/players.h"
#include "interface/first_person_weapons.h"
#include "objects/objects.h"
#include "render/render.h"
#include "shaders/shader_definitions.h"

#include "halo_system.h"
#include "dos_render.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

long dos_setting_integer(const char *name, long default_value);
unsigned long dos_settings_generation(void);
extern struct r_settings dos_render_settings;

/* the shaders' framebuffer blend functions */
enum
{
	BLEND_ALPHA,
	BLEND_MULTIPLY,
	BLEND_DOUBLE_MULTIPLY,
	BLEND_ADD,
	BLEND_SUBTRACT,
	BLEND_COMPONENT_MIN,
	BLEND_COMPONENT_MAX,
	BLEND_ALPHA_MULTIPLY_ADD
};

/* render_sprite.h's orientations */
enum
{
	SPRITE_FACING,
	SPRITE_PARALLEL,
	SPRITE_PERPENDICULAR
};

/* shader_effect flags (rasterizer_xbox_transparent_geometry.c) */
#define SHADER_EFFECT_NONLINEAR_TINT 0x02

static struct
{
	unsigned long generation;
	int enabled;
	int limit;
	long drawn;
	/* the view's height in pixels (dos_particles_draw's) */
	float view_height;
} particles = { 0xFFFFFFFFUL };

static void read_settings(void)
{
	unsigned long generation = dos_settings_generation();

	if (generation == particles.generation)
		return;
	particles.generation = generation;
	particles.enabled = dos_setting_integer("particles", 1) != 0;
	particles.limit = (int)dos_setting_integer("particle_limit", 96);
	if (particles.limit < 1)
		particles.limit = 1;
}

int dos_particles_enabled(void)
{
	read_settings();
	return particles.enabled;
}

/* effects/particles.c's particle_new: whether a particle may be made */
int dos_particles_allowed(void)
{
	read_settings();
	return particles.enabled && particle_data && particle_data->actual_count < particles.limit;
}

static void to_view(const float right[3], const float up[3], const float ahead[3], const float *v, float out[3])
{
	out[0] = v[0] * right[0] + v[1] * right[1] + v[2] * right[2];
	out[1] = v[0] * up[0] + v[1] * up[1] + v[2] * up[2];
	out[2] = v[0] * ahead[0] + v[1] * ahead[1] + v[2] * ahead[2];
}

static void cross(const float a[3], const float b[3], float out[3])
{
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

static int normalize(float v[3])
{
	float length = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);

	if (length < 1e-6f)
		return FALSE;
	v[0] /= length;
	v[1] /= length;
	v[2] /= length;
	return TRUE;
}

static unsigned long vertex_channel(float value)
{
	long channel = (long)(value * 128.0f + 0.5f);

	return (unsigned long)(channel < 0 ? 0 : channel > 255 ? 255 : channel);
}

/* where a particle is and which way it goes, in the world: on its object's
node (or the first-person weapon's) if it rides one; FALSE (and the
particle deleted, as the game's renderer does) if its object is gone */
static int particle_place(long particle_index, struct particle_datum *particle, real_point3d *position,
	real_vector3d *direction)
{
	real_matrix4x3 const *node_matrix;

	if (particle->object_index == NONE)
	{
		*position = particle->position;
		*direction = particle->direction;
		return TRUE;
	}
	if (TEST_FLAG(particle->flags, _particle_datum_attached_to_local_player_bit))
		node_matrix = first_person_weapon_get_node_matrix(particle->local_player_index, particle->node_index);
	else if (object_try_and_get(particle->object_index))
		node_matrix = object_get_node_matrix(particle->object_index, particle->node_index);
	else
	{
		particle_delete(particle_index);
		return FALSE;
	}
	if (!node_matrix)
		return FALSE;
	matrix4x3_transform_point(node_matrix, &particle->position, position);
	matrix4x3_transform_normal(node_matrix, &particle->direction, direction);
	return TRUE;
}

static void draw_particle(long particle_index, struct particle_datum *particle, const float right[3],
	const float up[3], const float ahead[3], const float eye[3])
{
	struct particle_definition *definition = particle_definition_get(particle->definition_index);
	struct bitmap_group *group;
	struct bitmap_group_sequence *sequence;
	struct bitmap_group_sprite *sprite;
	struct bitmap_data *bitmap;
	real_point3d position;
	real_vector3d direction;
	float world[3], origin[3], heading[3], basis_x[3], basis_y[3];
	float radius, distance, diameter, scale, fade, alpha, sine = 0.0f, cosine = 1.0f;
	short blend = definition->shader.framebuffer_blend_function;
	struct r_vertex vertices[4];
	static const unsigned short indices[6] = { 0, 1, 2, 0, 2, 3 };
	unsigned long color;
	unsigned flags;
	r_texture *texture;

	if (definition->bitmap.index == NONE ||
		blend == BLEND_SUBTRACT || blend == BLEND_COMPONENT_MIN)
	{
		return;
	}
	if (!particle_place(particle_index, particle, &position, &direction))
		return;
	radius = particle_get_radius(particle_index);
	world[0] = position.x;
	world[1] = position.y;
	world[2] = position.z;
	if (radius <= 0.0f || !r_sphere_visible(world, radius))
		return;
	/* (its size in pixels: under the lod cutoff it is not drawn) */
	distance = r_distance(world);
	diameter = 2.0f * radius * r_pixels_per_unit_at(distance);
	if (diameter <= definition->lod_cutoff)
		return;
	group = bitmap_group_get(definition->bitmap.index);
	if (!group || particle->sequence_index < 0 || particle->sequence_index >= group->sequences.count)
		return;
	sequence = TAG_BLOCK_GET_ELEMENT(&group->sequences, particle->sequence_index, struct bitmap_group_sequence);
	if (sequence->first_bitmap_index == NONE || particle->frame_index < 0 ||
		particle->frame_index >= sequence->sprites.count)
	{
		return;
	}
	sprite = TAG_BLOCK_GET_ELEMENT(&sequence->sprites, particle->frame_index, struct bitmap_group_sprite);
	if (sprite->bitmap_index < 0 || sprite->bitmap_index >= group->bitmaps.count)
		return;
	bitmap = TAG_BLOCK_GET_ELEMENT(&group->bitmaps, sprite->bitmap_index, struct bitmap_data);

	/* render_particles.c's size and fade */
	scale = 2.0f * radius * definition->runtime_oo_width;
	if (diameter < definition->minimum_pixels)
		scale *= definition->minimum_pixels / diameter;
	/* (no wider than most of the view: the muzzle flashes, a fifth of a
	unit from the eye, covered the whole screen in added pixels, a tenth
	of a second a shot on a DOS machine) */
	if (diameter > 0.6f * particles.view_height)
		scale *= 0.6f * particles.view_height / diameter;
	scale *= (float)bitmap->width;
	fade = 1.0f;
	if (definition->fade_in_time > 0.0f && particle->age < definition->fade_in_time)
		fade = particle->age / definition->fade_in_time;
	if (definition->fade_out_time > 0.0f && particle->lifespan - particle->age < definition->fade_out_time)
		fade *= (particle->lifespan - particle->age) / definition->fade_out_time;
	alpha = blend != BLEND_ALPHA && !(definition->shader.flags & SHADER_EFFECT_NONLINEAR_TINT) ? fade :
		particle->color.alpha * fade;
	if (alpha <= 0.02f)
		return;

	/* render_sprite.c's basis, in the view's space (x right, y up, z ahead) */
	{
		float relative[3] = { world[0] - eye[0], world[1] - eye[1], world[2] - eye[2] };
		float heading_world[3] = { direction.i, direction.j, direction.k };

		to_view(right, up, ahead, relative, origin);
		to_view(right, up, ahead, heading_world, heading);
	}
	if (definition->sprite_orientation == SPRITE_PARALLEL && normalize(heading))
	{
		memcpy(basis_x, heading, sizeof(basis_x));
		cross(origin, basis_x, basis_y);
		if (!normalize(basis_y))
			return;
	}
	else if (definition->sprite_orientation == SPRITE_PERPENDICULAR && normalize(heading))
	{
		static const float world_up[3] = { 0.0f, 0.0f, 1.0f }, world_forward[3] = { 1.0f, 0.0f, 0.0f };
		float reference[3], dot;

		to_view(right, up, ahead, world_up, reference);
		dot = heading[0] * reference[0] + heading[1] * reference[1] + heading[2] * reference[2];
		if (dot * dot > 0.99f)
			to_view(right, up, ahead, world_forward, reference);
		cross(heading, reference, basis_x);
		if (!normalize(basis_x))
			return;
		cross(basis_x, heading, basis_y);
	}
	else
	{
		basis_x[0] = 1.0f, basis_x[1] = 0.0f, basis_x[2] = 0.0f;
		basis_y[0] = 0.0f, basis_y[1] = 1.0f, basis_y[2] = 0.0f;
	}
	if (particle->rotation != 0.0f)
	{
		sine = sinf(particle->rotation);
		cosine = cosf(particle->rotation);
	}

	/* the colour: added ones fade by darkening, the others by their alpha.
	The 256-colour picture's light is grey: the tint goes into a copy of the
	texture there, the vertices keep the fade */
	{
		int multiply = blend == BLEND_MULTIPLY || blend == BLEND_DOUBLE_MULTIPLY;
		unsigned long tint = multiply ? 0x808080 : (vertex_channel(particle->color.red) << 16) |
			(vertex_channel(particle->color.green) << 8) | vertex_channel(particle->color.blue);
		int eight = dos_render_settings.bits == 8;

		texture = dos_particle_texture(definition->bitmap.index, sprite->bitmap_index, multiply,
			eight ? tint : 0x808080);
		if (!texture)
			return;
		if (eight)
			tint = 0x808080;
		if (blend == BLEND_ALPHA || multiply)
		{
			color = tint;
			flags = R_DRAW_BLEND | R_DRAW_ALPHA | R_DRAW_TWO_SIDED | R_DRAW_VIEW_SPACE;
		}
		else
		{
			color = (vertex_channel((float)((tint >> 16) & 0xFF) / 128.0f * alpha) << 16) |
				(vertex_channel((float)((tint >> 8) & 0xFF) / 128.0f * alpha) << 8) |
				vertex_channel((float)(tint & 0xFF) / 128.0f * alpha);
			flags = R_DRAW_ADDITIVE | R_DRAW_NO_FOG | R_DRAW_TWO_SIDED | R_DRAW_VIEW_SPACE;
		}
	}
	particle->last_rendered_frame_index = render.frame_index;
	for (int k = 0; k < 4; k++)
	{
		float u = (((k >> 1) ^ k) & 1) ? sprite->bounds.x1 : sprite->bounds.x0;
		float v = (k & 2) ? sprite->bounds.y0 : sprite->bounds.y1;
		float offset_x = u - (sprite->bounds.x0 + sprite->registration_point.x);
		float offset_y = (sprite->registration_point.y + sprite->bounds.y0) - v;
		float x = offset_x * cosine - offset_y * sine;
		float y = offset_y * cosine + offset_x * sine;

		if (TEST_FLAG(particle->flags, _particle_datum_u_mirror_bit))
			x = -x;
		if (TEST_FLAG(particle->flags, _particle_datum_v_mirror_bit))
			y = -y;
		for (int axis = 0; axis < 3; axis++)
			vertices[k].position[axis] = origin[axis] + (basis_x[axis] * x + basis_y[axis] * y) * scale;
		vertices[k].u = u;
		vertices[k].v = v;
		vertices[k].color = color;
	}
	if (flags & R_DRAW_ALPHA)
		r_set_alpha_scale(alpha);
	r_draw_triangles(vertices, 4, indices, 2, NULL, texture, flags);
	if (flags & R_DRAW_ALPHA)
		r_set_alpha_scale(1.0f);
	particles.drawn++;
	{
		/* (testing, setting debug_particles=<frame>: each one drawn then; -1:
		those on objects, every frame) */
		extern long dos_frame_number(void);
		long wanted = dos_setting_integer("debug_particles", 0);

		if (wanted && (dos_frame_number() == wanted || (wanted < 0 && particle->object_index != NONE)))
		{
			char line[256];

			snprintf(line, sizeof(line), "particle %s: radius %.3f at %.2f (%.0f px, min %.0f, cutoff %.0f), size %.3f, "
				"blend %d, orientation %d, fade %.2f, alpha %.2f, colour %.2f %.2f %.2f, object %ld, flags %X, format %d, sprite %.2f-%.2f",
				tag_get_name(particle->definition_index), (double)radius, (double)distance, (double)diameter,
				(double)definition->minimum_pixels, (double)definition->lod_cutoff, (double)scale, (int)blend,
				(int)definition->sprite_orientation, (double)fade, (double)alpha, (double)particle->color.red,
				(double)particle->color.green, (double)particle->color.blue, particle->object_index,
				(unsigned)particle->flags, (int)bitmap->format, (double)sprite->bounds.x0, (double)sprite->bounds.x1);
			system_log(line);
		}
	}
}

void dos_particles_draw(short local_player_index, int first_person, int view_height)
{
	float right[3], up[3], ahead[3], eye[3];
	short viewer;

	if (!particle_data || !dos_particles_enabled() || !particle_data->actual_count)
		return;
	/* render_particles.c: a particle of the local player's own first-person
	weapon (or one it should not see in first person) is the viewer's only
	while the viewer sees through its eyes */
	viewer = local_player_index;
	if (viewer == NONE || director_get_perspective(viewer) != _director_perspective_first_person)
		viewer = MAXIMUM_LOCAL_PLAYERS;
	particles.view_height = (float)(view_height > 0 ? view_height : 200);
	r_view_basis(right, up, ahead, eye);
	for (long particle_index = data_next_index(particle_data, NONE); particle_index != NONE;
		particle_index = data_next_index(particle_data, particle_index))
	{
		struct particle_datum *particle = particle_get(particle_index);
		int owned = particle->local_player_index == viewer;
		int on_weapon = owned && TEST_FLAG(particle->flags, _particle_datum_dont_draw_third_person_bit);

		if (TEST_FLAG(particle->flags, _particle_datum_dont_draw_first_person_bit) && owned)
			continue;
		if (TEST_FLAG(particle->flags, _particle_datum_dont_draw_third_person_bit) && !owned)
			continue;
		if (on_weapon != (first_person != 0))
			continue;
		draw_particle(particle_index, particle, right, up, ahead, eye);
	}
	if (!first_person && dos_setting_integer("debug_particles", 0))
	{
		/* (testing: in the log every 300 frames) */
		static long frames;

		if (++frames % 300 == 0)
		{
			char line[96];

			snprintf(line, sizeof(line), "particles: %d alive, %ld drawn in the last 300 frames",
				(int)particle_data->actual_count, particles.drawn);
			system_log(line);
			particles.drawn = 0;
		}
	}
}
