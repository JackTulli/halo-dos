/*
R_D3D.C

The Direct3D 8 rasterizer (r_d3d.h): what the software rasterizer draws,
drawn by the graphics card through Direct3D 8, for Windows 95 and 98
machines with DirectX 8 (the setting renderer = direct3d). It is the
OpenGL rasterizer's (r_gl.c) twin, and draws as it does:
	- the front end (r_main.c) culls, transforms, lights, fogs and clips;
	  its screen-space triangles come here (r_internal.h's seam) and each
	  vertex goes to the card in clip space (x z, y z, z - near, z), z the
	  view's depth: the card divides by z again, so the texture is
	  interpolated with its perspective, and the depth it stores is
	  1 - near / z (nearer is less);
	- light: the vertex's colour times twice the texel (MODULATE2X), 0x80
	  the texel as it is;
	- fog: linear in the view's depth, the vertex's factor worked out here
	  and blended by the card, the colour given back its unfogged light;
	- the first-person weapon in front of the world (r_set_in_front): the
	  first tenth of the depth range, the world the rest;
	- added, half and by-alpha blends, the screen's four, alpha-tested
	  textures;
	- detail maps (R_DRAW_DETAIL): the same triangles again, depth equal,
	  the picture times twice the texel (DESTCOLOR, SRCCOLOR), the texel
	  toward grey by the vertex's alpha (BLENDDIFFUSEALPHA, the vertex's
	  colour the grey: a texture factor came out black on a card), fogged
	  toward grey;
	- the Xbox's texture pass (R_RASTER_XBOX_DETAIL, r_set_detail_pass):
	  the same triangles again, depth equal, its stages (the detail maps
	  mixed by an alpha, the micro detail) on up to four textures at once,
	  each at the base map's coordinates times its scale (the device's
	  program scales them), the picture times twice what they give (or
	  once), fogged toward no change; or (a chicago shader's stack of maps,
	  r_detail_pass's replace) the stages instead of the triangle's texture,
	  blended as its flags say, as Halo's PC renderer drew those shaders;
	- the 2D helpers (text's glyphs, pictures, rectangles): quads, a
	  glyph's coverage a texture of its own (white, the coverage its alpha).
Triangles are gathered while the state stays the same and drawn together,
as indices into the batch's vertices: a corner the front end hands again
(most of a mesh's are shared by several triangles) goes to the card once.
Textures go to the card as they are made, in their full colours, as large
as TEXTURE QUALITY lets them, every level down to 1x1; TEXTURE FILTER
SMOOTH or DITHERED filters them trilinearly (anisotropically where the card
can), SHARP keeps texels squares.

Direct3D's pixels have their centres on whole coordinates, the picture's
on halves: a vertex at the picture's x goes to the back buffer's
x * scale - 0.5.
*/

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "r_internal.h"
#include "r_d3d.h"

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

/* the depth range's share of the first-person weapon (r_set_in_front) */
#define IN_FRONT_DEPTH 0.1f

/* triangles gathered before they are drawn: their vertices, and three
indices a triangle into them */
#define BATCH_VERTICES (3 * 1024)
#define BATCH_INDICES (6 * 1024)

/* the most corners a fogged triangle's cuts make (fogged_triangle) */
#define FOG_POLYGON_MAXIMUM 8

/* the room a triangle may take in the batch: its three corners, and the
pieces the fog may cut it into (three polygons, each a fan) */
#define TRIANGLE_VERTICES (3 + 3 * FOG_POLYGON_MAXIMUM)
#define TRIANGLE_INDICES (3 * 3 * (FOG_POLYGON_MAXIMUM - 2))

/* The front end hands a draw's triangles their corners from one array (the
draw's vertices, each projected once; a corner the near plane cut, from
the stack): a corner handed again within the batch, with the same values,
is the same vertex of the card's, only its index added again. Looked up
by its address (a draw's consecutive vertices in consecutive places), its
values compared (the array is the next draw's too). */
#define CORNER_CACHE 1024

struct corner_key
{
	/* (the bits of its x, y, w, s, t, r, g, b, fog, lightmap_s, lightmap_t;
	and of the colour's alpha, which the draw's flags give) */
	unsigned long x, y, w, s, t, r, g, b, fog, lightmap_s, lightmap_t, alpha;
};

struct cached_corner
{
	const struct r_raster_vertex *source;
	unsigned long batch;
	struct corner_key key;
	/* its fog factor, before it is clamped (whether a triangle is cut) */
	float fog;
	unsigned long index;
};

/* (corner_index reads a vertex's floats as words by their places) */
typedef char raster_vertex_words[offsetof(struct r_raster_vertex, r) == 7 * sizeof(float) &&
	offsetof(struct r_raster_vertex, fog) == 14 * sizeof(float) &&
	offsetof(struct r_raster_vertex, lightmap_t) == 16 * sizeof(float) ? 1 : -1];

/* how a batch's corners take their colour (constant in a batch: its state's
texture says) */
enum
{
	_corner_light, /* the vertex's light, given back what the front end's fog took */
	_corner_detail, /* a detail map's: grey, the vertex's strength its alpha */
	_corner_white /* the Xbox's texture pass's */
};

/* a texture's hardware handle where the card would not take it: drawn
without it, and not made again at each drawing (its full colours are gone
with the first try) */
#define REFUSED_TEXTURE 1UL

static struct
{
	int active;
	struct r_d3d_device device;
	/* the picture in the back buffer (set at the next frame's start:
	changed) */
	int viewport_x, viewport_y, viewport_width, viewport_height, window_width, window_height;
	int viewport_changed;
	/* the rectangle of the picture the triangles go into (the whole
	picture, or a split screen's view), and the card's viewport for it */
	int rect_x0, rect_y0, rect_x1, rect_y1;
	float card_x, card_y, card_width, card_height;
	/* the picture's x and y to clip space before the times z: x times the
	scale plus the offset (card_viewport) */
	double clip_x_scale, clip_x_offset, clip_y_scale, clip_y_offset;
	float z_near;
	int in_front;
	unsigned long alpha_scale;
	int screen_blend;
	int fogged;
	float fog_start, fog_end;
	/* (one over the fog's span, if it is not nothing) */
	double fog_scale;
	int fog_span;
	unsigned long fog_color;
	int smooth;
	/* the batch: its state, texture (and lightmap, or the Xbox's texture
	pass's stages) and vertices */
	struct r_d3d_state state;
	struct r_d3d_texture *texture, *lightmap;
	struct r_d3d_detail detail;
	/* (the pass g.detail was made from) */
	const struct r_detail_pass *detail_source;
	struct r_d3d_vertex vertices[BATCH_VERTICES];
	unsigned short indices[BATCH_INDICES];
	unsigned long count, index_count;
	/* the batch's number (its corners in the cache are its), and the
	corners (corner_index) */
	unsigned long batch;
	struct cached_corner corners[CORNER_CACHE];
	/* the pictures' textures (d3d_picture): each in tiles of a size the
	card takes, columns x rows of them (one where the card takes the
	picture whole) */
	struct
	{
		const unsigned long *argb;
		int width, height;
		unsigned long check;
		struct r_d3d_texture **tiles;
		int columns, rows, tile_width, tile_height;
		unsigned long used;
	} pictures[4];
	unsigned long clock;
} g;

static int power_of_two(int value)
{
	int result = 1;

	while (result < value)
		result <<= 1;
	return result;
}

/* the value rounded to 0-255: (unsigned long)(value + 0.5) between, as the
x87 makes it, worked out from the float's bits (the x87's conversion takes
two loads of its control word, each a stall on the Pentiums); the same for
every float (all 2^32 checked against the x87's) */
static unsigned long clamp_byte(float value)
{
	unsigned long bits, exponent;

	memcpy(&bits, &value, sizeof(bits));
	/* (below 0; 255 up and infinity; not a number, which the x87 gave 0) */
	if (bits & 0x80000000UL)
		return 0;
	if (bits >= 0x437F0000UL)
		return bits > 0x7F800000UL ? 0 : 255;
	/* (below a half) */
	if (bits < 0x3F000000UL)
		return 0;
	/* (the mantissa, its point 150 - exponent bits up, plus a half) */
	exponent = bits >> 23;
	return (((bits & 0x7FFFFFUL) | 0x800000UL) + (1UL << (149 - exponent))) >> (150 - exponent);
}

