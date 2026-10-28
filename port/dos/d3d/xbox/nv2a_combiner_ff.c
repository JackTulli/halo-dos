/*
NV2A_COMBINER_FF.C

The pixel side of the light renderer: Xbox pixel state for OpenGL 1.1's
fixed-function fragment pipeline (see nv2a_combiner_ff.h).

The compiler works on polynomials. The texture stages, the general
combiner stages and the final combiner are evaluated symbolically, the
way nv2a_psh.c writes them as GLSL, into one sum of products for the
colour and one for alpha. The factors ("atoms") are the texels, the two
vertex colours, the fog factor and the constants; whatever is not a
polynomial (a clamp that the known ranges cannot remove, a dot product, a
multiplexer) becomes an atom of its own that remembers its operation and
operands. Products with one and sums with zero vanish on the way, as do
stages whose results nothing reads.

A plan is then found by taking the polynomial apart from the outside in:
the last texture unit must compute it from its texture, its constant, the
primary colour and the result of the unit before it, in one of the forms
its environment offers (modulate: a product; add: a sum; interpolate: a
polynomial of first degree in the blend factor; ...), which says what the
unit before must have computed, down to the primary colour. What depends
on one texture and constants only can be asked for as a derived texture.
When one pass is not enough the polynomial is split along a sum (the
later passes blend additively) or along a product (they multiply), where
the draw's own blend allows that. When nothing fits, textures are
replaced by constants, fewest first, and the plan that the comparison
with the reference likes best is kept.
*/

#include "nv2a_combiner_ff.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

/* ---------- constants */

/* indices of the pixel shader render states (D3DRS_PS*) */
enum
{
	_state_alpha_inputs = 0,
	_state_final_inputs_abcd = 8,
	_state_final_inputs_efg = 9,
	_state_alpha_outputs = 26,
	_state_rgb_inputs = 34,
	_state_compare_mode = 42,
	_state_rgb_outputs = 45,
	_state_combiner_count = 53,
	_state_dot_mapping = 55,
	_state_input_texture = 56
};

/* combiner registers, as nv2a_psh.c */
enum
{
	_register_zero = 0,
	_register_c0 = 1,
	_register_c1 = 2,
	_register_fog = 3,
	_register_v0 = 4,
	_register_v1 = 5,
	_register_t0 = 8,
	_register_t1 = 9,
	_register_t2 = 10,
	_register_t3 = 11,
	_register_r0 = 12,
	_register_r1 = 13,
	_register_v1r0_sum = 14,
	_register_ef_product = 15,
	NUMBER_OF_REGISTERS = 16
};

/* texture modes, as nv2a_psh.c */
enum
{
	_mode_none = 0x00,
	_mode_project2d = 0x01,
	_mode_project3d = 0x02,
	_mode_cubemap = 0x03,
	_mode_passthru = 0x04,
	_mode_clipplane = 0x05,
	_mode_bumpenvmap = 0x06,
	_mode_bumpenvmap_luminance = 0x07,
	_mode_brdf = 0x08,
	_mode_dot_st = 0x09,
	_mode_dot_zw = 0x0a,
	_mode_dot_reflect_diffuse = 0x0b,
	_mode_dot_reflect_specular = 0x0c,
	_mode_dot_str_3d = 0x0d,
	_mode_dot_str_cube = 0x0e,
	_mode_dependent_ar = 0x0f,
	_mode_dependent_gb = 0x10,
	_mode_dot_product = 0x11,
	_mode_dot_reflect_specular_constant = 0x12
};

enum
{
	_sampler_none = 0,
	_sampler_2d,
	_sampler_3d,
	_sampler_cube
};

/* D3DFOG_* */
enum
{
	_fog_table_none = 0,
	_fog_table_exponential,
	_fog_table_exponential_squared,
	_fog_table_linear
};

enum
{
	LEAF_ATOM_COUNT = NV2A_FF_LEAF_ATOM_COUNT,
	MAXIMUM_OPAQUE_ATOMS = 255 - LEAF_ATOM_COUNT,
	POLYNOMIAL_TERMS = 24,
	WIDE_POLYNOMIAL_TERMS = POLYNOMIAL_TERMS * POLYNOMIAL_TERMS,
	POLYNOMIAL_STORE_SIZE = 384,
	RANGE_MAXIMUM_ATOMS = 9,
	RANGE_MAXIMUM_CORNERS = 512,
	FAILURE_CACHE_SIZE = 256,
	/* a power of two: an entry lives in one of two neighbours */
	RANGE_CACHE_SIZE = 8192
};

/* what an atom or a polynomial depends on */
enum
{
	_depends_stage0 = 1 << 0, /* << stage */
	_depends_textures = 15,
	_depends_vertex = 1 << 4,
	_depends_constants = 1 << 5
};

/* the operations of atoms that are not polynomials (the header's
_nv2a_ff_operation_*, by their names here) */
enum
{
	_operation_maximum_zero = _nv2a_ff_operation_maximum_zero,
	_operation_clamp_unit = _nv2a_ff_operation_clamp_unit,
	_operation_clamp_signed = _nv2a_ff_operation_clamp_signed,
	_operation_dot = _nv2a_ff_operation_dot,
	_operation_mux_msb = _nv2a_ff_operation_mux_msb,
	_operation_mux_lsb = _nv2a_ff_operation_mux_lsb,
	_operation_signed_byte = _nv2a_ff_operation_signed_byte,
	_operation_identity = _nv2a_ff_operation_identity
};

#define COEFFICIENT_EPSILON 1.0e-6f
#define RANGE_EPSILON 1.0e-4f

/* ---------- polynomials */

struct polynomial
{
	int count;
	struct nv2a_ff_term terms[POLYNOMIAL_TERMS];
};

struct wide_polynomial
{
	int count;
	struct nv2a_ff_term terms[WIDE_POLYNOMIAL_TERMS];
};

struct opaque_atom
{
	unsigned char operation;
	unsigned char dependencies;
	unsigned char vector;
	/* of the operation and its operands (polynomial_hash) */
	unsigned long hash;
	float low, high;
	/* indices into the polynomial store, -1 for none */
	short operands[3];
};

/* a value a texture, a constant colour, the primary colour or the unit
before has to hold */
struct slot
{
	int bound;
	struct polynomial value;
};

struct unit_build
{
	int stage;
	struct slot texture_rgb, texture_alpha;
	struct slot constant_rgb, constant_alpha;
	struct slot previous_rgb, previous_alpha;
	unsigned char environment;
	struct nv2a_ff_combine rgb, alpha;
};

struct pass_build
{
	int unit_count;
	struct unit_build units[NV2A_FF_MAXIMUM_UNITS];
	struct slot primary_rgb, primary_alpha;
	/* EXT_secondary_color: added to what the units computed */
	struct slot secondary_rgb;

	/* how the pass meets the framebuffer */
	int blend_enable;
	unsigned long source_blend, destination_blend, blend_operation;
	int write_color, write_alpha;
	int alpha_test;
	int alpha_test_greater_zero;
	int depth_equal;
	int fog;
	int fog_black;
};

enum
{
	_role_first = 0,  /* the draw's own blend */
	_role_add,        /* adds to what the passes before drew */
	_role_multiply    /* multiplies it */
};

enum
{
	_blend_kind_other = 0,  /* one pass only */
	_blend_kind_write,      /* no blending */
	_blend_kind_sum,        /* source * factor + destination * factor: splits along a sum */
	_blend_kind_multiply,   /* destination * source */
	_blend_kind_double      /* 2 * destination * source */
};

struct symbolic_register
{
	struct polynomial rgb;
	struct polynomial alpha;
};

static struct
{
	const struct nv2a_ff_state *state;
	const struct nv2a_ff_capabilities *capabilities;
	const struct nv2a_ff_draw *draw;
	/* the constants when the plan is made for their values, or NULL; and
	which of them (bit n: atom source n) are put in as numbers */
	const struct nv2a_ff_constants *bound_constants;
	unsigned long bound_sources;

	/* per stage: _nv2a_ff_substitute_* */
	unsigned char substitute[NV2A_FF_STAGE_COUNT];
	/* per stage: a unit can sample it */
	unsigned char available[NV2A_FF_STAGE_COUNT];
	/* per stage: its texel comes from a sampler (so it can be replaced) */
	unsigned char fetches[NV2A_FF_STAGE_COUNT];
	int ignore_destination_alpha;

	/* the arena */
	struct polynomial store[POLYNOMIAL_STORE_SIZE];
	int store_count;
	struct opaque_atom opaque[MAXIMUM_OPAQUE_ATOMS];
	int opaque_count;
	/* the arena or a polynomial ran out: the attempt fails */
	int overflow;
	struct wide_polynomial wide;

	/* symbolic evaluation */
	struct symbolic_register registers[NUMBER_OF_REGISTERS];
	struct symbolic_register ef_product, v1r0_sum;
	struct polynomial goal_rgb, goal_alpha;
	unsigned long approximations;
	int kill_stages;

	/* the search for one pass */
	struct pass_build build;
	struct slot *log[64];
	int log_count;
	int bake_policy;
	int strict_primary;
	int prefer_color_slots;
	long steps;
	const struct polynomial *unit_goal_alpha[NV2A_FF_MAXIMUM_UNITS];

	/* the passes */
	struct pass_build sequence[NV2A_FF_MAXIMUM_PASSES];
	int sequence_count;
	int maximum_passes;
	int blend_kind;
	unsigned long source_blend, destination_blend, blend_operation;
	int needs_alpha_for_blend;
	int needs_alpha_for_test;
	int needs_alpha_for_write;
	int fog_active;
	struct polynomial fog_factor, fog_color;
	int product_splits_allowed;
	int single_pass;
	/* the colour can exceed one and the blend scales it: passes of a sum
	would be scaled before the clamp instead of after */
	int sum_overflows;

	/* what the last compile cost, and the count of candidates at which
	the present search began (its budget counts from there) */
	struct nv2a_ff_statistics statistics;
	unsigned long step_base;
	unsigned long step_budget;

	/* 0 to 4: what the search may use (compile_attempt's rounds) */
	int policy_rank;
	int drop_terms;
	long total_steps;
	/* goals that could not be fitted, and the highest rank they failed at */
	struct
	{
		unsigned long key;
		int rank;
	} failures[FAILURE_CACHE_SIZE];
	/* sums that could not be drawn in the passes that were left */
	struct
	{
		unsigned long key;
		int rank;
		int passes;
	} splits[FAILURE_CACHE_SIZE];
	/* ranges of polynomials found at the corners: they depend on nothing
	but the polynomial, so they hold for a whole compile */
	struct
	{
		unsigned long key;
		unsigned long age;
		float low, high;
	} ranges[RANGE_CACHE_SIZE];
	unsigned long range_age;
} work;

static void polynomial_zero(struct polynomial *polynomial)
{
	polynomial->count = 0;
}

static void polynomial_literal(struct polynomial *polynomial, float red, float green, float blue)
{
	polynomial->count = 0;
	if (fabs(red) >= COEFFICIENT_EPSILON || fabs(green) >= COEFFICIENT_EPSILON || fabs(blue) >= COEFFICIENT_EPSILON)
	{
		polynomial->count = 1;
		memset(&polynomial->terms[0], 0, sizeof(polynomial->terms[0]));
		polynomial->terms[0].coefficient[0] = red;
		polynomial->terms[0].coefficient[1] = green;
		polynomial->terms[0].coefficient[2] = blue;
	}
}

static void polynomial_atom(struct polynomial *polynomial, int atom)
{
	polynomial->count = 1;
	memset(&polynomial->terms[0], 0, sizeof(polynomial->terms[0]));
	polynomial->terms[0].coefficient[0] = 1.0f;
	polynomial->terms[0].coefficient[1] = 1.0f;
	polynomial->terms[0].coefficient[2] = 1.0f;
	polynomial->terms[0].count = 1;
	polynomial->terms[0].atoms[0] = (unsigned char)atom;
}

static int term_compare(const struct nv2a_ff_term *a, const struct nv2a_ff_term *b)
{
	if (a->count != b->count)
		return (int)a->count - (int)b->count;
	return memcmp(a->atoms, b->atoms, a->count);
}

static int term_is_zero(const struct nv2a_ff_term *term)
{
	return fabs(term->coefficient[0]) < COEFFICIENT_EPSILON &&
		fabs(term->coefficient[1]) < COEFFICIENT_EPSILON &&
		fabs(term->coefficient[2]) < COEFFICIENT_EPSILON;
}

/* sorts the atoms of each term and the terms, adds up equal terms and
removes the ones that came to nothing; returns the new count */
static int terms_normalize(struct nv2a_ff_term *terms, int count)
{
	int index, other, kept;

	for (index = 0; index < count; index++)
	{
		struct nv2a_ff_term *term = &terms[index];
		int a, b;

		for (a = 1; a < term->count; a++)
		{
			unsigned char atom = term->atoms[a];

			for (b = a; b > 0 && term->atoms[b - 1] > atom; b--)
				term->atoms[b] = term->atoms[b - 1];
			term->atoms[b] = atom;
		}
		for (a = term->count; a < NV2A_FF_TERM_ATOMS; a++)
			term->atoms[a] = 0;
	}
	for (index = 1; index < count; index++)
	{
		struct nv2a_ff_term term;

		if (term_compare(&terms[index - 1], &terms[index]) <= 0)
			continue;
		term = terms[index];
		for (other = index; other > 0 && term_compare(&terms[other - 1], &term) > 0; other--)
			terms[other] = terms[other - 1];
		terms[other] = term;
	}
	kept = 0;
	for (index = 0; index < count; index++)
	{
		if (kept > 0 && term_compare(&terms[kept - 1], &terms[index]) == 0)
		{
			terms[kept - 1].coefficient[0] += terms[index].coefficient[0];
			terms[kept - 1].coefficient[1] += terms[index].coefficient[1];
			terms[kept - 1].coefficient[2] += terms[index].coefficient[2];
		}
		else
		{
			if (kept > 0 && term_is_zero(&terms[kept - 1]))
				kept--;
			if (kept != index)
				terms[kept] = terms[index];
			kept++;
		}
	}
	if (kept > 0 && term_is_zero(&terms[kept - 1]))
		kept--;
	return kept;
}

/* the work area's wide polynomial into result; FALSE when it is too long */
static int wide_finish(struct polynomial *result)
{
	work.wide.count = terms_normalize(work.wide.terms, work.wide.count);
	if (work.wide.count > POLYNOMIAL_TERMS)
		return FALSE;
	result->count = work.wide.count;
	memcpy(result->terms, work.wide.terms, work.wide.count * sizeof(struct nv2a_ff_term));
	return TRUE;
}

static int polynomial_add(struct polynomial *result, const struct polynomial *a, const struct polynomial *b)
{
	memcpy(work.wide.terms, a->terms, a->count * sizeof(struct nv2a_ff_term));
	memcpy(work.wide.terms + a->count, b->terms, b->count * sizeof(struct nv2a_ff_term));
	work.wide.count = a->count + b->count;
	return wide_finish(result);
}

static void polynomial_scale(struct polynomial *polynomial, float red, float green, float blue)
{
	int index;

	for (index = 0; index < polynomial->count; index++)
	{
		polynomial->terms[index].coefficient[0] *= red;
		polynomial->terms[index].coefficient[1] *= green;
		polynomial->terms[index].coefficient[2] *= blue;
	}
	polynomial->count = terms_normalize(polynomial->terms, polynomial->count);
}

static int opaque_atom_get(int operation, const struct polynomial *a, const struct polynomial *b,
	const struct polynomial *c);

/* result = offset + factor * a */
static void polynomial_affine(struct polynomial *result, const struct polynomial *a, float factor, float offset)
{
	struct polynomial literal;
	struct polynomial scaled = *a;

	polynomial_scale(&scaled, factor, factor, factor);
	polynomial_literal(&literal, offset, offset, offset);
	if (!polynomial_add(result, &scaled, &literal))
	{
		/* one term too many: the polynomial as a single atom */
		polynomial_atom(&scaled, opaque_atom_get(_operation_identity, a, NULL, NULL));
		polynomial_scale(&scaled, factor, factor, factor);
		polynomial_add(result, &scaled, &literal);
	}
}

static int polynomial_multiply(struct polynomial *result, const struct polynomial *a, const struct polynomial *b)
{
	int index_a, index_b, count = 0;

	for (index_a = 0; index_a < a->count; index_a++)
	{
		for (index_b = 0; index_b < b->count; index_b++)
		{
			const struct nv2a_ff_term *term_a = &a->terms[index_a];
			const struct nv2a_ff_term *term_b = &b->terms[index_b];
			struct nv2a_ff_term *term = &work.wide.terms[count++];

			if (term_a->count + term_b->count > NV2A_FF_TERM_ATOMS)
				return FALSE;
			memset(term, 0, sizeof(*term));
			term->coefficient[0] = term_a->coefficient[0] * term_b->coefficient[0];
			term->coefficient[1] = term_a->coefficient[1] * term_b->coefficient[1];
			term->coefficient[2] = term_a->coefficient[2] * term_b->coefficient[2];
			term->count = (unsigned char)(term_a->count + term_b->count);
			memcpy(term->atoms, term_a->atoms, term_a->count);
			memcpy(term->atoms + term_a->count, term_b->atoms, term_b->count);
		}
	}
	work.wide.count = count;
	return wide_finish(result);
}

static int polynomial_equal(const struct polynomial *a, const struct polynomial *b)
{
	int index, channel;

	if (a->count != b->count)
		return FALSE;
	for (index = 0; index < a->count; index++)
	{
		if (term_compare(&a->terms[index], &b->terms[index]) != 0)
			return FALSE;
		for (channel = 0; channel < 3; channel++)
		{
			if (fabs(a->terms[index].coefficient[channel] - b->terms[index].coefficient[channel]) > 1.0e-4f)
				return FALSE;
		}
	}
	return TRUE;
}

/* the polynomial is one number in every channel (value may be NULL) */
static int polynomial_is_literal(const struct polynomial *polynomial, float *value)
{
	if (polynomial->count == 0)
	{
		if (value)
			value[0] = value[1] = value[2] = 0.0f;
		return TRUE;
	}
	if (polynomial->count == 1 && polynomial->terms[0].count == 0)
	{
		if (value)
			memcpy(value, polynomial->terms[0].coefficient, 3 * sizeof(float));
		return TRUE;
	}
	return FALSE;
}

static int polynomial_is_number(const struct polynomial *polynomial, float number)
{
	float value[3];

	return polynomial_is_literal(polynomial, value) && fabs(value[0] - number) < 1.0e-4f &&
		fabs(value[1] - number) < 1.0e-4f && fabs(value[2] - number) < 1.0e-4f;
}

/* ---------- atoms */

static int atom_dependencies(int atom)
{
	if (atom < LEAF_ATOM_COUNT)
	{
		int source = atom / 3;

		if (source < NV2A_FF_STAGE_COUNT)
			return _depends_stage0 << source;
		if (source == _nv2a_ff_atom_v0 || source == _nv2a_ff_atom_v1)
			return _depends_vertex;
		if (source == _nv2a_ff_atom_fog)
			return atom % 3 == _nv2a_ff_channel_alpha ? _depends_vertex : _depends_constants;
		return _depends_constants;
	}
	return work.opaque[atom - LEAF_ATOM_COUNT].dependencies;
}

static int atom_is_vector(int atom)
{
	if (atom < LEAF_ATOM_COUNT)
		return atom % 3 == _nv2a_ff_channel_rgb;
	return work.opaque[atom - LEAF_ATOM_COUNT].vector;
}

static void atom_range(int atom, float *low, float *high)
{
	if (atom < LEAF_ATOM_COUNT)
	{
		*low = 0.0f;
		*high = 1.0f;
	}
	else
	{
		*low = work.opaque[atom - LEAF_ATOM_COUNT].low;
		*high = work.opaque[atom - LEAF_ATOM_COUNT].high;
	}
}

static int polynomial_dependencies(const struct polynomial *polynomial)
{
	int index, atom, dependencies = 0;

	for (index = 0; index < polynomial->count; index++)
	{
		for (atom = 0; atom < polynomial->terms[index].count; atom++)
			dependencies |= atom_dependencies(polynomial->terms[index].atoms[atom]);
	}
	return dependencies;
}

/* the channels can differ */
static int polynomial_is_vector(const struct polynomial *polynomial)
{
	int index, atom;

	for (index = 0; index < polynomial->count; index++)
	{
		const struct nv2a_ff_term *term = &polynomial->terms[index];

		if (fabs(term->coefficient[0] - term->coefficient[1]) > COEFFICIENT_EPSILON ||
			fabs(term->coefficient[0] - term->coefficient[2]) > COEFFICIENT_EPSILON)
		{
			return TRUE;
		}
		for (atom = 0; atom < term->count; atom++)
		{
			if (atom_is_vector(term->atoms[atom]))
				return TRUE;
		}
	}
	return FALSE;
}

/* a hash of what a polynomial means: atoms that are operations count by
their operands, not by their number */
static unsigned long polynomial_hash(const struct polynomial *polynomial, unsigned long hash)
{
	int index, atom, channel;

	for (index = 0; index < polynomial->count; index++)
	{
		const struct nv2a_ff_term *term = &polynomial->terms[index];

		for (channel = 0; channel < 3; channel++)
			hash = (hash ^ (unsigned long)(long)(term->coefficient[channel] * 4096.0f)) * 16777619UL;
		for (atom = 0; atom < term->count; atom++)
		{
			if (term->atoms[atom] < LEAF_ATOM_COUNT)
			{
				hash = (hash ^ term->atoms[atom]) * 16777619UL;
			}
			else
			{
				hash = (hash ^ work.opaque[term->atoms[atom] - LEAF_ATOM_COUNT].hash) * 16777619UL;
			}
		}
		hash = (hash ^ 0x55UL) * 16777619UL;
	}
	return hash & 0xffffffffUL;
}

/* The range of a polynomial over the ranges of its atoms. The sum of the
terms' ranges first; when that is not good enough for a polynomial of
several terms, the values at the corners of the atoms' box, which are the
extremes of a polynomial of first degree in each atom. An atom that occurs
twice in a term counts as two there, each free in the atom's range: the
box is then larger than what the polynomial can reach, so the range found
holds it (the range must never be too small: clamps are dropped on its
word). */
static void polynomial_range(const struct polynomial *polynomial, float *low_out, float *high_out)
{
	float low = 0.0f, high = 0.0f;
	int index, atom, channel;
	/* the box's variables: an atom, and which of its occurrences in a term */
	unsigned char atoms[RANGE_MAXIMUM_ATOMS];
	unsigned char occurrences[RANGE_MAXIMUM_ATOMS];
	/* per term: which variable each factor is */
	unsigned char factors[POLYNOMIAL_TERMS][NV2A_FF_TERM_ATOMS];
	float atom_low[RANGE_MAXIMUM_ATOMS], atom_high[RANGE_MAXIMUM_ATOMS];
	int atom_count = 0;
	long corners = 1;
	int grey = TRUE, enumerable = TRUE;
	unsigned long key, slot;

	for (channel = 0; channel < 3; channel++)
	{
		float channel_low = 0.0f, channel_high = 0.0f;

		for (index = 0; index < polynomial->count; index++)
		{
			const struct nv2a_ff_term *term = &polynomial->terms[index];
			float term_low = term->coefficient[channel], term_high = term->coefficient[channel];

			for (atom = 0; atom < term->count && term->atoms[atom] < LEAF_ATOM_COUNT; atom++)
				;
			if (atom == term->count && term->count > 0)
			{
				/* a product of values from 0 to 1 times a number */
				if (term_low > 0.0f)
					term_low = 0.0f;
				if (term_high < 0.0f)
					term_high = 0.0f;
				channel_low += term_low;
				channel_high += term_high;
				continue;
			}
			for (atom = 0; atom < term->count; atom++)
			{
				float a_low, a_high, candidates[4], minimum, maximum;
				int candidate;

				atom_range(term->atoms[atom], &a_low, &a_high);
				candidates[0] = term_low * a_low;
				candidates[1] = term_low * a_high;
				candidates[2] = term_high * a_low;
				candidates[3] = term_high * a_high;
				minimum = maximum = candidates[0];
				for (candidate = 1; candidate < 4; candidate++)
				{
					if (candidates[candidate] < minimum)
						minimum = candidates[candidate];
					if (candidates[candidate] > maximum)
						maximum = candidates[candidate];
				}
				term_low = minimum;
				term_high = maximum;
			}
			channel_low += term_low;
			channel_high += term_high;
		}
		if (channel == 0 || channel_low < low)
			low = channel_low;
		if (channel == 0 || channel_high > high)
			high = channel_high;
	}
	*low_out = low;
	*high_out = high;
	if (polynomial->count < 2 || (low >= -RANGE_EPSILON && high <= 1.0f + RANGE_EPSILON))
		return;
	key = polynomial_hash(polynomial, 2166136261UL) | 1;
	slot = key & (RANGE_CACHE_SIZE - 1);
	if (work.ranges[slot].key != key)
		slot ^= 1;
	if (work.ranges[slot].key == key)
	{
		*low_out = work.ranges[slot].low;
		*high_out = work.ranges[slot].high;
		return;
	}

	for (index = 0; index < polynomial->count && enumerable; index++)
	{
		const struct nv2a_ff_term *term = &polynomial->terms[index];

		if (fabs(term->coefficient[0] - term->coefficient[1]) > COEFFICIENT_EPSILON ||
			fabs(term->coefficient[0] - term->coefficient[2]) > COEFFICIENT_EPSILON)
		{
			grey = FALSE;
		}
		for (atom = 0; atom < term->count; atom++)
		{
			/* (the atoms of a term are sorted: equal ones are neighbours) */
			int occurrence = atom > 0 && term->atoms[atom - 1] == term->atoms[atom] ?
				occurrences[factors[index][atom - 1]] + 1 : 0;
			int known;

			for (known = 0; known < atom_count &&
				(atoms[known] != term->atoms[atom] || occurrences[known] != occurrence); known++)
				;
			if (known == atom_count)
			{
				if (atom_count == RANGE_MAXIMUM_ATOMS)
				{
					enumerable = FALSE;
					break;
				}
				atoms[atom_count] = term->atoms[atom];
				occurrences[atom_count] = (unsigned char)occurrence;
				atom_range(term->atoms[atom], &atom_low[atom_count], &atom_high[atom_count]);
				atom_count++;
			}
			factors[index][atom] = (unsigned char)known;
		}
	}
	for (atom = 0; atom < atom_count && enumerable; atom++)
	{
		corners *= 2;
		if (corners > RANGE_MAXIMUM_CORNERS)
			enumerable = FALSE;
	}
	for (channel = 0; channel < (grey ? 1 : 3) && enumerable; channel++)
	{
		/* the corners counted through like a binary number */
		unsigned char digits[RANGE_MAXIMUM_ATOMS];
		float values[RANGE_MAXIMUM_ATOMS];
		long corner;

		for (atom = 0; atom < atom_count; atom++)
		{
			digits[atom] = 0;
			values[atom] = atom_low[atom];
		}
		for (corner = 0; corner < corners; corner++)
		{
			float sum = 0.0f;

			for (atom = 0; corner > 0 && atom < atom_count; atom++)
			{
				if (!digits[atom])
				{
					digits[atom] = 1;
					values[atom] = atom_high[atom];
					break;
				}
				digits[atom] = 0;
				values[atom] = atom_low[atom];
			}
			for (index = 0; index < polynomial->count; index++)
			{
				const struct nv2a_ff_term *term = &polynomial->terms[index];
				float product = term->coefficient[channel];
				int factor;

				for (factor = 0; factor < term->count; factor++)
					product *= values[factors[index][factor]];
				sum += product;
			}
			if ((channel == 0 && corner == 0) || sum < low)
				low = sum;
			if ((channel == 0 && corner == 0) || sum > high)
				high = sum;
		}
	}
	/* (when the corners are too many, the sum of the terms' ranges stands) */
	*low_out = low;
	*high_out = high;
	/* into the free one of the two places, or over the older entry */
	slot = key & (RANGE_CACHE_SIZE - 1);
	if (work.ranges[slot].key != 0)
	{
		if (work.ranges[slot ^ 1].key == 0 || work.ranges[slot ^ 1].age < work.ranges[slot].age)
			slot ^= 1;
	}
	work.ranges[slot].key = key;
	work.ranges[slot].age = ++work.range_age;
	work.ranges[slot].low = low;
	work.ranges[slot].high = high;
}

/* a compile begins without ranges, so that what it finds does not depend
on the compiles before it */
static void ranges_forget(void)
{
	memset(work.ranges, 0, sizeof(work.ranges));
	work.range_age = 0;
}

static int polynomial_within_unit(const struct polynomial *polynomial)
{
	float low, high;

	polynomial_range(polynomial, &low, &high);
	return low >= -RANGE_EPSILON && high <= 1.0f + RANGE_EPSILON;
}

/* ---------- atoms that are not polynomials */

static short store_polynomial(const struct polynomial *polynomial)
{
	if (work.store_count == POLYNOMIAL_STORE_SIZE)
	{
		work.overflow = TRUE;
		return 0;
	}
	work.store[work.store_count] = *polynomial;
	return (short)work.store_count++;
}

