/*
T_PICTURE_TESTS.H

The comparisons (t_picture.c).
*/

static const char *pt_only;

static int pt_wanted(const char *name)
{
	return !pt_only || strstr(name, pt_only) != NULL;
}

#define PT_CLEAR_COLOR 0xff203040UL

static void pt_window(int width, int height)
{
	rec.window_width = width;
	rec.window_height = height;
	/* (the device measures the window after each presentation) */
	present();
	pt_clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, PT_CLEAR_COLOR, 1.0f);
}

/* a pixel state of the test's own: the first texture times the vertex colour, colour and alpha */
static void pt_state_texture_times_diffuse(struct site *out)
{
	memset(out, 0, sizeof(*out));
	strcpy(out->name, "t_picture: texture times diffuse");
	out->shader.rgb_inputs[0] = PS_COMBINERINPUTS(PS_REGISTER_T0, PS_REGISTER_V0, PS_REGISTER_ZERO, PS_REGISTER_ZERO);
	out->shader.rgb_outputs[0] = PS_COMBINEROUTPUTS(PS_REGISTER_R0, PS_REGISTER_DISCARD, PS_REGISTER_DISCARD, 0);
	out->shader.alpha_inputs[0] = PS_COMBINERINPUTS(PS_REGISTER_T0 | PS_CHANNEL_ALPHA, PS_REGISTER_V0 | PS_CHANNEL_ALPHA,
		PS_REGISTER_ZERO, PS_REGISTER_ZERO);
	out->shader.alpha_outputs[0] = PS_COMBINEROUTPUTS(PS_REGISTER_R0, PS_REGISTER_DISCARD, PS_REGISTER_DISCARD, 0);
	out->shader.final_combiner_inputs_abcd = PS_COMBINERINPUTS(PS_REGISTER_ZERO, PS_REGISTER_ZERO, PS_REGISTER_ZERO, PS_REGISTER_R0);
	out->shader.final_combiner_inputs_efg = PS_COMBINERINPUTS(PS_REGISTER_ZERO, PS_REGISTER_ZERO, PS_REGISTER_R0 | PS_CHANNEL_ALPHA, 0);
	out->shader.combiner_count = PS_COMBINERCOUNT(1, 0);
	out->shader.texture_modes = PS_TEXTUREMODES(PS_TEXTUREMODES_PROJECT2D, PS_TEXTUREMODES_NONE, PS_TEXTUREMODES_NONE,
		PS_TEXTUREMODES_NONE);
	out->write_color = out->write_alpha = 1;
	out->depth_write = 1;
	out->source_blend = BLEND_ONE;
	out->destination_blend = BLEND_ZERO;
	out->blend_operation = BLENDOP_ADD;
}

/* a texture alone (stage 0, or stage 1 for the programs that put their projective coordinates there) */
static void pt_state_texture(struct site *out, int stage)
{
	pt_state_texture_times_diffuse(out);
	snprintf(out->name, sizeof(out->name), "t_picture: texture %d", stage);
	out->shader.rgb_inputs[0] = PS_COMBINERINPUTS(PS_REGISTER_T0 + stage, PS_REGISTER_ONE, PS_REGISTER_ZERO, PS_REGISTER_ZERO);
	out->shader.alpha_inputs[0] = PS_COMBINERINPUTS((PS_REGISTER_T0 + stage) | PS_CHANNEL_ALPHA, PS_REGISTER_ONE | PS_CHANNEL_ALPHA,
		PS_REGISTER_ZERO, PS_REGISTER_ZERO);
	out->shader.texture_modes = PS_TEXTUREMODES_PROJECT2D << (5 * stage);
}

/* the vertex colour alone */
static void pt_state_diffuse(struct site *out)
{
	pt_state_texture_times_diffuse(out);
	strcpy(out->name, "t_picture: diffuse");
	out->shader.rgb_inputs[0] = PS_COMBINERINPUTS(PS_REGISTER_V0, PS_REGISTER_ONE, PS_REGISTER_ZERO, PS_REGISTER_ZERO);
	out->shader.alpha_inputs[0] = PS_COMBINERINPUTS(PS_REGISTER_V0 | PS_CHANNEL_ALPHA, PS_REGISTER_ONE | PS_CHANNEL_ALPHA,
		PS_REGISTER_ZERO, PS_REGISTER_ZERO);
	out->shader.texture_modes = 0;
}

static void pt_apply(const struct site *site)
{
	int stage;

	site_apply(site);
	D3D__RenderState[D3DRS_FOGENABLE] = FALSE;
	for (stage = 0; stage < 4; stage++)
	{
		D3D__TextureState[stage][D3DTSS_MIPFILTER] = D3DTEXF_NONE;
		D3D__TextureState[stage][D3DTSS_ADDRESSU] = D3DTADDRESS_WRAP;
		D3D__TextureState[stage][D3DTSS_ADDRESSV] = D3DTADDRESS_WRAP;
		D3D__TextureState[stage][D3DTSS_COLORSIGN] = 0;
	}
}

static void pt_filter(int stage, DWORD filter)
{
	D3D__TextureState[stage][D3DTSS_MAGFILTER] = filter;
	D3D__TextureState[stage][D3DTSS_MINFILTER] = filter;
}

static void pt_expect_counts(const char *name)
{
	/* (a sample point on an edge two triangles share may be drawn by both, in either renderer) */
	CHECK(!ref_behind_eye, "%s: the reference left out %d triangles with a vertex behind the eye (the test's geometry)", name,
		ref_behind_eye);
	CHECK(ref.fragments > 0 && sw.pixels_drawn > ref.fragments - ref.fragments / 50 - 4 &&
		sw.pixels_drawn < ref.fragments + ref.fragments / 50 + 4, "%s: the reference drew %lu pixels and OpenGL %lu", name,
		ref.fragments, sw.pixels_drawn);
}

/* ----- which outputs each of the game's programs writes */

static void pt_list_programs(void)
{
	int program, address;

	for (program = 0; program < NUMBER_OF_VERTEX_SHADERS; program++)
	{
		unsigned long written[13], inputs;

		ref_program_usage(program, written, &inputs);
		printf("# vs%d: inputs %04lx; writes", program, inputs);
		for (address = 0; address < 13; address++)
		{
			if (written[address])
				printf(" o%d:%lx", address, written[address]);
		}
		printf("\n");
	}
}

/* ----- the screen: a textured rectangle over all of it, through the HUD's program */

