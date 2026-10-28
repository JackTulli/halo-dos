/*
DOS_MODELS.C

Models for the DOS and Windows 95 renderer, as Halo's own render_model
(models/models.c) draws them: it chooses the level of detail and each
region's permutation, works out the skinning matrices (each node's matrix
times its default pose's inverse) and hands the rasterizer each part
(rasterizer_model_begin, rasterizer_model_draw, ...): those are here.

A part is decoded the first time it is drawn and kept: the Xbox's
compressed vertices (a position in the default pose, a packed normal,
texture coordinates over 32767, two node indices times 3 and the first's
weight) and its triangle strip (16-bit indices, its odd triangles turned
round, degenerate ones dropped). Each vertex follows its dominant node, as
on the N64 (tools/n64/models.py): one matrix, not a blend of two.

Each draw then puts the part's vertices in the world, all of one colour:
the object's light (the level's baked light under it, dos_render_game.c),
times its armour's colour where the shader takes it. On the Xbox's look
(Direct3D 8, dos_object_light) each vertex is lit instead, as the Xbox's
model vertex shaders light it: the object's ambient light plus each of its
two distant lights by how squarely the vertex's normal (Halo's packed one,
kept with the part, turned by its node) faces it; the distant copies are
not drawn then (they have no normals), and VIEW MODEL's lighter copies keep
their normals.
*/

#include "cseries.h"

#include "math/real_math.h"
#include "models/model_definitions.h"
#include "rasterizer/rasterizer_geometry.h"
#include "rasterizer/rasterizer_model_types.h"
#include "rasterizer/rasterizer_models.h"
#include "render/render.h"
#include "shaders/shader_definitions.h"
#include "tag_files/tag_groups.h"

#include "halo_system.h"
#include "dos_render.h"

extern struct r_settings dos_render_settings;

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *raw_malloc(size_t size) { return (malloc)(size); }
static void raw_free(void *pointer) { (free)(pointer); }

#define MODEL_VERTEX_SIZE 32
#define MAXIMUM_PART_VERTICES 8192

/* models.c's own (it is defined there, not in a header) */
struct render_sort_filth
{
	short *previous_group_presorted_index_reference;
	short *next_group_presorted_index_reference;
	short group_index;
	short next_part_index;
	short part_index;
	word pad;
};

/* ---------- parts */

/* a part's geometry: its own, or the coarse copy of it */
struct geometry
{
	int vertex_count, triangle_count;
	float *positions; /* x, y, z in the default pose */
	float *uvs;
	unsigned char *nodes;
	unsigned short *indices;
	/* the normals (x, y, z in the default pose), or NULL: lit as a whole */
	float *normals;
};

struct part
{
	const void *key;
	int vertex_count, triangle_count;
	float *positions; /* x, y, z in the default pose */
	float *uvs;
	unsigned char *nodes;
	unsigned short *indices;
	float *normals;
	int failed;
	/* the coarse copy drawn where the model is small on the screen
	(coarse_part): made the first time it is wanted; none where it would
	save too little */
	struct geometry coarse;
	int coarse_made;
	/* a first-person part's lighter copies (VIEW MODEL VERY LOW, LOW and
	MEDIUM) */
	struct geometry view[3];
	int view_made[3];
};

#define PART_SLOTS 2048

static struct part parts[PART_SLOTS];
static int part_count;

/* the D3D vertex buffer header the part's vertex buffer names: its data (the
buffer's own base_address is the tools' leftover, an address in no map) */
static const byte *vertex_data(struct vertex_buffer const *vertex_buffer)
{
	const unsigned long *header = (const unsigned long *)xbox_pointer(vertex_buffer->hardware_format);

	return header ? (const byte *)xbox_pointer(header[1]) : NULL;
}

/* Halo's packed vector: 11, 11 and 10 signed bits */
static void unpack_normal(unsigned long value, float *n)
{
	long x = (long)(value & 0x7FF), y = (long)((value >> 11) & 0x7FF), z = (long)((value >> 22) & 0x3FF);

	if (x & 0x400) x -= 0x800;
	if (y & 0x400) y -= 0x800;
	if (z & 0x200) z -= 0x400;
	n[0] = (float)x / 1023.0f;
	n[1] = (float)y / 1023.0f;
	n[2] = (float)z / 511.0f;
}