/* the atom for operation(a, b, c), the same atom for the same operands */
static int opaque_atom_get(int operation, const struct polynomial *a, const struct polynomial *b,
	const struct polynomial *c)
{
	const struct polynomial *operands[3];
	struct opaque_atom *atom;
	float low[3], high[3];
	int index, operand;

	operands[0] = a;
	operands[1] = b;
	operands[2] = c;
	for (index = 0; index < work.opaque_count; index++)
	{
		atom = &work.opaque[index];
		if (atom->operation != operation)
			continue;
		for (operand = 0; operand < 3; operand++)
		{
			if ((atom->operands[operand] < 0) != (operands[operand] == NULL))
				break;
			if (operands[operand] && !polynomial_equal(&work.store[atom->operands[operand]], operands[operand]))
				break;
		}
		if (operand == 3)
			return LEAF_ATOM_COUNT + index;
	}
	if (work.opaque_count == MAXIMUM_OPAQUE_ATOMS)
	{
		work.overflow = TRUE;
		return LEAF_ATOM_COUNT;
	}
	atom = &work.opaque[work.opaque_count];
	atom->operation = (unsigned char)operation;
	atom->dependencies = 0;
	atom->vector = FALSE;
	atom->hash = 0x100UL + (unsigned long)operation;
	for (operand = 0; operand < 3; operand++)
	{
		low[operand] = high[operand] = 0.0f;
		atom->operands[operand] = -1;
		if (operands[operand])
		{
			atom->operands[operand] = store_polynomial(operands[operand]);
			atom->dependencies |= polynomial_dependencies(operands[operand]);
			atom->hash = polynomial_hash(operands[operand], atom->hash * 16777619UL);
			polynomial_range(operands[operand], &low[operand], &high[operand]);
		}
		atom->hash = ((atom->hash ^ 0x33UL) * 16777619UL) & 0xffffffffUL;
	}
	switch (operation)
	{
	case _operation_maximum_zero:
		atom->vector = (unsigned char)polynomial_is_vector(a);
		atom->low = low[0] > 0.0f ? low[0] : 0.0f;
		atom->high = high[0] > 0.0f ? high[0] : 0.0f;
		break;
	case _operation_clamp_unit:
		atom->vector = (unsigned char)polynomial_is_vector(a);
		atom->low = low[0] < 0.0f ? 0.0f : low[0] > 1.0f ? 1.0f : low[0];
		atom->high = high[0] < 0.0f ? 0.0f : high[0] > 1.0f ? 1.0f : high[0];
		break;
	case _operation_clamp_signed:
		atom->vector = (unsigned char)polynomial_is_vector(a);
		atom->low = low[0] < -1.0f ? -1.0f : low[0] > 1.0f ? 1.0f : low[0];
		atom->high = high[0] < -1.0f ? -1.0f : high[0] > 1.0f ? 1.0f : high[0];
		break;
	case _operation_dot:
	{
		float size_a = (float)(fabs(low[0]) > fabs(high[0]) ? fabs(low[0]) : fabs(high[0]));
		float size_b = (float)(fabs(low[1]) > fabs(high[1]) ? fabs(low[1]) : fabs(high[1]));
		float numbers[3];
		int channel;

		atom->high = 3.0f * size_a * size_b;
		atom->low = (low[0] >= 0.0f && low[1] >= 0.0f) ? 0.0f : -atom->high;
		if (polynomial_is_literal(b, numbers) || polynomial_is_literal(a, numbers))
		{
			/* with three numbers: the sum of the channels' ranges */
			int other = polynomial_is_literal(b, numbers) ? 0 : 1;

			atom->low = atom->high = 0.0f;
			for (channel = 0; channel < 3; channel++)
			{
				float first = low[other] * numbers[channel], second = high[other] * numbers[channel];

				atom->low += first < second ? first : second;
				atom->high += first > second ? first : second;
			}
		}
		break;
	}
	case _operation_mux_msb:
	case _operation_mux_lsb:
		atom->vector = (unsigned char)(polynomial_is_vector(b) || polynomial_is_vector(c));
		atom->low = low[1] < low[2] ? low[1] : low[2];
		atom->high = high[1] > high[2] ? high[1] : high[2];
		break;
	case _operation_signed_byte:
		atom->vector = (unsigned char)polynomial_is_vector(a);
		atom->low = -128.0f / 127.0f;
		atom->high = 1.0f;
		break;
	default:
		atom->vector = (unsigned char)polynomial_is_vector(a);
		atom->low = low[0];
		atom->high = high[0];
		break;
	}
	return LEAF_ATOM_COUNT + work.opaque_count++;
}

/* max(value, 0) */
static void make_maximum_zero(struct polynomial *result, const struct polynomial *value)
{
	float low, high;

	polynomial_range(value, &low, &high);
	if (low >= -RANGE_EPSILON)
		*result = *value;
	else if (high <= 0.0f)
		polynomial_zero(result);
	else
		polynomial_atom(result, opaque_atom_get(_operation_maximum_zero, value, NULL, NULL));
}

/* clamp(value, 0, 1) */
static void make_clamp_unit(struct polynomial *result, const struct polynomial *value)
{
	float low, high;

	polynomial_range(value, &low, &high);
	if (low >= -RANGE_EPSILON && high <= 1.0f + RANGE_EPSILON)
		*result = *value;
	else if (high <= 0.0f)
		polynomial_zero(result);
	else if (low >= 1.0f)
		polynomial_literal(result, 1.0f, 1.0f, 1.0f);
	else
		polynomial_atom(result, opaque_atom_get(_operation_clamp_unit, value, NULL, NULL));
}

/* clamp(value, -1, 1) */
static void make_clamp_signed(struct polynomial *result, const struct polynomial *value)
{
	float low, high;

	polynomial_range(value, &low, &high);
	if (low >= -1.0f - RANGE_EPSILON && high <= 1.0f + RANGE_EPSILON)
		*result = *value;
	else if (low >= 1.0f)
		polynomial_literal(result, 1.0f, 1.0f, 1.0f);
	else if (high <= -1.0f)
		polynomial_literal(result, -1.0f, -1.0f, -1.0f);
	else if (low >= -RANGE_EPSILON)
		/* the same thing, in the form the texture units' own clamp has */
		polynomial_atom(result, opaque_atom_get(_operation_clamp_unit, value, NULL, NULL));
	else
		polynomial_atom(result, opaque_atom_get(_operation_clamp_signed, value, NULL, NULL));
}

/* a product that always fits: operands that would not multiply out are
wrapped into atoms */
static void make_product(struct polynomial *result, const struct polynomial *a, const struct polynomial *b)
{
	struct polynomial wrapped_a, wrapped_b;

	if (polynomial_multiply(result, a, b))
		return;
	wrapped_a = *a;
	wrapped_b = *b;
	if (a->count > 1)
		polynomial_atom(&wrapped_a, opaque_atom_get(_operation_identity, a, NULL, NULL));
	if (b->count > 1)
		polynomial_atom(&wrapped_b, opaque_atom_get(_operation_identity, b, NULL, NULL));
	if (!polynomial_multiply(result, &wrapped_a, &wrapped_b))
	{
		/* two single terms with too many factors between them */
		polynomial_atom(&wrapped_a, opaque_atom_get(_operation_identity, a, NULL, NULL));
		polynomial_atom(&wrapped_b, opaque_atom_get(_operation_identity, b, NULL, NULL));
		if (!polynomial_multiply(result, &wrapped_a, &wrapped_b))
			work.overflow = TRUE;
	}
}

static void make_sum(struct polynomial *result, const struct polynomial *a, const struct polynomial *b)
{
	struct polynomial wrapped_a, wrapped_b;

	if (polynomial_add(result, a, b))
		return;
	polynomial_atom(&wrapped_a, opaque_atom_get(_operation_identity, a, NULL, NULL));
	polynomial_atom(&wrapped_b, opaque_atom_get(_operation_identity, b, NULL, NULL));
	if (!polynomial_add(result, &wrapped_a, &wrapped_b))
		work.overflow = TRUE;
}

static void make_dot(struct polynomial *result, const struct polynomial *a, const struct polynomial *b)
{
	float value_a[3], value_b[3];

	if (a->count == 0 || b->count == 0)
	{
		polynomial_zero(result);
	}
	else if (polynomial_is_literal(a, value_a) && polynomial_is_literal(b, value_b))
	{
		float dot = value_a[0] * value_b[0] + value_a[1] * value_b[1] + value_a[2] * value_b[2];

		polynomial_literal(result, dot, dot, dot);
	}
	else if (!polynomial_is_vector(a) && !polynomial_is_vector(b))
	{
		make_product(result, a, b);
		polynomial_scale(result, 3.0f, 3.0f, 3.0f);
	}
	else
	{
		polynomial_atom(result, opaque_atom_get(_operation_dot, a, b, NULL));
	}
}

/* select >= 0.5 (or the low bit of its byte) ? cd : ab */
static void make_mux(struct polynomial *result, const struct polynomial *select, const struct polynomial *cd,
	const struct polynomial *ab, int msb)
{
	float low, high;
	float literal[3];

	polynomial_range(select, &low, &high);
	if (polynomial_equal(cd, ab))
		*result = *ab;
	else if (msb && low >= 0.5f)
		*result = *cd;
	else if (msb && high < 0.5f - 1.0e-3f)
		*result = *ab;
	else if (!msb && polynomial_is_literal(select, literal))
		*result = ((int)(literal[0] * 255.0f + 0.5f) & 1) ? *cd : *ab;
	else
		polynomial_atom(result, opaque_atom_get(msb ? _operation_mux_msb : _operation_mux_lsb, select, cd, ab));
}

/* The blue channel of a colour as a value for alpha: colour atoms become
their blue, and so do the operands of clamps and multiplexers. */
static void make_blue(struct polynomial *result, const struct polynomial *value);

static int atom_blue(int atom)
{
	struct polynomial operands[3];
	const struct opaque_atom *opaque;
	int operation, operand;
	int present[3];

	if (atom < LEAF_ATOM_COUNT)
		return atom % 3 == _nv2a_ff_channel_rgb ? atom + _nv2a_ff_channel_blue : atom;
	opaque = &work.opaque[atom - LEAF_ATOM_COUNT];
	if (!opaque->vector)
		return atom;
	operation = opaque->operation;
	for (operand = 0; operand < 3; operand++)
	{
		present[operand] = work.opaque[atom - LEAF_ATOM_COUNT].operands[operand] >= 0;
		if (present[operand])
		{
			struct polynomial source = work.store[work.opaque[atom - LEAF_ATOM_COUNT].operands[operand]];

			make_blue(&operands[operand], &source);
		}
	}
	return opaque_atom_get(operation, present[0] ? &operands[0] : NULL, present[1] ? &operands[1] : NULL,
		present[2] ? &operands[2] : NULL);
}

static void make_blue(struct polynomial *result, const struct polynomial *value)
{
	struct polynomial blue = *value;
	int index, atom;

	for (index = 0; index < blue.count; index++)
	{
		struct nv2a_ff_term *term = &blue.terms[index];

		term->coefficient[0] = term->coefficient[1] = term->coefficient[2];
		for (atom = 0; atom < term->count; atom++)
			term->atoms[atom] = (unsigned char)atom_blue(term->atoms[atom]);
	}
	blue.count = terms_normalize(blue.terms, blue.count);
	*result = blue;
}

/* ---------- the reference evaluator */

void nv2a_ff_color_from_argb(unsigned long argb, float color[4])
{
	color[0] = (float)((argb >> 16) & 0xff) / 255.0f;
	color[1] = (float)((argb >> 8) & 0xff) / 255.0f;
	color[2] = (float)(argb & 0xff) / 255.0f;
	color[3] = (float)((argb >> 24) & 0xff) / 255.0f;
}

static float clamp_unit(float value)
{
	return value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
}

static float clamp_signed(float value)
{
	return value < -1.0f ? -1.0f : value > 1.0f ? 1.0f : value;
}

static float signed_byte(float value)
{
	float byte = (float)floor(value * 255.0f + 0.5f);

	return (byte >= 128.0f ? byte - 256.0f : byte) / 127.0f;
}

static unsigned long stage_mode(const struct nv2a_ff_state *state, int stage)
{
	unsigned long mode = (state->texture_modes >> (5 * stage)) & 0x1f;

	if (state->sampler_type[stage] == _sampler_none && mode != _mode_passthru && mode != _mode_clipplane &&
		mode != _mode_dot_product && mode != _mode_dot_zw)
	{
		mode = _mode_none;
	}
	return mode;
}

static float fog_factor_from_coordinate(const struct nv2a_ff_state *state,
	const struct nv2a_ff_constants *constants, float coordinate)
{
	float factor = 1.0f;

	if (state->fog_enable)
	{
		float span;

		switch (state->fog_table_mode)
		{
		case _fog_table_exponential:
			factor = (float)exp(-constants->fog_parameters[2] * coordinate);
			break;
		case _fog_table_exponential_squared:
			factor = constants->fog_parameters[2] * coordinate;
			factor = (float)exp(-factor * factor);
			break;
		case _fog_table_linear:
			span = constants->fog_parameters[1] - constants->fog_parameters[0];
			factor = (constants->fog_parameters[1] - coordinate) / (span > 1.0e-6f ? span : 1.0e-6f);
			break;
		default:
			factor = coordinate;
			break;
		}
	}
	return clamp_unit(factor);
}

/* the registers of the reference: four floats each */
struct reference_registers
{
	float values[NUMBER_OF_REGISTERS][4];
	float final_c0[4], final_c1[4];
};

/* one combiner input: three channels for the colour portion, one value
(in all three) for the alpha portion */
static void reference_input(const struct reference_registers *registers, const struct nv2a_ff_constants *constants,
	unsigned long input, int alpha_portion, int stage, int unique_c0, int unique_c1, float value[3])
{
	unsigned long reg = input & 0x0f;
	int alpha_channel = (input & 0x10) != 0;
	unsigned long mapping = input & 0xe0;
	static const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	const float *source = zero;
	int channel;

	switch (reg)
	{
	case _register_c0:
		source = stage < 0 ? registers->final_c0 : constants->c0[unique_c0 ? stage : 0];
		break;
	case _register_c1:
		source = stage < 0 ? registers->final_c1 : constants->c1[unique_c1 ? stage : 0];
		break;
	case _register_fog:
	case _register_v0:
	case _register_v1:
	case _register_t0:
	case _register_t1:
	case _register_t2:
	case _register_t3:
	case _register_r0:
	case _register_r1:
		source = registers->values[reg];
		break;
	case _register_v1r0_sum:
	case _register_ef_product:
		if (stage < 0)
			source = registers->values[reg];
		break;
	default:
		break;
	}
	if (alpha_portion)
		value[0] = value[1] = value[2] = alpha_channel ? source[3] : source[2];
	else if (alpha_channel)
		value[0] = value[1] = value[2] = source[3];
	else
		memcpy(value, source, 3 * sizeof(float));

	for (channel = 0; channel < 3; channel++)
	{
		float x = value[channel];
		float positive = x > 0.0f ? x : 0.0f;

		if (stage < 0)
		{
			/* the final combiner has the unsigned mappings only */
			value[channel] = (input & 0x20) ? 1.0f - clamp_unit(x) : clamp_unit(x);
			continue;
		}
		switch (mapping)
		{
		case 0x00: value[channel] = positive; break;
		case 0x20: value[channel] = 1.0f - clamp_unit(x); break;
		case 0x40: value[channel] = 2.0f * positive - 1.0f; break;
		case 0x60: value[channel] = 1.0f - 2.0f * positive; break;
		case 0x80: value[channel] = positive - 0.5f; break;
		case 0xa0: value[channel] = 0.5f - positive; break;
		case 0xc0: value[channel] = x; break;
		default: value[channel] = -x; break;
		}
	}
}

static float reference_output(float value, unsigned long flags)
{
	switch (flags & 0x38)
	{
	case 0x08: value = value - 0.5f; break;
	case 0x10: value = value * 2.0f; break;
	case 0x18: value = (value - 0.5f) * 2.0f; break;
	case 0x20: value = value * 4.0f; break;
	case 0x30: value = value * 0.5f; break;
	default: break;
	}
	return clamp_signed(value);
}

/* the registers a combiner stage can write */
static int register_is_destination(unsigned long reg)
{
	return reg == _register_v0 || reg == _register_v1 || (reg >= _register_t0 && reg <= _register_r1);
}

static void reference_stage(struct reference_registers *registers, const struct nv2a_ff_state *state,
	const struct nv2a_ff_constants *constants, int stage)
{
	unsigned long combiner_count = state->combiner_state[_state_combiner_count];
	int unique_c0 = (combiner_count & 0x1000) != 0;
	int unique_c1 = (combiner_count & 0x10000) != 0;
	int mux_msb = (combiner_count & 0x100) != 0;
	float ab[2][3], cd[2][3], sum[2][3];
	int portion, channel;

	for (portion = 0; portion < 2; portion++)
	{
		int alpha = portion == 1;
		unsigned long inputs = state->combiner_state[(alpha ? _state_alpha_inputs : _state_rgb_inputs) + stage];
		unsigned long outputs = state->combiner_state[(alpha ? _state_alpha_outputs : _state_rgb_outputs) + stage];
		unsigned long flags = outputs >> 12;
		float a[3], b[3], c[3], d[3];

		reference_input(registers, constants, (inputs >> 24) & 0xff, alpha, stage, unique_c0, unique_c1, a);
		reference_input(registers, constants, (inputs >> 16) & 0xff, alpha, stage, unique_c0, unique_c1, b);
		reference_input(registers, constants, (inputs >> 8) & 0xff, alpha, stage, unique_c0, unique_c1, c);
		reference_input(registers, constants, inputs & 0xff, alpha, stage, unique_c0, unique_c1, d);
		for (channel = 0; channel < 3; channel++)
		{
			ab[portion][channel] = a[channel] * b[channel];
			cd[portion][channel] = c[channel] * d[channel];
		}
		if (!alpha && (flags & 0x02))
			ab[portion][0] = ab[portion][1] = ab[portion][2] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
		if (!alpha && (flags & 0x01))
			cd[portion][0] = cd[portion][1] = cd[portion][2] = c[0] * d[0] + c[1] * d[1] + c[2] * d[2];
		for (channel = 0; channel < 3; channel++)
		{
			if (flags & 0x04)
			{
				float select = registers->values[_register_r0][3];
				int take_cd = mux_msb ? select >= 0.5f : ((int)(select * 255.0f + 0.5f) & 1) != 0;

				sum[portion][channel] = take_cd ? cd[portion][channel] : ab[portion][channel];
			}
			else
			{
				sum[portion][channel] = ab[portion][channel] + cd[portion][channel];
			}
		}
		for (channel = 0; channel < 3; channel++)
		{
			ab[portion][channel] = reference_output(ab[portion][channel], flags);
			cd[portion][channel] = reference_output(cd[portion][channel], flags);
			sum[portion][channel] = reference_output(sum[portion][channel], flags);
		}
	}

	/* write back only after both portions have read their inputs */
	for (portion = 0; portion < 2; portion++)
	{
		int alpha = portion == 1;
		unsigned long outputs = state->combiner_state[(alpha ? _state_alpha_outputs : _state_rgb_outputs) + stage];
		unsigned long flags = outputs >> 12;
		unsigned long ab_register = (outputs >> 4) & 0x0f;
		unsigned long cd_register = outputs & 0x0f;
		unsigned long sum_register = (outputs >> 8) & 0x0f;

		if (register_is_destination(ab_register))
		{
			if (alpha)
				registers->values[ab_register][3] = ab[portion][0];
			else
			{
				memcpy(registers->values[ab_register], ab[portion], 3 * sizeof(float));
				if (flags & 0x80)
					registers->values[ab_register][3] = ab[portion][2];
			}
		}
		if (register_is_destination(cd_register))
		{
			if (alpha)
				registers->values[cd_register][3] = cd[portion][0];
			else
			{
				memcpy(registers->values[cd_register], cd[portion], 3 * sizeof(float));
				if (flags & 0x40)
					registers->values[cd_register][3] = cd[portion][2];
			}
		}
		if (register_is_destination(sum_register))
		{
			if (alpha)
				registers->values[sum_register][3] = sum[portion][0];
			else
				memcpy(registers->values[sum_register], sum[portion], 3 * sizeof(float));
		}
	}
}

static int compare_alpha(unsigned long function, float alpha, float reference)
{
	float value = (float)floor(clamp_unit(alpha) * 255.0f + 0.5f);

	switch (function)
	{
	case _nv2a_ff_compare_never: return FALSE;
	case _nv2a_ff_compare_less: return value < reference;
	case _nv2a_ff_compare_equal: return value == reference;
	case _nv2a_ff_compare_less_or_equal: return value <= reference;
	case _nv2a_ff_compare_greater: return value > reference;
	case _nv2a_ff_compare_not_equal: return value != reference;
	case _nv2a_ff_compare_greater_or_equal: return value >= reference;
	default: return TRUE;
	}
}

int nv2a_ff_reference(const struct nv2a_ff_state *state, const struct nv2a_ff_constants *constants,
	const struct nv2a_ff_fragment *fragment, float result[4])
{
	struct reference_registers registers;
	unsigned long combiner_count = state->combiner_state[_state_combiner_count] & 0xff;
	unsigned long final_abcd = state->combiner_state[_state_final_inputs_abcd];
	unsigned long final_efg = state->combiner_state[_state_final_inputs_efg];
	int stage, channel;
	int discarded = FALSE;

	memset(&registers, 0, sizeof(registers));
	memcpy(registers.final_c0, constants->final_c0, sizeof(registers.final_c0));
	memcpy(registers.final_c1, constants->final_c1, sizeof(registers.final_c1));
	memcpy(registers.values[_register_v0], fragment->v0, 4 * sizeof(float));
	memcpy(registers.values[_register_v1], fragment->v1, 4 * sizeof(float));

	for (stage = 0; stage < NV2A_FF_STAGE_COUNT; stage++)
	{
		float *texel = registers.values[_register_t0 + stage];
		unsigned long mode = stage_mode(state, stage);

		switch (mode)
		{
		case _mode_passthru:
			for (channel = 0; channel < 4; channel++)
				texel[channel] = clamp_unit(fragment->texcoord[stage][channel]);
			break;
		case _mode_clipplane:
		{
			unsigned long compare = (state->combiner_state[_state_compare_mode] >> (4 * stage)) & 0xf;

			for (channel = 0; channel < 4; channel++)
			{
				float coordinate = fragment->texcoord[stage][channel];

				if ((compare & (1 << channel)) ? coordinate >= 0.0f : coordinate < 0.0f)
					discarded = TRUE;
			}
			break;
		}
		case _mode_none:
		case _mode_dot_product:
		case _mode_dot_zw:
		case _mode_brdf:
			break;
		case _mode_bumpenvmap_luminance:
			memcpy(texel, fragment->texel[stage], 4 * sizeof(float));
			if (stage > 0)
			{
				float luminance = clamp_unit(constants->bump_luminance[stage][0] *
					registers.values[_register_t0 + stage - 1][2] + constants->bump_luminance[stage][1]);

				texel[0] *= luminance;
				texel[1] *= luminance;
				texel[2] *= luminance;
			}
			break;
		default:
			if (mode > _mode_dot_reflect_specular_constant)
				break;
			/* the sampler's answer, whatever coordinates the mode made */
			memcpy(texel, fragment->texel[stage], 4 * sizeof(float));
			break;
		}
		if (state->color_sign[stage] && mode != _mode_none)
		{
			/* bits: alpha, red, green, blue */
			if (state->color_sign[stage] & 1)
				texel[3] = signed_byte(texel[3]);
			if (state->color_sign[stage] & 2)
				texel[0] = signed_byte(texel[0]);
			if (state->color_sign[stage] & 4)
				texel[1] = signed_byte(texel[1]);
			if (state->color_sign[stage] & 8)
				texel[2] = signed_byte(texel[2]);
		}
		if (state->alpha_kill[stage] && mode != _mode_none && texel[3] == 0.0f)
			discarded = TRUE;
	}

	/* the fog register: rgb is the fog color, alpha the fog factor */
	memcpy(registers.values[_register_fog], constants->fog_color, 3 * sizeof(float));
	registers.values[_register_fog][3] = fog_factor_from_coordinate(state, constants, fragment->fog);
	registers.values[_register_r0][3] = registers.values[_register_t0][3];

	if (combiner_count > 8)
		combiner_count = 8;
	for (stage = 0; stage < (int)combiner_count; stage++)
		reference_stage(&registers, state, constants, stage);

	if (final_abcd == 0 && final_efg == 0)
	{
		memcpy(result, registers.values[_register_r0], 4 * sizeof(float));
	}
	else
	{
		unsigned long settings = final_efg & 0xff;
		float e[3], f[3], a[3], b[3], c[3], d[3], g[3];
		float *ef_product = registers.values[_register_ef_product];
		float *v1r0_sum = registers.values[_register_v1r0_sum];

		reference_input(&registers, constants, (final_efg >> 24) & 0xff, FALSE, -1, FALSE, FALSE, e);
		reference_input(&registers, constants, (final_efg >> 16) & 0xff, FALSE, -1, FALSE, FALSE, f);
		for (channel = 0; channel < 3; channel++)
		{
			float v1 = clamp_unit(registers.values[_register_v1][channel]);
			float r0 = clamp_unit(registers.values[_register_r0][channel]);

			ef_product[channel] = e[channel] * f[channel];
			v1r0_sum[channel] = ((settings & 0x40) ? 1.0f - v1 : v1) + ((settings & 0x20) ? 1.0f - r0 : r0);
			if (settings & 0x80)
				v1r0_sum[channel] = clamp_unit(v1r0_sum[channel]);
		}
		ef_product[3] = 0.0f;
		v1r0_sum[3] = 0.0f;
		reference_input(&registers, constants, (final_abcd >> 24) & 0xff, FALSE, -1, FALSE, FALSE, a);
		reference_input(&registers, constants, (final_abcd >> 16) & 0xff, FALSE, -1, FALSE, FALSE, b);
		reference_input(&registers, constants, (final_abcd >> 8) & 0xff, FALSE, -1, FALSE, FALSE, c);
		reference_input(&registers, constants, final_abcd & 0xff, FALSE, -1, FALSE, FALSE, d);
		reference_input(&registers, constants, (final_efg >> 8) & 0xff, TRUE, -1, FALSE, FALSE, g);
		for (channel = 0; channel < 3; channel++)
			result[channel] = a[channel] * b[channel] + (1.0f - a[channel]) * c[channel] + d[channel];
		result[3] = g[0];
	}

	if (state->alpha_test_function &&
		!compare_alpha(state->alpha_test_function, result[3], constants->alpha_reference))
	{
		discarded = TRUE;
	}
	for (channel = 0; channel < 4; channel++)
		result[channel] = clamp_unit(result[channel]);
	return !discarded;
}

/* ---------- symbolic evaluation

The same steps as the reference, on polynomials. */

static const float *constant_color(const struct nv2a_ff_constants *constants, int source)
{
	if (source >= _nv2a_ff_atom_c0 && source < _nv2a_ff_atom_c0 + 8)
		return constants->c0[source - _nv2a_ff_atom_c0];
	if (source >= _nv2a_ff_atom_c1 && source < _nv2a_ff_atom_c1 + 8)
		return constants->c1[source - _nv2a_ff_atom_c1];
	switch (source)
	{
	case _nv2a_ff_atom_final_c0: return constants->final_c0;
	case _nv2a_ff_atom_final_c1: return constants->final_c1;
	case _nv2a_ff_atom_blend_color: return constants->blend_color;
	default: return constants->fog_color;
	}
}

/* a constant as a register: atoms, or its numbers when they are known */
static void symbolic_constant(struct symbolic_register *result, int source)
{
	if (work.bound_constants && (work.bound_sources & (1UL << source)))
	{
		const float *color = constant_color(work.bound_constants, source);

		polynomial_literal(&result->rgb, color[0], color[1], color[2]);
		polynomial_literal(&result->alpha, color[3], color[3], color[3]);
	}
	else
	{
		polynomial_atom(&result->rgb, NV2A_FF_ATOM(source, _nv2a_ff_channel_rgb));
		polynomial_atom(&result->alpha, NV2A_FF_ATOM(source, _nv2a_ff_channel_alpha));
	}
}

static void symbolic_input(struct polynomial *result, unsigned long input, int alpha_portion, int stage,
	int unique_c0, int unique_c1)
{
	unsigned long reg = input & 0x0f;
	int alpha_channel = (input & 0x10) != 0;
	unsigned long mapping = input & 0xe0;
	struct symbolic_register constant;
	const struct symbolic_register *source = NULL;
	struct polynomial value, positive;

	switch (reg)
	{
	case _register_c0:
		symbolic_constant(&constant, stage < 0 ? _nv2a_ff_atom_final_c0 : _nv2a_ff_atom_c0 + (unique_c0 ? stage : 0));
		source = &constant;
		break;
	case _register_c1:
		symbolic_constant(&constant, stage < 0 ? _nv2a_ff_atom_final_c1 : _nv2a_ff_atom_c1 + (unique_c1 ? stage : 0));
		source = &constant;
		break;
	case _register_fog:
	case _register_v0:
	case _register_v1:
	case _register_t0:
	case _register_t1:
	case _register_t2:
	case _register_t3:
	case _register_r0:
	case _register_r1:
		source = &work.registers[reg];
		break;
	case _register_v1r0_sum:
		if (stage < 0)
			source = &work.v1r0_sum;
		break;
	case _register_ef_product:
		if (stage < 0)
			source = &work.ef_product;
		break;
	default:
		break;
	}
	if (!source)
		polynomial_zero(&value);
	else if (alpha_channel)
		value = source->alpha;
	else if (alpha_portion)
		make_blue(&value, &source->rgb);
	else
		value = source->rgb;

	if (stage < 0)
	{
		make_clamp_unit(result, &value);
		if (input & 0x20)
		{
			value = *result;
			polynomial_affine(result, &value, -1.0f, 1.0f);
		}
		return;
	}
	switch (mapping)
	{
	case 0x00:
		make_maximum_zero(result, &value);
		break;
	case 0x20:
		make_clamp_unit(&positive, &value);
		polynomial_affine(result, &positive, -1.0f, 1.0f);
		break;
	case 0x40:
		make_maximum_zero(&positive, &value);
		polynomial_affine(result, &positive, 2.0f, -1.0f);
		break;
	case 0x60:
		make_maximum_zero(&positive, &value);
		polynomial_affine(result, &positive, -2.0f, 1.0f);
		break;
	case 0x80:
		make_maximum_zero(&positive, &value);
		polynomial_affine(result, &positive, 1.0f, -0.5f);
		break;
	case 0xa0:
		make_maximum_zero(&positive, &value);
		polynomial_affine(result, &positive, -1.0f, 0.5f);
		break;
	case 0xc0:
		*result = value;
		break;
	default:
		*result = value;
		polynomial_scale(result, -1.0f, -1.0f, -1.0f);
		break;
	}
}

static void symbolic_output(struct polynomial *result, const struct polynomial *value, unsigned long flags)
{
	struct polynomial mapped;

	switch (flags & 0x38)
	{
	case 0x08: polynomial_affine(&mapped, value, 1.0f, -0.5f); break;
	case 0x10: polynomial_affine(&mapped, value, 2.0f, 0.0f); break;
	case 0x18: polynomial_affine(&mapped, value, 2.0f, -1.0f); break;
	case 0x20: polynomial_affine(&mapped, value, 4.0f, 0.0f); break;
	case 0x30: polynomial_affine(&mapped, value, 0.5f, 0.0f); break;
	default: mapped = *value; break;
	}
	make_clamp_signed(result, &mapped);
}

