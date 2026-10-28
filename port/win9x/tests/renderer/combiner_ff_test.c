/*
COMBINER_FF_TEST.C

The test of the light renderer's pixel side (port/linux/src/
nv2a_combiner_ff.c): a console program, no window and no OpenGL.

	clang -O2 -Iport/linux/src port/win9x/tests/renderer/combiner_ff_test.c
		port/linux/src/nv2a_combiner_ff.c -o combiner_ff_test.exe
	combiner_ff_test [--samples N] [--fuzz N] [--details file] [--site text]

For every pixel state the game can set (combiner_ff_sites.c) and each
capability level it compiles a plan the way the renderer does (first
without the constants; when that plan is not exact, again with them) and
draws thousands of random fragments both ways: the state's colour as
nv2a_psh.c's shader computes it, blended into a random framebuffer as the
draw says, and the plan's passes through the model of the fixed-function
pipeline. It prints one line per site with what the plans are (exact,
approximate, impossible), how many passes they take and what they drop;
with --details, a line per variant and level goes to a file. These are
failures:

- a plan that claims to be exact and differs from the reference by more
  than its tolerance;
- a state of the game that the compiler says nothing can be drawn for;
- a plan that asks the context for what its capabilities lack, or that
  counts on the format a texture is stored in where a combine environment
  would not (plan_asks_too_much);
- a state of the light pipeline (the passes the game draws only for this
  renderer) whose plan is not the one its author built it for: see
  _site_expect_* in combiner_ff_sites.c.

It also prints how long the compiler took, state by state, and how many
candidates it looked at: plans are compiled while the game draws, the
first time a state is seen.

Then the same with random states (--fuzz), every eighth of them random
bits, which looks for crashes and for plans wrongly called exact; and a
hand-written plan through the override table.

Lines that start with '#' are notes; the exit code is the number of
failures. The plans do not depend on the processor the test is built for
(--details gives the same file from a 64-bit build and from one for the
Pentium III), nor on what was compiled before.
*/

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#undef near
#endif

#include "nv2a_combiner_ff.h"
#include "combiner_ff_sites.c"

/* seconds, as finely as the system counts them */
static double test_time(void)
{
#ifdef _WIN32
	LARGE_INTEGER count, frequency;

	QueryPerformanceCounter(&count);
	QueryPerformanceFrequency(&frequency);
	return (double)count.QuadPart / (double)frequency.QuadPart;
#else
	return (double)clock() / CLOCKS_PER_SEC;
#endif
}

/* ---------- levels */

enum
{
	_level_0 = 0,  /* one unit, OpenGL 1.1 environments */
	_level_1,      /* two units, OpenGL 1.1 environments */
	_level_2,      /* two units, EXT_texture_env_combine and env_add */
	_level_new,    /* what the renderer asks for on a graphics card of today */
	_level_all,    /* level 2 with everything the compiler knows */
	NUMBER_OF_LEVELS
};

static const char *level_names[NUMBER_OF_LEVELS] = { "L0", "L1", "L2", "new", "all" };

/* The first four are what the renderer (xgpu_fixed.c) asks the compiler
for: derived textures only when no constant enters them, because the
game's constants change all the time and such a texture would be made
again each time; no fog coordinate, no secondary colour, no cube maps.
"new" is its request on a card with four units and every blend and
combine extension (OpenGL 1.3 and 1.4 in an OpenGL 4 driver); "all" turns
on the rest, which only this test asks for. */
static void level_capabilities(int level, struct nv2a_ff_capabilities *capabilities)
{
	nv2a_ff_capabilities_level(level >= _level_new ? 2 : level, capabilities);
	capabilities->bakes = _nv2a_ff_bakes_static;
	if (level >= _level_new)
	{
		capabilities->combine_arb = 1;
		capabilities->dot3 = 1;
		capabilities->blend_subtract = 1;
		capabilities->blend_minmax = 1;
		capabilities->blend_color = 1;
		capabilities->destination_alpha = 1;
	}
	if (level == _level_new)
		capabilities->units = 4;
	if (level == _level_all)
	{
		capabilities->fog_coordinate = 1;
		capabilities->secondary_color = 1;
		capabilities->bakes = _nv2a_ff_bakes_all;
	}
}

/* ---------- a site as the compiler's input */

struct test_state
{
	struct nv2a_ff_state state;
	struct nv2a_ff_constants constants;
	struct nv2a_ff_draw draw;
};

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

static void test_state_from_site(const struct site *site, struct test_state *test)
{
	const struct site_shader *shader = &site->shader;
	int index;

	memset(test, 0, sizeof(*test));
	/* as D3DDevice_SetPixelShaderProgram stores the definition (the key
	of the device has the constants zeroed) */
	for (index = 0; index < 8; index++)
	{
		test->state.combiner_state[0 + index] = shader->alpha_inputs[index];
		test->state.combiner_state[26 + index] = shader->alpha_outputs[index];
		test->state.combiner_state[34 + index] = shader->rgb_inputs[index];
		test->state.combiner_state[45 + index] = shader->rgb_outputs[index];
		nv2a_ff_color_from_argb(shader->constant_0[index], test->constants.c0[index]);
		nv2a_ff_color_from_argb(shader->constant_1[index], test->constants.c1[index]);
	}
	test->state.combiner_state[8] = shader->final_combiner_inputs_abcd;
	test->state.combiner_state[9] = shader->final_combiner_inputs_efg;
	test->state.combiner_state[42] = shader->compare_mode;
	test->state.combiner_state[53] = shader->combiner_count;
	test->state.combiner_state[55] = shader->dot_mapping;
	test->state.combiner_state[56] = shader->input_texture;
	test->state.texture_modes = shader->texture_modes;
	for (index = 0; index < 4; index++)
	{
		test->state.sampler_type[index] = mode_sampler((shader->texture_modes >> (5 * index)) & 0x1f);
		test->state.alpha_kill[index] = (unsigned char)((site->alpha_kill >> index) & 1);
		test->constants.bump_luminance[index][0] = 1.0f;
	}
	/* D3DRS_ALPHAFUNC is D3DCMP_GREATER and D3DRS_FOGENABLE is on from
	the start (rasterizer_xbox.c:3433-3439) */
	test->state.alpha_test_function = site->alpha_test ? _nv2a_ff_compare_greater : 0;
	test->state.fog_enable = 1;
	nv2a_ff_color_from_argb(shader->final_combiner_constant_0, test->constants.final_c0);
	nv2a_ff_color_from_argb(shader->final_combiner_constant_1, test->constants.final_c1);
	nv2a_ff_color_from_argb(site->fog_color, test->constants.fog_color);
	nv2a_ff_color_from_argb(site->blend_color, test->constants.blend_color);
	test->constants.fog_parameters[1] = 1.0f;
	test->constants.fog_parameters[2] = 1.0f;
	test->constants.alpha_reference = (float)site->alpha_reference;
	test->draw.blend_enable = (unsigned char)site->blend_enable;
	test->draw.source_blend = site->source_blend;
	test->draw.destination_blend = site->destination_blend;
	test->draw.blend_operation = site->blend_operation;
	test->draw.write_color = (unsigned char)site->write_color;
	test->draw.write_alpha = (unsigned char)site->write_alpha;
	test->draw.depth_write = (unsigned char)site->depth_write;
	memcpy(test->draw.stage_rank, site->stage_rank, sizeof(test->draw.stage_rank));
}

