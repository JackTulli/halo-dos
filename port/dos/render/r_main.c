/*
R_MAIN.C

The renderer's frontend (r_public.h): the picture, the views, culling,
transformation, light and fog at the vertices, clipping at the near plane,
projection, the texture level of each triangle; the rasterizer
(r_internal.h) draws what is left.

A view's space: x to the right, y up, z away from the eye. A point at
view-space (x, y, z) is at pixel (centre_x + x/z * focal, centre_y - y/z *
focal). The rasterizer interpolates 1/z, u/z and v/z across the screen.
*/

#include "r_internal.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

void r_raster_set_depth_scale(float z_near);

struct r_frame r_frame;
struct r_statistics r_statistics;

/* ---------- start-up */

int r_initialize(const struct r_settings *settings)
{
	r_dispose();
	r_frame.settings = *settings;
	r_frame.width = settings->width;
	r_frame.height = settings->height;
	r_frame.bits = settings->bits == 8 ? 8 : 32;
	/* (rows a whole number of 32-bit words apart, as a Windows bitmap's
	rows must be: an 8-bit picture 854 or 1366 wide was shown sheared) */
	r_frame.pitch = (r_frame.width * (r_frame.bits / 8) + 3) & ~3;
	r_frame.pixels = malloc((size_t)r_frame.pitch * r_frame.height);
	r_frame.depth = malloc((size_t)r_frame.width * r_frame.height * sizeof(unsigned long));
	if (!r_frame.pixels || !r_frame.depth)
	{
		r_dispose();
		return 0;
	}
	r_palette_initialize();
	return 1;
}

void r_dispose(void)
{
	free(r_frame.pixels);
	free(r_frame.depth);
	r_frame.pixels = NULL;
	r_frame.depth = NULL;
}

const void *r_frame_pixels(int *width, int *height, int *pitch, int *bits)
{
	*width = r_frame.width;
	*height = r_frame.height;
	*pitch = r_frame.pitch;
	*bits = r_frame.bits;
	return r_frame.pixels;
}

const struct r_statistics *r_frame_statistics(void)
{
	return &r_statistics;
}

/* ---------- frames */

void r_frame_begin(unsigned long clear_color)
{
	memset(&r_statistics, 0, sizeof(r_statistics));
	r_raster_clear(clear_color);
}

void r_frame_end(void)
{
	return;
}

/* the hardware rasterizer in use (r_internal.h), NULL for the software one */
const struct r_backend *r_backend;

int r_hardware_active(void)
{
	return r_backend != NULL;
}

int r_draws_detail(void)
{
	return r_backend && r_backend->draws_detail && r_backend->draws_detail();
}

/* ---------- lightmaps (a card's that draws them: r_backend's lightmap_new) */

/* the lightmap of the triangles being drawn (R_RASTER_LIGHTMAP's), and the
next draw's with its coordinates (r_set_lightmap) */
unsigned long r_lightmap;
static unsigned long next_lightmap;
static const float *next_lightmap_coordinates;

int r_draws_lightmaps(void)
{
	return r_backend && r_backend->lightmap_new;
}

unsigned long r_lightmap_new(const unsigned long *argb, int width, int height)
{
	return r_draws_lightmaps() && argb && width > 0 && height > 0 ? r_backend->lightmap_new(argb, width, height) : 0;
}

void r_lightmap_delete(unsigned long lightmap)
{
	if (lightmap && r_backend && r_backend->lightmap_delete)
		r_backend->lightmap_delete(lightmap);
}

void r_set_lightmap(unsigned long lightmap, const float *coordinates)
{
	next_lightmap = coordinates && r_draws_lightmaps() ? lightmap : 0;
	next_lightmap_coordinates = coordinates;
}

/* ---------- the Xbox's texture pass (r_backend's detail_stages) */

/* the pass of the triangles being drawn (R_RASTER_XBOX_DETAIL's), and the
next draw's (r_set_detail_pass) */
const struct r_detail_pass *r_detail_pass;
static const struct r_detail_pass *next_detail_pass;

int r_draws_xbox_detail(void)
{
	int stages = r_backend && r_backend->detail_stages ? r_backend->detail_stages() : 0;

	return stages >= 3 ? stages : 0;
}

unsigned long r_hardware_texture_new(unsigned long *const *levels, const int *widths, const int *heights, int count,
	int alpha)
{
	return r_draws_xbox_detail() && levels && count > 0 && r_backend->hardware_texture_new ?
		r_backend->hardware_texture_new(levels, widths, heights, count, alpha) : 0;
}

void r_hardware_texture_delete(unsigned long texture)
{
	if (texture && r_backend && r_backend->hardware_texture_delete)
		r_backend->hardware_texture_delete(texture);
}

void r_set_detail_pass(const struct r_detail_pass *pass)
{
	next_detail_pass = r_draws_xbox_detail() ? pass : NULL;
}

void r_hardware_read_back(void)
{
	if (r_backend)
		r_backend->read_back();
}

/* ---------- the current view, and vector arithmetic */

static struct
{
	struct r_view view;
	float right[3], up[3], forward[3];
	/* pixels a unit away, across and up (they differ where pixels are not
	square) */
	float focal, focal_x;
	float centre_x, centre_y;
	/* the half-widths of the view at z = 1, for culling */
	float tangent_x, tangent_y;
	float far;
	int fogged;
	float fog_scale;
} view;