static void symbolic_stage(int stage)
{
	const struct nv2a_ff_state *state = work.state;
	unsigned long combiner_count = state->combiner_state[_state_combiner_count];
	int unique_c0 = (combiner_count & 0x1000) != 0;
	int unique_c1 = (combiner_count & 0x10000) != 0;
	int mux_msb = (combiner_count & 0x100) != 0;
	/* static: six polynomials are a lot of stack */
	static struct polynomial ab[2], cd[2], sum[2];
	int portion;

	for (portion = 0; portion < 2; portion++)
	{
		int alpha = portion == 1;
		unsigned long inputs = state->combiner_state[(alpha ? _state_alpha_inputs : _state_rgb_inputs) + stage];
		unsigned long outputs = state->combiner_state[(alpha ? _state_alpha_outputs : _state_rgb_outputs) + stage];
		unsigned long flags = outputs >> 12;
		struct polynomial a, b, c, d, product_ab, product_cd, combined;

		/* products nothing reads are not worth their atoms */
		int ab_read = register_is_destination((outputs >> 4) & 0x0f);
		int cd_read = register_is_destination(outputs & 0x0f);
		int sum_read = register_is_destination((outputs >> 8) & 0x0f);

		polynomial_zero(&product_ab);
		polynomial_zero(&product_cd);
		if (ab_read || sum_read)
		{
			symbolic_input(&a, (inputs >> 24) & 0xff, alpha, stage, unique_c0, unique_c1);
			symbolic_input(&b, (inputs >> 16) & 0xff, alpha, stage, unique_c0, unique_c1);
			if (!alpha && (flags & 0x02))
				make_dot(&product_ab, &a, &b);
			else
				make_product(&product_ab, &a, &b);
		}
		if (cd_read || sum_read)
		{
			symbolic_input(&c, (inputs >> 8) & 0xff, alpha, stage, unique_c0, unique_c1);
			symbolic_input(&d, inputs & 0xff, alpha, stage, unique_c0, unique_c1);
			if (!alpha && (flags & 0x01))
				make_dot(&product_cd, &c, &d);
			else
				make_product(&product_cd, &c, &d);
		}
		polynomial_zero(&combined);
		if (sum_read)
		{
			if (flags & 0x04)
				make_mux(&combined, &work.registers[_register_r0].alpha, &product_cd, &product_ab, mux_msb);
			else
				make_sum(&combined, &product_ab, &product_cd);
		}
		symbolic_output(&ab[portion], &product_ab, flags);
		symbolic_output(&cd[portion], &product_cd, flags);
		symbolic_output(&sum[portion], &combined, flags);
	}

	/* write back only after both portions have read their inputs */
	for (portion = 0; portion < 2; portion++)
	{
		int alpha = portion == 1;
		unsigned long outputs = state->combiner_state[(alpha ? _state_alpha_outputs : _state_rgb_outputs) + stage];
		unsigned long flags = outputs >> 12;
		unsigned long ab_register = (outputs >> 4) & 0x0f;
		unsigned long cd_register = outputs & 0x0f;
		unsigned long sum_register = (outputs >> 8) & 0x0f;

		if (register_is_destination(ab_register))
		{
			if (alpha)
				work.registers[ab_register].alpha = ab[portion];
			else
			{
				work.registers[ab_register].rgb = ab[portion];
				if (flags & 0x80)
					make_blue(&work.registers[ab_register].alpha, &ab[portion]);
			}
		}
		if (register_is_destination(cd_register))
		{
			if (alpha)
				work.registers[cd_register].alpha = cd[portion];
			else
			{
				work.registers[cd_register].rgb = cd[portion];
				if (flags & 0x40)
					make_blue(&work.registers[cd_register].alpha, &cd[portion]);
			}
		}
		if (register_is_destination(sum_register))
		{
			if (alpha)
				work.registers[sum_register].alpha = sum[portion];
			else
				work.registers[sum_register].rgb = sum[portion];
		}
	}
}

static const float substitute_colors[NUMBER_OF_NV2A_FF_SUBSTITUTES][4] =
{
	{ 0.0f, 0.0f, 0.0f, 0.0f },
	{ 0.0f, 0.0f, 0.0f, 0.0f },
	{ 1.0f, 1.0f, 1.0f, 1.0f },
	{ 0.5f, 0.5f, 0.5f, 0.5f },
	{ 0.5f, 0.5f, 1.0f, 1.0f },
};

/* what kind of stage it is for the plan: 0 its texel is zero, 1 a
sampler's answer that a texture unit can give too, 2 a sampler's answer
that it cannot */
static int stage_kind(const struct nv2a_ff_state *state, const struct nv2a_ff_capabilities *capabilities, int stage)
{
	unsigned long mode = stage_mode(state, stage);
	int sign = state->color_sign[stage];

	switch (mode)
	{
	case _mode_none:
	case _mode_clipplane:
	case _mode_dot_product:
	case _mode_dot_zw:
	case _mode_brdf:
		return 0;
	case _mode_project2d:
		if (state->sampler_type[stage] != _sampler_2d)
			return 2;
		break;
	case _mode_cubemap:
		if (state->sampler_type[stage] != _sampler_cube || !capabilities->cube_maps)
			return 2;
		break;
	default:
		return mode > _mode_dot_reflect_specular_constant ? 0 : 2;
	}
	/* signed colours: all three channels or none */
	if ((sign & 0xe) != 0 && (sign & 0xe) != 0xe)
		return 2;
	return 1;
}

static int polynomial_has_atom(const struct polynomial *polynomial, int wanted)
{
	int index, atom, operand;

	for (index = 0; index < polynomial->count; index++)
	{
		for (atom = 0; atom < polynomial->terms[index].count; atom++)
		{
			int candidate = polynomial->terms[index].atoms[atom];

			if (candidate == wanted)
				return TRUE;
			if (candidate >= LEAF_ATOM_COUNT)
			{
				const struct opaque_atom *opaque = &work.opaque[candidate - LEAF_ATOM_COUNT];

				for (operand = 0; operand < 3; operand++)
				{
					if (opaque->operands[operand] >= 0 &&
						polynomial_has_atom(&work.store[opaque->operands[operand]], wanted))
					{
						return TRUE;
					}
				}
			}
		}
	}
	return FALSE;
}

/* the whole state into work.goal_rgb and work.goal_alpha (both before
the last clamp to 0..1) */
static void symbolic_evaluate(void)
{
	const struct nv2a_ff_state *state = work.state;
	unsigned long combiner_count = state->combiner_state[_state_combiner_count] & 0xff;
	unsigned long final_abcd = state->combiner_state[_state_final_inputs_abcd];
	unsigned long final_efg = state->combiner_state[_state_final_inputs_efg];
	int stage, index;

	work.store_count = 0;
	work.opaque_count = 0;
	work.overflow = FALSE;
	work.approximations = 0;
	work.kill_stages = 0;
	memset(work.failures, 0, sizeof(work.failures));
	memset(work.splits, 0, sizeof(work.splits));
	for (index = 0; index < NUMBER_OF_REGISTERS; index++)
	{
		polynomial_zero(&work.registers[index].rgb);
		polynomial_zero(&work.registers[index].alpha);
	}
	/* (zero until the final combiner computes them, as in the reference) */
	polynomial_zero(&work.ef_product.rgb);
	polynomial_zero(&work.ef_product.alpha);
	polynomial_zero(&work.v1r0_sum.rgb);
	polynomial_zero(&work.v1r0_sum.alpha);
	polynomial_atom(&work.registers[_register_v0].rgb, NV2A_FF_ATOM(_nv2a_ff_atom_v0, _nv2a_ff_channel_rgb));
	polynomial_atom(&work.registers[_register_v0].alpha, NV2A_FF_ATOM(_nv2a_ff_atom_v0, _nv2a_ff_channel_alpha));
	polynomial_atom(&work.registers[_register_v1].rgb, NV2A_FF_ATOM(_nv2a_ff_atom_v1, _nv2a_ff_channel_rgb));
	polynomial_atom(&work.registers[_register_v1].alpha, NV2A_FF_ATOM(_nv2a_ff_atom_v1, _nv2a_ff_channel_alpha));

	for (stage = 0; stage < NV2A_FF_STAGE_COUNT; stage++)
	{
		struct symbolic_register *texel = &work.registers[_register_t0 + stage];
		unsigned long mode = stage_mode(state, stage);
		int kind = stage_kind(state, work.capabilities, stage);
		int sign = state->color_sign[stage];

		work.fetches[stage] = (unsigned char)(kind != 0);
		work.available[stage] = (unsigned char)(kind == 1);
		if (mode == _mode_clipplane)
			work.approximations |= _nv2a_ff_approximate_clip_plane;
		if (kind != 0 && work.substitute[stage])
		{
			float color[4];

			memcpy(color, substitute_colors[work.substitute[stage]], sizeof(color));
			if (sign & 1)
				color[3] = signed_byte(color[3]);
			if (sign & 2)
				color[0] = signed_byte(color[0]);
			if (sign & 4)
				color[1] = signed_byte(color[1]);
			if (sign & 8)
				color[2] = signed_byte(color[2]);
			polynomial_literal(&texel->rgb, color[0], color[1], color[2]);
			polynomial_literal(&texel->alpha, color[3], color[3], color[3]);
			work.approximations |= _nv2a_ff_approximate_dropped_texture;
			work.available[stage] = FALSE;
		}
		else if (kind != 0)
		{
			polynomial_atom(&texel->rgb, NV2A_FF_ATOM(stage, _nv2a_ff_channel_rgb));
			polynomial_atom(&texel->alpha, NV2A_FF_ATOM(stage, _nv2a_ff_channel_alpha));
			if (sign & 0xe)
			{
				struct polynomial plain = texel->rgb;

				polynomial_atom(&texel->rgb, opaque_atom_get(_operation_signed_byte, &plain, NULL, NULL));
			}
			if (sign & 1)
			{
				struct polynomial plain = texel->alpha;

				polynomial_atom(&texel->alpha, opaque_atom_get(_operation_signed_byte, &plain, NULL, NULL));
			}
		}
		if (state->alpha_kill[stage] && mode != _mode_none)
		{
			if (kind != 0 && !work.substitute[stage])
				work.kill_stages |= 1 << stage;
			else
				work.approximations |= _nv2a_ff_approximate_alpha_kill;
		}
	}

	/* the fog register: rgb is the fog color, alpha the fog factor */
	{
		struct symbolic_register fog_color;

		symbolic_constant(&fog_color, _nv2a_ff_atom_fog);
		work.registers[_register_fog].rgb = fog_color.rgb;
		if (state->fog_enable)
			polynomial_atom(&work.registers[_register_fog].alpha, NV2A_FF_ATOM(_nv2a_ff_atom_fog, _nv2a_ff_channel_alpha));
		else
			polynomial_literal(&work.registers[_register_fog].alpha, 1.0f, 1.0f, 1.0f);
	}
	work.registers[_register_r0].alpha = work.registers[_register_t0].alpha;

	if (combiner_count > 8)
		combiner_count = 8;
	for (stage = 0; stage < (int)combiner_count; stage++)
		symbolic_stage(stage);

	if (final_abcd == 0 && final_efg == 0)
	{
		work.goal_rgb = work.registers[_register_r0].rgb;
		work.goal_alpha = work.registers[_register_r0].alpha;
	}
	else
	{
		unsigned long settings = final_efg & 0xff;
		struct polynomial e, f, a, b, c, d, v1, r0, inverse_a, product_ab, product_ac, sum;

		symbolic_input(&e, (final_efg >> 24) & 0xff, FALSE, -1, FALSE, FALSE);
		symbolic_input(&f, (final_efg >> 16) & 0xff, FALSE, -1, FALSE, FALSE);
		make_product(&work.ef_product.rgb, &e, &f);
		polynomial_zero(&work.ef_product.alpha);

		make_clamp_unit(&v1, &work.registers[_register_v1].rgb);
		make_clamp_unit(&r0, &work.registers[_register_r0].rgb);
		if (settings & 0x40)
		{
			sum = v1;
			polynomial_affine(&v1, &sum, -1.0f, 1.0f);
		}
		if (settings & 0x20)
		{
			sum = r0;
			polynomial_affine(&r0, &sum, -1.0f, 1.0f);
		}
		make_sum(&sum, &v1, &r0);
		if (settings & 0x80)
			make_clamp_unit(&work.v1r0_sum.rgb, &sum);
		else
			work.v1r0_sum.rgb = sum;
		polynomial_zero(&work.v1r0_sum.alpha);

		symbolic_input(&a, (final_abcd >> 24) & 0xff, FALSE, -1, FALSE, FALSE);
		symbolic_input(&b, (final_abcd >> 16) & 0xff, FALSE, -1, FALSE, FALSE);
		symbolic_input(&c, (final_abcd >> 8) & 0xff, FALSE, -1, FALSE, FALSE);
		symbolic_input(&d, final_abcd & 0xff, FALSE, -1, FALSE, FALSE);
		symbolic_input(&work.goal_alpha, (final_efg >> 8) & 0xff, TRUE, -1, FALSE, FALSE);
		polynomial_affine(&inverse_a, &a, -1.0f, 1.0f);
		make_product(&product_ab, &a, &b);
		make_product(&product_ac, &inverse_a, &c);
		make_sum(&sum, &product_ab, &product_ac);
		make_sum(&work.goal_rgb, &sum, &d);
	}

	/* the result is clamped to 0..1 in the end: a clamp of the whole
	polynomial is that clamp */
	for (index = 0; index < 2; index++)
	{
		struct polynomial *goal = index ? &work.goal_alpha : &work.goal_rgb;

		if (goal->count == 1 && goal->terms[0].count == 1 && goal->terms[0].atoms[0] >= LEAF_ATOM_COUNT &&
			work.opaque[goal->terms[0].atoms[0] - LEAF_ATOM_COUNT].operation == _operation_clamp_unit &&
			fabs(goal->terms[0].coefficient[0] - 1.0f) < COEFFICIENT_EPSILON &&
			fabs(goal->terms[0].coefficient[1] - 1.0f) < COEFFICIENT_EPSILON &&
			fabs(goal->terms[0].coefficient[2] - 1.0f) < COEFFICIENT_EPSILON)
		{
			struct polynomial inside = work.store[work.opaque[goal->terms[0].atoms[0] - LEAF_ATOM_COUNT].operands[0]];

			*goal = inside;
		}
	}

	if (state->fog_enable && state->fog_table_mode != _fog_table_none)
	{
		int fog_atom = NV2A_FF_ATOM(_nv2a_ff_atom_fog, _nv2a_ff_channel_alpha);

		if (polynomial_has_atom(&work.goal_rgb, fog_atom) || polynomial_has_atom(&work.goal_alpha, fog_atom))
			work.approximations |= _nv2a_ff_approximate_fog_table;
	}
}

/* ---------- taking polynomials apart */

/* an atom belongs to a class when it depends on nothing outside allowed
and, if required is not zero, on something in required */
static int atom_in_class(int atom, int allowed, int required)
{
	int dependencies = atom_dependencies(atom);

	return (dependencies & ~allowed) == 0 && (required == 0 || (dependencies & required) != 0);
}

/* polynomial = factor * rest, where factor holds the atoms of the class
and rest the others. FALSE when the polynomial is not such a product or
has no atom of the class. With normalize, a number in front of the factor
moves to the rest. */
static int polynomial_factor(const struct polynomial *polynomial, int allowed, int required, int normalize,
	struct polynomial *factor, struct polynomial *rest)
{
	struct polynomial product;
	struct nv2a_ff_term pivot_class, pivot_rest;
	int index, atom, channel;
	int any = FALSE;

	if (polynomial->count == 0)
		return FALSE;
	factor->count = 0;
	rest->count = 0;
	memset(&pivot_class, 0, sizeof(pivot_class));
	memset(&pivot_rest, 0, sizeof(pivot_rest));
	for (index = 0; index < polynomial->count; index++)
	{
		const struct nv2a_ff_term *term = &polynomial->terms[index];
		struct nv2a_ff_term class_part, rest_part;

		memset(&class_part, 0, sizeof(class_part));
		memset(&rest_part, 0, sizeof(rest_part));
		for (atom = 0; atom < term->count; atom++)
		{
			if (atom_in_class(term->atoms[atom], allowed, required))
			{
				class_part.atoms[class_part.count++] = term->atoms[atom];
				any = TRUE;
			}
			else
			{
				rest_part.atoms[rest_part.count++] = term->atoms[atom];
			}
		}
		if (index == 0)
		{
			pivot_class = class_part;
			pivot_rest = rest_part;
		}
		if (term_compare(&rest_part, &pivot_rest) == 0)
		{
			memcpy(class_part.coefficient, term->coefficient, sizeof(class_part.coefficient));
			factor->terms[factor->count++] = class_part;
		}
		if (term_compare(&class_part, &pivot_class) == 0)
		{
			for (channel = 0; channel < 3; channel++)
			{
				float pivot = polynomial->terms[0].coefficient[channel];

				if (fabs(pivot) < COEFFICIENT_EPSILON)
				{
					if (fabs(term->coefficient[channel]) >= COEFFICIENT_EPSILON)
						return FALSE;
					rest_part.coefficient[channel] = 1.0f;
				}
				else
				{
					rest_part.coefficient[channel] = term->coefficient[channel] / pivot;
				}
			}
			rest->terms[rest->count++] = rest_part;
		}
	}
	if (!any)
		return FALSE;
	factor->count = terms_normalize(factor->terms, factor->count);
	rest->count = terms_normalize(rest->terms, rest->count);
	if (factor->count == 0 || rest->count == 0)
		return FALSE;
	if (normalize)
	{
		const float *leading = factor->terms[0].coefficient;

		if (fabs(leading[0] - leading[1]) < COEFFICIENT_EPSILON && fabs(leading[0] - leading[2]) < COEFFICIENT_EPSILON &&
			fabs(leading[0]) > COEFFICIENT_EPSILON && fabs(leading[0] - 1.0f) > COEFFICIENT_EPSILON)
		{
			float number = leading[0];

			polynomial_scale(factor, 1.0f / number, 1.0f / number, 1.0f / number);
			polynomial_scale(rest, number, number, number);
		}
	}
	if (!polynomial_multiply(&product, factor, rest))
		return FALSE;
	return polynomial_equal(&product, polynomial);
}

/* polynomial = part + rest, where part holds the terms whose atoms all
belong to the class (and, with required, that have such an atom at all) */
static void polynomial_split(const struct polynomial *polynomial, int allowed, int required,
	struct polynomial *part, struct polynomial *rest)
{
	int index, atom;

	part->count = 0;
	rest->count = 0;
	for (index = 0; index < polynomial->count; index++)
	{
		const struct nv2a_ff_term *term = &polynomial->terms[index];
		int dependencies = 0;

		for (atom = 0; atom < term->count; atom++)
			dependencies |= atom_dependencies(term->atoms[atom]);
		if ((dependencies & ~allowed) == 0 && (required == 0 || (dependencies & required) != 0))
			part->terms[part->count++] = *term;
		else
			rest->terms[rest->count++] = *term;
	}
}

/* polynomial = constant_part + atom * linear_part; FALSE when the atom
is absent or occurs squared */
static int polynomial_linear(const struct polynomial *polynomial, int wanted, struct polynomial *constant_part,
	struct polynomial *linear_part)
{
	int index, atom;

	constant_part->count = 0;
	linear_part->count = 0;
	for (index = 0; index < polynomial->count; index++)
	{
		const struct nv2a_ff_term *term = &polynomial->terms[index];
		int occurrences = 0;

		for (atom = 0; atom < term->count; atom++)
		{
			if (term->atoms[atom] == wanted)
				occurrences++;
		}
		if (occurrences > 1)
			return FALSE;
		if (occurrences == 0)
		{
			constant_part->terms[constant_part->count++] = *term;
		}
		else
		{
			struct nv2a_ff_term *reduced = &linear_part->terms[linear_part->count++];
			int kept = 0;

			*reduced = *term;
			memset(reduced->atoms, 0, sizeof(reduced->atoms));
			for (atom = 0; atom < term->count; atom++)
			{
				if (term->atoms[atom] != wanted)
					reduced->atoms[kept++] = term->atoms[atom];
			}
			reduced->count = (unsigned char)kept;
		}
	}
	linear_part->count = terms_normalize(linear_part->terms, linear_part->count);
	return linear_part->count > 0;
}

static int polynomial_is_first_degree(const struct polynomial *polynomial, int dependencies);

/* an atom that depends on these things is of first degree in them: a
plain one, or the dot product of something of first degree with something
that does not depend on them (a colour's channel picked by a constant) */
static int atom_is_first_degree(int atom, int dependencies)
{
	const struct opaque_atom *opaque;
	const struct polynomial *a, *b;
	int depends_a, depends_b;

	if (atom < LEAF_ATOM_COUNT)
		return TRUE;
	opaque = &work.opaque[atom - LEAF_ATOM_COUNT];
	if (opaque->operation == _operation_identity)
		return polynomial_is_first_degree(&work.store[opaque->operands[0]], dependencies);
	if (opaque->operation != _operation_dot)
		return FALSE;
	a = &work.store[opaque->operands[0]];
	b = &work.store[opaque->operands[1]];
	depends_a = (polynomial_dependencies(a) & dependencies) != 0;
	depends_b = (polynomial_dependencies(b) & dependencies) != 0;
	if (depends_a && depends_b)
		return FALSE;
	return polynomial_is_first_degree(depends_a ? a : b, dependencies);
}

/* every term has at most one atom that depends on these things, and that
atom is of first degree in them: interpolating or filtering the inputs
gives the same as interpolating or filtering the value */
static int polynomial_is_first_degree(const struct polynomial *polynomial, int dependencies)
{
	int index, atom;

	for (index = 0; index < polynomial->count; index++)
	{
		const struct nv2a_ff_term *term = &polynomial->terms[index];
		int count = 0;

		for (atom = 0; atom < term->count; atom++)
		{
			if (atom_dependencies(term->atoms[atom]) & dependencies)
			{
				if (!atom_is_first_degree(term->atoms[atom], dependencies))
					return FALSE;
				count++;
			}
		}
		if (count > 1)
			return FALSE;
	}
	return TRUE;
}

/* The polynomial with its clamps taken away: every atom that is a clamp
of something no texture and no primary colour can hold (it reads two
textures, or a texture and a vertex colour) replaced by what it clamps,
multiplied out. FALSE when that does not fit. */
static int polynomial_declamp(struct polynomial *result, const struct polynomial *polynomial, int depth)
{
	struct polynomial sum, product, factor, next;
	int index, atom;

	polynomial_zero(&sum);
	for (index = 0; index < polynomial->count; index++)
	{
		const struct nv2a_ff_term *term = &polynomial->terms[index];

		polynomial_literal(&product, term->coefficient[0], term->coefficient[1], term->coefficient[2]);
		for (atom = 0; atom < term->count; atom++)
		{
			int operation = term->atoms[atom] >= LEAF_ATOM_COUNT ?
				work.opaque[term->atoms[atom] - LEAF_ATOM_COUNT].operation : -1;

			int dependencies = atom_dependencies(term->atoms[atom]);
			int textures = dependencies & _depends_textures;

			if ((operation == _operation_maximum_zero || operation == _operation_clamp_unit ||
				operation == _operation_clamp_signed || operation == _operation_identity) &&
				textures && ((textures & (textures - 1)) || (dependencies & _depends_vertex)))
			{
				struct polynomial inside = work.store[work.opaque[term->atoms[atom] - LEAF_ATOM_COUNT].operands[0]];

				if (depth >= 8 || !polynomial_declamp(&factor, &inside, depth + 1))
					return FALSE;
			}
			else
			{
				polynomial_atom(&factor, term->atoms[atom]);
			}
			if (!polynomial_multiply(&next, &product, &factor))
				return FALSE;
			product = next;
		}
		if (!polynomial_add(&next, &sum, &product))
			return FALSE;
		sum = next;
	}
	*result = sum;
	return TRUE;
}

/* ---------- fitting one pass */

/* The search is bounded by counting the candidates it looks at (ways to
compute a polynomial in one unit, and ways to split one into passes): so
many for one pass, for one round of an attempt, for one search and for
each of a search's two last resorts. Nothing new is begun once a limit is
passed, so a search ends within SEARCH_STEP_LIMIT + 2 * RESORT_STEP_LIMIT
candidates and what was left of the pass it was fitting each of the three
times (under 2000 candidates): 16000 at the very most, and twice that
with combine environments (compile_search). A compile makes one search
for all constants (none, when the last compile made it) and, given
constants, up to two more for their values. What that comes to in time is
printed by the test (port/win9x/tests/renderer/combiner_ff_test.c). */
enum
{
	STEP_LIMIT = 1500,
	TOTAL_STEP_LIMIT = 6000,
	SEARCH_STEP_LIMIT = 6000,
	RESORT_STEP_LIMIT = 2000,

	OPERAND_COLOR = 1 << _nv2a_ff_operand_color,
	OPERAND_INVERSE_COLOR = 1 << _nv2a_ff_operand_inverse_color,
	OPERAND_ALPHA = 1 << _nv2a_ff_operand_alpha,
	OPERAND_INVERSE_ALPHA = 1 << _nv2a_ff_operand_inverse_alpha,
	OPERANDS_COLOR = OPERAND_COLOR | OPERAND_INVERSE_COLOR,
	OPERANDS_ALPHA = OPERAND_ALPHA | OPERAND_INVERSE_ALPHA,
	OPERANDS_ALL = OPERANDS_COLOR | OPERANDS_ALPHA
};

enum
{
	_slot_texture = 0,
	_slot_constant,
	_slot_primary,
	_slot_previous
};

/* the present search has looked at as many candidates as it may */
static int budget_spent(void)
{
	return work.statistics.steps - work.step_base > work.step_budget;
}

static void slots_undo(int mark)
{
	while (work.log_count > mark)
		work.log[--work.log_count]->bound = FALSE;
}

static int slot_set(struct slot *slot, const struct polynomial *value)
{
	if (work.log_count == (int)(sizeof(work.log) / sizeof(work.log[0])))
		return FALSE;
	slot->bound = TRUE;
	slot->value = *value;
	work.log[work.log_count++] = slot;
	return TRUE;
}

static void stage_identity(int stage, struct polynomial *rgb, struct polynomial *alpha)
{
	polynomial_atom(rgb, NV2A_FF_ATOM(stage, _nv2a_ff_channel_rgb));
	polynomial_atom(alpha, NV2A_FF_ATOM(stage, _nv2a_ff_channel_alpha));
}

/* what the primary colour may hold */
static int primary_valid(const struct polynomial *value)
{
	if (polynomial_dependencies(value) & _depends_textures)
		return FALSE;
	if (!work.strict_primary)
		return TRUE;
	return polynomial_is_first_degree(value, _depends_vertex) && polynomial_within_unit(value);
}

/* The result a unit must leave for the next one: OpenGL clamps it to
0..1, so it must stay in that range by itself or be a clamp, which the
unit's own clamp then performs. */
static int previous_valid(const struct polynomial *value)
{
	if (value->count == 1 && value->terms[0].count == 1 && value->terms[0].atoms[0] >= LEAF_ATOM_COUNT &&
		work.opaque[value->terms[0].atoms[0] - LEAF_ATOM_COUNT].operation == _operation_clamp_unit &&
		fabs(value->terms[0].coefficient[0] - 1.0f) < COEFFICIENT_EPSILON &&
		fabs(value->terms[0].coefficient[1] - 1.0f) < COEFFICIENT_EPSILON &&
		fabs(value->terms[0].coefficient[2] - 1.0f) < COEFFICIENT_EPSILON)
	{
		return TRUE;
	}
	return polynomial_within_unit(value);
}

/* the polynomial a unit has to compute so that the next one finds value:
what is inside a clamp, or value itself */
static void previous_goal(const struct polynomial *value, struct polynomial *goal)
{
	*goal = *value;
	if (value->count == 1 && value->terms[0].count == 1 && value->terms[0].atoms[0] >= LEAF_ATOM_COUNT &&
		work.opaque[value->terms[0].atoms[0] - LEAF_ATOM_COUNT].operation == _operation_clamp_unit &&
		fabs(value->terms[0].coefficient[0] - 1.0f) < COEFFICIENT_EPSILON &&
		fabs(value->terms[0].coefficient[1] - 1.0f) < COEFFICIENT_EPSILON &&
		fabs(value->terms[0].coefficient[2] - 1.0f) < COEFFICIENT_EPSILON)
	{
		*goal = work.store[work.opaque[value->terms[0].atoms[0] - LEAF_ATOM_COUNT].operands[0]];
	}
}

/* what a unit's texture may hold: the stage's own colour or alpha, or a
derived texture the policy allows */
static int texture_valid(const struct unit_build *unit, const struct polynomial *value, int alpha_slot)
{
	struct polynomial identity_rgb, identity_alpha;
	int dependencies = polynomial_dependencies(value);

	if (unit->stage < 0)
		return FALSE;
	stage_identity(unit->stage, &identity_rgb, &identity_alpha);
	if (polynomial_equal(value, alpha_slot ? &identity_alpha : &identity_rgb))
		return TRUE;
	if (dependencies & ~((_depends_stage0 << unit->stage) | _depends_constants))
		return FALSE;
	if (work.bake_policy == _nv2a_ff_bakes_none)
		return FALSE;
	if (work.bake_policy == _nv2a_ff_bakes_static && (dependencies & _depends_constants))
		return FALSE;
	if (alpha_slot && polynomial_is_vector(value))
		return FALSE;
	return polynomial_within_unit(value);
}

static int constant_valid(const struct polynomial *value)
{
	return (polynomial_dependencies(value) & ~_depends_constants) == 0 && polynomial_within_unit(value);
}

static struct slot *unit_slot(int unit_index, int kind, int alpha_slot)
{
	struct unit_build *unit = &work.build.units[unit_index];

	switch (kind)
	{
	case _slot_texture:
		return alpha_slot ? &unit->texture_alpha : &unit->texture_rgb;
	case _slot_constant:
		return alpha_slot ? &unit->constant_alpha : &unit->constant_rgb;
	case _slot_previous:
		/* the first unit's previous colour is the primary colour */
		if (unit_index > 0)
			return alpha_slot ? &unit->previous_alpha : &unit->previous_rgb;
		/* fall through */
	default:
		return alpha_slot ? &work.build.primary_alpha : &work.build.primary_rgb;
	}
}

