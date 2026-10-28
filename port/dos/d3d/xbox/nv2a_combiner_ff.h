/*
NV2A_COMBINER_FF.H

The pixel side of the light renderer (nv2a_combiner_ff.c): what the Xbox
pixel state - texture modes, register combiners, final combiner, alpha
test - computes, turned into something OpenGL 1.1's fixed-function
fragment pipeline can draw.

Four parts:
- a reference evaluator, which computes the colour the state produces
  exactly as nv2a_psh.c's GLSL does;
- a compiler from the state to a plan for a given set of capabilities:
  one or more passes, each with its texture units, their environments, a
  primary colour for the vertex path to supply, a constant colour, an
  alpha test and a framebuffer blend;
- a software model of the fixed-function fragment pipeline, which runs a
  plan in C;
- a check that compares the two on sample fragments, which the compiler
  runs on every plan it makes.

Nothing here calls OpenGL or depends on the rest of the device: the
values of the enumerations are their own, and the renderer maps them to
GL's. Colours are red, green, blue, alpha floats.
*/

#ifndef __HALO_LINUX_NV2A_COMBINER_FF_H
#define __HALO_LINUX_NV2A_COMBINER_FF_H

/* ---------- limits */

enum
{
	NV2A_FF_STAGE_COUNT = 4,
	NV2A_FF_COMBINER_STATE_COUNT = 57, /* D3DRS_PS_MAX */
	NV2A_FF_MAXIMUM_UNITS = 4,
	NV2A_FF_MAXIMUM_PASSES = 4,
	NV2A_FF_MAXIMUM_BAKES = 8,
	NV2A_FF_MAXIMUM_EXPRESSIONS = 56,
	NV2A_FF_MAXIMUM_TERMS = 160,
	NV2A_FF_MAXIMUM_OPERATIONS = 24,
	NV2A_FF_TERM_ATOMS = 7,

	NV2A_FF_EXPRESSION_NONE = -1
};

/* ---------- the pixel state

The fields of struct nv2a_pixel_shader_key (xgpu.h) that the translation
reads, under the same names; combiner_state is indexed by D3DRS_PS*, and
the constants in it are ignored (they are in struct nv2a_ff_constants). */

struct nv2a_ff_state
{
	unsigned long combiner_state[NV2A_FF_COMBINER_STATE_COUNT];
	unsigned long texture_modes;
	/* _xgpu_sampler_*: 0 none, 1 2D, 2 3D, 3 cube */
	unsigned char sampler_type[NV2A_FF_STAGE_COUNT];
	unsigned char alpha_kill[NV2A_FF_STAGE_COUNT];
	unsigned char color_sign[NV2A_FF_STAGE_COUNT];
	/* D3DCMP_* function for the alpha test, or 0 when disabled */
	unsigned long alpha_test_function;
	unsigned char fog_enable;
	unsigned char fog_table_mode;
};

#ifdef __HALO_LINUX_XGPU_H
#define NV2A_FF_STATE_FROM_KEY(state, key) \
	do \
	{ \
		int nv2a_ff_index; \
		for (nv2a_ff_index = 0; nv2a_ff_index < NV2A_FF_COMBINER_STATE_COUNT; nv2a_ff_index++) \
			(state)->combiner_state[nv2a_ff_index] = (key)->combiner_state[nv2a_ff_index]; \
		(state)->texture_modes = (key)->texture_modes; \
		for (nv2a_ff_index = 0; nv2a_ff_index < NV2A_FF_STAGE_COUNT; nv2a_ff_index++) \
		{ \
			(state)->sampler_type[nv2a_ff_index] = (key)->sampler_type[nv2a_ff_index]; \
			(state)->alpha_kill[nv2a_ff_index] = (key)->alpha_kill[nv2a_ff_index]; \
			(state)->color_sign[nv2a_ff_index] = (key)->color_sign[nv2a_ff_index]; \
		} \
		(state)->alpha_test_function = (key)->alpha_test_function; \
		(state)->fog_enable = (key)->fog_enable; \
		(state)->fog_table_mode = (key)->fog_table_mode; \
	} while (0)
