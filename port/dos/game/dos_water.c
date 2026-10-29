/*
DOS_WATER.C

Halo's water (shader_transparent_water) on a card that draws it (Direct3D 8,
r_draws_water: r_public.h says how), from the shader's own data, as Halo's
rasterizer_xbox_water.c makes it:

- the ripples: up to four layers of the shader's ripple maps (normal maps),
  each repeated and moving at its own angle and speed, mixed by their
  contributions into one normal map of 128 texels a side each frame (Halo
  renders it on the card; here the processor does, the texels then
  normalized), its smaller levels fading toward flat by the shader's fade
  factor, so that the ripples settle in the distance;
- the surface's coordinates into that map: the vertices' times the ripple
  scale, moving at the shader's own angle and speed;
- the reflection: the shader's cube map, on the card as a cube map;
- the tint from the shader's parallel tint (looking along the water) to its
  perpendicular one (straight down): a row of 256 texels for the card to
  look up at each pixel (its alpha the reflection's share where the base
  map's alpha modulates it), and the whole draw's for a card that takes it
  once (Halo's own way: by the camera's direction and the water's plane);
- the base map, multiplying what is under the water where the shader says.

The game's time is the ripples' clock (they stop in the pause, as Halo's).
*/

#include "cseries.h"

#include "math/real_math.h"
#include "tag_files/tag_groups.h"

#include "halo_system.h"
#include "dos_render.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

long dos_setting_integer(const char *name, long default_value);
long dos_frame_number(void);
/* the game's time in seconds, between ticks too (dos_render_game.c) */
float dos_game_seconds(void);
unsigned long *dos_bitmap_tag_decode(long bitmap_tag_index, short bitmap_index, int largest, int *width, int *height);
unsigned long *dos_bitmap_tag_decode_face(long bitmap_tag_index, int face, int largest, int *width, int *height);

#define WATERS 8
#define RIPPLES 4
#define BUMP_SIZE 128
#define BUMP_LEVELS 8 /* 128 to 1 */
#define TINT_SIZE 256
#define CUBE_LARGEST 256

struct ripple
{
	float contribution, angle, velocity, offset[2];
	int repeats;
	/* its map's texels (signed: each channel's byte), or NULL: flat */
	unsigned long *texels;
	int width, height;
};

struct dos_water
{
	long shader_tag_index;
	unsigned short flags;
	long base_map, reflection_map;
	/* alpha, red, green, blue */
	float perpendicular[4], parallel[4];
	float angle, velocity, scale;
	int levels;
	float fade, lod_bias;
	struct ripple ripples[RIPPLES];
	/* the card's textures; whether they could be made */
	unsigned long bump, reflection, tint, base;
	int made, failed;
	/* the normal map's levels (128 down to 1), and the frame they are of */
	unsigned long *bump_levels[BUMP_LEVELS];
	long frame;
};

static struct dos_water waters[WATERS];
static int water_count;

static float read_real(const unsigned char *shader, long offset)
{
	float value;

	memcpy(&value, shader + offset, sizeof(value));
	return value;
}

static short read_short(const unsigned char *shader, long offset)
{
	short value;

	memcpy(&value, shader + offset, sizeof(value));
	return value;
}

static long reference_index(const unsigned char *shader, long offset)
{
	const struct tag_reference *reference = (const struct tag_reference *)(shader + offset);

	return reference->index;
}

struct dos_water *dos_water_new(long shader_tag_index, const void *definition)
{
	const unsigned char *shader = definition;
	const struct tag_block *block = (const struct tag_block *)(shader + 0x124);
	long ripple_maps = reference_index(shader, 0xC8);
	struct dos_water *water;

