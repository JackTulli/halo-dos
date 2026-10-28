/*
T_BENCH.C

The light renderer's processor cost for a frame of the game's kinds of
draws, without a display: t_fixed_device.c's harness is included as it is
(its recorder stands in for the driver, with the draws themselves replaced
by counters), and a frame of about 650 draws is made over and over:

- the environment through the light pipeline: lightmap x base map draws
  (vs16) over ranges of one shared vertex buffer and lightmap stream, some
  of the ranges drawn again for self-illumination (the same geometry, a
  second Direct3D draw), and the fog pass (vs06) over some of them;
- models through vs10 and vs17 (skinned), with a palette of node matrices
  that changes with every model;
- particles through vs65 (quads), decals through vs01;
- HUD quads through vs04.

	t_bench <driver> <extensions> [16|32] [frames] [-s]

prints the frame's processor time (median, lowest, highest, in
milliseconds), and with -s the device's own split of it (debug.gpu_stats,
logged every five seconds of its clock, which runs real time here).

Built by tools/t_bench.sh (the commands are the Windows build's).
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

/* the harness's clock runs a microsecond a call; the benchmark wants the
real one */
#define clock_gettime harness_clock_gettime
#define config_integer harness_config_integer
#define main harness_main
#include "t_fixed_device.c"
#undef main
#undef clock_gettime
#undef config_integer

/* -sites traces the device's draws (debug.gpu_trace_frame) */
static int trace;

long config_integer(const char *name)
{
	if (trace && !strcmp(name, "debug.gpu_trace_frame"))
		return 1;
	return harness_config_integer(name);
}

/* (the game's own names for these are shims; kernel32's by their decorated names) */
int __stdcall bench_counter(long long *count) __asm__("_QueryPerformanceCounter@4");
int __stdcall bench_frequency(long long *frequency) __asm__("_QueryPerformanceFrequency@4");

static long long clock_frequency;

int clock_gettime(clockid_t clock_id, struct timespec *time)
{
	long long count;

	(void)clock_id;
	if (!clock_frequency)
		bench_frequency(&clock_frequency);
	bench_counter(&count);
	time->tv_sec = (long)(count / clock_frequency);
	time->tv_nsec = (long)((count % clock_frequency) * 1000000000LL / clock_frequency);
	return 0;
}

static double now_milliseconds(void)
{
	struct timespec time;

	clock_gettime(CLOCK_MONOTONIC, &time);
	return (double)time.tv_sec * 1000.0 + (double)time.tv_nsec / 1000000.0;
}

/* ---------- the draws, counted instead of checked */

static struct
{
	unsigned long draws, vertices, indexed, arrays;
} counted;

static void APIENTRY count_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices)
{
	(void)mode; (void)type; (void)indices;
	counted.draws++;
	counted.indexed++;
	counted.vertices += (unsigned long)count;
}

static void APIENTRY count_glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
	(void)mode; (void)first;
	counted.draws++;
	counted.arrays++;
	counted.vertices += (unsigned long)count;
}

/* ---------- the frame */

#define ENVIRONMENT_VERTICES 20000
#define ENVIRONMENT_SURFACES 150
#define SURFACE_VERTICES 200
#define SURFACE_TRIANGLES 100
#define SPARSE_RANGE 1200
#define SPARSE_USED 60
#define MODEL_VERTICES 150
#define MODEL_TRIANGLES 100

static struct
{
	unsigned char *environment, *lightmap, *models, *particles, *hud_vertices, *decals;
	D3DVertexBuffer environment_buffer, lightmap_buffer, model_buffer, particle_buffer, hud_buffer, decal_buffer;
	unsigned short *surface_indices[ENVIRONMENT_SURFACES];
	unsigned short *model_indices;
	/* (site_find hands out one static site: copies) */
	struct site fused, illumination, fog, model, particle, hud, decal;
	unsigned long d3d_draws, frames;
} bench;

static unsigned long bench_random_state = 12345;

