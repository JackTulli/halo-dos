/*
DOS_HUD_GAME.C

The HUD and text as the game draws them, for the DOS and Windows 95
renderer: the rasterizer calls interface/hud*.c, motion_sensor.c, the
console and the game engines make (the N64 build's port/n64/game/
n64_hud_game.c is this file's model). The game lays them out on a 640x480
screen; the picture's pixels are that scaled (dos_screen_scale_x, _y).

- quads (rasterizer_psuedo_dynamic_screen_quad_draw): the bitmap as a
  texture with its alpha, Halo's premultiplied blend (colour + picture x
  (1 - alpha)) or alpha over, as the shader asks; meters and multitexture
  overlays (two or three maps, tinted, faded, scrolled) worked out on the
  processor into small textures of their own, as Halo's pixel shaders
  combine them;
- text: Halo's own layout (text/draw_string.c) with a glyph drawer here,
  each glyph's coverage scaled into the picture, a shadow under it;
- the motion sensor's blips: squares of their colour.

Meters (shields, health, ammunition) draw their shape: the fill level is
not shown yet.
*/

#include "cseries.h"

#include "bitmaps/bitmap_group.h"
#include "rasterizer/rasterizer.h"
#include "render/render.h"
#include "text/draw_string.h"

#include "halo_system.h"
#include "dos_render.h"

#include <stdio.h>
#include <stdlib.h>

#include <math.h>
#include <string.h>

/* motion_sensor.c's (the sensor's centre and its scale) */
extern real_point2d center_point;
extern real scale[];

/* text/font_group.c's */
struct font_character
{
	word character;
	short character_width;
	short bitmap_width;
	short bitmap_height;
	short bitmap_origin_x;
	short bitmap_origin_y;
	short hardware_character_index;
	word pad;
	long pixels_offset;
};

struct font_header
{
	unsigned long flags;
	short ascending_height;
	short descending_height;
	short leading_height;
	short leading_width;
	long pad[9];
	struct tag_block character_tables;
	struct tag_reference style_fonts[4];
	struct tag_block characters;
	struct tag_data pixels;
};

#define BITMAP_FLAG_LINEAR 0x0010
/* shader framebuffer blend functions (Halo's) */
#define BLEND_ALPHA 0
#define BLEND_MULTIPLY 1
#define BLEND_DOUBLE_MULTIPLY 2
#define BLEND_ADD 3
#define BLEND_ALPHA_MULTIPLY_ADD 7

static pixel32 text_shadow_color = 0xFF000000;

void dos_hud_frame_begin(void) { return; }
void rasterizer_hud_begin(void) { return; }
void rasterizer_hud_end(void) { return; }

static real screen_x(real x) { return DOS_SCREEN_X(x); }
static real screen_y(real y) { return y * dos_screen_scale_y; }

/* ---------- meters

Halo's meters (shields, health, ammunition, the flashlight...) are a
bitmap whose alpha is the meter's shape and whose grey is the place along
it, drawn by a combiner that fills each pixel whose place is below the
meter's value (rasterizer_xbox_dynavobgeom.c, decoded): a filled pixel is
the gradient's colour at its place (min to max), plus the flash colour
where the place is past the lower value; the rest is the empty colour. The
framebuffer then takes the colour times the tint plus what is under times
the filled pixels' tint alpha (the empty ones' background alpha). Here each
state of a meter is a small premultiplied texture of its rectangle of the
bitmap (cached while the state lasts), drawn as the HUD's premultiplied
quads are. */

/* rasterizer_meter_parameters (hud_draw.c) */
struct meter_parameters
{
	pixel32 gradient_min_color;
	pixel32 gradient_max_color;
	pixel32 background_color;
	pixel32 flash_color;
	boolean flash_color_is_negative;
	boolean tint_mode_2;
	byte pad12[2];
	pixel32 tint_color;
	real gradient;
};

#define METER_SLOTS 12

static struct meter_slot
{
	struct bitmap_data *bitmap;
	int x0, y0, x1, y1;
	struct meter_parameters parameters;
	r_texture *texture;
	unsigned long used;
} meter_slots[METER_SLOTS];
static unsigned long meter_clock;

/* each meter's or overlay's bitmap's texels, decoded once */
static struct
{
	struct bitmap_data *bitmap;
	unsigned long *texels;
	int width, height;
} hud_sources[12];
static int hud_source_next;

static void combined_dispose(void);

void dos_hud_dispose_from_old_map(void)
{
	/* (the fonts' glyphs go with the map) */
	r_coverage_cache_flush();
	for (int i = 0; i < METER_SLOTS; i++)
	{
		if (meter_slots[i].texture)
			r_texture_delete(meter_slots[i].texture);
	}
	memset(meter_slots, 0, sizeof(meter_slots));
	combined_dispose();
	for (int i = 0; i < (int)NUMBEROF(hud_sources); i++)
		(free)(hud_sources[i].texels);
	memset(hud_sources, 0, sizeof(hud_sources));
	hud_source_next = 0;
}

