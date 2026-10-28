/*
R_SOFT.C

The software rasterizer: screen-space triangles into the picture and its
depth buffer (r_internal.h's r_raster_triangle).

A triangle's attributes (1/z, the texture coordinates over z, the light
and the fog) are planes over the screen: their gradients are worked out
once, and each scanline's span starts at its first pixel's centre. Along a
span the texture coordinates are exact every 16 pixels (one division) and
linear between, in 16.16 fixed point, as Quake's spans were; depth, light
and fog are linear in screen space. Pixels are filled when their centre is
inside the triangle, a centre on the left or top edge counting as inside.

The spans come in kinds, picked per triangle: textured and lit with and
without fog in each picture (the cases nearly every pixel takes), alpha
tested, flat-coloured, and the screen's blended quads (affine, no depth).
*/

#include "r_internal.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define SUBDIVISION_SHIFT 4
#define SUBDIVISION (1 << SUBDIVISION_SHIFT)

/* the depth buffer's scale: w (1/z) times this, z_near giving 2^30 */
static float depth_scale = 1073741824.0f * 0.0625f;
/* added to the depth of what is drawn in front of everything (2^31: above
any depth the world has, r_set_in_front) */
static unsigned long depth_bias;
/* R_DRAW_ALPHA's texel alpha times this over 256 (r_set_alpha_scale) */
static unsigned long alpha_scale = 256;
static enum r_screen_blend screen_blend;
/* each screen quad's number (span8_screen's colours are kept for one) */
static unsigned long screen_quad;

void r_raster_set_depth_scale(float z_near)
{
	depth_scale = 1073741824.0f * z_near;
	if (r_backend)
		r_backend->set_near(z_near);
}

void r_raster_set_depth_bias(unsigned long bias)
{
	depth_bias = bias;
	if (r_backend)
		r_backend->set_in_front(bias != 0);
}

void r_raster_set_alpha_scale(unsigned long scale)
{
	alpha_scale = scale > 256 ? 256 : scale;
	if (r_backend)
		r_backend->set_alpha_scale(scale);
}

/* (a quad's: r_draw_screen_quad sets it before each one) */
void r_raster_screen_blend(enum r_screen_blend blend)
{
	screen_blend = blend;
	screen_quad++;
	if (r_backend)
		r_backend->screen_blend(blend);
}

void r_raster_clear(unsigned long color)
{
	int count = r_frame.width * r_frame.height;

	if (r_backend)
	{
		r_backend->clear(color);
		return;
	}
	if (r_frame.bits == 8)
	{
		unsigned char index = r_palette_nearest(color);

		for (int y = 0; y < r_frame.height; y++)
			memset(r_frame.pixels + (size_t)y * r_frame.pitch, index, (size_t)r_frame.width);
	}
	else
	{
		for (int y = 0; y < r_frame.height; y++)
		{
			unsigned long *row = (unsigned long *)(r_frame.pixels + (size_t)y * r_frame.pitch);

			for (int x = 0; x < r_frame.width; x++)
				row[x] = color & 0xFFFFFF;
		}
	}
	memset(r_frame.depth, 0, (size_t)count * sizeof(unsigned long));
}

void r_raster_fill_row(int x0, int x1, int y, unsigned long color)
{
	if (x0 >= x1)
		return;
	if (r_backend)
	{
		r_backend->fill_row(x0, x1, y, color);
		return;
	}
	if (r_frame.bits == 8)
	{
		memset(r_frame.pixels + (size_t)y * r_frame.pitch + x0, r_palette_nearest(color), (size_t)(x1 - x0));
	}
	else
	{
		unsigned long *row = (unsigned long *)(r_frame.pixels + (size_t)y * r_frame.pitch);

		for (int x = x0; x < x1; x++)
			row[x] = color & 0xFFFFFF;
	}
}

/* ---------- a span's starting values and steps */

struct span
{
	int y, x0, x1;
	/* at the first pixel's centre, and per pixel */
	float w, s, t;
	float dw, ds, dt;
	/* fixed point (16.16) */
	long r, g, b, dr, dg, db;
	long fog_r, fog_g, fog_b, dfog_r, dfog_g, dfog_b;
	long light, dlight, fog, dfog;
	/* the texture's level */
	const unsigned char *texels;
	int width_shift;
	unsigned long width_mask, height_mask;
	const unsigned long *palette;
	const unsigned char *remap;
	int alpha_test;
	/* s and t are the texel coordinates themselves, w unused for them: no
	perspective (a triangle whose depth hardly changes across it) */
	int affine;
};

static inline long fixed(float value)
{
	return r_round(value * 65536.0f);
}

/* a value rounded to a float's own precision, as storing it does: the x87
keeps more between its operations, and where the compiler happened to keep
a value in memory (a perspective row's texture coordinates between its
segments, the planes' row values between rows) the picture has that
rounding in it, and keeps it so */
static inline float to_float(float value)
{
	volatile float kept = value;

	return kept;
}

/* the same rounding kept modulo 2^32 however large the value (r_round's
32 bits give 0x80000000 past them): for a plane's value away from its
triangle, which adds in 32 bits bring back to the triangle's own */
static inline unsigned long wrap_round(float value)
{
	long long result;

	__asm__("fistpll %0" : "=m"(result) : "t"(value) : "st");
	return (unsigned long)result;
}

static inline unsigned long wrap_fixed(float value)
{
	return wrap_round(value * 65536.0f);
}

/* the texture coordinates in 16.16 texels at a w, s, t (a division: once
a span and every 16 pixels; none for an affine span) */
static inline void texel_coordinates(const struct span *span, float w, float s, float t, long *u, long *v)
{
	float z = span->affine ? 65536.0f : 65536.0f / w;

	*u = r_round(s * z);
	*v = r_round(t * z);
}

/* a span's depth at its first pixel, and its step */
static inline long depth_start(const struct span *span)
{
	return (long)((unsigned long)r_round(span->w * depth_scale) + depth_bias);
}

static inline long depth_step_of(const struct span *span)
{
	return r_round(span->dw * depth_scale);
}

#define TEXEL(span, u, v) \
	((span)->texels[((((unsigned long)(v) >> 16) & (span)->height_mask) << (span)->width_shift) | \
		(((unsigned long)(u) >> 16) & (span)->width_mask)])

/* ---------- 32-bit spans */

static inline unsigned long modulate(unsigned long texel, long r, long g, long b)
{
	unsigned long red = (((texel >> 16) & 0xFF) * (unsigned long)(r >> 16)) >> 7;
	unsigned long green = (((texel >> 8) & 0xFF) * (unsigned long)(g >> 16)) >> 7;
	unsigned long blue = ((texel & 0xFF) * (unsigned long)(b >> 16)) >> 7;

	if (red > 255) red = 255;
	if (green > 255) green = 255;
	if (blue > 255) blue = 255;
	return (red << 16) | (green << 8) | blue;
}

static inline unsigned long add_fog(unsigned long color, long fog_r, long fog_g, long fog_b)
{
	unsigned long red = ((color >> 16) & 0xFF) + (unsigned long)(fog_r >> 16);
	unsigned long green = ((color >> 8) & 0xFF) + (unsigned long)(fog_g >> 16);
	unsigned long blue = (color & 0xFF) + (unsigned long)(fog_b >> 16);

	if (red > 255) red = 255;
	if (green > 255) green = 255;
	if (blue > 255) blue = 255;
	return (red << 16) | (green << 8) | blue;
}

static int dither_offsets(const struct span *span, long du, long dv, long *offsets);

/* whether a segment's texels are bigger than its pixels (16.16 steps) */
static inline int magnified(long du, long dv)
{
	return du < 0x10000 && du > -0x10000 && dv < 0x10000 && dv > -0x10000;
}

/* four texels' colours mixed by fu, fv (0 to 255: the place between their
centres; red and blue a multiply together) */
static inline unsigned long mix4(unsigned long c00, unsigned long c10, unsigned long c01, unsigned long c11,
	unsigned long fu, unsigned long fv)
{
	unsigned long rb0 = (((c00 & 0xFF00FF) * (256 - fu) + (c10 & 0xFF00FF) * fu) >> 8) & 0xFF00FF;
	unsigned long g0 = (((c00 & 0xFF00) * (256 - fu) + (c10 & 0xFF00) * fu) >> 8) & 0xFF00;
	unsigned long rb1 = (((c01 & 0xFF00FF) * (256 - fu) + (c11 & 0xFF00FF) * fu) >> 8) & 0xFF00FF;
	unsigned long g1 = (((c01 & 0xFF00) * (256 - fu) + (c11 & 0xFF00) * fu) >> 8) & 0xFF00;

	return ((((rb0 * (256 - fv) + rb1 * fv) >> 8) & 0xFF00FF) | (((g0 * (256 - fv) + g1 * fv) >> 8) & 0xFF00));
}

/* the texture at u, v (16.16) filtered: the four texels around it */
static inline unsigned long bilinear(const unsigned char *texels, const unsigned long *palette, int width_shift,
	unsigned long width_mask, unsigned long height_mask, long u, long v)
{
	unsigned long x0 = ((unsigned long)(u - 0x8000) >> 16) & width_mask;
	unsigned long y0 = (((unsigned long)(v - 0x8000) >> 16) & height_mask) << width_shift;
	unsigned long x1 = (x0 + 1) & width_mask;
	unsigned long y1 = ((((unsigned long)(v - 0x8000) >> 16) + 1) & height_mask) << width_shift;

	return mix4(palette[texels[y0 | x0]], palette[texels[y0 | x1]], palette[texels[y1 | x0]],
		palette[texels[y1 | x1]], ((unsigned long)(u - 0x8000) >> 8) & 0xFF, ((unsigned long)(v - 0x8000) >> 8) & 0xFF);
}

