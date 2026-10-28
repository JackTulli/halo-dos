/*
T_FIXED_DEVICE.C

The light renderer (port/linux/src/xgpu_fixed.c, display.renderer "gl1")
behind the device's own Direct3D entry points (port/linux/src/d3d8_gl.c),
without OpenGL: a console program, no window.

	t_fixed_device <driver> <extensions> [16|32] [-v] [-s]

The device is linked as it is, with this file standing in for the platform
layer and for the driver: every OpenGL entry point resolves (through
gl_functions.c and xgpu_fixed.c's own loader) to a recorder here, which
keeps the state OpenGL would hold and checks each call against what the
pretended driver has and what debug.gl1_extensions (the second argument)
allows. A call to anything else is a call through a null pointer: the
renderers with shaders never run.

<driver> is "plain" (OpenGL 1.1, no extensions), "ext" (OpenGL 1.2 with the
extensions of its time, under their ARB and EXT entry points only) or
"core" (OpenGL 1.5: the same as parts of OpenGL itself, four units).

The draws are the game's: its vertex programs (rasterizer_xbox_vertex_
shaders_data.inc) with their vertex formats, and every pixel state it can
set (combiner_ff_sites.c). Checked:
- every draw that reaches OpenGL has its arrays enabled and readable for
  every vertex its indices name, the indices inside the range the draw's
  streams were evaluated for, each enabled unit a complete texture, the
  viewport inside the window, the matrices untouched, nothing locked left
  behind;
- the passes, units, environments, blends, alpha tests and depth functions
  are those of the plan nv2a_combiner_ff.c makes for the same state, which
  the test compiles for itself;
- positions against the test's own arithmetic: a 640x480 screen rectangle
  and a triangle in the world;
- textures: sizes, formats and mip chains by what the context has;
- clears, viewports, a draw that samples the back buffer, off-screen
  targets, visibility tests, and the processor's floating-point state.

-v prints the device's log as it is written; -s turns debug.gpu_stats on
(the device's clock then runs fast enough for its statistics to be logged,
and the lines it writes for each approximate plan are counted).

Lines that start with '#' are notes; the exit code is the number of
failures.

Built by tools/t_fixed_device.sh (the commands are the Windows build's).
*/

#include "xgpu.h"
#include "sdl_platform.h"
#include "port_config.h"
#include "nv2a_combiner_ff.h"
#include "nv2a_vsh_cpu.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <xmmintrin.h>

/* combiner_ff_sites.c writes out the game's pixel shader macros and enumerators
for a test without the device's headers; here those are in sight (xdk_pdb.h),
so its own go by other names */
#undef PS_COMBINERINPUTS
#undef PS_COMBINEROUTPUTS
#undef PS_COMBINERCOUNT
#undef PS_TEXTUREMODES
#undef PS_DOTMAPPING
#undef PS_INPUTTEXTURE
#define PS_CHANNEL_ALPHA SITE_PS_CHANNEL_ALPHA
#define PS_COMBINERCOUNT_UNIQUE_C0 SITE_PS_COMBINERCOUNT_UNIQUE_C0
#define PS_COMBINERCOUNT_UNIQUE_C1 SITE_PS_COMBINERCOUNT_UNIQUE_C1
#define PS_COMBINEROUTPUT_IDENTITY SITE_PS_COMBINEROUTPUT_IDENTITY
#define PS_COMBINEROUTPUT_AB_DOT_PRODUCT SITE_PS_COMBINEROUTPUT_AB_DOT_PRODUCT
#define PS_COMBINEROUTPUT_CD_DOT_PRODUCT SITE_PS_COMBINEROUTPUT_CD_DOT_PRODUCT
#define PS_DOTMAPPING_ZERO_TO_ONE SITE_PS_DOTMAPPING_ZERO_TO_ONE
#define PS_DOTMAPPING_MINUS1_TO_1_D3D SITE_PS_DOTMAPPING_MINUS1_TO_1_D3D
#define PS_INPUTMAPPING_UNSIGNED_INVERT SITE_PS_INPUTMAPPING_UNSIGNED_INVERT
#define PS_INPUTMAPPING_EXPAND_NORMAL SITE_PS_INPUTMAPPING_EXPAND_NORMAL
#define PS_INPUTMAPPING_SIGNED_NEGATE SITE_PS_INPUTMAPPING_SIGNED_NEGATE
#define PS_REGISTER_ZERO SITE_PS_REGISTER_ZERO
#define PS_REGISTER_DISCARD SITE_PS_REGISTER_DISCARD
#define PS_REGISTER_C0 SITE_PS_REGISTER_C0
#define PS_REGISTER_C1 SITE_PS_REGISTER_C1
#define PS_REGISTER_FOG SITE_PS_REGISTER_FOG
#define PS_REGISTER_V0 SITE_PS_REGISTER_V0
#define PS_REGISTER_V1 SITE_PS_REGISTER_V1
#define PS_REGISTER_T0 SITE_PS_REGISTER_T0
#define PS_REGISTER_T1 SITE_PS_REGISTER_T1
#define PS_REGISTER_T2 SITE_PS_REGISTER_T2
#define PS_REGISTER_T3 SITE_PS_REGISTER_T3
#define PS_REGISTER_R0 SITE_PS_REGISTER_R0
#define PS_REGISTER_R1 SITE_PS_REGISTER_R1
#define PS_REGISTER_EF_PROD SITE_PS_REGISTER_EF_PROD
#define PS_REGISTER_ONE SITE_PS_REGISTER_ONE
#define PS_REGISTER_ONE_HALF SITE_PS_REGISTER_ONE_HALF
#define PS_TEXTUREMODES_NONE SITE_PS_TEXTUREMODES_NONE
#define PS_TEXTUREMODES_PROJECT2D SITE_PS_TEXTUREMODES_PROJECT2D
#define PS_TEXTUREMODES_CUBEMAP SITE_PS_TEXTUREMODES_CUBEMAP
#define PS_TEXTUREMODES_DOT_ST SITE_PS_TEXTUREMODES_DOT_ST
#define PS_TEXTUREMODES_DOT_RFLCT_DIFF SITE_PS_TEXTUREMODES_DOT_RFLCT_DIFF
#define PS_TEXTUREMODES_DOT_RFLCT_SPEC SITE_PS_TEXTUREMODES_DOT_RFLCT_SPEC
#define PS_TEXTUREMODES_DOTPRODUCT SITE_PS_TEXTUREMODES_DOTPRODUCT
#include "combiner_ff_sites.c"

void WINAPI D3DDevice_SetRenderState_ZBias(DWORD value);

/* ---------- failures */

static int failures;
static int verbose;
/* -s: debug.gpu_stats */
static int statistics;
static const char *test_name = "";
/* while a candidate is tried (check_against_plan), failures are counted
apart and not printed */
static int fail_quiet, quiet_failures;

static void fail(const char *format, ...)
{
	va_list arguments;

	if (fail_quiet)
	{
		quiet_failures++;
		return;
	}
	failures++;
	if (failures > 60)
		return;
	printf("FAIL [%s] ", test_name);
	va_start(arguments, format);
	vprintf(format, arguments);
	va_end(arguments);
	printf("\n");
}

#define CHECK(condition, ...) do { if (!(condition)) fail(__VA_ARGS__); } while (0)

/* ---------- the pretended driver */

struct driver
{
	const char *name;
	const char *version;
	const char *extensions;
	int version_number;
	int units;
	int maximum_texture_size;
	int depth_bits, stencil_bits, alpha_bits;
	/* the core names of the entry points that were extensions' resolve */
	int core_names;
};

static struct driver driver;
static const char *extensions_setting = "all";
static const char *texture_bits_setting = "16";

/* what the driver has and the setting allows: the test's own reading of
the rules (xgpu_fixed.c, xgpu_fixed_initialize) */
static struct
{
	int multitexture, units, combine, combine_arb, env_add, dot3, bgra, packed, packed_565, s3tc, cva;
	int blend_color, blend_minmax, blend_subtract, edge_clamp, mirrored, texture_lod;
} allowed;

static int word_in_list(const char *list, const char *word)
{
	size_t length = strlen(word);
	const char *found;

	for (found = list; (found = strstr(found, word)) != NULL; found += length)
	{
		if ((found == list || found[-1] == ' ' || found[-1] == ',' || found[-1] == '_') &&
			(found[length] == ' ' || found[length] == ',' || !found[length]))
		{
			/* (a name may be written with or without its GL_) */
			if (found == list || found[-1] != '_' || (found >= list + 3 && !strncmp(found - 3, "GL_", 3)))
				return 1;
		}
	}
	return 0;
}

static int setting_allows(const char *extension)
{
	if (word_in_list(extensions_setting, "none"))
		return 0;
	return word_in_list(extensions_setting, "all") || word_in_list(extensions_setting, extension);
}

static int feature(const char *extension, const char *second, int core_version)
{
	int present = word_in_list(driver.extensions, extension) || (second && word_in_list(driver.extensions, second)) ||
		(core_version && driver.version_number >= core_version);

	return present && (setting_allows(extension) || (second && setting_allows(second)));
}

static void allowed_compute(void)
{
	allowed.multitexture = feature("ARB_multitexture", NULL, 130);
	allowed.units = allowed.multitexture ? (driver.units > 4 ? 4 : driver.units) : 1;
	if (allowed.units < 2)
		allowed.multitexture = 0;
	allowed.combine_arb = feature("ARB_texture_env_combine", NULL, 130);
	allowed.combine = feature("EXT_texture_env_combine", NULL, 0) || allowed.combine_arb;
	allowed.env_add = feature("EXT_texture_env_add", "ARB_texture_env_add", 130);
	allowed.dot3 = allowed.combine && feature("ARB_texture_env_dot3", "EXT_texture_env_dot3", 130);
	allowed.bgra = feature("EXT_bgra", NULL, 120);
	allowed.packed = feature("EXT_packed_pixels", NULL, 120);
	allowed.packed_565 = allowed.packed && driver.version_number >= 120;
	allowed.s3tc = feature("EXT_texture_compression_s3tc", NULL, 0);
	allowed.cva = feature("EXT_compiled_vertex_array", NULL, 0);
	allowed.blend_color = feature("EXT_blend_color", NULL, 140);
	allowed.blend_minmax = feature("EXT_blend_minmax", NULL, 140);
	allowed.blend_subtract = feature("EXT_blend_subtract", NULL, 140);
	allowed.edge_clamp = feature("EXT_texture_edge_clamp", "SGIS_texture_edge_clamp", 120);
	allowed.mirrored = feature("ARB_texture_mirrored_repeat", "IBM_texture_mirrored_repeat", 140);
	allowed.texture_lod = feature("SGIS_texture_lod", NULL, 120);
}

/* ---------- the recorder: OpenGL's state, and the draws */

#define REC_UNITS 4
#define REC_TEXTURES 4096
#define REC_LEVELS 14
#define REC_DRAWS 16
#define REC_DRAW_COLORS 8

struct rec_unit
{
	int enabled;
	GLuint texture;
	GLenum mode;
	GLenum combine[2];
	GLenum source[2][3], operand[2][3];
	int scale[2];
	float env_color[4];
	int coordinates;
	GLint coordinate_size;
	GLenum coordinate_type;
	GLsizei coordinate_stride;
	const void *coordinate_pointer;
	/* GL_TEXTURE, column major */
	float matrix[16];
};

struct rec_texture
{
	int used;
	int width[REC_LEVELS], height[REC_LEVELS];
	GLint internal[REC_LEVELS];
	int compressed;
	GLint min_filter, mag_filter, wrap_s, wrap_t;
	int max_level;
	int copies;
};

struct rec_state
{
	struct rec_unit units[REC_UNITS];
	int active_unit, client_unit;
	GLenum matrix_mode;
	/* the modelview matrix was changed (the device never does) */
	int matrix_touched;
	/* GL_PROJECTION, column major (the identity for the evaluator's
	clip-space positions; a lifted draw's matrix otherwise) */
	float projection[16];
	int vertex_array, color_array;
	GLint vertex_size;
	GLenum vertex_type;
	GLsizei vertex_stride;
	const void *vertex_pointer;
	const unsigned char *color_pointer;
	float color[4];
	int depth_test, blend, alpha_test, cull, stencil_test, scissor_test, lighting, fog, offset_fill;
	GLenum depth_function;
	int depth_mask;
	GLenum blend_source, blend_destination, blend_equation;
	float blend_color[4];
	GLenum alpha_function;
	float alpha_reference;
	int color_mask;
	GLenum front_face, cull_mode, polygon_mode;
	float polygon_offset[2];
	GLint viewport[4], scissor[4];
	double depth_range[2];
	int locked, lock_first, lock_count;
	/* the arrays that were enabled when the lock was made, which the driver
	may have compiled: the colour array, and the units' coordinate arrays (a bit each) */
	int locked_color, locked_coordinates;
};

struct rec_draw
{
	struct rec_state state;
	GLenum mode;
	GLsizei count;
	int indexed;
	const unsigned short *indices;
	/* glDrawArrays: the first array element (a lifted draw's is the
	stream's own first vertex; the evaluator's arrays start at 0) */
	GLint first;
	unsigned long minimum, maximum;
	/* the colour array's first vertices as they were when the draw was
	made (a later pass of the same Direct3D draw writes the array again) */
	unsigned char colors[REC_DRAW_COLORS][4];
};

static struct
{
	struct rec_state state;
	struct rec_texture textures[REC_TEXTURES];
	GLuint next_texture;
	struct rec_draw draws[REC_DRAWS];
	int draw_count;
	int clears, swaps, copies, depth_reads, color_reads, texture_images, compressed_images;
	GLbitfield last_clear_mask;
	GLint last_clear_scissor[4];
	int last_clear_scissored;
	GLenum pending_error;
	/* the range of vertices the current Direct3D draw may touch, when the test knows it */
	int expect_range;
	unsigned long expect_first, expect_last;
	/* the depth buffer the read-backs see: nearer than this left of the column */
	int depth_column;
	float depth_left, depth_right;
	/* reading the depth buffer raises an error */
	int depth_unreadable;
	int window_width, window_height;
} rec;

static void rec_reset_state(void)
{
	int unit;

	memset(&rec.state, 0, sizeof(rec.state));
	for (unit = 0; unit < REC_UNITS; unit++)
	{
		rec.state.units[unit].mode = GL_MODULATE;
		rec.state.units[unit].matrix[0] = rec.state.units[unit].matrix[5] = 1.0f;
		rec.state.units[unit].matrix[10] = rec.state.units[unit].matrix[15] = 1.0f;
		rec.state.units[unit].scale[0] = rec.state.units[unit].scale[1] = 1;
	}
	rec.state.matrix_mode = GL_MODELVIEW;
	rec.state.projection[0] = rec.state.projection[5] = rec.state.projection[10] = rec.state.projection[15] = 1.0f;
	rec.state.depth_function = GL_LESS;
	rec.state.depth_mask = 1;
	rec.state.blend_source = GL_ONE;
	rec.state.blend_destination = GL_ZERO;
	rec.state.blend_equation = GL_FUNC_ADD;
	rec.state.alpha_function = GL_ALWAYS;
	rec.state.color_mask = 15;
	rec.state.front_face = GL_CCW;
	rec.state.cull_mode = GL_BACK;
	rec.state.polygon_mode = GL_FILL;
	rec.state.depth_range[1] = 1.0;
	rec.state.color[0] = rec.state.color[1] = rec.state.color[2] = rec.state.color[3] = 1.0f;
	rec.next_texture = 1;
}

static struct rec_texture *rec_bound_texture(void)
{
	GLuint name = rec.state.units[rec.state.active_unit].texture;

	return name && name < REC_TEXTURES ? &rec.textures[name] : NULL;
}

/* whether OpenGL 1.1 would sample the texture: a first level, and with a
mipmap filter every level down to one texel (or to GL_TEXTURE_MAX_LEVEL) */
static int rec_texture_complete(const struct rec_texture *texture)
{
	int level, width, height;

	if (!texture->used || !texture->width[0])
		return 0;
	if (texture->min_filter == GL_NEAREST || texture->min_filter == GL_LINEAR)
		return 1;
	width = texture->width[0];
	height = texture->height[0];
	for (level = 1; width > 1 || height > 1; level++)
	{
		width = width > 1 ? width / 2 : 1;
		height = height > 1 ? height / 2 : 1;
		if (texture->max_level && level > texture->max_level)
			return 1;
		if (level >= REC_LEVELS || texture->width[level] != width || texture->height[level] != height ||
			texture->internal[level] != texture->internal[0])
		{
			return 0;
		}
	}
	return 1;
}

static int finite_float(float value)
{
	return value == value && value > -1.0e30f && value < 1.0e30f;
}

/* ----- a vertex as OpenGL would read it: the arrays by their types, then the matrices */

/* an element of an array: size components of type at stride bytes a
vertex, the missing ones 0, 0, 1 */
static void rec_element(const void *pointer, GLint size, GLenum type, GLsizei stride, unsigned long vertex, double out[4])
{
	const unsigned char *bytes = (const unsigned char *)pointer + (unsigned long)stride * vertex;
	int component;

	out[0] = out[1] = out[2] = 0.0;
	out[3] = 1.0;
	for (component = 0; component < size && component < 4; component++)
	{
		if (type == GL_SHORT)
			out[component] = ((const short *)bytes)[component];
		else
			out[component] = ((const float *)bytes)[component];
	}
}

/* a column-major matrix times a vector */
static void rec_transform(const float *matrix, const double in[4], double out[4])
{
	int row;

	for (row = 0; row < 4; row++)
		out[row] = matrix[row] * in[0] + matrix[4 + row] * in[1] + matrix[8 + row] * in[2] + matrix[12 + row] * in[3];
}

/* a vertex's clip-space position: the vertex array through GL_PROJECTION */
static void rec_vertex_clip(const struct rec_state *state, unsigned long vertex, double clip[4])
{
	double in[4];

	rec_element(state->vertex_pointer, state->vertex_size, state->vertex_type, state->vertex_stride, vertex, in);
	rec_transform(state->projection, in, clip);
}

/* a vertex's texture coordinates on a unit: its array through its
GL_TEXTURE matrix (the array disabled: 0, 0, 0, 1 through the matrix) */
static void rec_vertex_coordinates(const struct rec_state *state, int unit, unsigned long vertex, double out[4])
{
	const struct rec_unit *coordinates = &state->units[unit];
	double in[4] = { 0.0, 0.0, 0.0, 1.0 };

	if (coordinates->coordinates && coordinates->coordinate_pointer)
	{
		rec_element(coordinates->coordinate_pointer, coordinates->coordinate_size, coordinates->coordinate_type,
			coordinates->coordinate_stride, vertex, in);
	}
	rec_transform(coordinates->matrix, in, out);
}

/* whether a draw's positions are lifted: the game's own stream through a
matrix rather than the evaluator's clip-space array */
static int rec_lifted(const struct rec_state *state)
{
	return state->vertex_size != 4 || state->vertex_stride != 16 || state->projection[0] != 1.0f ||
		state->projection[5] != 1.0f || state->projection[12] != 0.0f || state->projection[14] != 0.0f;
}

