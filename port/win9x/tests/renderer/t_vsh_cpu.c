/*
T_VSH_CPU.C

The vertex program evaluators of the light renderer (port/linux/src/
nv2a_vsh_cpu.c), driven by tools/nv2a_vsh_cpu_test.py, which writes the
cases (programs, constants, vertex streams, the arrays to produce) and
compares the results with its own interpreter of the GLSL that
nv2a_vsh.c generates (tools/nv2a_vsh_reference.py).

	t_vsh_cpu run CASES RESULTS    every case and array set with both
	                               evaluators, exact and with estimates
	t_vsh_cpu classes CASES        each program's classification
	t_vsh_cpu bench CASES          vertices a second of each case
	t_vsh_cpu count CASES          instructions executed for each case, by
	                               kind (the processor single-stepped)

A case file is a sequence of little-endian words and floats; see
read_case. For each case, array set and mode (scalar exact, SSE exact,
scalar with estimates, SSE with estimates) the results file holds the
vertices as records of RECORD_BYTES bytes: position, two texture
coordinate sets, colour, fog, with bytes no array covers left at 0xCD.
After a case's records come the matrices of its affine outputs.

The evaluators must leave MXCSR as they found it; each run starts from a
different one (rounding modes, flags, flush to zero) and the number of
runs that changed it is printed, as is the number of runs whose scalar
and SSE results differ in any bit (NaNs are compared as NaNs only). The
x87's control word varies from run to run as well (precision and rounding),
must come back the same, and must not change a result: the records of
every run are compared with the reference's.
*/

#include "nv2a_vsh_cpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <xmmintrin.h>

#define CASE_MAGIC 0x54485356 /* VSHT */
#define RECORD_BYTES 56
#define RECORD_FILL 0xCD

struct layout
{
	unsigned int flags;
	unsigned int position;
	unsigned int texcoord_source[2];
	unsigned int texcoord_components[2];
	unsigned int color;
	unsigned int color_source;
	unsigned int alpha_source;
	unsigned int alpha_component;
	unsigned int has_scale;
	float scale[4];
	unsigned int fog;
};

struct test_case
{
	unsigned int instruction_count;
	DWORD *words;
	float constants[XGPU_VERTEX_CONSTANT_COUNT][4];
	float viewport_scale[4];
	float viewport_offset[4];
	float screen_offset;
	unsigned int vertex_count;
	unsigned int first;
	struct
	{
		unsigned int kind;
		unsigned int type;
		unsigned int offset;
		float value[4];
	} inputs[XGPU_VERTEX_ATTRIBUTE_COUNT];
	unsigned int stride;
	unsigned int stream_bytes;
	unsigned char *stream;
	unsigned int layout_count;
	struct layout *layouts;
};

static FILE *input;

static void read_bytes(void *data, size_t size)
{
	if (size && fread(data, 1, size, input) != size)
	{
		printf("# FAIL the case file ends early\n");
		exit(2);
	}
}

static unsigned int read_word(void)
{
	unsigned int word;

	read_bytes(&word, sizeof(word));
	return word;
}

static void read_case(struct test_case *test)
{
	unsigned int index;

	memset(test, 0, sizeof(*test));
	test->instruction_count = read_word();
	test->words = malloc((test->instruction_count * 4 + 1) * sizeof(DWORD));
	read_bytes(test->words, test->instruction_count * 4 * sizeof(DWORD));
	read_bytes(test->constants, sizeof(test->constants));
	read_bytes(test->viewport_scale, sizeof(test->viewport_scale));
	read_bytes(test->viewport_offset, sizeof(test->viewport_offset));
	read_bytes(&test->screen_offset, sizeof(test->screen_offset));
	test->vertex_count = read_word();
	test->first = read_word();
	for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
	{
		test->inputs[index].kind = read_word();
		test->inputs[index].type = read_word();
		test->inputs[index].offset = read_word();
		read_bytes(test->inputs[index].value, sizeof(test->inputs[index].value));
	}
	test->stride = read_word();
	test->stream_bytes = read_word();
	test->stream = malloc(test->stream_bytes + 1);
	read_bytes(test->stream, test->stream_bytes);
	test->layout_count = read_word();
	test->layouts = calloc(test->layout_count + 1, sizeof(*test->layouts));
	for (index = 0; index < test->layout_count; index++)
		read_bytes(&test->layouts[index], sizeof(test->layouts[index]));
}

static void free_case(struct test_case *test)
{
	free(test->words);
	free(test->stream);
	free(test->layouts);
}