#endif

/* the values the GLSL takes from uniforms */
struct nv2a_ff_constants
{
	/* D3DRS_PSCONSTANT0_0..7 and 1_0..7 */
	float c0[8][4];
	float c1[8][4];
	float final_c0[4];
	float final_c1[4];
	float fog_color[4];
	/* D3DRS_FOGSTART, FOGEND, FOGDENSITY */
	float fog_parameters[3];
	/* D3DRS_ALPHAREF, 0 to 255 */
	float alpha_reference;
	/* D3DRS_BLENDCOLOR */
	float blend_color[4];
	/* D3DTSS_BUMPENVLSCALE and LOFFSET of each stage */
	float bump_luminance[NV2A_FF_STAGE_COUNT][2];
};

/* an Xbox colour (alpha in the top byte) as floats */
void nv2a_ff_color_from_argb(unsigned long argb, float color[4]);

/* one fragment's varying inputs */
struct nv2a_ff_fragment
{
	/* what each stage's sampler returned, before D3DTSS_COLORSIGN */
	float texel[NV2A_FF_STAGE_COUNT][4];
	/* the stage's texture coordinates, which the pass-through and clip
	plane modes read */
	float texcoord[NV2A_FF_STAGE_COUNT][4];
	/* diffuse and specular */
	float v0[4];
	float v1[4];
	/* the fog coordinate (the vertex program's oFog) */
	float fog;
};

/* ---------- the reference evaluator

The colour nv2a_psh.c's shader writes for this state and fragment. Returns
0 when the fragment is discarded (alpha kill, clip plane or alpha test). */

int nv2a_ff_reference(const struct nv2a_ff_state *state, const struct nv2a_ff_constants *constants,
	const struct nv2a_ff_fragment *fragment, float result[4]);

/* ---------- capabilities */

enum
{
	/* derived textures the plan may ask for */
	_nv2a_ff_bakes_none = 0,
	/* only ones that do not depend on the constants: made once */
	_nv2a_ff_bakes_static,
	/* any: remade when a constant they read changes */
	_nv2a_ff_bakes_all
};

struct nv2a_ff_capabilities
{
	/* texture units, 1 without ARB_multitexture */
	unsigned char units;
	/* EXT_texture_env_combine or ARB_texture_env_combine */
	unsigned char combine;
	/* ARB_texture_env_combine: SUBTRACT, and every operand for the third
	argument of INTERPOLATE */
	unsigned char combine_arb;
	/* EXT_texture_env_add, or the ADD of the combine extension */
	unsigned char environment_add;
	/* ARB_texture_env_dot3 */
	unsigned char dot3;
	/* EXT_blend_subtract, EXT_blend_minmax, EXT_blend_color */
	unsigned char blend_subtract;
	unsigned char blend_minmax;
	unsigned char blend_color;
	/* EXT_fog_coord: the vertex path can give OpenGL the fog factor */
	unsigned char fog_coordinate;
	/* EXT_secondary_color: a second vertex colour, added after the units */
	unsigned char secondary_color;
	/* ARB_texture_cube_map */
	unsigned char cube_maps;
	/* the framebuffer has alpha bits */
	unsigned char destination_alpha;
	/* _nv2a_ff_bakes_* */
	unsigned char bakes;
	/* most passes a plan may take, 0 for NV2A_FF_MAXIMUM_PASSES */
	unsigned char maximum_passes;
};

/* The three levels of the design: 0 is one unit with the OpenGL 1.1
texture environments, 1 is two units without combine, 2 is two units with
EXT_texture_env_combine. Everything optional is off, derived textures are
allowed; the caller turns on what the driver has. */
void nv2a_ff_capabilities_level(int level, struct nv2a_ff_capabilities *capabilities);

/* ---------- the draw's framebuffer state */