static int slot_content_valid(int unit_index, int kind, int alpha_slot, const struct polynomial *value)
{
	switch (kind)
	{
	case _slot_texture: return texture_valid(&work.build.units[unit_index], value, alpha_slot);
	case _slot_constant: return constant_valid(value) && (!alpha_slot || !polynomial_is_vector(value));
	case _slot_previous:
		if (unit_index > 0)
			return previous_valid(value) && (!alpha_slot || !polynomial_is_vector(value));
		/* fall through */
	default:
		return primary_valid(value) && (!alpha_slot || !polynomial_is_vector(value));
	}
}

/* the slot must hold exactly this value */
static int slot_require(int unit_index, int kind, int alpha_slot, const struct polynomial *value)
{
	struct slot *slot = unit_slot(unit_index, kind, alpha_slot);

	if (slot->bound)
		return polynomial_equal(&slot->value, value);
	return slot_content_valid(unit_index, kind, alpha_slot, value) && slot_set(slot, value);
}

/* Makes value available as an argument from one of the two slots of a
kind (colour and alpha), through one of the allowed operands: a slot that
already holds it or its complement, or a free slot. */
static int argument_bind(int unit_index, int kind, const struct polynomial *value, int alpha_side,
	unsigned allowed, unsigned char *operand)
{
	struct unit_build *unit = &work.build.units[unit_index];
	struct polynomial complement, identity_rgb, identity_alpha;
	int scalar = !polynomial_is_vector(value);
	int order[2];
	int round;

	polynomial_affine(&complement, value, -1.0f, 1.0f);
	if (alpha_side)
		allowed &= OPERANDS_ALPHA;
	if (!scalar)
		allowed &= OPERANDS_COLOR;

	/* a slot that is in use already */
	for (round = 0; round < 2; round++)
	{
		struct slot *slot = unit_slot(unit_index, kind, round);

		if (!slot->bound)
			continue;
		if ((allowed & (round ? OPERAND_ALPHA : OPERAND_COLOR)) && polynomial_equal(&slot->value, value))
		{
			*operand = (unsigned char)(round ? _nv2a_ff_operand_alpha : _nv2a_ff_operand_color);
			return TRUE;
		}
		if ((allowed & (round ? OPERAND_INVERSE_ALPHA : OPERAND_INVERSE_COLOR)) &&
			polynomial_equal(&slot->value, &complement))
		{
			*operand = (unsigned char)(round ? _nv2a_ff_operand_inverse_alpha : _nv2a_ff_operand_inverse_color);
			return TRUE;
		}
	}

	/* a free slot: alpha first for single values, unless the texture has
	the value in its colour as it is */
	order[0] = scalar && !work.prefer_color_slots;
	if (kind == _slot_texture && unit->stage >= 0)
	{
		stage_identity(unit->stage, &identity_rgb, &identity_alpha);
		if (polynomial_equal(value, &identity_alpha) || polynomial_equal(&complement, &identity_alpha))
			order[0] = 1;
		else if (polynomial_equal(value, &identity_rgb) || polynomial_equal(&complement, &identity_rgb))
			order[0] = 0;
	}
	order[1] = !order[0];
	for (round = 0; round < 2; round++)
	{
		int alpha_slot = order[round];
		struct slot *slot = unit_slot(unit_index, kind, alpha_slot);
		unsigned direct = alpha_slot ? OPERAND_ALPHA : OPERAND_COLOR;
		unsigned inverse = alpha_slot ? OPERAND_INVERSE_ALPHA : OPERAND_INVERSE_COLOR;

		if (slot->bound)
			continue;
		if (kind == _slot_texture && unit->stage >= 0 && (allowed & inverse) &&
			polynomial_equal(&complement, alpha_slot ? &identity_alpha : &identity_rgb))
		{
			/* 1 - texture rather than a derived texture */
			if (slot_set(slot, &complement))
			{
				*operand = (unsigned char)(alpha_slot ? _nv2a_ff_operand_inverse_alpha : _nv2a_ff_operand_inverse_color);
				return TRUE;
			}
		}
		if ((allowed & direct) && slot_content_valid(unit_index, kind, alpha_slot, value) && slot_set(slot, value))
		{
			*operand = (unsigned char)(alpha_slot ? _nv2a_ff_operand_alpha : _nv2a_ff_operand_color);
			return TRUE;
		}
		if ((allowed & inverse) && !(allowed & direct) && slot_content_valid(unit_index, kind, alpha_slot, &complement) &&
			slot_set(slot, &complement))
		{
			*operand = (unsigned char)(alpha_slot ? _nv2a_ff_operand_inverse_alpha : _nv2a_ff_operand_inverse_color);
			return TRUE;
		}
	}
	return FALSE;
}

/* value as an argument from wherever it can come: the constant, the
primary colour, the unit's texture or the unit before */
static int argument_any(int unit_index, const struct polynomial *value, int alpha_side, unsigned allowed,
	unsigned char *source, unsigned char *operand)
{
	const struct unit_build *unit = &work.build.units[unit_index];
	int dependencies = polynomial_dependencies(value);

	if (!(dependencies & (_depends_textures | _depends_vertex)))
	{
		*source = _nv2a_ff_source_constant;
		if (argument_bind(unit_index, _slot_constant, value, alpha_side, allowed, operand))
			return TRUE;
		*source = _nv2a_ff_source_primary;
		return argument_bind(unit_index, _slot_primary, value, alpha_side, allowed, operand);
	}
	if (!(dependencies & _depends_textures))
	{
		*source = _nv2a_ff_source_primary;
		return argument_bind(unit_index, _slot_primary, value, alpha_side, allowed, operand);
	}
	if (unit->stage >= 0 && (dependencies & (_depends_textures | _depends_vertex)) == (_depends_stage0 << unit->stage))
	{
		*source = _nv2a_ff_source_texture;
		if (argument_bind(unit_index, _slot_texture, value, alpha_side, allowed, operand))
			return TRUE;
	}
	if (unit_index == 0)
		return FALSE;
	*source = _nv2a_ff_source_previous;
	return argument_bind(unit_index, _slot_previous, value, alpha_side, allowed, operand);
}

static int fit_unit(int unit_index, const struct polynomial *rgb, const struct polynomial *alpha);

/* the unit is settled: go on with the unit before it */
static int finish_unit(int unit_index)
{
	struct unit_build *unit = &work.build.units[unit_index];
	struct polynomial rgb, alpha;

	if (unit_index == 0)
		return TRUE;
	if (!unit->previous_rgb.bound && !unit->previous_alpha.bound)
		/* the unit before would do nothing: a plan with fewer units finds this */
		return FALSE;
	if (unit->previous_rgb.bound)
		previous_goal(&unit->previous_rgb.value, &rgb);
	if (unit->previous_alpha.bound)
		previous_goal(&unit->previous_alpha.value, &alpha);
	return fit_unit(unit_index - 1, unit->previous_rgb.bound ? &rgb : NULL, unit->previous_alpha.bound ? &alpha : NULL);
}

/* the classes a unit's own arguments come from */
enum
{
	_class_texture = 0,
	_class_primary,
	_class_constant,
	_class_texture_and_constants,
	NUMBER_OF_CLASSES
};

static int class_dependencies(const struct unit_build *unit, int class_index, int *allowed, int *required, int *kind)
{
	switch (class_index)
	{
	case _class_texture:
		if (unit->stage < 0)
			return FALSE;
		*allowed = *required = _depends_stage0 << unit->stage;
		*kind = _slot_texture;
		return TRUE;
	case _class_primary:
		*allowed = _depends_vertex | _depends_constants;
		*required = _depends_vertex;
		*kind = _slot_primary;
		return TRUE;
	case _class_constant:
		*allowed = *required = _depends_constants;
		*kind = _slot_constant;
		return TRUE;
	default:
		if (unit->stage < 0 || work.bake_policy != _nv2a_ff_bakes_all)
			return FALSE;
		*allowed = (_depends_stage0 << unit->stage) | _depends_constants;
		*required = 0;
		*kind = _slot_texture;
		return TRUE;
	}
}

static void combine_reset(struct nv2a_ff_combine *combine, int alpha_side)
{
	int argument;

	combine->function = _nv2a_ff_combine_replace;
	combine->scale = 1;
	for (argument = 0; argument < 3; argument++)
	{
		combine->source[argument] = _nv2a_ff_source_previous;
		combine->operand[argument] = (unsigned char)(alpha_side ? _nv2a_ff_operand_alpha : _nv2a_ff_operand_color);
	}
}

/* Tries every way one side (colour or alpha) of a combine environment
can compute goal; for each, goes on with next. */
static int channel_candidates(int unit_index, int alpha_side, const struct polynomial *goal,
	int (*next)(int unit_index))
{
	struct unit_build *unit = &work.build.units[unit_index];
	struct nv2a_ff_combine *combine = alpha_side ? &unit->alpha : &unit->rgb;
	const struct nv2a_ff_capabilities *capabilities = work.capabilities;
	unsigned all = alpha_side ? OPERANDS_ALPHA : OPERANDS_ALL;
	int mark = work.log_count;
	int scale, class_index, shape;

	combine_reset(combine, alpha_side);
	if (!goal)
		return next(unit_index);

	for (scale = 1; scale <= 4; scale *= 2)
	{
		struct polynomial scaled = *goal;
		struct polynomial part, rest, shifted;
		unsigned char atoms[12];
		int atom_count = 0, index, atom;

		if (scale > 1)
			polynomial_scale(&scaled, 1.0f / (float)scale, 1.0f / (float)scale, 1.0f / (float)scale);
		if (++work.steps > STEP_LIMIT)
			return FALSE;

		/* replace */
		combine_reset(combine, alpha_side);
		combine->scale = (unsigned char)scale;
		if (argument_any(unit_index, &scaled, alpha_side, all, &combine->source[0], &combine->operand[0]) &&
			next(unit_index))
		{
			return TRUE;
		}
		slots_undo(mark);

		/* modulate: argument 0 from one class, argument 1 the rest */
		for (class_index = 0; class_index < NUMBER_OF_CLASSES; class_index++)
		{
			int allowed, required, kind;

			if (!class_dependencies(unit, class_index, &allowed, &required, &kind))
				continue;
			if (!polynomial_factor(&scaled, allowed, required, kind == _slot_texture, &part, &rest))
				continue;
			if (polynomial_is_number(&rest, 1.0f))
				continue;
			work.steps++;
			combine_reset(combine, alpha_side);
			combine->scale = (unsigned char)scale;
			combine->function = _nv2a_ff_combine_modulate;
			combine->source[0] = (unsigned char)(kind == _slot_texture ? _nv2a_ff_source_texture :
				kind == _slot_primary ? _nv2a_ff_source_primary : _nv2a_ff_source_constant);
			if (argument_bind(unit_index, kind, &part, alpha_side, all, &combine->operand[0]) &&
				argument_any(unit_index, &rest, alpha_side, all, &combine->source[1], &combine->operand[1]) &&
				next(unit_index))
			{
				return TRUE;
			}
			slots_undo(mark);
		}

		/* add, add signed, subtract: argument 0 from one class, argument 1 the rest */
		for (shape = 0; shape < 4; shape++)
		{
			const struct polynomial *sum = &scaled;

			if (shape == 1)
			{
				polynomial_affine(&shifted, &scaled, 1.0f, 0.5f);
				sum = &shifted;
			}
			if (shape >= 2 && !capabilities->combine_arb)
				break;
			for (class_index = 0; class_index < NUMBER_OF_CLASSES; class_index++)
			{
				int allowed, required, kind;

				if (!class_dependencies(unit, class_index, &allowed, &required, &kind))
					continue;
				/* numbers belong to the constant */
				polynomial_split(sum, allowed, kind == _slot_constant ? 0 : required, &part, &rest);
				if (part.count == 0 || rest.count == 0)
					continue;
				work.steps++;
				combine_reset(combine, alpha_side);
				combine->scale = (unsigned char)scale;
				combine->source[0] = (unsigned char)(kind == _slot_texture ? _nv2a_ff_source_texture :
					kind == _slot_primary ? _nv2a_ff_source_primary : _nv2a_ff_source_constant);
				if (shape < 2)
				{
					combine->function = (unsigned char)(shape == 0 ? _nv2a_ff_combine_add : _nv2a_ff_combine_add_signed);
					if (argument_bind(unit_index, kind, &part, alpha_side, all, &combine->operand[0]) &&
						argument_any(unit_index, &rest, alpha_side, all, &combine->source[1], &combine->operand[1]) &&
						next(unit_index))
					{
						return TRUE;
					}
				}
				else if (shape == 2)
				{
					/* part - (-rest) */
					combine->function = _nv2a_ff_combine_subtract;
					polynomial_scale(&rest, -1.0f, -1.0f, -1.0f);
					if (argument_bind(unit_index, kind, &part, alpha_side, all, &combine->operand[0]) &&
						argument_any(unit_index, &rest, alpha_side, all, &combine->source[1], &combine->operand[1]) &&
						next(unit_index))
					{
						return TRUE;
					}
				}
				else
				{
					/* rest - (-part) */
					combine->function = _nv2a_ff_combine_subtract;
					polynomial_scale(&part, -1.0f, -1.0f, -1.0f);
					combine->source[1] = combine->source[0];
					if (argument_bind(unit_index, kind, &part, alpha_side, all, &combine->operand[1]) &&
						argument_any(unit_index, &rest, alpha_side, all, &combine->source[0], &combine->operand[0]) &&
						next(unit_index))
					{
						return TRUE;
					}
				}
				slots_undo(mark);
			}
		}

		/* interpolate: a0 * a2 + a1 * (1 - a2), for every atom as a2 */
		for (index = 0; index < scaled.count; index++)
		{
			for (atom = 0; atom < scaled.terms[index].count; atom++)
			{
				int known;

				for (known = 0; known < atom_count && atoms[known] != scaled.terms[index].atoms[atom]; known++)
					;
				if (known == atom_count && atom_count < (int)sizeof(atoms))
					atoms[atom_count++] = scaled.terms[index].atoms[atom];
			}
		}
		for (index = 0; index < atom_count; index++)
		{
			struct polynomial factor, both;
			int orientation;
			unsigned factor_operands;

			if (!polynomial_linear(&scaled, atoms[index], &rest, &part))
				continue;
			if (!polynomial_add(&both, &rest, &part))
				continue;
			/* EXT_texture_env_combine: argument 2 is SRC_ALPHA only */
			if (capabilities->combine_arb)
				factor_operands = all;
			else
				factor_operands = OPERAND_ALPHA;
			for (orientation = 0; orientation < (capabilities->combine_arb ? 1 : 2); orientation++)
			{
				const struct polynomial *argument0 = orientation ? &rest : &both;
				const struct polynomial *argument1 = orientation ? &both : &rest;

				polynomial_atom(&factor, atoms[index]);
				if (orientation)
				{
					struct polynomial plain = factor;

					polynomial_affine(&factor, &plain, -1.0f, 1.0f);
				}
				work.steps++;
				combine_reset(combine, alpha_side);
				combine->scale = (unsigned char)scale;
				combine->function = _nv2a_ff_combine_interpolate;
				if (argument_any(unit_index, &factor, alpha_side, factor_operands, &combine->source[2], &combine->operand[2]) &&
					argument_any(unit_index, argument0, alpha_side, all, &combine->source[0], &combine->operand[0]) &&
					argument_any(unit_index, argument1, alpha_side, all, &combine->source[1], &combine->operand[1]) &&
					next(unit_index))
				{
					return TRUE;
				}
				slots_undo(mark);
			}
		}

		/* dot3: 4 * dot(a0 - 0.5, a1 - 0.5) */
		if (!alpha_side && capabilities->dot3 && scaled.count == 1 && scaled.terms[0].count == 1 &&
			scaled.terms[0].atoms[0] >= LEAF_ATOM_COUNT)
		{
			const struct opaque_atom *opaque = &work.opaque[scaled.terms[0].atoms[0] - LEAF_ATOM_COUNT];

			if ((opaque->operation == _operation_maximum_zero || opaque->operation == _operation_clamp_unit) &&
				work.store[opaque->operands[0]].count == 1 && work.store[opaque->operands[0]].terms[0].count == 1 &&
				work.store[opaque->operands[0]].terms[0].atoms[0] >= LEAF_ATOM_COUNT &&
				fabs(work.store[opaque->operands[0]].terms[0].coefficient[0] - 1.0f) < COEFFICIENT_EPSILON)
			{
				opaque = &work.opaque[work.store[opaque->operands[0]].terms[0].atoms[0] - LEAF_ATOM_COUNT];
			}
			if (opaque->operation == _operation_dot && fabs(scaled.terms[0].coefficient[0] - 1.0f) < COEFFICIENT_EPSILON &&
				fabs(scaled.terms[0].coefficient[1] - 1.0f) < COEFFICIENT_EPSILON &&
				fabs(scaled.terms[0].coefficient[2] - 1.0f) < COEFFICIENT_EPSILON)
			{
				polynomial_affine(&part, &work.store[opaque->operands[0]], 0.5f, 0.5f);
				polynomial_affine(&rest, &work.store[opaque->operands[1]], 0.5f, 0.5f);
				combine_reset(combine, alpha_side);
				combine->scale = (unsigned char)scale;
				combine->function = _nv2a_ff_combine_dot3;
				if (argument_any(unit_index, &part, FALSE, OPERANDS_COLOR, &combine->source[0], &combine->operand[0]) &&
					argument_any(unit_index, &rest, FALSE, OPERANDS_COLOR, &combine->source[1], &combine->operand[1]) &&
					next(unit_index))
				{
					return TRUE;
				}
				slots_undo(mark);
			}
		}
	}
	return FALSE;
}

static int combine_alpha_next(int unit_index)
{
	return channel_candidates(unit_index, TRUE, work.unit_goal_alpha[unit_index], finish_unit);
}

/* the OpenGL 1.1 environments (and EXT_texture_env_add's), whose colour
and alpha sides come as a pair */
static int legacy_candidates(int unit_index, const struct polynomial *rgb, const struct polynomial *alpha)
{
	struct unit_build *unit = &work.build.units[unit_index];
	int texture = _depends_stage0 << unit->stage;
	int mark = work.log_count;
	struct polynomial factor, rest, alpha_factor, alpha_rest, one, both;
	unsigned char atoms[12];
	int atom_count = 0, index, atom, with_constants, add;

	polynomial_literal(&one, 1.0f, 1.0f, 1.0f);

	/* replace */
	if (unit_index == 0)
	{
		unit->environment = _nv2a_ff_environment_replace;
		if ((!rgb || slot_require(unit_index, _slot_texture, FALSE, rgb)) &&
			(!alpha || slot_require(unit_index, _slot_texture, TRUE, alpha)))
		{
			return TRUE;
		}
		slots_undo(mark);
	}

	/* modulate and add: the alpha side is a product in both */
	for (with_constants = 0; with_constants < (work.bake_policy == _nv2a_ff_bakes_all ? 2 : 1); with_constants++)
	{
		int allowed = texture | (with_constants ? _depends_constants : 0);
		int required = with_constants ? 0 : texture;

		if (alpha && !polynomial_factor(alpha, allowed, required, TRUE, &alpha_factor, &alpha_rest))
		{
			alpha_factor = one;
			alpha_rest = *alpha;
		}
		for (add = 0; add < (work.capabilities->environment_add ? 2 : 1); add++)
		{
			if (++work.steps > STEP_LIMIT)
				return FALSE;
			if (rgb && add)
			{
				polynomial_split(rgb, allowed, required, &factor, &rest);
			}
			else if (rgb && !polynomial_factor(rgb, allowed, required, TRUE, &factor, &rest))
			{
				factor = one;
				rest = *rgb;
			}
			unit->environment = (unsigned char)(add ? _nv2a_ff_environment_add : _nv2a_ff_environment_modulate);
			if ((!rgb || (slot_require(unit_index, _slot_texture, FALSE, &factor) &&
					slot_require(unit_index, _slot_previous, FALSE, &rest))) &&
				(!alpha || (slot_require(unit_index, _slot_texture, TRUE, &alpha_factor) &&
					slot_require(unit_index, _slot_previous, TRUE, &alpha_rest))) &&
				finish_unit(unit_index))
			{
				return TRUE;
			}
			slots_undo(mark);
		}
	}
	if (!rgb)
		return FALSE;

	/* decal and blend: first degree in something the texture holds */
	for (index = 0; index < rgb->count; index++)
	{
		for (atom = 0; atom < rgb->terms[index].count; atom++)
		{
			int candidate = rgb->terms[index].atoms[atom];
			int known;

			if (atom_dependencies(candidate) != texture)
				continue;
			for (known = 0; known < atom_count && atoms[known] != candidate; known++)
				;
			if (known == atom_count && atom_count < (int)sizeof(atoms))
				atoms[atom_count++] = (unsigned char)candidate;
		}
	}
	if (alpha && !polynomial_factor(alpha, texture, texture, TRUE, &alpha_factor, &alpha_rest))
	{
		alpha_factor = one;
		alpha_rest = *alpha;
	}
	for (index = 0; index < atom_count; index++)
	{
		int orientation;

		if (!polynomial_linear(rgb, atoms[index], &rest, &factor) || !polynomial_add(&both, &rest, &factor))
			continue;
		for (orientation = 0; orientation < 2; orientation++)
		{
			/* colour = previous * (1 - t) + other * t */
			const struct polynomial *previous = orientation ? &both : &rest;
			const struct polynomial *other = orientation ? &rest : &both;
			struct polynomial blend_factor;

			if (++work.steps > STEP_LIMIT)
				return FALSE;
			polynomial_atom(&blend_factor, atoms[index]);
			if (orientation)
			{
				struct polynomial plain = blend_factor;

				polynomial_affine(&blend_factor, &plain, -1.0f, 1.0f);
			}

			/* decal: t is the texture's alpha, other its colour; alpha passes through */
			if (!atom_is_vector(atoms[index]))
			{
				unit->environment = _nv2a_ff_environment_decal;
				if (slot_require(unit_index, _slot_texture, TRUE, &blend_factor) &&
					slot_require(unit_index, _slot_texture, FALSE, other) &&
					slot_require(unit_index, _slot_previous, FALSE, previous) &&
					(!alpha || slot_require(unit_index, _slot_previous, TRUE, alpha)) &&
					finish_unit(unit_index))
				{
					return TRUE;
				}
				slots_undo(mark);
			}

			/* blend: t is the texture's colour, other the constant; alpha is a product */
			unit->environment = _nv2a_ff_environment_blend;
			if (slot_require(unit_index, _slot_texture, FALSE, &blend_factor) &&
				slot_require(unit_index, _slot_constant, FALSE, other) &&
				slot_require(unit_index, _slot_previous, FALSE, previous) &&
				(!alpha || (slot_require(unit_index, _slot_texture, TRUE, &alpha_factor) &&
					slot_require(unit_index, _slot_previous, TRUE, &alpha_rest))) &&
				finish_unit(unit_index))
			{
				return TRUE;
			}
			slots_undo(mark);
		}
	}
	return FALSE;
}

/* units 0..unit_index compute these two polynomials (NULL: anything) */
static int fit_unit(int unit_index, const struct polynomial *rgb, const struct polynomial *alpha)
{
	struct unit_build *unit = &work.build.units[unit_index];
	int dependencies = (rgb ? polynomial_dependencies(rgb) : 0) | (alpha ? polynomial_dependencies(alpha) : 0);
	int mark = work.log_count;
	int stage, textures = 0;

	if (++work.steps > STEP_LIMIT)
		return FALSE;
	/* each texture the goals read needs a unit of its own */
	for (stage = 0; stage < NV2A_FF_STAGE_COUNT; stage++)
	{
		if (dependencies & (_depends_stage0 << stage))
			textures++;
	}
	if (textures > unit_index + 1)
		return FALSE;
	for (stage = 0; stage <= NV2A_FF_STAGE_COUNT; stage++)
	{
		if (stage < NV2A_FF_STAGE_COUNT)
		{
			if (!(dependencies & (_depends_stage0 << stage)) || !work.available[stage])
				continue;
			unit->stage = stage;
			if (legacy_candidates(unit_index, rgb, alpha))
				return TRUE;
			slots_undo(mark);
		}
		else
		{
			/* a combine unit that reads no texture */
			unit->stage = -1;
		}
		if (work.capabilities->combine)
		{
			unit->environment = _nv2a_ff_environment_combine;
			work.unit_goal_alpha[unit_index] = alpha;
			if (channel_candidates(unit_index, FALSE, rgb, combine_alpha_next))
				return TRUE;
			slots_undo(mark);
		}
	}
	return FALSE;
}

/* work.build empty, for a pass of so many units. (Not a memset of the
whole: the slots' polynomials are large, and only a bound slot's is read.) */
static void build_reset(int units)
{
	struct pass_build *build = &work.build;
	int unit_index;

	for (unit_index = 0; unit_index < NV2A_FF_MAXIMUM_UNITS; unit_index++)
	{
		struct unit_build *unit = &build->units[unit_index];

		unit->stage = 0;
		unit->texture_rgb.bound = unit->texture_alpha.bound = FALSE;
		unit->constant_rgb.bound = unit->constant_alpha.bound = FALSE;
		unit->previous_rgb.bound = unit->previous_alpha.bound = FALSE;
		unit->environment = 0;
		memset(&unit->rgb, 0, sizeof(unit->rgb));
		memset(&unit->alpha, 0, sizeof(unit->alpha));
	}
	build->primary_rgb.bound = build->primary_alpha.bound = build->secondary_rgb.bound = FALSE;
	build->blend_enable = 0;
	build->source_blend = build->destination_blend = build->blend_operation = 0;
	build->write_color = build->write_alpha = 0;
	build->alpha_test = build->alpha_test_greater_zero = 0;
	build->depth_equal = 0;
	build->fog = build->fog_black = 0;
	build->unit_count = units;
	work.log_count = 0;
}

/* one pass whose units compute rgb and alpha (NULL: not needed) into work.build */
static int fit_pass_units(const struct polynomial *rgb, const struct polynomial *alpha)
{
	int dependencies = (rgb ? polynomial_dependencies(rgb) : 0) | (alpha ? polynomial_dependencies(alpha) : 0);
	unsigned long key = 2166136261UL;
	int variant, units, stage, textures = 0;

	/* each texture needs a unit, and one a unit can sample */
	for (stage = 0; stage < NV2A_FF_STAGE_COUNT; stage++)
	{
		if (dependencies & (_depends_stage0 << stage))
		{
			if (!work.available[stage])
				return FALSE;
			textures++;
		}
	}
	if (textures > work.capabilities->units)
		return FALSE;
	if (budget_spent())
		return FALSE;
	/* a dot product or a multiplexer of two textures: no unit computes
	that, and no derived texture holds it */
	for (variant = 0; variant < 2; variant++)
	{
		const struct polynomial *goal = variant ? alpha : rgb;
		int index, atom;

		for (index = 0; goal && index < goal->count; index++)
		{
			for (atom = 0; atom < goal->terms[index].count; atom++)
			{
				int candidate = goal->terms[index].atoms[atom];
				int operation, mask, count = 0;

				if (candidate < LEAF_ATOM_COUNT)
					continue;
				operation = work.opaque[candidate - LEAF_ATOM_COUNT].operation;
				if (operation != _operation_dot && operation != _operation_mux_msb && operation != _operation_mux_lsb &&
					operation != _operation_signed_byte)
				{
					continue;
				}
				mask = atom_dependencies(candidate);
				for (stage = 0; stage < NV2A_FF_STAGE_COUNT; stage++)
					count += (mask >> stage) & 1;
				if (count > 1 || (count == 1 && (mask & _depends_vertex)))
					return FALSE;
			}
		}
	}

	if (rgb)
		key = polynomial_hash(rgb, key);
	key = (key ^ 0xaaUL) * 16777619UL;
	if (alpha)
		key = polynomial_hash(alpha, key);
	key &= 0xffffffffUL;
	if (work.failures[key % FAILURE_CACHE_SIZE].key == key && work.failures[key % FAILURE_CACHE_SIZE].rank >= work.policy_rank)
		return FALSE;

	work.steps = 4;
	work.statistics.fits++;
	for (variant = 0; variant < 2; variant++)
	{
		work.prefer_color_slots = variant;
		for (units = textures; units <= work.capabilities->units && units <= NV2A_FF_MAXIMUM_UNITS; units++)
		{
			build_reset(units);
			if (units == 0)
			{
				if (variant == 0 && (!rgb || slot_require(0, _slot_primary, FALSE, rgb)) &&
					(!alpha || slot_require(0, _slot_primary, TRUE, alpha)))
				{
					return TRUE;
				}
			}
			else if (fit_unit(units - 1, rgb, alpha))
			{
				work.total_steps += work.steps;
				work.statistics.steps += work.steps;
				return TRUE;
			}
			if (work.steps > STEP_LIMIT)
				break;
		}
		if (work.steps > STEP_LIMIT)
			break;
	}
	work.total_steps += work.steps;
	work.statistics.steps += work.steps;
	if (work.failures[key % FAILURE_CACHE_SIZE].key != key || work.failures[key % FAILURE_CACHE_SIZE].rank < work.policy_rank)
	{
		work.failures[key % FAILURE_CACHE_SIZE].key = key;
		work.failures[key % FAILURE_CACHE_SIZE].rank = work.policy_rank;
	}
	return FALSE;
}

/* One pass that computes rgb and alpha into work.build: by its units
alone or, with EXT_secondary_color, with the terms that read no texture
in the secondary colour, which OpenGL adds to the units' result. */
static int fit_pass(const struct polynomial *rgb, const struct polynomial *alpha)
{
	struct polynomial untextured, textured;
	float low, high;

	if (fit_pass_units(rgb, alpha))
		return TRUE;
	if (!work.capabilities->secondary_color || !rgb)
		return FALSE;
	polynomial_split(rgb, _depends_vertex | _depends_constants, 0, &untextured, &textured);
	if (untextured.count == 0 || textured.count == 0)
		return FALSE;
	/* the units clamp before the sum: what they compute must not be negative */
	polynomial_range(&textured, &low, &high);
	if (low < -RANGE_EPSILON || !primary_valid(&untextured))
		return FALSE;
	if (!fit_pass_units(&textured, alpha))
		return FALSE;
	work.build.secondary_rgb.bound = TRUE;
	work.build.secondary_rgb.value = untextured;
	return TRUE;
}

/* how large a term can get */
static float term_magnitude(const struct nv2a_ff_term *term)
{
	float magnitude = 0.0f;
	int atom, channel;

	for (channel = 0; channel < 3; channel++)
	{
		if ((float)fabs(term->coefficient[channel]) > magnitude)
			magnitude = (float)fabs(term->coefficient[channel]);
	}
	for (atom = 0; atom < term->count; atom++)
	{
		float low, high;

		atom_range(term->atoms[atom], &low, &high);
		magnitude *= (float)(fabs(low) > fabs(high) ? fabs(low) : fabs(high));
	}
	return magnitude;
}

