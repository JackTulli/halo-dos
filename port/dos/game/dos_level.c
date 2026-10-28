/*
DOS_LEVEL.C

The level (the structure BSP's render geometry) for the DOS and Windows 95
renderer, made when a map loads:

- each lightmap material's vertices decoded (the Xbox's compressed ones: a
  position, three packed vectors and texture coordinates, then the lightmap
  vertices' packed normal and coordinates over 32767), each lit by its
  lightmap page around its lightmap coordinates (the texels within a texel
  and a half, without the yellow and red that fill the pages' unused
  texels; tools/n64/level.py does the same), or by the material's lighting
  summary where there is no page;
- the triangles (the BSP's surfaces, clockwise front faces, as the
  renderer takes them) batched by subcluster and material: each batch its
  own vertices, so that drawing it transforms only those, and its sphere
  for culling.

On Direct3D 8 (r_draws_lightmaps) with the setting d3d_lightmaps (the
default there) the level is lit as the Xbox lights it instead: each
lightmap page goes to the card whole (its unused texels given their
neighbours' colours first, so filtering does not bleed them in), each
vertex keeps its lightmap coordinates, and the card multiplies the texture
by the page (the vertices' colour then the material's tint alone); no
coarse copies are drawn for such batches (the card draws them whole).

A frame draws the clusters in the eye's potentially visible set (Halo's,
from the BSP's cluster data), each batch whose sphere is in the view, the
opaque ones first. A surface two subclusters list is drawn by both (the
depth buffer keeps the first).
*/

#include "cseries.h"

#include "bitmaps/bitmap_group.h"
#include "cache/cache_files.h"
#include "render/render.h"
#include "scenario/scenario.h"
#include "scenario/sky_definitions.h"
#include "structures/structure_bsp_definitions.h"
#include "structures/structure_vector_tests.h"
#include "tag_files/tag_groups.h"

#include "halo_system.h"
#include "dos_render.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *raw_malloc(size_t size) { return (malloc)(size); }
static void *raw_calloc(size_t count, size_t size) { return (calloc)(count, size); }
static void raw_free(void *pointer) { (free)(pointer); }

/* the Xbox's compressed vertices */
#define RENDERED_VERTEX_SIZE 32
#define LIGHTMAP_VERTEX_SIZE 8

/* a cluster's blocks (Invader's definitions: ScenarioStructureBSPCluster) */
#define CLUSTER_SUBCLUSTERS 0x34
#define CLUSTER_SURFACE_INDICES 0x44
#define SUBCLUSTER_SIZE 36
#define SUBCLUSTER_SURFACE_INDICES 24

struct batch
{
	const struct dos_material *material;
	/* its shader, and its texture: the material's, or a terrain tile's of
	its own (and a smaller one for its coarse copy), which the batch owns */
	long shader_tag_index;
	r_texture *texture, *coarse_texture;
	int tile_size, coarse_tile_size;
	struct r_vertex *vertices;
	unsigned short *indices;
	int vertex_count, triangle_count;
	float center[3], radius;
	/* its material's detail maps' copies of its vertices (dos_detail_maps:
	build_detail_layers): their coordinates, and their grey the map's
	strength there; NULL where it has none */
	struct r_vertex *detail_vertices[2];
	/* its lightmap page on the card and its vertices' coordinates in it (two
	floats each), or 0 and NULL: the vertices' colours are the light */
	unsigned long lightmap;
	float *lightmap_coordinates;
	/* its coarse copy, drawn past the distant geometry's distance (NULL
	where it would save little: build_coarse_batches) */
	struct r_vertex *coarse_vertices;
	unsigned short *coarse_indices;
	int coarse_vertex_count, coarse_triangle_count;
};

struct cluster
{
	int first_batch, batch_count;
};

static struct
{
	struct batch *batches;
	int batch_count, batch_size;
	struct cluster *clusters;
	int cluster_count;
	unsigned long sky_color;
	const unsigned long *sky_colors;
	float fog_start;
	/* the sky whose colour sky_color is (the eye's cluster's:
	dos_level_set_sky), and whether one was set since the load */
	short sky_index;
	int sky_set;
	/* the gradient made from the sky's colours where the map has none of
	its own: the horizon's sky_color to its model's colour overhead */
	unsigned long sky_gradient[R_SKY_STOPS];
	int sky_gradient_made;
	long triangles, vertices;
	/* each structure material's vertices' baked light (its lightmap's,
	sampled at load), for the objects over them (dos_level_light_at): by
	lightmap, its first material's index in these */
	unsigned long **material_lights;
	long *material_vertex_counts;
	int material_count;
	long *first_material;
	int lightmap_count;
	/* terrain tiles: how many, their texels, their texels for lod_tile
	units (the setting's, or fewer to fit the memory) */
	int tiles;
	long tile_texels;
	int tile_density;
	/* lit as the Xbox lights it: its lightmap pages on the card (by
	lightmap; 0 where there is none) */
	int lightmapped;
	unsigned long *lightmap_pages;
	/* the structure BSP it was built from: a campaign's scripts switch
	between a level's BSPs (dos_level_follow_bsp) */
	int built;
	short bsp_index;
} level;

const char *dos_game_map_name(void);
float dos_setting_real(const char *name, float default_value);
long dos_setting_integer(const char *name, long default_value);
extern struct r_settings dos_render_settings;

/* ---------- the sky and the fog

The level fades into fog toward the draw distance, from this share of it.
Halo's own fog is a light haze (Blood Gulch's: pale peach, at most 40%
dense); the software renderer's is opaque where the level ends, and in
Halo's colour over a sky of that colour Blood Gulch looks like a dust
storm. So the skies the N64 port drew (its n64_render_game.c) are drawn the
same way here: the dome's colour from the horizon up, the fog the horizon's
colour. Blood Gulch's "clear afternoon": deep blue overhead to pale blue at
the rim under light grey clouds, each stop a third of the way to their
colour; its fog starts later, the valley's middle distance clear. Other
maps: Halo's fog colour, everywhere behind the level. */

#define DOS_FOG_START 0.55f

static const struct dos_sky
{
	const char *map;
	unsigned long colors[R_SKY_STOPS];
	float fog_start;
} dos_skies[] =
{
	{ "bloodgulch", { 0xBBCCDD, 0xADC1DA, 0x90AAD3, 0x778CC6, 0x474AB1 }, 0.6f },
};

/* ---------- lighting */

struct page
{
	unsigned long *texels;
	int width, height;
};

static int is_fill(unsigned long c)
{
	/* (pure yellow and pure red, as 5:6:5 decodes them) */
	unsigned long r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;

	return b < 8 && r > 247 && (g > 247 || g < 8);
}

static int sample_page(const struct page *page, float u, float v, unsigned long *color)
{
	float x = u * page->width - 0.5f, y = v * page->height - 0.5f;

	for (float radius = 1.5f; radius <= 3.0f; radius += 1.5f)
	{
		float total[3] = { 0, 0, 0 }, weight = 0;

		for (int ty = (int)floorf(y - radius); ty <= (int)ceilf(y + radius); ty++)
		{
			for (int tx = (int)floorf(x - radius); tx <= (int)ceilf(x + radius); tx++)
			{
				float dx = (float)tx - x, dy = (float)ty - y, d, w;
				unsigned long texel;

				if (tx < 0 || ty < 0 || tx >= page->width || ty >= page->height)
					continue;
				d = sqrtf(dx * dx + dy * dy);
				if (d > radius)
					continue;
				texel = page->texels[ty * page->width + tx];
				if (is_fill(texel))
					continue;
				w = radius - d + 0.25f;
				total[0] += w * (float)((texel >> 16) & 0xFF);
				total[1] += w * (float)((texel >> 8) & 0xFF);
				total[2] += w * (float)(texel & 0xFF);
				weight += w;
			}
		}
		if (weight > 0)
		{
			unsigned long r = (unsigned long)(total[0] / weight), g = (unsigned long)(total[1] / weight),
				b = (unsigned long)(total[2] / weight);

			*color = (r << 16) | (g << 8) | b;
			return TRUE;
		}
	}
	return FALSE;
}