static void pt_test_screen(void)
{
	static const int windows[4][2] = { { 640, 480 }, { 1280, 960 }, { 1000, 600 }, { 500, 500 } };
	struct site site;
	int window;

	test_name = "screen";
	pt_state_texture_times_diffuse(&site);
	for (window = 0; window < 4; window++)
	{
		char name[64];

		pt_window(windows[window][0], windows[window][1]);
		constants_fill();
		constants_screen(640.0f, 480.0f);
		pt_apply(&site);
		/* (texel edges fall on sample points where the window's scale is not a whole number: linear there) */
		pt_filter(0, window < 2 ? D3DTEXF_POINT : D3DTEXF_LINEAR);
		pt_SetVertexShader(vertex_shaders[4]);
		screen_quad(0.0f, 0.0f, 640.0f, 480.0f, 8);
		/* (the top half of the texture over the 480 rows: 15 rows a texel at
		640x480, so that no texel edge falls on a row's sample point. The
		quad's positions and coordinates go through OpenGL's own transform
		now, which rounds otherwise than the Xbox's program, and point
		sampling at an edge takes either texel.) */
		{
			struct hud_vertex *quad = (struct hud_vertex *)streams.data[0] + 8;
			int corner;

			for (corner = 0; corner < 4; corner++)
				quad[corner].coordinates[1] *= 0.5f;
		}
		pt_counts_reset();
		pt_draw(D3DPT_QUADLIST, 8, 4);
		snprintf(name, sizeof(name), "screen %dx%d", windows[window][0], windows[window][1]);
		pt_expect_same(name, pt_texture_tolerance(), 0);
		pt_expect_counts(name);

		/* a part of it, with linear filtering */
		pt_clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, PT_CLEAR_COLOR, 1.0f);
		pt_filter(0, D3DTEXF_LINEAR);
		screen_quad(37.0f, 21.0f, 411.0f, 300.0f, 8);
		pt_counts_reset();
		pt_draw(D3DPT_QUADLIST, 8, 4);
		snprintf(name, sizeof(name), "screen part %dx%d", windows[window][0], windows[window][1]);
		pt_expect_same(name, pt_texture_tolerance(), 0);
		pt_expect_counts(name);
	}
	pt_window(640, 480);
}

/* ----- the world: triangles at depths, windings and cull modes, viewports and depth ranges */

static void pt_world_vertex(struct environment_vertex *vertex, float x, float y, float z, float s, float t)
{
	vertex->position[0] = x; vertex->position[1] = y; vertex->position[2] = z;
	vertex->normal = 0x7fc00000UL;
	vertex->binormal = 0x000003ffUL;
	vertex->tangent = 0x001ff800UL;
	vertex->coordinates[0] = s;
	vertex->coordinates[1] = t;
}