static unsigned long argb(float red, float green, float blue, float alpha)
{
	return (clamp_byte(alpha) << 24) | (clamp_byte(red) << 16) | (clamp_byte(green) << 8) | clamp_byte(blue);
}

void r_d3d_flush(void)
{
	if (g.active && g.index_count)
	{
		g.device.draw(&g.state, g.texture, g.state.lightmap ? g.lightmap : NULL,
			g.state.texture == _r_d3d_xbox_detail ? &g.detail : NULL, g.vertices, g.count, g.indices, g.index_count);
	}
	g.count = 0;
	g.index_count = 0;
	/* (its corners are no longer the next batch's) */
	g.batch++;
}

/* ---------- textures */

/* the texture's largest level as ARGB (its full colours, or its 8-bit
level 0 through its palette), its size; NULL without memory */
static unsigned long *texture_colours(struct r_texture *texture, int *width, int *height)
{
	int alpha = (texture->flags & R_TEXTURE_ALPHA) != 0, alpha_test = (texture->flags & R_TEXTURE_ALPHA_TEST) != 0;
	unsigned long *colours;

	if (texture->argb)
	{
		/* (its own colours, taken: the card keeps them now) */
		colours = texture->argb;
		*width = texture->argb_width;
		*height = texture->argb_height;
		texture->argb = NULL;
		for (long i = 0; i < (long)*width * *height; i++)
		{
			unsigned long color = colours[i];

			colours[i] = (color & 0x00FFFFFFUL) |
				(alpha_test ? ((color >> 24) >= 0x80 ? 0xFF000000UL : 0) : alpha ? color & 0xFF000000UL : 0xFF000000UL);
		}
		return colours;
	}
	*width = 1 << texture->width_shift;
	*height = 1 << texture->height_shift;
	colours = malloc((size_t)*width * *height * sizeof(*colours));
	if (!colours)
		return NULL;
	for (long i = 0; i < (long)*width * *height; i++)
	{
		unsigned char texel = texture->texels[0][i];
		unsigned long color = texture->palette[texel];

		colours[i] = (color & 0x00FFFFFFUL) |
			(alpha_test && !texel ? 0 : alpha ? color & 0xFF000000UL : 0xFF000000UL);
	}
	return colours;
}

/* the next level down, averaged (in place: the level's first quarter) */
static void halve(unsigned long *colours, int *width, int *height)
{
	int w = *width > 1 ? *width / 2 : 1, h = *height > 1 ? *height / 2 : 1;

	for (int y = 0; y < h; y++)
	{
		for (int x = 0; x < w; x++)
		{
			int x0 = *width > 1 ? 2 * x : x, y0 = *height > 1 ? 2 * y : y;
			int x1 = *width > 1 ? x0 + 1 : x0, y1 = *height > 1 ? y0 + 1 : y0;
			unsigned long a = colours[y0 * *width + x0], b = colours[y0 * *width + x1];
			unsigned long c = colours[y1 * *width + x0], d = colours[y1 * *width + x1];
			unsigned long result = 0;

			for (int shift = 0; shift < 32; shift += 8)
			{
				unsigned long sum = ((a >> shift) & 0xFF) + ((b >> shift) & 0xFF) + ((c >> shift) & 0xFF) +
					((d >> shift) & 0xFF);

				result |= ((sum + 2) / 4) << shift;
			}
			colours[y * w + x] = result;
		}
	}
	*width = w;
	*height = h;
}

static int is_power_of_two(int value)
{
	return value > 0 && !(value & (value - 1));
}

/* the shape the card takes for a texture of width x height, its longer
side kept: square where it takes no other, the shorter side widened as far
as the widest shape it takes needs (a Voodoo's 8:1) */
static void card_shape(int *width, int *height)
{
	if (g.device.square)
		*width = *height = *width > *height ? *width : *height;
	if (g.device.maximum_aspect > 0)
	{
		while (*width > *height * g.device.maximum_aspect)
			*height *= 2;
		while (*height > *width * g.device.maximum_aspect)
			*width *= 2;
	}
}

/* the texels at width x height, each between the four nearest (round the
edges where the texture repeats, else held at them); NULL without memory */
static unsigned long *resample(const unsigned long *in, int in_width, int in_height, int width, int height, int wrap)
{
	unsigned long *out = malloc((size_t)width * height * sizeof(*out));

	if (!out)
		return NULL;
	for (int y = 0; y < height; y++)
	{
		float sy = ((float)y + 0.5f) * (float)in_height / (float)height - 0.5f;
		int y0 = sy >= 0.0f ? (int)sy : (int)sy - 1, y1 = y0 + 1;
		float fy = sy - (float)y0;

		y0 = wrap ? ((y0 % in_height) + in_height) % in_height : y0 < 0 ? 0 : y0;
		y1 = wrap ? y1 % in_height : y1 >= in_height ? in_height - 1 : y1;
		for (int x = 0; x < width; x++)
		{
			float sx = ((float)x + 0.5f) * (float)in_width / (float)width - 0.5f;
			int x0 = sx >= 0.0f ? (int)sx : (int)sx - 1, x1 = x0 + 1;
			float fx = sx - (float)x0;
			unsigned long a, b, c, d, result = 0;

			x0 = wrap ? ((x0 % in_width) + in_width) % in_width : x0 < 0 ? 0 : x0;
			x1 = wrap ? x1 % in_width : x1 >= in_width ? in_width - 1 : x1;
			a = in[(size_t)y0 * in_width + x0];
			b = in[(size_t)y0 * in_width + x1];
			c = in[(size_t)y1 * in_width + x0];
			d = in[(size_t)y1 * in_width + x1];
			for (int shift = 0; shift < 32; shift += 8)
			{
				float top = (float)((a >> shift) & 0xFF) + (float)((long)((b >> shift) & 0xFF) - (long)((a >> shift) & 0xFF)) * fx;
				float bottom = (float)((c >> shift) & 0xFF) + (float)((long)((d >> shift) & 0xFF) - (long)((c >> shift) & 0xFF)) * fx;

				result |= clamp_byte(top + (bottom - top) * fy) << shift;
			}
			out[(size_t)y * width + x] = result;
		}
	}
	return out;
}

/* A texture's largest level made one the card takes: resampled to powers
of two where it takes no other sides, averaged down to its largest side,
widened to the shapes it takes (the coordinates go from 0 to 1 whatever
the size, so drawing is the same). The texels themselves, or new ones in
their place (the old freed), the size changed to match; NULL without
memory. */
static unsigned long *card_texels(unsigned long *colours, int *width, int *height, int wrap)
{
	int shaped_width, shaped_height;

	if ((g.device.power_of_two || g.device.square) && (!is_power_of_two(*width) || !is_power_of_two(*height)))
	{
		unsigned long *resized = resample(colours, *width, *height, power_of_two(*width), power_of_two(*height), wrap);

		free(colours);
		if (!resized)
			return NULL;
		colours = resized;
		*width = power_of_two(*width);
		*height = power_of_two(*height);
	}
	/* (larger than the card takes: averaged down) */
	while (*width > g.device.maximum_texture_size || *height > g.device.maximum_texture_size)
		halve(colours, width, height);
	shaped_width = *width;
	shaped_height = *height;
	card_shape(&shaped_width, &shaped_height);
	if (shaped_width != *width || shaped_height != *height)
	{
		unsigned long *resized = resample(colours, *width, *height, shaped_width, shaped_height, wrap);

		free(colours);
		if (!resized)
			return NULL;
		colours = resized;
		*width = shaped_width;
		*height = shaped_height;
	}
	return colours;
}

