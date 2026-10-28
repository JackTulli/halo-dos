/*
NV2A_VSH_CPU.C

NV2A vertex programs (Xbox vertex shader microcode) on the processor, for
a renderer whose OpenGL has no vertex shaders (OpenGL 1.1).

A program is decoded once (the instruction layout is nv2a_vsh.c's) and
classified: what each output is computed from, and whether OpenGL's own
matrices could compute it. For the outputs they cannot, the program is
turned into a list of operations on single components ("slots": one
component of one register), with the swizzles, write masks and negations
resolved, and everything that does not reach the outputs a draw uses
removed. The list is run for sixteen vertices at a time, each slot
holding the component of four times four vertices in SSE registers, or
for one vertex at a time in plain C.

Every operation computes what its GLSL in nv2a_vsh.c computes, in single
precision and in the order written there; sums of products are added from
left to right (x, y, z, w). The two evaluators give the same bits as each
other (a result that is not a number may differ in its sign and payload).
With NV2A_VSH_CPU_EXACT those are the bits of the GLSL's own arithmetic;
without it rcp, rcc and rsq come from the processor's estimates and are
off by NV2A_VSH_CPU_ESTIMATE_TOLERANCE at most.

tools/nv2a_vsh_cpu_test.py checks all of this against an interpreter of
the GLSL (port/win9x/tests/renderer).
*/

#include "nv2a_vsh_cpu.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef __SSE__
#include <xmmintrin.h>
#endif

#pragma STDC FP_CONTRACT OFF

/* ---------- instruction fields (nv2a_vsh.c) */

static unsigned long field(const DWORD *instruction, int word, int low_bit, int bit_count)
{
	return (instruction[word] >> low_bit) & ((1UL << bit_count) - 1);
}

enum
{
	_mac_nop, _mac_mov, _mac_mul, _mac_add, _mac_mad, _mac_dp3, _mac_dph, _mac_dp4,
	_mac_dst, _mac_min, _mac_max, _mac_slt, _mac_sge, _mac_arl,
};

enum
{
	_ilu_nop, _ilu_mov, _ilu_rcp, _ilu_rcc, _ilu_rsq, _ilu_exp, _ilu_log, _ilu_lit,
};

enum
{
	_mux_unknown, _mux_temporary, _mux_input, _mux_constant,
};

/* the viewport constants of the screen-space conversion, c[-38] and c[-37] */
#define CONSTANT_VIEWPORT_SCALE (XGPU_VERTEX_CONSTANT_BIAS - 38)
#define CONSTANT_VIEWPORT_OFFSET (XGPU_VERTEX_CONSTANT_BIAS - 37)

/* r12 reads back the position output */
#define TEMPORARY_POSITION 12
#define TEMPORARY_COUNT 12

struct vsh_operand
{
	unsigned char mux;
	unsigned char index;
	unsigned char negate;
	unsigned char swizzle[4];
};

enum
{
	_operand_a,
	_operand_b,
	_operand_c,
	NUMBER_OF_OPERANDS
};

struct vsh_instruction
{
	unsigned char mac;
	unsigned char ilu;
	struct vsh_operand operands[NUMBER_OF_OPERANDS];
	/* the one constant and the one input register an instruction names */
	unsigned char constant;
	unsigned char input;
	unsigned char relative;
	unsigned char mac_mask;
	unsigned char temporary;
	unsigned char ilu_mask;
	unsigned char output_mask;
	unsigned char output_is_register;
	unsigned char output_address;
	unsigned char output_from_ilu;
};

static void decode_instruction(const DWORD *words, struct vsh_instruction *instruction)
{
	struct vsh_operand *operand;

	operand = &instruction->operands[_operand_a];
	operand->negate = (unsigned char)field(words, 1, 8, 1);
	operand->swizzle[0] = (unsigned char)field(words, 1, 6, 2);
	operand->swizzle[1] = (unsigned char)field(words, 1, 4, 2);
	operand->swizzle[2] = (unsigned char)field(words, 1, 2, 2);
	operand->swizzle[3] = (unsigned char)field(words, 1, 0, 2);
	operand->index = (unsigned char)field(words, 2, 28, 4);
	operand->mux = (unsigned char)field(words, 2, 26, 2);

	operand = &instruction->operands[_operand_b];
	operand->negate = (unsigned char)field(words, 2, 25, 1);
	operand->swizzle[0] = (unsigned char)field(words, 2, 23, 2);
	operand->swizzle[1] = (unsigned char)field(words, 2, 21, 2);
	operand->swizzle[2] = (unsigned char)field(words, 2, 19, 2);
	operand->swizzle[3] = (unsigned char)field(words, 2, 17, 2);
	operand->index = (unsigned char)field(words, 2, 13, 4);
	operand->mux = (unsigned char)field(words, 2, 11, 2);

	operand = &instruction->operands[_operand_c];
	operand->negate = (unsigned char)field(words, 2, 10, 1);
	operand->swizzle[0] = (unsigned char)field(words, 2, 8, 2);
	operand->swizzle[1] = (unsigned char)field(words, 2, 6, 2);
	operand->swizzle[2] = (unsigned char)field(words, 2, 4, 2);
	operand->swizzle[3] = (unsigned char)field(words, 2, 2, 2);
	operand->index = (unsigned char)((field(words, 2, 0, 2) << 2) | field(words, 3, 30, 2));
	operand->mux = (unsigned char)field(words, 3, 28, 2);

	instruction->mac = (unsigned char)field(words, 1, 21, 4);
	instruction->ilu = (unsigned char)field(words, 1, 25, 3);
	instruction->constant = (unsigned char)field(words, 1, 13, 8);
	instruction->input = (unsigned char)field(words, 1, 9, 4);
	instruction->mac_mask = (unsigned char)field(words, 3, 24, 4);
	instruction->temporary = (unsigned char)field(words, 3, 20, 4);
	instruction->ilu_mask = (unsigned char)field(words, 3, 16, 4);
	instruction->output_mask = (unsigned char)field(words, 3, 12, 4);
	instruction->output_is_register = (unsigned char)field(words, 3, 11, 1);
	instruction->output_address = (unsigned char)field(words, 3, 3, 8);
	instruction->output_from_ilu = (unsigned char)field(words, 3, 2, 1);
	instruction->relative = (unsigned char)field(words, 3, 1, 1);
	/* (operations past the ones nv2a_vsh.c knows give zero there, and
	here: compile_mac) */
}

/* the registers an operation's units read: bit 0 A, bit 1 B, bit 2 C */
static unsigned long mac_operands(unsigned long mac)
{
	switch (mac)
	{
	case _mac_mov: case _mac_arl: return 1;
	case _mac_mul: case _mac_dp3: case _mac_dph: case _mac_dp4: case _mac_dst:
	case _mac_min: case _mac_max: case _mac_slt: case _mac_sge: return 3;
	case _mac_add: return 5;
	case _mac_mad: return 7;
	default: return 0;
	}
}

static BOOL output_address_valid(unsigned long address)
{
	return address == _nv2a_vsh_output_position ||
		(address >= _nv2a_vsh_output_diffuse && address < NV2A_VSH_OUTPUT_COUNT);
}

/* ---------- scalar pieces of the instruction set

Written from the GLSL of nv2a_vsh.c. min and max are spelled the way SSE's
MINPS and MAXPS choose (the second operand unless the first is strictly
smaller or larger), with the operands ordered so that the result is GLSL's
min(x, y) = y < x ? y : x and max(x, y) = x < y ? y : x. */

#define SCALAR_MIN(x, y) ((y) < (x) ? (y) : (x))
#define SCALAR_MAX(x, y) ((x) < (y) ? (y) : (x))

#define RCC_SMALLEST 5.42101e-20f
#define RCC_LARGEST 1.884467e+19f
#define LIT_EXPONENT_LIMIT 127.9961f
#define ARL_BIAS 0.001f

union float_bits
{
	float f;
	unsigned int u;
	int i;
};

static float scalar_reciprocal_clamp(float r)
{
	if (r > 0.0f)
	{
		r = SCALAR_MAX(r, RCC_SMALLEST);
		r = SCALAR_MIN(r, RCC_LARGEST);
	}
	else
	{
		r = SCALAR_MAX(r, -RCC_LARGEST);
		r = SCALAR_MIN(r, -RCC_SMALLEST);
	}
	return r;
}

static float scalar_rcc(float x)
{
	return scalar_reciprocal_clamp(1.0f / x);
}

static float scalar_rsq(float x)
{
	union float_bits bits;

	bits.f = x;
	bits.u &= 0x7FFFFFFF;
	return 1.0f / sqrtf(bits.f);
}

/* The estimates of the SSE evaluator (vector_reciprocal_estimate), one
number at a time: the processor's estimate of one number is the same alone
as in a register of four, so the two evaluators agree to the bit here as
well. A processor without the estimates computes exactly. */
#ifdef __SSE__
static float scalar_reciprocal_estimate(float x)
{
	float estimate = _mm_cvtss_f32(_mm_rcp_ss(_mm_set_ss(x)));
	union float_bits bits;

	bits.f = estimate;
	if ((bits.u & 0x7FFFFFFF) == 0x7F800000 || estimate == 0.0f)
		return 1.0f / x;
	return estimate * (2.0f - x * estimate);
}

static float scalar_reciprocal_square_root_estimate(float x)
{
	union float_bits bits;
	float estimate;

	bits.f = x;
	bits.u &= 0x7FFFFFFF;
	x = bits.f;
	estimate = _mm_cvtss_f32(_mm_rsqrt_ss(_mm_set_ss(x)));
	bits.f = estimate;
	if (bits.u == 0x7F800000 || estimate == 0.0f)
		return 1.0f / sqrtf(x);
	return (0.5f * estimate) * (3.0f - (x * estimate) * estimate);
}
#else
static float scalar_reciprocal_estimate(float x)
{
	return 1.0f / x;
}

static float scalar_reciprocal_square_root_estimate(float x)
{
	return scalar_rsq(x);
}
#endif

/* int(floor(x)); values no int holds give the smallest int, which the
constant index's clamp turns into constant 0 */
static int scalar_floor_to_int(float x)
{
	int whole;

	if (!(x >= -2147483648.0f && x < 2147483648.0f))
		return (int)0x80000000;
	whole = (int)x;
	if ((float)whole > x)
		whole--;
	return whole;
}

/* (the library's exponential only for the components that are read, bit n
for component n: the game's programs read the fraction alone) */
static void scalar_exp(float x, unsigned long components, float result[4])
{
	float whole = floorf(x);

	result[0] = (components & 1) ? exp2f(whole) : 0.0f;
	result[1] = x - whole;
	result[2] = (components & 4) ? exp2f(x) : 0.0f;
	result[3] = 1.0f;
}

/* floor(log2(x)) is the number's exponent and x / exp2(that) its
mantissa: both taken from the bits, which is what the expressions mean
without the rounding of a logarithm near a power of two */
static void scalar_log(float x, float result[4])
{
	union float_bits bits, power;
	int exponent;

	bits.f = x;
	bits.u &= 0x7FFFFFFF;
	x = bits.f;
	if (x == 0.0f)
	{
		result[0] = -1.0e30f;
		result[1] = 1.0f;
		result[2] = -1.0e30f;
		result[3] = 1.0f;
		return;
	}
	if (bits.u >= 0x7F800000)
	{
		/* infinity and NaN, by the expressions themselves */
		result[0] = floorf(log2f(x));
		result[1] = x / exp2f(result[0]);
		result[2] = log2f(x);
		result[3] = 1.0f;
		return;
	}
	exponent = (int)(bits.u >> 23) - 127;
	if (exponent == -127)
	{
		/* a denormal: the highest set bit of the mantissa */
		unsigned int mantissa = bits.u;

		exponent = -126 - 23;
		while (mantissa > 1)
		{
			mantissa >>= 1;
			exponent++;
		}
		power.u = 1U << (exponent + 149);
	}
	else
	{
		power.u = (unsigned int)(exponent + 127) << 23;
	}
	result[0] = (float)exponent;
	result[1] = x / power.f;
	result[2] = log2f(x);
	result[3] = 1.0f;
}

/* s is C.x, C.y and C.w */
static void scalar_lit(float x, float y, float w, float result[4])
{
	float specular = 0.0f;

	if (x > 0.0f)
	{
		float exponent = SCALAR_MAX(w, -LIT_EXPONENT_LIMIT);

		exponent = SCALAR_MIN(exponent, LIT_EXPONENT_LIMIT);
		specular = powf(SCALAR_MAX(y, 0.0f), exponent);
	}
	result[0] = 1.0f;
	result[1] = SCALAR_MAX(x, 0.0f);
	result[2] = specular;
	result[3] = 1.0f;
}

/* ---------- programs */

/* a reduced program (below) */
struct vsh_variant;

#define MAXIMUM_VARIANTS 12

struct nv2a_vsh_cpu_program
{
	struct vsh_instruction *instructions;
	unsigned long instruction_count;
	struct nv2a_vsh_classification classification;
	/* an instruction takes the reciprocal of the position's w: the
	position before it can be captured */
	BOOL capture;
	struct vsh_variant *variants[MAXIMUM_VARIANTS];
	unsigned long variant_count;
	unsigned long variant_clock;
};

static void variant_delete(struct vsh_variant *variant);
static struct vsh_variant *variant_new(const struct nv2a_vsh_cpu_program *program, nv2a_vsh_output_mask mask,
	unsigned long flags);
static void classify(struct nv2a_vsh_cpu_program *program);

/* n / 255 for the bytes of colours and PBYTE elements */
static float byte_fractions[256];
static void byte_fractions_prepare(void);

struct nv2a_vsh_cpu_program *nv2a_vsh_cpu_program_new(const DWORD *instructions, unsigned long instruction_count)
{
	struct nv2a_vsh_cpu_program *program = calloc(1, sizeof(*program));
	unsigned long index;

	if (!program)
		return NULL;
	byte_fractions_prepare();
	program->instructions = calloc(instruction_count ? instruction_count : 1, sizeof(*program->instructions));
	if (!program->instructions)
	{
		free(program);
		return NULL;
	}
	for (index = 0; index < instruction_count; index++)
	{
		const DWORD *words = instructions + index * 4;
		struct vsh_instruction *instruction = &program->instructions[index];

		decode_instruction(words, instruction);
		program->instruction_count++;
		if (instruction->ilu == _ilu_rcc && instruction->operands[_operand_c].mux == _mux_temporary &&
			instruction->operands[_operand_c].index == TEMPORARY_POSITION)
		{
			program->capture = TRUE;
		}
		/* the last instruction's flag */
		if (field(words, 3, 0, 1))
			break;
	}
	classify(program);
	return program;
}

void nv2a_vsh_cpu_program_delete(struct nv2a_vsh_cpu_program *program)
{
	unsigned long index;

	if (!program)
		return;
	for (index = 0; index < program->variant_count; index++)
		variant_delete(program->variants[index]);
	free(program->instructions);
	free(program);
}

const struct nv2a_vsh_classification *nv2a_vsh_cpu_classification(const struct nv2a_vsh_cpu_program *program)
{
	return &program->classification;
}

/* ---------- classification

Each component of each register is followed through the program as one of
the classes of nv2a_vsh_cpu.h, with one more for a constant read relative
to a0 that nothing has been done with yet. */

enum
{
	_class_relative = _nv2a_vsh_class_other + 1
};

struct value_class
{
	unsigned char class;
	unsigned char input;
};

static struct value_class make_class(unsigned long class, unsigned long input)
{
	struct value_class result;

	result.class = (unsigned char)class;
	result.input = (unsigned char)input;
	return result;
}

static BOOL class_is_matrix(struct value_class value)
{
	return value.class == _nv2a_vsh_class_indexed || value.class == _class_relative;
}

/* a sum */
static struct value_class class_add(struct value_class a, struct value_class b)
{
	if (class_is_matrix(a) && (class_is_matrix(b) || b.class == _nv2a_vsh_class_constant))
		return make_class(_nv2a_vsh_class_indexed, 0);
	if (class_is_matrix(b) && a.class == _nv2a_vsh_class_constant)
		return make_class(_nv2a_vsh_class_indexed, 0);
	if (a.class == _nv2a_vsh_class_constant)
		return b;
	if (b.class == _nv2a_vsh_class_constant)
		return a;
	if (a.class == _nv2a_vsh_class_affine && b.class == _nv2a_vsh_class_affine && a.input == b.input)
		return a;
	if ((a.class == _nv2a_vsh_class_skinned || b.class == _nv2a_vsh_class_skinned) &&
		(a.class == _nv2a_vsh_class_skinned || a.class == _nv2a_vsh_class_affine) &&
		(b.class == _nv2a_vsh_class_skinned || b.class == _nv2a_vsh_class_affine) && a.input == b.input)
	{
		return make_class(_nv2a_vsh_class_skinned, a.input);
	}
	return make_class(_nv2a_vsh_class_other, 0);
}

/* a product */
static struct value_class class_multiply(struct value_class a, struct value_class b)
{
	if (class_is_matrix(a) || class_is_matrix(b))
	{
		struct value_class other = class_is_matrix(a) ? b : a;

		/* a bone's row weighted by a constant or by an input (the weight) */
		if (other.class == _nv2a_vsh_class_constant || class_is_matrix(other) ||
			other.class == _nv2a_vsh_class_affine)
		{
			return make_class(_nv2a_vsh_class_indexed, 0);
		}
		return make_class(_nv2a_vsh_class_other, 0);
	}
	if (a.class == _nv2a_vsh_class_constant)
		return b;
	if (b.class == _nv2a_vsh_class_constant)
		return a;
	return make_class(_nv2a_vsh_class_other, 0);
}

/* a comparison, a selection or a function: constant when all it reads is */
static struct value_class class_function(const struct value_class *values, unsigned long count)
{
	unsigned long index;

	for (index = 0; index < count; index++)
	{
		if (values[index].class != _nv2a_vsh_class_constant)
			return make_class(_nv2a_vsh_class_other, 0);
	}
	return make_class(_nv2a_vsh_class_constant, 0);
}