static float dot(const float *a, const float *b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void normalize(float *v)
{
	float length = sqrtf(dot(v, v));

	if (length > 1e-12f)
	{
		v[0] /= length;
		v[1] /= length;
		v[2] /= length;
	}
}

static void cross(const float *a, const float *b, float *out)
{
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

/* ---------- the sky

Behind a view: each of its rows above the horizon takes the sky's colour at
its height, as the N64 draws Halo's sky (port n64_render.c: the dome as it
looks from the horizon up); below, the frame's clear colour, which is the
fog's, so the level fades into it where it ends. The eye's up is the
world's but for a vehicle's tilt, which this leaves out. */

static const float sky_stop_degrees[R_SKY_STOPS] = { 0.0f, 15.0f, 30.0f, 50.0f, 90.0f };

static unsigned long sky_color_at(const unsigned long *colors, float degrees)
{
	unsigned long result = 0;
	int i = 0;
	float f;

	while (i < R_SKY_STOPS - 2 && degrees > sky_stop_degrees[i + 1])
		i++;
	f = (degrees - sky_stop_degrees[i]) / (sky_stop_degrees[i + 1] - sky_stop_degrees[i]);
	if (f < 0.0f)
		f = 0.0f;
	if (f > 1.0f)
		f = 1.0f;
	for (int shift = 0; shift <= 16; shift += 8)
	{
		float a = (float)((colors[i] >> shift) & 0xFF), b = (float)((colors[i + 1] >> shift) & 0xFF);

		result |= (unsigned long)r_round(a + (b - a) * f) << shift;
	}
	return result;
}

static void draw_sky(const unsigned long *colors)
{
	for (int y = view.view.y0; y < view.view.y1; y++)
	{
		/* the row's direction: forward, and up by its distance from the
		view's middle over the focal length */
		float t = (view.centre_y - ((float)y + 0.5f)) / view.focal;
		float sine = (view.forward[2] + view.up[2] * t) / sqrtf(1.0f + t * t);

		if (sine <= 0.0f)
			continue;
		r_raster_fill_row(view.view.x0, view.view.x1, y,
			sky_color_at(colors, asinf(sine > 1.0f ? 1.0f : sine) * 57.2957795f));
	}
}

/* ---------- views */

void r_view_begin(const struct r_view *description)
{
	float height = (float)(description->y1 - description->y0);
	float width = (float)(description->x1 - description->x0);

	view.view = *description;
	memcpy(view.forward, description->forward, sizeof(view.forward));
	normalize(view.forward);
	/* (Halo's axes: x forward, y left, z up: forward x up is the right) */
	cross(view.forward, description->up, view.right);
	normalize(view.right);
	cross(view.right, view.forward, view.up);
	normalize(view.up);
	view.focal = (height * 0.5f) / tanf(description->vertical_field_of_view * 0.5f);
	view.focal_x = description->pixel_aspect > 0.0f ? view.focal / description->pixel_aspect : view.focal;
	view.centre_x = (float)description->x0 + width * 0.5f;
	view.centre_y = (float)description->y0 + height * 0.5f;
	view.tangent_x = width * 0.5f / view.focal_x;
	view.tangent_y = height * 0.5f / view.focal;
	view.far = description->fog_end > 0.0f ? description->fog_end : description->z_far;
	if (view.far <= description->z_near)
		view.far = description->z_near + 1.0f;
	view.fogged = description->fog_end > description->fog_start && description->fog_end > 0.0f;
	view.fog_scale = view.fogged ? 1.0f / (description->fog_end - description->fog_start) : 0.0f;
	if (r_frame.bits == 8 && view.fogged)
		r_palette_set_fog(description->fog_color);
	r_raster_set_depth_scale(description->z_near);
	if (r_backend)
		r_backend->view_fog(view.fogged, description->fog_start, description->fog_end, description->fog_color);
	if (r_backend && r_backend->view_rect)
		r_backend->view_rect(description->x0, description->y0, description->x1, description->y1, 1);
	if (description->sky_colors)
		draw_sky(description->sky_colors);
	r_statistics.views++;
}

void r_view_end(void)
{
	if (r_backend && r_backend->view_rect)
		r_backend->view_rect(0, 0, 0, 0, 0);
}

void r_view_basis(float right[3], float up[3], float ahead[3], float eye[3])
{
	memcpy(right, view.right, sizeof(view.right));
	memcpy(up, view.up, sizeof(view.up));
	memcpy(ahead, view.forward, sizeof(view.forward));
	memcpy(eye, view.view.position, sizeof(view.view.position));
}

int r_sphere_visible(const float center[3], float radius)
{
	float d[3] = { center[0] - view.view.position[0], center[1] - view.view.position[1],
		center[2] - view.view.position[2] };
	float x = dot(d, view.right), y = dot(d, view.up), z = dot(d, view.forward);

	if (z + radius < view.view.z_near || z - radius > view.far)
		return 0;
	/* the side planes: |x| <= z * tangent, pushed out by the radius along
	their normals */
	if (fabsf(x) - z * view.tangent_x > radius * sqrtf(1.0f + view.tangent_x * view.tangent_x))
		return 0;
	if (fabsf(y) - z * view.tangent_y > radius * sqrtf(1.0f + view.tangent_y * view.tangent_y))
		return 0;
	return 1;
}

float r_distance(const float point[3])
{
	float d[3] = { point[0] - view.view.position[0], point[1] - view.view.position[1],
		point[2] - view.view.position[2] };

	return sqrtf(dot(d, d));
}

float r_pixels_per_unit_at(float distance)
{
	return distance > 0.01f ? view.focal / distance : view.focal * 100.0f;
}

/* ---------- triangles */

/* a vertex in view space with what is interpolated */
struct clip_vertex
{
	float x, y, z;
	float u, v;
	float r, g, b;
	float fog_r, fog_g, fog_b;
	float light, fog;
	/* the lightmap's coordinates (R_RASTER_LIGHTMAP) */
	float lightmap_u, lightmap_v;
};

/* a vertex's outcode: where it is outside the view (the near plane, the
fog's end, the sides), and whether it is fogged */
#define CODE_NEAR 0x01
#define CODE_FAR 0x02
#define CODE_LEFT 0x04
#define CODE_RIGHT 0x08
#define CODE_TOP 0x10
#define CODE_BOTTOM 0x20
#define CODES_OUTSIDE 0x3F
#define CODE_FOGGED 0x40

/* a draw's vertices: in view space, projected (those in front of the near
plane), their outcodes */
static struct clip_vertex *transformed;
static struct r_raster_vertex *projected;
static unsigned char *codes;
static int transformed_size;

static int reserve(int count)
{
	if (count > transformed_size)
	{
		struct clip_vertex *grown = realloc(transformed, (size_t)count * sizeof(*grown));
		struct r_raster_vertex *grown_projected;
		unsigned char *grown_codes;

		if (!grown)
			return 0;
		transformed = grown;
		grown_projected = realloc(projected, (size_t)count * sizeof(*grown_projected));
		if (!grown_projected)
			return 0;
		projected = grown_projected;
		grown_codes = realloc(codes, (size_t)count);
		if (!grown_codes)
			return 0;
		codes = grown_codes;
		transformed_size = count;
	}
	return 1;
}

/* the vertex loop of the software 8-bit picture's textured draws
(r_span8.s), and what it reads; the last colour light_vertex worked out a
light level for, and that level, shared with it */
struct r_vertices8_constants r_vertices8 =
{
	.last_color = 0xFFFFFFFFUL,
	.red = 0.30f,
	.green = 0.59f,
	.blue = 0.11f,
	.eighth = 16.0f / 128.0f,
	.fifteen = (float)(R_FOG_LEVELS - 1),
	.one = 1.0f,
};

/* (r_span8.s reads it by offset) */
typedef char r_vertices8_layout_check[offsetof(struct r_vertices8_constants, scratch) == 148 ? 1 : -1];

/* the vertex's light and fog, those the picture's spans read: in 8 bits a
textured triangle's light level and fog level alone (r_soft.c) */
static void light_vertex(struct clip_vertex *out, unsigned long color, unsigned flags, int eight, int textured)
{
	float r = (float)((color >> 16) & 0xFF), g = (float)((color >> 8) & 0xFF), b = (float)(color & 0xFF);
	float fog = 0.0f;

	if (view.fogged && !(flags & R_DRAW_NO_FOG))
	{
		float distance = out->z;

		fog = (distance - view.view.fog_start) * view.fog_scale;
		if (fog < 0.0f) fog = 0.0f;
		if (fog > 1.0f) fog = 1.0f;
	}
	/* (added light, a glow's or water's reflection: faded out by the fog,
	not turned to the fog's colour, which would add that too; as Halo fades
	its added passes) */
	if ((flags & R_DRAW_ADDITIVE) && fog > 0.0f)
	{
		r *= 1.0f - fog;
		g *= 1.0f - fog;
		b *= 1.0f - fog;
		color = ((unsigned long)r << 16) | ((unsigned long)g << 8) | (unsigned long)b;
		fog = 0.0f;
	}
	if (eight && textured)
	{
		/* (its light level alone; the last colour's remembered: without
		lighting every vertex has the same) */
		out->r = out->g = out->b = 0.0f;
		out->fog_r = out->fog_g = out->fog_b = 0.0f;
		if (color != r_vertices8.last_color)
		{
			r_vertices8.last_color = color;
			/* (a whole level: the picture's own steps of light, so that a
			triangle lit evenly, most of a level's, is one light, drawn the
			cheaper way, r_soft.c raster8_opaque) */
			r_vertices8.last_light = (float)r_round((0.30f * r + 0.59f * g + 0.11f * b) * (16.0f / 128.0f));
			if (r_vertices8.last_light > (float)(R_LIGHT_LEVELS - 1))
				r_vertices8.last_light = (float)(R_LIGHT_LEVELS - 1);
		}
		out->light = r_vertices8.last_light;
		out->fog = fog * (float)(R_FOG_LEVELS - 1);
		return;
	}
	else
	{
		out->r = r * (1.0f - fog);
		out->g = g * (1.0f - fog);
		out->b = b * (1.0f - fog);
		out->fog_r = (float)((view.view.fog_color >> 16) & 0xFF) * fog;
		out->fog_g = (float)((view.view.fog_color >> 8) & 0xFF) * fog;
		out->fog_b = (float)(view.view.fog_color & 0xFF) * fog;
	}
	/* the 8-bit picture: the light's brightness in levels of 1/16 (0x80 is
	16), and the fog in 16ths */
	out->light = (0.30f * r + 0.59f * g + 0.11f * b) * (16.0f / 128.0f);
	if (out->light > (float)(R_LIGHT_LEVELS - 1)) out->light = (float)(R_LIGHT_LEVELS - 1);
	out->fog = fog * (float)(R_FOG_LEVELS - 1);
}

static void project(const struct clip_vertex *in, struct r_raster_vertex *out)
{
	float w = 1.0f / in->z;

	out->x = view.centre_x + in->x * w * view.focal_x;
	out->y = view.centre_y - in->y * w * view.focal;
	out->w = w;
	out->s = in->u * w;
	out->t = in->v * w;
	out->u = in->u;
	out->v = in->v;
	out->r = in->r;
	out->g = in->g;
	out->b = in->b;
	out->fog_r = in->fog_r;
	out->fog_g = in->fog_g;
	out->fog_b = in->fog_b;
	out->light = in->light;
	out->fog = in->fog;
	out->lightmap_s = in->lightmap_u * w;
	out->lightmap_t = in->lightmap_v * w;
}

/* the same for the 8-bit picture's textured triangles, whose spans read the
position, w, the texture coordinates, the light and fog levels alone
(r_soft.c attributes_needed) */
static void project8(const struct clip_vertex *in, struct r_raster_vertex *out)
{
	float w = 1.0f / in->z;

	out->x = view.centre_x + in->x * w * view.focal_x;
	out->y = view.centre_y - in->y * w * view.focal;
	out->w = w;
	out->s = in->u * w;
	out->t = in->v * w;
	out->u = in->u;
	out->v = in->v;
	out->light = in->light;
	out->fog = in->fog;
}

static void lerp_vertex(const struct clip_vertex *a, const struct clip_vertex *b, float t, struct clip_vertex *out)
{
	const float *pa = &a->x, *pb = &b->x;
	float *po = &out->x;

	for (int i = 0; i < (int)(sizeof(struct clip_vertex) / sizeof(float)); i++)
		po[i] = pa[i] + (pb[i] - pa[i]) * t;
}

void r_settings_changed(const struct r_settings *settings)
{
	r_frame.settings.detail = settings->detail;
	r_frame.settings.texture_dither = settings->texture_dither;
	r_frame.settings.texture_smooth = settings->texture_smooth;
	r_frame.settings.light_dither = settings->light_dither;
	r_frame.settings.affine_tolerance = settings->affine_tolerance;
}

/* the texture level whose texels are nearest a pixel in size */
static int texture_level(const struct r_texture *texture, const struct clip_vertex *a, const struct clip_vertex *b,
	const struct clip_vertex *c, float screen_area)
{
	float texel_area = fabsf((b->u - a->u) * (c->v - a->v) - (c->u - a->u) * (b->v - a->v)) *
		(float)(1 << texture->width_shift) * (float)(1 << texture->height_shift);
	int level = 0;
	/* (more detail draws a level sharper) */
	float bias = r_frame.settings.detail >= 3 ? 0.5f : r_frame.settings.detail <= 0 ? 2.0f : 1.0f;

	float limit = screen_area * bias * 4.0f;
	float nearest = a->z < b->z ? (a->z < c->z ? a->z : c->z) : (b->z < c->z ? b->z : c->z);
	float middle = (a->z + b->z + c->z) * (1.0f / 3.0f);
	union
	{
		float value;
		unsigned long bits;
	} ratio;
	int exponent;

	if (screen_area < 0.5f)
		return texture->levels - 1;
	/* (texels a pixel grow with the square of the depth: a big triangle
	seen edge on, the ground's, takes the level its nearest corner needs,
	not its whole area's, which its far end makes coarse) */
	if (nearest > 0.0f && middle > nearest)
		ratio.value = texel_area * nearest * nearest / (limit * middle * middle);
	else
		ratio.value = texel_area / limit;
	/* (texels a pixel over 4, each level a quarter as many: the first level
	at which the ratio, a quarter a level, is at most 1, read off its
	exponent -- what a loop of quarterings found, a division and no loop) */
	if (!(ratio.value > 1.0f))
		return 0;
	exponent = (int)((ratio.bits >> 23) & 0xFF) - 127;
	level = (ratio.bits & 0x7FFFFF) ? (exponent >> 1) + 1 : (exponent + 1) >> 1;
	return level < texture->levels - 1 ? level : texture->levels - 1;
}

static void draw_polygon(struct clip_vertex *polygon, int count, const struct r_texture *texture, unsigned flags)
{
	struct r_raster_vertex projected[8];
	unsigned raster_flags = flags;
	float area;
	int level = 0;

	for (int i = 0; i < count; i++)
	{
		project(&polygon[i], &projected[i]);
		if (polygon[i].fog_r > 0.0f || polygon[i].fog_g > 0.0f || polygon[i].fog_b > 0.0f || polygon[i].fog > 0.0f)
			raster_flags |= R_RASTER_FOG;
	}
	/* (Halo's front faces wind clockwise on the screen: y down, the area is
	positive) */
	area = (projected[1].x - projected[0].x) * (projected[2].y - projected[0].y) -
		(projected[2].x - projected[0].x) * (projected[1].y - projected[0].y);
	if (!(flags & R_DRAW_TWO_SIDED) && area <= 0.0f)
	{
		r_statistics.triangles_culled++;
		return;
	}
	/* (the software rasterizer's choice: a card picks its own level) */
	if (texture && !r_backend)
		level = texture_level(texture, &polygon[0], &polygon[1], &polygon[2], fabsf(area) * 0.5f);
	for (int i = 1; i + 1 < count; i++)
	{
		r_raster_triangle(&projected[0], &projected[i], &projected[i + 1], texture, level, raster_flags,
			view.view.x0, view.view.y0, view.view.x1, view.view.y1);
	}
	r_statistics.triangles_drawn++;
}

static void draw_triangle(const struct clip_vertex *a, const struct clip_vertex *b, const struct clip_vertex *c,
	const struct r_texture *texture, unsigned flags)
{
	const struct clip_vertex *in[3] = { a, b, c };
	struct clip_vertex polygon[4];
	int count = 0;
	float near = view.view.z_near;

	if (a->z >= near && b->z >= near && c->z >= near)
	{
		polygon[0] = *a;
		polygon[1] = *b;
		polygon[2] = *c;
		draw_polygon(polygon, 3, texture, flags);
		return;
	}
	/* clipped at the near plane: each edge kept where it is in front */
	for (int i = 0; i < 3; i++)
	{
		const struct clip_vertex *p = in[i], *q = in[(i + 1) % 3];
		int p_in = p->z >= near, q_in = q->z >= near;

		if (p_in)
			polygon[count++] = *p;
		if (p_in != q_in)
			lerp_vertex(p, q, (near - p->z) / (q->z - p->z), &polygon[count++]);
	}
	if (count >= 3)
		draw_polygon(polygon, count, texture, flags);
}

/* r_draw_triangles for the software rasterizer's 8-bit picture's textured
draws but added light's (whose fog fades its colour first): the same, its
vertex loop r_span8.s's (r_vertices8_draw: the C loop's operations, in
their order and precision, in a fraction of the instructions). Copies, the
clipping r_draw_triangles inlines too, so that it (every other draw's, and
a card's) is the code it was: the x87 keeps more than a float's precision
between operations, and where a compiler keeps or stores a value shows in
the last bits */
static void project_8(const struct clip_vertex *in, struct r_raster_vertex *out)
{
	float w = 1.0f / in->z;

	out->x = view.centre_x + in->x * w * view.focal_x;
	out->y = view.centre_y - in->y * w * view.focal;
	out->w = w;
	out->s = in->u * w;
	out->t = in->v * w;
	out->u = in->u;
	out->v = in->v;
	out->r = in->r;
	out->g = in->g;
	out->b = in->b;
	out->fog_r = in->fog_r;
	out->fog_g = in->fog_g;
	out->fog_b = in->fog_b;
	out->light = in->light;
	out->fog = in->fog;
	out->lightmap_s = in->lightmap_u * w;
	out->lightmap_t = in->lightmap_v * w;
}
static void lerp_vertex_8(const struct clip_vertex *a, const struct clip_vertex *b, float t, struct clip_vertex *out)
{
	const float *pa = &a->x, *pb = &b->x;
	float *po = &out->x;

	for (int i = 0; i < (int)(sizeof(struct clip_vertex) / sizeof(float)); i++)
		po[i] = pa[i] + (pb[i] - pa[i]) * t;
}
static void draw_polygon_8(struct clip_vertex *polygon, int count, const struct r_texture *texture, unsigned flags)
{
	struct r_raster_vertex projected[8];
	unsigned raster_flags = flags;
	float area;
	int level = 0;

	for (int i = 0; i < count; i++)
	{
		project_8(&polygon[i], &projected[i]);
		if (polygon[i].fog_r > 0.0f || polygon[i].fog_g > 0.0f || polygon[i].fog_b > 0.0f || polygon[i].fog > 0.0f)
			raster_flags |= R_RASTER_FOG;
	}
	/* (Halo's front faces wind clockwise on the screen: y down, the area is
	positive) */
	area = (projected[1].x - projected[0].x) * (projected[2].y - projected[0].y) -
		(projected[2].x - projected[0].x) * (projected[1].y - projected[0].y);
	if (!(flags & R_DRAW_TWO_SIDED) && area <= 0.0f)
	{
		r_statistics.triangles_culled++;
		return;
	}
	if (texture)
		level = texture_level(texture, &polygon[0], &polygon[1], &polygon[2], fabsf(area) * 0.5f);
	for (int i = 1; i + 1 < count; i++)
	{
		r_raster_triangle(&projected[0], &projected[i], &projected[i + 1], texture, level, raster_flags,
			view.view.x0, view.view.y0, view.view.x1, view.view.y1);
	}
	r_statistics.triangles_drawn++;
}
static void draw_triangle_8(const struct clip_vertex *a, const struct clip_vertex *b, const struct clip_vertex *c,
	const struct r_texture *texture, unsigned flags)
{
	const struct clip_vertex *in[3] = { a, b, c };
	struct clip_vertex polygon[4];
	int count = 0;
	float near = view.view.z_near;

	if (a->z >= near && b->z >= near && c->z >= near)
	{
		polygon[0] = *a;
		polygon[1] = *b;
		polygon[2] = *c;
		draw_polygon_8(polygon, 3, texture, flags);
		return;
	}
	/* clipped at the near plane: each edge kept where it is in front */
	for (int i = 0; i < 3; i++)
	{
		const struct clip_vertex *p = in[i], *q = in[(i + 1) % 3];
		int p_in = p->z >= near, q_in = q->z >= near;

		if (p_in)
			polygon[count++] = *p;
		if (p_in != q_in)
			lerp_vertex_8(p, q, (near - p->z) / (q->z - p->z), &polygon[count++]);
	}
	if (count >= 3)
		draw_polygon_8(polygon, count, texture, flags);
}
static __attribute__((noinline)) void draw_triangles8(const struct r_vertex *vertices, int vertex_count,
	const unsigned short *indices, int triangle_count, const float *matrix, r_texture *texture, unsigned flags)
{
	float near = view.view.z_near, far = view.far;
	int two_sided = (flags & R_DRAW_TWO_SIDED) != 0;
	/* view space = turn * position + offset: the object's matrix (forward,
	left, up, then the position) and the view's axes in one */
	float turn[3][3], offset[3];

	r_lightmap = next_lightmap;
	next_lightmap = 0;
	next_lightmap_coordinates = NULL;
	if (r_lightmap && texture)
		flags |= R_RASTER_LIGHTMAP;
	/* (the Xbox's texture pass, r_set_detail_pass's: this draw's alone) */
	r_detail_pass = next_detail_pass;
	next_detail_pass = NULL;
	if (r_detail_pass && texture)
		flags |= R_RASTER_XBOX_DETAIL;
	if (vertex_count <= 0 || triangle_count <= 0 || !reserve(vertex_count))
		return;
	if ((flags & R_DRAW_DETAIL) && !r_draws_detail())
		return;
	if (!(flags & R_DRAW_VIEW_SPACE))
	{
		const float *axes[3] = { view.right, view.up, view.forward };

		for (int k = 0; k < 3; k++)
		{
			float origin[3];

			for (int j = 0; j < 3; j++)
				origin[j] = (matrix ? matrix[9 + j] : 0.0f) - view.view.position[j];
			for (int i = 0; i < 3; i++)
			{
				turn[k][i] = matrix ? axes[k][0] * matrix[i * 3] + axes[k][1] * matrix[i * 3 + 1] +
					axes[k][2] * matrix[i * 3 + 2] : axes[k][i];
			}
			offset[k] = dot(axes[k], origin);
		}
	}
	/* each vertex once, by r_span8.s: into view space, lit and fogged, its
	outcode, and projected if it is in front of the near plane */
	if (!(flags & R_DRAW_VIEW_SPACE))
	{
		memcpy(r_vertices8.turn, turn, sizeof(turn));
		memcpy(r_vertices8.offset, offset, sizeof(offset));
	}
	r_vertices8.fog_start = view.view.fog_start;
	r_vertices8.fog_scale = view.fog_scale;
	r_vertices8.z_near = near;
	r_vertices8.far = far;
	r_vertices8.tangent_x = view.tangent_x;
	r_vertices8.tangent_y = view.tangent_y;
	r_vertices8.focal_x = view.focal_x;
	r_vertices8.focal = view.focal;
	r_vertices8.centre_x = view.centre_x;
	r_vertices8.centre_y = view.centre_y;
	r_vertices8.vertices = vertices;
	r_vertices8.transformed = transformed;
	r_vertices8.projected = projected;
	r_vertices8.codes = codes;
	r_vertices8.count = vertex_count;
	r_vertices8.view_space = (flags & R_DRAW_VIEW_SPACE) != 0;
	r_vertices8.fogged = view.fogged && !(flags & R_DRAW_NO_FOG);
	r_vertices8_draw();
	for (int i = 0; i < triangle_count; i++)
	{
		int ia = indices[i * 3 + 0], ib = indices[i * 3 + 1], ic = indices[i * 3 + 2];
		unsigned ca = codes[ia], cb = codes[ib], cc = codes[ic];
		const struct r_raster_vertex *pa, *pb, *pc;
		float area;
		int level = 0;

		/* behind the eye, past the fog's end, or off one side of the view */
		if (ca & cb & cc & CODES_OUTSIDE)
		{
			r_statistics.triangles_culled++;
			continue;
		}
		if ((ca | cb | cc) & CODE_NEAR)
		{
			/* (crossing the near plane: clipped, then projected) */
			draw_triangle_8(&transformed[ia], &transformed[ib], &transformed[ic], texture, flags);
			continue;
		}
		pa = &projected[ia];
		pb = &projected[ib];
		pc = &projected[ic];
		/* (Halo's front faces wind clockwise on the screen: y down, the area is
		positive) */
		area = (pb->x - pa->x) * (pc->y - pa->y) - (pc->x - pa->x) * (pb->y - pa->y);
		if (!two_sided && area <= 0.0f)
		{
			r_statistics.triangles_culled++;
			continue;
		}
		if (texture && !r_backend)
			level = texture_level(texture, &transformed[ia], &transformed[ib], &transformed[ic], fabsf(area) * 0.5f);
		r_raster_triangle(pa, pb, pc, texture, level, flags | ((ca | cb | cc) & CODE_FOGGED ? R_RASTER_FOG : 0),
			view.view.x0, view.view.y0, view.view.x1, view.view.y1);
		r_statistics.triangles_drawn++;
	}
}

void r_draw_triangles(const struct r_vertex *vertices, int vertex_count, const unsigned short *indices,
	int triangle_count, const float *matrix, r_texture *texture, unsigned flags)
{
	if (!r_backend && r_frame.bits == 8 && texture && !(flags & R_DRAW_ADDITIVE))
	{
		draw_triangles8(vertices, vertex_count, indices, triangle_count, matrix, texture, flags);
		return;
	}
	float near = view.view.z_near, far = view.far;
	int two_sided = (flags & R_DRAW_TWO_SIDED) != 0;
	int eight = r_frame.bits == 8;
	int eight_textured = eight && texture != NULL;
	/* view space = turn * position + offset: the object's matrix (forward,
	left, up, then the position) and the view's axes in one */
	float turn[3][3], offset[3];
	/* (its lightmap, r_set_lightmap's: this draw's alone) */
	const float *lightmap_coordinates = next_lightmap_coordinates;

	r_lightmap = next_lightmap;
	next_lightmap = 0;
	next_lightmap_coordinates = NULL;
	if (r_lightmap && texture)
		flags |= R_RASTER_LIGHTMAP;
	/* (the Xbox's texture pass, r_set_detail_pass's: this draw's alone) */
	r_detail_pass = next_detail_pass;
	next_detail_pass = NULL;
	if (r_detail_pass && texture)
		flags |= R_RASTER_XBOX_DETAIL;
	if (vertex_count <= 0 || triangle_count <= 0 || !reserve(vertex_count))
		return;
	if ((flags & R_DRAW_DETAIL) && !r_draws_detail())
		return;
	if (!(flags & R_DRAW_VIEW_SPACE))
	{
		const float *axes[3] = { view.right, view.up, view.forward };

		for (int k = 0; k < 3; k++)
		{
			float origin[3];

			for (int j = 0; j < 3; j++)
				origin[j] = (matrix ? matrix[9 + j] : 0.0f) - view.view.position[j];
			for (int i = 0; i < 3; i++)
			{
				turn[k][i] = matrix ? axes[k][0] * matrix[i * 3] + axes[k][1] * matrix[i * 3 + 1] +
					axes[k][2] * matrix[i * 3 + 2] : axes[k][i];
			}
			offset[k] = dot(axes[k], origin);
		}
	}
	/* each vertex once: into view space, lit and fogged, its outcode, and
	projected if it is in front of the near plane */
	for (int i = 0; i < vertex_count; i++)
	{
		const float *p = vertices[i].position;
		struct clip_vertex *out = &transformed[i];
		unsigned code = 0;

		if (flags & R_DRAW_VIEW_SPACE)
		{
			out->x = p[0];
			out->y = p[1];
			out->z = p[2];
		}
		else
		{
			out->x = turn[0][0] * p[0] + turn[0][1] * p[1] + turn[0][2] * p[2] + offset[0];
			out->y = turn[1][0] * p[0] + turn[1][1] * p[1] + turn[1][2] * p[2] + offset[1];
			out->z = turn[2][0] * p[0] + turn[2][1] * p[1] + turn[2][2] * p[2] + offset[2];
		}
		out->u = vertices[i].u;
		out->v = vertices[i].v;
		if (flags & R_RASTER_LIGHTMAP)
		{
			out->lightmap_u = lightmap_coordinates[i * 2];
			out->lightmap_v = lightmap_coordinates[i * 2 + 1];
		}
		else
		{
			out->lightmap_u = out->lightmap_v = 0.0f;
		}
		light_vertex(out, vertices[i].color, flags, eight, texture != NULL);
		if (out->z < near)
			code |= CODE_NEAR;
		if (out->z > far)
			code |= CODE_FAR;
		if (out->x > out->z * view.tangent_x)
			code |= CODE_RIGHT;
		if (out->x < -out->z * view.tangent_x)
			code |= CODE_LEFT;
		if (out->y > out->z * view.tangent_y)
			code |= CODE_TOP;
		if (out->y < -out->z * view.tangent_y)
			code |= CODE_BOTTOM;
		if (out->fog > 0.0f)
			code |= CODE_FOGGED;
		if (!(code & CODE_NEAR))
		{
			if (eight_textured)
				project8(out, &projected[i]);
			else
				project(out, &projected[i]);
		}
		codes[i] = (unsigned char)code;
	}
	for (int i = 0; i < triangle_count; i++)
	{
		int ia = indices[i * 3 + 0], ib = indices[i * 3 + 1], ic = indices[i * 3 + 2];
		unsigned ca = codes[ia], cb = codes[ib], cc = codes[ic];
		const struct r_raster_vertex *pa, *pb, *pc;
		float area;
		int level = 0;

		/* behind the eye, past the fog's end, or off one side of the view */
		if (ca & cb & cc & CODES_OUTSIDE)
		{
			r_statistics.triangles_culled++;
			continue;
		}
		if ((ca | cb | cc) & CODE_NEAR)
		{
			/* (crossing the near plane: clipped, then projected) */
			draw_triangle(&transformed[ia], &transformed[ib], &transformed[ic], texture, flags);
			continue;
		}
		pa = &projected[ia];
		pb = &projected[ib];
		pc = &projected[ic];
		/* (Halo's front faces wind clockwise on the screen: y down, the area is
		positive) */
		area = (pb->x - pa->x) * (pc->y - pa->y) - (pc->x - pa->x) * (pb->y - pa->y);
		if (!two_sided && area <= 0.0f)
		{
			r_statistics.triangles_culled++;
			continue;
		}
		/* (the software rasterizer's choice: a card picks its own level) */
		if (texture && !r_backend)
			level = texture_level(texture, &transformed[ia], &transformed[ib], &transformed[ic], fabsf(area) * 0.5f);
		r_raster_triangle(pa, pb, pc, texture, level, flags | ((ca | cb | cc) & CODE_FOGGED ? R_RASTER_FOG : 0),
			view.view.x0, view.view.y0, view.view.x1, view.view.y1);
		r_statistics.triangles_drawn++;
	}
}

/* a square facing the eye, of one colour (0x00RRGGBB as it is), radius world
units from its middle to each side: a distant object's dot */
void r_draw_billboard(const float center[3], float radius, unsigned long color)
{
	static const unsigned short indices[6] = { 0, 1, 2, 0, 2, 3 };
	static const float corners[4][2] = { { -1.0f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, -1.0f }, { -1.0f, -1.0f } };
	struct r_vertex vertices[4];

	for (int k = 0; k < 4; k++)
	{
		for (int i = 0; i < 3; i++)
		{
			vertices[k].position[i] = center[i] + (view.right[i] * corners[k][0] + view.up[i] * corners[k][1]) * radius;
		}
		vertices[k].u = vertices[k].v = 0.0f;
		vertices[k].color = color;
	}
	r_draw_triangles(vertices, 4, indices, 2, NULL, NULL, R_DRAW_TWO_SIDED);
}

void r_view_set_near(float z_near)
{
	if (z_near <= 0.0f)
		return;
	view.view.z_near = z_near;
	r_raster_set_depth_scale(z_near);
}

void r_set_in_front(int in_front)
{
	r_raster_set_depth_bias(in_front ? 0x80000000UL : 0);
}

void r_set_alpha_scale(float scale)
{
	r_raster_set_alpha_scale(scale <= 0.0f ? 0 : scale >= 1.0f ? 256 : (unsigned long)(scale * 256.0f));
}

/* ---------- the screen */

void r_draw_screen_quad(const struct r_screen_vertex corners[4], r_texture *texture, enum r_screen_blend blend)
{
	struct r_raster_vertex v[4];

	/* (a rectangle, on the screen and in its texture, of one colour: the
	HUD's, filled without triangles) */
	if (corners[0].y == corners[1].y && corners[2].y == corners[3].y && corners[0].x == corners[3].x &&
		corners[1].x == corners[2].x && corners[0].v == corners[1].v && corners[2].v == corners[3].v &&
		corners[0].u == corners[3].u && corners[1].u == corners[2].u && corners[0].color == corners[1].color &&
		corners[0].color == corners[2].color && corners[0].color == corners[3].color &&
		corners[0].x < corners[1].x && corners[0].y < corners[3].y)
	{
		r_raster_screen_blend(blend);
		r_raster_screen_rect(corners[0].x, corners[0].y, corners[1].x, corners[2].y, corners[0].u, corners[0].v,
			corners[1].u, corners[2].v, texture, corners[0].color);
		return;
	}
	for (int i = 0; i < 4; i++)
	{
		unsigned long color = corners[i].color;

		memset(&v[i], 0, sizeof(v[i]));
		v[i].x = corners[i].x;
		v[i].y = corners[i].y;
		v[i].w = 1.0f;
		/* (0 to 1 across the texture: the rasterizer scales them to its
		texels) */
		v[i].s = v[i].u = corners[i].u;
		v[i].t = v[i].v = corners[i].v;
		v[i].r = (float)((color >> 16) & 0xFF);
		v[i].g = (float)((color >> 8) & 0xFF);
		v[i].b = (float)(color & 0xFF);
		/* (the screen's spans read the alpha here) */
		v[i].fog_r = (float)(color >> 24);
	}
	r_raster_screen_blend(blend);
	r_raster_triangle(&v[0], &v[1], &v[2], texture, 0, R_RASTER_SCREEN | R_RASTER_AFFINE, 0, 0, r_frame.width,
		r_frame.height);
	r_raster_triangle(&v[0], &v[2], &v[3], texture, 0, R_RASTER_SCREEN | R_RASTER_AFFINE, 0, 0, r_frame.width,
		r_frame.height);
}

void r_clear_depth(int x0, int y0, int x1, int y1)
{
	if (r_backend)
	{
		r_backend->clear_depth(x0, y0, x1, y1);
		return;
	}
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > r_frame.width) x1 = r_frame.width;
	if (y1 > r_frame.height) y1 = r_frame.height;
	for (int y = y0; y < y1; y++)
		memset(r_frame.depth + (size_t)y * r_frame.width + x0, 0, (size_t)(x1 - x0) * sizeof(unsigned long));
}

/* ---------- text: glyphs' coverage at the picture's size, kept

Text is drawn glyph by glyph from its font's coverage (r_draw_coverage),
the same few dozen glyphs every frame. Each is filtered to the picture's
scale once (the texels under each pixel averaged, those it covers in part
weighed by how much; text drawn smaller than its font made a touch bolder,
or its thin strokes fade), at each half-pixel offset it is drawn at, and
kept until r_coverage_cache_flush (a map's fonts go with it) or until the
cache fills: a frame only blends what was kept. */

#define COVERAGE_SLOTS 1024
#define COVERAGE_PROBES 8

static struct coverage_slot
{
	const unsigned char *source;
	int width, height, stride;
	float scale_x, scale_y;
	int phase; /* the glyph's corner a half pixel right (1) or down (2) */
	unsigned char *cover;
	int cover_width, cover_height;
	/* its texture in the hardware rasterizer, 0 until drawn there */
	unsigned long hardware;
} coverage_slots[COVERAGE_SLOTS];
static int coverage_used;

void r_coverage_cache_flush(void)
{
	for (int i = 0; i < COVERAGE_SLOTS; i++)
	{
		free(coverage_slots[i].cover);
		if (r_backend && coverage_slots[i].hardware)
			r_backend->coverage_forget(coverage_slots[i].hardware);
	}
	memset(coverage_slots, 0, sizeof(coverage_slots));
	coverage_used = 0;
}

static void coverage_filter(struct coverage_slot *slot)
{
	float offset_x = slot->phase & 1 ? 0.5f : 0.0f, offset_y = slot->phase & 2 ? 0.5f : 0.0f;
	float step_x = 1.0f / slot->scale_x, step_y = 1.0f / slot->scale_y;
	float area = step_x * step_y;
	float boost = slot->scale_y < 0.95f ? 1.0f + 0.6f * (0.95f - slot->scale_y) : 1.0f;

	for (int py = 0; py < slot->cover_height; py++)
	{
		float fy0 = ((float)py - offset_y) * step_y, fy1 = fy0 + step_y;
		int sy0, sy1;

		if (fy0 < 0.0f) fy0 = 0.0f;
		if (fy1 > (float)slot->height) fy1 = (float)slot->height;
		sy0 = (int)fy0;
		sy1 = (int)ceilf(fy1);
		for (int px = 0; px < slot->cover_width; px++)
		{
			float fx0 = ((float)px - offset_x) * step_x, fx1 = fx0 + step_x, sum = 0.0f;
			int sx0, sx1;

			if (fx0 < 0.0f) fx0 = 0.0f;
			if (fx1 > (float)slot->width) fx1 = (float)slot->width;
			if (fx1 > fx0 && fy1 > fy0)
			{
				sx0 = (int)fx0;
				sx1 = (int)ceilf(fx1);
				for (int sy = sy0; sy < sy1; sy++)
				{
					float wy = ((float)(sy + 1) < fy1 ? (float)(sy + 1) : fy1) - ((float)sy > fy0 ? (float)sy : fy0);
					const unsigned char *row = slot->source + sy * slot->stride;
					float line = 0.0f;

					for (int sx = sx0; sx < sx1; sx++)
					{
						float wx = ((float)(sx + 1) < fx1 ? (float)(sx + 1) : fx1) - ((float)sx > fx0 ? (float)sx : fx0);

						line += wx * (float)row[sx];
					}
					sum += wy * line;
				}
				sum *= boost / area;
			}
			slot->cover[py * slot->cover_width + px] = (unsigned char)(sum < 8.0f ? 0.0f : sum > 255.0f ? 255.0f : sum);
		}
	}
}

static const struct coverage_slot *coverage_get(const unsigned char *coverage, int width, int height, int stride,
	float scale_x, float scale_y, int phase)
{
	unsigned long hash = (unsigned long)(size_t)coverage * 2654435761UL ^ (unsigned long)width * 73856093UL ^
		(unsigned long)height * 19349663UL ^ (unsigned long)(scale_y * 1024.0f) * 83492791UL ^ (unsigned long)phase;
	struct coverage_slot *slot = NULL;

	for (int probe = 0; probe < COVERAGE_PROBES; probe++)
	{
		struct coverage_slot *candidate = &coverage_slots[(hash + (unsigned long)probe) & (COVERAGE_SLOTS - 1)];

		if (!candidate->cover)
		{
			if (!slot)
				slot = candidate;
			break;
		}
		if (candidate->source == coverage && candidate->width == width && candidate->height == height &&
			candidate->stride == stride && candidate->scale_x == scale_x && candidate->scale_y == scale_y &&
			candidate->phase == phase)
		{
			return candidate;
		}
	}
	/* (full: started again) */
	if (!slot || coverage_used >= COVERAGE_SLOTS * 3 / 4)
	{
		r_coverage_cache_flush();
		slot = &coverage_slots[hash & (COVERAGE_SLOTS - 1)];
	}
	slot->cover_width = (int)ceilf((float)width * scale_x + 0.5f);
	slot->cover_height = (int)ceilf((float)height * scale_y + 0.5f);
	slot->cover = malloc((size_t)slot->cover_width * slot->cover_height);
	if (!slot->cover)
		return NULL;
	slot->source = coverage;
	slot->width = width;
	slot->height = height;
	slot->stride = stride;
	slot->scale_x = scale_x;
	slot->scale_y = scale_y;
	slot->phase = phase;
	coverage_filter(slot);
	coverage_used++;
	return slot;
}

void r_draw_coverage(float x, float y, const unsigned char *coverage, int width, int height, int stride,
	float scale_x, float scale_y, unsigned long argb, int x0, int y0, int x1, int y1)
{
	/* (the glyph's corner on a whole or half pixel) */
	int half_x = (int)floorf(x * 2.0f + 0.5f), half_y = (int)floorf(y * 2.0f + 0.5f);
	int corner_x = half_x >> 1, corner_y = half_y >> 1;
	const struct coverage_slot *slot = coverage_get(coverage, width, height, stride, scale_x, scale_y,
		(half_x & 1) | ((half_y & 1) << 1));
	unsigned long alpha = argb >> 24;
	unsigned char index = r_frame.bits == 8 ? r_palette_nearest(argb) : 0;
	const unsigned char *blend = r_frame.bits == 8 ? r_palette_blend_table() : NULL;
	int left, top, right, bottom;

	if (!slot)
		return;
	if (r_backend)
	{
		r_backend->coverage(corner_x, corner_y, slot->cover, slot->cover_width, slot->cover_height, argb,
			&((struct coverage_slot *)slot)->hardware, x0, y0, x1, y1);
		return;
	}
	left = corner_x > x0 ? corner_x : x0;
	top = corner_y > y0 ? corner_y : y0;
	right = corner_x + slot->cover_width < x1 ? corner_x + slot->cover_width : x1;
	bottom = corner_y + slot->cover_height < y1 ? corner_y + slot->cover_height : y1;
	if (left < 0) left = 0;
	if (top < 0) top = 0;
	if (right > r_frame.width) right = r_frame.width;
	if (bottom > r_frame.height) bottom = r_frame.height;
	for (int py = top; py < bottom; py++)
	{
		const unsigned char *cover_row = slot->cover + (py - corner_y) * slot->cover_width - corner_x;

		if (r_frame.bits == 8)
		{
			/* (256 colours: the text's colour, or half of it over what is
			under it at its edges) */
			unsigned char *row = r_frame.pixels + (size_t)py * r_frame.pitch;

			for (int px = left; px < right; px++)
			{
				unsigned long cover = (cover_row[px] * alpha) >> 8;

				if (cover >= 150)
					row[px] = index;
				else if (cover >= 60 && blend)
					row[px] = blend[index * 256 + row[px]];
			}
		}
		else
		{
			unsigned long *row = (unsigned long *)(r_frame.pixels + (size_t)py * r_frame.pitch);

			for (int px = left; px < right; px++)
			{
				unsigned long cover = (cover_row[px] * alpha) >> 8, under;

				if (!cover)
					continue;
				under = row[px];
				row[px] = (((((argb >> 16) & 0xFF) * cover + ((under >> 16) & 0xFF) * (255 - cover)) >> 8) << 16) |
					(((((argb >> 8) & 0xFF) * cover + ((under >> 8) & 0xFF) * (255 - cover)) >> 8) << 8) |
					(((argb & 0xFF) * cover + (under & 0xFF) * (255 - cover)) >> 8);
			}
		}
	}
}

/* a picture of argb texels (width x height, alpha over) at (x, y) as it is:
already at the picture's size */
void r_draw_picture(int x, int y, const unsigned long *argb, int width, int height)
{
	const unsigned char *blend = r_frame.bits == 8 ? r_palette_blend_table() : NULL;
	int left = x < 0 ? 0 : x, top = y < 0 ? 0 : y;
	int right = x + width < r_frame.width ? x + width : r_frame.width;
	int bottom = y + height < r_frame.height ? y + height : r_frame.height;

	if (r_backend)
	{
		r_backend->picture(x, y, argb, width, height);
		return;
	}
	for (int py = top; py < bottom; py++)
	{
		const unsigned long *source = argb + (size_t)(py - y) * width - x;

		if (r_frame.bits == 8)
		{
			unsigned char *row = r_frame.pixels + (size_t)py * r_frame.pitch;

			for (int px = left; px < right; px++)
			{
				unsigned long texel = source[px], alpha = texel >> 24;

				if (alpha >= 160)
					row[px] = r_palette_nearest(texel);
				else if (alpha >= 64 && blend)
					row[px] = blend[r_palette_nearest(texel) * 256 + row[px]];
			}
		}
		else
		{
			unsigned long *row = (unsigned long *)(r_frame.pixels + (size_t)py * r_frame.pitch);

			for (int px = left; px < right; px++)
			{
				unsigned long texel = source[px], alpha = texel >> 24, under;

				if (!alpha)
					continue;
				under = row[px];
				row[px] = (((((texel >> 16) & 0xFF) * alpha + ((under >> 16) & 0xFF) * (255 - alpha)) >> 8) << 16) |
					(((((texel >> 8) & 0xFF) * alpha + ((under >> 8) & 0xFF) * (255 - alpha)) >> 8) << 8) |
					(((texel & 0xFF) * alpha + (under & 0xFF) * (255 - alpha)) >> 8);
			}
		}
	}
}

void r_fill_rectangle(int x0, int y0, int x1, int y1, unsigned long argb)
{
	unsigned long alpha = argb >> 24;

	if (r_backend)
	{
		r_backend->fill_rectangle(x0, y0, x1, y1, argb);
		return;
	}
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > r_frame.width) x1 = r_frame.width;
	if (y1 > r_frame.height) y1 = r_frame.height;
	/* (256 colours: none under a quarter's alpha, as the 8-bit spans'
	blends; a faint shade, or one fading in from nothing, was half dark) */
	if (r_frame.bits == 8 && alpha < 64)
		return;
	for (int y = y0; y < y1; y++)
	{
		if (r_frame.bits == 8)
		{
			unsigned char *row = r_frame.pixels + (size_t)y * r_frame.pitch;
			unsigned char index = r_palette_nearest(argb);
			const unsigned char *blend = alpha < 255 ? r_palette_blend_table() : NULL;

			for (int x = x0; x < x1; x++)
				row[x] = blend && alpha < 192 ? blend[index * 256 + row[x]] : index;
		}
		else
		{
			unsigned long *row = (unsigned long *)(r_frame.pixels + (size_t)y * r_frame.pitch);

			for (int x = x0; x < x1; x++)
			{
				unsigned long under = row[x];
				unsigned long red = (((argb >> 16) & 0xFF) * alpha + ((under >> 16) & 0xFF) * (255 - alpha)) >> 8;
				unsigned long green = (((argb >> 8) & 0xFF) * alpha + ((under >> 8) & 0xFF) * (255 - alpha)) >> 8;
				unsigned long blue = ((argb & 0xFF) * alpha + (under & 0xFF) * (255 - alpha)) >> 8;

				row[x] = (red << 16) | (green << 8) | blue;
			}
		}
	}
}