static const unsigned long *hud_source(struct bitmap_data *bitmap, int *width, int *height)
{
	int i, free_slot = -1;

	for (i = 0; i < (int)NUMBEROF(hud_sources); i++)
	{
		if (hud_sources[i].bitmap == bitmap)
		{
			*width = hud_sources[i].width;
			*height = hud_sources[i].height;
			return hud_sources[i].texels;
		}
		if (!hud_sources[i].bitmap && free_slot < 0)
			free_slot = i;
	}
	/* (all taken: each in turn makes room) */
	if (free_slot < 0)
	{
		free_slot = hud_source_next;
		hud_source_next = (hud_source_next + 1) % (int)NUMBEROF(hud_sources);
		(free)(hud_sources[free_slot].texels);
	}
	hud_sources[free_slot].bitmap = bitmap;
	hud_sources[free_slot].texels = dos_bitmap_decode(bitmap, 1024, &hud_sources[free_slot].width,
		&hud_sources[free_slot].height);
	*width = hud_sources[free_slot].width;
	*height = hud_sources[free_slot].height;
	return hud_sources[free_slot].texels;
}

static unsigned long channel(pixel32 color, int shift)
{
	return (color >> shift) & 0xFF;
}

static float clamp_signed(float x)
{
	return x < -1.0f ? -1.0f : x > 1.0f ? 1.0f : x;
}

static float clamp_unsigned(float x)
{
	return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
}

static float positive(float x)
{
	return x < 0.0f ? 0.0f : x;
}

/* Halo's meter pixel shader (rasterizer_xbox_dynavobgeom.c: four combiner
stages and the final combiner) worked through for one texel: its result
depends only on the texel's blue (where along the meter it lies) and alpha
(its shape). For each blue, the colour before the shape and the alpha the
blend keeps what is under by (DESTBLEND SRCALPHA):
- stage 0: r0.a = 4 * 2 * (1 / 8) * blue, r0.b = 4 * (min.a - blue);
- stage 1: colour = min.rgb to max.rgb by r0.a, r0.a = 1 - 2 * max(r0.b, 0);
- stage 2: colour += flash.rgb (or -flash.rgb) * max(r0.a, 0), r0.a = blue +
  0.5 - flash.a;
- stage 3: past the meter's value (r0.a >= 0.5) the background's colour and
  alpha, else the colour and the tint's alpha;
- final: colour * the texel's alpha, alpha. Every value clamped to [-1, 1]
  between stages and inputs read before outputs, as the hardware does. */
static void meter_program(const struct meter_parameters *m, unsigned char color[256][3], unsigned char keep[256])
{
	float gradient = m->gradient * 8.0f, scale, minimum_alpha = (float)(m->gradient_min_color >> 24) / 255.0f;
	float flash_alpha = (float)(m->flash_color >> 24) / 255.0f;

	/* (the constant as the game packs it: a byte of alpha) */
	scale = (float)(long)(255.0f / (gradient > 1.0f ? gradient : 1.0f) + 0.5f) / 255.0f;
	for (int b = 0; b < 256; b++)
	{
		float blue = (float)b / 255.0f;
		float r0a = clamp_signed(4.0f * (scale * blue + scale * blue));
		float r0b = clamp_signed(4.0f * (minimum_alpha - blue));
		float lerp = clamp_unsigned(r0a), flash, rgb[3];
		int background;

		/* stage 1, then 2 (each reading what the stage before wrote) */
		r0a = clamp_signed(1.0f - 2.0f * positive(r0b));
		flash = positive(r0a);
		for (int c = 0; c < 3; c++)
		{
			int shift = 16 - 8 * c;
			float low = (float)channel(m->gradient_min_color, shift) / 255.0f;
			float high = (float)channel(m->gradient_max_color, shift) / 255.0f;
			float add = (float)channel(m->flash_color, shift) / 255.0f;

			rgb[c] = clamp_signed((1.0f - lerp) * low + lerp * high);
			rgb[c] = clamp_signed(positive(rgb[c]) + flash * (m->flash_color_is_negative ? -add : add));
		}
		r0a = clamp_signed(blue + 0.5f - flash_alpha);
		/* stage 3 (the mux on r0.a's top bit) and the final combiner */
		background = r0a >= 0.5f;
		for (int c = 0; c < 3; c++)
		{
			float value = background ? (float)channel(m->background_color, 16 - 8 * c) / 255.0f :
				positive(rgb[c]);

			color[b][c] = (unsigned char)(clamp_unsigned(value) * 255.0f + 0.5f);
		}
		keep[b] = (unsigned char)(background ? m->background_color >> 24 : m->tint_color >> 24);
	}
}