static void pt_test_world(void)
{
	static const D3DVIEWPORT8 viewports[3] = { { 0, 0, 640, 480, 0.0f, 1.0f }, { 100, 50, 400, 300, 0.25f, 0.75f },
		{ 0, 0, 640, 480, 0.0f, 1.0f } };
	static const int windows[3][2] = { { 640, 480 }, { 640, 480 }, { 1280, 960 } };
	struct environment_vertex *vertices = (struct environment_vertex *)streams.data[0];
	unsigned short *indices = guest_alloc(4096);
	struct site site;
	int variant, cull;

	test_name = "world";
	pt_state_texture(&site, 0);
	/* three triangles that cross each other in depth, far from the start of the stream; a fourth wound the other way */
	pt_world_vertex(&vertices[300], -2.1f, -1.3f, 8.0f, 0.0f, 0.0f);
	pt_world_vertex(&vertices[301], 2.3f, -1.1f, 9.0f, 1.0f, 0.0f);
	pt_world_vertex(&vertices[302], 0.2f, 1.7f, 12.0f, 0.5f, 1.0f);
	pt_world_vertex(&vertices[303], -1.3f, 0.9f, 5.0f, 0.0f, 0.3f);
	pt_world_vertex(&vertices[304], 1.9f, 1.2f, 15.0f, 2.0f, 0.2f);
	pt_world_vertex(&vertices[305], 0.1f, -1.6f, 9.5f, 1.1f, 1.7f);
	pt_world_vertex(&vertices[306], -0.4f, -0.2f, 3.0f, 0.2f, 0.1f);
	pt_world_vertex(&vertices[307], 0.5f, -0.3f, 30.0f, 0.9f, 0.3f);
	pt_world_vertex(&vertices[308], 0.1f, 0.6f, 6.0f, 0.4f, 0.8f);
	pt_world_vertex(&vertices[309], -3.0f, 2.0f, 20.0f, 0.0f, 0.0f);
	pt_world_vertex(&vertices[310], -1.0f, 2.2f, 20.0f, 0.0f, 3.0f);
	pt_world_vertex(&vertices[311], -2.5f, 0.3f, 14.0f, 3.0f, 0.0f);
	for (variant = 0; variant < 3; variant++)
	{
		for (cull = 0; cull < 3; cull++)
		{
			static const DWORD cull_modes[3] = { D3DCULL_NONE, D3DCULL_CW, D3DCULL_CCW };
			static const unsigned short order[12] = { 302, 300, 301, 303, 304, 305, 308, 307, 306, 309, 310, 311 };
			char name[64];

			pt_window(windows[variant][0], windows[variant][1]);
			constants_fill();
			constants_world();
			pt_SetViewport(&viewports[variant]);
			pt_apply(&site);
			pt_filter(0, D3DTEXF_LINEAR);
			D3D__RenderState[D3DRS_CULLMODE] = cull_modes[cull];
			D3D__RenderState[D3DRS_FRONTFACE] = D3DFRONT_CW;
			pt_SetVertexShader(vertex_shaders[40]);
			pt_SetStreamSource(0, &streams.buffer[0], sizeof(struct environment_vertex));
			memcpy(indices, order, sizeof(order));
			pt_counts_reset();
			pt_draw_indexed(D3DPT_TRIANGLELIST, 12, indices);
			snprintf(name, sizeof(name), "world viewport %d cull %d", variant, cull);
			pt_expect_same(name, pt_texture_tolerance(), 0);
			pt_expect_counts(name);
			/* the same again with the depth function EQUAL and another colour: every pixel of the first pass, no other */
			D3D__RenderState[D3DRS_ZFUNC] = D3DCMP_EQUAL;
			D3D__RenderState[D3DRS_ZWRITEENABLE] = FALSE;
			pt_SetTexture(0, stage_textures[1]);
			pt_counts_reset();
			pt_draw_indexed(D3DPT_TRIANGLELIST, 12, indices);
			snprintf(name, sizeof(name), "world viewport %d cull %d, depth EQUAL", variant, cull);
			pt_expect_same(name, pt_texture_tolerance(), 0);
			/* (the last triangle drawn at a pixel passes; an earlier one at the same pixel does not) */
			CHECK(ref.fragments > 0, "%s: nothing passes", name);
			CHECK(sw.pixels_drawn > ref.fragments - ref.fragments / 50 - 4 && sw.pixels_drawn < ref.fragments + ref.fragments / 50 + 4,
				"%s: %lu pixels pass in OpenGL, %lu on the Xbox", name, sw.pixels_drawn, ref.fragments);
			D3D__RenderState[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
		}
	}
	/* strips, fans, quads: unindexed, from the middle of the stream */
	{
		static const D3DPRIMITIVETYPE types[4] = { D3DPT_TRIANGLESTRIP, D3DPT_TRIANGLEFAN, D3DPT_QUADLIST, D3DPT_QUADSTRIP };
		int type, index;

		for (index = 0; index < 8; index++)
		{
			float angle = (float)index * 0.7f;

			pt_world_vertex(&vertices[500 + index], -2.0f + 0.6f * (float)index, (index & 1 ? 1.2f : -1.0f) + 0.1f * (float)index,
				6.0f + 2.0f * (float)index, (float)index * 0.3f, cosf(angle));
		}
		/* (a fan and quads want a convex outline) */
		pt_world_vertex(&vertices[520], 0.0f, 0.0f, 9.0f, 0.5f, 0.5f);
		for (index = 0; index < 7; index++)
		{
			float angle = (float)index * 1.0f;

			pt_world_vertex(&vertices[521 + index], 2.2f * cosf(angle), 1.5f * sinf(angle), 7.0f + (float)index, cosf(angle), sinf(angle));
		}
		for (type = 0; type < 4; type++)
		{
			char name[64];

			pt_window(640, 480);
			constants_fill();
			constants_world();
			pt_SetViewport(&viewports[0]);
			pt_apply(&site);
			D3D__RenderState[D3DRS_CULLMODE] = type == 0 ? D3DCULL_CCW : D3DCULL_NONE;
			pt_SetVertexShader(vertex_shaders[40]);
			pt_SetStreamSource(0, &streams.buffer[0], sizeof(struct environment_vertex));
			pt_counts_reset();
			pt_draw(types[type], type == 0 || type == 3 ? 500 : 520, 8);
			snprintf(name, sizeof(name), "world primitive type %d", (int)types[type]);
			pt_expect_same(name, pt_texture_tolerance(), 0);
			pt_expect_counts(name);
		}
	}
	pt_SetViewport(&viewports[0]);
	pt_window(640, 480);
}

/* ----- textures: every texel format the game has, swizzled and linear */

static D3DBaseTexture *pt_texture_new(DWORD format, unsigned long width, unsigned long height, int linear, unsigned long bytes)
{
	D3DBaseTexture *texture;
	unsigned char *texels;
	unsigned long index, size;

	if (linear)
		return texture_new_linear(format, width, height, bytes);
	texture = texture_new(format, width, height, 1, 1, FALSE);
	/* (smooth enough that 16 bits a texel are near the texel: every byte within a few steps of its neighbours' is
	not possible in a swizzled level, so only the tolerance takes it up) */
	texels = PLATFORM_PHYSICAL_TO_VIRTUAL(texture->Data);
	size = width * height * (bytes ? bytes : 4);
	for (index = 0; index < size; index++)
		texels[index] = (unsigned char)test_random();
	return texture;
}

static void pt_test_formats(void)
{
	static const struct
	{
		const char *name;
		DWORD format;
		unsigned long width, height;
		int linear;
		unsigned long bytes;
		/* the most a channel may be off with 16-bit textures */
		int tolerance_16;
	} formats[] =
	{
		{ "A8R8G8B8", 0x06, 64, 64, 0, 4, 12 },
		{ "A8R8G8B8 64x16", 0x06, 64, 16, 0, 4, 12 },
		{ "A8R8G8B8 8x32", 0x06, 8, 32, 0, 4, 12 },
		{ "X8R8G8B8", 0x07, 32, 32, 0, 4, 6 },
		{ "R5G6B5", 0x05, 32, 32, 0, 2, 6 },
		{ "A1R5G5B5", 0x02, 32, 32, 0, 2, 6 },
		{ "X1R5G5B5", 0x03, 32, 32, 0, 2, 6 },
		{ "A4R4G4B4", 0x04, 32, 32, 0, 2, 3 },
		{ "L8", 0x00, 32, 32, 0, 1, 3 },
		{ "AL8", 0x01, 32, 32, 0, 1, 3 },
		{ "A8", 0x19, 32, 32, 0, 1, 3 },
		{ "A8L8", 0x1a, 32, 32, 0, 2, 3 },
		{ "P8", 0x0b, 32, 32, 0, 1, 12 },
		{ "DXT1", 0x0c, 32, 32, 0, 2, 12 },
		{ "DXT3", 0x0e, 32, 32, 0, 4, 12 },
		{ "DXT5", 0x0f, 32, 32, 0, 4, 12 },
		{ "LIN_A8R8G8B8 100x60", 0x12, 100, 60, 1, 4, 12 },
		{ "LIN_X8R8G8B8 33x7", 0x1e, 33, 7, 1, 4, 6 },
		{ "LIN_R5G6B5 64x64", 0x11, 64, 64, 1, 2, 6 },
		{ "LIN_A1R5G5B5 20x90", 0x10, 20, 90, 1, 2, 6 },
		{ "LIN_A4R4G4B4 17x17", 0x1d, 17, 17, 1, 2, 3 },
		{ "LIN_L8 50x50", 0x13, 50, 50, 1, 1, 3 },
		{ "LIN_A8L8 50x40", 0x20, 50, 40, 1, 2, 3 },
		{ "LIN_A8 30x40", 0x1f, 30, 40, 1, 1, 3 },
		{ "LIN_AL8 30x40", 0x1b, 30, 40, 1, 1, 3 },
	};
	static D3DBaseTexture *textures[sizeof(formats) / sizeof(formats[0])];
	static D3DPalette palette;
	struct site site;
	unsigned long format;
	int filter;

	test_name = "formats";
	if (!palette.Data)
	{
		DWORD *colors = guest_alloc(1024);
		int index;

		for (index = 0; index < 256; index++)
			colors[index] = (DWORD)(test_random() | (test_random() << 16));
		palette.Common = 1;
		palette.Data = PLATFORM_VIRTUAL_TO_PHYSICAL(colors);
	}
	pt_state_texture_times_diffuse(&site);
	for (format = 0; format < sizeof(formats) / sizeof(formats[0]); format++)
	{
		if (!textures[format])
			textures[format] = pt_texture_new(formats[format].format, formats[format].width, formats[format].height,
				formats[format].linear, formats[format].bytes);
		for (filter = 0; filter < 2; filter++)
		{
			struct hud_vertex *vertices = (struct hud_vertex *)streams.data[0] + 8;
			float width = formats[format].linear ? (float)formats[format].width : 1.0f;
			float height = formats[format].linear ? (float)formats[format].height : 1.0f;
			char name[96];
			int tolerance = !strcmp(texture_bits_setting, "32") ? 3 : formats[format].tolerance_16;

			pt_clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, PT_CLEAR_COLOR, 1.0f);
			constants_fill();
			constants_screen(640.0f, 480.0f);
			pt_apply(&site);
			pt_filter(0, filter ? D3DTEXF_LINEAR : D3DTEXF_POINT);
			/* clamp: a linear texture has nothing else */
			D3D__TextureState[0][D3DTSS_ADDRESSU] = formats[format].linear ? D3DTADDRESS_CLAMP : D3DTADDRESS_WRAP;
			/* (without ARB_texture_mirrored_repeat the light renderer repeats a mirrored texture: not asked for there) */
			D3D__TextureState[0][D3DTSS_ADDRESSV] = formats[format].linear ? D3DTADDRESS_CLAMP :
				allowed.mirrored ? D3DTADDRESS_MIRROR : D3DTADDRESS_WRAP;
			pt_SetTexture(0, textures[format]);
			pt_SetPalette(0, formats[format].format == 0x0b ? &palette : NULL);
			pt_SetVertexShader(vertex_shaders[4]);
			/* 16 window pixels to a texel, the texture one and a quarter times across: texel centres and edges fall
			between sample points */
			screen_quad(40.0f, 30.0f, 40.0f + (float)formats[format].width * 4.0f, 30.0f + (float)formats[format].height * 4.0f, 8);
			{
				/* (OpenGL 1.1's own clamp runs into the border colour at the texture's edge, which no texture of the
				Xbox has: without the edge clamp extension a clamped texture is only looked at inside its edges) */
				float low = formats[format].linear && !allowed.edge_clamp ? 0.125f : -0.125f, high = 1.0f - low;

				vertices[0].coordinates[0] = vertices[3].coordinates[0] = low * width;
				vertices[1].coordinates[0] = vertices[2].coordinates[0] = high * width;
				vertices[0].coordinates[1] = vertices[1].coordinates[1] = low * height;
				vertices[2].coordinates[1] = vertices[3].coordinates[1] = high * height;
			}
			pt_counts_reset();
			pt_draw(D3DPT_QUADLIST, 8, 4);
			snprintf(name, sizeof(name), "format %s %s", formats[format].name, filter ? "linear" : "point");
			pt_expect_same(name, tolerance, 0);
			pt_expect_counts(name);
		}
	}
	pt_SetPalette(0, NULL);
}