/* dp3, dph (homogeneous) and dp4 of a and b */
static struct value_class class_dot(const struct value_class *a, const struct value_class *b, unsigned long count,
	BOOL homogeneous)
{
	struct value_class result = make_class(_nv2a_vsh_class_constant, 0);
	BOOL matrix = FALSE, skinned = FALSE, plain = TRUE;
	unsigned long index, input = 255;

	for (index = 0; index < count * 2; index++)
	{
		struct value_class value = index < count ? a[index] : b[index - count];

		if (class_is_matrix(value))
			matrix = TRUE;
		else if (value.class == _nv2a_vsh_class_skinned || value.class == _nv2a_vsh_class_affine)
		{
			if (value.class == _nv2a_vsh_class_skinned)
				skinned = TRUE;
			if (input != 255 && input != value.input)
				plain = FALSE;
			input = value.input;
		}
		else if (value.class != _nv2a_vsh_class_constant)
			plain = FALSE;
	}
	if (matrix)
	{
		/* a bone's row and one input register: skinning; a bone's row and
		constants: still a matrix */
		if (!plain || skinned)
			return make_class(_nv2a_vsh_class_other, 0);
		return input == 255 ? make_class(_nv2a_vsh_class_indexed, 0) :
			make_class(_nv2a_vsh_class_skinned, input);
	}
	if (skinned)
	{
		/* a skinned vector through constant rows (the projection) */
		BOOL constant_rows = TRUE;

		for (index = 0; index < count; index++)
		{
			if (a[index].class != _nv2a_vsh_class_constant && b[index].class != _nv2a_vsh_class_constant)
				constant_rows = FALSE;
		}
		if (homogeneous && b[3].class != _nv2a_vsh_class_constant && b[3].class != _nv2a_vsh_class_skinned)
			constant_rows = FALSE;
		if (plain && constant_rows)
			return make_class(_nv2a_vsh_class_skinned, input);
		return make_class(_nv2a_vsh_class_other, 0);
	}
	for (index = 0; index < count; index++)
		result = class_add(result, class_multiply(a[index], b[index]));
	if (homogeneous)
		result = class_add(result, b[3]);
	return result;
}

struct class_state
{
	struct value_class temporaries[TEMPORARY_COUNT][4];
	struct value_class outputs[NV2A_VSH_OUTPUT_COUNT][4];
	unsigned char written[NV2A_VSH_OUTPUT_COUNT];
	BOOL address_constant;
};

static void class_operand(const struct class_state *state, const struct vsh_instruction *instruction,
	unsigned long which, struct value_class values[4])
{
	const struct vsh_operand *operand = &instruction->operands[which];
	unsigned long index;

	for (index = 0; index < 4; index++)
	{
		unsigned long component = operand->swizzle[index];

		switch (operand->mux)
		{
		case _mux_temporary:
			if (operand->index == TEMPORARY_POSITION)
				values[index] = state->outputs[_nv2a_vsh_output_position][component];
			else if (operand->index < TEMPORARY_COUNT)
				values[index] = state->temporaries[operand->index][component];
			else
				values[index] = make_class(_nv2a_vsh_class_constant, 0);
			break;
		case _mux_input:
			values[index] = make_class(_nv2a_vsh_class_affine, instruction->input);
			break;
		case _mux_constant:
			values[index] = make_class(instruction->relative && !state->address_constant ?
				_class_relative : _nv2a_vsh_class_constant, 0);
			break;
		default:
			values[index] = make_class(_nv2a_vsh_class_constant, 0);
			break;
		}
	}
}

/* the instruction is part of the screen-space conversion */
static BOOL epilogue_scale_instruction(const struct vsh_instruction *instruction)
{
	return instruction->mac == _mac_mul && !instruction->relative &&
		instruction->operands[_operand_a].mux == _mux_temporary &&
		instruction->operands[_operand_a].index == TEMPORARY_POSITION &&
		instruction->operands[_operand_b].mux == _mux_constant &&
		instruction->constant == CONSTANT_VIEWPORT_SCALE;
}

static BOOL epilogue_offset_instruction(const struct vsh_instruction *instruction)
{
	return instruction->mac == _mac_mad && !instruction->relative &&
		instruction->operands[_operand_a].mux == _mux_temporary &&
		instruction->operands[_operand_a].index == TEMPORARY_POSITION &&
		instruction->operands[_operand_c].mux == _mux_constant &&
		instruction->constant == CONSTANT_VIEWPORT_OFFSET;
}

static BOOL epilogue_reciprocal_instruction(const struct vsh_instruction *instruction)
{
	return instruction->ilu == _ilu_rcc && instruction->operands[_operand_c].mux == _mux_temporary &&
		instruction->operands[_operand_c].index == TEMPORARY_POSITION;
}

/* the components of the position an instruction's units write */
static void position_writes(const struct vsh_instruction *instruction, unsigned long *mac_mask, unsigned long *ilu_mask)
{
	unsigned long ilu_temporary = instruction->mac != _mac_nop ? 1 : instruction->temporary;
	BOOL output = instruction->output_mask && instruction->output_is_register &&
		instruction->output_address == _nv2a_vsh_output_position;

	*mac_mask = *ilu_mask = 0;
	if (instruction->mac != _mac_nop && instruction->mac != _mac_arl && instruction->temporary == TEMPORARY_POSITION)
		*mac_mask |= instruction->mac_mask;
	if (instruction->mac != _mac_nop && output && !instruction->output_from_ilu)
		*mac_mask |= instruction->output_mask;
	if (instruction->ilu != _ilu_nop && ilu_temporary == TEMPORARY_POSITION)
		*ilu_mask |= instruction->ilu_mask;
	if (instruction->ilu != _ilu_nop && output && instruction->output_from_ilu)
		*ilu_mask |= instruction->output_mask;
}

/* a unit of the instruction reads the position back (r12) */
static BOOL reads_position(const struct vsh_instruction *instruction)
{
	unsigned long operands = mac_operands(instruction->mac) | (instruction->ilu != _ilu_nop ? 4 : 0);
	unsigned long which;

	for (which = 0; which < NUMBER_OF_OPERANDS; which++)
	{
		if ((operands & (1UL << which)) && instruction->operands[which].mux == _mux_temporary &&
			instruction->operands[which].index == TEMPORARY_POSITION)
		{
			return TRUE;
		}
	}
	return FALSE;
}

/* The three instructions found are the screen-space conversion and nothing
but it: oPos.xyz = oPos.xyz * c[-38], then oPos.xyz = oPos.xyz * r + c[-37]
with r the reciprocal of the w the position had before, and nothing else in
the program sees the position in between or afterwards. Only then can the
conversion be left out of what the position (and everything computed from
it) is classified as; a program that does anything else here is classified
with its conversion as ordinary instructions. */
static BOOL epilogue_is_plain(const struct nv2a_vsh_cpu_program *program)
{
	const struct nv2a_vsh_classification *classification = &program->classification;
	const struct vsh_instruction *scale = &program->instructions[classification->epilogue_scale];
	const struct vsh_instruction *reciprocal = &program->instructions[classification->epilogue_reciprocal];
	const struct vsh_instruction *offset = &program->instructions[classification->epilogue_offset];
	const struct vsh_operand *factor = &offset->operands[_operand_b];
	unsigned long mask, other_mask, ilu_mask, component, reciprocal_component = 4, index;

	if (classification->epilogue_reciprocal > classification->epilogue_scale)
		return FALSE;

	/* both write x, y and z (or fewer) of the position and nothing else */
	position_writes(scale, &mask, &ilu_mask);
	position_writes(offset, &other_mask, &index);
	if (!mask || (mask & 1) || ilu_mask || index || other_mask != mask)
		return FALSE;
	if ((scale->temporary != TEMPORARY_POSITION && scale->mac_mask) ||
		(offset->temporary != TEMPORARY_POSITION && offset->mac_mask))
	{
		return FALSE;
	}
	if ((scale->output_mask && !scale->output_from_ilu && scale->output_is_register &&
			scale->output_address != _nv2a_vsh_output_position) ||
		(offset->output_mask && !offset->output_from_ilu && offset->output_is_register &&
			offset->output_address != _nv2a_vsh_output_position))
	{
		return FALSE;
	}

	/* each component from itself, c[-38]'s and c[-37]'s, and one component
	of a temporary for all of them */
	if (scale->operands[_operand_a].negate || scale->operands[_operand_b].negate ||
		offset->operands[_operand_a].negate || offset->operands[_operand_c].negate || factor->negate ||
		factor->mux != _mux_temporary || factor->index >= TEMPORARY_COUNT)
	{
		return FALSE;
	}
	for (component = 0; component < 4; component++)
	{
		if (!(mask & (8 >> component)))
			continue;
		if (scale->operands[_operand_a].swizzle[component] != component ||
			scale->operands[_operand_b].swizzle[component] != component ||
			offset->operands[_operand_a].swizzle[component] != component ||
			offset->operands[_operand_c].swizzle[component] != component)
		{
			return FALSE;
		}
		if (reciprocal_component != 4 && factor->swizzle[component] != reciprocal_component)
			return FALSE;
		reciprocal_component = factor->swizzle[component];
	}

	/* that component is the reciprocal of the position's w */
	if (reciprocal->operands[_operand_c].negate || reciprocal->operands[_operand_c].swizzle[0] != 3 ||
		(reciprocal->mac != _mac_nop ? 1UL : reciprocal->temporary) != factor->index ||
		!(reciprocal->ilu_mask & (8 >> reciprocal_component)))
	{
		return FALSE;
	}

	for (index = classification->epilogue_reciprocal; index < program->instruction_count; index++)
	{
		const struct vsh_instruction *instruction = &program->instructions[index];
		unsigned long ilu_temporary = instruction->mac != _mac_nop ? 1 : instruction->temporary;

		position_writes(instruction, &other_mask, &ilu_mask);
		/* the position the reciprocal saw is the one the scale multiplies */
		if ((long)index < classification->epilogue_scale && (other_mask || ilu_mask))
			return FALSE;
		if ((long)index > classification->epilogue_scale && (long)index != classification->epilogue_offset &&
			(other_mask || ilu_mask || reads_position(instruction)))
		{
			return FALSE;
		}
		/* the reciprocal is still there when the offset multiplies by it */
		if ((long)index > classification->epilogue_reciprocal && (long)index < classification->epilogue_offset)
		{
			if (instruction->mac != _mac_nop && instruction->mac != _mac_arl && instruction->temporary == factor->index &&
				(instruction->mac_mask & (8 >> reciprocal_component)))
			{
				return FALSE;
			}
			if (instruction->ilu != _ilu_nop && ilu_temporary == factor->index &&
				(instruction->ilu_mask & (8 >> reciprocal_component)))
			{
				return FALSE;
			}
		}
	}
	return TRUE;
}

static void class_write(struct class_state *state, unsigned long temporary, unsigned long mask,
	const struct value_class values[4])
{
	unsigned long index;

	for (index = 0; index < 4; index++)
	{
		if (!(mask & (8 >> index)))
			continue;
		if (temporary == TEMPORARY_POSITION)
		{
			state->outputs[_nv2a_vsh_output_position][index] = values[index];
			state->written[_nv2a_vsh_output_position] |= (unsigned char)(8 >> index);
		}
		else if (temporary < TEMPORARY_COUNT)
		{
			state->temporaries[temporary][index] = values[index];
		}
	}
}

/* the usual position: four rows of constants applied to one input */
static void classify_position_rows(struct nv2a_vsh_cpu_program *program)
{
	struct nv2a_vsh_classification *classification = &program->classification;
	unsigned long index, component, found = 0, input = 0, operation = 0;

	for (index = 0; index < program->instruction_count; index++)
	{
		const struct vsh_instruction *instruction = &program->instructions[index];
		const struct vsh_operand *a = &instruction->operands[_operand_a], *b = &instruction->operands[_operand_b];
		unsigned long mask = 0;
		BOOL rows;

		if (classification->epilogue &&
			((long)index == classification->epilogue_scale || (long)index == classification->epilogue_offset))
		{
			continue;
		}
		if (instruction->mac != _mac_nop && instruction->mac != _mac_arl && instruction->temporary == TEMPORARY_POSITION)
			mask |= instruction->mac_mask;
		if (instruction->output_mask && instruction->output_is_register &&
			instruction->output_address == _nv2a_vsh_output_position && !instruction->output_from_ilu &&
			instruction->mac != _mac_nop)
		{
			mask |= instruction->output_mask;
		}
		if (instruction->ilu != _ilu_nop)
		{
			/* a write to the position by the other unit */
			if ((instruction->mac == _mac_nop && instruction->temporary == TEMPORARY_POSITION && instruction->ilu_mask) ||
				(instruction->output_mask && instruction->output_is_register && instruction->output_from_ilu &&
					instruction->output_address == _nv2a_vsh_output_position))
			{
				classification->position_rows_valid = FALSE;
				return;
			}
		}
		if (!mask)
			continue;
		rows = (instruction->mac == _mac_dph || instruction->mac == _mac_dp4) && !instruction->relative &&
			a->mux == _mux_input && !a->negate && b->mux == _mux_constant && !b->negate &&
			instruction->constant < XGPU_VERTEX_CONSTANT_COUNT;
		for (component = 0; rows && component < 4; component++)
		{
			if (a->swizzle[component] != component || b->swizzle[component] != component)
				rows = FALSE;
		}
		if (rows && found && (input != instruction->input || operation != instruction->mac))
			rows = FALSE;
		if (!rows)
		{
			classification->position_rows_valid = FALSE;
			return;
		}
		input = instruction->input;
		operation = instruction->mac;
		for (component = 0; component < 4; component++)
		{
			if (mask & (8 >> component))
			{
				classification->position_rows[component] = instruction->constant;
				found |= 8 >> component;
			}
		}
	}
	classification->position_rows_valid = found == 15;
	classification->position_homogeneous = operation == _mac_dp4;
}

static void classify_dependencies(struct nv2a_vsh_cpu_program *program);

static void classify(struct nv2a_vsh_cpu_program *program)
{
	struct nv2a_vsh_classification *classification = &program->classification;
	struct class_state state;
	unsigned long index, component, address;

	memset(&state, 0, sizeof(state));
	for (index = 0; index < TEMPORARY_COUNT; index++)
	{
		for (component = 0; component < 4; component++)
			state.temporaries[index][component] = make_class(_nv2a_vsh_class_constant, 0);
	}
	for (index = 0; index < NV2A_VSH_OUTPUT_COUNT; index++)
	{
		for (component = 0; component < 4; component++)
			state.outputs[index][component] = make_class(_nv2a_vsh_class_constant, 0);
	}
	state.address_constant = TRUE;

	/* the screen-space conversion: the last multiply by c[-38], the last
	multiply-add with c[-37] after it and a reciprocal of the position's w */
	classification->epilogue_scale = classification->epilogue_reciprocal = classification->epilogue_offset = -1;
	for (index = 0; index < program->instruction_count; index++)
	{
		const struct vsh_instruction *instruction = &program->instructions[index];
		BOOL to_position = (instruction->temporary == TEMPORARY_POSITION && instruction->mac_mask) ||
			(instruction->output_mask && instruction->output_is_register && !instruction->output_from_ilu &&
				instruction->output_address == _nv2a_vsh_output_position);

		if (epilogue_scale_instruction(instruction) && to_position)
		{
			classification->epilogue_scale = (short)index;
			classification->epilogue_offset = -1;
		}
		if (epilogue_offset_instruction(instruction) && to_position && classification->epilogue_scale >= 0)
			classification->epilogue_offset = (short)index;
		if (epilogue_reciprocal_instruction(instruction))
			classification->epilogue_reciprocal = (short)index;
	}
	classification->epilogue = classification->epilogue_scale >= 0 && classification->epilogue_offset >= 0 &&
		classification->epilogue_reciprocal >= 0 &&
		classification->epilogue_reciprocal < classification->epilogue_offset;
	if (classification->epilogue && !epilogue_is_plain(program))
		classification->epilogue = FALSE;

	for (index = 0; index < program->instruction_count; index++)
	{
		const struct vsh_instruction *instruction = &program->instructions[index];
		struct value_class a[4], b[4], c[4], mac[4], ilu[4];
		BOOL epilogue = classification->epilogue &&
			((long)index == classification->epilogue_scale || (long)index == classification->epilogue_offset);
		unsigned long operands = mac_operands(instruction->mac);

		class_operand(&state, instruction, _operand_a, a);
		class_operand(&state, instruction, _operand_b, b);
		class_operand(&state, instruction, _operand_c, c);
		if (instruction->ilu != _ilu_nop)
			operands |= 4;
		for (component = 0; component < NUMBER_OF_OPERANDS; component++)
		{
			if ((operands & (1UL << component)) && instruction->operands[component].mux == _mux_input)
				classification->inputs |= (unsigned short)(1U << instruction->input);
			if ((operands & (1UL << component)) && instruction->operands[component].mux == _mux_constant &&
				instruction->relative)
			{
				classification->relative_constants = TRUE;
			}
		}

		for (component = 0; component < 4; component++)
		{
			mac[component] = make_class(_nv2a_vsh_class_constant, 0);
			ilu[component] = make_class(_nv2a_vsh_class_constant, 0);
			switch (instruction->mac)
			{
			case _mac_mov: case _mac_arl: mac[component] = a[component]; break;
			case _mac_mul: mac[component] = class_multiply(a[component], b[component]); break;
			case _mac_add: mac[component] = class_add(a[component], c[component]); break;
			case _mac_mad:
				mac[component] = class_add(class_multiply(a[component], b[component]), c[component]);
				break;
			case _mac_dp3: mac[component] = class_dot(a, b, 3, FALSE); break;
			case _mac_dph: mac[component] = class_dot(a, b, 3, TRUE); break;
			case _mac_dp4: mac[component] = class_dot(a, b, 4, FALSE); break;
			case _mac_dst:
				if (component == 1)
					mac[component] = class_multiply(a[1], b[1]);
				else if (component == 2)
					mac[component] = a[2];
				else if (component == 3)
					mac[component] = b[3];
				break;
			case _mac_min: case _mac_max: case _mac_slt: case _mac_sge:
			{
				struct value_class both[2];

				both[0] = a[component];
				both[1] = b[component];
				mac[component] = class_function(both, 2);
				break;
			}
			default: break;
			}
			/* a constant read relative to a0 and passed on is a bone's row */
			if (mac[component].class == _class_relative)
				mac[component] = make_class(_nv2a_vsh_class_indexed, 0);

			switch (instruction->ilu)
			{
			case _ilu_mov:
				ilu[component] = c[component];
				if (ilu[component].class == _class_relative)
					ilu[component] = make_class(_nv2a_vsh_class_indexed, 0);
				break;
			case _ilu_rcp: case _ilu_rcc: case _ilu_rsq: case _ilu_exp: case _ilu_log:
				ilu[component] = class_function(c, 1);
				break;
			case _ilu_lit:
			{
				struct value_class read[3];

				read[0] = c[0];
				read[1] = c[1];
				read[2] = c[3];
				ilu[component] = class_function(read, 3);
				break;
			}
			default: break;
			}
		}

		/* the writes, in the GLSL's order */
		if (instruction->mac == _mac_arl)
			state.address_constant = a[0].class == _nv2a_vsh_class_constant;
		else if (instruction->mac != _mac_nop && instruction->mac_mask &&
			!(epilogue && instruction->temporary == TEMPORARY_POSITION))
		{
			class_write(&state, instruction->temporary, instruction->mac_mask, mac);
		}
		if (instruction->ilu != _ilu_nop && instruction->ilu_mask)
			class_write(&state, instruction->mac != _mac_nop ? 1 : instruction->temporary, instruction->ilu_mask, ilu);
		if (instruction->output_mask && (instruction->output_from_ilu ? instruction->ilu : instruction->mac) != 0 &&
			instruction->output_is_register && output_address_valid(instruction->output_address))
		{
			const struct value_class *source = instruction->output_from_ilu ? ilu : mac;

			address = instruction->output_address;
			for (component = 0; component < 4; component++)
			{
				if (!(instruction->output_mask & (8 >> component)))
					continue;
				state.written[address] |= (unsigned char)(8 >> component);
				if (address == _nv2a_vsh_output_position && epilogue && !instruction->output_from_ilu)
					continue;
				state.outputs[address][component] = source[component];
			}
		}
	}

	for (address = 0; address < NV2A_VSH_OUTPUT_COUNT; address++)
	{
		struct nv2a_vsh_output_class *output = &classification->outputs[address];
		BOOL first = TRUE;

		output->written = state.written[address];
		output->class = _nv2a_vsh_class_unwritten;
		for (component = 0; component < 4; component++)
		{
			struct value_class value = state.outputs[address][component];

			if (!(output->written & (8 >> component)))
			{
				output->component_class[component] = _nv2a_vsh_class_unwritten;
				continue;
			}
			if (value.class == _class_relative)
				value.class = _nv2a_vsh_class_indexed;
			output->component_class[component] = value.class;
			output->component_input[component] = value.input;
			if (first)
			{
				output->class = value.class;
				output->input = value.input;
				first = FALSE;
			}
			else if (value.class == _nv2a_vsh_class_constant)
			{
				/* a constant component beside others changes nothing */
			}
			else if (output->class == _nv2a_vsh_class_constant)
			{
				output->class = value.class;
				output->input = value.input;
			}
			else if (output->class != value.class || output->input != value.input)
			{
				output->class = _nv2a_vsh_class_other;
			}
		}
	}
	classification->position_class = classification->outputs[_nv2a_vsh_output_position].class;
	classification->position_input = classification->outputs[_nv2a_vsh_output_position].input;
	classify_position_rows(program);
	classify_dependencies(program);
}