/* the same for any texture, a cut-out's too: filtered among four solid
texels, and at a cut-out's edge the nearest texel (the edge stays sharp);
FALSE where that is a hole */
static inline int filtered_texel(const struct span *span, long u, long v, unsigned long *color)
{
	unsigned long x0 = ((unsigned long)(u - 0x8000) >> 16) & span->width_mask;
	unsigned long y0 = (((unsigned long)(v - 0x8000) >> 16) & span->height_mask) << span->width_shift;
	unsigned long x1 = (x0 + 1) & span->width_mask;
	unsigned long y1 = ((((unsigned long)(v - 0x8000) >> 16) + 1) & span->height_mask) << span->width_shift;
	unsigned char i00 = span->texels[y0 | x0], i10 = span->texels[y0 | x1];
	unsigned char i01 = span->texels[y1 | x0], i11 = span->texels[y1 | x1];

	if (span->alpha_test && !(i00 && i10 && i01 && i11))
	{
		unsigned char index = TEXEL(span, u, v);

		*color = span->palette[index];
		return index != 0;
	}
	*color = mix4(span->palette[i00], span->palette[i10], span->palette[i01], span->palette[i11],
		((unsigned long)(u - 0x8000) >> 8) & 0xFF, ((unsigned long)(v - 0x8000) >> 8) & 0xFF);
	return 1;
}

/* textured, lit, perhaps fogged and alpha tested, depth tested and written */
static void span32_textured(struct span *span, int fogged, unsigned flags)
{
	unsigned long *pixels = (unsigned long *)(r_frame.pixels + (size_t)span->y * r_frame.pitch);
	unsigned long *depth = r_frame.depth + (size_t)span->y * r_frame.width;
	int x = span->x0;
	float w = span->w, s = span->s, t = span->t;
	long depth_value = depth_start(span), depth_step = depth_step_of(span);
	long r = span->r, g = span->g, b = span->b;
	long fr = span->fog_r, fg = span->fog_g, fb = span->fog_b;
	long u, v;
	int test = !(flags & R_DRAW_NO_DEPTH_TEST);
	int write = !(flags & (R_DRAW_ADDITIVE | R_DRAW_BLEND));
	long offsets[4];

	texel_coordinates(span, w, s, t, &u, &v);
	while (x < span->x1)
	{
		int count = span->x1 - x < SUBDIVISION ? span->x1 - x : SUBDIVISION;
		long u_end, v_end, du, dv;

		w += span->dw * count;
		s += span->ds * count;
		t += span->dt * count;
		texel_coordinates(span, w, s, t, &u_end, &v_end);
		du = (u_end - u) / count;
		dv = (v_end - v) / count;
		/* (filtered where magnified, if asked; an alpha-tested texture is
		not dithered: its edges stay sharp) */
		int smooth = r_frame.settings.texture_smooth && magnified(du, dv);

		if (smooth || span->alpha_test || !dither_offsets(span, du, dv, offsets))
			memset(offsets, 0, sizeof(offsets));
		for (int i = 0; i < count; i++, x++)
		{
			if (!test || (unsigned long)depth_value > depth[x])
			{
				const long *offset = offsets + ((x & 1) << 1);
				unsigned long texel;
				int solid;

				unsigned long alpha = 0;

				if (smooth)
				{
					solid = filtered_texel(span, u, v, &texel);
				}
				else
				{
					unsigned char index = TEXEL(span, u + offset[0], v + offset[1]);

					texel = span->palette[index];
					solid = !span->alpha_test || index;
				}
				if (flags & R_DRAW_ALPHA)
				{
					/* (the nearest texel's: the filter keeps no alpha) */
					alpha = span->palette[TEXEL(span, u, v)] >> 24;
					alpha += alpha >> 7;
					alpha = (alpha * alpha_scale) >> 8;
					solid &= alpha != 0;
				}
				if (solid)
				{
					unsigned long color = modulate(texel, r, g, b);

					if (fogged)
						color = add_fog(color, fr, fg, fb);
					if (flags & R_DRAW_ADDITIVE)
						color = add_fog(pixels[x], (long)(color & 0xFF0000) << 0, (long)(color & 0xFF00) << 8,
							(long)(color & 0xFF) << 16);
					else if (flags & R_DRAW_ALPHA)
					{
						unsigned long under = pixels[x];

						color = ((((color & 0xFF00FF) * alpha + (under & 0xFF00FF) * (256 - alpha)) >> 8) & 0xFF00FF) |
							((((color & 0xFF00) * alpha + (under & 0xFF00) * (256 - alpha)) >> 8) & 0xFF00);
					}
					else if (flags & R_DRAW_BLEND)
						color = ((color & 0xFEFEFE) >> 1) + ((pixels[x] & 0xFEFEFE) >> 1);
					pixels[x] = color;
					if (write)
						depth[x] = (unsigned long)depth_value;
					r_statistics.pixels_drawn++;
				}
			}
			depth_value += depth_step;
			u += du;
			v += dv;
			r += span->dr;
			g += span->dg;
			b += span->db;
			fr += span->dfog_r;
			fg += span->dfog_g;
			fb += span->dfog_b;
		}
		u = u_end;
		v = v_end;
	}
}

/* Unreal's software renderer's ordered dither of the texture coordinates:
each pixel of a 2 x 2 square takes its texel a different fraction of a
texel over (a quarter, a half, three quarters), so a magnified texture's
texels run into one another as if filtered, for an add a pixel. Only where
the texels are bigger than the pixels (dithering smaller ones is noise).
The row's two offsets, by x's parity: u, then v, in 16.16 texels. */
static int dither_offsets(const struct span *span, long du, long dv, long *offsets)
{
	static const long kernel[2][2][2] =
	{
		{ { 0x4000, 0x0000 }, { 0x8000, 0xC000 } },
		{ { 0xC000, 0x8000 }, { 0x0000, 0x4000 } },
	};
	const long (*row)[2] = kernel[span->y & 1];

	if (!r_frame.settings.texture_dither || du >= 0x10000 || du <= -0x10000 || dv >= 0x10000 || dv <= -0x10000)
		return 0;
	offsets[0] = row[0][0];
	offsets[1] = row[0][1];
	offsets[2] = row[1][0];
	offsets[3] = row[1][1];
	return 1;
}

/* the opaque case of nearly every pixel: textured, lit, no fog, depth
tested and written */
static void span32_textured_opaque(struct span *span)
{
	unsigned long *pixels = (unsigned long *)(r_frame.pixels + (size_t)span->y * r_frame.pitch);
	unsigned long *depth = r_frame.depth + (size_t)span->y * r_frame.width;
	const unsigned char *texels = span->texels;
	const unsigned long *palette = span->palette;
	unsigned long width_mask = span->width_mask, height_mask = span->height_mask;
	int width_shift = span->width_shift;
	int x = span->x0;
	float w = span->w, s = span->s, t = span->t;
	long depth_value = depth_start(span), depth_step = depth_step_of(span);
	long r = span->r, g = span->g, b = span->b, dr = span->dr, dg = span->dg, db = span->db;
	long u, v;
	long offsets[4];

	texel_coordinates(span, w, s, t, &u, &v);
	while (x < span->x1)
	{
		int count = span->x1 - x < SUBDIVISION ? span->x1 - x : SUBDIVISION;
		long u_end, v_end, du, dv;

		w += span->dw * count;
		s += span->ds * count;
		t += span->dt * count;
		texel_coordinates(span, w, s, t, &u_end, &v_end);
		du = (u_end - u) / count;
		dv = (v_end - v) / count;
		if (r_frame.settings.texture_smooth && magnified(du, dv))
		{
			/* (filtered: its own loop, the usual one untouched) */
			for (int i = 0; i < count; i++, x++)
			{
				if ((unsigned long)depth_value > depth[x])
				{
					unsigned long texel = bilinear(texels, palette, width_shift, width_mask, height_mask, u, v);
					unsigned long red = (((texel >> 16) & 0xFF) * (unsigned long)(r >> 16)) >> 7;
					unsigned long green = (((texel >> 8) & 0xFF) * (unsigned long)(g >> 16)) >> 7;
					unsigned long blue = ((texel & 0xFF) * (unsigned long)(b >> 16)) >> 7;

					if (red > 255) red = 255;
					if (green > 255) green = 255;
					if (blue > 255) blue = 255;
					pixels[x] = (red << 16) | (green << 8) | blue;
					depth[x] = (unsigned long)depth_value;
				}
				depth_value += depth_step;
				u += du;
				v += dv;
				r += dr;
				g += dg;
				b += db;
			}
			u = u_end;
			v = v_end;
			continue;
		}
		if (!dither_offsets(span, du, dv, offsets))
			memset(offsets, 0, sizeof(offsets));
		for (int i = 0; i < count; i++, x++)
		{
			if ((unsigned long)depth_value > depth[x])
			{
				const long *offset = offsets + ((x & 1) << 1);
				unsigned long texel = palette[texels[((((unsigned long)(v + offset[1]) >> 16) & height_mask) << width_shift) |
					(((unsigned long)(u + offset[0]) >> 16) & width_mask)]];
				unsigned long red = (((texel >> 16) & 0xFF) * (unsigned long)(r >> 16)) >> 7;
				unsigned long green = (((texel >> 8) & 0xFF) * (unsigned long)(g >> 16)) >> 7;
				unsigned long blue = ((texel & 0xFF) * (unsigned long)(b >> 16)) >> 7;

				if (red > 255) red = 255;
				if (green > 255) green = 255;
				if (blue > 255) blue = 255;
				pixels[x] = (red << 16) | (green << 8) | blue;
				depth[x] = (unsigned long)depth_value;
			}
			depth_value += depth_step;
			u += du;
			v += dv;
			r += dr;
			g += dg;
			b += db;
		}
		u = u_end;
		v = v_end;
	}
	r_statistics.pixels_drawn += span->x1 - span->x0;
}