/* ----- projective coordinates, through immediate mode */

static void pt_test_projective(void)
{
	/* programs that write a q, and the stage it is for */
	static const int programs[][2] = { { 12, 1 }, { 48, 0 }, { 23, 1 }, { 23, 2 }, { 23, 3 }, { 26, 1 }, { 26, 3 }, { 51, 3 }, { 28, 2 },
		{ 61, 3 }, { 66, 2 }, { 66, 3 } };
	float vertices[4][16][4];
	struct site site;
	int which, vertex, reg, compared = 0;

	test_name = "projective";
	for (which = 0; which < (int)(sizeof(programs) / sizeof(programs[0])); which++)
	{
		int program = programs[which][0], stage = programs[which][1];
		unsigned long written[13], inputs;
		char name[64];

		ref_program_usage(program, written, &inputs);
		if (!(written[9 + stage] & 1))
		{
			fail("vs%d does not write the q of stage %d", program, stage);
			continue;
		}
		pt_state_texture(&site, stage);
		pt_clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, PT_CLEAR_COLOR, 1.0f);
		constants_fill();
		constants_world();
		pt_apply(&site);
		pt_filter(stage, D3DTEXF_LINEAR);
		pt_SetTexture(stage, stage_textures[0]);
		pt_SetVertexShader(vertex_shaders[program]);
		for (vertex = 0; vertex < 4; vertex++)
		{
			float x = vertex == 0 || vertex == 3 ? 0.0f : 1.0f, y = vertex < 2 ? 0.0f : 1.0f;

			for (reg = 0; reg < 16; reg++)
			{
				vertices[vertex][reg][0] = 0.1f + 0.05f * (float)reg + 0.3f * x;
				vertices[vertex][reg][1] = 0.2f + 0.03f * (float)reg + 0.4f * y;
				vertices[vertex][reg][2] = 0.5f;
				vertices[vertex][reg][3] = 1.0f + 0.5f * x + 0.25f * y;
			}
			vertices[vertex][0][0] = -1.5f + 3.0f * x;
			vertices[vertex][0][1] = -1.0f + 2.0f * y;
			vertices[vertex][0][2] = 5.0f + 6.0f * x;
			vertices[vertex][0][3] = 1.0f;
		}
		pt_counts_reset();
		ref_behind_eye = 0;
		pt_draw_immediate(D3DPT_QUADLIST, 4, &vertices[0][0][0]);
		snprintf(name, sizeof(name), "projective vs%d stage %d", program, stage);
		/* ("simple": the built-in plan multiplies by the vertex colour, which these vertices do not set) */
		if (ref_behind_eye || !ref.fragments || word_in_list(extensions_setting, "simple"))
		{
			/* (the program wants constants or vertices the test does not know how to give it) */
			printf("# %s: not compared (%d triangles behind the eye, %lu pixels)\n", name, ref_behind_eye, ref.fragments);
			ref_behind_eye = 0;
			continue;
		}
		compared++;
		/* (where q passes through zero the coordinates have no bound: a line of pixels either renderer may put anything on;
		and the quad's diagonal, a line of pixels either triangle may own, is sampled with a q near zero as well) */
		pt_expect_same(name, pt_texture_tolerance(), ref.fragments / 50);
		pt_expect_counts(name);
		if (verbose)
			printf("# %s: compared, %lu pixels\n", name, ref.fragments);
	}
	CHECK(compared >= 4 || word_in_list(extensions_setting, "simple"), "only %d programs with projective coordinates were compared", compared);
}

/* ----- clears: rectangles, channels, depth */

static void pt_test_clears(void)
{
	static const D3DRECT rectangles[3] = { { 10, 20, 200, 100 }, { 300, 5, 640, 60 }, { 0, 400, 77, 480 } };
	static const int windows[2][2] = { { 640, 480 }, { 1280, 960 } };
	struct site site;
	int window;

	test_name = "clears";
	pt_state_diffuse(&site);
	for (window = 0; window < 2; window++)
	{
		char name[64];

		pt_window(windows[window][0], windows[window][1]);
		pt_clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0x80c0a060UL, 1.0f);
		pt_clear(3, rectangles, D3DCLEAR_TARGET, 0x40ff2010UL, 1.0f);
		pt_clear(1, &rectangles[1], D3DCLEAR_TARGET_R | D3DCLEAR_TARGET_A, 0xff0000ffUL, 1.0f);
		pt_clear(1, &rectangles[0], D3DCLEAR_ZBUFFER, 0, 0.3f);
		snprintf(name, sizeof(name), "clears %dx%d", windows[window][0], windows[window][1]);
		pt_expect_same(name, 0, 0);
		/* the depth cleared in the first rectangle hides a quad at depth 0.5 there, and nowhere else */
		constants_fill();
		constants_screen(640.0f, 480.0f);
		pt_apply(&site);
		pt_SetVertexShader(vertex_shaders[4]);
		screen_quad(0.0f, 0.0f, 640.0f, 480.0f, 8);
		pt_counts_reset();
		pt_draw(D3DPT_QUADLIST, 8, 4);
		snprintf(name, sizeof(name), "clears %dx%d, then a quad", windows[window][0], windows[window][1]);
		pt_expect_same(name, 2, 0);
		pt_expect_counts(name);
	}
	pt_window(640, 480);
}

/* ----- a draw that samples the frame */

