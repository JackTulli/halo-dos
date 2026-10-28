/*
T_PICTURE.C

The light renderer's picture, checked pixel by pixel without a display.

t_fixed_device.c's harness is included as it is (its recorder, its platform
layer, the game's vertex programs and pixel states); on top of it:
- t_picture_gl.h: a software OpenGL 1.1 that draws what the device asks
  OpenGL for into a window's worth of pixels;
- t_picture_ref.h: the same draws as the Xbox would make them, from the
  Direct3D state alone.
The two pictures are compared. Where the device's plan for a pixel state is
exact they must agree to rounding; where it is approximate the difference is
reported.

	t_picture <driver> <extensions> [16|32] [-v] [-d folder] [-only name]

-d writes a PNG (reference | light renderer | difference x8) for every
comparison that fails, and for the named tests.
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

/* what the harness's helpers tell the device is noted for the reference too */
static void WINAPI pt_SetVertexShaderConstant(INT reg, CONST void *constant_data, DWORD constant_count);
static void WINAPI pt_SetStreamSource(UINT stream, D3DVertexBuffer *buffer, UINT stride);
static void WINAPI pt_SetVertexShader(DWORD handle);
static void WINAPI pt_SetTexture(DWORD stage, D3DBaseTexture *texture);
static void WINAPI pt_SetPalette(DWORD stage, D3DPalette *palette);
static void WINAPI pt_SetViewport(CONST D3DVIEWPORT8 *viewport);

#define D3DDevice_SetVertexShaderConstant pt_SetVertexShaderConstant
#define D3DDevice_SetStreamSource pt_SetStreamSource
#define D3DDevice_SetVertexShader pt_SetVertexShader
#define D3DDevice_SetTexture pt_SetTexture
#define D3DDevice_SetPalette pt_SetPalette
#define D3DDevice_SetViewport pt_SetViewport
#define main harness_main
#define SDL_GL_GetProcAddress harness_proc_address
#include "t_fixed_device.c"
#undef main
#undef SDL_GL_GetProcAddress
#undef D3DDevice_SetVertexShaderConstant
#undef D3DDevice_SetStreamSource
#undef D3DDevice_SetVertexShader
#undef D3DDevice_SetTexture
#undef D3DDevice_SetPalette
#undef D3DDevice_SetViewport

#include "t_picture_gl.h"
#include "t_picture_ref.h"

function_pointer SDL_GL_GetProcAddress(const char *name)
{
	function_pointer found = harness_proc_address(name);
	unsigned long index;

	if (!found)
		return NULL;
	for (index = 0; index < sizeof(sw_entry_points) / sizeof(sw_entry_points[0]); index++)
	{
		if (!strcmp(sw_entry_points[index].name, name))
			return sw_entry_points[index].function;
	}
	return found;
}

/* ---------- the wrappers */

static void WINAPI pt_SetVertexShaderConstant(INT reg, CONST void *constant_data, DWORD constant_count)
{
	long first = reg + XGPU_VERTEX_CONSTANT_BIAS;

	if (first >= 0 && first + (long)constant_count <= XGPU_VERTEX_CONSTANT_COUNT)
		memcpy(pt_constants[first], constant_data, constant_count * 4 * sizeof(float));
	D3DDevice_SetVertexShaderConstant(reg, constant_data, constant_count);
}

static void WINAPI pt_SetStreamSource(UINT stream, D3DVertexBuffer *buffer, UINT stride)
{
	if (stream < 4)
	{
		pt_streams[stream].data = buffer && buffer->Data ? PLATFORM_PHYSICAL_TO_VIRTUAL(buffer->Data) : NULL;
		pt_streams[stream].stride = stride;
	}
	D3DDevice_SetStreamSource(stream, buffer, stride);
}

static void WINAPI pt_SetVertexShader(DWORD handle)
{
	int index;

	for (index = 0; index < NUMBER_OF_VERTEX_SHADERS; index++)
	{
		if (vertex_shaders[index] == handle)
			pt_program = index;
	}
	D3DDevice_SetVertexShader(handle);
}