/* the vertices' colour alone */
static void span32_flat(struct span *span, int fogged, unsigned flags)
{
	unsigned long *pixels = (unsigned long *)(r_frame.pixels + (size_t)span->y * r_frame.pitch);
	unsigned long *depth = r_frame.depth + (size_t)span->y * r_frame.width;
	long depth_value = depth_start(span), depth_step = depth_step_of(span);
	long r = span->r, g = span->g, b = span->b;
	long fr = span->fog_r, fg = span->fog_g, fb = span->fog_b;
	int test = !(flags & R_DRAW_NO_DEPTH_TEST);
	int write = !(flags & (R_DRAW_ADDITIVE | R_DRAW_BLEND));

	for (int x = span->x0; x < span->x1; x++)
	{
		if (!test || (unsigned long)depth_value > depth[x])
		{
			/* (the light is the colour here: 0x80 as 0x80) */
			unsigned long red = (unsigned long)(r >> 16), green = (unsigned long)(g >> 16),
				blue = (unsigned long)(b >> 16);
			unsigned long color = ((red > 255 ? 255 : red) << 16) | ((green > 255 ? 255 : green) << 8) |
				(blue > 255 ? 255 : blue);

			if (fogged)
				color = add_fog(color, fr, fg, fb);
			if (flags & R_DRAW_ADDITIVE)
				color = add_fog(pixels[x], (long)(color & 0xFF0000), (long)(color & 0xFF00) << 8,
					(long)(color & 0xFF) << 16);
			else if (flags & R_DRAW_BLEND)
				color = ((color & 0xFEFEFE) >> 1) + ((pixels[x] & 0xFEFEFE) >> 1);
			pixels[x] = color;
			if (write)
				depth[x] = (unsigned long)depth_value;
		}
		depth_value += depth_step;
		r += span->dr;
		g += span->dg;
		b += span->db;
		fr += span->dfog_r;
		fg += span->dfog_g;
		fb += span->dfog_b;
	}
	r_statistics.pixels_drawn += span->x1 - span->x0;
}

/* a screen quad's span: affine, no depth, the screen blend; the colour
(r, g, b and, in fog_r, the alpha) times the texel */
static void span32_screen(struct span *span, int textured)
{
	unsigned long *pixels = (unsigned long *)(r_frame.pixels + (size_t)span->y * r_frame.pitch);
	long u = fixed(span->s), v = fixed(span->t), du = fixed(span->ds), dv = fixed(span->dt);
	long r = span->r, g = span->g, b = span->b, a = span->fog_r;

	for (int x = span->x0; x < span->x1; x++)
	{
		unsigned long texel = 0xFFFFFFFF, alpha, red, green, blue, under = pixels[x];

		if (textured)
		{
			unsigned char index = TEXEL(span, u, v);

			if (span->alpha_test && !index)
				goto next;
			texel = span->palette[index];
		}
		/* (a texture's palette carries its alpha in the top byte) */
		alpha = ((texel >> 24) * (unsigned long)(a >> 16)) >> 8;
		red = (((texel >> 16) & 0xFF) * (unsigned long)(r >> 16)) >> 8;
		green = (((texel >> 8) & 0xFF) * (unsigned long)(g >> 16)) >> 8;
		blue = ((texel & 0xFF) * (unsigned long)(b >> 16)) >> 8;
		switch (screen_blend)
		{
		case _r_screen_opaque:
			break;
		case _r_screen_alpha:
			red = (red * alpha + ((under >> 16) & 0xFF) * (255 - alpha)) >> 8;
			green = (green * alpha + ((under >> 8) & 0xFF) * (255 - alpha)) >> 8;
			blue = (blue * alpha + (under & 0xFF) * (255 - alpha)) >> 8;
			break;
		case _r_screen_add:
			red = ((under >> 16) & 0xFF) + ((red * alpha) >> 8);
			green = ((under >> 8) & 0xFF) + ((green * alpha) >> 8);
			blue = (under & 0xFF) + ((blue * alpha) >> 8);
			break;
		case _r_screen_premultiplied:
			red += (((under >> 16) & 0xFF) * (255 - alpha)) >> 8;
			green += (((under >> 8) & 0xFF) * (255 - alpha)) >> 8;
			blue += ((under & 0xFF) * (255 - alpha)) >> 8;
			break;
		}
		pixels[x] = ((red > 255 ? 255 : red) << 16) | ((green > 255 ? 255 : green) << 8) | (blue > 255 ? 255 : blue);
	next:
		u += du;
		v += dv;
		r += span->dr;
		g += span->dg;
		b += span->db;
		a += span->dfog_r;
	}
}

/* ---------- 8-bit spans */

static void span8_textured(struct span *span, int fogged, unsigned flags)
{
	unsigned char *pixels = r_frame.pixels + (size_t)span->y * r_frame.pitch;
	unsigned long *depth = r_frame.depth + (size_t)span->y * r_frame.width;
	int x = span->x0;
	float w = span->w, s = span->s, t = span->t;
	long depth_value = depth_start(span), depth_step = depth_step_of(span);
	long light = span->light, fog = span->fog;
	long u, v;
	int test = !(flags & R_DRAW_NO_DEPTH_TEST);
	int write = !(flags & (R_DRAW_ADDITIVE | R_DRAW_BLEND));
	const unsigned char *blend_table = (flags & R_DRAW_ADDITIVE) ? r_palette_add_table() :
		(flags & R_DRAW_BLEND) ? r_palette_blend_table() : NULL;

	texel_coordinates(span, w, s, t, &u, &v);
	while (x < span->x1)
	{
		int count = span->x1 - x < SUBDIVISION ? span->x1 - x : SUBDIVISION;
		long u_end, v_end, du, dv;

		w += span->dw * count;
		s += span->ds * count;
		t += span->dt * count;
		texel_coordinates(span, w, s, t, &u_end, &v_end);
		du = (u_end - u) / count;
		dv = (v_end - v) / count;
		for (int i = 0; i < count; i++, x++)
		{
			if (!test || (unsigned long)depth_value > depth[x])
			{
				unsigned char index = TEXEL(span, u, v);

				/* (by alpha: none under a quarter, whole above three) */
				unsigned long alpha = (flags & R_DRAW_ALPHA) ? ((span->palette[index] >> 24) * alpha_scale) >> 8 : 0x80;

				/* (an added texel that, lit, adds less than a 15-bit colour's
				step: nothing, as in 32 bits. A sheet's black, as the
				picture's darkest colour added, moved what was under the
				whole quad to its neighbours) */
				if (flags & R_DRAW_ADDITIVE)
				{
					unsigned long texel = span->palette[index], brightest = (texel >> 16) & 0xFF;

					if (((texel >> 8) & 0xFF) > brightest) brightest = (texel >> 8) & 0xFF;
					if ((texel & 0xFF) > brightest) brightest = texel & 0xFF;
					if (brightest * (unsigned long)(light >> 16) < 8 * 16)
						alpha = 0;
				}
				if ((!span->alpha_test || index) && alpha >= 0x40)
				{
					unsigned char color = r_color_map[light >> 16][span->remap[index]];

					if (fogged)
						color = r_fog_map[fog >> 16][color];
					if (blend_table && alpha < 0xC0)
						color = blend_table[color * 256 + pixels[x]];
					pixels[x] = color;
					if (write)
						depth[x] = (unsigned long)depth_value;
				}
			}
			depth_value += depth_step;
			u += du;
			v += dv;
			light += span->dlight;
			fog += span->dfog;
		}
		u = u_end;
		v = v_end;
	}
	r_statistics.pixels_drawn += span->x1 - span->x0;
}

static void span8_flat(struct span *span, int fogged, unsigned flags)
{
	unsigned char *pixels = r_frame.pixels + (size_t)span->y * r_frame.pitch;
	unsigned long *depth = r_frame.depth + (size_t)span->y * r_frame.width;
	long depth_value = depth_start(span), depth_step = depth_step_of(span);
	long r = span->r, g = span->g, b = span->b, fog = span->fog;
	int test = !(flags & R_DRAW_NO_DEPTH_TEST);
	int write = !(flags & (R_DRAW_ADDITIVE | R_DRAW_BLEND));
	const unsigned char *blend_table = (flags & R_DRAW_ADDITIVE) ? r_palette_add_table() :
		(flags & R_DRAW_BLEND) ? r_palette_blend_table() : NULL;

	for (int x = span->x0; x < span->x1; x++)
	{
		if (!test || (unsigned long)depth_value > depth[x])
		{
			unsigned long red = (unsigned long)(r >> 16), green = (unsigned long)(g >> 16),
				blue = (unsigned long)(b >> 16);
			unsigned char color = r_palette_nearest(((red > 255 ? 255 : red) << 16) |
				((green > 255 ? 255 : green) << 8) | (blue > 255 ? 255 : blue));

			if (fogged)
				color = r_fog_map[fog >> 16][color];
			if (blend_table)
				color = blend_table[color * 256 + pixels[x]];
			pixels[x] = color;
			if (write)
				depth[x] = (unsigned long)depth_value;
		}
		depth_value += depth_step;
		r += span->dr;
		g += span->dg;
		b += span->db;
		fog += span->dfog;
	}
	r_statistics.pixels_drawn += span->x1 - span->x0;
}

