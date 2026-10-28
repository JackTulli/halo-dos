/*
COMBINER_FF_SITES.C

The pixel states of the game, for the test of nv2a_combiner_ff.c
(combiner_ff_test.c includes this file).

Every place in source/rasterizer/xbox that builds a pixel shader is
written out again here, with the numbers it uses, in the order of the
file names and lines given in each site's name. Where the state depends
on tag data or on debugging options, the variants the code can build are
generated: every case of a switch, both sides of a flag; the two
preprocessors of the transparent shaders, whose input is the artists', are
enumerated where that is small (chicago shaders of one and two maps) and
sampled with a fixed random sequence where it is not (chicago shaders of
three and four maps, generic shaders).

Each state comes with the framebuffer state its site draws with (blend,
colour write mask, alpha test, depth writes) as far as the site sets it
itself, and with a mark for the passes the light pipeline keeps (design
study, 3.2 and 3.7).

The states the light pipeline sets in place of the others
(rasterizer_light_pipeline: the environment's fused pass, its alpha-tested
and self-illumination passes, the water's and the camouflage's tints, the
glass on two textures) come last, each with the plan its author built it
for, which the test holds the compiler to.
*/

/* ---------- the game's pixel shader definition and its macros
(rasterizer_xbox_pixel_shader.h, xdk_d3d8.h, xdk_pdb.h) */

struct site_shader
{
	unsigned long alpha_inputs[8];
	unsigned long final_combiner_inputs_abcd;
	unsigned long final_combiner_inputs_efg;
	unsigned long constant_0[8];
	unsigned long constant_1[8];
	unsigned long alpha_outputs[8];
	unsigned long rgb_inputs[8];
	unsigned long compare_mode;
	unsigned long final_combiner_constant_0;
	unsigned long final_combiner_constant_1;
	unsigned long rgb_outputs[8];
	unsigned long combiner_count;
	unsigned long texture_modes;
	unsigned long dot_mapping;
	unsigned long input_texture;
};

#define PS_COMBINERINPUTS(a, b, c, d) \
	(((unsigned long)(a) << 24) | ((unsigned long)(b) << 16) | ((unsigned long)(c) << 8) | (unsigned long)(d))
#define PS_COMBINEROUTPUTS(ab, cd, mux_sum, flags) \
	(((unsigned long)(flags) << 12) | ((unsigned long)(mux_sum) << 8) | ((unsigned long)(ab) << 4) | (unsigned long)(cd))
#define PS_COMBINERCOUNT(count, flags) (((unsigned long)(flags) << 8) | (unsigned long)(count))
#define PS_TEXTUREMODES(t0, t1, t2, t3) \
	(((unsigned long)(t3) << 15) | ((unsigned long)(t2) << 10) | ((unsigned long)(t1) << 5) | (unsigned long)(t0))
#define PS_DOTMAPPING(t0, t1, t2, t3) (((unsigned long)(t3) << 8) | ((unsigned long)(t2) << 4) | (unsigned long)(t1))
#define PS_INPUTTEXTURE(t0, t1, t2, t3) (((unsigned long)(t3) << 20) | ((unsigned long)(t2) << 16) | (unsigned long)(t1))

enum
{
	PS_CHANNEL_ALPHA = 16,

	PS_COMBINERCOUNT_UNIQUE_C0 = 16,
	PS_COMBINERCOUNT_UNIQUE_C1 = 256,

	PS_COMBINEROUTPUT_IDENTITY = 0,
	PS_COMBINEROUTPUT_AB_DOT_PRODUCT = 2,
	PS_COMBINEROUTPUT_CD_DOT_PRODUCT = 1,

	PS_DOTMAPPING_ZERO_TO_ONE = 0,
	PS_DOTMAPPING_MINUS1_TO_1_D3D = 1,

	PS_INPUTMAPPING_UNSIGNED_INVERT = 32,
	PS_INPUTMAPPING_EXPAND_NORMAL = 64,
	PS_INPUTMAPPING_SIGNED_NEGATE = 224,

	PS_REGISTER_ZERO = 0,
	PS_REGISTER_DISCARD = 0,
	PS_REGISTER_C0 = 1,
	PS_REGISTER_C1 = 2,
	PS_REGISTER_FOG = 3,
	PS_REGISTER_V0 = 4,
	PS_REGISTER_V1 = 5,
	PS_REGISTER_T0 = 8,
	PS_REGISTER_T1 = 9,
	PS_REGISTER_T2 = 10,
	PS_REGISTER_T3 = 11,
	PS_REGISTER_R0 = 12,
	PS_REGISTER_R1 = 13,
	PS_REGISTER_EF_PROD = 15,
	PS_REGISTER_ONE = 32,
	PS_REGISTER_ONE_HALF = 160,

	PS_TEXTUREMODES_NONE = 0,
	PS_TEXTUREMODES_PROJECT2D = 1,
	PS_TEXTUREMODES_CUBEMAP = 3,
	PS_TEXTUREMODES_DOT_ST = 9,
	PS_TEXTUREMODES_DOT_RFLCT_DIFF = 11,
	PS_TEXTUREMODES_DOT_RFLCT_SPEC = 12,
	PS_TEXTUREMODES_DOTPRODUCT = 17
};

/* D3DBLEND_*, D3DBLENDOP_* */
enum
{
	BLEND_ZERO = 0,
	BLEND_ONE = 1,
	BLEND_SRCCOLOR = 0x300,
	BLEND_INVSRCCOLOR = 0x301,
	BLEND_SRCALPHA = 0x302,
	BLEND_INVSRCALPHA = 0x303,
	BLEND_DESTALPHA = 0x304,
	BLEND_INVDESTALPHA = 0x305,
	BLEND_DESTCOLOR = 0x306,
	BLEND_INVDESTCOLOR = 0x307,
	BLEND_CONSTANTCOLOR = 0x8001,
	BLEND_INVCONSTANTCOLOR = 0x8002,
	BLEND_CONSTANTALPHA = 0x8003,

	BLENDOP_ADD = 0x8006,
	BLENDOP_MIN = 0x8007,
	BLENDOP_MAX = 0x8008,
	BLENDOP_REVSUBTRACT = 0x800b
};

/* the shader framebuffer blend functions, in the order of
rasterizer_set_framebuffer_blend_function's tables (rasterizer_xbox.c) */
enum
{
	_blend_function_alpha_blend = 0,
	_blend_function_multiply,
	_blend_function_double_multiply,
	_blend_function_add,
	_blend_function_reverse_subtract,
	_blend_function_min,
	_blend_function_max,
	_blend_function_alpha_multiply_add,
	NUMBER_OF_BLEND_FUNCTIONS
};

static const char *blend_function_names[NUMBER_OF_BLEND_FUNCTIONS] =
{
	"alpha", "multiply", "double", "add", "subtract", "min", "max", "alpha-add"
};

struct site
{
	char name[128];
	struct site_shader shader;
	/* a pass the light pipeline keeps */
	int light;
	/* _site_expect_*: for the light pipeline's own states, the plan their
	author built them for */
	int expect;
	int blend_enable;
	unsigned long source_blend, destination_blend, blend_operation;
	int write_color, write_alpha, depth_write;
	int alpha_test;
	unsigned long alpha_reference;
	/* stages with D3DTSS_ALPHAKILL */
	int alpha_kill;
	unsigned long blend_color, fog_color;
	/* what the renderer would tell the compiler about the stages' worth
	(nv2a_ff_draw.stage_rank); all zero where stage 0 matters most */
	unsigned char stage_rank[4];
};

/* What a state of the light pipeline has to come out as. Each means: with
two units and the OpenGL 1.1 environments (level 1) an exact plan of one
pass, and with one unit (level 0) a plan that draws something. */
enum
{
	_site_expect_nothing = 0,
	/* one unit or none, and exact with one unit too */
	_site_expect_one_unit,
	/* two units with the stages' own textures; with one unit, exact in two passes */
	_site_expect_two_units,
	/* two units, one of them a derived texture that does not read the constants */
	_site_expect_two_units_derived,
	/* two units, exact only with EXT_texture_env_add (level 2) */
	_site_expect_two_units_add
};

typedef void (*site_callback)(const struct site *site);

static struct site site;
static site_callback site_emit_callback;
/* the fixed random sequence of the sampled variants */
static unsigned long site_seed;

static unsigned long site_random(unsigned long range)
{
	site_seed = (site_seed * 1664525UL + 1013904223UL) & 0xffffffffUL;
	return ((site_seed >> 10) & 0xfffffUL) % range;
}

static void site_begin(int light)
{
	memset(&site, 0, sizeof(site));
	site.light = light;
	site.write_color = 1;
	site.blend_operation = BLENDOP_ADD;
	site.source_blend = BLEND_ONE;
	site.destination_blend = BLEND_ZERO;
	site.blend_color = 0xc0a06040UL;
	site.fog_color = 0xff8090a0UL;
}

static void site_blend(unsigned long source, unsigned long destination, unsigned long operation)
{
	site.blend_enable = 1;
	site.source_blend = source;
	site.destination_blend = destination;
	site.blend_operation = operation;
}

static void site_blend_function(int function)
{
	static const unsigned long source[NUMBER_OF_BLEND_FUNCTIONS] =
	{
		BLEND_SRCALPHA, BLEND_DESTCOLOR, BLEND_DESTCOLOR, BLEND_ONE, BLEND_ONE, BLEND_ONE, BLEND_ONE, BLEND_ONE
	};
	static const unsigned long destination[NUMBER_OF_BLEND_FUNCTIONS] =
	{
		BLEND_INVSRCALPHA, BLEND_ZERO, BLEND_SRCCOLOR, BLEND_ONE, BLEND_ONE, BLEND_ONE, BLEND_ONE, BLEND_INVSRCALPHA
	};
	static const unsigned long operation[NUMBER_OF_BLEND_FUNCTIONS] =
	{
		BLENDOP_ADD, BLENDOP_ADD, BLENDOP_ADD, BLENDOP_ADD, BLENDOP_REVSUBTRACT, BLENDOP_MIN, BLENDOP_MAX, BLENDOP_ADD
	};

	site_blend(source[function], destination[function], operation[function]);
}

static void site_end(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	vsprintf(site.name, format, arguments);
	va_end(arguments);
	site_emit_callback(&site);
}

/* tag colours stand in for the artists': any will do, none is 0 or 1 in
every channel */
#define COLOR_A 0xff80c040UL
#define COLOR_B 0xc04060e0UL
#define COLOR_C 0x60e0a020UL
#define COLOR_D 0xa0306090UL
#define COLOR_E 0x40b05070UL

/* ---------- rasterizer_xbox_models.c:840-1108, set_environment_shader_pixel_shader */

static void site_model(int detail_function, int detail_mask, int double_diffuse_intensity,
	int true_atmospheric_fog)
{
	struct site_shader *pixel_shader = &site.shader;
	static const unsigned long detail_mask_alpha_inputs[9] =
	{
		PS_REGISTER_ONE,
		PS_REGISTER_R1 | PS_INPUTMAPPING_UNSIGNED_INVERT,
		PS_REGISTER_R1,
		PS_REGISTER_R0 | PS_CHANNEL_ALPHA | PS_INPUTMAPPING_UNSIGNED_INVERT,
		PS_REGISTER_R0 | PS_CHANNEL_ALPHA,
		PS_REGISTER_T1 | PS_CHANNEL_ALPHA | PS_INPUTMAPPING_UNSIGNED_INVERT,
		PS_REGISTER_T1 | PS_CHANNEL_ALPHA,
		PS_REGISTER_T2 | PS_CHANNEL_ALPHA | PS_INPUTMAPPING_UNSIGNED_INVERT,
		PS_REGISTER_T2 | PS_CHANNEL_ALPHA,
	};
	static const unsigned long detail_function_alpha_inputs[3] =
	{
		PS_REGISTER_ONE_HALF,
		PS_REGISTER_ONE,
		PS_REGISTER_ONE_HALF,
	};

	pixel_shader->constant_0[2] = COLOR_A; /* base_color */
	pixel_shader->constant_0[1] = COLOR_B; /* detail_color */
	pixel_shader->rgb_inputs[3] = PS_COMBINERINPUTS(
		detail_mask_alpha_inputs[detail_mask] ^ PS_INPUTMAPPING_UNSIGNED_INVERT,
		detail_function_alpha_inputs[detail_function],
		detail_mask_alpha_inputs[detail_mask],
		PS_REGISTER_T1);
	if (double_diffuse_intensity)
	{
		static const unsigned long detail_function_rgb_inputs[3] =
		{
			PS_COMBINERINPUTS(PS_REGISTER_R0, PS_REGISTER_T1, PS_REGISTER_R0, PS_REGISTER_T1),
			PS_COMBINERINPUTS(PS_REGISTER_R0, PS_REGISTER_T1, PS_REGISTER_ZERO, PS_REGISTER_ZERO),
			PS_COMBINERINPUTS(PS_REGISTER_R0, PS_REGISTER_ONE, PS_REGISTER_T1 | PS_INPUTMAPPING_EXPAND_NORMAL,
				PS_REGISTER_ONE),
		};

		pixel_shader->rgb_inputs[6] = PS_COMBINERINPUTS(
			PS_REGISTER_T0, PS_REGISTER_V0, PS_REGISTER_T3, PS_REGISTER_R1 | PS_CHANNEL_ALPHA);
		pixel_shader->rgb_outputs[6] = PS_COMBINEROUTPUTS(
			PS_REGISTER_DISCARD, PS_REGISTER_DISCARD, PS_REGISTER_R0, PS_COMBINEROUTPUT_IDENTITY);
		pixel_shader->rgb_inputs[7] = detail_function_rgb_inputs[detail_function];
		pixel_shader->rgb_outputs[7] = PS_COMBINEROUTPUTS(
			PS_REGISTER_DISCARD, PS_REGISTER_DISCARD, PS_REGISTER_R0, PS_COMBINEROUTPUT_IDENTITY);
	}
	else
	{
		static const unsigned long detail_function_rgb_inputs[3] =
		{
			PS_COMBINERINPUTS(PS_REGISTER_T0, PS_REGISTER_T1, PS_REGISTER_T0, PS_REGISTER_T1),
			PS_COMBINERINPUTS(PS_REGISTER_T0, PS_REGISTER_T1, PS_REGISTER_ZERO, PS_REGISTER_ZERO),
			PS_COMBINERINPUTS(PS_REGISTER_T0, PS_REGISTER_ONE, PS_REGISTER_T1 | PS_INPUTMAPPING_EXPAND_NORMAL,
				PS_REGISTER_ONE),
		};

		pixel_shader->rgb_inputs[6] = detail_function_rgb_inputs[detail_function];
		pixel_shader->rgb_outputs[6] = PS_COMBINEROUTPUTS(
			PS_REGISTER_DISCARD, PS_REGISTER_DISCARD, PS_REGISTER_T0, PS_COMBINEROUTPUT_IDENTITY);
		pixel_shader->rgb_inputs[7] = PS_COMBINERINPUTS(
			PS_REGISTER_T0, PS_REGISTER_V0, PS_REGISTER_T3, PS_REGISTER_R1 | PS_CHANNEL_ALPHA);
		pixel_shader->rgb_outputs[7] = PS_COMBINEROUTPUTS(
			PS_REGISTER_DISCARD, PS_REGISTER_DISCARD, PS_REGISTER_R0, PS_COMBINEROUTPUT_IDENTITY);
	}
	if (true_atmospheric_fog)
	{
		pixel_shader->final_combiner_inputs_abcd = PS_COMBINERINPUTS(
			PS_REGISTER_FOG | PS_CHANNEL_ALPHA | PS_INPUTMAPPING_UNSIGNED_INVERT,
			PS_REGISTER_R0, PS_REGISTER_FOG, PS_REGISTER_ZERO);
		pixel_shader->final_combiner_inputs_efg = PS_COMBINERINPUTS(
			PS_REGISTER_ZERO, PS_REGISTER_ZERO, PS_REGISTER_T0 | PS_CHANNEL_ALPHA, 0);
	}
	else
	{
		pixel_shader->constant_0[5] = COLOR_C; /* fog_error_color */
		pixel_shader->constant_1[5] = COLOR_D; /* atmospheric_fog_color */
		pixel_shader->final_combiner_constant_0 = COLOR_E; /* fog_color */
		pixel_shader->final_combiner_constant_1 = COLOR_D;
		pixel_shader->final_combiner_inputs_abcd = PS_COMBINERINPUTS(
			PS_REGISTER_V0 | PS_CHANNEL_ALPHA | PS_INPUTMAPPING_UNSIGNED_INVERT,
			PS_REGISTER_EF_PROD, PS_REGISTER_C0, PS_REGISTER_R1);
		pixel_shader->final_combiner_inputs_efg = PS_COMBINERINPUTS(
			PS_REGISTER_R0, PS_REGISTER_C0 | PS_CHANNEL_ALPHA, PS_REGISTER_T0 | PS_CHANNEL_ALPHA, 0);
	}

	/* the part set once (local_pixel_shader_dirty_flag) */
	pixel_shader->texture_modes = PS_TEXTUREMODES(
		PS_TEXTUREMODES_PROJECT2D, PS_TEXTUREMODES_PROJECT2D, PS_TEXTUREMODES_PROJECT2D, PS_TEXTUREMODES_CUBEMAP);
	pixel_shader->combiner_count = PS_COMBINERCOUNT(8, PS_COMBINERCOUNT_UNIQUE_C0 | PS_COMBINERCOUNT_UNIQUE_C1);
	pixel_shader->constant_0[0] = 0x00FF0000;
	pixel_shader->constant_1[0] = 0x0000FF00;
	pixel_shader->alpha_inputs[0] = PS_COMBINERINPUTS(PS_REGISTER_T2, PS_REGISTER_ONE, PS_REGISTER_ZERO, PS_REGISTER_ZERO);
	pixel_shader->alpha_outputs[0] = PS_COMBINEROUTPUTS(
		PS_REGISTER_R0, PS_REGISTER_DISCARD, PS_REGISTER_DISCARD, PS_COMBINEROUTPUT_IDENTITY);
	pixel_shader->rgb_inputs[0] = PS_COMBINERINPUTS(PS_REGISTER_T2, PS_REGISTER_C1, PS_REGISTER_T2, PS_REGISTER_C0);
	pixel_shader->rgb_outputs[0] = PS_COMBINEROUTPUTS(
		PS_REGISTER_R0, PS_REGISTER_R1, PS_REGISTER_DISCARD,
		PS_COMBINEROUTPUT_AB_DOT_PRODUCT | PS_COMBINEROUTPUT_CD_DOT_PRODUCT);
	pixel_shader->alpha_inputs[1] = PS_COMBINERINPUTS(
		PS_REGISTER_R0 | PS_CHANNEL_ALPHA, PS_REGISTER_ONE, PS_REGISTER_ZERO, PS_REGISTER_ZERO);
	pixel_shader->alpha_outputs[1] = PS_COMBINEROUTPUTS(
		PS_REGISTER_T1, PS_REGISTER_DISCARD, PS_REGISTER_DISCARD, PS_COMBINEROUTPUT_IDENTITY);
	pixel_shader->rgb_inputs[1] = PS_COMBINERINPUTS(PS_REGISTER_V0, PS_REGISTER_ONE, PS_REGISTER_R0, PS_REGISTER_C0);
	pixel_shader->rgb_outputs[1] = PS_COMBINEROUTPUTS(
		PS_REGISTER_DISCARD, PS_REGISTER_DISCARD, PS_REGISTER_V0, PS_COMBINEROUTPUT_IDENTITY);
	pixel_shader->alpha_inputs[2] = PS_COMBINERINPUTS(
		PS_REGISTER_R0, PS_REGISTER_ONE, PS_REGISTER_R1, PS_REGISTER_V1 | PS_CHANNEL_ALPHA);
	pixel_shader->alpha_outputs[2] = PS_COMBINEROUTPUTS(
		PS_REGISTER_R0, PS_REGISTER_R1, PS_REGISTER_DISCARD, PS_COMBINEROUTPUT_IDENTITY);
	pixel_shader->rgb_inputs[2] = PS_COMBINERINPUTS(
		PS_REGISTER_R0 | PS_CHANNEL_ALPHA | PS_INPUTMAPPING_UNSIGNED_INVERT, PS_REGISTER_ONE,
		PS_REGISTER_R0 | PS_CHANNEL_ALPHA, PS_REGISTER_C0);
	pixel_shader->rgb_outputs[2] = PS_COMBINEROUTPUTS(
		PS_REGISTER_DISCARD, PS_REGISTER_DISCARD, PS_REGISTER_R0, PS_COMBINEROUTPUT_IDENTITY);
	pixel_shader->rgb_outputs[3] = PS_COMBINEROUTPUTS(
		PS_REGISTER_DISCARD, PS_REGISTER_DISCARD, PS_REGISTER_T1, PS_COMBINEROUTPUT_IDENTITY);
	pixel_shader->rgb_inputs[4] = PS_COMBINERINPUTS(PS_REGISTER_T3, PS_REGISTER_V1, PS_REGISTER_V0, PS_REGISTER_R0);
	pixel_shader->rgb_outputs[4] = PS_COMBINEROUTPUTS(
		PS_REGISTER_T3, PS_REGISTER_V0, PS_REGISTER_DISCARD, PS_COMBINEROUTPUT_IDENTITY);
	pixel_shader->rgb_inputs[5] = PS_COMBINERINPUTS(
		PS_REGISTER_C1, PS_REGISTER_ONE, PS_REGISTER_V0 | PS_CHANNEL_ALPHA,
		PS_REGISTER_C0 | PS_INPUTMAPPING_SIGNED_NEGATE);
	pixel_shader->rgb_outputs[5] = PS_COMBINEROUTPUTS(
		PS_REGISTER_DISCARD, PS_REGISTER_DISCARD, PS_REGISTER_R1, PS_COMBINEROUTPUT_IDENTITY);
}