static void WINAPI pt_SetTexture(DWORD stage, D3DBaseTexture *texture)
{
	if (stage < 4)
		pt_textures[stage] = texture;
	D3DDevice_SetTexture(stage, texture);
}

static void WINAPI pt_SetPalette(DWORD stage, D3DPalette *palette)
{
	if (stage < 4)
		pt_palettes[stage] = palette;
	D3DDevice_SetPalette(stage, palette);
}

static void WINAPI pt_SetViewport(CONST D3DVIEWPORT8 *viewport)
{
	pt_viewport = *viewport;
	D3DDevice_SetViewport(viewport);
}

static unsigned long pt_back_buffer_data;

static void pt_clear(DWORD count, const D3DRECT *rectangles, DWORD flags, D3DCOLOR color, float z)
{
	D3DDevice_Clear(count, rectangles, flags, color, z, 0);
	ref_clear(count, rectangles, flags, color, z);
}

static void pt_draw(D3DPRIMITIVETYPE type, unsigned long first, unsigned long count)
{
	begin_draw();
	D3DDevice_DrawVertices(type, first, count);
	ref_draw(type, first, count, NULL, NULL, pt_back_buffer_data);
}

static void pt_draw_indexed(D3DPRIMITIVETYPE type, unsigned long count, const unsigned short *indices)
{
	begin_draw();
	D3DDevice_DrawIndexedVertices(type, count, indices);
	ref_draw(type, 0, count, indices, NULL, pt_back_buffer_data);
}

/* immediate mode: every register of each vertex (sixteen of four floats) */
static void pt_draw_immediate(D3DPRIMITIVETYPE type, unsigned long count, const float *vertices)
{
	unsigned long vertex;
	int reg;

	begin_draw();
	D3DDevice_Begin(type);
	for (vertex = 0; vertex < count; vertex++)
	{
		const float *v = vertices + vertex * 64;

		for (reg = 15; reg >= 0; reg--)
			D3DDevice_SetVertexData4f(reg, v[reg * 4], v[reg * 4 + 1], v[reg * 4 + 2], v[reg * 4 + 3]);
	}
	D3DDevice_End();
	memcpy(pt_attributes, vertices + (count - 1) * 64, sizeof(pt_attributes));
	ref_draw(type, 0, count, NULL, vertices, pt_back_buffer_data);
}

/* ---------- PNG (stored, no compression) */

static const char *dump_folder;

static unsigned long png_crc(unsigned long crc, const unsigned char *data, size_t size)
{
	static unsigned long table[256];
	size_t index;
	int bit;

	if (!table[1])
	{
		for (index = 0; index < 256; index++)
		{
			unsigned long value = (unsigned long)index;

			for (bit = 0; bit < 8; bit++)
				value = value & 1 ? 0xedb88320UL ^ (value >> 1) : value >> 1;
			table[index] = value;
		}
	}
	for (index = 0; index < size; index++)
		crc = table[(crc ^ data[index]) & 0xff] ^ (crc >> 8);
	return crc;
}

static void png_chunk(FILE *file, const char *type, const unsigned char *data, size_t size)
{
	unsigned char header[8];
	unsigned long crc;

	header[0] = (unsigned char)(size >> 24); header[1] = (unsigned char)(size >> 16); header[2] = (unsigned char)(size >> 8);
	header[3] = (unsigned char)size;
	memcpy(header + 4, type, 4);
	fwrite(header, 1, 8, file);
	if (size)
		fwrite(data, 1, size, file);
	crc = png_crc(0xffffffffUL, header + 4, 4);
	crc = png_crc(crc, data, size) ^ 0xffffffffUL;
	header[0] = (unsigned char)(crc >> 24); header[1] = (unsigned char)(crc >> 16); header[2] = (unsigned char)(crc >> 8);
	header[3] = (unsigned char)crc;
	fwrite(header, 1, 4, file);
}