/* A screen quad's colour for each texel (texel x the quad's colour, as
the 8-bit picture's nearest, and whether it covers) is the same across the
quad where its corners share a colour, as the HUD's do: worked out the first
time a texel is drawn in a quad, then kept. Each quad and colour has its own
stamp (screen_stamp, a new one whenever either changes): a kept colour is
the current one's where its stamp is (one comparison a pixel). */
static struct
{
	unsigned long stamp;
	/* what is under is first darkened to darken / 16 (the light maps, 16
	as it is), then the colour added (add) or put there; covers 0: the texel
	leaves the pixel as it is (a cut-out's hole too) */
	unsigned char color, covers, darken;
} screen_colors[256];
static unsigned long screen_stamp, stamp_quad, stamp_rgba;

static void screen_color8(unsigned long texel, long r, long g, long b, long a, const unsigned char *add,
	unsigned char *color, unsigned char *covers, unsigned char *darken)
{
	unsigned long alpha = ((texel >> 24) * (unsigned long)(a >> 16)) >> 8;
	unsigned long red = (((texel >> 16) & 0xFF) * (unsigned long)(r >> 16)) >> 8;
	unsigned long green = (((texel >> 8) & 0xFF) * (unsigned long)(g >> 16)) >> 8;
	unsigned long blue = ((texel & 0xFF) * (unsigned long)(b >> 16)) >> 8;

	*covers = 1;
	*darken = 16;
	if (screen_blend == _r_screen_premultiplied)
	{
		/* (premultiplied: the colour added as it is, over what is under
		darkened by 1 - alpha) */
		*color = r_palette_nearest((red << 16) | (green << 8) | blue);
		*darken = (unsigned char)(((255 - alpha) * 16 + 127) / 255);
		/* (nothing added, nothing darkened: a texel that leaves the pixel
		as it is, most of a HUD bitmap's, is skipped) */
		*covers = (red | green | blue) || *darken != 16;
	}
	else if (add)
	{
		unsigned long added = (((red * alpha) >> 8) << 16) | (((green * alpha) >> 8) << 8) | ((blue * alpha) >> 8);

		*color = r_palette_nearest(added);
		*covers = added != 0;
	}
	else
	{
		*color = r_palette_nearest((red << 16) | (green << 8) | blue);
		*covers = screen_blend == _r_screen_opaque || alpha >= 128;
	}
}

/* the screen's quads in 8 bits: covered where alpha is over half, the
colour looked up (blends by the blend tables) */
static void span8_screen(struct span *span, int textured)
{
	unsigned char *pixels = r_frame.pixels + (size_t)span->y * r_frame.pitch;
	long u = fixed(span->s), v = fixed(span->t), du = fixed(span->ds), dv = fixed(span->dt);
	long r = span->r, g = span->g, b = span->b, a = span->fog_r;
	const unsigned char *add = screen_blend == _r_screen_add || screen_blend == _r_screen_premultiplied ?
		r_palette_add_table() : NULL;

	if (textured && !span->dr && !span->dg && !span->db && !span->dfog_r)
	{
		/* (one colour across the span: the quad's kept colours) */
		unsigned long rgba = ((unsigned long)(r >> 16) << 24) | ((unsigned long)(g >> 16) << 16) |
			((unsigned long)(b >> 16) << 8) | (unsigned long)((a >> 16) & 0xFF);
		/* (the texture's in locals: a pixel's store could be anything's, as
		far as the compiler knows; a rectangle's row of texels, v the same
		across it, looked up once) */
		const unsigned char *texels = span->texels;
		unsigned long width_mask = span->width_mask, height_mask = span->height_mask;
		int width_shift = span->width_shift, alpha_test = span->alpha_test;
		unsigned long stamp;

		if (screen_quad != stamp_quad || rgba != stamp_rgba || !screen_stamp)
		{
			screen_stamp++;
			stamp_quad = screen_quad;
			stamp_rgba = rgba;
		}
		stamp = screen_stamp;
#define SCREEN_KEPT(index_of, draw) \
	for (int x = span->x0; x < span->x1; x++, u += du, v += dv) \
	{ \
		unsigned index = (index_of); \
\
		if (screen_colors[index].stamp != stamp) \
		{ \
			screen_colors[index].stamp = stamp; \
			screen_color8(span->palette[index], r, g, b, a, add, &screen_colors[index].color, \
				&screen_colors[index].covers, &screen_colors[index].darken); \
			if (alpha_test && !index) \
				screen_colors[index].covers = 0; \
		} \
		if (screen_colors[index].covers) \
			pixels[x] = (draw); \
	}
#define ROW_TEXEL texels[((unsigned long)u >> 16) & width_mask]
#define ANY_TEXEL texels[((((unsigned long)v >> 16) & height_mask) << width_shift) | (((unsigned long)u >> 16) & width_mask)]
#define ADDED add[screen_colors[index].color * 256 + r_color_map[screen_colors[index].darken][pixels[x]]]
		if (!dv)
		{
			texels += (((unsigned long)v >> 16) & height_mask) << width_shift;
			if (add)
				SCREEN_KEPT(ROW_TEXEL, ADDED)
			else
				SCREEN_KEPT(ROW_TEXEL, screen_colors[index].color)
		}
		else if (add)
		{
			SCREEN_KEPT(ANY_TEXEL, ADDED)
		}
		else
		{
			SCREEN_KEPT(ANY_TEXEL, screen_colors[index].color)
		}
#undef SCREEN_KEPT
#undef ROW_TEXEL
#undef ANY_TEXEL
#undef ADDED
		return;
	}
	for (int x = span->x0; x < span->x1; x++)
	{
		unsigned long texel = 0xFFFFFFFF, alpha, red, green, blue;

		if (textured)
		{
			unsigned char index = TEXEL(span, u, v);

			if (span->alpha_test && !index)
				goto next;
			texel = span->palette[index];
		}
		alpha = ((texel >> 24) * (unsigned long)(a >> 16)) >> 8;
		red = (((texel >> 16) & 0xFF) * (unsigned long)(r >> 16)) >> 8;
		green = (((texel >> 8) & 0xFF) * (unsigned long)(g >> 16)) >> 8;
		blue = ((texel & 0xFF) * (unsigned long)(b >> 16)) >> 8;
		if (add)
		{
			unsigned char color, covers, darken;

			screen_color8(texel, r, g, b, a, add, &color, &covers, &darken);
			if (covers)
				pixels[x] = add[color * 256 + r_color_map[darken][pixels[x]]];
		}
		else if (screen_blend == _r_screen_opaque || alpha >= 128)
		{
			pixels[x] = r_palette_nearest((red << 16) | (green << 8) | blue);
		}
	next:
		u += du;
		v += dv;
		r += span->dr;
		g += span->dg;
		b += span->db;
		a += span->dfog_r;
	}
}

/* a 4 x 4 ordered dither (Bayer's) of a level's fraction, in 16.16 */
static const long light_dither[4][4] =
{
	{ 0x0800, 0x8800, 0x2800, 0xA800 },
	{ 0xC800, 0x4800, 0xE800, 0x6800 },
	{ 0x3800, 0xB800, 0x1800, 0x9800 },
	{ 0xF800, 0x7800, 0xD800, 0x5800 },
};

/* a triangle this many pixels across, whose light changes, has it a pixel
at a time and dithered between its levels: across a large one, one level a
segment shows as steps */
#define DITHERED_EXTENT 24

/* ---------- the screen's rectangles

A screen quad that is a rectangle on the screen and in its texture (the
HUD's are), of one colour: filled a row at a time with the screen's spans,
no triangles' set-up or edges. A pixel is in it when its centre is, as in a
triangle; its texel is the one under its centre. */
void r_raster_screen_rect(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
	const struct r_texture *texture, unsigned long color)
{
	struct span span;
	int xs = (int)r_round(x0), xe = (int)r_round(x1), ys = (int)r_round(y0), ye = (int)r_round(y1);
	float du = (u1 - u0) / (x1 - x0), dv = (v1 - v0) / (y1 - y0);

	if (r_backend)
	{
		r_backend->screen_rect(x0, y0, x1, y1, u0, v0, u1, v1, texture, color);
		return;
	}
	if (xs < 0)
		xs = 0;
	if (xe > r_frame.width)
		xe = r_frame.width;
	if (ys < 0)
		ys = 0;
	if (ye > r_frame.height)
		ye = r_frame.height;
	if (xs >= xe || ys >= ye)
		return;
	memset(&span, 0, sizeof(span));
	if (texture)
	{
		float width = (float)(1 << texture->width_shift), height = (float)(1 << texture->height_shift);

		span.texels = texture->texels[0];
		span.width_shift = texture->width_shift;
		span.width_mask = (1UL << texture->width_shift) - 1;
		span.height_mask = (1UL << texture->height_shift) - 1;
		span.palette = texture->palette;
		span.remap = texture->remap;
		span.alpha_test = (texture->flags & R_TEXTURE_ALPHA_TEST) != 0;
		span.affine = 1;
		u0 *= width;
		du *= width;
		v0 *= height;
		dv *= height;
	}
	span.ds = du;
	span.r = (long)((color >> 16) & 0xFF) << 16;
	span.g = (long)((color >> 8) & 0xFF) << 16;
	span.b = (long)(color & 0xFF) << 16;
	/* (the screen's spans read the alpha here) */
	span.fog_r = (long)(color >> 24) << 16;
	span.x0 = xs;
	span.x1 = xe;
	span.s = u0 + ((float)xs + 0.5f - x0) * du;
	for (int y = ys; y < ye; y++)
	{
		span.y = y;
		span.t = v0 + ((float)y + 0.5f - y0) * dv;
		if (r_frame.bits == 8)
			span8_screen(&span, texture != NULL);
		else
			span32_screen(&span, texture != NULL);
	}
}

