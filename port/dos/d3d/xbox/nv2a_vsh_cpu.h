/*
NV2A_VSH_CPU.H

NV2A vertex programs without a GPU that runs them (nv2a_vsh_cpu.c): what a
program computes, described so that a fixed-function renderer can hand the
work to OpenGL's matrices where that is possible, and an evaluator that
runs the program on the processor where it is not.

The instruction layout is the one nv2a_vsh.c translates to GLSL, and every
instruction means here what its GLSL means there.
*/

#ifndef __HALO_LINUX_NV2A_VSH_CPU_H
#define __HALO_LINUX_NV2A_VSH_CPU_H

#include "xgpu.h"

/* ---------- output registers */

/* output register addresses (o[]), as in the instruction's field */
enum
{
	_nv2a_vsh_output_position = 0,
	_nv2a_vsh_output_diffuse = 3,
	_nv2a_vsh_output_specular = 4,
	_nv2a_vsh_output_fog = 5,
	_nv2a_vsh_output_point_size = 6,
	_nv2a_vsh_output_back_diffuse = 7,
	_nv2a_vsh_output_back_specular = 8,
	_nv2a_vsh_output_texcoord0 = 9,
	_nv2a_vsh_output_texcoord1 = 10,
	_nv2a_vsh_output_texcoord2 = 11,
	_nv2a_vsh_output_texcoord3 = 12,
	NV2A_VSH_OUTPUT_COUNT = 13,

	NV2A_VSH_OUTPUT_NONE = 255
};

/* A set of output components: four bits for each output register, bit 3
of them x and bit 0 w (the order of the instruction's write masks). */
typedef unsigned long long nv2a_vsh_output_mask;

#define NV2A_VSH_OUTPUT_MASK(address, components) \
	((nv2a_vsh_output_mask)(components) << ((address) * 4))

/* ---------- classification */

enum
{
	/* the program never writes it: the register's initial value */
	_nv2a_vsh_class_unwritten = 0,
	/* constants only: the same for every vertex */
	_nv2a_vsh_class_constant,
	/* a sum of constants times the components of one input register, plus
	constants: a matrix that OpenGL can apply */
	_nv2a_vsh_class_affine,
	/* constants chosen by the vertex (a0-relative reads) and blended by
	inputs: a bone or instance matrix, not yet applied to anything */
	_nv2a_vsh_class_indexed,
	/* such a matrix applied to one input register: skinning */
	_nv2a_vsh_class_skinned,
	/* anything else */
	_nv2a_vsh_class_other
};

struct nv2a_vsh_output_class
{
	/* components the program writes (bit 3 is x) */
	unsigned char written;
	/* of the components written together: the class when they agree and
	_nv2a_vsh_class_other when they do not */
	unsigned char class;
	/* the input register of an affine or skinned output */
	unsigned char input;
	unsigned char component_class[4];
	unsigned char component_input[4];
	/* what the written components are computed from, after everything that
	does not reach them is removed: input registers (a bit each), constants
	read by their own number (hardware numbering: c[-96] is bit 0 of word
	0), and whether constants are read relative to a0 */
	unsigned short inputs;
	DWORD constants[XGPU_VERTEX_CONSTANT_COUNT / 32];
	BOOL relative_constants;
};

struct nv2a_vsh_classification
{
	/* The position before the screen-space conversion the program ends
	with (the clip-space position); the screen-space conversion itself is
	described by the epilogue members. outputs[0] describes the same. */
	unsigned char position_class;
	unsigned char position_input;
	/* The position is four dph or dp4 of one input register with four
	constants, each written straight to a component of oPos: the usual
	case (c[-96..-93] for the world, c[-68..-65] for the screen).
	position_rows are the constants of x, y, z and w in hardware
	numbering; position_homogeneous is FALSE when the rows are applied
	with dph (the input's w taken as 1). */
	BOOL position_rows_valid;
	BOOL position_homogeneous;
	unsigned char position_rows[4];

	/* The program converts its position to screen space the way Direct3D's
	own programs do: oPos.xyz = oPos.xyz * c[-38] * rcc(oPos.w) + c[-37].
	The instruction numbers of the multiply, the reciprocal and the
	multiply-add. TRUE only when that is all they do and nothing else reads
	or writes the position from the multiply on (every program of the game
	is so); otherwise the conversion is classified as the ordinary
	instructions it is, and the position comes out as
	_nv2a_vsh_class_other. */
	BOOL epilogue;
	short epilogue_scale;
	short epilogue_reciprocal;
	short epilogue_offset;

	/* every input register the program reads */
	unsigned short inputs;
	/* the program reads constants relative to a0 */
	BOOL relative_constants;

	struct nv2a_vsh_output_class outputs[NV2A_VSH_OUTPUT_COUNT];
};