/* blend factors and operations: the values of D3DBLEND_* and D3DBLENDOP_*,
which are OpenGL's */
enum
{
	_nv2a_ff_blend_zero = 0,
	_nv2a_ff_blend_one = 1,
	_nv2a_ff_blend_source_color = 0x300,
	_nv2a_ff_blend_inverse_source_color = 0x301,
	_nv2a_ff_blend_source_alpha = 0x302,
	_nv2a_ff_blend_inverse_source_alpha = 0x303,
	_nv2a_ff_blend_destination_alpha = 0x304,
	_nv2a_ff_blend_inverse_destination_alpha = 0x305,
	_nv2a_ff_blend_destination_color = 0x306,
	_nv2a_ff_blend_inverse_destination_color = 0x307,
	_nv2a_ff_blend_source_alpha_saturate = 0x308,
	_nv2a_ff_blend_constant_color = 0x8001,
	_nv2a_ff_blend_inverse_constant_color = 0x8002,
	_nv2a_ff_blend_constant_alpha = 0x8003,
	_nv2a_ff_blend_inverse_constant_alpha = 0x8004,

	_nv2a_ff_blend_operation_add = 0x8006,
	_nv2a_ff_blend_operation_minimum = 0x8007,
	_nv2a_ff_blend_operation_maximum = 0x8008,
	_nv2a_ff_blend_operation_subtract = 0x800a,
	_nv2a_ff_blend_operation_reverse_subtract = 0x800b
};

/* comparison functions: the values of D3DCMP_*, which are OpenGL's */
enum
{
	_nv2a_ff_compare_never = 0x200,
	_nv2a_ff_compare_less,
	_nv2a_ff_compare_equal,
	_nv2a_ff_compare_less_or_equal,
	_nv2a_ff_compare_greater,
	_nv2a_ff_compare_not_equal,
	_nv2a_ff_compare_greater_or_equal,
	_nv2a_ff_compare_always
};

struct nv2a_ff_draw
{
	/* D3DRS_ALPHABLENDENABLE, SRCBLEND, DESTBLEND, BLENDOP */
	unsigned char blend_enable;
	unsigned long source_blend;
	unsigned long destination_blend;
	unsigned long blend_operation;
	/* D3DRS_COLORWRITEENABLE: any of red, green and blue; alpha */
	unsigned char write_color;
	unsigned char write_alpha;
	/* the draw tests and writes depth, so a later pass over the same
	triangles can test for equal depth */
	unsigned char depth_write;
	/* How much each stage's texture matters to the picture, 0 to 3: when
	textures have to be dropped, the ones that matter least go first. All
	zero: stage 0 matters most, stage 3 least. */
	unsigned char stage_rank[NV2A_FF_STAGE_COUNT];
};

/* ---------- the plan */

enum
{
	/* the plan computes what the state computes (within its tolerance) */
	_nv2a_ff_plan_exact = 0,
	/* something was left out or simplified: see approximations */
	_nv2a_ff_plan_approximate,
	/* nothing can be drawn */
	_nv2a_ff_plan_impossible
};

/* what an approximate plan gave up */
enum
{
	/* a texture stage was replaced by a constant (dropped_stage) */
	_nv2a_ff_approximate_dropped_texture = 1 << 0,
	/* the primary colour holds a term that is not a first-degree function
	of the vertex colours and fog, so interpolating it differs */
	_nv2a_ff_approximate_vertex_term = 1 << 1,
	/* a derived texture is not a first-degree function of its texel:
	right at texel centres, different between them */
	_nv2a_ff_approximate_bake_filter = 1 << 2,
	/* alpha kill became an alpha test, or was left out */
	_nv2a_ff_approximate_alpha_kill = 1 << 3,
	/* a clip plane stage was left out */
	_nv2a_ff_approximate_clip_plane = 1 << 4,
	/* the fog table is evaluated at the vertices */
	_nv2a_ff_approximate_fog_table = 1 << 5,
	/* the blend operation or factor was replaced by a similar one */
	_nv2a_ff_approximate_blend = 1 << 6,
	/* the comparison of plan and reference found a difference that the
	other flags do not explain */
	_nv2a_ff_approximate_unexplained = 1 << 7,
	/* destination alpha is not what the state writes */
	_nv2a_ff_approximate_destination_alpha = 1 << 8,
	/* a term of the colour or of alpha was left out */
	_nv2a_ff_approximate_dropped_term = 1 << 9,
	/* a clamp between two combiner stages was left out: right as long as
	the clamped value stays within its range */
	_nv2a_ff_approximate_clamp = 1 << 10
};