/* ---------- the 8-bit picture's opaque triangles

Nearly everything the level and the models draw in 8 bits: textured
(texels the picture's palette indices already, r_texture_new; or cut out,
an alpha-tested texture's index 0 left undrawn, the others remapped), lit,
perhaps fogged, depth tested and written. Halo's triangles at 320x240 are small (a
few rows of a few pixels), so what a triangle and a row cost matters more
than what a pixel does: one function, no span structure or call a row. A
row's start values come from the triangle's planes; its pixels go in
segments of up to 16 (a perspective division at each end, none for an
affine triangle), each with one light level and one fog level (the colour
maps' rows): light and fog change slowly across Halo's small triangles; a
large one whose light changes has its light a pixel at a time, dithered
between the light maps' levels. In the pixel loop v is kept times the
texture's width, so a texel's index is two
masks and an or (worked out in plain texels, then shifted in 32 bits,
which wrap with the texture). */

static int light_row(long level, int levels)
{
	long row = level >> 16;

	return row < 0 ? 0 : row >= levels ? levels - 1 : (int)row;
}

struct r_span8_constants r_span8;

/* (r_span8.s reads it by offset) */
typedef char r_span8_layout_check[offsetof(struct r_span8_constants, width_shift) == 164 &&
	offsetof(struct r_span8_constants, p_w_dy) == 272 && offsetof(struct r_span8_constants, depth_step) == 280 ?
	1 : -1];