static void rec_draw(GLenum mode, GLsizei count, const unsigned short *indices, GLint first)
{
	struct rec_draw *draw;
	unsigned long minimum = 0xffffffffUL, maximum = 0, index;
	int unit;

	if (rec.draw_count >= REC_DRAWS)
	{
		fail("more than %d OpenGL draws for one Direct3D draw", REC_DRAWS);
		return;
	}
	draw = &rec.draws[rec.draw_count++];
	draw->state = rec.state;
	draw->mode = mode;
	draw->count = count;
	draw->indexed = indices != NULL;
	draw->indices = indices;
	draw->first = first;
	for (index = 0; index < (unsigned long)count; index++)
	{
		unsigned long vertex = indices ? indices[index] : (unsigned long)first + index;

		if (vertex < minimum)
			minimum = vertex;
		if (vertex > maximum)
			maximum = vertex;
	}
	draw->minimum = minimum;
	draw->maximum = maximum;

	CHECK(count > 0, "a draw of %d vertices", (int)count);
	CHECK(rec.state.vertex_array && rec.state.vertex_pointer, "a draw without a vertex array");
	CHECK(rec.state.vertex_size >= 2 && rec.state.vertex_stride >= rec.state.vertex_size * 4, "vertex array of %d floats, stride %d",
		(int)rec.state.vertex_size, (int)rec.state.vertex_stride);
	CHECK(!rec.state.matrix_touched, "the modelview matrix was changed");
	/* (the evaluator's positions go through the identity; a lifted draw's
	stream through its matrix: not one through the other) */
	CHECK((rec.state.vertex_size == 4 && rec.state.vertex_stride == 16) || rec_lifted(&rec.state),
		"a %d-float vertex array of stride %d with the identity projection", (int)rec.state.vertex_size, (int)rec.state.vertex_stride);
	CHECK(!rec.state.lighting && !rec.state.fog, "lighting or fog is on");
	CHECK(rec.state.viewport[0] >= 0 && rec.state.viewport[1] >= 0 && rec.state.viewport[2] > 0 &&
		rec.state.viewport[3] > 0 && rec.state.viewport[0] + rec.state.viewport[2] <= rec.window_width &&
		rec.state.viewport[1] + rec.state.viewport[3] <= rec.window_height,
		"viewport %d,%d %dx%d outside the %dx%d window", (int)rec.state.viewport[0], (int)rec.state.viewport[1],
		(int)rec.state.viewport[2], (int)rec.state.viewport[3], rec.window_width, rec.window_height);
	CHECK(!rec.state.scissor_test, "a draw with the scissor test on");
	if (rec.expect_range)
	{
		/* (a draw without indices names its vertices from its first) */
		unsigned long base = indices ? 0 : (unsigned long)first;

		CHECK(minimum - base >= rec.expect_first && maximum - base <= rec.expect_last,
			"vertices %lu..%lu drawn, the draw's are %lu..%lu", minimum - base, maximum - base, rec.expect_first, rec.expect_last);
	}
	if (rec.state.locked)
	{
		CHECK(minimum >= (unsigned long)rec.state.lock_first &&
			maximum < (unsigned long)(rec.state.lock_first + rec.state.lock_count),
			"vertices %lu..%lu drawn outside the locked %d..%d", minimum, maximum, rec.state.lock_first,
			rec.state.lock_first + rec.state.lock_count - 1);
	}
	if (rec.state.vertex_array && rec.state.vertex_pointer && count > 0)
	{
		/* every vertex the draw names is readable and a number */
		for (index = 0; index < (unsigned long)count; index++)
		{
			unsigned long vertex = indices ? indices[index] : (unsigned long)first + index;
			double position[4], coordinates[4];

			rec_vertex_clip(&rec.state, vertex, position);
			if (!finite_float((float)position[0]) || !finite_float((float)position[1]) || !finite_float((float)position[2]) ||
				!finite_float((float)position[3]))
			{
				fail("vertex %lu has the position %g %g %g %g", vertex, position[0], position[1], position[2], position[3]);
				break;
			}
			for (unit = 0; unit < REC_UNITS; unit++)
			{
				const struct rec_unit *state = &rec.state.units[unit];

				rec_vertex_coordinates(&rec.state, unit, vertex, coordinates);
				if (state->enabled && state->coordinates &&
					(!finite_float((float)coordinates[0]) || !finite_float((float)coordinates[1])))
				{
					fail("vertex %lu has texture coordinates that are not numbers on unit %d", vertex, unit);
					index = (unsigned long)count;
					break;
				}
			}
		}
		if (rec.state.color_array)
		{
			volatile unsigned char touch = rec.state.color_pointer[minimum * 4] + rec.state.color_pointer[maximum * 4 + 3];

			(void)touch;
			for (index = 0; index < REC_DRAW_COLORS && minimum + index <= maximum; index++)
				memcpy(draw->colors[index], rec.state.color_pointer + (minimum + index) * 4, 4);
		}
	}
	CHECK(!rec.state.color_array || rec.state.color_pointer, "the colour array has no pointer");
	for (unit = 0; unit < REC_UNITS; unit++)
	{
		const struct rec_unit *state = &rec.state.units[unit];

		if (!state->enabled)
		{
			CHECK(!state->coordinates, "unit %d is off and has a coordinate array", unit);
			continue;
		}
		CHECK(unit < allowed.units, "unit %d is on and the context has %d", unit, allowed.units);
		CHECK(state->texture && state->texture < REC_TEXTURES && rec_texture_complete(&rec.textures[state->texture]),
			"unit %d has texture %u, which is not complete", unit, (unsigned)state->texture);
		if (state->coordinates)
		{
			CHECK(state->coordinate_pointer && state->coordinate_size >= 1 && state->coordinate_size <= 4 &&
				(state->coordinate_type == GL_SHORT || state->coordinate_stride >= state->coordinate_size * 4),
				"unit %d: coordinate array of %d of type %04x, stride %d", unit,
				(int)state->coordinate_size, (unsigned)state->coordinate_type, (int)state->coordinate_stride);
			/* (the evaluator's coordinates are four floats apart; a lifted
			draw's come from the game's streams) */
			CHECK((state->coordinate_type == GL_FLOAT && state->coordinate_stride == 16 &&
				(state->coordinate_size == 2 || state->coordinate_size == 4)) || rec_lifted(&rec.state),
				"unit %d: a coordinate array of %d of type %04x, stride %d, with the evaluator's positions", unit,
				(int)state->coordinate_size, (unsigned)state->coordinate_type, (int)state->coordinate_stride);
		}
	}
}

/* ----- the entry points */

static const GLubyte *APIENTRY rec_glGetString(GLenum name)
{
	switch (name)
	{
	case GL_VERSION: return (const GLubyte *)driver.version;
	case GL_EXTENSIONS: return (const GLubyte *)driver.extensions;
	case GL_RENDERER: return (const GLubyte *)"t_fixed_device";
	case GL_VENDOR: return (const GLubyte *)"test";
	default:
		fail("glGetString(%04x)", (unsigned)name);
		return (const GLubyte *)"";
	}
}

static void APIENTRY rec_glGetIntegerv(GLenum name, GLint *value)
{
	switch (name)
	{
	case GL_MAX_TEXTURE_SIZE: *value = driver.maximum_texture_size; break;
	case GL_MAX_TEXTURE_UNITS:
		CHECK(allowed.multitexture || word_in_list(driver.extensions, "ARB_multitexture") || driver.version_number >= 130,
			"GL_MAX_TEXTURE_UNITS asked without multitexture");
		*value = driver.units;
		break;
	case GL_RED_BITS: *value = 5; break;
	case GL_GREEN_BITS: *value = 6; break;
	case GL_BLUE_BITS: *value = 5; break;
	case GL_ALPHA_BITS: *value = driver.alpha_bits; break;
	case GL_DEPTH_BITS: *value = driver.depth_bits; break;
	case GL_STENCIL_BITS: *value = driver.stencil_bits; break;
	default: fail("glGetIntegerv(%04x)", (unsigned)name); *value = 0; break;
	}
}

static GLenum APIENTRY rec_glGetError(void)
{
	GLenum error = rec.pending_error;

	rec.pending_error = GL_NO_ERROR;
	return error;
}

static void rec_capability(GLenum capability, int value)
{
	switch (capability)
	{
	case GL_TEXTURE_2D: rec.state.units[rec.state.active_unit].enabled = value; break;
	case GL_DEPTH_TEST: rec.state.depth_test = value; break;
	case GL_BLEND: rec.state.blend = value; break;
	case GL_ALPHA_TEST: rec.state.alpha_test = value; break;
	case GL_CULL_FACE: rec.state.cull = value; break;
	case GL_STENCIL_TEST: rec.state.stencil_test = value; break;
	case GL_SCISSOR_TEST: rec.state.scissor_test = value; break;
	case GL_LIGHTING: rec.state.lighting = value; break;
	case GL_FOG: rec.state.fog = value; break;
	case GL_POLYGON_OFFSET_FILL: rec.state.offset_fill = value; break;
	case GL_POLYGON_OFFSET_LINE: break;
	default: fail("glEnable/glDisable(%04x) is not OpenGL 1.1's", (unsigned)capability); break;
	}
}

static void APIENTRY rec_glEnable(GLenum capability) { rec_capability(capability, 1); }
static void APIENTRY rec_glDisable(GLenum capability) { rec_capability(capability, 0); }

static void APIENTRY rec_glViewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
	rec.state.viewport[0] = x; rec.state.viewport[1] = y; rec.state.viewport[2] = width; rec.state.viewport[3] = height;
}

static void APIENTRY rec_glDepthRange(GLclampd nearest, GLclampd farthest)
{
	rec.state.depth_range[0] = nearest;
	rec.state.depth_range[1] = farthest;
}

static void APIENTRY rec_glScissor(GLint x, GLint y, GLsizei width, GLsizei height)
{
	rec.state.scissor[0] = x; rec.state.scissor[1] = y; rec.state.scissor[2] = width; rec.state.scissor[3] = height;
	CHECK(x >= 0 && y >= 0 && width > 0 && height > 0 && x + width <= rec.window_width && y + height <= rec.window_height,
		"scissor %d,%d %dx%d outside the %dx%d window", (int)x, (int)y, (int)width, (int)height,
		rec.window_width, rec.window_height);
}

static void APIENTRY rec_glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a) { (void)r; (void)g; (void)b; (void)a; }
static void APIENTRY rec_glClearDepth(GLclampd depth) { (void)depth; }
static void APIENTRY rec_glClearStencil(GLint stencil) { (void)stencil; }

static void APIENTRY rec_glClear(GLbitfield mask)
{
	rec.clears++;
	rec.last_clear_mask = mask;
	rec.last_clear_scissored = rec.state.scissor_test;
	memcpy(rec.last_clear_scissor, rec.state.scissor, sizeof(rec.last_clear_scissor));
	CHECK(!(mask & GL_STENCIL_BUFFER_BIT) || driver.stencil_bits, "stencil cleared without stencil bits");
}

static void APIENTRY rec_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
	rec.state.color_mask = (r ? 1 : 0) | (g ? 2 : 0) | (b ? 4 : 0) | (a ? 8 : 0);
}

static void APIENTRY rec_glDepthMask(GLboolean flag) { rec.state.depth_mask = flag != 0; }
static void APIENTRY rec_glDepthFunc(GLenum function) { rec.state.depth_function = function; }
static void APIENTRY rec_glStencilFunc(GLenum function, GLint reference, GLuint mask) { (void)function; (void)reference; (void)mask; }
static void APIENTRY rec_glStencilOp(GLenum fail_, GLenum zfail, GLenum zpass) { (void)fail_; (void)zfail; (void)zpass; }
static void APIENTRY rec_glStencilMask(GLuint mask) { (void)mask; }

static int constant_factor(GLenum factor)
{
	return factor >= 0x8001 && factor <= 0x8004;
}

static void APIENTRY rec_glBlendFunc(GLenum source, GLenum destination)
{
	rec.state.blend_source = source;
	rec.state.blend_destination = destination;
	CHECK(allowed.blend_color || (!constant_factor(source) && !constant_factor(destination)),
		"glBlendFunc(%04x, %04x) without EXT_blend_color", (unsigned)source, (unsigned)destination);
}

static void APIENTRY rec_glBlendEquation(GLenum equation)
{
	rec.state.blend_equation = equation;
	CHECK(allowed.blend_minmax || allowed.blend_subtract, "glBlendEquation without the blend extensions");
	CHECK(allowed.blend_minmax || (equation != GL_MIN && equation != GL_MAX), "glBlendEquation(MIN/MAX) without EXT_blend_minmax");
	CHECK(allowed.blend_subtract || (equation != GL_FUNC_SUBTRACT && equation != GL_FUNC_REVERSE_SUBTRACT),
		"glBlendEquation(SUBTRACT) without EXT_blend_subtract");
}

static void APIENTRY rec_glBlendColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a)
{
	rec.state.blend_color[0] = r; rec.state.blend_color[1] = g; rec.state.blend_color[2] = b; rec.state.blend_color[3] = a;
	CHECK(allowed.blend_color, "glBlendColor without EXT_blend_color");
}

static void APIENTRY rec_glCullFace(GLenum mode) { rec.state.cull_mode = mode; }
static void APIENTRY rec_glFrontFace(GLenum mode) { rec.state.front_face = mode; }
static void APIENTRY rec_glPolygonMode(GLenum face, GLenum mode) { (void)face; rec.state.polygon_mode = mode; }
static void APIENTRY rec_glPolygonOffset(GLfloat factor, GLfloat units) { rec.state.polygon_offset[0] = factor; rec.state.polygon_offset[1] = units; }
static void APIENTRY rec_glLineWidth(GLfloat width) { (void)width; }
static void APIENTRY rec_glPixelStorei(GLenum name, GLint value) { (void)value; CHECK(name == GL_UNPACK_ALIGNMENT || name == GL_PACK_ALIGNMENT, "glPixelStorei(%04x)", (unsigned)name); }

static void APIENTRY rec_glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels)
{
	CHECK(x >= 0 && y >= 0 && x + width <= rec.window_width && y + height <= rec.window_height,
		"glReadPixels %d,%d %dx%d outside the window", (int)x, (int)y, (int)width, (int)height);
	if (format == GL_DEPTH_COMPONENT && type == GL_FLOAT)
	{
		GLsizei row, column;

		rec.depth_reads++;
		if (rec.depth_unreadable)
			rec.pending_error = GL_INVALID_OPERATION;
		for (row = 0; row < height; row++)
			for (column = 0; column < width; column++)
				((float *)pixels)[row * width + column] = x + column < rec.depth_column ? rec.depth_left : rec.depth_right;
	}
	else
	{
		rec.color_reads++;
		memset(pixels, 0, (size_t)width * (size_t)height * 4);
	}
}

static void APIENTRY rec_glFinish(void) {}
static void APIENTRY rec_glFlush(void) {}

static void APIENTRY rec_glGenTextures(GLsizei count, GLuint *names)
{
	GLsizei index;

	for (index = 0; index < count; index++)
	{
		names[index] = rec.next_texture < REC_TEXTURES ? rec.next_texture++ : 0;
		if (names[index])
		{
			memset(&rec.textures[names[index]], 0, sizeof(rec.textures[0]));
			rec.textures[names[index]].min_filter = GL_NEAREST_MIPMAP_LINEAR;
			rec.textures[names[index]].mag_filter = GL_LINEAR;
			rec.textures[names[index]].wrap_s = rec.textures[names[index]].wrap_t = GL_REPEAT;
		}
	}
}

static void APIENTRY rec_glDeleteTextures(GLsizei count, const GLuint *names)
{
	GLsizei index;

	for (index = 0; index < count; index++)
	{
		if (names[index] < REC_TEXTURES)
			rec.textures[names[index]].used = 0;
	}
}

static void APIENTRY rec_glBindTexture(GLenum target, GLuint name)
{
	CHECK(target == GL_TEXTURE_2D, "glBindTexture(%04x): not a 2D texture", (unsigned)target);
	rec.state.units[rec.state.active_unit].texture = name;
	if (name && name < REC_TEXTURES)
		rec.textures[name].used = 1;
}

static int power_of_two(int value)
{
	return value > 0 && !(value & (value - 1));
}

static void rec_image(GLenum target, GLint level, GLint internal, GLsizei width, GLsizei height, GLint border, int compressed)
{
	struct rec_texture *texture = rec_bound_texture();

	CHECK(target == GL_TEXTURE_2D && border == 0, "texture image target %04x border %d", (unsigned)target, (int)border);
	CHECK(power_of_two(width) && power_of_two(height), "texture level of %dx%d: not powers of two", (int)width, (int)height);
	CHECK(width <= driver.maximum_texture_size && height <= driver.maximum_texture_size,
		"texture level of %dx%d above the context's %d", (int)width, (int)height, driver.maximum_texture_size);
	if (!texture)
	{
		fail("a texture image with no texture bound");
		return;
	}
	if (level >= 0 && level < REC_LEVELS)
	{
		texture->width[level] = width;
		texture->height[level] = height;
		texture->internal[level] = internal;
	}
	texture->compressed = compressed;
}

static void APIENTRY rec_glTexImage2D(GLenum target, GLint level, GLint internal, GLsizei width, GLsizei height, GLint border,
	GLenum format, GLenum type, const void *pixels)
{
	rec.texture_images++;
	rec_image(target, level, internal, width, height, border, 0);
	switch (internal)
	{
	case GL_RGB: case GL_RGBA: case GL_RGB5: case GL_RGB5_A1: case GL_RGBA4: case GL_RGB8: case GL_RGBA8:
	case GL_LUMINANCE8: case GL_INTENSITY8: case GL_LUMINANCE8_ALPHA8: case GL_ALPHA8:
		break;
	default: fail("glTexImage2D internal format %04x is not OpenGL 1.1's", (unsigned)internal); break;
	}
	CHECK(format != GL_BGRA || allowed.bgra, "GL_BGRA texels without EXT_bgra");
	CHECK(format == GL_RGBA || format == GL_RGB || format == GL_BGRA || format == GL_LUMINANCE || format == GL_LUMINANCE_ALPHA,
		"glTexImage2D format %04x", (unsigned)format);
	if (type == GL_UNSIGNED_SHORT_4_4_4_4 || type == GL_UNSIGNED_SHORT_5_5_5_1)
		CHECK(allowed.packed, "packed texels without EXT_packed_pixels");
	else if (type == GL_UNSIGNED_SHORT_5_6_5)
		CHECK(allowed.packed_565, "5:6:5 texels without OpenGL 1.2");
	else
		CHECK(type == GL_UNSIGNED_BYTE, "glTexImage2D type %04x", (unsigned)type);
	if (pixels)
	{
		/* every byte of the image is there to read */
		size_t bytes = type == GL_UNSIGNED_BYTE ? (format == GL_LUMINANCE ? 1 : format == GL_LUMINANCE_ALPHA ? 2 :
			format == GL_RGB ? 3 : 4) : 2;
		const volatile unsigned char *data = pixels;
		volatile unsigned char touch = data[0] + data[(size_t)width * (size_t)height * bytes - 1];

		(void)touch;
	}
}

static void APIENTRY rec_glCompressedTexImage2D(GLenum target, GLint level, GLenum internal, GLsizei width, GLsizei height,
	GLint border, GLsizei size, const void *data)
{
	rec.compressed_images++;
	CHECK(allowed.s3tc, "a compressed texture without EXT_texture_compression_s3tc");
	CHECK(internal >= 0x83f0 && internal <= 0x83f3, "compressed format %04x", (unsigned)internal);
	CHECK(size == ((width + 3) / 4) * ((height + 3) / 4) * (internal == 0x83f1 || internal == 0x83f0 ? 8 : 16),
		"compressed level %dx%d of %d bytes", (int)width, (int)height, (int)size);
	rec_image(target, level, (GLint)internal, width, height, border, 1);
	(void)data;
}

static void APIENTRY rec_glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei width, GLsizei height,
	GLenum format, GLenum type, const void *pixels)
{
	(void)target; (void)level; (void)x; (void)y; (void)width; (void)height; (void)format; (void)type; (void)pixels;
	fail("glTexSubImage2D is not used by the light renderer");
}

static void APIENTRY rec_glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y,
	GLsizei width, GLsizei height)
{
	struct rec_texture *texture = rec_bound_texture();

	rec.copies++;
	CHECK(target == GL_TEXTURE_2D && level == 0 && xoffset == 0 && yoffset == 0, "glCopyTexSubImage2D arguments");
	CHECK(x >= 0 && y >= 0 && x + width <= rec.window_width && y + height <= rec.window_height,
		"glCopyTexSubImage2D %d,%d %dx%d outside the window", (int)x, (int)y, (int)width, (int)height);
	if (texture)
	{
		CHECK(width <= texture->width[0] && height <= texture->height[0], "a copy of %dx%d into a texture of %dx%d",
			(int)width, (int)height, texture->width[0], texture->height[0]);
		texture->copies++;
	}
	else
	{
		fail("glCopyTexSubImage2D with no texture bound");
	}
}

static void rec_parameter(GLenum target, GLenum name, GLint value)
{
	struct rec_texture *texture = rec_bound_texture();

	CHECK(target == GL_TEXTURE_2D, "glTexParameter target %04x", (unsigned)target);
	if (!texture)
	{
		fail("glTexParameter with no texture bound");
		return;
	}
	switch (name)
	{
	case GL_TEXTURE_MIN_FILTER: texture->min_filter = value; break;
	case GL_TEXTURE_MAG_FILTER: texture->mag_filter = value; break;
	case GL_TEXTURE_WRAP_S:
	case GL_TEXTURE_WRAP_T:
		if (name == GL_TEXTURE_WRAP_S) texture->wrap_s = value; else texture->wrap_t = value;
		CHECK(value == GL_REPEAT || value == GL_CLAMP || (value == GL_CLAMP_TO_EDGE && allowed.edge_clamp) ||
			(value == GL_MIRRORED_REPEAT && allowed.mirrored), "wrap mode %04x is not the context's", (unsigned)value);
		break;
	case GL_TEXTURE_MAX_LEVEL:
		CHECK(allowed.texture_lod, "GL_TEXTURE_MAX_LEVEL without OpenGL 1.2");
		texture->max_level = value;
		break;
	case GL_TEXTURE_MIN_LOD:
		CHECK(allowed.texture_lod, "GL_TEXTURE_MIN_LOD without OpenGL 1.2");
		break;
	case GL_TEXTURE_BORDER_COLOR:
		break;
	default: fail("glTexParameter(%04x) is not the context's", (unsigned)name); break;
	}
}

static void APIENTRY rec_glTexParameteri(GLenum target, GLenum name, GLint value) { rec_parameter(target, name, value); }
static void APIENTRY rec_glTexParameteriv(GLenum target, GLenum name, const GLint *value) { rec_parameter(target, name, value[0]); }
static void APIENTRY rec_glTexParameterf(GLenum target, GLenum name, GLfloat value) { rec_parameter(target, name, (GLint)value); }
static void APIENTRY rec_glTexParameterfv(GLenum target, GLenum name, const GLfloat *value) { rec_parameter(target, name, (GLint)value[0]); }
static void APIENTRY rec_glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, void *pixels)
{
	(void)target; (void)level; (void)format; (void)type; (void)pixels;
	fail("glGetTexImage is not used by the light renderer");
}
static void APIENTRY rec_glDrawBuffer(GLenum mode) { (void)mode; fail("glDrawBuffer is not used by the light renderer"); }
static void APIENTRY rec_glReadBuffer(GLenum mode) { (void)mode; fail("glReadBuffer is not used by the light renderer"); }

static void APIENTRY rec_glDrawArrays(GLenum mode, GLint first, GLsizei count) { rec_draw(mode, count, NULL, first); }

static void APIENTRY rec_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices)
{
	CHECK(type == GL_UNSIGNED_SHORT && indices, "glDrawElements type %04x", (unsigned)type);
	rec_draw(mode, count, indices, 0);
}

/* fixed function */

static void APIENTRY rec_glMatrixMode(GLenum mode) { rec.state.matrix_mode = mode; }

static float *rec_current_matrix(void)
{
	if (rec.state.matrix_mode == GL_TEXTURE)
		return rec.state.units[rec.state.active_unit].matrix;
	if (rec.state.matrix_mode == GL_PROJECTION)
		return rec.state.projection;
	rec.state.matrix_touched = 1;
	return NULL;
}

static void APIENTRY rec_glLoadIdentity(void)
{
	float *matrix = rec.state.matrix_mode == GL_MODELVIEW ? NULL : rec_current_matrix();

	/* (the modelview is the identity already) */
	if (matrix)
	{
		memset(matrix, 0, 16 * sizeof(float));
		matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
	}
}

static void APIENTRY rec_glLoadMatrixf(const GLfloat *matrix)
{
	float *current = rec_current_matrix();

	if (current)
		memcpy(current, matrix, 16 * sizeof(float));
}

static void rec_client_state(GLenum array, int value)
{
	switch (array)
	{
	case GL_VERTEX_ARRAY: rec.state.vertex_array = value; break;
	case GL_COLOR_ARRAY: rec.state.color_array = value; break;
	case GL_TEXTURE_COORD_ARRAY: rec.state.units[rec.state.client_unit].coordinates = value; break;
	default: fail("client state %04x", (unsigned)array); break;
	}
}