/* what a dropped stage's texel was replaced by */
enum
{
	_nv2a_ff_substitute_none = 0,
	_nv2a_ff_substitute_black,   /* 0, 0, 0, 0 */
	_nv2a_ff_substitute_white,   /* 1, 1, 1, 1 */
	_nv2a_ff_substitute_grey,    /* 0.5 everywhere */
	_nv2a_ff_substitute_flat,    /* a flat normal: 0.5, 0.5, 1, alpha 1 */
	NUMBER_OF_NV2A_FF_SUBSTITUTES
};

/* Texture environments. A plan takes a texel to be red, green, blue and
alpha whatever its texture is stored as (alpha one where the format has
none), which is what OpenGL's combine environments do. Its other
environments go by the texture's base format, and three of them then
compute something else: REPLACE with an RGB or LUMINANCE texture keeps the
alpha that came in, DECAL is undefined for LUMINANCE, LUMINANCE_ALPHA and
INTENSITY textures, and BLEND and ADD with an INTENSITY texture mix or add
the alphas instead of multiplying them. Where the capabilities have
combine environments the compiler asks for those in their place (BLEND
only with ARB_texture_env_combine). Elsewhere the renderer must keep the
stage texture of a DECAL, BLEND or ADD unit in a format with all four
channels (LUMINANCE_ALPHA will do for BLEND and ADD); REPLACE is then only
asked for at the first unit of a pass that reads no primary alpha, where
the alpha that comes in is one. */
enum
{
	_nv2a_ff_environment_replace = 0,
	_nv2a_ff_environment_modulate,
	_nv2a_ff_environment_decal,
	_nv2a_ff_environment_blend,
	_nv2a_ff_environment_add,
	_nv2a_ff_environment_combine
};

/* combine functions */
enum
{
	_nv2a_ff_combine_replace = 0,
	_nv2a_ff_combine_modulate,
	_nv2a_ff_combine_add,
	_nv2a_ff_combine_add_signed,
	_nv2a_ff_combine_interpolate,
	_nv2a_ff_combine_subtract,
	_nv2a_ff_combine_dot3
};

/* combine sources */
enum
{
	_nv2a_ff_source_texture = 0,
	_nv2a_ff_source_constant,
	_nv2a_ff_source_primary,
	_nv2a_ff_source_previous
};

/* combine operands */
enum
{
	_nv2a_ff_operand_color = 0,
	_nv2a_ff_operand_inverse_color,
	_nv2a_ff_operand_alpha,
	_nv2a_ff_operand_inverse_alpha
};

enum
{
	NV2A_FF_STAGE_NONE = 255,
	NV2A_FF_BAKE_NONE = 255
};

/* One side of a combine environment: function(argument 0, 1, 2) * scale. */
struct nv2a_ff_combine
{
	unsigned char function;
	unsigned char source[3];
	unsigned char operand[3];
	/* 1, 2 or 4 */
	unsigned char scale;
};

struct nv2a_ff_unit
{
	/* the NV2A stage whose texture and texture coordinates the unit
	takes, or NV2A_FF_STAGE_NONE: any texture will do (its colour is not
	read), but OpenGL wants one bound */
	unsigned char stage;
	/* the derived texture to bind in place of the stage's own, or
	NV2A_FF_BAKE_NONE */
	unsigned char bake;
	/* _nv2a_ff_environment_* */
	unsigned char environment;
	struct nv2a_ff_combine rgb;
	struct nv2a_ff_combine alpha;
	/* GL_TEXTURE_ENV_COLOR: expressions of the constants, or
	NV2A_FF_EXPRESSION_NONE when the environment does not read it */
	short constant_rgb;
	short constant_alpha;
};