static void texture_upload(struct r_texture *texture)
{
	int width, height, levels = 1, level;
	int alpha = (texture->flags & R_TEXTURE_ALPHA) ? _r_d3d_alpha_full :
		(texture->flags & R_TEXTURE_ALPHA_TEST) ? _r_d3d_alpha_test : _r_d3d_alpha_none;
	unsigned long *colours = texture_colours(texture, &width, &height);
	struct r_d3d_texture *card;

	if (!colours)
		return;
	colours = card_texels(colours, &width, &height, !(texture->flags & R_TEXTURE_CLAMP));
	if (!colours)
		return;
	for (int w = width, h = height; w > 1 || h > 1; w = w > 1 ? w / 2 : 1, h = h > 1 ? h / 2 : 1)
		levels++;
	card = g.device.texture_new(width, height, levels, alpha);
	if (card)
	{
		for (level = 0; level < levels; level++)
		{
			g.device.texture_write(card, level, colours);
			if (level + 1 < levels)
				halve(colours, &width, &height);
		}
	}
	free(colours);
	texture->hardware = card ? (unsigned long)card : REFUSED_TEXTURE;
}

/* a texture just made (r_texture.c): to the card now, in its full colours,
which it then gives up */
static void d3d_texture_created(struct r_texture *texture)
{
	if (!g.active || texture->hardware)
		return;
	texture_upload(texture);
}

static int d3d_draws_detail(void)
{
	return g.active && g.device.draws_detail;
}

/* a lightmap page (r_lightmap_new): every level down to 1x1, as a texture;
0 where the card has no second texture stage */
static unsigned long d3d_lightmap_new(const unsigned long *argb_texels, int width, int height)
{
	unsigned long *colours;
	struct r_d3d_texture *card;
	int levels = 1;

	if (!g.active || !g.device.draws_lightmaps)
		return 0;
	colours = malloc((size_t)width * height * sizeof(*colours));
	if (!colours)
		return 0;
	for (long i = 0; i < (long)width * height; i++)
		colours[i] = argb_texels[i] | 0xFF000000UL;
	colours = card_texels(colours, &width, &height, FALSE);
	if (!colours)
		return 0;
	for (int w = width, h = height; w > 1 || h > 1; w = w > 1 ? w / 2 : 1, h = h > 1 ? h / 2 : 1)
		levels++;
	r_d3d_flush();
	card = g.device.texture_new(width, height, levels, _r_d3d_alpha_none);
	for (int level = 0; card && level < levels; level++)
	{
		g.device.texture_write(card, level, colours);
		if (level + 1 < levels)
			halve(colours, &width, &height);
	}
	free(colours);
	return (unsigned long)card;
}

/* a texture of the Xbox's texture pass (r_hardware_texture_new): the levels
given (those larger than the card takes left out), then each further one
averaged from the last, down to 1x1 */
static int d3d_detail_stages(void)
{
	return g.active ? g.device.detail_stages : 0;
}

static unsigned long d3d_hardware_texture_new(unsigned long *const *levels, const int *widths, const int *heights,
	int count, int alpha)
{
	struct r_d3d_texture *card;
	unsigned long *colours;
	int first = 0, width, height, chain = 1, reshaped;

	if (!g.active || count <= 0)
		return 0;
	while (first + 1 < count && (widths[first] > g.device.maximum_texture_size ||
		heights[first] > g.device.maximum_texture_size))
	{
		first++;
	}
	width = widths[first];
	height = heights[first];
	colours = malloc((size_t)width * height * sizeof(*colours));
	if (!colours)
		return 0;
	for (long i = 0; i < (long)width * height; i++)
		colours[i] = alpha ? levels[first][i] : levels[first][i] | 0xFF000000UL;
	/* (a shape the card does not take: resampled, its levels then averaged
	from that, not the map's) */
	colours = card_texels(colours, &width, &height, TRUE);
	if (!colours)
		return 0;
	reshaped = width != widths[first] || height != heights[first];
	for (int w = width, h = height; w > 1 || h > 1; w = w > 1 ? w / 2 : 1, h = h > 1 ? h / 2 : 1)
		chain++;
	r_d3d_flush();
	card = g.device.texture_new(width, height, chain, alpha ? _r_d3d_alpha_full : _r_d3d_alpha_none);
	for (int level = 0; card && level < chain; level++)
	{
		int given = first + level;

		/* (a level the map stores, where it is the size the chain needs) */
		if (level && !reshaped && given < count && widths[given] == width && heights[given] == height)
		{
			for (long i = 0; i < (long)width * height; i++)
				colours[i] = alpha ? levels[given][i] : levels[given][i] | 0xFF000000UL;
		}
		g.device.texture_write(card, level, colours);
		if (level + 1 < chain)
			halve(colours, &width, &height);
	}
	free(colours);
	return (unsigned long)card;
}

static void d3d_hardware_texture_write(unsigned long texture, int level, const unsigned long *argb)
{
	if (!g.active || !texture || !argb)
		return;
	r_d3d_flush();
	g.device.texture_write((struct r_d3d_texture *)texture, level, argb);
}

static unsigned long d3d_hardware_cube_new(unsigned long *const *faces[6], int size, int levels)
{
	struct r_d3d_texture *cube;

	if (!g.active || !g.device.cube_new || !g.device.cube_write || size <= 0 || levels <= 0)
		return 0;
	r_d3d_flush();
	cube = g.device.cube_new(size, levels);
	for (int face = 0; cube && face < 6; face++)
	{
		for (int level = 0; level < levels; level++)
		{
			if (faces[face] && faces[face][level])
				g.device.cube_write(cube, face, level, faces[face][level]);
		}
	}
	return (unsigned long)cube;
}

static int d3d_draws_water(void)
{
	return g.active && g.device.draw_water ? g.device.water : 0;
}

/* Halo's water: each vertex into clip space as the triangles' are (no
division: the card clips and divides), its sets for the card's program */
static void d3d_water(const struct r_water *water, const struct r_water_vertex *vertices, int vertex_count,
	const unsigned short *indices, int triangle_count)
{
	static struct r_d3d_water_vertex *out;
	static int out_size;
	struct r_d3d_water card;

	if (!g.active || !g.device.draw_water || !g.device.water || !water->bump || !water->reflection || !water->tint)
		return;
	if (vertex_count > out_size)
	{
		struct r_d3d_water_vertex *grown = realloc(out, (size_t)vertex_count * sizeof(*grown));

		if (!grown)
			return;
		out = grown;
		out_size = vertex_count;
	}
	r_d3d_flush();
	for (int i = 0; i < vertex_count; i++)
	{
		const struct r_water_vertex *v = &vertices[i];
		struct r_d3d_water_vertex *o = &out[i];
		const float *normal = &v->frame[0], *binormal = &v->frame[3], *tangent = &v->frame[6];

		memset(o, 0, sizeof(*o));
		o->x = (float)((double)v->xz * g.clip_x_scale + g.clip_x_offset * (double)v->z);
		o->y = (float)((double)v->yz * g.clip_y_scale + g.clip_y_offset * (double)v->z);
		o->z = v->z - g.z_near;
		o->w = v->z;
		o->color = (clamp_byte(v->fade * 255.0f) << 24) | 0xFFFFFFUL;
		o->t0[0] = v->u * water->bump_scale[0] + water->bump_offset[0];
		o->t0[1] = v->v * water->bump_scale[1] + water->bump_offset[1];
		o->t5[0] = v->u;
		o->t5[1] = v->v;
		if (g.device.water >= 2)
		{
			for (int k = 0; k < 3; k++)
			{
				o->t1[k] = v->eye[k];
				o->t2[k] = tangent[k];
				o->t3[k] = binormal[k];
				o->t4[k] = normal[k];
			}
		}
		else
		{
			/* (the rows of the matrix to the world, the eye's way in w) */
			float *rows[3] = { o->t1, o->t2, o->t3 };

			for (int k = 0; k < 3; k++)
			{
				rows[k][0] = tangent[k];
				rows[k][1] = binormal[k];
				rows[k][2] = normal[k];
				rows[k][3] = v->eye[k];
			}
		}
	}
	memset(&card, 0, sizeof(card));
	card.program = g.device.water;
	card.bump = (struct r_d3d_texture *)water->bump;
	card.reflection = (struct r_d3d_texture *)water->reflection;
	card.tint = (struct r_d3d_texture *)water->tint;
	card.base = (struct r_d3d_texture *)water->base;
	card.bump_lod_bias = water->bump_lod_bias;
	card.tint_color = water->tint_color;
	card.base_alpha = water->base_alpha && water->base;
	card.base_multiply = water->base_multiply && water->base;
	g.device.draw_water(&card, out, (unsigned long)vertex_count, indices, (unsigned long)triangle_count * 3);
}