static void pt_test_frame(void)
{
	struct site site;
	D3DSurface *back_buffer = NULL;
	struct hud_vertex *vertices = (struct hud_vertex *)streams.data[0] + 8;
	int window;

	test_name = "frame";
	D3DDevice_GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &back_buffer);
	for (window = 0; window < 2; window++)
	{
		char name[64];

		pt_window(window ? 1280 : 640, window ? 960 : 480);
		/* something to sample: a textured quad on the left */
		pt_state_texture_times_diffuse(&site);
		constants_fill();
		constants_screen(640.0f, 480.0f);
		pt_apply(&site);
		pt_filter(0, D3DTEXF_POINT);
		pt_SetVertexShader(vertex_shaders[4]);
		screen_quad(20.0f, 40.0f, 276.0f, 296.0f, 8);
		pt_draw(D3DPT_QUADLIST, 8, 4);
		/* the frame's texels 30..190 by 50..170 drawn at 400,200, one to one */
		pt_state_texture(&site, 0);
		pt_apply(&site);
		pt_filter(0, D3DTEXF_POINT);
		D3D__TextureState[0][D3DTSS_ADDRESSU] = D3DTADDRESS_CLAMP;
		D3D__TextureState[0][D3DTSS_ADDRESSV] = D3DTADDRESS_CLAMP;
		pt_SetTexture(0, (D3DBaseTexture *)back_buffer);
		screen_quad(400.0f, 200.0f, 560.0f, 320.0f, 8);
		vertices[0].coordinates[0] = vertices[3].coordinates[0] = 30.0f;
		vertices[1].coordinates[0] = vertices[2].coordinates[0] = 190.0f;
		vertices[0].coordinates[1] = vertices[1].coordinates[1] = 50.0f;
		vertices[2].coordinates[1] = vertices[3].coordinates[1] = 170.0f;
		ref_copy_frame();
		pt_counts_reset();
		snprintf(name, sizeof(name), "frame sampled %dx%d", ref.width, ref.height);
		if ((window ? 2048 : 1024) > driver.maximum_texture_size)
		{
			/* the frame does not fit a texture of this context: the draw is left out, and the log says so */
			begin_draw();
			D3DDevice_DrawVertices(D3DPT_QUADLIST, 8, 4);
			CHECK(sw.pixels_drawn == 0 && log_count("does not fit a texture"), "%s: %lu pixels drawn from a frame that fits no texture",
				name, sw.pixels_drawn);
			continue;
		}
		pt_draw(D3DPT_QUADLIST, 8, 4);
		pt_expect_same(name, pt_texture_tolerance(), 0);
		pt_expect_counts(name);
	}
	pt_SetTexture(0, stage_textures[0]);
	pt_window(640, 480);
}

/* ----- the motion sensor (rasterizer_xbox_motion_sensor.c): its 64x64
target cleared to 0x00000000, a blip added with red, green and blue
written, then the target added to the screen with ONE, INVSRCALPHA (its
alpha, 0, takes nothing away). In a window with alpha the target is drawn
in the window's corner and the corner put back; in one without, the
target and what samples it are left out (xgpu_fixed.c, corner_target).
Either way the screen keeps its colour where the sensor is empty. The
reference draws no targets: the expected pixels are the test's own. */

static void pt_pixel(int x, int y_from_top, unsigned char *rgba)
{
	memcpy(rgba, sw.color + ((size_t)(sw.height - 1 - y_from_top) * sw.width + x) * 4, 4);
}

static void pt_sensor_state(struct site *out, int composite)
{
	memset(out, 0, sizeof(*out));
	strcpy(out->name, composite ? "t_picture: sensor on the screen" : "t_picture: sensor blip");
	/* the final combiner: rgb D (the texel, or the vertex colour), alpha G */
	out->shader.final_combiner_inputs_abcd = PS_COMBINERINPUTS(PS_REGISTER_ZERO, PS_REGISTER_ZERO, PS_REGISTER_ZERO,
		composite ? PS_REGISTER_T0 : PS_REGISTER_V0);
	out->shader.final_combiner_inputs_efg = PS_COMBINERINPUTS(PS_REGISTER_ZERO, PS_REGISTER_ZERO,
		(composite ? PS_REGISTER_T0 : PS_REGISTER_V0) | PS_CHANNEL_ALPHA, 0);
	out->shader.combiner_count = PS_COMBINERCOUNT(1, 0);
	out->shader.texture_modes = composite ? PS_TEXTUREMODES(PS_TEXTUREMODES_PROJECT2D, PS_TEXTUREMODES_NONE,
		PS_TEXTUREMODES_NONE, PS_TEXTUREMODES_NONE) : 0;
	out->blend_enable = 1;
	out->source_blend = BLEND_ONE;
	out->destination_blend = composite ? BLEND_INVSRCALPHA : BLEND_ONE;
	out->blend_operation = BLENDOP_ADD;
	out->write_color = 1;
}

static void pt_test_sensor(void)
{
	D3DSurface *back_buffer = NULL, *depth_buffer = NULL;
	D3DSurface sensor;
	D3DBaseTexture *as_texture = guest_alloc(sizeof(*as_texture));
	D3DVIEWPORT8 small = { 0, 0, 64, 64, 0.0f, 1.0f }, screen = { 0, 0, 640, 480, 0.0f, 1.0f };
	struct hud_vertex *vertices = (struct hud_vertex *)streams.data[0] + 8;
	struct site site;
	unsigned char corner_before[4], corner_after[4], empty[4], blip[4];
	int index, channel;

	test_name = "sensor";
	pt_window(640, 480);
	D3DDevice_GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &back_buffer);
	D3DDevice_GetDepthStencilSurface(&depth_buffer);
	/* something in the window's corner, which the target's pictures borrow */
	constants_fill();
	constants_screen(640.0f, 480.0f);
	pt_state_diffuse(&site);
	pt_apply(&site);
	D3D__RenderState[D3DRS_ZENABLE] = FALSE;
	pt_SetVertexShader(vertex_shaders[4]);
	screen_quad(0.0f, 400.0f, 100.0f, 480.0f, 8);
	pt_draw(D3DPT_QUADLIST, 8, 4);
	pt_pixel(5, 475, corner_before);

	/* the sensor's picture */
	d3d8_surface_initialize(&sensor, D3DFMT_LIN_A8R8G8B8, 64, 64);
	D3DDevice_SetRenderTarget(&sensor, NULL);
	pt_SetViewport(&small);
	constants_screen(64.0f, 64.0f);
	D3DDevice_Clear(0, NULL, D3DCLEAR_TARGET, 0x00000000UL, 1.0f, 0);
	pt_sensor_state(&site, 0);
	pt_apply(&site);
	D3D__RenderState[D3DRS_ZENABLE] = FALSE;
	screen_quad(40.0f, 40.0f, 56.0f, 56.0f, 8);
	for (index = 0; index < 4; index++)
		vertices[index].color = 0xff402010UL;
	begin_draw();
	D3DDevice_DrawVertices(D3DPT_QUADLIST, 8, 4);

	/* on the screen, a texel a pixel (the target is linear: its coordinates are texels) */
	D3DDevice_SetRenderTarget(back_buffer, depth_buffer);
	pt_SetViewport(&screen);
	constants_screen(640.0f, 480.0f);
	pt_sensor_state(&site, 1);
	pt_apply(&site);
	D3D__RenderState[D3DRS_ZENABLE] = FALSE;
	pt_filter(0, D3DTEXF_POINT);
	D3D__TextureState[0][D3DTSS_ADDRESSU] = D3DTADDRESS_CLAMP;
	D3D__TextureState[0][D3DTSS_ADDRESSV] = D3DTADDRESS_CLAMP;
	memset(as_texture, 0, sizeof(*as_texture));
	as_texture->Common = 1;
	as_texture->Data = sensor.Data;
	as_texture->Format = sensor.Format | (1UL << D3DFORMAT_MIPMAP_SHIFT);
	as_texture->Size = sensor.Size;
	pt_SetTexture(0, as_texture);
	screen_quad(200.0f, 200.0f, 264.0f, 264.0f, 8);
	for (index = 0; index < 4; index++)
	{
		vertices[index].coordinates[0] *= 64.0f;
		vertices[index].coordinates[1] *= 64.0f;
	}
	begin_draw();
	D3DDevice_DrawVertices(D3DPT_QUADLIST, 8, 4);
	/* (off the quads' diagonals, which a sample point on them would have drawn twice) */
	pt_pixel(210, 216, empty);
	pt_pixel(244, 250, blip);
	pt_pixel(5, 475, corner_after);
	for (channel = 0; channel < 3; channel++)
	{
		/* the window's colour, 20 30 40; on the blip 40 20 10 more where the target is drawn */
		int window = (int)((PT_CLEAR_COLOR >> (16 - 8 * channel)) & 0xff);
		int added = driver.alpha_bits ? (int)((0x402010UL >> (16 - 8 * channel)) & 0xff) : 0;

		CHECK(abs(empty[channel] - window) <= 1, "where the sensor is empty, channel %d is %02x: the window's is %02x", channel,
			empty[channel], window);
		CHECK(abs(blip[channel] - (window + added)) <= 1, "on the blip, channel %d is %02x: expected %02x", channel, blip[channel],
			window + added);
		CHECK(corner_after[channel] == corner_before[channel], "the window's corner, channel %d, was %02x and is %02x", channel,
			corner_before[channel], corner_after[channel]);
	}
	pt_SetTexture(0, stage_textures[0]);
	D3D__RenderState[D3DRS_ZENABLE] = TRUE;
	back_buffer->Common--;
	depth_buffer->Common--;
	pt_window(640, 480);
}