	for (int i = 0; i < water_count; i++)
	{
		if (waters[i].shader_tag_index == shader_tag_index)
			return waters[i].failed ? NULL : &waters[i];
	}
	if (water_count == WATERS)
		return NULL;
	water = &waters[water_count++];
	memset(water, 0, sizeof(*water));
	water->shader_tag_index = shader_tag_index;
	water->flags = (unsigned short)read_short(shader, 0x28);
	water->base_map = reference_index(shader, 0x4C);
	water->reflection_map = reference_index(shader, 0x9C);
	for (int k = 0; k < 4; k++)
	{
		water->perpendicular[k] = read_real(shader, 0x6C + k * 4);
		water->parallel[k] = read_real(shader, 0x7C + k * 4);
	}
	water->angle = read_real(shader, 0xBC);
	water->velocity = read_real(shader, 0xC0);
	water->scale = read_real(shader, 0xC4);
	water->levels = read_short(shader, 0xD8);
	water->levels = water->levels < 1 ? 1 : water->levels > RIPPLES ? RIPPLES : water->levels;
	water->fade = read_real(shader, 0xDC);
	water->lod_bias = read_real(shader, 0xE0);
	if (water->reflection_map == NONE)
	{
		water->failed = TRUE;
		return NULL;
	}
	/* each layer (a layer the shader has not, flat, its contribution none;
	a pair without any, the second whole, as Halo does) */
	for (int r = 0; r < RIPPLES; r++)
	{
		struct ripple *ripple = &water->ripples[r];

		ripple->repeats = 1;
		if (r < block->count && block->address)
		{
			const unsigned char *element = (const unsigned char *)xbox_pointer(block->address) + r * 0x4C;
			short map_index = read_short(element, 0x3A);

			ripple->contribution = read_real(element, 0x04);
			ripple->angle = read_real(element, 0x28);
			ripple->velocity = read_real(element, 0x2C);
			ripple->offset[0] = read_real(element, 0x30);
			ripple->offset[1] = read_real(element, 0x34);
			ripple->repeats = read_short(element, 0x38) > 0 ? read_short(element, 0x38) : 1;
			if (ripple_maps != NONE)
				ripple->texels = dos_bitmap_tag_decode(ripple_maps, map_index, 256, &ripple->width, &ripple->height);
			/* (testing, setting debug_shaders: each layer and its map's mean) */
			if (dos_setting_integer("debug_shaders", 0))
			{
				unsigned long sum[4] = { 0, 0, 0, 0 };
				long texels = ripple->texels ? (long)ripple->width * ripple->height : 0;
				char line[200];

				for (long i = 0; i < texels; i++)
				{
					for (int k = 0; k < 4; k++)
						sum[k] += (ripple->texels[i] >> (24 - 8 * k)) & 0xFF;
				}
				snprintf(line, sizeof(line), "  ripple %d: map %d (%dx%d, mean %02lX%02lX%02lX%02lX), contribution %.2f, "
					"repeats %d, angle %.2f, velocity %.3f", r, map_index, ripple->width, ripple->height,
					texels ? sum[0] / texels : 0, texels ? sum[1] / texels : 0, texels ? sum[2] / texels : 0,
					texels ? sum[3] / texels : 0, (double)ripple->contribution, ripple->repeats, (double)ripple->angle,
					(double)ripple->velocity);
				system_log(line);
			}
		}
	}
	if (water->ripples[0].contribution == 0.0f && water->ripples[1].contribution == 0.0f)
		water->ripples[1].contribution = 1.0f;
	if (water->ripples[2].contribution == 0.0f && water->ripples[3].contribution == 0.0f)
		water->ripples[3].contribution = 1.0f;
	if (dos_setting_integer("debug_shaders", 0))
	{
		char line[300];

		snprintf(line, sizeof(line), "water %s: flags %04X, %ld ripples, scale %.2f, levels %d fade %.2f bias %.2f, "
			"contributions %.2f %.2f %.2f %.2f, perpendicular %.2f %.2f %.2f %.2f parallel %.2f %.2f %.2f %.2f",
			tag_get_name(shader_tag_index), water->flags, block->count, (double)water->scale, water->levels,
			(double)water->fade, (double)water->lod_bias, (double)water->ripples[0].contribution,
			(double)water->ripples[1].contribution, (double)water->ripples[2].contribution,
			(double)water->ripples[3].contribution, (double)water->perpendicular[0], (double)water->perpendicular[1],
			(double)water->perpendicular[2], (double)water->perpendicular[3], (double)water->parallel[0],
			(double)water->parallel[1], (double)water->parallel[2], (double)water->parallel[3]);
		system_log(line);
	}
	return water;
}

void dos_water_dispose(void)
{
	for (int i = 0; i < water_count; i++)
	{
		struct dos_water *water = &waters[i];

		for (int r = 0; r < RIPPLES; r++)
			(free)(water->ripples[r].texels);
		for (int l = 0; l < BUMP_LEVELS; l++)
			(free)(water->bump_levels[l]);
		r_hardware_texture_delete(water->bump);
		r_hardware_texture_delete(water->reflection);
		r_hardware_texture_delete(water->tint);
		r_hardware_texture_delete(water->base);
	}
	memset(waters, 0, sizeof(waters));
	water_count = 0;
}