/* ---------- affine outputs as matrices

The program is run on coefficients instead of numbers: each component is
x * [0] + y * [1] + z * [2] + w * [3] + [4] of the chosen input register,
or unknown when it is anything else. */

struct affine_value
{
	double coefficients[5];
	BOOL unknown;
};

static BOOL affine_is_constant(const struct affine_value *value)
{
	return !value->unknown && value->coefficients[0] == 0.0 && value->coefficients[1] == 0.0 &&
		value->coefficients[2] == 0.0 && value->coefficients[3] == 0.0;
}

static struct affine_value affine_constant(double number)
{
	struct affine_value result;

	memset(&result, 0, sizeof(result));
	result.coefficients[4] = number;
	return result;
}

static struct affine_value affine_unknown(void)
{
	struct affine_value result;

	memset(&result, 0, sizeof(result));
	result.unknown = TRUE;
	return result;
}

/* What is computed from constants alone is the single-precision number the
evaluators compute, rounded at every step as they round: a function of it
that is not smooth (exp's fraction, a comparison, a0) must see their number,
not a more exact one. */
static struct affine_value affine_settle(struct affine_value value)
{
	if (affine_is_constant(&value))
		value.coefficients[4] = (double)(float)value.coefficients[4];
	return value;
}

static struct affine_value affine_add(struct affine_value a, struct affine_value b)
{
	unsigned long index;

	if (a.unknown || b.unknown)
		return affine_unknown();
	for (index = 0; index < 5; index++)
		a.coefficients[index] += b.coefficients[index];
	return affine_settle(a);
}

static struct affine_value affine_multiply(struct affine_value a, struct affine_value b)
{
	unsigned long index;
	double factor;

	if (a.unknown || b.unknown)
		return affine_unknown();
	if (affine_is_constant(&a))
	{
		factor = a.coefficients[4];
		a = b;
	}
	else if (affine_is_constant(&b))
		factor = b.coefficients[4];
	else
		return affine_unknown();
	for (index = 0; index < 5; index++)
		a.coefficients[index] *= factor;
	return affine_settle(a);
}

struct affine_state
{
	struct affine_value temporaries[TEMPORARY_COUNT][4];
	struct affine_value outputs[NV2A_VSH_OUTPUT_COUNT][4];
	int address;
	BOOL address_unknown;
};

static void affine_operand(const struct affine_state *state, const struct vsh_instruction *instruction,
	unsigned long which, unsigned long input, const float (*constants)[4], struct affine_value values[4])
{
	const struct vsh_operand *operand = &instruction->operands[which];
	unsigned long index, scale;

	for (index = 0; index < 4; index++)
	{
		unsigned long component = operand->swizzle[index];

		switch (operand->mux)
		{
		case _mux_temporary:
			if (operand->index == TEMPORARY_POSITION)
				values[index] = state->outputs[_nv2a_vsh_output_position][component];
			else if (operand->index < TEMPORARY_COUNT)
				values[index] = state->temporaries[operand->index][component];
			else
				values[index] = affine_constant(0.0);
			break;
		case _mux_input:
			if (instruction->input == input)
			{
				values[index] = affine_constant(0.0);
				values[index].coefficients[component] = 1.0;
			}
			else
				values[index] = affine_unknown();
			break;
		case _mux_constant:
			if (instruction->relative && state->address_unknown)
				values[index] = affine_unknown();
			else
			{
				int number = instruction->constant;

				/* (wrapping, as the evaluators' sum does) */
				if (instruction->relative)
					number = (int)((unsigned int)state->address + instruction->constant);
				if (number < 0)
					number = 0;
				if (number > XGPU_VERTEX_CONSTANT_COUNT - 1)
					number = XGPU_VERTEX_CONSTANT_COUNT - 1;
				values[index] = affine_constant(constants[number][component]);
			}
			break;
		default:
			values[index] = affine_constant(0.0);
			break;
		}
		if (operand->negate && !values[index].unknown)
		{
			for (scale = 0; scale < 5; scale++)
				values[index].coefficients[scale] = -values[index].coefficients[scale];
		}
	}
}

static void affine_write(struct affine_state *state, unsigned long temporary, unsigned long mask,
	const struct affine_value values[4])
{
	unsigned long index;

	for (index = 0; index < 4; index++)
	{
		if (!(mask & (8 >> index)))
			continue;
		if (temporary == TEMPORARY_POSITION)
			state->outputs[_nv2a_vsh_output_position][index] = values[index];
		else if (temporary < TEMPORARY_COUNT)
			state->temporaries[temporary][index] = values[index];
	}
}

BOOL nv2a_vsh_cpu_affine_output(const struct nv2a_vsh_cpu_program *program, unsigned long output_address,
	const float (*constants)[4], float matrix[4][5])
{
	const struct nv2a_vsh_classification *classification = &program->classification;
	struct affine_state *state;
	unsigned long index, component, input;
	BOOL result = TRUE;

	if (!output_address_valid(output_address))
		return FALSE;
	if (classification->outputs[output_address].class > _nv2a_vsh_class_affine)
		return FALSE;
	/* an unwritten point size is the device's, which is not known here */
	if (output_address == _nv2a_vsh_output_point_size && !classification->outputs[output_address].written)
		return FALSE;
	input = classification->outputs[output_address].class == _nv2a_vsh_class_affine ?
		classification->outputs[output_address].input : XGPU_VERTEX_ATTRIBUTE_COUNT;
	state = calloc(1, sizeof(*state));
	if (!state)
		return FALSE;
	for (index = 0; index < NV2A_VSH_OUTPUT_COUNT; index++)
		state->outputs[index][3] = affine_constant(1.0);
	for (component = 0; component < 4; component++)
		state->outputs[_nv2a_vsh_output_fog][component] = affine_constant(1.0);

	for (index = 0; index < program->instruction_count; index++)
	{
		const struct vsh_instruction *instruction = &program->instructions[index];
		struct affine_value a[4], b[4], c[4], mac[4], ilu[4];
		BOOL epilogue = classification->epilogue &&
			((long)index == classification->epilogue_scale || (long)index == classification->epilogue_offset);
		float function[4];

		affine_operand(state, instruction, _operand_a, input, constants, a);
		affine_operand(state, instruction, _operand_b, input, constants, b);
		affine_operand(state, instruction, _operand_c, input, constants, c);
		for (component = 0; component < 4; component++)
		{
			mac[component] = affine_constant(0.0);
			ilu[component] = affine_constant(0.0);
			switch (instruction->mac)
			{
			case _mac_mov: case _mac_arl: mac[component] = a[component]; break;
			case _mac_mul: mac[component] = affine_multiply(a[component], b[component]); break;
			case _mac_add: mac[component] = affine_add(a[component], c[component]); break;
			case _mac_mad:
				mac[component] = affine_add(affine_multiply(a[component], b[component]), c[component]);
				break;
			case _mac_dp3: case _mac_dph: case _mac_dp4:
				mac[component] = affine_add(affine_add(affine_multiply(a[0], b[0]), affine_multiply(a[1], b[1])),
					affine_multiply(a[2], b[2]));
				if (instruction->mac == _mac_dph)
					mac[component] = affine_add(mac[component], b[3]);
				if (instruction->mac == _mac_dp4)
					mac[component] = affine_add(mac[component], affine_multiply(a[3], b[3]));
				break;
			case _mac_dst:
				if (component == 0)
					mac[component] = affine_constant(1.0);
				else if (component == 1)
					mac[component] = affine_multiply(a[1], b[1]);
				else if (component == 2)
					mac[component] = a[2];
				else
					mac[component] = b[3];
				break;
			case _mac_min: case _mac_max: case _mac_slt: case _mac_sge:
				if (affine_is_constant(&a[component]) && affine_is_constant(&b[component]))
				{
					float x = (float)a[component].coefficients[4], y = (float)b[component].coefficients[4];

					switch (instruction->mac)
					{
					case _mac_min: mac[component] = affine_constant(SCALAR_MIN(x, y)); break;
					case _mac_max: mac[component] = affine_constant(SCALAR_MAX(x, y)); break;
					case _mac_slt: mac[component] = affine_constant(x < y ? 1.0 : 0.0); break;
					default: mac[component] = affine_constant(x >= y ? 1.0 : 0.0); break;
					}
				}
				else
					mac[component] = affine_unknown();
				break;
			default: break;
			}
		}
		if (instruction->ilu == _ilu_mov)
			memcpy(ilu, c, sizeof(ilu));
		else if (instruction->ilu != _ilu_nop)
		{
			BOOL known = affine_is_constant(&c[0]);
			float x = (float)c[0].coefficients[4];

			function[0] = function[1] = function[2] = function[3] = 0.0f;
			switch (instruction->ilu)
			{
			case _ilu_rcp: function[0] = function[1] = function[2] = function[3] = 1.0f / x; break;
			case _ilu_rcc: function[0] = function[1] = function[2] = function[3] = scalar_rcc(x); break;
			case _ilu_rsq: function[0] = function[1] = function[2] = function[3] = scalar_rsq(x); break;
			case _ilu_exp: scalar_exp(x, 15, function); break;
			case _ilu_log: scalar_log(x, function); break;
			case _ilu_lit:
				known = known && affine_is_constant(&c[1]) && affine_is_constant(&c[3]);
				scalar_lit(x, (float)c[1].coefficients[4], (float)c[3].coefficients[4], function);
				break;
			default: break;
			}
			for (component = 0; component < 4; component++)
				ilu[component] = known ? affine_constant(function[component]) : affine_unknown();
		}

		if (instruction->mac == _mac_arl)
		{
			state->address_unknown = !affine_is_constant(&a[0]);
			if (!state->address_unknown)
				state->address = scalar_floor_to_int((float)a[0].coefficients[4] + ARL_BIAS);
		}
		else if (instruction->mac != _mac_nop && instruction->mac_mask &&
			!(epilogue && instruction->temporary == TEMPORARY_POSITION))
		{
			affine_write(state, instruction->temporary, instruction->mac_mask, mac);
		}
		if (instruction->ilu != _ilu_nop && instruction->ilu_mask)
			affine_write(state, instruction->mac != _mac_nop ? 1 : instruction->temporary, instruction->ilu_mask, ilu);
		if (instruction->output_mask && (instruction->output_from_ilu ? instruction->ilu : instruction->mac) != 0 &&
			instruction->output_is_register && output_address_valid(instruction->output_address) &&
			!(epilogue && instruction->output_address == _nv2a_vsh_output_position && !instruction->output_from_ilu))
		{
			for (component = 0; component < 4; component++)
			{
				if (instruction->output_mask & (8 >> component))
				{
					state->outputs[instruction->output_address][component] =
						instruction->output_from_ilu ? ilu[component] : mac[component];
				}
			}
		}
	}

	for (component = 0; component < 4; component++)
	{
		const struct affine_value *value = &state->outputs[output_address][component];

		if (value->unknown)
			result = FALSE;
		for (index = 0; index < 5; index++)
			matrix[component][index] = (float)value->coefficients[index];
	}
	free(state);
	return result;
}

/* ---------- slots

One component of one register. The fixed ones first; then the constant
components a reduced program reads (negated ones apart), then what single
instructions need for a moment. */

enum
{
	_slot_zero = 0,
	_slot_one,
	/* a0, as an integer */
	_slot_address,
	_slot_unused,
	/* the position before the screen-space conversion */
	_slot_capture,
	_slot_temporaries = _slot_capture + 4,
	_slot_outputs = _slot_temporaries + TEMPORARY_COUNT * 4,
	_slot_inputs = _slot_outputs + NV2A_VSH_OUTPUT_COUNT * 4,
	_slot_dynamic = _slot_inputs + XGPU_VERTEX_ATTRIBUTE_COUNT * 4,

	/* a slot of the instruction being compiled, until the constants are
	counted */
	SLOT_SCRATCH_FLAG = 0x4000,
	/* a finished program names its slots by their byte offsets, in 16 bits */
	SLOT_BYTES = 64,
	MAXIMUM_SLOTS = 0x10000 / SLOT_BYTES
};

/* ---------- operations */

enum
{
	/* a component each: dst[n] from a[n], b[n], c[n] */
	_op_move,
	_op_negate,
	_op_multiply,
	_op_add,
	_op_subtract,           /* a - b */
	_op_multiply_add,       /* a * b + c */
	_op_multiply_subtract,  /* a * b - c */
	_op_subtract_product,   /* c - a * b */
	_op_minimum,
	_op_maximum,
	_op_less,
	_op_greater_equal,
	/* one result: dst[0] from a[0..3] and b[0..3] */
	_op_dot3,
	_op_dot_homogeneous,
	_op_dot4,
	/* a0 = dst[0] from a[0] */
	_op_address,
	/* components b[n] of constant a0 + c[0] to dst[n]; a[0] is a0's slot */
	_op_gather,
	/* one result: dst[0] from a[0] */
	_op_reciprocal,
	_op_reciprocal_estimate,
	_op_reciprocal_clamped,
	_op_reciprocal_clamped_estimate,
	_op_reciprocal_square_root,
	_op_reciprocal_square_root_estimate,
	/* components b[n] of the function's result to dst[n], from a[0] (and
	a[1], a[2] for lit: C.x, C.y, C.w) */
	_op_exponent,
	_op_logarithm,
	_op_light,
	NUMBER_OF_OPS
};

struct micro_op
{
	unsigned char op;
	unsigned char count;
	unsigned short dst[4];
	unsigned short a[4];
	unsigned short b[4];
	unsigned short c[4];
};

static BOOL op_is_componentwise(unsigned long op)
{
	return op <= _op_greater_equal;
}

/* how many of a[], b[] and c[] an operation's entry reads */
static void op_reads(unsigned long op, unsigned long *a, unsigned long *b, unsigned long *c)
{
	*a = *b = *c = 0;
	switch (op)
	{
	case _op_move: case _op_negate: *a = 1; break;
	case _op_multiply: case _op_add: case _op_subtract: case _op_minimum: case _op_maximum:
	case _op_less: case _op_greater_equal: *a = 1; *b = 1; break;
	case _op_multiply_add: case _op_multiply_subtract: case _op_subtract_product: *a = 1; *b = 1; *c = 1; break;
	case _op_dot3: *a = 3; *b = 3; break;
	case _op_dot_homogeneous: *a = 3; *b = 4; break;
	case _op_dot4: *a = 4; *b = 4; break;
	case _op_gather: *a = 1; break;
	case _op_light: *a = 3; break;
	default: *a = 1; break;
	}
}

struct constant_slot
{
	unsigned short slot;
	unsigned char constant;
	unsigned char component;
	unsigned char negate;
};

struct input_load
{
	unsigned char input;
	/* bit n: component n is read */
	unsigned char components;
};

struct vsh_variant
{
	nv2a_vsh_output_mask mask;
	unsigned long flags;
	unsigned long used;
	struct micro_op *ops;
	unsigned long op_count;
	struct constant_slot *constants;
	unsigned long constant_count;
	struct input_load inputs[XGPU_VERTEX_ATTRIBUTE_COUNT];
	unsigned long input_count;
	/* the position is in the capture slots */
	BOOL captured;
	BOOL relative_constants;
	unsigned long slot_count;
	/* slot_count slots of SLOT_BYTES bytes, each on its own cache lines */
	void *slot_memory;
	void *slots;
};

/* the flags a reduced program depends on */
#define VARIANT_FLAGS (NV2A_VSH_CPU_EXACT | NV2A_VSH_CPU_POSITION_CAPTURED)

static void variant_delete(struct vsh_variant *variant)
{
	if (!variant)
		return;
	free(variant->ops);
	free(variant->constants);
	free(variant->slot_memory);
	free(variant);
}

/* ---------- compiling instructions to operations */

/* what one unit computes for one component, before it has a place */
struct entry
{
	unsigned char op;
	/* the unit's result component it is, or 255 for all of them (the
	operation has one result) */
	unsigned char component;
	/* the function's component for exp, log and lit */
	unsigned char selector;
	unsigned short a[4];
	unsigned short b[4];
	unsigned short c;
	unsigned short home;
};