static struct part *part_get(struct triangle_buffer const *triangle_buffer, struct vertex_buffer const *vertex_buffer,
	long triangle_count)
{
	const byte *data = vertex_data(vertex_buffer);
	const unsigned short *strip = (const unsigned short *)xbox_pointer(triangle_buffer->base_address);
	long vertex_count = vertex_buffer->count;
	struct part *part;
	long strip_length;

	for (int i = 0; i < part_count; i++)
	{
		if (parts[i].key == vertex_buffer)
			return parts[i].failed ? NULL : &parts[i];
	}
	if (part_count == PART_SLOTS)
		return NULL;
	part = &parts[part_count++];
	memset(part, 0, sizeof(*part));
	part->key = vertex_buffer;
	part->failed = TRUE;
	if (!data || !strip || vertex_count <= 0 || vertex_count > MAXIMUM_PART_VERTICES)
		return NULL;
	/* a strip of count + 2 indices (_triangle_buffer_type_precompiled_strip),
	or a list of 3 a triangle */
	strip_length = triangle_buffer->type == 0 ? triangle_count * 3 : triangle_count + 2;
	part->positions = raw_malloc((size_t)vertex_count * 3 * sizeof(float));
	part->uvs = raw_malloc((size_t)vertex_count * 2 * sizeof(float));
	part->nodes = raw_malloc((size_t)vertex_count);
	part->indices = raw_malloc((size_t)strip_length * 3 * sizeof(unsigned short));
	/* (the normals for the Xbox's look alone: DOS's memory is short) */
	part->normals = dos_object_light() ? raw_malloc((size_t)vertex_count * 3 * sizeof(float)) : NULL;
	if (!part->positions || !part->uvs || !part->nodes || !part->indices)
	{
		raw_free(part->positions);
		raw_free(part->uvs);
		raw_free(part->nodes);
		raw_free(part->indices);
		raw_free(part->normals);
		return NULL;
	}
	part->vertex_count = (int)vertex_count;
	for (long v = 0; v < vertex_count; v++)
	{
		const byte *vertex = data + v * MODEL_VERTEX_SIZE;
		short tu = *(const short *)(vertex + 24), tv = *(const short *)(vertex + 26);
		signed char n0 = *(const signed char *)(vertex + 28), n1 = *(const signed char *)(vertex + 29);
		unsigned short w = *(const unsigned short *)(vertex + 30);
		float weight = (float)(w & 0x7FFF) / 32767.0f - ((w & 0x8000) ? 1.0f : 0.0f);
		int node0 = n0 >= 0 ? n0 / 3 : -1, node1 = n1 >= 0 ? n1 / 3 : -1;
		int node = (node1 < 0 || (node0 >= 0 && weight >= 0.5f)) ? node0 : node1;

		memcpy(&part->positions[v * 3], vertex, 12);
		if (part->normals)
			unpack_normal(*(const unsigned long *)(vertex + 12), &part->normals[v * 3]);
		part->uvs[v * 2 + 0] = (float)tu / 32767.0f;
		part->uvs[v * 2 + 1] = (float)tv / 32767.0f;
		part->nodes[v] = (unsigned char)(node >= 0 ? node : 0);
	}
	if (triangle_buffer->type == 0)
	{
		for (long t = 0; t < triangle_count; t++)
		{
			unsigned short a = strip[t * 3], b = strip[t * 3 + 1], c = strip[t * 3 + 2];

			if (a >= vertex_count || b >= vertex_count || c >= vertex_count)
				continue;
			part->indices[part->triangle_count * 3 + 0] = a;
			part->indices[part->triangle_count * 3 + 1] = b;
			part->indices[part->triangle_count * 3 + 2] = c;
			part->triangle_count++;
		}
	}
	else
	{
		/* each wound as the strip's first: the odd ones turned round */
		for (long i = 0; i + 2 < strip_length; i++)
		{
			unsigned short a = strip[i], b = strip[i + 1], c = strip[i + 2];

			if (a == b || b == c || a == c || a >= vertex_count || b >= vertex_count || c >= vertex_count)
				continue;
			if (i & 1)
			{
				unsigned short k = a;

				a = b;
				b = k;
			}
			part->indices[part->triangle_count * 3 + 0] = a;
			part->indices[part->triangle_count * 3 + 1] = b;
			part->indices[part->triangle_count * 3 + 2] = c;
			part->triangle_count++;
		}
	}
	part->failed = FALSE;
	return part;
}

/* ---------- coarse copies

A model far enough to be a few dozen pixels across draws hundreds of
triangles for them (c10's swamp trees: 536 each, 84 of them in a view).
Its parts' coarse copies, as the level's distant terrain (dos_level.c): the
vertices clustered on a grid over the part, COARSE_CELLS cells along its
longest side, one vertex a cell and a node (a cell's vertices of two nodes
stay apart, as they move apart), the triangles that keep three corners
kept. Drawn under the DETAIL setting's size (dos_coarse_model_pixels); a
copy that saves under a third is not made. */

#define COARSE_CELLS 8

real dos_model_pixels;

static void geometry_free(struct geometry *geometry)
{
	raw_free(geometry->positions);
	raw_free(geometry->uvs);
	raw_free(geometry->nodes);
	raw_free(geometry->indices);
	raw_free(geometry->normals);
	memset(geometry, 0, sizeof(*geometry));
}