/* ---------- programs */

struct nv2a_vsh_cpu_program;

/* Decodes and classifies a program (the instruction words after the
program header, as nv2a_vertex_shader_to_glsl takes them). NULL when
memory runs out. */
struct nv2a_vsh_cpu_program *nv2a_vsh_cpu_program_new(const DWORD *instructions, unsigned long instruction_count);
void nv2a_vsh_cpu_program_delete(struct nv2a_vsh_cpu_program *program);

const struct nv2a_vsh_classification *nv2a_vsh_cpu_classification(const struct nv2a_vsh_cpu_program *program);

/* The matrix of an affine (or constant) output with the current constants:

	output[row] = matrix[row][0] * v.x + matrix[row][1] * v.y +
		matrix[row][2] * v.z + matrix[row][3] * v.w + matrix[row][4]

where v is the output's input register and row 0 is x. For the position
this is the clip-space position (the epilogue is left out). Components the
program does not write keep their initial value (0, 0, 0, 1). The
arithmetic is done in double precision on the coefficients themselves, so
nothing is lost to the sizes of the vertices. FALSE when the output is not
affine. */
BOOL nv2a_vsh_cpu_affine_output(const struct nv2a_vsh_cpu_program *program, unsigned long output_address,
	const float (*constants)[4], float matrix[4][5]);

/* ---------- evaluation */

/* one input register */
struct nv2a_vsh_cpu_input
{
	/* the register's bytes in vertex 0 (the stream's data plus the
	element's offset), or NULL: the register holds value for every vertex */
	const void *data;
	/* bytes from one vertex to the next; 0 reads vertex 0 every time */
	unsigned long stride;
	/* D3DVSDT_* */
	unsigned long type;
	float value[4];
};

/* where a result goes: vertex N of the range is written at data + N * stride */
struct nv2a_vsh_cpu_array
{
	void *data;
	unsigned long stride;
};

struct nv2a_vsh_cpu_texcoord
{
	/* 2, 3 or 4 floats (s, t, r, q), or data NULL for none */
	struct nv2a_vsh_cpu_array array;
	/* the output register they come from (not the position: with
	NV2A_VSH_CPU_POSITION_CAPTURED the program's own oPos is not kept) */
	unsigned char source;
	unsigned char components;
};

/* a colour array: four bytes, red, green, blue, alpha; red, green and blue
are color_source's (oD0 as a rule) and alpha is component alpha_component
(0 is x) of alpha_source, each clamped to 0..1 as the GLSL does, then
multiplied by color_scale when that is not NULL and clamped again, and
stored as the nearest of 256 levels (0 for what is not a number); data NULL
for none */
struct nv2a_vsh_cpu_color
{
	struct nv2a_vsh_cpu_array array;
	unsigned char color_source;
	unsigned char alpha_source;
	unsigned char alpha_component;
	const float *color_scale;
};

/* the arrays one run can produce: texture coordinate sets and colours
(a fixed-function renderer's passes each want a colour of their own, and
one run over the vertices is cheaper than one for each array) */
#define NV2A_VSH_CPU_TEXCOORDS 4
#define NV2A_VSH_CPU_COLORS 6

enum
{
	/* rcp, rcc and rsq by division and square root, and the position
	divided by the viewport's scale rather than multiplied by its
	reciprocal: the GLSL's arithmetic to the bit. Without it the
	evaluators use the processor's reciprocal estimates refined by one
	Newton step (NV2A_VSH_CPU_ESTIMATE_TOLERANCE), which is faster. */
	_nv2a_vsh_cpu_exact_bit = 0,
	/* Take the clip-space position from before the program's screen-space
	conversion when the program has one (what nv2a_vsh.c does under
	HALO_ANDROID), instead of undoing the conversion by multiplying with w
	again. Without the rcc clamp's damage near the camera plane, and the
	conversion's instructions need not run. */
	_nv2a_vsh_cpu_position_captured_bit,
	/* OpenGL's own clip space, for a context without glClipControl: rows
	from the bottom (y negated) and depth from -w to w (z = 2z - w) */
	_nv2a_vsh_cpu_flip_y_bit,
	_nv2a_vsh_cpu_signed_depth_bit,

	NV2A_VSH_CPU_EXACT = 1 << _nv2a_vsh_cpu_exact_bit,
	NV2A_VSH_CPU_POSITION_CAPTURED = 1 << _nv2a_vsh_cpu_position_captured_bit,
	NV2A_VSH_CPU_FLIP_Y = 1 << _nv2a_vsh_cpu_flip_y_bit,
	NV2A_VSH_CPU_SIGNED_DEPTH = 1 << _nv2a_vsh_cpu_signed_depth_bit
};