static void make_draw(const struct test_case *test, const struct layout *layout, unsigned char *records,
	struct nv2a_vsh_cpu_draw *draw)
{
	unsigned int index;

	memset(draw, 0, sizeof(*draw));
	draw->constants = test->constants;
	for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
	{
		struct nv2a_vsh_cpu_input *in = &draw->inputs[index];

		memcpy(in->value, test->inputs[index].value, sizeof(in->value));
		in->type = test->inputs[index].type;
		if (test->inputs[index].kind)
		{
			in->data = test->stream + test->inputs[index].offset;
			in->stride = test->inputs[index].kind == 1 ? test->stride : 0;
		}
	}
	memcpy(draw->viewport_scale, test->viewport_scale, sizeof(draw->viewport_scale));
	memcpy(draw->viewport_offset, test->viewport_offset, sizeof(draw->viewport_offset));
	draw->screen_offset = test->screen_offset;
	draw->flags = layout->flags;
	if (layout->position)
	{
		draw->position.data = records;
		draw->position.stride = RECORD_BYTES;
	}
	for (index = 0; index < 2; index++)
	{
		draw->texcoords[index].source = (unsigned char)layout->texcoord_source[index];
		draw->texcoords[index].components = (unsigned char)layout->texcoord_components[index];
		if (layout->texcoord_source[index] != NV2A_VSH_OUTPUT_NONE)
		{
			draw->texcoords[index].array.data = records + 16 + index * 16;
			draw->texcoords[index].array.stride = RECORD_BYTES;
		}
	}
	if (layout->color)
	{
		draw->colors[0].array.data = records + 48;
		draw->colors[0].array.stride = RECORD_BYTES;
	}
	draw->colors[0].color_source = (unsigned char)layout->color_source;
	draw->colors[0].alpha_source = (unsigned char)layout->alpha_source;
	draw->colors[0].alpha_component = (unsigned char)layout->alpha_component;
	draw->colors[0].color_scale = layout->has_scale ? layout->scale : NULL;
	if (layout->fog)
	{
		draw->fog.data = records + 52;
		draw->fog.stride = RECORD_BYTES;
	}
}

/* the floating-point states the runs start from: every rounding mode,
flush to zero on and off, exception flags set and clear (exceptions stay
masked) */
static unsigned int start_mxcsr(unsigned int run)
{
	return 0x1F80 | ((run & 3) << 13) | ((run & 4) ? 0x8000 : 0) | ((run & 8) ? 0x003D : 0);
}

/* and the x87 control words: the usual one, single and extended precision,
rounding down, up and towards zero (exceptions masked) */
#ifdef __i386__
static unsigned short start_x87(unsigned int run)
{
	static const unsigned short words[] = { 0x027F, 0x007F, 0x037F, 0x067F, 0x0A7F, 0x0E7F };

	return words[(run / 16) % (sizeof(words) / sizeof(words[0]))];
}

static unsigned short x87_control(void)
{
	unsigned short word;

	__asm__ volatile ("fnstcw %0" : "=m" (word));
	return word;
}

static void x87_set_control(unsigned short word)
{
	__asm__ volatile ("fldcw %0" : : "m" (word));
}
#else
#define start_x87(run) 0
#define x87_control() 0
#define x87_set_control(word) (void)(word)
#endif

/* The same bytes, or NaNs in the same places: which of two NaN operands an
operation passes on is the compiler's choice of operand order, so their
sign and payload may differ between the evaluators. */
static BOOL records_equal(const unsigned char *a, const unsigned char *b, unsigned int size)
{
	unsigned int index;

	for (index = 0; index + 4 <= size; index += 4)
	{
		unsigned int x, y;

		memcpy(&x, a + index, 4);
		memcpy(&y, b + index, 4);
		if (x == y)
			continue;
		/* the colour is the record's bytes 48 to 51 */
		if (index % RECORD_BYTES == 48 || (x & 0x7FFFFFFF) <= 0x7F800000 || (y & 0x7FFFFFFF) <= 0x7F800000)
			return FALSE;
	}
	return TRUE;
}