static void APIENTRY rec_glEnableClientState(GLenum array) { rec_client_state(array, 1); }
static void APIENTRY rec_glDisableClientState(GLenum array) { rec_client_state(array, 0); }

static void APIENTRY rec_glVertexPointer(GLint size, GLenum type, GLsizei stride, const void *pointer)
{
	CHECK(type == GL_FLOAT && size >= 2 && size <= 4, "glVertexPointer(%d, %04x)", (int)size, (unsigned)type);
	CHECK(!rec.state.locked, "glVertexPointer while the arrays are locked");
	rec.state.vertex_size = size;
	rec.state.vertex_type = type;
	rec.state.vertex_stride = stride ? stride : size * 4;
	rec.state.vertex_pointer = pointer;
}

static void APIENTRY rec_glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const void *pointer)
{
	struct rec_unit *unit = &rec.state.units[rec.state.client_unit];

	CHECK((type == GL_FLOAT || type == GL_SHORT) && size >= 1 && size <= 4, "glTexCoordPointer(%d, %04x)", (int)size, (unsigned)type);
	CHECK(!rec.state.locked || !(rec.state.locked_coordinates & (1 << rec.state.client_unit)),
		"glTexCoordPointer on unit %d, whose array was enabled when the arrays were locked", rec.state.client_unit);
	unit->coordinate_size = size;
	unit->coordinate_type = type;
	unit->coordinate_stride = stride ? stride : size * (type == GL_FLOAT ? 4 : 2);
	unit->coordinate_pointer = pointer;
}

static void APIENTRY rec_glColorPointer(GLint size, GLenum type, GLsizei stride, const void *pointer)
{
	CHECK(size == 4 && type == GL_UNSIGNED_BYTE && stride == 4, "glColorPointer(%d, %04x, %d)", (int)size, (unsigned)type, (int)stride);
	CHECK(!rec.state.locked || !rec.state.locked_color, "glColorPointer for a colour array that was enabled when the arrays were locked");
	rec.state.color_pointer = pointer;
}

static void APIENTRY rec_glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
	rec.state.color[0] = r; rec.state.color[1] = g; rec.state.color[2] = b; rec.state.color[3] = a;
}

static void APIENTRY rec_glTexEnvi(GLenum target, GLenum name, GLint value)
{
	struct rec_unit *unit = &rec.state.units[rec.state.active_unit];

	CHECK(target == GL_TEXTURE_ENV, "glTexEnvi target %04x", (unsigned)target);
	if (name == GL_TEXTURE_ENV_MODE)
	{
		unit->mode = (GLenum)value;
		CHECK(value == GL_REPLACE || value == GL_MODULATE || value == GL_DECAL || value == GL_BLEND ||
			(value == GL_ADD && allowed.env_add) || (value == GL_COMBINE && allowed.combine),
			"texture environment %04x is not the context's", (unsigned)value);
		return;
	}
	CHECK(allowed.combine, "glTexEnvi(%04x) without the combine extension", (unsigned)name);
	if (name == GL_COMBINE_RGB || name == GL_COMBINE_ALPHA)
	{
		unit->combine[name == GL_COMBINE_ALPHA] = (GLenum)value;
		CHECK(value == GL_REPLACE || value == GL_MODULATE || value == GL_ADD || value == GL_ADD_SIGNED ||
			value == GL_INTERPOLATE || (value == GL_SUBTRACT && allowed.combine_arb) ||
			(value == GL_DOT3_RGB && allowed.dot3 && name == GL_COMBINE_RGB), "combine function %04x is not the context's",
			(unsigned)value);
	}
	else if (name >= GL_SOURCE0_RGB && name < GL_SOURCE0_RGB + 3)
	{
		unit->source[0][name - GL_SOURCE0_RGB] = (GLenum)value;
		CHECK(value == GL_TEXTURE || value == GL_CONSTANT || value == GL_PRIMARY_COLOR || value == GL_PREVIOUS,
			"combine source %04x", (unsigned)value);
	}
	else if (name >= GL_SOURCE0_ALPHA && name < GL_SOURCE0_ALPHA + 3)
	{
		unit->source[1][name - GL_SOURCE0_ALPHA] = (GLenum)value;
		CHECK(value == GL_TEXTURE || value == GL_CONSTANT || value == GL_PRIMARY_COLOR || value == GL_PREVIOUS,
			"combine source %04x", (unsigned)value);
	}
	else if (name >= GL_OPERAND0_RGB && name < GL_OPERAND0_RGB + 3)
	{
		unit->operand[0][name - GL_OPERAND0_RGB] = (GLenum)value;
		CHECK(value == GL_SRC_COLOR || value == GL_ONE_MINUS_SRC_COLOR || value == GL_SRC_ALPHA || value == GL_ONE_MINUS_SRC_ALPHA,
			"combine operand %04x", (unsigned)value);
		/* (EXT_texture_env_combine's third argument is an alpha as it is) */
		CHECK(allowed.combine_arb || name != GL_OPERAND0_RGB + 2 || value == GL_SRC_ALPHA,
			"the third colour operand %04x needs ARB_texture_env_combine", (unsigned)value);
	}
	else if (name >= GL_OPERAND0_ALPHA && name < GL_OPERAND0_ALPHA + 3)
	{
		unit->operand[1][name - GL_OPERAND0_ALPHA] = (GLenum)value;
		CHECK(value == GL_SRC_ALPHA || value == GL_ONE_MINUS_SRC_ALPHA, "combine alpha operand %04x", (unsigned)value);
		CHECK(allowed.combine_arb || name != GL_OPERAND0_ALPHA + 2 || value == GL_SRC_ALPHA,
			"the third alpha operand %04x needs ARB_texture_env_combine", (unsigned)value);
	}
	else if (name == GL_RGB_SCALE || name == GL_ALPHA_SCALE)
	{
		unit->scale[name == GL_ALPHA_SCALE] = value;
		CHECK(value == 1 || value == 2 || value == 4, "combine scale %d", (int)value);
	}
	else
	{
		fail("glTexEnvi(%04x)", (unsigned)name);
	}
}

static void APIENTRY rec_glTexEnvfv(GLenum target, GLenum name, const GLfloat *value)
{
	CHECK(target == GL_TEXTURE_ENV && name == GL_TEXTURE_ENV_COLOR, "glTexEnvfv(%04x, %04x)", (unsigned)target, (unsigned)name);
	memcpy(rec.state.units[rec.state.active_unit].env_color, value, 4 * sizeof(float));
}

static void APIENTRY rec_glAlphaFunc(GLenum function, GLclampf reference)
{
	rec.state.alpha_function = function;
	rec.state.alpha_reference = reference;
}

static void APIENTRY rec_glShadeModel(GLenum mode) { CHECK(mode == GL_SMOOTH, "glShadeModel(%04x)", (unsigned)mode); }

/* extensions */

static void APIENTRY rec_glActiveTexture(GLenum texture)
{
	CHECK(allowed.multitexture, "glActiveTexture without ARB_multitexture");
	CHECK(texture >= GL_TEXTURE0 && texture < GL_TEXTURE0 + (GLenum)allowed.units, "glActiveTexture(%04x)", (unsigned)texture);
	if (texture >= GL_TEXTURE0 && texture < GL_TEXTURE0 + REC_UNITS)
		rec.state.active_unit = (int)(texture - GL_TEXTURE0);
}

static void APIENTRY rec_glClientActiveTexture(GLenum texture)
{
	CHECK(allowed.multitexture, "glClientActiveTexture without ARB_multitexture");
	CHECK(texture >= GL_TEXTURE0 && texture < GL_TEXTURE0 + (GLenum)allowed.units, "glClientActiveTexture(%04x)", (unsigned)texture);
	if (texture >= GL_TEXTURE0 && texture < GL_TEXTURE0 + REC_UNITS)
		rec.state.client_unit = (int)(texture - GL_TEXTURE0);
}

static void APIENTRY rec_glLockArrays(GLint first, GLsizei count)
{
	CHECK(allowed.cva, "glLockArraysEXT without EXT_compiled_vertex_array");
	CHECK(!rec.state.locked, "glLockArraysEXT twice");
	rec.state.locked = 1;
	rec.state.lock_first = first;
	rec.state.lock_count = count;
	/* Only the positions are locked: they alone stay as they are through
	the passes. The arrays that change between passes are enabled after the
	lock, as in Quake III's renderer, whose use of the extension is the one
	the drivers of the time were made for. */
	{
		int unit;

		rec.state.locked_color = rec.state.color_array;
		rec.state.locked_coordinates = 0;
		for (unit = 0; unit < REC_UNITS; unit++)
			rec.state.locked_coordinates |= rec.state.units[unit].coordinates ? 1 << unit : 0;
		CHECK(rec.state.vertex_array && !rec.state.locked_color && !rec.state.locked_coordinates,
			"glLockArraysEXT with the colour array %s and coordinate arrays %x enabled", rec.state.locked_color ? "on" : "off",
			(unsigned)rec.state.locked_coordinates);
	}
}

static void APIENTRY rec_glUnlockArrays(void)
{
	CHECK(rec.state.locked, "glUnlockArraysEXT without a lock");
	rec.state.locked = 0;
}

/* ----- resolving */

typedef void (*function_pointer)(void);

static const struct
{
	const char *name;
	function_pointer function;
	/* 0: OpenGL 1.1; 1: an extension's own name; 2: the name OpenGL took it up under */
	int kind;
} entry_points[] =
{
#define BASE(name) { #name, (function_pointer)rec_##name, 0 },
	BASE(glGetString) BASE(glGetIntegerv) BASE(glGetTexImage) BASE(glGetError) BASE(glEnable) BASE(glDisable)
	BASE(glViewport) BASE(glDepthRange) BASE(glScissor) BASE(glClearColor) BASE(glClearDepth) BASE(glClearStencil)
	BASE(glClear) BASE(glColorMask) BASE(glDepthMask) BASE(glDepthFunc) BASE(glStencilFunc) BASE(glStencilOp)
	BASE(glStencilMask) BASE(glBlendFunc) BASE(glCullFace) BASE(glFrontFace) BASE(glPolygonMode) BASE(glPolygonOffset)
	BASE(glLineWidth) BASE(glPixelStorei) BASE(glReadPixels) BASE(glFinish) BASE(glFlush) BASE(glGenTextures)
	BASE(glDeleteTextures) BASE(glBindTexture) BASE(glTexImage2D) BASE(glTexSubImage2D) BASE(glCopyTexSubImage2D)
	BASE(glTexParameteri) BASE(glTexParameteriv) BASE(glTexParameterf) BASE(glTexParameterfv) BASE(glDrawBuffer)
	BASE(glReadBuffer) BASE(glDrawArrays) BASE(glDrawElements)
	BASE(glMatrixMode) BASE(glLoadIdentity) BASE(glLoadMatrixf) BASE(glEnableClientState) BASE(glDisableClientState)
	BASE(glVertexPointer) BASE(glTexCoordPointer) BASE(glColorPointer) BASE(glColor4f) BASE(glTexEnvi) BASE(glTexEnvfv)
	BASE(glAlphaFunc) BASE(glShadeModel)
#undef BASE
	{ "glActiveTextureARB", (function_pointer)rec_glActiveTexture, 1 },
	{ "glClientActiveTextureARB", (function_pointer)rec_glClientActiveTexture, 1 },
	{ "glLockArraysEXT", (function_pointer)rec_glLockArrays, 1 },
	{ "glUnlockArraysEXT", (function_pointer)rec_glUnlockArrays, 1 },
	{ "glBlendEquationEXT", (function_pointer)rec_glBlendEquation, 1 },
	{ "glBlendColorEXT", (function_pointer)rec_glBlendColor, 1 },
	{ "glCompressedTexImage2DARB", (function_pointer)rec_glCompressedTexImage2D, 1 },
	{ "glActiveTexture", (function_pointer)rec_glActiveTexture, 2 },
	{ "glClientActiveTexture", (function_pointer)rec_glClientActiveTexture, 2 },
	{ "glBlendEquation", (function_pointer)rec_glBlendEquation, 2 },
	{ "glBlendColor", (function_pointer)rec_glBlendColor, 2 },
	{ "glCompressedTexImage2D", (function_pointer)rec_glCompressedTexImage2D, 2 },
};

/* what gl_functions.c and xgpu_fixed.c resolve entry points with */
function_pointer SDL_GL_GetProcAddress(const char *name)
{
	unsigned long index;

	for (index = 0; index < sizeof(entry_points) / sizeof(entry_points[0]); index++)
	{
		if (!strcmp(entry_points[index].name, name))
		{
			if (entry_points[index].kind == 1 && !*driver.extensions)
				return NULL;
			if (entry_points[index].kind == 2 && !driver.core_names)
				return NULL;
			return entry_points[index].function;
		}
	}
	return NULL;
}

/* ---------- the platform layer */

static char log_text[1 << 18];
static size_t log_length;

void platform_log(const char *format, ...)
{
	char line[4096];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	if (verbose)
		printf("# log: %s\n", line);
	if (log_length + strlen(line) + 2 < sizeof(log_text))
	{
		strcpy(log_text + log_length, line);
		log_length += strlen(line);
		log_text[log_length++] = '\n';
		log_text[log_length] = 0;
	}
}

static int log_count(const char *text)
{
	const char *found;
	int count = 0;

	for (found = log_text; (found = strstr(found, text)) != NULL; found += strlen(text))
		count++;
	return count;
}

const char *config_string(const char *name)
{
	if (!strcmp(name, "display.renderer"))
		return "gl1";
	if (!strcmp(name, "debug.gl1_extensions"))
		return extensions_setting;
	return "";
}

int config_boolean(const char *name)
{
	return statistics && !strcmp(name, "debug.gpu_stats");
}

long config_integer(const char *name)
{
	if (!strcmp(name, "display.gl1_texture_bits"))
		return atol(texture_bits_setting);
	if (!strcmp(name, "debug.gpu_trace_frame"))
		return -1;
	return 0;
}

int gl_functions_load(void);

BOOL platform_video_initialize(unsigned long width, unsigned long height)
{
	(void)width;
	(void)height;
	return gl_functions_load() != 0;
}

BOOL platform_video_core_context(void) { return FALSE; }

void platform_video_window_bits(int *depth, int *stencil)
{
	*depth = driver.depth_bits;
	*stencil = driver.stencil_bits;
}

void platform_video_drawable_size(int *width, int *height)
{
	*width = rec.window_width;
	*height = rec.window_height;
}

void platform_video_window_size(int *width, int *height)
{
	*width = rec.window_width;
	*height = rec.window_height;
}

void platform_video_swap(void) { rec.swaps++; }
BOOL platform_screen_mode(long *width, long *height) { (void)width; (void)height; return FALSE; }
void platform_pump_events(void) {}
int halo_interpolation_enabled(void) { return 1; }
void platform_ui_pointer_set_active(BOOL active) { (void)active; }
BOOL platform_ui_pointer_read(struct platform_ui_pointer *pointer) { (void)pointer; return FALSE; }

int pthread_mutex_lock(pthread_mutex_t *mutex) { (void)mutex; return 0; }
int pthread_mutex_unlock(pthread_mutex_t *mutex) { (void)mutex; return 0; }
int pthread_cond_broadcast(pthread_cond_t *condition) { (void)condition; return 0; }
int pthread_cond_wait(pthread_cond_t *condition, pthread_mutex_t *mutex) { (void)condition; (void)mutex; return 0; }
int pthread_create(pthread_t *thread, const pthread_attr_t *attributes, void *(*start)(void *), void *argument)
{
	(void)thread; (void)attributes; (void)start; (void)argument;
	return 1;
}
int pthread_detach(pthread_t thread) { (void)thread; return 0; }

static long fake_nanoseconds, fake_seconds;

int clock_gettime(clockid_t clock_id, struct timespec *time)
{
	(void)clock_id;
	/* (a microsecond a call; a millisecond with -s, so that the
	seconds between two lines of statistics pass) */
	fake_nanoseconds += statistics ? 1000000 : 1000;
	if (fake_nanoseconds >= 1000000000L)
	{
		fake_nanoseconds -= 1000000000L;
		fake_seconds++;
	}
	time->tv_sec = 100 + fake_seconds;
	time->tv_nsec = fake_nanoseconds;
	return 0;
}

int clock_nanosleep(clockid_t clock_id, int flags, const struct timespec *time, struct timespec *remaining)
{
	(void)clock_id; (void)flags; (void)time; (void)remaining;
	return 0;
}

/* the game's own exponential (halo_math.h), which nv2a_combiner_ff.c's fog
calls: the C library's here */
#undef exp
double halo_exp(double x)
{
	return exp(x);
}

/* ----- guest memory: a piece of the contiguous window */

#define GUEST_SIZE (48UL * 1024 * 1024)

void *__stdcall VirtualAlloc(void *address, unsigned long size, unsigned long type, unsigned long protect);

static unsigned char *guest_base;
static unsigned long guest_used;

static void guest_initialize(void)
{
	guest_base = VirtualAlloc((void *)PLATFORM_CONTIGUOUS_BASE, GUEST_SIZE, 0x3000 /* MEM_COMMIT | MEM_RESERVE */, 4 /* PAGE_READWRITE */);
	if (guest_base != (unsigned char *)PLATFORM_CONTIGUOUS_BASE)
	{
		printf("FAIL cannot map the memory window at %08lx\n", (unsigned long)PLATFORM_CONTIGUOUS_BASE);
		exit(99);
	}
	/* (physical address 0 is no resource's: to the device a buffer whose
	Data is 0 is no buffer) */
	guest_used = 4096;
}

static void *guest_alloc(unsigned long size)
{
	void *memory = guest_base + guest_used;

	guest_used += (size + 4095) & ~4095UL;
	if (guest_used > GUEST_SIZE)
	{
		printf("FAIL out of guest memory\n");
		exit(99);
	}
	return memory;
}

BOOL platform_is_contiguous(const void *address)
{
	return (const unsigned char *)address >= guest_base && (const unsigned char *)address < guest_base + GUEST_SIZE;
}

static unsigned long watch_generation = 1;

void memory_watch_initialize(void) {}
void memory_watch_protect(unsigned long address, unsigned long size) { (void)address; (void)size; }
unsigned long memory_watch_generation(unsigned long address, unsigned long size) { (void)address; (void)size; return watch_generation; }
unsigned long memory_watch_serial(void) { return watch_generation; }

/* d3d8_resources.c's, without its allocator */
void d3d8_surface_resize(D3DSurface *surface, D3DFORMAT format, unsigned long width, unsigned long height)
{
	unsigned long pitch = (width * 4 + D3DTEXTURE_PITCH_ALIGNMENT - 1) & ~(unsigned long)(D3DTEXTURE_PITCH_ALIGNMENT - 1);

	(void)format;
	surface->Size = ((pitch / D3DTEXTURE_PITCH_ALIGNMENT - 1) << D3DSIZE_PITCH_SHIFT) |
		((height - 1) << D3DSIZE_HEIGHT_SHIFT) | (width - 1);
}

void d3d8_surface_initialize(D3DSurface *surface, D3DFORMAT format, unsigned long width, unsigned long height)
{
	void *memory = guest_alloc(64 * 1024);

	memset(surface, 0, sizeof(*surface));
	surface->Common = 1;
	surface->Data = PLATFORM_VIRTUAL_TO_PHYSICAL(memory);
	surface->Format = ((DWORD)format << D3DFORMAT_FORMAT_SHIFT) | (2 << D3DFORMAT_DIMENSION_SHIFT);
	d3d8_surface_resize(surface, format, width, height);
}

/* ---------- the game's vertex programs and formats (tools/gl2_shader_dump.c) */

static const DWORD vertex_shader_code[] =
{
#include "../../../../source/rasterizer/xbox/rasterizer_xbox_vertex_shaders_data.inc"
};

#define NUMBER_OF_VERTEX_SHADERS 67

/* source/rasterizer/xbox/rasterizer_xbox_vertex_shaders_initialize.c */
static const DWORD vertex_shader_declarations[] =
{
	0x20000000, 0x40320000, 0x40400009, 0xFFFFFFFF,
	0x20000000, 0x40320000, 0x40210004, 0xFFFFFFFF,
	0x20000000, 0x40340000, 0x40340009, 0x40150008, 0xFFFFFFFF,
	0x20000000, 0x40320000, 0x40220004, 0x40400009, 0xFFFFFFFF,
	0x20000000, 0x40320000, 0x40220004, 0x40400009, 0x20000001, 0x4024000B, 0xFFFFFFFF,
	0x20000000, 0x40220000, 0x40220004, 0x40400009, 0xFFFFFFFF,
	0x20000000, 0x40320000, 0x40160001, 0x40160002, 0x40160003, 0x40210004, 0x40240005, 0x40110006, 0xFFFFFFFF,
	0x20000000, 0x40320000, 0x40160001, 0x40160002, 0x40160003, 0x40220004, 0x20000001, 0x40160007, 0x40210008, 0xFFFFFFFF,
	0x20000000, 0x40320000, 0x40160001, 0x40160002, 0x40160003, 0x40220004, 0xFFFFFFFF,
};

static const unsigned char vertex_shader_declaration_offsets[NUMBER_OF_VERTEX_SHADERS] =
{
	0x00, 0x10, 0x34, 0x64, 0x64, 0x78, 0xC4, 0xC4, 0xC4, 0x78, 0x78, 0x20,
	0x34, 0x78, 0x78, 0x78, 0x9C, 0x78, 0x78, 0xC4, 0xC4, 0xC4, 0xC4, 0xC4,
	0xC4, 0x9C, 0xC4, 0x78, 0xC4, 0xC4, 0x78, 0x78, 0x78, 0x20, 0xC4, 0xC4,
	0x78, 0xC4, 0x64, 0x78, 0xC4, 0xC4, 0xC4, 0xC4, 0x9C, 0x78, 0xC4, 0x78,
	0xC4, 0xC4, 0x78, 0xC4, 0xC4, 0xC4, 0xC4, 0xC4, 0x34, 0x78, 0x9C, 0x9C,
	0x78, 0xC4, 0x78, 0x78, 0x78, 0x34, 0x48,
};

