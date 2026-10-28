/*
R_GL.C

The OpenGL 1.1 rasterizer (r_gl.h): what the software rasterizer draws,
drawn by the graphics card, for Windows 95 machines with an OpenGL driver
(a 3D card's, or Microsoft's own in software). The front end (r_main.c)
culls, transforms, lights, fogs and clips as ever; its screen-space
triangles come here (r_internal.h's seam), and each vertex goes to OpenGL
as (x z, y z, z - 2 near, z), z the view's depth: with a projection that
maps the picture's pixels to the viewport, the card divides by z again, so
the texture is interpolated with its perspective, and the depth it stores
is 1 - 2 near / z (nearer is less: GL_LESS).

The software rasterizer's ways, in OpenGL's:
	- light: the vertex's colour times the texel, 0x80 the texel as it
	  is: with the texture environment combine extension (EXT/ARB) at
	  twice the colour (RGB_SCALE 2), else the colour doubled and clamped
	  (nothing brighter than the texel);
	- fog: the front end's, linear in the view's depth, is OpenGL's linear
	  fog of the eye's z (the vertex's third coordinate), the colour given
	  back its unfogged light;
	- the first-person weapon in front of the world (r_set_in_front): the
	  first tenth of the depth range, the world the rest;
	- added (R_DRAW_ADDITIVE), half (R_DRAW_BLEND) and by alpha
	  (R_DRAW_ALPHA, times r_set_alpha_scale) blends, the screen's four
	  (r_screen_blend), alpha-tested textures (index 0 a hole);
	- detail maps (R_DRAW_DETAIL, TEXTURE QUALITY HIGH and up): the same
	  triangles again, depth equal, the picture times twice the texel
	  (DST_COLOR, SRC_COLOR), the texel toward grey by the vertices' grey
	  (the combine extension's INTERPOLATE with a grey constant), fogged
	  toward grey;
	- the 2D helpers (text's glyphs, pictures, rectangles): quads, a
	  glyph's coverage an alpha texture of its own.
Triangles are gathered while the state stays the same (the texture, the
blend, the depth and fog), each vertex once, and drawn together by one
glDrawElements (the batch's, below). Textures go to the card
as they are made, in their own full colours (r_texture's argb: the 8-bit
levels and their palette are the software rasterizer's), as large as the
TEXTURE QUALITY setting lets them (texture_size), then averaged down to
1x1 (a mipmapped texture needs every level). With TEXTURE FILTER SMOOTH or
DITHERED they are filtered between texels and between levels (trilinear),
and anisotropically where the driver can (EXT_texture_filter_anisotropic,
up to 8 samples); SHARP keeps texels squares.
*/

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "r_internal.h"
#include "r_gl.h"

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

/* ---------- OpenGL's numbers */

#define GL_TRIANGLES 0x0004
#define GL_QUADS 0x0007
#define GL_DEPTH_TEST 0x0B71
#define GL_BLEND 0x0BE2
#define GL_ALPHA_TEST 0x0BC0
#define GL_TEXTURE_2D 0x0DE1
#define GL_RGB8 0x8051
#define GL_RGBA8 0x8058
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_TEXTURE_MAX_ANISOTROPY 0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY 0x84FF
#define GL_FOG 0x0B60
#define GL_SCISSOR_TEST 0x0C11
#define GL_CULL_FACE 0x0B44
#define GL_DITHER 0x0BD0
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_DEPTH_BUFFER_BIT 0x0100
#define GL_PROJECTION 0x1701
#define GL_MODELVIEW 0x1700
#define GL_LESS 0x0201
#define GL_GREATER 0x0204
#define GL_ZERO 0
#define GL_ONE 1
#define GL_SRC_COLOR 0x0300
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_SMOOTH 0x1D01
#define GL_PERSPECTIVE_CORRECTION_HINT 0x0C50
#define GL_FOG_HINT 0x0C54
#define GL_NICEST 0x1102
#define GL_FOG_MODE 0x0B65
#define GL_FOG_START 0x0B63
#define GL_FOG_END 0x0B64
#define GL_FOG_COLOR 0x0B66
#define GL_TEXTURE_ENV 0x2300
#define GL_TEXTURE_ENV_MODE 0x2200
#define GL_MODULATE 0x2100
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_LINEAR_MIPMAP_NEAREST 0x2701
#define GL_CLAMP 0x2900
#define GL_REPEAT 0x2901
#define GL_RGBA 0x1908
#define GL_ALPHA 0x1906
#define GL_LUMINANCE_ALPHA 0x190A
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_TEXTURE 0x1702
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_EXTENSIONS 0x1F03
/* the texture environment combine extension's */
#define GL_COMBINE 0x8570
#define GL_COMBINE_RGB 0x8571
#define GL_COMBINE_ALPHA 0x8572
#define GL_RGB_SCALE 0x8573
#define GL_PRIMARY_COLOR 0x8577
#define GL_SOURCE0_RGB 0x8580
#define GL_SOURCE1_RGB 0x8581
#define GL_SOURCE0_ALPHA 0x8588
#define GL_SOURCE1_ALPHA 0x8589
#define GL_OPERAND0_RGB 0x8590
#define GL_OPERAND1_RGB 0x8591
#define GL_OPERAND0_ALPHA 0x8598
#define GL_OPERAND1_ALPHA 0x8599
#define GL_INTERPOLATE 0x8575
#define GL_CONSTANT 0x8576
#define GL_SOURCE2_RGB 0x8582
#define GL_OPERAND2_RGB 0x8592
#define GL_TEXTURE_ENV_COLOR 0x2201
#define GL_DST_COLOR 0x0306
#define GL_LEQUAL 0x0203
#define GL_MAX_TEXTURE_SIZE 0x0D33
/* OpenGL 1.2's (or EXT/SGIS_texture_edge_clamp's) */
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_FLOAT 0x1406
#define GL_UNSIGNED_SHORT 0x1403
#define GL_VERTEX_ARRAY 0x8074
#define GL_COLOR_ARRAY 0x8076
#define GL_TEXTURE_COORD_ARRAY 0x8078

/* the depth range's share of the first-person weapon (r_set_in_front) */
#define IN_FRONT_DEPTH 0.1

/* ---------- state */

/* how the next triangles are drawn: what changes between batches */
#define STATE_TEXTURE 0x0001
#define STATE_SCREEN 0x0002
#define STATE_DEPTH_TEST 0x0004
#define STATE_DEPTH_WRITE 0x0008
#define STATE_ALPHA_TEST 0x0010
#define STATE_FOG 0x0020
#define STATE_BLEND_ALPHA 0x0040 /* source alpha, one minus it */
#define STATE_BLEND_ADD 0x0080 /* one, one */
#define STATE_BLEND_ADD_ALPHA 0x0100 /* source alpha, one */
#define STATE_BLEND_PREMULTIPLIED 0x0200 /* one, one minus source alpha */
#define STATE_SCALE 0x0400 /* the light at twice (combine) */
#define STATE_IN_FRONT 0x0800
#define STATE_CLAMP 0x1000
#define STATE_DETAIL 0x2000 /* a detail map: depth equal, the texel toward grey, fog grey */
#define STATE_BLEND_DETAIL 0x4000 /* destination colour, source colour */