/* the part's vertices clustered: cells cells along its longest side. FALSE
if none could be made */
static int cluster(const struct part *part, int cells, struct geometry *coarse)
{
	float low[3], high[3], extent = 0.0f, cell;
	int size = 1, *slots = NULL, *map = NULL;

	memset(coarse, 0, sizeof(*coarse));
	for (int k = 0; k < 3; k++)
		low[k] = high[k] = part->positions[k];
	for (int v = 1; v < part->vertex_count; v++)
	{
		for (int k = 0; k < 3; k++)
		{
			float x = part->positions[v * 3 + k];

			if (x < low[k]) low[k] = x;
			if (x > high[k]) high[k] = x;
		}
	}
	for (int k = 0; k < 3; k++)
		extent = high[k] - low[k] > extent ? high[k] - low[k] : extent;
	if (extent <= 0.0f)
		return FALSE;
	cell = extent / (float)cells;
	while (size < part->vertex_count * 2)
		size <<= 1;
	slots = raw_malloc((size_t)size * sizeof(int));
	map = raw_malloc((size_t)part->vertex_count * sizeof(int));
	coarse->positions = raw_malloc((size_t)part->vertex_count * 3 * sizeof(float));
	coarse->uvs = raw_malloc((size_t)part->vertex_count * 2 * sizeof(float));
	coarse->nodes = raw_malloc((size_t)part->vertex_count);
	coarse->indices = raw_malloc((size_t)part->triangle_count * 3 * sizeof(unsigned short));
	if (!slots || !map || !coarse->positions || !coarse->uvs || !coarse->nodes || !coarse->indices)
		goto none;
	for (int i = 0; i < size; i++)
		slots[i] = -1;
	/* each vertex's cell (and node): the cell's first vertex stands for it */
	for (int v = 0; v < part->vertex_count; v++)
	{
		unsigned long x = (unsigned long)((part->positions[v * 3] - low[0]) / cell);
		unsigned long y = (unsigned long)((part->positions[v * 3 + 1] - low[1]) / cell);
		unsigned long z = (unsigned long)((part->positions[v * 3 + 2] - low[2]) / cell);
		unsigned long key = ((x * 31 + y) * 31 + z) * 131 + part->nodes[v];
		unsigned long slot = (key * 2654435761UL) & (unsigned long)(size - 1);

		for (;;)
		{
			int other = slots[slot];

			if (other < 0)
			{
				int n = coarse->vertex_count++;

				slots[slot] = v;
				memcpy(&coarse->positions[n * 3], &part->positions[v * 3], 3 * sizeof(float));
				memcpy(&coarse->uvs[n * 2], &part->uvs[v * 2], 2 * sizeof(float));
				coarse->nodes[n] = part->nodes[v];
				map[v] = n;
				break;
			}
			if (part->nodes[other] == part->nodes[v] &&
				(unsigned long)((part->positions[other * 3] - low[0]) / cell) == x &&
				(unsigned long)((part->positions[other * 3 + 1] - low[1]) / cell) == y &&
				(unsigned long)((part->positions[other * 3 + 2] - low[2]) / cell) == z)
			{
				map[v] = map[other];
				break;
			}
			slot = (slot + 1) & (unsigned long)(size - 1);
		}
	}
	for (int t = 0; t < part->triangle_count; t++)
	{
		int a = map[part->indices[t * 3]], b = map[part->indices[t * 3 + 1]], c = map[part->indices[t * 3 + 2]];

		if (a == b || b == c || a == c)
			continue;
		coarse->indices[coarse->triangle_count * 3 + 0] = (unsigned short)a;
		coarse->indices[coarse->triangle_count * 3 + 1] = (unsigned short)b;
		coarse->indices[coarse->triangle_count * 3 + 2] = (unsigned short)c;
		coarse->triangle_count++;
	}
	if (coarse->triangle_count > 0)
	{
		raw_free(slots);
		raw_free(map);
		return TRUE;
	}
none:
	raw_free(slots);
	raw_free(map);
	geometry_free(coarse);
	return FALSE;
}

/* the distant copy: only where it saves a third at least */
static void make_coarse(struct part *part)
{
	part->coarse_made = TRUE;
	if (part->triangle_count < 24 || !cluster(part, COARSE_CELLS, &part->coarse))
		return;
	if (part->coarse.triangle_count * 3 > part->triangle_count * 2)
		geometry_free(&part->coarse);
}