#define MAXIMUM_ENTRIES 8
#define ENTRY_ALL_COMPONENTS 255

/* a write of an instruction: component of a unit's result to a slot */
struct assignment
{
	unsigned short slot;
	unsigned char unit;
	unsigned char component;
	unsigned char dropped;
};

struct builder
{
	unsigned long flags;
	struct micro_op *ops;
	unsigned long op_count;
	unsigned long op_capacity;
	struct constant_slot *constants;
	unsigned long constant_count;
	unsigned long constant_capacity;
	unsigned long scratch_count;
	BOOL failed;
	BOOL captured;

	/* the instruction being compiled */
	const struct vsh_instruction *instruction;
	unsigned long scratch_next;
	unsigned short gather_slots[4];
	unsigned long gather_components;
	struct
	{
		unsigned short from;
		unsigned short to;
	} negated[12];
	unsigned long negated_count;
};

static struct micro_op *builder_op(struct builder *builder, unsigned long op)
{
	static struct micro_op discarded;
	struct micro_op *result;

	if (builder->op_count == builder->op_capacity)
	{
		unsigned long capacity = builder->op_capacity ? builder->op_capacity * 2 : 64;
		struct micro_op *ops = realloc(builder->ops, capacity * sizeof(*ops));

		if (!ops)
		{
			builder->failed = TRUE;
			memset(&discarded, 0, sizeof(discarded));
			return &discarded;
		}
		builder->ops = ops;
		builder->op_capacity = capacity;
	}
	result = &builder->ops[builder->op_count++];
	memset(result, 0, sizeof(*result));
	result->op = (unsigned char)op;
	return result;
}

static unsigned short builder_scratch(struct builder *builder)
{
	unsigned long slot = builder->scratch_next++;

	if (builder->scratch_next > builder->scratch_count)
		builder->scratch_count = builder->scratch_next;
	return (unsigned short)(SLOT_SCRATCH_FLAG | slot);
}

static unsigned short builder_constant(struct builder *builder, unsigned long constant, unsigned long component,
	unsigned long negate)
{
	unsigned long index;
	struct constant_slot *slot;

	if (constant > XGPU_VERTEX_CONSTANT_COUNT - 1)
		constant = XGPU_VERTEX_CONSTANT_COUNT - 1;
	for (index = 0; index < builder->constant_count; index++)
	{
		slot = &builder->constants[index];
		if (slot->constant == constant && slot->component == component && slot->negate == negate)
			return slot->slot;
	}
	if (builder->constant_count == builder->constant_capacity)
	{
		unsigned long capacity = builder->constant_capacity ? builder->constant_capacity * 2 : 64;
		struct constant_slot *constants = realloc(builder->constants, capacity * sizeof(*constants));

		if (!constants)
		{
			builder->failed = TRUE;
			return _slot_zero;
		}
		builder->constants = constants;
		builder->constant_capacity = capacity;
	}
	slot = &builder->constants[builder->constant_count];
	slot->slot = (unsigned short)(_slot_dynamic + builder->constant_count);
	slot->constant = (unsigned char)constant;
	slot->component = (unsigned char)component;
	slot->negate = (unsigned char)negate;
	builder->constant_count++;
	return slot->slot;
}

/* the slot of component `index` of an operand as the instruction reads it
(after the swizzle); *negate is set when the value is the slot's, negated */
static unsigned short operand_slot(struct builder *builder, unsigned long which, unsigned long index,
	unsigned long *negate)
{
	const struct vsh_instruction *instruction = builder->instruction;
	const struct vsh_operand *operand = &instruction->operands[which];
	unsigned long component = operand->swizzle[index];

	*negate = operand->negate;
	switch (operand->mux)
	{
	case _mux_temporary:
		if (operand->index == TEMPORARY_POSITION)
			return (unsigned short)(_slot_outputs + _nv2a_vsh_output_position * 4 + component);
		if (operand->index < TEMPORARY_COUNT)
			return (unsigned short)(_slot_temporaries + operand->index * 4 + component);
		return _slot_zero;
	case _mux_input:
		return (unsigned short)(_slot_inputs + instruction->input * 4 + component);
	case _mux_constant:
		if (instruction->relative)
		{
			if (!(builder->gather_components & (1UL << component)))
			{
				builder->gather_components |= 1UL << component;
				builder->gather_slots[component] = builder_scratch(builder);
			}
			return builder->gather_slots[component];
		}
		*negate = 0;
		return builder_constant(builder, instruction->constant, component, operand->negate);
	default:
		return _slot_zero;
	}
}

/* the same with the negation done: a slot that holds the value */
static unsigned short operand_value(struct builder *builder, unsigned long which, unsigned long index)
{
	unsigned long negate, entry;
	unsigned short slot = operand_slot(builder, which, index, &negate);

	if (!negate)
		return slot;
	for (entry = 0; entry < builder->negated_count; entry++)
	{
		if (builder->negated[entry].from == slot)
			return builder->negated[entry].to;
	}
	if (builder->negated_count == sizeof(builder->negated) / sizeof(builder->negated[0]))
	{
		builder->failed = TRUE;
		return slot;
	}
	builder->negated[builder->negated_count].from = slot;
	builder->negated[builder->negated_count].to = builder_scratch(builder);
	return builder->negated[builder->negated_count++].to;
}

/* the entries of the MAC unit for the components in `needed` (bit 3 x) */
static unsigned long compile_mac(struct builder *builder, unsigned long needed, struct entry *entries)
{
	const struct vsh_instruction *instruction = builder->instruction;
	unsigned long count = 0, component, index, negate_a, negate_b, negate_c;
	struct entry *entry;

	switch (instruction->mac)
	{
	case _mac_dp3: case _mac_dph: case _mac_dp4:
		if (!needed)
			return 0;
		entry = &entries[count++];
		entry->op = instruction->mac == _mac_dp3 ? _op_dot3 : instruction->mac == _mac_dph ? _op_dot_homogeneous : _op_dot4;
		entry->component = ENTRY_ALL_COMPONENTS;
		for (index = 0; index < 4; index++)
		{
			if (index < 3 || instruction->mac == _mac_dp4)
				entry->a[index] = operand_value(builder, _operand_a, index);
			if (index < 3 || instruction->mac != _mac_dp3)
				entry->b[index] = operand_value(builder, _operand_b, index);
		}
		return count;
	case _mac_arl:
		entry = &entries[count++];
		entry->op = _op_address;
		entry->component = ENTRY_ALL_COMPONENTS;
		entry->a[0] = operand_value(builder, _operand_a, 0);
		entry->home = _slot_address;
		break;
	default:
		break;
	}

	for (component = 0; component < 4; component++)
	{
		if (!(needed & (8 >> component)))
			continue;
		entry = &entries[count++];
		entry->component = (unsigned char)component;
		switch (instruction->mac)
		{
		case _mac_mov: case _mac_arl:
			entry->op = _op_move;
			entry->a[0] = operand_value(builder, _operand_a, component);
			break;
		case _mac_mul:
			entry->op = _op_multiply;
			entry->a[0] = operand_value(builder, _operand_a, component);
			entry->b[0] = operand_value(builder, _operand_b, component);
			break;
		case _mac_add:
			/* a + -c is a - c and -a + c is c - a, to the bit */
			entry->a[0] = operand_slot(builder, _operand_a, component, &negate_a);
			entry->b[0] = operand_slot(builder, _operand_c, component, &negate_c);
			if (negate_a && negate_c)
			{
				entry->a[0] = operand_value(builder, _operand_a, component);
				entry->op = _op_subtract;
			}
			else if (negate_c)
				entry->op = _op_subtract;
			else if (negate_a)
			{
				unsigned short swap = entry->a[0];

				entry->a[0] = entry->b[0];
				entry->b[0] = swap;
				entry->op = _op_subtract;
			}
			else
				entry->op = _op_add;
			break;
		case _mac_mad:
			/* the product's sign: -a * b is -(a * b), so -a * b + c is
			c - a * b and a * b + -c is a * b - c */
			entry->a[0] = operand_slot(builder, _operand_a, component, &negate_a);
			entry->b[0] = operand_slot(builder, _operand_b, component, &negate_b);
			entry->c = operand_slot(builder, _operand_c, component, &negate_c);
			if (negate_a && negate_b)
				negate_a = negate_b = 0;
			if ((negate_a || negate_b) && negate_c)
			{
				/* -(a * b) - c: negate one factor for real */
				if (negate_a)
					entry->a[0] = operand_value(builder, _operand_a, component);
				else
					entry->b[0] = operand_value(builder, _operand_b, component);
				entry->op = _op_multiply_subtract;
			}
			else if (negate_a || negate_b)
				entry->op = _op_subtract_product;
			else if (negate_c)
				entry->op = _op_multiply_subtract;
			else
				entry->op = _op_multiply_add;
			break;
		case _mac_dst:
			/* (1, A.y * B.y, A.z, B.w) */
			if (component == 0)
			{
				entry->op = _op_move;
				entry->a[0] = _slot_one;
			}
			else if (component == 1)
			{
				entry->op = _op_multiply;
				entry->a[0] = operand_value(builder, _operand_a, 1);
				entry->b[0] = operand_value(builder, _operand_b, 1);
			}
			else
			{
				entry->op = _op_move;
				entry->a[0] = component == 2 ? operand_value(builder, _operand_a, 2) :
					operand_value(builder, _operand_b, 3);
			}
			break;
		case _mac_min: case _mac_max: case _mac_slt: case _mac_sge:
			entry->op = instruction->mac == _mac_min ? _op_minimum : instruction->mac == _mac_max ? _op_maximum :
				instruction->mac == _mac_slt ? _op_less : _op_greater_equal;
			entry->a[0] = operand_value(builder, _operand_a, component);
			entry->b[0] = operand_value(builder, _operand_b, component);
			break;
		default:
			/* an operation the GLSL has no text for: zero */
			entry->op = _op_move;
			entry->a[0] = _slot_zero;
			break;
		}
	}
	return count;
}

static unsigned long compile_ilu(struct builder *builder, unsigned long needed, struct entry *entries)
{
	const struct vsh_instruction *instruction = builder->instruction;
	BOOL exact = (builder->flags & NV2A_VSH_CPU_EXACT) != 0;
	unsigned long count = 0, component;
	struct entry *entry;

	if (!needed)
		return 0;
	switch (instruction->ilu)
	{
	case _ilu_rcp: case _ilu_rcc: case _ilu_rsq:
		entry = &entries[count++];
		entry->op = instruction->ilu == _ilu_rcp ? (exact ? _op_reciprocal : _op_reciprocal_estimate) :
			instruction->ilu == _ilu_rcc ? (exact ? _op_reciprocal_clamped : _op_reciprocal_clamped_estimate) :
			(exact ? _op_reciprocal_square_root : _op_reciprocal_square_root_estimate);
		entry->component = ENTRY_ALL_COMPONENTS;
		entry->a[0] = operand_value(builder, _operand_c, 0);
		return count;
	default:
		break;
	}
	for (component = 0; component < 4; component++)
	{
		if (!(needed & (8 >> component)))
			continue;
		entry = &entries[count++];
		entry->component = (unsigned char)component;
		entry->selector = (unsigned char)component;
		switch (instruction->ilu)
		{
		case _ilu_mov:
			entry->op = _op_move;
			entry->a[0] = operand_value(builder, _operand_c, component);
			break;
		case _ilu_exp: case _ilu_log:
			entry->op = instruction->ilu == _ilu_exp ? _op_exponent : _op_logarithm;
			entry->a[0] = operand_value(builder, _operand_c, 0);
			break;
		case _ilu_lit:
			entry->op = _op_light;
			entry->a[0] = operand_value(builder, _operand_c, 0);
			entry->a[1] = operand_value(builder, _operand_c, 1);
			entry->a[2] = operand_value(builder, _operand_c, 3);
			break;
		default:
			entry->op = _op_move;
			entry->a[0] = _slot_zero;
			break;
		}
	}
	return count;
}

static BOOL entry_reads(const struct entry *entry, unsigned short slot)
{
	unsigned long a, b, c, index;

	op_reads(entry->op, &a, &b, &c);
	for (index = 0; index < a; index++)
	{
		if (entry->a[index] == slot)
			return TRUE;
	}
	for (index = 0; index < b; index++)
	{
		if (entry->b[index] == slot)
			return TRUE;
	}
	return c && entry->c == slot;
}

/* entries to operations: neighbours of one kind (and, for the functions,
of one argument) share an operation */
static void emit_entries(struct builder *builder, const struct entry *entries, unsigned long count)
{
	struct micro_op *op = NULL;
	unsigned long index, a, b, c, read;

	for (index = 0; index < count; index++)
	{
		const struct entry *entry = &entries[index];
		BOOL join = op && op->op == entry->op && op->count < 4 && entry->component != ENTRY_ALL_COMPONENTS;

		if (join && !op_is_componentwise(entry->op))
		{
			join = (entry->op == _op_exponent || entry->op == _op_logarithm || entry->op == _op_light) &&
				op->a[0] == entry->a[0] && (entry->op != _op_light || (op->a[1] == entry->a[1] && op->a[2] == entry->a[2]));
		}
		if (!join)
			op = builder_op(builder, entry->op);
		op_reads(entry->op, &a, &b, &c);
		if (op_is_componentwise(entry->op))
		{
			op->a[op->count] = entry->a[0];
			op->b[op->count] = entry->b[0];
			op->c[op->count] = entry->c;
		}
		else
		{
			for (read = 0; read < a; read++)
				op->a[read] = entry->a[read];
			if (entry->op == _op_exponent || entry->op == _op_logarithm || entry->op == _op_light)
				op->b[op->count] = entry->selector;
			else
			{
				for (read = 0; read < b; read++)
					op->b[read] = entry->b[read];
			}
		}
		op->dst[op->count++] = entry->home;
		if (!op_is_componentwise(entry->op) && entry->component == ENTRY_ALL_COMPONENTS)
			op = NULL;
	}
}

static void compile_instruction(struct builder *builder, const struct vsh_instruction *instruction)
{
	struct assignment assignments[12];
	struct entry entries[2][MAXIMUM_ENTRIES], moves[12], prelude[4];
	unsigned long assignment_count = 0, entry_count[2] = { 0, 0 }, move_count = 0, prelude_count;
	unsigned long needed[2] = { 0, 0 };
	unsigned long component, index, other, unit, ilu_temporary;
	unsigned long gather_op, negate_op;

	builder->instruction = instruction;
	builder->scratch_next = 0;
	builder->gather_components = 0;
	builder->negated_count = 0;
	memset(entries, 0, sizeof(entries));
	memset(moves, 0, sizeof(moves));
	memset(prelude, 0, sizeof(prelude));

	/* the writes, in the GLSL's order; a later write to the same place
	makes an earlier one pointless */
	if (instruction->mac != _mac_nop && instruction->mac != _mac_arl && instruction->mac_mask &&
		(instruction->temporary < TEMPORARY_COUNT || instruction->temporary == TEMPORARY_POSITION))
	{
		for (component = 0; component < 4; component++)
		{
			if (!(instruction->mac_mask & (8 >> component)))
				continue;
			assignments[assignment_count].slot = (unsigned short)(instruction->temporary == TEMPORARY_POSITION ?
				_slot_outputs + component : _slot_temporaries + instruction->temporary * 4 + component);
			assignments[assignment_count].unit = 0;
			assignments[assignment_count].dropped = 0;
			assignments[assignment_count++].component = (unsigned char)component;
		}
	}
	ilu_temporary = instruction->mac != _mac_nop ? 1 : instruction->temporary;
	if (instruction->ilu != _ilu_nop && instruction->ilu_mask &&
		(ilu_temporary < TEMPORARY_COUNT || ilu_temporary == TEMPORARY_POSITION))
	{
		for (component = 0; component < 4; component++)
		{
			if (!(instruction->ilu_mask & (8 >> component)))
				continue;
			assignments[assignment_count].slot = (unsigned short)(ilu_temporary == TEMPORARY_POSITION ?
				_slot_outputs + component : _slot_temporaries + ilu_temporary * 4 + component);
			assignments[assignment_count].unit = 1;
			assignments[assignment_count].dropped = 0;
			assignments[assignment_count++].component = (unsigned char)component;
		}
	}
	if (instruction->output_mask && (instruction->output_from_ilu ? instruction->ilu : instruction->mac) != 0 &&
		instruction->output_is_register && output_address_valid(instruction->output_address))
	{
		for (component = 0; component < 4; component++)
		{
			if (!(instruction->output_mask & (8 >> component)))
				continue;
			assignments[assignment_count].slot = (unsigned short)(_slot_outputs + instruction->output_address * 4 + component);
			assignments[assignment_count].unit = instruction->output_from_ilu ? 1 : 0;
			assignments[assignment_count].dropped = 0;
			assignments[assignment_count++].component = (unsigned char)component;
		}
	}
	for (index = 0; index < assignment_count; index++)
	{
		for (other = index + 1; other < assignment_count; other++)
		{
			if (assignments[other].slot == assignments[index].slot)
				assignments[index].dropped = 1;
		}
		if (!assignments[index].dropped)
			needed[assignments[index].unit] |= 8 >> assignments[index].component;
	}

	/* The position as it is before this instruction's writes, when the
	instruction is the one that takes its w's reciprocal */
	prelude_count = 0;
	if ((builder->flags & NV2A_VSH_CPU_POSITION_CAPTURED) && epilogue_reciprocal_instruction(instruction))
	{
		for (component = 0; component < 4; component++)
		{
			prelude[prelude_count].op = _op_move;
			prelude[prelude_count].a[0] = (unsigned short)(_slot_outputs + component);
			prelude[prelude_count++].home = (unsigned short)(_slot_capture + component);
		}
		builder->captured = TRUE;
	}
	emit_entries(builder, prelude, prelude_count);

	/* places for the constants read relative to a0 and for negated values
	are found while the units are compiled, and filled before them */
	gather_op = builder->op_count;
	builder_op(builder, _op_gather);
	negate_op = builder->op_count;
	builder_op(builder, _op_negate);
	builder_op(builder, _op_negate);
	builder_op(builder, _op_negate);

	entry_count[0] = compile_mac(builder, needed[0], entries[0]);
	entry_count[1] = compile_ilu(builder, needed[1], entries[1]);

	if (!builder->failed)
	{
		struct micro_op *op = &builder->ops[gather_op];

		op->a[0] = _slot_address;
		op->c[0] = instruction->constant;
		for (component = 0; component < 4; component++)
		{
			if (builder->gather_components & (1UL << component))
			{
				op->b[op->count] = (unsigned short)component;
				op->dst[op->count++] = builder->gather_slots[component];
			}
		}
		for (index = 0; index < builder->negated_count; index++)
		{
			op = &builder->ops[negate_op + index / 4];
			op->a[op->count] = builder->negated[index].from;
			op->dst[op->count++] = builder->negated[index].to;
		}
	}

	/* A unit's result goes straight to the first place it is written to,
	unless something the instruction still reads lives there: both units
	read their operands before either writes. Then it waits in a slot of
	its own. Further places get copies afterwards. */
	for (unit = 0; unit < 2; unit++)
	{
		for (index = 0; index < entry_count[unit]; index++)
		{
			struct entry *entry = &entries[unit][index];
			BOOL placed = FALSE;

			if (entry->op == _op_address)
				continue;
			for (other = 0; other < assignment_count && !placed; other++)
			{
				const struct assignment *assignment = &assignments[other];
				unsigned long later;
				BOOL hazard = FALSE;

				if (assignment->dropped || assignment->unit != unit ||
					(entry->component != ENTRY_ALL_COMPONENTS && assignment->component != entry->component))
				{
					continue;
				}
				for (later = index + 1; later < entry_count[unit]; later++)
					hazard = hazard || entry_reads(&entries[unit][later], assignment->slot);
				for (later = 0; unit == 0 && later < entry_count[1]; later++)
					hazard = hazard || entry_reads(&entries[1][later], assignment->slot);
				if (!hazard)
				{
					entry->home = assignment->slot;
					placed = TRUE;
				}
			}
			if (!placed)
				entry->home = builder_scratch(builder);
			for (other = 0; other < assignment_count; other++)
			{
				const struct assignment *assignment = &assignments[other];

				if (assignment->dropped || assignment->unit != unit || assignment->slot == entry->home ||
					(entry->component != ENTRY_ALL_COMPONENTS && assignment->component != entry->component))
				{
					continue;
				}
				moves[move_count].op = _op_move;
				moves[move_count].a[0] = entry->home;
				moves[move_count++].home = assignment->slot;
			}
		}
	}
	emit_entries(builder, entries[0], entry_count[0]);
	emit_entries(builder, entries[1], entry_count[1]);
	emit_entries(builder, moves, move_count);
}