static struct
{
	int active;
	struct r_gl_functions gl;
	/* the driver's vertex arrays (else the batches go vertex by vertex) */
	int arrays;
	/* the combine extension: the light at twice the texel */
	int combine;
	/* its environment as set (texture_environment) */
	unsigned environment_rgb;
	int environment_scale;
	/* the picture in the window (set at the next frame's start: changed) */
	int viewport_x, viewport_y, viewport_width, viewport_height, window_width, window_height;
	int viewport_changed;
	float z_near;
	int in_front;
	unsigned long alpha_scale;
	int screen_blend;
	int fogged;
	float fog_start, fog_end;
	unsigned long fog_color;
	/* the batch: begun (its state set in OpenGL's, its triangles gathered
	in batch below), its state and texture */
	int begun;
	unsigned state;
	const struct r_texture *texture;
	int smooth;
	/* the glyph's texture a batch of glyphs is drawn from (COVERAGE_BATCH) */
	unsigned coverage_name;
	/* the most samples an anisotropic filter takes (1: none) */
	float anisotropy;
	/* the largest texture side the card takes (a power of two: 256 on a
	3dfx card, 1024 or 2048 on others of the time) */
	int maximum_texture_size;
	/* how a clamped texture keeps to its edge: GL_CLAMP_TO_EDGE where the
	driver has it, else OpenGL 1.1's GL_CLAMP (which filters the edge's
	texels half with the border's colour) */
	int clamp;
	/* the view's rectangle the scissor keeps its triangles in (r_view_begin
	to r_view_end) */
	int view_active, view_x0, view_y0, view_x1, view_y1;
	/* the pictures' textures (r_gl_picture): one, or tiles of the largest
	size the card takes, columns x rows of them */
	struct
	{
		const unsigned long *argb;
		int width, height;
		unsigned long check;
		unsigned *names;
		int tile, columns, rows;
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

/* 0-255 to a byte, the nearest (a half up) */
static unsigned char clamp_byte(float value)
{
	long rounded;

	if (value <= 0.0f)
		return 0;
	if (value >= 255.0f)
		return 255;
	/* (fistp, not a cast's truncation of value + 0.5, which loads the FPU's
	control word twice a byte: slow on a Pentium II; fistp rounds a half to
	even, so a half rounded down goes up again) */
	rounded = r_round(value);
	if (value - (float)rounded == 0.5f)
		rounded++;
	return (unsigned char)rounded;
}

/* ---------- the batch's triangles

A batch's triangles are gathered as vertices and the indices of their
corners, and drawn at its end by one glDrawElements: not three calls a
vertex, and the driver lights and transforms a vertex the triangles share
once. The front end hands a draw's shared vertices over at one address, so
a corner whose vertex is the same as one already in the batch (the same
numbers the front end gave, and the same alpha) takes that one's index, not
worked out again. About as large as Quake III's batches, which the drivers
of its day were tuned for. */

#define BATCH_VERTICES 1024
#define BATCH_INDICES 3072
#define VERTEX_CACHE 256

static struct
{
	float position[BATCH_VERTICES][4];
	float texcoord[BATCH_VERTICES][2];
	unsigned char color[BATCH_VERTICES][4];
	unsigned short index[BATCH_INDICES];
	int vertices, indices;
	/* the vertices made: by their address, what they were made from and
	where they are in the batch; good only in the generation that made them
	(each batch drawn, or the near plane moved, starts another) */
	unsigned long generation;
	struct
	{
		unsigned long generation;
		/* the front end's x, y, w, s, t, r, g, b and fog, bit for bit */
		unsigned long key[9];
		unsigned long alpha;
		unsigned long index;
	} cache[VERTEX_CACHE];
} batch;

/* the batch gathered so far drawn, and the next one begun empty */
static void batch_draw(void)
{
	if (batch.indices)
	{
		if (g.arrays)
		{
			g.gl.DrawElements(GL_TRIANGLES, batch.indices, GL_UNSIGNED_SHORT, batch.index);
		}
		else
		{
			int textured = (g.state & STATE_TEXTURE) != 0;

			g.gl.Begin(GL_TRIANGLES);
			for (int i = 0; i < batch.indices; i++)
			{
				const unsigned char *color = batch.color[batch.index[i]];
				const float *position = batch.position[batch.index[i]];

				g.gl.Color4ub(color[0], color[1], color[2], color[3]);
				if (textured)
					g.gl.TexCoord2f(batch.texcoord[batch.index[i]][0], batch.texcoord[batch.index[i]][1]);
				g.gl.Vertex4f(position[0], position[1], position[2], position[3]);
			}
			g.gl.End();
		}
	}
	batch.vertices = 0;
	batch.indices = 0;
	batch.generation++;
}

static void batch_end(void)
{
	if (g.begun)
	{
		batch_draw();
		g.begun = FALSE;
	}
}

/* room in the batch for so many more vertices and indices (it is drawn and
begun again if not) */
static void batch_room(int vertices, int indices)
{
	if (batch.vertices + vertices > BATCH_VERTICES || batch.indices + indices > BATCH_INDICES)
		batch_draw();
}

static unsigned long float_bits(const float *value)
{
	unsigned long bits;

	memcpy(&bits, value, sizeof(bits));
	return bits;
}

/* ---------- textures */

/* A texture's name on the card and the filter it was given, kept in its
r_texture's hardware: the name times two, plus one when it filters
(TEXTURE FILTER SMOOTH or DITHERED). The filter is a texture's own in
OpenGL, set when it is made: one the menu changes is set again on the
textures drawn after (texture_bind). */
#define TEXTURE_NAME(texture) ((unsigned)((texture)->hardware >> 1))
#define TEXTURE_SMOOTH(texture) ((int)((texture)->hardware & 1))

/* the bound texture's filter: between texels and levels (trilinear, and
anisotropic where the driver can), or the texels squares */
static void texture_filter(int smooth)
{
	g.gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, smooth ? GL_LINEAR : GL_NEAREST);
	g.gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, smooth ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST);
	if (g.anisotropy > 1.0f)
		g.gl.TexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, smooth ? g.anisotropy : 1.0f);
}

/* a side of an 8-bit level (log2 of the largest's side, the level) */
static int level_side(int shift, int level)
{
	return 1 << (shift > level ? shift - level : 0);
}

/* RGBA bytes' next level down, averaged (in place: the level's first
quarter) */
static void halve_rgba(unsigned char *rgba, int *width, int *height)
{
	int w = *width > 1 ? *width / 2 : 1, h = *height > 1 ? *height / 2 : 1;

	for (int y = 0; y < h; y++)
	{
		for (int x = 0; x < w; x++)
		{
			int x0 = *width > 1 ? 2 * x : x, y0 = *height > 1 ? 2 * y : y;
			int x1 = *width > 1 ? x0 + 1 : x0, y1 = *height > 1 ? y0 + 1 : y0;

			for (int c = 0; c < 4; c++)
			{
				unsigned sum = rgba[(y0 * *width + x0) * 4 + c] + rgba[(y0 * *width + x1) * 4 + c] +
					rgba[(y1 * *width + x0) * 4 + c] + rgba[(y1 * *width + x1) * 4 + c];

				rgba[(y * w + x) * 4 + c] = (unsigned char)((sum + 2) / 4);
			}
		}
	}
	*width = w;
	*height = h;
}