/* a ripple map's signed normal at (u, v), wrapped and filtered */
static void ripple_sample(const struct ripple *ripple, float u, float v, float *n)
{
	float x = u * (float)ripple->width - 0.5f, y = v * (float)ripple->height - 0.5f;
	float fx = floorf(x), fy = floorf(y), ax = x - fx, ay = y - fy;
	int x0 = (int)fx % ripple->width, y0 = (int)fy % ripple->height;
	int x1, y1;
	unsigned long t[4];

	x0 = x0 < 0 ? x0 + ripple->width : x0;
	y0 = y0 < 0 ? y0 + ripple->height : y0;
	x1 = (x0 + 1) % ripple->width;
	y1 = (y0 + 1) % ripple->height;
	t[0] = ripple->texels[y0 * ripple->width + x0];
	t[1] = ripple->texels[y0 * ripple->width + x1];
	t[2] = ripple->texels[y1 * ripple->width + x0];
	t[3] = ripple->texels[y1 * ripple->width + x1];
	for (int c = 0; c < 3; c++)
	{
		int shift = 16 - 8 * c;
		float top = (float)((t[0] >> shift) & 0xFF) * (1.0f - ax) + (float)((t[1] >> shift) & 0xFF) * ax;
		float bottom = (float)((t[2] >> shift) & 0xFF) * (1.0f - ax) + (float)((t[3] >> shift) & 0xFF) * ax;

		n[c] = (top * (1.0f - ay) + bottom * ay) * (2.0f / 255.0f) - 1.0f;
	}
}

static unsigned long encode_normal(float x, float y, float z)
{
	float length = sqrtf(x * x + y * y + z * z);
	unsigned long result = 0xFF000000UL;
	float n[3];

	if (length < 1e-6f)
	{
		x = y = 0.0f;
		z = length = 1.0f;
	}
	n[0] = x / length;
	n[1] = y / length;
	n[2] = z / length;
	for (int c = 0; c < 3; c++)
	{
		float value = (n[c] * 0.5f + 0.5f) * 255.0f + 0.5f;

		result |= (unsigned long)(value < 0.0f ? 0.0f : value > 255.0f ? 255.0f : value) << (16 - 8 * c);
	}
	return result;
}

/* the ripples' normal map at the game's time (Halo's water bump map: two
pairs of layers each mixed by its contributions, the pairs mixed by theirs),
and its smaller levels averaged, faded toward flat */
static void build_bump(struct dos_water *water, float seconds)
{
	const struct ripple *r = water->ripples;
	float a = r[0].contribution / (r[0].contribution + r[1].contribution);
	float b = r[2].contribution / (r[2].contribution + r[3].contribution);
	float c = (r[0].contribution + r[1].contribution) /
		(r[0].contribution + r[1].contribution + r[2].contribution + r[3].contribution);
	float shift[RIPPLES][2];
	unsigned long *level = water->bump_levels[0];

	for (int k = 0; k < RIPPLES; k++)
	{
		shift[k][0] = seconds * r[k].velocity * cosf(r[k].angle) + r[k].offset[0];
		shift[k][1] = seconds * r[k].velocity * sinf(r[k].angle) + r[k].offset[1];
	}
	for (int y = 0; y < BUMP_SIZE; y++)
	{
		float v = ((float)y + 0.5f) / (float)BUMP_SIZE;

		for (int x = 0; x < BUMP_SIZE; x++)
		{
			float u = ((float)x + 0.5f) / (float)BUMP_SIZE, n[RIPPLES][3], out[3];

			for (int k = 0; k < RIPPLES; k++)
			{
				if (r[k].texels)
				{
					ripple_sample(&r[k], (float)r[k].repeats * u + shift[k][0], (float)r[k].repeats * v + shift[k][1], n[k]);
				}
				else
				{
					n[k][0] = n[k][1] = 0.0f;
					n[k][2] = 1.0f;
				}
			}
			/* (as Halo's combiners: the first of each pair weighted by the
			other's share) */
			for (int i = 0; i < 3; i++)
			{
				float first = (1.0f - a) * n[0][i] + a * n[1][i];
				float second = (1.0f - b) * n[2][i] + b * n[3][i];

				out[i] = (1.0f - c) * first + c * second;
			}
			level[y * BUMP_SIZE + x] = encode_normal(out[0], out[1], out[2]);
		}
	}
	for (int l = 1; l < BUMP_LEVELS; l++)
	{
		int size = BUMP_SIZE >> l, above = size * 2;
		/* (Halo's fade: the level's place among the shader's levels) */
		int place = l < water->levels - 1 ? l : water->levels - 1;
		float fade = water->levels > 1 ? (float)place / (float)(water->levels - 1) * water->fade : 0.0f;
		const unsigned long *in = water->bump_levels[l - 1];
		unsigned long *out_level = water->bump_levels[l];

		fade = fade < 0.0f ? 0.0f : fade > 1.0f ? 1.0f : fade;
		for (int y = 0; y < size; y++)
		{
			for (int x = 0; x < size; x++)
			{
				float n[3] = { 0.0f, 0.0f, 0.0f };

				for (int dy = 0; dy < 2; dy++)
				{
					for (int dx = 0; dx < 2; dx++)
					{
						unsigned long t = in[(y * 2 + dy) * above + x * 2 + dx];

						for (int i = 0; i < 3; i++)
							n[i] += (float)((t >> (16 - 8 * i)) & 0xFF) * (2.0f / 255.0f / 4.0f) - 0.25f;
					}
				}
				out_level[y * size + x] = encode_normal(n[0] * (1.0f - fade), n[1] * (1.0f - fade),
					n[2] * (1.0f - fade) + fade);
			}
		}
	}
}