static DWORD vertex_shaders[NUMBER_OF_VERTEX_SHADERS];

static void vertex_shaders_create(void)
{
	unsigned long word = 0, total = sizeof(vertex_shader_code) / sizeof(vertex_shader_code[0]);
	int index;

	for (index = 0; index < NUMBER_OF_VERTEX_SHADERS && word < total; index++)
	{
		unsigned long count = vertex_shader_code[word] >> 16;

		D3DDevice_CreateVertexShader(&vertex_shader_declarations[vertex_shader_declaration_offsets[index] / sizeof(DWORD)],
			&vertex_shader_code[word], &vertex_shaders[index], 0);
		word += 1 + count * 4;
		if (word < total && !vertex_shader_code[word])
			word++;
	}
	if (index != NUMBER_OF_VERTEX_SHADERS || word != total)
	{
		printf("FAIL the vertex program table does not hold %d programs\n", NUMBER_OF_VERTEX_SHADERS);
		exit(99);
	}
}

/* ---------- textures */

static unsigned long log2_of(unsigned long value)
{
	unsigned long result = 0;

	while (value > 1)
	{
		value >>= 1;
		result++;
	}
	return result;
}

static unsigned long test_seed = 0x2468ace1UL;

static unsigned long test_random(void)
{
	test_seed = (test_seed * 1664525UL + 1013904223UL) & 0xffffffffUL;
	return test_seed >> 8;
}

/* a swizzled texture with levels, a cube map or a volume */
static D3DBaseTexture *texture_new(DWORD format, unsigned long width, unsigned long height, unsigned long depth,
	unsigned long levels, BOOL cube)
{
	D3DBaseTexture *texture = guest_alloc(sizeof(*texture));
	unsigned long size = width * height * depth * 4 * 2 * (cube ? 6 : 1) + 4096, index;
	unsigned char *texels = guest_alloc(size);

	for (index = 0; index < size; index++)
		texels[index] = (unsigned char)test_random();
	memset(texture, 0, sizeof(*texture));
	texture->Common = 1;
	texture->Data = PLATFORM_VIRTUAL_TO_PHYSICAL(texels);
	texture->Format = (format << D3DFORMAT_FORMAT_SHIFT) | ((depth > 1 ? 3UL : 2UL) << D3DFORMAT_DIMENSION_SHIFT) |
		(levels << D3DFORMAT_MIPMAP_SHIFT) | (log2_of(width) << D3DFORMAT_USIZE_SHIFT) |
		(log2_of(height) << D3DFORMAT_VSIZE_SHIFT) | (log2_of(depth) << D3DFORMAT_PSIZE_SHIFT) |
		(cube ? D3DFORMAT_CUBEMAP : 0);
	return texture;
}

/* a linear texture: any size, addressed in texels */
static D3DBaseTexture *texture_new_linear(DWORD format, unsigned long width, unsigned long height, unsigned long bytes)
{
	D3DBaseTexture *texture = guest_alloc(sizeof(*texture));
	unsigned long pitch = (width * bytes + D3DTEXTURE_PITCH_ALIGNMENT - 1) & ~(unsigned long)(D3DTEXTURE_PITCH_ALIGNMENT - 1);
	unsigned char *texels = guest_alloc(pitch * height);
	unsigned long index;

	for (index = 0; index < pitch * height; index++)
		texels[index] = (unsigned char)test_random();
	memset(texture, 0, sizeof(*texture));
	texture->Common = 1;
	texture->Data = PLATFORM_VIRTUAL_TO_PHYSICAL(texels);
	texture->Format = (format << D3DFORMAT_FORMAT_SHIFT) | (2UL << D3DFORMAT_DIMENSION_SHIFT) | (1UL << D3DFORMAT_MIPMAP_SHIFT);
	texture->Size = ((pitch / D3DTEXTURE_PITCH_ALIGNMENT - 1) << D3DSIZE_PITCH_SHIFT) | ((height - 1) << D3DSIZE_HEIGHT_SHIFT) |
		(width - 1);
	return texture;
}

/* one 2D texture for each stage, told apart by their sizes (a unit's
bound texture then says which stage it took), a cube map and a volume */
static D3DBaseTexture *stage_textures[4];
static const int stage_texture_size[4] = { 64, 32, 16, 8 };
static D3DBaseTexture *cube_texture, *volume_texture;

/* ---------- Direct3D state for a draw */

static struct
{
	D3DVertexBuffer buffer[2];
	unsigned char *data[2];
	unsigned long stride[2];
} streams;

static void streams_initialize(void)
{
	int index;

	for (index = 0; index < 2; index++)
	{
		streams.data[index] = guest_alloc(1024 * 1024);
		streams.buffer[index].Common = 1;
		streams.buffer[index].Data = PLATFORM_VIRTUAL_TO_PHYSICAL(streams.data[index]);
	}
}

static void put_floats(unsigned char *at, int count, ...)
{
	va_list arguments;
	int index;

	va_start(arguments, count);
	for (index = 0; index < count; index++)
	{
		float value = (float)va_arg(arguments, double);

		memcpy(at + index * 4, &value, 4);
	}
	va_end(arguments);
}

static DWORD float_bits(float value)
{
	DWORD bits;

	memcpy(&bits, &value, 4);
	return bits;
}

/* every vertex constant: small numbers, so that whatever a program reads
is finite; the test's draws then set the ones they mean */
static void constants_fill(void)
{
	float constants[XGPU_VERTEX_CONSTANT_COUNT][4];
	int index, component;

	for (index = 0; index < XGPU_VERTEX_CONSTANT_COUNT; index++)
	{
		for (component = 0; component < 4; component++)
			constants[index][component] = 0.05f + (float)((index * 7 + component * 3) % 11) * 0.02f;
	}
	/* (c[-38] and c[-37] are Direct3D's own, the viewport's scale and
	offset, which the device keeps and the game never writes) */
	D3DDevice_SetVertexShaderConstant(-96, constants, XGPU_VERTEX_CONSTANT_BIAS - 38);
	D3DDevice_SetVertexShaderConstant(-36, constants[XGPU_VERTEX_CONSTANT_BIAS - 36],
		XGPU_VERTEX_CONSTANT_COUNT - (XGPU_VERTEX_CONSTANT_BIAS - 36));
}

/* the game's screen projection for a width and height (rasterizer_xbox.c, 2291-2315) */
static void constants_screen(float width, float height)
{
	float constants[5][4] =
	{
		{ 2.0f * (1.0f / width), 0.0f, 0.0f, -1.0f - 1.0f / width },
		{ 0.0f, -2.0f * (1.0f / height), 0.0f, 1.0f + 1.0f / height },
		{ 0.0f, 0.0f, 0.0f, 0.5f },
		{ 0.0f, 0.0f, 0.0f, 1.0f },
		{ 1.0f, 1.0f, 0.0f, 1.0f },
	};

	D3DDevice_SetVertexShaderConstant(-68, constants, 5);
}

/* a view of the world: x and y scaled, depth from 1 to 100 in front */
static const float world_rows[4][4] =
{
	{ 1.2f, 0.0f, 0.1f, 0.3f },
	{ 0.0f, 1.6f, -0.2f, -0.1f },
	{ 0.0f, 0.0f, 100.0f / 99.0f, -100.0f / 99.0f },
	{ 0.0f, 0.0f, 1.0f, 0.0f },
};

static void constants_world(void)
{
	D3DDevice_SetVertexShaderConstant(-96, world_rows, 4);
}

static unsigned char mode_sampler(unsigned long mode)
{
	switch (mode)
	{
	case 0x01: case 0x06: case 0x07: case 0x09: case 0x0f: case 0x10: return 1;
	case 0x02: case 0x0d: return 2;
	case 0x03: case 0x0b: case 0x0c: case 0x0e: case 0x12: return 3;
	default: return 0;
	}
}