static r_texture *meter_texture(struct bitmap_data *bitmap, const struct meter_parameters *m, int x0, int y0,
	int x1, int y1)
{
	struct meter_slot *slot = NULL;
	const unsigned long *source;
	unsigned long *texels;
	int width, height, w = x1 - x0, h = y1 - y0;
	unsigned char color[256][3], keep[256];
	unsigned long tint[3];

	meter_clock++;
	for (int i = 0; i < METER_SLOTS; i++)
	{
		struct meter_slot *s = &meter_slots[i];

		if (s->bitmap == bitmap && s->x0 == x0 && s->y0 == y0 && s->x1 == x1 && s->y1 == y1 &&
			!memcmp(&s->parameters, m, sizeof(*m)))
		{
			s->used = meter_clock;
			return s->texture;
		}
		if (!slot || s->used < slot->used)
			slot = s;
	}
	source = hud_source(bitmap, &width, &height);
	if (!source || w <= 0 || h <= 0 || x0 < 0 || y0 < 0 || x1 > width || y1 > height)
		return NULL;
	texels = (malloc)((size_t)w * h * sizeof(unsigned long));
	if (!texels)
		return NULL;
	meter_program(m, color, keep);
	for (int c = 0; c < 3; c++)
		tint[c] = channel(m->tint_color, 16 - 8 * c);
	for (int y = 0; y < h; y++)
	{
		for (int x = 0; x < w; x++)
		{
			unsigned long t = source[(y0 + y) * width + x0 + x];
			unsigned long shape = t >> 24, place = t & 0xFF, rgb[3];

			/* (alpha 0 is killed: nothing drawn) */
			if (!shape)
			{
				texels[y * w + x] = 0;
				continue;
			}
			/* premultiplied: the colour times the shape and the blend's
			constant colour (SRCBLEND CONSTANTCOLOR = the tint); what is
			under kept by keep, so alpha 1 - keep */
			for (int c = 0; c < 3; c++)
				rgb[c] = (unsigned long)color[place][c] * shape / 255 * tint[c] / 255;
			texels[y * w + x] = ((255 - (unsigned long)keep[place]) << 24) | (rgb[0] << 16) | (rgb[1] << 8) | rgb[2];
		}
	}
	if (slot->texture)
		r_texture_delete(slot->texture);
	slot->bitmap = bitmap;
	slot->x0 = x0;
	slot->y0 = y0;
	slot->x1 = x1;
	slot->y1 = y1;
	slot->parameters = *m;
	slot->used = meter_clock;
	slot->texture = r_texture_new(texels, w, h, R_TEXTURE_ALPHA | R_TEXTURE_CLAMP);
	(free)(texels);
	return slot->texture;
}

static int draw_meter(struct rasterizer_dynamic_screen_geometry_parameters *parameters,
	struct dynamic_screen_vertex *vertices, struct bitmap_data *bitmap, real texel_u, real texel_v,
	real origin_x, real origin_y)
{
	const struct meter_parameters *m = (const struct meter_parameters *)parameters->meter_parameters;
	struct r_screen_vertex corners[4];
	real u0 = 1e9f, u1 = -1e9f, v0 = 1e9f, v1 = -1e9f;
	int x0, y0, x1, y1;
	r_texture *texture;

	for (int i = 0; i < 4; i++)
	{
		real u = vertices[i].texture_coordinates.x * texel_u * bitmap->width;
		real v = vertices[i].texture_coordinates.y * texel_v * bitmap->height;

		if (u < u0) u0 = u;
		if (u > u1) u1 = u;
		if (v < v0) v0 = v;
		if (v > v1) v1 = v;
	}
	x0 = (int)floorf(u0);
	y0 = (int)floorf(v0);
	x1 = (int)ceilf(u1);
	y1 = (int)ceilf(v1);
	texture = meter_texture(bitmap, m, x0, y0, x1, y1);
	{
		/* (testing, setting debug_hud=<n>: each meter frame n draws) */
		extern long dos_setting_integer(const char *name, long default_value);
		extern long dos_frame_number(void);
		long wanted = dos_setting_integer("debug_hud", 0);

		if (wanted && dos_frame_number() == wanted)
		{
			char line[200];

			snprintf(line, sizeof(line), "hud meter: texels %d,%d to %d,%d of %dx%d, min %08lX max %08lX flash %08lX%s "
				"tint %08lX background %08lX gradient %.2f: %s", x0, y0, x1, y1, bitmap->width, bitmap->height,
				(unsigned long)m->gradient_min_color, (unsigned long)m->gradient_max_color,
				(unsigned long)m->flash_color, m->flash_color_is_negative ? " (negative)" : "",
				(unsigned long)m->tint_color, (unsigned long)m->background_color, (double)m->gradient,
				texture ? "drawn" : "NONE");
			system_log(line);
			{
				extern void dos_debug_player_vitality(char *line, int size);

				dos_debug_player_vitality(line, sizeof(line));
				system_log(line);
			}
		}
	}
	if (!texture)
		return FALSE;
	for (int i = 0; i < 4; i++)
	{
		corners[i].x = screen_x(origin_x + vertices[i].position.x);
		corners[i].y = screen_y(origin_y + vertices[i].position.y);
		corners[i].u = (vertices[i].texture_coordinates.x * texel_u * bitmap->width - (real)x0) / (real)(x1 - x0);
		corners[i].v = (vertices[i].texture_coordinates.y * texel_v * bitmap->height - (real)y0) / (real)(y1 - y0);
		corners[i].color = 0xFFFFFFFF;
	}
	r_draw_screen_quad(corners, texture, _r_screen_premultiplied);
	return TRUE;
}

/* ---------- multitexture overlays

Halo's multitexture overlays (hud_draw.c's hud_draw_multitexture_overlay:
the sniper rifle's elevation ticks and the like) are a quad of up to three
maps that rasterizer_xbox_dynavobgeom.c's pixel shader combines. Each map
is placed by the screen vertex program (vs03): its coordinate is the
quad's texture coordinate (or, anchored to the screen, the position) times
the map's scale plus its offset, times its texture scale. Its constant
tints and fades it. Then:
  r0 = map 0 * its tint and fade * the vertex colour,
  r0 = r0 <first function> map 1, then r0 <second function> map 2,
the functions as the combiner stages have them for the shader framebuffer
function asked (the HUD's add, multiply... renamed on the way): alpha
blend r0 + map, multiply r0 * map, double multiply r0 - map, add
2 * r0 * map, reverse subtract the dot product of the colours (the alphas
multiplied), min the smaller; every stage clamped to -1..1, the result to
0..1. Here that is worked out for each texel of a texture the quad's size
on Halo's 640x480 screen (kept while its inputs stay the same) and drawn
with the framebuffer function's blend: alpha blend, add (one plus one) and
alpha multiply add exactly, multiply and double multiply as a darkening by
the colour's grey, the rest alpha over. */