/* each level as RGBA bytes, then the rest down to 1x1 averaged; levels
larger than the card takes left out (or, of its full colours, averaged
down to that size first) */
static void texture_upload(struct r_texture *texture)
{
	int alpha = (texture->flags & R_TEXTURE_ALPHA) != 0, alpha_test = (texture->flags & R_TEXTURE_ALPHA_TEST) != 0;
	int format = alpha || alpha_test ? GL_RGBA8 : GL_RGB8;
	int width, height, level = 0, first = 0;
	unsigned char *rgba;
	unsigned name = 0;

	if (texture->argb)
	{
		width = texture->argb_width;
		height = texture->argb_height;
	}
	else
	{
		/* (the first of its 8-bit levels the card takes) */
		while (first + 1 < texture->levels && (level_side(texture->width_shift, first) > g.maximum_texture_size ||
			level_side(texture->height_shift, first) > g.maximum_texture_size))
		{
			first++;
		}
		width = level_side(texture->width_shift, first);
		height = level_side(texture->height_shift, first);
	}
	rgba = malloc((size_t)width * height * 4);
	if (!rgba)
		return;
	g.gl.GenTextures(1, &name);
	texture->hardware = ((unsigned long)name << 1) | (g.smooth ? 1 : 0);
	g.gl.BindTexture(GL_TEXTURE_2D, name);
	g.gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
	/* (its own colours: the largest level, the rest averaged from it) */
	if (texture->argb)
	{
		for (long i = 0; i < (long)width * height; i++)
		{
			unsigned long color = texture->argb[i];
			unsigned char *out = rgba + i * 4;

			out[0] = (unsigned char)(color >> 16);
			out[1] = (unsigned char)(color >> 8);
			out[2] = (unsigned char)color;
			out[3] = alpha_test ? ((color >> 24) >= 0x80 ? 255 : 0) : alpha ? (unsigned char)(color >> 24) : 255;
		}
		free(texture->argb);
		texture->argb = NULL;
		while (width > g.maximum_texture_size || height > g.maximum_texture_size)
			halve_rgba(rgba, &width, &height);
		g.gl.TexImage2D(GL_TEXTURE_2D, level++, format, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	}
	else
	{
		for (int source = first; source < texture->levels; source++)
		{
			int w = level_side(texture->width_shift, source), h = level_side(texture->height_shift, source);
			const unsigned char *texels = texture->texels[source];

			for (long i = 0; i < (long)w * h; i++)
			{
				unsigned long color = texture->palette[texels[i]];
				unsigned char *out = rgba + i * 4;

				out[0] = (unsigned char)(color >> 16);
				out[1] = (unsigned char)(color >> 8);
				out[2] = (unsigned char)color;
				out[3] = alpha_test && !texels[i] ? 0 : alpha ? (unsigned char)(color >> 24) : 255;
			}
			width = w;
			height = h;
			/* (its smallest level still too large: averaged down, the rest from it) */
			if (width > g.maximum_texture_size || height > g.maximum_texture_size)
			{
				while (width > g.maximum_texture_size || height > g.maximum_texture_size)
					halve_rgba(rgba, &width, &height);
				g.gl.TexImage2D(GL_TEXTURE_2D, level++, format, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
				break;
			}
			g.gl.TexImage2D(GL_TEXTURE_2D, level++, format, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
		}
	}
	/* (the last level halved to 1x1: a mipmapped texture needs them all) */
	while (width > 1 || height > 1)
	{
		halve_rgba(rgba, &width, &height);
		g.gl.TexImage2D(GL_TEXTURE_2D, level++, format, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	}
	free(rgba);
	texture_filter(g.smooth);
	g.gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (texture->flags & R_TEXTURE_CLAMP) ? g.clamp : GL_REPEAT);
	g.gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (texture->flags & R_TEXTURE_CLAMP) ? g.clamp : GL_REPEAT);
}

/* a texture just made (r_texture.c): to the card now, in its full colours,
which it then gives up (the batch drawing ended first: no texture is made
between glBegin and glEnd) */
static void gl_texture_created(struct r_texture *texture)
{
	if (!g.active || texture->hardware)
		return;
	batch_end();
	/* (the filter as the settings have it now: a map's textures are made
	before its first frame clears) */
	g.smooth = r_frame.settings.texture_smooth || r_frame.settings.texture_dither;
	texture_upload(texture);
	/* (bound now: the next batch binds its own) */
	g.texture = texture;
}

/* a texture bound for drawing (made on the card first, if it is not yet),
its filter the settings' now */
static void texture_bind(const struct r_texture *texture)
{
	struct r_texture *card = (struct r_texture *)texture;

	if (!card->hardware)
		texture_upload(card);
	g.gl.BindTexture(GL_TEXTURE_2D, TEXTURE_NAME(card));
	if (card->hardware && TEXTURE_SMOOTH(card) != (g.smooth != 0))
	{
		texture_filter(g.smooth);
		card->hardware ^= 1;
	}
}

static int gl_draws_detail(void)
{
	return g.active && g.combine;
}

static void gl_texture_delete(struct r_texture *texture)
{
	unsigned name = TEXTURE_NAME(texture);

	if (!g.active || !name)
		return;
	batch_end();
	g.gl.DeleteTextures(1, &name);
	texture->hardware = 0;
	/* (a texture made later in its place is bound again) */
	if (g.texture == texture)
		g.texture = NULL;
}

/* ---------- the batch's state */

/* the texture environment, all of it (state_reset): the light at the
texel's (MODULATE: the combine extension's, its third source and constant
there for a detail map's) */
static void texture_environment_reset(void)
{
	static const float grey[4] = { 0.5f, 0.5f, 0.5f, 0.5f };

	if (!g.combine)
	{
		g.gl.TexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
		return;
	}
	g.gl.TexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
	g.gl.TexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);
	g.gl.TexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE);
	g.gl.TexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);
	g.gl.TexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_PRIMARY_COLOR);
	g.gl.TexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);
	g.gl.TexEnvi(GL_TEXTURE_ENV, GL_SOURCE2_RGB, GL_PRIMARY_COLOR);
	g.gl.TexEnvi(GL_TEXTURE_ENV, GL_OPERAND2_RGB, GL_SRC_COLOR);
	g.gl.TexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, grey);
	g.gl.TexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_MODULATE);
	g.gl.TexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_TEXTURE);
	g.gl.TexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
	g.gl.TexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_ALPHA, GL_PRIMARY_COLOR);
	g.gl.TexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_ALPHA, GL_SRC_ALPHA);
	g.gl.TexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, 1.0f);
	g.environment_rgb = GL_MODULATE;
	g.environment_scale = 1;
}

/* the texture environment of a batch: the light at the texel's, or twice
it (scale); or a detail map's, the texel toward grey (0x80) as the vertex's
colour is below white (INTERPOLATE: texel times colour, plus grey times one
minus it). Only what differs from the batch before is set: the two share
the rest (texture_environment_reset), a driver's checks are made for each
call. */
static void texture_environment(int detail, int scale)
{
	unsigned rgb = detail ? GL_INTERPOLATE : GL_MODULATE;

	if (!g.combine)
		return;
	if (rgb != g.environment_rgb)
	{
		g.gl.TexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, (int)rgb);
		g.gl.TexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, detail ? GL_CONSTANT : GL_PRIMARY_COLOR);
		g.environment_rgb = rgb;
	}
	if ((scale ? 2 : 1) != g.environment_scale)
	{
		g.gl.TexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, scale ? 2.0f : 1.0f);
		g.environment_scale = scale ? 2 : 1;
	}
}