/* ---------- random numbers (the same sequence everywhere) */

static unsigned long test_seed = 0x13579bdfUL;

static unsigned long test_random(unsigned long range)
{
	test_seed = (test_seed * 1664525UL + 1013904223UL) & 0xffffffffUL;
	return ((test_seed >> 10) & 0xfffffUL) % range;
}

static void test_random_color(float color[4])
{
	int channel;

	for (channel = 0; channel < 4; channel++)
	{
		unsigned long value = test_random(256 + 32);

		/* the ends come up often */
		color[channel] = value >= 256 + 16 ? 1.0f : value >= 256 ? 0.0f : (float)value / 255.0f;
	}
}

static void test_random_constants(const struct nv2a_ff_constants *like, struct nv2a_ff_constants *constants)
{
	int index;

	*constants = *like;
	for (index = 0; index < 8; index++)
	{
		test_random_color(constants->c0[index]);
		test_random_color(constants->c1[index]);
	}
	test_random_color(constants->final_c0);
	test_random_color(constants->final_c1);
	test_random_color(constants->fog_color);
	test_random_color(constants->blend_color);
	if (test_random(2))
		constants->alpha_reference = (float)test_random(256);
}

/* ---------- checking one plan */

struct check_result
{
	float mean_error;
	float maximum_error;
	/* samples that differ by more than the plan's tolerance */
	long over_tolerance;
	long samples;
};

static void check_plan(const struct test_state *test, const struct nv2a_ff_capabilities *capabilities,
	const struct nv2a_ff_plan *plan, long samples, struct check_result *result)
{
	struct nv2a_ff_constants constants = test->constants;
	unsigned long seed = test_seed ^ plan->state_hash;
	double total = 0.0;
	long index;

	memset(result, 0, sizeof(*result));
	result->samples = samples;
	for (index = 0; index < samples; index++)
	{
		struct nv2a_ff_sample sample;
		float difference;

		/* a plan is tried with other constants than the site's too, as far
		as it was not made for the site's */
		if ((index & 7) == 4)
		{
			test_random_constants(&test->constants, &constants);
			if (plan->constants_bound)
			{
				nv2a_ff_plan_bound_constants(plan, &test->constants, &constants);
				constants.alpha_reference = test->constants.alpha_reference;
			}
		}
		if ((index & 63) == 0)
			constants = test->constants;
		nv2a_ff_sample_random(&seed, &sample);
		difference = nv2a_ff_sample_compare(&test->state, &constants, capabilities, &test->draw, plan, &sample, NULL, NULL);
		total += difference;
		if (difference > result->maximum_error)
			result->maximum_error = difference;
		if (difference > (float)plan->tolerance + 0.01f)
			result->over_tolerance++;
	}
	result->mean_error = (float)(total / (double)(samples > 0 ? samples : 1));
}

/* What a plan asks of the context that the capabilities do not offer, or
NULL: texture units, environments and their arguments (the EXT combine
extension takes only SRC_ALPHA for the third argument), blends (OpenGL 1.1
has no source factor that reads the source's colour and no destination
factor that reads the destination's), derived textures. And what it counts
on a texture's stored format for: with combine environments a plan must
not use the OpenGL 1.1 environments where they differ by the format
(REPLACE after a primary alpha or another unit, DECAL, ADD; BLEND too with
ARB_texture_env_combine), see plan_portable_environments. */
static const char *plan_asks_too_much(const struct nv2a_ff_plan *plan, const struct nv2a_ff_capabilities *capabilities)
{
	int pass_index, unit_index, side, argument;

	if (plan->pass_count > (capabilities->maximum_passes ? capabilities->maximum_passes : NV2A_FF_MAXIMUM_PASSES))
		return "more passes than allowed";
	for (pass_index = 0; pass_index < plan->pass_count; pass_index++)
	{
		const struct nv2a_ff_pass *pass = &plan->passes[pass_index];

		if (pass->unit_count > capabilities->units)
			return "more texture units than there are";
		for (unit_index = 0; unit_index < pass->unit_count; unit_index++)
		{
			const struct nv2a_ff_unit *unit = &pass->units[unit_index];
			int own_texture = unit->stage != NV2A_FF_STAGE_NONE && unit->bake == NV2A_FF_BAKE_NONE;

			if (unit->bake != NV2A_FF_BAKE_NONE)
			{
				if (unit->bake >= plan->bake_count || capabilities->bakes == _nv2a_ff_bakes_none)
					return "a derived texture that is not allowed";
				if (capabilities->bakes == _nv2a_ff_bakes_static && plan->bakes[unit->bake].uses_constants)
					return "a derived texture that reads constants";
			}
			switch (unit->environment)
			{
			case _nv2a_ff_environment_modulate:
				break;
			case _nv2a_ff_environment_replace:
				/* (with or without combine environments: without, the alpha that
				comes in must be one) */
				if (own_texture && (unit_index > 0 || pass->primary_alpha != NV2A_FF_EXPRESSION_NONE))
					return "REPLACE where the texture's format decides the alpha";
				break;
			case _nv2a_ff_environment_decal:
				if (capabilities->combine && own_texture)
					return "DECAL where a combine environment does the same for every format";
				break;
			case _nv2a_ff_environment_blend:
				if (capabilities->combine_arb && own_texture)
					return "BLEND where a combine environment does the same for every format";
				break;
			case _nv2a_ff_environment_add:
				if (!capabilities->environment_add)
					return "ADD without EXT_texture_env_add";
				if (capabilities->combine && own_texture)
					return "ADD where a combine environment does the same for every format";
				break;
			case _nv2a_ff_environment_combine:
				if (!capabilities->combine)
					return "a combine environment without the extension";
				for (side = 0; side < 2; side++)
				{
					const struct nv2a_ff_combine *combine = side ? &unit->alpha : &unit->rgb;
					int arguments = combine->function == _nv2a_ff_combine_replace ? 1 :
						combine->function == _nv2a_ff_combine_interpolate ? 3 : 2;

					if (combine->scale != 1 && combine->scale != 2 && combine->scale != 4)
						return "a combine scale that is not 1, 2 or 4";
					if (combine->function > _nv2a_ff_combine_dot3)
						return "an unknown combine function";
					if (combine->function == _nv2a_ff_combine_subtract && !capabilities->combine_arb)
						return "SUBTRACT without ARB_texture_env_combine";
					if (combine->function == _nv2a_ff_combine_dot3 && (!capabilities->dot3 || side))
						return "DOT3 without the extension, or for alpha";
					for (argument = 0; argument < arguments; argument++)
					{
						if (combine->source[argument] > _nv2a_ff_source_previous ||
							combine->operand[argument] > _nv2a_ff_operand_inverse_alpha)
						{
							return "an unknown combine source or operand";
						}
						if (side && combine->operand[argument] < _nv2a_ff_operand_alpha)
							return "a colour operand on the alpha side";
						if (argument == 2 && !capabilities->combine_arb && combine->operand[argument] != _nv2a_ff_operand_alpha)
							return "a third argument other than SRC_ALPHA without ARB_texture_env_combine";
					}
				}
				break;
			default:
				return "an unknown environment";
			}
		}
		if (pass->fog_enable && !capabilities->fog_coordinate)
			return "fog without EXT_fog_coord";
		if (pass->secondary_rgb != NV2A_FF_EXPRESSION_NONE && !capabilities->secondary_color)
			return "a secondary colour without the extension";
		if (pass->blend_enable)
		{
			unsigned long source = pass->source_blend, destination = pass->destination_blend;
			unsigned long operation = pass->blend_operation;

			if ((operation == _nv2a_ff_blend_operation_subtract || operation == _nv2a_ff_blend_operation_reverse_subtract) &&
				!capabilities->blend_subtract)
			{
				return "a subtracting blend without EXT_blend_subtract";
			}
			if ((operation == _nv2a_ff_blend_operation_minimum || operation == _nv2a_ff_blend_operation_maximum))
			{
				if (!capabilities->blend_minmax)
					return "a minimum or maximum blend without EXT_blend_minmax";
				continue;
			}
			if (!capabilities->blend_color &&
				((source >= _nv2a_ff_blend_constant_color && source <= _nv2a_ff_blend_inverse_constant_alpha) ||
					(destination >= _nv2a_ff_blend_constant_color && destination <= _nv2a_ff_blend_inverse_constant_alpha)))
			{
				return "a constant blend factor without EXT_blend_color";
			}
			if (source == _nv2a_ff_blend_source_color || source == _nv2a_ff_blend_inverse_source_color)
				return "a source factor OpenGL 1.1 does not have";
			if (destination == _nv2a_ff_blend_destination_color || destination == _nv2a_ff_blend_inverse_destination_color ||
				destination == _nv2a_ff_blend_source_alpha_saturate)
			{
				return "a destination factor OpenGL 1.1 does not have";
			}
		}
	}
	return NULL;
}