static void d3d_hardware_texture_delete(unsigned long texture)
{
	if (!g.active || !texture)
		return;
	r_d3d_flush();
	g.detail_source = NULL;
	g.device.texture_delete((struct r_d3d_texture *)texture);
}

static void d3d_lightmap_delete(unsigned long lightmap)
{
	if (!g.active || !lightmap)
		return;
	if (g.lightmap == (struct r_d3d_texture *)lightmap)
	{
		r_d3d_flush();
		g.lightmap = NULL;
	}
	g.device.texture_delete((struct r_d3d_texture *)lightmap);
}

static void d3d_texture_delete(struct r_texture *texture)
{
	if (!g.active || !texture->hardware)
		return;
	if (texture->hardware == REFUSED_TEXTURE)
	{
		texture->hardware = 0;
		return;
	}
	if (g.texture == (struct r_d3d_texture *)texture->hardware)
	{
		r_d3d_flush();
		g.texture = NULL;
	}
	g.device.texture_delete((struct r_d3d_texture *)texture->hardware);
	texture->hardware = 0;
}

/* ---------- the viewport */

static const struct r_backend d3d_backend;

int r_d3d_use(const struct r_d3d_device *device)
{
	if (!device || !device->texture_new || !device->texture_write || !device->texture_delete || !device->viewport ||
		!device->clear || !device->draw || !device->read_back)
	{
		return FALSE;
	}
	memset(&g, 0, sizeof(g));
	g.device = *device;
	if (g.device.maximum_texture_size <= 0)
		g.device.maximum_texture_size = 256;
	g.active = TRUE;
	g.batch = 1;
	g.alpha_scale = 256;
	g.z_near = 0.0625f;
	g.viewport_width = g.viewport_height = 1;
	g.viewport_changed = TRUE;
	g.smooth = r_frame.settings.texture_smooth || r_frame.settings.texture_dither;
	r_backend = &d3d_backend;
	return TRUE;
}

void r_d3d_viewport(int x, int y, int width, int height, int window_width, int window_height)
{
	if (!g.active)
		return;
	if (x != g.viewport_x || y != g.viewport_y || width != g.viewport_width || height != g.viewport_height)
		g.viewport_changed = TRUE;
	g.viewport_x = x;
	g.viewport_y = y;
	g.viewport_width = width > 0 ? width : 1;
	g.viewport_height = height > 0 ? height : 1;
	g.window_width = window_width;
	g.window_height = window_height;
}

/* the picture's x and y in the back buffer */
static float back_x(float x)
{
	return (float)g.viewport_x + x * (float)g.viewport_width / (float)r_frame.width;
}

static float back_y(float y)
{
	return (float)g.viewport_y + y * (float)g.viewport_height / (float)r_frame.height;
}

/* the card's viewport: the rectangle's pixels of the back buffer, and the
depth range the world or the first-person weapon takes */
static void card_viewport(void)
{
	int x0 = (int)(back_x((float)g.rect_x0) + 0.5f), y0 = (int)(back_y((float)g.rect_y0) + 0.5f);
	int x1 = (int)(back_x((float)g.rect_x1) + 0.5f), y1 = (int)(back_y((float)g.rect_y1) + 0.5f);

	r_d3d_flush();
	if (x1 <= x0)
		x1 = x0 + 1;
	if (y1 <= y0)
		y1 = y0 + 1;
	g.card_x = (float)x0;
	g.card_y = (float)y0;
	g.card_width = (float)(x1 - x0);
	g.card_height = (float)(y1 - y0);
	{
		/* (back_x's and the rectangle's in one: no divisions a vertex) */
		double kx = 2.0 / (double)g.card_width, ky = 2.0 / (double)g.card_height;

		g.clip_x_scale = (double)g.viewport_width / (double)r_frame.width * kx;
		g.clip_x_offset = ((double)g.viewport_x - 0.5 - (double)g.card_x) * kx - 1.0;
		g.clip_y_scale = -((double)g.viewport_height / (double)r_frame.height * ky);
		g.clip_y_offset = 1.0 - ((double)g.viewport_y - 0.5 - (double)g.card_y) * ky;
	}
	g.device.viewport(x0, y0, x1 - x0, y1 - y0, g.in_front ? 0.0f : IN_FRONT_DEPTH, g.in_front ? IN_FRONT_DEPTH : 1.0f);
}

/* a vertex at the picture's x, y, at depth z (1 for the screen's) */
static void clip_position(struct r_d3d_vertex *vertex, float x, float y, float z, float depth)
{
	vertex->x = (float)(((double)x * g.clip_x_scale + g.clip_x_offset) * (double)z);
	vertex->y = (float)(((double)y * g.clip_y_scale + g.clip_y_offset) * (double)z);
	vertex->z = depth;
	vertex->w = z;
}

static void d3d_read_back(void)
{
	unsigned long *rgb;

	if (!g.active || !r_frame.pixels || r_frame.bits != 32)
		return;
	r_d3d_flush();
	rgb = malloc((size_t)g.viewport_width * g.viewport_height * sizeof(*rgb));
	if (!rgb)
		return;
	if (g.device.read_back(g.viewport_x, g.viewport_y, g.viewport_width, g.viewport_height, rgb))
	{
		/* (the viewport's pixels at the picture's size) */
		for (int y = 0; y < r_frame.height; y++)
		{
			unsigned long *row = (unsigned long *)(r_frame.pixels + (size_t)y * r_frame.pitch);
			int source_y = (int)((long)y * g.viewport_height / r_frame.height);

			for (int x = 0; x < r_frame.width; x++)
				row[x] = rgb[(size_t)source_y * g.viewport_width + (size_t)((long)x * g.viewport_width / r_frame.width)];
		}
	}
	free(rgb);
}

/* ---------- the seam */

static void d3d_set_near(float z_near)
{
	if (z_near > 0.0f && z_near != g.z_near)
	{
		r_d3d_flush();
		g.z_near = z_near;
	}
}

static void d3d_set_in_front(int in_front)
{
	if (!g.active || in_front == g.in_front)
		return;
	g.in_front = in_front;
	card_viewport();
}

static void d3d_set_alpha_scale(unsigned long scale)
{
	g.alpha_scale = scale > 256 ? 256 : scale;
}

static void d3d_screen_blend(int blend)
{
	g.screen_blend = blend;
}

static void d3d_view_fog(int fogged, float fog_start, float fog_end, unsigned long fog_color)
{
	if (!g.active)
		return;
	r_d3d_flush();
	g.fogged = fogged;
	g.fog_start = fog_start;
	g.fog_end = fog_end;
	g.fog_span = fog_end - fog_start > 1.0e-6f;
	g.fog_scale = g.fog_span ? 1.0 / ((double)fog_end - (double)fog_start) : 0.0;
	g.fog_color = fog_color;
}

static void d3d_clear(unsigned long color)
{
	if (!g.active)
		return;
	r_d3d_flush();
	g.smooth = r_frame.settings.texture_smooth || r_frame.settings.texture_dither;
	g.viewport_changed = FALSE;
	/* (a pass's stages are made again each frame: a map's passes go with it) */
	g.detail_source = NULL;
	/* (the back buffer's borders black, the picture the colour: one clear
	where the picture is the whole back buffer) */
	if (g.viewport_x <= 0 && g.viewport_y <= 0 && g.viewport_x + g.viewport_width >= g.window_width &&
		g.viewport_y + g.viewport_height >= g.window_height)
	{
		g.device.clear(0, 0, g.window_width, g.window_height, TRUE, TRUE, color);
	}
	else
	{
		g.device.clear(0, 0, g.window_width, g.window_height, TRUE, TRUE, 0);
		g.device.clear(g.viewport_x, g.viewport_y, g.viewport_x + g.viewport_width, g.viewport_y + g.viewport_height,
			TRUE, FALSE, color);
	}
	g.rect_x0 = 0;
	g.rect_y0 = 0;
	g.rect_x1 = r_frame.width;
	g.rect_y1 = r_frame.height;
	g.in_front = FALSE;
	card_viewport();
}