static unsigned long bench_random(unsigned long range)
{
	bench_random_state = bench_random_state * 1664525UL + 1013904223UL;
	return (bench_random_state >> 8) % range;
}

static float bench_random_float(float low, float high)
{
	return low + (float)bench_random(10000) * (1.0f / 10000.0f) * (high - low);
}

static void buffer_set(D3DVertexBuffer *buffer, unsigned char *data)
{
	buffer->Common = 1;
	buffer->Data = PLATFORM_VIRTUAL_TO_PHYSICAL(data);
}

static void bench_initialize(void)
{
	unsigned long index, surface;

	bench.environment = guest_alloc(ENVIRONMENT_VERTICES * sizeof(struct environment_vertex));
	bench.lightmap = guest_alloc(ENVIRONMENT_VERTICES * sizeof(struct lightmap_vertex));
	bench.models = guest_alloc(MODEL_VERTICES * sizeof(struct model_vertex));
	bench.particles = guest_alloc(64 * sizeof(struct screen_vertex));
	bench.hud_vertices = guest_alloc(64 * sizeof(struct hud_vertex));
	bench.decals = guest_alloc(64 * sizeof(struct screen_vertex));
	buffer_set(&bench.environment_buffer, bench.environment);
	buffer_set(&bench.lightmap_buffer, bench.lightmap);
	buffer_set(&bench.model_buffer, bench.models);
	buffer_set(&bench.particle_buffer, bench.particles);
	buffer_set(&bench.hud_buffer, bench.hud_vertices);
	buffer_set(&bench.decal_buffer, bench.decals);

	/* the environment: a cloud of vertices in front of the camera */
	for (index = 0; index < ENVIRONMENT_VERTICES; index++)
	{
		struct environment_vertex *vertex = (struct environment_vertex *)bench.environment + index;
		struct lightmap_vertex *light = (struct lightmap_vertex *)bench.lightmap + index;

		vertex->position[0] = bench_random_float(-20.0f, 20.0f);
		vertex->position[1] = bench_random_float(-5.0f, 5.0f);
		vertex->position[2] = bench_random_float(2.0f, 80.0f);
		vertex->normal = 0x000003ffUL;
		vertex->binormal = 0x001ff800UL;
		vertex->tangent = 0x7fc00000UL;
		vertex->coordinates[0] = bench_random_float(0.0f, 4.0f);
		vertex->coordinates[1] = bench_random_float(0.0f, 4.0f);
		light->incident = 0x000003ffUL;
		light->coordinates[0] = (short)bench_random(32767);
		light->coordinates[1] = (short)bench_random(32767);
	}
	/* each surface: SURFACE_TRIANGLES triangles over a range of
	SURFACE_VERTICES vertices of the shared buffer; every third surface is
	sparse, as a map's often are: a range of SPARSE_RANGE vertices of which
	SPARSE_USED are drawn */
	for (surface = 0; surface < ENVIRONMENT_SURFACES; surface++)
	{
		BOOL sparse = surface % 3 == 2;
		unsigned long range = sparse ? SPARSE_RANGE : SURFACE_VERTICES, used = sparse ? SPARSE_USED : SURFACE_VERTICES;
		unsigned long first = (surface * 131) % (ENVIRONMENT_VERTICES - range);
		unsigned short *indices = guest_alloc(SURFACE_TRIANGLES * 3 * sizeof(unsigned short));

		for (index = 0; index < SURFACE_TRIANGLES * 3; index++)
			indices[index] = (unsigned short)(first + bench_random(used) * (range / used));
		/* (the range's ends are used, as the game's are) */
		indices[0] = (unsigned short)first;
		indices[1] = (unsigned short)(first + range - 1);
		bench.surface_indices[surface] = indices;
	}
	for (index = 0; index < MODEL_VERTICES; index++)
	{
		struct model_vertex *vertex = (struct model_vertex *)bench.models + index;

		vertex->position[0] = bench_random_float(-0.5f, 0.5f);
		vertex->position[1] = bench_random_float(-0.5f, 0.5f);
		vertex->position[2] = bench_random_float(4.0f, 6.0f);
		vertex->normal = 0x7fc00000UL;
		vertex->binormal = 0x000003ffUL;
		vertex->tangent = 0x001ff800UL;
		vertex->coordinates[0] = (short)bench_random(32767);
		vertex->coordinates[1] = (short)bench_random(32767);
		vertex->nodes[0] = (unsigned char)((index % 3) * 3);
		vertex->nodes[1] = (unsigned char)(((index + 1) % 3) * 3);
		vertex->weight = (short)(32767 - bench_random(20000));
	}
	bench.model_indices = guest_alloc(MODEL_TRIANGLES * 3 * sizeof(unsigned short));
	for (index = 0; index < MODEL_TRIANGLES * 3; index++)
		bench.model_indices[index] = (unsigned short)bench_random(MODEL_VERTICES);
	for (index = 0; index < 64; index++)
	{
		struct screen_vertex *particle = (struct screen_vertex *)bench.particles + index;
		struct screen_vertex *decal = (struct screen_vertex *)bench.decals + index;
		struct hud_vertex *hud = (struct hud_vertex *)bench.hud_vertices + index;

		particle->position[0] = (float)(index % 4) - 1.5f;
		particle->position[1] = (float)(index / 4 % 4) - 1.5f;
		particle->position[2] = 4.0f + (float)(index % 9);
		particle->coordinates[0] = (float)(index & 1);
		particle->coordinates[1] = (float)((index >> 1) & 1);
		particle->color = 0x80c0e0ffUL - (DWORD)index * 0x01010101UL;
		*decal = *particle;
		hud->position[0] = (float)(index % 8) * 60.0f;
		hud->position[1] = (float)(index / 8) * 40.0f;
		hud->coordinates[0] = (float)(index & 1);
		hud->coordinates[1] = (float)((index >> 1) & 1);
		hud->color = 0xffe0d0c0UL;
	}

	bench.fused = *site_find("light pipeline, fused, lightmap");
	bench.illumination = *site_find("light pipeline, self illumination");
	bench.fog = *site_find("environment_fog.c:726 fog");
	bench.model = *site_find("models.c:1011");
	bench.particle = *site_find("transparent_geometry.c:1747 effect");
	bench.hud = *site_find("widgets.c:525 textured");
	bench.decal = *site_find("decals.c:682");
}