#define COMBINED_SLOTS 8
#define COMBINED_LARGEST 256

/* what a combined quad's texture depends on */
struct combined_key
{
	struct bitmap_data *map[3];
	boolean wrapped[3], anchored[3];
	real scale[3][2], texture_scale[3][2], offset[3][2], tint[3][3], fade[3];
	short functions[2], framebuffer_blend_function;
	short pad;
	struct dynamic_screen_vertex vertices[4];
};

static struct combined_slot
{
	struct combined_key key;
	r_texture *texture;
	enum r_screen_blend blend;
	/* (every texel leaves the picture as it is: nothing drawn) */
	int empty;
	unsigned long used;
} combined_slots[COMBINED_SLOTS];
static unsigned long combined_clock;

static void combined_dispose(void)
{
	for (int i = 0; i < COMBINED_SLOTS; i++)
	{
		if (combined_slots[i].texture)
			r_texture_delete(combined_slots[i].texture);
	}
	memset(combined_slots, 0, sizeof(combined_slots));
}

/* a quad the single-map drawing below cannot show: more maps, or its map
tinted, faded, scrolled, scaled or anchored to the screen */
static int needs_combiner(const struct rasterizer_dynamic_screen_geometry_parameters *parameters)
{
	const real_point2d *offset = parameters->map_offset[0];
	real scale_x = parameters->map_scale[0].i, scale_y = parameters->map_scale[0].j;

	return parameters->map[1] || parameters->map_tint[0] || parameters->map_fade[0] ||
		parameters->map_anchor_screen[0] || (offset && (offset->x != 0.0f || offset->y != 0.0f)) ||
		(scale_x != 0.0f && scale_x != 1.0f) || (scale_y != 0.0f && scale_y != 1.0f);
}

static real combiner_clamp(real x)
{
	return x < -1.0f ? -1.0f : x > 1.0f ? 1.0f : x;
}

/* the texel at u, v (0 to 1 across the map, wrapped or clamped) */
static unsigned long combined_texel(const unsigned long *texels, int width, int height, real u, real v, boolean wrapped)
{
	long x, y;

	if (!(u == u) || !(v == v) || fabsf(u) > 1.0e6f || fabsf(v) > 1.0e6f)
		return 0;
	if (wrapped)
	{
		u -= floorf(u);
		v -= floorf(v);
	}
	x = (long)floorf(u * (real)width);
	y = (long)floorf(v * (real)height);
	x = PIN(x, 0, width - 1);
	y = PIN(y, 0, height - 1);
	return texels[y * width + x];
}

/* r0 <function> m, colour and alpha (the combiner stage the shader
framebuffer function makes, rasterizer_xbox_dynavobgeom.c) */
static void combiner_stage(real r0[4], const real m[4], short function)
{
	switch (function)
	{
	case 0: /* alpha blend: r0 + m */
		for (int c = 0; c < 4; c++)
			r0[c] = combiner_clamp(r0[c] + m[c]);
		break;
	case 2: /* double multiply: r0 - m */
		for (int c = 0; c < 4; c++)
			r0[c] = combiner_clamp(r0[c] - m[c]);
		break;
	case 3: /* add: 2 r0 m */
		for (int c = 0; c < 4; c++)
			r0[c] = combiner_clamp(2.0f * r0[c] * m[c]);
		break;
	case 4: /* reverse subtract: the colours' dot product, the alphas' product */
	{
		real dot = combiner_clamp(r0[0] * m[0] + r0[1] * m[1] + r0[2] * m[2]);

		r0[0] = r0[1] = r0[2] = dot;
		r0[3] = combiner_clamp(r0[3] * m[3]);
		break;
	}
	case 5: /* min */
		for (int c = 0; c < 4; c++)
			r0[c] = m[c] < r0[c] ? m[c] : r0[c];
		break;
	default: /* multiply: r0 m */
		for (int c = 0; c < 4; c++)
			r0[c] = combiner_clamp(r0[c] * m[c]);
		break;
	}
}

static struct combined_slot *combined_quad(struct rasterizer_dynamic_screen_geometry_parameters *parameters,
	struct dynamic_screen_vertex *vertices)
{
	struct combined_key key;
	struct combined_slot *slot = NULL;
	const unsigned long *sources[3] = { NULL, NULL, NULL };
	int source_width[3] = { 0, 0, 0 }, source_height[3] = { 0, 0, 0 }, map_count = 0, width, height, empty = TRUE;
	real normal_u[3], normal_v[3];
	unsigned long *texels;
	enum r_screen_blend blend;