/* the pixel state of one of the game's sites, as the game sets it */
static void site_apply(const struct site *site)
{
	DWORD *rs = D3D__RenderState;
	int stage;

	D3DDevice_SetPixelShaderProgram((D3DPIXELSHADERDEF *)&site->shader);
	rs[D3DRS_ALPHABLENDENABLE] = site->blend_enable;
	rs[D3DRS_SRCBLEND] = site->source_blend;
	rs[D3DRS_DESTBLEND] = site->destination_blend;
	rs[D3DRS_BLENDOP] = site->blend_operation;
	rs[D3DRS_COLORWRITEENABLE] = (site->write_color ? D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
		D3DCOLORWRITEENABLE_BLUE : 0) | (site->write_alpha ? D3DCOLORWRITEENABLE_ALPHA : 0);
	rs[D3DRS_ZENABLE] = TRUE;
	rs[D3DRS_ZWRITEENABLE] = site->depth_write;
	rs[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
	rs[D3DRS_ALPHATESTENABLE] = site->alpha_test;
	rs[D3DRS_ALPHAFUNC] = D3DCMP_GREATER;
	rs[D3DRS_ALPHAREF] = site->alpha_reference;
	rs[D3DRS_BLENDCOLOR] = site->blend_color;
	rs[D3DRS_FOGCOLOR] = site->fog_color;
	rs[D3DRS_FOGENABLE] = TRUE;
	rs[D3DRS_FOGTABLEMODE] = D3DFOG_NONE;
	rs[D3DRS_CULLMODE] = D3DCULL_NONE;
	rs[D3DRS_STENCILENABLE] = FALSE;
	for (stage = 0; stage < 4; stage++)
	{
		unsigned long mode = (site->shader.texture_modes >> (5 * stage)) & 0x1f;
		unsigned char sampler = mode_sampler(mode);

		D3D__TextureState[stage][D3DTSS_ALPHAKILL] = (site->alpha_kill >> stage) & 1 ? D3DTALPHAKILL_ENABLE : D3DTALPHAKILL_DISABLE;
		D3D__TextureState[stage][D3DTSS_MINFILTER] = D3DTEXF_LINEAR;
		D3D__TextureState[stage][D3DTSS_MAGFILTER] = D3DTEXF_LINEAR;
		D3D__TextureState[stage][D3DTSS_MIPFILTER] = D3DTEXF_LINEAR;
		D3D__TextureState[stage][D3DTSS_ADDRESSU] = stage & 1 ? D3DTADDRESS_CLAMP : D3DTADDRESS_WRAP;
		D3D__TextureState[stage][D3DTSS_ADDRESSV] = stage & 2 ? D3DTADDRESS_MIRROR : D3DTADDRESS_WRAP;
		D3DDevice_SetTexture(stage, sampler == 1 ? stage_textures[stage] : sampler == 2 ? volume_texture :
			sampler == 3 ? cube_texture : NULL);
	}
}

/* ---------- the plan the test expects: the same question asked of the
compiler, from the site rather than from the device's state */

static struct nv2a_ff_capabilities expected_capabilities;
/* for a plan made for constants: no derived textures (xgpu_fixed.c, plan_get) */
static struct nv2a_ff_capabilities expected_bound_capabilities;

static void expected_capabilities_compute(void)
{
	nv2a_ff_capabilities_level(allowed.units > 1 ? (allowed.combine ? 2 : 1) : 0, &expected_capabilities);
	expected_capabilities.units = (unsigned char)allowed.units;
	expected_capabilities.combine = (unsigned char)allowed.combine;
	expected_capabilities.combine_arb = (unsigned char)allowed.combine_arb;
	expected_capabilities.environment_add = (unsigned char)(allowed.env_add || allowed.combine);
	expected_capabilities.dot3 = (unsigned char)allowed.dot3;
	expected_capabilities.blend_subtract = (unsigned char)allowed.blend_subtract;
	expected_capabilities.blend_minmax = (unsigned char)allowed.blend_minmax;
	expected_capabilities.blend_color = (unsigned char)allowed.blend_color;
	expected_capabilities.fog_coordinate = 0;
	expected_capabilities.cube_maps = 0;
	expected_capabilities.destination_alpha = driver.alpha_bits > 0;
	/* (derived textures that no constant enters: xgpu_fixed.c says why) */
	expected_capabilities.bakes = _nv2a_ff_bakes_static;
	/* (a plan for constants may have them as well: xgpu_fixed.c, plan_get) */
	expected_bound_capabilities = expected_capabilities;
}

/* The plan the device may draw a state with is one of a few. It asks the
compiler without constants when it first meets the state, and, when that
plan is not exact, again with the constants of the draw; and what the
compiler finds with constants depends on what it has kept from the call
before (the plan for every constant of the same state, which leaves it its
whole search for the constants' own). So: the plan of the two calls one
after the other, as the device makes them for a state it has not met; the
plan without constants; and the plan of a call with constants on its own,
as the device makes it for a state it met before. plan and fallback are the
first of these, and after check_against_plan the one the device drew with. */
#define EXPECTED_CANDIDATES 3

struct expected
{
	struct nv2a_ff_state state;
	struct nv2a_ff_constants constants;
	struct nv2a_ff_draw draw;
	struct nv2a_ff_plan plan;
	int fallback;
	struct nv2a_ff_plan candidates[EXPECTED_CANDIDATES];
	int candidate_fallback[EXPECTED_CANDIDATES];
	int candidate_count;
};

static void color_from_argb(unsigned long argb, float *color)
{
	color[0] = (float)((argb >> 16) & 0xff) / 255.0f;
	color[1] = (float)((argb >> 8) & 0xff) / 255.0f;
	color[2] = (float)(argb & 0xff) / 255.0f;
	color[3] = (float)(argb >> 24) / 255.0f;
}

/* the device's rule (xgpu_fixed.c, plan_textureless): a plan that is not
exact and draws none of the state's textures, where the built-in plan would
draw the first, gives way to the built-in plan */
static int expected_textureless(const struct expected *expected, const struct nv2a_ff_plan *plan)
{
	int pass, unit;

	if (plan->status == _nv2a_ff_plan_exact || !(plan->approximations & _nv2a_ff_approximate_dropped_texture) ||
		expected->state.sampler_type[0] != 1 || (expected->state.texture_modes & 0x1f) != 1)
	{
		return 0;
	}
	for (pass = 0; pass < plan->pass_count; pass++)
	{
		for (unit = 0; unit < plan->passes[pass].unit_count; unit++)
		{
			if (plan->passes[pass].units[unit].stage < 4)
				return 0;
		}
	}
	return 1;
}

/* the device's choice (xgpu_fixed.c, plan_get) between the plan without
constants, which is candidate 1, and the one with them that candidate
index holds: the latter where the first is not exact and the compiler made
a plan for the constants; index then holds the choice */
static void expected_choose(struct expected *expected, int index)
{
	const struct nv2a_ff_plan *general = &expected->candidates[1], *bound = &expected->candidates[index];
	int try_bound = expected->candidate_fallback[1] || general->status != _nv2a_ff_plan_exact;

	if (word_in_list(extensions_setting, "simple") || !try_bound || !bound->constants_bound ||
		bound->status == _nv2a_ff_plan_impossible || !bound->pass_count || expected_textureless(expected, bound))
	{
		expected->candidates[index] = *general;
		expected->candidate_fallback[index] = expected->candidate_fallback[1];
	}
	else
	{
		expected->candidate_fallback[index] = 0;
	}
}

static void expected_compute(const struct site *site, struct expected *expected)
{
	const struct site_shader *shader = &site->shader;
	int index;

	memset(expected, 0, sizeof(*expected));
	for (index = 0; index < 8; index++)
	{
		expected->state.combiner_state[D3DRS_PSALPHAINPUTS0 + index] = shader->alpha_inputs[index];
		expected->state.combiner_state[D3DRS_PSALPHAOUTPUTS0 + index] = shader->alpha_outputs[index];
		expected->state.combiner_state[D3DRS_PSRGBINPUTS0 + index] = shader->rgb_inputs[index];
		expected->state.combiner_state[D3DRS_PSRGBOUTPUTS0 + index] = shader->rgb_outputs[index];
		color_from_argb(shader->constant_0[index], expected->constants.c0[index]);
		color_from_argb(shader->constant_1[index], expected->constants.c1[index]);
	}
	expected->state.combiner_state[D3DRS_PSFINALCOMBINERINPUTSABCD] = shader->final_combiner_inputs_abcd;
	expected->state.combiner_state[D3DRS_PSFINALCOMBINERINPUTSEFG] = shader->final_combiner_inputs_efg;
	expected->state.combiner_state[D3DRS_PSCOMPAREMODE] = shader->compare_mode;
	expected->state.combiner_state[D3DRS_PSCOMBINERCOUNT] = shader->combiner_count;
	expected->state.combiner_state[D3DRS_PSDOTMAPPING] = shader->dot_mapping;
	expected->state.combiner_state[D3DRS_PSINPUTTEXTURE] = shader->input_texture;
	expected->state.texture_modes = shader->texture_modes;
	for (index = 0; index < 4; index++)
	{
		unsigned long mode = (shader->texture_modes >> (5 * index)) & 0x1f;

		/* (the device's rule: d3d8_gl.c bind_textures) */
		expected->state.sampler_type[index] = mode == 0x11 ? 1 : mode == 0 || mode == 4 || mode == 5 ? 0 : mode_sampler(mode);
		expected->state.alpha_kill[index] = (unsigned char)((site->alpha_kill >> index) & 1);
	}
	expected->state.alpha_test_function = site->alpha_test ? D3DCMP_GREATER : 0;
	expected->state.fog_enable = 1;
	expected->state.fog_table_mode = 0;
	color_from_argb(shader->final_combiner_constant_0, expected->constants.final_c0);
	color_from_argb(shader->final_combiner_constant_1, expected->constants.final_c1);
	color_from_argb(site->fog_color, expected->constants.fog_color);
	color_from_argb(site->blend_color, expected->constants.blend_color);
	expected->constants.alpha_reference = (float)(site->alpha_reference & 0xff);
	expected->draw.blend_enable = site->blend_enable != 0;
	expected->draw.source_blend = site->blend_enable ? site->source_blend : _nv2a_ff_blend_one;
	expected->draw.destination_blend = site->blend_enable ? site->destination_blend : _nv2a_ff_blend_zero;
	expected->draw.blend_operation = site->blend_enable ? site->blend_operation : _nv2a_ff_blend_operation_add;
	expected->draw.write_color = site->write_color != 0;
	expected->draw.write_alpha = site->write_alpha != 0;
	expected->draw.depth_write = site->depth_write && driver.depth_bits > 0;
	{
		struct nv2a_ff_plan *general = &expected->candidates[1];

		nv2a_ff_compile(&expected->state, NULL, &expected_capabilities, &expected->draw, general);
		nv2a_ff_compile(&expected->state, &expected->constants, &expected_bound_capabilities, &expected->draw,
			&expected->candidates[0]);
		expected->candidate_fallback[1] = general->status == _nv2a_ff_plan_impossible || !general->pass_count ||
			expected_textureless(expected, general) || word_in_list(extensions_setting, "simple");
		expected_choose(expected, 0);
		expected->candidate_count = 2;
	}
	expected->plan = expected->candidates[0];
	expected->fallback = expected->candidate_fallback[0];
}

/* the plan of a call with constants that finds nothing kept from the call
before, as candidate 2 */
static void expected_compute_alone(struct expected *expected)
{
	struct nv2a_ff_state other;
	struct nv2a_ff_plan scratch;

	memset(&other, 0, sizeof(other));
	nv2a_ff_compile(&other, NULL, &expected_capabilities, &expected->draw, &scratch);
	nv2a_ff_compile(&expected->state, &expected->constants, &expected_bound_capabilities, &expected->draw,
		&expected->candidates[2]);
	expected_choose(expected, 2);
	expected->candidate_count = 3;
}

static GLenum expected_environment(const struct nv2a_ff_unit *unit)
{
	switch (unit->environment)
	{
	case _nv2a_ff_environment_replace: return GL_REPLACE;
	case _nv2a_ff_environment_modulate: return GL_MODULATE;
	case _nv2a_ff_environment_decal: return GL_DECAL;
	case _nv2a_ff_environment_blend: return GL_BLEND;
	case _nv2a_ff_environment_add: return allowed.env_add ? GL_ADD : GL_COMBINE;
	default: return GL_COMBINE;
	}
}

/* what the device may turn a pass's blend into where the context lacks
it (xgpu_fixed.c pass_blend); the compiler's plans should not need it */
static int blend_supported(const struct nv2a_ff_pass *pass)
{
	if (!pass->blend_enable)
		return 1;
	if ((pass->blend_operation == GL_FUNC_SUBTRACT || pass->blend_operation == GL_FUNC_REVERSE_SUBTRACT) && !allowed.blend_subtract)
		return 0;
	if ((pass->blend_operation == GL_MIN || pass->blend_operation == GL_MAX) && !allowed.blend_minmax)
		return 0;
	if ((constant_factor(pass->source_blend) || constant_factor(pass->destination_blend)) && !allowed.blend_color)
		return 0;
	return 1;
}

static struct
{
	int draws, exact, approximate, fallback, skipped, passes, unsupported_blends, combine_units, bakes, general;
} sweep;

/* the colours of the screen quad's vertices (screen_quad), alpha in the
top byte */
static const DWORD screen_quad_colors[4] = { 0xffffffffUL, 0x80c04020UL, 0xff20c060UL, 0x403060e0UL };

/* The draw whose passes are being checked is the screen quad through the
HUD's program (vs4): what the vertex program hands the pixel side is then
known, and every pass's primary colour is compared with the plan's
expression of it. v0 is the vertex's colour; v1 and the fog coordinate are
not written by the program (0, 0, 0, 1 and 1). */
static int primary_known;

static void check_against_one_plan(const struct site *site, const struct expected *expected);

/* Compares the OpenGL draws recorded for one Direct3D draw with the plans
the device may have drawn it with: the first that fits is expected's plan
from then on; when none fits, the failures are those against the first. */
static void check_against_plan(const struct site *site, struct expected *expected, int expect_color_write)
{
	int candidate, found = -1;

	(void)expect_color_write;
	for (candidate = 0; candidate < EXPECTED_CANDIDATES && found < 0; candidate++)
	{
		__typeof__(sweep) saved = sweep;

		if (candidate == 2)
			expected_compute_alone(expected);
		expected->plan = expected->candidates[candidate];
		expected->fallback = expected->candidate_fallback[candidate];
		fail_quiet = 1;
		quiet_failures = 0;
		check_against_one_plan(site, expected);
		fail_quiet = 0;
		sweep = saved;
		if (!quiet_failures)
			found = candidate;
	}
	expected->plan = expected->candidates[found < 0 ? 0 : found];
	expected->fallback = expected->candidate_fallback[found < 0 ? 0 : found];
	check_against_one_plan(site, expected);
}

static void check_against_one_plan(const struct site *site, const struct expected *expected)
{
	const struct nv2a_ff_plan *plan = &expected->plan;
	int pass_count = expected->fallback ? 1 : plan->pass_count;
	int pass_index, unit;

	if (rec.draw_count != pass_count)
	{
		fail("%s: %d OpenGL draws for a plan of %d passes", site->name, rec.draw_count, pass_count);
		return;
	}
	sweep.passes += pass_count;
	for (pass_index = 0; pass_index < pass_count; pass_index++)
	{
		const struct rec_state *state = &rec.draws[pass_index].state;
		const struct nv2a_ff_pass *pass = &plan->passes[pass_index];
		int depth_equal = !expected->fallback && pass->depth_equal;
		int write_color = expected->fallback ? site->write_color : pass->write_color;
		int write_alpha = expected->fallback ? site->write_alpha : pass->write_alpha;
		int expected_mask = (write_color && site->write_color ? 7 : 0) | (write_alpha && site->write_alpha ? 8 : 0);

		CHECK(!state->locked || pass_count > 1, "%s: arrays locked for a single pass", site->name);
		CHECK(state->color_mask == expected_mask, "%s pass %d: colour mask %x, the plan's is %x", site->name, pass_index,
			state->color_mask, expected_mask);
		if (driver.depth_bits)
		{
			CHECK(state->depth_test, "%s pass %d: no depth test", site->name, pass_index);
			CHECK(state->depth_function == (GLenum)(depth_equal ? GL_EQUAL : GL_LEQUAL), "%s pass %d: depth function %04x",
				site->name, pass_index, (unsigned)state->depth_function);
			CHECK(state->depth_mask == (site->depth_write && !depth_equal), "%s pass %d: depth mask %d", site->name,
				pass_index, state->depth_mask);
		}
		else
		{
			CHECK(!state->depth_test, "%s pass %d: depth test without a depth buffer", site->name, pass_index);
		}
		if (expected->fallback)
		{
			/* the built-in plan: the first texture times the vertex colour */
			int textured = expected->state.sampler_type[0] == 1 && (site->shader.texture_modes & 0x1f) == 1;

			CHECK(state->units[0].enabled == textured, "%s: the built-in plan has unit 0 %s", site->name,
				state->units[0].enabled ? "on" : "off");
			CHECK(!state->units[0].enabled || state->units[0].mode == GL_MODULATE, "%s: the built-in plan's environment is %04x",
				site->name, (unsigned)state->units[0].mode);
			CHECK(!state->units[1].enabled, "%s: the built-in plan uses a second unit", site->name);
			CHECK(state->alpha_test == (site->alpha_test != 0), "%s: the built-in plan's alpha test", site->name);
			CHECK(state->blend == (site->blend_enable != 0), "%s: the built-in plan's blend", site->name);
			continue;
		}
		/* units */
		for (unit = 0; unit < REC_UNITS; unit++)
		{
			const struct rec_unit *recorded = &state->units[unit];
			const struct nv2a_ff_unit *planned = &pass->units[unit];

			if (unit >= pass->unit_count)
			{
				CHECK(!recorded->enabled, "%s pass %d: unit %d is on, the plan has %d", site->name, pass_index, unit, pass->unit_count);
				continue;
			}
			CHECK(recorded->enabled, "%s pass %d: unit %d is off", site->name, pass_index, unit);
			if (!recorded->enabled)
				continue;
			CHECK(recorded->mode == expected_environment(planned), "%s pass %d unit %d: environment %04x, the plan's is %04x",
				site->name, pass_index, unit, (unsigned)recorded->mode, (unsigned)expected_environment(planned));
			if (planned->environment == _nv2a_ff_environment_combine)
			{
				static const GLenum functions[] = { GL_REPLACE, GL_MODULATE, GL_ADD, GL_ADD_SIGNED, GL_INTERPOLATE, GL_SUBTRACT, GL_DOT3_RGB };
				static const GLenum sources[] = { GL_TEXTURE, GL_CONSTANT, GL_PRIMARY_COLOR, GL_PREVIOUS };
				int side, argument;

				sweep.combine_units++;
				for (side = 0; side < 2; side++)
				{
					const struct nv2a_ff_combine *combine = side ? &planned->alpha : &planned->rgb;
					int arguments = combine->function == _nv2a_ff_combine_replace ? 1 :
						combine->function == _nv2a_ff_combine_interpolate ? 3 : 2;

					CHECK(recorded->combine[side] == functions[combine->function], "%s pass %d unit %d side %d: function %04x",
						site->name, pass_index, unit, side, (unsigned)recorded->combine[side]);
					CHECK(recorded->scale[side] == (combine->scale ? combine->scale : 1), "%s pass %d unit %d side %d: scale %d",
						site->name, pass_index, unit, side, recorded->scale[side]);
					for (argument = 0; argument < arguments; argument++)
					{
						CHECK(recorded->source[side][argument] == sources[combine->source[argument]],
							"%s pass %d unit %d side %d argument %d: source %04x", site->name, pass_index, unit, side, argument,
							(unsigned)recorded->source[side][argument]);
					}
				}
			}
			if (planned->constant_rgb != NV2A_FF_EXPRESSION_NONE || planned->constant_alpha != NV2A_FF_EXPRESSION_NONE)
			{
				float constant[4];
				int channel;

				nv2a_ff_plan_constant(plan, pass_index, unit, &expected->constants, constant);
				for (channel = 0; channel < 4; channel++)
				{
					CHECK(fabsf(recorded->env_color[channel] - constant[channel]) < 1.0e-5f,
						"%s pass %d unit %d: environment colour %g, the plan's is %g", site->name, pass_index, unit,
						recorded->env_color[channel], constant[channel]);
				}
			}
			/* the unit's texture is its stage's (or one made from it), by size */
			if (recorded->texture && recorded->texture < REC_TEXTURES)
			{
				int width = rec.textures[recorded->texture].width[0];

				if (planned->stage < 4 && expected->state.sampler_type[planned->stage] == 1)
				{
					CHECK(width == stage_texture_size[planned->stage], "%s pass %d unit %d: a texture %d wide for stage %d",
						site->name, pass_index, unit, width, planned->stage);
					CHECK(recorded->coordinates, "%s pass %d unit %d: no coordinates for stage %d", site->name, pass_index,
						unit, planned->stage);
				}
				else
				{
					CHECK(width == 1, "%s pass %d unit %d: a texture %d wide where a white texel was due", site->name,
						pass_index, unit, width);
				}
				if (planned->bake != NV2A_FF_BAKE_NONE)
					sweep.bakes++;
			}
		}
		/* alpha test */
		CHECK(state->alpha_test == (pass->alpha_test_function != 0), "%s pass %d: alpha test %d", site->name, pass_index,
			state->alpha_test);
		if (pass->alpha_test_function)
		{
			float reference = expected->constants.alpha_reference / 255.0f;

			if (pass->alpha_reference != NV2A_FF_EXPRESSION_NONE)
			{
				struct nv2a_ff_values values;
				float value[3];

				memset(&values, 0, sizeof(values));
				values.constants = &expected->constants;
				nv2a_ff_plan_evaluate(plan, pass->alpha_reference, &values, value);
				reference = value[0] < 0.0f ? 0.0f : value[0] > 1.0f ? 1.0f : value[0];
			}
			CHECK(state->alpha_function == pass->alpha_test_function && fabsf(state->alpha_reference - reference) < 1.0e-5f,
				"%s pass %d: alpha test %04x against %g, the plan's is %04x against %g", site->name, pass_index,
				(unsigned)state->alpha_function, state->alpha_reference, (unsigned)pass->alpha_test_function, reference);
		}
		/* blend */
		CHECK(state->blend == (pass->blend_enable != 0), "%s pass %d: blend %d", site->name, pass_index, state->blend);
		if (pass->blend_enable)
		{
			if (!blend_supported(pass))
			{
				sweep.unsupported_blends++;
				fail("%s pass %d: the plan's blend %04x/%04x/%04x is not the context's", site->name, pass_index,
					(unsigned)pass->source_blend, (unsigned)pass->destination_blend, (unsigned)pass->blend_operation);
			}
			else
			{
				CHECK(state->blend_source == pass->source_blend && state->blend_destination == pass->destination_blend,
					"%s pass %d: blend %04x/%04x, the plan's is %04x/%04x", site->name, pass_index,
					(unsigned)state->blend_source, (unsigned)state->blend_destination, (unsigned)pass->source_blend,
					(unsigned)pass->destination_blend);
				if (allowed.blend_minmax || allowed.blend_subtract)
				{
					CHECK(state->blend_equation == (GLenum)(pass->blend_operation ? pass->blend_operation : GL_FUNC_ADD),
						"%s pass %d: blend equation %04x", site->name, pass_index, (unsigned)state->blend_equation);
				}
			}
		}
		/* the primary colour: an array or a colour, and one colour where
		the plan says it is the same at every vertex */
		if (pass->primary_rgb == NV2A_FF_EXPRESSION_NONE && pass->primary_alpha == NV2A_FF_EXPRESSION_NONE)
		{
			CHECK(!state->color_array, "%s pass %d: a colour array the plan does not read", site->name, pass_index);
		}
		else if (primary_known)
		{
			static const float unwritten[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
			int vertex, channel, wrong = 0;

			for (vertex = 0; vertex < 4 && !wrong; vertex++)
			{
				struct nv2a_ff_values values;
				float v0[4], color[4];

				color_from_argb(screen_quad_colors[vertex], v0);
				memset(&values, 0, sizeof(values));
				values.constants = &expected->constants;
				values.v0 = v0;
				values.v1 = unwritten;
				values.fog_factor = 1.0f;
				nv2a_ff_plan_primary(plan, pass_index, &values, color);
				for (channel = 0; channel < 4; channel++)
				{
					float wanted = color[channel] < 0.0f ? 0.0f : color[channel] > 1.0f ? 1.0f : color[channel];
					float drawn = state->color_array ? (float)rec.draws[pass_index].colors[vertex][channel] / 255.0f :
						state->color[channel];

					/* (bytes: the vertex's colour is one, and the result another) */
					if (fabsf(drawn - wanted) > 1.01f / 255.0f)
					{
						fail("%s pass %d vertex %d channel %d: primary colour %g (%s), the plan's is %g", site->name, pass_index,
							vertex, channel, drawn, state->color_array ? "from the array" : "one for the draw", wanted);
						wrong = 1;
					}
				}
			}
		}
		else if (pass->primary_is_constant)
		{
			struct nv2a_ff_values values;
			float color[4];
			int channel;

			memset(&values, 0, sizeof(values));
			values.constants = &expected->constants;
			nv2a_ff_plan_primary(plan, pass_index, &values, color);
			CHECK(!state->color_array, "%s pass %d: a colour array for a constant colour", site->name, pass_index);
			for (channel = 0; channel < 4; channel++)
			{
				CHECK(fabsf(state->color[channel] - color[channel]) < 1.0e-5f, "%s pass %d: colour %g, the plan's is %g",
					site->name, pass_index, state->color[channel], color[channel]);
			}
		}
	}
	CHECK(!rec.state.locked, "%s: the arrays are left locked", site->name);
}

/* ---------- the tests */

struct screen_vertex
{
	float position[3];
	float coordinates[2];
	DWORD color;
};

/* the screen's own format, which the HUD's program (vs4) is declared with
(rasterizer_xbox_vertex_shaders_initialize.c): v0 float2, v4 float2, v9
colour; the depth comes from the projection's constants */
struct hud_vertex
{
	float position[2];
	float coordinates[2];
	DWORD color;
};

/* a quad of the screen's vertex format */
static void screen_quad(float x0, float y0, float x1, float y1, unsigned long first)
{
	struct hud_vertex *vertices = (struct hud_vertex *)streams.data[0] + first;
	int index;

	for (index = 0; index < 4; index++)
	{
		vertices[index].position[0] = index == 0 || index == 3 ? x0 : x1;
		vertices[index].position[1] = index < 2 ? y0 : y1;
		vertices[index].coordinates[0] = index == 0 || index == 3 ? 0.0f : 1.0f;
		vertices[index].coordinates[1] = index < 2 ? 0.0f : 1.0f;
		vertices[index].color = screen_quad_colors[index];
	}
	D3DDevice_SetStreamSource(0, &streams.buffer[0], sizeof(struct hud_vertex));
}

static const struct site *wanted_site_result;
static const char *wanted_site_name;
static struct site found_site;

static void site_pick(const struct site *site)
{
	if (!wanted_site_result && strstr(site->name, wanted_site_name))
	{
		found_site = *site;
		wanted_site_result = &found_site;
	}
}

static const struct site *site_find(const char *name)
{
	wanted_site_result = NULL;
	wanted_site_name = name;
	sites_enumerate(site_pick);
	if (!wanted_site_result)
	{
		printf("FAIL no site named %s\n", name);
		exit(99);
	}
	return wanted_site_result;
}

/* a recorded draw's vertex as OpenGL would place it: window x, y (from the
bottom) and depth */
/* the array element of a recorded draw's vertex: the number itself with
indices, counted from the draw's first element without */
static unsigned long rec_draw_vertex(const struct rec_draw *draw, unsigned long vertex)
{
	return draw->indexed ? vertex : vertex + (unsigned long)draw->first;
}

static void window_position(const struct rec_draw *draw, unsigned long vertex, double *x, double *y, double *z)
{
	double position[4];

	rec_vertex_clip(&draw->state, rec_draw_vertex(draw, vertex), position);

	*x = draw->state.viewport[0] + (position[0] / position[3] + 1.0) * 0.5 * draw->state.viewport[2];
	*y = draw->state.viewport[1] + (position[1] / position[3] + 1.0) * 0.5 * draw->state.viewport[3];
	*z = draw->state.depth_range[0] + (position[2] / position[3] + 1.0) * 0.5 *
		(draw->state.depth_range[1] - draw->state.depth_range[0]);
}

static void begin_draw(void)
{
	rec.draw_count = 0;
	rec.expect_range = 0;
}

static void present(void)
{
	D3DDevice_Present(NULL, NULL, NULL, NULL);
}

/* the screen rectangle through the HUD's program: the right pixels, the
right way up, in a window of the screen's size, a larger one and a wider one */
static void test_screen_quad(void)
{
	static const int windows[3][2] = { { 640, 480 }, { 1280, 960 }, { 1000, 600 } };
	const struct site *site = site_find("widgets.c:525 textured");
	struct expected expected;
	int window, index;

	test_name = "screen quad";
	for (window = 0; window < 3; window++)
	{
		/* the picture keeps its shape: scaled by the smaller ratio and centred */
		double scale = (double)windows[window][0] / 640.0 < (double)windows[window][1] / 480.0 ?
			(double)windows[window][0] / 640.0 : (double)windows[window][1] / 480.0;
		double left = (windows[window][0] - 640.0 * scale) / 2.0, bottom = (windows[window][1] - 480.0 * scale) / 2.0;
		static const float corners[4][2] = { { 0.0f, 0.0f }, { 640.0f, 0.0f }, { 640.0f, 480.0f }, { 0.0f, 480.0f } };

		rec.window_width = windows[window][0];
		rec.window_height = windows[window][1];
		/* (the device measures the window after each presentation) */
		present();
		constants_fill();
		constants_screen(640.0f, 480.0f);
		site_apply(site);
		expected_compute(site, &expected);
		D3DDevice_SetVertexShader(vertex_shaders[4]);
		/* The game's projection carries Direct3D's half pixel (the -1 - 1/width
		of constants_screen): a quad from 0 to 640 is at -0.5 to 639.5 on the
		Xbox's screen, whose pixel centres are whole numbers, and so covers
		pixels 0 to 639, which are OpenGL's window from 0 to 640. */
		screen_quad(0.0f, 0.0f, 640.0f, 480.0f, 8);
		begin_draw();
		rec.expect_range = 1;
		rec.expect_first = 0;
		rec.expect_last = 3;
		D3DDevice_DrawVertices(D3DPT_QUADLIST, 8, 4);
		if (rec.draw_count < 1)
		{
			fail("the screen quad was not drawn (window %dx%d)", windows[window][0], windows[window][1]);
			continue;
		}
		primary_known = 1;
		check_against_plan(site, &expected, 1);
		primary_known = 0;
		CHECK(rec.draws[0].mode == GL_QUADS && rec.draws[0].count == 4 && !rec.draws[0].indexed, "the quad list is mode %04x, %d vertices",
			(unsigned)rec.draws[0].mode, (int)rec.draws[0].count);
		for (index = 0; index < 4; index++)
		{
			double x, y, z;
			double expected_x = left + corners[index][0] * scale;
			double expected_y = bottom + (480.0 - corners[index][1]) * scale;

			window_position(&rec.draws[0], (unsigned long)index, &x, &y, &z);
			CHECK(fabs(x - expected_x) < 0.01 && fabs(y - expected_y) < 0.01,
				"window %dx%d corner %d at %.3f,%.3f: expected %.3f,%.3f", windows[window][0], windows[window][1], index,
				x, y, expected_x, expected_y);
			CHECK(fabs(z - 0.5) < 1.0e-6, "corner %d at depth %g: expected 0.5", index, z);
		}
		/* the top of the screen is the top of the window, and the first
		row of the texture is there */
		if (rec.draws[0].state.units[0].enabled && rec.draws[0].state.units[0].coordinates)
		{
			double x, y, z, top_left[4], bottom_right[4];

			rec_vertex_coordinates(&rec.draws[0].state, 0, rec_draw_vertex(&rec.draws[0], 0), top_left);
			rec_vertex_coordinates(&rec.draws[0].state, 0, rec_draw_vertex(&rec.draws[0], 2), bottom_right);
			window_position(&rec.draws[0], 0, &x, &y, &z);
			CHECK(y > bottom + 479.0 * scale && top_left[1] < 0.001, "the screen's top left is at window y %.2f with t %g", y,
				top_left[1]);
			window_position(&rec.draws[0], 2, &x, &y, &z);
			CHECK(y < bottom + 1.0 * scale && bottom_right[1] > 0.0, "the screen's bottom right is at window y %.2f with t %g",
				y, bottom_right[1]);
		}
		/* (the primary colours: check_against_plan, with primary_known) */
	}
	rec.window_width = 640;
	rec.window_height = 480;
	present();
}

/* environment vertices: v0 position, v1..v3 packed normals, v4 texture
coordinates; stream 1 has v7 packed and v8 the lightmap's coordinates */
struct environment_vertex
{
	float position[3];
	DWORD normal, binormal, tangent;
	float coordinates[2];
};

struct lightmap_vertex
{
	DWORD incident;
	short coordinates[2];
};

static const float world_triangle[3][3] = { { -1.0f, -0.5f, 3.0f }, { 2.0f, 0.25f, 10.0f }, { 0.5f, 1.5f, 40.0f } };

static void test_world_triangle(void)
{
	const struct site *site = site_find("environment.c:874 lightmap");
	struct environment_vertex *vertices = (struct environment_vertex *)streams.data[0];
	struct lightmap_vertex *lightmap = (struct lightmap_vertex *)streams.data[1];
	unsigned short *indices = guest_alloc(4096);
	struct expected expected;
	D3DVIEWPORT8 viewport = { 0, 0, 640, 480, 0.0f, 1.0f };
	int index, pass;

	test_name = "world triangle";
	/* the triangle is vertices 100..102 of the streams; the draw names them in another order */
	for (index = 0; index < 3; index++)
	{
		memcpy(vertices[100 + index].position, world_triangle[index], sizeof(world_triangle[index]));
		vertices[100 + index].normal = 0x000003ffUL;
		vertices[100 + index].binormal = 0x001ff800UL;
		vertices[100 + index].tangent = 0x7fc00000UL;
		vertices[100 + index].coordinates[0] = (float)index * 0.5f;
		vertices[100 + index].coordinates[1] = 1.0f - (float)index * 0.25f;
		lightmap[100 + index].incident = 0x000003ffUL;
		lightmap[100 + index].coordinates[0] = (short)(index * 10000);
		lightmap[100 + index].coordinates[1] = (short)(32767 - index * 5000);
	}
	indices[0] = 102;
	indices[1] = 100;
	indices[2] = 101;
	constants_fill();
	constants_world();
	D3DDevice_SetViewport(&viewport);
	site_apply(site);
	/* the bump map is palettized, as the game's are */
	{
		static D3DBaseTexture *bump;
		static D3DPalette palette;

		if (!bump)
		{
			DWORD *colors = guest_alloc(1024);

			for (index = 0; index < 256; index++)
				colors[index] = 0xff8080ffUL;
			palette.Common = 1;
			palette.Data = PLATFORM_VIRTUAL_TO_PHYSICAL(colors);
			bump = texture_new(0x0b, 64, 64, 1, 1, FALSE);
		}
		D3DDevice_SetTexture(0, bump);
		D3DDevice_SetPalette(0, &palette);
	}
	D3D__RenderState[D3DRS_CULLMODE] = D3DCULL_CCW;
	D3D__RenderState[D3DRS_FRONTFACE] = D3DFRONT_CW;
	D3DDevice_SetVertexShader(vertex_shaders[16]);
	D3DDevice_SetStreamSource(0, &streams.buffer[0], sizeof(struct environment_vertex));
	D3DDevice_SetStreamSource(1, &streams.buffer[1], sizeof(struct lightmap_vertex));
	begin_draw();
	rec.expect_range = 1;
	rec.expect_first = 100;
	rec.expect_last = 102;
	D3DDevice_DrawIndexedVertices(D3DPT_TRIANGLELIST, 3, indices);
	if (rec.draw_count < 1)
	{
		fail("the triangle was not drawn");
		return;
	}
	/* the device ranks the lightmap pass's textures itself: whatever else
	the context has units for, the lightmap (stage 2, 16 texels wide) is
	among them, with its coordinates from the second stream */
	(void)expected;
	{
		int unit, lightmap_unit = -1, lightmap_pass = -1;

		for (pass = 0; pass < rec.draw_count; pass++)
		{
			for (unit = 0; unit < REC_UNITS; unit++)
			{
				const struct rec_unit *state = &rec.draws[pass].state.units[unit];

				if (state->enabled && state->texture < REC_TEXTURES && rec.textures[state->texture].width[0] == 16)
				{
					lightmap_unit = unit;
					lightmap_pass = pass;
				}
			}
		}
		/* (the built-in plan of "simple" has the first stage's texture alone) */
		CHECK(lightmap_unit >= 0 || word_in_list(extensions_setting, "simple"), "the lightmap pass is drawn without its lightmap");
		if (lightmap_unit >= 0)
		{
			double coordinates[4];

			/* vs16 passes v8 through: normalised shorts */
			rec_vertex_coordinates(&rec.draws[lightmap_pass].state, lightmap_unit, 101, coordinates);
			CHECK(fabs(coordinates[0] - 10000.0 / 32767.0) < 1.0e-4 && fabs(coordinates[1] - 27767.0 / 32767.0) < 1.0e-4,
				"the lightmap's coordinates of vertex 101 are %g,%g", coordinates[0], coordinates[1]);
		}
	}
	D3DDevice_SetPalette(0, NULL);
	for (pass = 0; pass < rec.draw_count; pass++)
	{
		const struct rec_draw *draw = &rec.draws[pass];

		CHECK(draw->indexed && draw->indices == indices && draw->count == 3 && draw->mode == GL_TRIANGLES,
			"the indexed draw: mode %04x, %d indices, the game's own indices %s", (unsigned)draw->mode, (int)draw->count,
			draw->indices == indices ? "used" : "not used");
		CHECK(draw->state.cull && draw->state.front_face == GL_CW && draw->state.cull_mode == GL_BACK,
			"culling: on %d front %04x cull %04x (the game's front is clockwise and it culls the other side)",
			draw->state.cull, (unsigned)draw->state.front_face, (unsigned)draw->state.cull_mode);
		for (index = 0; index < 3; index++)
		{
			const float *v = world_triangle[index];
			double clip[4], x, y, z, screen_x, screen_y, expected_x, expected_y, expected_z;
			int row;

			/* Direct3D: clip space by the four rows, the viewport's
			screen (pixel centres on whole numbers, rows from the top),
			depth from 0 to 1 */
			for (row = 0; row < 4; row++)
				clip[row] = (double)world_rows[row][0] * v[0] + (double)world_rows[row][1] * v[1] +
					(double)world_rows[row][2] * v[2] + (double)world_rows[row][3];
			screen_x = clip[0] / clip[3] * 320.0 + 320.0;
			screen_y = clip[1] / clip[3] * -240.0 + 240.0;
			expected_x = screen_x + 0.5;
			expected_y = 480.0 - (screen_y + 0.5);
			expected_z = clip[2] / clip[3];
			window_position(draw, 100 + (unsigned long)index, &x, &y, &z);
			CHECK(fabs(x - expected_x) < 0.01 && fabs(y - expected_y) < 0.01 && fabs(z - expected_z) < 1.0e-5,
				"pass %d vertex %d at %.4f,%.4f depth %.6f: expected %.4f,%.4f depth %.6f", pass, index, x, y, z,
				expected_x, expected_y, expected_z);
		}
	}
	/* the passes of one draw share their positions: the same array and matrix */
	for (pass = 1; pass < rec.draw_count; pass++)
	{
		CHECK(rec.draws[pass].state.vertex_pointer == rec.draws[0].state.vertex_pointer &&
			!memcmp(rec.draws[pass].state.projection, rec.draws[0].state.projection, sizeof(rec.draws[0].state.projection)),
			"pass %d has other positions", pass);
	}

	/* a second program over the same vertices (the diffuse texture pass,
	drawn with the depth function EQUAL): the same bits */
	{
		double first[3][4], second_clip[3][4];
		const struct site *second = site_find("environment.c:1681 diffuse textures");

		for (index = 0; index < 3; index++)
			rec_vertex_clip(&rec.draws[0].state, 100 + (unsigned long)index, first[index]);
		site_apply(second);
		D3D__RenderState[D3DRS_ZFUNC] = D3DCMP_EQUAL;
		D3DDevice_SetVertexShader(vertex_shaders[40]);
		begin_draw();
		D3DDevice_DrawIndexedVertices(D3DPT_TRIANGLELIST, 3, indices);
		if (rec.draw_count < 1)
			fail("the second pass over the triangle was not drawn");
		else
		{
			for (index = 0; index < 3; index++)
				rec_vertex_clip(&rec.draws[0].state, 100 + (unsigned long)index, second_clip[index]);
			CHECK(!memcmp(first, second_clip, sizeof(first)), "vs16 and vs40 give different positions for the same vertices");
			CHECK(rec.draws[0].state.depth_function == GL_EQUAL, "the game's depth function EQUAL arrives as %04x",
				(unsigned)rec.draws[0].state.depth_function);
		}
	}
	D3D__RenderState[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
}

/* model vertices: v0 position, v1..v3 packed, v4 normalised shorts, v5 two
node indices, v6 the first node's weight */
/* The environment as the light pipeline draws it (rasterizer_xbox_
environment.c, rasterizer_environment_light_pipeline_draw): one draw through
vs16 with the base map on stage 0 and the lightmap on stage 2, light times
material colour times base map. With two units that is one pass, each
texture on a unit with its own coordinates; with one unit two passes, the
second multiplied into the first where the depths are equal. */
static void test_fused(void)
{
	const struct site *site = site_find("light pipeline, fused, lightmap");
	struct environment_vertex *vertices = (struct environment_vertex *)streams.data[0];
	struct lightmap_vertex *lightmap = (struct lightmap_vertex *)streams.data[1];
	unsigned short *indices = guest_alloc(4096);
	/* c[-84]: the map's scale; c[-83], c[-82]: the rows of the texture animation */
	static const float texture_constants[3][4] =
	{
		{ 1.0f, 1.0f, 1.0f, 1.0f }, { 2.0f, 0.0f, 0.0f, 0.25f }, { 0.0f, 0.5f, 0.0f, -0.125f },
	};
	D3DVIEWPORT8 viewport = { 0, 0, 640, 480, 0.0f, 1.0f };
	struct expected expected;
	int index, pass, unit, base_passes = 0, lightmap_passes = 0;

	test_name = "fused";
	for (index = 0; index < 3; index++)
	{
		memcpy(vertices[200 + index].position, world_triangle[index], sizeof(world_triangle[index]));
		vertices[200 + index].normal = 0x000003ffUL;
		vertices[200 + index].binormal = 0x001ff800UL;
		vertices[200 + index].tangent = 0x7fc00000UL;
		vertices[200 + index].coordinates[0] = 0.25f + (float)index * 0.5f;
		vertices[200 + index].coordinates[1] = 1.0f - (float)index * 0.25f;
		lightmap[200 + index].incident = 0x000003ffUL;
		lightmap[200 + index].coordinates[0] = (short)(1000 + index * 9000);
		lightmap[200 + index].coordinates[1] = (short)(30000 - index * 4000);
	}
	indices[0] = 200;
	indices[1] = 201;
	indices[2] = 202;
	constants_fill();
	constants_world();
	D3DDevice_SetVertexShaderConstant(-84, texture_constants, 3);
	D3DDevice_SetViewport(&viewport);
	site_apply(site);
	expected_compute(site, &expected);
	D3D__RenderState[D3DRS_CULLMODE] = D3DCULL_CCW;
	D3D__RenderState[D3DRS_FRONTFACE] = D3DFRONT_CW;
	D3DDevice_SetVertexShader(vertex_shaders[16]);
	D3DDevice_SetStreamSource(0, &streams.buffer[0], sizeof(struct environment_vertex));
	D3DDevice_SetStreamSource(1, &streams.buffer[1], sizeof(struct lightmap_vertex));
	begin_draw();
	rec.expect_range = 1;
	rec.expect_first = 200;
	rec.expect_last = 202;
	D3DDevice_DrawIndexedVertices(D3DPT_TRIANGLELIST, 3, indices);
	D3D__RenderState[D3DRS_CULLMODE] = D3DCULL_NONE;
	if (rec.draw_count < 1)
	{
		fail("the fused pass was not drawn");
		return;
	}
	if (expected.fallback)
		return;
	check_against_plan(site, &expected, 1);
	CHECK(rec.draw_count == (allowed.units > 1 ? 1 : 2), "%d OpenGL draws with %d unit%s (one pass with two units, two with one)",
		rec.draw_count, allowed.units, allowed.units == 1 ? "" : "s");
	for (pass = 0; pass < rec.draw_count; pass++)
	{
		const struct rec_draw *draw = &rec.draws[pass];

		CHECK(draw->indexed && draw->indices == indices && draw->count == 3 && draw->mode == GL_TRIANGLES,
			"pass %d: mode %04x, %d indices", pass, (unsigned)draw->mode, (int)draw->count);
		/* the material colour is one colour for the draw: no array of them */
		CHECK(!draw->state.color_array, "pass %d has a colour array: the light pipeline's primary colour is a constant", pass);
		for (unit = 0; unit < REC_UNITS; unit++)
		{
			const struct rec_unit *state = &draw->state.units[unit];
			double coordinates[3][4];
			int width;

			if (!state->enabled)
				continue;
			width = state->texture < REC_TEXTURES ? rec.textures[state->texture].width[0] : 0;
			CHECK(state->coordinates && state->coordinate_pointer, "pass %d unit %d has no coordinate array", pass, unit);
			if (!state->coordinates || !state->coordinate_pointer)
				continue;
			for (index = 0; index < 3; index++)
				rec_vertex_coordinates(&draw->state, unit, 200 + (unsigned long)index, coordinates[index]);
			/* (a texture addressed from 0 to 1 has no padding to undo: the
			evaluator's coordinates come through the identity, a lifted draw's
			through the program's own matrix alone) */
			CHECK(rec_lifted(&draw->state) || (state->matrix[0] == 1.0f && state->matrix[5] == 1.0f && state->matrix[12] == 0.0f &&
				state->matrix[13] == 0.0f), "pass %d unit %d: a texture matrix on a texture addressed from 0 to 1", pass, unit);
			if (width == stage_texture_size[0])
			{
				/* the base map: v4 through the animation's rows, times the scale */
				base_passes++;
				for (index = 0; index < 3; index++)
				{
					float s_expected = vertices[200 + index].coordinates[0] * 2.0f + 0.25f;
					float t_expected = vertices[200 + index].coordinates[1] * 0.5f - 0.125f;

					CHECK(fabs(coordinates[index][0] - s_expected) < 1.0e-5 && fabs(coordinates[index][1] - t_expected) < 1.0e-5,
						"pass %d unit %d: the base map's coordinates of vertex %d are %g,%g: expected %g,%g", pass, unit, index,
						coordinates[index][0], coordinates[index][1], s_expected, t_expected);
				}
			}
			else if (width == stage_texture_size[2])
			{
				/* the lightmap: v8 of the second stream, normalised shorts */
				lightmap_passes++;
				for (index = 0; index < 3; index++)
				{
					float s_expected = (float)lightmap[200 + index].coordinates[0] / 32767.0f;
					float t_expected = (float)lightmap[200 + index].coordinates[1] / 32767.0f;

					CHECK(fabs(coordinates[index][0] - s_expected) < 1.0e-5 && fabs(coordinates[index][1] - t_expected) < 1.0e-5,
						"pass %d unit %d: the lightmap's coordinates of vertex %d are %g,%g: expected %g,%g", pass, unit, index,
						coordinates[index][0], coordinates[index][1], s_expected, t_expected);
				}
			}
			else
			{
				fail("pass %d unit %d has a texture %d texels wide: neither the base map nor the lightmap", pass, unit, width);
			}
		}
		for (index = 0; index < 3; index++)
		{
			const float *v = world_triangle[index];
			double clip[4], x, y, z;
			int row;

			for (row = 0; row < 4; row++)
				clip[row] = (double)world_rows[row][0] * v[0] + (double)world_rows[row][1] * v[1] +
					(double)world_rows[row][2] * v[2] + (double)world_rows[row][3];
			window_position(draw, 200 + (unsigned long)index, &x, &y, &z);
			CHECK(fabs(x - (clip[0] / clip[3] * 320.0 + 320.5)) < 0.01 && fabs(y - (480.0 - (clip[1] / clip[3] * -240.0 + 240.5))) < 0.01 &&
				fabs(z - clip[2] / clip[3]) < 1.0e-5, "pass %d vertex %d at %.4f,%.4f depth %.6f", pass, index, x, y, z);
		}
	}
	CHECK(base_passes == 1 && lightmap_passes == 1, "the base map is on %d unit%s and the lightmap on %d: each once", base_passes,
		base_passes == 1 ? "" : "s", lightmap_passes);
	if (rec.draw_count == 2)
	{
		const struct rec_state *second = &rec.draws[1].state;

		CHECK(second->blend && ((second->blend_source == GL_DST_COLOR && second->blend_destination == GL_ZERO) ||
			(second->blend_source == GL_ZERO && second->blend_destination == GL_SRC_COLOR)),
			"the second pass is not multiplied into the first: blend %d %04x,%04x", second->blend,
			(unsigned)second->blend_source, (unsigned)second->blend_destination);
		CHECK(second->depth_function == GL_EQUAL && !second->depth_mask, "the second pass: depth function %04x, mask %d",
			(unsigned)second->depth_function, second->depth_mask);
		CHECK(rec.draws[1].state.vertex_pointer == rec.draws[0].state.vertex_pointer &&
			!memcmp(rec.draws[1].state.projection, rec.draws[0].state.projection, sizeof(rec.draws[0].state.projection)),
			"the second pass has other positions");
	}
}

#pragma pack(push, 1)
struct model_vertex
{
	float position[3];
	DWORD normal, binormal, tangent;
	short coordinates[2];
	unsigned char nodes[2];
	short weight;
};
#pragma pack(pop)

static void test_model(void)
{
	static const int programs[] = { 10, 17, 9, 13, 31, 47 };
	const struct site *site = site_find("models.c:1011");
	struct model_vertex *vertices = (struct model_vertex *)streams.data[0];
	unsigned short *indices = guest_alloc(4096);
	unsigned long index;
	unsigned program;
	float palette[30][4];

	test_name = "model";
	for (index = 0; index < 40; index++)
	{
		vertices[index].position[0] = (float)(index % 7) * 0.1f - 0.3f;
		vertices[index].position[1] = (float)(index % 5) * 0.1f - 0.2f;
		vertices[index].position[2] = 5.0f + (float)(index % 3);
		vertices[index].normal = 0x7fc00000UL;
		vertices[index].binormal = 0x000003ffUL;
		vertices[index].tangent = 0x001ff800UL;
		vertices[index].coordinates[0] = (short)(index * 700);
		vertices[index].coordinates[1] = (short)(index * -300);
		vertices[index].nodes[0] = (unsigned char)((index % 3) * 3);
		vertices[index].nodes[1] = (unsigned char)(((index + 1) % 3) * 3);
		vertices[index].weight = (short)(32767 - index * 500);
	}
	for (index = 0; index < 60; index++)
		indices[index] = (unsigned short)(5 + (index * 7) % 30);
	constants_fill();
	constants_world();
	/* a palette of node matrices near the identity, from c[0] on */
	for (index = 0; index < 30; index++)
	{
		palette[index][0] = index % 3 == 0 ? 1.0f : 0.01f;
		palette[index][1] = index % 3 == 1 ? 1.0f : 0.02f;
		palette[index][2] = index % 3 == 2 ? 1.0f : 0.03f;
		palette[index][3] = 0.1f * (float)(index % 3);
	}
	D3DDevice_SetVertexShaderConstant(0, palette, 30);
	site_apply(site);
	D3DDevice_SetStreamSource(0, &streams.buffer[0], sizeof(struct model_vertex));
	for (program = 0; program < sizeof(programs) / sizeof(programs[0]); program++)
	{
		D3DDevice_SetVertexShader(vertex_shaders[programs[program]]);
		/* (a new frame: the device makes so many plans for constants in one,
		and the model's plan for all constants has no texture in it) */
		present();
		begin_draw();
		rec.expect_range = 1;
		rec.expect_first = 5;
		rec.expect_last = 34;
		D3DDevice_DrawIndexedVertices(D3DPT_TRIANGLELIST, 60, indices);
		CHECK(rec.draw_count >= 1, "the model was not drawn with vs%d", programs[program]);
		if (rec.draw_count)
		{
			/* (the plan may start with a pass of the vertex colours alone;
			the base map, or a texture derived from it, is in one of them) */
			int pass, unit, base_map = 0;

			for (pass = 0; pass < rec.draw_count; pass++)
			{
				for (unit = 0; unit < REC_UNITS; unit++)
				{
					const struct rec_unit *state = &rec.draws[pass].state.units[unit];

					if (state->enabled && state->texture < REC_TEXTURES &&
						rec.textures[state->texture].width[0] == stage_texture_size[0])
					{
						base_map = 1;
					}
				}
			}
			CHECK(base_map || word_in_list(extensions_setting, "simple"), "vs%d: no pass of the model has its base map",
				programs[program]);
			CHECK(rec.draws[0].indexed && rec.draws[0].indices == indices && rec.draws[0].count == 60,
				"vs%d: the model's own indices are not drawn", programs[program]);
		}
	}
}

/* a particle quad through each of the dynamic programs, and strips and fans */
static void test_particles(void)
{
	static const struct
	{
		int program;
		D3DPRIMITIVETYPE type;
		unsigned long count;
		GLenum mode;
	} draws[] =
	{
		{ 65, D3DPT_QUADLIST, 8, GL_QUADS }, { 2, D3DPT_QUADLIST, 4, GL_QUADS }, { 56, D3DPT_TRIANGLESTRIP, 6, GL_TRIANGLE_STRIP },
		{ 12, D3DPT_TRIANGLEFAN, 5, GL_TRIANGLE_FAN }, { 1, D3DPT_TRIANGLELIST, 6, GL_TRIANGLES },
		{ 0, D3DPT_LINELIST, 4, GL_LINES }, { 0, D3DPT_LINESTRIP, 5, GL_LINE_STRIP },
	};
	const struct site *site = site_find("transparent_geometry.c:1747 effect");
	unsigned long index;
	unsigned draw;

	test_name = "particles";
	for (index = 0; index < 64; index++)
	{
		/* the dynamic format: v0 float3, v4 float2, v9 colour (as the screen's) */
		struct screen_vertex *vertex = (struct screen_vertex *)streams.data[0] + index;

		vertex->position[0] = (float)(index % 4) - 1.5f;
		vertex->position[1] = (float)(index / 4 % 4) - 1.5f;
		vertex->position[2] = 4.0f + (float)(index % 9);
		vertex->coordinates[0] = (float)(index & 1);
		vertex->coordinates[1] = (float)((index >> 1) & 1);
		vertex->color = 0x80c0e0ffUL - (DWORD)index * 0x01010101UL;
	}
	constants_fill();
	constants_world();
	site_apply(site);
	D3DDevice_SetStreamSource(0, &streams.buffer[0], sizeof(struct screen_vertex));
	for (draw = 0; draw < sizeof(draws) / sizeof(draws[0]); draw++)
	{
		D3DDevice_SetVertexShader(vertex_shaders[draws[draw].program]);
		begin_draw();
		rec.expect_range = 1;
		rec.expect_first = 0;
		rec.expect_last = draws[draw].count - 1;
		D3DDevice_DrawVertices(draws[draw].type, 20, draws[draw].count);
		CHECK(rec.draw_count >= 1, "vs%d type %d was not drawn", draws[draw].program, (int)draws[draw].type);
		if (rec.draw_count)
		{
			CHECK(rec.draws[0].mode == draws[draw].mode && rec.draws[0].count == (GLsizei)draws[draw].count && !rec.draws[0].indexed,
				"vs%d type %d: mode %04x, %d vertices", draws[draw].program, (int)draws[draw].type, (unsigned)rec.draws[0].mode,
				(int)rec.draws[0].count);
		}
	}
}

/* immediate mode: the secondary target's quad (rasterizer_xbox.c:2289-2331) */
static void test_immediate(void)
{
	const struct site *site = site_find("rasterizer_xbox.c:2318");
	struct expected expected;
	unsigned int mxcsr_before, mxcsr_after;
	unsigned short x87_before = 0, x87_after = 0, x87_odd;
	double x, y, z;

	test_name = "immediate";
	constants_fill();
	constants_screen(640.0f, 480.0f);
	site_apply(site);
	expected_compute(site, &expected);
	D3DDevice_SetVertexShader(vertex_shaders[4]);
	begin_draw();
	/* odd floating-point modes going in: rounding down, flush to zero;
	x87 at single precision, rounding up */
	mxcsr_before = (_mm_getcsr() & ~0x6000u) | 0x2000u | 0x8000u;
	_mm_setcsr(mxcsr_before);
	__asm__ volatile ("fnstcw %0" : "=m" (x87_before));
	x87_odd = (unsigned short)((x87_before & ~0x0f00) | 0x0800);
	__asm__ volatile ("fldcw %0" : : "m" (x87_odd));
	D3DDevice_Begin(D3DPT_TRIANGLEFAN);
	D3DDevice_SetVertexData2s(D3DVSDE_SPECULAR, 0, 1);
	D3DDevice_SetVertexData2s(D3DVSDE_POSITION, 100, 50);
	D3DDevice_SetVertexData2s(D3DVSDE_SPECULAR, 1, 1);
	D3DDevice_SetVertexData2s(D3DVSDE_POSITION, 300, 50);
	D3DDevice_SetVertexData2s(D3DVSDE_SPECULAR, 1, 0);
	D3DDevice_SetVertexData2s(D3DVSDE_POSITION, 300, 200);
	D3DDevice_SetVertexData2s(D3DVSDE_SPECULAR, 0, 0);
	D3DDevice_SetVertexData2s(D3DVSDE_POSITION, 100, 200);
	D3DDevice_End();
	mxcsr_after = _mm_getcsr();
	__asm__ volatile ("fnstcw %0" : "=m" (x87_after));
	__asm__ volatile ("fldcw %0" : : "m" (x87_before));
	_mm_setcsr(mxcsr_before & ~0xa000u);
	CHECK((mxcsr_after & 0xffc0u) == (mxcsr_before & 0xffc0u), "MXCSR was %04x before the draw and is %04x after", mxcsr_before, mxcsr_after);
	CHECK(x87_after == x87_odd, "the x87 control word was %04x before the draw and is %04x after", x87_odd, x87_after);
	if (rec.draw_count < 1)
	{
		fail("the immediate fan was not drawn");
		return;
	}
	check_against_plan(site, &expected, 1);
	CHECK(rec.draws[0].mode == GL_TRIANGLE_FAN && rec.draws[0].count == 4 && !rec.draws[0].indexed, "the fan is mode %04x, %d vertices",
		(unsigned)rec.draws[0].mode, (int)rec.draws[0].count);
	window_position(&rec.draws[0], 0, &x, &y, &z);
	/* (as the screen quad: the projection takes the half pixel off and the
	device puts it back) */
	CHECK(fabs(x - 100.0) < 0.01 && fabs(y - (480.0 - 50.0)) < 0.01, "the fan's first vertex is at %.3f,%.3f: expected 100,430", x, y);
	window_position(&rec.draws[0], 2, &x, &y, &z);
	CHECK(fabs(x - 300.0) < 0.01 && fabs(y - (480.0 - 200.0)) < 0.01, "the fan's third vertex is at %.3f,%.3f: expected 300,280", x, y);
}

/* every pixel state of the game, through two programs: the plans the
device draws with are the compiler's, whatever was drawn before */
static int sweep_program;
static int sweep_only_light;

static void sweep_site(const struct site *site)
{
	struct expected expected;

	if (sweep_only_light && !site->light)
		return;
	/* (the two sites whose textures the game ranks: the device ranks them
	itself and may keep others) */
	if (site->stage_rank[0] | site->stage_rank[1] | site->stage_rank[2] | site->stage_rank[3])
	{
		sweep.skipped++;
		site_apply(site);
		begin_draw();
		D3DDevice_DrawVertices(D3DPT_QUADLIST, 0, 4);
		CHECK(rec.draw_count >= 1, "%s: not drawn", site->name);
		present();
		return;
	}
	site_apply(site);
	expected_compute(site, &expected);
	begin_draw();
	rec.expect_range = 1;
	rec.expect_first = 0;
	rec.expect_last = 3;
	D3DDevice_DrawVertices(D3DPT_QUADLIST, 0, 4);
	sweep.draws++;
	if (expected.fallback)
		sweep.fallback++;
	else if (expected.plan.status == _nv2a_ff_plan_exact)
		sweep.exact++;
	else
		sweep.approximate++;
	check_against_plan(site, &expected, 1);
	/* the constants' budget of plans a frame (xgpu_fixed.c) must not run out here */
	present();
}

static void test_sweep(void)
{
	clock_t before = clock();

	test_name = "sweep";
	memset(&sweep, 0, sizeof(sweep));
	constants_fill();
	constants_screen(640.0f, 480.0f);
	D3DDevice_SetVertexShader(vertex_shaders[4]);
	screen_quad(100.0f, 100.0f, 200.0f, 200.0f, 0);
	sweep_program = 4;
	primary_known = 1;
	sites_enumerate(sweep_site);
	primary_known = 0;
	printf("# sweep, screen program: %d states drawn (%d exact, %d approximate, %d built-in), %d passes, %d combine units, "
		"%d derived textures; %d states with ranks left to the device; %.1f s\n", sweep.draws, sweep.exact, sweep.approximate,
		sweep.fallback, sweep.passes, sweep.combine_units, sweep.bakes, sweep.skipped,
		(double)(clock() - before) / CLOCKS_PER_SEC);

	/* again, with a program that writes both colours, fog and four sets of
	coordinates (the transparent shaders', on environment vertices): the
	plans are cached now */
	before = clock();
	memset(&sweep, 0, sizeof(sweep));
	{
		struct environment_vertex *vertices = (struct environment_vertex *)streams.data[0];
		int index;

		for (index = 0; index < 4; index++)
		{
			vertices[index].position[0] = index == 0 || index == 3 ? -1.0f : 1.0f;
			vertices[index].position[1] = index < 2 ? -1.0f : 1.0f;
			vertices[index].position[2] = 6.0f;
			vertices[index].normal = 0x7fc00000UL;
			vertices[index].binormal = 0x000003ffUL;
			vertices[index].tangent = 0x001ff800UL;
			vertices[index].coordinates[0] = (float)(index & 1);
			vertices[index].coordinates[1] = (float)(index >> 1);
		}
	}
	constants_world();
	D3DDevice_SetVertexShader(vertex_shaders[24]);
	D3DDevice_SetStreamSource(0, &streams.buffer[0], sizeof(struct environment_vertex));
	sweep_program = 24;
	sites_enumerate(sweep_site);
	printf("# sweep, transparent program: %d states drawn, %d passes; %.1f s\n", sweep.draws, sweep.passes,
		(double)(clock() - before) / CLOCKS_PER_SEC);
}

/* textures: what each kind of Xbox texture becomes */
static void test_textures(void)
{
	static const struct
	{
		const char *name;
		DWORD format;
		unsigned long width, height, levels;
		int linear;
		unsigned long bytes;
		/* 0 for any */
		int level_count;
	} cases[] =
	{
		{ "A8R8G8B8 64x64, whole chain", 0x06, 64, 64, 7, 0, 4, 7 },
		{ "A8R8G8B8 128x32, 3 levels", 0x06, 128, 32, 3, 0, 4, 8 },
		{ "R5G6B5 256x256, 1 level", 0x05, 256, 256, 1, 0, 2, 1 },
		{ "A4R4G4B4 32x32, 2 levels", 0x04, 32, 32, 2, 0, 2, 6 },
		{ "A1R5G5B5 16x16, whole chain", 0x02, 16, 16, 5, 0, 2, 5 },
		{ "DXT1 256x256, 4 levels", 0x0c, 256, 256, 4, 0, 0, 0 },
		{ "DXT3 64x64, whole chain", 0x0e, 64, 64, 7, 0, 0, 7 },
		{ "DXT5 512x512, whole chain", 0x0f, 512, 512, 10, 0, 0, 0 },
		{ "P8 64x64, whole chain", 0x0b, 64, 64, 7, 0, 1, 7 },
		{ "L8 32x32", 0x00, 32, 32, 1, 0, 1, 1 },
		{ "AL8 32x32, whole chain", 0x01, 32, 32, 6, 0, 1, 6 },
		{ "A8L8 32x32", 0x1a, 32, 32, 1, 0, 2, 1 },
		{ "A8 16x16", 0x19, 16, 16, 1, 0, 1, 1 },
		{ "X8R8G8B8 1024x1024, whole chain", 0x07, 1024, 1024, 11, 0, 4, 0 },
		{ "linear A8R8G8B8 640x480", 0x12, 640, 480, 1, 1, 4, 1 },
		{ "linear A4R4G4B4 100x30", 0x1d, 100, 30, 1, 1, 2, 1 },
		{ "linear X8R8G8B8 720x300", 0x1e, 720, 300, 1, 1, 4, 1 },
	};
	/* a state that samples its texture as it is (the base map multiplied into
	the frame): the plan binds the texture itself, not one derived from it */
	const struct site *site = site_find("light pipeline, map multiplied");
	struct expected expected;
	D3DPalette palette;
	DWORD *colors = guest_alloc(1024);
	unsigned index;

	test_name = "textures";
	for (index = 0; index < 256; index++)
		colors[index] = 0xff000000UL | (index * 0x010203UL);
	expected_compute(site, &expected);
	CHECK(expected.fallback || (expected.plan.passes[0].unit_count >= 1 && expected.plan.passes[0].units[0].stage == 0 &&
		expected.plan.passes[0].units[0].bake == NV2A_FF_BAKE_NONE),
		"the plan of %s does not bind its texture as it is: the checks below need a state that does", site->name);
	palette.Common = 1;
	palette.Data = PLATFORM_VIRTUAL_TO_PHYSICAL(colors);
	palette.Lock = 0;
	constants_fill();
	constants_screen(640.0f, 480.0f);
	D3DDevice_SetVertexShader(vertex_shaders[4]);
	screen_quad(10.0f, 10.0f, 74.0f, 74.0f, 0);
	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++)
	{
		D3DBaseTexture *texture = cases[index].linear ?
			texture_new_linear(cases[index].format, cases[index].width, cases[index].height, cases[index].bytes) :
			texture_new(cases[index].format, cases[index].width, cases[index].height, 1, cases[index].levels, FALSE);
		const struct rec_texture *recorded;
		const struct rec_unit *unit;
		int compressed = cases[index].format == 0x0c || cases[index].format == 0x0e || cases[index].format == 0x0f;
		int maximum = driver.maximum_texture_size;
		int padded_width = 1, padded_height = 1, expected_width, expected_height, level, levels = 0;
		GLint internal;

		site_apply(site);
		D3DDevice_SetTexture(0, texture);
		D3DDevice_SetPalette(0, cases[index].format == 0x0b ? &palette : NULL);
		begin_draw();
		D3DDevice_DrawVertices(D3DPT_QUADLIST, 0, 4);
		if (rec.draw_count < 1 || !rec.draws[0].state.units[0].enabled)
		{
			fail("%s: not drawn with a texture", cases[index].name);
			continue;
		}
		unit = &rec.draws[0].state.units[0];
		recorded = &rec.textures[unit->texture];
		while (padded_width < (int)cases[index].width) padded_width *= 2;
		while (padded_height < (int)cases[index].height) padded_height *= 2;
		expected_width = padded_width;
		expected_height = padded_height;
		while (expected_width > maximum || expected_height > maximum)
		{
			expected_width = expected_width > 1 ? expected_width / 2 : 1;
			expected_height = expected_height > 1 ? expected_height / 2 : 1;
		}
		CHECK(recorded->width[0] == expected_width && recorded->height[0] == expected_height, "%s: first level %dx%d, expected %dx%d",
			cases[index].name, recorded->width[0], recorded->height[0], expected_width, expected_height);
		for (level = 0; level < REC_LEVELS && recorded->width[level]; level++)
			levels++;
		internal = recorded->internal[0];
		if (compressed && allowed.s3tc)
		{
			CHECK(recorded->compressed, "%s: decoded although the context has S3TC", cases[index].name);
		}
		else
		{
			CHECK(!recorded->compressed, "%s: compressed", cases[index].name);
			/* luminance formats as they are where the combine extension reads
			them as the Xbox does, and as red, green, blue, alpha with all
			their bits without it; 16 bits unless 32 are asked for */
			if (cases[index].format == 0x00)
				CHECK(internal == (allowed.combine ? GL_LUMINANCE8 : GL_RGBA8), "%s: internal format %04x", cases[index].name, (unsigned)internal);
			else if (cases[index].format == 0x01)
				CHECK(internal == (allowed.combine ? GL_INTENSITY8 : GL_RGBA8), "%s: internal format %04x", cases[index].name, (unsigned)internal);
			else if (cases[index].format == 0x1a || cases[index].format == 0x19)
				CHECK(internal == (allowed.combine ? GL_LUMINANCE8_ALPHA8 : GL_RGBA8), "%s: internal format %04x", cases[index].name, (unsigned)internal);
			else if (!strcmp(texture_bits_setting, "32"))
				CHECK(internal == GL_RGBA8, "%s: internal format %04x with 32-bit textures", cases[index].name, (unsigned)internal);
			else
				CHECK(internal == GL_RGB5 || internal == GL_RGB5_A1 || internal == GL_RGBA4, "%s: internal format %04x with 16-bit textures",
					cases[index].name, (unsigned)internal);
			/* (an opaque texture keeps an alpha of its own where OpenGL 1.1's REPLACE would take the fragment's) */
			if (cases[index].format == 0x05 || cases[index].format == 0x07 || cases[index].format == 0x1e)
				CHECK(internal == (allowed.combine ? GL_RGB5 : GL_RGB5_A1) || internal == GL_RGBA8, "%s: an opaque texture as %04x",
					cases[index].name, (unsigned)internal);
		}
		/* a mipmapped texture is sampled with its mip levels (rec_draw
		checked that it is complete); one level is not */
		if (cases[index].levels > 1 && !(compressed && allowed.s3tc))
		{
			CHECK(recorded->min_filter == GL_LINEAR_MIPMAP_LINEAR && recorded->width[levels - 1] == 1 && recorded->height[levels - 1] == 1,
				"%s: filter %04x, %d levels, the last %dx%d", cases[index].name, (unsigned)recorded->min_filter, levels,
				recorded->width[levels - 1], recorded->height[levels - 1]);
		}
		if (cases[index].levels == 1)
			CHECK(recorded->min_filter == GL_LINEAR && levels == 1, "%s: filter %04x, %d levels", cases[index].name,
				(unsigned)recorded->min_filter, levels);
		/* a linear texture's coordinates are texels: the matrix makes them
		parts of the padded texture */
		if (cases[index].linear)
		{
			CHECK(fabsf(unit->matrix[0] - 1.0f / (float)padded_width) < 1.0e-7f && fabsf(unit->matrix[5] - 1.0f / (float)padded_height) < 1.0e-7f,
				"%s: texture matrix scale %g,%g, expected 1/%d,1/%d", cases[index].name, unit->matrix[0], unit->matrix[5],
				padded_width, padded_height);
		}
		else
		{
			CHECK(unit->matrix[0] == 1.0f && unit->matrix[5] == 1.0f && unit->matrix[12] == 0.0f && unit->matrix[13] == 0.0f,
				"%s: a texture matrix on a texture addressed from 0 to 1", cases[index].name);
		}
		/* drawn again: nothing is uploaded */
		{
			int images = rec.texture_images + rec.compressed_images;

			begin_draw();
			D3DDevice_DrawVertices(D3DPT_QUADLIST, 0, 4);
			CHECK(rec.texture_images + rec.compressed_images == images, "%s: uploaded again for the next draw", cases[index].name);
			/* and after the game writes its texels, once more */
			watch_generation++;
			begin_draw();
			D3DDevice_DrawVertices(D3DPT_QUADLIST, 0, 4);
			CHECK(rec.texture_images + rec.compressed_images > images, "%s: not uploaded again after its texels changed", cases[index].name);
		}
	}
	D3DDevice_SetPalette(0, NULL);
}

/* clears and viewports: rows from the bottom of the window */
static void test_frame(void)
{
	static const D3DRECT rectangle = { 10, 20, 110, 70 };
	D3DVIEWPORT8 viewport = { 32, 16, 320, 240, 0.25f, 0.75f };
	D3DVIEWPORT8 whole = { 0, 0, 640, 480, 0.0f, 1.0f };
	const struct site *site = site_find("widgets.c:434 untextured");
	int clears;

	test_name = "frame";
	clears = rec.clears;
	D3DDevice_Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0xff102030UL, 1.0f, 0);
	CHECK(rec.clears == clears + 1 && !rec.last_clear_scissored, "the whole clear: %d clears, scissored %d", rec.clears - clears,
		rec.last_clear_scissored);
	CHECK((rec.last_clear_mask & GL_COLOR_BUFFER_BIT) && (!driver.depth_bits == !(rec.last_clear_mask & GL_DEPTH_BUFFER_BIT)) &&
		(!driver.stencil_bits == !(rec.last_clear_mask & GL_STENCIL_BUFFER_BIT)), "the whole clear's mask is %x",
		(unsigned)rec.last_clear_mask);
	clears = rec.clears;
	D3DDevice_Clear(1, &rectangle, D3DCLEAR_TARGET, 0, 1.0f, 0);
	CHECK(rec.clears == clears + 1 && rec.last_clear_scissored && rec.last_clear_scissor[0] == 10 && rec.last_clear_scissor[1] == 410 &&
		rec.last_clear_scissor[2] == 100 && rec.last_clear_scissor[3] == 50, "a clear of 10,20..110,70 is scissored to %d,%d %dx%d",
		(int)rec.last_clear_scissor[0], (int)rec.last_clear_scissor[1], (int)rec.last_clear_scissor[2], (int)rec.last_clear_scissor[3]);
	CHECK(!rec.state.scissor_test, "the scissor test is left on after a clear");

	constants_fill();
	constants_screen(640.0f, 480.0f);
	site_apply(site);
	D3DDevice_SetVertexShader(vertex_shaders[4]);
	screen_quad(40.0f, 40.0f, 80.0f, 80.0f, 0);
	D3DDevice_SetViewport(&viewport);
	D3D__RenderState[D3DRS_ZWRITEENABLE] = TRUE;
	begin_draw();
	D3DDevice_DrawVertices(D3DPT_QUADLIST, 0, 4);
	if (rec.draw_count)
	{
		const struct rec_state *state = &rec.draws[0].state;

		CHECK(state->viewport[0] == 32 && state->viewport[1] == 480 - 16 - 240 && state->viewport[2] == 320 && state->viewport[3] == 240,
			"viewport 32,16 320x240 arrives as %d,%d %dx%d", (int)state->viewport[0], (int)state->viewport[1],
			(int)state->viewport[2], (int)state->viewport[3]);
		CHECK(state->depth_range[0] == 0.25 && state->depth_range[1] == 0.75, "depth range %g..%g", state->depth_range[0],
			state->depth_range[1]);
		/* (the clear before left the depth mask on: the draw's own state is still set) */
		CHECK(state->depth_mask == (driver.depth_bits > 0), "depth writes %d", state->depth_mask);
	}
	else
	{
		fail("the draw in a viewport was not drawn");
	}
	/* z bias: a slope factor, and steps of this depth buffer */
	D3DDevice_SetRenderState_ZBias(8);
	begin_draw();
	D3DDevice_DrawVertices(D3DPT_QUADLIST, 0, 4);
	if (rec.draw_count)
	{
		const struct rec_state *state = &rec.draws[0].state;
		float expected_units = driver.depth_bits >= 24 || !driver.depth_bits ? -8.0f : -2.0f;

		CHECK(state->offset_fill && state->polygon_offset[0] == -2.0f && state->polygon_offset[1] == expected_units,
			"z bias 8: polygon offset %d %g, %g; expected -2, %g", state->offset_fill, state->polygon_offset[0], state->polygon_offset[1],
			expected_units);
	}
	D3DDevice_SetRenderState_ZBias(0);
	D3DDevice_SetViewport(&whole);
}

/* what the game draws into its own targets is left out, and what would
sample them; a draw that samples the back buffer gets a copy of it */
static void test_targets(void)
{
	const struct site *site = site_find("widgets.c:525 textured");
	D3DSurface *back_buffer = NULL, *depth_buffer = NULL;
	D3DSurface off_screen;
	D3DBaseTexture *as_texture = guest_alloc(sizeof(*as_texture));
	int clears, copies, reports;

	test_name = "targets";
	D3DDevice_GetBackBuffer(0, 0, &back_buffer);
	D3DDevice_GetDepthStencilSurface(&depth_buffer);
	d3d8_surface_initialize(&off_screen, D3DFMT_LIN_A8R8G8B8, 128, 128);
	constants_fill();
	constants_screen(640.0f, 480.0f);
	site_apply(site);
	D3DDevice_SetVertexShader(vertex_shaders[4]);
	screen_quad(40.0f, 40.0f, 80.0f, 80.0f, 0);

	D3DDevice_SetRenderTarget(&off_screen, NULL);
	clears = rec.clears;
	reports = log_count("off-screen target");
	D3DDevice_Clear(0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
	begin_draw();
	D3DDevice_DrawVertices(D3DPT_QUADLIST, 0, 4);
	D3DDevice_DrawVertices(D3DPT_QUADLIST, 0, 4);
	CHECK(rec.clears == clears && rec.draw_count == 0, "an off-screen target: %d clears and %d draws reached OpenGL", rec.clears - clears,
		rec.draw_count);
	CHECK(log_count("off-screen target") == reports + 1, "the off-screen target is reported %d times", log_count("off-screen target") - reports);

	/* back to the screen: a draw that samples that target is left out */
	D3DDevice_SetRenderTarget(back_buffer, depth_buffer);
	memset(as_texture, 0, sizeof(*as_texture));
	as_texture->Common = 1;
	as_texture->Data = off_screen.Data;
	as_texture->Format = off_screen.Format | (1UL << D3DFORMAT_MIPMAP_SHIFT);
	as_texture->Size = off_screen.Size;
	D3DDevice_SetTexture(0, as_texture);
	begin_draw();
	D3DDevice_DrawVertices(D3DPT_QUADLIST, 0, 4);
	CHECK(rec.draw_count == 0, "a draw that samples an off-screen target reached OpenGL");

	/* the back buffer as a texture: a copy, its rows from the bottom */
	as_texture->Data = back_buffer->Data;
	as_texture->Format = back_buffer->Format | (1UL << D3DFORMAT_MIPMAP_SHIFT);
	as_texture->Size = back_buffer->Size;
	copies = rec.copies;
	begin_draw();
	D3DDevice_DrawVertices(D3DPT_QUADLIST, 0, 4);
	if (driver.maximum_texture_size < 1024)
	{
		CHECK(rec.draw_count == 0 && rec.copies == copies && log_count("does not fit a texture") == 1,
			"a frame larger than the context's textures: %d draws, %d copies", rec.draw_count, rec.copies - copies);
	}
	else if (rec.draw_count && rec.draws[0].state.units[0].enabled)
	{
		const struct rec_unit *unit = &rec.draws[0].state.units[0];
		const struct rec_texture *texture = &rec.textures[unit->texture];

		CHECK(rec.copies == copies + 1 && texture->copies, "the back buffer was copied %d times", rec.copies - copies);
		CHECK(texture->width[0] == 1024 && texture->height[0] == 512, "the copy is %dx%d, expected 1024x512", texture->width[0],
			texture->height[0]);
		/* texel 0,0 is the picture's top left: s = 0, t = 480/512; texel 640,480 the bottom right: s = 640/1024, t = 0 */
		CHECK(fabsf(unit->matrix[0] * 640.0f - 0.625f) < 1.0e-6f && fabsf(unit->matrix[13] - 0.9375f) < 1.0e-6f &&
			fabsf(unit->matrix[5] * 480.0f + unit->matrix[13]) < 1.0e-6f, "the copy's texture matrix: %g %g offset %g %g",
			unit->matrix[0], unit->matrix[5], unit->matrix[12], unit->matrix[13]);
		/* drawn into since: the next draw that samples it copies again; one that does not, does not */
		begin_draw();
		D3DDevice_DrawVertices(D3DPT_QUADLIST, 0, 4);
		CHECK(rec.copies == copies + 2, "the back buffer was not copied again after being drawn into");
	}
	else
	{
		fail("a draw that samples the back buffer was not drawn with a texture");
	}
	D3DDevice_SetTexture(0, stage_textures[0]);
	back_buffer->Common--;
	depth_buffer->Common--;
}

/* a small off-screen target (the motion sensor's 64x64) is drawn in the
window's lower left corner and sampled from a copy of it */
static void test_corner(void)
{
	const struct site *site = site_find("widgets.c:525 textured");
	D3DSurface *back_buffer = NULL, *depth_buffer = NULL;
	D3DSurface sensor;
	D3DBaseTexture *as_texture = guest_alloc(sizeof(*as_texture));
	D3DVIEWPORT8 viewport = { 0, 0, 64, 64, 0.0f, 1.0f }, screen = { 0, 0, 640, 480, 0.0f, 1.0f };
	int clears, copies, draws, pass;

	test_name = "corner";
	D3DDevice_GetBackBuffer(0, 0, &back_buffer);
	D3DDevice_GetDepthStencilSurface(&depth_buffer);
	d3d8_surface_initialize(&sensor, D3DFMT_LIN_A8R8G8B8, 64, 64);
	constants_fill();
	constants_screen(64.0f, 64.0f);
	site_apply(site);
	D3D__RenderState[D3DRS_ZENABLE] = FALSE;
	D3DDevice_SetVertexShader(vertex_shaders[4]);
	screen_quad(8.0f, 8.0f, 56.0f, 56.0f, 0);

	/* a window without alpha leaves the small target out, as the larger
	ones (its pictures' alpha would be 1: xgpu_fixed.c, corner_target) */
	if (!driver.alpha_bits)
	{
		D3DDevice_SetRenderTarget(&sensor, NULL);
		D3DDevice_SetViewport(&viewport);
		clears = rec.clears;
		copies = rec.copies;
		begin_draw();
		D3DDevice_Clear(0, NULL, D3DCLEAR_TARGET, 0xff102030, 1.0f, 0);
		D3DDevice_DrawVertices(D3DPT_QUADLIST, 0, 4);
		CHECK(rec.clears == clears && rec.copies == copies && rec.draw_count == 0,
			"a window without alpha: the small target's clear and draw made %d clears, %d copies, %d draws",
			rec.clears - clears, rec.copies - copies, rec.draw_count);
		D3DDevice_SetRenderTarget(back_buffer, depth_buffer);
		D3DDevice_SetViewport(&screen);
		D3D__RenderState[D3DRS_ZENABLE] = TRUE;
		back_buffer->Common--;
		depth_buffer->Common--;
		present();
		return;
	}

	/* the clear and the draws land in the corner: the window's pixels
	there are saved first */
	D3DDevice_SetRenderTarget(&sensor, NULL);
	D3DDevice_SetViewport(&viewport);
	clears = rec.clears;
	copies = rec.copies;
	D3DDevice_Clear(0, NULL, D3DCLEAR_TARGET, 0xff102030, 1.0f, 0);
	CHECK(rec.copies == copies + 1, "the corner was saved %d times before the clear", rec.copies - copies);
	CHECK(rec.clears == clears + 1 && rec.last_clear_scissored && rec.last_clear_scissor[0] == 0 && rec.last_clear_scissor[1] == 0 &&
		rec.last_clear_scissor[2] == 64 && rec.last_clear_scissor[3] == 64 && rec.last_clear_mask == GL_COLOR_BUFFER_BIT,
		"the target's clear: %d clears, scissor %d,%d %dx%d mask %lx", rec.clears - clears, (int)rec.last_clear_scissor[0],
		(int)rec.last_clear_scissor[1], (int)rec.last_clear_scissor[2], (int)rec.last_clear_scissor[3], (unsigned long)rec.last_clear_mask);
	begin_draw();
	D3DDevice_DrawVertices(D3DPT_QUADLIST, 0, 4);
	CHECK(rec.draw_count >= 1, "the draw into the small target did not reach OpenGL");
	for (pass = 0; pass < rec.draw_count; pass++)
	{
		const struct rec_state *state = &rec.draws[pass].state;
		double x, y, z;

		CHECK(state->viewport[0] == 0 && state->viewport[1] == 0 && state->viewport[2] == 64 && state->viewport[3] == 64,
			"pass %d into the small target: viewport %d,%d %dx%d", pass, (int)state->viewport[0], (int)state->viewport[1],
			(int)state->viewport[2], (int)state->viewport[3]);
		CHECK(!state->depth_test, "pass %d into the small target tests depth", pass);
		window_position(&rec.draws[pass], 0, &x, &y, &z);
		CHECK(fabs(x - 8.0) < 0.01 && fabs(y - 56.0) < 0.01, "the quad's first corner is at %.2f,%.2f in the corner: expected 8,56", x, y);
	}
	CHECK(rec.copies == copies + 1, "the corner was copied again during the draws");

	/* back to the screen: the first draw there copies the corner to the
	target's texture and puts the window's pixels back (a draw of its
	own); a draw that samples the target gets that texture */
	D3DDevice_SetRenderTarget(back_buffer, depth_buffer);
	D3DDevice_SetViewport(&screen);
	constants_screen(640.0f, 480.0f);
	screen_quad(100.0f, 100.0f, 164.0f, 164.0f, 0);
	memset(as_texture, 0, sizeof(*as_texture));
	as_texture->Common = 1;
	as_texture->Data = sensor.Data;
	as_texture->Format = sensor.Format | (1UL << D3DFORMAT_MIPMAP_SHIFT);
	as_texture->Size = sensor.Size;
	D3DDevice_SetTexture(0, as_texture);
	begin_draw();
	draws = rec.draw_count;
	D3DDevice_DrawVertices(D3DPT_QUADLIST, 0, 4);
	CHECK(rec.copies == copies + 2, "the corner was copied %d times in all (saved, then to the target's texture)", rec.copies - copies);
	CHECK(rec.draw_count == 2 || rec.draw_count == 3, "%d OpenGL draws: the corner put back, then the draw that samples the target",
		rec.draw_count);
	if (rec.draw_count >= 2)
	{
		const struct rec_draw *restore = &rec.draws[0], *sampled = &rec.draws[rec.draw_count - 1];
		const struct rec_unit *unit = &sampled->state.units[0];

		CHECK(restore->state.viewport[2] == 64 && restore->state.viewport[3] == 64 && !restore->state.depth_test &&
			!restore->state.blend && restore->state.units[0].enabled && restore->state.units[0].mode == GL_REPLACE &&
			rec.textures[restore->state.units[0].texture].copies == 1,
			"the restoring draw: viewport %dx%d, depth test %d, blend %d, unit 0 %s mode %04x", (int)restore->state.viewport[2],
			(int)restore->state.viewport[3], restore->state.depth_test, restore->state.blend,
			restore->state.units[0].enabled ? "on" : "off", (unsigned)restore->state.units[0].mode);
		CHECK(unit->enabled && unit->texture < REC_TEXTURES && rec.textures[unit->texture].copies == 1 &&
			rec.textures[unit->texture].width[0] == 64 && rec.textures[unit->texture].height[0] == 64,
			"the sampling draw's texture: %s, %d copies, %dx%d", unit->enabled ? "on" : "off",
			unit->texture < REC_TEXTURES ? rec.textures[unit->texture].copies : -1,
			unit->texture < REC_TEXTURES ? rec.textures[unit->texture].width[0] : 0,
			unit->texture < REC_TEXTURES ? rec.textures[unit->texture].height[0] : 0);
		CHECK(unit->texture != restore->state.units[0].texture, "the target's texture is the saved corner");
		/* a linear texture is addressed in texels, and the target's rows from
		the top: t becomes 1 - t/64 over the 64 used rows of a 64-row texture */
		CHECK(fabsf(unit->matrix[5] + 1.0f / 64.0f) < 1.0e-6f && fabsf(unit->matrix[13] - 1.0f) < 1.0e-6f &&
			fabsf(unit->matrix[0] - 1.0f / 64.0f) < 1.0e-6f,
			"the target texture's matrix: %g %g offset %g %g", unit->matrix[0], unit->matrix[5], unit->matrix[12], unit->matrix[13]);
	}
	/* a second small target drawn while the first is sampled (the sun's
	glow goes back and forth between two): each keeps its picture */
	{
		D3DSurface second;
		GLuint first_picture = rec.draw_count >= 2 ? rec.draws[rec.draw_count - 1].state.units[0].texture : 0;

		d3d8_surface_initialize(&second, D3DFMT_LIN_A8R8G8B8, 64, 64);
		D3DDevice_SetRenderTarget(&second, NULL);
		D3DDevice_SetViewport(&viewport);
		constants_screen(64.0f, 64.0f);
		screen_quad(8.0f, 8.0f, 56.0f, 56.0f, 0);
		copies = rec.copies;
		begin_draw();
		D3DDevice_DrawVertices(D3DPT_QUADLIST, 0, 4);
		CHECK(rec.copies == copies + 1 && rec.draw_count >= 1, "the second target: %d copies (the corner saved), %d draws",
			rec.copies - copies, rec.draw_count);
		if (rec.draw_count >= 1)
		{
			const struct rec_state *state = &rec.draws[0].state;

			CHECK(state->viewport[2] == 64 && state->viewport[3] == 64 && state->units[0].enabled &&
				state->units[0].texture == first_picture, "the draw into the second target: viewport %dx%d, unit 0 %s texture %u (the first's picture is %u)",
				(int)state->viewport[2], (int)state->viewport[3], state->units[0].enabled ? "on" : "off",
				(unsigned)state->units[0].texture, (unsigned)first_picture);
		}
		D3DDevice_SetRenderTarget(back_buffer, depth_buffer);
		D3DDevice_SetViewport(&screen);
		constants_screen(640.0f, 480.0f);
		screen_quad(100.0f, 100.0f, 164.0f, 164.0f, 0);
		as_texture->Data = second.Data;
		as_texture->Size = second.Size;
		D3DDevice_SetTexture(0, as_texture);
		begin_draw();
		D3DDevice_DrawVertices(D3DPT_QUADLIST, 0, 4);
		CHECK(rec.copies == copies + 2 && rec.draw_count >= 2, "sampling the second target: %d copies, %d draws", rec.copies - copies,
			rec.draw_count);
		if (rec.draw_count >= 2)
		{
			const struct rec_unit *unit = &rec.draws[rec.draw_count - 1].state.units[0];

			CHECK(unit->enabled && unit->texture != first_picture && unit->texture < REC_TEXTURES && rec.textures[unit->texture].copies == 1,
				"the second target's picture: unit 0 %s texture %u, %d copies", unit->enabled ? "on" : "off", (unsigned)unit->texture,
				unit->texture < REC_TEXTURES ? rec.textures[unit->texture].copies : -1);
		}
	}
	(void)draws;
	D3DDevice_SetTexture(0, stage_textures[0]);
	D3D__RenderState[D3DRS_ZENABLE] = TRUE;
	back_buffer->Common--;
	depth_buffer->Common--;
	present();
}

/* visibility tests: counted from the depth buffer when the frame ends */
static void test_visibility(void)
{
	const struct site *site = site_find("widgets.c:434 untextured");
	UINT result = 12345;
	int reads;

	test_name = "visibility";
	constants_fill();
	constants_screen(640.0f, 480.0f);
	site_apply(site);
	D3D__RenderState[D3DRS_COLORWRITEENABLE] = 0;
	D3D__RenderState[D3DRS_ZWRITEENABLE] = FALSE;
	D3DDevice_SetVertexShader(vertex_shaders[4]);
	/* the flare's rectangle (rasterizer_xbox_widgets.c): 32 by 32 pixels at depth 0.5; the depth buffer is nearer
	than that left of column 116 */
	rec.depth_column = 116;
	rec.depth_left = 0.25f;
	rec.depth_right = 1.0f;
	reads = rec.depth_reads;
	D3DDevice_BeginVisibilityTest();
	D3DDevice_Begin(D3DPT_TRIANGLEFAN);
	D3DDevice_SetVertexData4f(0, 100.0f, 100.0f, 0.5f, 1.0f);
	D3DDevice_SetVertexData4f(0, 132.0f, 100.0f, 0.5f, 1.0f);
	D3DDevice_SetVertexData4f(0, 132.0f, 132.0f, 0.5f, 1.0f);
	D3DDevice_SetVertexData4f(0, 100.0f, 132.0f, 0.5f, 1.0f);
	D3DDevice_End();
	D3DDevice_EndVisibilityTest(77);
	present();
	D3DDevice_GetVisibilityTestResult(77, &result, NULL);
	if (driver.depth_bits)
	{
		CHECK(rec.depth_reads > reads, "the depth buffer was not read for the visibility test");
		/* the rectangle covers window columns 100.5..132.5: half of it is right of column 116 */
		CHECK(result >= 448 && result <= 576, "a half hidden rectangle of 1024 pixels counts %u", (unsigned)result);
	}
	else
	{
		CHECK(result == 1024, "without a depth buffer a rectangle of 1024 pixels counts %u", (unsigned)result);
	}
	/* a slot that was never tested answers 0 */
	D3DDevice_GetVisibilityTestResult(78, &result, NULL);
	CHECK(result == 0, "an unused slot answers %u", (unsigned)result);

	/* a context that cannot be read: the whole rectangle counts, and the log says so once */
	if (driver.depth_bits)
	{
		D3DDevice_BeginVisibilityTest();
		D3DDevice_Begin(D3DPT_TRIANGLEFAN);
		D3DDevice_SetVertexData4f(0, 100.0f, 100.0f, 0.5f, 1.0f);
		D3DDevice_SetVertexData4f(0, 132.0f, 100.0f, 0.5f, 1.0f);
		D3DDevice_SetVertexData4f(0, 132.0f, 132.0f, 0.5f, 1.0f);
		D3DDevice_SetVertexData4f(0, 100.0f, 132.0f, 0.5f, 1.0f);
		D3DDevice_End();
		D3DDevice_EndVisibilityTest(77);
		rec.depth_column = -1;
		present();
		D3DDevice_GetVisibilityTestResult(77, &result, NULL);
		CHECK(result == 1024, "a wholly visible rectangle of 1024 pixels counts %u", (unsigned)result);

		/* and a wholly hidden one behind a depth buffer that cannot be
		read: counted as visible, said once, and not read again */
		rec.depth_column = 10000;
		rec.depth_unreadable = 1;
		D3DDevice_BeginVisibilityTest();
		D3DDevice_Begin(D3DPT_TRIANGLEFAN);
		D3DDevice_SetVertexData4f(0, 100.0f, 100.0f, 0.5f, 1.0f);
		D3DDevice_SetVertexData4f(0, 132.0f, 100.0f, 0.5f, 1.0f);
		D3DDevice_SetVertexData4f(0, 132.0f, 132.0f, 0.5f, 1.0f);
		D3DDevice_SetVertexData4f(0, 100.0f, 132.0f, 0.5f, 1.0f);
		D3DDevice_End();
		D3DDevice_EndVisibilityTest(79);
		present();
		D3DDevice_GetVisibilityTestResult(79, &result, NULL);
		CHECK(result == 1024 && log_count("depth buffer cannot be read") == 1,
			"behind an unreadable depth buffer a rectangle of 1024 pixels counts %u", (unsigned)result);
		reads = rec.depth_reads;
		D3DDevice_BeginVisibilityTest();
		D3DDevice_Begin(D3DPT_TRIANGLEFAN);
		D3DDevice_SetVertexData4f(0, 100.0f, 100.0f, 0.5f, 1.0f);
		D3DDevice_SetVertexData4f(0, 132.0f, 100.0f, 0.5f, 1.0f);
		D3DDevice_SetVertexData4f(0, 132.0f, 132.0f, 0.5f, 1.0f);
		D3DDevice_SetVertexData4f(0, 100.0f, 132.0f, 0.5f, 1.0f);
		D3DDevice_End();
		D3DDevice_EndVisibilityTest(79);
		present();
		CHECK(rec.depth_reads == reads, "the depth buffer is read again after it could not be");
		rec.depth_unreadable = 0;
		rec.pending_error = GL_NO_ERROR;
	}
	rec.depth_column = 0;
}

/* ---------- */

static const struct driver drivers[] =
{
	{ "plain", "1.1.0", "", 110, 1, 1024, 16, 0, 0, 0 },
	{ "ext", "1.2.1 test",
		"GL_ARB_multitexture GL_EXT_texture_env_combine GL_EXT_texture_env_add GL_EXT_texture_env_dot3 GL_EXT_bgra "
		"GL_EXT_packed_pixels GL_EXT_texture_compression_s3tc GL_ARB_texture_compression GL_EXT_compiled_vertex_array "
		"GL_EXT_blend_color GL_EXT_blend_minmax GL_EXT_blend_subtract GL_EXT_texture_edge_clamp GL_SGIS_generate_mipmap "
		"GL_SGIS_texture_lod GL_EXT_texture_object", 120, 2, 256, 16, 0, 0, 0 },
	{ "core", "1.5.0 test", "GL_EXT_texture_compression_s3tc GL_EXT_compiled_vertex_array GL_ARB_texture_env_combine "
		"GL_ARB_multitexture", 150, 4, 2048, 24, 8, 8, 1 },
};

int main(int argc, char **argv)
{
	D3DPRESENT_PARAMETERS parameters;
	D3DDevice *device = NULL;
	int argument, index;

	setvbuf(stdout, NULL, _IONBF, 0);
	driver = drivers[0];
	for (argument = 1; argument < argc; argument++)
	{
		if (!strcmp(argv[argument], "-v"))
			verbose = 1;
		else if (!strcmp(argv[argument], "-s"))
			statistics = 1;
		else if (!strcmp(argv[argument], "16") || !strcmp(argv[argument], "32"))
			texture_bits_setting = argv[argument];
		else if (argument == 1)
		{
			for (index = 0; index < 3; index++)
			{
				if (!strcmp(drivers[index].name, argv[argument]))
					driver = drivers[index];
			}
		}
		else if (argument == 2)
			extensions_setting = argv[argument];
	}
	allowed_compute();
	expected_capabilities_compute();
	printf("# driver %s (OpenGL %s), debug.gl1_extensions \"%s\", %s-bit textures: %d unit%s%s%s%s%s%s%s%s\n", driver.name,
		driver.version, extensions_setting, texture_bits_setting, allowed.units, allowed.units == 1 ? "" : "s",
		allowed.combine ? ", combine" : "", allowed.combine_arb ? " (ARB)" : "", allowed.env_add ? ", add" : "",
		allowed.s3tc ? ", S3TC" : "", allowed.bgra ? ", BGRA" : "", allowed.packed ? ", packed pixels" : "",
		allowed.cva ? ", compiled vertex arrays" : "");

	rec_reset_state();
	rec.window_width = 640;
	rec.window_height = 480;
	guest_initialize();
	streams_initialize();

	memset(&parameters, 0, sizeof(parameters));
	parameters.BackBufferWidth = 640;
	parameters.BackBufferHeight = 480;
	Direct3D_CreateDevice(0, D3DDEVTYPE_HAL, NULL, 0, &parameters, &device);
	if (!log_count("renderer: gl1") || !log_count("gl1: OpenGL"))
	{
		printf("FAIL the device did not start the light renderer:\n%s", log_text);
		return 1;
	}
	{
		const char *line = strstr(log_text, "gl1: OpenGL");

		printf("# %.*s\n", (int)strcspn(line, "\n"), line);
	}
	vertex_shaders_create();
	for (index = 0; index < 4; index++)
		stage_textures[index] = texture_new(0x06, (unsigned long)stage_texture_size[index],
			(unsigned long)stage_texture_size[index], 1, 1, FALSE);
	cube_texture = texture_new(0x06, 16, 16, 1, 1, TRUE);
	volume_texture = texture_new(0x06, 16, 16, 16, 1, FALSE);
	D3DDevice_Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0, 1.0f, 0);

	test_screen_quad();
	test_world_triangle();
	test_fused();
	test_model();
	test_particles();
	test_immediate();
	test_frame();
	test_textures();
	test_targets();
	test_corner();
	test_visibility();
	test_sweep();

	test_name = "end";
	CHECK(rec.swaps > 0, "nothing was presented");
	if (statistics)
	{
		const char *line = log_text, *last = NULL;

		while ((line = strstr(line, " frames, ")) != NULL)
			last = line++;
		CHECK(last != NULL, "debug.gpu_stats is on and no statistics were logged");
		if (last)
		{
			while (last > log_text && last[-1] != '\n')
				last--;
			printf("# %d lines of statistics, the last: %.*s\n", log_count(" frames, "), (int)strcspn(last, "\n"), last);
		}
		printf("# %d approximate plans described\n", log_count(" without: "));
	}
	CHECK(!rec.state.locked, "the arrays are left locked");
	if (verbose || failures)
		printf("# the device's log:\n%s", log_text);
	printf("# no fixed-function form: %d states, only a plan without textures: %d; %d swaps, %d texture images, %d compressed, %d copies of the back buffer, %d depth reads\n",
		log_count("no fixed-function form"), log_count("only a plan without textures"), rec.swaps, rec.texture_images, rec.compressed_images, rec.copies, rec.depth_reads);
	printf("%s: %d failure%s\n", failures ? "FAILED" : "passed", failures, failures == 1 ? "" : "s");
	return failures > 255 ? 255 : failures;
}