/* ----- z bias: a decal on a wall */

static void pt_test_bias(void)
{
	struct site site;
	struct environment_vertex *vertices = (struct environment_vertex *)streams.data[0];
	static const D3DVIEWPORT8 viewport = { 0, 0, 640, 480, 0.0f, 1.0f };
	double wall_depth, decal_depth, step;
	unsigned long wall_pixels;

	test_name = "bias";
	pt_state_texture(&site, 0);
	pt_window(640, 480);
	constants_fill();
	constants_world();
	pt_SetViewport(&viewport);
	pt_apply(&site);
	pt_SetVertexShader(vertex_shaders[40]);
	pt_SetStreamSource(0, &streams.buffer[0], sizeof(struct environment_vertex));
	/* a wall seen head on */
	pt_world_vertex(&vertices[0], -1.0f, -1.0f, 10.0f, 0.0f, 0.0f);
	pt_world_vertex(&vertices[1], 1.0f, -1.0f, 10.0f, 1.0f, 0.0f);
	pt_world_vertex(&vertices[2], 1.0f, 1.0f, 10.0f, 1.0f, 1.0f);
	pt_world_vertex(&vertices[3], -1.0f, 1.0f, 10.0f, 0.0f, 1.0f);
	pt_counts_reset();
	pt_draw(D3DPT_QUADLIST, 0, 4);
	wall_pixels = sw.pixels_drawn;
	wall_depth = sw.last_depth;
	/* the same again, strictly nearer only: nothing */
	D3D__RenderState[D3DRS_ZFUNC] = D3DCMP_LESS;
	pt_counts_reset();
	begin_draw();
	D3DDevice_DrawVertices(D3DPT_QUADLIST, 0, 4);
	CHECK(sw.pixels_drawn == 0, "a second copy of the wall passes LESS at %lu pixels", sw.pixels_drawn);
	/* with the decals' bias: all of it */
	D3DDevice_SetRenderState_ZBias(8);
	pt_counts_reset();
	begin_draw();
	D3DDevice_DrawVertices(D3DPT_QUADLIST, 0, 4);
	decal_depth = sw.last_depth;
	step = 1.0 / (double)(1UL << (driver.depth_bits >= 24 ? 24 : driver.depth_bits));
	CHECK(sw.pixels_drawn == wall_pixels && wall_pixels > 0, "the biased decal passes at %lu of the wall's %lu pixels", sw.pixels_drawn,
		wall_pixels);
	/* nearer by the bias: eight steps of the Xbox's 24-bit depth, and in a 16-bit buffer no less than two of its own */
	CHECK(wall_depth - decal_depth >= (driver.depth_bits >= 24 ? 7.5 : 1.9) * step &&
		wall_depth - decal_depth <= (driver.depth_bits >= 24 ? 8.5 : 2.5) * step,
		"the decal is %.2f steps of the %d-bit depth buffer nearer than the wall", (wall_depth - decal_depth) / step, driver.depth_bits);
	D3DDevice_SetRenderState_ZBias(0);
	D3D__RenderState[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
}

/* ----- every pixel state of the game */

static struct
{
	unsigned long sites, unsupported, exact, exact_failed, approximate, fallback, suspects, borderline, structural;
	double approximate_mean, approximate_worst_mean, fallback_mean;
	char worst_name[128];
	int program;
	/* approximate plans by their mean difference: below 8, 8 to 24, 24 to 64, above */
	unsigned long buckets[4];
} pt_sweep;

/* what OpenGL was asked for, pass by pass, and the plan it should be */
static void pt_describe_draws(const struct site *site, const struct expected *expected)
{
	char text[4096];
	int pass, unit;

	nv2a_ff_plan_describe(&expected->plan, text, sizeof(text));
	printf("# %s: %s plan of %d passes, status %d, flags %lx, tolerance %d:\n%s\n", site->name,
		expected->fallback ? "built-in" : expected->plan.constants_bound ? "bound" : "general", expected->plan.pass_count,
		expected->plan.status, expected->plan.approximations, expected->plan.tolerance, text);
	for (pass = 0; pass < rec.draw_count; pass++)
	{
		const struct rec_state *state = &rec.draws[pass].state;

		printf("#  OpenGL pass %d: colour %s", pass, state->color_array ? "array" : "constant");
		if (state->color_array)
			printf(" (%d %d %d %d, %d %d %d %d, ...)", rec.draws[pass].colors[0][0], rec.draws[pass].colors[0][1],
				rec.draws[pass].colors[0][2], rec.draws[pass].colors[0][3], rec.draws[pass].colors[1][0], rec.draws[pass].colors[1][1],
				rec.draws[pass].colors[1][2], rec.draws[pass].colors[1][3]);
		else
			printf(" %.3f %.3f %.3f %.3f", state->color[0], state->color[1], state->color[2], state->color[3]);
		printf("; blend %d %04x/%04x eq %04x; alpha test %d %04x %.3f; depth %d %04x mask %d; colour mask %x\n", state->blend,
			(unsigned)state->blend_source, (unsigned)state->blend_destination, (unsigned)state->blend_equation, state->alpha_test,
			(unsigned)state->alpha_function, state->alpha_reference, state->depth_test, (unsigned)state->depth_function,
			state->depth_mask, state->color_mask);
		for (unit = 0; unit < REC_UNITS; unit++)
		{
			const struct rec_unit *u = &state->units[unit];

			if (!u->enabled)
				continue;
			printf("#   unit %d: texture %u (%dx%d base %d), mode %04x", unit, (unsigned)u->texture,
				u->texture < REC_TEXTURES ? rec.textures[u->texture].width[0] : 0,
				u->texture < REC_TEXTURES ? rec.textures[u->texture].height[0] : 0,
				u->texture < REC_TEXTURES ? sw_textures[u->texture].base : -1, (unsigned)u->mode);
			if (u->mode == GL_COMBINE)
				printf(" rgb %04x(%04x:%04x, %04x:%04x, %04x:%04x)x%d alpha %04x(%04x:%04x, %04x:%04x, %04x:%04x)x%d",
					(unsigned)u->combine[0], (unsigned)u->source[0][0], (unsigned)u->operand[0][0], (unsigned)u->source[0][1],
					(unsigned)u->operand[0][1], (unsigned)u->source[0][2], (unsigned)u->operand[0][2], u->scale[0],
					(unsigned)u->combine[1], (unsigned)u->source[1][0], (unsigned)u->operand[1][0], (unsigned)u->source[1][1],
					(unsigned)u->operand[1][1], (unsigned)u->source[1][2], (unsigned)u->operand[1][2], u->scale[1]);
			printf(", env colour %.3f %.3f %.3f %.3f, coordinates %s size %d, matrix %g %g %g %g\n", u->env_color[0], u->env_color[1],
				u->env_color[2], u->env_color[3], u->coordinates ? "array" : "none", (int)u->coordinate_size, u->matrix[0], u->matrix[5],
				u->matrix[12], u->matrix[13]);
		}
	}
}

static void pt_sweep_site(const struct site *site)
{
	struct expected expected;
	struct pt_result result;
	int stage, exact;

	if (pt_only && strcmp(pt_only, "sweep") && !strstr(site->name, pt_only))
		return;
	pt_sweep.sites++;
	pt_clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0x80607090UL, 1.0f);
	site_apply(site);
	for (stage = 0; stage < 4; stage++)
	{
		D3D__TextureState[stage][D3DTSS_MIPFILTER] = D3DTEXF_NONE;
		D3D__TextureState[stage][D3DTSS_ADDRESSU] = D3DTADDRESS_WRAP;
		/* (clamped where the context clamps to the edge texel as the Xbox does: OpenGL 1.1's own clamp blends the
		border colour in at the edge) */
		D3D__TextureState[stage][D3DTSS_ADDRESSV] = (stage & 1) && allowed.edge_clamp ? D3DTADDRESS_CLAMP : D3DTADDRESS_WRAP;
		pt_filter(stage, D3DTEXF_LINEAR);
	}
	expected_compute(site, &expected);
	pt_counts_reset();
	pt_draw(D3DPT_QUADLIST, 0, 4);
	if (ref_unsupported)
	{
		pt_sweep.unsupported++;
		present();
		return;
	}
	/* which of its plans the device drew with (the harness's own check finds it) */
	if (site->stage_rank[0] | site->stage_rank[1] | site->stage_rank[2] | site->stage_rank[3])
	{
		exact = 0;
		expected.fallback = 0;
	}
	else
	{
		/* (the harness compares the OpenGL state with the plans it expects; what it finds is counted here, not failed:
		the pictures are what this test is about) */
		{
			int saved = failures;

			failures = 100000;
			check_against_plan(site, &expected, 1);
			pt_sweep.structural += failures > 100000;
			failures = saved;
		}
		exact = !expected.fallback && expected.plan.status == _nv2a_ff_plan_exact;
	}
	if (pt_only && strcmp(pt_only, "sweep"))
		pt_describe_draws(site, &expected);
	pt_compare_pictures(0, 0, 0, 0, 0, &result);
	if (exact)
	{
		/* the plan's own tolerance, and the texels' rounding in 16 bits */
		int tolerance = expected.plan.tolerance + (!strcmp(texture_bits_setting, "32") ? 2 : 40);

		pt_sweep.exact++;
		pt_compare_pictures(0, 0, 0, 0, tolerance, &result);
		/* (an alpha test or an alpha kill: the pixels whose alpha is within rounding of the reference go either way) */
		if (result.differing && (site->alpha_test || site->alpha_kill) && result.differing <= ref.fragments / (!strcmp(texture_bits_setting, "32") ? 25 : 8) + 4)
		{
			pt_sweep.borderline++;
		}
		else if (result.differing)
		{
			char name[200];

			pt_sweep.exact_failed++;
			snprintf(name, sizeof(name), "vs%d %s", pt_sweep.program, site->name);
			pt_expect_same(name, tolerance, 0);
		}
	}
	else
	{
		/* over the pixels of the quad */
		double mean = ref.fragments ? result.mean * (double)result.pixels / (double)ref.fragments : 0.0;

		if (expected.fallback)
		{
			pt_sweep.fallback++;
			pt_sweep.fallback_mean += mean;
		}
		else
		{
			pt_sweep.approximate++;
			pt_sweep.approximate_mean += mean;
			pt_sweep.buckets[mean < 8.0 ? 0 : mean < 24.0 ? 1 : mean < 64.0 ? 2 : 3]++;
			if (mean > pt_sweep.approximate_worst_mean)
			{
				pt_sweep.approximate_worst_mean = mean;
				strcpy(pt_sweep.worst_name, site->name);
			}
			/* further off than the compiler's own comparison found the plan to be: the device may not be drawing
			the plan as the compiler meant it */
			if (mean > expected.plan.mean_error * 2.5 + 10.0 || result.worst > expected.plan.maximum_error + 40.0f)
			{
				pt_sweep.suspects++;
				if (pt_sweep.suspects <= 400)
				{
					printf("# SUSPECT vs%d %s: mean difference %.1f, worst %d; the compiler measured mean %.1f, worst %.0f "
						"(%d passes, flags %lx)\n", pt_sweep.program, site->name, mean, result.worst, expected.plan.mean_error,
						expected.plan.maximum_error, expected.plan.pass_count, expected.plan.approximations);
				}
			}
		}
		if (verbose || (pt_only && strcmp(pt_only, "sweep")))
		{
			printf("# vs%d %s: %s, mean difference %.1f, worst %d (the compiler's own: mean %.1f, worst %.0f)\n", pt_sweep.program,
				site->name, expected.fallback ? "built-in plan" : "approximate plan", mean, result.worst,
				expected.plan.mean_error, expected.plan.maximum_error);
			if (pt_only && strcmp(pt_only, "sweep"))
				pt_dump(site->name);
		}
	}
	present();
}