/* each face's levels from its largest, averaged (a level's sides halved) */
static unsigned long **cube_levels(unsigned long *face, int size, int levels)
{
	unsigned long **out = (calloc)((size_t)levels, sizeof(*out));

	if (!out)
		return NULL;
	out[0] = face;
	for (int l = 1; l < levels; l++)
	{
		int half = size >> l, above = half * 2;

		out[l] = (malloc)((size_t)half * half * sizeof(unsigned long));
		if (!out[l])
			break;
		for (int y = 0; y < half; y++)
		{
			for (int x = 0; x < half; x++)
			{
				unsigned long sum[4] = { 0, 0, 0, 0 };

				for (int dy = 0; dy < 2; dy++)
				{
					for (int dx = 0; dx < 2; dx++)
					{
						unsigned long t = out[l - 1][(y * 2 + dy) * above + x * 2 + dx];

						for (int i = 0; i < 4; i++)
							sum[i] += (t >> (24 - 8 * i)) & 0xFF;
					}
				}
				out[l][y * half + x] = ((sum[0] / 4) << 24) | ((sum[1] / 4) << 16) | ((sum[2] / 4) << 8) | (sum[3] / 4);
			}
		}
	}
	return out;
}

/* the card's textures, made once */
static int make(struct dos_water *water)
{
	unsigned long *faces[6] = { NULL };
	unsigned long **face_levels[6] = { NULL };
	int size = 0, levels = 1;

	water->made = TRUE;
	/* the reflection: six faces of one size */
	for (int f = 0; f < 6; f++)
	{
		int width, height;

		faces[f] = dos_bitmap_tag_decode_face(water->reflection_map, f, CUBE_LARGEST, &width, &height);
		if (!faces[f] || width != height || (size && width != size))
		{
			for (int k = 0; k <= f; k++)
				(free)(faces[k]);
			return FALSE;
		}
		size = width;
		for (long i = 0; i < (long)width * height; i++)
			faces[f][i] |= 0xFF000000UL;
	}
	while ((size >> levels) > 0)
		levels++;
	for (int f = 0; f < 6; f++)
		face_levels[f] = cube_levels(faces[f], size, levels);
	water->reflection = r_hardware_cube_new((unsigned long *const **)face_levels, size, levels);
	for (int f = 0; f < 6; f++)
	{
		if (face_levels[f])
		{
			for (int l = 0; l < levels; l++)
				(free)(face_levels[f][l]);
			(free)(face_levels[f]);
		}
		else
		{
			(free)(faces[f]);
		}
	}
	/* the tint by how squarely the water is seen: along it (0) to straight
	down (255); its alpha the reflection's share where the base map's alpha
	modulates it, else whole */
	{
		unsigned long row[TINT_SIZE];
		int alpha_modulates = (water->flags & 0x0001) != 0;
		unsigned long *rows[1] = { row };
		int width = TINT_SIZE, height = 1;

		for (int i = 0; i < TINT_SIZE; i++)
		{
			float t = (float)i / (float)(TINT_SIZE - 1), channel[4];

			for (int k = 0; k < 4; k++)
				channel[k] = water->parallel[k] * (1.0f - t) + water->perpendicular[k] * t;
			if (!alpha_modulates)
				channel[0] = 1.0f;
			row[i] = 0;
			for (int k = 0; k < 4; k++)
			{
				float value = channel[k] * 255.0f + 0.5f;

				row[i] |= (unsigned long)(value < 0.0f ? 0.0f : value > 255.0f ? 255.0f : value) << (24 - 8 * k);
			}
		}
		water->tint = r_hardware_texture_new(rows, &width, &height, 1, TRUE);
	}
	/* the base map */
	if (water->base_map != NONE && (water->flags & 0x0003))
	{
		int width, height;
		unsigned long *texels = dos_bitmap_tag_decode(water->base_map, 0, 512, &width, &height);

		if (texels)
		{
			unsigned long *rows[1] = { texels };

			water->base = r_hardware_texture_new(rows, &width, &height, 1, TRUE);
			(free)(texels);
		}
	}
	/* the ripples' normal map: its levels, the card's texture of them */
	{
		int widths[BUMP_LEVELS], heights[BUMP_LEVELS];

		for (int l = 0; l < BUMP_LEVELS; l++)
		{
			widths[l] = heights[l] = BUMP_SIZE >> l;
			water->bump_levels[l] = (malloc)((size_t)widths[l] * heights[l] * sizeof(unsigned long));
			if (!water->bump_levels[l])
				return FALSE;
		}
		build_bump(water, dos_game_seconds());
		water->frame = dos_frame_number();
		water->bump = r_hardware_texture_new(water->bump_levels, widths, heights, BUMP_LEVELS, FALSE);
	}
	return water->bump && water->reflection && water->tint;
}