/* VIEW MODEL (the N64 port's choices, and one lower): the first-person
weapon and hands with fewer triangles, as the N64's had a triangle budget
for each quality: VERY LOW about a third of a part's triangles, LOW about
half, MEDIUM about seven tenths; HIGH the part itself, and OFF draws nothing
(dos_render_game.c). The setting view_model: 4 very low, 0 low, 1 medium,
2 high, 3 off (the numbers of the settings written before VERY LOW). */
int dos_view_model_quality(void)
{
	extern long dos_setting_integer(const char *name, long default_value);
	extern unsigned long dos_settings_generation(void);
	static unsigned long generation = (unsigned long)-1;
	static int quality;

	if (generation != dos_settings_generation())
	{
		generation = dos_settings_generation();
		quality = PIN((int)dos_setting_integer("view_model", 2), 0, 4);
	}
	return quality;
}

/* The weapon is seen up close, where a distant model's clustering shows (a
seam's two sides pulled apart into cracks, triangles turned over and culled
into holes): its copies are made by collapsing edges (dos_simplify.c), the
first time a part is drawn so. */

/* a geometry of the part's own vertices that the kept triangles use */
static int geometry_from_kept(const struct part *part, const unsigned short *kept, int count, struct geometry *out)
{
	int *slot = raw_malloc((size_t)part->vertex_count * sizeof(int));

	memset(out, 0, sizeof(*out));
	out->positions = raw_malloc((size_t)part->vertex_count * 3 * sizeof(float));
	out->uvs = raw_malloc((size_t)part->vertex_count * 2 * sizeof(float));
	out->nodes = raw_malloc((size_t)part->vertex_count);
	out->indices = raw_malloc((size_t)count * 3 * sizeof(unsigned short));
	if (part->normals)
		out->normals = raw_malloc((size_t)part->vertex_count * 3 * sizeof(float));
	if (!slot || !out->positions || !out->uvs || !out->nodes || !out->indices || (part->normals && !out->normals))
	{
		raw_free(slot);
		geometry_free(out);
		return FALSE;
	}
	for (int v = 0; v < part->vertex_count; v++)
		slot[v] = -1;
	for (int i = 0; i < count * 3; i++)
	{
		int v = kept[i];

		if (slot[v] < 0)
		{
			int n = out->vertex_count++;

			slot[v] = n;
			memcpy(&out->positions[n * 3], &part->positions[v * 3], 3 * sizeof(float));
			memcpy(&out->uvs[n * 2], &part->uvs[v * 2], 2 * sizeof(float));
			if (out->normals)
				memcpy(&out->normals[n * 3], &part->normals[v * 3], 3 * sizeof(float));
			out->nodes[n] = part->nodes[v];
		}
		out->indices[i] = (unsigned short)slot[v];
	}
	out->triangle_count = count;
	raw_free(slot);
	return TRUE;
}

/* the copy for a quality (4 very low, 0 low, 1 medium), NULL to draw the
part itself */
static const struct geometry *view_model_copy(struct part *part, int quality)
{
	static const int targets[3] = { 35, 50, 70 };
	int which = quality == 4 ? 0 : quality == 0 ? 1 : 2;
	struct geometry *copy = &part->view[which];

	if (!part->view_made[which])
	{
		part->view_made[which] = TRUE;
		if (part->triangle_count >= 24)
		{
			unsigned long long start = system_microseconds();
			char line[96];
			unsigned short *kept = raw_malloc((size_t)part->triangle_count * 3 * sizeof(unsigned short));
			int count = kept ? dos_simplify(part->positions, 3, part->nodes, part->vertex_count, part->indices,
				part->triangle_count, part->triangle_count * targets[which] / 100, 1e30, kept) : -1;

			if (count > 0 && count < part->triangle_count)
				geometry_from_kept(part, kept, count, copy);
			raw_free(kept);
			snprintf(line, sizeof(line), "view model: a part of %d triangles has %d at %d%%, made in %lu ms",
				part->triangle_count, copy->triangle_count ? copy->triangle_count : part->triangle_count,
				targets[which], (unsigned long)((system_microseconds() - start) / 1000));
			system_log(line);
		}
	}
	return copy->triangle_count ? copy : NULL;
}

void dos_models_dispose_from_old_map(void)
{
	for (int i = 0; i < part_count; i++)
	{
		raw_free(parts[i].positions);
		raw_free(parts[i].uvs);
		raw_free(parts[i].nodes);
		raw_free(parts[i].indices);
		raw_free(parts[i].normals);
		geometry_free(&parts[i].coarse);
		geometry_free(&parts[i].view[0]);
		geometry_free(&parts[i].view[1]);
		geometry_free(&parts[i].view[2]);
	}
	part_count = 0;
}

/* ---------- drawing */