static void d3d_view_rect(int x0, int y0, int x1, int y1, int active)
{
	if (!g.active)
		return;
	if (active)
	{
		g.rect_x0 = x0;
		g.rect_y0 = y0;
		g.rect_x1 = x1;
		g.rect_y1 = y1;
	}
	else
	{
		g.rect_x0 = 0;
		g.rect_y0 = 0;
		g.rect_x1 = r_frame.width;
		g.rect_y1 = r_frame.height;
	}
	card_viewport();
}

static void d3d_clear_depth(int x0, int y0, int x1, int y1)
{
	if (!g.active)
		return;
	r_d3d_flush();
	g.device.clear((int)(back_x((float)x0) + 0.5f), (int)(back_y((float)y0) + 0.5f), (int)(back_x((float)x1) + 0.5f),
		(int)(back_y((float)y1) + 0.5f), FALSE, TRUE, 0);
}

/* whether two states are the same, word by word (each is cleared before it
is set: the padding is 0) */
typedef char state_in_words[sizeof(struct r_d3d_state) % sizeof(unsigned long) == 0 ? 1 : -1];

static int same_state(const struct r_d3d_state *a, const struct r_d3d_state *b)
{
	const unsigned long *x = (const unsigned long *)a, *y = (const unsigned long *)b;

	for (int i = 0; i < (int)(sizeof(*a) / sizeof(*x)); i++)
	{
		if (x[i] != y[i])
			return FALSE;
	}
	return TRUE;
}

/* the batch drawn so far, if the next triangles' state, texture or
lightmap differs, or a triangle more might not fit */
static void batch_state(const struct r_d3d_state *state, struct r_d3d_texture *texture,
	struct r_d3d_texture *lightmap)
{
	if (g.count && (texture != g.texture || (state->lightmap && lightmap != g.lightmap) ||
		!same_state(state, &g.state) || g.count + TRIANGLE_VERTICES > BATCH_VERTICES ||
		g.index_count + TRIANGLE_INDICES > BATCH_INDICES))
	{
		r_d3d_flush();
	}
	g.state = *state;
	g.texture = texture;
	g.lightmap = state->lightmap ? lightmap : NULL;
}

static void emit_triangle(unsigned long a, unsigned long b, unsigned long c)
{
	unsigned short *out = &g.indices[g.index_count];

	out[0] = (unsigned short)a;
	out[1] = (unsigned short)b;
	out[2] = (unsigned short)c;
	g.index_count += 3;
}

static struct r_d3d_texture *card_texture(const struct r_texture *texture)
{
	if (!texture)
		return NULL;
	if (!texture->hardware)
		texture_upload((struct r_texture *)texture);
	return texture->hardware != REFUSED_TEXTURE ? (struct r_d3d_texture *)texture->hardware : NULL;
}

static void screen_state(struct r_d3d_state *state, const struct r_texture *texture)
{
	memset(state, 0, sizeof(*state));
	state->texture = texture ? _r_d3d_modulate : _r_d3d_colour;
	state->smooth = (unsigned char)g.smooth;
	state->clamp = texture && (texture->flags & R_TEXTURE_CLAMP);
	state->alpha_test = texture && (texture->flags & R_TEXTURE_ALPHA_TEST);
	switch (g.screen_blend)
	{
	case _r_screen_alpha: state->blend = _r_d3d_blend_alpha; break;
	case _r_screen_add: state->blend = _r_d3d_blend_add_alpha; break;
	case _r_screen_premultiplied: state->blend = _r_d3d_blend_premultiplied; break;
	default: break;
	}
}

/* A vertex before its colour and fog factor are clamped: everything in it
is linear in the view's space (clip space is, and so are the colour and
the coordinates, the card interpolating them with their perspective; the
fog factor is linear in the depth). The card fogs each pixel by the fog
factor interpolated between the vertices, clamped at each vertex first: a
triangle from near to far past the fog's end would come out fogged too
little between them (OpenGL fogs each pixel by its own depth). So a
triangle is cut where its factor crosses 0 and 1, and in each piece the
factor is either exact or the same clamped value throughout. */
struct fog_vertex
{
	struct r_d3d_vertex vertex;
	float color[4];
	float fog;
};

static void fog_vertex_between(const struct fog_vertex *a, const struct fog_vertex *b, float t, struct fog_vertex *out)
{
	out->vertex.x = a->vertex.x + (b->vertex.x - a->vertex.x) * t;
	out->vertex.y = a->vertex.y + (b->vertex.y - a->vertex.y) * t;
	out->vertex.z = a->vertex.z + (b->vertex.z - a->vertex.z) * t;
	out->vertex.w = a->vertex.w + (b->vertex.w - a->vertex.w) * t;
	out->vertex.u = a->vertex.u + (b->vertex.u - a->vertex.u) * t;
	out->vertex.v = a->vertex.v + (b->vertex.v - a->vertex.v) * t;
	out->vertex.lightmap_u = a->vertex.lightmap_u + (b->vertex.lightmap_u - a->vertex.lightmap_u) * t;
	out->vertex.lightmap_v = a->vertex.lightmap_v + (b->vertex.lightmap_v - a->vertex.lightmap_v) * t;
	for (int i = 0; i < 4; i++)
		out->color[i] = a->color[i] + (b->color[i] - a->color[i]) * t;
	out->fog = a->fog + (b->fog - a->fog) * t;
}

/* the polygon's parts with the factor at most and at least the value */
static void fog_split(const struct fog_vertex *in, int count, float value, struct fog_vertex *below, int *below_count,
	struct fog_vertex *above, int *above_count)
{
	*below_count = *above_count = 0;
	for (int i = 0; i < count; i++)
	{
		const struct fog_vertex *a = &in[i], *b = &in[(i + 1) % count];

		if (a->fog <= value)
			below[(*below_count)++] = *a;
		if (a->fog >= value)
			above[(*above_count)++] = *a;
		if ((a->fog < value && b->fog > value) || (a->fog > value && b->fog < value))
		{
			struct fog_vertex cut;

			fog_vertex_between(a, b, (value - a->fog) / (b->fog - a->fog), &cut);
			cut.fog = value;
			below[(*below_count)++] = cut;
			above[(*above_count)++] = cut;
		}
	}
	if (*below_count < 3)
		*below_count = 0;
	if (*above_count < 3)
		*above_count = 0;
}

/* the vertex as the card takes it, its colour and factor clamped */
static void fog_vertex_out(const struct fog_vertex *in, struct r_d3d_vertex *out)
{
	float fog = in->fog;

	*out = in->vertex;
	out->color = argb(in->color[0], in->color[1], in->color[2], in->color[3]);
	out->fog = fog < 0.0f ? 0.0f : fog > 1.0f ? 1.0f : fog;
}

/* a polygon to the batch as a fan of triangles */
static void fog_emit(const struct fog_vertex *polygon, int count)
{
	unsigned long first = g.count;

	if (count < 3)
		return;
	for (int i = 0; i < count; i++)
		fog_vertex_out(&polygon[i], &g.vertices[g.count++]);
	for (int i = 1; i + 1 < count; i++)
		emit_triangle(first, first + i, first + i + 1);
}

/* whether the factors of a triangle's corners cross 0 or 1 between them
(where the card's clamped factors would be wrong) */
static int fog_crosses(float a, float b, float c)
{
	return !((a >= 0.0f && a <= 1.0f && b >= 0.0f && b <= 1.0f && c >= 0.0f && c <= 1.0f) ||
		(a <= 0.0f && b <= 0.0f && c <= 0.0f) || (a >= 1.0f && b >= 1.0f && c >= 1.0f));
}

static void fogged_triangle(const struct fog_vertex *triangle)
{
	struct fog_vertex beyond[FOG_POLYGON_MAXIMUM], inside[FOG_POLYGON_MAXIMUM];
	struct fog_vertex fogged[FOG_POLYGON_MAXIMUM], clear[FOG_POLYGON_MAXIMUM];
	int beyond_count, inside_count, fogged_count, clear_count;

	if (!fog_crosses(triangle[0].fog, triangle[1].fog, triangle[2].fog))
	{
		fog_emit(triangle, 3);
		return;
	}
	/* (past the fog's end, then the rest: before its start, and between) */
	fog_split(triangle, 3, 0.0f, beyond, &beyond_count, inside, &inside_count);
	fog_emit(beyond, beyond_count);
	fog_split(inside, inside_count, 1.0f, fogged, &fogged_count, clear, &clear_count);
	fog_emit(fogged, fogged_count);
	fog_emit(clear, clear_count);
}