void dos_water_draw(const struct dos_material *material, const struct r_vertex *vertices, const float *frames,
	int vertex_count, const unsigned short *indices, int triangle_count, const float *plane_normal)
{
	struct dos_water *water = material->water;
	struct r_water draw;
	float seconds = dos_game_seconds();

	if (!water || water->failed)
		return;
	if (!water->made && !make(water))
		water->failed = TRUE;
	if (water->failed)
		return;
	/* (the ripples moved on: once a frame) */
	if (water->frame != dos_frame_number())
	{
		water->frame = dos_frame_number();
		build_bump(water, seconds);
		for (int l = 0; l < BUMP_LEVELS; l++)
			r_hardware_texture_write(water->bump, l, water->bump_levels[l]);
	}
	memset(&draw, 0, sizeof(draw));
	draw.bump = water->bump;
	draw.bump_scale[0] = draw.bump_scale[1] = water->scale != 0.0f ? water->scale : 1.0f;
	draw.bump_offset[0] = cosf(water->angle) * water->velocity * seconds;
	draw.bump_offset[1] = sinf(water->angle) * water->velocity * seconds;
	draw.bump_lod_bias = water->lod_bias;
	draw.reflection = water->reflection;
	draw.tint = water->tint;
	/* (the whole draw's tint, Halo's own: by the camera's direction against
	the water's plane, straight down the perpendicular) */
	{
		float right[3], up[3], ahead[3], eye[3], amount, tint[3];

		r_view_basis(right, up, ahead, eye);
		amount = -(ahead[0] * plane_normal[0] + ahead[1] * plane_normal[1] + ahead[2] * plane_normal[2]);
		amount = amount < 0.0f ? 0.0f : amount > 1.0f ? 1.0f : amount;
		for (int k = 0; k < 3; k++)
		{
			tint[k] = water->parallel[k + 1] * (1.0f - amount) + water->perpendicular[k + 1] * amount;
			tint[k] = tint[k] < 0.0f ? 0.0f : tint[k] > 1.0f ? 1.0f : tint[k];
		}
		draw.tint_color = ((unsigned long)(tint[0] * 255.0f + 0.5f) << 16) | ((unsigned long)(tint[1] * 255.0f + 0.5f) << 8) |
			(unsigned long)(tint[2] * 255.0f + 0.5f);
	}
	draw.base = water->base;
	draw.base_alpha = (water->flags & 0x0001) != 0;
	draw.base_multiply = (water->flags & 0x0002) != 0;
	/* (testing, setting debug_water=<frame>: what the water drew with) */
	if (dos_setting_integer("debug_water", 0) == dos_frame_number())
	{
		char line[300];
		long flat = 0;

		for (int i = 0; i < BUMP_SIZE * BUMP_SIZE; i++)
			flat += (water->bump_levels[0][i] & 0xFFFFFF) == 0x8080FF;
		snprintf(line, sizeof(line), "water draw: %d triangles, bump %lu (texel 0 %08lX, %ld flat), reflection %lu, "
			"tint %lu %06lX, base %lu, offset %.2f %.2f, first vertex uv %.2f %.2f frame %.2f %.2f %.2f", triangle_count,
			draw.bump, water->bump_levels[0][0], flat, draw.reflection, draw.tint, draw.tint_color, draw.base,
			(double)draw.bump_offset[0], (double)draw.bump_offset[1], (double)vertices[0].u, (double)vertices[0].v,
			(double)frames[0], (double)frames[1], (double)frames[2]);
		system_log(line);
	}
	r_draw_water(&draw, vertices, frames, vertex_count, indices, triangle_count);
}