/* The most a result of rcp, rcc or rsq computed from an estimate differs
from the exact one, relative to it: 2^-20. The processor's estimates are
within 1.5 * 2^-12; a Newton step squares that (2.25 * 2^-24 for the
reciprocal, 3.375 * 2^-24 for the reciprocal square root) and rounds four
times on the way (2^-24 each at most), and the exact result it is compared
with is itself rounded once or twice. */
#define NV2A_VSH_CPU_ESTIMATE_TOLERANCE (1.0f / 1048576.0f)

struct nv2a_vsh_cpu_draw
{
	/* c[-96..95], hardware numbering */
	const float (*constants)[4];
	struct nv2a_vsh_cpu_input inputs[XGPU_VERTEX_ATTRIBUTE_COUNT];

	/* the device's viewport_scale, viewport_offset and screen_offset
	uniforms (d3d8_gl.c) */
	float viewport_scale[4];
	float viewport_offset[4];
	float screen_offset;

	/* NV2A_VSH_CPU_* */
	unsigned long flags;

	/* Results; an array whose data is NULL is not produced, and what only
	it needs is not computed.
	position: four floats, the clip-space position.
	texcoords, colors: as above, any number of each.
	fog: one float, oFog.x. */
	struct nv2a_vsh_cpu_array position;
	struct nv2a_vsh_cpu_texcoord texcoords[NV2A_VSH_CPU_TEXCOORDS];
	struct nv2a_vsh_cpu_color colors[NV2A_VSH_CPU_COLORS];
	struct nv2a_vsh_cpu_array fog;
};

/* The output components a draw's arrays need. */
nv2a_vsh_output_mask nv2a_vsh_cpu_draw_mask(const struct nv2a_vsh_cpu_draw *draw);

/* Runs the program for vertices first to first + count - 1 of the inputs
and writes result N for vertex first + N. Only what the draw's arrays need
is computed; the reduced programs are kept with the program (by output set
and flags), so the first run with a new set costs more. FALSE when the
program cannot be run (out of memory, or too large).

nv2a_vsh_cpu_run works on four vertices in a register and sixteen at a
time with SSE (not SSE2); nv2a_vsh_cpu_run_scalar is the same in plain C,
one vertex at a time, and gives the same bits (where a result is not a
number, its sign and payload may differ). Both compute in single precision
with denormal results flushed to zero, whatever rounding the caller has set
(MXCSR and the x87's control word), and leave the processor's
floating-point state as they found it, but for MXCSR's denormals-are-zero
bit, which comes back clear (a Pentium III faults on a load that sets it).
They are not reentrant for one program, and the results must not overlap
the input streams (sixteen vertices are read before the first is written). */
BOOL nv2a_vsh_cpu_run(struct nv2a_vsh_cpu_program *program, const struct nv2a_vsh_cpu_draw *draw,
	unsigned long first, unsigned long count);
BOOL nv2a_vsh_cpu_run_scalar(struct nv2a_vsh_cpu_program *program, const struct nv2a_vsh_cpu_draw *draw,
	unsigned long first, unsigned long count);

/* What a reduced program does for each sixteen vertices (or each vertex in
the scalar evaluator), for cost estimates: operations by kind and the
component results they produce. */
enum
{
	_nv2a_vsh_cpu_cost_move,        /* copies and negations */
	_nv2a_vsh_cpu_cost_arithmetic,  /* mul, add, sub, min, max: one operation a component */
	_nv2a_vsh_cpu_cost_multiply_add,/* mad: two operations a component */
	_nv2a_vsh_cpu_cost_compare,     /* slt, sge */
	_nv2a_vsh_cpu_cost_dot3,
	_nv2a_vsh_cpu_cost_dot4,        /* dph and dp4 */
	_nv2a_vsh_cpu_cost_reciprocal,  /* rcp, rcc */
	_nv2a_vsh_cpu_cost_reciprocal_square_root,
	_nv2a_vsh_cpu_cost_address,     /* arl */
	_nv2a_vsh_cpu_cost_gather,      /* a0-relative constant components */
	_nv2a_vsh_cpu_cost_library,     /* exp, log, lit */
	NV2A_VSH_CPU_COST_COUNT
};

struct nv2a_vsh_cpu_cost
{
	unsigned long operations;       /* dispatched operations */
	unsigned long operations_by_kind[NV2A_VSH_CPU_COST_COUNT];
	unsigned long results_by_kind[NV2A_VSH_CPU_COST_COUNT];
	unsigned long input_components; /* read from the vertex streams */
	unsigned long constant_components;
	unsigned long slots;
};

BOOL nv2a_vsh_cpu_cost(struct nv2a_vsh_cpu_program *program, nv2a_vsh_output_mask mask, unsigned long flags,
	struct nv2a_vsh_cpu_cost *cost);

#endif