/* The corner as the card takes it, before its colour and fog factor are
clamped: its place and coordinates in the vertex, its colour (kind: how the
batch takes it; alpha, the colour's alpha where the draw gives it), and
the fog factor where the batch is fogged */
static float corner_values(const struct r_raster_vertex *v, int kind, int fogged, float alpha,
	struct r_d3d_vertex *vertex, float *color)
{
	float z = 1.0f / v->w;

	clip_position(vertex, v->x, v->y, z, z - g.z_near);
	vertex->u = v->s * z;
	vertex->v = v->t * z;
	vertex->lightmap_u = v->lightmap_s * z;
	vertex->lightmap_v = v->lightmap_t * z;
	if (kind == _corner_white)
	{
		color[0] = color[1] = color[2] = color[3] = 255.0f;
	}
	else
	{
		/* (the light before the front end's fog: the card fogs it again) */
		float unfog = 1.0f;

		if (fogged && v->fog != 0.0f)
		{
			float fog = v->fog / (float)(R_FOG_LEVELS - 1);

			if (fog < 0.999f)
				unfog = 1.0f / (1.0f - fog);
		}
		if (kind == _corner_detail)
		{
			/* (a detail map's grey is its strength: the colour's alpha; the
			colour itself the grey it goes toward) */
			color[0] = color[1] = color[2] = 128.0f;
			color[3] = v->r * unfog;
		}
		else
		{
			color[0] = v->r * unfog;
			color[1] = v->g * unfog;
			color[2] = v->b * unfog;
			color[3] = alpha;
		}
	}
	if (!fogged)
		return 1.0f;
	return g.fog_span ? (float)(((double)g.fog_end - (double)z) * g.fog_scale) : z < g.fog_end ? 1.0f : 0.0f;
}

static void make_corner(const struct r_raster_vertex *v, int kind, int fogged, float alpha, struct fog_vertex *out)
{
	out->fog = corner_values(v, kind, fogged, alpha, &out->vertex, out->color);
}

/* the corner's vertex in the batch (made, or the one made for it before)
and its fog factor before it is clamped */
static unsigned long corner_index(const struct r_raster_vertex *v, int kind, int fogged, float alpha, float *fog)
{
	struct cached_corner *slot = &g.corners[((unsigned long)v / sizeof(*v)) % CORNER_CACHE];
	const unsigned long *bits = (const unsigned long *)v;
	unsigned long alpha_bits;

	/* (the vertex's floats as words: x, y, w, s, t at 0 to 4, r, g, b at 7 to
	9, fog at 14, the lightmap's s and t at 15 and 16) */
	memcpy(&alpha_bits, &alpha, sizeof(alpha_bits));
	if (slot->batch != g.batch || slot->source != v || slot->index >= g.count || slot->key.x != bits[0] || slot->key.y != bits[1] ||
		slot->key.w != bits[2] || slot->key.s != bits[3] || slot->key.t != bits[4] || slot->key.r != bits[7] ||
		slot->key.g != bits[8] || slot->key.b != bits[9] || slot->key.fog != bits[14] ||
		slot->key.lightmap_s != bits[15] || slot->key.lightmap_t != bits[16] || slot->key.alpha != alpha_bits)
	{
		struct r_d3d_vertex *out = &g.vertices[g.count];
		float color[4], value;

		value = corner_values(v, kind, fogged, alpha, out, color);
		out->color = argb(color[0], color[1], color[2], color[3]);
		out->fog = value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
		slot->source = v;
		slot->batch = g.batch;
		slot->key.x = bits[0];
		slot->key.y = bits[1];
		slot->key.w = bits[2];
		slot->key.s = bits[3];
		slot->key.t = bits[4];
		slot->key.r = bits[7];
		slot->key.g = bits[8];
		slot->key.b = bits[9];
		slot->key.fog = bits[14];
		slot->key.lightmap_s = bits[15];
		slot->key.lightmap_t = bits[16];
		slot->key.alpha = alpha_bits;
		slot->fog = value;
		slot->index = g.count++;
	}
	*fog = slot->fog;
	return slot->index;
}

/* a triangle to the batch (its room made by batch_state): its corners' vertices,
or, where its fog factors cross 0 or 1, the pieces it is cut into */
static void corners_triangle(const struct r_raster_vertex *const *corners, int kind, int fogged, float alpha)
{
	unsigned long a, b, c;
	float fog_a, fog_b, fog_c;

	a = corner_index(corners[0], kind, fogged, alpha, &fog_a);
	b = corner_index(corners[1], kind, fogged, alpha, &fog_b);
	c = corner_index(corners[2], kind, fogged, alpha, &fog_c);
	if (!fogged || !fog_crosses(fog_a, fog_b, fog_c))
	{
		emit_triangle(a, b, c);
	}
	else
	{
		struct fog_vertex polygon[3];

		for (int i = 0; i < 3; i++)
			make_corner(corners[i], kind, fogged, alpha, &polygon[i]);
		fogged_triangle(polygon);
	}
}

/* The Xbox's texture pass over the triangle drawn just before: its stages
as r_detail_pass has them (the device's program scales the base map's
coordinates for each), the picture times twice what they give (Halo's
double biased multiply) or once (its plain multiply), fogged toward what
changes nothing (grey, or white), a pixel's once. Or, replacing the
triangle's texture, the stages blended as the triangle's flags say, and
cut out where its texture is (an alpha-tested shader's: the stages' alpha
is tested as the texture's would be). */
static void xbox_detail_triangle(const struct r_raster_vertex *const *corners, const struct r_texture *texture,
	unsigned flags)
{
	struct r_d3d_state state;

	/* (its stages, made again where the pass is not the one g.detail was
	made from: a draw's triangles share theirs) */
	if (r_detail_pass != g.detail_source)
	{
		struct r_d3d_detail detail;
		int stages = r_detail_pass->stage_count < g.device.detail_stages ? r_detail_pass->stage_count :
			g.device.detail_stages;

		memset(&detail, 0, sizeof(detail));
		detail.stage_count = stages > R_D3D_DETAIL_STAGES ? R_D3D_DETAIL_STAGES : stages;
		for (int s = 0; s < detail.stage_count; s++)
		{
			detail.textures[s] = (struct r_d3d_texture *)r_detail_pass->textures[s];
			detail.scales[s][0] = r_detail_pass->scales[s][0];
			detail.scales[s][1] = r_detail_pass->scales[s][1];
			detail.offsets[s][0] = r_detail_pass->offsets[s][0];
			detail.offsets[s][1] = r_detail_pass->offsets[s][1];
			detail.operations[s] = r_detail_pass->operations[s];
			detail.alpha_operations[s] = r_detail_pass->alpha_operations[s];
			if (!detail.textures[s])
				return;
		}
		if (g.count && g.state.texture == _r_d3d_xbox_detail && memcmp(&detail, &g.detail, sizeof(detail)))
			r_d3d_flush();
		g.detail = detail;
		g.detail_source = r_detail_pass;
	}
	memset(&state, 0, sizeof(state));
	state.texture = _r_d3d_xbox_detail;
	state.smooth = TRUE;
	state.depth_test = !(flags & R_DRAW_NO_DEPTH_TEST);
	if (r_detail_pass->replace)
	{
		/* (blended as the triangle itself would be) */
		if (flags & R_DRAW_ADDITIVE)
			state.blend = _r_d3d_blend_add;
		else if (flags & R_DRAW_BLEND)
			state.blend = _r_d3d_blend_alpha;
		else
			state.depth_write = TRUE;
		state.alpha_test = (texture->flags & R_TEXTURE_ALPHA_TEST) != 0;
		/* (added light fades out in the fog, as the front end fades the
		other triangles' added light: fogged toward black, which adds
		nothing; the stages take no vertex colour to fade) */
		if (g.fogged && !(flags & R_DRAW_NO_FOG))
		{
			state.fog = TRUE;
			state.fog_color = (flags & R_DRAW_ADDITIVE) ? 0x000000UL : g.fog_color;
		}
	}
	else
	{
		state.stencil_once = TRUE;
		state.blend = r_detail_pass->multiply ? _r_d3d_blend_multiply : _r_d3d_blend_detail;
		if (g.fogged && !(flags & R_DRAW_NO_FOG))
		{
			state.fog = TRUE;
			state.fog_color = r_detail_pass->multiply ? 0xFFFFFFUL : 0x808080UL;
		}
	}
	batch_state(&state, NULL, NULL);
	corners_triangle(corners, _corner_white, state.fog, 255.0f);
}