	/* the inputs: the maps (map 2 only after map 1, as the shader has
	them), their placing and constants, the corners */
	memset(&key, 0, sizeof(key));
	for (int m = 0; m < 3 && parameters->map[m]; m++)
	{
		const real_rgb_color *tint = parameters->map_tint[m];

		map_count = m + 1;
		key.map[m] = parameters->map[m];
		key.wrapped[m] = parameters->map_wrapped[m];
		key.anchored[m] = parameters->map_anchor_screen[m];
		key.scale[m][0] = parameters->map_scale[m].i;
		key.scale[m][1] = parameters->map_scale[m].j;
		key.texture_scale[m][0] = parameters->map_texture_scale[m].i != 0.0f ? parameters->map_texture_scale[m].i : 1.0f;
		key.texture_scale[m][1] = parameters->map_texture_scale[m].j != 0.0f ? parameters->map_texture_scale[m].j : 1.0f;
		key.offset[m][0] = parameters->map_offset[m] ? parameters->map_offset[m]->x : 0.0f;
		key.offset[m][1] = parameters->map_offset[m] ? parameters->map_offset[m]->y : 0.0f;
		key.tint[m][0] = tint ? tint->red : 1.0f;
		key.tint[m][1] = tint ? tint->green : 1.0f;
		key.tint[m][2] = tint ? tint->blue : 1.0f;
		key.fade[m] = parameters->map_fade[m] ? *parameters->map_fade[m] : 1.0f;
	}
	key.functions[0] = parameters->map0_to_1_blend_function;
	key.functions[1] = parameters->map1_to_2_blend_function;
	key.framebuffer_blend_function = parameters->framebuffer_blend_function;
	memcpy(key.vertices, vertices, sizeof(key.vertices));
	combined_clock++;
	for (int i = 0; i < COMBINED_SLOTS; i++)
	{
		struct combined_slot *s = &combined_slots[i];

		if ((s->texture || s->empty) && !memcmp(&s->key, &key, sizeof(key)))
		{
			s->used = combined_clock;
			return s;
		}
		if (!slot || s->used < slot->used)
			slot = s;
	}

	for (int m = 0; m < map_count; m++)
	{
		sources[m] = hud_source(key.map[m], &source_width[m], &source_height[m]);
		if (!sources[m] || source_width[m] <= 0 || source_height[m] <= 0)
			return NULL;
		/* (linear bitmaps are addressed in texels, swizzled ones from 0 to
		1, as the single maps below) */
		normal_u[m] = key.map[m]->flags & BITMAP_FLAG_LINEAR ? 1.0f / (real)key.map[m]->width : 1.0f;
		normal_v[m] = key.map[m]->flags & BITMAP_FLAG_LINEAR ? 1.0f / (real)key.map[m]->height : 1.0f;
	}
	/* (the quad's sides on Halo's screen: corners 0 to 1 and 0 to 3) */
	{
		real dx = vertices[1].position.x - vertices[0].position.x, dy = vertices[1].position.y - vertices[0].position.y;

		width = (int)(sqrtf(dx * dx + dy * dy) + 0.5f);
		dx = vertices[3].position.x - vertices[0].position.x;
		dy = vertices[3].position.y - vertices[0].position.y;
		height = (int)(sqrtf(dx * dx + dy * dy) + 0.5f);
	}
	width = PIN(width, 1, COMBINED_LARGEST);
	height = PIN(height, 1, COMBINED_LARGEST);
	texels = (malloc)((size_t)width * height * sizeof(unsigned long));
	if (!texels)
		return NULL;
	switch (key.framebuffer_blend_function)
	{
	case BLEND_ALPHA_MULTIPLY_ADD:
	case BLEND_ADD:
	case BLEND_MULTIPLY:
	case BLEND_DOUBLE_MULTIPLY:
		blend = _r_screen_premultiplied;
		break;
	default:
		blend = _r_screen_alpha;
		break;
	}
	for (int y = 0; y < height; y++)
	{
		real t = ((real)y + 0.5f) / (real)height;

		for (int x = 0; x < width; x++)
		{
			real s = ((real)x + 0.5f) / (real)width, r0[4], colour[4];
			real position[2], coordinate[2];
			unsigned long out[4], texel;

			/* (the corners' values across the quad: 0 to 1 along s, 0 to 3
			along t) */
			position[0] = vertices[0].position.x + s * (vertices[1].position.x - vertices[0].position.x) +
				t * (vertices[3].position.x - vertices[0].position.x);
			position[1] = vertices[0].position.y + s * (vertices[1].position.y - vertices[0].position.y) +
				t * (vertices[3].position.y - vertices[0].position.y);
			coordinate[0] = vertices[0].texture_coordinates.x +
				s * (vertices[1].texture_coordinates.x - vertices[0].texture_coordinates.x) +
				t * (vertices[3].texture_coordinates.x - vertices[0].texture_coordinates.x);
			coordinate[1] = vertices[0].texture_coordinates.y +
				s * (vertices[1].texture_coordinates.y - vertices[0].texture_coordinates.y) +
				t * (vertices[3].texture_coordinates.y - vertices[0].texture_coordinates.y);
			for (int c = 0; c < 4; c++)
			{
				int shift = c == 3 ? 24 : 16 - 8 * c;
				real c0 = (real)((vertices[0].color >> shift) & 0xFF), c1 = (real)((vertices[1].color >> shift) & 0xFF);
				real c3 = (real)((vertices[3].color >> shift) & 0xFF);

				colour[c] = (c0 + s * (c1 - c0) + t * (c3 - c0)) / 255.0f;
			}
			for (int m = 0; m < map_count; m++)
			{
				const real *base = key.anchored[m] ? position : coordinate;
				real u = (base[0] * key.scale[m][0] + key.offset[m][0]) * key.texture_scale[m][0] * normal_u[m];
				real v = (base[1] * key.scale[m][1] + key.offset[m][1]) * key.texture_scale[m][1] * normal_v[m];
				unsigned long sample = combined_texel(sources[m], source_width[m], source_height[m], u, v,
					key.wrapped[m]);
				real value[4];

				value[0] = (real)((sample >> 16) & 0xFF) / 255.0f * key.tint[m][0];
				value[1] = (real)((sample >> 8) & 0xFF) / 255.0f * key.tint[m][1];
				value[2] = (real)(sample & 0xFF) / 255.0f * key.tint[m][2];
				value[3] = (real)(sample >> 24) / 255.0f * key.fade[m];
				if (m == 0)
				{
					for (int c = 0; c < 4; c++)
						r0[c] = combiner_clamp(value[c] * colour[c]);
				}
				else
				{
					combiner_stage(r0, value, key.functions[m - 1]);
				}
			}
			for (int c = 0; c < 4; c++)
				out[c] = (unsigned long)((r0[c] < 0.0f ? 0.0f : r0[c] > 1.0f ? 1.0f : r0[c]) * 255.0f + 0.5f);
			switch (key.framebuffer_blend_function)
			{
			case BLEND_ADD:
				/* (one plus one: what is under kept whole) */
				texel = (out[0] << 16) | (out[1] << 8) | out[2];
				break;
			case BLEND_MULTIPLY:
			case BLEND_DOUBLE_MULTIPLY:
			{
				/* (what is under times the colour: darkened by its grey) */
				unsigned long grey = (out[0] * 77 + out[1] * 150 + out[2] * 29) >> 8;

				if (key.framebuffer_blend_function == BLEND_DOUBLE_MULTIPLY)
					grey = grey * 2 > 255 ? 255 : grey * 2;
				texel = (255 - grey) << 24;
				break;
			}
			default:
				texel = (out[3] << 24) | (out[0] << 16) | (out[1] << 8) | out[2];
				break;
			}
			if (blend == _r_screen_alpha ? (texel >> 24) != 0 : texel != 0)
				empty = FALSE;
			texels[y * width + x] = texel;
		}
	}
	if (slot->texture)
		r_texture_delete(slot->texture);
	slot->key = key;
	slot->blend = blend;
	slot->empty = empty;
	slot->used = combined_clock;
	slot->texture = empty ? NULL : r_texture_new(texels, width, height, R_TEXTURE_ALPHA | R_TEXTURE_CLAMP |
		R_TEXTURE_LARGE);
	(free)(texels);
	return slot;
}