/* A derived texture: a function of one stage's texel (and maybe of the
constants) to run over every texel of that stage's texture. */
struct nv2a_ff_bake
{
	unsigned char stage;
	/* reads the constants: remake it when they change */
	unsigned char uses_constants;
	/* not a first-degree function of the texel */
	unsigned char nonlinear;
	unsigned char pad;
	/* expressions of the texel; NV2A_FF_EXPRESSION_NONE keeps the
	texel's own colour or alpha */
	short rgb;
	short alpha;
};

struct nv2a_ff_pass
{
	unsigned char unit_count;
	struct nv2a_ff_unit units[NV2A_FF_MAXIMUM_UNITS];

	/* The primary colour: expressions of v0, v1, the fog factor and the
	constants that the vertex path evaluates at each vertex (or once,
	when primary_is_constant). NV2A_FF_EXPRESSION_NONE: not read. */
	short primary_rgb;
	short primary_alpha;
	unsigned char primary_is_constant;

	/* The secondary colour (needs secondary_color): an expression like
	the primary's, which OpenGL adds to the result of the units
	(GL_COLOR_SUM), or NV2A_FF_EXPRESSION_NONE. */
	short secondary_rgb;

	/* OpenGL fog (needs fog_coordinate): the fragment is
	factor * colour + (1 - factor) * fog colour. */
	unsigned char fog_enable;
	short fog_factor;
	short fog_color;

	/* the alpha test: 0 or a comparison; the reference is the draw's
	D3DRS_ALPHAREF unless alpha_reference names an expression of the
	constants (whose value is 0 to 1) */
	unsigned short alpha_test_function;
	short alpha_reference;

	unsigned char blend_enable;
	unsigned short source_blend;
	unsigned short destination_blend;
	unsigned short blend_operation;
	/* GL_BLEND_COLOR, when a factor reads it and it is not the draw's
	D3DRS_BLENDCOLOR */
	unsigned char write_color;
	unsigned char write_alpha;
	/* draw with depth function EQUAL and without depth writes */
	unsigned char depth_equal;
};

/* expressions: sums of products, see nv2a_ff_plan_evaluate */
struct nv2a_ff_term
{
	float coefficient[3];
	unsigned char count;
	unsigned char atoms[NV2A_FF_TERM_ATOMS];
};

struct nv2a_ff_expression
{
	unsigned short first_term;
	unsigned short term_count;
};

struct nv2a_ff_operation
{
	unsigned char operation;
	unsigned char pad;
	short operands[3];
};

struct nv2a_ff_plan
{
	/* _nv2a_ff_plan_* */
	unsigned char status;
	unsigned char pass_count;
	/* the plan is right for the caller's constants only: the general plan
	was not exact. Which of them it was made for is in bound_sources. */
	unsigned char constants_bound;
	/* made by an entry of the override table */
	unsigned char from_override;
	/* _nv2a_ff_approximate_* */
	unsigned long approximations;
	/* _nv2a_ff_substitute_* of each stage */
	unsigned char dropped_stage[NV2A_FF_STAGE_COUNT];
	/* The most the framebuffer may differ from the reference when the
	plan is exact, in steps of 1/255: what rounding alone can do. One step
	for a plan of one pass without derived textures (the plan and the
	reference may round the same value to different sides); more when
	there are derived textures (their texels are bytes) and passes (each
	rounds what it writes), by how the units and the blends scale and add
	up those errors. */
	unsigned char tolerance;
	unsigned char bake_count;
	/* what the compiler's own comparison measured, in steps of 1/255 */
	float mean_error;
	float maximum_error;
	/* nv2a_ff_state_hash of the state */
	unsigned long state_hash;
	/* With constants_bound: the constants whose values the plan was made
	for, bit n for atom source n (_nv2a_ff_atom_fog: the fog colour, _c0 +
	stage, _c1 + stage, _final_c0, _final_c1, _blend_color). The plan holds
	for any values of the others: see nv2a_ff_plan_constants_match. */
	unsigned long bound_sources;