static void raster8_opaque(const struct r_raster_vertex *a, const struct r_raster_vertex *b,
	const struct r_raster_vertex *c, const struct r_raster_vertex *top, const struct r_raster_vertex *middle,
	const struct r_raster_vertex *bottom, float determinant, int y_start, int y_end, int affine,
	const struct r_texture *texture, int level, int fogged, int x0, int x1)
{
	float ab_x = b->x - a->x, ab_y = b->y - a->y, ac_x = c->x - a->x, ac_y = c->y - a->y;
	float inverse = 1.0f / determinant;
	int width_shift = texture->width_shift - level, height_shift = texture->height_shift - level;
	float width, height;
	const unsigned char *texels = texture->texels[level];
	const unsigned char *remap = texture->remap;
	int cut_out = (texture->flags & R_TEXTURE_ALPHA_TEST) != 0;
	unsigned long umask, vmask;
	/* the planes at x = 0 on the current row's centre, and their steps */
	float w_row, s_row, t_row, l_row, f_row = 0.0f;
	float w_dx, s_dx, t_dx, l_dx, f_dx = 0.0f, w_dy, s_dy, t_dy, l_dy, f_dy = 0.0f;
	float py, x_long, long_slope, upper_slope, lower_slope;
	long dz, du_affine = 0, dv_affine = 0, dl, df = 0;
	int dithered, unlit;

	if (width_shift < 0) width_shift = 0;
	if (height_shift < 0) height_shift = 0;
	width = (float)(1 << width_shift);
	height = (float)(1 << height_shift);
	umask = (1UL << width_shift) - 1;
	vmask = ((1UL << height_shift) - 1) << width_shift;
	py = (float)y_start + 0.5f;

#define PLANE(get, scale, row_value, step_x, step_y) \
	{ \
		float v0 = (get(a)) * (scale), d1 = (get(b)) * (scale) - v0, d2 = (get(c)) * (scale) - v0; \
		step_x = (d1 * ac_y - d2 * ab_y) * inverse; \
		step_y = (d2 * ab_x - d1 * ac_x) * inverse; \
		row_value = v0 - a->x * step_x + (py - a->y) * step_y; \
	}
#define GET_W(vertex) ((vertex)->w)
#define GET_S(vertex) (affine ? (vertex)->u : (vertex)->s)
#define GET_T(vertex) (affine ? (vertex)->v : (vertex)->t)
#define GET_LIGHT(vertex) ((vertex)->light)
#define GET_FOG(vertex) ((vertex)->fog)
	PLANE(GET_W, depth_scale, w_row, w_dx, w_dy)
	PLANE(GET_S, width, s_row, s_dx, s_dy)
	PLANE(GET_T, height, t_row, t_dx, t_dy)
	if (a->light == b->light && a->light == c->light)
	{
		/* (one light across it, as without lighting: no plane) */
		l_row = a->light;
		l_dx = l_dy = 0.0f;
	}
	else
	{
		PLANE(GET_LIGHT, 1.0f, l_row, l_dx, l_dy)
	}
	if (fogged)
		PLANE(GET_FOG, 1.0f, f_row, f_dx, f_dy)
#undef PLANE
#undef GET_W
#undef GET_S
#undef GET_T
#undef GET_LIGHT
#undef GET_FOG
	dz = (long)wrap_round(w_dx);
	dl = (long)wrap_fixed(l_dx);
	{
		float x_low = a->x, x_high = a->x;

		if (b->x < x_low) x_low = b->x;
		if (b->x > x_high) x_high = b->x;
		if (c->x < x_low) x_low = c->x;
		if (c->x > x_high) x_high = c->x;
		dithered = r_frame.settings.light_dither && (l_dx != 0.0f || l_dy != 0.0f) &&
			(x_high - x_low >= DITHERED_EXTENT || bottom->y - top->y >= DITHERED_EXTENT);
	}
	if (fogged)
		df = (long)wrap_fixed(f_dx);
	if (affine)
	{
		du_affine = (long)wrap_fixed(s_dx);
		dv_affine = (long)wrap_fixed(t_dx) * (1L << width_shift);
	}
	long_slope = (bottom->x - top->x) / (bottom->y - top->y);
	upper_slope = middle->y > top->y ? (middle->x - top->x) / (middle->y - top->y) : 0.0f;
	lower_slope = bottom->y > middle->y ? (bottom->x - middle->x) / (bottom->y - middle->y) : 0.0f;
	x_long = top->x + (py - top->y) * long_slope;
	r_span8.texels = texels;
	r_span8.umask16 = umask << 16;
	r_span8.index_mask = umask | vmask;
	r_span8.remap = remap;
	r_span8.dz = (unsigned long)dz;
	/* (a pixel's depth is at this plus four times its address, on the
	first row; a row on, the depth buffer's row is its width in words on,
	the picture's its pitch in bytes, a whole number of words that may
	exceed the width) */
	r_span8.depth_offset = (unsigned long)(size_t)(r_frame.depth + (size_t)y_start * r_frame.width) -
		4 * (unsigned long)(size_t)(r_frame.pixels + (size_t)y_start * r_frame.pitch);
	r_span8.depth_step = 4 * ((unsigned long)r_frame.width - (unsigned long)r_frame.pitch);
	/* (one light across it, level 16, as without lighting: the texel as it
	is) */
	unlit = l_dx == 0.0f && l_dy == 0.0f && light_row(fixed(l_row), R_LIGHT_LEVELS) == 16;

	if (affine && !dithered)
	{
		/* No perspective and no dither: the rows in whole numbers, all of
		them in r_span8.s (r_span8_rows_*: no call or C a row). Each edge's x
		and each value at a row's x = 0 (the half pixel to the first centre
		folded in) step down a row by an add; a row's first pixel's values
		are those plus x times their steps. 16.16 fixed point; depth and the
		texture coordinates wrap in 32 bits as they do in the pixel loop (v
		kept times the width from the start: the shift spreads over the
		sums). The light and fog the row's first pixel's; one light across
		the triangle and level 16 (no lighting) is the texel as it is. Two
		calls: the rows down to the middle corner's, along the upper short
		edge, and those after it, along the lower */
		int y_middle = (int)r_round(middle->y);
		int split = y_middle < y_start ? y_start : y_middle > y_end ? y_end : y_middle;
		void (*rows)(void) = cut_out ? (fogged ? r_span8_rows_cut_fogged : r_span8_rows_cut_shaded) :
			fogged ? r_span8_rows_fogged : unlit ? r_span8_rows_unlit : r_span8_rows_shaded;

		r_span8.x_long = fixed(x_long);
		r_span8.long_step = fixed(long_slope);
		r_span8.x_short = fixed(top->x + (py - top->y) * upper_slope);
		r_span8.short_step = fixed(upper_slope);
		r_span8.x0 = x0;
		r_span8.x1 = x1;
		/* (the values at x = 0 are often past 32 bits, a plane's reach from
		where the triangle is: kept modulo 2^32, which the adds along a row
		bring back to the triangle's own) */
		r_span8.u_row = wrap_fixed(s_row + 0.5f * s_dx);
		r_span8.v_row = wrap_fixed(t_row + 0.5f * t_dx) << width_shift;
		r_span8.z_row = wrap_round(w_row + 0.5f * w_dx) + depth_bias;
		r_span8.u_dy = wrap_fixed(s_dy);
		r_span8.v_dy = wrap_fixed(t_dy) << width_shift;
		r_span8.z_dy = wrap_round(w_dy);
		r_span8.du = (unsigned long)du_affine;
		r_span8.dv = (unsigned long)dv_affine;
		r_span8.light_row = (long)wrap_fixed(l_row + 0.5f * l_dx);
		r_span8.light_dy = (long)wrap_fixed(l_dy);
		r_span8.dlight = dl;
		r_span8.fog_row = fogged ? (long)wrap_fixed(f_row + 0.5f * f_dx) : 0;
		r_span8.fog_dy = (long)wrap_fixed(f_dy);
		r_span8.dfog = df;
		r_span8.constant_shade = l_dx == 0.0f && l_dy == 0.0f ?
			r_color_map[light_row(r_span8.light_row, R_LIGHT_LEVELS)] : NULL;
		r_span8.pixel_row = r_frame.pixels + (size_t)y_start * r_frame.pitch;
		r_span8.pitch = r_frame.pitch;
		if (split > y_start)
		{
			r_span8.rows = split - y_start;
			rows();
		}
		if (y_end > split)
		{
			/* (onto the lower short edge) */
			r_span8.x_short = fixed(middle->x + ((float)split + 0.5f - middle->y) * lower_slope);
			r_span8.short_step = fixed(lower_slope);
			r_span8.rows = y_end - split;
			rows();
		}
		return;
	}

	/* the edges in 16.16, stepped a row, as the affine rows' (so an edge's
	pixels are the same whichever of its two triangles draws it); one light
	across the triangle (no lighting) looked up once */
	long x_long_f = fixed(x_long), long_step = fixed(long_slope);
	long x_short_f = fixed(top->x + (py - top->y) * upper_slope), short_step = fixed(upper_slope);
	int y_middle = (int)r_round(middle->y), lower = 0;
	const unsigned char *constant_shade = l_dx == 0.0f && l_dy == 0.0f ?
		r_color_map[light_row(fixed(l_row), R_LIGHT_LEVELS)] : NULL;

	if (!dithered)
	{
		/* No dither (light a segment at a time): the rows all in r_span8.s
		(r_span8_perspective_*), which works their values out from the
		planes as the loop below does, operation for operation, the same
		in each precision (w and its steps as the x87 has them: a double
		holds them, at the 53 bits the runtime keeps it to; the rest
		floats). Two calls, as the affine rows' */
		int split = y_middle < y_start ? y_start : y_middle > y_end ? y_end : y_middle;
		void (*rows)(void) = cut_out ? (fogged ? r_span8_perspective_cut_fogged : r_span8_perspective_cut_shaded) :
			fogged ? r_span8_perspective_fogged : unlit ? r_span8_perspective_unlit : r_span8_perspective_shaded;

		r_span8.constant_shade = constant_shade;
		r_span8.width_shift = width_shift;
		r_span8.dlight16 = dl * SUBDIVISION;
		r_span8.dfog16 = df * SUBDIVISION;
		r_span8.x_long = x_long_f;
		r_span8.long_step = long_step;
		r_span8.x_short = x_short_f;
		r_span8.short_step = short_step;
		r_span8.x0 = x0;
		r_span8.x1 = x1;
		r_span8.pixel_row = r_frame.pixels + (size_t)y_start * r_frame.pitch;
		r_span8.pitch = r_frame.pitch;
		r_span8.p_w_row = w_row;
		r_span8.p_s_row = s_row;
		r_span8.p_t_row = t_row;
		r_span8.p_l_row = l_row;
		r_span8.p_f_row = f_row;
		r_span8.p_w_dy = w_dy;
		r_span8.p_s_dy = s_dy;
		r_span8.p_t_dy = t_dy;
		r_span8.p_l_dy = l_dy;
		r_span8.p_f_dy = f_dy;
		r_span8.p_w_dx = w_dx;
		r_span8.p_s_dx = s_dx;
		r_span8.p_t_dx = t_dx;
		r_span8.p_l_dx = l_dx;
		r_span8.p_f_dx = f_dx;
		r_span8.p_k = 65536.0f * depth_scale;
		r_span8.p_half = 0.5f;
		r_span8.p_fixed = 65536.0f;
		r_span8.p_depth_bias = depth_bias;
		if (split > y_start)
		{
			r_span8.rows = split - y_start;
			rows();
		}
		if (y_end > split)
		{
			/* (onto the lower short edge) */
			r_span8.x_short = fixed(middle->x + ((float)split + 0.5f - middle->y) * lower_slope);
			r_span8.short_step = fixed(lower_slope);
			r_span8.rows = y_end - split;
			rows();
		}
		return;
	}

	/* dithered (the light a pixel at a time): here, the planes' values
	kept floats between their rows, as they always were (w's steps as the
	x87 holds them): this loop has no call to make the compiler keep them so */
	w_row = to_float(w_row);
	s_row = to_float(s_row);
	t_row = to_float(t_row);
	l_row = to_float(l_row);
	f_row = to_float(f_row);
	s_dx = to_float(s_dx);
	t_dx = to_float(t_dx);
	l_dx = to_float(l_dx);
	f_dx = to_float(f_dx);
	s_dy = to_float(s_dy);
	t_dy = to_float(t_dy);
	l_dy = to_float(l_dy);
	f_dy = to_float(f_dy);

	for (int y = y_start; y < y_end; y++)
	{
		long left, right;
		int xs, xe;

		if (!lower && y >= y_middle)
		{
			/* (onto the lower short edge) */
			lower = 1;
			x_short_f = fixed(middle->x + ((float)y + 0.5f - middle->y) * lower_slope);
			short_step = fixed(lower_slope);
		}
		left = x_long_f < x_short_f ? x_long_f : x_short_f;
		right = x_long_f < x_short_f ? x_short_f : x_long_f;
		xs = (int)((left + 0x8000) >> 16);
		xe = (int)((right + 0x8000) >> 16);
		if (xs < x0)
			xs = x0;
		if (xe > x1)
			xe = x1;
		if (xs < xe)
		{
			unsigned char *p = r_frame.pixels + (size_t)y * r_frame.pitch + xs;
			unsigned long *d = r_frame.depth + (size_t)y * r_frame.width + xs;
			float fx = (float)xs + 0.5f;
			/* (s and t kept as floats: to_float) */
			float w = w_row + fx * w_dx, s = to_float(s_row + fx * s_dx), t = to_float(t_row + fx * t_dx);
			unsigned long z = (unsigned long)r_round(w) + depth_bias;
			long light = fixed(l_row + fx * l_dx), fog = fogged ? fixed(f_row + fx * f_dx) : 0;
			/* u in 16.16 texels; v the same (row_v) and times the width (v) */
			unsigned long u, row_v, v;
			int n = xe - xs;

			if (affine)
			{
				u = (unsigned long)fixed(s);
				row_v = (unsigned long)fixed(t);
			}
			else
			{
				/* (the texture coordinates over z: divided by 1/z) */
				float iz = 65536.0f * depth_scale / w;

				u = (unsigned long)r_round(s * iz);
				row_v = (unsigned long)r_round(t * iz);
			}
			v = row_v << width_shift;
			r_statistics.pixels_drawn += n;
			while (n > 0)
			{
				int count = n < SUBDIVISION ? n : SUBDIVISION;
				const unsigned char *fog_shade = fogged ? r_fog_map[light_row(fog, R_FOG_LEVELS)] : NULL;
				unsigned long u_next = 0;
				long du, dv;

				if (affine)
				{
					du = du_affine;
					dv = dv_affine;
				}
				else
				{
					float iz;
					unsigned long u_end, v_end;

					w += w_dx * count;
					s += s_dx * count;
					t += t_dx * count;
					iz = 65536.0f * depth_scale / w;
					u_end = (unsigned long)r_round(s * iz);
					v_end = (unsigned long)r_round(t * iz);
					s = to_float(s);
					t = to_float(t);
					if (count == SUBDIVISION)
					{
						du = (long)(u_end - u) >> SUBDIVISION_SHIFT;
						dv = (long)(v_end - row_v) >> SUBDIVISION_SHIFT;
					}
					else
					{
						du = (long)(u_end - u) / count;
						dv = (long)(v_end - row_v) / count;
					}
					dv *= 1L << width_shift;
					/* (the next segment starts where this one ends exactly) */
					u_next = u_end;
					row_v = v_end;
				}
				n -= count;
				{
					/* (dithered: the light a pixel at a time; kept inside the
					maps' rows for the whole segment, the dither reaching at
					most the spare top one) */
					const long *dither = light_dither[y & 3];
					int phase = (int)((p - (r_frame.pixels + (size_t)y * r_frame.pitch)) & 3);
					long top_level = (long)(R_LIGHT_LEVELS - 1) << 16, step = dl, last;

					if (light < 0)
						light = 0;
					if (light > top_level)
						light = top_level;
					last = light + step * (count - 1);
					if (last < 0 || last > top_level)
						step = 0;
#define DITHERED(test, color) \
	do \
	{ \
		if (z > *d) \
		{ \
			unsigned index = texels[((v >> 16) & vmask) | ((u >> 16) & umask)]; \
\
			if (test) \
			{ \
				const unsigned char *lit = r_color_map[(light + dither[phase]) >> 16]; \
\
				*d = z; \
				*p = color; \
			} \
		} \
		p++; \
		d++; \
		u += du; \
		v += dv; \
		z += dz; \
		light += step; \
		phase = (phase + 1) & 3; \
	} while (--count)
					if (cut_out)
					{
						if (fogged)
							DITHERED(index, fog_shade[lit[remap[index]]]);
						else
							DITHERED(index, lit[remap[index]]);
					}
					else if (fogged)
					{
						DITHERED(1, fog_shade[lit[index]]);
					}
					else
					{
						DITHERED(1, lit[index]);
					}
#undef DITHERED
					if (!affine)
					{
						u = u_next;
						v = row_v << width_shift;
					}
					fog += df * SUBDIVISION;
				}
			}
		}
		/* down a row */
		x_long_f += long_step;
		x_short_f += short_step;
		w_row = to_float(w_row + w_dy);
		s_row = to_float(s_row + s_dy);
		t_row = to_float(t_row + t_dy);
		l_row = to_float(l_row + l_dy);
		f_row = to_float(f_row + f_dy);
	}
}