/* ---------- shader_transparent_chicago_preprocessor.c, and the fog stage
rasterizer_xbox_transparent_geometry.c appends (2640-2787; the same code
as for generic shaders, 2117-2264) */

enum
{
	NUMBER_OF_SHADER_FUNCTIONS = 13,

	_fade_mode_none = 0,
	_fade_mode_fade_when_perpendicular,
	_fade_mode_fade_when_parallel,
	NUMBER_OF_FADE_MODES
};

static const unsigned long chicago_rgb_inputs[2][NUMBER_OF_SHADER_FUNCTIONS] =
{
	{
		0x0c200000, 0x08200000, 0x080c0000, 0x080c080c, 0x08200c20,
		0x08204c20, 0x0c204820, 0x08200c40, 0x0c200840, 0x081c0c3c,
		0x0c1c083c, 0x08180c38, 0x0c180838
	},
	{
		0x0c200000, 0x18200000, 0x180c0000, 0x180c180c, 0x18200c20,
		0x18205c20, 0x0c205820, 0x18200c40, 0x0c201840, 0x181c0c3c,
		0x0c1c183c, 0x18180c38, 0x0c181838
	}
};
static const unsigned long chicago_alpha_inputs[NUMBER_OF_SHADER_FUNCTIONS] =
{
	0x1c200000, 0x18200000, 0x181c0000, 0x181c181c, 0x18201c20,
	0x18205c20, 0x1c205820, 0x18201c40, 0x1c201840, 0x181c1c3c,
	0x1c1c183c, 0x18181c38, 0x1c181838
};
static const unsigned long chicago_stage_increments[NUMBER_OF_SHADER_FUNCTIONS] =
{
	0x00000000, 0x01000000, 0x01000000, 0x01000100, 0x01000000,
	0x01000000, 0x00000100, 0x01000000, 0x00000100, 0x01000000,
	0x00000100, 0x01010001, 0x00010101
};

/* the stage that fades the result for the framebuffer blend; sky is the
planar fog of an alpha-blended sky shader */
static void site_transparent_fog_stage(struct site_shader *pixel_shader, int stage_count, int blend_function,
	int fade_mode, int sky)
{
	unsigned long combiner_constant = fade_mode == _fade_mode_none ? 0x14 :
		fade_mode == _fade_mode_fade_when_perpendicular ? 0x15 : 0x05;

	if (sky && blend_function == _blend_function_alpha_blend)
	{
		pixel_shader->constant_0[stage_count] = COLOR_D;
		pixel_shader->rgb_inputs[stage_count] = 0x310C1101;
		pixel_shader->rgb_outputs[stage_count] = 0xC00;
		return;
	}
	switch (blend_function)
	{
	case _blend_function_alpha_blend:
		pixel_shader->alpha_inputs[stage_count] = (combiner_constant | 0x1C00) << 16;
		pixel_shader->alpha_outputs[stage_count] = 0xC00;
		break;
	case _blend_function_multiply:
	case _blend_function_min:
		pixel_shader->rgb_inputs[stage_count] = ((combiner_constant ^ 0x20) | (combiner_constant << 16)) | 0x0C002000;
		pixel_shader->rgb_outputs[stage_count] = 0xC00;
		break;
	case _blend_function_double_multiply:
		pixel_shader->rgb_inputs[stage_count] = ((combiner_constant ^ 0x20) | (combiner_constant << 16)) | 0x0C00A000;
		pixel_shader->rgb_outputs[stage_count] = 0xC00;
		break;
	case _blend_function_add:
	case _blend_function_reverse_subtract:
	case _blend_function_max:
		pixel_shader->rgb_inputs[stage_count] = (combiner_constant | 0x0C00) << 16;
		pixel_shader->rgb_outputs[stage_count] = 0xC00;
		break;
	default:
		pixel_shader->alpha_inputs[stage_count] = (combiner_constant | 0x1C00) << 16;
		pixel_shader->alpha_outputs[stage_count] = 0xC00;
		pixel_shader->rgb_inputs[stage_count] = (combiner_constant | 0x0C00) << 16;
		pixel_shader->rgb_outputs[stage_count] = 0xC00;
		break;
	}
}

static void site_chicago(int map_count, int first_map_cube, const int *color_functions, const int *alpha_functions,
	const int *alpha_replicate)
{
	struct site_shader *pixel_shader = &site.shader;
	int map_index;

	pixel_shader->combiner_count = (map_count + 1) | 0x11000;
	pixel_shader->texture_modes =
		((((unsigned long)(map_count > 3) << 5 | (map_count > 2)) << 5 | (map_count > 1)) << 5) |
		(2 * (first_map_cube != 0) + 1);
	for (map_index = 0; map_index < map_count; map_index++)
	{
		if (map_index != map_count - 1)
		{
			pixel_shader->alpha_inputs[map_index + 1] =
				chicago_stage_increments[alpha_functions[map_index]] * (map_index + 1) +
				chicago_alpha_inputs[alpha_functions[map_index]];
			pixel_shader->rgb_inputs[map_index + 1] =
				chicago_stage_increments[color_functions[map_index]] * (map_index + 1) +
				chicago_rgb_inputs[alpha_replicate[map_index]][color_functions[map_index]];
		}
		else
		{
			pixel_shader->alpha_inputs[0] = 0x18200000;
			pixel_shader->rgb_inputs[0] = 0x08200000;
		}
		pixel_shader->alpha_outputs[map_index] = 0x00000c00;
		pixel_shader->rgb_outputs[map_index] = 0x00000c00;
	}
	pixel_shader->final_combiner_inputs_abcd = 12;
	pixel_shader->final_combiner_inputs_efg = 0x1c00;
}

/* ---------- shader_transparent_generic_preprocessor.c */

enum
{
	NUMBER_OF_GENERIC_INPUTS = 25,
	NUMBER_OF_GENERIC_INPUT_MAPPINGS = 8,
	NUMBER_OF_GENERIC_OUTPUTS = 9,
	NUMBER_OF_GENERIC_OUTPUT_MAPPINGS = 6
};

struct generic_stage
{
	int color_mux, alpha_mux;
	int color_input[4], color_input_mapping[4];
	int color_output_AB, color_output_AB_dot, color_output_CD, color_output_CD_dot;
	int color_output_AB_CD_mux_sum, color_output_mapping;
	int alpha_input[4], alpha_input_mapping[4];
	int alpha_output_A, alpha_output_B, alpha_output_C, alpha_output_mapping;
	unsigned long color1;
};

static const long generic_color_input_registers[NUMBER_OF_GENERIC_INPUTS] =
{
	-1, -1, -1, -1, -1,
	8, 9, 10, 11,
	4, 5,
	12, 13,
	1, 2,
	24, 25, 26, 27,
	20, 21,
	28, 29,
	17, 18
};
static const long generic_alpha_input_registers[NUMBER_OF_GENERIC_INPUTS] =
{
	-1, -1, -1, -1, -1,
	24, 25, 26, 27,
	20, 21,
	28, 29,
	17, 18,
	8, 9, 10, 11,
	4, 5,
	12, 13,
	1, 2
};
static const long generic_input_mappings[NUMBER_OF_GENERIC_INPUT_MAPPINGS] =
{
	0x00, 0x20, 0x40, 0x60, 0x80, 0xa0, 0xc0, 0xe0
};
static const long generic_constant_input_registers[5][NUMBER_OF_GENERIC_INPUT_MAPPINGS] =
{
	{ 0x00, 0x20, 0x40, 0x20, 0x80, 0xa0, 0x00, 0x00 },
	{ 0x20, 0x00, 0x20, 0x40, 0xa0, 0x80, 0x20, 0x40 },
	{ 0xa0, 0xa0, 0x00, 0x00, 0x00, 0x00, 0xa0, 0x80 },
	{ 0x00, 0x20, 0x40, 0x20, 0x80, 0xa0, 0x40, 0x20 },
	{ 0x00, 0x20, 0x40, 0x20, 0x80, 0xa0, 0x80, 0xa0 }
};
static const long generic_color_output_registers[NUMBER_OF_GENERIC_OUTPUTS] =
{
	0, 12, 13, 4, 5, 8, 9, 10, 11
};
static const long generic_alpha_output_registers[NUMBER_OF_GENERIC_OUTPUTS] =
{
	0, 28, 29, 20, 21, 24, 25, 26, 27
};
static const long generic_output_mappings[NUMBER_OF_GENERIC_OUTPUT_MAPPINGS] =
{
	0x00, 0x30, 0x10, 0x20, 0x08, 0x18
};

static long generic_input(const long *registers, int register_index, int mapping_index)
{
	long register_value = registers[register_index];

	if (register_value == -1)
		return generic_constant_input_registers[register_index][mapping_index];
	return register_value | generic_input_mappings[mapping_index];
}

/* shader_stage_verify */
static int generic_stage_verify(const struct generic_stage *stage)
{
	if ((stage->color_output_AB != 0 && stage->color_output_CD != 0 && stage->color_output_AB == stage->color_output_CD) ||
		(stage->color_output_AB != 0 && stage->color_output_AB_CD_mux_sum != 0 &&
			stage->color_output_AB == stage->color_output_AB_CD_mux_sum) ||
		(stage->color_output_CD != 0 && stage->color_output_AB_CD_mux_sum != 0 &&
			stage->color_output_CD == stage->color_output_AB_CD_mux_sum) ||
		(stage->alpha_output_A != 0 && stage->alpha_output_B != 0 && stage->alpha_output_A == stage->alpha_output_B) ||
		(stage->alpha_output_A != 0 && stage->alpha_output_C != 0 && stage->alpha_output_A == stage->alpha_output_C) ||
		(stage->alpha_output_B != 0 && stage->alpha_output_C != 0 && stage->alpha_output_B == stage->alpha_output_C))
	{
		return 0;
	}
	if ((stage->color_output_AB_dot || stage->color_output_CD_dot) && stage->color_output_AB_CD_mux_sum != 0)
		return 0;
	/* _shader_transparent_generic_stage_output_vertex_color_0 is output 3 */
	if (stage->alpha_output_A == 3 || stage->alpha_output_C == 3)
		return 0;
	if (stage->color_mux && (stage->color_output_AB_dot || stage->color_output_CD_dot))
		return 0;
	return 1;
}

static void site_generic(int map_count, int first_map_cube, const struct generic_stage *stages, int stage_count)
{
	struct site_shader *pixel_shader = &site.shader;
	int stage_index;

	pixel_shader->texture_modes =
		((((unsigned long)(map_count > 3) << 5 | (map_count > 2)) << 5 | (map_count > 1)) << 5) |
		(map_count > 0 ? (first_map_cube ? 3 : 1) : 0);
	pixel_shader->combiner_count = ((stage_count > 1 ? stage_count : 1) + 1) | 0x11100;
	if (stage_count > 0)
	{
		for (stage_index = 0; stage_index < stage_count; stage_index++)
		{
			const struct generic_stage *stage = &stages[stage_index];
			long color_input[4], alpha_input[4];
			long color_output_flags, alpha_output_flags;
			int input;

			pixel_shader->constant_1[stage_index] = stage->color1;
			for (input = 0; input < 4; input++)
			{
				color_input[input] = generic_input(generic_color_input_registers, stage->color_input[input],
					stage->color_input_mapping[input]);
				alpha_input[input] = generic_input(generic_alpha_input_registers, stage->alpha_input[input],
					stage->alpha_input_mapping[input]);
			}
			color_output_flags = generic_output_mappings[stage->color_output_mapping];
			if (stage->color_output_AB_dot)
				color_output_flags |= 1 << 1;
			if (stage->color_output_CD_dot)
				color_output_flags |= 1 << 0;
			if (stage->color_mux)
				color_output_flags |= 1 << 2;
			pixel_shader->rgb_inputs[stage_index] =
				((unsigned long)((color_input[0] << 8 | color_input[1]) << 8 | color_input[2]) << 8) | color_input[3];
			pixel_shader->rgb_outputs[stage_index] =
				((color_output_flags << 4 | (generic_color_output_registers[stage->color_output_AB_CD_mux_sum] & 0x0f)) << 4 |
				(generic_color_output_registers[stage->color_output_AB] & 0x0f)) << 4 |
				(generic_color_output_registers[stage->color_output_CD] & 0x0f);
			alpha_output_flags = generic_output_mappings[stage->alpha_output_mapping];
			if (stage->alpha_mux)
				alpha_output_flags |= 1 << 2;
			pixel_shader->alpha_inputs[stage_index] =
				((unsigned long)((alpha_input[0] << 8 | alpha_input[1]) << 8 | alpha_input[2]) << 8) | alpha_input[3];
			pixel_shader->alpha_outputs[stage_index] =
				((alpha_output_flags << 4 | (generic_alpha_output_registers[stage->alpha_output_C] & 0x0f)) << 4 |
				(generic_alpha_output_registers[stage->alpha_output_A] & 0x0f)) << 4 |
				(generic_alpha_output_registers[stage->alpha_output_B] & 0x0f);
			/* the draw sets each stage's first constant from its animated colour */
			pixel_shader->constant_0[stage_index] = stage_index & 1 ? COLOR_C : COLOR_A;
		}
	}
	else
	{
		pixel_shader->rgb_inputs[0] = 0x08200000;
		pixel_shader->rgb_outputs[0] = 0x000000c0;
		pixel_shader->alpha_inputs[0] = 0x18200000;
		pixel_shader->alpha_outputs[0] = 0x000000c0;
	}
	pixel_shader->final_combiner_inputs_abcd = 12;
	pixel_shader->final_combiner_inputs_efg = 0x1c00;
}

/* A stage an artist might build: inputs from the maps there are, the
colours and the scratch registers; the result into r0 or scratch. */
static void generic_stage_random(struct generic_stage *stage, int map_count, int last)
{
	/* indices into the input tables: zero, one, one half, negative one,
	negative one half; maps 0-3; vertex colours; scratch; constants; the
	same again for alpha */
	static const int plain_inputs[] = { 0, 1, 2, 9, 10, 11, 12, 13, 14, 19, 21, 22, 23, 24 };
	int input, attempt;

	for (attempt = 0; attempt < 64; attempt++)
	{
		memset(stage, 0, sizeof(*stage));
		stage->color1 = site_random(2) ? COLOR_B : COLOR_E;
		stage->color_mux = site_random(8) == 0;
		stage->alpha_mux = site_random(8) == 0;
		for (input = 0; input < 4; input++)
		{
			int choice = (int)site_random(3);

			if (choice == 0 && map_count > 0)
			{
				int map = (int)site_random(map_count);

				/* map colour, or map alpha */
				stage->color_input[input] = (site_random(4) ? 5 : 15) + map;
				stage->alpha_input[input] = 5 + (int)site_random(map_count);
			}
			else
			{
				stage->color_input[input] = plain_inputs[site_random(sizeof(plain_inputs) / sizeof(plain_inputs[0]))];
				stage->alpha_input[input] = plain_inputs[site_random(11)];
			}
			/* mostly the plain mapping and its inverse */
			stage->color_input_mapping[input] = site_random(4) ? (int)site_random(2) : (int)site_random(8);
			stage->alpha_input_mapping[input] = site_random(4) ? (int)site_random(2) : (int)site_random(8);
		}
		/* outputs: 0 discard, 1 scratch 0 (r0), 2 scratch 1, 3-4 vertex colours, 5-8 maps */
		stage->color_output_AB_CD_mux_sum = last || site_random(3) ? 1 : 2;
		if (site_random(6) == 0)
		{
			stage->color_output_AB_CD_mux_sum = 0;
			stage->color_output_AB = 1;
			stage->color_output_AB_dot = site_random(3) == 0;
			stage->color_output_CD = site_random(2) ? 2 : 0;
		}
		stage->color_output_mapping = site_random(4) ? 0 : (int)site_random(NUMBER_OF_GENERIC_OUTPUT_MAPPINGS);
		stage->alpha_output_C = last || site_random(3) ? 1 : 2;
		stage->alpha_output_mapping = site_random(6) ? 0 : (int)site_random(NUMBER_OF_GENERIC_OUTPUT_MAPPINGS);
		if (generic_stage_verify(stage))
			return;
	}
}

/* ---------- every site */