static void describe_approximations(const struct nv2a_ff_plan *plan, char *text)
{
	static const char *substitutes[] = { "", "black", "white", "grey", "flat" };
	int stage;

	text[0] = 0;
	for (stage = 0; stage < 4; stage++)
	{
		if (plan->dropped_stage[stage])
			sprintf(text + strlen(text), "t%d=%s ", stage, substitutes[plan->dropped_stage[stage]]);
	}
	if (plan->approximations & _nv2a_ff_approximate_vertex_term)
		strcat(text, "vertex-term ");
	if (plan->approximations & _nv2a_ff_approximate_bake_filter)
		strcat(text, "bake-filter ");
	if (plan->approximations & _nv2a_ff_approximate_alpha_kill)
		strcat(text, "alpha-kill ");
	if (plan->approximations & _nv2a_ff_approximate_clip_plane)
		strcat(text, "clip-plane ");
	if (plan->approximations & _nv2a_ff_approximate_fog_table)
		strcat(text, "fog-table ");
	if (plan->approximations & _nv2a_ff_approximate_blend)
		strcat(text, "blend ");
	if (plan->approximations & _nv2a_ff_approximate_destination_alpha)
		strcat(text, "dest-alpha ");
	if (plan->approximations & _nv2a_ff_approximate_dropped_term)
		strcat(text, "dropped-term ");
	if (plan->approximations & _nv2a_ff_approximate_clamp)
		strcat(text, "clamp ");
	if (plan->approximations & _nv2a_ff_approximate_unexplained)
		strcat(text, "UNEXPLAINED ");
}

/* ---------- the sites */

enum
{
	MAXIMUM_GROUPS = 96
};

struct group_level
{
	long exact, approximate, impossible;
	int maximum_passes;
	int maximum_units;
	long with_bakes;
	long bound;
	float worst_mean, worst_maximum;
	/* of the exact plans: the largest error seen and the largest tolerance */
	float worst_exact;
	int maximum_tolerance;
	/* stages dropped by any variant, and the other approximations */
	int dropped_stages;
	unsigned long approximations;
};

struct group
{
	char name[64];
	long variants;
	long light_variants;
	struct group_level levels[NUMBER_OF_LEVELS];
};

static struct group groups[MAXIMUM_GROUPS];
static int group_count;
static long failures;
static long sample_count = 4000;
static FILE *details;
/* --byte-colors: primary and environment colours rounded to bytes in the model */
static int byte_colors;
static const char *site_filter;
static long site_total;
static long plans_total, plans_checked_samples;
/* The time the compiler takes, per level and kind of call, as the
renderer calls it: for a state it has not seen, without constants; then,
when that plan is not exact, with the constants of the moment. */
enum
{
	_compile_general = 0,
	_compile_bound,
	NUMBER_OF_COMPILE_KINDS,

	MAXIMUM_TIMES = 4096
};

static struct
{
	long count;
	/* seconds */
	double total, longest;
	float times[MAXIMUM_TIMES];
	char longest_name[128];
	struct nv2a_ff_statistics longest_statistics;
	unsigned long most_candidates;
} compile_times[NUMBER_OF_LEVELS][NUMBER_OF_COMPILE_KINDS];
/* the most candidates the compiler looked at for a plan that came out exact */
static unsigned long exact_most_candidates;
static struct nv2a_ff_plan plan;

static void timed_compile(const struct test_state *test, const struct nv2a_ff_capabilities *capabilities,
	int level, int kind, const char *name)
{
	struct nv2a_ff_statistics statistics;
	double before = test_time(), seconds;

	nv2a_ff_compile(&test->state, kind == _compile_bound ? &test->constants : NULL, capabilities, &test->draw, &plan);
	seconds = test_time() - before;
	nv2a_ff_compile_statistics(&statistics);
	if (compile_times[level][kind].count < MAXIMUM_TIMES)
		compile_times[level][kind].times[compile_times[level][kind].count] = (float)seconds;
	compile_times[level][kind].count++;
	compile_times[level][kind].total += seconds;
	if (seconds > compile_times[level][kind].longest)
	{
		compile_times[level][kind].longest = seconds;
		strcpy(compile_times[level][kind].longest_name, name);
		compile_times[level][kind].longest_statistics = statistics;
	}
	if (statistics.steps > compile_times[level][kind].most_candidates)
		compile_times[level][kind].most_candidates = statistics.steps;
	if (plan.status == _nv2a_ff_plan_exact && statistics.steps > exact_most_candidates)
		exact_most_candidates = statistics.steps;
}