static void png_write(const char *path, const unsigned char *rgb, int width, int height)
{
	static const unsigned char signature[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
	size_t row_bytes = (size_t)width * 3 + 1, raw_size = row_bytes * height, position = 0, out = 0;
	unsigned char *raw = malloc(raw_size), *stream = malloc(raw_size + raw_size / 65535 * 5 + 64), header[13];
	unsigned long a = 1, b = 0;
	FILE *file = fopen(path, "wb");
	int y;

	if (!file)
	{
		free(raw); free(stream);
		return;
	}
	for (y = 0; y < height; y++)
	{
		raw[y * row_bytes] = 0;
		memcpy(raw + y * row_bytes + 1, rgb + (size_t)y * width * 3, (size_t)width * 3);
	}
	stream[out++] = 0x78;
	stream[out++] = 0x01;
	while (position < raw_size)
	{
		size_t block = raw_size - position > 65535 ? 65535 : raw_size - position;

		stream[out++] = position + block == raw_size;
		stream[out++] = (unsigned char)block; stream[out++] = (unsigned char)(block >> 8);
		stream[out++] = (unsigned char)~block; stream[out++] = (unsigned char)(~block >> 8);
		memcpy(stream + out, raw + position, block);
		out += block;
		position += block;
	}
	for (position = 0; position < raw_size; position++)
	{
		a = (a + raw[position]) % 65521;
		b = (b + a) % 65521;
	}
	stream[out++] = (unsigned char)(b >> 8); stream[out++] = (unsigned char)b;
	stream[out++] = (unsigned char)(a >> 8); stream[out++] = (unsigned char)a;
	fwrite(signature, 1, 8, file);
	header[0] = (unsigned char)(width >> 24); header[1] = (unsigned char)(width >> 16); header[2] = (unsigned char)(width >> 8);
	header[3] = (unsigned char)width;
	header[4] = (unsigned char)(height >> 24); header[5] = (unsigned char)(height >> 16); header[6] = (unsigned char)(height >> 8);
	header[7] = (unsigned char)height;
	header[8] = 8; header[9] = 2; header[10] = header[11] = header[12] = 0;
	png_chunk(file, "IHDR", header, 13);
	png_chunk(file, "IDAT", stream, out);
	png_chunk(file, "IEND", NULL, 0);
	fclose(file);
	free(raw);
	free(stream);
}

/* ---------- comparing the two pictures */

struct pt_result
{
	unsigned long pixels, differing, forgiven;
	int worst;
	double mean;
	int first_x, first_y;
};

/* a rectangle of the window (rows from the top), or all of it with a width of 0 */
static void pt_compare_pictures(int x0, int y0, int width, int height, int tolerance, struct pt_result *result)
{
	int x, y, channel, channels = driver.alpha_bits ? 4 : 3;
	double sum = 0.0;

	memset(result, 0, sizeof(*result));
	result->first_x = result->first_y = -1;
	if (!width)
	{
		x0 = y0 = 0;
		width = ref.width;
		height = ref.height;
	}
	for (y = y0; y < y0 + height && y < ref.height; y++)
	{
		for (x = x0; x < x0 + width && x < ref.width; x++)
		{
			const unsigned char *wanted = ref.color + ((size_t)y * ref.width + x) * 4;
			const unsigned char *drawn = sw.color + ((size_t)(sw.height - 1 - y) * sw.width + x) * 4;
			int worst = 0;

			for (channel = 0; channel < channels; channel++)
			{
				int difference = abs((int)wanted[channel] - (int)drawn[channel]);

				if (difference > worst)
					worst = difference;
			}
			result->pixels++;
			if (ref.edge[(size_t)y * ref.width + x])
			{
				if (worst > tolerance)
					result->forgiven++;
				continue;
			}
			sum += worst;
			if (worst > result->worst)
				result->worst = worst;
			if (worst > tolerance)
			{
				if (!result->differing)
				{
					result->first_x = x;
					result->first_y = y;
				}
				result->differing++;
			}
		}
	}
	result->mean = result->pixels ? sum / result->pixels : 0.0;
}

static void pt_dump(const char *name)
{
	unsigned char *rgb;
	char path[512], clean[128];
	int x, y, channel, index;

	if (!dump_folder)
		return;
	for (index = 0; name[index] && index < 100; index++)
		clean[index] = (name[index] >= 'a' && name[index] <= 'z') || (name[index] >= 'A' && name[index] <= 'Z') ||
			(name[index] >= '0' && name[index] <= '9') ? name[index] : '_';
	clean[index] = 0;
	rgb = malloc((size_t)ref.width * 3 * ref.height * 3);
	for (y = 0; y < ref.height; y++)
	{
		for (x = 0; x < ref.width; x++)
		{
			const unsigned char *wanted = ref.color + ((size_t)y * ref.width + x) * 4;
			const unsigned char *drawn = sw.color + ((size_t)(sw.height - 1 - y) * sw.width + x) * 4;
			unsigned char *row = rgb + (size_t)y * ref.width * 9;

			for (channel = 0; channel < 3; channel++)
			{
				int difference = abs((int)wanted[channel] - (int)drawn[channel]) * 8;

				row[x * 3 + channel] = wanted[channel];
				row[(ref.width + x) * 3 + channel] = drawn[channel];
				row[(2 * ref.width + x) * 3 + channel] = (unsigned char)(difference > 255 ? 255 : difference);
			}
		}
	}
	snprintf(path, sizeof(path), "%s/%s_%s_%s.png", dump_folder, driver.name, texture_bits_setting, clean);
	png_write(path, rgb, ref.width * 3, ref.height);
	free(rgb);
}

/* the pictures must agree: at most `allowed` pixels differ by more than the tolerance */
static int pt_expect_same(const char *name, int tolerance, unsigned long allowed)
{
	struct pt_result result;

	pt_compare_pictures(0, 0, 0, 0, tolerance, &result);
	if (verbose)
	{
		printf("# %s: %lu pixels, worst difference %d, mean %.2f, %lu on edges forgiven; %lu drawn by the reference, %lu by OpenGL\n",
			name, result.pixels, result.worst, result.mean, result.forgiven, ref.fragments, sw.pixels_drawn);
	}
	if (result.differing > allowed)
	{
		const unsigned char *wanted = ref.color + ((size_t)result.first_y * ref.width + result.first_x) * 4;
		const unsigned char *drawn = sw.color + ((size_t)(sw.height - 1 - result.first_y) * sw.width + result.first_x) * 4;

		fail("%s: %lu of %lu pixels differ by more than %d (worst %d, mean %.2f); the first at %d,%d: the Xbox's %d %d %d %d, "
			"the light renderer's %d %d %d %d; %lu fragments by the reference, %lu by OpenGL", name, result.differing, result.pixels,
			tolerance, result.worst, result.mean, result.first_x, result.first_y, wanted[0], wanted[1], wanted[2], wanted[3],
			drawn[0], drawn[1], drawn[2], drawn[3], ref.fragments, sw.pixels_drawn);
		pt_dump(name);
		return 0;
	}
	return 1;
}

static void pt_counts_reset(void)
{
	ref.fragments = 0;
	sw.pixels_drawn = 0;
	sw.triangles_drawn = 0;
}

/* the tolerance of a comparison of textured pixels: the 16-bit texel formats keep four or five bits a channel */
static int pt_texture_tolerance(void)
{
	return !strcmp(texture_bits_setting, "32") ? 3 : 20;
}

/* ---------- memory with a page behind it that cannot be touched

guard_malloc and its kin stand in for the C library's in the device's units
when they are compiled with t_picture_guard_alloc.h (tools/t_picture.sh guard): a block ends at a page boundary
and the page after it has no access, so that reading or writing past the end
of an array stops the test at once. guest_alloc_guarded does the same for
what the test gives the device in the Xbox's memory (streams, indices,
texels, palettes). */

/* (Windows' own, not the Xbox's the game's headers rename them to) */
#undef VirtualProtect
#undef VirtualFree
#undef SetUnhandledExceptionFilter
BOOL __stdcall VirtualProtect(void *address, unsigned long size, unsigned long protect, unsigned long *old);
BOOL __stdcall VirtualFree(void *address, unsigned long size, unsigned long type);
void *__stdcall SetUnhandledExceptionFilter(void *filter);

struct guard_block
{
	void *block, *base;
	size_t size;
};

static struct guard_block *guard_blocks;
static unsigned long guard_block_count, guard_block_capacity;
static unsigned long guard_allocations;

void *guard_malloc(size_t size)
{
	size_t pages = (size + 4095) / 4096 + 1;
	unsigned char *base = VirtualAlloc(NULL, (unsigned long)(pages * 4096), 0x3000, 4);
	unsigned long old = 0, index;
	void *block;

	if (!base)
		return NULL;
	VirtualProtect(base + (pages - 1) * 4096, 4096, 1 /* PAGE_NOACCESS */, &old);
	/* (floats and words: the block's end is within three bytes of the page's) */
	block = base + (pages - 1) * 4096 - ((size + 3) & ~(size_t)3);
	for (index = 0; index < guard_block_count && guard_blocks[index].block; index++)
		;
	if (index == guard_block_capacity)
	{
		guard_block_capacity = guard_block_capacity ? guard_block_capacity * 2 : 1024;
		guard_blocks = (realloc)(guard_blocks, guard_block_capacity * sizeof(*guard_blocks));
	}
	if (index == guard_block_count)
		guard_block_count++;
	guard_blocks[index].block = block;
	guard_blocks[index].base = base;
	guard_blocks[index].size = size;
	guard_allocations++;
	return block;
}

void *guard_calloc(size_t count, size_t size)
{
	/* (fresh pages are zero) */
	return guard_malloc(count * size);
}

static struct guard_block *guard_find(void *block)
{
	unsigned long index;

	for (index = 0; index < guard_block_count; index++)
	{
		if (guard_blocks[index].block == block)
			return &guard_blocks[index];
	}
	printf("FAIL a block is freed or grown that was not allocated: %p\n", block);
	exit(97);
}

void guard_free(void *block)
{
	struct guard_block *found;

	if (!block)
		return;
	found = guard_find(block);
	VirtualFree(found->base, 0, 0x8000 /* MEM_RELEASE */);
	found->block = NULL;
}

void *guard_realloc(void *block, size_t size)
{
	void *grown = guard_malloc(size);

	if (block && grown)
	{
		size_t old_size = guard_find(block)->size;

		memcpy(grown, block, old_size < size ? old_size : size);
		guard_free(block);
	}
	return grown;
}

/* size bytes of the Xbox's memory whose end is a page's, the next page without access */
static void *guest_alloc_guarded(unsigned long size)
{
	unsigned long rounded = (size + 4095) & ~4095UL, old = 0;
	unsigned char *memory = guest_alloc(rounded + 4096);

	VirtualProtect(memory + rounded, 4096, 1, &old);
	return memory + rounded - size;
}

struct exception_pointers
{
	struct
	{
		unsigned long code, flags;
		void *record, *address;
		unsigned long parameter_count;
		unsigned long parameters[15];
	} *record;
	void *context;
};

static long __stdcall guard_exception(struct exception_pointers *exception)
{
	printf("FAIL [%s] exception %08lx at code address %p%s%p: memory was touched that the draw has no business with\n", test_name,
		exception->record->code, exception->record->address,
		exception->record->code == 0xc0000005UL ? (exception->record->parameters[0] ? ", writing " : ", reading ") : " ",
		exception->record->code == 0xc0000005UL ? (void *)exception->record->parameters[1] : NULL);
	fflush(stdout);
	exit(98);
	return 0;
}

#include "t_picture_tests.h"

int main(int argc, char **argv)
{
	D3DPRESENT_PARAMETERS parameters;
	D3DDevice *device = NULL;
	D3DSurface *back_buffer = NULL;
	const char *only = NULL;
	unsigned long stage_format = 0x06;
	int argument, index, plain = 0;

	setvbuf(stdout, NULL, _IONBF, 0);
	SetUnhandledExceptionFilter(guard_exception);
	driver = drivers[0];
	for (argument = 1; argument < argc; argument++)
	{
		if (!strcmp(argv[argument], "-v"))
			verbose = 1;
		else if (!strcmp(argv[argument], "-d") && argument + 1 < argc)
			dump_folder = argv[++argument];
		else if (!strcmp(argv[argument], "-only") && argument + 1 < argc)
			only = argv[++argument];
		else if (!strcmp(argv[argument], "-stagefmt") && argument + 1 < argc)
			stage_format = strtoul(argv[++argument], NULL, 16);
		else if (!strcmp(argv[argument], "16") || !strcmp(argv[argument], "32"))
			texture_bits_setting = argv[argument];
		else if (!plain)
		{
			for (index = 0; index < 3; index++)
			{
				if (!strcmp(drivers[index].name, argv[argument]))
					driver = drivers[index];
			}
			plain = 1;
		}
		else
			extensions_setting = argv[argument];
	}
	allowed_compute();
	expected_capabilities_compute();
	printf("# driver %s (OpenGL %s), debug.gl1_extensions \"%s\", %s-bit textures: %d unit%s%s%s%s\n", driver.name,
		driver.version, extensions_setting, texture_bits_setting, allowed.units, allowed.units == 1 ? "" : "s",
		allowed.combine ? ", combine" : "", allowed.combine_arb ? " (ARB)" : "", allowed.env_add ? ", add" : "");

	rec_reset_state();
	rec.window_width = 640;
	rec.window_height = 480;
	guest_initialize();
	streams_initialize();
	for (index = 0; index < 16; index++)
		pt_attributes[index][3] = 1.0f;

	memset(&parameters, 0, sizeof(parameters));
	parameters.BackBufferWidth = 640;
	parameters.BackBufferHeight = 480;
	Direct3D_CreateDevice(0, D3DDEVTYPE_HAL, NULL, 0, &parameters, &device);
	if (!log_count("renderer: gl1") || !log_count("gl1: OpenGL"))
	{
		printf("FAIL the device did not start the light renderer:\n%s", log_text);
		return 1;
	}
	D3DDevice_GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &back_buffer);
	pt_back_buffer_data = back_buffer ? back_buffer->Data : 0;
	vertex_shaders_create();
	for (index = 0; index < 4; index++)
		stage_textures[index] = texture_new(stage_format, (unsigned long)stage_texture_size[index],
			(unsigned long)stage_texture_size[index], 1, 1, FALSE);
	cube_texture = texture_new(0x06, 16, 16, 1, 1, TRUE);
	volume_texture = texture_new(0x06, 16, 16, 16, 1, FALSE);
	for (index = 0; index < 16; index++)
		D3DDevice_SetVertexData4f(index, 0.0f, 0.0f, 0.0f, 1.0f);

	pt_run_tests(only);

	test_name = "end";
	CHECK(!rec.state.locked, "the arrays are left locked");
	if (failures)
	{
		const char *line = log_text;
		int lines = 0;

		printf("# the device's log (first lines):\n");
		while (*line && lines++ < 40)
		{
			printf("#   %.*s\n", (int)strcspn(line, "\n") > 300 ? 300 : (int)strcspn(line, "\n"), line);
			line += strcspn(line, "\n");
			if (*line)
				line++;
		}
	}
	printf("%s: %d failure%s\n", failures ? "FAILED" : "passed", failures, failures == 1 ? "" : "s");
	return failures > 255 ? 255 : failures;
}