/* the fog's colour: the view's, or grey for a detail map (fogged toward
no change) */
static void fog_color(int detail)
{
	float color[4];

	color[0] = detail ? 0.5f : (float)((g.fog_color >> 16) & 0xFF) / 255.0f;
	color[1] = detail ? 0.5f : (float)((g.fog_color >> 8) & 0xFF) / 255.0f;
	color[2] = detail ? 0.5f : (float)(g.fog_color & 0xFF) / 255.0f;
	color[3] = 1.0f;
	g.gl.Fogfv(GL_FOG_COLOR, color);
}

/* the batch drawn so far ended and the next one's state set, if it is not
the same */
static void batch_state(unsigned state, const struct r_texture *texture)
{
	unsigned changed;

	if (g.begun && state == g.state && texture == g.texture)
		return;
	batch_end();
	changed = state ^ g.state;
	if (texture != g.texture || (changed & STATE_TEXTURE))
	{
		if (texture)
			texture_bind(texture);
	}
	if (changed & STATE_TEXTURE)
	{
		if (state & STATE_TEXTURE)
			g.gl.Enable(GL_TEXTURE_2D);
		else
			g.gl.Disable(GL_TEXTURE_2D);
		/* (an untextured batch's texture coordinates are not made) */
		if (g.arrays && (state & STATE_TEXTURE))
			g.gl.EnableClientState(GL_TEXTURE_COORD_ARRAY);
		else if (g.arrays)
			g.gl.DisableClientState(GL_TEXTURE_COORD_ARRAY);
	}
	if (changed & STATE_DETAIL)
	{
		g.gl.DepthFunc((state & STATE_DETAIL) ? GL_LEQUAL : GL_LESS);
		fog_color((state & STATE_DETAIL) != 0);
	}
	if (changed & (STATE_SCALE | STATE_DETAIL))
		texture_environment((state & STATE_DETAIL) != 0, (state & STATE_SCALE) != 0);
	if (changed & STATE_DEPTH_TEST)
	{
		if (state & STATE_DEPTH_TEST)
			g.gl.Enable(GL_DEPTH_TEST);
		else
			g.gl.Disable(GL_DEPTH_TEST);
	}
	if (changed & STATE_DEPTH_WRITE)
		g.gl.DepthMask((state & STATE_DEPTH_WRITE) ? 1 : 0);
	if (changed & STATE_ALPHA_TEST)
	{
		if (state & STATE_ALPHA_TEST)
			g.gl.Enable(GL_ALPHA_TEST);
		else
			g.gl.Disable(GL_ALPHA_TEST);
	}
	if (changed & STATE_FOG)
	{
		if (state & STATE_FOG)
			g.gl.Enable(GL_FOG);
		else
			g.gl.Disable(GL_FOG);
	}
	if (changed & (STATE_BLEND_ALPHA | STATE_BLEND_ADD | STATE_BLEND_ADD_ALPHA | STATE_BLEND_PREMULTIPLIED |
		STATE_BLEND_DETAIL))
	{
		if (state & STATE_BLEND_ALPHA)
			g.gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		else if (state & STATE_BLEND_ADD)
			g.gl.BlendFunc(GL_ONE, GL_ONE);
		else if (state & STATE_BLEND_ADD_ALPHA)
			g.gl.BlendFunc(GL_SRC_ALPHA, GL_ONE);
		else if (state & STATE_BLEND_PREMULTIPLIED)
			g.gl.BlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		else if (state & STATE_BLEND_DETAIL)
			g.gl.BlendFunc(GL_DST_COLOR, GL_SRC_COLOR);
		if (state & (STATE_BLEND_ALPHA | STATE_BLEND_ADD | STATE_BLEND_ADD_ALPHA | STATE_BLEND_PREMULTIPLIED |
			STATE_BLEND_DETAIL))
			g.gl.Enable(GL_BLEND);
		else
			g.gl.Disable(GL_BLEND);
	}
	if (changed & STATE_IN_FRONT)
	{
		if (state & STATE_IN_FRONT)
			g.gl.DepthRange(0.0, IN_FRONT_DEPTH);
		else
			g.gl.DepthRange(IN_FRONT_DEPTH, 1.0);
	}
	g.state = state;
	g.texture = texture;
	g.begun = TRUE;
}

/* the whole state set again, OpenGL's as the batches assume it */
static void state_reset(void)
{
	batch_end();
	/* (a detail map's grey fog: the view's again) */
	if (g.state & STATE_DETAIL)
		fog_color(FALSE);
	g.gl.Disable(GL_TEXTURE_2D);
	g.gl.Disable(GL_DEPTH_TEST);
	/* (no depth written, as state 0 says: batch_state changes the mask only
	when STATE_DEPTH_WRITE changes, and left on here a frame's first blended
	triangles, before any opaque one, wrote their depth: a sky's layers hid
	those drawn after them, b30's fog band) */
	g.gl.DepthMask(0);
	g.gl.Disable(GL_ALPHA_TEST);
	g.gl.Disable(GL_FOG);
	g.gl.Disable(GL_BLEND);
	g.gl.DepthRange(IN_FRONT_DEPTH, 1.0);
	g.gl.DepthFunc(GL_LESS);
	g.gl.AlphaFunc(GL_GREATER, 0.5f);
	g.gl.Disable(GL_CULL_FACE);
	g.gl.ShadeModel(GL_SMOOTH);
	g.gl.Hint(GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST);
	/* (the fog each pixel's, where the driver can: fogged at the vertices
	alone, a large triangle reaching past the fog's end is fogged too little
	between them) */
	g.gl.Hint(GL_FOG_HINT, GL_NICEST);
	g.gl.Fogi(GL_FOG_MODE, GL_LINEAR);
	texture_environment_reset();
	if (g.arrays)
		g.gl.DisableClientState(GL_TEXTURE_COORD_ARRAY);
	g.state = 0;
	g.texture = NULL;
}

/* ---------- start-up and the window */

static const struct r_backend gl_backend;

static int has_extension(const char *extensions, const char *name)
{
	size_t length = strlen(name);

	for (const char *at = extensions; at && (at = strstr(at, name)) != NULL; at += length)
	{
		if ((at == extensions || at[-1] == ' ') && (at[length] == ' ' || !at[length]))
			return TRUE;
	}
	return FALSE;
}

/* whether the driver's OpenGL is at least major.minor (GL_VERSION begins
"major.minor") */
static int version_at_least(const char *version, int major, int minor)
{
	int have_major = 0, have_minor = 0;

	if (!version)
		return FALSE;
	while (*version >= '0' && *version <= '9')
		have_major = have_major * 10 + *version++ - '0';
	if (*version == '.')
	{
		version++;
		while (*version >= '0' && *version <= '9')
			have_minor = have_minor * 10 + *version++ - '0';
	}
	return have_major > major || (have_major == major && have_minor >= minor);
}