static int command_run(const char *results_name)
{
	FILE *results = fopen(results_name, "wb");
	unsigned int case_count, case_index, layout_index, mode, run = 0;
	unsigned long changed_state = 0, different = 0, runs = 0, failed = 0, variants = 0;

	if (!results)
	{
		printf("# FAIL cannot write %s\n", results_name);
		return 2;
	}
	if (read_word() != CASE_MAGIC)
	{
		printf("# FAIL not a case file\n");
		return 2;
	}
	case_count = read_word();
	for (case_index = 0; case_index < case_count; case_index++)
	{
		struct test_case test;
		struct nv2a_vsh_cpu_program *program;
		const struct nv2a_vsh_classification *classification;
		unsigned char *records[4];
		unsigned int size, address, affine_count = 0;

		read_case(&test);
		/* (programs are made in the caller's state too: the first one, which
		fills the evaluator's tables, while rounding down) */
		_mm_setcsr(start_mxcsr(case_index + 1));
		program = nv2a_vsh_cpu_program_new(test.words, test.instruction_count);
		_mm_setcsr(0x1F80);
		size = test.vertex_count * RECORD_BYTES;
		for (mode = 0; mode < 4; mode++)
			records[mode] = malloc(size + 1);
		for (layout_index = 0; layout_index < test.layout_count; layout_index++)
		{
			for (mode = 0; mode < 4; mode++)
			{
				struct nv2a_vsh_cpu_draw draw;
				struct layout layout = test.layouts[layout_index];
				unsigned short x87_start = x87_control(), x87_before = start_x87(run), x87_after;
				unsigned int before = start_mxcsr(run++), after;
				BOOL done;

				if (mode < 2)
					layout.flags |= NV2A_VSH_CPU_EXACT;
				else
					layout.flags &= ~(unsigned int)NV2A_VSH_CPU_EXACT;
				memset(records[mode], RECORD_FILL, size);
				make_draw(&test, &layout, records[mode], &draw);
				x87_set_control(x87_before);
				_mm_setcsr(before);
				done = (mode & 1) ? nv2a_vsh_cpu_run(program, &draw, test.first, test.vertex_count) :
					nv2a_vsh_cpu_run_scalar(program, &draw, test.first, test.vertex_count);
				after = _mm_getcsr();
				_mm_setcsr(0x1F80);
				x87_after = x87_control();
				x87_set_control(x87_start);
				runs++;
				if (after != before || x87_after != x87_before)
					changed_state++;
				if (!done)
					failed++;
				fwrite(records[mode], 1, size, results);
			}
			if (!records_equal(records[0], records[1], size))
			{
				different++;
				printf("# FAIL case %u array set %u: scalar and SSE differ (exact)\n", case_index, layout_index);
			}
			if (!records_equal(records[2], records[3], size))
			{
				different++;
				printf("# FAIL case %u array set %u: scalar and SSE differ (estimates)\n", case_index, layout_index);
			}
			variants += 2;
		}

		/* the affine outputs' matrices: address, input, 20 floats */
		classification = nv2a_vsh_cpu_classification(program);
		for (address = 0; address < NV2A_VSH_OUTPUT_COUNT; address++)
		{
			float matrix[4][5];

			if (nv2a_vsh_cpu_affine_output(program, address, test.constants, matrix))
				affine_count++;
		}
		fwrite(&affine_count, sizeof(affine_count), 1, results);
		for (address = 0; address < NV2A_VSH_OUTPUT_COUNT; address++)
		{
			float matrix[4][5];
			unsigned int header[2];

			if (!nv2a_vsh_cpu_affine_output(program, address, test.constants, matrix))
				continue;
			header[0] = address;
			header[1] = classification->outputs[address].class == _nv2a_vsh_class_affine ?
				classification->outputs[address].input : 255;
			fwrite(header, sizeof(header), 1, results);
			fwrite(matrix, sizeof(matrix), 1, results);
		}
		for (mode = 0; mode < 4; mode++)
			free(records[mode]);
		nv2a_vsh_cpu_program_delete(program);
		free_case(&test);
	}
	fclose(results);
	printf("cases %u runs %lu failed %lu mxcsr_changed %lu scalar_sse_pairs %lu different %lu\n",
		case_count, runs, failed, changed_state, variants, different);
	return failed || changed_state || different ? 1 : 0;
}

static const char *class_name(unsigned long class, unsigned long input, char *text)
{
	switch (class)
	{
	case _nv2a_vsh_class_unwritten: return "-";
	case _nv2a_vsh_class_constant: return "C";
	case _nv2a_vsh_class_affine: sprintf(text, "A:v%lu", input); return text;
	case _nv2a_vsh_class_indexed: return "M";
	case _nv2a_vsh_class_skinned: sprintf(text, "S:v%lu", input); return text;
	default: return "X";
	}
}