/* raster8_opaque's edges are 16.16 fixed point: a corner more than 32768
pixels off the picture (one cut at the near plane beside the eye: a wall
alongside, the ground under a falling camera; at 640x480 a wall 5 units to
the side does it) would wrap, and the rows fill the wrong side of their
edges. Such a triangle is cut down to FAR_X either side of the view
first; each piece is drawn as its part of the whole (the same planes, as
affine or not as the whole), an edge two triangles share cut at the same
point in both (worked out from its left end). */
#define FAR_X 8192.0f

/* a vertex between two, at x (raster8_opaque's values alone) */
static void far_cut(const struct r_raster_vertex *p, const struct r_raster_vertex *q, float x,
	struct r_raster_vertex *out)
{
	const struct r_raster_vertex *from = p->x < q->x ? p : q, *to = p->x < q->x ? q : p;
	float f = (x - from->x) / (to->x - from->x);

	out->x = x;
	out->y = from->y + (to->y - from->y) * f;
	out->w = from->w + (to->w - from->w) * f;
	out->s = from->s + (to->s - from->s) * f;
	out->t = from->t + (to->t - from->t) * f;
	out->u = from->u + (to->u - from->u) * f;
	out->v = from->v + (to->v - from->v) * f;
	out->light = from->light + (to->light - from->light) * f;
	out->fog = from->fog + (to->fog - from->fog) * f;
}

/* the polygon's part on the side of x that keep says (1 right of it, -1
left), into out: how many corners */
static int far_clip(const struct r_raster_vertex *in, int count, float x, int keep, struct r_raster_vertex *out)
{
	int n = 0;

	for (int i = 0; i < count; i++)
	{
		const struct r_raster_vertex *p = &in[i], *q = &in[(i + 1) % count];
		int p_in = keep > 0 ? p->x >= x : p->x <= x, q_in = keep > 0 ? q->x >= x : q->x <= x;

		if (p_in)
			out[n++] = *p;
		if (p_in != q_in)
			far_cut(p, q, x, &out[n++]);
	}
	return n;
}

static void raster8_far(const struct r_raster_vertex *a, const struct r_raster_vertex *b,
	const struct r_raster_vertex *c, int affine, const struct r_texture *texture, int level, int fogged, int x0,
	int y0, int x1, int y1)
{
	struct r_raster_vertex polygon[3], left_cut[4], both_cut[5];
	int count;

	polygon[0] = *a;
	polygon[1] = *b;
	polygon[2] = *c;
	count = far_clip(polygon, 3, (float)x0 - FAR_X, 1, left_cut);
	count = far_clip(left_cut, count, (float)x1 + FAR_X, -1, both_cut);
	for (int i = 1; i + 1 < count; i++)
	{
		const struct r_raster_vertex *p = &both_cut[0], *q = &both_cut[i], *r = &both_cut[i + 1];
		const struct r_raster_vertex *top = p, *middle = q, *bottom = r, *swap;
		float determinant = (q->x - p->x) * (r->y - p->y) - (r->x - p->x) * (q->y - p->y);
		int y_start, y_end;

		if (!(determinant > 1e-6f || determinant < -1e-6f))
			continue;
		if (middle->y < top->y) { swap = top; top = middle; middle = swap; }
		if (bottom->y < middle->y) { swap = middle; middle = bottom; bottom = swap; }
		if (middle->y < top->y) { swap = top; top = middle; middle = swap; }
		y_start = (int)r_round(top->y);
		y_end = (int)r_round(bottom->y);
		if (y_start < y0)
			y_start = y0;
		if (y_end > y1)
			y_end = y1;
		if (y_start < y_end)
			raster8_opaque(p, q, r, top, middle, bottom, determinant, y_start, y_end, affine, texture, level, fogged, x0, x1);
	}
}

/* ---------- triangles */

/* the attributes as an array, for the gradients */
enum
{
	_attribute_w,
	_attribute_s,
	_attribute_t,
	_attribute_r,
	_attribute_g,
	_attribute_b,
	_attribute_fog_r,
	_attribute_fog_g,
	_attribute_fog_b,
	_attribute_light,
	_attribute_fog,
	ATTRIBUTE_COUNT
};

#define ATTRIBUTE(name) (1U << _attribute_##name)
#define ATTRIBUTES_COLOR (ATTRIBUTE(r) | ATTRIBUTE(g) | ATTRIBUTE(b))
#define ATTRIBUTES_FOG_COLOR (ATTRIBUTE(fog_r) | ATTRIBUTE(fog_g) | ATTRIBUTE(fog_b))

static float attribute_of(const struct r_raster_vertex *vertex, int attribute, int affine)
{
	switch (attribute)
	{
	case _attribute_w: return vertex->w;
	case _attribute_s: return affine ? vertex->u : vertex->s;
	case _attribute_t: return affine ? vertex->v : vertex->t;
	case _attribute_r: return vertex->r;
	case _attribute_g: return vertex->g;
	case _attribute_b: return vertex->b;
	case _attribute_fog_r: return vertex->fog_r;
	case _attribute_fog_g: return vertex->fog_g;
	case _attribute_fog_b: return vertex->fog_b;
	case _attribute_light: return vertex->light;
	default: return vertex->fog;
	}
}

/* what the kind of span a triangle takes reads: only those get gradients
(once a triangle) and a start each row */
static unsigned attributes_needed(int screen, int eight, int textured, int fogged)
{
	unsigned needed;

	if (screen)
	{
		/* (the alpha rides in fog_r) */
		return ATTRIBUTES_COLOR | ATTRIBUTE(fog_r) | (textured ? ATTRIBUTE(s) | ATTRIBUTE(t) : 0);
	}
	needed = ATTRIBUTE(w) | (textured ? ATTRIBUTE(s) | ATTRIBUTE(t) : 0);
	if (eight)
	{
		needed |= textured ? ATTRIBUTE(light) : ATTRIBUTES_COLOR;
		if (fogged)
			needed |= ATTRIBUTE(fog);
	}
	else
	{
		needed |= ATTRIBUTES_COLOR;
		if (fogged)
			needed |= ATTRIBUTES_FOG_COLOR;
	}
	return needed;
}

/* a light or fog level's start along a span, and its step, kept inside the
colour maps' range from the first pixel to the last (rounding must not step
out of it) */
static void level_span(float start, long step, int length, int levels, long *value, long *value_step)
{
	long first = fixed(start), last;

	if (first < 0)
		first = 0;
	if (first > ((long)(levels - 1) << 16))
		first = (long)(levels - 1) << 16;
	last = first + step * (length - 1);
	*value = first;
	*value_step = last < 0 || last > ((long)(levels - 1) << 16) ? 0 : step;
}

/* the texture's error in pixels a triangle may have and still be drawn
without perspective: the setting's, else by the detail setting, a quarter
(3) to four (0); eight times that in front (r_set_in_front) */
static float affine_tolerance(void)
{
	static const float tolerance[4] = { 4.0f, 2.0f, 1.0f, 0.25f };
	int detail = r_frame.settings.detail;

	float value = r_frame.settings.affine_tolerance >= 0.0f ? r_frame.settings.affine_tolerance :
		tolerance[detail < 0 ? 0 : detail > 3 ? 3 : detail];

	/* (what is in front, the first-person weapon, moves with the eye: its
	textures cannot swim as it turns, and it takes eight times as much) */
	return depth_bias ? value * 8.0f : value;
}