/* a page's unused texels (is_fill) given the colours of their neighbours,
a ring at a time, so that a card's filtering and smaller levels see the
lit texels' colours past their edges; any left the page's mean */
static void fill_unused_texels(unsigned long *texels, int width, int height)
{
	unsigned char *unused = raw_malloc((size_t)width * height);
	unsigned long sum[3] = { 0, 0, 0 }, used = 0, mean;
	int changed = TRUE;

	if (!unused)
		return;
	for (long i = 0; i < (long)width * height; i++)
	{
		unused[i] = (unsigned char)is_fill(texels[i]);
		if (!unused[i])
		{
			sum[0] += (texels[i] >> 16) & 0xFF;
			sum[1] += (texels[i] >> 8) & 0xFF;
			sum[2] += texels[i] & 0xFF;
			used++;
		}
	}
	mean = used ? ((sum[0] / used) << 16) | ((sum[1] / used) << 8) | (sum[2] / used) : 0x808080;
	for (int ring = 0; ring < 16 && changed; ring++)
	{
		changed = FALSE;
		for (int y = 0; y < height; y++)
		{
			for (int x = 0; x < width; x++)
			{
				static const int dx[4] = { -1, 1, 0, 0 }, dy[4] = { 0, 0, -1, 1 };
				unsigned long total[3] = { 0, 0, 0 }, count = 0;

				if (unused[y * width + x] != 1)
					continue;
				for (int k = 0; k < 4; k++)
				{
					int nx = x + dx[k], ny = y + dy[k];
					unsigned long c;

					/* (2: given a colour this ring, a neighbour from the next) */
					if (nx < 0 || ny < 0 || nx >= width || ny >= height || unused[ny * width + nx])
						continue;
					c = texels[ny * width + nx];
					total[0] += (c >> 16) & 0xFF;
					total[1] += (c >> 8) & 0xFF;
					total[2] += c & 0xFF;
					count++;
				}
				if (!count)
					continue;
				texels[y * width + x] = ((total[0] / count) << 16) | ((total[1] / count) << 8) | (total[2] / count);
				unused[y * width + x] = 2;
				changed = TRUE;
			}
		}
		for (long i = 0; i < (long)width * height; i++)
		{
			if (unused[i] == 2)
				unused[i] = 0;
		}
	}
	for (long i = 0; i < (long)width * height; i++)
	{
		if (unused[i])
			texels[i] = mean;
	}
	raw_free(unused);
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

static unsigned long summary_light(const struct render_lighting *lighting, const float *normal)
{
	float c[3] = { lighting->ambient_color.red, lighting->ambient_color.green, lighting->ambient_color.blue };
	unsigned long r, g, b;

	for (int i = 0; i < lighting->distant_light_count && i < 2; i++)
	{
		const struct render_distant_light *light = &lighting->distant_lights[i];
		float d = -(normal[0] * light->direction.i + normal[1] * light->direction.j + normal[2] * light->direction.k);

		if (d > 0)
		{
			c[0] += light->color.red * d;
			c[1] += light->color.green * d;
			c[2] += light->color.blue * d;
		}
	}
	/* (1 is the texture as it is: 0x80) */
	r = (unsigned long)PIN(c[0] * 128.0f, 0.0f, 255.0f);
	g = (unsigned long)PIN(c[1] * 128.0f, 0.0f, 255.0f);
	b = (unsigned long)PIN(c[2] * 128.0f, 0.0f, 255.0f);
	return (r << 16) | (g << 8) | b;
}

/* ---------- distant geometry: coarse batches

Past a distance (the setting lod_distance) a batch is drawn coarse: a copy
with its edges collapsed (dos_simplify.c) as long as no vertex moves farther
than lod_error world units from its triangles' planes. Its outer edge does
not move, so a coarse batch meets its neighbours, coarse or full, exactly;
no triangle turns over (it would be culled: a hole), and small things (a
base far across the map) keep their shape, the flat ground thinning out
instead. A batch keeps a coarse copy only where it saves a fifth of its
triangles or more. (The earlier copies merged vertices on a grid: they
opened holes in the ground and flattened distant buildings.) */

static struct
{
	float distance, error, tile;
	long coarse_triangles;
} lod;

static void build_coarse_batches(void)
{
	lod.coarse_triangles = 0;
	if (lod.distance <= 0.0f || lod.error <= 0.0f)
		return;
	for (int b = 0; b < level.batch_count; b++)
	{
		struct batch *batch = &level.batches[b];
		unsigned short *kept, *indices;
		int *slot;
		struct r_vertex *vertices;
		int count, vertex_count = 0;

		if (batch->triangle_count < 8 || batch->lightmap)
			continue;
		kept = raw_malloc((size_t)batch->triangle_count * 3 * sizeof(unsigned short));
		count = kept ? dos_simplify(batch->vertices[0].position, (int)(sizeof(struct r_vertex) / sizeof(float)), NULL,
			batch->vertex_count, batch->indices, batch->triangle_count, 1, lod.error, kept) : -1;
		if (count <= 0 || count * 5 > batch->triangle_count * 4)
		{
			raw_free(kept);
			continue;
		}
		/* (the batch's own vertices that the kept triangles use) */
		slot = raw_malloc((size_t)batch->vertex_count * sizeof(int));
		vertices = raw_malloc((size_t)batch->vertex_count * sizeof(struct r_vertex));
		indices = raw_malloc((size_t)count * 3 * sizeof(unsigned short));
		if (!slot || !vertices || !indices)
		{
			raw_free(kept);
			raw_free(slot);
			raw_free(vertices);
			raw_free(indices);
			continue;
		}
		for (int v = 0; v < batch->vertex_count; v++)
			slot[v] = -1;
		for (int i = 0; i < count * 3; i++)
		{
			int v = kept[i];

			if (slot[v] < 0)
			{
				slot[v] = vertex_count;
				vertices[vertex_count++] = batch->vertices[v];
			}
			indices[i] = (unsigned short)slot[v];
		}
		raw_free(kept);
		raw_free(slot);
		batch->coarse_vertices = vertices;
		batch->coarse_indices = indices;
		batch->coarse_vertex_count = vertex_count;
		batch->coarse_triangle_count = count;
		lod.coarse_triangles += batch->triangle_count - count;
	}
}

/* ---------- terrain tiles

The batches of a blended environment shader (outdoor ground) each get a
texture of their own: their square of the shader's base map, times its
detail maps (dos_textures.c, dos_terrain_tile). The setting terrain_tiles
is the texels a side for a square of lod_tile world units (a batch gets as
many as its size needs, at most 512 a side), 0 for none (the material's texture, as before); a coarse copy
draws it too, at the renderer's smaller levels. The batch's texture coordinates become the
tile's own: 0 to 1 over the square (place_tile_coordinates puts the texel
centres at its edges once the coarse copies have taken theirs). */

static int pow2_at_least(float value, int smallest, int largest)
{
	int size = smallest;

	while (size < largest && (float)size < value)
		size <<= 1;
	return size;
}

/* a batch's square of its base map and of the world, if it can be a tile:
a blended shader's, small in its base map (the level's colour map) */
static int tile_bounds(const struct batch *batch, float *low, float *high, float *extent)
{
	float world_low[2] = { 1e30f, 1e30f }, world_high[2] = { -1e30f, -1e30f };

	if (batch->material->transparent || batch->vertex_count < 3 || !dos_terrain_shader(batch->shader_tag_index))
		return FALSE;
	low[0] = low[1] = 1e30f;
	high[0] = high[1] = -1e30f;
	for (int v = 0; v < batch->vertex_count; v++)
	{
		const struct r_vertex *vertex = &batch->vertices[v];

		low[0] = MIN(low[0], vertex->u);
		low[1] = MIN(low[1], vertex->v);
		high[0] = MAX(high[0], vertex->u);
		high[1] = MAX(high[1], vertex->v);
		for (int k = 0; k < 2; k++)
		{
			world_low[k] = MIN(world_low[k], vertex->position[k]);
			world_high[k] = MAX(world_high[k], vertex->position[k]);
		}
	}
	*extent = MAX(world_high[0] - world_low[0], world_high[1] - world_low[1]);
	if (high[0] - low[0] <= 0.0f || high[1] - low[1] <= 0.0f || high[0] - low[0] > 1.0f || high[1] - low[1] > 1.0f)
	{
		if (dos_setting_integer("debug_tiles", 0))
		{
			char line[200];

			snprintf(line, sizeof(line), "tiles: %s: uv %.2f..%.2f, %.2f..%.2f: not a tile",
				tag_get_name(batch->shader_tag_index), (double)low[0], (double)high[0], (double)low[1],
				(double)high[1]);
			system_log(line);
		}
		return FALSE;
	}
	return TRUE;
}

static int tile_size(float extent, int density)
{
	return pow2_at_least(extent * (float)density / (lod.tile > 0.0f ? lod.tile : 16.0f), 8, 512);
}

/* The detail maps' layers (dos_detail_maps: OpenGL at TEXTURE QUALITY
HIGH and up): each batch whose environment shader has details gets a copy
of its vertices for each, its coordinates the base map's times the
detail's scale, its grey the detail's strength: a blended shader's base
map alpha at the vertex for the primary (one minus it for the secondary),
whole for the others. Drawn over the batch up close (draw_batch): Halo's
grain where the textures alone are a few texels a metre. Made before the
terrain tiles take the coordinates over. */
#define DETAIL_DISTANCE 64.0f

void dos_loading_level(long done, long total);

static void build_detail_layers(void)
{
	struct bitmap_data *decoded_base = NULL;
	unsigned long *base_texels = NULL;
	int base_width = 0, base_height = 0, layers = 0;

	if (!dos_detail_maps())
		return;
	for (int b = 0; b < level.batch_count; b++)
	{
		struct batch *batch = &level.batches[b];
		const struct dos_material *material = batch->material;

		if (material->transparent || material->skip || batch->vertex_count <= 0)
			continue;
		if (material->detail_blended && material->detail_base != decoded_base)
		{
			raw_free(base_texels);
			decoded_base = material->detail_base;
			base_texels = dos_bitmap_decode(decoded_base, 256, &base_width, &base_height);
		}
		for (int k = 0; k < 2; k++)
		{
			struct r_vertex *vertices;
			int any = FALSE;

			if (!material->detail[k])
				continue;
			vertices = raw_malloc((size_t)batch->vertex_count * sizeof(struct r_vertex));
			if (!vertices)
				continue;
			for (int v = 0; v < batch->vertex_count; v++)
			{
				const struct r_vertex *in = &batch->vertices[v];
				unsigned long weight = 255;

				vertices[v] = *in;
				vertices[v].u = in->u * material->detail_scale[k][0];
				vertices[v].v = in->v * material->detail_scale[k][1];
				if (material->detail_blended && base_texels)
				{
					long x = (long)floorf(in->u * (float)base_width), y = (long)floorf(in->v * (float)base_height);
					unsigned long alpha;

					x %= base_width;
					y %= base_height;
					alpha = base_texels[(y < 0 ? y + base_height : y) * base_width + (x < 0 ? x + base_width : x)] >> 24;
					weight = k == 0 ? alpha : 255 - alpha;
				}
				vertices[v].color = (weight << 16) | (weight << 8) | weight;
				any |= weight > 0;
			}
			if (!any)
			{
				raw_free(vertices);
				continue;
			}
			batch->detail_vertices[k] = vertices;
			layers++;
		}
	}
	raw_free(base_texels);
	if (dos_setting_integer("debug_tiles", 0))
	{
		char line[96];

		snprintf(line, sizeof(line), "detail: %d layers of the level's batches", layers);
		system_log(line);
	}
}

static void bake_terrain_tiles(void)
{
	/* texels a side for lod_tile world units (in 8 bits the slower machines:
	half as many), so the same texels a metre for every tile, at most 512 a
	side; fewer where the map's tiles would take more memory than
	terrain_tiles_kb (a texel a byte, and a third more for the renderer's
	smaller levels). Both follow TEXTURE QUALITY (texture_size: 128 the
	128 texels and 12 MB of before, 4096 KB in 8 bits; each step up twice
	the texels a side and four times the memory, to 48 MB, 16 MB in 8
	bits) */
	int eight = dos_render_settings.bits == 8;
	int quality = PIN(dos_render_settings.texture_size > 0 ? dos_render_settings.texture_size : 128, 32, 512);
	long quality_kb = (eight ? 4096L : 12288L) * quality / 128 * quality / 128;
	int density = (int)dos_setting_integer("terrain_tiles", eight ? quality / 2 : quality);
	long budget = dos_setting_integer("terrain_tiles_kb", MIN(quality_kb, eight ? 16384L : 49152L)) * 1024L;

	level.tiles = 0;
	level.tile_texels = 0;
	if (density < 8)
		return;
	for (;;)
	{
		long texels = 0;

		for (int b = 0; b < level.batch_count; b++)
		{
			float low[2], high[2], extent;
			int size;

			if (!tile_bounds(&level.batches[b], low, high, &extent))
				continue;
			size = tile_size(extent, density);
			texels += (long)size * size;
		}
		if (texels / 3 * 4 <= budget || density <= 8)
			break;
		density /= 2;
	}
	/* each shader's tiles in turn: their previews (a quarter of their texels
	a side) to one histogram, its palette, then the tiles in it */
	for (int first = 0; first < level.batch_count; first++)
	{
		long shader = level.batches[first].shader_tag_index;
		float low[2], high[2], extent;
		unsigned long *histogram;
		r_shared_palette *palette = NULL;
		int done = FALSE;

		if (level.batches[first].tile_size || !tile_bounds(&level.batches[first], low, high, &extent))
			continue;
		/* (a shader met before: its tiles are made) */
		for (int b = 0; b < first && !done; b++)
			done = level.batches[b].shader_tag_index == shader && level.batches[b].tile_size;
		if (done)
			continue;
		histogram = r_histogram_new();
		if (histogram)
		{
			for (int b = first; b < level.batch_count; b++)
			{
				if (level.batches[b].shader_tag_index != shader || !tile_bounds(&level.batches[b], low, high, &extent))
					continue;
				dos_terrain_preview(shader, low, high, MAX(tile_size(extent, density) / 4, 8), histogram);
			}
			palette = r_shared_palette_new(histogram);
			(free)(histogram);
		}
		for (int b = first; b < level.batch_count; b++)
		{
			struct batch *batch = &level.batches[b];
			r_texture *texture;
			int size;

			if (batch->shader_tag_index != shader || !tile_bounds(batch, low, high, &extent))
				continue;
			size = tile_size(extent, density);
			texture = dos_terrain_tile(shader, low, high, size, palette);
			if (!texture)
				continue;
			/* (its coarse copy draws it too: the renderer's own smaller levels
			of it are the distance's) */
			batch->texture = texture;
			batch->tile_size = size;
			/* (the square's own coordinates, 0 to 1 for now) */
			for (int v = 0; v < batch->vertex_count; v++)
			{
				struct r_vertex *vertex = &batch->vertices[v];

				vertex->u = (vertex->u - low[0]) / (high[0] - low[0]);
				vertex->v = (vertex->v - low[1]) / (high[1] - low[1]);
			}
			level.tiles++;
			level.tile_texels += (long)size * size;
			if (dos_setting_integer("debug_tiles", 0) > 1)
			{
				char line[200];

				snprintf(line, sizeof(line), "tiles: %s: %d texels for %.0f units at %.0f,%.0f, uv %.3f..%.3f, %.3f..%.3f",
					tag_get_name(shader), size, (double)extent, (double)batch->center[0], (double)batch->center[1],
					(double)low[0], (double)high[0], (double)low[1], (double)high[1]);
				system_log(line);
			}
		}
		r_shared_palette_delete(palette);
	}
	level.tile_density = density;
	dos_terrain_tiles_done();
}

/* a tile's 0 to 1 as its texels' middles: the first's to the last's */
static void place_tile_coordinates(void)
{
	for (int b = 0; b < level.batch_count; b++)
	{
		struct batch *batch = &level.batches[b];
		float size = (float)batch->tile_size, coarse = (float)batch->coarse_tile_size;

		if (!batch->tile_size)
			continue;
		for (int v = 0; v < batch->vertex_count; v++)
		{
			batch->vertices[v].u = (0.5f + batch->vertices[v].u * (size - 1.0f)) / size;
			batch->vertices[v].v = (0.5f + batch->vertices[v].v * (size - 1.0f)) / size;
		}
		if (coarse > 0.0f)
		{
			for (int v = 0; v < batch->coarse_vertex_count; v++)
			{
				batch->coarse_vertices[v].u = (0.5f + batch->coarse_vertices[v].u * (coarse - 1.0f)) / coarse;
				batch->coarse_vertices[v].v = (0.5f + batch->coarse_vertices[v].v * (coarse - 1.0f)) / coarse;
			}
		}
		else
		{
			for (int v = 0; v < batch->coarse_vertex_count; v++)
			{
				batch->coarse_vertices[v].u = (0.5f + batch->coarse_vertices[v].u * (size - 1.0f)) / size;
				batch->coarse_vertices[v].v = (0.5f + batch->coarse_vertices[v].v * (size - 1.0f)) / size;
			}
		}
	}
}

/* ---------- the 8-bit picture's palette for the map

The colours the level shows: each batch's texture's colours (a few dozen)
times the light of a dozen of its vertices (as the 8-bit picture lights
them: their brightness alone, r_main.c's light_vertex), and the same a third
and two thirds of the way into the fog, each by the batch's triangles; the
sky (a third of the weight: about as much of the picture); a few colours
fixed for the HUD (its blues, white, black, the blips' red and green). A
median cut of them is the palette (r_palette.c). */

#define PALETTE_SAMPLED_TEXELS 48
#define PALETTE_SAMPLED_VERTICES 12

static const unsigned long palette_fixed[] =
{
	0x000000, 0xFFFFFF, 0x2896FF, 0xAAD5FF, 0xE6F0FF, 0xE02020, 0x20E020, 0x808080,
};

static void histogram_add(unsigned long *histogram, unsigned long color, unsigned long weight)
{
	histogram[((color >> 9) & 0x7C00) | ((color >> 6) & 0x03E0) | ((color >> 3) & 0x001F)] += weight;
}

/* a colour lit by a light's brightness (0x80 as it is) */
static unsigned long lit_by(unsigned long color, unsigned long light)
{
	unsigned long level = (((light >> 16) & 0xFF) * 77 + ((light >> 8) & 0xFF) * 151 + (light & 0xFF) * 28) >> 8;
	unsigned long result = 0;

	for (int shift = 0; shift <= 16; shift += 8)
	{
		unsigned long c = (((color >> shift) & 0xFF) * level) >> 7;

		result |= (c > 255 ? 255 : c) << shift;
	}
	return result;
}

/* a colour a share (in 16ths) of the way to another */
static unsigned long toward(unsigned long color, unsigned long target, int share)
{
	unsigned long result = 0;

	for (int shift = 0; shift <= 16; shift += 8)
	{
		long c = (long)((color >> shift) & 0xFF), t = (long)((target >> shift) & 0xFF);

		result |= (unsigned long)(c + (t - c) * share / 16) << shift;
	}
	return result;
}

/* the sky's colour at a height above the horizon (r_main.c's stops) */
static unsigned long sky_at(const unsigned long *colors, int degrees)
{
	static const int stops[R_SKY_STOPS] = { 0, 15, 30, 50, 90 };
	int i = 0;

	while (i < R_SKY_STOPS - 2 && degrees > stops[i + 1])
		i++;
	return toward(colors[i], colors[i + 1], (degrees - stops[i]) * 16 / (stops[i + 1] - stops[i]));
}

/* the map's model shaders (its objects' and its skies'), their materials
made while the level loads (in 8 bits before its palette, which takes
their colours too: a level with no geometry of its own, ui.map's, has
theirs alone) */
static const unsigned long shader_groups[] = { 'soso', 'schi', 'scex' };

static void load_shader_materials(void)
{
	for (unsigned g = 0; g < NUMBEROF(shader_groups); g++)
	{
		struct tag_iterator iterator;
		long index;

		tag_iterator_new(&iterator, (long)shader_groups[g]);
		while ((index = tag_iterator_next(&iterator)) != NONE)
			dos_shader_material(index);
	}
}

static void histogram_add_shaders(unsigned long *histogram, unsigned long total, unsigned long fog_color)
{
	unsigned long texels[PALETTE_SAMPLED_TEXELS];
	long count = 0;

	for (unsigned g = 0; g < NUMBEROF(shader_groups); g++)
	{
		struct tag_iterator iterator;

		tag_iterator_new(&iterator, (long)shader_groups[g]);
		while (tag_iterator_next(&iterator) != NONE)
			count++;
	}
	if (!count)
		return;
	for (unsigned g = 0; g < NUMBEROF(shader_groups); g++)
	{
		struct tag_iterator iterator;
		long index;

		tag_iterator_new(&iterator, (long)shader_groups[g]);
		while ((index = tag_iterator_next(&iterator)) != NONE)
		{
			const struct dos_material *material = dos_shader_material(index);
			int texel_count = material && !material->skip ?
				r_texture_colors(material->texture, texels, PALETTE_SAMPLED_TEXELS) : 0;
			unsigned long weight = texel_count ? total / (unsigned long)count / (unsigned long)texel_count + 1 : 0;

			for (int t = 0; t < texel_count; t++)
			{
				histogram_add(histogram, texels[t] & 0xFFFFFF, weight * 2);
				histogram_add(histogram, toward(texels[t] & 0xFFFFFF, fog_color, 6), weight);
			}
			/* (a skybox's faces: much of the picture, a shader's share each) */
			for (int face = 0; material && face < 6; face++)
			{
				texel_count = r_texture_colors(material->cube[face], texels, PALETTE_SAMPLED_TEXELS);
				weight = texel_count ? total / (unsigned long)count / (unsigned long)texel_count + 1 : 0;
				for (int t = 0; t < texel_count; t++)
					histogram_add(histogram, texels[t] & 0xFFFFFF, weight * 2);
			}
		}
	}
}

static void level_palette(void)
{
	unsigned long *histogram = raw_calloc(32768, sizeof(unsigned long));
	unsigned long colors[256], texels[PALETTE_SAMPLED_TEXELS], level_weight = 0;
	unsigned char rgb[256 * 3];
	int fixed = (int)NUMBEROF(palette_fixed), count, i;

	if (!histogram)
		return;
	for (int b = 0; b < level.batch_count; b++)
	{
		const struct batch *batch = &level.batches[b];
		int texel_count = r_texture_colors(batch->texture, texels, PALETTE_SAMPLED_TEXELS);
		int step = batch->vertex_count / PALETTE_SAMPLED_VERTICES + 1;
		unsigned long weight = (unsigned long)batch->triangle_count;

		if (!texel_count)
		{
			texels[0] = 0x808080;
			texel_count = 1;
		}
		for (int v = 0; v < batch->vertex_count; v += step)
		{
			for (int t = 0; t < texel_count; t++)
			{
				unsigned long lit = lit_by(texels[t], batch->vertices[v].color);

				histogram_add(histogram, lit, weight * 2);
				histogram_add(histogram, toward(lit, level.sky_color, 5), weight);
				histogram_add(histogram, toward(lit, level.sky_color, 11), weight);
				level_weight += weight * 4;
			}
		}
	}
	/* (the objects' and skies' a third as much as the level's) */
	histogram_add_shaders(histogram, level_weight ? level_weight / 3 : 300000, level.sky_color);
	if (level.sky_colors)
	{
		for (int degrees = 0; degrees <= 90; degrees++)
			histogram_add(histogram, sky_at(level.sky_colors, degrees), level_weight / 2 / 91 + 1);
	}
	else
	{
		histogram_add(histogram, level.sky_color, level_weight / 2 + 1);
	}
	count = r_palette_median_cut(histogram, 256 - fixed, colors);
	raw_free(histogram);
	for (i = 0; i < 256; i++)
	{
		unsigned long color = i < fixed ? palette_fixed[i] : i - fixed < count ? colors[i - fixed] : 0;

		rgb[i * 3 + 0] = (unsigned char)(color >> 16);
		rgb[i * 3 + 1] = (unsigned char)(color >> 8);
		rgb[i * 3 + 2] = (unsigned char)color;
	}
	r_palette_set(rgb);
	system_display_palette(rgb);
}

/* ---------- building */

struct source_material
{
	const struct dos_material *material;
	long shader_tag_index;
	struct r_vertex *vertices;
	/* each vertex's light alone (0x00RRGGBB, 0x80 as the texture is) */
	unsigned long *lights;
	/* the lightmap's page on the card and each vertex's coordinates in it,
	or 0 and NULL (the vertices' colours are their light) */
	unsigned long lightmap;
	float *lightmap_coordinates;
	long vertex_count;
	long first_surface, surface_count;
};

static struct batch *new_batch(void)
{
	if (level.batch_count == level.batch_size)
	{
		int size = level.batch_size ? level.batch_size * 2 : 256;
		struct batch *grown = (realloc)(level.batches, (size_t)size * sizeof(*grown));

		if (!grown)
			return NULL;
		level.batches = grown;
		level.batch_size = size;
	}
	memset(&level.batches[level.batch_count], 0, sizeof(struct batch));
	return &level.batches[level.batch_count++];
}

/* the tile of a triangle's middle: a square of lod_tile world units (one
square when there are none) */
static unsigned long tile_key(const struct r_vertex *a, const struct r_vertex *b, const struct r_vertex *c)
{
	float x = (a->position[0] + b->position[0] + c->position[0]) / 3.0f;
	float y = (a->position[1] + b->position[1] + c->position[1]) / 3.0f;
	unsigned long tx, ty;

	if (lod.tile <= 0.0f)
		return 1;
	tx = (unsigned long)((long)floorf(x / lod.tile) + 32768) & 0xFFFF;
	ty = (unsigned long)((long)floorf(y / lod.tile) + 32768) & 0xFFFF;
	/* (0 is a triangle already in a batch) */
	return ((tx << 16) | ty) + 1;
}

/* the batches of a list of surfaces: one per material and tile (the
triangles whose middles are in one square, lod_tile world units a side:
each tile is culled on its own, and is drawn coarse on its own past the
distant geometry's distance) */
/* (seen: each surface's cluster stamp, so that one a cluster lists in two
of its subclusters goes into the first's batches alone: drawn twice, the
Xbox's texture pass, which multiplies the picture, would darken it) */
static void add_batches(const struct structure_bsp *bsp, const struct source_material *materials, int material_count,
	const short *surface_material, const long *surfaces, long surface_count, short *vertex_map, long *seen, long stamp)
{
	const struct structure_surface *all = (const struct structure_surface *)xbox_pointer(bsp->surfaces.address);
	long *listed = raw_malloc((size_t)(surface_count ? surface_count : 1) * sizeof(long));
	unsigned long *tiles = raw_malloc((size_t)(surface_count ? surface_count : 1) * sizeof(unsigned long));

	if (!listed || !tiles)
	{
		raw_free(listed);
		raw_free(tiles);
		return;
	}
	for (int m = 0; m < material_count; m++)
	{
		const struct source_material *source = &materials[m];
		long count = 0;

		if (!source->vertices || source->material->skip)
			continue;
		/* the material's triangles, each with its tile */
		for (long i = 0; i < surface_count; i++)
		{
			long s = surfaces[i];
			long v[3];

			if (s < 0 || s >= bsp->surfaces.count || surface_material[s] != m)
				continue;
			if (seen)
			{
				if (seen[s] == stamp)
					continue;
				seen[s] = stamp;
			}
			for (int k = 0; k < 3; k++)
			{
				v[k] = (long)all[s].vertex_indices[k];
				if (v[k] >= source->vertex_count)
					v[k] = 0;
			}
			listed[count] = s;
			tiles[count] = tile_key(&source->vertices[v[0]], &source->vertices[v[1]], &source->vertices[v[2]]);
			count++;
		}
		/* a batch for each tile in turn */
		for (long first = 0; first < count; first++)
		{
			unsigned long tile = tiles[first];
			long triangles = 0;
			struct batch *batch;
			float low[3] = { 1e30f, 1e30f, 1e30f }, high[3] = { -1e30f, -1e30f, -1e30f };

			if (!tile)
				continue;
			for (long i = first; i < count; i++)
				triangles += tiles[i] == tile;
			if (triangles * 3 > 65535)
			{
				for (long i = first; i < count; i++)
					tiles[i] = tiles[i] == tile ? 0 : tiles[i];
				continue;
			}
			batch = new_batch();
			if (!batch)
				goto out;
			batch->material = source->material;
			batch->shader_tag_index = source->shader_tag_index;
			batch->texture = source->material->texture;
			batch->indices = raw_malloc((size_t)triangles * 3 * sizeof(unsigned short));
			batch->vertices = raw_malloc((size_t)triangles * 3 * sizeof(struct r_vertex));
			if (source->lightmap)
			{
				batch->lightmap_coordinates = raw_malloc((size_t)triangles * 3 * 2 * sizeof(float));
				if (batch->lightmap_coordinates)
					batch->lightmap = source->lightmap;
			}
			if (!batch->indices || !batch->vertices)
			{
				raw_free(batch->indices);
				raw_free(batch->vertices);
				raw_free(batch->lightmap_coordinates);
				level.batch_count--;
				goto out;
			}
			memset(vertex_map, 0xFF, (size_t)source->vertex_count * sizeof(short));
			for (long i = first; i < count; i++)
			{
				long s = listed[i];

				if (tiles[i] != tile)
					continue;
				tiles[i] = 0;
				for (int k = 0; k < 3; k++)
				{
					long v = (long)all[s].vertex_indices[k];

					if (v >= source->vertex_count)
						v = 0;
					if (vertex_map[v] < 0)
					{
						vertex_map[v] = (short)batch->vertex_count;
						if (batch->lightmap)
						{
							batch->lightmap_coordinates[batch->vertex_count * 2] = source->lightmap_coordinates[v * 2];
							batch->lightmap_coordinates[batch->vertex_count * 2 + 1] =
								source->lightmap_coordinates[v * 2 + 1];
						}
						batch->vertices[batch->vertex_count++] = source->vertices[v];
					}
					batch->indices[batch->triangle_count * 3 + k] = (unsigned short)vertex_map[v];
				}
				batch->triangle_count++;
			}
			for (int v = 0; v < batch->vertex_count; v++)
			{
				for (int k = 0; k < 3; k++)
				{
					if (batch->vertices[v].position[k] < low[k]) low[k] = batch->vertices[v].position[k];
					if (batch->vertices[v].position[k] > high[k]) high[k] = batch->vertices[v].position[k];
				}
			}
			for (int k = 0; k < 3; k++)
				batch->center[k] = (low[k] + high[k]) * 0.5f;
			batch->radius = 0.5f * sqrtf((high[0] - low[0]) * (high[0] - low[0]) +
				(high[1] - low[1]) * (high[1] - low[1]) + (high[2] - low[2]) * (high[2] - low[2]));
			/* (testing, setting debug_light: where the water and glass are) */
			if (batch->material->transparent && dos_setting_integer("debug_light", 0))
			{
				char line[160];

				snprintf(line, sizeof(line), "transparent batch: %d triangles about %.1f %.1f %.1f, radius %.1f",
					batch->triangle_count, (double)batch->center[0], (double)batch->center[1], (double)batch->center[2],
					(double)batch->radius);
				system_log(line);
			}
			level.triangles += batch->triangle_count;
			level.vertices += batch->vertex_count;
		}
	}
out:
	raw_free(listed);
	raw_free(tiles);
}

/* a vertex's light times its material's tint, kept from lighting the
material's average texel past white: the three channels scaled down
together, so the hue stays (the N64 port's fit_shade, tools/n64/level.py:
clipped one by one, Blood Gulch's orange sand in its warm sun went yellow).
Halo draws the lightmap and then multiplies the texture onto it
(rasterizer_xbox_environment.c: the texture pass's source blend is the
destination's colour): a lightmap's white is the texture as it is, 0x80 a
channel here, not twice it (the doubling is the detail maps', already in
the textures: terrain tiles, detail_tint). */
static unsigned long fit_light(unsigned long light, unsigned long tint, unsigned long average)
{
	unsigned long c[3], top = 0;

	for (int k = 0; k < 3; k++)
	{
		int shift = 16 - 8 * k;
		unsigned long lit;

		c[k] = (((light >> shift) & 0xFF) * ((tint >> shift) & 0xFF) + 128) >> 8;
		lit = (c[k] * ((average >> shift) & 0xFF)) >> 7;
		if (lit > top)
			top = lit;
	}
	for (int k = 0; k < 3; k++)
	{
		if (top > 255)
			c[k] = c[k] * 255 / top;
		if (c[k] > 255)
			c[k] = 255;
	}
	return (c[0] << 16) | (c[1] << 8) | c[2];
}

static void decode_material(const struct structure_bsp *bsp, const struct structure_material *material,
	const struct page *page, unsigned long lightmap, struct source_material *out)
{
	const byte *data = (const byte *)xbox_pointer(material->compressed_vertex_data.address);
	long count = material->vertices.count;
	int lit = page && page->texels && material->lightmap_vertices.count >= count;
	int unlit;
	long light_sum[3] = { 0, 0, 0 };

	out->material = dos_shader_material(material->shader.index);
	out->shader_tag_index = material->shader.index;
	/* (level_light = 0: every vertex the texture as it is; and always the
	water and glass, which Halo draws without the lightmaps: their
	lighting summary is black) */
	unlit = !dos_lighting || out->material->transparent;
	if (!out->material->skip && !out->material->texture && dos_setting_integer("debug_untextured", 0))
	{
		char line[160];

		snprintf(line, sizeof(line), "level shader without a texture: %s (%ld vertices)",
			material->shader.index != NONE ? tag_get_name(material->shader.index) : "none", (long)count);
		system_log(line);
	}
	out->vertex_count = count;
	out->first_surface = material->first_surface_index;
	out->surface_count = material->surface_count;
	out->vertices = NULL;
	out->lights = NULL;
	out->lightmap = 0;
	out->lightmap_coordinates = NULL;
	if (!data || count <= 0 || count > 32767)
		return;
	if (material->compressed_vertex_data.size < count * RENDERED_VERTEX_SIZE)
		return;
	/* (no light kept for the objects: they take none, dos_render_game.c) */
	if (!out->material->skip)
		out->vertices = raw_malloc((size_t)count * sizeof(struct r_vertex));
	if (!out->lights && !out->vertices)
		return;
	/* (lit as the Xbox lights it: the page on the card, the coordinates) */
	if (lit && lightmap && !unlit && out->vertices)
	{
		out->lightmap_coordinates = raw_malloc((size_t)count * 2 * sizeof(float));
		if (out->lightmap_coordinates)
			out->lightmap = lightmap;
	}
	for (long v = 0; v < count; v++)
	{
		const byte *rendered = data + v * RENDERED_VERTEX_SIZE;
		struct r_vertex unused;
		struct r_vertex *vertex = out->vertices ? &out->vertices[v] : &unused;
		unsigned long color;
		float normal[3];

		memcpy(vertex->position, rendered, 12);
		unpack_normal(*(const unsigned long *)(rendered + 12), normal);
		memcpy(&vertex->u, rendered + 24, 4);
		memcpy(&vertex->v, rendered + 28, 4);
		if (unlit)
		{
			color = 0x808080;
		}
		else if (lit)
		{
			const byte *lightmap = data + count * RENDERED_VERTEX_SIZE + v * LIGHTMAP_VERTEX_SIZE;
			short lu = *(const short *)(lightmap + 4), lv = *(const short *)(lightmap + 6);

			if (!sample_page(page, (float)lu / 32767.0f, (float)lv / 32767.0f, &color))
				color = summary_light(&material->lighting, normal);
		}
		else
		{
			color = summary_light(&material->lighting, normal);
		}
		if (out->lights)
			out->lights[v] = color;
		vertex->color = fit_light(color, out->material->tint, r_texture_average(out->material->texture));
		if (out->lightmap)
		{
			const byte *lightmap_vertex = data + count * RENDERED_VERTEX_SIZE + v * LIGHTMAP_VERTEX_SIZE;

			out->lightmap_coordinates[v * 2] = (float)*(const short *)(lightmap_vertex + 4) / 32767.0f;
			out->lightmap_coordinates[v * 2 + 1] = (float)*(const short *)(lightmap_vertex + 6) / 32767.0f;
			/* (the page multiplies the texture: the colour is the tint alone) */
			vertex->color = out->material->tint;
		}
		light_sum[0] += (color >> 16) & 0xFF;
		light_sum[1] += (color >> 8) & 0xFF;
		light_sum[2] += color & 0xFF;
	}
	/* (testing, setting debug_light: each material's light and texture) */
	if (dos_setting_integer("debug_light", 0) && count > 0)
	{
		char line[200];

		snprintf(line, sizeof(line), "light: %s: %ld vertices, light (%ld,%ld,%ld), texture %06lX, tint %06lX%s, "
			"first vertex %06lX at %.1f %.1f %.1f",
			material->shader.index != NONE ? tag_get_name(material->shader.index) : "none", (long)count,
			light_sum[0] / count, light_sum[1] / count, light_sum[2] / count,
			r_texture_average(out->material->texture) & 0xFFFFFF, out->material->tint, lit ? "" : " (summary)",
			out->vertices ? out->vertices[0].color & 0xFFFFFF : 0,
			out->vertices ? (double)out->vertices[0].position[0] : 0.0,
			out->vertices ? (double)out->vertices[0].position[1] : 0.0,
			out->vertices ? (double)out->vertices[0].position[2] : 0.0);
		system_log(line);
	}
}

/* the level of the current structure BSP; with a new 8-bit palette for the
map (new_palette) or in the one it has (a BSP switch: the textures already
baked into it stay as they are) */
static void load(int new_palette)
{
	struct structure_bsp *bsp = global_structure_bsp_get();
	struct source_material *materials = NULL;
	int material_count = 0, material_index = 0;
	short *surface_material = NULL, *vertex_map = NULL;
	long *seen = NULL;
	long *surfaces = NULL;
	long surfaces_size = 0;
	long largest_vertex_count = 1;
	char line[256];
	unsigned long start = (unsigned long)(system_microseconds() / 1000);

	dos_level_unload();
	lod.distance = dos_setting_real("lod_distance", 40.0f);
	lod.error = dos_setting_real("lod_error", 1.0f);
	lod.tile = dos_setting_real("lod_tile", 16.0f);
	/* (in 8 bits the level's textures wait for its palette: level_palette) */
	if (dos_render_settings.bits == 8 && new_palette)
		r_textures_defer_baking(TRUE);
	level.built = TRUE;
	level.bsp_index = global_structure_bsp_index;
	level.sky_color = 0x7090B8;
	level.sky_colors = NULL;
	level.fog_start = DOS_FOG_START;
	level.sky_set = FALSE;
	level.sky_gradient_made = FALSE;
	{
		struct sky *sky = global_scenario_get() ? scenario_get_sky(0) : NULL;

		if (sky)
		{
			real_rgb_color const *color = &sky->outdoor_fog.color;
			unsigned long r = (unsigned long)(PIN(color->red, 0.0f, 1.0f) * 255.0f);
			unsigned long g = (unsigned long)(PIN(color->green, 0.0f, 1.0f) * 255.0f);
			unsigned long b = (unsigned long)(PIN(color->blue, 0.0f, 1.0f) * 255.0f);

			if (r + g + b > 48)
				level.sky_color = (r << 16) | (g << 8) | b;
		}
		/* (Direct3D 8 draws the map's own sky model instead, as the Xbox:
		dos_xbox_sky) */
		for (unsigned i = 0; i < NUMBEROF(dos_skies) && !dos_xbox_sky(); i++)
		{
			if (!_stricmp(dos_game_map_name(), dos_skies[i].map))
			{
				level.sky_colors = dos_skies[i].colors;
				level.sky_color = dos_skies[i].colors[0];
				level.fog_start = dos_skies[i].fog_start;
			}
		}
	}
	if (!bsp)
		goto done;

	/* every lightmap's materials, in order */
	for (long l = 0; l < bsp->lightmaps.count; l++)
		material_count += TAG_BLOCK_GET_ELEMENT(&bsp->lightmaps, l, struct structure_lightmap)->materials.count;
	materials = raw_calloc((size_t)(material_count ? material_count : 1), sizeof(*materials));
	surface_material = raw_malloc((size_t)(bsp->surfaces.count ? bsp->surfaces.count : 1) * sizeof(short));
	level.material_lights = raw_calloc((size_t)(material_count ? material_count : 1), sizeof(unsigned long *));
	level.material_vertex_counts = raw_calloc((size_t)(material_count ? material_count : 1), sizeof(long));
	level.first_material = raw_calloc((size_t)(bsp->lightmaps.count ? bsp->lightmaps.count : 1), sizeof(long));
	if (!materials || !surface_material || !level.material_lights || !level.material_vertex_counts ||
		!level.first_material)
		goto done;
	level.material_count = material_count;
	level.lightmap_count = (int)bsp->lightmaps.count;
	level.lightmapped = r_draws_lightmaps() && dos_lighting && dos_setting_integer("d3d_lightmaps", 1);
	if (level.lightmapped)
	{
		level.lightmap_pages = raw_calloc((size_t)(bsp->lightmaps.count ? bsp->lightmaps.count : 1),
			sizeof(unsigned long));
	}
	memset(surface_material, 0xFF, (size_t)bsp->surfaces.count * sizeof(short));
	for (long l = 0; l < bsp->lightmaps.count; l++)
	{
		struct structure_lightmap *lightmap = TAG_BLOCK_GET_ELEMENT(&bsp->lightmaps, l, struct structure_lightmap);
		struct page page = { NULL, 0, 0 };

		level.first_material[l] = material_index;
		if (dos_lighting && lightmap->bitmap_index >= 0 && bsp->lightmap_group.index != NONE)
		{
			struct bitmap_group *group = bitmap_group_get(bsp->lightmap_group.index);

			if (group && lightmap->bitmap_index < group->bitmaps.count)
			{
				page.texels = dos_bitmap_decode(
					TAG_BLOCK_GET_ELEMENT(&group->bitmaps, lightmap->bitmap_index, struct bitmap_data), 1024,
					&page.width, &page.height);
			}
		}
		if (level.lightmap_pages && page.texels)
		{
			/* (a copy: the page's own texels are sampled for the vertices'
			light, which tells the unused ones apart) */
			unsigned long *copy = raw_malloc((size_t)page.width * page.height * sizeof(unsigned long));

			if (copy)
			{
				memcpy(copy, page.texels, (size_t)page.width * page.height * sizeof(unsigned long));
				fill_unused_texels(copy, page.width, page.height);
				level.lightmap_pages[l] = r_lightmap_new(copy, page.width, page.height);
				raw_free(copy);
			}
		}
		for (long m = 0; m < lightmap->materials.count; m++, material_index++)
		{
			struct structure_material *material = TAG_BLOCK_GET_ELEMENT(&lightmap->materials, m,
				struct structure_material);

			decode_material(bsp, material, &page, level.lightmap_pages ? level.lightmap_pages[l] : 0,
				&materials[material_index]);
			/* (the light stays for the objects; the vertices go to batches) */
			level.material_lights[material_index] = materials[material_index].lights;
			level.material_vertex_counts[material_index] = materials[material_index].lights ?
				materials[material_index].vertex_count : 0;
			materials[material_index].lights = NULL;
			if (materials[material_index].vertex_count > largest_vertex_count)
				largest_vertex_count = materials[material_index].vertex_count;
			for (long s = material->first_surface_index;
				s < material->first_surface_index + material->surface_count && s < bsp->surfaces.count; s++)
			{
				if (s >= 0)
					surface_material[s] = (short)material_index;
			}
		}
		raw_free(page.texels);
	}

	/* each cluster's batches, by subcluster */
	vertex_map = raw_malloc((size_t)largest_vertex_count * sizeof(short));
	seen = raw_calloc((size_t)(bsp->surfaces.count > 0 ? bsp->surfaces.count : 1), sizeof(long));
	level.cluster_count = (int)bsp->clusters.count;
	level.clusters = raw_calloc((size_t)(level.cluster_count ? level.cluster_count : 1), sizeof(struct cluster));
	if (!vertex_map || !seen || !level.clusters)
		goto done;
	for (int c = 0; c < level.cluster_count; c++)
	{
		const byte *cluster = (const byte *)xbox_pointer(bsp->clusters.address) + c * sizeof(struct structure_cluster);
		const struct tag_block *subclusters = (const struct tag_block *)(cluster + CLUSTER_SUBCLUSTERS);
		const struct tag_block *cluster_surfaces = (const struct tag_block *)(cluster + CLUSTER_SURFACE_INDICES);

		level.clusters[c].first_batch = level.batch_count;
		for (long k = 0; k < subclusters->count; k++)
		{
			const byte *subcluster = (const byte *)xbox_pointer(subclusters->address) + k * SUBCLUSTER_SIZE;
			const struct tag_block *indices = (const struct tag_block *)(subcluster + SUBCLUSTER_SURFACE_INDICES);

			if (indices->count > 0)
			{
				add_batches(bsp, materials, material_count, surface_material, (const long *)xbox_pointer(indices->address),
					indices->count, vertex_map, seen, c + 1);
			}
		}
		if (cluster_surfaces->count > 0)
		{
			/* (the cluster's own list, beside its subclusters': a surface in
			both is drawn twice, which the depth test hides) */
			if (!subclusters->count)
			{
				add_batches(bsp, materials, material_count, surface_material, (const long *)xbox_pointer(cluster_surfaces->address),
					cluster_surfaces->count, vertex_map, seen, c + 1);
			}
		}
		level.clusters[c].batch_count = level.batch_count - level.clusters[c].first_batch;
	}
	dos_loading_level(1, 4);
	build_detail_layers();
	/* (none on the Xbox's texture pass: the card mixes the detail maps) */
	if (!dos_xbox_detail())
		bake_terrain_tiles();
	dos_loading_level(3, 4);
	build_coarse_batches();
	dos_loading_level(4, 4);
	place_tile_coordinates();
	(void)surfaces;
	(void)surfaces_size;

done:
	for (int m = 0; m < material_count && materials; m++)
	{
		raw_free(materials[m].vertices);
		raw_free(materials[m].lightmap_coordinates);
	}
	raw_free(materials);
	raw_free(surface_material);
	raw_free(vertex_map);
	raw_free(seen);
	if (dos_render_settings.bits == 8 && new_palette)
	{
		load_shader_materials();
		r_textures_defer_baking(FALSE);
		level_palette();
	}
	snprintf(line, sizeof(line), "level: %d clusters, %d batches, %ld triangles (%ld fewer coarse past %.0f), "
		"%ld vertices, %d terrain tiles (%ld KB, %d texels a tile), %lu ms", level.cluster_count, level.batch_count, level.triangles,
		lod.coarse_triangles, (double)lod.distance, level.vertices, level.tiles, level.tile_texels * 4 / 3 / 1024,
		level.tile_density,
		(unsigned long)(system_microseconds() / 1000) - start);
	system_log(line);
}

void dos_level_load(void)
{
	extern void dos_node_matrices_forget(void);

	dos_node_matrices_forget();
	load(TRUE);
}

void dos_level_follow_bsp(void)
{
	if (level.built && global_structure_bsp_index != level.bsp_index && global_structure_bsp_get())
	{
		char line[64];

		snprintf(line, sizeof(line), "level: structure bsp %d -> %d", level.bsp_index, global_structure_bsp_index);
		system_log(line);
		load(FALSE);
	}
}

void dos_level_unload(void)
{
	for (int i = 0; i < level.batch_count; i++)
	{
		raw_free(level.batches[i].vertices);
		raw_free(level.batches[i].indices);
		raw_free(level.batches[i].coarse_vertices);
		raw_free(level.batches[i].coarse_indices);
		raw_free(level.batches[i].detail_vertices[0]);
		raw_free(level.batches[i].detail_vertices[1]);
		raw_free(level.batches[i].lightmap_coordinates);
		if (level.batches[i].tile_size)
		{
			r_texture_delete(level.batches[i].texture);
			r_texture_delete(level.batches[i].coarse_texture);
		}
	}
	for (int i = 0; i < level.lightmap_count && level.lightmap_pages; i++)
		r_lightmap_delete(level.lightmap_pages[i]);
	raw_free(level.lightmap_pages);
	for (int i = 0; i < level.material_count && level.material_lights; i++)
		raw_free(level.material_lights[i]);
	raw_free(level.material_lights);
	raw_free(level.material_vertex_counts);
	raw_free(level.first_material);
	raw_free(level.batches);
	raw_free(level.clusters);
	memset(&level, 0, sizeof(level));
}

/* The level's light at a point, for the objects (no light of their own is
worked out: the level's is baked, and so theirs is): the surface below it
within 10 units, as Halo's lightmap samples look (object_lights.c), its
vertices' light blended at the point straight under, as Halo blends a
surface's corners (shade_scalar). 0x00RRGGBB, 0x80 as the texture is;
FALSE where nothing is below. */
int dos_level_light_at(const real_point3d *point, unsigned long *color)
{
	static real_vector3d const down = { 0.0f, 0.0f, -10.0f };
	struct structure_bsp *bsp = global_structure_bsp_get();
	real_point3d collision;
	short lightmap_index, material_index;
	long surface_index, index, count, corners[3];
	real s, t;
	const unsigned long *lights;
	const struct structure_surface *surface;
	unsigned long result = 0;

	if (!bsp || !level.material_lights ||
		!structure_test_vector(point, &down, &collision, &lightmap_index, &material_index, &surface_index, &s, &t))
		return FALSE;
	if (lightmap_index < 0 || lightmap_index >= level.lightmap_count || material_index < 0 || surface_index < 0 ||
		surface_index >= bsp->surfaces.count)
		return FALSE;
	index = level.first_material[lightmap_index] + material_index;
	if (index >= level.material_count || !(lights = level.material_lights[index]))
		return FALSE;
	count = level.material_vertex_counts[index];
	surface = (const struct structure_surface *)xbox_pointer(bsp->surfaces.address) + surface_index;
	for (int k = 0; k < 3; k++)
	{
		corners[k] = (long)surface->vertex_indices[k];
		if (corners[k] < 0 || corners[k] >= count)
			return FALSE;
	}
	for (int shift = 0; shift <= 16; shift += 8)
	{
		real c0 = (real)((lights[corners[0]] >> shift) & 0xFF), c1 = (real)((lights[corners[1]] >> shift) & 0xFF);
		real c2 = (real)((lights[corners[2]] >> shift) & 0xFF);

		result |= (unsigned long)PIN((c1 - c0) * s + (c2 - c0) * t + c0, 0.0f, 255.0f) << shift;
	}
	*color = result;
	return TRUE;
}

unsigned long dos_level_sky_color(void)
{
	return level.sky_color ? level.sky_color : 0x7090B8;
}

/* The sky of the eye's cluster (dos_render_game.c, each frame): the colour
behind the level and its fog's are its own, not the scenario's first sky's
(a10's cinematic outside the ship is in space, its second). Its fog's
colour, or where that is dark (no fog: space), what the sky's model
averages to (model_color: black for space, the clouds' for a sky of
them). A map with its own gradient (dos_skies) keeps it. */
static void make_sky_gradient(unsigned long horizon, unsigned long overhead);

void dos_level_set_sky(short sky_index, unsigned long model_color, int has_model)
{
	struct sky *sky;

	if (level.sky_colors || (level.sky_set && sky_index == level.sky_index))
		return;
	level.sky_set = TRUE;
	level.sky_index = sky_index;
	sky = sky_index != NONE && global_scenario_get() ? scenario_get_sky(sky_index) : NULL;
	if (sky)
	{
		real_rgb_color const *color = &sky->outdoor_fog.color;
		unsigned long r = (unsigned long)(PIN(color->red, 0.0f, 1.0f) * 255.0f);
		unsigned long g = (unsigned long)(PIN(color->green, 0.0f, 1.0f) * 255.0f);
		unsigned long b = (unsigned long)(PIN(color->blue, 0.0f, 1.0f) * 255.0f);

		if (r + g + b > 48)
		{
			level.sky_color = (r << 16) | (g << 8) | b;
			/* (where the sky's model is not drawn, SKY: GRADIENT: from the
			fog's colour at the horizon to the model's overhead) */
			level.sky_gradient_made = FALSE;
			if (has_model && model_color)
				make_sky_gradient(level.sky_color, model_color);
			return;
		}
	}
	level.sky_gradient_made = FALSE;
	if (has_model)
		level.sky_color = model_color ? model_color : 0x010101;
}

const unsigned long *dos_level_sky_colors(void)
{
	return level.sky_colors;
}

const unsigned long *dos_level_sky_gradient(void)
{
	return level.sky_colors ? level.sky_colors : level.sky_gradient_made ? level.sky_gradient : NULL;
}

/* the gradient from the horizon's colour to the one overhead, its stops
spaced as Blood Gulch's (dos_skies) */
static void make_sky_gradient(unsigned long horizon, unsigned long overhead)
{
	static const int shares[R_SKY_STOPS] = { 0, 85, 150, 215, 256 };

	for (int i = 0; i < R_SKY_STOPS; i++)
	{
		unsigned long color = 0;

		for (int shift = 0; shift <= 16; shift += 8)
		{
			long a = (long)((horizon >> shift) & 0xFF), b = (long)((overhead >> shift) & 0xFF);

			color |= (unsigned long)(a + (b - a) * shares[i] / 256) << shift;
		}
		level.sky_gradient[i] = color;
	}
	level.sky_gradient_made = TRUE;
}

float dos_level_fog_start(void)
{
	return level.fog_start;
}

/* Halo's texture pass over a batch, as the Xbox's (Direct3D 8,
dos_xbox_detail): drawn after every opaque batch, as Halo draws it after
all its lightmap passes, so that each pixel's nearest surface is there
already (the card gives a pixel its pass once); everywhere, the detail
maps' far levels being grey */
static void draw_xbox_detail(const struct batch *batch, int coarse)
{
	if (coarse || !batch->material->xbox_detail.stage_count || batch->material->xbox_detail.replace)
		return;
	r_set_detail_pass(&batch->material->xbox_detail);
	r_draw_triangles(batch->vertices, batch->vertex_count, batch->indices, batch->triangle_count, NULL,
		batch->texture, batch->material->flags & R_DRAW_TWO_SIDED);
}

static void draw_batch(const struct batch *batch, int coarse)
{
	if (coarse)
	{
		r_draw_triangles(batch->coarse_vertices, batch->coarse_vertex_count, batch->coarse_indices,
			batch->coarse_triangle_count, NULL, batch->coarse_texture ? batch->coarse_texture : batch->texture,
			batch->material->flags);
		return;
	}
	/* (times its lightmap page, on the Xbox's look; a chicago shader's
	stack on the card's stages instead of its texture) */
	if (batch->lightmap)
		r_set_lightmap(batch->lightmap, batch->lightmap_coordinates);
	if (batch->material->xbox_detail.replace && batch->material->xbox_detail.stage_count)
		r_set_detail_pass(&batch->material->xbox_detail);
	r_draw_triangles(batch->vertices, batch->vertex_count, batch->indices, batch->triangle_count, NULL,
		batch->texture, batch->material->flags);
	/* (its detail maps over it, up close) */
	if ((batch->detail_vertices[0] || batch->detail_vertices[1]) &&
		r_distance(batch->center) - batch->radius < DETAIL_DISTANCE)
	{
		for (int k = 0; k < 2; k++)
		{
			if (batch->detail_vertices[k])
			{
				r_draw_triangles(batch->detail_vertices[k], batch->vertex_count, batch->indices,
					batch->triangle_count, NULL, batch->material->detail[k],
					R_DRAW_DETAIL | (batch->material->flags & R_DRAW_TWO_SIDED));
			}
		}
	}
}

/* The opaque batches front to back, so that the nearer fill the depth
buffer first and the pixels behind them are refused before they are
textured (Blood Gulch's hills draw over one another: about twice the
screen's pixels a frame). Sorted by bucket, by the distance to each one's
near side in steps of SORT_STEP world units: two passes, no comparisons. */
#define SORT_BUCKETS 256
#define SORT_STEP 2.0f

static struct
{
	/* each visible batch: its index times 2, plus 1 for its coarse copy */
	int *entries, *sorted;
	unsigned char *buckets;
	int size;
} sort;

static void draw_batches(unsigned long const *visible_clusters, short cluster_count, int transparent)
{
	int count = 0, starts[SORT_BUCKETS + 1];

	if (!transparent && sort.size < level.batch_count)
	{
		/* (the C library's: the game's free stops at a null) */
		raw_free(sort.entries);
		raw_free(sort.sorted);
		raw_free(sort.buckets);
		sort.entries = raw_malloc((size_t)level.batch_count * sizeof(int));
		sort.sorted = raw_malloc((size_t)level.batch_count * sizeof(int));
		sort.buckets = raw_malloc((size_t)level.batch_count);
		sort.size = sort.entries && sort.sorted && sort.buckets ? level.batch_count : 0;
	}
	for (int c = 0; c < level.cluster_count; c++)
	{
		if (visible_clusters && c < cluster_count && !(visible_clusters[c >> 5] & (1UL << (c & 31))))
			continue;
		for (int b = level.clusters[c].first_batch; b < level.clusters[c].first_batch + level.clusters[c].batch_count; b++)
		{
			struct batch *batch = &level.batches[b];
			float near_side;
			int coarse;

			if (batch->material->transparent != transparent || !r_sphere_visible(batch->center, batch->radius))
				continue;
			near_side = r_distance(batch->center) - batch->radius;
			coarse = batch->coarse_vertices && near_side > lod.distance;
			if (transparent || !sort.size)
			{
				draw_batch(batch, coarse);
				draw_xbox_detail(batch, coarse);
				continue;
			}
			{
				int bucket = near_side > 0.0f ? (int)(near_side * (1.0f / SORT_STEP)) : 0;

				sort.buckets[count] = (unsigned char)(bucket < SORT_BUCKETS - 1 ? bucket : SORT_BUCKETS - 1);
				sort.entries[count++] = b * 2 + coarse;
			}
		}
	}
	if (transparent || !count)
		return;
	memset(starts, 0, sizeof(starts));
	for (int i = 0; i < count; i++)
		starts[sort.buckets[i] + 1]++;
	for (int k = 0; k < SORT_BUCKETS; k++)
		starts[k + 1] += starts[k];
	for (int i = 0; i < count; i++)
		sort.sorted[starts[sort.buckets[i]]++] = sort.entries[i];
	for (int i = 0; i < count; i++)
		draw_batch(&level.batches[sort.sorted[i] >> 1], sort.sorted[i] & 1);
	for (int i = 0; i < count; i++)
		draw_xbox_detail(&level.batches[sort.sorted[i] >> 1], sort.sorted[i] & 1);
}

void dos_level_draw(unsigned long const *visible_clusters, short cluster_count)
{
	draw_batches(visible_clusters, cluster_count, FALSE);
}

/* its water and glass: after the objects (they write no depth, so what is
behind them must be drawn first) */
void dos_level_draw_transparent(unsigned long const *visible_clusters, short cluster_count)
{
	draw_batches(visible_clusters, cluster_count, TRUE);
}