/* A state of the light pipeline must come out as its author built it
(combiner_ff_sites.c, _site_expect_*); what is wrong with the plan, or
NULL. */
static const char *expectation_broken(const struct site *site, int level, const struct nv2a_ff_plan *plan)
{
	int pass, unit, bake, units = 0, exact_wanted;

	if (site->expect == _site_expect_nothing)
		return NULL;
	if (plan->status == _nv2a_ff_plan_impossible || plan->pass_count == 0)
		return "nothing can be drawn";
	for (pass = 0; pass < plan->pass_count; pass++)
	{
		if (plan->passes[pass].unit_count > units)
			units = plan->passes[pass].unit_count;
		for (unit = 0; unit < plan->passes[pass].unit_count; unit++)
		{
			if (level == _level_1 && plan->passes[pass].units[unit].environment == _nv2a_ff_environment_combine)
				return "a combine environment without the extension";
		}
	}
	for (bake = 0; bake < plan->bake_count; bake++)
	{
		if (plan->bakes[bake].uses_constants)
			return "a derived texture reads a constant";
		if (plan->bakes[bake].nonlinear)
			return "a derived texture that filtering changes";
	}
	/* (the plan may be made for the constants that pick a channel; not
	for the colours, which change from frame to frame) */
	if (plan->constants_bound && (site->expect != _site_expect_two_units_derived ||
		(plan->bound_sources & ((1UL << _nv2a_ff_atom_final_c0) | (1UL << _nv2a_ff_atom_final_c1)))))
	{
		return "made for the values of constants that change";
	}
	if (level == _level_0)
	{
		if (site->expect == _site_expect_one_unit && (plan->status != _nv2a_ff_plan_exact || plan->pass_count != 1))
			return "not exact in one pass";
		if (site->expect == _site_expect_two_units &&
			(plan->status != _nv2a_ff_plan_exact || plan->pass_count > 2 || plan->bake_count))
		{
			return "not exact in two passes of the stages' own textures";
		}
		return NULL;
	}
	exact_wanted = site->expect != _site_expect_two_units_add || level >= _level_2;
	if (exact_wanted && (plan->status != _nv2a_ff_plan_exact || plan->pass_count != 1))
		return "not exact in one pass";
	if (!exact_wanted && (plan->approximations & ~(unsigned long)_nv2a_ff_approximate_clamp))
		return "gives up more than the clamp of the sum";
	if (units > 2)
		return "more than two units";
	if (site->expect == _site_expect_one_unit && (units > 1 || plan->bake_count))
		return "more than one unit of the stage's own texture";
	if (site->expect == _site_expect_two_units && plan->bake_count)
		return "a derived texture where the stages' own do";
	return NULL;
}

static struct group *group_find(const char *site_name)
{
	char name[64];
	int length = 0, index;

	/* the file and line: up to the first space */
	while (site_name[length] && site_name[length] != ' ' && length < (int)sizeof(name) - 1)
	{
		name[length] = site_name[length];
		length++;
	}
	name[length] = 0;
	for (index = 0; index < group_count; index++)
	{
		if (!strcmp(groups[index].name, name))
			return &groups[index];
	}
	if (group_count == MAXIMUM_GROUPS)
		return &groups[MAXIMUM_GROUPS - 1];
	strcpy(groups[group_count].name, name);
	return &groups[group_count++];
}

static void site_test(const struct site *site)
{
	struct test_state test;
	struct group *group;
	int level;

	if (site_filter && !strstr(site->name, site_filter))
		return;
	site_total++;
	test_state_from_site(site, &test);
	group = group_find(site->name);
	group->variants++;
	group->light_variants += site->light;
	if (details)
	{
		struct nv2a_ff_capabilities capabilities;
		char text[4096];

		level_capabilities(_level_all, &capabilities);
		nv2a_ff_state_describe(&test.state, NULL, &capabilities, text, sizeof(text));
		fprintf(details, "\n%s%s\n%s", site->name, site->light ? "" : " (not in the light pipeline)", text);
	}
	for (level = 0; level < NUMBER_OF_LEVELS; level++)
	{
		struct nv2a_ff_capabilities capabilities;
		struct group_level *totals = &group->levels[level];
		struct check_result result;
		char notes[160];
		int pass, units = 0, stage;

		level_capabilities(level, &capabilities);
		timed_compile(&test, &capabilities, level, _compile_general, site->name);
		if (plan.status != _nv2a_ff_plan_exact)
			timed_compile(&test, &capabilities, level, _compile_bound, site->name);
		if (expectation_broken(site, level, &plan))
		{
			failures++;
			printf("# FAIL %s %s: %s\n", site->name, level_names[level], expectation_broken(site, level, &plan));
		}
		if (plan.status != _nv2a_ff_plan_impossible && plan_asks_too_much(&plan, &capabilities))
		{
			failures++;
			printf("# FAIL %s %s: the plan asks for %s\n", site->name, level_names[level],
				plan_asks_too_much(&plan, &capabilities));
		}
		plans_total++;
		memset(&result, 0, sizeof(result));
		if (plan.status != _nv2a_ff_plan_impossible)
		{
			check_plan(&test, &capabilities, &plan, sample_count, &result);
			plans_checked_samples += sample_count;
		}
		for (pass = 0; pass < plan.pass_count; pass++)
		{
			if (plan.passes[pass].unit_count > units)
				units = plan.passes[pass].unit_count;
		}
		describe_approximations(&plan, notes);

		switch (plan.status)
		{
		case _nv2a_ff_plan_exact:
			totals->exact++;
			if (result.maximum_error > totals->worst_exact)
				totals->worst_exact = result.maximum_error;
			if (plan.tolerance > totals->maximum_tolerance)
				totals->maximum_tolerance = plan.tolerance;
			if (result.over_tolerance && !byte_colors)
			{
				failures++;
				printf("# FAIL %s %s: called exact, %ld of %ld samples differ by more than %d (worst %.2f)\n",
					site->name, level_names[level], result.over_tolerance, result.samples, plan.tolerance,
					result.maximum_error);
			}
			break;
		case _nv2a_ff_plan_approximate:
			totals->approximate++;
			if (result.mean_error > totals->worst_mean)
				totals->worst_mean = result.mean_error;
			if (result.maximum_error > totals->worst_maximum)
				totals->worst_maximum = result.maximum_error;
			break;
		default:
			totals->impossible++;
			failures++;
			printf("# FAIL %s %s: nothing can be drawn\n", site->name, level_names[level]);
			break;
		}
		if (plan.pass_count > totals->maximum_passes)
			totals->maximum_passes = plan.pass_count;
		if (units > totals->maximum_units)
			totals->maximum_units = units;
		if (plan.bake_count)
			totals->with_bakes++;
		if (plan.constants_bound)
			totals->bound++;
		for (stage = 0; stage < 4; stage++)
		{
			if (plan.dropped_stage[stage])
				totals->dropped_stages |= 1 << stage;
		}
		totals->approximations |= plan.approximations;

		if (details)
		{
			char text[1024];

			fprintf(details, "%s | %s | passes %d units %d bakes %d%s | tolerance %d | mean %.3f max %.2f | %s\n",
				level_names[level],
				plan.status == _nv2a_ff_plan_exact ? "exact" : plan.status == _nv2a_ff_plan_approximate ? "approximate" :
				"impossible", plan.pass_count, units, plan.bake_count, plan.constants_bound ? " (constants bound)" : "",
				plan.tolerance, result.mean_error, result.maximum_error, notes);
			nv2a_ff_plan_describe(&plan, text, sizeof(text));
			fprintf(details, "%s", text);
			fflush(details);
		}
	}
}