/* ---------- removing what the outputs do not need */

static void mark(unsigned char *live, unsigned short slot)
{
	live[slot] = 1;
}

/* walks the operations backwards; `live` holds the slots needed after
them and ends up with the slots needed before them */
static void remove_dead(struct micro_op *ops, unsigned long *op_count, unsigned char *live)
{
	unsigned long index = *op_count, kept = *op_count, entry, a, b, c, read;

	while (index--)
	{
		struct micro_op *op = &ops[index];
		struct micro_op reduced = *op;

		reduced.count = 0;
		op_reads(op->op, &a, &b, &c);
		if (op_is_componentwise(op->op))
		{
			/* the last entry first: entries run in order */
			unsigned char keep[4] = { 0, 0, 0, 0 };

			entry = op->count;
			while (entry--)
			{
				if (!live[op->dst[entry]])
					continue;
				keep[entry] = 1;
				live[op->dst[entry]] = 0;
				mark(live, op->a[entry]);
				if (b)
					mark(live, op->b[entry]);
				if (c)
					mark(live, op->c[entry]);
			}
			for (entry = 0; entry < op->count; entry++)
			{
				if (!keep[entry])
					continue;
				reduced.dst[reduced.count] = op->dst[entry];
				reduced.a[reduced.count] = op->a[entry];
				reduced.b[reduced.count] = op->b[entry];
				reduced.c[reduced.count] = op->c[entry];
				reduced.count++;
			}
		}
		else
		{
			/* all results come from one reading of the operands */
			for (entry = 0; entry < op->count; entry++)
			{
				if (!live[op->dst[entry]])
					continue;
				live[op->dst[entry]] = 0;
				reduced.dst[reduced.count] = op->dst[entry];
				if (op->op == _op_gather || op->op == _op_exponent || op->op == _op_logarithm || op->op == _op_light)
					reduced.b[reduced.count] = op->b[entry];
				reduced.count++;
			}
			if (reduced.count)
			{
				for (read = 0; read < a; read++)
					mark(live, op->a[read]);
				for (read = 0; read < b; read++)
					mark(live, op->b[read]);
			}
		}
		if (reduced.count)
			ops[--kept] = reduced;
	}
	memmove(ops, ops + kept, (*op_count - kept) * sizeof(*ops));
	*op_count -= kept;
}

/* every slot number of an operation, for renumbering */
static void op_slots(struct micro_op *op, unsigned long *count, unsigned short **slots)
{
	unsigned long a, b, c, index, total = 0;

	op_reads(op->op, &a, &b, &c);
	for (index = 0; index < op->count; index++)
		slots[total++] = &op->dst[index];
	if (op_is_componentwise(op->op))
	{
		for (index = 0; index < op->count; index++)
		{
			slots[total++] = &op->a[index];
			if (b)
				slots[total++] = &op->b[index];
			if (c)
				slots[total++] = &op->c[index];
		}
	}
	else
	{
		for (index = 0; index < a; index++)
			slots[total++] = &op->a[index];
		for (index = 0; index < b; index++)
			slots[total++] = &op->b[index];
	}
	*count = total;
}

static struct vsh_variant *variant_new(const struct nv2a_vsh_cpu_program *program, nv2a_vsh_output_mask mask,
	unsigned long flags)
{
	struct builder builder;
	struct vsh_variant *variant = calloc(1, sizeof(*variant));
	unsigned char *live = NULL;
	unsigned short *renumber = NULL;
	unsigned long index, component, slot, slot_count, total, used_constants;
	struct micro_op *initial;
	unsigned long initial_count = 0;

	memset(&builder, 0, sizeof(builder));
	builder.flags = flags & VARIANT_FLAGS;
	if (!variant)
		return NULL;
	variant->mask = mask;
	variant->flags = builder.flags;

	for (index = 0; index < program->instruction_count && !builder.failed; index++)
		compile_instruction(&builder, &program->instructions[index]);
	slot_count = _slot_dynamic + builder.constant_count + builder.scratch_count;
	if (builder.failed || slot_count >= SLOT_SCRATCH_FLAG)
		goto failed;
	live = calloc(slot_count, 1);
	renumber = calloc(slot_count, sizeof(*renumber));
	/* room for the operations that give registers their initial values */
	initial = malloc((builder.op_count + TEMPORARY_COUNT + NV2A_VSH_OUTPUT_COUNT + 2) * sizeof(*initial));
	if (!live || !renumber || !initial)
	{
		free(initial);
		goto failed;
	}

	/* scratch slots get their numbers after the constants */
	for (index = 0; index < builder.op_count; index++)
	{
		unsigned short *slots[16];

		op_slots(&builder.ops[index], &total, slots);
		for (slot = 0; slot < total; slot++)
		{
			if (*slots[slot] & SLOT_SCRATCH_FLAG)
			{
				*slots[slot] = (unsigned short)(_slot_dynamic + builder.constant_count +
					(*slots[slot] & ~SLOT_SCRATCH_FLAG));
			}
		}
	}

	/* what the draw's arrays read */
	variant->captured = builder.captured;
	for (index = 0; index < NV2A_VSH_OUTPUT_COUNT; index++)
	{
		for (component = 0; component < 4; component++)
		{
			if (!((mask >> (index * 4)) & (8 >> component)))
				continue;
			if (index == _nv2a_vsh_output_position && variant->captured)
				live[_slot_capture + component] = 1;
			else
				live[_slot_outputs + index * 4 + component] = 1;
		}
	}
	remove_dead(builder.ops, &builder.op_count, live);

	/* Registers read before the program writes them hold their initial
	values: 0 for the temporaries and a0, (0, 0, 0, 1) for the outputs and
	1 for the fog. The slots keep the previous vertices' values otherwise. */
	for (index = 0; index < TEMPORARY_COUNT + NV2A_VSH_OUTPUT_COUNT + 1; index++)
	{
		struct micro_op *op = &initial[initial_count];
		unsigned long base = index < TEMPORARY_COUNT ? _slot_temporaries + index * 4 :
			_slot_outputs + (index - TEMPORARY_COUNT) * 4;

		memset(op, 0, sizeof(*op));
		op->op = _op_move;
		if (index == TEMPORARY_COUNT + NV2A_VSH_OUTPUT_COUNT)
		{
			if (live[_slot_address])
			{
				op->dst[op->count] = _slot_address;
				op->a[op->count++] = _slot_zero;
			}
		}
		else
		{
			for (component = 0; component < 4; component++)
			{
				if (!live[base + component])
					continue;
				op->dst[op->count] = (unsigned short)(base + component);
				op->a[op->count++] = (unsigned short)(index < TEMPORARY_COUNT ? _slot_zero :
					component == 3 || index - TEMPORARY_COUNT == _nv2a_vsh_output_fog ? _slot_one : _slot_zero);
			}
		}
		if (op->count)
			initial_count++;
	}
	memcpy(initial + initial_count, builder.ops, builder.op_count * sizeof(*initial));
	free(builder.ops);
	builder.ops = NULL;
	variant->ops = initial;
	variant->op_count = initial_count + builder.op_count;

	/* the inputs and constants still read, and the slots still used,
	numbered without gaps */
	for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
	{
		unsigned long components = 0;

		for (component = 0; component < 4; component++)
		{
			if (live[_slot_inputs + index * 4 + component])
				components |= 1UL << component;
		}
		if (components)
		{
			variant->inputs[variant->input_count].input = (unsigned char)index;
			variant->inputs[variant->input_count++].components = (unsigned char)components;
		}
	}
	for (index = 0; index < slot_count; index++)
		renumber[index] = (unsigned short)(index < _slot_dynamic ? index : 0xFFFF);
	slot_count = _slot_dynamic;
	used_constants = 0;
	for (index = 0; index < builder.constant_count; index++)
	{
		struct constant_slot *constant = &builder.constants[index];

		if (!live[constant->slot])
			continue;
		renumber[constant->slot] = (unsigned short)slot_count;
		constant->slot = (unsigned short)slot_count++;
		builder.constants[used_constants++] = *constant;
	}
	variant->constants = builder.constants;
	variant->constant_count = used_constants;
	builder.constants = NULL;
	for (index = 0; index < variant->op_count; index++)
	{
		unsigned short *slots[16];

		if (variant->ops[index].op == _op_gather)
			variant->relative_constants = TRUE;
		op_slots(&variant->ops[index], &total, slots);
		for (slot = 0; slot < total; slot++)
		{
			if (renumber[*slots[slot]] == 0xFFFF)
				renumber[*slots[slot]] = (unsigned short)slot_count++;
			*slots[slot] = renumber[*slots[slot]];
		}
	}
	/* (more slots than 16 bits of offset reach: no program of the game's
	comes near) */
	if (slot_count > MAXIMUM_SLOTS)
		goto failed;
	/* from here on the operations hold byte offsets (one multiplication
	less for every operand of every operation run) */
	for (index = 0; index < variant->op_count; index++)
	{
		unsigned short *slots[16];

		op_slots(&variant->ops[index], &total, slots);
		for (slot = 0; slot < total; slot++)
			*slots[slot] = (unsigned short)(*slots[slot] * SLOT_BYTES);
	}
	variant->slot_count = slot_count;
	variant->slot_memory = malloc(slot_count * SLOT_BYTES + SLOT_BYTES);
	if (!variant->slot_memory)
		goto failed;
	variant->slots = (void *)(((size_t)variant->slot_memory + SLOT_BYTES - 1) & ~(size_t)(SLOT_BYTES - 1));
	memset(variant->slots, 0, slot_count * SLOT_BYTES);
	free(live);
	free(renumber);
	return variant;

failed:
	free(live);
	free(renumber);
	free(builder.ops);
	free(builder.constants);
	variant_delete(variant);
	return NULL;
}

static struct vsh_variant *variant_get(struct nv2a_vsh_cpu_program *program, nv2a_vsh_output_mask mask,
	unsigned long flags)
{
	unsigned long index, oldest = 0;
	struct vsh_variant *variant;

	flags &= VARIANT_FLAGS;
	for (index = 0; index < program->variant_count; index++)
	{
		variant = program->variants[index];
		if (variant->mask == mask && variant->flags == flags)
		{
			variant->used = ++program->variant_clock;
			return variant;
		}
		if (variant->used < program->variants[oldest]->used)
			oldest = index;
	}
	variant = variant_new(program, mask, flags);
	if (!variant)
		return NULL;
	variant->used = ++program->variant_clock;
	if (program->variant_count < MAXIMUM_VARIANTS)
		program->variants[program->variant_count++] = variant;
	else
	{
		variant_delete(program->variants[oldest]);
		program->variants[oldest] = variant;
	}
	return variant;
}

/* what each output is computed from: the program reduced to it */
static void classify_dependencies(struct nv2a_vsh_cpu_program *program)
{
	unsigned long address, index;

	for (address = 0; address < NV2A_VSH_OUTPUT_COUNT; address++)
	{
		struct nv2a_vsh_output_class *output = &program->classification.outputs[address];
		struct vsh_variant *variant;

		if (!output->written)
			continue;
		variant = variant_new(program, NV2A_VSH_OUTPUT_MASK(address, output->written), 0);
		if (!variant)
			continue;
		for (index = 0; index < variant->input_count; index++)
			output->inputs |= (unsigned short)(1U << variant->inputs[index].input);
		for (index = 0; index < variant->constant_count; index++)
			output->constants[variant->constants[index].constant / 32] |= 1UL << (variant->constants[index].constant % 32);
		output->relative_constants = variant->relative_constants;
		variant_delete(variant);
	}
}

BOOL nv2a_vsh_cpu_cost(struct nv2a_vsh_cpu_program *program, nv2a_vsh_output_mask mask, unsigned long flags,
	struct nv2a_vsh_cpu_cost *cost)
{
	struct vsh_variant *variant = variant_get(program, mask, flags);
	unsigned long index, component;

	memset(cost, 0, sizeof(*cost));
	if (!variant)
		return FALSE;
	for (index = 0; index < variant->op_count; index++)
	{
		const struct micro_op *op = &variant->ops[index];
		unsigned long kind;

		switch (op->op)
		{
		case _op_move: case _op_negate: kind = _nv2a_vsh_cpu_cost_move; break;
		case _op_multiply: case _op_add: case _op_subtract: case _op_minimum: case _op_maximum:
			kind = _nv2a_vsh_cpu_cost_arithmetic; break;
		case _op_multiply_add: case _op_multiply_subtract: case _op_subtract_product:
			kind = _nv2a_vsh_cpu_cost_multiply_add; break;
		case _op_less: case _op_greater_equal: kind = _nv2a_vsh_cpu_cost_compare; break;
		case _op_dot3: kind = _nv2a_vsh_cpu_cost_dot3; break;
		case _op_dot_homogeneous: case _op_dot4: kind = _nv2a_vsh_cpu_cost_dot4; break;
		case _op_address: kind = _nv2a_vsh_cpu_cost_address; break;
		case _op_gather: kind = _nv2a_vsh_cpu_cost_gather; break;
		case _op_reciprocal: case _op_reciprocal_estimate: case _op_reciprocal_clamped:
		case _op_reciprocal_clamped_estimate: kind = _nv2a_vsh_cpu_cost_reciprocal; break;
		case _op_reciprocal_square_root: case _op_reciprocal_square_root_estimate:
			kind = _nv2a_vsh_cpu_cost_reciprocal_square_root; break;
		default: kind = _nv2a_vsh_cpu_cost_library; break;
		}
		cost->operations++;
		cost->operations_by_kind[kind]++;
		cost->results_by_kind[kind] += op->count;
	}
	for (index = 0; index < variant->input_count; index++)
	{
		for (component = 0; component < 4; component++)
		{
			if (variant->inputs[index].components & (1UL << component))
				cost->input_components++;
		}
	}
	cost->constant_components = variant->constant_count;
	cost->slots = variant->slot_count;
	return TRUE;
}

/* ---------- the draw */

nv2a_vsh_output_mask nv2a_vsh_cpu_draw_mask(const struct nv2a_vsh_cpu_draw *draw)
{
	nv2a_vsh_output_mask mask = 0;
	unsigned long index;

	if (draw->position.data)
		mask |= NV2A_VSH_OUTPUT_MASK(_nv2a_vsh_output_position, 15);
	for (index = 0; index < NV2A_VSH_CPU_TEXCOORDS; index++)
	{
		const struct nv2a_vsh_cpu_texcoord *texcoord = &draw->texcoords[index];

		if (texcoord->array.data && texcoord->source < NV2A_VSH_OUTPUT_COUNT)
		{
			mask |= NV2A_VSH_OUTPUT_MASK(texcoord->source,
				texcoord->components <= 2 ? 12 : texcoord->components == 3 ? 14 : 15);
		}
	}
	for (index = 0; index < NV2A_VSH_CPU_COLORS; index++)
	{
		const struct nv2a_vsh_cpu_color *color = &draw->colors[index];

		if (!color->array.data)
			continue;
		if (color->color_source < NV2A_VSH_OUTPUT_COUNT)
			mask |= NV2A_VSH_OUTPUT_MASK(color->color_source, 14);
		if (color->alpha_source < NV2A_VSH_OUTPUT_COUNT)
			mask |= NV2A_VSH_OUTPUT_MASK(color->alpha_source, 8 >> (color->alpha_component & 3));
	}
	if (draw->fog.data)
		mask |= NV2A_VSH_OUTPUT_MASK(_nv2a_vsh_output_fog, 8);
	return mask;
}

/* how many components an element type holds; the rest are 0, 0 and 1 */
static unsigned long type_components(unsigned long type)
{
	switch (type)
	{
	case D3DVSDT_FLOAT1: case D3DVSDT_SHORT1: case D3DVSDT_NORMSHORT1: case D3DVSDT_PBYTE1: return 1;
	case D3DVSDT_FLOAT2: case D3DVSDT_SHORT2: case D3DVSDT_NORMSHORT2: case D3DVSDT_PBYTE2: return 2;
	case D3DVSDT_FLOAT3: case D3DVSDT_FLOAT2H: case D3DVSDT_SHORT3: case D3DVSDT_NORMSHORT3:
	case D3DVSDT_PBYTE3: case D3DVSDT_NORMPACKED3: return 3;
	default: return 4;
	}
}