/* ---------- quads */


void rasterizer_psuedo_dynamic_screen_quad_draw(
	struct rasterizer_dynamic_screen_geometry_parameters *parameters,
	struct dynamic_screen_vertex *vertices)
{
	struct bitmap_data *bitmap = parameters->map[0];
	r_texture *texture = NULL;
	struct r_screen_vertex corners[4];
	real origin_x, origin_y, texel_u = 1.f, texel_v = 1.f;
	enum r_screen_blend blend;

	if (bitmap)
	{
		extern void *_texture_cache_bitmap_get_hardware_format(struct bitmap_data *bitmap, boolean block,
			boolean load);

		texture = _texture_cache_bitmap_get_hardware_format(bitmap, FALSE, TRUE);
		if (!texture)
			return;
		/* linear (interface) bitmaps are addressed in texels, swizzled ones
		from 0 to 1; either way scaled by map_texture_scale first */
		if (bitmap->flags & BITMAP_FLAG_LINEAR)
		{
			texel_u = 1.f / (real)bitmap->width;
			texel_v = 1.f / (real)bitmap->height;
		}
		if (parameters->map_texture_scale[0].i)
			texel_u *= parameters->map_texture_scale[0].i;
		if (parameters->map_texture_scale[0].j)
			texel_v *= parameters->map_texture_scale[0].j;
	}
	origin_x = render.camera.viewport_bounds.x0 + (parameters->offset ? parameters->offset->i : 0.f);
	origin_y = render.camera.viewport_bounds.y0 + (parameters->offset ? parameters->offset->j : 0.f);
	for (int i = 0; i < 4; i++)
	{
		corners[i].x = screen_x(origin_x + vertices[i].position.x);
		corners[i].y = screen_y(origin_y + vertices[i].position.y);
		corners[i].u = vertices[i].texture_coordinates.x * texel_u;
		corners[i].v = vertices[i].texture_coordinates.y * texel_v;
		corners[i].color = vertices[i].color;
	}
	if (parameters->meter_parameters && bitmap && draw_meter(parameters, vertices, bitmap, texel_u, texel_v,
		origin_x, origin_y))
	{
		return;
	}
	switch (parameters->framebuffer_blend_function)
	{
	case BLEND_ALPHA_MULTIPLY_ADD: blend = _r_screen_premultiplied; break;
	case BLEND_ADD: blend = _r_screen_add; break;
	default: blend = _r_screen_alpha; break;
	}
	{
		/* (testing, setting debug_hud=<n>: each quad frame n draws) */
		extern long dos_setting_integer(const char *name, long default_value);
		extern long dos_frame_number(void);
		long wanted = dos_setting_integer("debug_hud", 0);

		if (wanted && dos_frame_number() == wanted)
		{
			char line[160];

			snprintf(line, sizeof(line), "hud quad: %.0f,%.0f to %.0f,%.0f, uv %.2f,%.2f to %.2f,%.2f, blend %d, "
				"bitmap %dx%d", (double)corners[0].x, (double)corners[0].y, (double)corners[2].x, (double)corners[2].y,
				(double)corners[0].u, (double)corners[0].v, (double)corners[2].u, (double)corners[2].v, (int)blend,
				bitmap ? bitmap->width : 0, bitmap ? bitmap->height : 0);
			system_log(line);
			snprintf(line, sizeof(line), "  function %d, colour %08lX, maps %dx%d %dx%d (%d, %d)",
				parameters->framebuffer_blend_function, (unsigned long)vertices[0].color,
				parameters->map[1] ? parameters->map[1]->width : 0, parameters->map[1] ? parameters->map[1]->height : 0,
				parameters->map[2] ? parameters->map[2]->width : 0, parameters->map[2] ? parameters->map[2]->height : 0,
				parameters->map0_to_1_blend_function, parameters->map1_to_2_blend_function);
			system_log(line);
			for (int m = 0; m < 3; m++)
			{
				if (!parameters->map[m])
					continue;
				snprintf(line, sizeof(line), "  map %d: format %d flags %04X, scale %.3f,%.3f, texture scale %.4f,%.4f, "
					"offset %.3f,%.3f, %s, tint %s%.2f %.2f %.2f, fade %.2f", m, parameters->map[m]->format,
					parameters->map[m]->flags, (double)parameters->map_scale[m].i, (double)parameters->map_scale[m].j,
					(double)parameters->map_texture_scale[m].i, (double)parameters->map_texture_scale[m].j,
					parameters->map_offset[m] ? (double)parameters->map_offset[m]->x : 0.0,
					parameters->map_offset[m] ? (double)parameters->map_offset[m]->y : 0.0,
					parameters->map_wrapped[m] ? "wrapped" : "clamped", parameters->map_tint[m] ? "" : "(none) ",
					parameters->map_tint[m] ? (double)parameters->map_tint[m]->red : 1.0,
					parameters->map_tint[m] ? (double)parameters->map_tint[m]->green : 1.0,
					parameters->map_tint[m] ? (double)parameters->map_tint[m]->blue : 1.0,
					parameters->map_fade[m] ? (double)*parameters->map_fade[m] : 1.0);
				system_log(line);
			}
		}
	}
	if (bitmap && needs_combiner(parameters))
	{
		struct combined_slot *slot = combined_quad(parameters, vertices);

		if (slot)
		{
			if (slot->texture)
			{
				for (int i = 0; i < 4; i++)
				{
					corners[i].u = i == 1 || i == 2 ? 1.0f : 0.0f;
					corners[i].v = i >= 2 ? 1.0f : 0.0f;
					corners[i].color = 0xFFFFFFFF;
				}
				r_draw_screen_quad(corners, slot->texture, slot->blend);
			}
			return;
		}
	}
	r_draw_screen_quad(corners, texture, blend);
}