/* a term this much smaller than the largest of its polynomial may be left out */
#define SMALL_TERM 0.25f

/* A pass for the two goals with one term left out of one of them, the
smallest that makes them fit. */
static int fit_pass_without_a_term(const struct polynomial *rgb, const struct polynomial *alpha)
{
	enum { MAXIMUM_TRIES = 10 };
	const struct polynomial *goals[2];
	int tried[2][POLYNOMIAL_TERMS];
	float largest[2];
	int tries, side, index;

	goals[0] = alpha;
	goals[1] = rgb;
	memset(tried, 0, sizeof(tried));
	for (side = 0; side < 2; side++)
	{
		largest[side] = 0.0f;
		for (index = 0; goals[side] && index < goals[side]->count; index++)
		{
			float magnitude = term_magnitude(&goals[side]->terms[index]);

			if (magnitude > largest[side])
				largest[side] = magnitude;
		}
	}
	for (tries = 0; tries < MAXIMUM_TRIES; tries++)
	{
		struct polynomial reduced;
		int best_side = -1, best_index = 0;
		float best = 0.0f;

		for (side = 0; side < 2; side++)
		{
			if (!goals[side] || goals[side]->count < 2)
				continue;
			for (index = 0; index < goals[side]->count; index++)
			{
				float magnitude = term_magnitude(&goals[side]->terms[index]);

				/* a term that can be as large as the others is no detail */
				if (magnitude > SMALL_TERM * largest[side])
					continue;
				if (!tried[side][index] && (best_side < 0 || magnitude < best))
				{
					best_side = side;
					best_index = index;
					best = magnitude;
				}
			}
		}
		if (best_side < 0)
			break;
		tried[best_side][best_index] = TRUE;
		reduced = *goals[best_side];
		memmove(&reduced.terms[best_index], &reduced.terms[best_index + 1],
			(reduced.count - best_index - 1) * sizeof(struct nv2a_ff_term));
		reduced.count--;
		if (fit_pass(best_side ? &reduced : rgb, best_side ? alpha : &reduced))
			return TRUE;
		if (work.total_steps > TOTAL_STEP_LIMIT)
			break;
	}
	return FALSE;
}

/* ---------- the framebuffer side: passes */

static int factor_reads_source_alpha(unsigned long factor)
{
	return factor == _nv2a_ff_blend_source_alpha || factor == _nv2a_ff_blend_inverse_source_alpha ||
		factor == _nv2a_ff_blend_source_alpha_saturate;
}

static int factor_reads_constant(unsigned long factor)
{
	return factor >= _nv2a_ff_blend_constant_color && factor <= _nv2a_ff_blend_inverse_constant_alpha;
}

/* a source factor that does not depend on the source's colour or on what
earlier passes changed: the passes of a sum can each use it */
static int source_factor_distributes(unsigned long factor)
{
	switch (factor)
	{
	case _nv2a_ff_blend_zero:
	case _nv2a_ff_blend_one:
	case _nv2a_ff_blend_source_alpha:
	case _nv2a_ff_blend_inverse_source_alpha:
	case _nv2a_ff_blend_constant_color:
	case _nv2a_ff_blend_inverse_constant_color:
	case _nv2a_ff_blend_constant_alpha:
	case _nv2a_ff_blend_inverse_constant_alpha:
		return TRUE;
	case _nv2a_ff_blend_destination_alpha:
	case _nv2a_ff_blend_inverse_destination_alpha:
		/* as long as the passes leave destination alpha alone */
		return !(work.draw->write_alpha && work.capabilities->destination_alpha);
	default:
		return FALSE;
	}
}

static int destination_factor_distributes(unsigned long factor)
{
	switch (factor)
	{
	case _nv2a_ff_blend_zero:
	case _nv2a_ff_blend_one:
	case _nv2a_ff_blend_source_alpha:
	case _nv2a_ff_blend_inverse_source_alpha:
	case _nv2a_ff_blend_constant_color:
	case _nv2a_ff_blend_inverse_constant_color:
	case _nv2a_ff_blend_constant_alpha:
	case _nv2a_ff_blend_inverse_constant_alpha:
		return TRUE;
	default:
		return FALSE;
	}
}

/* alpha kill does nothing visible: wherever a killed texel's alpha is
zero the fragment's alpha is zero, and the blend then leaves the
framebuffer as it was */
static int alpha_kill_is_invisible(void)
{
	int stage, index, atom;

	if (work.draw->depth_write || !work.draw->blend_enable || work.blend_operation != _nv2a_ff_blend_operation_add)
		return FALSE;
	if (work.source_blend != _nv2a_ff_blend_source_alpha ||
		(work.destination_blend != _nv2a_ff_blend_inverse_source_alpha && work.destination_blend != _nv2a_ff_blend_one))
	{
		return FALSE;
	}
	if (work.draw->write_alpha && work.capabilities->destination_alpha)
		return FALSE;
	for (stage = 0; stage < NV2A_FF_STAGE_COUNT; stage++)
	{
		if (!(work.kill_stages & (1 << stage)))
			continue;
		if (work.goal_alpha.count == 0)
			return FALSE;
		for (index = 0; index < work.goal_alpha.count; index++)
		{
			const struct nv2a_ff_term *term = &work.goal_alpha.terms[index];

			for (atom = 0; atom < term->count; atom++)
			{
				if (term->atoms[atom] == NV2A_FF_ATOM(stage, _nv2a_ff_channel_alpha))
					break;
			}
			if (atom == term->count)
				return FALSE;
		}
	}
	return TRUE;
}

/* one more pass that computes rgb, blended as its role says */
static int pass_emit(const struct polynomial *rgb, int role)
{
	const struct nv2a_ff_state *state = work.state;
	const struct nv2a_ff_draw *draw = work.draw;
	struct pass_build *pass;
	unsigned long source_blend, destination_blend, blend_operation;
	int blend_enable = TRUE;
	int first_drawn = work.sequence_count == 0;
	int alpha_test, kill_test, write_alpha, needs_alpha;

	if (work.sequence_count >= work.maximum_passes)
		return FALSE;
	switch (role)
	{
	case _role_first:
		source_blend = work.source_blend;
		destination_blend = work.destination_blend;
		blend_operation = work.blend_operation;
		blend_enable = work.blend_kind != _blend_kind_write;
		break;
	case _role_add:
		source_blend = work.blend_kind == _blend_kind_write ? _nv2a_ff_blend_one : work.source_blend;
		destination_blend = _nv2a_ff_blend_one;
		blend_operation = work.blend_kind == _blend_kind_write ? _nv2a_ff_blend_operation_add : work.blend_operation;
		break;
	default:
		source_blend = _nv2a_ff_blend_destination_color;
		destination_blend = _nv2a_ff_blend_zero;
		blend_operation = _nv2a_ff_blend_operation_add;
		break;
	}

	/* the first pass drawn decides which fragments exist; with depth
	writes the later ones find them again by their depth */
	alpha_test = state->alpha_test_function != 0 && (first_drawn || !draw->depth_write);
	kill_test = FALSE;
	if (work.kill_stages && !alpha_kill_is_invisible())
	{
		work.approximations |= _nv2a_ff_approximate_alpha_kill;
		kill_test = state->alpha_test_function == 0 && (first_drawn || !draw->depth_write);
	}
	write_alpha = role == _role_first && draw->write_alpha && work.capabilities->destination_alpha &&
		!work.ignore_destination_alpha;
	needs_alpha = alpha_test || kill_test || write_alpha ||
		(blend_enable && (factor_reads_source_alpha(source_blend) || factor_reads_source_alpha(destination_blend)));

	if (role == _role_first && !draw->write_color)
		rgb = NULL;
	if (!fit_pass(rgb, needs_alpha ? &work.goal_alpha : NULL))
	{
		if (!work.drop_terms || !fit_pass_without_a_term(rgb, needs_alpha ? &work.goal_alpha : NULL))
			return FALSE;
		work.approximations |= _nv2a_ff_approximate_dropped_term;
	}

	pass = &work.sequence[work.sequence_count++];
	*pass = work.build;
	pass->blend_enable = blend_enable;
	pass->source_blend = source_blend;
	pass->destination_blend = destination_blend;
	pass->blend_operation = blend_operation;
	pass->write_color = role == _role_first ? draw->write_color : TRUE;
	pass->write_alpha = role == _role_first && draw->write_alpha;
	pass->alpha_test = alpha_test;
	pass->alpha_test_greater_zero = kill_test;
	pass->depth_equal = !first_drawn && draw->depth_write;
	pass->fog = work.fog_active && role != _role_multiply;
	pass->fog_black = role == _role_add;
	return TRUE;
}

/* The two parts of a sum can be drawn as passes: each pass clamps what
it writes, so neither part may go below zero. When the search is relaxed
anyway (the last rounds) a part that can is let through, and the plan is
marked for it. */
static int sum_parts_valid(const struct polynomial *first, const struct polynomial *second,
	unsigned long *approximations)
{
	float low, high;

	*approximations = 0;
	if (!work.sum_overflows)
	{
		polynomial_range(first, &low, &high);
		if (low >= -RANGE_EPSILON)
		{
			polynomial_range(second, &low, &high);
			if (low >= -RANGE_EPSILON)
				return TRUE;
		}
	}
	if (work.strict_primary)
		return FALSE;
	*approximations = _nv2a_ff_approximate_clamp;
	return TRUE;
}

/* rgb drawn in one pass or, split along a sum or a product, in several */
static int realise(const struct polynomial *rgb, int role)
{
	int mark = work.sequence_count;
	int sum_allowed, product_allowed;
	unsigned long approximations;
	unsigned long key;
	int passes_left;

	if (budget_spent())
		return FALSE;
	work.total_steps++;
	work.statistics.steps++;
	if (pass_emit(rgb, role))
		return TRUE;
	work.sequence_count = mark;
	if (!rgb || work.sequence_count + 2 > work.maximum_passes || work.total_steps > TOTAL_STEP_LIMIT || budget_spent())
		return FALSE;

	if (role == _role_first)
	{
		sum_allowed = work.blend_kind == _blend_kind_write || work.blend_kind == _blend_kind_sum;
		product_allowed = work.blend_kind == _blend_kind_write || work.blend_kind == _blend_kind_multiply ||
			work.blend_kind == _blend_kind_double;
	}
	else
	{
		sum_allowed = role == _role_add;
		product_allowed = role == _role_multiply;
	}
	if (!work.product_splits_allowed)
		product_allowed = FALSE;

	/* what failed before with as many passes left fails again */
	key = (polynomial_hash(rgb, 2166136261UL ^ (unsigned long)role) | 1) & 0xffffffffUL;
	passes_left = work.maximum_passes - work.sequence_count;
	if (work.splits[key % FAILURE_CACHE_SIZE].key == key && work.splits[key % FAILURE_CACHE_SIZE].rank >= work.policy_rank &&
		work.splits[key % FAILURE_CACHE_SIZE].passes >= passes_left)
	{
		return FALSE;
	}
	/* a term that reads more textures than a pass has units is in no
	pass, however the sum is split */
	if (!product_allowed)
	{
		int index, atom, stage;

		for (index = 0; index < rgb->count; index++)
		{
			int mask = 0, count = 0;

			for (atom = 0; atom < rgb->terms[index].count; atom++)
				mask |= atom_dependencies(rgb->terms[index].atoms[atom]) & _depends_textures;
			for (stage = 0; stage < NV2A_FF_STAGE_COUNT; stage++)
				count += (mask >> stage) & 1;
			if (count > work.capabilities->units)
				return FALSE;
		}
	}

	if (sum_allowed && rgb->count >= 2)
	{
		/* The terms by the vertex colour they hold (diffuse, specular,
		both, none): a pass has one primary colour, so terms with
		different ones rarely share a pass, whatever textures they read.
		The terms that read no texture make one part: the primary colour
		holds any of them. */
		struct polynomial parts[5];
		int part_count = 0, class_index, index, atom;
		unsigned long part_approximations = 0;

		for (class_index = -1; class_index < 4; class_index++)
		{
			struct polynomial *part = &parts[part_count];

			part->count = 0;
			for (index = 0; index < rgb->count; index++)
			{
				const struct nv2a_ff_term *term = &rgb->terms[index];
				int term_class = 0, textured = FALSE;

				for (atom = 0; atom < term->count; atom++)
				{
					if (term->atoms[atom] == NV2A_FF_ATOM(_nv2a_ff_atom_v0, _nv2a_ff_channel_rgb))
						term_class |= 1;
					if (term->atoms[atom] == NV2A_FF_ATOM(_nv2a_ff_atom_v1, _nv2a_ff_channel_rgb))
						term_class |= 2;
					if (atom_dependencies(term->atoms[atom]) & _depends_textures)
						textured = TRUE;
				}
				if (!textured)
					term_class = -1;
				if (term_class == class_index)
					part->terms[part->count++] = *term;
			}
			if (part->count)
			{
				float part_low, part_high;

				polynomial_range(part, &part_low, &part_high);
				if (part_low < -RANGE_EPSILON)
					part_approximations = _nv2a_ff_approximate_clamp;
				part_count++;
			}
		}
		if (work.capabilities->secondary_color && part_count >= 2 && parts[0].count &&
			!(polynomial_dependencies(&parts[0]) & _depends_textures) &&
			parts[0].count + parts[1].count <= POLYNOMIAL_TERMS)
		{
			/* the secondary colour carries the terms without a texture
			along with the first pass that has one */
			memcpy(&parts[1].terms[parts[1].count], parts[0].terms, parts[0].count * sizeof(struct nv2a_ff_term));
			parts[1].count += parts[0].count;
			parts[1].count = terms_normalize(parts[1].terms, parts[1].count);
			part_count--;
			memmove(&parts[0], &parts[1], part_count * sizeof(parts[0]));
		}
		if (work.sum_overflows)
			part_approximations = _nv2a_ff_approximate_clamp;
		if (part_count >= 2 && (!part_approximations || !work.strict_primary) &&
			work.sequence_count + part_count <= work.maximum_passes)
		{
			for (class_index = 0; class_index < part_count; class_index++)
			{
				if (!realise(&parts[class_index], class_index == 0 ? role : role == _role_first ? _role_add : role))
					break;
			}
			if (class_index == part_count)
			{
				work.approximations |= part_approximations;
				return TRUE;
			}
			work.sequence_count = mark;
			if (work.total_steps > TOTAL_STEP_LIMIT)
				return FALSE;
		}
	}

	if (sum_allowed && rgb->count >= 2)
	{
		/* the terms in groups by the textures they read; when that gives
		one group only, each term a group of its own */
		unsigned char group[POLYNOMIAL_TERMS];
		int masks[POLYNOMIAL_TERMS];
		int group_count = 0, index, atom;
		long subset;

		for (index = 0; index < rgb->count; index++)
		{
			int mask = 0, known;

			for (atom = 0; atom < rgb->terms[index].count; atom++)
				mask |= atom_dependencies(rgb->terms[index].atoms[atom]) & _depends_textures;
			for (known = 0; known < group_count && masks[known] != mask; known++)
				;
			if (known == group_count)
				masks[group_count++] = mask;
			group[index] = (unsigned char)known;
		}
		if (group_count == 1 && rgb->count <= 6)
		{
			group_count = rgb->count;
			for (index = 0; index < rgb->count; index++)
				group[index] = (unsigned char)index;
		}
		if (group_count >= 2 && group_count <= 7)
		{
			/* subsets that hold the first group, the rest in the second part */
			for (subset = 1; subset < (1L << group_count) - 1; subset += 2)
			{
				struct polynomial first, second;

				first.count = 0;
				second.count = 0;
				for (index = 0; index < rgb->count; index++)
				{
					if (subset & (1L << group[index]))
						first.terms[first.count++] = rgb->terms[index];
					else
						second.terms[second.count++] = rgb->terms[index];
				}
				/* each pass clamps: neither part may go below zero */
				if (!sum_parts_valid(&first, &second, &approximations))
					continue;
				if (realise(&first, role) && realise(&second, role == _role_first ? _role_add : role))
				{
					work.approximations |= approximations;
					return TRUE;
				}
				work.sequence_count = mark;
				if (work.total_steps > TOTAL_STEP_LIMIT)
					return FALSE;
			}
		}

		/* the terms that hold some atom, and the others: the sum of two
		products that have different factors comes apart this way */
		{
			unsigned char atoms[16];
			int atom_count = 0, candidate;

			for (index = 0; index < rgb->count; index++)
			{
				for (atom = 0; atom < rgb->terms[index].count; atom++)
				{
					int known;

					for (known = 0; known < atom_count && atoms[known] != rgb->terms[index].atoms[atom]; known++)
						;
					if (known == atom_count && atom_count < (int)sizeof(atoms))
						atoms[atom_count++] = rgb->terms[index].atoms[atom];
				}
			}
			for (candidate = 0; candidate < atom_count; candidate++)
			{
				struct polynomial first, second;

				first.count = 0;
				second.count = 0;
				for (index = 0; index < rgb->count; index++)
				{
					const struct nv2a_ff_term *term = &rgb->terms[index];

					for (atom = 0; atom < term->count && term->atoms[atom] != atoms[candidate]; atom++)
						;
					if (atom < term->count)
						first.terms[first.count++] = *term;
					else
						second.terms[second.count++] = *term;
				}
				if (first.count == 0 || second.count == 0)
					continue;
				if (!sum_parts_valid(&first, &second, &approximations))
					continue;
				if (realise(&second, role) && realise(&first, role == _role_first ? _role_add : role))
				{
					work.approximations |= approximations;
					return TRUE;
				}
				work.sequence_count = mark;
				if (work.total_steps > TOTAL_STEP_LIMIT)
					return FALSE;
			}
		}
	}

	if (product_allowed)
	{
		int textures = polynomial_dependencies(rgb) & _depends_textures;
		int mask, with_constants;

		for (with_constants = 0; with_constants < 2; with_constants++)
		{
			for (mask = 1; mask < 16; mask++)
			{
				struct polynomial first, second;

				if ((mask & textures) != mask || mask == textures)
					continue;
				if (!polynomial_factor(rgb, mask | (with_constants ? _depends_constants : 0), mask, FALSE, &first, &second))
					continue;
				if (!polynomial_within_unit(&first) || !polynomial_within_unit(&second))
					continue;
				if (role == _role_first && work.blend_kind == _blend_kind_double)
				{
					/* the doubling belongs to the last pass */
					if (realise(&first, _role_multiply) && realise(&second, _role_first))
						return TRUE;
				}
				else if (realise(&first, role) && realise(&second, _role_multiply))
				{
					return TRUE;
				}
				work.sequence_count = mark;
				if (work.total_steps > TOTAL_STEP_LIMIT)
					return FALSE;
			}
		}

		/* What no texture enters first, with the alpha the draw tests,
		then the textures multiplied in: for a draw whose alpha comes from
		another texture than its colour, on fewer units than both need. */
		if (textures && role == _role_first && work.blend_kind != _blend_kind_double)
		{
			struct polynomial first, second;

			if (polynomial_factor(rgb, textures, textures, FALSE, &first, &second) &&
				polynomial_within_unit(&first) && polynomial_within_unit(&second) &&
				realise(&second, role) && realise(&first, _role_multiply))
			{
				return TRUE;
			}
			work.sequence_count = mark;
		}
	}
	work.splits[key % FAILURE_CACHE_SIZE].key = key;
	work.splits[key % FAILURE_CACHE_SIZE].rank = work.policy_rank;
	work.splits[key % FAILURE_CACHE_SIZE].passes = passes_left;
	return FALSE;
}

/* The draw's blend into work.source_blend and so on, with what the
capabilities lack replaced, and its kind. May change the colour goal
(factors folded into it) and emit a first pass (a constant destination
factor). FALSE when the blend cannot be drawn at all. */
static int blend_prepare(struct polynomial *rgb)
{
	const struct nv2a_ff_capabilities *capabilities = work.capabilities;
	const struct nv2a_ff_draw *draw = work.draw;
	struct symbolic_register blend_color;
	struct polynomial factor, product, clamped;

	work.blend_kind = _blend_kind_other;
	work.source_blend = draw->blend_enable ? draw->source_blend : _nv2a_ff_blend_one;
	work.destination_blend = draw->blend_enable ? draw->destination_blend : _nv2a_ff_blend_zero;
	work.blend_operation = draw->blend_enable ? draw->blend_operation : _nv2a_ff_blend_operation_add;

	if (work.blend_operation == _nv2a_ff_blend_operation_minimum ||
		work.blend_operation == _nv2a_ff_blend_operation_maximum)
	{
		/* the factors play no part (OpenGL's rule, and the NV2A's) */
		if (capabilities->blend_minmax)
			return TRUE;
		work.approximations |= _nv2a_ff_approximate_blend;
		if (work.blend_operation == _nv2a_ff_blend_operation_minimum)
		{
			work.source_blend = _nv2a_ff_blend_destination_color;
			work.destination_blend = _nv2a_ff_blend_zero;
		}
		else
		{
			work.source_blend = _nv2a_ff_blend_inverse_destination_color;
			work.destination_blend = _nv2a_ff_blend_one;
		}
		work.blend_operation = _nv2a_ff_blend_operation_add;
	}

	/* constant factors without EXT_blend_color: the source's goes into
	the colour, the destination's into a pass of its own */
	if (!capabilities->blend_color && factor_reads_constant(work.source_blend))
	{
		symbolic_constant(&blend_color, _nv2a_ff_atom_blend_color);
		switch (work.source_blend)
		{
		case _nv2a_ff_blend_constant_color: factor = blend_color.rgb; break;
		case _nv2a_ff_blend_inverse_constant_color: polynomial_affine(&factor, &blend_color.rgb, -1.0f, 1.0f); break;
		case _nv2a_ff_blend_constant_alpha: factor = blend_color.alpha; break;
		default: polynomial_affine(&factor, &blend_color.alpha, -1.0f, 1.0f); break;
		}
		/* (the factor scales the colour after its clamp to 0..1) */
		make_clamp_unit(&clamped, rgb);
		make_product(&product, &clamped, &factor);
		*rgb = product;
		work.source_blend = _nv2a_ff_blend_one;
		/* a destination factor that reads the source colour now reads the
		scaled one */
		if (work.destination_blend == _nv2a_ff_blend_source_color ||
			work.destination_blend == _nv2a_ff_blend_inverse_source_color)
		{
			work.approximations |= _nv2a_ff_approximate_blend;
		}
		if (draw->write_alpha && capabilities->destination_alpha)
			work.approximations |= _nv2a_ff_approximate_destination_alpha;
	}
	if (!capabilities->destination_alpha && work.blend_operation != _nv2a_ff_blend_operation_minimum &&
		work.blend_operation != _nv2a_ff_blend_operation_maximum &&
		(work.source_blend == _nv2a_ff_blend_destination_alpha ||
			work.source_blend == _nv2a_ff_blend_inverse_destination_alpha ||
			work.source_blend == _nv2a_ff_blend_source_alpha_saturate ||
			work.destination_blend == _nv2a_ff_blend_destination_alpha ||
			work.destination_blend == _nv2a_ff_blend_inverse_destination_alpha))
	{
		/* the framebuffer has no alpha: it reads as one */
		work.approximations |= _nv2a_ff_approximate_destination_alpha;
	}
	if (!capabilities->blend_color && factor_reads_constant(work.destination_blend) &&
		(work.source_blend == _nv2a_ff_blend_destination_color ||
			work.source_blend == _nv2a_ff_blend_inverse_destination_color ||
			work.source_blend == _nv2a_ff_blend_source_alpha_saturate ||
			work.blend_operation != _nv2a_ff_blend_operation_add))
	{
		/* the source's factor reads the destination, which a pass for
		the constant would have changed: the constant is left out */
		work.approximations |= _nv2a_ff_approximate_blend;
		work.destination_blend = _nv2a_ff_blend_one;
	}
	if (!capabilities->blend_color && factor_reads_constant(work.destination_blend))
	{
		int kind = work.blend_kind;
		unsigned long source_blend = work.source_blend;

		symbolic_constant(&blend_color, _nv2a_ff_atom_blend_color);
		switch (work.destination_blend)
		{
		case _nv2a_ff_blend_constant_color: factor = blend_color.rgb; break;
		case _nv2a_ff_blend_inverse_constant_color: polynomial_affine(&factor, &blend_color.rgb, -1.0f, 1.0f); break;
		case _nv2a_ff_blend_constant_alpha: factor = blend_color.alpha; break;
		default: polynomial_affine(&factor, &blend_color.alpha, -1.0f, 1.0f); break;
		}
		/* destination * constant first, then the draw with factor one */
		work.blend_kind = _blend_kind_multiply;
		if (!pass_emit(&factor, _role_multiply))
			return FALSE;
		work.blend_kind = kind;
		work.source_blend = source_blend;
		work.destination_blend = _nv2a_ff_blend_one;
		if (draw->write_alpha && capabilities->destination_alpha)
			work.approximations |= _nv2a_ff_approximate_destination_alpha;
	}

	if (work.blend_operation == _nv2a_ff_blend_operation_subtract ||
		work.blend_operation == _nv2a_ff_blend_operation_reverse_subtract)
	{
		if (!capabilities->blend_subtract)
		{
			if (work.blend_operation == _nv2a_ff_blend_operation_subtract)
				return FALSE;
			/* destination - source becomes destination * (1 - source) */
			work.approximations |= _nv2a_ff_approximate_blend;
			if (work.source_blend == _nv2a_ff_blend_source_alpha)
			{
				struct polynomial alpha;

				make_clamp_unit(&clamped, rgb);
				make_clamp_unit(&alpha, &work.goal_alpha);
				make_product(&product, &clamped, &alpha);
				*rgb = product;
			}
			work.source_blend = _nv2a_ff_blend_zero;
			work.destination_blend = _nv2a_ff_blend_inverse_source_color;
			work.blend_operation = _nv2a_ff_blend_operation_add;
			return TRUE;
		}
		if (work.blend_operation == _nv2a_ff_blend_operation_reverse_subtract &&
			source_factor_distributes(work.source_blend) && destination_factor_distributes(work.destination_blend))
		{
			work.blend_kind = _blend_kind_sum;
		}
		return TRUE;
	}

	if (work.source_blend == _nv2a_ff_blend_one && work.destination_blend == _nv2a_ff_blend_zero)
		work.blend_kind = _blend_kind_write;
	else if ((work.source_blend == _nv2a_ff_blend_destination_color && work.destination_blend == _nv2a_ff_blend_zero) ||
		(work.source_blend == _nv2a_ff_blend_zero && work.destination_blend == _nv2a_ff_blend_source_color))
		work.blend_kind = _blend_kind_multiply;
	else if (work.source_blend == _nv2a_ff_blend_destination_color &&
		work.destination_blend == _nv2a_ff_blend_source_color)
		work.blend_kind = _blend_kind_double;
	else if (source_factor_distributes(work.source_blend) && destination_factor_distributes(work.destination_blend))
		work.blend_kind = _blend_kind_sum;
	return TRUE;
}

/* all the passes for the goal, into work.sequence */
static int sequence_plan(void)
{
	struct polynomial rgb = work.goal_rgb;
	int fog_atom = NV2A_FF_ATOM(_nv2a_ff_atom_fog, _nv2a_ff_channel_alpha);
	struct polynomial without, with, both;
	int mark;

	work.sequence_count = 0;
	work.fog_active = FALSE;
	work.product_splits_allowed = TRUE;
	if (!blend_prepare(&rgb) || work.overflow)
		return FALSE;
	mark = work.sequence_count;
	/* The state clamps its colour to one before the blend scales it. A
	sum drawn in passes is scaled pass by pass: the same thing when the
	source factor is one or the colour cannot exceed one. */
	work.sum_overflows = work.blend_kind == _blend_kind_sum && work.source_blend != _nv2a_ff_blend_one &&
		work.source_blend != _nv2a_ff_blend_zero && !polynomial_within_unit(&rgb);
	if (work.single_pass && work.maximum_passes > mark + 1)
		work.maximum_passes = mark + 1;

	/* OpenGL's own fog: colour * factor + fog colour * (1 - factor) */
	if (work.capabilities->fog_coordinate && work.state->fog_enable && work.draw->write_color &&
		work.blend_operation == _nv2a_ff_blend_operation_add &&
		(work.blend_kind == _blend_kind_write || work.blend_kind == _blend_kind_sum) && mark == 0 &&
		polynomial_linear(&rgb, fog_atom, &without, &with) && polynomial_add(&both, &without, &with))
	{
		struct polynomial factor;

		polynomial_atom(&factor, fog_atom);
		work.fog_active = TRUE;
		work.product_splits_allowed = FALSE;
		/* (the units' colour is clamped before the fog is mixed in, the
		state's after: the same only when it stays within 0 to 1) */
		if (constant_valid(&without) && polynomial_within_unit(&both))
		{
			/* fog colour + factor * (colour - fog colour) */
			work.fog_factor = factor;
			work.fog_color = without;
			if (realise(&both, _role_first))
				return TRUE;
		}
		else if (constant_valid(&both) && polynomial_within_unit(&without))
		{
			polynomial_affine(&work.fog_factor, &factor, -1.0f, 1.0f);
			work.fog_color = both;
			if (realise(&without, _role_first))
				return TRUE;
		}
		work.fog_active = FALSE;
		work.product_splits_allowed = TRUE;
		work.sequence_count = mark;
	}
	return realise(&rgb, _role_first);
}

/* ---------- from the work area to a plan */

static struct
{
	struct nv2a_ff_plan *plan;
	/* per atom of the work area: its operation in the plan, or -1 */
	short operation_of[MAXIMUM_OPAQUE_ATOMS];
	struct polynomial bake_rgb[NV2A_FF_MAXIMUM_BAKES];
	struct polynomial bake_alpha[NV2A_FF_MAXIMUM_BAKES];
	int failed;
} exporter;

static short export_polynomial(const struct polynomial *polynomial);