/* The integer formats as OpenGL 4.5 turns them into floats (d3d8_gl.c's
attribute_format, and unpack_normpacked3 in nv2a_vsh.c): a quotient,
rounded to single precision. A double's product with the reciprocal rounds
to the same single as the quotient does for integers this small (the
quotient is never closer to the middle of two singles than one part in
2^40, the product's error is one part in 2^53), and costs a multiplication
on the x87 instead of a division. */
static float normalized_short(int value)
{
	float result = (float)((double)value * (1.0 / 32767.0));

	return result < -1.0f ? -1.0f : result;
}

static float packed_11(int value)
{
	return (float)((double)value * (1.0 / 1023.0));
}

static float packed_10(int value)
{
	return (float)((double)value * (1.0 / 511.0));
}

/* one element of one vertex */
static void read_element(unsigned long type, const unsigned char *data, float value[4])
{
	unsigned long index, count = type_components(type);
	const float *floats = (const float *)data;
	const short *shorts = (const short *)data;

	value[0] = value[1] = value[2] = 0.0f;
	value[3] = 1.0f;
	switch (type)
	{
	case D3DVSDT_D3DCOLOR:
		/* blue, green, red, alpha in memory */
		value[0] = byte_fractions[data[2]];
		value[1] = byte_fractions[data[1]];
		value[2] = byte_fractions[data[0]];
		value[3] = byte_fractions[data[3]];
		break;
	case D3DVSDT_SHORT1: case D3DVSDT_SHORT2: case D3DVSDT_SHORT3: case D3DVSDT_SHORT4:
		for (index = 0; index < count; index++)
			value[index] = (float)shorts[index];
		break;
	case D3DVSDT_NORMSHORT1: case D3DVSDT_NORMSHORT2: case D3DVSDT_NORMSHORT3: case D3DVSDT_NORMSHORT4:
		for (index = 0; index < count; index++)
			value[index] = normalized_short(shorts[index]);
		break;
	case D3DVSDT_NORMPACKED3:
	{
		unsigned int packed = *(const unsigned int *)data;

		value[0] = packed_11((int)(packed << 21) >> 21);
		value[1] = packed_11((int)(packed << 10) >> 21);
		value[2] = packed_10((int)packed >> 22);
		break;
	}
	case D3DVSDT_PBYTE1: case D3DVSDT_PBYTE2: case D3DVSDT_PBYTE3: case D3DVSDT_PBYTE4:
		for (index = 0; index < count; index++)
			value[index] = byte_fractions[data[index]];
		break;
	default:
		/* the float formats, and four floats for anything unknown */
		for (index = 0; index < count; index++)
			value[index] = floats[index];
		break;
	}
}

/* an input the vertices are read for, group by group */
struct stream_input
{
	const unsigned char *data;
	unsigned long stride;
	unsigned long type;
	unsigned long slot;
	/* the components read from the stream (the type has them and the
	program reads them) */
	unsigned long components;
};

/* the position's arithmetic, the same for every vertex of a draw */
struct position_terms
{
	BOOL captured;
	BOOL exact;
	BOOL flip_y;
	BOOL signed_depth;
	/* screen: (position + bias - offset) / scale * w */
	float bias[3];
	float offset[3];
	float scale[3];
	float reciprocal[3];
	/* captured: (clip * multiplier + addend * w) / scale */
	float multiplier[3];
	float addend[3];
};

static void position_terms_prepare(const struct vsh_variant *variant, const struct nv2a_vsh_cpu_draw *draw,
	struct position_terms *terms)
{
	unsigned long index;

	memset(terms, 0, sizeof(*terms));
	terms->captured = variant->captured;
	terms->exact = (draw->flags & NV2A_VSH_CPU_EXACT) != 0;
	terms->flip_y = (draw->flags & NV2A_VSH_CPU_FLIP_Y) != 0;
	terms->signed_depth = (draw->flags & NV2A_VSH_CPU_SIGNED_DEPTH) != 0;
	/* Direct3D 8 puts pixel centres on integer screen coordinates,
	OpenGL on half-integers (nv2a_vsh.c) */
	terms->bias[0] = 0.5f + draw->screen_offset;
	terms->bias[1] = 0.5f;
	terms->bias[2] = 0.0f;
	for (index = 0; index < 3; index++)
	{
		terms->offset[index] = draw->viewport_offset[index];
		terms->scale[index] = draw->viewport_scale[index] != 0.0f ? draw->viewport_scale[index] : 1.0f;
		terms->reciprocal[index] = 1.0f / terms->scale[index];
		terms->multiplier[index] = draw->constants[CONSTANT_VIEWPORT_SCALE][index];
		terms->addend[index] = (draw->constants[CONSTANT_VIEWPORT_OFFSET][index] + terms->bias[index]) -
			terms->offset[index];
	}
}

/* A colour component from 0 to 1 as the nearest of 256 levels (a tie goes
to the even one): adding 2^23 leaves a float no room for a fraction, so
the sum is rounded to a whole number, in the rounding mode the evaluators
set. The SSE evaluator has no other way to round four numbers at once. */
#define COLOR_ROUNDING 8388608.0f

static unsigned long color_byte(float value)
{
	return (unsigned long)(int)((value * 255.0f + COLOR_ROUNDING) - COLOR_ROUNDING);
}

/* ---------- the scalar evaluator */

union scalar_slot
{
	float f;
	int i;
};

/* The slots are SLOT_BYTES apart in both evaluators. An operation names a
slot by its byte offset (S, SCALAR_SLOT), everything else by its number
(SN, SCALAR_SLOT_NUMBER). */
#define SCALAR_SLOT(slots, offset) (*(union scalar_slot *)((unsigned char *)(slots) + (offset)))
#define SCALAR_SLOT_NUMBER(slots, number) SCALAR_SLOT(slots, (number) * SLOT_BYTES)
#define S(offset) SCALAR_SLOT(slots, offset).f
#define SN(number) SCALAR_SLOT_NUMBER(slots, number).f

static void execute_scalar(const struct vsh_variant *variant, const float (*constants)[4])
{
	void *slots = variant->slots;
	const struct micro_op *op = variant->ops, *end = op + variant->op_count;
	unsigned long index;
	float result[4];

	for (; op < end; op++)
	{
		switch (op->op)
		{
		case _op_move:
			for (index = 0; index < op->count; index++)
				SCALAR_SLOT(slots, op->dst[index]) = SCALAR_SLOT(slots, op->a[index]);
			break;
		case _op_negate:
			for (index = 0; index < op->count; index++)
				S(op->dst[index]) = -S(op->a[index]);
			break;
		case _op_multiply:
			for (index = 0; index < op->count; index++)
				S(op->dst[index]) = S(op->a[index]) * S(op->b[index]);
			break;
		case _op_add:
			for (index = 0; index < op->count; index++)
				S(op->dst[index]) = S(op->a[index]) + S(op->b[index]);
			break;
		case _op_subtract:
			for (index = 0; index < op->count; index++)
				S(op->dst[index]) = S(op->a[index]) - S(op->b[index]);
			break;
		case _op_multiply_add:
			for (index = 0; index < op->count; index++)
				S(op->dst[index]) = S(op->a[index]) * S(op->b[index]) + S(op->c[index]);
			break;
		case _op_multiply_subtract:
			for (index = 0; index < op->count; index++)
				S(op->dst[index]) = S(op->a[index]) * S(op->b[index]) - S(op->c[index]);
			break;
		case _op_subtract_product:
			for (index = 0; index < op->count; index++)
				S(op->dst[index]) = S(op->c[index]) - S(op->a[index]) * S(op->b[index]);
			break;
		case _op_minimum:
			for (index = 0; index < op->count; index++)
				S(op->dst[index]) = SCALAR_MIN(S(op->a[index]), S(op->b[index]));
			break;
		case _op_maximum:
			for (index = 0; index < op->count; index++)
				S(op->dst[index]) = SCALAR_MAX(S(op->a[index]), S(op->b[index]));
			break;
		case _op_less:
			for (index = 0; index < op->count; index++)
				S(op->dst[index]) = S(op->a[index]) < S(op->b[index]) ? 1.0f : 0.0f;
			break;
		case _op_greater_equal:
			for (index = 0; index < op->count; index++)
				S(op->dst[index]) = S(op->a[index]) >= S(op->b[index]) ? 1.0f : 0.0f;
			break;
		case _op_dot3:
			S(op->dst[0]) = S(op->a[0]) * S(op->b[0]) + S(op->a[1]) * S(op->b[1]) + S(op->a[2]) * S(op->b[2]);
			break;
		case _op_dot_homogeneous:
			S(op->dst[0]) = S(op->a[0]) * S(op->b[0]) + S(op->a[1]) * S(op->b[1]) + S(op->a[2]) * S(op->b[2]) +
				S(op->b[3]);
			break;
		case _op_dot4:
			S(op->dst[0]) = S(op->a[0]) * S(op->b[0]) + S(op->a[1]) * S(op->b[1]) + S(op->a[2]) * S(op->b[2]) +
				S(op->a[3]) * S(op->b[3]);
			break;
		case _op_address:
			SCALAR_SLOT(slots, op->dst[0]).i = scalar_floor_to_int(S(op->a[0]) + ARL_BIAS);
			break;
		case _op_gather:
		{
			/* (the sum wraps as GLSL's does: a0 can be within 255 of the
			largest int) */
			int number = (int)((unsigned int)SCALAR_SLOT(slots, op->a[0]).i + op->c[0]);

			number = number < 0 ? 0 : number > XGPU_VERTEX_CONSTANT_COUNT - 1 ? XGPU_VERTEX_CONSTANT_COUNT - 1 : number;
			for (index = 0; index < op->count; index++)
				S(op->dst[index]) = constants[number][op->b[index]];
			break;
		}
		case _op_reciprocal:
			S(op->dst[0]) = 1.0f / S(op->a[0]);
			break;
		case _op_reciprocal_estimate:
			S(op->dst[0]) = scalar_reciprocal_estimate(S(op->a[0]));
			break;
		case _op_reciprocal_clamped:
			S(op->dst[0]) = scalar_rcc(S(op->a[0]));
			break;
		case _op_reciprocal_clamped_estimate:
			S(op->dst[0]) = scalar_reciprocal_clamp(scalar_reciprocal_estimate(S(op->a[0])));
			break;
		case _op_reciprocal_square_root:
			S(op->dst[0]) = scalar_rsq(S(op->a[0]));
			break;
		case _op_reciprocal_square_root_estimate:
			S(op->dst[0]) = scalar_reciprocal_square_root_estimate(S(op->a[0]));
			break;
		case _op_exponent:
		{
			unsigned long components = 0;

			for (index = 0; index < op->count; index++)
				components |= 1UL << op->b[index];
			scalar_exp(S(op->a[0]), components, result);
			for (index = 0; index < op->count; index++)
				S(op->dst[index]) = result[op->b[index]];
			break;
		}
		case _op_logarithm:
			scalar_log(S(op->a[0]), result);
			for (index = 0; index < op->count; index++)
				S(op->dst[index]) = result[op->b[index]];
			break;
		case _op_light:
			scalar_lit(S(op->a[0]), S(op->a[1]), S(op->a[2]), result);
			for (index = 0; index < op->count; index++)
				S(op->dst[index]) = result[op->b[index]];
			break;
		default:
			break;
		}
	}
}

static float clamp_unit(float value)
{
	/* clamp(x, 0.0, 1.0), with the comparisons turned so that what is not
	a number becomes 0 */
	value = value > 0.0f ? value : 0.0f;
	return value < 1.0f ? value : 1.0f;
}

static void store_scalar(const struct vsh_variant *variant, const struct nv2a_vsh_cpu_draw *draw,
	const struct position_terms *terms, unsigned long vertex)
{
	void *slots = variant->slots;
	unsigned long index, component;

	if (draw->position.data)
	{
		float *out = (float *)((unsigned char *)draw->position.data + vertex * draw->position.stride);
		float position[4];

		if (terms->captured)
		{
			float w = SN(_slot_capture + 3);

			for (index = 0; index < 3; index++)
			{
				float sum = SN(_slot_capture + index) * terms->multiplier[index] + terms->addend[index] * w;

				position[index] = terms->exact ? sum / terms->scale[index] : sum * terms->reciprocal[index];
			}
			position[3] = w;
		}
		else
		{
			float w = SN(_slot_outputs + 3);

			for (index = 0; index < 3; index++)
			{
				/* the half pixel is added to x and y only */
				float screen = SN(_slot_outputs + index);

				if (index < 2)
					screen = screen + terms->bias[index];
				screen = screen - terms->offset[index];

				screen = terms->exact ? screen / terms->scale[index] : screen * terms->reciprocal[index];
				position[index] = screen * w;
			}
			position[3] = w;
		}
		if (terms->flip_y)
			position[1] = -position[1];
		if (terms->signed_depth)
			position[2] = 2.0f * position[2] - position[3];
		memcpy(out, position, sizeof(position));
	}
	for (index = 0; index < NV2A_VSH_CPU_TEXCOORDS; index++)
	{
		const struct nv2a_vsh_cpu_texcoord *texcoord = &draw->texcoords[index];
		float *out = (float *)((unsigned char *)texcoord->array.data + vertex * texcoord->array.stride);
		unsigned long count = texcoord->components <= 2 ? 2 : texcoord->components == 3 ? 3 : 4;

		if (!texcoord->array.data || texcoord->source >= NV2A_VSH_OUTPUT_COUNT)
			continue;
		for (component = 0; component < count; component++)
			out[component] = SN(_slot_outputs + texcoord->source * 4 + component);
	}
	for (index = 0; index < NV2A_VSH_CPU_COLORS; index++)
	{
		const struct nv2a_vsh_cpu_color *array = &draw->colors[index];
		unsigned char *out = (unsigned char *)array->array.data + vertex * array->array.stride;
		float color[4];

		if (!array->array.data)
			continue;
		for (component = 0; component < 4; component++)
		{
			unsigned long source = component < 3 ? array->color_source : array->alpha_source;
			unsigned long from = component < 3 ? component : (array->alpha_component & 3UL);

			color[component] = source < NV2A_VSH_OUTPUT_COUNT ? clamp_unit(SN(_slot_outputs + source * 4 + from)) :
				component < 3 ? 0.0f : 1.0f;
			if (array->color_scale)
				color[component] = clamp_unit(color[component] * array->color_scale[component]);
			out[component] = (unsigned char)color_byte(color[component]);
		}
	}
	if (draw->fog.data)
		*(float *)((unsigned char *)draw->fog.data + vertex * draw->fog.stride) = SN(_slot_outputs + _nv2a_vsh_output_fog * 4);
}

#undef S
#undef SN

/* ---------- the floating-point state

Round to nearest, exceptions masked, results too small for a normal number
flushed to zero (a Pentium III takes a microcode trap for each denormal
result otherwise). The caller's state comes back afterwards: the game's
simulation shares the thread and its arithmetic must not change. */

#ifdef __SSE__
#define MXCSR_EVALUATOR 0x9F80

/* A processor without SSE2 computes doubles on the x87, and the integer
formats are converted through one (normalized_short): the x87 rounds to
nearest with its exceptions masked as well, or the conversions would be a
unit in the last place off for a caller that rounds another way, and
computes to a double's precision, which a 32-bit C library's floor, exp2,
log2 and pow count on (with SSE2 as without). That is the state a Windows
program starts in and the game keeps (real_math.c), so the usual caller
costs no load of the control word. */
#if defined(__i386__)
#define X87_EVALUATOR(control) (((control) & ~0x0F00) | 0x023F)
#define X87_STATE_ENTER() \
	unsigned short saved_x87, evaluator_x87; \
	__asm__ volatile ("fnstcw %0" : "=m" (saved_x87)); \
	evaluator_x87 = (unsigned short)X87_EVALUATOR(saved_x87); \
	if (evaluator_x87 != saved_x87) \
		__asm__ volatile ("fldcw %0" : : "m" (evaluator_x87))
/* (the exceptions noted meanwhile are dropped first: one the caller has
not masked would be raised by the caller's next x87 instruction) */
#define X87_STATE_LEAVE() \
	if (evaluator_x87 != saved_x87) \
		__asm__ volatile ("fnclex; fldcw %0" : : "m" (saved_x87))
#else
#define X87_STATE_ENTER() (void)0
#define X87_STATE_LEAVE() (void)0
#endif

#define FLOAT_STATE_ENTER() unsigned int saved_mxcsr = _mm_getcsr(); X87_STATE_ENTER(); _mm_setcsr(MXCSR_EVALUATOR)
/* (never DAZ, bit 6: a Pentium III without it faults on the load, and
tools/win9x_check.py wants every load shown to clear it, here by the
instruction before; a caller that had it set gets it back clear) */
#define FLOAT_STATE_LEAVE() \
	do \
	{ \
		X87_STATE_LEAVE(); \
		__asm__ volatile ("andl $-0x41, %0; ldmxcsr %0" : "+m" (saved_mxcsr)); \
	} while (0)
#else
#define FLOAT_STATE_ENTER() (void)0
#define FLOAT_STATE_LEAVE() (void)0
#endif

/* (in the evaluators' state: the first program may be made while the
caller rounds another way, and the quotients must be the nearest floats) */
static void byte_fractions_prepare(void)
{
	unsigned long index;

	if (byte_fractions[255] == 1.0f)
		return;
	{
		FLOAT_STATE_ENTER();

		for (index = 0; index < 256; index++)
			byte_fractions[index] = (float)index / 255.0f;
		FLOAT_STATE_LEAVE();
	}
}

/* the slots both evaluators fill once a run: 0 and 1, the constants, the
inputs without a stream; returns the inputs with one */
static unsigned long prepare_inputs(const struct vsh_variant *variant, const struct nv2a_vsh_cpu_draw *draw,
	unsigned long first, float fixed[XGPU_VERTEX_ATTRIBUTE_COUNT][4], unsigned long fixed_components[XGPU_VERTEX_ATTRIBUTE_COUNT],
	struct stream_input *streams)
{
	unsigned long index, count = 0;