/* Front ends cull before this: the corners are on the numbers (finite), and
not all off to one side. A triangle is set up once (its attributes' planes,
its edges' steps), then each row's start is the planes' value at its first
pixel, stepped down from the row above: one multiply and add an attribute
a row. */
void r_raster_triangle(const struct r_raster_vertex *a, const struct r_raster_vertex *b,
	const struct r_raster_vertex *c, const struct r_texture *texture, int level, unsigned flags, int x0, int y0,
	int x1, int y1)
{
	const struct r_raster_vertex *top = a, *middle = b, *bottom = c, *swap;
	/* the attributes the span reads, by number */
	int list[ATTRIBUTE_COUNT], listed = 0;
	/* each attribute at x = 0 on the current row's centre, and its steps */
	float row[ATTRIBUTE_COUNT], dx[ATTRIBUTE_COUNT], dy[ATTRIBUTE_COUNT];
	float determinant, inverse, ab_x, ab_y, ac_x, ac_y;
	float long_slope, upper_slope, lower_slope, py, x_long;
	long light_step = 0, fog_step = 0;
	int y_start, y_end;
	struct span span;
	int screen = (flags & R_RASTER_SCREEN) != 0;
	int fogged = (flags & R_RASTER_FOG) != 0;
	int eight = r_frame.bits == 8;
	int affine = 0;
	unsigned needed;

	if (r_backend)
	{
		r_backend->triangle(a, b, c, texture, flags);
		r_statistics.triangles_drawn++;
		return;
	}
	ab_x = b->x - a->x;
	ab_y = b->y - a->y;
	ac_x = c->x - a->x;
	ac_y = c->y - a->y;
	determinant = ab_x * ac_y - ac_x * ab_y;
	/* (no area; or a corner not a number, whose comparisons are all false) */
	if (!(determinant > 1e-6f || determinant < -1e-6f))
		return;

	/* top to bottom; the rows whose centres are inside (a row's first pixel
	centre at or below the top: the top edge counts as inside) */
	if (middle->y < top->y) { swap = top; top = middle; middle = swap; }
	if (bottom->y < middle->y) { swap = middle; middle = bottom; bottom = swap; }
	if (middle->y < top->y) { swap = top; top = middle; middle = swap; }
	y_start = (int)r_round(top->y);
	y_end = (int)r_round(bottom->y);
	if (y_start < y0)
		y_start = y0;
	if (y_end > y1)
		y_end = y1;
	if (y_start >= y_end)
		return;
	r_statistics.rows_scanned += y_end - y_start;

	/* No perspective where it would not show (Quake's way): the texture's
	error across the triangle is about its size on the screen times its
	change in 1/z over the mean; under the tolerance it is drawn affine,
	its texture coordinates straight across the screen, with no division a
	row */
	if (texture && !screen)
	{
		float w_low = a->w, w_high = a->w, x_low = a->x, x_high = a->x, extent;

		if (b->w < w_low) w_low = b->w;
		if (b->w > w_high) w_high = b->w;
		if (c->w < w_low) w_low = c->w;
		if (c->w > w_high) w_high = c->w;
		if (b->x < x_low) x_low = b->x;
		if (b->x > x_high) x_high = b->x;
		if (c->x < x_low) x_low = c->x;
		if (c->x > x_high) x_high = c->x;
		extent = x_high - x_low > bottom->y - top->y ? x_high - x_low : bottom->y - top->y;
		affine = extent * (w_high - w_low) < affine_tolerance() * (w_high + w_low);
		r_statistics.triangles_affine += affine;
	}

	/* the 8-bit picture's opaque triangles: their own rasterizer */
	if (eight && texture && !screen && !(flags & (R_DRAW_ADDITIVE | R_DRAW_BLEND | R_DRAW_NO_DEPTH_TEST)))
	{
		if (a->x < (float)x0 - FAR_X || a->x > (float)x1 + FAR_X || b->x < (float)x0 - FAR_X ||
			b->x > (float)x1 + FAR_X || c->x < (float)x0 - FAR_X || c->x > (float)x1 + FAR_X)
		{
			raster8_far(a, b, c, affine, texture, level, fogged, x0, y0, x1, y1);
			return;
		}
		raster8_opaque(a, b, c, top, middle, bottom, determinant, y_start, y_end, affine, texture, level, fogged, x0, x1);
		return;
	}

	/* the planes of what the span reads, at x = 0 on the first row */
	needed = attributes_needed(screen, eight, texture != NULL, fogged);
	inverse = 1.0f / determinant;
	py = (float)y_start + 0.5f;
	for (int i = 0; i < ATTRIBUTE_COUNT; i++)
	{
		float value0, d1, d2;

		if (!(needed & (1U << i)))
		{
			row[i] = dx[i] = dy[i] = 0.0f;
			continue;
		}
		value0 = attribute_of(a, i, affine);
		d1 = attribute_of(b, i, affine) - value0;
		d2 = attribute_of(c, i, affine) - value0;
		dx[i] = (d1 * ac_y - d2 * ab_y) * inverse;
		dy[i] = (d2 * ab_x - d1 * ac_x) * inverse;
		row[i] = value0 - a->x * dx[i] + (py - a->y) * dy[i];
		list[listed++] = i;
	}

	memset(&span, 0, sizeof(span));
	if (texture)
	{
		int width_shift = texture->width_shift - level, height_shift = texture->height_shift - level;
		float width, height;

		if (width_shift < 0) width_shift = 0;
		if (height_shift < 0) height_shift = 0;
		width = (float)(1 << width_shift);
		height = (float)(1 << height_shift);
		span.texels = texture->texels[level];
		span.width_shift = width_shift;
		span.width_mask = (1UL << width_shift) - 1;
		span.height_mask = (1UL << height_shift) - 1;
		span.palette = texture->palette;
		span.remap = texture->remap;
		span.alpha_test = (texture->flags & R_TEXTURE_ALPHA_TEST) != 0;
		span.affine = affine;
		/* the texture coordinates in this level's texels */
		row[_attribute_s] *= width;
		dx[_attribute_s] *= width;
		dy[_attribute_s] *= width;
		row[_attribute_t] *= height;
		dx[_attribute_t] *= height;
		dy[_attribute_t] *= height;
	}
	if (needed & ATTRIBUTE(w))
		span.dw = dx[_attribute_w];
	if (texture)
	{
		span.ds = dx[_attribute_s];
		span.dt = dx[_attribute_t];
	}
	if (needed & ATTRIBUTES_COLOR)
	{
		span.dr = fixed(dx[_attribute_r]);
		span.dg = fixed(dx[_attribute_g]);
		span.db = fixed(dx[_attribute_b]);
	}
	if (needed & ATTRIBUTE(fog_r))
		span.dfog_r = fixed(dx[_attribute_fog_r]);
	if (needed & ATTRIBUTE(fog_g))
	{
		span.dfog_g = fixed(dx[_attribute_fog_g]);
		span.dfog_b = fixed(dx[_attribute_fog_b]);
	}
	if (needed & ATTRIBUTE(light))
		light_step = fixed(dx[_attribute_light]);
	if (needed & ATTRIBUTE(fog))
		fog_step = fixed(dx[_attribute_fog]);

	/* the edges' steps in x a row: the long one from the top to the bottom,
	the short ones through the middle corner */
	long_slope = (bottom->x - top->x) / (bottom->y - top->y);
	upper_slope = middle->y > top->y ? (middle->x - top->x) / (middle->y - top->y) : 0.0f;
	lower_slope = bottom->y > middle->y ? (bottom->x - middle->x) / (bottom->y - middle->y) : 0.0f;
	x_long = top->x + (py - top->y) * long_slope;

	for (int y = y_start; y < y_end; y++)
	{
		float x_short = py < middle->y ? top->x + (py - top->y) * upper_slope :
			middle->x + (py - middle->y) * lower_slope;
		float left = x_long < x_short ? x_long : x_short, right = x_long < x_short ? x_short : x_long;
		int xs = (int)r_round(left), xe = (int)r_round(right);

		if (xs < x0)
			xs = x0;
		if (xe > x1)
			xe = x1;
		if (xs < xe)
		{
			float fx = (float)xs + 0.5f;

			span.y = y;
			span.x0 = xs;
			span.x1 = xe;
			span.w = row[_attribute_w] + fx * dx[_attribute_w];
			if (texture)
			{
				span.s = row[_attribute_s] + fx * dx[_attribute_s];
				span.t = row[_attribute_t] + fx * dx[_attribute_t];
			}
			if (needed & ATTRIBUTE(light))
			{
				level_span(row[_attribute_light] + fx * dx[_attribute_light], light_step, xe - xs, R_LIGHT_LEVELS,
					&span.light, &span.dlight);
			}
			if (needed & ATTRIBUTE(fog))
			{
				level_span(row[_attribute_fog] + fx * dx[_attribute_fog], fog_step, xe - xs, R_FOG_LEVELS, &span.fog,
					&span.dfog);
			}
			if (needed & ATTRIBUTES_COLOR)
			{
				span.r = fixed(row[_attribute_r] + fx * dx[_attribute_r]);
				span.g = fixed(row[_attribute_g] + fx * dx[_attribute_g]);
				span.b = fixed(row[_attribute_b] + fx * dx[_attribute_b]);
			}
			if (needed & ATTRIBUTE(fog_r))
				span.fog_r = fixed(row[_attribute_fog_r] + fx * dx[_attribute_fog_r]);
			if (needed & ATTRIBUTE(fog_g))
			{
				span.fog_g = fixed(row[_attribute_fog_g] + fx * dx[_attribute_fog_g]);
				span.fog_b = fixed(row[_attribute_fog_b] + fx * dx[_attribute_fog_b]);
			}

			if (screen)
			{
				if (eight)
					span8_screen(&span, texture != NULL);
				else
					span32_screen(&span, texture != NULL);
			}
			else if (eight)
			{
				if (!texture)
					span8_flat(&span, fogged, flags);
				else
					span8_textured(&span, fogged, flags);
			}
			else
			{
				if (!texture)
					span32_flat(&span, fogged, flags);
				else if (!fogged && !span.alpha_test && !(flags & (R_DRAW_ADDITIVE | R_DRAW_BLEND | R_DRAW_NO_DEPTH_TEST)))
					span32_textured_opaque(&span);
				else
					span32_textured(&span, fogged, flags);
			}
		}
		/* down a row */
		py += 1.0f;
		x_long += long_slope;
		for (int k = 0; k < listed; k++)
			row[list[k]] += dy[list[k]];
	}
}