int r_gl_use(const struct r_gl_functions *functions)
{
	const void *const *pointer = (const void *const *)functions;
	/* (the vertex arrays' functions last: those a driver may go without) */
	size_t required = offsetof(struct r_gl_functions, EnableClientState) / sizeof(void *);
	const char *extensions, *version;

	for (size_t i = 0; i < required; i++)
	{
		if (!pointer[i])
			return FALSE;
	}
	memset(&g, 0, sizeof(g));
	g.gl = *functions;
	g.active = TRUE;
	g.arrays = TRUE;
	for (size_t i = required; i < sizeof(*functions) / sizeof(void *); i++)
	{
		if (!pointer[i])
			g.arrays = FALSE;
	}
	memset(&batch, 0, sizeof(batch));
	batch.generation = 1;
	if (g.arrays)
	{
		/* (the batch's arrays never move: given once) */
		g.gl.VertexPointer(4, GL_FLOAT, 0, batch.position);
		g.gl.ColorPointer(4, GL_UNSIGNED_BYTE, 0, batch.color);
		g.gl.TexCoordPointer(2, GL_FLOAT, 0, batch.texcoord);
		g.gl.EnableClientState(GL_VERTEX_ARRAY);
		g.gl.EnableClientState(GL_COLOR_ARRAY);
	}
	g.alpha_scale = 256;
	g.z_near = 0.0625f;
	g.viewport_width = g.viewport_height = 1;
	g.viewport_changed = TRUE;
	extensions = (const char *)g.gl.GetString(GL_EXTENSIONS);
	version = (const char *)g.gl.GetString(GL_VERSION);
	/* (OpenGL 1.3's own, which a driver need not list any more) */
	g.combine = has_extension(extensions, "GL_ARB_texture_env_combine") ||
		has_extension(extensions, "GL_EXT_texture_env_combine") || version_at_least(version, 1, 3);
	g.smooth = r_frame.settings.texture_smooth || r_frame.settings.texture_dither;
	g.anisotropy = 1.0f;
	if (has_extension(extensions, "GL_EXT_texture_filter_anisotropic") ||
		has_extension(extensions, "GL_ARB_texture_filter_anisotropic"))
	{
		float most = 1.0f;

		g.gl.GetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &most);
		g.anisotropy = most > 8.0f ? 8.0f : most;
	}
	{
		/* (a power of two, at least OpenGL's least, 64) */
		float most = 0.0f;

		g.gl.GetFloatv(GL_MAX_TEXTURE_SIZE, &most);
		g.maximum_texture_size = 64;
		while (g.maximum_texture_size < 16384 && (float)(g.maximum_texture_size * 2) <= most)
			g.maximum_texture_size *= 2;
	}
	g.clamp = version_at_least(version, 1, 2) ||
		has_extension(extensions, "GL_EXT_texture_edge_clamp") || has_extension(extensions, "GL_SGIS_texture_edge_clamp") ?
		GL_CLAMP_TO_EDGE : GL_CLAMP;
	state_reset();
	r_backend = &gl_backend;
	return TRUE;
}

void r_gl_viewport(int x, int y, int width, int height, int window_width, int window_height)
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

void r_gl_flush(void)
{
	if (g.active)
		batch_end();
}

/* the viewport and the projection, at a frame's start (r_frame's size is
the picture's by then) */
static void viewport_apply(void)
{
	float matrix[16];

	g.viewport_changed = FALSE;
	g.gl.Viewport(g.viewport_x, g.viewport_y, g.viewport_width, g.viewport_height);
	/* (the picture's pixels, y down, to the viewport; z and w as given) */
	memset(matrix, 0, sizeof(matrix));
	matrix[0] = 2.0f / (float)r_frame.width;
	matrix[5] = -2.0f / (float)r_frame.height;
	matrix[10] = 1.0f;
	matrix[12] = -1.0f;
	matrix[13] = 1.0f;
	matrix[15] = 1.0f;
	g.gl.MatrixMode(GL_PROJECTION);
	g.gl.LoadMatrixf(matrix);
	g.gl.MatrixMode(GL_MODELVIEW);
	g.gl.LoadIdentity();
	g.gl.MatrixMode(GL_TEXTURE);
	g.gl.LoadIdentity();
	g.gl.MatrixMode(GL_MODELVIEW);
}

/* a rectangle of the picture as the window's (OpenGL's, from the bottom
left) */
static void scissor_picture(int x0, int y0, int x1, int y1)
{
	int left = g.viewport_x + (int)((long)x0 * g.viewport_width / r_frame.width);
	int right = g.viewport_x + (int)((long)x1 * g.viewport_width / r_frame.width);
	int top = g.viewport_y + (int)((long)(r_frame.height - y0) * g.viewport_height / r_frame.height);
	int bottom = g.viewport_y + (int)((long)(r_frame.height - y1) * g.viewport_height / r_frame.height);

	g.gl.Scissor(left, bottom, right - left, top - bottom);
}