static void print_group_table(void)
{
	int index, level, stage;

	printf("\n");
	printf("site (file:line)                      variants light");
	for (level = 0; level < NUMBER_OF_LEVELS; level++)
		printf(" | %-3s exact approx imposs passes", level_names[level]);
	printf("\n");
	for (index = 0; index < group_count; index++)
	{
		const struct group *group = &groups[index];

		printf("%-38s %7ld %5ld", group->name, group->variants, group->light_variants);
		for (level = 0; level < NUMBER_OF_LEVELS; level++)
		{
			const struct group_level *totals = &group->levels[level];

			printf(" |     %5ld %6ld %6ld %6d", totals->exact, totals->approximate, totals->impossible, totals->maximum_passes);
		}
		printf("\n");
	}

	printf("\nthe exact plans: largest difference from the reference seen, and largest tolerance, in 1/255:\n");
	for (level = 0; level < NUMBER_OF_LEVELS; level++)
	{
		float worst = 0.0f;
		int tolerance = 0;

		for (index = 0; index < group_count; index++)
		{
			if (groups[index].levels[level].worst_exact > worst)
				worst = groups[index].levels[level].worst_exact;
			if (groups[index].levels[level].maximum_tolerance > tolerance)
				tolerance = groups[index].levels[level].maximum_tolerance;
		}
		printf("  %-3s %.0f (tolerance up to %d)\n", level_names[level], worst, tolerance);
	}

	printf("\nwhat the approximate plans gave up (dropped stages, other approximations; worst mean and largest error in 1/255):\n");
	for (index = 0; index < group_count; index++)
	{
		const struct group *group = &groups[index];

		for (level = 0; level < NUMBER_OF_LEVELS; level++)
		{
			const struct group_level *totals = &group->levels[level];
			struct nv2a_ff_plan flags;
			char notes[160];

			if (!totals->approximate && !totals->impossible)
				continue;
			memset(&flags, 0, sizeof(flags));
			flags.approximations = totals->approximations & ~(unsigned long)_nv2a_ff_approximate_dropped_texture;
			describe_approximations(&flags, notes);
			printf("%-38s %-3s:", group->name, level_names[level]);
			for (stage = 0; stage < 4; stage++)
			{
				if (totals->dropped_stages & (1 << stage))
					printf(" t%d", stage);
			}
			printf(" %s| mean %.2f max %.0f", notes, totals->worst_mean, totals->worst_maximum);
			if (totals->with_bakes)
				printf(" | %ld with derived textures", totals->with_bakes);
			if (totals->bound)
				printf(" | %ld for the constants' values", totals->bound);
			printf("\n");
		}
	}
}

static int compare_times(const void *a, const void *b)
{
	float first = *(const float *)a, second = *(const float *)b;

	return first < second ? -1 : first > second;
}

static void print_compile_times(void)
{
	int level, kind;

	printf("\ncompile time on this machine, in microseconds (a state seen for the first time, without constants;\n"
		"then, for the ones whose plan is not exact, with the constants of the moment):\n");
	for (kind = 0; kind < NUMBER_OF_COMPILE_KINDS; kind++)
	{
		for (level = 0; level < NUMBER_OF_LEVELS; level++)
		{
			long count = compile_times[level][kind].count;
			long kept = count < MAXIMUM_TIMES ? count : MAXIMUM_TIMES;

			if (count == 0)
				continue;
			qsort(compile_times[level][kind].times, (size_t)kept, sizeof(float), compare_times);
			printf("  %-3s %s %5ld states: mean %6.0f, median %6.0f, 95%% under %6.0f, longest %6.0f (%s: %lu attempts, "
				"%lu passes tried, %lu candidates); most candidates %lu\n",
				level_names[level], kind == _compile_general ? "first    " : "constants", count,
				1.0e6 * compile_times[level][kind].total / (double)count,
				1.0e6 * compile_times[level][kind].times[kept / 2], 1.0e6 * compile_times[level][kind].times[kept * 95 / 100],
				1.0e6 * compile_times[level][kind].longest, compile_times[level][kind].longest_name,
				compile_times[level][kind].longest_statistics.attempts, compile_times[level][kind].longest_statistics.fits,
				compile_times[level][kind].longest_statistics.steps, compile_times[level][kind].most_candidates);
		}
	}
	printf("  most candidates looked at for an exact plan: %lu\n", exact_most_candidates);
}

/* ---------- random states */

static unsigned long fuzz_input(int final)
{
	static const unsigned long registers[] = { 0, 0, 1, 2, 3, 4, 4, 5, 8, 8, 8, 9, 9, 10, 11, 12, 12, 12, 13 };
	unsigned long input = registers[test_random(sizeof(registers) / sizeof(registers[0]))];

	if (final && test_random(6) == 0)
		input = 14 + test_random(2);
	if (test_random(3) == 0)
		input |= 0x10;
	if (final)
		input |= test_random(4) == 0 ? 0x20 : 0;
	else
		input |= test_random(2) ? 0 : test_random(3) ? 0x20 : test_random(8) << 5;
	return input;
}

static unsigned long fuzz_outputs(int alpha, int last)
{
	static const unsigned long registers[] = { 0, 0, 0, 4, 5, 8, 9, 10, 11, 12, 12, 13, 13 };
	static const unsigned long mappings[] = { 0x00, 0x00, 0x00, 0x00, 0x08, 0x10, 0x18, 0x20, 0x30 };
	unsigned long ab = 0, cd = 0, sum = 0, flags;

	if (test_random(3) || last)
		sum = last && test_random(4) ? 12 : registers[test_random(sizeof(registers) / sizeof(registers[0]))];
	if (test_random(4) == 0)
		ab = registers[test_random(sizeof(registers) / sizeof(registers[0]))];
	if (test_random(5) == 0)
		cd = registers[test_random(sizeof(registers) / sizeof(registers[0]))];
	flags = mappings[test_random(sizeof(mappings) / sizeof(mappings[0]))];
	if (test_random(8) == 0)
		flags |= 0x04;
	if (!alpha)
	{
		if (test_random(10) == 0)
			flags |= 0x02;
		if (test_random(12) == 0)
			flags |= 0x01;
		if (test_random(16) == 0)
			flags |= 0x80;
		if (test_random(16) == 0)
			flags |= 0x40;
	}
	return (flags << 12) | (sum << 8) | (ab << 4) | cd;
}