	struct nv2a_ff_pass passes[NV2A_FF_MAXIMUM_PASSES];
	struct nv2a_ff_bake bakes[NV2A_FF_MAXIMUM_BAKES];

	unsigned short expression_count;
	unsigned short term_count;
	unsigned short operation_count;
	struct nv2a_ff_expression expressions[NV2A_FF_MAXIMUM_EXPRESSIONS];
	struct nv2a_ff_term terms[NV2A_FF_MAXIMUM_TERMS];
	struct nv2a_ff_operation operations[NV2A_FF_MAXIMUM_OPERATIONS];
};

/* ---------- the compiler

Fills *plan for the state under these capabilities and this draw state.
constants may be NULL. The compiler first makes a plan that holds for
every value of the constants; when that plan is not exact and constants
were given, it tries again with their values known and keeps the better
plan, marked constants_bound: first with the values of the constants that
decide what is computed (the ones inside clamps, dot products and
multiplexers: a mask that picks a channel, say), then, if that is not exact
either, with the values of all (which removes terms that are multiplied by
a zero, and so on). Returns plan->status.

The plan for all constants of the last call is kept: asking again for the
same state, capabilities and draw with constants costs only the searches
with their values.

Deterministic; uses static work areas (no allocation), so not reentrant.
The search is bounded by a count of the candidates it looks at (see
SEARCH_STEP_LIMIT in nv2a_combiner_ff.c). */

int nv2a_ff_compile(const struct nv2a_ff_state *state, const struct nv2a_ff_constants *constants,
	const struct nv2a_ff_capabilities *capabilities, const struct nv2a_ff_draw *draw,
	struct nv2a_ff_plan *plan);

/* what the last nv2a_ff_compile did: plans attempted (one for each set
of dropped textures tried), passes it tried to fit, and candidates looked
at while fitting them */
struct nv2a_ff_statistics
{
	unsigned long attempts;
	unsigned long fits;
	unsigned long steps;
};

void nv2a_ff_compile_statistics(struct nv2a_ff_statistics *statistics);

/* A plan with constants_bound is right for other constants too as long
as the ones it was made for have the same values: non-zero when a and b
agree in those (always, for a plan that is not bound). A renderer that
keeps plans by their constants looks them up with this instead of
comparing all the constants, or a colour that changes every frame without
mattering to the plan makes it compile the same plan again and again. */
int nv2a_ff_plan_constants_match(const struct nv2a_ff_plan *plan, const struct nv2a_ff_constants *a,
	const struct nv2a_ff_constants *b);

/* copies the constants the plan was made for from one set into another
(nothing, for a plan that is not bound) */
void nv2a_ff_plan_bound_constants(const struct nv2a_ff_plan *plan, const struct nv2a_ff_constants *from,
	struct nv2a_ff_constants *to);

/* a hash of everything in the state that the translation reads */
unsigned long nv2a_ff_state_hash(const struct nv2a_ff_state *state);

/* Hand-written plans. An override is tried before the general compiler for
the state with its hash; it fills the plan's passes through the
nv2a_ff_override_* functions and returns non-zero, or returns 0 to decline
(wrong capabilities, wrong blend). The compiler then checks the plan
against the reference like any other. */
typedef int (*nv2a_ff_override_function)(const struct nv2a_ff_state *state,
	const struct nv2a_ff_constants *constants, const struct nv2a_ff_capabilities *capabilities,
	const struct nv2a_ff_draw *draw, struct nv2a_ff_plan *plan);

int nv2a_ff_override_register(unsigned long state_hash, nv2a_ff_override_function function);

/* for overrides: one more pass, which draws with the draw's own blend,
write mask and alpha test and reads no expression yet; one more unit of
a pass, with the stage's own texture and no constant (NULL when full) */
struct nv2a_ff_pass *nv2a_ff_override_pass(struct nv2a_ff_plan *plan, const struct nv2a_ff_state *state,
	const struct nv2a_ff_draw *draw);
struct nv2a_ff_unit *nv2a_ff_override_unit(struct nv2a_ff_pass *pass, int stage, int environment);