static struct
{
	int active;
	real_matrix4x3 const *node_matrices;
	short node_count;
	/* the object's light (0x00RRGGBB, 0x80 as the texture is) */
	unsigned long color;
	real_vector2d base_map_scale;
	int first_person;
	/* the object's armour colour, for shaders that take it (dos_render_game.c
	sets it before render_model) */
	unsigned long change_color;
	/* each node's matrix and the view's in one (rows right, up, ahead: the
	node's axes and position in the view's space), made at the model's
	first part: its vertices go straight into the view's space */
	int view_matrices_made;
	/* lit at each vertex (the Xbox's look): the object's ambient light and
	its distant lights' colours and directions (the world's, then the
	view's with the view matrices), 1 the texture as it is */
	int lit;
	float ambient[3];
	int light_count;
	float light_colors[2][3];
	float light_directions[2][3];
	float view_light_directions[2][3];
} model;

#define MAXIMUM_VIEW_NODES 128
static float view_matrices[MAXIMUM_VIEW_NODES][12];

static void make_view_matrices(void)
{
	float right[3], up[3], ahead[3], eye[3];
	const float *axes[3] = { right, up, ahead };
	int count = model.node_count < MAXIMUM_VIEW_NODES ? model.node_count : MAXIMUM_VIEW_NODES;

	r_view_basis(right, up, ahead, eye);
	for (int n = 0; n < count; n++)
	{
		real_matrix4x3 const *m = &model.node_matrices[n];
		float *out = view_matrices[n];
		float t[3] = { m->position.x - eye[0], m->position.y - eye[1], m->position.z - eye[2] };

		for (int r = 0; r < 3; r++)
		{
			const float *a = axes[r];

			out[r * 4 + 0] = m->scale * (m->forward.i * a[0] + m->forward.j * a[1] + m->forward.k * a[2]);
			out[r * 4 + 1] = m->scale * (m->left.i * a[0] + m->left.j * a[1] + m->left.k * a[2]);
			out[r * 4 + 2] = m->scale * (m->up.i * a[0] + m->up.j * a[1] + m->up.k * a[2]);
			out[r * 4 + 3] = t[0] * a[0] + t[1] * a[1] + t[2] * a[2];
		}
	}
	for (int i = 0; i < model.light_count; i++)
	{
		const float *d = model.light_directions[i];

		for (int r = 0; r < 3; r++)
			model.view_light_directions[i][r] = d[0] * axes[r][0] + d[1] * axes[r][1] + d[2] * axes[r][2];
	}
	model.view_matrices_made = TRUE;
}

static struct r_vertex *world_vertices;
static int world_vertex_size;

void dos_models_first_person(int first_person)
{
	model.first_person = first_person;
}

/* which parts draw_part draws: all, the opaque ones only (it notes whether
it left any transparent ones out), or the transparent ones only */
static int model_pass;
static int transparent_left_out;

void dos_models_pass(int pass)
{
	model_pass = pass;
	if (pass == DOS_MODELS_OPAQUE)
		transparent_left_out = FALSE;
}

int dos_models_transparent_left_out(void)
{
	return transparent_left_out;
}

void dos_models_set_change_color(unsigned long color)
{
	model.change_color = color;
}

void rasterizer_models_begin(void) { return; }

/* (testing, setting debug_parts=<n>: each part frame n draws, its shader
and triangles, logged) */
static int logging_parts(void)
{
	extern long dos_setting_integer(const char *name, long default_value);
	extern long dos_frame_number(void);
	long wanted = dos_setting_integer("debug_parts", 0);

	return wanted && dos_frame_number() == wanted;
}
void rasterizer_models_end(void) { return; }

void rasterizer_model_begin(
	struct rasterizer_model_begin_parameters const *parameters,
	boolean is_dynamic)
{
	(void)is_dynamic;
	model.active = TRUE;
	model.node_matrices = parameters->skinning.node_matrices;
	model.node_count = parameters->skinning.node_matrix_count;
	model.view_matrices_made = FALSE;
	{
		/* (the ambient: a lighting with distant lights, which this build's
		objects do not have, gets their mean over a half sphere added) */
		const struct render_lighting *lighting = &parameters->lighting;
		real c[3] = { lighting->ambient_color.red, lighting->ambient_color.green, lighting->ambient_color.blue };

		for (int i = 0; i < lighting->distant_light_count && i < 2; i++)
		{
			c[0] += lighting->distant_lights[i].color.red * 0.5f;
			c[1] += lighting->distant_lights[i].color.green * 0.5f;
			c[2] += lighting->distant_lights[i].color.blue * 0.5f;
		}
		model.color = ((unsigned long)PIN(c[0] * 255.0f, 0.0f, 255.0f) << 16) |
			((unsigned long)PIN(c[1] * 255.0f, 0.0f, 255.0f) << 8) | (unsigned long)PIN(c[2] * 255.0f, 0.0f, 255.0f);
		/* (the Xbox's look: each vertex lit by these, dos_object_light --
		Halo's light has its distant lights; a fixed one without them, the
		sky's, the unlit 0x80, stays as it is) */
		model.lit = dos_object_light() && lighting->distant_light_count > 0;
		model.ambient[0] = lighting->ambient_color.red;
		model.ambient[1] = lighting->ambient_color.green;
		model.ambient[2] = lighting->ambient_color.blue;
		model.light_count = lighting->distant_light_count < 2 ? lighting->distant_light_count : 2;
		for (int i = 0; i < model.light_count; i++)
		{
			model.light_colors[i][0] = lighting->distant_lights[i].color.red;
			model.light_colors[i][1] = lighting->distant_lights[i].color.green;
			model.light_colors[i][2] = lighting->distant_lights[i].color.blue;
			model.light_directions[i][0] = lighting->distant_lights[i].direction.i;
			model.light_directions[i][1] = lighting->distant_lights[i].direction.j;
			model.light_directions[i][2] = lighting->distant_lights[i].direction.k;
		}
	}
	model.base_map_scale = parameters->base_map_scale;
	if (model.base_map_scale.i == 0.0f)
		model.base_map_scale.i = 1.0f;
	if (model.base_map_scale.j == 0.0f)
		model.base_map_scale.j = 1.0f;
}