static void fuzz_state(struct test_state *test)
{
	static const unsigned long modes[] = { 0, 1, 1, 1, 1, 1, 3, 2, 0x11, 4, 5, 6 };
	static const unsigned long sources[] = { 1, 1, 0x302, 0x302, 0x306, 0, 0x304, 0x8001, 0x307 };
	static const unsigned long destinations[] = { 0, 1, 1, 0x303, 0x303, 0x300, 0x302, 0x8002, 0x301 };
	static const unsigned long operations[] = { 0x8006, 0x8006, 0x8006, 0x8006, 0x800b, 0x8007, 0x8008, 0x800a };
	int count = 1 + (int)(test_random(3) ? test_random(2) : test_random(8));
	int index, stage;

	memset(test, 0, sizeof(*test));
	test->state.combiner_state[53] = (unsigned long)count | (test_random(2) ? 0x100 : 0) | (test_random(2) ? 0x1000 : 0) |
		(test_random(2) ? 0x10000 : 0);
	for (index = 0; index < count; index++)
	{
		test->state.combiner_state[0 + index] = (fuzz_input(0) << 24) | (fuzz_input(0) << 16) |
			(test_random(2) ? (fuzz_input(0) << 8) | fuzz_input(0) : 0);
		test->state.combiner_state[34 + index] = (fuzz_input(0) << 24) | (fuzz_input(0) << 16) |
			(test_random(2) ? (fuzz_input(0) << 8) | fuzz_input(0) : 0);
		test->state.combiner_state[26 + index] = fuzz_outputs(1, index == count - 1);
		test->state.combiner_state[45 + index] = fuzz_outputs(0, index == count - 1);
	}
	if (test_random(8))
	{
		test->state.combiner_state[8] = test_random(3) ? 12 + (test_random(4) ? 0 : fuzz_input(1) << 8) :
			(fuzz_input(1) << 24) | (fuzz_input(1) << 16) | (fuzz_input(1) << 8) | fuzz_input(1);
		test->state.combiner_state[9] = (test_random(4) ? 0 : (fuzz_input(1) << 24) | (fuzz_input(1) << 16)) |
			((test_random(3) ? 0x1c : fuzz_input(1)) << 8) | (test_random(8) ? 0 : test_random(8) << 5);
	}
	for (stage = 0; stage < 4; stage++)
	{
		unsigned long mode = test_random(4 - stage + 1) ? modes[test_random(sizeof(modes) / sizeof(modes[0]))] : 0;

		if (stage == 0 && (mode == 0x11 || mode == 6))
			mode = 1;
		test->state.texture_modes |= mode << (5 * stage);
		test->state.sampler_type[stage] = mode_sampler(mode);
		test->state.alpha_kill[stage] = (unsigned char)(test_random(16) == 0);
		test->state.color_sign[stage] = (unsigned char)(test_random(24) == 0 ? test_random(16) : 0);
		test->constants.bump_luminance[stage][0] = 1.0f;
	}
	test->state.combiner_state[42] = test_random(0x10000);
	test->state.alpha_test_function = test_random(4) ? 0 : 0x200 + test_random(8);
	test->state.fog_enable = (unsigned char)test_random(2);
	test->state.fog_table_mode = (unsigned char)(test_random(8) ? 0 : test_random(4));
	test_random_constants(&test->constants, &test->constants);
	test->constants.fog_parameters[1] = 1.0f;
	test->constants.fog_parameters[2] = 1.0f;
	test->constants.alpha_reference = (float)test_random(256);
	test->draw.blend_enable = (unsigned char)(test_random(3) != 0);
	index = (int)test_random(sizeof(sources) / sizeof(sources[0]));
	test->draw.source_blend = sources[index];
	test->draw.destination_blend = destinations[test_random(3) ? (unsigned long)index : test_random(sizeof(destinations) / sizeof(destinations[0]))];
	test->draw.blend_operation = operations[test_random(sizeof(operations) / sizeof(operations[0]))];
	test->draw.write_color = (unsigned char)(test_random(10) != 0);
	test->draw.write_alpha = (unsigned char)(test_random(3) == 0);
	test->draw.depth_write = (unsigned char)test_random(2);
	for (stage = 0; stage < 4; stage++)
		test->draw.stage_rank[stage] = (unsigned char)(test_random(4) ? 0 : test_random(4));

	if (test_random(8) == 0)
	{
		/* now and then anything at all: words no game would set, which
		the compiler has to survive */
		for (index = 0; index < NV2A_FF_COMBINER_STATE_COUNT; index++)
		{
			if (test_random(3) == 0)
				test->state.combiner_state[index] = (test_random(0x10000) << 16) | test_random(0x10000);
		}
		if (test_random(2))
			test->state.texture_modes = test_random(0x100000);
		for (stage = 0; stage < 4; stage++)
		{
			if (test_random(2))
				test->state.sampler_type[stage] = (unsigned char)test_random(4);
			test->state.color_sign[stage] = (unsigned char)test_random(16);
		}
		if (test_random(4) == 0)
			test->draw.source_blend = test_random(0x10000);
		if (test_random(4) == 0)
			test->draw.destination_blend = test_random(0x10000);
		if (test_random(4) == 0)
			test->draw.blend_operation = test_random(0x10000);
		if (test_random(4) == 0)
			test->state.alpha_test_function = test_random(0x10000);
		if (test_random(4) == 0)
			test->state.fog_table_mode = (unsigned char)test_random(256);
	}
}

static void fuzz(long count, long samples)
{
	long totals[NUMBER_OF_LEVELS][3];
	long wrong = 0, asking = 0, index;
	int level;

	memset(totals, 0, sizeof(totals));
	for (index = 0; index < count; index++)
	{
		struct test_state test;

		fuzz_state(&test);
		for (level = 0; level < NUMBER_OF_LEVELS; level++)
		{
			struct nv2a_ff_capabilities capabilities;
			struct check_result result;

			level_capabilities(level, &capabilities);
			/* every other state without the constants: the general plan only */
			nv2a_ff_compile(&test.state, (index & 1) ? &test.constants : NULL, &capabilities, &test.draw, &plan);
			totals[level][plan.status]++;
			if (plan.status == _nv2a_ff_plan_impossible)
				continue;
			/* (random blends of the draw's own are the draw's, not the plan's:
			only the game's are held to OpenGL 1.1's factors) */
			if (plan_asks_too_much(&plan, &capabilities) && !strstr(plan_asks_too_much(&plan, &capabilities), "OpenGL 1.1"))
			{
				asking++;
				failures++;
				if (asking <= 10)
				{
					printf("# FAIL random state %ld %s: the plan asks for %s\n", index, level_names[level],
						plan_asks_too_much(&plan, &capabilities));
				}
			}
			check_plan(&test, &capabilities, &plan, samples, &result);
			if (plan.status == _nv2a_ff_plan_exact && result.over_tolerance && !byte_colors)
			{
				wrong++;
				failures++;
				if (wrong <= 10)
				{
					char text[1024];
					int word;

					printf("# FAIL random state %ld %s: called exact, %ld of %ld samples differ (worst %.2f)\n", index,
						level_names[level], result.over_tolerance, result.samples, result.maximum_error);
					printf("#   modes %05lx count %05lx final %08lx %08lx blend %d %lx %lx %lx alpha test %lx\n",
						test.state.texture_modes, test.state.combiner_state[53], test.state.combiner_state[8],
						test.state.combiner_state[9], test.draw.blend_enable, test.draw.source_blend,
						test.draw.destination_blend, test.draw.blend_operation, test.state.alpha_test_function);
					for (word = 0; word < (int)(test.state.combiner_state[53] & 0xff); word++)
					{
						printf("#   stage %d: rgb %08lx -> %05lx, alpha %08lx -> %05lx\n", word,
							test.state.combiner_state[34 + word], test.state.combiner_state[45 + word],
							test.state.combiner_state[0 + word], test.state.combiner_state[26 + word]);
					}
					nv2a_ff_plan_describe(&plan, text, sizeof(text));
					printf("%s", text);
				}
			}
		}
	}
	printf("\nrandom states: %ld, %ld samples each\n", count, samples);
	for (level = 0; level < NUMBER_OF_LEVELS; level++)
	{
		printf("  %-3s exact %ld, approximate %ld, impossible %ld\n", level_names[level], totals[level][0],
			totals[level][1], totals[level][2]);
	}
	printf("  called exact but wrong: %ld\n", wrong);
	printf("  asking for what the capabilities lack: %ld\n", asking);
}