/* ---------- text */

static struct
{
	real origin_x, origin_y;
	int clip_x0, clip_y0, clip_x1, clip_y1;
	/* (rasterizer_text_set_scale: the text scale times larger about a
	point, in the view's units) */
	real scale, scale_x, scale_y;
} text = { 0.0f, 0.0f, 0, 0, 0, 0, 1.0f, 0.0f, 0.0f };

/* the game's (the scoreboard at three quarters, the players' names) and the
menu's (dos_menu.c, larger at low resolutions): text drawn scale times
larger about a point until set back to 1, laid out as before */
void rasterizer_text_set_scale(real scale, real origin_x, real origin_y)
{
	text.scale = scale > 0.0f ? scale : 1.0f;
	text.scale_x = origin_x;
	text.scale_y = origin_y;
}

static void draw_glyph(
	struct parse_string_state *state,
	struct font_header *font,
	struct font_character *character,
	pixel32 color,
	short x0,
	short y0,
	short x,
	short y,
	short dx,
	short dy)
{
	const unsigned char *pixels;
	real left, top, glyph_x = x0, glyph_y = y0;
	float scale_x = dos_screen_scale_x, scale_y = dos_screen_scale_y;

	(void)state;
	if (dx <= 0 || dy <= 0 || !font->pixels.address)
		return;
	pixels = (const unsigned char *)xbox_pointer(font->pixels.address) + character->pixels_offset + character->bitmap_width * y + x;
	if (text.scale != 1.0f)
	{
		glyph_x = text.scale_x + (glyph_x - text.scale_x) * text.scale;
		glyph_y = text.scale_y + (glyph_y - text.scale_y) * text.scale;
		scale_x *= text.scale;
		scale_y *= text.scale;
	}
	left = screen_x(text.origin_x + glyph_x);
	top = screen_y(text.origin_y + glyph_y);
	/* the shadow a picture's pixel down and right, then the glyph */
	if (text_shadow_color >> 24)
	{
		r_draw_coverage(left + 1.f, top + 1.f, pixels, dx, dy, character->bitmap_width, scale_x, scale_y,
			(text_shadow_color & 0x00FFFFFF) | (color & 0xFF000000), text.clip_x0, text.clip_y0, text.clip_x1,
			text.clip_y1);
	}
	r_draw_coverage(left, top, pixels, dx, dy, character->bitmap_width, scale_x, scale_y, color, text.clip_x0,
		text.clip_y0, text.clip_x1, text.clip_y1);
}