static void pt_sweep_report(void)
{
	printf("# sweep vs%d: %lu states; %lu with textures the reference does not sample; %lu exact plans, %lu of them wrong (%lu more differ at a few pixels on the edge of an alpha test); "
		"%lu approximate, mean difference %.1f over the quad (%lu below 8, %lu below 24, %lu below 64, %lu above; the worst %.1f: %s); "
		"%lu built-in, mean difference %.1f; %lu states whose OpenGL state is not what the harness expects of the plan\n",
		pt_sweep.program, pt_sweep.sites, pt_sweep.unsupported, pt_sweep.exact,
		pt_sweep.exact_failed, pt_sweep.borderline, pt_sweep.approximate, pt_sweep.approximate ? pt_sweep.approximate_mean / pt_sweep.approximate : 0.0,
		pt_sweep.buckets[0], pt_sweep.buckets[1], pt_sweep.buckets[2], pt_sweep.buckets[3], pt_sweep.approximate_worst_mean,
		pt_sweep.worst_name, pt_sweep.fallback, pt_sweep.fallback ? pt_sweep.fallback_mean / pt_sweep.fallback : 0.0, pt_sweep.structural);
}

/* ----- plans for constants: a state drawn with constants that keep changing, frame after frame

The device keeps a few plans made for particular constants and, once it has made its share, draws with the newest of
them whatever the draw's constants (xgpu_fixed.c, plan_get). Measured: the mean difference from the Xbox's picture of
each draw, by frame, for states whose plan for all constants left textures out. */