static int command_classes(void)
{
	static const char *const names[NV2A_VSH_OUTPUT_COUNT] =
	{
		"oPos", NULL, NULL, "oD0", "oD1", "oFog", "oPts", "oB0", "oB1", "oT0", "oT1", "oT2", "oT3"
	};
	unsigned int case_count, case_index, address, component;

	if (read_word() != CASE_MAGIC)
		return 2;
	case_count = read_word();
	for (case_index = 0; case_index < case_count; case_index++)
	{
		struct test_case test;
		struct nv2a_vsh_cpu_program *program;
		const struct nv2a_vsh_classification *classification;
		char text[4][16];

		read_case(&test);
		program = nv2a_vsh_cpu_program_new(test.words, test.instruction_count);
		classification = nv2a_vsh_cpu_classification(program);
		printf("vs%02u n=%2u ", case_index, test.instruction_count);
		for (address = 0; address < NV2A_VSH_OUTPUT_COUNT; address++)
		{
			const struct nv2a_vsh_output_class *output = &classification->outputs[address];
			BOOL same = TRUE;

			if (!output->written || !names[address])
				continue;
			for (component = 0; component < 4; component++)
			{
				if (output->component_class[component] != output->class ||
					(output->component_input[component] != output->input &&
						(output->class == _nv2a_vsh_class_affine || output->class == _nv2a_vsh_class_skinned)))
				{
					same = FALSE;
				}
			}
			if (same)
				printf(" %s=%s", names[address], class_name(output->class, output->input, text[0]));
			else
			{
				printf(" %s=%s/%s/%s/%s", names[address],
					class_name(output->component_class[0], output->component_input[0], text[0]),
					class_name(output->component_class[1], output->component_input[1], text[1]),
					class_name(output->component_class[2], output->component_input[2], text[2]),
					class_name(output->component_class[3], output->component_input[3], text[3]));
			}
			printf("[in %04x%s]", output->inputs, output->relative_constants ? " rel" : "");
		}
		printf("  | position %s", class_name(classification->position_class, classification->position_input, text[0]));
		if (classification->position_rows_valid)
		{
			printf(" rows %d %d %d %d %s", (int)classification->position_rows[0] - XGPU_VERTEX_CONSTANT_BIAS,
				(int)classification->position_rows[1] - XGPU_VERTEX_CONSTANT_BIAS,
				(int)classification->position_rows[2] - XGPU_VERTEX_CONSTANT_BIAS,
				(int)classification->position_rows[3] - XGPU_VERTEX_CONSTANT_BIAS,
				classification->position_homogeneous ? "dp4" : "dph");
		}
		if (classification->epilogue)
		{
			printf(" epilogue %d %d %d", classification->epilogue_scale, classification->epilogue_reciprocal,
				classification->epilogue_offset);
		}
		printf(" inputs %04x%s\n", classification->inputs, classification->relative_constants ? " rel" : "");
		nv2a_vsh_cpu_program_delete(program);
		free_case(&test);
	}
	return 0;
}

static double seconds(void)
{
	return (double)clock() / CLOCKS_PER_SEC;
}

static int command_bench(void)
{
	unsigned int case_count, case_index, layout_index, mode;

	if (read_word() != CASE_MAGIC)
		return 2;
	case_count = read_word();
	for (case_index = 0; case_index < case_count; case_index++)
	{
		struct test_case test;
		struct nv2a_vsh_cpu_program *program;
		unsigned char *records;

		read_case(&test);
		program = nv2a_vsh_cpu_program_new(test.words, test.instruction_count);
		records = malloc(test.vertex_count * RECORD_BYTES + 1);
		for (layout_index = 0; layout_index < test.layout_count; layout_index++)
		{
			struct nv2a_vsh_cpu_draw draw;
			struct nv2a_vsh_cpu_cost cost;
			double rate[2];
			unsigned int kind;

			make_draw(&test, &test.layouts[layout_index], records, &draw);
			for (mode = 0; mode < 2; mode++)
			{
				unsigned long repeats = 1, repeat;
				double elapsed = 0.0;

				/* until a run of the loop takes a quarter of a second */
				for (;;)
				{
					double start = seconds();

					for (repeat = 0; repeat < repeats; repeat++)
					{
						if (mode)
							nv2a_vsh_cpu_run(program, &draw, test.first, test.vertex_count);
						else
							nv2a_vsh_cpu_run_scalar(program, &draw, test.first, test.vertex_count);
					}
					elapsed = seconds() - start;
					if (elapsed >= 0.25)
						break;
					repeats *= 2;
				}
				rate[mode] = (double)repeats * test.vertex_count / elapsed;
			}
			nv2a_vsh_cpu_cost(program, nv2a_vsh_cpu_draw_mask(&draw), draw.flags, &cost);
			printf("%u %u %.0f %.0f %lu %lu %lu %lu", case_index, layout_index, rate[0], rate[1],
				cost.operations, cost.input_components, cost.constant_components, cost.slots);
			for (kind = 0; kind < NV2A_VSH_CPU_COST_COUNT; kind++)
				printf(" %lu:%lu", cost.operations_by_kind[kind], cost.results_by_kind[kind]);
			printf("\n");
			fflush(stdout);
		}
		free(records);
		nv2a_vsh_cpu_program_delete(program);
		free_case(&test);
	}
	return 0;
}