/* for overrides: an expression that is the product of up to two of the
atoms below, times a factor, added to an earlier expression (or
NV2A_FF_EXPRESSION_NONE); returns its index or NV2A_FF_EXPRESSION_NONE
when the plan is full */
short nv2a_ff_override_expression(struct nv2a_ff_plan *plan, short sum_with, float factor,
	int atom0, int atom1);

/* The atoms of expressions: a source and a channel. NV2A_FF_ATOM(source,
channel); NV2A_FF_ATOM_ONE stands for no atom. */
enum
{
	_nv2a_ff_atom_texel = 0,      /* +stage: plain plans do not use these;
	                                  in a bake it is the texel being baked */
	_nv2a_ff_atom_v0 = 4,
	_nv2a_ff_atom_v1 = 5,
	_nv2a_ff_atom_fog = 6,        /* rgb: fog colour; alpha: fog factor */
	_nv2a_ff_atom_c0 = 7,         /* +combiner stage */
	_nv2a_ff_atom_c1 = 15,        /* +combiner stage */
	_nv2a_ff_atom_final_c0 = 23,
	_nv2a_ff_atom_final_c1 = 24,
	_nv2a_ff_atom_blend_color = 25,
	NUMBER_OF_NV2A_FF_ATOM_SOURCES = 26,

	_nv2a_ff_channel_rgb = 0,
	_nv2a_ff_channel_alpha = 1,
	_nv2a_ff_channel_blue = 2,

	NV2A_FF_ATOM_ONE = 255
};

#define NV2A_FF_ATOM(source, channel) ((source) * 3 + (channel))

/* An atom from NV2A_FF_LEAF_ATOM_COUNT on is an operation on expressions
(plan->operations[atom - NV2A_FF_LEAF_ATOM_COUNT]), which a renderer that
evaluates the expressions itself (xgpu_fixed.c) computes as
nv2a_ff_plan_evaluate does: */
#define NV2A_FF_LEAF_ATOM_COUNT (NUMBER_OF_NV2A_FF_ATOM_SOURCES * 3)

enum
{
	_nv2a_ff_operation_maximum_zero = 0,  /* max(a, 0) */
	_nv2a_ff_operation_clamp_unit,        /* clamp(a, 0, 1) */
	_nv2a_ff_operation_clamp_signed,      /* clamp(a, -1, 1) */
	_nv2a_ff_operation_dot,               /* dot(a, b) in every channel */
	_nv2a_ff_operation_mux_msb,           /* a >= 0.5 ? b : c */
	_nv2a_ff_operation_mux_lsb,           /* the low bit of a's byte ? b : c */
	_nv2a_ff_operation_signed_byte,       /* the GLSL's signed_byte(): the nearest byte, read as signed, over 127 */
	_nv2a_ff_operation_identity           /* a, kept apart to stop a product growing */
};

/* ---------- evaluating a plan's expressions (the renderer and the model) */

struct nv2a_ff_values
{
	const struct nv2a_ff_constants *constants;
	/* the vertex's colours and fog factor (after the fog table and the
	clamp to 0..1), for primary colours and fog factors */
	const float *v0;
	const float *v1;
	float fog_factor;
	/* the texels of the stages, for bakes (only the bake's stage is read) */
	const float (*texel)[4];
};

/* the value of expression (three channels; an alpha expression has the
same value in each) */
void nv2a_ff_plan_evaluate(const struct nv2a_ff_plan *plan, short expression,
	const struct nv2a_ff_values *values, float result[3]);

/* a pass's primary colour at a vertex; channels the pass does not read
are 1 */
void nv2a_ff_plan_primary(const struct nv2a_ff_plan *plan, int pass,
	const struct nv2a_ff_values *values, float color[4]);

/* a pass's secondary colour at a vertex (zero when it has none) */
void nv2a_ff_plan_secondary(const struct nv2a_ff_plan *plan, int pass,
	const struct nv2a_ff_values *values, float color[3]);

/* a unit's GL_TEXTURE_ENV_COLOR */
void nv2a_ff_plan_constant(const struct nv2a_ff_plan *plan, int pass, int unit,
	const struct nv2a_ff_constants *constants, float color[4]);