static struct
{
	int sites;
	double by_frame[8];
	unsigned long draws_by_frame[8];
	double worst;
	char worst_name[128];
} pt_bound;

static void pt_bound_site(const struct site *site)
{
	struct expected expected;
	int frame, draw, stage;

	if (pt_bound.sites >= 12)
		return;
	/* a state whose general plan gave textures up, where the plans for constants come in */
	site_apply(site);
	expected_compute(site, &expected);
	if (site->stage_rank[0] | site->stage_rank[1] | site->stage_rank[2] | site->stage_rank[3])
		return;
	if (!(expected.candidates[1].approximations & _nv2a_ff_approximate_dropped_texture) && !expected.candidate_fallback[1])
		return;
	pt_bound.sites++;
	for (frame = 0; frame < 8; frame++)
	{
		for (draw = 0; draw < 12; draw++)
		{
			struct pt_result result;
			double mean;
			unsigned long seed = 0x1234u + (unsigned long)draw * 77u;

			pt_clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0x80607090UL, 1.0f);
			site_apply(site);
			for (stage = 0; stage < 4; stage++)
			{
				D3D__TextureState[stage][D3DTSS_MIPFILTER] = D3DTEXF_NONE;
				D3D__TextureState[stage][D3DTSS_ADDRESSU] = D3DTADDRESS_WRAP;
				D3D__TextureState[stage][D3DTSS_ADDRESSV] = D3DTADDRESS_WRAP;
				pt_filter(stage, D3DTEXF_LINEAR);
			}
			/* the constants of this draw: the same twelve sets every frame */
			for (stage = 0; stage < 8; stage++)
			{
				seed = seed * 1664525UL + 1013904223UL;
				D3D__RenderState[D3DRS_PSCONSTANT0_0 + stage] ^= (seed >> 8) & 0x00ffffffUL;
				seed = seed * 1664525UL + 1013904223UL;
				D3D__RenderState[D3DRS_PSCONSTANT1_0 + stage] ^= (seed >> 8) & 0x00ffffffUL;
			}
			ref_unsupported = 0;
			pt_counts_reset();
			pt_draw(D3DPT_QUADLIST, 0, 4);
			if (ref_unsupported)
			{
				pt_bound.sites--;
				return;
			}
			pt_compare_pictures(0, 0, 0, 0, 0, &result);
			mean = ref.fragments ? result.mean * (double)result.pixels / (double)ref.fragments : 0.0;
			pt_bound.by_frame[frame] += mean;
			pt_bound.draws_by_frame[frame]++;
			if (frame >= 4 && mean > pt_bound.worst)
			{
				pt_bound.worst = mean;
				snprintf(pt_bound.worst_name, sizeof(pt_bound.worst_name), "%s (draw %d)", site->name, draw);
			}
		}
		present();
	}
}

static void pt_test_bound(void)
{
	static const D3DVIEWPORT8 viewport = { 0, 0, 640, 480, 0.0f, 1.0f };
	int frame;

	test_name = "bound";
	pt_window(640, 480);
	pt_SetViewport(&viewport);
	memset(&pt_bound, 0, sizeof(pt_bound));
	constants_fill();
	constants_screen(640.0f, 480.0f);
	pt_SetVertexShader(vertex_shaders[4]);
	screen_quad(100.0f, 100.0f, 164.0f, 164.0f, 0);
	sites_enumerate(pt_bound_site);
	printf("# bound: %d states with textures given up, 12 sets of constants a frame; mean difference by frame:", pt_bound.sites);
	for (frame = 0; frame < 8; frame++)
		printf(" %.1f", pt_bound.draws_by_frame[frame] ? pt_bound.by_frame[frame] / pt_bound.draws_by_frame[frame] : 0.0);
	printf("; the worst draw from frame 4 on: %.1f, %s\n", pt_bound.worst, pt_bound.worst_name);
}

static void pt_test_sweep(void)
{
	static const D3DVIEWPORT8 viewport = { 0, 0, 640, 480, 0.0f, 1.0f };

	test_name = "sweep";
	pt_window(640, 480);
	pt_SetViewport(&viewport);
	/* the screen's program: one set of coordinates, the vertices' colours */
	memset(&pt_sweep, 0, sizeof(pt_sweep));
	pt_sweep.program = 4;
	constants_fill();
	constants_screen(640.0f, 480.0f);
	pt_SetVertexShader(vertex_shaders[4]);
	screen_quad(100.0f, 100.0f, 164.0f, 164.0f, 0);
	sites_enumerate(pt_sweep_site);
	pt_sweep_report();
	/* the transparent shaders' program on environment vertices: both colours, fog, four sets of coordinates */
	memset(&pt_sweep, 0, sizeof(pt_sweep));
	pt_sweep.program = 24;
	{
		struct environment_vertex *vertices = (struct environment_vertex *)streams.data[0];
		int index;

		for (index = 0; index < 4; index++)
		{
			pt_world_vertex(&vertices[index], index == 0 || index == 3 ? -0.5f : 0.5f, index < 2 ? -0.5f : 0.5f,
				index == 0 || index == 3 ? 5.0f : 7.0f, (float)(index == 1 || index == 2), (float)(index >> 1));
		}
	}
	constants_fill();
	constants_world();
	pt_SetVertexShader(vertex_shaders[24]);
	pt_SetStreamSource(0, &streams.buffer[0], sizeof(struct environment_vertex));
	sites_enumerate(pt_sweep_site);
	pt_sweep_report();
}

#include "t_picture_guard.h"

static void pt_run_tests(const char *only)
{
	pt_only = only;
	if (only && !strcmp(only, "list"))
	{
		pt_list_programs();
		return;
	}
	if (only && !strcmp(only, "guardself"))
	{
		/* the guard pages themselves: each of these must end the test with an exception */
		volatile unsigned char *guest = guest_alloc_guarded(20);
		volatile unsigned char *block = guard_malloc(20);
		const char *which = getenv("PT_GUARDSELF");

		test_name = "guardself";
		guest[19] = 1;
		block[19] = 1;
		if (which && !strcmp(which, "guest"))
			guest[20] = 1;
		else if (which && !strcmp(which, "block"))
			printf("%d", block[20]);
		printf("# guardself: no exception\n");
		return;
	}
	if (pt_wanted("screen"))
		pt_test_screen();
	if (pt_wanted("world"))
		pt_test_world();
	if (pt_wanted("formats"))
		pt_test_formats();
	if (pt_wanted("projective"))
		pt_test_projective();
	if (pt_wanted("clears"))
		pt_test_clears();
	if (pt_wanted("frame"))
		pt_test_frame();
	if (pt_wanted("bias"))
		pt_test_bias();
	if (pt_wanted("sensor"))
		pt_test_sensor();
	if (pt_wanted("guard"))
		pt_test_guard();
	if (only && !strcmp(only, "bound"))
		pt_test_bound();
	if (!only || !strcmp(only, "sweep") || (strcmp(only, "screen") && strcmp(only, "world") && strcmp(only, "formats") &&
		strcmp(only, "projective") && strcmp(only, "clears") && strcmp(only, "frame") && strcmp(only, "bias") && strcmp(only, "sensor") && strcmp(only, "guard") && strcmp(only, "bound")))
	{
		pt_test_sweep();
	}
}