void rasterizer_model_end(void)
{
	model.active = FALSE;
	model.change_color = 0;
}

static void draw_geometry(const struct geometry *part, long shader_index, const struct dos_material *material,
	int transparent_pass);

/* a vertex's light, 0 up (its channel times the armour's), to the integer
below as a cast takes it, without the cast's two loads of the x87's
control word (each a stall on the Pentiums): rounded as the control word
has it (to nearest), one less where that came out above */
static __inline__ unsigned long channel_byte(float value)
{
	long result;

	__asm__(
		"fistl %0\n\t"
		"fildl %0\n\t"
		"fcompp\n\t"
		"fnstsw %%ax\n\t"
		"sahf\n\t"
		"jbe 1f\n\t"
		"decl %0\n"
		"1:"
		: "=m"(result) : "t"(value) : "st", "eax", "cc");
	return (unsigned long)result;
}

static void draw_part(struct shader *shader, struct triangle_buffer const *triangle_buffer, long triangle_count,
	struct vertex_buffer const *vertex_buffer, int transparent_pass)
{
	long shader_index = NONE;
	const struct dos_material *material;
	struct part *part;

	if (!model.active || !shader || !model.node_matrices)
		return;
	/* (the shader's tag, from its definition's address) */
	{
		extern long dos_shader_tag_index(struct shader const *shader);

		shader_index = dos_shader_tag_index(shader);
	}
	material = dos_shader_material(shader_index);
	if (material->skip)
		return;
	if (!material->texture)
	{
		/* (testing, setting debug_untextured=1: which shaders draw bare) */
		extern long dos_setting_integer(const char *name, long default_value);
		static long logged[32];
		static int logged_count;
		int seen = FALSE;

		for (int i = 0; i < logged_count; i++)
			seen |= logged[i] == shader_index;
		if (!seen && logged_count < 32 && dos_setting_integer("debug_untextured", 0))
		{
			char line[160];

			logged[logged_count++] = shader_index;
			snprintf(line, sizeof(line), "model shader without a texture: %s",
				shader_index != NONE ? tag_get_name(shader_index) : "none");
			system_log(line);
		}
	}
	if (model_pass == DOS_MODELS_OPAQUE && material->transparent)
	{
		transparent_left_out = TRUE;
		return;
	}
	if (model_pass == DOS_MODELS_TRANSPARENT && !material->transparent)
		return;
	part = part_get(triangle_buffer, vertex_buffer, triangle_count);
	if (!part || !part->triangle_count)
		return;
	{
		/* (small on the screen: its coarse copy, where it has one) */
		extern real dos_coarse_model_pixels(void);
		struct geometry geometry = { part->vertex_count, part->triangle_count, part->positions, part->uvs,
			part->nodes, part->indices, part->normals };

		/* (lit at each vertex: the distant copy has no normals, and the card
		draws the part whole) */
		if (!model.first_person && !(model.lit && part->normals) && dos_model_pixels < dos_coarse_model_pixels())
		{
			if (!part->coarse_made)
				make_coarse(part);
			if (part->coarse.triangle_count)
				geometry = part->coarse;
		}
		if (model.first_person && dos_view_model_quality() != 2 && dos_view_model_quality() != 3)
		{
			const struct geometry *copy = view_model_copy(part, dos_view_model_quality());

			if (copy)
				geometry = *copy;
		}
		draw_geometry(&geometry, shader_index, material, transparent_pass);
	}
}