/* ---------- counting instructions

The processor's trap flag stops it after every instruction; the handler
below counts the instruction about to run and sets the flag again. What
the counts say about a Pentium III is worked out by the Python side. */

enum
{
	_step_total,
	_step_packed_load,       /* movaps, movlps, movhps from memory */
	_step_packed_store,      /* movaps to memory */
	_step_half_store,        /* movlps, movhps, movss to memory */
	_step_packed_register,   /* packed arithmetic and logic, register operands */
	_step_packed_memory,     /* the same with a memory operand */
	_step_shuffle,           /* shufps, unpcklps, unpckhps, movhlps, movlhps, movaps between registers */
	_step_divide,            /* divps, sqrtps */
	_step_scalar_sse,        /* movss, the ss operations, conversions, comiss */
	_step_x87,
	_step_indirect_jump,
	_step_call,
	NUMBER_OF_STEP_KINDS
};

static volatile int stepping;
static unsigned long steps[NUMBER_OF_STEP_KINDS];

struct step_exception_pointers
{
	DWORD *record;           /* the code is its first word */
	unsigned char *context;  /* Win32's CONTEXT for x86 */
};

#define STEP_CONTEXT_EIP 0xB8
#define STEP_CONTEXT_EFLAGS 0xC0
#define STEP_TRAP_FLAG 0x100

static void step_classify(const unsigned char *code)
{
	BOOL scalar = FALSE;

	steps[_step_total]++;
	while (*code == 0xF3 || *code == 0xF2 || *code == 0x66 || *code == 0x2E || *code == 0x36 || *code == 0x3E ||
		*code == 0x26 || *code == 0x64 || *code == 0x65 || *code == 0x67)
	{
		if (*code == 0xF3)
			scalar = TRUE;
		code++;
	}
	if (*code >= 0xD8 && *code <= 0xDF)
		steps[_step_x87]++;
	else if (*code == 0xE8)
		steps[_step_call]++;
	else if (*code == 0xFF && ((code[1] >> 3) & 7) == 4)
		steps[_step_indirect_jump]++;
	else if (*code == 0xFF && ((code[1] >> 3) & 7) == 2)
		steps[_step_call]++;
	else if (*code == 0x0F)
	{
		unsigned char op = code[1];
		BOOL memory = (code[2] & 0xC0) != 0xC0;

		if (scalar)
		{
			if ((op >= 0x10 && op <= 0x11) || (op >= 0x51 && op <= 0x5F) || op == 0x2A || op == 0x2C || op == 0x2D ||
				op == 0xC2)
			{
				if (op == 0x11 && memory)
					steps[_step_half_store]++;
				else
					steps[_step_scalar_sse]++;
			}
			return;
		}
		switch (op)
		{
		case 0x28: steps[memory ? _step_packed_load : _step_shuffle]++; break;
		case 0x29: steps[memory ? _step_packed_store : _step_shuffle]++; break;
		case 0x10: steps[memory ? _step_packed_load : _step_shuffle]++; break;
		case 0x11: steps[_step_packed_store]++; break;
		case 0x12: case 0x16: steps[memory ? _step_packed_load : _step_shuffle]++; break;
		case 0x13: case 0x17: steps[_step_half_store]++; break;
		case 0x14: case 0x15: case 0xC6: steps[_step_shuffle]++; break;
		case 0x5E: case 0x51: steps[_step_divide]++; break;
		case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: case 0x59: case 0x5C:
		case 0x5D: case 0x5F: case 0xC2:
			steps[memory ? _step_packed_memory : _step_packed_register]++;
			break;
		case 0x2E: case 0x2F: case 0x50: steps[_step_scalar_sse]++; break;
		default: break;
		}
	}
}