static void sites_enumerate(site_callback callback)
{
	struct site_shader *pixel_shader = &site.shader;
	int a, b, c, d, e;

	site_emit_callback = callback;
	site_seed = 20261002UL;

	/* rasterizer_xbox.c:2318-2322, the secondary render target on screen (debugging) */
	site_begin(0);
	pixel_shader->texture_modes = 1;
	pixel_shader->combiner_count = 1;
	pixel_shader->final_combiner_inputs_abcd = 8;
	site_end("rasterizer_xbox.c:2318");

	/* rasterizer_xbox_active_camouflage.c:399-411, the copy of the frame */
	site_begin(1);
	pixel_shader->texture_modes = PS_TEXTUREMODES(
		PS_TEXTUREMODES_PROJECT2D, PS_TEXTUREMODES_NONE, PS_TEXTUREMODES_NONE, PS_TEXTUREMODES_NONE);
	pixel_shader->combiner_count = PS_COMBINERCOUNT(1, 0);
	pixel_shader->final_combiner_inputs_abcd = PS_COMBINERINPUTS(
		PS_REGISTER_ZERO, PS_REGISTER_ZERO, PS_REGISTER_ZERO, PS_REGISTER_T0);
	site_end("active_camouflage.c:399 frame copy");

	/* rasterizer_xbox_active_camouflage.c:646-658, the model's depth, alpha tested */
	site_begin(1);
	site.alpha_test = 1;
	site.alpha_reference = 0x7f;
	site.depth_write = 1;
	site.write_color = 0;
	pixel_shader->texture_modes = PS_TEXTUREMODES(
		PS_TEXTUREMODES_PROJECT2D, PS_TEXTUREMODES_NONE, PS_TEXTUREMODES_NONE, PS_TEXTUREMODES_NONE);
	pixel_shader->combiner_count = PS_COMBINERCOUNT(1, 0);
	pixel_shader->final_combiner_inputs_efg = PS_COMBINERINPUTS(
		PS_REGISTER_ZERO, PS_REGISTER_ZERO, PS_REGISTER_T0 | PS_CHANNEL_ALPHA, 0);
	site_end("active_camouflage.c:646 depth");

	/* rasterizer_xbox_active_camouflage.c:857-909, the refraction: b = at full
	intensity, where it is not blended (921-931) */
	for (a = 0; a < 2; a++)
	for (b = 0; b < 2; b++)
	{
		site_begin(0);
		if (!b)
			site_blend(BLEND_SRCALPHA, BLEND_INVSRCALPHA, BLENDOP_ADD);
		pixel_shader->texture_modes = PS_TEXTUREMODES(
			PS_TEXTUREMODES_CUBEMAP, PS_TEXTUREMODES_DOTPRODUCT, PS_TEXTUREMODES_DOT_ST, PS_TEXTUREMODES_NONE);
		pixel_shader->input_texture = PS_INPUTTEXTURE(0, 0, 0, 0);
		pixel_shader->dot_mapping = PS_DOTMAPPING(
			0, PS_DOTMAPPING_MINUS1_TO_1_D3D, PS_DOTMAPPING_MINUS1_TO_1_D3D, PS_DOTMAPPING_ZERO_TO_ONE);
		pixel_shader->combiner_count = PS_COMBINERCOUNT(2, 0);
		pixel_shader->rgb_inputs[0] = a ?
			PS_COMBINERINPUTS(
				PS_REGISTER_T0 | PS_CHANNEL_ALPHA | PS_INPUTMAPPING_UNSIGNED_INVERT, PS_REGISTER_ONE,
				PS_REGISTER_T0 | PS_CHANNEL_ALPHA, PS_REGISTER_V0) :
			PS_COMBINERINPUTS(PS_REGISTER_V0, PS_REGISTER_ONE, PS_REGISTER_ZERO, PS_REGISTER_ZERO);
		pixel_shader->rgb_outputs[0] = PS_COMBINEROUTPUTS(PS_REGISTER_DISCARD, PS_REGISTER_DISCARD, PS_REGISTER_R0, 0);
		pixel_shader->rgb_inputs[1] = PS_COMBINERINPUTS(
			PS_REGISTER_V0 | PS_CHANNEL_ALPHA | PS_INPUTMAPPING_UNSIGNED_INVERT, PS_REGISTER_ONE,
			PS_REGISTER_V0 | PS_CHANNEL_ALPHA, PS_REGISTER_R0);
		pixel_shader->rgb_outputs[1] = PS_COMBINEROUTPUTS(PS_REGISTER_DISCARD, PS_REGISTER_DISCARD, PS_REGISTER_R0, 0);
		pixel_shader->final_combiner_constant_0 = 0xa0000000UL;
		pixel_shader->final_combiner_inputs_abcd = PS_COMBINERINPUTS(
			PS_REGISTER_T2, PS_REGISTER_R0, PS_REGISTER_ZERO, PS_REGISTER_ZERO);
		pixel_shader->final_combiner_inputs_efg = PS_COMBINERINPUTS(
			PS_REGISTER_ZERO, PS_REGISTER_ZERO, PS_REGISTER_C0 | PS_CHANNEL_ALPHA, 0);
		site_end("active_camouflage.c:857 refraction%s%s", a ? ", tint edge density" : "", b ? ", full" : "");
	}

	/* rasterizer_xbox_debug.c:100-119, 200-204, 411-415 */
	for (a = 0; a < 3; a++)
	{
		site_begin(0);
		if (a == 0)
			site.depth_write = 1;
		if (a == 1)
			site_blend(BLEND_SRCALPHA, BLEND_INVSRCALPHA, BLENDOP_ADD);
		pixel_shader->texture_modes = 0;
		pixel_shader->combiner_count = 1;
		pixel_shader->final_combiner_inputs_abcd = 4;
		site_end("debug.c:%s", a == 0 ? "100 opaque" : a == 1 ? "200 blended" : "411 screen");
	}

	/* rasterizer_xbox_decals.c:682-795: each framebuffer blend function,
	plain, with the fog hack, and in the alpha tested layer. The shader is
	kept between decals and a function sets only its own inputs: the
	functions that do not set alpha_inputs[1] are given it as zero. */
	for (a = 0; a < NUMBER_OF_BLEND_FUNCTIONS; a++)
	{
		for (b = 0; b < 3; b++)
		{
			site_begin(1);
			if (b == 1)
			{
				site.alpha_test = 1;
				site.alpha_reference = 0;
			}
			if (b == 2)
			{
				site.alpha_test = 1;
				site.alpha_reference = 0x7f;
			}
			pixel_shader->texture_modes = 1;
			pixel_shader->rgb_outputs[0] = 0xc00;
			pixel_shader->alpha_outputs[1] = 0xc00;
			pixel_shader->rgb_outputs[1] = 0xc00;
			if (b == 1)
			{
				pixel_shader->combiner_count = 3;
				pixel_shader->constant_0[0] = 0x1000000;
				pixel_shader->alpha_inputs[2] = 0x1c151115;
				pixel_shader->alpha_outputs[2] = 0xc00;
			}
			else
			{
				pixel_shader->combiner_count = 2;
			}
			pixel_shader->final_combiner_inputs_abcd = 0xc;
			pixel_shader->final_combiner_inputs_efg = 0x1c00;
			site.write_alpha = a == _blend_function_multiply || a == _blend_function_double_multiply;
			switch (a)
			{
			case _blend_function_add:
			case _blend_function_reverse_subtract:
			case _blend_function_max:
				pixel_shader->rgb_inputs[0] = 0x08040000;
				pixel_shader->rgb_inputs[1] = 0x340c0000;
				break;
			case _blend_function_multiply:
			case _blend_function_min:
				pixel_shader->rgb_inputs[0] = 0x28240820;
				pixel_shader->rgb_inputs[1] = 0x340c1420;
				pixel_shader->alpha_inputs[1] = 0x341c1420;
				break;
			case _blend_function_double_multiply:
				pixel_shader->rgb_inputs[0] = 0xa8240820;
				pixel_shader->rgb_inputs[1] = 0x340c14a0;
				pixel_shader->alpha_inputs[1] = 0x341c14a0;
				break;
			case _blend_function_alpha_blend:
				pixel_shader->rgb_inputs[0] = 0x08040000;
				pixel_shader->rgb_inputs[1] = 0x200c0000;
				pixel_shader->alpha_inputs[1] = 0x34180000;
				break;
			default:
				pixel_shader->rgb_inputs[0] = 0x08040000;
				pixel_shader->rgb_inputs[1] = 0x340c0000;
				pixel_shader->alpha_inputs[1] = 0x34180000;
				break;
			}
			site_blend_function(a);
			site_end("decals.c:682 %s%s", blend_function_names[a],
				b == 1 ? ", fog hack" : b == 2 ? ", alpha tested" : "");
		}
	}

	/* rasterizer_xbox_detail_objects.c:443-452 */
	site_begin(1);
	site_blend(BLEND_SRCALPHA, BLEND_INVSRCALPHA, BLENDOP_ADD);
	pixel_shader->texture_modes = 1;
	pixel_shader->combiner_count = 1;
	pixel_shader->rgb_inputs[0] = 0x08040000;
	pixel_shader->rgb_outputs[0] = 0xC0;
	pixel_shader->alpha_inputs[0] = 0x18140000;
	pixel_shader->alpha_outputs[0] = 0xC0;
	pixel_shader->final_combiner_inputs_abcd = 0xC;
	pixel_shader->final_combiner_inputs_efg = 0x1C00;
	site_end("detail_objects.c:443");

	/* rasterizer_xbox_dynavobgeom.c:448-492, the meters of the HUD */
	for (a = 0; a < 2; a++)
	{
		site_begin(1);
		site_blend(BLEND_CONSTANTCOLOR, BLEND_SRCALPHA, BLENDOP_ADD);
		site.blend_color = COLOR_D; /* tint_color */
		site.alpha_kill = 1;
		pixel_shader->texture_modes = 1;
		pixel_shader->combiner_count = 0x00011104;
		pixel_shader->constant_0[0] = COLOR_A; /* gradient_min_color */
		pixel_shader->constant_1[0] = 0x20000000UL | (COLOR_B & 0x00FFFFFF); /* 1/8, gradient_max_color */
		pixel_shader->alpha_inputs[0] = 0x12081208;
		pixel_shader->rgb_inputs[0] = 0x1120E820;
		pixel_shader->alpha_outputs[0] = 0x00020C00;
		pixel_shader->rgb_outputs[0] = 0x00020C00;
		pixel_shader->constant_0[1] = COLOR_A;
		pixel_shader->constant_1[1] = COLOR_B;
		pixel_shader->alpha_inputs[1] = 0x6C200000;
		pixel_shader->alpha_outputs[1] = 0x000000C0;
		pixel_shader->rgb_inputs[1] = 0x3C011C02;
		pixel_shader->rgb_outputs[1] = 0x00000C00;
		pixel_shader->constant_0[2] = COLOR_A;
		pixel_shader->constant_1[2] = COLOR_C; /* flash_color */
		pixel_shader->alpha_inputs[2] = 0x0820B220;
		pixel_shader->alpha_outputs[2] = 0x00000C00;
		pixel_shader->rgb_inputs[2] = 0x0C201C00 | (a ? 0x000000E2 : 0x00000002);
		pixel_shader->rgb_outputs[2] = 0x00000C00;
		pixel_shader->constant_0[3] = COLOR_E; /* background_color */
		pixel_shader->constant_1[3] = COLOR_D; /* tint_color */
		pixel_shader->alpha_inputs[3] = 0x12201120;
		pixel_shader->alpha_outputs[3] = 0x00004C00;
		pixel_shader->rgb_inputs[3] = 0x0C200120;
		pixel_shader->rgb_outputs[3] = 0x00004C00;
		pixel_shader->final_combiner_inputs_abcd = 0x0C180000;
		pixel_shader->final_combiner_inputs_efg = 0x00001C00;
		site_end("dynavobgeom.c:448 meter%s", a ? ", negative flash" : "");
	}

	/* rasterizer_xbox_dynavobgeom.c:493-643, screen geometry of one to
	three maps: a = maps, b = how map 1 joins map 0, c = how map 2 joins,
	d = plasma, e = the draw's blend (alpha blend and add) */
	for (a = 1; a <= 3; a++)
	{
		for (b = 0; b < (a >= 2 ? NUMBER_OF_BLEND_FUNCTIONS - 1 : 1); b++)
		{
			for (c = 0; c < (a >= 3 ? 5 : 1); c++)
			{
				for (d = 0; d < (a >= 3 && c == 0 && b == _blend_function_min ? 2 : 1); d++)
				{
					for (e = 0; e < 2; e++)
					{
						/* the functions map 2 can join by */
						static const int second_functions[5] =
						{
							_blend_function_alpha_blend, _blend_function_multiply, _blend_function_double_multiply,
							_blend_function_add, _blend_function_reverse_subtract
						};
						int combiner_index = 2;

						site_begin(1);
						site_blend_function(e ? _blend_function_add : _blend_function_alpha_blend);
						pixel_shader->texture_modes = ((((a >= 3 ? 1UL : 0UL) << 5) | (a >= 2 ? 1 : 0)) << 5) | 1;
						pixel_shader->constant_0[0] = COLOR_A;
						pixel_shader->constant_1[0] = COLOR_B;
						pixel_shader->constant_0[1] = COLOR_C;
						pixel_shader->constant_0[4] = COLOR_D;
						pixel_shader->constant_0[5] = COLOR_D;
						pixel_shader->constant_0[6] = COLOR_D;
						pixel_shader->constant_0[7] = COLOR_D;
						pixel_shader->rgb_outputs[0] = 0x00000089;
						pixel_shader->alpha_outputs[0] = 0x00000089;
						pixel_shader->rgb_inputs[0] = 0x08010902;
						pixel_shader->alpha_inputs[0] = 0x18111912;
						pixel_shader->rgb_inputs[1] = 0x0A010804;
						pixel_shader->rgb_outputs[1] = 0x000000AC;
						pixel_shader->alpha_inputs[1] = 0x1A111814;
						pixel_shader->alpha_outputs[1] = 0x000000AC;
						if (a >= 2)
						{
							switch (b)
							{
							case _blend_function_alpha_blend:
								pixel_shader->rgb_inputs[2] = 0x0C200920;
								pixel_shader->alpha_inputs[2] = 0x1C201920;
								pixel_shader->alpha_outputs[2] = 0x00000C00;
								pixel_shader->rgb_outputs[2] = 0x00000C00;
								break;
							case _blend_function_multiply:
								pixel_shader->rgb_inputs[2] = 0x0C090000;
								pixel_shader->rgb_outputs[2] = 0x000000C0;
								pixel_shader->alpha_inputs[2] = 0x1C190000;
								pixel_shader->alpha_outputs[2] = 0x000000C0;
								break;
							case _blend_function_double_multiply:
								pixel_shader->rgb_inputs[2] = 0x0C20E920;
								pixel_shader->alpha_inputs[2] = 0x1C20F920;
								pixel_shader->alpha_outputs[2] = 0x00000C00;
								pixel_shader->rgb_outputs[2] = 0x00000C00;
								break;
							case _blend_function_add:
								pixel_shader->rgb_inputs[2] = 0x0C090000;
								pixel_shader->rgb_outputs[2] = 0x000100C0;
								pixel_shader->alpha_inputs[2] = 0x1C190000;
								pixel_shader->alpha_outputs[2] = 0x000100C0;
								break;
							case _blend_function_reverse_subtract:
								pixel_shader->rgb_inputs[2] = 0x0C090000;
								pixel_shader->rgb_outputs[2] = 0x000020C0;
								pixel_shader->alpha_inputs[2] = 0x1C190000;
								pixel_shader->alpha_outputs[2] = 0x000000C0;
								break;
							case _blend_function_min:
								pixel_shader->alpha_inputs[2] = 0x0820A920;
								pixel_shader->rgb_inputs[2] = 0x1920B820;
								pixel_shader->alpha_inputs[3] = 0x1C1C0C0C;
								pixel_shader->alpha_outputs[3] = 0x00024C00;
								pixel_shader->rgb_inputs[3] = 0x00000000;
								pixel_shader->rgb_outputs[3] = 0x00000000;
								pixel_shader->alpha_inputs[4] = 0x00005C5C;
								pixel_shader->alpha_outputs[4] = 0x00004D00;
								pixel_shader->rgb_inputs[4] = 0x00000000;
								pixel_shader->rgb_outputs[4] = 0x00000000;
								combiner_index = 5;
								pixel_shader->alpha_inputs[5] = 0x00000000;
								pixel_shader->alpha_outputs[5] = 0x00000C00;
								pixel_shader->rgb_inputs[5] = 0x1CA01DA0;
								pixel_shader->rgb_outputs[5] = 0x00000C00;
								pixel_shader->alpha_outputs[2] = 0x00000C00;
								pixel_shader->rgb_outputs[2] = 0x00000C00;
								break;
							default:
								/* max: the switch has no case, the stage stays empty */
								break;
							}
							combiner_index++;
						}
						if (a >= 3)
						{
							switch (second_functions[c])
							{
							case _blend_function_alpha_blend:
								if (b == _blend_function_min)
									pixel_shader->rgb_inputs[combiner_index] = 0x0C010A00 | (d ? 0x00000004 : 0x00000020);
								else
									pixel_shader->rgb_inputs[combiner_index] = 0x0C200A20;
								pixel_shader->rgb_outputs[combiner_index] = 0x00000C00;
								pixel_shader->alpha_inputs[combiner_index] = 0x1C201A20;
								pixel_shader->alpha_outputs[combiner_index] = 0x00000C00;
								break;
							case _blend_function_multiply:
								pixel_shader->rgb_inputs[combiner_index] = 0x0C0A0000;
								pixel_shader->rgb_outputs[combiner_index] = 0x000000C0;
								pixel_shader->alpha_inputs[combiner_index] = 0x1C1A0000;
								pixel_shader->alpha_outputs[combiner_index] = 0x000000C0;
								break;
							case _blend_function_double_multiply:
								pixel_shader->rgb_inputs[combiner_index] = 0x0C20EA20;
								pixel_shader->rgb_outputs[combiner_index] = 0x00000C00;
								pixel_shader->alpha_inputs[combiner_index] = 0x1C20FA20;
								pixel_shader->alpha_outputs[combiner_index] = 0x00000C00;
								break;
							case _blend_function_add:
								pixel_shader->rgb_inputs[combiner_index] = 0x0C0A0000;
								pixel_shader->rgb_outputs[combiner_index] = 0x000100C0;
								pixel_shader->alpha_inputs[combiner_index] = 0x1C1A0000;
								pixel_shader->alpha_outputs[combiner_index] = 0x000100C0;
								break;
							default:
								pixel_shader->rgb_inputs[combiner_index] = 0x0C0A0000;
								pixel_shader->rgb_outputs[combiner_index] = 0x000020C0;
								pixel_shader->alpha_inputs[combiner_index] = 0x1C1A0000;
								pixel_shader->alpha_outputs[combiner_index] = 0x000000C0;
								break;
							}
							combiner_index++;
						}
						pixel_shader->combiner_count = combiner_index | 0x00011100;
						pixel_shader->final_combiner_inputs_abcd = 0x0000000C;
						pixel_shader->final_combiner_inputs_efg = 0x00001C00;
						if (a == 1)
							site_end("dynavobgeom.c:493 1 map, drawn %s", e ? "add" : "alpha");
						else if (a == 2)
							site_end("dynavobgeom.c:493 2 maps %s, drawn %s", blend_function_names[b], e ? "add" : "alpha");
						else
							site_end("dynavobgeom.c:493 3 maps %s %s%s, drawn %s", blend_function_names[b],
								blend_function_names[second_functions[c]], d ? " plasma" : "", e ? "add" : "alpha");
					}
				}
			}
		}
	}

	/* rasterizer_xbox_environment.c:722-758, vectors (debugging) */
	for (a = 0; a < 2; a++)
	{
		site_begin(0);
		site.depth_write = 1;
		pixel_shader->texture_modes = 0x00018C60 | (a ? 1 : 3);
		if (a)
		{
			pixel_shader->combiner_count = 3;
			pixel_shader->constant_0[0] = 0x00FF0000;
			pixel_shader->constant_1[0] = 0x000000FF;
			pixel_shader->rgb_inputs[0] = 0x4849484A;
			pixel_shader->rgb_outputs[0] = 0x000030CD;
			pixel_shader->rgb_inputs[1] = 0x0C0C0D0D;
			pixel_shader->rgb_outputs[1] = 0x000000CD;
			pixel_shader->rgb_inputs[2] = 0x0C010D02;
			pixel_shader->rgb_outputs[2] = 0x00000C00;
			pixel_shader->final_combiner_inputs_abcd = 8 | 0x18200000;
		}
		else
		{
			pixel_shader->combiner_count = 1;
			pixel_shader->final_combiner_inputs_abcd = 9;
		}
		site_end("environment.c:722 vectors%s", a ? ", test pattern" : "");
	}

	/* rasterizer_xbox_environment.c:874-1067, the lightmap pass: a = self
	illumination, b = lightmap missing, c = bump, d = lightmap mode */
	for (a = 0; a < 2; a++)
	{
		for (b = 0; b < 2; b++)
		{
			for (c = 0; c < 2; c++)
			{
				for (d = 0; d < 3; d++)
				{
					site_begin(1);
					site.depth_write = 1;
					site.write_alpha = 1;
					/* the lightmap (stage 2), the self illumination map (1), the bump map (0) */
					site.stage_rank[0] = 1;
					site.stage_rank[1] = 2;
					site.stage_rank[2] = 3;
					if (!a)
					{
						pixel_shader->combiner_count = 2;
						pixel_shader->texture_modes = b ? 0x00018001 : 0x00018401;
						pixel_shader->rgb_inputs[0] = c ? 0x484B0A01 : 0x20200000;
						pixel_shader->rgb_outputs[0] = 0x0000208C;
						pixel_shader->alpha_inputs[1] = 0x34201408;
						pixel_shader->alpha_outputs[1] = 0x00000C00;
						pixel_shader->final_combiner_inputs_abcd = 0x0A0F000C;
						pixel_shader->final_combiner_inputs_efg = 0x1C011800;
					}
					else
					{
						pixel_shader->texture_modes = b ? 0x00018021 : 0x00018421;
						pixel_shader->combiner_count = 0x00011106;
						pixel_shader->constant_0[0] = 0x90000000UL; /* plasma_value */
						pixel_shader->alpha_inputs[0] = 0x1120B920;
						pixel_shader->alpha_outputs[0] = 0x00000C00;
						pixel_shader->rgb_inputs[0] = 0x1920B120;
						pixel_shader->rgb_outputs[0] = 0x00000C00;
						pixel_shader->alpha_inputs[1] = 0xDCDCCCCC;
						pixel_shader->alpha_outputs[1] = 0x00024C00;
						pixel_shader->rgb_inputs[1] = c ? 0x484B0A01 : 0x20200000;
						pixel_shader->rgb_outputs[1] = 0x00002080;
						pixel_shader->constant_0[2] = 0x00FF0000;
						pixel_shader->constant_1[2] = 0x0000FF00;
						pixel_shader->alpha_inputs[2] = 0x1C1C0920;
						pixel_shader->alpha_outputs[2] = 0x000000C9;
						pixel_shader->rgb_inputs[2] = 0x09010902;
						pixel_shader->rgb_outputs[2] = 0x000030CD;
						pixel_shader->constant_0[3] = COLOR_A & 0x00FFFFFF; /* primary_color */
						pixel_shader->constant_1[3] = COLOR_B & 0x00FFFFFF; /* secondary_color */
						pixel_shader->alpha_inputs[3] = 0x00005C5C;
						pixel_shader->alpha_outputs[3] = 0x00004C00;
						pixel_shader->rgb_inputs[3] = 0x0C010D02;
						pixel_shader->rgb_outputs[3] = 0x00000D00;
						pixel_shader->constant_0[4] = COLOR_C & 0x00FFFFFF; /* plasma_on_color */
						pixel_shader->constant_1[4] = COLOR_D & 0x00FFFFFF; /* plasma_off_color */
						pixel_shader->alpha_inputs[4] = 0x34201408;
						pixel_shader->alpha_outputs[4] = 0x00000C00;
						pixel_shader->rgb_inputs[4] = 0x011C0220;
						pixel_shader->rgb_outputs[4] = 0x00000C00;
						pixel_shader->rgb_inputs[5] = 0x0C190D20;
						pixel_shader->rgb_outputs[5] = 0x00000C00;
						pixel_shader->final_combiner_inputs_abcd = 0x0A0F000C;
						pixel_shader->final_combiner_inputs_efg = 0x1C011800;
					}
					pixel_shader->final_combiner_constant_0 = COLOR_E & 0x00FFFFFF; /* material_color */
					if (d == 1)
					{
						pixel_shader->final_combiner_inputs_abcd = 0x2004000C;
					}
					else if (d == 2)
					{
						pixel_shader->final_combiner_constant_1 = 0x00404040;
						pixel_shader->final_combiner_inputs_abcd = 0x2002000C;
					}
					site_end("environment.c:874 lightmap%s%s%s%s", a ? ", self illumination" : "",
						b ? ", lightmap missing" : "", c ? ", bump" : "",
						d == 1 ? ", no lightmap mode" : d == 2 ? ", fullbright mode" : "");
				}
			}
		}
	}
	/* the lightmap pass in the drawing modes that show one input (debugging) */
	for (a = 0; a < 3; a++)
	{
		site_begin(0);
		site.depth_write = 1;
		site.write_alpha = 1;
		pixel_shader->texture_modes = 0x00018401;
		pixel_shader->combiner_count = 1;
		pixel_shader->final_combiner_inputs_efg = 0x1C011800;
		pixel_shader->final_combiner_constant_0 = COLOR_E & 0x00FFFFFF;
		if (a == 0)
		{
			pixel_shader->final_combiner_inputs_abcd = 0x00000008;
		}
		else if (a == 1)
		{
			pixel_shader->final_combiner_inputs_abcd = 0x00000020;
		}
		else
		{
			pixel_shader->alpha_inputs[0] = 0x48402020;
			pixel_shader->alpha_outputs[0] = 0x00020D00;
			pixel_shader->final_combiner_inputs_abcd = 0x0000001D;
		}
		site_end("environment.c:1028 lightmap drawing mode %s", a == 0 ? "bump colour" : a == 1 ? "specular mask" : "bump edge");
	}

	/* rasterizer_xbox_environment.c:1441-1456, dynamic diffuse lights */
	site_begin(0);
	site_blend(BLEND_ONE, BLEND_ONE, BLENDOP_ADD);
	site.alpha_test = 1;
	site.alpha_reference = 0;
	pixel_shader->texture_modes = 0x00018861;
	pixel_shader->combiner_count = 4;
	pixel_shader->alpha_inputs[0] = 0x4B204B20;
	pixel_shader->alpha_outputs[0] = 0x00020C00;
	pixel_shader->rgb_inputs[0] = 0x090A484B;
	pixel_shader->rgb_outputs[0] = 0x000010CD;
	pixel_shader->rgb_inputs[1] = 0x0C0D0000;
	pixel_shader->rgb_outputs[1] = 0x000000C0;
	pixel_shader->rgb_inputs[2] = 0x0C1C0000;
	pixel_shader->rgb_outputs[2] = 0x000000C0;
	pixel_shader->rgb_inputs[3] = 0x0C010C01;
	pixel_shader->rgb_outputs[3] = 0x000010CD;
	pixel_shader->final_combiner_inputs_abcd = 0x0C010000;
	pixel_shader->final_combiner_inputs_efg = 0x00000D00;
	site_end("environment.c:1441 diffuse lights");

	/* rasterizer_xbox_environment.c:1681-1802, the diffuse textures: a =
	shader type, b = detail function, c = micro detail function, d =
	drawing mode (normal; overdraw and specular mask are debugging) */
	for (a = 0; a < 3; a++)
	{
		for (b = 0; b < 3; b++)
		{
			for (c = 0; c < 3; c++)
			{
				for (d = 0; d < 3; d++)
				{
					if (d != 0 && (a != 0 || b != 0 || c != 0))
						continue;
					site_begin(d == 0);
					site_blend(d == 1 ? BLEND_ONE : BLEND_DESTCOLOR, d == 1 ? BLEND_ONE : BLEND_ZERO, BLENDOP_ADD);
					site.write_alpha = 1; /* environment_specular_mask_enabled */
					pixel_shader->texture_modes = 0x00008421;
					pixel_shader->combiner_count = 3;
					switch (a)
					{
					case 0:
						pixel_shader->alpha_inputs[0] = 0x3A1A1A19;
						pixel_shader->rgb_inputs[0] = 0x3A0A1A09;
						pixel_shader->alpha_inputs[1] = 0x181C0000;
						pixel_shader->alpha_outputs[1] = 0x000000C0;
						pixel_shader->alpha_outputs[0] = 0x00000C00;
						pixel_shader->rgb_outputs[0] = 0x00000C00;
						break;
					case 1:
						pixel_shader->alpha_inputs[0] = 0x381A1819;
						pixel_shader->rgb_inputs[0] = 0x380A1809;
						pixel_shader->alpha_outputs[1] = 0;
						pixel_shader->alpha_outputs[0] = 0x00000C00;
						pixel_shader->rgb_outputs[0] = 0x00000C00;
						break;
					default:
						pixel_shader->alpha_inputs[0] = 0x18200000;
						pixel_shader->alpha_outputs[0] = 0x000000C0;
						pixel_shader->rgb_inputs[0] = 0x380A1809;
						pixel_shader->alpha_outputs[1] = 0;
						pixel_shader->rgb_outputs[0] = 0x00000C00;
						break;
					}
					switch (b)
					{
					case 0:
						pixel_shader->rgb_inputs[1] = 0x080C080C;
						pixel_shader->rgb_outputs[1] = 0x00000C00;
						break;
					case 1:
						pixel_shader->rgb_inputs[1] = 0x080C0000;
						pixel_shader->rgb_outputs[1] = 0x00000C00;
						break;
					default:
						pixel_shader->rgb_inputs[1] = 0x08204C20;
						pixel_shader->rgb_outputs[1] = 0x00000C00;
						break;
					}
					pixel_shader->alpha_inputs[2] = 0x1C1B0000;
					pixel_shader->alpha_outputs[2] = 0x000000C0;
					switch (c)
					{
					case 0:
						pixel_shader->rgb_inputs[2] = 0x0C0B0C0B;
						pixel_shader->rgb_outputs[2] = 0x00000C00;
						break;
					case 1:
						pixel_shader->rgb_inputs[2] = 0x0C0B0000;
						pixel_shader->rgb_outputs[2] = 0x00000C00;
						break;
					default:
						pixel_shader->rgb_inputs[2] = 0x0C204B20;
						pixel_shader->rgb_outputs[2] = 0x00000C00;
						break;
					}
					pixel_shader->final_combiner_inputs_efg = 0x00001C00;
					if (d == 0)
					{
						pixel_shader->final_combiner_inputs_abcd = 0x0000000C;
					}
					else if (d == 1)
					{
						pixel_shader->final_combiner_constant_0 = 0x54000000UL; /* alpha 0.33 */
						pixel_shader->final_combiner_inputs_abcd = 0x0C110000;
					}
					else
					{
						pixel_shader->final_combiner_inputs_abcd = 0x0000001C;
					}
					site_end("environment.c:1681 diffuse textures %s, detail %s, micro %s%s",
						a == 0 ? "normal" : a == 1 ? "blended" : "blended base specular",
						b == 0 ? "double biased multiply" : b == 1 ? "multiply" : "double biased add",
						c == 0 ? "double biased multiply" : c == 1 ? "multiply" : "double biased add",
						d == 1 ? ", overdraw mode" : d == 2 ? ", specular mask mode" : "");
				}
			}
		}
	}

	/* rasterizer_xbox_environment.c:1953-1984, specular lights */
	site_begin(0);
	site_blend(BLEND_DESTALPHA, BLEND_ONE, BLENDOP_ADD);
	site.alpha_test = 1;
	site.alpha_reference = 0;
	pixel_shader->texture_modes = 0x00018C41;
	pixel_shader->combiner_count = 0x00011006;
	pixel_shader->constant_0[0] = 0x000000FF;
	pixel_shader->alpha_inputs[0] = 0x4B204B20;
	pixel_shader->alpha_outputs[0] = 0x00020400;
	pixel_shader->rgb_inputs[0] = 0x484A0000;
	pixel_shader->rgb_outputs[0] = 0x000020C0;
	pixel_shader->constant_0[1] = 0x000000FF;
	pixel_shader->alpha_inputs[1] = 0x4A204A20;
	pixel_shader->alpha_outputs[1] = 0x00020500;
	pixel_shader->rgb_inputs[1] = 0x48CC8A40;
	pixel_shader->rgb_outputs[1] = 0x00010D00;
	pixel_shader->alpha_inputs[2] = 0x2C120C11;
	pixel_shader->alpha_outputs[2] = 0x00000C00;
	pixel_shader->rgb_inputs[2] = 0xCD4B0809;
	pixel_shader->rgb_outputs[2] = 0x000020D0;
	pixel_shader->alpha_inputs[3] = 0x0D0D1415;
	pixel_shader->alpha_outputs[3] = 0x000000D5;
	pixel_shader->rgb_inputs[3] = 0x2C020C01;
	pixel_shader->rgb_outputs[3] = 0x00000C00;
	pixel_shader->alpha_inputs[4] = 0x1D1D151C;
	pixel_shader->alpha_outputs[4] = 0x000000D5;
	pixel_shader->rgb_inputs[4] = 0x0C091C09;
	pixel_shader->rgb_outputs[4] = 0x000110CD;
	pixel_shader->alpha_inputs[5] = 0x1D1D0000;
	pixel_shader->alpha_outputs[5] = 0x000000D0;
	pixel_shader->rgb_inputs[5] = 0x0C150D1D;
	pixel_shader->rgb_outputs[5] = 0x000010CD;
	pixel_shader->final_combiner_inputs_abcd = 0x0C0F0000;
	pixel_shader->final_combiner_inputs_efg = 0x1D200D00;
	site_end("environment.c:1953 specular lights");

	/* rasterizer_xbox_environment.c:2125-2173, the same for each shader:
	a = the bump map is the specular mask, b = overbright, c = extra shiny */
	for (a = 0; a < 2; a++)
	{
		for (b = 0; b < 2; b++)
		{
			for (c = 0; c < 2; c++)
			{
				struct site_shader begun = *pixel_shader;

				site_begin(0);
				site_blend(BLEND_DESTALPHA, BLEND_ONE, BLENDOP_ADD);
				site.alpha_test = 1;
				site.alpha_reference = 0;
				*pixel_shader = begun;
				pixel_shader->constant_0[2] = 0xa0000000UL; /* brightness */
				pixel_shader->constant_1[2] = 0xa0000000UL;
				pixel_shader->constant_0[3] = COLOR_A & 0x00FFFFFF; /* view perpendicular colour */
				pixel_shader->constant_1[3] = COLOR_B & 0x00FFFFFF; /* view parallel colour */
				pixel_shader->rgb_inputs[0] = a ? 0x014A0000 : 0x484A0000;
				pixel_shader->rgb_inputs[1] = a ? 0x01CC8A40 : 0x48CC8A40;
				pixel_shader->rgb_outputs[2] = a ? 0x000020D9 : 0x000020D0;
				pixel_shader->rgb_outputs[4] = b ? 0x000210CD : 0x000110CD;
				pixel_shader->combiner_count = c ? 0x00011008 : 0x00011006;
				pixel_shader->alpha_inputs[6] = pixel_shader->alpha_inputs[7] = c ? 0x1D1D0000 : 0;
				pixel_shader->alpha_outputs[6] = pixel_shader->alpha_outputs[7] = c ? 0x000000D0 : 0;
				site_end("environment.c:2125 specular lights%s%s%s", a ? ", bump map is mask" : "", b ? ", overbright" : "",
					c ? ", extra shiny" : "");
			}
		}
	}

	/* rasterizer_xbox_environment.c:2208-2240, the specular lightmap */
	site_begin(0);
	site_blend(BLEND_DESTALPHA, BLEND_ONE, BLENDOP_ADD);
	site.alpha_test = 1;
	site.alpha_reference = 0;
	pixel_shader->texture_modes = 0x00018C21;
	pixel_shader->combiner_count = 0x00011006;
	pixel_shader->constant_0[0] = 0x800000FF;
	pixel_shader->alpha_inputs[0] = 0x4B204B20;
	pixel_shader->alpha_outputs[0] = 0x00020400;
	pixel_shader->rgb_inputs[0] = 0x484A0911;
	pixel_shader->rgb_outputs[0] = 0x000030C9;
	pixel_shader->constant_0[1] = 0x000000FF;
	pixel_shader->alpha_inputs[1] = 0x4A204A20;
	pixel_shader->alpha_outputs[1] = 0x00020500;
	pixel_shader->rgb_inputs[1] = 0x48CC8A40;
	pixel_shader->rgb_outputs[1] = 0x00010D00;
	pixel_shader->alpha_inputs[2] = 0x2C120C11;
	pixel_shader->alpha_outputs[2] = 0x00000C00;
	pixel_shader->rgb_inputs[2] = 0xCD4B0809;
	pixel_shader->rgb_outputs[2] = 0x000020D0;
	pixel_shader->alpha_inputs[3] = 0x0D0D1415;
	pixel_shader->alpha_outputs[3] = 0x000000D5;
	pixel_shader->rgb_inputs[3] = 0x2C020C01;
	pixel_shader->rgb_outputs[3] = 0x00000C00;
	pixel_shader->alpha_inputs[4] = 0x1D1D151C;
	pixel_shader->alpha_outputs[4] = 0x000000D5;
	pixel_shader->rgb_inputs[4] = 0x0C091C09;
	pixel_shader->rgb_outputs[4] = 0x000110CD;
	pixel_shader->alpha_inputs[5] = 0x1D1D0000;
	pixel_shader->alpha_outputs[5] = 0x000000D0;
	pixel_shader->rgb_inputs[5] = 0x0C150D1D;
	pixel_shader->rgb_outputs[5] = 0x000010CD;
	pixel_shader->final_combiner_constant_0 = 0x80000000;
	pixel_shader->final_combiner_inputs_abcd = 0x0C0F0000;
	pixel_shader->final_combiner_inputs_efg = 0x1D110D00;
	site_end("environment.c:2208 specular lightmap");

	/* rasterizer_xbox_environment.c:2405-2451, the same for each shader:
	a = the bump map is the specular mask, b = overbright, c = extra shiny */
	for (a = 0; a < 2; a++)
	{
		for (b = 0; b < 2; b++)
		{
			for (c = 0; c < 2; c++)
			{
				struct site_shader begun = *pixel_shader;

				site_begin(0);
				site_blend(BLEND_DESTALPHA, BLEND_ONE, BLENDOP_ADD);
				site.alpha_test = 1;
				site.alpha_reference = 0;
				*pixel_shader = begun;
				pixel_shader->constant_0[2] = 0xa0000000UL; /* brightness */
				pixel_shader->constant_1[2] = 0xa0000000UL;
				pixel_shader->constant_0[3] = COLOR_A & 0x00FFFFFF; /* view perpendicular colour */
				pixel_shader->constant_1[3] = COLOR_B & 0x00FFFFFF; /* view parallel colour */
				pixel_shader->rgb_inputs[0] = a ? 0x014A0911 : 0x484A0911;
				pixel_shader->rgb_inputs[1] = a ? 0x01CC8A40 : 0x48CC8A40;
				pixel_shader->rgb_outputs[2] = a ? 0x000020D9 : 0x000020D0;
				pixel_shader->rgb_outputs[4] = b ? 0x000210CD : 0x000110CD;
				pixel_shader->combiner_count = c ? 0x00011008 : 0x00011006;
				pixel_shader->alpha_inputs[6] = pixel_shader->alpha_inputs[7] = c ? 0x1D1D0000 : 0;
				pixel_shader->alpha_outputs[6] = pixel_shader->alpha_outputs[7] = c ? 0x000000D0 : 0;
				site_end("environment.c:2405 specular lightmap%s%s%s", a ? ", bump map is mask" : "", b ? ", overbright" : "",
					c ? ", extra shiny" : "");
			}
		}
	}

	/* rasterizer_xbox_environment.c:2449-2459, the reflection's lightmap mask */
	site_begin(0);
	site_blend(BLEND_DESTALPHA, BLEND_ZERO, BLENDOP_ADD);
	site.write_color = 0;
	site.write_alpha = 1;
	pixel_shader->texture_modes = 1;
	pixel_shader->combiner_count = 2;
	pixel_shader->constant_0[0] = 0x0080B050;
	pixel_shader->rgb_inputs[0] = 0x08010000;
	pixel_shader->rgb_outputs[0] = 0x000020C0;
	pixel_shader->alpha_inputs[1] = 0x2C120C20;
	pixel_shader->alpha_outputs[1] = 0x00000C00;
	pixel_shader->final_combiner_inputs_abcd = 0;
	pixel_shader->final_combiner_inputs_efg = 0x00001C00;
	site_end("environment.c:2449 reflection lightmap mask");

	/* rasterizer_xbox_environment.c:2699-2803 mirrors and 2979-3114
	reflections: a = site and texture modes, b = no bump map (the view
	direction constant), c = bump map is specular mask */
	for (a = 0; a < 3; a++)
	{
		for (b = 0; b < 2; b++)
		{
			for (c = 0; c < 2; c++)
			{
				if (a == 1 && b == 0)
					/* the bumped reflections always take the constant */
					continue;
				site_begin(0);
				site_blend(BLEND_DESTALPHA, BLEND_ONE, BLENDOP_ADD);
				if (a == 0)
				{
					pixel_shader->texture_modes = PS_TEXTUREMODES(
						PS_TEXTUREMODES_PROJECT2D, PS_TEXTUREMODES_CUBEMAP, PS_TEXTUREMODES_CUBEMAP,
						PS_TEXTUREMODES_PROJECT2D);
				}
				else if (a == 1)
				{
					pixel_shader->texture_modes = PS_TEXTUREMODES(
						PS_TEXTUREMODES_PROJECT2D, PS_TEXTUREMODES_DOTPRODUCT, PS_TEXTUREMODES_DOT_RFLCT_DIFF,
						PS_TEXTUREMODES_DOT_RFLCT_SPEC);
					pixel_shader->input_texture = PS_INPUTTEXTURE(0, 0, 0, 0);
					pixel_shader->dot_mapping = PS_DOTMAPPING(
						PS_DOTMAPPING_ZERO_TO_ONE, PS_DOTMAPPING_MINUS1_TO_1_D3D, PS_DOTMAPPING_MINUS1_TO_1_D3D,
						PS_DOTMAPPING_MINUS1_TO_1_D3D);
				}
				else
				{
					pixel_shader->texture_modes = PS_TEXTUREMODES(
						PS_TEXTUREMODES_PROJECT2D, PS_TEXTUREMODES_CUBEMAP, PS_TEXTUREMODES_CUBEMAP,
						PS_TEXTUREMODES_CUBEMAP);
				}
				pixel_shader->combiner_count = PS_COMBINERCOUNT(5, PS_COMBINERCOUNT_UNIQUE_C0 | PS_COMBINERCOUNT_UNIQUE_C1);
				if (b)
				{
					pixel_shader->constant_0[0] = 0x00a04060; /* mirror_color */
					pixel_shader->rgb_inputs[0] = PS_COMBINERINPUTS(
						PS_REGISTER_T2 | PS_INPUTMAPPING_EXPAND_NORMAL, PS_REGISTER_C0 | PS_INPUTMAPPING_EXPAND_NORMAL,
						PS_REGISTER_T3, PS_REGISTER_T3);
				}
				else
				{
					pixel_shader->rgb_inputs[0] = PS_COMBINERINPUTS(
						PS_REGISTER_T1 | PS_INPUTMAPPING_EXPAND_NORMAL, PS_REGISTER_T0 | PS_INPUTMAPPING_EXPAND_NORMAL,
						PS_REGISTER_T3, PS_REGISTER_T3);
				}
				pixel_shader->rgb_outputs[0] = PS_COMBINEROUTPUTS(
					PS_REGISTER_R0, PS_REGISTER_R1, PS_REGISTER_DISCARD, PS_COMBINEROUTPUT_AB_DOT_PRODUCT);
				pixel_shader->rgb_inputs[1] = PS_COMBINERINPUTS(PS_REGISTER_R0, PS_REGISTER_R0, PS_REGISTER_R1, PS_REGISTER_R1);
				pixel_shader->rgb_outputs[1] = PS_COMBINEROUTPUTS(
					PS_REGISTER_R0, PS_REGISTER_R1, PS_REGISTER_DISCARD, PS_COMBINEROUTPUT_IDENTITY);
				pixel_shader->rgb_inputs[2] = PS_COMBINERINPUTS(PS_REGISTER_R0, PS_REGISTER_R0, PS_REGISTER_R1, PS_REGISTER_R1);
				pixel_shader->rgb_outputs[2] = PS_COMBINEROUTPUTS(
					PS_REGISTER_DISCARD, PS_REGISTER_R1, PS_REGISTER_DISCARD, PS_COMBINEROUTPUT_IDENTITY);
				pixel_shader->constant_0[3] = COLOR_A;
				pixel_shader->constant_1[3] = COLOR_B;
				pixel_shader->alpha_inputs[3] = PS_COMBINERINPUTS(
					PS_REGISTER_R0 | PS_INPUTMAPPING_UNSIGNED_INVERT, PS_REGISTER_C1 | PS_CHANNEL_ALPHA,
					PS_REGISTER_R0, PS_REGISTER_C0 | PS_CHANNEL_ALPHA);
				pixel_shader->alpha_outputs[3] = PS_COMBINEROUTPUTS(
					PS_REGISTER_DISCARD, PS_REGISTER_DISCARD, PS_REGISTER_R0, PS_COMBINEROUTPUT_IDENTITY);
				pixel_shader->rgb_inputs[3] = PS_COMBINERINPUTS(
					PS_REGISTER_R0 | PS_INPUTMAPPING_UNSIGNED_INVERT, PS_REGISTER_C1, PS_REGISTER_R0, PS_REGISTER_C0);
				pixel_shader->rgb_outputs[3] = PS_COMBINEROUTPUTS(
					PS_REGISTER_DISCARD, PS_REGISTER_DISCARD, PS_REGISTER_R0, PS_COMBINEROUTPUT_IDENTITY);
				pixel_shader->rgb_inputs[4] = PS_COMBINERINPUTS(
					PS_REGISTER_R0 | PS_INPUTMAPPING_UNSIGNED_INVERT, PS_REGISTER_R1, PS_REGISTER_R0, PS_REGISTER_T3);
				pixel_shader->rgb_outputs[4] = PS_COMBINEROUTPUTS(
					PS_REGISTER_DISCARD, PS_REGISTER_DISCARD, PS_REGISTER_R0, PS_COMBINEROUTPUT_IDENTITY);
				pixel_shader->final_combiner_inputs_abcd = PS_COMBINERINPUTS(
					PS_REGISTER_R0, PS_REGISTER_EF_PROD, PS_REGISTER_ZERO, PS_REGISTER_ZERO);
				pixel_shader->final_combiner_inputs_efg = PS_COMBINERINPUTS(
					PS_REGISTER_R0 | PS_CHANNEL_ALPHA, c ? PS_REGISTER_T0 : PS_REGISTER_ONE, PS_REGISTER_ZERO, 0);
				site_end("environment.c:%s%s%s", a == 0 ? "2699 mirror" : a == 1 ? "2979 bumped reflection" : "2979 flat reflection",
					b ? ", no bump map" : "", c ? ", bump is specular mask" : "");
			}
		}
	}

	/* rasterizer_xbox_environment.c:3393-3424, specular spot light */
	site_begin(0);
	site_blend(BLEND_DESTALPHA, BLEND_ONE, BLENDOP_ADD);
	site.alpha_test = 1;
	site.alpha_reference = 0;
	pixel_shader->texture_modes = 0x00018C61;
	pixel_shader->combiner_count = 0x00011006;
	pixel_shader->constant_0[0] = 0x000000FF;
	pixel_shader->alpha_inputs[0] = 0x4B204B20;
	pixel_shader->alpha_outputs[0] = 0x00020400;
	pixel_shader->rgb_inputs[0] = 0x484A0000;
	pixel_shader->rgb_outputs[0] = 0x000020C0;
	pixel_shader->constant_0[1] = 0x000000FF;
	pixel_shader->alpha_inputs[1] = 0x4A204A20;
	pixel_shader->alpha_outputs[1] = 0x00020500;
	pixel_shader->rgb_inputs[1] = 0x48CC8A40;
	pixel_shader->rgb_outputs[1] = 0x00010D00;
	pixel_shader->alpha_inputs[2] = 0x2C120C11;
	pixel_shader->alpha_outputs[2] = 0x00000C00;
	pixel_shader->rgb_inputs[2] = 0xCD4B0809;
	pixel_shader->rgb_outputs[2] = 0x000020D0;
	pixel_shader->alpha_inputs[3] = 0x0D0D1415;
	pixel_shader->alpha_outputs[3] = 0x000000D5;
	pixel_shader->rgb_inputs[3] = 0x2C020C01;
	pixel_shader->rgb_outputs[3] = 0x00000C00;
	pixel_shader->alpha_inputs[4] = 0x1D1D151C;
	pixel_shader->alpha_outputs[4] = 0x000000D5;
	pixel_shader->rgb_inputs[4] = 0x0C091C09;
	pixel_shader->rgb_outputs[4] = 0x000110CD;
	pixel_shader->alpha_inputs[5] = 0x1D1D0000;
	pixel_shader->alpha_outputs[5] = 0x000000D0;
	pixel_shader->rgb_inputs[5] = 0x0C150D1D;
	pixel_shader->rgb_outputs[5] = 0x000010CD;
	pixel_shader->final_combiner_inputs_abcd = 0x0C0F0000;
	pixel_shader->final_combiner_inputs_efg = 0x1D330D00;
	site_end("environment.c:3393 specular spot light");

	/* rasterizer_xbox_environment_fog.c:726-750, atmospheric and planar fog */
	site_begin(1);
	site_blend(BLEND_ONE, BLEND_INVSRCALPHA, BLENDOP_ADD);
	site.alpha_test = 1;
	site.alpha_reference = 0;
	pixel_shader->texture_modes = 0x21;
	pixel_shader->combiner_count = 0x11002;
	pixel_shader->constant_0[0] = 0x60b0b0b0UL; /* alpha: eye density, intensity: maximum density */
	pixel_shader->constant_1[0] = 0x30505050UL;
	pixel_shader->alpha_inputs[0] = 0x02191209;
	pixel_shader->alpha_outputs[0] = 0xC00;
	pixel_shader->rgb_inputs[0] = 0x11180118;
	pixel_shader->rgb_outputs[0] = 0x48;
	pixel_shader->constant_0[1] = 0x80a0b0c0UL; /* eye density, atmospheric colour */
	pixel_shader->constant_1[1] = 0x00405060UL; /* planar colour */
	pixel_shader->alpha_inputs[1] = 0x283C311C;
	pixel_shader->alpha_outputs[1] = 0xCD;
	pixel_shader->rgb_inputs[1] = 0x0108021C;
	pixel_shader->rgb_outputs[1] = 0xCD;
	pixel_shader->final_combiner_inputs_abcd = 0x0C3D000F;
	pixel_shader->final_combiner_inputs_efg = 0x0D243C00;
	site_end("environment_fog.c:726 fog");

	/* rasterizer_xbox_environment_fog.c:1457-1489, the fog screen's depth
	pass over models: a = no texture, b = the chicago map's alpha kill
	(texture modes set to 0x21 afterwards, 1513) */
	for (a = 0; a < 2; a++)
	{
		for (b = 0; b < (a ? 1 : 2); b++)
		{
			site_begin(1);
			site.write_color = 0;
			site.write_alpha = 1;
			site.depth_write = 1;
			if (a)
			{
				pixel_shader->combiner_count = 1;
				pixel_shader->final_combiner_inputs_efg = 0x3300;
			}
			else
			{
				pixel_shader->texture_modes = b ? 0x21 : 1;
				pixel_shader->combiner_count = 1;
				pixel_shader->constant_0[0] = 0x70000000UL; /* near density / far density */
				pixel_shader->alpha_inputs[0] = 0x28110820;
				pixel_shader->alpha_outputs[0] = 0xC00;
				pixel_shader->final_combiner_inputs_efg = 0x3C00;
				if (b)
					site.alpha_kill = 2;
			}
			site_end("environment_fog.c:1457 fog screen depth%s%s", a ? ", no texture" : "", b ? ", chicago alpha kill" : "");
		}
	}

	/* rasterizer_xbox_environment_fog.c:1738-1850, the fog screen's layers:
	a = layer count, b = the three framebuffer states it draws with */
	for (a = 1; a <= 4; a++)
	{
		for (b = 0; b < 3; b++)
		{
			site_begin(1);
			if (b == 0)
				site_blend(BLEND_ONE, BLEND_SRCALPHA, BLENDOP_ADD);
			else
				site_blend(BLEND_INVDESTALPHA, BLEND_ONE, BLENDOP_ADD);
			if (b == 2)
			{
				site.write_color = 0;
				site.write_alpha = 1;
			}
			pixel_shader->texture_modes = PS_TEXTUREMODES(
				PS_TEXTUREMODES_PROJECT2D,
				a > 1 ? PS_TEXTUREMODES_PROJECT2D : PS_TEXTUREMODES_NONE,
				a > 2 ? PS_TEXTUREMODES_PROJECT2D : PS_TEXTUREMODES_NONE,
				a > 3 ? PS_TEXTUREMODES_PROJECT2D : PS_TEXTUREMODES_NONE);
			pixel_shader->combiner_count = PS_COMBINERCOUNT(4, PS_COMBINERCOUNT_UNIQUE_C0 | PS_COMBINERCOUNT_UNIQUE_C1);
			pixel_shader->constant_0[0] = 0x00ff0000UL; /* the layers' channel selectors */
			pixel_shader->constant_1[0] = 0x0000ff00UL;
			pixel_shader->rgb_inputs[0] = PS_COMBINERINPUTS(PS_REGISTER_T0, PS_REGISTER_C0, PS_REGISTER_T1, PS_REGISTER_C1);
			pixel_shader->rgb_outputs[0] = PS_COMBINEROUTPUTS(
				PS_REGISTER_T0, PS_REGISTER_T1, PS_REGISTER_DISCARD,
				PS_COMBINEROUTPUT_AB_DOT_PRODUCT | PS_COMBINEROUTPUT_CD_DOT_PRODUCT);
			pixel_shader->constant_0[1] = 0x000000ffUL;
			pixel_shader->constant_1[1] = 0x00ff0000UL;
			pixel_shader->alpha_inputs[1] = PS_COMBINERINPUTS(
				PS_REGISTER_T0 | PS_INPUTMAPPING_UNSIGNED_INVERT,
				a > 1 ? PS_REGISTER_T1 | PS_INPUTMAPPING_UNSIGNED_INVERT : PS_REGISTER_ONE,
				PS_REGISTER_ZERO, PS_REGISTER_ZERO);
			pixel_shader->alpha_outputs[1] = PS_COMBINEROUTPUTS(
				PS_REGISTER_R0, PS_REGISTER_DISCARD, PS_REGISTER_DISCARD, PS_COMBINEROUTPUT_IDENTITY);
			pixel_shader->rgb_inputs[1] = PS_COMBINERINPUTS(PS_REGISTER_T2, PS_REGISTER_C0, PS_REGISTER_T3, PS_REGISTER_C1);
			pixel_shader->rgb_outputs[1] = PS_COMBINEROUTPUTS(
				PS_REGISTER_T2, PS_REGISTER_T3, PS_REGISTER_DISCARD,
				PS_COMBINEROUTPUT_AB_DOT_PRODUCT | PS_COMBINEROUTPUT_CD_DOT_PRODUCT);
			pixel_shader->alpha_inputs[2] = PS_COMBINERINPUTS(
				a > 2 ? PS_REGISTER_T2 | PS_INPUTMAPPING_UNSIGNED_INVERT : PS_REGISTER_ONE,
				a > 3 ? PS_REGISTER_T3 | PS_INPUTMAPPING_UNSIGNED_INVERT : PS_REGISTER_ONE,
				PS_REGISTER_ZERO, PS_REGISTER_ZERO);
			pixel_shader->alpha_outputs[2] = PS_COMBINEROUTPUTS(
				PS_REGISTER_R1, PS_REGISTER_DISCARD, PS_REGISTER_DISCARD, PS_COMBINEROUTPUT_IDENTITY);
			pixel_shader->rgb_inputs[2] = PS_COMBINERINPUTS(
				a > 2 ? PS_REGISTER_T2 | PS_INPUTMAPPING_UNSIGNED_INVERT : PS_REGISTER_ONE,
				a > 3 ? PS_REGISTER_T3 : PS_REGISTER_ZERO,
				a > 2 ? PS_REGISTER_T2 : PS_REGISTER_ZERO,
				PS_REGISTER_ONE);
			pixel_shader->rgb_outputs[2] = PS_COMBINEROUTPUTS(
				PS_REGISTER_DISCARD, PS_REGISTER_DISCARD, PS_REGISTER_R0, PS_COMBINEROUTPUT_IDENTITY);
			pixel_shader->alpha_inputs[3] = PS_COMBINERINPUTS(
				PS_REGISTER_R0 | PS_CHANNEL_ALPHA, PS_REGISTER_R1 | PS_CHANNEL_ALPHA, PS_REGISTER_ZERO, PS_REGISTER_ZERO);
			pixel_shader->alpha_outputs[3] = PS_COMBINEROUTPUTS(
				PS_REGISTER_R0, PS_REGISTER_DISCARD, PS_REGISTER_DISCARD, PS_COMBINEROUTPUT_IDENTITY);
			pixel_shader->rgb_inputs[3] = PS_COMBINERINPUTS(
				a > 1 ? PS_REGISTER_T1 | PS_INPUTMAPPING_UNSIGNED_INVERT : PS_REGISTER_ONE,
				PS_REGISTER_R0,
				a > 1 ? PS_REGISTER_T1 : PS_REGISTER_ZERO,
				PS_REGISTER_ONE);
			pixel_shader->rgb_outputs[3] = PS_COMBINEROUTPUTS(
				PS_REGISTER_DISCARD, PS_REGISTER_DISCARD, PS_REGISTER_R0, PS_COMBINEROUTPUT_IDENTITY);
			pixel_shader->final_combiner_constant_0 = COLOR_D;
			pixel_shader->final_combiner_inputs_abcd = PS_COMBINERINPUTS(
				PS_REGISTER_T0, PS_REGISTER_C0, PS_REGISTER_EF_PROD, PS_REGISTER_ZERO);
			pixel_shader->final_combiner_inputs_efg = PS_COMBINERINPUTS(
				PS_REGISTER_R0, PS_REGISTER_C0, PS_REGISTER_R0 | PS_CHANNEL_ALPHA, 0);
			site_end("environment_fog.c:1738 fog screen, %d layer%s, %s", a, a > 1 ? "s" : "",
				b == 0 ? "first" : b == 1 ? "over fog" : "alpha only");
		}
	}

	/* rasterizer_xbox_environment_fog.c:1878-1880, the fog screen's second quad */
	site_begin(1);
	site_blend(BLEND_INVDESTALPHA, BLEND_ONE, BLENDOP_ADD);
	pixel_shader->combiner_count = PS_COMBINERCOUNT(1, 0);
	site_end("environment_fog.c:1878 fog screen, empty shader");

	/* rasterizer_xbox_lights.c:318-322, sun glow: clear alpha */
	site_begin(0);
	site.write_color = 0;
	site.write_alpha = 1;
	pixel_shader->combiner_count = 1;
	pixel_shader->final_combiner_constant_0 = 0;
	pixel_shader->final_combiner_inputs_efg = 0x1100;
	site_end("lights.c:318 sun glow alpha clear");

	/* rasterizer_xbox_lights.c:408-414, sun glow: the sun into alpha */
	site_begin(0);
	site.write_color = 0;
	site.write_alpha = 1;
	pixel_shader->texture_modes = 1;
	pixel_shader->combiner_count = 1;
	pixel_shader->alpha_inputs[0] = 0x18200000;
	pixel_shader->alpha_outputs[0] = 0x200C0;
	pixel_shader->final_combiner_inputs_efg = 0x1C00;
	site_end("lights.c:408 sun glow alpha");

	/* rasterizer_xbox_lights.c:507-518, sun glow: the rays */
	site_begin(0);
	site_blend(BLEND_SRCALPHA, BLEND_ONE, BLENDOP_ADD);
	pixel_shader->texture_modes = 1;
	pixel_shader->combiner_count = 2;
	pixel_shader->constant_0[0] = 0xB0B080;
	pixel_shader->constant_1[0] = 0xFFFFFF;
	pixel_shader->alpha_inputs[0] = 0x48200000;
	pixel_shader->alpha_outputs[0] = 0xC0;
	pixel_shader->rgb_inputs[1] = 0x3C011C02;
	pixel_shader->rgb_outputs[1] = 0xC00;
	pixel_shader->final_combiner_inputs_abcd = 0xC080000;
	pixel_shader->final_combiner_inputs_efg = 0x1400;
	site_end("lights.c:507 sun glow rays");

	/* rasterizer_xbox_lights.c:914-922, sun glow: copy of the frame */
	site_begin(0);
	site.write_alpha = 1;
	pixel_shader->texture_modes = 0x21;
	pixel_shader->combiner_count = 1;
	pixel_shader->constant_0[0] = 0xC0000000;
	pixel_shader->alpha_inputs[0] = 0x19110000;
	pixel_shader->alpha_outputs[0] = 0x100C0;
	pixel_shader->final_combiner_inputs_abcd = 0x18;
	pixel_shader->final_combiner_inputs_efg = 0x1C00;
	site_end("lights.c:914 sun glow copy");

	/* rasterizer_xbox_lights.c:1073-1084, sun glow: convolution */
	site_begin(0);
	site_blend(BLEND_DESTALPHA, BLEND_ZERO, BLENDOP_ADD);
	pixel_shader->texture_modes = 0x8421;
	pixel_shader->combiner_count = 2;
	pixel_shader->constant_0[0] = 0xFF000000;
	pixel_shader->alpha_inputs[0] = 0x8A009A0;
	pixel_shader->alpha_outputs[0] = 0xC00;
	pixel_shader->rgb_inputs[0] = 0xAA00BA0;
	pixel_shader->rgb_outputs[0] = 0xC00;
	pixel_shader->rgb_inputs[1] = 0x1C110C11;
	pixel_shader->rgb_outputs[1] = 0xC00;
	pixel_shader->final_combiner_inputs_abcd = 0xC;
	site_end("lights.c:1073 sun glow convolution");

	/* rasterizer_xbox_models.c:1011-1108, models: a = detail function, b
	= detail mask, c = double diffuse intensity, d = true atmospheric fog,
	e = alpha tested */
	for (a = 0; a < 3; a++)
	{
		for (b = 0; b < 9; b++)
		{
			for (c = 0; c < 2; c++)
			{
				for (d = 0; d < 2; d++)
				{
					for (e = 0; e < 2; e++)
					{
						static const char *masks[9] =
						{
							"none", "reflection mask inverse", "reflection mask", "self illumination mask inverse",
							"self illumination mask", "change colour mask inverse", "change colour mask",
							"multipurpose alpha inverse", "multipurpose alpha"
						};

						if (e && (a != 0 || b != 0))
							continue;
						site_begin(1);
						site.depth_write = 1;
						site.alpha_test = e;
						site.alpha_reference = 0x7f;
						/* the base map (stage 0), the multipurpose map (2), the detail map (1), the cube map (3) */
						site.stage_rank[0] = 3;
						site.stage_rank[1] = 1;
						site.stage_rank[2] = 2;
						site_model(a, b, c, d);
						site_end("models.c:1011 detail %s, mask %s%s%s%s",
							a == 0 ? "double biased multiply" : a == 1 ? "multiply" : "double biased add", masks[b],
							c ? ", double diffuse" : "", d ? ", true atmospheric fog" : "", e ? ", alpha tested" : "");
					}
				}
			}
		}
	}

	/* rasterizer_xbox_motion_sensor.c:255-259, blips */
	site_begin(1);
	site_blend(BLEND_ONE, BLEND_ONE, BLENDOP_ADD);
	pixel_shader->texture_modes = 1;
	pixel_shader->combiner_count = 1;
	pixel_shader->final_combiner_inputs_abcd = 0x08040000;
	site_end("motion_sensor.c:255 blips");

	/* rasterizer_xbox_motion_sensor.c:466-475, the sweep */
	site_begin(1);
	site_blend(BLEND_ONE, BLEND_SRCALPHA, BLENDOP_ADD);
	pixel_shader->texture_modes = 1;
	pixel_shader->combiner_count = 1;
	pixel_shader->rgb_inputs[0] = 0x08040000;
	pixel_shader->rgb_outputs[0] = 0xC0;
	pixel_shader->alpha_inputs[0] = 0x18140000;
	pixel_shader->alpha_outputs[0] = 0xC0;
	pixel_shader->final_combiner_inputs_abcd = 0xC;
	pixel_shader->final_combiner_inputs_efg = 0x1C00;
	site_end("motion_sensor.c:466 sweep");

	/* rasterizer_xbox_motion_sensor.c:512-517, the sweep's mask */
	site_begin(1);
	site_blend(BLEND_ZERO, BLEND_SRCALPHA, BLENDOP_ADD);
	pixel_shader->texture_modes = 1;
	pixel_shader->combiner_count = 1;
	pixel_shader->final_combiner_inputs_abcd = 8;
	pixel_shader->final_combiner_inputs_efg = 0x1800;
	site_end("motion_sensor.c:512 sweep mask");

	/* rasterizer_xbox_motion_sensor.c:594-599, the sensor on screen: its
	render states are set by number at 557-562 (62 and 63 are the source
	and destination blends: one, one minus the source's alpha) */
	site_begin(1);
	site_blend(BLEND_ONE, BLEND_INVSRCALPHA, BLENDOP_ADD);
	pixel_shader->texture_modes = 1;
	pixel_shader->combiner_count = 1;
	pixel_shader->final_combiner_inputs_abcd = 8;
	pixel_shader->final_combiner_inputs_efg = 0x1800;
	site_end("motion_sensor.c:594 to screen");

	/* rasterizer_xbox_plasma_energy.c:318-339 */
	site_begin(0);
	site_blend(BLEND_SRCALPHA, BLEND_ONE, BLENDOP_ADD);
	pixel_shader->texture_modes = 0x42;
	pixel_shader->combiner_count = 0x104;
	pixel_shader->alpha_inputs[0] = 0x0820A920;
	pixel_shader->alpha_outputs[0] = 0xC00;
	pixel_shader->rgb_inputs[0] = 0x1920B820;
	pixel_shader->rgb_outputs[0] = 0xC00;
	pixel_shader->alpha_inputs[1] = 0x1C1C0C0C;
	pixel_shader->alpha_outputs[1] = 0x24C00;
	pixel_shader->rgb_inputs[1] = 0;
	pixel_shader->rgb_outputs[1] = 0;
	pixel_shader->alpha_inputs[2] = 0x5C5C;
	pixel_shader->alpha_outputs[2] = 0x4D00;
	pixel_shader->rgb_inputs[2] = 0;
	pixel_shader->rgb_outputs[2] = 0;
	pixel_shader->alpha_inputs[3] = 0x14150000;
	pixel_shader->alpha_outputs[3] = 0x40;
	pixel_shader->rgb_inputs[3] = 0x1C051DA0;
	pixel_shader->rgb_outputs[3] = 0xC00;
	pixel_shader->final_combiner_inputs_abcd = 0x0C0F0000;
	pixel_shader->final_combiner_inputs_efg = 0x1C1C1400;
	site_end("plasma_energy.c:318");

	/* rasterizer_xbox_screen_effect.c:765-829, video: a = pass, b = overbright mode */
	site_begin(0);
	pixel_shader->texture_modes = 0x00000001;
	pixel_shader->combiner_count = 1;
	pixel_shader->final_combiner_inputs_abcd = 0x00000008;
	site_end("screen_effect.c:765 video pass 0");
	for (b = 0; b < 3; b++)
	{
		static const unsigned long video_overbright_scales[3] = { 0x00, 0x10, 0x20 };

		site_begin(0);
		site_blend(BLEND_INVSRCALPHA, BLEND_ZERO, BLENDOP_ADD);
		pixel_shader->texture_modes = 0x00000421;
		pixel_shader->combiner_count = 4;
		pixel_shader->constant_0[0] = 0x50000000UL; /* video_noise_intensity */
		pixel_shader->alpha_inputs[0] = 0x3120111a;
		pixel_shader->alpha_outputs[0] = 0x00000c00;
		pixel_shader->rgb_inputs[0] = 0x3120110a;
		pixel_shader->rgb_outputs[0] = 0x00000c00;
		pixel_shader->rgb_inputs[1] = 0x0c091c19;
		pixel_shader->rgb_outputs[1] = 0x000000c4;
		pixel_shader->rgb_inputs[2] = 0x08080000;
		pixel_shader->rgb_outputs[2] = 0x000000d0;
		pixel_shader->rgb_inputs[3] = 0x0d0d0820;
		pixel_shader->rgb_outputs[3] = (video_overbright_scales[b] << 12) | 0x000000d8;
		pixel_shader->final_combiner_inputs_abcd = 0x2c0d0800;
		pixel_shader->final_combiner_inputs_efg = 0x00000400;
		site_end("screen_effect.c:765 video pass 1, overbright %d", b);
	}

	/* rasterizer_xbox_screen_effect.c:835-1001, convolution and filters:
	a = the five shader bodies, b = convolution mask, c = light
	enhancement, d = desaturation (off, on, additive) */
	for (a = 0; a < 4; a++)
	{
		for (b = 0; b < 2; b++)
		{
			for (c = 0; c < 2; c++)
			{
				for (d = 0; d < 3; d++)
				{
					int combiner_count = 0;
					unsigned long light_enhancement_input, desaturation_input;

					if ((a == 0 || a == 1) && b)
						continue;
					site_begin(0);
					if (a == 0)
					{
						/* no convolution, a single pass */
						pixel_shader->texture_modes = 0x00000001;
						pixel_shader->rgb_inputs[0] = 0x08200000;
						pixel_shader->rgb_outputs[0] = 0x000000c0;
						combiner_count = 1;
						pixel_shader->final_combiner_inputs_efg = 0x00000800;
						pixel_shader->final_combiner_inputs_abcd = 0x0000000c;
					}
					else if (a == 1)
					{
						/* no convolution, pass 0 of two (never the last pass) */
						if (c || d)
							continue;
						pixel_shader->texture_modes = 0x00000001;
						pixel_shader->rgb_inputs[0] = 0x08200000;
						pixel_shader->rgb_outputs[0] = 0x000000c0;
						combiner_count = 1;
						pixel_shader->final_combiner_inputs_efg = 0;
						pixel_shader->final_combiner_inputs_abcd = 0x0000000c;
					}
					else if (a == 2)
					{
						/* no convolution, pass 1 of two */
						pixel_shader->texture_modes = 0x00000021;
						pixel_shader->rgb_inputs[0] = b ? 0x09200000 : 0x08200000;
						pixel_shader->rgb_outputs[0] = 0x000000c0;
						combiner_count = 1;
						pixel_shader->final_combiner_inputs_efg = b ? 0x00000800 : 0;
						pixel_shader->final_combiner_inputs_abcd = 0x0000000c;
					}
					else if (b)
					{
						pixel_shader->texture_modes = 0x00008421;
						pixel_shader->rgb_inputs[0] = 0x89208a20;
						pixel_shader->rgb_outputs[0] = 0x00000c00;
						pixel_shader->constant_0[1] = 0x55000000UL; /* alpha 1/3 */
						pixel_shader->rgb_inputs[1] = 0xcc118b11;
						pixel_shader->rgb_outputs[1] = 0x00000c00;
						pixel_shader->rgb_inputs[2] = 0xcc20a020;
						pixel_shader->rgb_outputs[2] = 0x00000c00;
						pixel_shader->rgb_inputs[3] = 0x3809180c;
						pixel_shader->rgb_outputs[3] = 0x00000c00;
						combiner_count = 4;
						pixel_shader->final_combiner_inputs_efg = 0x00000800;
						pixel_shader->final_combiner_inputs_abcd = 0x0000000c;
					}
					else
					{
						pixel_shader->texture_modes = 0x00008421;
						pixel_shader->rgb_inputs[0] = 0x88208920;
						pixel_shader->rgb_outputs[0] = 0x00030c00;
						pixel_shader->rgb_inputs[1] = 0x8a208b20;
						pixel_shader->rgb_outputs[1] = 0x00030d00;
						pixel_shader->rgb_inputs[2] = 0xcc20cd20;
						pixel_shader->rgb_outputs[2] = 0x00030c00;
						pixel_shader->rgb_inputs[3] = 0xcc20a020;
						pixel_shader->rgb_outputs[3] = 0x00000c00;
						combiner_count = 4;
						pixel_shader->final_combiner_inputs_efg = 0;
						pixel_shader->final_combiner_inputs_abcd = 0x0000000c;
					}
					/* both filters "use the convolution mask" when there is one */
					light_enhancement_input = b ? 0x68 : 0x20;
					desaturation_input = b ? 0x68 : 0x20;
					pixel_shader->constant_0[combiner_count] = 0x90000000UL; /* light enhancement intensity */
					pixel_shader->constant_1[combiner_count] = 0x60000000UL; /* desaturation intensity */
					pixel_shader->alpha_inputs[combiner_count] =
						(light_enhancement_input << 16) | desaturation_input | 0x11001200;
					pixel_shader->alpha_outputs[combiner_count] = 0x000000cd;
					if (c)
					{
						pixel_shader->rgb_inputs[combiner_count] = 0x2c2c0000;
						pixel_shader->rgb_outputs[combiner_count] = 0x00000d00;
						combiner_count++;
						pixel_shader->rgb_inputs[combiner_count] = 0x0d0d0000;
						pixel_shader->rgb_outputs[combiner_count] = 0x00000d00;
						combiner_count++;
						pixel_shader->final_combiner_inputs_abcd = 0x3c0c2d00;
						pixel_shader->final_combiner_inputs_efg = (unsigned long)(b ? 8 : 0) << 8;
					}
					if (d)
					{
						if (c)
						{
							pixel_shader->constant_0[combiner_count] = 0x90000000UL;
							pixel_shader->rgb_inputs[combiner_count] = 0x3c0c1c2d;
							pixel_shader->rgb_outputs[combiner_count] = 0x00000c00;
							combiner_count++;
						}
						pixel_shader->constant_0[combiner_count] = 0x00555555UL; /* desaturation weights */
						pixel_shader->rgb_inputs[combiner_count] = 0x0c010000;
						pixel_shader->rgb_outputs[combiner_count] = 0x000020d0;
						combiner_count++;
						pixel_shader->final_combiner_constant_0 = COLOR_A & 0x00FFFFFF; /* desaturation tint */
						pixel_shader->final_combiner_inputs_abcd = d == 2 ? 0x1d0f000c : 0x1d0f0c00;
						pixel_shader->final_combiner_inputs_efg = ((unsigned long)((b ? 8 : 0) | 0x00010d00)) << 8;
					}
					pixel_shader->combiner_count = combiner_count | 0x00011000;
					site_end("screen_effect.c:835 %s%s%s%s",
						a == 0 ? "plain" : a == 1 ? "plain, first of two" : a == 2 ? "plain, second of two" : "convolution",
						b ? ", mask" : "", c ? ", light enhancement" : "",
						d == 1 ? ", desaturation" : d == 2 ? ", additive desaturation" : "");
				}
			}
		}
	}

	/* rasterizer_xbox_screen_effect.c:1436-1445, screen flashes: the seven types */
	for (a = 0; a < 7; a++)
	{
		static const char *types[7] = { "lighten", "darken", "max", "min", "invert", "tint", "none" };
		unsigned long rgb_input = 0, alpha_input = 0;

		site_begin(1);
		site.blend_color = COLOR_B; /* the flash colour */
		switch (a)
		{
		case 0:
			site_blend(BLEND_ONE, BLEND_INVSRCALPHA, BLENDOP_ADD);
			rgb_input = 0x01200000;
			alpha_input = 0x11200000;
			break;
		case 1:
			site_blend(BLEND_ONE, BLEND_ONE, BLENDOP_REVSUBTRACT);
			rgb_input = 0x01200000;
			alpha_input = 0x11200000;
			break;
		case 2:
			site_blend(BLEND_INVDESTCOLOR, BLEND_INVCONSTANTCOLOR, BLENDOP_MAX);
			rgb_input = 0x01201140;
			break;
		case 3:
			site_blend(BLEND_INVDESTCOLOR, BLEND_INVCONSTANTCOLOR, BLENDOP_MIN);
			rgb_input = 0x01201120;
			break;
		case 4:
			site_blend(BLEND_INVDESTCOLOR, BLEND_INVCONSTANTCOLOR, BLENDOP_ADD);
			rgb_input = 0x01411120;
			break;
		case 5:
			site_blend(BLEND_ONE, BLEND_INVCONSTANTCOLOR, BLENDOP_ADD);
			rgb_input = 0x11200000;
			break;
		default:
			/* the switch's default arm: blending is on, the factors are whatever was set */
			site_blend(BLEND_ONE, BLEND_ZERO, BLENDOP_ADD);
			break;
		}
		pixel_shader->combiner_count = 1;
		pixel_shader->constant_0[0] = COLOR_B;
		pixel_shader->alpha_inputs[0] = alpha_input;
		pixel_shader->alpha_outputs[0] = 0x00000C00;
		pixel_shader->rgb_inputs[0] = rgb_input;
		pixel_shader->rgb_outputs[0] = 0x00000C00;
		pixel_shader->final_combiner_inputs_abcd = 0x0000000C;
		pixel_shader->final_combiner_inputs_efg = 0x00001C00;
		site_end("screen_effect.c:1436 flash %s", types[a]);
	}

	/* rasterizer_xbox_shadows.c:353-358, the shadow's silhouette */
	site_begin(0);
	site.alpha_test = 1;
	site.alpha_reference = 0x7F;
	pixel_shader->texture_modes = 1;
	pixel_shader->combiner_count = 1;
	pixel_shader->final_combiner_inputs_abcd = 0x00000020;
	pixel_shader->final_combiner_inputs_efg = 0x00001800;
	site_end("shadows.c:353 silhouette");

	/* rasterizer_xbox_shadows.c:822-845, the shadow on the environment */
	for (a = 0; a < 2; a++)
	{
		site_begin(0);
		site_blend(BLEND_ZERO, BLEND_INVSRCCOLOR, BLENDOP_ADD);
		site.write_alpha = 1;
		site.alpha_test = !a;
		site.alpha_reference = 0;
		pixel_shader->texture_modes = 0x00000021;
		pixel_shader->combiner_count = 4;
		pixel_shader->constant_0[0] = COLOR_C & 0x00FFFFFF; /* shadow colour */
		pixel_shader->constant_1[0] = 0x00FFFFFF;
		pixel_shader->rgb_inputs[0] = 0x14200000;
		pixel_shader->rgb_outputs[0] = 0x000000C0;
		pixel_shader->rgb_inputs[1] = 0x290C0821;
		pixel_shader->rgb_outputs[1] = 0x000000CD;
		pixel_shader->rgb_inputs[2] = 0x2C200C2D;
		pixel_shader->rgb_outputs[2] = 0x00000C00;
		pixel_shader->rgb_inputs[3] = 0x2C020000;
		pixel_shader->rgb_outputs[3] = 0x000020D0;
		pixel_shader->final_combiner_inputs_abcd = a ? 0x0000000C : 0x0000002C;
		pixel_shader->final_combiner_inputs_efg = 0x00000D00;
		site_end("shadows.c:822 projection%s", a ? ", debug" : "");
	}

	/* rasterizer_xbox_shadows.c:1056-1064, the shadow's blur */
	site_begin(0);
	pixel_shader->texture_modes = 0x00008421;
	pixel_shader->combiner_count = 1;
	pixel_shader->alpha_inputs[0] = 0x08A009A0;
	pixel_shader->alpha_outputs[0] = 0x00030C00;
	pixel_shader->rgb_inputs[0] = 0x0AA00BA0;
	pixel_shader->rgb_outputs[0] = 0x00030C00;
	pixel_shader->final_combiner_inputs_abcd = 0x0C20001C;
	site_end("shadows.c:1056 blur");

	/* rasterizer_xbox_text.c:362-408: a = maps, b = the draw's blend function */
	for (a = 1; a <= 3; a++)
	{
		for (b = 0; b < NUMBER_OF_BLEND_FUNCTIONS; b++)
		{
			site_begin(1);
			site_blend_function(b);
			pixel_shader->texture_modes = ((((a >= 3 ? 1UL : 0UL) << 5) | (a >= 2 ? 1 : 0)) << 5) | 1;
			pixel_shader->constant_0[0] = COLOR_A;
			pixel_shader->constant_1[0] = COLOR_B;
			pixel_shader->constant_0[1] = COLOR_C;
			pixel_shader->constant_0[4] = COLOR_D;
			pixel_shader->constant_0[5] = COLOR_D;
			pixel_shader->constant_0[6] = COLOR_D;
			pixel_shader->constant_0[7] = COLOR_D;
			pixel_shader->rgb_outputs[0] = 0x89;
			pixel_shader->alpha_outputs[0] = 0x89;
			pixel_shader->rgb_inputs[0] = 0x08010902;
			pixel_shader->alpha_inputs[0] = 0x18111912;
			pixel_shader->rgb_inputs[1] = 0x0A010804;
			pixel_shader->rgb_outputs[1] = 0xAC;
			pixel_shader->alpha_inputs[1] = 0x1A111814;
			pixel_shader->alpha_outputs[1] = 0xAC;
			pixel_shader->combiner_count = 0x11102;
			pixel_shader->final_combiner_inputs_abcd = 12;
			pixel_shader->final_combiner_inputs_efg = 0x1C00;
			site_end("text.c:362 %d map%s, %s", a, a > 1 ? "s" : "", blend_function_names[b]);
		}
	}

	/* rasterizer_xbox_transparent_geometry.c:1155-1211, group colours (debugging) */
	site_begin(0);
	site_blend(BLEND_ONE, BLEND_ONE, BLENDOP_ADD);
	pixel_shader->final_combiner_constant_0 = 0x00302840;
	pixel_shader->combiner_count = 1;
	pixel_shader->final_combiner_inputs_abcd = 1;
	site_end("transparent_geometry.c:1155 group colours");

	/* rasterizer_xbox_transparent_geometry.c:1317-1319, depth only */
	site_begin(1);
	site.write_color = 0;
	site.depth_write = 1;
	pixel_shader->combiner_count = 1;
	site_end("transparent_geometry.c:1317 depth only");

	/* rasterizer_xbox_transparent_geometry.c:1747-1869, effects (particles,
	contrails): a = nonlinear tint, b = secondary map, c = z-sprite, d =
	fog stage, e = framebuffer blend function */
	for (a = 0; a < 2; a++)
	{
		for (b = 0; b < 2; b++)
		{
			for (c = 0; c < (b ? 2 : 1); c++)
			{
				for (d = 0; d < 2; d++)
				{
					for (e = 0; e < NUMBER_OF_BLEND_FUNCTIONS; e++)
					{
						int combiner_index;

						if (c && (d || e != _blend_function_alpha_blend))
							/* the z-sprite modes with one framebuffer function: the light
							pipeline turns z-sprites off */
							continue;
						site_begin(!c);
						site_blend_function(e);
						pixel_shader->alpha_inputs[0] = 0x18201415;
						pixel_shader->alpha_outputs[0] = 0x000000C4;
						pixel_shader->final_combiner_inputs_abcd = 0x0000000C;
						pixel_shader->final_combiner_inputs_efg = 0x00001C00;
						if (a)
						{
							pixel_shader->rgb_inputs[0] = 0x08080000;
							pixel_shader->rgb_outputs[0] = 0x000000C0;
							pixel_shader->rgb_inputs[1] = 0x0C0C0000;
							pixel_shader->rgb_outputs[1] = 0x000000C0;
							pixel_shader->rgb_inputs[2] = 0x250C0508;
							pixel_shader->rgb_outputs[2] = 0x00000C00;
							combiner_index = 3;
						}
						else
						{
							pixel_shader->rgb_inputs[0] = 0x08050000;
							pixel_shader->rgb_outputs[0] = 0x000000C0;
							combiner_index = 1;
						}
						if (b)
						{
							pixel_shader->alpha_inputs[combiner_index] = 0x1C190000;
							pixel_shader->alpha_outputs[combiner_index] = 0x000000C0;
							pixel_shader->rgb_inputs[combiner_index] = 0x0C090000;
							pixel_shader->rgb_outputs[combiner_index] = 0x000000C0;
							combiner_index++;
						}
						if (c)
						{
							pixel_shader->texture_modes = 0x00054421;
							pixel_shader->input_texture = 0x00110000;
							pixel_shader->dot_mapping = 0;
						}
						else
						{
							pixel_shader->texture_modes = PS_TEXTUREMODES(
								PS_TEXTUREMODES_PROJECT2D, b ? PS_TEXTUREMODES_PROJECT2D : PS_TEXTUREMODES_NONE,
								PS_TEXTUREMODES_NONE, PS_TEXTUREMODES_NONE);
						}
						if (d)
						{
							switch (e)
							{
							case _blend_function_alpha_blend:
								pixel_shader->alpha_inputs[combiner_index] = 0x1C140000;
								pixel_shader->alpha_outputs[combiner_index] = 0x00000C00;
								break;
							case _blend_function_multiply:
							case _blend_function_min:
								pixel_shader->rgb_inputs[combiner_index] = 0x0C142034;
								pixel_shader->rgb_outputs[combiner_index] = 0x00000C00;
								break;
							case _blend_function_double_multiply:
								pixel_shader->rgb_inputs[combiner_index] = 0x0C14A034;
								pixel_shader->rgb_outputs[combiner_index] = 0x00000C00;
								break;
							case _blend_function_add:
							case _blend_function_reverse_subtract:
							case _blend_function_max:
								pixel_shader->rgb_inputs[combiner_index] = 0x0C140000;
								pixel_shader->rgb_outputs[combiner_index] = 0x00000C00;
								break;
							default:
								pixel_shader->alpha_inputs[combiner_index] = 0x1C140000;
								pixel_shader->alpha_outputs[combiner_index] = 0x00000C00;
								pixel_shader->rgb_inputs[combiner_index] = 0x0C140000;
								pixel_shader->rgb_outputs[combiner_index] = 0x00000C00;
								break;
							}
							combiner_index++;
						}
						pixel_shader->combiner_count = combiner_index;
						site_end("transparent_geometry.c:1747 effect%s%s%s%s, %s", a ? ", nonlinear tint" : "",
							b ? ", secondary map" : "", c ? ", z-sprite" : "", d ? ", fog" : "", blend_function_names[e]);
					}
				}
			}
		}
	}

	/* rasterizer_xbox_transparent_geometry.c:2117-2264, generic shaders
	(shader_transparent_generic_preprocessor.c): the shader without
	stages, with each fog stage; then sampled ones */
	for (a = 1; a <= 2; a++)
	{
		for (e = 0; e < NUMBER_OF_BLEND_FUNCTIONS; e++)
		{
			for (d = 0; d < NUMBER_OF_FADE_MODES + 2; d++)
			{
				/* d: the fade modes, then the sky's planar fog, then no fog stage */
				if (d == NUMBER_OF_FADE_MODES && e != _blend_function_alpha_blend)
					continue;
				site_begin(1);
				site_blend_function(e);
				site_generic(a, 0, NULL, 0);
				if (d <= NUMBER_OF_FADE_MODES)
					site_transparent_fog_stage(pixel_shader, 1, e, d < NUMBER_OF_FADE_MODES ? d : 0, d == NUMBER_OF_FADE_MODES);
				site_end("transparent_geometry.c:2117 generic, %d map%s, no stages, %s, %s", a, a > 1 ? "s" : "",
					d == 0 ? "fade none" : d == 1 ? "fade perpendicular" : d == 2 ? "fade parallel" : d == 3 ? "sky fog" : "no fog",
					blend_function_names[e]);
			}
		}
	}
	for (a = 0; a < 240; a++)
	{
		struct generic_stage stages[3];
		int map_count = 1 + (int)site_random(a < 120 ? 2 : 4);
		int stage_count = 1 + (a % 3);
		int blend_function = (int)site_random(NUMBER_OF_BLEND_FUNCTIONS);
		int fade_mode = (int)site_random(NUMBER_OF_FADE_MODES);
		int alpha_tested = site_random(6) == 0;

		for (b = 0; b < stage_count; b++)
			generic_stage_random(&stages[b], map_count, b == stage_count - 1);
		site_begin(1);
		site_blend_function(blend_function);
		site.alpha_test = alpha_tested;
		site.alpha_reference = 0x7F;
		site_generic(map_count, 0, stages, stage_count);
		site_transparent_fog_stage(pixel_shader, stage_count > 1 ? stage_count : 1, blend_function, fade_mode, 0);
		site_end("transparent_geometry.c:2117 generic sample %d: %d map%s, %d stage%s, %s%s", a, map_count,
			map_count > 1 ? "s" : "", stage_count, stage_count > 1 ? "s" : "", blend_function_names[blend_function],
			alpha_tested ? ", alpha tested" : "");
	}

	/* rasterizer_xbox_transparent_geometry.c:2640-2787, chicago shaders
	(shader_transparent_chicago_preprocessor.c). One map: every blend
	function and fog stage. Two maps: every colour function, alpha
	function and alpha replicate, alpha blended; and every colour
	function with every blend function. Three and four maps: sampled. */
	for (e = 0; e < NUMBER_OF_BLEND_FUNCTIONS; e++)
	{
		for (d = 0; d < NUMBER_OF_FADE_MODES + 2; d++)
		{
			for (c = 0; c < 2; c++)
			{
				int none[1] = { 0 };

				if (d == NUMBER_OF_FADE_MODES && e != _blend_function_alpha_blend)
					continue;
				if (c && (d != 0 || e > _blend_function_multiply))
					continue;
				site_begin(1);
				site_blend_function(e);
				site.alpha_test = c;
				site.alpha_reference = 0x7F;
				site_chicago(1, 0, none, none, none);
				if (d <= NUMBER_OF_FADE_MODES)
					site_transparent_fog_stage(pixel_shader, 1, e, d < NUMBER_OF_FADE_MODES ? d : 0, d == NUMBER_OF_FADE_MODES);
				site_end("transparent_geometry.c:2640 chicago, 1 map, %s, %s%s",
					d == 0 ? "fade none" : d == 1 ? "fade perpendicular" : d == 2 ? "fade parallel" : d == 3 ? "sky fog" : "no fog",
					blend_function_names[e], c ? ", alpha tested" : "");
			}
		}
	}
	for (a = 0; a < NUMBER_OF_SHADER_FUNCTIONS; a++)
	{
		for (b = 0; b < NUMBER_OF_SHADER_FUNCTIONS; b++)
		{
			for (c = 0; c < 2; c++)
			{
				int color_functions[2], alpha_functions[2], alpha_replicate[2];

				color_functions[0] = a;
				alpha_functions[0] = b;
				alpha_replicate[0] = c;
				color_functions[1] = alpha_functions[1] = alpha_replicate[1] = 0;
				site_begin(1);
				site_blend_function(_blend_function_alpha_blend);
				site_chicago(2, 0, color_functions, alpha_functions, alpha_replicate);
				site_transparent_fog_stage(pixel_shader, 2, _blend_function_alpha_blend, _fade_mode_none, 0);
				site_end("transparent_geometry.c:2640 chicago, 2 maps, colour %d alpha %d%s, alpha", a, b,
					c ? " replicate" : "");
			}
		}
		for (e = 1; e < NUMBER_OF_BLEND_FUNCTIONS; e++)
		{
			int color_functions[2], alpha_functions[2], alpha_replicate[2];

			color_functions[0] = a;
			alpha_functions[0] = a;
			alpha_replicate[0] = 0;
			color_functions[1] = alpha_functions[1] = alpha_replicate[1] = 0;
			site_begin(1);
			site_blend_function(e);
			site_chicago(2, 0, color_functions, alpha_functions, alpha_replicate);
			site_transparent_fog_stage(pixel_shader, 2, e, _fade_mode_fade_when_perpendicular, 0);
			site_end("transparent_geometry.c:2640 chicago, 2 maps, colour %d alpha %d, %s", a, a, blend_function_names[e]);
		}
	}
	for (a = 0; a < 160; a++)
	{
		int color_functions[4], alpha_functions[4], alpha_replicate[4];
		int map_count = 3 + (a & 1);
		int blend_function = (int)site_random(NUMBER_OF_BLEND_FUNCTIONS);
		int fade_mode = (int)site_random(NUMBER_OF_FADE_MODES);

		for (b = 0; b < 4; b++)
		{
			color_functions[b] = (int)site_random(NUMBER_OF_SHADER_FUNCTIONS);
			alpha_functions[b] = (int)site_random(NUMBER_OF_SHADER_FUNCTIONS);
			alpha_replicate[b] = site_random(4) == 0;
		}
		site_begin(1);
		site_blend_function(blend_function);
		site_chicago(map_count, 0, color_functions, alpha_functions, alpha_replicate);
		site_transparent_fog_stage(pixel_shader, map_count, blend_function, fade_mode, 0);
		site_end("transparent_geometry.c:2640 chicago sample %d: %d maps, colour %d %d %d, alpha %d %d %d, %s", a, map_count,
			color_functions[0], color_functions[1], color_functions[2], alpha_functions[0], alpha_functions[1],
			alpha_functions[2], blend_function_names[blend_function]);
	}
	/* a chicago shader whose first map is a cube map */
	{
		int none[1] = { 0 };

		site_begin(1);
		site_blend_function(_blend_function_add);
		site_chicago(1, 1, none, none, none);
		site_transparent_fog_stage(pixel_shader, 1, _blend_function_add, _fade_mode_none, 0);
		site_end("transparent_geometry.c:2640 chicago, 1 cube map, add");
	}

	/* rasterizer_xbox_transparent_geometry.c:2893-2910, glass tint: a = active camouflage */
	for (a = 0; a < 2; a++)
	{
		site_begin(1);
		site_blend(BLEND_ZERO, BLEND_SRCCOLOR, BLENDOP_ADD);
		site.alpha_test = 1;
		site.alpha_reference = 0;
		pixel_shader->texture_modes = 1;
		pixel_shader->combiner_count = 1;
		pixel_shader->constant_0[0] = COLOR_A & 0x00FFFFFF; /* tint colour */
		pixel_shader->rgb_inputs[0] = 0x08010000;
		pixel_shader->rgb_outputs[0] = 0xC0;
		if (a)
		{
			pixel_shader->constant_1[0] = 0xb0000000UL; /* effect intensity */
			pixel_shader->alpha_inputs[0] = 0x14320000;
			pixel_shader->alpha_outputs[0] = 0x40;
		}
		pixel_shader->final_combiner_inputs_abcd = 0x140C2000;
		pixel_shader->final_combiner_inputs_efg = 0x1400;
		site_end("transparent_geometry.c:2893 glass tint%s", a ? ", active camouflage" : "");
	}

	/* rasterizer_xbox_transparent_geometry.c:3036-3113, glass reflection:
	a = reflection type, b = bump map with a flat type, c = active
	camouflage, d = bump map is specular mask */
	for (a = 0; a < 3; a++)
	{
		for (b = 0; b < (a == 0 ? 1 : 2); b++)
		{
			for (c = 0; c < 2; c++)
			{
				for (d = 0; d < 2; d++)
				{
					site_begin(0);
					site_blend(BLEND_SRCALPHA, BLEND_ONE, BLENDOP_ADD);
					if (a == 0)
					{
						pixel_shader->texture_modes = 0x62E21;
						pixel_shader->input_texture = 0;
						pixel_shader->dot_mapping = 0x111;
					}
					else
					{
						pixel_shader->texture_modes = a == 1 ? 0x18C61 : 0x8C61;
					}
					pixel_shader->combiner_count = 0x11005;
					if (b)
					{
						pixel_shader->rgb_inputs[0] = 0x49480B0B;
					}
					else
					{
						pixel_shader->constant_0[0] = 0x00a04060; /* view direction */
						pixel_shader->rgb_inputs[0] = 0x4A410B0B;
					}
					pixel_shader->rgb_outputs[0] = 0x20CD;
					pixel_shader->rgb_inputs[1] = 0x0C0C0D0D;
					pixel_shader->rgb_outputs[1] = 0xCD;
					if (c)
					{
						pixel_shader->constant_1[1] = 0xb0000000UL;
						pixel_shader->alpha_inputs[1] = 0x14320000;
						pixel_shader->alpha_outputs[1] = 0x40;
					}
					pixel_shader->rgb_inputs[2] = 0x0C0C0D0D;
					pixel_shader->rgb_outputs[2] = 0xD;
					pixel_shader->constant_0[3] = COLOR_A;
					pixel_shader->constant_1[3] = COLOR_B;
					pixel_shader->alpha_outputs[3] = 0xC00;
					pixel_shader->rgb_outputs[3] = 0xC00;
					pixel_shader->rgb_outputs[4] = 0xC00;
					pixel_shader->alpha_inputs[3] = 0x2C120C11;
					pixel_shader->rgb_inputs[3] = 0x2C020C01;
					pixel_shader->rgb_inputs[4] = 0x2C0D0C0B;
					pixel_shader->final_combiner_inputs_abcd = 0x0C0F0000;
					pixel_shader->final_combiner_inputs_efg = ((unsigned long)(d ? 0x08 : 0x14) << 16) | 0x1C002000;
					site_end("transparent_geometry.c:3036 glass reflection %s%s%s%s",
						a == 0 ? "bumped cube map" : a == 1 ? "flat cube map" : "dynamic mirror", b ? ", bump map" : "",
						c ? ", active camouflage" : "", d ? ", bump is specular mask" : "");
				}
			}
		}
	}

	/* rasterizer_xbox_transparent_geometry.c:3217-3232, glass diffuse */
	site_begin(1);
	site_blend(BLEND_SRCALPHA, BLEND_INVSRCALPHA, BLENDOP_ADD);
	site.alpha_test = 1;
	site.alpha_reference = 0;
	pixel_shader->texture_modes = 0x421;
	pixel_shader->combiner_count = 3;
	pixel_shader->alpha_inputs[0] = 0x18190000;
	pixel_shader->alpha_outputs[0] = 0xC0;
	pixel_shader->rgb_inputs[0] = 0x08090000;
	pixel_shader->rgb_outputs[0] = 0x100C0;
	pixel_shader->alpha_inputs[1] = 0x1C140000;
	pixel_shader->alpha_outputs[1] = 0xC0;
	pixel_shader->rgb_inputs[1] = 0x0A200420;
	pixel_shader->rgb_outputs[1] = 0xD00;
	pixel_shader->rgb_inputs[2] = 0x0C0D0000;
	pixel_shader->rgb_outputs[2] = 0xC0;
	pixel_shader->final_combiner_inputs_abcd = 0xC;
	pixel_shader->final_combiner_inputs_efg = 0x1C00;
	site_end("transparent_geometry.c:3217 glass diffuse");

	/* rasterizer_xbox_transparent_geometry.c:3429-3475, meter shaders: a =
	tint mode 2, b = negative flash; then the two debugging shaders */
	for (a = 0; a < 2; a++)
	{
		for (b = 0; b < 2; b++)
		{
			site_begin(1);
			site_blend(a ? BLEND_CONSTANTCOLOR : BLEND_CONSTANTALPHA, a ? BLEND_SRCALPHA : BLEND_CONSTANTCOLOR, BLENDOP_ADD);
			site.blend_color = COLOR_D; /* tint_pixel */
			site.alpha_kill = 1;
			pixel_shader->texture_modes = 1;
			pixel_shader->combiner_count = 0x11104;
			pixel_shader->constant_0[0] = COLOR_C; /* flash_pixel */
			pixel_shader->constant_1[0] = COLOR_B; /* gradient_max_pixel */
			pixel_shader->alpha_inputs[0] = 0x12081208;
			pixel_shader->alpha_outputs[0] = 0x20C00;
			pixel_shader->rgb_inputs[0] = 0x1120E820;
			pixel_shader->rgb_outputs[0] = 0x20C00;
			pixel_shader->constant_0[1] = COLOR_A; /* gradient_min_pixel */
			pixel_shader->constant_1[1] = COLOR_B;
			pixel_shader->alpha_inputs[1] = 0x6C200000;
			pixel_shader->alpha_outputs[1] = 0xC0;
			pixel_shader->rgb_inputs[1] = 0x3C011C02;
			pixel_shader->rgb_outputs[1] = 0xC00;
			pixel_shader->constant_0[2] = COLOR_A;
			pixel_shader->constant_1[2] = COLOR_C;
			pixel_shader->alpha_inputs[2] = 0x0820B120;
			pixel_shader->alpha_outputs[2] = 0xC00;
			pixel_shader->rgb_inputs[2] = ((b ? 0xE0 : 0) + 2) | 0x0C201C00;
			pixel_shader->rgb_outputs[2] = 0xC00;
			pixel_shader->constant_0[3] = COLOR_E; /* background_pixel */
			pixel_shader->constant_1[3] = COLOR_D;
			pixel_shader->alpha_inputs[3] = 0x12201120;
			pixel_shader->alpha_outputs[3] = 0x4C00;
			pixel_shader->rgb_inputs[3] = 0x0C200120;
			pixel_shader->rgb_outputs[3] = 0x4C00;
			pixel_shader->final_combiner_inputs_abcd = 0x0C180000;
			pixel_shader->final_combiner_inputs_efg = 0x1C00;
			site_end("transparent_geometry.c:3429 meter%s%s", a ? ", tint mode 2" : "", b ? ", negative flash" : "");
		}
	}
	for (a = 0; a < 2; a++)
	{
		site_begin(0);
		pixel_shader->texture_modes = 1;
		pixel_shader->combiner_count = 1;
		pixel_shader->final_combiner_inputs_abcd = (a ? 0x10 : 0) + 8;
		site_end("transparent_geometry.c:3463 meter debugging %d", a);
	}

	/* rasterizer_xbox_water.c:400-431, the ripple map */
	site_begin(0);
	pixel_shader->texture_modes = 0x8421;
	pixel_shader->combiner_count = 0x11004;
	pixel_shader->rgb_inputs[0] = 0x31481149;
	pixel_shader->rgb_outputs[0] = 0xC00;
	pixel_shader->rgb_inputs[1] = 0x314A114B;
	pixel_shader->rgb_outputs[1] = 0xD00;
	pixel_shader->rgb_inputs[2] = 0x31CC11CD;
	pixel_shader->rgb_outputs[2] = 0x30C00;
	pixel_shader->rgb_inputs[3] = 0xCC20A020;
	pixel_shader->rgb_outputs[3] = 0xC00;
	pixel_shader->final_combiner_inputs_abcd = 0x310C0100;
	pixel_shader->final_combiner_inputs_efg = 0;
	pixel_shader->constant_0[0] = 0x80000000UL;
	pixel_shader->constant_0[1] = 0x60000000UL;
	pixel_shader->constant_0[2] = 0x90000000UL;
	site_end("water.c:400 ripples");

	/* rasterizer_xbox_water.c:685-687, depth only */
	site_begin(1);
	site.depth_write = 1;
	site.write_color = 0;
	pixel_shader->combiner_count = 1;
	site_end("water.c:685 depth only");

	/* rasterizer_xbox_water.c:799-812, opacity into alpha */
	site_begin(1);
	site.write_color = 0;
	site.write_alpha = 1;
	site.depth_write = 1;
	pixel_shader->texture_modes = 0x61;
	pixel_shader->combiner_count = 2;
	pixel_shader->constant_0[0] = 0xc0000000UL;
	pixel_shader->constant_1[0] = 0x40000000UL;
	pixel_shader->alpha_inputs[0] = 0x29120911;
	pixel_shader->alpha_outputs[0] = 0xC00;
	pixel_shader->alpha_inputs[1] = 0x1C180000;
	pixel_shader->alpha_outputs[1] = 0xC0;
	pixel_shader->final_combiner_inputs_abcd = 0;
	pixel_shader->final_combiner_inputs_efg = 0x1C00;
	site_end("water.c:799 opacity");

	/* rasterizer_xbox_water.c:870-876, the base map: a = atmospheric fog */
	for (a = 0; a < 2; a++)
	{
		site_begin(1);
		site_blend(BLEND_ZERO, BLEND_SRCCOLOR, BLENDOP_ADD);
		pixel_shader->texture_modes = 1;
		pixel_shader->combiner_count = 1;
		pixel_shader->final_combiner_inputs_abcd = ((unsigned long)(a ? 0x13 : 0) << 24) | 0x200800;
		site_end("water.c:870 base%s", a ? ", atmospheric fog" : "");
	}

	/* rasterizer_xbox_water.c:995-1042, the reflection: a = atmospheric fog,
	b = the base map's alpha (in the framebuffer) scales it or not (1041-1045) */
	for (a = 0; a < 2; a++)
	for (b = 0; b < 2; b++)
	{
		site_begin(0);
		site_blend(b ? BLEND_ONE : BLEND_DESTALPHA, BLEND_ONE, BLENDOP_ADD);
		pixel_shader->texture_modes = 0x64621;
		pixel_shader->dot_mapping = 0x111;
		if (a)
		{
			pixel_shader->combiner_count = 4;
			pixel_shader->rgb_inputs[0] = 0x0B0B0120;
			pixel_shader->rgb_outputs[0] = 0xCD;
			pixel_shader->rgb_inputs[1] = 0x0C0C0000;
			pixel_shader->rgb_outputs[1] = 0xC0;
			pixel_shader->rgb_inputs[2] = 0x0C0C0000;
			pixel_shader->rgb_outputs[2] = 0xC0;
			pixel_shader->rgb_inputs[3] = 0x2D0C0D0B;
			pixel_shader->rgb_outputs[3] = 0xC00;
			pixel_shader->final_combiner_inputs_abcd = 0x330C0000;
		}
		else
		{
			pixel_shader->combiner_count = 2;
			pixel_shader->rgb_inputs[0] = 0x0B0B0120;
			pixel_shader->rgb_outputs[0] = 0xCD;
			pixel_shader->rgb_inputs[1] = 0x0C0C0000;
			pixel_shader->rgb_outputs[1] = 0xC0;
			pixel_shader->final_combiner_inputs_abcd = 0x2D0F0B00;
			pixel_shader->final_combiner_inputs_efg = 0x0C0C0000;
		}
		pixel_shader->constant_0[0] = COLOR_A & 0x00FFFFFF; /* tint colour */
		site_end("water.c:995 reflection%s%s", a ? ", atmospheric fog" : "", b ? ", not scaled" : "");
	}

	/* rasterizer_xbox_widgets.c:369-441, untextured widgets (the lens
	flares' occlusion test): not blended, and no colour is written unless
	lens_flare_occlusion_debug is on, which also writes depth: a = that */
	for (a = 0; a < 2; a++)
	{
		site_begin(1);
		site.write_color = a;
		site.depth_write = a;
		pixel_shader->combiner_count = PS_COMBINERCOUNT(1, 0);
		pixel_shader->final_combiner_inputs_abcd = PS_COMBINERINPUTS(
			PS_REGISTER_ZERO, PS_REGISTER_ZERO, PS_REGISTER_ZERO, PS_REGISTER_ONE);
		site_end("widgets.c:434 untextured%s", a ? ", debug" : "");
	}

	/* rasterizer_xbox_widgets.c:448-572, textured widgets (the lens
	flares): added by their alpha (457-470) */
	site_begin(1);
	site_blend(BLEND_SRCALPHA, BLEND_ONE, BLENDOP_ADD);
	pixel_shader->texture_modes = PS_TEXTUREMODES(
		PS_TEXTUREMODES_PROJECT2D, PS_TEXTUREMODES_NONE, PS_TEXTUREMODES_NONE, PS_TEXTUREMODES_NONE);
	pixel_shader->combiner_count = PS_COMBINERCOUNT(3, 0);
	pixel_shader->rgb_inputs[0] = PS_COMBINERINPUTS(PS_REGISTER_T0, PS_REGISTER_T0, PS_REGISTER_ZERO, PS_REGISTER_ZERO);
	pixel_shader->rgb_outputs[0] = PS_COMBINEROUTPUTS(
		PS_REGISTER_R0, PS_REGISTER_DISCARD, PS_REGISTER_DISCARD, PS_COMBINEROUTPUT_IDENTITY);
	pixel_shader->rgb_inputs[1] = PS_COMBINERINPUTS(PS_REGISTER_R0, PS_REGISTER_R0, PS_REGISTER_ZERO, PS_REGISTER_ZERO);
	pixel_shader->rgb_outputs[1] = PS_COMBINEROUTPUTS(
		PS_REGISTER_R1, PS_REGISTER_DISCARD, PS_REGISTER_DISCARD, PS_COMBINEROUTPUT_IDENTITY);
	pixel_shader->rgb_inputs[2] = PS_COMBINERINPUTS(
		PS_REGISTER_V0, PS_REGISTER_T0, PS_REGISTER_V0 | PS_INPUTMAPPING_UNSIGNED_INVERT,
		PS_REGISTER_V1 | PS_CHANNEL_ALPHA);
	pixel_shader->rgb_outputs[2] = PS_COMBINEROUTPUTS(
		PS_REGISTER_V0, PS_REGISTER_V1, PS_REGISTER_DISCARD, PS_COMBINEROUTPUT_IDENTITY);
	pixel_shader->final_combiner_inputs_abcd = PS_COMBINERINPUTS(
		PS_REGISTER_V1, PS_REGISTER_EF_PROD, PS_REGISTER_ZERO, PS_REGISTER_V0);
	pixel_shader->final_combiner_inputs_efg = PS_COMBINERINPUTS(
		PS_REGISTER_R0, PS_REGISTER_R1, PS_REGISTER_V0 | PS_CHANNEL_ALPHA, 0);
	site_end("widgets.c:525 textured");

	/* ---------- the light pipeline's own states (rasterizer_light_pipeline):
	what the game sets in place of the passes above when the renderer is
	the light one. The compiler has to plan these as their author meant
	them: site.expect says how. */

	/* rasterizer_xbox_environment.c:3690-3704, the environment's one pass,
	light * material colour * base map: a = the light (lightmap, vertex
	light, ambient constant), b = lightmap missing */
	for (a = 0; a < 3; a++)
	{
		for (b = 0; b < 2; b++)
		{
			unsigned long light_input = a == 0 ? 0x0A : a == 1 ? 0x04 : 0x02;

			site_begin(1);
			site.write_alpha = 1;
			site.depth_write = 1;
			site.expect = _site_expect_two_units;
			pixel_shader->texture_modes = (b ? 0 : 0x00000400) | 0x00000001;
			pixel_shader->combiner_count = 1;
			pixel_shader->alpha_inputs[0] = 0x1A180000;
			pixel_shader->alpha_outputs[0] = 0x000000C0;
			pixel_shader->final_combiner_inputs_abcd = (light_input << 24) | 0x000F0000;
			pixel_shader->final_combiner_inputs_efg = 0x08011C00;
			pixel_shader->final_combiner_constant_0 = a == 0 ? COLOR_A & 0x00FFFFFF : 0x00FFFFFF; /* material colour */
			pixel_shader->final_combiner_constant_1 = COLOR_B & 0x00FFFFFF; /* ambient colour */
			site_end("environment.c:3690 light pipeline, fused, %s%s", a == 0 ? "lightmap" : a == 1 ? "vertex light" : "ambient",
				b ? ", lightmap missing" : "");
		}
	}

	/* rasterizer_xbox_environment.c:3727-3736, alpha-tested shaders, first
	pass: light * material colour, alpha from texture 0; a, b as above */
	for (a = 0; a < 3; a++)
	{
		for (b = 0; b < 2; b++)
		{
			unsigned long light_input = a == 0 ? 0x0A : a == 1 ? 0x04 : 0x02;

			site_begin(1);
			site.write_alpha = 1;
			site.depth_write = 1;
			site.alpha_test = 1;
			site.alpha_reference = 0x7F;
			site.expect = _site_expect_two_units_derived;
			pixel_shader->texture_modes = (b ? 0 : 0x00000400) | 0x00000001;
			pixel_shader->combiner_count = 1;
			pixel_shader->final_combiner_inputs_abcd = (light_input << 24) | 0x00010000;
			pixel_shader->final_combiner_inputs_efg = 0x00001800;
			pixel_shader->final_combiner_constant_0 = a == 0 ? COLOR_A & 0x00FFFFFF : 0x00FFFFFF;
			pixel_shader->final_combiner_constant_1 = COLOR_B & 0x00FFFFFF;
			site_end("environment.c:3727 light pipeline, alpha tested, %s%s",
				a == 0 ? "lightmap" : a == 1 ? "vertex light" : "ambient", b ? ", lightmap missing" : "");
		}
	}

	/* rasterizer_xbox_environment.c:3754-3758 and 3906-3910, the base map
	after the alpha-tested pass and the detail map: a = multiply, double
	multiply */
	for (a = 0; a < 2; a++)
	{
		site_begin(1);
		site_blend(BLEND_DESTCOLOR, a ? BLEND_SRCCOLOR : BLEND_ZERO, BLENDOP_ADD);
		site.expect = _site_expect_one_unit;
		pixel_shader->texture_modes = 0x00000001;
		pixel_shader->combiner_count = 1;
		pixel_shader->final_combiner_inputs_abcd = 0x00000008;
		site_end("environment.c:3754 light pipeline, map %s", a ? "double multiplied" : "multiplied");
	}

	/* rasterizer_xbox_environment.c:3858-3867, self-illumination: a = the
	map's channel (red, green, blue) */
	for (a = 0; a < 3; a++)
	{
		site_begin(1);
		site_blend(BLEND_ONE, BLEND_ONE, BLENDOP_ADD);
		site.expect = _site_expect_two_units_derived;
		pixel_shader->texture_modes = 0x00000021;
		pixel_shader->combiner_count = 1;
		pixel_shader->constant_0[0] = 0x00FF0000UL >> (8 * a);
		pixel_shader->rgb_inputs[0] = 0x08010000;
		pixel_shader->rgb_outputs[0] = 0x000020C0;
		pixel_shader->final_combiner_inputs_abcd = 0x0C0F0000;
		pixel_shader->final_combiner_inputs_efg = 0x09010000;
		pixel_shader->final_combiner_constant_0 = COLOR_C & 0x00FFFFFF; /* the channel's colour */
		site_end("environment.c:3858 light pipeline, self illumination %s", a == 0 ? "red" : a == 1 ? "green" : "blue");
	}

	/* rasterizer_xbox_water.c:952-961, the tint in place of the water's
	reflection: a = atmospheric fog, b = sky water (not blended) */
	for (a = 0; a < 2; a++)
	{
		for (b = 0; b < 2; b++)
		{
			site_begin(1);
			if (!b)
				site_blend(BLEND_ONE, BLEND_ONE, BLENDOP_ADD);
			site.depth_write = 1;
			site.expect = _site_expect_one_unit;
			pixel_shader->combiner_count = 1;
			pixel_shader->final_combiner_constant_0 = COLOR_A & 0x00FFFFFF; /* tint colour */
			pixel_shader->final_combiner_inputs_abcd = a ? 0x33010000 : 0x00000001;
			site_end("water.c:952 light pipeline, tint%s%s", a ? ", atmospheric fog" : "", b ? ", sky" : "");
		}
	}

	/* rasterizer_xbox_active_camouflage.c:808-815, the frame times the tint */
	site_begin(1);
	site_blend(BLEND_DESTCOLOR, BLEND_ZERO, BLENDOP_ADD);
	site.expect = _site_expect_one_unit;
	pixel_shader->combiner_count = PS_COMBINERCOUNT(1, 0);
	pixel_shader->final_combiner_constant_0 = COLOR_D & 0x00FFFFFF; /* tint colour */
	pixel_shader->final_combiner_inputs_abcd = PS_COMBINERINPUTS(
		PS_REGISTER_ZERO, PS_REGISTER_ZERO, PS_REGISTER_ZERO, PS_REGISTER_C0);
	site_end("active_camouflage.c:808 light pipeline, tint");

	/* rasterizer_xbox_transparent_geometry.c:3244-3252, glass diffuse on
	two textures: (lightmap + vertex light) * diffuse map */
	site_begin(1);
	site_blend(BLEND_SRCALPHA, BLEND_INVSRCALPHA, BLENDOP_ADD);
	site.alpha_test = 1;
	site.alpha_reference = 0;
	site.expect = _site_expect_two_units_add;
	pixel_shader->texture_modes = 0x401;
	pixel_shader->combiner_count = 3;
	pixel_shader->alpha_inputs[0] = 0x18300000;
	pixel_shader->alpha_outputs[0] = 0xC0;
	pixel_shader->rgb_inputs[0] = 0x08200000;
	pixel_shader->rgb_outputs[0] = 0xC0;
	pixel_shader->alpha_inputs[1] = 0x1C140000;
	pixel_shader->alpha_outputs[1] = 0xC0;
	pixel_shader->rgb_inputs[1] = 0x0A200420;
	pixel_shader->rgb_outputs[1] = 0xD00;
	pixel_shader->rgb_inputs[2] = 0x0C0D0000;
	pixel_shader->rgb_outputs[2] = 0xC0;
	pixel_shader->final_combiner_inputs_abcd = 0xC;
	pixel_shader->final_combiner_inputs_efg = 0x1C00;
	site_end("transparent_geometry.c:3244 light pipeline, glass diffuse");
}