static void gl_read_back(void)
{
	unsigned char *rgba;

	if (!g.active || !r_frame.pixels || r_frame.bits != 32)
		return;
	batch_end();
	rgba = malloc((size_t)g.viewport_width * g.viewport_height * 4);
	if (!rgba)
		return;
	g.gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
	g.gl.ReadPixels(g.viewport_x, g.viewport_y, g.viewport_width, g.viewport_height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	/* (the viewport's pixels, bottom up, at the picture's size) */
	for (int y = 0; y < r_frame.height; y++)
	{
		unsigned long *row = (unsigned long *)(r_frame.pixels + (size_t)y * r_frame.pitch);
		int source_y = g.viewport_height - 1 - (int)((long)y * g.viewport_height / r_frame.height);

		for (int x = 0; x < r_frame.width; x++)
		{
			const unsigned char *p = rgba + ((size_t)source_y * g.viewport_width +
				(size_t)((long)x * g.viewport_width / r_frame.width)) * 4;

			row[x] = ((unsigned long)p[0] << 16) | ((unsigned long)p[1] << 8) | p[2];
		}
	}
	free(rgba);
}

/* ---------- the seam */

static void gl_set_near(float z_near)
{
	if (z_near > 0.0f)
		g.z_near = z_near;
	/* (the vertices made so far had the old plane's depth) */
	batch.generation++;
	if (g.active && g.fogged)
	{
		/* (the eye's z is the depth less twice the near plane) */
		batch_end();
		g.gl.Fogf(GL_FOG_START, g.fog_start - 2.0f * g.z_near);
		g.gl.Fogf(GL_FOG_END, g.fog_end - 2.0f * g.z_near);
	}
}

static void gl_set_in_front(int in_front)
{
	g.in_front = in_front;
}

static void gl_set_alpha_scale(unsigned long scale)
{
	g.alpha_scale = scale > 256 ? 256 : scale;
}

static void gl_screen_blend(int blend)
{
	g.screen_blend = blend;
}

static void gl_view_fog(int fogged, float fog_start, float fog_end, unsigned long color)
{
	if (!g.active)
		return;
	g.fogged = fogged;
	g.fog_start = fog_start;
	g.fog_end = fog_end;
	g.fog_color = color;
	if (!fogged)
		return;
	batch_end();
	/* (the view's colour, or grey still while a detail map's state is
	OpenGL's: batch_state sets the colour only when that changes) */
	fog_color((g.state & STATE_DETAIL) != 0);
	gl_set_near(g.z_near);
}

static void gl_clear(unsigned long color)
{
	if (!g.active)
		return;
	batch_end();
	g.smooth = r_frame.settings.texture_smooth || r_frame.settings.texture_dither;
	if (g.viewport_changed)
		viewport_apply();
	/* (the window's borders black, the picture the colour) */
	g.gl.Disable(GL_SCISSOR_TEST);
	g.gl.DepthMask(1);
	g.gl.ClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	g.gl.Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	g.gl.Enable(GL_SCISSOR_TEST);
	scissor_picture(0, 0, r_frame.width, r_frame.height);
	g.gl.ClearColor((float)((color >> 16) & 0xFF) / 255.0f, (float)((color >> 8) & 0xFF) / 255.0f,
		(float)(color & 0xFF) / 255.0f, 0.0f);
	g.gl.Clear(GL_COLOR_BUFFER_BIT);
	g.gl.Disable(GL_SCISSOR_TEST);
	g.view_active = FALSE;
	state_reset();
}

/* the scissor as the view wants it: its rectangle, or none */
static void scissor_view(void)
{
	if (g.view_active)
	{
		g.gl.Enable(GL_SCISSOR_TEST);
		scissor_picture(g.view_x0, g.view_y0, g.view_x1, g.view_y1);
	}
	else
	{
		g.gl.Disable(GL_SCISSOR_TEST);
	}
}

static void gl_view_rect(int x0, int y0, int x1, int y1, int active)
{
	if (!g.active)
		return;
	batch_end();
	g.view_active = active;
	g.view_x0 = x0;
	g.view_y0 = y0;
	g.view_x1 = x1;
	g.view_y1 = y1;
	scissor_view();
}

static void gl_clear_depth(int x0, int y0, int x1, int y1)
{
	if (!g.active)
		return;
	batch_end();
	g.gl.Enable(GL_SCISSOR_TEST);
	scissor_picture(x0, y0, x1, y1);
	g.gl.DepthMask(1);
	g.gl.Clear(GL_DEPTH_BUFFER_BIT);
	scissor_view();
	g.gl.DepthMask((g.state & STATE_DEPTH_WRITE) ? 1 : 0);
}

static unsigned screen_state(int textured)
{
	unsigned state = STATE_SCREEN | (textured ? STATE_TEXTURE : 0);

	switch (g.screen_blend)
	{
	case _r_screen_alpha: state |= STATE_BLEND_ALPHA; break;
	case _r_screen_add: state |= STATE_BLEND_ADD_ALPHA; break;
	case _r_screen_premultiplied: state |= STATE_BLEND_PREMULTIPLIED; break;
	default: break;
	}
	return state;
}

/* a corner of a triangle in the world, in the batch (state its state):
its index, the vertex made the first time the batch has it */
static unsigned short world_vertex(const struct r_raster_vertex *v, unsigned state, unsigned long alpha)
{
	unsigned slot = (unsigned)((((unsigned long)(size_t)v >> 2) * 2654435761UL) >> 24) & (VERTEX_CACHE - 1);
	unsigned long *key = batch.cache[slot].key;
	unsigned long fog_bits = float_bits(&v->fog);
	int k;
	float z, light, unfog = 1.0f;

	if (batch.cache[slot].generation == batch.generation && key[0] == float_bits(&v->x) &&
		key[1] == float_bits(&v->y) && key[2] == float_bits(&v->w) && key[3] == float_bits(&v->s) &&
		key[4] == float_bits(&v->t) && key[5] == float_bits(&v->r) && key[6] == float_bits(&v->g) &&
		key[7] == float_bits(&v->b) && key[8] == fog_bits && batch.cache[slot].alpha == alpha)
	{
		return (unsigned short)batch.cache[slot].index;
	}
	k = batch.vertices++;
	batch.cache[slot].generation = batch.generation;
	key[0] = float_bits(&v->x);
	key[1] = float_bits(&v->y);
	key[2] = float_bits(&v->w);
	key[3] = float_bits(&v->s);
	key[4] = float_bits(&v->t);
	key[5] = float_bits(&v->r);
	key[6] = float_bits(&v->g);
	key[7] = float_bits(&v->b);
	key[8] = fog_bits;
	batch.cache[slot].alpha = alpha;
	batch.cache[slot].index = (unsigned long)k;
	z = 1.0f / v->w;
	/* (rounded to a float before the products, as the depth the card is
	given: the FPU's registers hold more) */
	__asm__("" : "+m"(z));
	/* (the light before the front end's fog: OpenGL fogs it again; a detail
	map's grey is its strength, as it is. No fog, nothing to undo: 1 / (1 -
	0) is 1) */
	if ((state & STATE_FOG) && (fog_bits & 0x7FFFFFFFUL))
	{
		float fog = v->fog / (float)(R_FOG_LEVELS - 1);

		if (fog < 0.999f)
			unfog = 1.0f / (1.0f - fog);
	}
	light = (state & (STATE_SCALE | STATE_DETAIL)) ? unfog : 2.0f * unfog;
	batch.color[k][0] = clamp_byte(v->r * light);
	batch.color[k][1] = clamp_byte(v->g * light);
	batch.color[k][2] = clamp_byte(v->b * light);
	batch.color[k][3] = (unsigned char)alpha;
	if (state & STATE_TEXTURE)
	{
		batch.texcoord[k][0] = v->s * z;
		batch.texcoord[k][1] = v->t * z;
	}
	batch.position[k][0] = v->x * z;
	batch.position[k][1] = v->y * z;
	batch.position[k][2] = z - 2.0f * g.z_near;
	batch.position[k][3] = z;
	return (unsigned short)k;
}

/* a corner of a screen triangle (its colour 0-255 as the texel, its alpha
in fog_r) */
static unsigned short screen_vertex(const struct r_raster_vertex *v, int textured)
{
	int k = batch.vertices++;

	batch.color[k][0] = clamp_byte(v->r);
	batch.color[k][1] = clamp_byte(v->g);
	batch.color[k][2] = clamp_byte(v->b);
	batch.color[k][3] = clamp_byte(v->fog_r);
	if (textured)
	{
		batch.texcoord[k][0] = v->u;
		batch.texcoord[k][1] = v->v;
	}
	batch.position[k][0] = v->x;
	batch.position[k][1] = v->y;
	batch.position[k][2] = 0.0f;
	batch.position[k][3] = 1.0f;
	return (unsigned short)k;
}

static void gl_triangle(const struct r_raster_vertex *a, const struct r_raster_vertex *b, const struct r_raster_vertex *c,
	const struct r_texture *texture, unsigned flags)
{
	unsigned state;
	unsigned long alpha = 255;
	unsigned short *index;

	if (flags & R_RASTER_SCREEN)
	{
		state = screen_state(texture != NULL);
		if (texture && (texture->flags & R_TEXTURE_ALPHA_TEST))
			state |= STATE_ALPHA_TEST;
		batch_state(state, texture);
		batch_room(3, 3);
		index = batch.index + batch.indices;
		index[0] = screen_vertex(a, texture != NULL);
		index[1] = screen_vertex(b, texture != NULL);
		index[2] = screen_vertex(c, texture != NULL);
		batch.indices += 3;
		return;
	}
	state = (texture ? STATE_TEXTURE : 0) | (g.in_front ? STATE_IN_FRONT : 0);
	if (!(flags & R_DRAW_NO_DEPTH_TEST))
		state |= STATE_DEPTH_TEST;
	if ((flags & R_DRAW_DETAIL) && texture)
		state |= STATE_DETAIL | STATE_BLEND_DETAIL;
	else if (flags & R_DRAW_ADDITIVE)
		state |= STATE_BLEND_ADD;
	else if (flags & R_DRAW_BLEND)
	{
		state |= STATE_BLEND_ALPHA;
		alpha = (flags & R_DRAW_ALPHA) ? (unsigned char)(g.alpha_scale * 255 / 256) : 128;
	}
	else
		state |= STATE_DEPTH_WRITE;
	if (texture && (texture->flags & R_TEXTURE_ALPHA_TEST))
		state |= STATE_ALPHA_TEST;
	/* (added light is faded by the front end: R_DRAW_ADDITIVE) */
	if (g.fogged && !(flags & (R_DRAW_NO_FOG | R_DRAW_ADDITIVE)))
		state |= STATE_FOG;
	if (texture && g.combine && !(state & STATE_DETAIL))
		state |= STATE_SCALE;
	batch_state(state, texture);
	batch_room(3, 3);
	index = batch.index + batch.indices;
	index[0] = world_vertex(a, state, alpha);
	index[1] = world_vertex(b, state, alpha);
	index[2] = world_vertex(c, state, alpha);
	batch.indices += 3;
}

/* a rectangle (x0, y0) to (x1, y1) of the picture as two triangles, its
corners top left, top right, bottom right, bottom left: (u0, v0) to (u1,
v1) of the texture if textured */
static void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, int textured,
	unsigned char red, unsigned char green, unsigned char blue, unsigned char alpha)
{
	const float xs[4] = { x0, x1, x1, x0 }, ys[4] = { y0, y0, y1, y1 };
	const float us[4] = { u0, u1, u1, u0 }, vs[4] = { v0, v0, v1, v1 };
	int k;
	unsigned short *index;

	batch_room(4, 6);
	k = batch.vertices;
	for (int i = 0; i < 4; i++)
	{
		batch.color[k + i][0] = red;
		batch.color[k + i][1] = green;
		batch.color[k + i][2] = blue;
		batch.color[k + i][3] = alpha;
		if (textured)
		{
			batch.texcoord[k + i][0] = us[i];
			batch.texcoord[k + i][1] = vs[i];
		}
		batch.position[k + i][0] = xs[i];
		batch.position[k + i][1] = ys[i];
		batch.position[k + i][2] = 0.0f;
		batch.position[k + i][3] = 1.0f;
	}
	batch.vertices += 4;
	index = batch.index + batch.indices;
	index[0] = (unsigned short)k;
	index[1] = (unsigned short)(k + 1);
	index[2] = (unsigned short)(k + 2);
	index[3] = (unsigned short)k;
	index[4] = (unsigned short)(k + 2);
	index[5] = (unsigned short)(k + 3);
	batch.indices += 6;
}