static long __stdcall step_handler(struct step_exception_pointers *pointers)
{
	/* EXCEPTION_SINGLE_STEP; anything else is not this handler's */
	if (pointers->record[0] != 0x80000004)
		return 0;
	if (stepping)
	{
		step_classify(*(const unsigned char **)(pointers->context + STEP_CONTEXT_EIP));
		*(DWORD *)(pointers->context + STEP_CONTEXT_EFLAGS) |= STEP_TRAP_FLAG;
	}
	/* EXCEPTION_CONTINUE_EXECUTION */
	return -1;
}

/* the last trap arrives inside */
static __attribute__((noinline)) void step_stop(void)
{
	stepping = 0;
}

static void step_run(struct nv2a_vsh_cpu_program *program, const struct nv2a_vsh_cpu_draw *draw, unsigned long first,
	unsigned long count, BOOL vector)
{
	memset(steps, 0, sizeof(steps));
	stepping = 1;
	__asm__ volatile("pushfl; orl $0x100, (%esp); popfl");
	if (vector)
		nv2a_vsh_cpu_run(program, draw, first, count);
	else
		nv2a_vsh_cpu_run_scalar(program, draw, first, count);
	step_stop();
}

/* KERNEL32's, declared here because the game's headers are the Xbox's;
the handler list is Windows XP's and later (the count runs on the PC) */
__declspec(dllimport) void *__stdcall GetModuleHandleA(const char *name);
__declspec(dllimport) void *__stdcall GetProcAddress(void *module, const char *name);

static int command_count(void)
{
	unsigned int case_count, case_index, layout_index, mode, kind;
	void *(__stdcall *add_handler)(unsigned long first, long (__stdcall *handler)(struct step_exception_pointers *)) =
		(void *)GetProcAddress(GetModuleHandleA("KERNEL32"), "AddVectoredExceptionHandler");

	if (!add_handler)
	{
		printf("# FAIL this Windows has no AddVectoredExceptionHandler\n");
		return 2;
	}
	add_handler(1, step_handler);
	if (read_word() != CASE_MAGIC)
		return 2;
	case_count = read_word();
	for (case_index = 0; case_index < case_count; case_index++)
	{
		struct test_case test;
		struct nv2a_vsh_cpu_program *program;
		unsigned char *records;

		read_case(&test);
		program = nv2a_vsh_cpu_program_new(test.words, test.instruction_count);
		records = malloc(test.vertex_count * RECORD_BYTES + 1);
		for (layout_index = 0; layout_index < test.layout_count; layout_index++)
		{
			struct nv2a_vsh_cpu_draw draw;

			make_draw(&test, &test.layouts[layout_index], records, &draw);
			for (mode = 0; mode < 2; mode++)
			{
				/* once to reduce the program; then counted for a quarter of
				the vertices and for all of them, which tells the cost of a
				vertex from the cost of a call */
				if (mode)
					nv2a_vsh_cpu_run(program, &draw, test.first, test.vertex_count);
				else
					nv2a_vsh_cpu_run_scalar(program, &draw, test.first, test.vertex_count);
				printf("%u %u %s %u", case_index, layout_index, mode ? "sse" : "scalar", test.vertex_count);
				step_run(program, &draw, test.first, test.vertex_count / 4, mode);
				for (kind = 0; kind < NUMBER_OF_STEP_KINDS; kind++)
					printf(" %lu", steps[kind]);
				step_run(program, &draw, test.first, test.vertex_count, mode);
				for (kind = 0; kind < NUMBER_OF_STEP_KINDS; kind++)
					printf(" %lu", steps[kind]);
				printf("\n");
			}
			fflush(stdout);
		}
		free(records);
		nv2a_vsh_cpu_program_delete(program);
		free_case(&test);
	}
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 3)
	{
		printf("usage: t_vsh_cpu run CASES RESULTS | classes CASES | bench CASES | count CASES\n");
		return 2;
	}
	input = fopen(argv[2], "rb");
	if (!input)
	{
		printf("# FAIL cannot read %s\n", argv[2]);
		return 2;
	}
	if (!strcmp(argv[1], "run") && argc >= 4)
		return command_run(argv[3]);
	if (!strcmp(argv[1], "classes"))
		return command_classes();
	if (!strcmp(argv[1], "bench"))
		return command_bench();
	if (!strcmp(argv[1], "count"))
		return command_count();
	return 2;
}