static int export_operation(int atom)
{
	struct nv2a_ff_plan *plan = exporter.plan;
	const struct opaque_atom *opaque = &work.opaque[atom - LEAF_ATOM_COUNT];
	short operands[3];
	int operand, index;

	if (exporter.operation_of[atom - LEAF_ATOM_COUNT] >= 0)
		return exporter.operation_of[atom - LEAF_ATOM_COUNT];
	for (operand = 0; operand < 3; operand++)
	{
		operands[operand] = NV2A_FF_EXPRESSION_NONE;
		if (opaque->operands[operand] >= 0)
			operands[operand] = export_polynomial(&work.store[opaque->operands[operand]]);
	}
	if (plan->operation_count == NV2A_FF_MAXIMUM_OPERATIONS)
	{
		exporter.failed = TRUE;
		return 0;
	}
	index = plan->operation_count++;
	plan->operations[index].operation = opaque->operation;
	plan->operations[index].pad = 0;
	memcpy(plan->operations[index].operands, operands, sizeof(operands));
	exporter.operation_of[atom - LEAF_ATOM_COUNT] = (short)index;
	return index;
}

static short export_polynomial(const struct polynomial *polynomial)
{
	struct nv2a_ff_plan *plan = exporter.plan;
	struct nv2a_ff_expression *expression;
	int index, atom;

	/* the operations first: their operands are expressions too, and an
	expression's terms lie together */
	for (index = 0; index < polynomial->count; index++)
	{
		for (atom = 0; atom < polynomial->terms[index].count; atom++)
		{
			if (polynomial->terms[index].atoms[atom] >= LEAF_ATOM_COUNT)
				export_operation(polynomial->terms[index].atoms[atom]);
		}
	}
	if (plan->expression_count == NV2A_FF_MAXIMUM_EXPRESSIONS ||
		plan->term_count + polynomial->count > NV2A_FF_MAXIMUM_TERMS)
	{
		exporter.failed = TRUE;
		return NV2A_FF_EXPRESSION_NONE;
	}
	expression = &plan->expressions[plan->expression_count];
	expression->first_term = plan->term_count;
	expression->term_count = (unsigned short)polynomial->count;
	for (index = 0; index < polynomial->count; index++)
	{
		struct nv2a_ff_term *term = &plan->terms[plan->term_count++];

		*term = polynomial->terms[index];
		for (atom = 0; atom < term->count; atom++)
		{
			if (term->atoms[atom] >= LEAF_ATOM_COUNT)
				term->atoms[atom] = (unsigned char)(LEAF_ATOM_COUNT + exporter.operation_of[term->atoms[atom] - LEAF_ATOM_COUNT]);
		}
	}
	return (short)plan->expression_count++;
}

static int export_bake(int stage, const struct polynomial *rgb, const struct polynomial *alpha)
{
	struct nv2a_ff_plan *plan = exporter.plan;
	struct nv2a_ff_bake *bake;
	struct polynomial none;
	int index, dependencies;
	int texture = _depends_stage0 << stage;

	polynomial_zero(&none);
	for (index = 0; index < plan->bake_count; index++)
	{
		bake = &plan->bakes[index];
		if (bake->stage == stage && (bake->rgb != NV2A_FF_EXPRESSION_NONE) == (rgb != NULL) &&
			(bake->alpha != NV2A_FF_EXPRESSION_NONE) == (alpha != NULL) &&
			(!rgb || polynomial_equal(rgb, &exporter.bake_rgb[index])) &&
			(!alpha || polynomial_equal(alpha, &exporter.bake_alpha[index])))
		{
			return index;
		}
	}
	if (plan->bake_count == NV2A_FF_MAXIMUM_BAKES)
	{
		exporter.failed = TRUE;
		return 0;
	}
	index = plan->bake_count++;
	bake = &plan->bakes[index];
	exporter.bake_rgb[index] = rgb ? *rgb : none;
	exporter.bake_alpha[index] = alpha ? *alpha : none;
	dependencies = (rgb ? polynomial_dependencies(rgb) : 0) | (alpha ? polynomial_dependencies(alpha) : 0);
	bake->stage = (unsigned char)stage;
	bake->uses_constants = (unsigned char)((dependencies & _depends_constants) != 0);
	bake->nonlinear = (unsigned char)((rgb && !polynomial_is_first_degree(rgb, texture)) ||
		(alpha && !polynomial_is_first_degree(alpha, texture)));
	bake->pad = 0;
	bake->rgb = rgb ? export_polynomial(rgb) : NV2A_FF_EXPRESSION_NONE;
	bake->alpha = alpha ? export_polynomial(alpha) : NV2A_FF_EXPRESSION_NONE;
	if (bake->nonlinear)
		plan->approximations |= _nv2a_ff_approximate_bake_filter;
	return index;
}

/* work.sequence into the plan; FALSE when the plan has no room */
static int export_sequence(struct nv2a_ff_plan *plan)
{
	int pass_index, unit_index, index;

	memset(plan, 0, sizeof(*plan));
	exporter.plan = plan;
	exporter.failed = FALSE;
	for (index = 0; index < MAXIMUM_OPAQUE_ATOMS; index++)
		exporter.operation_of[index] = -1;
	plan->approximations = work.approximations;
	plan->pass_count = (unsigned char)work.sequence_count;
	plan->constants_bound = (unsigned char)(work.bound_constants != NULL);
	plan->bound_sources = work.bound_constants ? work.bound_sources : 0;
	memcpy(plan->dropped_stage, work.substitute, sizeof(plan->dropped_stage));
	if (work.ignore_destination_alpha)
		plan->approximations |= _nv2a_ff_approximate_destination_alpha;

	for (pass_index = 0; pass_index < work.sequence_count; pass_index++)
	{
		const struct pass_build *build = &work.sequence[pass_index];
		struct nv2a_ff_pass *pass = &plan->passes[pass_index];
		int dependencies = 0;

		pass->unit_count = (unsigned char)build->unit_count;
		for (unit_index = 0; unit_index < build->unit_count; unit_index++)
		{
			const struct unit_build *unit_build = &build->units[unit_index];
			struct nv2a_ff_unit *unit = &pass->units[unit_index];

			unit->stage = (unsigned char)(unit_build->stage < 0 ? NV2A_FF_STAGE_NONE : unit_build->stage);
			unit->bake = NV2A_FF_BAKE_NONE;
			unit->environment = unit_build->environment;
			unit->rgb = unit_build->rgb;
			unit->alpha = unit_build->alpha;
			unit->constant_rgb = unit_build->constant_rgb.bound ?
				export_polynomial(&unit_build->constant_rgb.value) : NV2A_FF_EXPRESSION_NONE;
			unit->constant_alpha = unit_build->constant_alpha.bound ?
				export_polynomial(&unit_build->constant_alpha.value) : NV2A_FF_EXPRESSION_NONE;
			if (unit_build->stage >= 0)
			{
				struct polynomial identity_rgb, identity_alpha;
				int bake_rgb, bake_alpha;

				stage_identity(unit_build->stage, &identity_rgb, &identity_alpha);
				bake_rgb = unit_build->texture_rgb.bound && !polynomial_equal(&unit_build->texture_rgb.value, &identity_rgb);
				bake_alpha = unit_build->texture_alpha.bound &&
					!polynomial_equal(&unit_build->texture_alpha.value, &identity_alpha);
				if (bake_rgb || bake_alpha)
				{
					unit->bake = (unsigned char)export_bake(unit_build->stage,
						bake_rgb ? &unit_build->texture_rgb.value : NULL,
						bake_alpha ? &unit_build->texture_alpha.value : NULL);
				}
			}
		}

		pass->primary_rgb = NV2A_FF_EXPRESSION_NONE;
		pass->primary_alpha = NV2A_FF_EXPRESSION_NONE;
		if (build->primary_rgb.bound)
		{
			pass->primary_rgb = export_polynomial(&build->primary_rgb.value);
			dependencies |= polynomial_dependencies(&build->primary_rgb.value);
			if (!polynomial_is_first_degree(&build->primary_rgb.value, _depends_vertex) ||
				!polynomial_within_unit(&build->primary_rgb.value))
			{
				plan->approximations |= _nv2a_ff_approximate_vertex_term;
			}
		}
		if (build->primary_alpha.bound)
		{
			pass->primary_alpha = export_polynomial(&build->primary_alpha.value);
			dependencies |= polynomial_dependencies(&build->primary_alpha.value);
			if (!polynomial_is_first_degree(&build->primary_alpha.value, _depends_vertex) ||
				!polynomial_within_unit(&build->primary_alpha.value))
			{
				plan->approximations |= _nv2a_ff_approximate_vertex_term;
			}
		}
		pass->primary_is_constant = (unsigned char)((dependencies & _depends_vertex) == 0);
		pass->secondary_rgb = NV2A_FF_EXPRESSION_NONE;
		if (build->secondary_rgb.bound)
		{
			pass->secondary_rgb = export_polynomial(&build->secondary_rgb.value);
			if (!polynomial_is_first_degree(&build->secondary_rgb.value, _depends_vertex) ||
				!polynomial_within_unit(&build->secondary_rgb.value))
			{
				plan->approximations |= _nv2a_ff_approximate_vertex_term;
			}
		}

		pass->fog_enable = (unsigned char)build->fog;
		pass->fog_factor = NV2A_FF_EXPRESSION_NONE;
		pass->fog_color = NV2A_FF_EXPRESSION_NONE;
		if (build->fog)
		{
			struct polynomial black;

			polynomial_zero(&black);
			pass->fog_factor = export_polynomial(&work.fog_factor);
			pass->fog_color = export_polynomial(build->fog_black ? &black : &work.fog_color);
		}

		pass->alpha_test_function = 0;
		pass->alpha_reference = NV2A_FF_EXPRESSION_NONE;
		if (build->alpha_test)
		{
			pass->alpha_test_function = (unsigned short)work.state->alpha_test_function;
		}
		else if (build->alpha_test_greater_zero)
		{
			struct polynomial zero;

			polynomial_zero(&zero);
			pass->alpha_test_function = _nv2a_ff_compare_greater;
			pass->alpha_reference = export_polynomial(&zero);
		}
		pass->blend_enable = (unsigned char)build->blend_enable;
		pass->source_blend = (unsigned short)build->source_blend;
		pass->destination_blend = (unsigned short)build->destination_blend;
		pass->blend_operation = (unsigned short)build->blend_operation;
		pass->write_color = (unsigned char)build->write_color;
		pass->write_alpha = (unsigned char)build->write_alpha;
		pass->depth_equal = (unsigned char)build->depth_equal;
	}
	return !exporter.failed;
}

/* ---------- evaluating a plan's expressions */

static void evaluate_atom(const struct nv2a_ff_plan *plan, int atom, const struct nv2a_ff_values *values,
	float result[3])
{
	static const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

	if (atom < LEAF_ATOM_COUNT)
	{
		int source = atom / 3;
		int channel = atom % 3;
		const float *color = zero;
		float fog[4];

		if (source < NV2A_FF_STAGE_COUNT)
		{
			if (values->texel)
				color = values->texel[source];
		}
		else if (source == _nv2a_ff_atom_v0)
		{
			if (values->v0)
				color = values->v0;
		}
		else if (source == _nv2a_ff_atom_v1)
		{
			if (values->v1)
				color = values->v1;
		}
		else if (source == _nv2a_ff_atom_fog)
		{
			if (values->constants)
				memcpy(fog, values->constants->fog_color, 3 * sizeof(float));
			else
				memset(fog, 0, sizeof(fog));
			fog[3] = values->fog_factor;
			color = fog;
		}
		else if (values->constants)
		{
			color = constant_color(values->constants, source);
		}
		if (channel == _nv2a_ff_channel_rgb)
			memcpy(result, color, 3 * sizeof(float));
		else
			result[0] = result[1] = result[2] = color[channel == _nv2a_ff_channel_alpha ? 3 : 2];
	}
	else
	{
		const struct nv2a_ff_operation *operation = &plan->operations[atom - LEAF_ATOM_COUNT];
		float a[3], b[3], c[3];
		int channel, take_first;

		nv2a_ff_plan_evaluate(plan, operation->operands[0], values, a);
		switch (operation->operation)
		{
		case _operation_maximum_zero:
			for (channel = 0; channel < 3; channel++)
				result[channel] = a[channel] > 0.0f ? a[channel] : 0.0f;
			break;
		case _operation_clamp_unit:
			for (channel = 0; channel < 3; channel++)
				result[channel] = clamp_unit(a[channel]);
			break;
		case _operation_clamp_signed:
			for (channel = 0; channel < 3; channel++)
				result[channel] = clamp_signed(a[channel]);
			break;
		case _operation_dot:
			nv2a_ff_plan_evaluate(plan, operation->operands[1], values, b);
			result[0] = result[1] = result[2] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
			break;
		case _operation_mux_msb:
		case _operation_mux_lsb:
			nv2a_ff_plan_evaluate(plan, operation->operands[1], values, b);
			nv2a_ff_plan_evaluate(plan, operation->operands[2], values, c);
			if (operation->operation == _operation_mux_msb)
				take_first = a[0] >= 0.5f;
			else
				take_first = ((int)(a[0] * 255.0f + 0.5f) & 1) != 0;
			memcpy(result, take_first ? b : c, 3 * sizeof(float));
			break;
		case _operation_signed_byte:
			for (channel = 0; channel < 3; channel++)
				result[channel] = signed_byte(a[channel]);
			break;
		default:
			memcpy(result, a, 3 * sizeof(float));
			break;
		}
	}
}

void nv2a_ff_plan_evaluate(const struct nv2a_ff_plan *plan, short expression,
	const struct nv2a_ff_values *values, float result[3])
{
	const struct nv2a_ff_expression *entry;
	int index, atom;

	if (expression < 0 || expression >= plan->expression_count)
	{
		result[0] = result[1] = result[2] = 1.0f;
		return;
	}
	entry = &plan->expressions[expression];
	result[0] = result[1] = result[2] = 0.0f;
	for (index = 0; index < entry->term_count; index++)
	{
		const struct nv2a_ff_term *term = &plan->terms[entry->first_term + index];
		float product[3];

		memcpy(product, term->coefficient, sizeof(product));
		for (atom = 0; atom < term->count; atom++)
		{
			float value[3];

			evaluate_atom(plan, term->atoms[atom], values, value);
			product[0] *= value[0];
			product[1] *= value[1];
			product[2] *= value[2];
		}
		result[0] += product[0];
		result[1] += product[1];
		result[2] += product[2];
	}
}

void nv2a_ff_plan_primary(const struct nv2a_ff_plan *plan, int pass,
	const struct nv2a_ff_values *values, float color[4])
{
	float alpha[3];
	int channel;

	nv2a_ff_plan_evaluate(plan, plan->passes[pass].primary_rgb, values, color);
	nv2a_ff_plan_evaluate(plan, plan->passes[pass].primary_alpha, values, alpha);
	color[3] = alpha[0];
	for (channel = 0; channel < 4; channel++)
		color[channel] = clamp_unit(color[channel]);
}

void nv2a_ff_plan_secondary(const struct nv2a_ff_plan *plan, int pass,
	const struct nv2a_ff_values *values, float color[3])
{
	int channel;

	if (plan->passes[pass].secondary_rgb == NV2A_FF_EXPRESSION_NONE)
	{
		color[0] = color[1] = color[2] = 0.0f;
		return;
	}
	nv2a_ff_plan_evaluate(plan, plan->passes[pass].secondary_rgb, values, color);
	for (channel = 0; channel < 3; channel++)
		color[channel] = clamp_unit(color[channel]);
}

void nv2a_ff_plan_constant(const struct nv2a_ff_plan *plan, int pass, int unit,
	const struct nv2a_ff_constants *constants, float color[4])
{
	struct nv2a_ff_values values;
	float alpha[3];
	int channel;

	memset(&values, 0, sizeof(values));
	values.constants = constants;
	nv2a_ff_plan_evaluate(plan, plan->passes[pass].units[unit].constant_rgb, &values, color);
	nv2a_ff_plan_evaluate(plan, plan->passes[pass].units[unit].constant_alpha, &values, alpha);
	color[3] = alpha[0];
	for (channel = 0; channel < 4; channel++)
		color[channel] = clamp_unit(color[channel]);
}

void nv2a_ff_plan_bake(const struct nv2a_ff_plan *plan, int bake,
	const struct nv2a_ff_constants *constants, const float texel[4], float baked[4])
{
	struct nv2a_ff_values values;
	float texels[NV2A_FF_STAGE_COUNT][4];
	float alpha[3];
	int channel;

	memset(&values, 0, sizeof(values));
	memset(texels, 0, sizeof(texels));
	memcpy(texels[plan->bakes[bake].stage], texel, 4 * sizeof(float));
	values.constants = constants;
	values.texel = (const float (*)[4])texels;
	memcpy(baked, texel, 4 * sizeof(float));
	if (plan->bakes[bake].rgb != NV2A_FF_EXPRESSION_NONE)
		nv2a_ff_plan_evaluate(plan, plan->bakes[bake].rgb, &values, baked);
	if (plan->bakes[bake].alpha != NV2A_FF_EXPRESSION_NONE)
	{
		nv2a_ff_plan_evaluate(plan, plan->bakes[bake].alpha, &values, alpha);
		baked[3] = alpha[0];
	}
	for (channel = 0; channel < 4; channel++)
		baked[channel] = clamp_unit(baked[channel]);
}

/* ---------- the software model */

static float round_to_byte(float value)
{
	return (float)floor(clamp_unit(value) * 255.0f + 0.5f) / 255.0f;
}

static int model_byte_colors;

void nv2a_ff_model_byte_colors(int enable)
{
	model_byte_colors = enable;
}

/* a colour the application hands to OpenGL */
static float application_color(float value)
{
	return model_byte_colors ? round_to_byte(value) : clamp_unit(value);
}

static void combine_argument(const struct nv2a_ff_combine *combine, int argument, const float previous[4],
	const float primary[4], const float texel[4], const float constant[4], float value[3])
{
	const float *source;

	switch (combine->source[argument])
	{
	case _nv2a_ff_source_texture: source = texel; break;
	case _nv2a_ff_source_constant: source = constant; break;
	case _nv2a_ff_source_primary: source = primary; break;
	default: source = previous; break;
	}
	switch (combine->operand[argument])
	{
	case _nv2a_ff_operand_color:
		memcpy(value, source, 3 * sizeof(float));
		break;
	case _nv2a_ff_operand_inverse_color:
		value[0] = 1.0f - source[0];
		value[1] = 1.0f - source[1];
		value[2] = 1.0f - source[2];
		break;
	case _nv2a_ff_operand_alpha:
		value[0] = value[1] = value[2] = source[3];
		break;
	default:
		value[0] = value[1] = value[2] = 1.0f - source[3];
		break;
	}
}

static void combine_evaluate(const struct nv2a_ff_combine *combine, const float previous[4],
	const float primary[4], const float texel[4], const float constant[4], float result[3])
{
	float a[3], b[3], c[3];
	int channel;

	combine_argument(combine, 0, previous, primary, texel, constant, a);
	combine_argument(combine, 1, previous, primary, texel, constant, b);
	combine_argument(combine, 2, previous, primary, texel, constant, c);
	for (channel = 0; channel < 3; channel++)
	{
		float value;

		switch (combine->function)
		{
		case _nv2a_ff_combine_modulate: value = a[channel] * b[channel]; break;
		case _nv2a_ff_combine_add: value = a[channel] + b[channel]; break;
		case _nv2a_ff_combine_add_signed: value = a[channel] + b[channel] - 0.5f; break;
		case _nv2a_ff_combine_interpolate: value = a[channel] * c[channel] + b[channel] * (1.0f - c[channel]); break;
		case _nv2a_ff_combine_subtract: value = a[channel] - b[channel]; break;
		case _nv2a_ff_combine_dot3:
			value = 4.0f * ((a[0] - 0.5f) * (b[0] - 0.5f) + (a[1] - 0.5f) * (b[1] - 0.5f) + (a[2] - 0.5f) * (b[2] - 0.5f));
			break;
		default: value = a[channel]; break;
		}
		result[channel] = clamp_unit(value * (float)combine->scale);
	}
}

void nv2a_ff_model_unit(const struct nv2a_ff_unit *unit, const float previous[4], const float primary[4],
	const float texel[4], const float constant[4], float result[4])
{
	float color[4], alpha[3];
	int channel;

	switch (unit->environment)
	{
	case _nv2a_ff_environment_replace:
		memcpy(color, texel, sizeof(color));
		break;
	case _nv2a_ff_environment_modulate:
		for (channel = 0; channel < 4; channel++)
			color[channel] = previous[channel] * texel[channel];
		break;
	case _nv2a_ff_environment_decal:
		for (channel = 0; channel < 3; channel++)
			color[channel] = previous[channel] * (1.0f - texel[3]) + texel[channel] * texel[3];
		color[3] = previous[3];
		break;
	case _nv2a_ff_environment_blend:
		for (channel = 0; channel < 3; channel++)
			color[channel] = previous[channel] * (1.0f - texel[channel]) + constant[channel] * texel[channel];
		color[3] = previous[3] * texel[3];
		break;
	case _nv2a_ff_environment_add:
		for (channel = 0; channel < 3; channel++)
			color[channel] = previous[channel] + texel[channel];
		color[3] = previous[3] * texel[3];
		break;
	default:
		combine_evaluate(&unit->rgb, previous, primary, texel, constant, color);
		combine_evaluate(&unit->alpha, previous, primary, texel, constant, alpha);
		color[3] = alpha[0];
		break;
	}
	for (channel = 0; channel < 4; channel++)
		result[channel] = clamp_unit(color[channel]);
}

static void blend_factor(unsigned long factor, const float source[4], const float destination[4],
	const float blend_color[4], float result[4])
{
	int channel;
	float saturate;

	for (channel = 0; channel < 4; channel++)
	{
		switch (factor)
		{
		case _nv2a_ff_blend_zero: result[channel] = 0.0f; break;
		case _nv2a_ff_blend_source_color: result[channel] = source[channel]; break;
		case _nv2a_ff_blend_inverse_source_color: result[channel] = 1.0f - source[channel]; break;
		case _nv2a_ff_blend_source_alpha: result[channel] = source[3]; break;
		case _nv2a_ff_blend_inverse_source_alpha: result[channel] = 1.0f - source[3]; break;
		case _nv2a_ff_blend_destination_alpha: result[channel] = destination[3]; break;
		case _nv2a_ff_blend_inverse_destination_alpha: result[channel] = 1.0f - destination[3]; break;
		case _nv2a_ff_blend_destination_color: result[channel] = destination[channel]; break;
		case _nv2a_ff_blend_inverse_destination_color: result[channel] = 1.0f - destination[channel]; break;
		case _nv2a_ff_blend_source_alpha_saturate:
			saturate = source[3] < 1.0f - destination[3] ? source[3] : 1.0f - destination[3];
			result[channel] = channel == 3 ? 1.0f : saturate;
			break;
		case _nv2a_ff_blend_constant_color: result[channel] = blend_color[channel]; break;
		case _nv2a_ff_blend_inverse_constant_color: result[channel] = 1.0f - blend_color[channel]; break;
		case _nv2a_ff_blend_constant_alpha: result[channel] = blend_color[3]; break;
		case _nv2a_ff_blend_inverse_constant_alpha: result[channel] = 1.0f - blend_color[3]; break;
		default: result[channel] = 1.0f; break;
		}
	}
}

void nv2a_ff_model_blend(unsigned long source_blend, unsigned long destination_blend,
	unsigned long blend_operation, const float source[4], const float destination[4],
	const float blend_color[4], float result[4])
{
	float source_factor[4], destination_factor[4];
	int channel;

	blend_factor(source_blend, source, destination, blend_color, source_factor);
	blend_factor(destination_blend, source, destination, blend_color, destination_factor);
	for (channel = 0; channel < 4; channel++)
	{
		float from_source = source[channel] * source_factor[channel];
		float from_destination = destination[channel] * destination_factor[channel];

		switch (blend_operation)
		{
		case _nv2a_ff_blend_operation_subtract: result[channel] = from_source - from_destination; break;
		case _nv2a_ff_blend_operation_reverse_subtract: result[channel] = from_destination - from_source; break;
		/* minimum and maximum take the colours as they are */
		case _nv2a_ff_blend_operation_minimum:
			result[channel] = source[channel] < destination[channel] ? source[channel] : destination[channel];
			break;
		case _nv2a_ff_blend_operation_maximum:
			result[channel] = source[channel] > destination[channel] ? source[channel] : destination[channel];
			break;
		default: result[channel] = from_source + from_destination; break;
		}
	}
}

/* what a fragment colour does to the framebuffer */
static void framebuffer_write(int blend_enable, unsigned long source_blend, unsigned long destination_blend,
	unsigned long blend_operation, int write_color, int write_alpha, const float color[4],
	const float blend_color[4], float framebuffer[4])
{
	float blended[4];
	int channel;

	if (blend_enable)
		nv2a_ff_model_blend(source_blend, destination_blend, blend_operation, color, framebuffer, blend_color, blended);
	else
		memcpy(blended, color, sizeof(blended));
	for (channel = 0; channel < 4; channel++)
	{
		if (channel < 3 ? write_color : write_alpha)
			framebuffer[channel] = round_to_byte(blended[channel]);
	}
}

/* nv2a_ff_model_pass, which also tells the fragment's alpha (alpha may be NULL) */
static int model_pass(const struct nv2a_ff_plan *plan, int pass_index,
	const struct nv2a_ff_constants *constants, const struct nv2a_ff_model_fragment *fragment,
	float framebuffer[4], float *alpha)
{
	const struct nv2a_ff_pass *pass = &plan->passes[pass_index];
	float color[4], constant[4];
	int unit, channel;

	memcpy(color, fragment->primary, sizeof(color));
	for (unit = 0; unit < pass->unit_count; unit++)
	{
		float result[4];

		nv2a_ff_plan_constant(plan, pass_index, unit, constants, constant);
		for (channel = 0; channel < 4; channel++)
			constant[channel] = application_color(constant[channel]);
		nv2a_ff_model_unit(&pass->units[unit], color, fragment->primary, fragment->texel[unit], constant, result);
		memcpy(color, result, sizeof(color));
	}
	if (pass->secondary_rgb != NV2A_FF_EXPRESSION_NONE)
	{
		/* the colour sum */
		for (channel = 0; channel < 3; channel++)
			color[channel] = clamp_unit(color[channel] + fragment->secondary[channel]);
	}
	if (pass->fog_enable)
	{
		struct nv2a_ff_values values;
		float fog_color[3];
		float factor = clamp_unit(fragment->fog_factor);

		memset(&values, 0, sizeof(values));
		values.constants = constants;
		nv2a_ff_plan_evaluate(plan, pass->fog_color, &values, fog_color);
		for (channel = 0; channel < 3; channel++)
			color[channel] = clamp_unit(factor * color[channel] + (1.0f - factor) * application_color(fog_color[channel]));
	}
	if (alpha)
		*alpha = color[3];
	if (pass->alpha_test_function)
	{
		float reference = constants->alpha_reference;

		if (pass->alpha_reference != NV2A_FF_EXPRESSION_NONE)
		{
			struct nv2a_ff_values values;
			float value[3];

			memset(&values, 0, sizeof(values));
			values.constants = constants;
			nv2a_ff_plan_evaluate(plan, pass->alpha_reference, &values, value);
			reference = (float)floor(clamp_unit(value[0]) * 255.0f + 0.5f);
		}
		if (!compare_alpha(pass->alpha_test_function, color[3], reference))
			return FALSE;
	}
	framebuffer_write(pass->blend_enable, pass->source_blend, pass->destination_blend, pass->blend_operation,
		pass->write_color, pass->write_alpha, color, constants->blend_color, framebuffer);
	return TRUE;
}

int nv2a_ff_model_pass(const struct nv2a_ff_plan *plan, int pass_index,
	const struct nv2a_ff_constants *constants, const struct nv2a_ff_model_fragment *fragment,
	float framebuffer[4])
{
	return model_pass(plan, pass_index, constants, fragment, framebuffer, NULL);
}

/* ---------- comparing a plan with the reference */

static unsigned long random_next(unsigned long *seed)
{
	*seed = (*seed * 1664525UL + 1013904223UL) & 0xffffffffUL;
	return (*seed >> 8) & 0xffffUL;
}

/* a value on the 8-bit grid; the ends and the middle come up often */
static float random_byte(unsigned long *seed)
{
	unsigned long value = random_next(seed);

	switch (value & 15)
	{
	case 0: return 0.0f;
	case 1: return 1.0f;
	case 2: return 128.0f / 255.0f;
	case 3: return 127.0f / 255.0f;
	default: return (float)((value >> 4) & 0xff) / 255.0f;
	}
}

void nv2a_ff_sample_random(unsigned long *seed, struct nv2a_ff_sample *sample)
{
	int vertex, stage, channel;
	int flat = (random_next(seed) & 3) == 0;
	float weights[3], total;

	for (vertex = 0; vertex < 3; vertex++)
	{
		for (channel = 0; channel < 4; channel++)
		{
			sample->vertex_v0[vertex][channel] = vertex > 0 && flat ? sample->vertex_v0[0][channel] : random_byte(seed);
			sample->vertex_v1[vertex][channel] = vertex > 0 && flat ? sample->vertex_v1[0][channel] : random_byte(seed);
		}
		sample->vertex_fog[vertex] = vertex > 0 && flat ? sample->vertex_fog[0] : random_byte(seed);
		weights[vertex] = (float)(random_next(seed) & 0xff) + 1.0f;
	}
	total = weights[0] + weights[1] + weights[2];
	for (vertex = 0; vertex < 3; vertex++)
		sample->weights[vertex] = weights[vertex] / total;
	for (stage = 0; stage < NV2A_FF_STAGE_COUNT; stage++)
	{
		/* at a texel's centre as often as between two */
		int centre = (random_next(seed) & 1) == 0;

		for (channel = 0; channel < 4; channel++)
		{
			sample->texel[0][stage][channel] = random_byte(seed);
			sample->texel[1][stage][channel] = centre ? sample->texel[0][stage][channel] : random_byte(seed);
			sample->texcoord[stage][channel] = (float)(random_next(seed) & 0xff) / 127.5f - 1.0f;
		}
		sample->texel_weight[stage] = (float)(random_next(seed) & 0xff) / 255.0f;
	}
	for (channel = 0; channel < 4; channel++)
		sample->framebuffer[channel] = (float)(random_next(seed) & 0xff) / 255.0f;
}