static void gl_fill_row(int x0, int x1, int y, unsigned long color)
{
	if (x0 >= x1)
		return;
	batch_state(0, NULL);
	quad((float)x0, (float)y, (float)x1, (float)(y + 1), 0.0f, 0.0f, 0.0f, 0.0f, FALSE, (unsigned char)(color >> 16),
		(unsigned char)(color >> 8), (unsigned char)color, 255);
}

static void gl_screen_rect(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
	const struct r_texture *texture, unsigned long color)
{
	unsigned state = screen_state(texture != NULL);

	if (texture && (texture->flags & R_TEXTURE_ALPHA_TEST))
		state |= STATE_ALPHA_TEST;
	batch_state(state, texture);
	quad(x0, y0, x1, y1, u0, v0, u1, v1, texture != NULL, (unsigned char)(color >> 16), (unsigned char)(color >> 8),
		(unsigned char)color, (unsigned char)(color >> 24));
}

static void gl_fill_rectangle(int x0, int y0, int x1, int y1, unsigned long argb)
{
	if (x0 >= x1 || y0 >= y1)
		return;
	batch_state(STATE_SCREEN | STATE_BLEND_ALPHA, NULL);
	quad((float)x0, (float)y0, (float)x1, (float)y1, 0.0f, 0.0f, 0.0f, 0.0f, FALSE, (unsigned char)(argb >> 16),
		(unsigned char)(argb >> 8), (unsigned char)argb, (unsigned char)(argb >> 24));
}

/* ---------- glyphs and pictures

A glyph's coverage and a picture go to the card as textures of their own,
padded to powers of two. The padding repeats the image's last column and
row: a filter that reaches past the image's edge (the picture magnified
into a larger window) finds its own texels there, not whatever the driver
left in a texture made without texels. */

/* the texture's linear filter, its edges kept (no border's colour) */
static void image_parameters(void)
{
	g.gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	g.gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	g.gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, g.clamp);
	g.gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, g.clamp);
}

/* the batch's texture while it is a glyph's (gl_coverage: its name in
coverage_name), not a struct r_texture */
#define COVERAGE_BATCH ((const struct r_texture *)2)

static void gl_coverage(int x, int y, const unsigned char *cover, int width, int height, unsigned long argb,
	unsigned long *handle, int clip_x0, int clip_y0, int clip_x1, int clip_y1)
{
	unsigned names[1] = { (unsigned)*handle }, *name = names;

	int texture_width = power_of_two(width), texture_height = power_of_two(height);
	int clipped = x < clip_x0 || y < clip_y0 || x + width > clip_x1 || y + height > clip_y1;

	if (!g.active || width <= 0 || height <= 0)
		return;
	if (x >= clip_x1 || y >= clip_y1 || x + width <= clip_x0 || y + height <= clip_y0)
		return;
	/* (larger than the card takes: a glyph that size is not text) */
	if (texture_width > g.maximum_texture_size || texture_height > g.maximum_texture_size)
		return;
	/* (a glyph's shadow is drawn first from the same texture: the glyph goes
	in its batch, after it) */
	if (!clipped && *name && g.begun && g.texture == COVERAGE_BATCH && g.coverage_name == *name &&
		g.state == (STATE_SCREEN | STATE_TEXTURE | STATE_BLEND_ALPHA))
	{
		quad((float)x, (float)y, (float)(x + width), (float)(y + height), 0.0f, 0.0f,
			(float)width / (float)texture_width, (float)height / (float)texture_height, TRUE,
			(unsigned char)(argb >> 16), (unsigned char)(argb >> 8), (unsigned char)argb, (unsigned char)(argb >> 24));
		return;
	}
	batch_end();
	if (!*name)
	{
		/* (white, the coverage its alpha: the texture's colour counts in
		the combine extension's modulation) */
		unsigned char *texels = malloc((size_t)texture_width * texture_height * 2);

		if (!texels)
			return;
		for (int row = 0; row < texture_height; row++)
		{
			const unsigned char *source = cover + (size_t)(row < height ? row : height - 1) * width;
			unsigned char *out = texels + (size_t)row * texture_width * 2;

			for (int column = 0; column < texture_width; column++)
			{
				out[column * 2] = 255;
				out[column * 2 + 1] = source[column < width ? column : width - 1];
			}
		}
		g.gl.GenTextures(1, name);
		g.gl.BindTexture(GL_TEXTURE_2D, *name);
		g.gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
		g.gl.TexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, texture_width, texture_height, 0, GL_LUMINANCE_ALPHA,
			GL_UNSIGNED_BYTE, texels);
		free(texels);
		image_parameters();
		*handle = *name;
	}
	if (clipped)
	{
		g.gl.Enable(GL_SCISSOR_TEST);
		scissor_picture(clip_x0, clip_y0, clip_x1, clip_y1);
	}
	/* (an alpha texture of its own: not a struct r_texture, bound here) */
	batch_state(STATE_SCREEN | STATE_TEXTURE | STATE_BLEND_ALPHA, NULL);
	batch_end();
	g.gl.BindTexture(GL_TEXTURE_2D, *name);
	g.begun = TRUE;
	quad((float)x, (float)y, (float)(x + width), (float)(y + height), 0.0f, 0.0f,
		(float)width / (float)texture_width, (float)height / (float)texture_height, TRUE, (unsigned char)(argb >> 16),
		(unsigned char)(argb >> 8), (unsigned char)argb, (unsigned char)(argb >> 24));
	if (clipped)
	{
		batch_end();
		scissor_view();
		/* (the next batch binds its own texture again) */
		g.texture = (const struct r_texture *)1;
		return;
	}
	/* (left open for the glyph over its shadow; any other batch binds its
	own texture again) */
	g.texture = COVERAGE_BATCH;
	g.coverage_name = *name;
}