/* a part's geometry put in the view and drawn */
static void draw_geometry(const struct geometry *part, long shader_index, const struct dos_material *material,
	int transparent_pass)
{
	float u_scale, v_scale;
	unsigned long color;
	r_texture *texture;
	int lit;
	float change[3];

	(void)transparent_pass;
	/* (testing, setting debug_hide_part=<n>: the n-th part a frame draws
	left out) */
	{
		extern long dos_setting_integer(const char *name, long default_value);
		extern long dos_frame_number(void);
		static long counted_frame = -1;
		static long counted;
		long hide = dos_setting_integer("debug_hide_part", 0);

		if (hide)
		{
			if (counted_frame != dos_frame_number())
			{
				counted_frame = dos_frame_number();
				counted = 0;
			}
			if (++counted == hide)
				return;
		}
	}
	if (part->vertex_count > world_vertex_size)
	{
		struct r_vertex *grown = realloc(world_vertices, (size_t)part->vertex_count * sizeof(*grown));

		if (!grown)
			return;
		world_vertices = grown;
		world_vertex_size = part->vertex_count;
	}
	u_scale = model.base_map_scale.i * material->u_scale;
	v_scale = model.base_map_scale.j * material->v_scale;
	color = model.color;
	texture = material->texture;
	if (material->change_color && model.change_color && dos_render_settings.bits == 8)
	{
		/* (the 8-bit picture's light is monochrome: a tinted copy) */
		texture = dos_material_tinted(material, model.change_color);
	}
	else if (material->change_color && model.change_color)
	{
		unsigned long change = model.change_color;

		color = ((((color >> 16) & 0xFF) * ((change >> 16) & 0xFF) / 255) << 16) |
			((((color >> 8) & 0xFF) * ((change >> 8) & 0xFF) / 255) << 8) | ((color & 0xFF) * (change & 0xFF) / 255);
	}
	/* (lit at each vertex: the Xbox's look, where the part has its normals;
	the armour's colour times each vertex's light, 128 its 1) */
	lit = model.lit && part->normals;
	change[0] = change[1] = change[2] = 128.0f;
	if (lit && material->change_color && model.change_color && dos_render_settings.bits != 8)
	{
		change[0] = (float)((model.change_color >> 16) & 0xFF) * (128.0f / 255.0f);
		change[1] = (float)((model.change_color >> 8) & 0xFF) * (128.0f / 255.0f);
		change[2] = (float)(model.change_color & 0xFF) * (128.0f / 255.0f);
	}
	/* skinned straight into the view's space (one transform a vertex) */
	if (!model.view_matrices_made)
		make_view_matrices();
	for (int v = 0; v < part->vertex_count; v++)
	{
		int node = part->nodes[v] < model.node_count && part->nodes[v] < MAXIMUM_VIEW_NODES ? part->nodes[v] : 0;
		const float *m = view_matrices[node];
		const float *p = &part->positions[v * 3];
		struct r_vertex *out = &world_vertices[v];

		out->position[0] = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3];
		out->position[1] = m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7];
		out->position[2] = m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11];
		out->u = part->uvs[v * 2] * u_scale;
		out->v = part->uvs[v * 2 + 1] * v_scale;
		out->color = color;
		if (lit)
		{
			/* the normal in the view's space (the node's turn; its scale
			divided out), then the ambient and the distant lights it faces */
			const float *n = &part->normals[v * 3];
			float x = m[0] * n[0] + m[1] * n[1] + m[2] * n[2];
			float y = m[4] * n[0] + m[5] * n[1] + m[6] * n[2];
			float z = m[8] * n[0] + m[9] * n[1] + m[10] * n[2];
			float length = sqrtf(x * x + y * y + z * z), c[3];

			if (length > 1e-6f)
			{
				/* (one division: the x87 works to 53 bits, so its inverse
				times each is the quotient, but where a float rounds) */
				double inverse = 1.0 / (double)length;

				x = (float)(x * inverse);
				y = (float)(y * inverse);
				z = (float)(z * inverse);
			}
			c[0] = model.ambient[0];
			c[1] = model.ambient[1];
			c[2] = model.ambient[2];
			for (int i = 0; i < model.light_count; i++)
			{
				const float *d = model.view_light_directions[i];
				float facing = -(x * d[0] + y * d[1] + z * d[2]);

				/* (the first light also reaches the far side by the
				shader's translucency, as Halo's model program) */
				if (facing < 0.0f && i == 0)
					facing *= -material->translucency;
				if (facing > 0.0f)
				{
					c[0] += model.light_colors[i][0] * facing;
					c[1] += model.light_colors[i][1] * facing;
					c[2] += model.light_colors[i][2] * facing;
				}
			}
			/* (at most 1, the texture as it is: 0x80, as the Xbox's colour
			register holds it; then times the armour's colour) */
			out->color = (channel_byte(PIN(c[0], 0.0f, 1.0f) * change[0]) << 16) |
				(channel_byte(PIN(c[1], 0.0f, 1.0f) * change[1]) << 8) |
				channel_byte(PIN(c[2], 0.0f, 1.0f) * change[2]);
		}
	}
	/* (a chicago shader's stack on the card's stages instead: Direct3D 8) */
	if (material->xbox_detail.replace && material->xbox_detail.stage_count)
		r_set_detail_pass(&material->xbox_detail);
	r_draw_triangles(world_vertices, part->vertex_count, part->indices, part->triangle_count, NULL, texture,
		material->flags | R_DRAW_VIEW_SPACE | (model.first_person ? R_DRAW_NO_FOG : 0));
	/* its layer added at its own tiling (a space sky's stars), then its
	detail map's grain over it (dos_detail_maps: OpenGL; not where alpha
	tested, whose holes the pass would cover) */
	if (material->layer)
	{
		for (int v = 0; v < part->vertex_count; v++)
		{
			world_vertices[v].u = part->uvs[v * 2] * u_scale * material->layer_scale[0];
			world_vertices[v].v = part->uvs[v * 2 + 1] * v_scale * material->layer_scale[1];
			world_vertices[v].color = material->layer_tint;
		}
		r_draw_triangles(world_vertices, part->vertex_count, part->indices, part->triangle_count, NULL,
			material->layer, (material->flags & ~(unsigned)(R_DRAW_BLEND | R_DRAW_ALPHA)) | R_DRAW_ADDITIVE |
			R_DRAW_VIEW_SPACE | (model.first_person ? R_DRAW_NO_FOG : 0));
	}
	if (material->detail[0] && !material->transparent && !(material->texture_flags & R_TEXTURE_ALPHA_TEST))
	{
		for (int v = 0; v < part->vertex_count; v++)
		{
			world_vertices[v].u = part->uvs[v * 2] * u_scale * material->detail_scale[0][0];
			world_vertices[v].v = part->uvs[v * 2 + 1] * v_scale * material->detail_scale[0][1];
			world_vertices[v].color = 0xFFFFFF;
		}
		r_draw_triangles(world_vertices, part->vertex_count, part->indices, part->triangle_count, NULL,
			material->detail[0], R_DRAW_DETAIL | (material->flags & R_DRAW_TWO_SIDED) | R_DRAW_VIEW_SPACE |
			(model.first_person ? R_DRAW_NO_FOG : 0));
	}
	if (logging_parts())
	{
		char line[160];

		snprintf(line, sizeof(line), "part: %d triangles, %d vertices, %s", part->triangle_count, part->vertex_count,
			shader_index != NONE ? tag_get_name(shader_index) : "none");
		system_log(line);
	}
}