static void d3d_triangle(const struct r_raster_vertex *a, const struct r_raster_vertex *b,
	const struct r_raster_vertex *c, const struct r_texture *texture, unsigned flags)
{
	const struct r_raster_vertex *corners[3] = { a, b, c };
	struct r_d3d_state state;
	struct r_d3d_texture *card = card_texture(texture);
	float alpha = 255.0f;
	int detail;

	if (!g.active)
		return;
	if (texture && !card)
		texture = NULL;
	if (flags & R_RASTER_SCREEN)
	{
		unsigned long first;

		screen_state(&state, texture);
		batch_state(&state, card, NULL);
		first = g.count;
		for (int i = 0; i < 3; i++)
		{
			const struct r_raster_vertex *v = corners[i];
			struct r_d3d_vertex *out = &g.vertices[g.count++];

			/* (the screen's colour is 0-255 as the texel; its alpha in fog_r) */
			clip_position(out, v->x, v->y, 1.0f, 0.0f);
			out->color = argb(v->r, v->g, v->b, v->fog_r);
			out->fog = 1.0f;
			out->u = v->u;
			out->v = v->v;
			out->lightmap_u = out->lightmap_v = 0.0f;
		}
		emit_triangle(first, first + 1, first + 2);
		return;
	}
	memset(&state, 0, sizeof(state));
	if ((flags & R_RASTER_XBOX_DETAIL) && r_detail_pass && texture && g.device.detail_stages >= 3)
	{
		xbox_detail_triangle(corners, texture, flags);
		return;
	}
	detail = (flags & R_DRAW_DETAIL) && texture;
	state.texture = !texture ? _r_d3d_colour : detail ? _r_d3d_detail : _r_d3d_modulate_twice;
	state.smooth = (unsigned char)g.smooth;
	state.clamp = texture && (texture->flags & R_TEXTURE_CLAMP);
	state.alpha_test = texture && (texture->flags & R_TEXTURE_ALPHA_TEST);
	state.depth_test = !(flags & R_DRAW_NO_DEPTH_TEST);
	if (detail)
		state.blend = _r_d3d_blend_detail;
	else if (flags & R_DRAW_ADDITIVE)
	{
		state.blend = (flags & R_DRAW_PREMULTIPLIED) ? _r_d3d_blend_premultiplied : _r_d3d_blend_add;
	}
	else if (flags & R_DRAW_BLEND)
	{
		state.blend = _r_d3d_blend_alpha;
		alpha = (flags & R_DRAW_ALPHA) ? (float)(g.alpha_scale * 255 / 256) : 128.0f;
	}
	else
	{
		state.depth_write = TRUE;
	}
	/* (added light is faded by the front end: R_DRAW_ADDITIVE) */
	if (g.fogged && !(flags & (R_DRAW_NO_FOG | R_DRAW_ADDITIVE)))
	{
		state.fog = TRUE;
		/* (a detail map fogs toward no change: grey) */
		state.fog_color = detail ? 0x808080UL : g.fog_color;
	}
	/* (times its lightmap: the level's on the Xbox's look, r_set_lightmap) */
	state.lightmap = (flags & R_RASTER_LIGHTMAP) && r_lightmap && texture && !detail;
	batch_state(&state, card, state.lightmap ? (struct r_d3d_texture *)r_lightmap : NULL);
	corners_triangle(corners, detail ? _corner_detail : _corner_light, state.fog, alpha);
}

static void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, unsigned long color)
{
	const float xs[4] = { x0, x1, x1, x0 }, ys[4] = { y0, y0, y1, y1 };
	const float us[4] = { u0, u1, u1, u0 }, vs[4] = { v0, v0, v1, v1 };
	unsigned long first = g.count;

	for (int i = 0; i < 4; i++)
	{
		struct r_d3d_vertex *out = &g.vertices[g.count++];

		clip_position(out, xs[i], ys[i], 1.0f, 0.0f);
		out->color = color;
		out->fog = 1.0f;
		out->u = us[i];
		out->v = vs[i];
		out->lightmap_u = out->lightmap_v = 0.0f;
	}
	emit_triangle(first, first + 1, first + 2);
	emit_triangle(first, first + 2, first + 3);
}

static void d3d_fill_row(int x0, int x1, int y, unsigned long color)
{
	struct r_d3d_state state;

	if (!g.active || x0 >= x1)
		return;
	memset(&state, 0, sizeof(state));
	batch_state(&state, NULL, NULL);
	quad((float)x0, (float)y, (float)x1, (float)(y + 1), 0.0f, 0.0f, 0.0f, 0.0f, 0xFF000000UL | color);
}

static void d3d_screen_rect(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
	const struct r_texture *texture, unsigned long color)
{
	struct r_d3d_state state;
	struct r_d3d_texture *card = card_texture(texture);

	if (!g.active)
		return;
	if (texture && !card)
		texture = NULL;
	screen_state(&state, texture);
	batch_state(&state, card, NULL);
	quad(x0, y0, x1, y1, u0, v0, u1, v1, color);
}

static void d3d_fill_rectangle(int x0, int y0, int x1, int y1, unsigned long color)
{
	struct r_d3d_state state;

	if (!g.active || x0 >= x1 || y0 >= y1)
		return;
	memset(&state, 0, sizeof(state));
	state.blend = _r_d3d_blend_alpha;
	batch_state(&state, NULL, NULL);
	quad((float)x0, (float)y0, (float)x1, (float)y1, 0.0f, 0.0f, 0.0f, 0.0f, color);
}

/* ---------- glyphs and pictures */

/* a textured quad clipped to a rectangle of the picture */
static void clipped_quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
	unsigned long color, int clip_x0, int clip_y0, int clip_x1, int clip_y1)
{
	float width = x1 - x0, height = y1 - y0;

	if (x0 < (float)clip_x0)
	{
		u0 += (u1 - u0) * ((float)clip_x0 - x0) / width;
		x0 = (float)clip_x0;
	}
	if (x1 > (float)clip_x1)
	{
		u1 -= (u1 - u0) * (x1 - (float)clip_x1) / (x1 - x0);
		x1 = (float)clip_x1;
	}
	if (y0 < (float)clip_y0)
	{
		v0 += (v1 - v0) * ((float)clip_y0 - y0) / height;
		y0 = (float)clip_y0;
	}
	if (y1 > (float)clip_y1)
	{
		v1 -= (v1 - v0) * (y1 - (float)clip_y1) / (y1 - y0);
		y1 = (float)clip_y1;
	}
	if (x0 < x1 && y0 < y1)
		quad(x0, y0, x1, y1, u0, v0, u1, v1, color);
}

static void d3d_coverage(int x, int y, const unsigned char *cover, int width, int height, unsigned long color,
	unsigned long *handle, int clip_x0, int clip_y0, int clip_x1, int clip_y1)
{
	struct r_d3d_state state;
	int texture_width = power_of_two(width), texture_height = power_of_two(height);

	if (!g.active || width <= 0 || height <= 0)
		return;
	if (x >= clip_x1 || y >= clip_y1 || x + width <= clip_x0 || y + height <= clip_y0)
		return;
	/* (a thin glyph's padded out to a shape the card takes: an 'l' is
	16 times as tall as wide) */
	card_shape(&texture_width, &texture_height);
	if (texture_width > g.device.maximum_texture_size || texture_height > g.device.maximum_texture_size)
		return;
	if (!*handle)
	{
		/* (white, the coverage its alpha, padded to powers of two) */
		unsigned long *texels = calloc((size_t)texture_width * texture_height, sizeof(*texels));
		struct r_d3d_texture *card;

		if (!texels)
			return;
		for (int row = 0; row < height; row++)
		{
			for (int column = 0; column < width; column++)
				texels[row * texture_width + column] = ((unsigned long)cover[row * width + column] << 24) | 0x00FFFFFFUL;
		}
		r_d3d_flush();
		card = g.device.texture_new(texture_width, texture_height, 1, _r_d3d_alpha_full);
		if (card)
			g.device.texture_write(card, 0, texels);
		free(texels);
		if (!card)
			return;
		*handle = (unsigned long)card;
	}
	memset(&state, 0, sizeof(state));
	state.texture = _r_d3d_modulate;
	state.blend = _r_d3d_blend_alpha;
	state.smooth = TRUE;
	state.clamp = TRUE;
	batch_state(&state, (struct r_d3d_texture *)*handle, NULL);
	clipped_quad((float)x, (float)y, (float)(x + width), (float)(y + height), 0.0f, 0.0f,
		(float)width / (float)texture_width, (float)height / (float)texture_height, color, clip_x0, clip_y0, clip_x1,
		clip_y1);
}