	for (index = 0; index < variant->input_count; index++)
	{
		unsigned long input = variant->inputs[index].input;
		unsigned long needed = variant->inputs[index].components;
		const struct nv2a_vsh_cpu_input *source = &draw->inputs[input];

		fixed[input][0] = fixed[input][1] = fixed[input][2] = 0.0f;
		fixed[input][3] = 1.0f;
		fixed_components[input] = needed;
		if (!source->data)
			memcpy(fixed[input], source->value, sizeof(fixed[input]));
		else if (!source->stride)
			read_element(source->type, source->data, fixed[input]);
		else
		{
			unsigned long present = (1UL << type_components(source->type)) - 1;

			/* components the type lacks stay 0, 0, 1 */
			fixed_components[input] = needed & ~present;
			if (needed & present)
			{
				streams[count].data = (const unsigned char *)source->data + first * source->stride;
				streams[count].stride = source->stride;
				streams[count].type = source->type;
				streams[count].slot = _slot_inputs + input * 4;
				streams[count++].components = needed & present;
			}
		}
	}
	return count;
}

BOOL nv2a_vsh_cpu_run_scalar(struct nv2a_vsh_cpu_program *program, const struct nv2a_vsh_cpu_draw *draw,
	unsigned long first, unsigned long count)
{
	struct vsh_variant *variant = variant_get(program, nv2a_vsh_cpu_draw_mask(draw), draw->flags);
	struct stream_input streams[XGPU_VERTEX_ATTRIBUTE_COUNT];
	float fixed[XGPU_VERTEX_ATTRIBUTE_COUNT][4];
	unsigned long fixed_components[XGPU_VERTEX_ATTRIBUTE_COUNT];
	struct position_terms terms;
	unsigned long stream_count, index, component, vertex;
	void *slots;

	if (!variant)
		return FALSE;
	if (!count)
		return TRUE;
	{
		FLOAT_STATE_ENTER();

		slots = variant->slots;
		position_terms_prepare(variant, draw, &terms);
		SCALAR_SLOT_NUMBER(slots, _slot_zero).f = 0.0f;
		SCALAR_SLOT_NUMBER(slots, _slot_one).f = 1.0f;
		for (index = 0; index < variant->constant_count; index++)
		{
			const struct constant_slot *constant = &variant->constants[index];
			float value = draw->constants[constant->constant][constant->component];

			SCALAR_SLOT_NUMBER(slots, constant->slot).f = constant->negate ? -value : value;
		}
		stream_count = prepare_inputs(variant, draw, first, fixed, fixed_components, streams);
		for (index = 0; index < variant->input_count; index++)
		{
			unsigned long input = variant->inputs[index].input;

			for (component = 0; component < 4; component++)
			{
				if (fixed_components[input] & (1UL << component))
					SCALAR_SLOT_NUMBER(slots, _slot_inputs + input * 4 + component).f = fixed[input][component];
			}
		}
		for (vertex = 0; vertex < count; vertex++)
		{
			for (index = 0; index < stream_count; index++)
			{
				const struct stream_input *stream = &streams[index];
				float value[4];

				read_element(stream->type, stream->data + vertex * stream->stride, value);
				for (component = 0; component < 4; component++)
				{
					if (stream->components & (1UL << component))
						SCALAR_SLOT_NUMBER(slots, stream->slot + component).f = value[component];
				}
			}
			execute_scalar(variant, draw->constants);
			store_scalar(variant, draw, &terms, vertex);
		}
		FLOAT_STATE_LEAVE();
	}
	return TRUE;
}

/* ---------- the SSE evaluator */

#ifdef __SSE__

/* registers of four vertices in a slot (SLOT_BYTES) */
#define VECTOR_GROUPS 4

union vector_slot
{
	__m128 v;
	float f[4];
	int i[4];
};

/* The packed compares and the sign mask. For a processor without SSE2
clang has no type for a compare's result and turns these intrinsics into
scalar code through memory, so there the instructions are written out. */
#if defined(__SSE2__) || !(defined(__GNUC__) || defined(__clang__))
#define vector_less(a, b) _mm_cmplt_ps(a, b)
#define vector_less_equal(a, b) _mm_cmple_ps(a, b)
#define vector_equal(a, b) _mm_cmpeq_ps(a, b)
#define vector_sign_mask(a) _mm_movemask_ps(a)
#else
static inline __m128 vector_less(__m128 a, __m128 b)
{
	__asm__("cmpltps %1, %0" : "+x"(a) : "x"(b));
	return a;
}

static inline __m128 vector_less_equal(__m128 a, __m128 b)
{
	__asm__("cmpleps %1, %0" : "+x"(a) : "x"(b));
	return a;
}

static inline __m128 vector_equal(__m128 a, __m128 b)
{
	__asm__("cmpeqps %1, %0" : "+x"(a) : "x"(b));
	return a;
}

static inline int vector_sign_mask(__m128 a)
{
	int mask;

	__asm__("movmskps %1, %0" : "=r"(mask) : "x"(a));
	return mask;
}
#endif

static __m128 vector_select(__m128 mask, __m128 yes, __m128 no)
{
	return _mm_or_ps(_mm_and_ps(mask, yes), _mm_andnot_ps(mask, no));
}

static __m128 vector_bits(unsigned int bits)
{
	union float_bits value;

	value.u = bits;
	return _mm_set1_ps(value.f);
}

/* The processor's estimate of 1 / x, made nearly exact by one Newton
step: r' = r * (2 - x * r). Where the estimate is zero or infinite (x
infinite, zero, denormal or huge) the step has nothing to work with, and
the elements are divided instead; that is rare enough to cost nothing. */
static __m128 vector_reciprocal_estimate(__m128 x)
{
	__m128 estimate = _mm_rcp_ps(x);
	__m128 refined = _mm_mul_ps(estimate, _mm_sub_ps(_mm_set1_ps(2.0f), _mm_mul_ps(x, estimate)));
	__m128 magnitude = _mm_and_ps(estimate, vector_bits(0x7FFFFFFF));
	__m128 useless = _mm_or_ps(vector_equal(magnitude, vector_bits(0x7F800000)), vector_equal(estimate, _mm_setzero_ps()));

	if (vector_sign_mask(useless))
		refined = vector_select(useless, _mm_div_ps(_mm_set1_ps(1.0f), x), refined);
	return refined;
}

/* the same for 1 / sqrt(x), x not negative: r' = 0.5 * r * (3 - x * r * r) */
static __m128 vector_reciprocal_square_root_estimate(__m128 x)
{
	__m128 estimate = _mm_rsqrt_ps(x);
	__m128 refined = _mm_mul_ps(_mm_mul_ps(_mm_set1_ps(0.5f), estimate),
		_mm_sub_ps(_mm_set1_ps(3.0f), _mm_mul_ps(_mm_mul_ps(x, estimate), estimate)));
	__m128 useless = _mm_or_ps(vector_equal(estimate, vector_bits(0x7F800000)), vector_equal(estimate, _mm_setzero_ps()));

	if (vector_sign_mask(useless))
		refined = vector_select(useless, _mm_div_ps(_mm_set1_ps(1.0f), _mm_sqrt_ps(x)), refined);
	return refined;
}

static __m128 vector_reciprocal_clamp(__m128 r)
{
	__m128 positive = vector_less(_mm_setzero_ps(), r);
	__m128 above = _mm_min_ps(_mm_set1_ps(RCC_LARGEST), _mm_max_ps(_mm_set1_ps(RCC_SMALLEST), r));
	__m128 below = _mm_min_ps(_mm_set1_ps(-RCC_SMALLEST), _mm_max_ps(_mm_set1_ps(-RCC_LARGEST), r));

	return vector_select(positive, above, below);
}

/* int(floor(x)) of the lowest element, as scalar_floor_to_int */
static int vector_floor_to_int(__m128 x)
{
	int whole = _mm_cvttss_si32(x);

	/* the conversion's answer for what no int holds is the smallest int */
	if (whole != (int)0x80000000 && _mm_comilt_ss(x, _mm_cvtsi32_ss(x, whole)))
		whole--;
	return whole;
}

/* A slot holds VECTOR_GROUPS registers of four vertices each. An operation
is done for every group before the next operation is looked at, so that
finding the operation's code (a jump the processor cannot predict) is paid
once for sixteen vertices. V is the first register of the slot at a byte
offset (as S of the scalar evaluator), VN that of a slot number. */
#define V(offset) ((__m128 *)((unsigned char *)slots + (offset)))
#define VN(number) (*V((number) * SLOT_BYTES))

#define FOR_EACH_GROUP(statement) \
	do \
	{ \
		for (group = 0; group < groups; group++) \
		{ \
			statement; \
		} \
	} while (0)

static void execute_vector(const struct vsh_variant *variant, const float (*constants)[4], unsigned long groups)
{
	unsigned char *slots = variant->slots;
	const struct micro_op *op = variant->ops, *end = op + variant->op_count;
	unsigned long index, lane, group, count;
	const __m128 one = _mm_set1_ps(1.0f);
	const __m128 magnitude = vector_bits(0x7FFFFFFF);
	__m128 *dst;
	const __m128 *a, *b, *c;

	for (; op < end; op++)
	{
		count = op->count;
		switch (op->op)
		{
		case _op_move:
			for (index = 0; index < count; index++)
			{
				dst = V(op->dst[index]);
				a = V(op->a[index]);
				FOR_EACH_GROUP(dst[group] = a[group]);
			}
			break;
		case _op_negate:
		{
			__m128 sign = vector_bits(0x80000000);

			for (index = 0; index < count; index++)
			{
				dst = V(op->dst[index]);
				a = V(op->a[index]);
				FOR_EACH_GROUP(dst[group] = _mm_xor_ps(a[group], sign));
			}
			break;
		}
		case _op_multiply:
			for (index = 0; index < count; index++)
			{
				dst = V(op->dst[index]);
				a = V(op->a[index]);
				b = V(op->b[index]);
				FOR_EACH_GROUP(dst[group] = _mm_mul_ps(a[group], b[group]));
			}
			break;
		case _op_add:
			for (index = 0; index < count; index++)
			{
				dst = V(op->dst[index]);
				a = V(op->a[index]);
				b = V(op->b[index]);
				FOR_EACH_GROUP(dst[group] = _mm_add_ps(a[group], b[group]));
			}
			break;
		case _op_subtract:
			for (index = 0; index < count; index++)
			{
				dst = V(op->dst[index]);
				a = V(op->a[index]);
				b = V(op->b[index]);
				FOR_EACH_GROUP(dst[group] = _mm_sub_ps(a[group], b[group]));
			}
			break;
		case _op_multiply_add:
			for (index = 0; index < count; index++)
			{
				dst = V(op->dst[index]);
				a = V(op->a[index]);
				b = V(op->b[index]);
				c = V(op->c[index]);
				FOR_EACH_GROUP(dst[group] = _mm_add_ps(_mm_mul_ps(a[group], b[group]), c[group]));
			}
			break;
		case _op_multiply_subtract:
			for (index = 0; index < count; index++)
			{
				dst = V(op->dst[index]);
				a = V(op->a[index]);
				b = V(op->b[index]);
				c = V(op->c[index]);
				FOR_EACH_GROUP(dst[group] = _mm_sub_ps(_mm_mul_ps(a[group], b[group]), c[group]));
			}
			break;
		case _op_subtract_product:
			for (index = 0; index < count; index++)
			{
				dst = V(op->dst[index]);
				a = V(op->a[index]);
				b = V(op->b[index]);
				c = V(op->c[index]);
				FOR_EACH_GROUP(dst[group] = _mm_sub_ps(c[group], _mm_mul_ps(a[group], b[group])));
			}
			break;
		case _op_minimum:
			/* MINPS gives its second operand unless the first is smaller */
			for (index = 0; index < count; index++)
			{
				dst = V(op->dst[index]);
				a = V(op->a[index]);
				b = V(op->b[index]);
				FOR_EACH_GROUP(dst[group] = _mm_min_ps(b[group], a[group]));
			}
			break;
		case _op_maximum:
			for (index = 0; index < count; index++)
			{
				dst = V(op->dst[index]);
				a = V(op->a[index]);
				b = V(op->b[index]);
				FOR_EACH_GROUP(dst[group] = _mm_max_ps(b[group], a[group]));
			}
			break;
		case _op_less:
			for (index = 0; index < count; index++)
			{
				dst = V(op->dst[index]);
				a = V(op->a[index]);
				b = V(op->b[index]);
				FOR_EACH_GROUP(dst[group] = _mm_and_ps(vector_less(a[group], b[group]), one));
			}
			break;
		case _op_greater_equal:
			for (index = 0; index < count; index++)
			{
				dst = V(op->dst[index]);
				a = V(op->a[index]);
				b = V(op->b[index]);
				FOR_EACH_GROUP(dst[group] = _mm_and_ps(vector_less_equal(b[group], a[group]), one));
			}
			break;
		case _op_dot3:
		{
			const __m128 *a0 = V(op->a[0]), *a1 = V(op->a[1]), *a2 = V(op->a[2]);
			const __m128 *b0 = V(op->b[0]), *b1 = V(op->b[1]), *b2 = V(op->b[2]);

			dst = V(op->dst[0]);
			FOR_EACH_GROUP(dst[group] = _mm_add_ps(_mm_add_ps(_mm_mul_ps(a0[group], b0[group]),
				_mm_mul_ps(a1[group], b1[group])), _mm_mul_ps(a2[group], b2[group])));
			break;
		}
		case _op_dot_homogeneous:
		{
			const __m128 *a0 = V(op->a[0]), *a1 = V(op->a[1]), *a2 = V(op->a[2]);
			const __m128 *b0 = V(op->b[0]), *b1 = V(op->b[1]), *b2 = V(op->b[2]), *b3 = V(op->b[3]);

			dst = V(op->dst[0]);
			FOR_EACH_GROUP(dst[group] = _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(a0[group], b0[group]),
				_mm_mul_ps(a1[group], b1[group])), _mm_mul_ps(a2[group], b2[group])), b3[group]));
			break;
		}
		case _op_dot4:
		{
			const __m128 *a0 = V(op->a[0]), *a1 = V(op->a[1]), *a2 = V(op->a[2]), *a3 = V(op->a[3]);
			const __m128 *b0 = V(op->b[0]), *b1 = V(op->b[1]), *b2 = V(op->b[2]), *b3 = V(op->b[3]);

			dst = V(op->dst[0]);
			FOR_EACH_GROUP(dst[group] = _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(a0[group], b0[group]),
				_mm_mul_ps(a1[group], b1[group])), _mm_mul_ps(a2[group], b2[group])),
				_mm_mul_ps(a3[group], b3[group])));
			break;
		}
		case _op_address:
		{
			__m128 bias = _mm_set1_ps(ARL_BIAS);

			a = V(op->a[0]);
			for (group = 0; group < groups; group++)
			{
				__m128 biased = _mm_add_ps(a[group], bias);
				int *address = (int *)(V(op->dst[0]) + group);

				address[0] = vector_floor_to_int(biased);
				address[1] = vector_floor_to_int(_mm_shuffle_ps(biased, biased, _MM_SHUFFLE(1, 1, 1, 1)));
				address[2] = vector_floor_to_int(_mm_shuffle_ps(biased, biased, _MM_SHUFFLE(2, 2, 2, 2)));
				address[3] = vector_floor_to_int(_mm_shuffle_ps(biased, biased, _MM_SHUFFLE(3, 3, 3, 3)));
			}
			break;
		}
		case _op_gather:
			for (group = 0; group < groups; group++)
			{
				const int *address = (const int *)(V(op->a[0]) + group);
				const float *rows[4];

				for (lane = 0; lane < 4; lane++)
				{
					int number = (int)((unsigned int)address[lane] + op->c[0]);

					number = number < 0 ? 0 : number > XGPU_VERTEX_CONSTANT_COUNT - 1 ? XGPU_VERTEX_CONSTANT_COUNT - 1 : number;
					rows[lane] = constants[number];
				}
				for (index = 0; index < count; index++)
				{
					unsigned long component = op->b[index];

					V(op->dst[index])[group] = _mm_movelh_ps(
						_mm_unpacklo_ps(_mm_load_ss(rows[0] + component), _mm_load_ss(rows[1] + component)),
						_mm_unpacklo_ps(_mm_load_ss(rows[2] + component), _mm_load_ss(rows[3] + component)));
				}
			}
			break;
		case _op_reciprocal:
			dst = V(op->dst[0]);
			a = V(op->a[0]);
			FOR_EACH_GROUP(dst[group] = _mm_div_ps(one, a[group]));
			break;
		case _op_reciprocal_estimate:
			dst = V(op->dst[0]);
			a = V(op->a[0]);
			FOR_EACH_GROUP(dst[group] = vector_reciprocal_estimate(a[group]));
			break;
		case _op_reciprocal_clamped:
			dst = V(op->dst[0]);
			a = V(op->a[0]);
			FOR_EACH_GROUP(dst[group] = vector_reciprocal_clamp(_mm_div_ps(one, a[group])));
			break;
		case _op_reciprocal_clamped_estimate:
			dst = V(op->dst[0]);
			a = V(op->a[0]);
			FOR_EACH_GROUP(dst[group] = vector_reciprocal_clamp(vector_reciprocal_estimate(a[group])));
			break;
		case _op_reciprocal_square_root:
			dst = V(op->dst[0]);
			a = V(op->a[0]);
			FOR_EACH_GROUP(dst[group] = _mm_div_ps(one, _mm_sqrt_ps(_mm_and_ps(a[group], magnitude))));
			break;
		case _op_reciprocal_square_root_estimate:
			dst = V(op->dst[0]);
			a = V(op->a[0]);
			FOR_EACH_GROUP(dst[group] = vector_reciprocal_square_root_estimate(_mm_and_ps(a[group], magnitude)));
			break;
		case _op_exponent: case _op_logarithm: case _op_light:
			/* the C library's functions, a vertex at a time */
		{
			unsigned long components = 0;

			for (index = 0; index < count; index++)
				components |= 1UL << op->b[index];
			for (group = 0; group < groups; group++)
			{
				union vector_slot x, y, w;
				float result[4][4];

				x.v = V(op->a[0])[group];
				y.v = w.v = x.v;
				if (op->op == _op_light)
				{
					y.v = V(op->a[1])[group];
					w.v = V(op->a[2])[group];
				}
				for (lane = 0; lane < 4; lane++)
				{
					if (op->op == _op_exponent)
						scalar_exp(x.f[lane], components, result[lane]);
					else if (op->op == _op_logarithm)
						scalar_log(x.f[lane], result[lane]);
					else
						scalar_lit(x.f[lane], y.f[lane], w.f[lane], result[lane]);
				}
				for (index = 0; index < count; index++)
				{
					unsigned long component = op->b[index];

					V(op->dst[index])[group] = _mm_setr_ps(result[0][component], result[1][component],
						result[2][component], result[3][component]);
				}
			}
			break;
		}
		default:
			break;
		}
	}
}

#undef V
#undef VN