/* a derived texture's texel from the stage's texel */
void nv2a_ff_plan_bake(const struct nv2a_ff_plan *plan, int bake,
	const struct nv2a_ff_constants *constants, const float texel[4], float baked[4]);

/* ---------- the software model of the fixed-function pipeline

Arithmetic in floats; every unit's result is clamped to 0..1, as OpenGL
clamps it; the framebuffer rounds to the nearest of 256 levels after each
pass. Texels are bytes, those of derived textures too. The primary colour
and the environment colours are the floats the renderer gives OpenGL,
unless nv2a_ff_model_byte_colors is on: then they are rounded to bytes as
well (primary colours at the vertices), which is what a renderer that
sends byte colours, or a chip that keeps 8 bits, does to them. */

void nv2a_ff_model_byte_colors(int enable);

struct nv2a_ff_model_fragment
{
	/* the interpolated primary and secondary colours */
	float primary[4];
	float secondary[3];
	/* each unit's texel, after its bake */
	float texel[NV2A_FF_MAXIMUM_UNITS][4];
	/* the interpolated fog factor, when the pass has fog */
	float fog_factor;
};

/* one texture unit: previous -> result */
void nv2a_ff_model_unit(const struct nv2a_ff_unit *unit, const float previous[4], const float primary[4],
	const float texel[4], const float constant[4], float result[4]);

/* the blend equation: source and destination -> result (not rounded) */
void nv2a_ff_model_blend(unsigned long source_blend, unsigned long destination_blend,
	unsigned long blend_operation, const float source[4], const float destination[4],
	const float blend_color[4], float result[4]);

/* Draws one fragment of one pass into framebuffer (0..1 values on the
8-bit grid). Returns 0 when the alpha test rejects the fragment. */
int nv2a_ff_model_pass(const struct nv2a_ff_plan *plan, int pass,
	const struct nv2a_ff_constants *constants, const struct nv2a_ff_model_fragment *fragment,
	float framebuffer[4]);

/* ---------- comparing a plan with the reference

A sample: the fragment lies inside a triangle, so the colours and the fog
coordinate are given at three vertices with the fragment's weights, and
each stage's texel lies between two texels of its texture. */

struct nv2a_ff_sample
{
	float vertex_v0[3][4];
	float vertex_v1[3][4];
	float vertex_fog[3];
	float weights[3];
	float texel[2][NV2A_FF_STAGE_COUNT][4];
	float texel_weight[NV2A_FF_STAGE_COUNT];
	float texcoord[NV2A_FF_STAGE_COUNT][4];
	float framebuffer[4];
};

/* a deterministic sample from a seed (which it advances) */
void nv2a_ff_sample_random(unsigned long *seed, struct nv2a_ff_sample *sample);

/* Draws the sample both ways: the state's colour blended into the
framebuffer as the draw says, and the plan's passes through the model.
Either may be NULL. Returns the largest difference between the two
framebuffers in steps of 1/255 over the channels the draw writes (alpha
only when the capabilities have destination alpha). */
float nv2a_ff_sample_compare(const struct nv2a_ff_state *state, const struct nv2a_ff_constants *constants,
	const struct nv2a_ff_capabilities *capabilities, const struct nv2a_ff_draw *draw,
	const struct nv2a_ff_plan *plan, const struct nv2a_ff_sample *sample,
	float reference_framebuffer[4], float plan_framebuffer[4]);

/* ---------- describing a plan (logs, the test's table) */

/* one line per pass and bake, at most size characters */
void nv2a_ff_plan_describe(const struct nv2a_ff_plan *plan, char *text, unsigned long size);

/* what the state computes, as the compiler sees it: the colour and alpha
as sums of products, before the last clamp to 0..1 (with constants, their
values are put in) */
void nv2a_ff_state_describe(const struct nv2a_ff_state *state, const struct nv2a_ff_constants *constants,
	const struct nv2a_ff_capabilities *capabilities, char *text, unsigned long size);

#endif