/* the window and clip rectangles as the Xbox's rasterizer_draw_string
works them out: relative to the view's corner */
static void text_begin(
	rectangle2d const *bounds,
	rectangle2d const *clip,
	rectangle2d *window_bounds,
	rectangle2d *clip_bounds)
{
	if (!bounds)
	{
		*window_bounds = render.camera.window_bounds;
		window_bounds->x0 -= render.camera.viewport_bounds.x0;
		window_bounds->x1 -= render.camera.viewport_bounds.x0;
		window_bounds->y0 -= render.camera.viewport_bounds.y0;
		window_bounds->y1 -= render.camera.viewport_bounds.y0;
	}
	else
	{
		*window_bounds = *bounds;
	}
	if (!clip)
	{
		clip_bounds->x0 = 0;
		clip_bounds->y0 = 0;
		clip_bounds->x1 = render.camera.viewport_bounds.x1 - render.camera.viewport_bounds.x0;
		clip_bounds->y1 = render.camera.viewport_bounds.y1 - render.camera.viewport_bounds.y0;
	}
	else
	{
		*clip_bounds = *clip;
	}
	text.origin_x = render.camera.viewport_bounds.x0;
	text.origin_y = render.camera.viewport_bounds.y0;
	text.clip_x0 = (int)screen_x(text.origin_x + clip_bounds->x0);
	text.clip_y0 = (int)screen_y(text.origin_y + clip_bounds->y0);
	text.clip_x1 = (int)screen_x(text.origin_x + clip_bounds->x1) + 1;
	text.clip_y1 = (int)screen_y(text.origin_y + clip_bounds->y1) + 1;
	/* (scaled text: the layout's clip where the scaled glyphs reach the
	clip, which itself holds for the pixels) */
	if (text.scale != 1.0f)
	{
		clip_bounds->x0 = (short)(text.scale_x + (clip_bounds->x0 - text.scale_x) / text.scale);
		clip_bounds->x1 = (short)(text.scale_x + (clip_bounds->x1 - text.scale_x) / text.scale);
		clip_bounds->y0 = (short)(text.scale_y + (clip_bounds->y0 - text.scale_y) / text.scale);
		clip_bounds->y1 = (short)(text.scale_y + (clip_bounds->y1 - text.scale_y) / text.scale);
	}
}

void rasterizer_draw_string(
	rectangle2d const *bounds,
	rectangle2d const *clip,
	point2d *cursor_reference,
	short height_adjust,
	char const *string)
{
	rectangle2d window_bounds, clip_bounds;

	if (!string || !string[0])
		return;
	text_begin(bounds, clip, &window_bounds, &clip_bounds);
	draw_string(draw_glyph, &window_bounds, cursor_reference, &clip_bounds, height_adjust, string);
}

void rasterizer_draw_unicode_string(
	rectangle2d const *bounds,
	rectangle2d const *clip,
	point2d *cursor_reference,
	short height_adjust,
	wchar_t const *string)
{
	rectangle2d window_bounds, clip_bounds;

	if (!string || !string[0])
		return;
	text_begin(bounds, clip, &window_bounds, &clip_bounds);
	draw_unicode_string(draw_glyph, &window_bounds, cursor_reference, &clip_bounds, height_adjust, string);
}

void rasterizer_text_set_shadow_color(pixel32 shadow_color)
{
	text_shadow_color = shadow_color;
}

void rasterizer_text_cache_flush(void) { return; }

/* ---------- the motion sensor: blips in the sensor's -1..1 space */

#define MOTION_SENSOR_RADIUS 32.f

void rasterizer_hud_motion_sensor_blip_begin(void) { return; }

void rasterizer_hud_motion_sensor_blip_draw(
	real_point2d const *position,
	real intensity,
	real size,
	real_rgb_color const *color,
	boolean large_blip)
{
	real radius = MOTION_SENSOR_RADIUS * scale[0];
	real x = center_point.x + render.camera.viewport_bounds.x0 + position->x * -0.03125f * radius * 2.f;
	real y = center_point.y + render.camera.viewport_bounds.y0 + position->y * 0.03125f * radius * 2.f;
	real pixels = MAX(3.f, size * 0.125f * radius);
	/* (the intensity is the blip's fade as it ages: how much of it covers,
	not its darkness) */
	unsigned long argb = ((unsigned long)PIN(intensity * 255.f, 0.f, 255.f) << 24) |
		((unsigned long)PIN(color->red * 255.f, 0.f, 255.f) << 16) |
		((unsigned long)PIN(color->green * 255.f, 0.f, 255.f) << 8) |
		(unsigned long)PIN(color->blue * 255.f, 0.f, 255.f);

	if (intensity <= 0.02f)
		return;
	int half_x = (int)(pixels * dos_screen_scale_x * 0.5f) + (large_blip ? 1 : 0);
	int half_y = (int)(pixels * dos_screen_scale_y * 0.5f) + (large_blip ? 1 : 0);
	int px = (int)screen_x(x), py = (int)screen_y(y);

	if (half_x < 1) half_x = 1;
	if (half_y < 1) half_y = 1;
	r_fill_rectangle(px - half_x, py - half_y, px + half_x, py + half_y, argb);
}

void rasterizer_hud_motion_sensor_blip_end(
	real_point2d const *center,
	real sweep)
{
	(void)center;
	(void)sweep;
}