static void d3d_coverage_forget(unsigned long handle)
{
	if (!g.active || !handle)
		return;
	if (g.texture == (struct r_d3d_texture *)handle)
	{
		r_d3d_flush();
		g.texture = NULL;
	}
	g.device.texture_delete((struct r_d3d_texture *)handle);
}

/* a picture slot's tiles given back */
static void picture_forget(int slot)
{
	r_d3d_flush();
	for (int i = 0; g.pictures[slot].tiles && i < g.pictures[slot].columns * g.pictures[slot].rows; i++)
	{
		if (!g.pictures[slot].tiles[i])
			continue;
		if (g.texture == g.pictures[slot].tiles[i])
			g.texture = NULL;
		g.device.texture_delete(g.pictures[slot].tiles[i]);
	}
	free(g.pictures[slot].tiles);
	g.pictures[slot].tiles = NULL;
	g.pictures[slot].columns = g.pictures[slot].rows = 0;
}

/* the picture in tiles the card takes: as large as the picture in powers
of two, or the card's largest side (a Voodoo's 256: the title's logo is
1024 wide at 640x480), in shapes it takes; FALSE without memory */
static int picture_tiles(int slot, const unsigned long *argb_pixels, int width, int height)
{
	int tile_width = power_of_two(width), tile_height = power_of_two(height);
	unsigned long *texels;

	if (tile_width > g.device.maximum_texture_size)
		tile_width = g.device.maximum_texture_size;
	if (tile_height > g.device.maximum_texture_size)
		tile_height = g.device.maximum_texture_size;
	card_shape(&tile_width, &tile_height);
	g.pictures[slot].columns = (width + tile_width - 1) / tile_width;
	g.pictures[slot].rows = (height + tile_height - 1) / tile_height;
	g.pictures[slot].tile_width = tile_width;
	g.pictures[slot].tile_height = tile_height;
	g.pictures[slot].tiles = calloc((size_t)g.pictures[slot].columns * g.pictures[slot].rows,
		sizeof(*g.pictures[slot].tiles));
	texels = malloc((size_t)tile_width * tile_height * sizeof(*texels));
	if (!g.pictures[slot].tiles || !texels)
	{
		free(texels);
		free(g.pictures[slot].tiles);
		g.pictures[slot].tiles = NULL;
		return FALSE;
	}
	for (int row = 0; row < g.pictures[slot].rows; row++)
	{
		for (int column = 0; column < g.pictures[slot].columns; column++)
		{
			int x0 = column * tile_width, y0 = row * tile_height;
			int part_width = width - x0 < tile_width ? width - x0 : tile_width;
			int part_height = height - y0 < tile_height ? height - y0 : tile_height;
			struct r_d3d_texture *tile;
			int alpha = _r_d3d_alpha_none;

			/* (padded with nothing) */
			memset(texels, 0, (size_t)tile_width * tile_height * sizeof(*texels));
			for (int line = 0; line < part_height; line++)
			{
				memcpy(texels + (size_t)line * tile_width, argb_pixels + (size_t)(y0 + line) * width + x0,
					(size_t)part_width * sizeof(*texels));
			}
			/* (what its alpha holds, the padding's too: on a card of 16-bit
			textures an opaque tile, a loading screen's, keeps 5 and 6 bits of
			colour, not the 4 of one with alpha) */
			for (long i = 0; i < (long)tile_width * tile_height && alpha != _r_d3d_alpha_full; i++)
			{
				unsigned long a = texels[i] >> 24;

				if (a != 0xFF)
					alpha = a ? _r_d3d_alpha_full : _r_d3d_alpha_test;
			}
			tile = g.device.texture_new(tile_width, tile_height, 1, alpha);
			if (tile)
				g.device.texture_write(tile, 0, texels);
			g.pictures[slot].tiles[row * g.pictures[slot].columns + column] = tile;
		}
	}
	free(texels);
	return TRUE;
}

static void d3d_picture(int x, int y, const unsigned long *argb_pixels, int width, int height)
{
	struct r_d3d_state state;
	unsigned long check = 0;
	int slot = -1, oldest = 0;

	if (!g.active || width <= 0 || height <= 0)
		return;
	/* (the same picture as before: its address, size and a few texels) */
	for (int i = 0; i < 8; i++)
		check = check * 31 + argb_pixels[(size_t)i * width * height / 8];
	g.clock++;
	for (int i = 0; i < (int)(sizeof(g.pictures) / sizeof(g.pictures[0])); i++)
	{
		if (g.pictures[i].tiles && g.pictures[i].argb == argb_pixels && g.pictures[i].width == width &&
			g.pictures[i].height == height && g.pictures[i].check == check)
		{
			slot = i;
			break;
		}
		if (g.pictures[i].used < g.pictures[oldest].used)
			oldest = i;
	}
	if (slot < 0)
	{
		slot = oldest;
		picture_forget(slot);
		if (!picture_tiles(slot, argb_pixels, width, height))
			return;
		g.pictures[slot].argb = argb_pixels;
		g.pictures[slot].width = width;
		g.pictures[slot].height = height;
		g.pictures[slot].check = check;
	}
	g.pictures[slot].used = g.clock;
	memset(&state, 0, sizeof(state));
	state.texture = _r_d3d_modulate;
	state.blend = _r_d3d_blend_alpha;
	state.smooth = TRUE;
	state.clamp = TRUE;
	for (int row = 0; row < g.pictures[slot].rows; row++)
	{
		for (int column = 0; column < g.pictures[slot].columns; column++)
		{
			struct r_d3d_texture *tile = g.pictures[slot].tiles[row * g.pictures[slot].columns + column];
			int x0 = column * g.pictures[slot].tile_width, y0 = row * g.pictures[slot].tile_height;
			int part_width = width - x0 < g.pictures[slot].tile_width ? width - x0 : g.pictures[slot].tile_width;
			int part_height = height - y0 < g.pictures[slot].tile_height ? height - y0 : g.pictures[slot].tile_height;

			if (!tile)
				continue;
			batch_state(&state, tile, NULL);
			quad((float)(x + x0), (float)(y + y0), (float)(x + x0 + part_width), (float)(y + y0 + part_height), 0.0f,
				0.0f, (float)part_width / (float)g.pictures[slot].tile_width,
				(float)part_height / (float)g.pictures[slot].tile_height, 0xFFFFFFFFUL);
		}
	}
}

static const struct r_backend d3d_backend =
{
	d3d_triangle,
	d3d_screen_blend,
	d3d_clear,
	d3d_fill_row,
	d3d_screen_rect,
	d3d_set_near,
	d3d_set_in_front,
	d3d_set_alpha_scale,
	d3d_view_fog,
	d3d_clear_depth,
	d3d_coverage,
	d3d_coverage_forget,
	d3d_picture,
	d3d_fill_rectangle,
	d3d_texture_delete,
	d3d_read_back,
	d3d_texture_created,
	d3d_draws_detail,
	d3d_view_rect,
	d3d_lightmap_new,
	d3d_lightmap_delete,
	d3d_detail_stages,
	d3d_hardware_texture_new,
	d3d_hardware_texture_delete,
	d3d_hardware_texture_write,
	d3d_hardware_cube_new,
	d3d_draws_water,
	d3d_water,
};