float nv2a_ff_sample_compare(const struct nv2a_ff_state *state, const struct nv2a_ff_constants *constants,
	const struct nv2a_ff_capabilities *capabilities, const struct nv2a_ff_draw *draw,
	const struct nv2a_ff_plan *plan, const struct nv2a_ff_sample *sample,
	float reference_framebuffer[4], float plan_framebuffer[4])
{
	float reference_result[4], plan_result[4];
	float difference = 0.0f;
	float reference_alpha = 0.0f, plan_alpha = 0.0f;
	/* the alpha test's verdicts, when both sides ran the state's test */
	int reference_passed = TRUE, plan_passed = TRUE, plan_tested = FALSE, reference_dropped = FALSE;
	int vertex, stage, channel, pass_index, unit;

	memcpy(reference_result, sample->framebuffer, sizeof(reference_result));
	memcpy(plan_result, sample->framebuffer, sizeof(plan_result));
	if (!capabilities->destination_alpha)
		/* without alpha bits the destination's alpha reads as one */
		plan_result[3] = 1.0f;

	if (state)
	{
		struct nv2a_ff_fragment fragment;
		float color[4];

		memset(&fragment, 0, sizeof(fragment));
		for (vertex = 0; vertex < 3; vertex++)
		{
			for (channel = 0; channel < 4; channel++)
			{
				fragment.v0[channel] += sample->weights[vertex] * sample->vertex_v0[vertex][channel];
				fragment.v1[channel] += sample->weights[vertex] * sample->vertex_v1[vertex][channel];
			}
			fragment.fog += sample->weights[vertex] * sample->vertex_fog[vertex];
		}
		for (stage = 0; stage < NV2A_FF_STAGE_COUNT; stage++)
		{
			for (channel = 0; channel < 4; channel++)
			{
				fragment.texel[stage][channel] = sample->texel[0][stage][channel] +
					sample->texel_weight[stage] * (sample->texel[1][stage][channel] - sample->texel[0][stage][channel]);
				fragment.texcoord[stage][channel] = sample->texcoord[stage][channel];
			}
		}
		if (nv2a_ff_reference(state, constants, &fragment, color))
		{
			framebuffer_write(draw->blend_enable, draw->source_blend, draw->destination_blend, draw->blend_operation,
				draw->write_color, draw->write_alpha, color, constants->blend_color, reference_result);
		}
		else
		{
			reference_dropped = TRUE;
		}
		reference_alpha = color[3];
		if (state->alpha_test_function)
			reference_passed = compare_alpha(state->alpha_test_function, color[3], constants->alpha_reference);
	}

	if (plan)
	{
		for (pass_index = 0; pass_index < plan->pass_count; pass_index++)
		{
			const struct nv2a_ff_pass *pass = &plan->passes[pass_index];
			struct nv2a_ff_model_fragment fragment;
			struct nv2a_ff_values values;

			/* a pass that draws where the depth is equal finds no depth
			where the first pass's alpha test left none */
			if (pass_index > 0 && pass->depth_equal && !plan_passed)
				continue;
			memset(&fragment, 0, sizeof(fragment));
			memset(&values, 0, sizeof(values));
			values.constants = constants;
			for (vertex = 0; vertex < 3; vertex++)
			{
				float primary[4], factor[3];

				values.v0 = sample->vertex_v0[vertex];
				values.v1 = sample->vertex_v1[vertex];
				values.fog_factor = state ? fog_factor_from_coordinate(state, constants, sample->vertex_fog[vertex]) :
					clamp_unit(sample->vertex_fog[vertex]);
				nv2a_ff_plan_primary(plan, pass_index, &values, primary);
				for (channel = 0; channel < 4; channel++)
					fragment.primary[channel] += sample->weights[vertex] * application_color(primary[channel]);
				nv2a_ff_plan_secondary(plan, pass_index, &values, primary);
				for (channel = 0; channel < 3; channel++)
					fragment.secondary[channel] += sample->weights[vertex] * application_color(primary[channel]);
				if (pass->fog_enable)
				{
					nv2a_ff_plan_evaluate(plan, pass->fog_factor, &values, factor);
					fragment.fog_factor += sample->weights[vertex] * clamp_unit(factor[0]);
				}
			}
			for (unit = 0; unit < pass->unit_count; unit++)
			{
				int unit_stage = pass->units[unit].stage;

				if (unit_stage == NV2A_FF_STAGE_NONE)
				{
					fragment.texel[unit][0] = fragment.texel[unit][1] = fragment.texel[unit][2] =
						fragment.texel[unit][3] = 1.0f;
					continue;
				}
				for (channel = 0; channel < 4; channel++)
				{
					fragment.texel[unit][channel] = sample->texel[0][unit_stage][channel] +
						sample->texel_weight[unit_stage] *
						(sample->texel[1][unit_stage][channel] - sample->texel[0][unit_stage][channel]);
				}
				if (pass->units[unit].bake != NV2A_FF_BAKE_NONE)
				{
					float baked[2][4];

					/* the derived texture's texels are bytes; the filter runs on them */
					nv2a_ff_plan_bake(plan, pass->units[unit].bake, constants, sample->texel[0][unit_stage], baked[0]);
					nv2a_ff_plan_bake(plan, pass->units[unit].bake, constants, sample->texel[1][unit_stage], baked[1]);
					for (channel = 0; channel < 4; channel++)
					{
						baked[0][channel] = round_to_byte(baked[0][channel]);
						baked[1][channel] = round_to_byte(baked[1][channel]);
						fragment.texel[unit][channel] = baked[0][channel] +
							sample->texel_weight[unit_stage] * (baked[1][channel] - baked[0][channel]);
					}
				}
			}
			if (pass_index == 0)
			{
				plan_passed = model_pass(plan, pass_index, constants, &fragment, plan_result, &plan_alpha);
				plan_tested = state && pass->alpha_test_function == state->alpha_test_function &&
					pass->alpha_reference == NV2A_FF_EXPRESSION_NONE;
			}
			else
			{
				model_pass(plan, pass_index, constants, &fragment, plan_result, NULL);
			}
		}
	}

	/* A fragment whose alpha is within the plan's tolerance of the test's
	reference can pass on one side and fail on the other: that is the
	tolerance at work, not a difference in what is computed, and it counts
	by how far apart the two alphas are. */
	if (state && plan && state->alpha_test_function && plan_tested && reference_passed != plan_passed &&
		!(reference_passed && reference_dropped))
	{
		float distance = (float)fabs(reference_alpha - plan_alpha) * 255.0f;

		if (distance <= (float)plan->tolerance + 0.01f)
		{
			if (reference_framebuffer)
				memcpy(reference_framebuffer, reference_result, sizeof(reference_result));
			if (plan_framebuffer)
				memcpy(plan_framebuffer, plan_result, sizeof(plan_result));
			return distance;
		}
	}

	for (channel = 0; channel < 4; channel++)
	{
		float distance = (float)fabs(reference_result[channel] - plan_result[channel]) * 255.0f;

		if (channel < 3 ? !draw->write_color : !(draw->write_alpha && capabilities->destination_alpha))
			continue;
		if (distance > difference)
			difference = distance;
	}
	if (reference_framebuffer)
		memcpy(reference_framebuffer, reference_result, sizeof(reference_result));
	if (plan_framebuffer)
		memcpy(plan_framebuffer, plan_result, sizeof(plan_result));
	return difference;
}

static void random_constants(unsigned long *seed, struct nv2a_ff_constants *constants)
{
	int index, channel;

	memset(constants, 0, sizeof(*constants));
	for (index = 0; index < 8; index++)
	{
		for (channel = 0; channel < 4; channel++)
		{
			constants->c0[index][channel] = random_byte(seed);
			constants->c1[index][channel] = random_byte(seed);
		}
	}
	for (channel = 0; channel < 4; channel++)
	{
		constants->final_c0[channel] = random_byte(seed);
		constants->final_c1[channel] = random_byte(seed);
		constants->fog_color[channel] = random_byte(seed);
		constants->blend_color[channel] = random_byte(seed);
	}
	constants->fog_parameters[0] = 0.0f;
	constants->fog_parameters[1] = 1.0f;
	constants->fog_parameters[2] = 1.0f;
	constants->alpha_reference = (float)(random_next(seed) & 0xff);
	for (index = 0; index < NV2A_FF_STAGE_COUNT; index++)
	{
		constants->bump_luminance[index][0] = 1.0f;
		constants->bump_luminance[index][1] = 0.0f;
	}
}

/* the constants a plan can be made for: the atom sources that are colours
of struct nv2a_ff_constants */
#define BOUND_SOURCES_ALL (((1UL << NUMBER_OF_NV2A_FF_ATOM_SOURCES) - 1UL) & ~((1UL << _nv2a_ff_atom_fog) - 1UL))

/* the colours of these sources from one set of constants into another */
static void constants_copy_sources(const struct nv2a_ff_constants *from, struct nv2a_ff_constants *to,
	unsigned long sources)
{
	int source;

	for (source = _nv2a_ff_atom_fog; source < NUMBER_OF_NV2A_FF_ATOM_SOURCES; source++)
	{
		if (sources & (1UL << source))
			memcpy((float *)constant_color(to, source), constant_color(from, source), 4 * sizeof(float));
	}
}

void nv2a_ff_plan_bound_constants(const struct nv2a_ff_plan *plan, const struct nv2a_ff_constants *from,
	struct nv2a_ff_constants *to)
{
	if (plan->constants_bound)
		constants_copy_sources(from, to, plan->bound_sources);
}

int nv2a_ff_plan_constants_match(const struct nv2a_ff_plan *plan, const struct nv2a_ff_constants *a,
	const struct nv2a_ff_constants *b)
{
	int source;

	if (!plan->constants_bound)
		return TRUE;
	for (source = _nv2a_ff_atom_fog; source < NUMBER_OF_NV2A_FF_ATOM_SOURCES; source++)
	{
		if ((plan->bound_sources & (1UL << source)) &&
			memcmp(constant_color(a, source), constant_color(b, source), 4 * sizeof(float)))
		{
			return FALSE;
		}
	}
	return TRUE;
}

/* The constants for a group of sample fragments: the bound ones when the
plan is made for all of them; random ones otherwise, with the values of
those the plan is made for put in. */
static void sample_constants(unsigned long *seed, struct nv2a_ff_constants *constants)
{
	if (work.bound_constants && work.bound_sources == BOUND_SOURCES_ALL)
	{
		*constants = *work.bound_constants;
		return;
	}
	random_constants(seed, constants);
	if (work.bound_constants)
		constants_copy_sources(work.bound_constants, constants, work.bound_sources);
}

enum
{
	CHECK_SAMPLES = 64
};

/* How far a combine side's result can be from the true value, in steps of
1/255, when its arguments are off by the given amounts (colour and alpha
of each source). */
static float combine_error(const struct nv2a_ff_combine *combine, const float source_error[4][2])
{
	float errors[3], error;
	int argument;

	for (argument = 0; argument < 3; argument++)
	{
		int alpha = combine->operand[argument] == _nv2a_ff_operand_alpha ||
			combine->operand[argument] == _nv2a_ff_operand_inverse_alpha;

		errors[argument] = source_error[combine->source[argument]][alpha];
	}
	switch (combine->function)
	{
	case _nv2a_ff_combine_replace: error = errors[0]; break;
	case _nv2a_ff_combine_interpolate: error = (errors[0] > errors[1] ? errors[0] : errors[1]) + errors[2]; break;
	case _nv2a_ff_combine_dot3: error = 6.0f * (errors[0] + errors[1]); break;
	default: error = errors[0] + errors[1]; break;
	}
	return error * (float)combine->scale;
}

/* The tolerance of an exact plan: what rounding alone can do to it. A
derived texture's texels are rounded to bytes (half a step each way), each
pass rounds what it writes (half a step), and so does the reference;
units and blends pass these on, scaled or added up. */
static int plan_tolerance(const struct nv2a_ff_plan *plan)
{
	float total = 0.0f;
	int pass_index, unit_index;

	for (pass_index = 0; pass_index < plan->pass_count; pass_index++)
	{
		const struct nv2a_ff_pass *pass = &plan->passes[pass_index];
		float source_error[4][2];
		float error;

		memset(source_error, 0, sizeof(source_error));
		for (unit_index = 0; unit_index < pass->unit_count; unit_index++)
		{
			const struct nv2a_ff_unit *unit = &pass->units[unit_index];
			float texture = unit->bake != NV2A_FF_BAKE_NONE ? 0.5f : 0.0f;
			float *previous = source_error[_nv2a_ff_source_previous];
			float rgb, alpha;

			source_error[_nv2a_ff_source_texture][0] = source_error[_nv2a_ff_source_texture][1] = texture;
			switch (unit->environment)
			{
			case _nv2a_ff_environment_replace:
				rgb = alpha = texture;
				break;
			case _nv2a_ff_environment_decal:
				rgb = previous[0] + 2.0f * texture;
				alpha = previous[1];
				break;
			case _nv2a_ff_environment_combine:
				rgb = combine_error(&unit->rgb, (const float (*)[2])source_error);
				alpha = combine_error(&unit->alpha, (const float (*)[2])source_error);
				break;
			default:
				rgb = previous[0] + texture;
				alpha = previous[1] + texture;
				break;
			}
			previous[0] = rgb;
			previous[1] = alpha;
		}
		error = source_error[_nv2a_ff_source_previous][0];
		if (pass->blend_enable)
		{
			if (pass->source_blend == _nv2a_ff_blend_source_alpha || pass->source_blend == _nv2a_ff_blend_inverse_source_alpha)
				error += source_error[_nv2a_ff_source_previous][1];
			if (pass->destination_blend == _nv2a_ff_blend_source_alpha ||
				pass->destination_blend == _nv2a_ff_blend_inverse_source_alpha)
			{
				error += source_error[_nv2a_ff_source_previous][1];
			}
			/* a factor that reads the source's colour: the result moves by up
			to twice what the colour does (source * destination twice over,
			source * alpha + destination * source, ...) */
			if (pass->source_blend == _nv2a_ff_blend_source_color ||
				pass->source_blend == _nv2a_ff_blend_inverse_source_color ||
				pass->destination_blend == _nv2a_ff_blend_source_color ||
				pass->destination_blend == _nv2a_ff_blend_inverse_source_color)
			{
				error *= 2.0f;
			}
		}
		if (source_error[_nv2a_ff_source_previous][1] > error)
			error = source_error[_nv2a_ff_source_previous][1];
		total += error;
		/* every pass but the last: its rounding stays in the framebuffer */
		if (pass_index + 1 < plan->pass_count)
			total += 0.5f;
	}
	return (int)(total + 1.0e-3f) + 1;
}

/* The plan's status, its tolerance and the errors of its own comparison
with the reference. */
static void plan_finish(struct nv2a_ff_plan *plan)
{
	unsigned long seed = 0x2545f491UL ^ plan->state_hash;
	struct nv2a_ff_constants constants;
	float total = 0.0f, maximum = 0.0f;
	int every = work.bound_constants && work.bound_sources == BOUND_SOURCES_ALL;
	int index;

	plan->tolerance = (unsigned char)plan_tolerance(plan);
	if (every)
		constants = *work.bound_constants;
	for (index = 0; index < CHECK_SAMPLES; index++)
	{
		struct nv2a_ff_sample sample;
		float difference;

		if (!every && (index & 3) == 0)
		{
			sample_constants(&seed, &constants);
			if (work.state->alpha_test_function == 0)
				constants.alpha_reference = 0.0f;
		}
		nv2a_ff_sample_random(&seed, &sample);
		difference = nv2a_ff_sample_compare(work.state, &constants, work.capabilities, work.draw, plan, &sample,
			NULL, NULL);
		total += difference;
		if (difference > maximum)
			maximum = difference;
	}
	plan->mean_error = total / (float)CHECK_SAMPLES;
	plan->maximum_error = maximum;
	if (!plan->approximations && maximum > (float)plan->tolerance + 0.01f)
		plan->approximations |= _nv2a_ff_approximate_unexplained;
	plan->status = (unsigned char)(plan->approximations ? _nv2a_ff_plan_approximate : _nv2a_ff_plan_exact);
}

static void combine_set(struct nv2a_ff_combine *combine, int function, int source0, int operand0, int source1,
	int operand1, int source2, int operand2)
{
	combine->function = (unsigned char)function;
	combine->source[0] = (unsigned char)source0;
	combine->operand[0] = (unsigned char)operand0;
	combine->source[1] = (unsigned char)source1;
	combine->operand[1] = (unsigned char)operand1;
	combine->source[2] = (unsigned char)source2;
	combine->operand[2] = (unsigned char)operand2;
	combine->scale = 1;
}

/* The OpenGL 1.1 environments read a texel by the base format its texture
is stored in: REPLACE keeps the alpha of what came before when the texture
has none (RGB, LUMINANCE), DECAL is not defined for the luminance formats,
and ADD and BLEND do not multiply by the alpha of an INTENSITY texture. The
plans count on a texel being red, green, blue and alpha whatever it is
stored as, alpha one where there is none, which is how the combine
environments read it. A renderer stores textures in the smaller formats,
and the compiler is not told which: so where there are combine
environments they stand in for the ones that differ, with the same
arithmetic. MODULATE is the same for every format; so is REPLACE at the
first unit of a pass that gives no primary alpha, which is then one.
Derived textures and the texture of a unit without a stage are RGBA.

Without the extension the plans stay as they are, and it is the renderer's
to keep the textures of DECAL, BLEND and ADD units in a format that has
all four channels. */
static void plan_portable_environments(struct nv2a_ff_plan *plan, const struct nv2a_ff_capabilities *capabilities)
{
	int pass_index, unit_index;

	if (!capabilities->combine || plan->status == _nv2a_ff_plan_impossible)
		return;
	for (pass_index = 0; pass_index < plan->pass_count && pass_index < NV2A_FF_MAXIMUM_PASSES; pass_index++)
	{
		struct nv2a_ff_pass *pass = &plan->passes[pass_index];

		for (unit_index = 0; unit_index < pass->unit_count && unit_index < NV2A_FF_MAXIMUM_UNITS; unit_index++)
		{
			struct nv2a_ff_unit *unit = &pass->units[unit_index];
			const int texture = _nv2a_ff_source_texture, previous = _nv2a_ff_source_previous;
			const int color = _nv2a_ff_operand_color, alpha = _nv2a_ff_operand_alpha;

			if (unit->stage == NV2A_FF_STAGE_NONE || unit->bake != NV2A_FF_BAKE_NONE)
				continue;
			switch (unit->environment)
			{
			case _nv2a_ff_environment_replace:
				if (unit_index == 0 && pass->primary_alpha == NV2A_FF_EXPRESSION_NONE)
					continue;
				combine_set(&unit->rgb, _nv2a_ff_combine_replace, texture, color, previous, color, previous, alpha);
				combine_set(&unit->alpha, _nv2a_ff_combine_replace, texture, alpha, previous, alpha, previous, alpha);
				break;
			case _nv2a_ff_environment_decal:
				combine_set(&unit->rgb, _nv2a_ff_combine_interpolate, texture, color, previous, color, texture, alpha);
				combine_set(&unit->alpha, _nv2a_ff_combine_replace, previous, alpha, previous, alpha, previous, alpha);
				break;
			case _nv2a_ff_environment_add:
				combine_set(&unit->rgb, _nv2a_ff_combine_add, previous, color, texture, color, previous, alpha);
				combine_set(&unit->alpha, _nv2a_ff_combine_modulate, previous, alpha, texture, alpha, previous, alpha);
				break;
			case _nv2a_ff_environment_blend:
				/* (the texel's colour as the third argument: not in the EXT extension) */
				if (!capabilities->combine_arb)
					continue;
				combine_set(&unit->rgb, _nv2a_ff_combine_interpolate, _nv2a_ff_source_constant, color, previous, color,
					texture, color);
				combine_set(&unit->alpha, _nv2a_ff_combine_modulate, previous, alpha, texture, alpha, previous, alpha);
				break;
			default:
				continue;
			}
			unit->environment = _nv2a_ff_environment_combine;
		}
	}
}

/* ---------- the compiler */

unsigned long nv2a_ff_state_hash(const struct nv2a_ff_state *state)
{
	unsigned long hash = 2166136261UL;
	unsigned long words[8 * 4 + 16];
	unsigned long count = state->combiner_state[_state_combiner_count] & 0xff;
	int word_count = 0, index, byte;

	if (count > 8)
		count = 8;
	words[word_count++] = state->texture_modes;
	words[word_count++] = state->combiner_state[_state_combiner_count];
	words[word_count++] = state->combiner_state[_state_final_inputs_abcd];
	words[word_count++] = state->combiner_state[_state_final_inputs_efg];
	words[word_count++] = state->combiner_state[_state_dot_mapping];
	words[word_count++] = state->combiner_state[_state_input_texture];
	words[word_count++] = state->combiner_state[_state_compare_mode];
	words[word_count++] = state->alpha_test_function;
	words[word_count++] = ((unsigned long)state->fog_enable << 8) | state->fog_table_mode;
	for (index = 0; index < NV2A_FF_STAGE_COUNT; index++)
	{
		words[word_count++] = ((unsigned long)state->sampler_type[index] << 16) |
			((unsigned long)state->alpha_kill[index] << 8) | state->color_sign[index];
	}
	for (index = 0; index < (int)count; index++)
	{
		words[word_count++] = state->combiner_state[_state_alpha_inputs + index];
		words[word_count++] = state->combiner_state[_state_alpha_outputs + index];
		words[word_count++] = state->combiner_state[_state_rgb_inputs + index];
		words[word_count++] = state->combiner_state[_state_rgb_outputs + index];
	}
	for (index = 0; index < word_count; index++)
	{
		for (byte = 0; byte < 4; byte++)
		{
			hash ^= (words[index] >> (8 * byte)) & 0xff;
			hash = (hash * 16777619UL) & 0xffffffffUL;
		}
	}
	return hash;
}

enum
{
	MAXIMUM_OVERRIDES = 32,
	ATTEMPT_LIMIT = 24
};

/* a mean error (in steps of 1/255 over random fragments) below which the
search for something better stops */
#define GOOD_ENOUGH_ERROR 3.0f

static struct
{
	unsigned long state_hash;
	nv2a_ff_override_function function;
} overrides[MAXIMUM_OVERRIDES];
static int override_count;

int nv2a_ff_override_register(unsigned long state_hash, nv2a_ff_override_function function)
{
	if (override_count == MAXIMUM_OVERRIDES)
		return FALSE;
	overrides[override_count].state_hash = state_hash;
	overrides[override_count].function = function;
	override_count++;
	return TRUE;
}

struct nv2a_ff_pass *nv2a_ff_override_pass(struct nv2a_ff_plan *plan, const struct nv2a_ff_state *state,
	const struct nv2a_ff_draw *draw)
{
	struct nv2a_ff_pass *pass;

	if (plan->pass_count == NV2A_FF_MAXIMUM_PASSES)
		return NULL;
	pass = &plan->passes[plan->pass_count++];
	memset(pass, 0, sizeof(*pass));
	pass->primary_rgb = NV2A_FF_EXPRESSION_NONE;
	pass->primary_alpha = NV2A_FF_EXPRESSION_NONE;
	pass->secondary_rgb = NV2A_FF_EXPRESSION_NONE;
	pass->fog_factor = NV2A_FF_EXPRESSION_NONE;
	pass->fog_color = NV2A_FF_EXPRESSION_NONE;
	pass->alpha_reference = NV2A_FF_EXPRESSION_NONE;
	pass->alpha_test_function = (unsigned short)state->alpha_test_function;
	pass->blend_enable = draw->blend_enable;
	pass->source_blend = (unsigned short)draw->source_blend;
	pass->destination_blend = (unsigned short)draw->destination_blend;
	pass->blend_operation = (unsigned short)draw->blend_operation;
	pass->write_color = draw->write_color;
	pass->write_alpha = draw->write_alpha;
	return pass;
}

struct nv2a_ff_unit *nv2a_ff_override_unit(struct nv2a_ff_pass *pass, int stage, int environment)
{
	struct nv2a_ff_unit *unit;
	int argument;

	if (pass->unit_count == NV2A_FF_MAXIMUM_UNITS)
		return NULL;
	unit = &pass->units[pass->unit_count++];
	memset(unit, 0, sizeof(*unit));
	unit->stage = (unsigned char)stage;
	unit->bake = NV2A_FF_BAKE_NONE;
	unit->environment = (unsigned char)environment;
	unit->constant_rgb = NV2A_FF_EXPRESSION_NONE;
	unit->constant_alpha = NV2A_FF_EXPRESSION_NONE;
	unit->rgb.scale = unit->alpha.scale = 1;
	for (argument = 0; argument < 3; argument++)
	{
		unit->rgb.source[argument] = unit->alpha.source[argument] = _nv2a_ff_source_previous;
		unit->alpha.operand[argument] = _nv2a_ff_operand_alpha;
	}
	return unit;
}

short nv2a_ff_override_expression(struct nv2a_ff_plan *plan, short sum_with, float factor, int atom0, int atom1)
{
	struct nv2a_ff_expression *expression;
	struct nv2a_ff_term *term;
	int count = 1, index;

	if (sum_with >= 0 && sum_with < plan->expression_count)
		count += plan->expressions[sum_with].term_count;
	if (plan->expression_count == NV2A_FF_MAXIMUM_EXPRESSIONS || plan->term_count + count > NV2A_FF_MAXIMUM_TERMS)
		return NV2A_FF_EXPRESSION_NONE;
	expression = &plan->expressions[plan->expression_count];
	expression->first_term = plan->term_count;
	expression->term_count = (unsigned short)count;
	for (index = 0; index < count - 1; index++)
		plan->terms[plan->term_count++] = plan->terms[plan->expressions[sum_with].first_term + index];
	term = &plan->terms[plan->term_count++];
	memset(term, 0, sizeof(*term));
	term->coefficient[0] = term->coefficient[1] = term->coefficient[2] = factor;
	if (atom0 != NV2A_FF_ATOM_ONE)
		term->atoms[term->count++] = (unsigned char)atom0;
	if (atom1 != NV2A_FF_ATOM_ONE)
		term->atoms[term->count++] = (unsigned char)atom1;
	return (short)plan->expression_count++;
}

void nv2a_ff_capabilities_level(int level, struct nv2a_ff_capabilities *capabilities)
{
	memset(capabilities, 0, sizeof(*capabilities));
	capabilities->units = (unsigned char)(level >= 1 ? 2 : 1);
	capabilities->combine = (unsigned char)(level >= 2);
	capabilities->environment_add = (unsigned char)(level >= 2);
	capabilities->bakes = _nv2a_ff_bakes_all;
	capabilities->maximum_passes = NV2A_FF_MAXIMUM_PASSES;
}

/* one round of an attempt: the passes for the goals under these rules */
static int attempt_round(int rank, int bakes, int strict, int single_pass, int drop_terms,
	unsigned long approximations, struct nv2a_ff_plan *plan)
{
	int store_count = work.store_count;
	int opaque_count = work.opaque_count;
	int maximum_passes = work.capabilities->maximum_passes;
	int succeeded;

	if (maximum_passes == 0 || maximum_passes > NV2A_FF_MAXIMUM_PASSES)
		maximum_passes = NV2A_FF_MAXIMUM_PASSES;
	if (bakes > work.capabilities->bakes && strict)
		return FALSE;
	if (budget_spent())
		return FALSE;
	work.bake_policy = bakes < work.capabilities->bakes ? bakes : work.capabilities->bakes;
	work.strict_primary = strict;
	work.maximum_passes = maximum_passes;
	work.single_pass = single_pass;
	work.policy_rank = rank;
	work.drop_terms = drop_terms;
	work.steps = 0;
	work.total_steps = 0;
	work.approximations = approximations;
	work.overflow = FALSE;
	succeeded = sequence_plan() && !work.overflow && export_sequence(plan);
	/* what the round added to the arena is not needed any more */
	work.store_count = store_count;
	work.opaque_count = opaque_count;
	return succeeded;
}

/* One attempt with the substitutes and settings in the work area. What
the plan may use, cheapest first: derived textures cost memory and, when
they read constants, time whenever those change; passes cost fill rate. */
static int compile_attempt(struct nv2a_ff_plan *plan)
{
	/* static: plans are large */
	static struct nv2a_ff_plan permissive, without_clamps;
	struct polynomial rgb, alpha, declamped_rgb, declamped_alpha;
	unsigned long approximations;
	int have_terms, have_clamps = FALSE;

	work.statistics.attempts++;
	symbolic_evaluate();
	if (work.overflow)
		return FALSE;
	approximations = work.approximations;

	/* one pass, nothing derived, nothing relaxed */
	if (attempt_round(0, _nv2a_ff_bakes_none, TRUE, TRUE, FALSE, approximations, plan))
		return TRUE;
	/* everything allowed: when this finds nothing the stricter rounds will not either */
	if (attempt_round(3, _nv2a_ff_bakes_all, FALSE, FALSE, FALSE, approximations, &permissive))
	{
		if (!attempt_round(1, _nv2a_ff_bakes_static, TRUE, TRUE, FALSE, approximations, plan) &&
			!attempt_round(0, _nv2a_ff_bakes_none, TRUE, FALSE, FALSE, approximations, plan) &&
			!attempt_round(1, _nv2a_ff_bakes_static, TRUE, FALSE, FALSE, approximations, plan) &&
			!attempt_round(2, _nv2a_ff_bakes_all, TRUE, FALSE, FALSE, approximations, plan))
		{
			*plan = permissive;
		}
		return TRUE;
	}

	/* Nothing computes the goals as they are. Two ways on: leave a term
	out, or take the clamps between the stages away (which leaves a
	polynomial that can be split into passes); the comparison with the
	reference says which is closer. */
	rgb = work.goal_rgb;
	alpha = work.goal_alpha;
	have_terms = attempt_round(4, _nv2a_ff_bakes_all, FALSE, FALSE, TRUE, approximations, plan);
	if (polynomial_declamp(&declamped_rgb, &rgb, 0) && polynomial_declamp(&declamped_alpha, &alpha, 0) &&
		(!polynomial_equal(&declamped_rgb, &rgb) || !polynomial_equal(&declamped_alpha, &alpha)))
	{
		work.goal_rgb = declamped_rgb;
		work.goal_alpha = declamped_alpha;
		have_clamps = attempt_round(3, _nv2a_ff_bakes_all, FALSE, FALSE, FALSE,
			approximations | _nv2a_ff_approximate_clamp, &without_clamps) ||
			attempt_round(4, _nv2a_ff_bakes_all, FALSE, FALSE, TRUE,
			approximations | _nv2a_ff_approximate_clamp, &without_clamps);
		work.goal_rgb = rgb;
		work.goal_alpha = alpha;
	}
	if (have_terms && have_clamps)
	{
		plan->state_hash = without_clamps.state_hash = nv2a_ff_state_hash(work.state);
		plan_finish(plan);
		plan_finish(&without_clamps);
		if (without_clamps.mean_error < plan->mean_error)
			*plan = without_clamps;
	}
	else if (have_clamps)
	{
		*plan = without_clamps;
	}
	return have_terms || have_clamps;
}