static void environment_draw(const struct site *site, int program, unsigned long surface)
{
	site_apply(site);
	D3D__RenderState[D3DRS_CULLMODE] = D3DCULL_CCW;
	D3D__RenderState[D3DRS_FRONTFACE] = D3DFRONT_CW;
	D3DDevice_SetVertexShader(vertex_shaders[program]);
	D3DDevice_SetStreamSource(0, &bench.environment_buffer, sizeof(struct environment_vertex));
	D3DDevice_SetStreamSource(1, &bench.lightmap_buffer, sizeof(struct lightmap_vertex));
	D3DDevice_DrawIndexedVertices(D3DPT_TRIANGLELIST, SURFACE_TRIANGLES * 3, bench.surface_indices[surface]);
	bench.d3d_draws++;
}

static void bench_frame(void)
{
	/* c[-84]: the map's scale; c[-83], c[-82]: the rows of the texture animation */
	static const float texture_constants[3][4] =
	{
		{ 1.0f, 1.0f, 1.0f, 1.0f }, { 1.0f, 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f, 0.0f },
	};
	D3DVIEWPORT8 viewport = { 0, 0, 640, 480, 0.0f, 1.0f };
	float palette[30][4];
	unsigned long index, surface;

	D3DDevice_Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
	constants_fill();
	constants_world();
	D3DDevice_SetVertexShaderConstant(-84, texture_constants, 3);
	D3DDevice_SetViewport(&viewport);

	/* the environment: every surface lit, the second pass over some, the
	fog pass over some (the light pipeline draws each with the lightmap
	pass's program; the fog pass has its own) */
	for (surface = 0; surface < ENVIRONMENT_SURFACES; surface++)
		environment_draw(&bench.fused, 16, surface);
	for (surface = 0; surface < ENVIRONMENT_SURFACES; surface += 3)
	{
		D3D__RenderState[D3DRS_ZFUNC] = D3DCMP_EQUAL;
		environment_draw(&bench.illumination, 16, surface);
	}
	D3D__RenderState[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
	for (surface = 1; surface < ENVIRONMENT_SURFACES; surface += 3)
		environment_draw(&bench.fog, 6, surface);
	D3D__RenderState[D3DRS_CULLMODE] = D3DCULL_NONE;

	/* the models: a palette of node matrices for each */
	site_apply(&bench.model);
	D3DDevice_SetStreamSource(0, &bench.model_buffer, sizeof(struct model_vertex));
	for (index = 0; index < 100; index++)
	{
		unsigned long node;

		for (node = 0; node < 30; node++)
		{
			palette[node][0] = node % 3 == 0 ? 1.0f : 0.01f * (float)(index % 7);
			palette[node][1] = node % 3 == 1 ? 1.0f : 0.02f;
			palette[node][2] = node % 3 == 2 ? 1.0f : 0.03f;
			palette[node][3] = 0.1f * (float)(node % 3) + 0.01f * (float)index;
		}
		D3DDevice_SetVertexShaderConstant(0, palette, 30);
		D3DDevice_SetVertexShader(vertex_shaders[index & 1 ? 17 : 10]);
		D3DDevice_DrawIndexedVertices(D3DPT_TRIANGLELIST, MODEL_TRIANGLES * 3, bench.model_indices);
		bench.d3d_draws++;
	}

	/* particles: eight quads each */
	site_apply(&bench.particle);
	D3DDevice_SetStreamSource(0, &bench.particle_buffer, sizeof(struct screen_vertex));
	D3DDevice_SetVertexShader(vertex_shaders[65]);
	for (index = 0; index < 100; index++)
	{
		D3DDevice_DrawVertices(D3DPT_QUADLIST, (index % 4) * 8, 32);
		bench.d3d_draws++;
	}

	/* decals */
	site_apply(&bench.decal);
	D3DDevice_SetStreamSource(0, &bench.decal_buffer, sizeof(struct screen_vertex));
	D3DDevice_SetVertexShader(vertex_shaders[1]);
	for (index = 0; index < 40; index++)
	{
		D3DDevice_DrawVertices(D3DPT_TRIANGLEFAN, (index % 15) * 4, 4);
		bench.d3d_draws++;
	}

	/* the HUD */
	constants_screen(640.0f, 480.0f);
	site_apply(&bench.hud);
	D3DDevice_SetStreamSource(0, &bench.hud_buffer, sizeof(struct hud_vertex));
	D3DDevice_SetVertexShader(vertex_shaders[4]);
	for (index = 0; index < 150; index++)
	{
		D3DDevice_DrawVertices(D3DPT_QUADLIST, (index % 15) * 4, 4);
		bench.d3d_draws++;
	}

	D3DDevice_Present(NULL, NULL, NULL, NULL);
	bench.frames++;
}

/* -sites: the light pipeline's pixel states, each drawn once through vs16 over a
surface and once through vs10 over the model, with the frame traced: what the
device makes of each (the plan, its passes, its primary colours) */
static void bench_sites_site(const struct site *site)
{
	if (!site->light)
		return;
	printf("== %s\n", site->name);
	log_length = 0;
	log_text[0] = 0;
	environment_draw(site, 16, 0);
	D3DDevice_SetStreamSource(1, NULL, 0);
	site_apply(site);
	D3DDevice_SetStreamSource(0, &bench.model_buffer, sizeof(struct model_vertex));
	D3DDevice_SetVertexShader(vertex_shaders[10]);
	D3DDevice_DrawIndexedVertices(D3DPT_TRIANGLELIST, MODEL_TRIANGLES * 3, bench.model_indices);
	printf("%s", log_text);
}

static int compare_doubles(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return x < y ? -1 : x > y ? 1 : 0;
}

int main(int argc, char **argv)
{
	D3DPRESENT_PARAMETERS parameters;
	D3DDevice *device = NULL;
	int argument, index, frames = 100;
	int sites = 0;
	double *times, start;

	setvbuf(stdout, NULL, _IONBF, 0);
	driver = drivers[0];
	for (argument = 1; argument < argc; argument++)
	{
		if (!strcmp(argv[argument], "-s"))
			statistics = 1;
		else if (!strcmp(argv[argument], "-v"))
			verbose = 1;
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
		else if (!strcmp(argv[argument], "-sites"))
			sites = 1;
		else if (atoi(argv[argument]) > 0)
			frames = atoi(argv[argument]);
	}
	allowed_compute();
	expected_capabilities_compute();
	printf("# driver %s (OpenGL %s), debug.gl1_extensions \"%s\", %s-bit textures: %d unit%s%s%s\n", driver.name,
		driver.version, extensions_setting, texture_bits_setting, allowed.units, allowed.units == 1 ? "" : "s",
		allowed.combine ? ", combine" : "", allowed.cva ? ", compiled vertex arrays" : "");

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
	/* the draws are counted, not checked: the recorder's checks read every
	vertex of every draw and would be most of the time measured */
	halo_glDrawElements = count_glDrawElements;
	halo_glDrawArrays = count_glDrawArrays;
	vertex_shaders_create();
	for (index = 0; index < 4; index++)
		stage_textures[index] = texture_new(0x06, (unsigned long)stage_texture_size[index],
			(unsigned long)stage_texture_size[index], 1, 1, FALSE);
	cube_texture = texture_new(0x06, 16, 16, 1, 1, TRUE);
	volume_texture = texture_new(0x06, 16, 16, 16, 1, FALSE);
	bench_initialize();

	if (sites)
	{
		/* (the device reads the frame to trace at its first draw: the one after
		the warm-up frame) */
		trace = 1;
		bench_frame();
		sites_enumerate(bench_sites_site);
		return 0;
	}
	/* a warm-up frame makes the plans and the programs' variants */
	bench_frame();
	bench_frame();
	memset(&counted, 0, sizeof(counted));
	bench.d3d_draws = 0;
	times = malloc((size_t)frames * sizeof(double));
	for (index = 0; index < frames; index++)
	{
		start = now_milliseconds();
		bench_frame();
		times[index] = now_milliseconds() - start;
	}
	printf("# %lu Direct3D draws a frame, %lu OpenGL draws (%lu indexed, %lu arrays), %lu vertices through them\n",
		bench.d3d_draws / (unsigned long)frames, counted.draws / (unsigned long)frames, counted.indexed / (unsigned long)frames,
		counted.arrays / (unsigned long)frames, counted.vertices / (unsigned long)frames);
	/* (the device logs its statistics every few seconds of its clock: frames
	until it has) */
	if (statistics)
	{
		unsigned long extra = 0;

		while (!strstr(log_text, " ms a frame") && extra++ < 5000)
			bench_frame();
	}
	qsort(times, (size_t)frames, sizeof(double), compare_doubles);
	printf("frame: %.2f ms median, %.2f lowest, %.2f highest over %d frames\n", times[frames / 2], times[0], times[frames - 1], frames);
	if (statistics || verbose)
	{
		const char *line = log_text;

		/* the context's line and the statistics ("N frames, ... ms a
		frame", "N draws, ... a frame") */
		while ((line = strstr(line, "gl1: ")) != NULL)
		{
			size_t length = strcspn(line, "\n");
			const char *frame = strstr(line, " a frame");

			if (verbose || !strncmp(line, "gl1: OpenGL", 11) || (frame && frame < line + length))
				printf("%.*s\n", (int)length, line);
			line += length;
		}
	}
	free(times);
	return 0;
}