/* ---------- small checks of the parts by hand */

static void check(int condition, const char *text)
{
	if (!condition)
	{
		failures++;
		printf("# FAIL %s\n", text);
	}
}

static int near(float a, float b)
{
	return fabs(a - b) < 1.0e-5;
}

static void hand_checks(void)
{
	struct nv2a_ff_state state;
	struct nv2a_ff_constants constants;
	struct nv2a_ff_fragment fragment;
	struct nv2a_ff_unit unit;
	float result[4], previous[4], primary[4], texel[4], constant[4], destination[4];
	int drawn;

	/* the reference: detail_objects.c:443, t0 * v0 in colour and alpha */
	memset(&state, 0, sizeof(state));
	memset(&constants, 0, sizeof(constants));
	memset(&fragment, 0, sizeof(fragment));
	state.texture_modes = 1;
	state.sampler_type[0] = 1;
	state.combiner_state[53] = 1;
	state.combiner_state[34] = 0x08040000;
	state.combiner_state[45] = 0xC0;
	state.combiner_state[0] = 0x18140000;
	state.combiner_state[26] = 0xC0;
	state.combiner_state[8] = 0xC;
	state.combiner_state[9] = 0x1C00;
	fragment.texel[0][0] = 0.5f; fragment.texel[0][1] = 0.25f; fragment.texel[0][2] = 1.0f; fragment.texel[0][3] = 0.5f;
	fragment.v0[0] = 0.5f; fragment.v0[1] = 1.0f; fragment.v0[2] = 0.75f; fragment.v0[3] = 0.5f;
	drawn = nv2a_ff_reference(&state, &constants, &fragment, result);
	check(drawn && near(result[0], 0.25f) && near(result[1], 0.25f) && near(result[2], 0.75f) && near(result[3], 0.25f),
		"reference: t0 * v0");

	/* the alpha test compares bytes: 0.25 is 64 */
	state.alpha_test_function = _nv2a_ff_compare_greater;
	constants.alpha_reference = 64.0f;
	check(!nv2a_ff_reference(&state, &constants, &fragment, result), "reference: alpha test rejects 64 > 64");
	constants.alpha_reference = 63.0f;
	check(nv2a_ff_reference(&state, &constants, &fragment, result), "reference: alpha test passes 64 > 63");
	state.alpha_test_function = 0;

	/* expand and the output scale: (2 * t0 - 1) * 2 clamps to 1, and the final clamp to 0 */
	state.combiner_state[34] = 0x48200000; /* expand(t0) * 1 */
	state.combiner_state[45] = 0x100C0;    /* shift left 1, into r0 */
	drawn = nv2a_ff_reference(&state, &constants, &fragment, result);
	check(drawn && near(result[0], 0.0f) && near(result[1], 0.0f) && near(result[2], 1.0f), "reference: expand, scale, clamps");

	/* the final combiner's sum register: v1 + r0 */
	state.combiner_state[34] = 0x08200000;
	state.combiner_state[45] = 0xC0;
	state.combiner_state[8] = 0x0000000E;
	fragment.v1[0] = 0.25f; fragment.v1[1] = 0.5f; fragment.v1[2] = 0.5f;
	drawn = nv2a_ff_reference(&state, &constants, &fragment, result);
	check(drawn && near(result[0], 0.75f) && near(result[1], 0.75f) && near(result[2], 1.0f), "reference: v1 + r0");

	/* the model: the OpenGL 1.1 environments */
	memset(&unit, 0, sizeof(unit));
	previous[0] = 0.5f; previous[1] = 0.5f; previous[2] = 0.5f; previous[3] = 0.5f;
	texel[0] = 1.0f; texel[1] = 0.5f; texel[2] = 0.0f; texel[3] = 0.25f;
	constant[0] = 0.0f; constant[1] = 1.0f; constant[2] = 1.0f; constant[3] = 1.0f;
	memcpy(primary, previous, sizeof(primary));
	unit.environment = _nv2a_ff_environment_modulate;
	nv2a_ff_model_unit(&unit, previous, primary, texel, constant, result);
	check(near(result[0], 0.5f) && near(result[1], 0.25f) && near(result[2], 0.0f) && near(result[3], 0.125f), "model: modulate");
	unit.environment = _nv2a_ff_environment_decal;
	nv2a_ff_model_unit(&unit, previous, primary, texel, constant, result);
	check(near(result[0], 0.625f) && near(result[1], 0.5f) && near(result[2], 0.375f) && near(result[3], 0.5f), "model: decal");
	unit.environment = _nv2a_ff_environment_blend;
	nv2a_ff_model_unit(&unit, previous, primary, texel, constant, result);
	check(near(result[0], 0.0f) && near(result[1], 0.75f) && near(result[2], 0.5f) && near(result[3], 0.125f), "model: blend");
	unit.environment = _nv2a_ff_environment_add;
	nv2a_ff_model_unit(&unit, previous, primary, texel, constant, result);
	check(near(result[0], 1.0f) && near(result[1], 1.0f) && near(result[2], 0.5f) && near(result[3], 0.125f), "model: add");

	/* the model: combine, interpolate(texture, constant, primary alpha) * 2 */
	unit.environment = _nv2a_ff_environment_combine;
	unit.rgb.function = _nv2a_ff_combine_interpolate;
	unit.rgb.source[0] = _nv2a_ff_source_texture;
	unit.rgb.source[1] = _nv2a_ff_source_constant;
	unit.rgb.source[2] = _nv2a_ff_source_primary;
	unit.rgb.operand[2] = _nv2a_ff_operand_alpha;
	unit.rgb.scale = 2;
	unit.alpha.function = _nv2a_ff_combine_add_signed;
	unit.alpha.source[0] = _nv2a_ff_source_texture;
	unit.alpha.operand[0] = _nv2a_ff_operand_alpha;
	unit.alpha.source[1] = _nv2a_ff_source_previous;
	unit.alpha.operand[1] = _nv2a_ff_operand_inverse_alpha;
	unit.alpha.scale = 1;
	nv2a_ff_model_unit(&unit, previous, primary, texel, constant, result);
	check(near(result[0], 1.0f) && near(result[1], 1.0f) && near(result[2], 1.0f) && near(result[3], 0.25f), "model: combine");

	/* the model: blending */
	previous[0] = 0.5f; previous[1] = 0.25f; previous[2] = 1.0f; previous[3] = 0.5f;
	destination[0] = 0.5f; destination[1] = 1.0f; destination[2] = 0.5f; destination[3] = 1.0f;
	nv2a_ff_model_blend(_nv2a_ff_blend_source_alpha, _nv2a_ff_blend_inverse_source_alpha, _nv2a_ff_blend_operation_add,
		previous, destination, constant, result);
	check(near(result[0], 0.5f) && near(result[1], 0.625f) && near(result[2], 0.75f), "model: alpha blend");
	nv2a_ff_model_blend(_nv2a_ff_blend_destination_color, _nv2a_ff_blend_source_color, _nv2a_ff_blend_operation_add,
		previous, destination, constant, result);
	check(near(result[0], 0.5f) && near(result[1], 0.5f) && near(result[2], 1.0f), "model: double multiply");
	nv2a_ff_model_blend(_nv2a_ff_blend_one, _nv2a_ff_blend_one, _nv2a_ff_blend_operation_reverse_subtract,
		previous, destination, constant, result);
	check(near(result[0], 0.0f) && near(result[1], 0.75f) && near(result[2], -0.5f), "model: reverse subtract");
	nv2a_ff_model_blend(_nv2a_ff_blend_zero, _nv2a_ff_blend_zero, _nv2a_ff_blend_operation_maximum,
		previous, destination, constant, result);
	check(near(result[0], 0.5f) && near(result[1], 1.0f) && near(result[2], 1.0f), "model: maximum ignores the factors");
}