/* From here on a group at a time: `slots` is the group's first register
of slot 0, and VN the group's register of a slot number. */
#define VN(number) (*(__m128 *)((unsigned char *)slots + (number) * SLOT_BYTES))

/* four floats of four vertices into one register, without a store that
the load would have to wait for */
static __m128 vector_gather(const float *a, const float *b, const float *c, const float *d)
{
	return _mm_movelh_ps(_mm_unpacklo_ps(_mm_load_ss(a), _mm_load_ss(b)),
		_mm_unpacklo_ps(_mm_load_ss(c), _mm_load_ss(d)));
}

/* two neighbouring floats of four vertices into two slots */
static void vector_load_pairs(const unsigned char *const lanes[4], unsigned long offset, __m128 *first, __m128 *second)
{
	__m128 low = _mm_loadh_pi(_mm_loadl_pi(_mm_setzero_ps(), (const __m64 *)(lanes[0] + offset)),
		(const __m64 *)(lanes[1] + offset));
	__m128 high = _mm_loadh_pi(_mm_loadl_pi(_mm_setzero_ps(), (const __m64 *)(lanes[2] + offset)),
		(const __m64 *)(lanes[3] + offset));

	*first = _mm_shuffle_ps(low, high, _MM_SHUFFLE(2, 0, 2, 0));
	*second = _mm_shuffle_ps(low, high, _MM_SHUFFLE(3, 1, 3, 1));
}

static void load_vector(unsigned char *slots, const struct stream_input *stream, const unsigned char *const lanes[4])
{
	unsigned long component, lane, components = stream->components;
	__m128 first, second;

	switch (stream->type)
	{
	case D3DVSDT_FLOAT1: case D3DVSDT_FLOAT2: case D3DVSDT_FLOAT3: case D3DVSDT_FLOAT4: case D3DVSDT_FLOAT2H:
		if ((components & 3) == 3)
		{
			vector_load_pairs(lanes, 0, &first, &second);
			VN(stream->slot) = first;
			VN(stream->slot + 1) = second;
			components &= ~3UL;
		}
		if ((components & 12) == 12)
		{
			vector_load_pairs(lanes, 8, &first, &second);
			VN(stream->slot + 2) = first;
			VN(stream->slot + 3) = second;
			components &= ~12UL;
		}
		for (component = 0; component < 4; component++)
		{
			if (components & (1UL << component))
			{
				VN(stream->slot + component) = vector_gather((const float *)lanes[0] + component,
					(const float *)lanes[1] + component, (const float *)lanes[2] + component,
					(const float *)lanes[3] + component);
			}
		}
		break;
	/* the integer formats as read_element converts them, only the
	components the program reads */
	case D3DVSDT_D3DCOLOR:
		for (component = 0; component < 4; component++)
		{
			/* blue, green, red, alpha in memory */
			unsigned long byte = component < 3 ? 2 - component : 3;

			if (components & (1UL << component))
			{
				VN(stream->slot + component) = vector_gather(&byte_fractions[lanes[0][byte]],
					&byte_fractions[lanes[1][byte]], &byte_fractions[lanes[2][byte]], &byte_fractions[lanes[3][byte]]);
			}
		}
		break;
	case D3DVSDT_PBYTE1: case D3DVSDT_PBYTE2: case D3DVSDT_PBYTE3: case D3DVSDT_PBYTE4:
		for (component = 0; component < 4; component++)
		{
			if (components & (1UL << component))
			{
				VN(stream->slot + component) = vector_gather(&byte_fractions[lanes[0][component]],
					&byte_fractions[lanes[1][component]], &byte_fractions[lanes[2][component]],
					&byte_fractions[lanes[3][component]]);
			}
		}
		break;
	case D3DVSDT_SHORT1: case D3DVSDT_SHORT2: case D3DVSDT_SHORT3: case D3DVSDT_SHORT4:
		for (component = 0; component < 4; component++)
		{
			if (components & (1UL << component))
			{
				VN(stream->slot + component) = _mm_setr_ps((float)((const short *)lanes[0])[component],
					(float)((const short *)lanes[1])[component], (float)((const short *)lanes[2])[component],
					(float)((const short *)lanes[3])[component]);
			}
		}
		break;
	case D3DVSDT_NORMSHORT1: case D3DVSDT_NORMSHORT2: case D3DVSDT_NORMSHORT3: case D3DVSDT_NORMSHORT4:
		for (component = 0; component < 4; component++)
		{
			if (components & (1UL << component))
			{
				VN(stream->slot + component) = _mm_setr_ps(normalized_short(((const short *)lanes[0])[component]),
					normalized_short(((const short *)lanes[1])[component]),
					normalized_short(((const short *)lanes[2])[component]),
					normalized_short(((const short *)lanes[3])[component]));
			}
		}
		break;
	case D3DVSDT_NORMPACKED3:
	{
		unsigned int packed[4];

		for (lane = 0; lane < 4; lane++)
			packed[lane] = *(const unsigned int *)lanes[lane];
		if (components & 1)
		{
			VN(stream->slot) = _mm_setr_ps(packed_11((int)(packed[0] << 21) >> 21), packed_11((int)(packed[1] << 21) >> 21),
				packed_11((int)(packed[2] << 21) >> 21), packed_11((int)(packed[3] << 21) >> 21));
		}
		if (components & 2)
		{
			VN(stream->slot + 1) = _mm_setr_ps(packed_11((int)(packed[0] << 10) >> 21), packed_11((int)(packed[1] << 10) >> 21),
				packed_11((int)(packed[2] << 10) >> 21), packed_11((int)(packed[3] << 10) >> 21));
		}
		if (components & 4)
		{
			VN(stream->slot + 2) = _mm_setr_ps(packed_10((int)packed[0] >> 22), packed_10((int)packed[1] >> 22),
				packed_10((int)packed[2] >> 22), packed_10((int)packed[3] >> 22));
		}
		break;
	}
	default:
	{
		float values[4][4];

		for (lane = 0; lane < 4; lane++)
			read_element(stream->type, lanes[lane], values[lane]);
		for (component = 0; component < 4; component++)
		{
			if (components & (1UL << component))
			{
				VN(stream->slot + component) = _mm_setr_ps(values[0][component], values[1][component],
					values[2][component], values[3][component]);
			}
		}
		break;
	}
	}
}

/* four vertices' x y z w (or fewer components) from four slots' worth of
registers to the array; `valid` vertices are written */
static void store_floats(const struct nv2a_vsh_cpu_array *array, unsigned long vertex, unsigned long valid,
	unsigned long components, __m128 x, __m128 y, __m128 z, __m128 w)
{
	unsigned char *out = (unsigned char *)array->data + vertex * array->stride;
	unsigned long stride = array->stride, lane;
	/* x0 y0 x1 y1 and x2 y2 x3 y3 */
	__m128 xy[2], zw[2];

	xy[0] = _mm_unpacklo_ps(x, y);
	xy[1] = _mm_unpackhi_ps(x, y);
	if (valid == 4 && components == 2)
	{
		_mm_storel_pi((__m64 *)out, xy[0]);
		_mm_storeh_pi((__m64 *)(out + stride), xy[0]);
		_mm_storel_pi((__m64 *)(out + stride * 2), xy[1]);
		_mm_storeh_pi((__m64 *)(out + stride * 3), xy[1]);
		return;
	}
	zw[0] = _mm_unpacklo_ps(z, w);
	zw[1] = _mm_unpackhi_ps(z, w);
	if (valid == 4 && components == 4)
	{
		_mm_storel_pi((__m64 *)out, xy[0]);
		_mm_storel_pi((__m64 *)(out + 8), zw[0]);
		out += stride;
		_mm_storeh_pi((__m64 *)out, xy[0]);
		_mm_storeh_pi((__m64 *)(out + 8), zw[0]);
		out += stride;
		_mm_storel_pi((__m64 *)out, xy[1]);
		_mm_storel_pi((__m64 *)(out + 8), zw[1]);
		out += stride;
		_mm_storeh_pi((__m64 *)out, xy[1]);
		_mm_storeh_pi((__m64 *)(out + 8), zw[1]);
		return;
	}
	for (lane = 0; lane < valid; lane++, out += stride)
	{
		if (lane & 1)
			_mm_storeh_pi((__m64 *)out, xy[lane >> 1]);
		else
			_mm_storel_pi((__m64 *)out, xy[lane >> 1]);
		if (components == 4)
		{
			if (lane & 1)
				_mm_storeh_pi((__m64 *)(out + 8), zw[lane >> 1]);
			else
				_mm_storel_pi((__m64 *)(out + 8), zw[lane >> 1]);
		}
		else if (components == 3)
		{
			/* z of this vertex: element 0 or 2 of the pair */
			_mm_store_ss((float *)(out + 8), (lane & 1) ? _mm_movehl_ps(zw[lane >> 1], zw[lane >> 1]) : zw[lane >> 1]);
		}
	}
}

/* position_terms, each number in a register */
struct vector_position_terms
{
	__m128 bias[2];
	__m128 offset[3];
	__m128 scale[3];
	__m128 multiplier[3];
	__m128 addend[3];
};

static void vector_position_terms_prepare(const struct position_terms *terms, struct vector_position_terms *vector)
{
	unsigned long index;

	for (index = 0; index < 3; index++)
	{
		if (index < 2)
			vector->bias[index] = _mm_set1_ps(terms->bias[index]);
		vector->offset[index] = _mm_set1_ps(terms->offset[index]);
		/* the divisor, or the factor that stands in for it */
		vector->scale[index] = _mm_set1_ps(terms->exact ? terms->scale[index] : terms->reciprocal[index]);
		vector->multiplier[index] = _mm_set1_ps(terms->multiplier[index]);
		vector->addend[index] = _mm_set1_ps(terms->addend[index]);
	}
}

static void store_vector(unsigned char *slots, const struct nv2a_vsh_cpu_draw *draw,
	const struct position_terms *terms, const struct vector_position_terms *vector, unsigned long vertex,
	unsigned long valid)
{
	unsigned long index, component, lane;

	if (draw->position.data)
	{
		__m128 position[3], w;

		if (terms->captured)
		{
			w = VN(_slot_capture + 3);
			for (index = 0; index < 3; index++)
			{
				__m128 sum = _mm_add_ps(_mm_mul_ps(VN(_slot_capture + index), vector->multiplier[index]),
					_mm_mul_ps(vector->addend[index], w));

				position[index] = terms->exact ? _mm_div_ps(sum, vector->scale[index]) :
					_mm_mul_ps(sum, vector->scale[index]);
			}
		}
		else
		{
			w = VN(_slot_outputs + 3);
			for (index = 0; index < 3; index++)
			{
				__m128 screen = VN(_slot_outputs + index);

				if (index < 2)
					screen = _mm_add_ps(screen, vector->bias[index]);
				screen = _mm_sub_ps(screen, vector->offset[index]);
				screen = terms->exact ? _mm_div_ps(screen, vector->scale[index]) : _mm_mul_ps(screen, vector->scale[index]);
				position[index] = _mm_mul_ps(screen, w);
			}
		}
		if (terms->flip_y)
			position[1] = _mm_xor_ps(position[1], vector_bits(0x80000000));
		if (terms->signed_depth)
			position[2] = _mm_sub_ps(_mm_mul_ps(_mm_set1_ps(2.0f), position[2]), w);
		store_floats(&draw->position, vertex, valid, 4, position[0], position[1], position[2], w);
	}
	for (index = 0; index < NV2A_VSH_CPU_TEXCOORDS; index++)
	{
		const struct nv2a_vsh_cpu_texcoord *texcoord = &draw->texcoords[index];
		unsigned long base = _slot_outputs + texcoord->source * 4;

		if (!texcoord->array.data || texcoord->source >= NV2A_VSH_OUTPUT_COUNT)
			continue;
		store_floats(&texcoord->array, vertex, valid, texcoord->components <= 2 ? 2 : texcoord->components == 3 ? 3 : 4,
			VN(base), VN(base + 1), VN(base + 2), VN(base + 3));
	}
	for (index = 0; index < NV2A_VSH_CPU_COLORS; index++)
	{
		const struct nv2a_vsh_cpu_color *array = &draw->colors[index];
		unsigned char *out = (unsigned char *)array->array.data + vertex * array->array.stride;
		__m128 zero = _mm_setzero_ps(), one = _mm_set1_ps(1.0f), rounding = _mm_set1_ps(COLOR_ROUNDING);
		__m128 color[4], low;

		if (!array->array.data)
			continue;
		for (component = 0; component < 4; component++)
		{
			unsigned long source = component < 3 ? array->color_source : array->alpha_source;
			unsigned long from = component < 3 ? component : (array->alpha_component & 3UL);

			if (source < NV2A_VSH_OUTPUT_COUNT)
				color[component] = _mm_min_ps(_mm_max_ps(VN(_slot_outputs + source * 4 + from), zero), one);
			else
				color[component] = component < 3 ? zero : one;
			if (array->color_scale)
			{
				color[component] = _mm_min_ps(_mm_max_ps(
					_mm_mul_ps(color[component], _mm_set1_ps(array->color_scale[component])), zero), one);
			}
			/* color_byte, still as a float */
			color[component] = _mm_sub_ps(_mm_add_ps(_mm_mul_ps(color[component], _mm_set1_ps(255.0f)), rounding),
				rounding);
		}
		/* red, green and blue are whole numbers below 256: together they
		are a whole number a float holds exactly, and one conversion */
		low = _mm_add_ps(_mm_add_ps(color[0], _mm_mul_ps(color[1], _mm_set1_ps(256.0f))),
			_mm_mul_ps(color[2], _mm_set1_ps(65536.0f)));
		for (lane = 0; lane < valid; lane++, out += array->array.stride)
		{
			unsigned int packed = (unsigned int)_mm_cvttss_si32(low) | ((unsigned int)_mm_cvttss_si32(color[3]) << 24);

			out[0] = (unsigned char)packed;
			out[1] = (unsigned char)(packed >> 8);
			out[2] = (unsigned char)(packed >> 16);
			out[3] = (unsigned char)(packed >> 24);
			/* the next vertex's elements down to element 0 */
			low = _mm_shuffle_ps(low, low, _MM_SHUFFLE(0, 3, 2, 1));
			color[3] = _mm_shuffle_ps(color[3], color[3], _MM_SHUFFLE(0, 3, 2, 1));
		}
	}
	if (draw->fog.data)
	{
		unsigned char *out = (unsigned char *)draw->fog.data + vertex * draw->fog.stride;
		__m128 fog = VN(_slot_outputs + _nv2a_vsh_output_fog * 4);

		for (lane = 0; lane < valid; lane++, out += draw->fog.stride)
		{
			_mm_store_ss((float *)out, fog);
			fog = _mm_shuffle_ps(fog, fog, _MM_SHUFFLE(0, 3, 2, 1));
		}
	}
}

BOOL nv2a_vsh_cpu_run(struct nv2a_vsh_cpu_program *program, const struct nv2a_vsh_cpu_draw *draw,
	unsigned long first, unsigned long count)
{
	struct vsh_variant *variant = variant_get(program, nv2a_vsh_cpu_draw_mask(draw), draw->flags);
	struct stream_input streams[XGPU_VERTEX_ATTRIBUTE_COUNT];
	float fixed[XGPU_VERTEX_ATTRIBUTE_COUNT][4];
	unsigned long fixed_components[XGPU_VERTEX_ATTRIBUTE_COUNT];
	struct position_terms terms;
	struct vector_position_terms vector_terms;
	const unsigned char *cursors[XGPU_VERTEX_ATTRIBUTE_COUNT];
	unsigned long stream_count, index, component, vertex, lane, group;
	unsigned char *slots;

	if (!variant)
		return FALSE;
	if (!count)
		return TRUE;
	{
		FLOAT_STATE_ENTER();

		position_terms_prepare(variant, draw, &terms);
		vector_position_terms_prepare(&terms, &vector_terms);
		stream_count = prepare_inputs(variant, draw, first, fixed, fixed_components, streams);
		for (index = 0; index < stream_count; index++)
			cursors[index] = streams[index].data;
		/* what does not change from vertex to vertex, in every group the
		run will use */
		for (group = 0; group < VECTOR_GROUPS && group * 4 < count; group++)
		{
			slots = (unsigned char *)variant->slots + group * sizeof(__m128);
			VN(_slot_zero) = _mm_setzero_ps();
			VN(_slot_one) = _mm_set1_ps(1.0f);
			for (index = 0; index < variant->constant_count; index++)
			{
				const struct constant_slot *constant = &variant->constants[index];
				float value = draw->constants[constant->constant][constant->component];

				VN(constant->slot) = _mm_set1_ps(constant->negate ? -value : value);
			}
			for (index = 0; index < variant->input_count; index++)
			{
				unsigned long input = variant->inputs[index].input;

				for (component = 0; component < 4; component++)
				{
					if (fixed_components[input] & (1UL << component))
						VN(_slot_inputs + input * 4 + component) = _mm_set1_ps(fixed[input][component]);
				}
			}
		}
		for (vertex = 0; vertex < count; vertex += VECTOR_GROUPS * 4)
		{
			unsigned long valid = count - vertex < VECTOR_GROUPS * 4 ? count - vertex : VECTOR_GROUPS * 4;
			unsigned long groups = (valid + 3) / 4;

			for (group = 0; group < groups; group++)
			{
				slots = (unsigned char *)variant->slots + group * sizeof(__m128);
				for (index = 0; index < stream_count; index++)
				{
					const struct stream_input *stream = &streams[index];
					const unsigned char *lanes[4];

					/* past the range's end the last vertex is read again */
					for (lane = 0; lane < 4; lane++)
					{
						lanes[lane] = cursors[index];
						if (vertex + group * 4 + lane + 1 < count)
							cursors[index] += stream->stride;
					}
					load_vector(slots, stream, lanes);
				}
			}
			execute_vector(variant, draw->constants, groups);
			for (group = 0; group < groups; group++)
			{
				unsigned long left = valid - group * 4;

				store_vector((unsigned char *)variant->slots + group * sizeof(__m128), draw, &terms, &vector_terms,
					vertex + group * 4, left < 4 ? left : 4);
			}
		}
		FLOAT_STATE_LEAVE();
	}
	return TRUE;
}

#undef VN

#else

BOOL nv2a_vsh_cpu_run(struct nv2a_vsh_cpu_program *program, const struct nv2a_vsh_cpu_draw *draw,
	unsigned long first, unsigned long count)
{
	return nv2a_vsh_cpu_run_scalar(program, draw, first, count);
}

#endif