void rasterizer_model_draw(
	struct shader *shader,
	short shader_permutation_index,
	struct triangle_buffer const *triangle_buffer,
	long dynamic_triangle_buffer_index,
	long triangle_count,
	struct vertex_buffer const *vertex_buffer,
	long dynamic_vertex_buffer_index)
{
	(void)shader_permutation_index;
	(void)dynamic_triangle_buffer_index;
	(void)dynamic_vertex_buffer_index;
	draw_part(shader, triangle_buffer, triangle_count, vertex_buffer, FALSE);
}

void rasterizer_model_transparent_geometry_submit(
	struct shader *shader,
	short shader_permutation_index,
	struct triangle_buffer const *triangle_buffer,
	long dynamic_triangle_buffer_index,
	long triangle_count,
	struct vertex_buffer const *vertex_buffer,
	long dynamic_vertex_buffer_index,
	union real_point3d const *centroid,
	struct render_sort_filth *sort_filth)
{
	(void)shader_permutation_index;
	(void)dynamic_triangle_buffer_index;
	(void)dynamic_vertex_buffer_index;
	(void)centroid;
	/* (drawn now, unsorted: no group for models.c to link) */
	if (sort_filth)
		sort_filth->group_index = NONE;
	draw_part(shader, triangle_buffer, triangle_count, vertex_buffer, TRUE);
}

void rasterizer_environment_shadow_model_begin(struct rasterizer_model_begin_parameters const *parameters)
{
	(void)parameters;
}

void rasterizer_environment_shadow_model_end(void) { return; }

void rasterizer_environment_shadow_model_draw(struct shader const *shader, short shader_permutation_index,
	struct triangle_buffer const *triangle_buffer, struct vertex_buffer const *vertex_buffer)
{
	(void)shader;
	(void)shader_permutation_index;
	(void)triangle_buffer;
	(void)vertex_buffer;
}

void rasterizer_debug_model_vertices(long object_index, struct render_skinning const *skinning,
	struct model_geometry_part const *part)
{
	(void)object_index;
	(void)skinning;
	(void)part;
}