/* ---------- a hand-written plan */

/* 0: declines; 1: texture 0 times the diffuse colour, which is what the
state computes; 2: the texture alone, which is not */
static int override_mode;

static int override_plan(const struct nv2a_ff_state *state, const struct nv2a_ff_constants *constants,
	const struct nv2a_ff_capabilities *capabilities, const struct nv2a_ff_draw *draw, struct nv2a_ff_plan *result)
{
	struct nv2a_ff_pass *pass;
	struct nv2a_ff_unit *unit;

	(void)constants;
	(void)capabilities;
	if (!override_mode)
		return 0;
	pass = nv2a_ff_override_pass(result, state, draw);
	unit = pass ? nv2a_ff_override_unit(pass, 0, override_mode == 1 ? _nv2a_ff_environment_modulate :
		_nv2a_ff_environment_replace) : NULL;
	if (!unit)
		return 0;
	pass->primary_rgb = nv2a_ff_override_expression(result, NV2A_FF_EXPRESSION_NONE, 1.0f,
		NV2A_FF_ATOM(_nv2a_ff_atom_v0, _nv2a_ff_channel_rgb), NV2A_FF_ATOM_ONE);
	pass->primary_alpha = nv2a_ff_override_expression(result, NV2A_FF_EXPRESSION_NONE, 1.0f,
		NV2A_FF_ATOM(_nv2a_ff_atom_v0, _nv2a_ff_channel_alpha), NV2A_FF_ATOM_ONE);
	return 1;
}

static void override_checks(void)
{
	struct test_state test;
	struct nv2a_ff_capabilities capabilities;
	struct check_result result;

	/* detail_objects.c:443 again: t0 * v0, alpha blended */
	memset(&test, 0, sizeof(test));
	test.state.texture_modes = 1;
	test.state.sampler_type[0] = 1;
	test.state.combiner_state[53] = 1;
	test.state.combiner_state[34] = 0x08040000;
	test.state.combiner_state[45] = 0xC0;
	test.state.combiner_state[0] = 0x18140000;
	test.state.combiner_state[26] = 0xC0;
	test.state.combiner_state[8] = 0xC;
	test.state.combiner_state[9] = 0x1C00;
	test.draw.blend_enable = 1;
	test.draw.source_blend = _nv2a_ff_blend_source_alpha;
	test.draw.destination_blend = _nv2a_ff_blend_inverse_source_alpha;
	test.draw.blend_operation = _nv2a_ff_blend_operation_add;
	test.draw.write_color = 1;
	level_capabilities(_level_1, &capabilities);
	check(nv2a_ff_override_register(nv2a_ff_state_hash(&test.state), override_plan), "override: registered");

	override_mode = 0;
	nv2a_ff_compile(&test.state, NULL, &capabilities, &test.draw, &plan);
	check(plan.status == _nv2a_ff_plan_exact && !plan.from_override, "override: declined, the compiler's plan stands");

	override_mode = 1;
	nv2a_ff_compile(&test.state, NULL, &capabilities, &test.draw, &plan);
	check_plan(&test, &capabilities, &plan, 200, &result);
	check(plan.from_override && plan.status == _nv2a_ff_plan_exact && plan.pass_count == 1 &&
		plan.passes[0].blend_enable && !result.over_tolerance, "override: a right plan is taken and found exact");

	override_mode = 2;
	nv2a_ff_compile(&test.state, NULL, &capabilities, &test.draw, &plan);
	check(plan.from_override && plan.status == _nv2a_ff_plan_approximate &&
		(plan.approximations & _nv2a_ff_approximate_unexplained), "override: a wrong plan is taken and found out");
	override_mode = 0;
}

/* ---------- main */

int main(int argc, char **argv)
{
	long fuzz_count = 3000;
	const char *details_name = NULL;
	int argument;

	for (argument = 1; argument < argc; argument++)
	{
		if (!strcmp(argv[argument], "--samples") && argument + 1 < argc)
			sample_count = atol(argv[++argument]);
		else if (!strcmp(argv[argument], "--fuzz") && argument + 1 < argc)
			fuzz_count = atol(argv[++argument]);
		else if (!strcmp(argv[argument], "--details") && argument + 1 < argc)
			details_name = argv[++argument];
		else if (!strcmp(argv[argument], "--site") && argument + 1 < argc)
			site_filter = argv[++argument];
		else if (!strcmp(argv[argument], "--byte-colors"))
			byte_colors = 1;
		else
		{
			printf("usage: combiner_ff_test [--samples N] [--fuzz N] [--details file] [--site text]\n");
			return 1;
		}
	}
	if (details_name)
		details = fopen(details_name, "w");
	nv2a_ff_model_byte_colors(byte_colors);

	hand_checks();
	sites_enumerate(site_test);
	printf("# %ld states in %d sites, %d levels, %ld samples for each plan\n", site_total, group_count, NUMBER_OF_LEVELS,
		sample_count);
	print_group_table();
	print_compile_times();
	if (fuzz_count > 0)
		fuzz(fuzz_count, sample_count < 500 ? sample_count : 500);
	override_checks();
	if (details)
		fclose(details);
	printf("\n# combiner_ff_test: %ld plans, %ld samples, %ld failed\n", plans_total, plans_checked_samples, failures);
	return failures > 255 ? 255 : (int)failures;
}