enum
{
	SUBSTITUTION_SAMPLES = 32
};

/* what substitution_error asks about, kept for one search */
static struct
{
	int valid;
	struct nv2a_ff_constants constants[SUBSTITUTION_SAMPLES / 4];
	struct nv2a_ff_sample samples[SUBSTITUTION_SAMPLES];
	float original[SUBSTITUTION_SAMPLES][4];
} substitution;

/* How far the state's colour moves when the stages in dropped are given
the substitutes in substitute, in steps of 1/255 over sample fragments:
the smaller, the less the plan loses by dropping them. */
static float substitution_error(const struct nv2a_ff_constants *bound_constants, int dropped,
	const unsigned char *substitute)
{
	float total = 0.0f;
	int index, stage, channel;
	int every = bound_constants && work.bound_sources == BOUND_SOURCES_ALL;

	if (!substitution.valid)
	{
		/* the sample fragments and the state's colour for them: the same
		for every question a search asks */
		unsigned long seed = 0x9e3779b9UL;

		for (index = 0; index < SUBSTITUTION_SAMPLES; index++)
		{
			struct nv2a_ff_constants *constants = &substitution.constants[every ? 0 : index / 4];

			if (every ? index == 0 : (index & 3) == 0)
				sample_constants(&seed, constants);
			nv2a_ff_sample_random(&seed, &substitution.samples[index]);
			nv2a_ff_sample_compare(work.state, constants, work.capabilities, work.draw, NULL,
				&substitution.samples[index], substitution.original[index], NULL);
		}
		substitution.valid = TRUE;
	}
	for (index = 0; index < SUBSTITUTION_SAMPLES; index++)
	{
		const struct nv2a_ff_constants *constants = &substitution.constants[every ? 0 : index / 4];
		const float *original = substitution.original[index];
		struct nv2a_ff_sample sample = substitution.samples[index];
		float changed[4], difference = 0.0f;

		for (stage = 0; stage < NV2A_FF_STAGE_COUNT; stage++)
		{
			if (dropped & (1 << stage))
			{
				memcpy(sample.texel[0][stage], substitute_colors[substitute[stage]], 4 * sizeof(float));
				memcpy(sample.texel[1][stage], substitute_colors[substitute[stage]], 4 * sizeof(float));
			}
		}
		nv2a_ff_sample_compare(work.state, constants, work.capabilities, work.draw, NULL, &sample, changed, NULL);
		for (channel = 0; channel < 4; channel++)
		{
			float distance = (float)fabs(original[channel] - changed[channel]) * 255.0f;

			if (channel < 3 ? !work.draw->write_color : !(work.draw->write_alpha && work.capabilities->destination_alpha))
				continue;
			if (distance > difference)
				difference = distance;
		}
		total += difference;
	}
	return total / (float)SUBSTITUTION_SAMPLES;
}

/* How much a stage's texture matters, 16 to 1024: sums of fewer stages
are smaller than any one stage above them. */
static int stage_weight(int stage)
{
	const unsigned char *rank = work.draw->stage_rank;

	return 16 << (2 * (rank[0] | rank[1] | rank[2] | rank[3] ? rank[stage] & 3 : NV2A_FF_STAGE_COUNT - 1 - stage));
}

/* what a plan lost with the textures it dropped */
static int plan_dropped_weight(const struct nv2a_ff_plan *plan)
{
	int stage, weight = 0;

	for (stage = 0; stage < NV2A_FF_STAGE_COUNT; stage++)
	{
		if (plan->dropped_stage[stage] != _nv2a_ff_substitute_none)
			weight += stage_weight(stage);
	}
	return weight;
}

/* A stage's substitute: of the four, the one that changes the picture
least when this stage alone is replaced (a detail map that is doubled
comes out grey, a mask that is added black). */
static unsigned char neutral_substitute(const struct nv2a_ff_constants *bound_constants, int stage,
	unsigned char *neutral)
{
	static const unsigned char substitute_order[4] =
	{
		_nv2a_ff_substitute_white, _nv2a_ff_substitute_black, _nv2a_ff_substitute_grey, _nv2a_ff_substitute_flat
	};
	unsigned char substitute[NV2A_FF_STAGE_COUNT];
	float best = 0.0f;
	int choice;

	if (neutral[stage] != _nv2a_ff_substitute_none)
		return neutral[stage];
	memset(substitute, 0, sizeof(substitute));
	for (choice = 0; choice < 4; choice++)
	{
		float error;

		substitute[stage] = substitute_order[choice];
		error = substitution_error(bound_constants, 1 << stage, substitute);
		if (choice == 0 || error < best - 1.0e-3f)
		{
			best = error;
			neutral[stage] = substitute_order[choice];
		}
	}
	return neutral[stage];
}

/* The constants that decide what the state computes rather than scale it:
the ones inside its clamps, dot products and multiplexers (a mask that
picks a colour's channel, a factor that keeps a sum under one). Their
values known, such an operation often becomes a polynomial. */
static unsigned long polynomial_deciding_sources(const struct polynomial *polynomial, int inside)
{
	unsigned long sources = 0;
	int index, atom, operand;

	for (index = 0; index < polynomial->count; index++)
	{
		for (atom = 0; atom < polynomial->terms[index].count; atom++)
		{
			int candidate = polynomial->terms[index].atoms[atom];

			if (candidate < LEAF_ATOM_COUNT)
			{
				if (inside && (atom_dependencies(candidate) & _depends_constants))
					sources |= 1UL << (candidate / 3);
				continue;
			}
			for (operand = 0; operand < 3; operand++)
			{
				int stored = work.opaque[candidate - LEAF_ATOM_COUNT].operands[operand];

				if (stored >= 0)
					sources |= polynomial_deciding_sources(&work.store[stored], TRUE);
			}
		}
	}
	return sources;
}

static unsigned long deciding_sources(void)
{
	work.bound_constants = NULL;
	work.bound_sources = 0;
	memset(work.substitute, 0, sizeof(work.substitute));
	symbolic_evaluate();
	if (work.overflow)
		return 0;
	return (polynomial_deciding_sources(&work.goal_rgb, FALSE) | polynomial_deciding_sources(&work.goal_alpha, FALSE)) &
		BOUND_SOURCES_ALL;
}

/* the best plan for these constants (NULL: for all) with the values of
these sources put in, dropping textures when it has to */
static void compile_search_once(const struct nv2a_ff_constants *bound_constants, unsigned long bound_sources,
	struct nv2a_ff_plan *plan)
{
	/* static: plans are large */
	static struct nv2a_ff_plan candidate;
	struct
	{
		unsigned char substitute[NV2A_FF_STAGE_COUNT];
		int used;
	} subsets[16];
	/* per stage: what stands in for its texel when it is dropped, or
	_nv2a_ff_substitute_none while nobody has asked */
	unsigned char neutral[NV2A_FF_STAGE_COUNT];
	int forced = 0, optional = 0, optional_count = 0;
	int stage, extra, ignore_alpha, subset, attempts = 0;
	int found = FALSE, levels_after_found = 0;
	int stage_rank[NV2A_FF_STAGE_COUNT];
	unsigned long hash = nv2a_ff_state_hash(work.state);

	work.bound_constants = bound_constants;
	work.bound_sources = bound_constants ? bound_sources : 0;
	work.step_base = work.statistics.steps;
	work.step_budget = SEARCH_STEP_LIMIT;
	for (stage = 0; stage < NV2A_FF_STAGE_COUNT; stage++)
		stage_rank[stage] = stage_weight(stage);
	for (stage = 0; stage < NV2A_FF_STAGE_COUNT; stage++)
	{
		int kind = stage_kind(work.state, work.capabilities, stage);

		if (kind == 2)
			forced |= 1 << stage;
		if (kind == 1)
		{
			optional |= 1 << stage;
			optional_count++;
		}
	}
	memset(plan, 0, sizeof(*plan));
	plan->status = _nv2a_ff_plan_impossible;
	plan->state_hash = hash;

	/* stages the result does not depend on need no substitute and are no
	candidates for dropping */
	memset(work.substitute, 0, sizeof(work.substitute));
	symbolic_evaluate();
	if (!work.overflow)
	{
		const struct nv2a_ff_draw *draw = work.draw;
		int dependencies = 0;

		if (draw->write_color)
			dependencies |= polynomial_dependencies(&work.goal_rgb);
		/* alpha: when it is tested, blended with or written */
		if (work.state->alpha_test_function || work.kill_stages ||
			(draw->blend_enable && (factor_reads_source_alpha(draw->source_blend) ||
				factor_reads_source_alpha(draw->destination_blend) ||
				draw->blend_operation == _nv2a_ff_blend_operation_minimum ||
				draw->blend_operation == _nv2a_ff_blend_operation_maximum)) ||
			(draw->write_alpha && work.capabilities->destination_alpha))
		{
			dependencies |= polynomial_dependencies(&work.goal_alpha);
		}
		forced &= dependencies;
		optional_count = 0;
		for (stage = 0; stage < NV2A_FF_STAGE_COUNT; stage++)
		{
			if (!(dependencies & (_depends_stage0 << stage)))
				optional &= ~(1 << stage);
			else if (optional & (1 << stage))
				optional_count++;
		}
	}

	/* (the stages' substitutes are worked out when a stage is first dropped) */
	substitution.valid = FALSE;
	memset(neutral, 0, sizeof(neutral));

	for (ignore_alpha = 0; ignore_alpha < 2 && !found; ignore_alpha++)
	{
		if (ignore_alpha && !(work.draw->write_alpha && work.capabilities->destination_alpha))
			break;
		work.ignore_destination_alpha = ignore_alpha;
		for (extra = 0; extra <= optional_count; extra++)
		{
			int found_here = FALSE;

			/* the sets of this many stages to drop */
			for (subset = 0; subset < 16; subset++)
			{
				int dropped = forced | subset;
				int bits = 0;

				subsets[subset].used = FALSE;
				if ((subset & optional) != subset)
					continue;
				for (stage = 0; stage < NV2A_FF_STAGE_COUNT; stage++)
					bits += (subset >> stage) & 1;
				if (bits != extra)
					continue;
				subsets[subset].used = TRUE;
				for (stage = 0; stage < NV2A_FF_STAGE_COUNT; stage++)
				{
					subsets[subset].substitute[stage] = (unsigned char)((dropped & (1 << stage)) ?
						neutral_substitute(bound_constants, stage, neutral) : _nv2a_ff_substitute_none);
				}
			}

			/* plans for them, the stages that matter least first; the
			first that works is taken */
			for (;;)
			{
				int best = -1, best_rank = 0;

				for (subset = 15; subset >= 0; subset--)
				{
					int rank = 0;

					if (!subsets[subset].used)
						continue;
					for (stage = 0; stage < NV2A_FF_STAGE_COUNT; stage++)
					{
						if (subset & (1 << stage))
							rank += stage_rank[stage];
					}
					if (best < 0 || rank < best_rank)
					{
						best = subset;
						best_rank = rank;
					}
				}
				if (best < 0)
					break;
				subsets[best].used = FALSE;
				if (attempts++ >= ATTEMPT_LIMIT || budget_spent())
					goto finish;
				memcpy(work.substitute, subsets[best].substitute, sizeof(work.substitute));
				if (!compile_attempt(&candidate))
					continue;
				candidate.state_hash = hash;
				plan_finish(&candidate);
				if (!found || candidate.status < plan->status ||
					(candidate.status == plan->status && candidate.mean_error < plan->mean_error - 1.0e-4f))
				{
					*plan = candidate;
				}
				found = TRUE;
				found_here = TRUE;
				if (plan->status == _nv2a_ff_plan_exact)
					return;
				break;
			}

			/* A plan that is far off may be beaten by dropping one more
			texture: when it left a term out, or when the textures it
			dropped happen to leave little of the picture (a black
			substitute under a product). */
			if (found && ((extra == 0 && !(plan->approximations & _nv2a_ff_approximate_dropped_term)) ||
				plan->mean_error <= GOOD_ENOUGH_ERROR || levels_after_found++ >= 1))
			{
				break;
			}
			(void)found_here;
		}
	}
	finish:
	if (!found)
	{
		/* the last resorts: the stage that matters most alone, then no
		texture at all (which always fits: the primary colour holds it) */
		int keep = -1, resort;

		for (stage = 0; stage < NV2A_FF_STAGE_COUNT; stage++)
		{
			if ((optional & (1 << stage)) && (keep < 0 || stage_rank[stage] > stage_rank[keep]))
				keep = stage;
		}
		work.ignore_destination_alpha = work.draw->write_alpha && work.capabilities->destination_alpha;
		for (resort = keep >= 0 ? 0 : 1; resort < 2 && !found; resort++)
		{
			for (stage = 0; stage < NV2A_FF_STAGE_COUNT; stage++)
			{
				work.substitute[stage] = (unsigned char)(((forced | optional) & (1 << stage)) && (resort || stage != keep) ?
					neutral_substitute(bound_constants, stage, neutral) : _nv2a_ff_substitute_none);
			}
			work.step_base = work.statistics.steps;
			work.step_budget = RESORT_STEP_LIMIT;
			if (compile_attempt(&candidate))
			{
				candidate.state_hash = hash;
				plan_finish(&candidate);
				*plan = candidate;
				found = TRUE;
			}
		}
	}
}

/* a is the better plan: exact rather than approximate; of two exact ones,
the one with fewer passes, then with fewer derived textures; of two
approximate ones, the one that kept the textures that matter more, then
the one the comparison with the reference likes better */
static int plan_is_better(const struct nv2a_ff_plan *a, const struct nv2a_ff_plan *b)
{
	int weight_a, weight_b;

	if (a->status != b->status)
		return a->status < b->status;
	if (a->status == _nv2a_ff_plan_exact)
	{
		if (a->pass_count != b->pass_count)
			return a->pass_count < b->pass_count;
		return a->bake_count < b->bake_count;
	}
	if (a->status != _nv2a_ff_plan_approximate)
		return FALSE;
	weight_a = plan_dropped_weight(a);
	weight_b = plan_dropped_weight(b);
	if (weight_a != weight_b)
		return weight_a < weight_b;
	return a->mean_error < b->mean_error - 1.0e-4f;
}

/* The search, twice where there are combine environments: first with the
OpenGL 1.1 environments alone, among which there is little to try, so an
exact plan is found at once and a state too long for the search still
gets the plan a lesser card would; then with everything, unless that was
exact in one pass of the stages' own textures. More capabilities never
give a worse plan this way. */
static void compile_search(const struct nv2a_ff_constants *bound_constants, unsigned long bound_sources,
	struct nv2a_ff_plan *plan)
{
	static struct nv2a_ff_plan combined;
	const struct nv2a_ff_capabilities *capabilities = work.capabilities;
	struct nv2a_ff_capabilities plain;

	if (!capabilities->combine)
	{
		compile_search_once(bound_constants, bound_sources, plan);
		return;
	}
	plain = *capabilities;
	plain.combine = plain.combine_arb = plain.dot3 = 0;
	work.capabilities = &plain;
	compile_search_once(bound_constants, bound_sources, plan);
	work.capabilities = capabilities;
	if (plan->status == _nv2a_ff_plan_exact && plan->pass_count == 1 && plan->bake_count == 0)
		return;
	compile_search_once(bound_constants, bound_sources, &combined);
	if (plan_is_better(&combined, plan))
		*plan = combined;
}

/* The plan for all constants that the last compile made, and what it was
made for: a renderer asks for it first and, when it is not exact, again
with the constants of the moment, and again when they change. */
static struct
{
	int valid;
	struct nv2a_ff_state state;
	struct nv2a_ff_capabilities capabilities;
	struct nv2a_ff_draw draw;
	struct nv2a_ff_plan plan;
} general_memory;

static int general_memory_matches(const struct nv2a_ff_state *state,
	const struct nv2a_ff_capabilities *capabilities, const struct nv2a_ff_draw *draw)
{
	const struct nv2a_ff_state *kept = &general_memory.state;
	const struct nv2a_ff_draw *kept_draw = &general_memory.draw;

	/* (field by field: the structures have padding) */
	return general_memory.valid &&
		!memcmp(kept->combiner_state, state->combiner_state, sizeof(state->combiner_state)) &&
		kept->texture_modes == state->texture_modes &&
		!memcmp(kept->sampler_type, state->sampler_type, sizeof(state->sampler_type)) &&
		!memcmp(kept->alpha_kill, state->alpha_kill, sizeof(state->alpha_kill)) &&
		!memcmp(kept->color_sign, state->color_sign, sizeof(state->color_sign)) &&
		kept->alpha_test_function == state->alpha_test_function &&
		kept->fog_enable == state->fog_enable && kept->fog_table_mode == state->fog_table_mode &&
		!memcmp(&general_memory.capabilities, capabilities, sizeof(*capabilities)) &&
		kept_draw->blend_enable == draw->blend_enable && kept_draw->source_blend == draw->source_blend &&
		kept_draw->destination_blend == draw->destination_blend &&
		kept_draw->blend_operation == draw->blend_operation && kept_draw->write_color == draw->write_color &&
		kept_draw->write_alpha == draw->write_alpha && kept_draw->depth_write == draw->depth_write &&
		!memcmp(kept_draw->stage_rank, draw->stage_rank, sizeof(draw->stage_rank));
}

int nv2a_ff_compile(const struct nv2a_ff_state *state, const struct nv2a_ff_constants *constants,
	const struct nv2a_ff_capabilities *capabilities, const struct nv2a_ff_draw *draw,
	struct nv2a_ff_plan *plan)
{
	static struct nv2a_ff_plan bound_plan;
	unsigned long hash = nv2a_ff_state_hash(state);
	int index;

	work.state = state;
	work.capabilities = capabilities;
	work.draw = draw;
	memset(&work.statistics, 0, sizeof(work.statistics));
	work.bound_constants = NULL;
	memset(work.substitute, 0, sizeof(work.substitute));
	work.ignore_destination_alpha = FALSE;
	ranges_forget();

	for (index = 0; index < override_count; index++)
	{
		if (overrides[index].state_hash != hash)
			continue;
		memset(plan, 0, sizeof(*plan));
		if (overrides[index].function(state, constants, capabilities, draw, plan) && plan->pass_count > 0)
		{
			plan->state_hash = hash;
			plan->from_override = TRUE;
			work.bound_constants = plan->constants_bound ? constants : NULL;
			work.bound_sources = BOUND_SOURCES_ALL;
			plan->bound_sources = plan->constants_bound ? BOUND_SOURCES_ALL : 0;
			plan_finish(plan);
			plan_portable_environments(plan, capabilities);
			return plan->status;
		}
	}

	if (general_memory_matches(state, capabilities, draw))
	{
		*plan = general_memory.plan;
	}
	else
	{
		compile_search(NULL, 0, plan);
		general_memory.valid = TRUE;
		general_memory.state = *state;
		general_memory.capabilities = *capabilities;
		general_memory.draw = *draw;
		general_memory.plan = *plan;
	}
	if (plan->status != _nv2a_ff_plan_exact && constants)
	{
		/* First with the values of the constants that decide what is
		computed, which seldom change; then, when that is not exact either,
		with all of them. */
		unsigned long sources[2];
		int round;

		sources[0] = deciding_sources();
		sources[1] = BOUND_SOURCES_ALL;
		for (round = sources[0] != 0 && sources[0] != BOUND_SOURCES_ALL ? 0 : 1;
			round < 2 && plan->status != _nv2a_ff_plan_exact; round++)
		{
			float present_error = 0.0f;

			if (plan->status != _nv2a_ff_plan_impossible)
			{
				/* the present plan's error with these constants, to compare like with like */
				struct nv2a_ff_plan measured = *plan;

				work.bound_constants = constants;
				work.bound_sources = sources[round];
				plan_finish(&measured);
				present_error = measured.mean_error;
			}
			compile_search(constants, sources[round], &bound_plan);
			/* A plan that must be remade when a constant changes has to be
			clearly better. One made for the deciding constants alone is
			also better when it keeps a texture that matters more: the
			errors, measured on random texels, do not know which do. */
			if (bound_plan.status < plan->status ||
				(bound_plan.status == plan->status && bound_plan.status != _nv2a_ff_plan_impossible &&
					(bound_plan.mean_error < 0.75f * present_error - 0.25f ||
						(round == 0 && plan_dropped_weight(&bound_plan) < plan_dropped_weight(plan)))))
			{
				*plan = bound_plan;
			}
		}
	}
	/* (last, on the plan handed out: the search and the plan kept for the
	next call work with the environments as they were found) */
	plan_portable_environments(plan, capabilities);
	return plan->status;
}

void nv2a_ff_compile_statistics(struct nv2a_ff_statistics *statistics)
{
	*statistics = work.statistics;
}

/* ---------- describing a plan */

static void text_append(char *text, unsigned long size, const char *format, ...)
{
	char buffer[256];
	unsigned long length = (unsigned long)strlen(text);
	va_list arguments;

	va_start(arguments, format);
	vsprintf(buffer, format, arguments);
	va_end(arguments);
	if (length + 1 < size)
	{
		strncpy(text + length, buffer, size - length - 1);
		text[size - 1] = 0;
	}
}

static const char *blend_factor_name(unsigned long factor)
{
	switch (factor)
	{
	case _nv2a_ff_blend_zero: return "0";
	case _nv2a_ff_blend_one: return "1";
	case _nv2a_ff_blend_source_color: return "src";
	case _nv2a_ff_blend_inverse_source_color: return "1-src";
	case _nv2a_ff_blend_source_alpha: return "srca";
	case _nv2a_ff_blend_inverse_source_alpha: return "1-srca";
	case _nv2a_ff_blend_destination_alpha: return "dsta";
	case _nv2a_ff_blend_inverse_destination_alpha: return "1-dsta";
	case _nv2a_ff_blend_destination_color: return "dst";
	case _nv2a_ff_blend_inverse_destination_color: return "1-dst";
	case _nv2a_ff_blend_source_alpha_saturate: return "sat";
	case _nv2a_ff_blend_constant_color: return "const";
	case _nv2a_ff_blend_inverse_constant_color: return "1-const";
	case _nv2a_ff_blend_constant_alpha: return "consta";
	case _nv2a_ff_blend_inverse_constant_alpha: return "1-consta";
	default: return "?";
	}
}

static void describe_combine(char *text, unsigned long size, const struct nv2a_ff_combine *combine)
{
	static const char *functions[] = { "rep", "mod", "add", "adds", "lerp", "sub", "dot3" };
	static const char *sources[] = { "tex", "con", "pri", "prv" };
	static const unsigned char argument_counts[] = { 1, 2, 2, 2, 3, 2, 2 };
	int argument;

	text_append(text, size, "%s(", functions[combine->function]);
	for (argument = 0; argument < argument_counts[combine->function]; argument++)
	{
		int operand = combine->operand[argument];

		text_append(text, size, "%s%s%s%s", argument ? "," : "",
			(operand == _nv2a_ff_operand_inverse_color || operand == _nv2a_ff_operand_inverse_alpha) ? "1-" : "",
			sources[combine->source[argument]],
			(operand == _nv2a_ff_operand_alpha || operand == _nv2a_ff_operand_inverse_alpha) ? ".a" : "");
	}
	text_append(text, size, ")");
	if (combine->scale != 1)
		text_append(text, size, "x%d", combine->scale);
}

/* a polynomial of the work area as text */
static void describe_polynomial(char *text, unsigned long size, const struct polynomial *polynomial);

static void describe_atom(char *text, unsigned long size, int atom)
{
	static const char *channels[3] = { "", ".a", ".b" };
	static const char *operations[] = { "max0", "clamp", "clamp11", "dot", "mux", "muxlsb", "signed", "" };

	if (atom < LEAF_ATOM_COUNT)
	{
		int source = atom / 3;

		if (source < NV2A_FF_STAGE_COUNT)
			text_append(text, size, "t%d%s", source, channels[atom % 3]);
		else if (source == _nv2a_ff_atom_v0 || source == _nv2a_ff_atom_v1)
			text_append(text, size, "v%d%s", source - _nv2a_ff_atom_v0, channels[atom % 3]);
		else if (source == _nv2a_ff_atom_fog)
			text_append(text, size, "fog%s", channels[atom % 3]);
		else if (source < _nv2a_ff_atom_c1)
			text_append(text, size, "c0[%d]%s", source - _nv2a_ff_atom_c0, channels[atom % 3]);
		else if (source < _nv2a_ff_atom_final_c0)
			text_append(text, size, "c1[%d]%s", source - _nv2a_ff_atom_c1, channels[atom % 3]);
		else if (source == _nv2a_ff_atom_final_c0 || source == _nv2a_ff_atom_final_c1)
			text_append(text, size, "fc%d%s", source - _nv2a_ff_atom_final_c0, channels[atom % 3]);
		else
			text_append(text, size, "blend%s", channels[atom % 3]);
	}
	else
	{
		const struct opaque_atom *opaque = &work.opaque[atom - LEAF_ATOM_COUNT];
		int operand;

		text_append(text, size, "%s(", operations[opaque->operation]);
		for (operand = 0; operand < 3; operand++)
		{
			if (opaque->operands[operand] < 0)
				continue;
			if (operand)
				text_append(text, size, ", ");
			describe_polynomial(text, size, &work.store[opaque->operands[operand]]);
		}
		text_append(text, size, ")");
	}
}

static void describe_polynomial(char *text, unsigned long size, const struct polynomial *polynomial)
{
	int index, atom;

	if (polynomial->count == 0)
		text_append(text, size, "0");
	for (index = 0; index < polynomial->count; index++)
	{
		const struct nv2a_ff_term *term = &polynomial->terms[index];
		int grey = fabs(term->coefficient[0] - term->coefficient[1]) < COEFFICIENT_EPSILON &&
			fabs(term->coefficient[0] - term->coefficient[2]) < COEFFICIENT_EPSILON;

		if (index)
			text_append(text, size, " + ");
		if (!grey)
			text_append(text, size, "(%g,%g,%g)", term->coefficient[0], term->coefficient[1], term->coefficient[2]);
		else if (term->count == 0 || fabs(term->coefficient[0] - 1.0f) > COEFFICIENT_EPSILON)
			text_append(text, size, "%g", term->coefficient[0]);
		for (atom = 0; atom < term->count; atom++)
		{
			if (atom || !grey || fabs(term->coefficient[0] - 1.0f) > COEFFICIENT_EPSILON)
				text_append(text, size, "*");
			describe_atom(text, size, term->atoms[atom]);
		}
	}
}

void nv2a_ff_state_describe(const struct nv2a_ff_state *state, const struct nv2a_ff_constants *constants,
	const struct nv2a_ff_capabilities *capabilities, char *text, unsigned long size)
{
	static const struct nv2a_ff_draw draw = { 0, 0, 0, 0, 1, 1, 0, { 0, 0, 0, 0 } };

	if (size == 0)
		return;
	text[0] = 0;
	work.state = state;
	work.capabilities = capabilities;
	work.draw = &draw;
	work.bound_constants = constants;
	memset(work.substitute, 0, sizeof(work.substitute));
	ranges_forget();
	symbolic_evaluate();
	text_append(text, size, "rgb = ");
	describe_polynomial(text, size, &work.goal_rgb);
	text_append(text, size, "\nalpha = ");
	describe_polynomial(text, size, &work.goal_alpha);
	text_append(text, size, "\n");
	if (work.overflow)
		text_append(text, size, "(too long for the work area)\n");
}

void nv2a_ff_plan_describe(const struct nv2a_ff_plan *plan, char *text, unsigned long size)
{
	static const char *environments[] = { "replace", "modulate", "decal", "blend", "add", "combine" };
	int pass_index, unit_index, bake_index;

	if (size == 0)
		return;
	text[0] = 0;
	for (pass_index = 0; pass_index < plan->pass_count; pass_index++)
	{
		const struct nv2a_ff_pass *pass = &plan->passes[pass_index];

		text_append(text, size, "pass %d:", pass_index);
		if (pass->primary_rgb != NV2A_FF_EXPRESSION_NONE || pass->primary_alpha != NV2A_FF_EXPRESSION_NONE)
			text_append(text, size, " primary %s", pass->primary_is_constant ? "constant" : "per vertex");
		for (unit_index = 0; unit_index < pass->unit_count; unit_index++)
		{
			const struct nv2a_ff_unit *unit = &pass->units[unit_index];

			text_append(text, size, " | ");
			if (unit->stage == NV2A_FF_STAGE_NONE)
				text_append(text, size, "none");
			else if (unit->bake != NV2A_FF_BAKE_NONE)
				text_append(text, size, "bake%d(t%d)", unit->bake, unit->stage);
			else
				text_append(text, size, "t%d", unit->stage);
			text_append(text, size, " %s", environments[unit->environment]);
			if (unit->environment == _nv2a_ff_environment_combine)
			{
				text_append(text, size, " rgb ");
				describe_combine(text, size, &unit->rgb);
				text_append(text, size, " a ");
				describe_combine(text, size, &unit->alpha);
			}
		}
		if (pass->blend_enable)
		{
			text_append(text, size, " | blend %s,%s", blend_factor_name(pass->source_blend),
				blend_factor_name(pass->destination_blend));
			if (pass->blend_operation != _nv2a_ff_blend_operation_add)
				text_append(text, size, " op %x", pass->blend_operation);
		}
		if (pass->secondary_rgb != NV2A_FF_EXPRESSION_NONE)
			text_append(text, size, " | + secondary");
		if (pass->fog_enable)
			text_append(text, size, " | fog");
		if (pass->alpha_test_function)
			text_append(text, size, " | alpha test");
		if (pass->depth_equal)
			text_append(text, size, " | depth equal");
		text_append(text, size, "\n");
	}
	for (bake_index = 0; bake_index < plan->bake_count; bake_index++)
	{
		const struct nv2a_ff_bake *bake = &plan->bakes[bake_index];

		text_append(text, size, "bake %d: of t%d%s%s%s%s\n", bake_index, bake->stage,
			bake->rgb != NV2A_FF_EXPRESSION_NONE ? ", colour" : "",
			bake->alpha != NV2A_FF_EXPRESSION_NONE ? ", alpha" : "",
			bake->uses_constants ? ", reads constants" : "",
			bake->nonlinear ? ", not first degree" : "");
	}
}