static void gl_coverage_forget(unsigned long handle)
{
	unsigned name = (unsigned)handle;

	if (!g.active || !name)
		return;
	batch_end();
	g.gl.DeleteTextures(1, &name);
}

/* a picture slot's textures given back */
static void picture_forget(int slot)
{
	if (g.pictures[slot].names)
	{
		g.gl.DeleteTextures(g.pictures[slot].columns * g.pictures[slot].rows, g.pictures[slot].names);
		free(g.pictures[slot].names);
	}
	g.pictures[slot].names = NULL;
	g.pictures[slot].columns = g.pictures[slot].rows = 0;
}

/* a picture to the card: one texture, or (larger than the card takes, a
title picture on a 3dfx card's 256 texels) tiles as large as it takes;
FALSE without memory */
static int picture_make(int slot, const unsigned long *argb, int width, int height)
{
	int tile = g.maximum_texture_size;
	int columns = (width + tile - 1) / tile, rows = (height + tile - 1) / tile;
	int texture_width = power_of_two(width < tile ? width : tile), texture_height = power_of_two(height < tile ? height : tile);
	unsigned char *rgba = malloc((size_t)texture_width * texture_height * 4);
	unsigned *names = calloc((size_t)columns * rows, sizeof(*names));

	if (!rgba || !names)
	{
		free(rgba);
		free(names);
		return FALSE;
	}
	g.gl.GenTextures(columns * rows, names);
	g.gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
	for (int row = 0; row < rows; row++)
	{
		for (int column = 0; column < columns; column++)
		{
			int x0 = column * tile, y0 = row * tile;
			int w = width - x0 < tile ? width - x0 : tile, h = height - y0 < tile ? height - y0 : tile;
			int tw = power_of_two(w), th = power_of_two(h);

			for (int ty = 0; ty < th; ty++)
			{
				const unsigned long *source = argb + (size_t)(y0 + (ty < h ? ty : h - 1)) * width + x0;
				unsigned char *out = rgba + (size_t)ty * tw * 4;

				for (int tx = 0; tx < tw; tx++)
				{
					unsigned long texel = source[tx < w ? tx : w - 1];

					out[tx * 4] = (unsigned char)(texel >> 16);
					out[tx * 4 + 1] = (unsigned char)(texel >> 8);
					out[tx * 4 + 2] = (unsigned char)texel;
					out[tx * 4 + 3] = (unsigned char)(texel >> 24);
				}
			}
			g.gl.BindTexture(GL_TEXTURE_2D, names[row * columns + column]);
			g.gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
			image_parameters();
		}
	}
	free(rgba);
	g.pictures[slot].argb = argb;
	g.pictures[slot].width = width;
	g.pictures[slot].height = height;
	g.pictures[slot].names = names;
	g.pictures[slot].tile = tile;
	g.pictures[slot].columns = columns;
	g.pictures[slot].rows = rows;
	return TRUE;
}

static void gl_picture(int x, int y, const unsigned long *argb, int width, int height)
{
	unsigned long check = 0;
	int slot = -1, oldest = 0;

	if (!g.active || width <= 0 || height <= 0)
		return;
	/* (the same picture as before: its address, size and texels across the
	whole of it -- a loading screen's is made again at the same address for
	the next map, the same size, its left column black like the last's) */
	for (int i = 0; i < 32; i++)
	{
		const unsigned long *row = argb + (size_t)((2 * i + 1) * height / 64) * width;

		for (int j = 0; j < 32; j++)
			check = check * 31 + row[(2 * j + 1) * width / 64];
	}
	g.clock++;
	for (int i = 0; i < (int)(sizeof(g.pictures) / sizeof(g.pictures[0])); i++)
	{
		if (g.pictures[i].names && g.pictures[i].argb == argb && g.pictures[i].width == width &&
			g.pictures[i].height == height && g.pictures[i].check == check)
		{
			slot = i;
			break;
		}
		if (g.pictures[i].used < g.pictures[oldest].used)
			oldest = i;
	}
	batch_end();
	if (slot < 0)
	{
		slot = oldest;
		picture_forget(slot);
		if (!picture_make(slot, argb, width, height))
			return;
		g.pictures[slot].check = check;
	}
	g.pictures[slot].used = g.clock;
	batch_state(STATE_SCREEN | STATE_TEXTURE | STATE_BLEND_ALPHA, NULL);
	batch_end();
	for (int row = 0; row < g.pictures[slot].rows; row++)
	{
		for (int column = 0; column < g.pictures[slot].columns; column++)
		{
			int tile = g.pictures[slot].tile, x0 = column * tile, y0 = row * tile;
			int w = width - x0 < tile ? width - x0 : tile, h = height - y0 < tile ? height - y0 : tile;

			g.gl.BindTexture(GL_TEXTURE_2D, g.pictures[slot].names[row * g.pictures[slot].columns + column]);
			g.begun = TRUE;
			quad((float)(x + x0), (float)(y + y0), (float)(x + x0 + w), (float)(y + y0 + h), 0.0f, 0.0f,
				(float)w / (float)power_of_two(w), (float)h / (float)power_of_two(h), TRUE, 255, 255, 255, 255);
			batch_end();
		}
	}
	g.texture = (const struct r_texture *)1;
}

static const struct r_backend gl_backend =
{
	gl_triangle,
	gl_screen_blend,
	gl_clear,
	gl_fill_row,
	gl_screen_rect,
	gl_set_near,
	gl_set_in_front,
	gl_set_alpha_scale,
	gl_view_fog,
	gl_clear_depth,
	gl_coverage,
	gl_coverage_forget,
	gl_picture,
	gl_fill_rectangle,
	gl_texture_delete,
	gl_read_back,
	gl_texture_created,
	gl_draws_detail,
	gl_view_rect,
};
