/*
T_PICTURE_GUARD.H

Memory (t_picture.c): what the device reads of the Xbox's memory ends where
the draw's data ends. Streams, indices, texels and palettes are put at the
end of pages whose successors have no access; with pg.sh's build the device's
own allocations are too.
*/

static unsigned long pt_declaration_stride(int program, unsigned long stream_wanted)
{
	const DWORD *token = &vertex_shader_declarations[vertex_shader_declaration_offsets[program] / sizeof(DWORD)];
	unsigned long stream = 0, stride = 0;

	for (; *token != 0xffffffffUL; token++)
	{
		if ((*token >> 29) == 1)
			stream = *token & 15;
		else if ((*token >> 29) == 2 && stream == stream_wanted)
			stride += ref_type_bytes((*token >> 16) & 0xff);
	}
	return stride;
}

static void pt_fill_floats(unsigned char *memory, unsigned long size)
{
	unsigned long index;

	for (index = 0; index + 4 <= size; index += 4)
	{
		float value = 0.25f + (float)(test_random() % 1000) * 0.002f;

		memcpy(memory + index, &value, 4);
	}
}

static void pt_test_guard(void)
{
	static const unsigned long counts[] = { 1, 2, 3, 4, 5, 7, 8, 15, 16, 17, 31, 33, 63, 64, 65, 100 };
	static D3DVertexBuffer buffers[2];
	struct site site;
	int program;
	unsigned long which, stream, draws = 0;

	test_name = "guard";
	pt_window(640, 480);
	pt_state_texture_times_diffuse(&site);
	constants_fill();
	constants_world();
	constants_screen(640.0f, 480.0f);
	pt_apply(&site);
	pt_SetTexture(0, stage_textures[0]);
	/* every program with its own vertex format, the streams ending at pages without access: unindexed draws that
	end at the last vertex, and indexed ones whose indices end at such a page too */
	for (program = 0; program < NUMBER_OF_VERTEX_SHADERS; program++)
	{
		for (which = 0; which < sizeof(counts) / sizeof(counts[0]); which++)
		{
			unsigned long count = counts[which], first = which & 3, index;
			unsigned short *indices;

			for (stream = 0; stream < 2; stream++)
			{
				unsigned long stride = pt_declaration_stride(program, stream);

				if (!stride)
				{
					pt_SetStreamSource(stream, NULL, 0);
					continue;
				}
				{
					unsigned char *memory = guest_alloc_guarded((first + count) * stride);

					pt_fill_floats(memory, (first + count) * stride);
					memset(&buffers[stream], 0, sizeof(buffers[stream]));
					buffers[stream].Common = 1;
					buffers[stream].Data = PLATFORM_VIRTUAL_TO_PHYSICAL(memory);
					pt_SetStreamSource(stream, &buffers[stream], stride);
				}
			}
			pt_SetVertexShader(vertex_shaders[program]);
			/* (what the positions come to is not this test's business: the recorder's own checks are quiet here) */
			fail_quiet = 1;
			begin_draw();
			D3DDevice_DrawVertices(count >= 3 ? D3DPT_TRIANGLESTRIP : D3DPT_POINTLIST, first, count);
			indices = guest_alloc_guarded(count * 2);
			for (index = 0; index < count; index++)
				indices[index] = (unsigned short)(first + (index * 7) % count);
			/* (the last vertex of the stream is among them) */
			indices[count / 2] = (unsigned short)(first + count - 1);
			begin_draw();
			D3DDevice_DrawIndexedVertices(count >= 3 ? D3DPT_TRIANGLELIST : D3DPT_POINTLIST, count, indices);
			fail_quiet = 0;
			draws += 2;
		}
		present();
		/* (the test's own memory is not without end) */
		if (guest_used > GUEST_SIZE - 8UL * 1024 * 1024)
			break;
	}
	printf("# guard: %lu draws through %d programs with their streams and indices at the end of their memory; %lu device "
		"allocations guarded\n", draws, program, guard_allocations);
	pt_SetStreamSource(1, NULL, 0);

	/* textures and palettes whose last byte is the last of their memory: every format, with and without levels */
	{
		static const struct
		{
			DWORD format;
			unsigned long width, height, levels, bytes;
			int linear;
		} textures[] =
		{
			{ 0x06, 64, 64, 1, 4, 0 }, { 0x06, 64, 64, 7, 4, 0 }, { 0x06, 32, 8, 6, 4, 0 }, { 0x06, 4, 64, 3, 4, 0 },
			{ 0x07, 16, 16, 5, 4, 0 }, { 0x05, 32, 32, 6, 2, 0 }, { 0x02, 32, 32, 1, 2, 0 }, { 0x04, 16, 32, 2, 2, 0 },
			{ 0x00, 32, 32, 6, 1, 0 }, { 0x01, 32, 32, 1, 1, 0 }, { 0x19, 8, 8, 4, 1, 0 }, { 0x1a, 32, 32, 6, 2, 0 },
			{ 0x0b, 32, 32, 6, 1, 0 }, { 0x0b, 128, 128, 1, 1, 0 }, { 0x0c, 32, 32, 1, 8, 0 }, { 0x0c, 64, 64, 7, 8, 0 },
			{ 0x0e, 32, 32, 6, 16, 0 }, { 0x0f, 16, 64, 7, 16, 0 }, { 0x0f, 8, 8, 4, 16, 0 }, { 0x0c, 4, 4, 3, 8, 0 },
			{ 0x12, 100, 60, 1, 4, 1 }, { 0x1e, 33, 7, 1, 4, 1 }, { 0x11, 64, 64, 1, 2, 1 }, { 0x10, 20, 90, 1, 2, 1 },
			{ 0x1d, 17, 17, 1, 2, 1 }, { 0x13, 50, 50, 1, 1, 1 }, { 0x20, 50, 40, 1, 2, 1 }, { 0x1f, 31, 3, 1, 1, 1 },
			{ 0x12, 1, 1, 1, 4, 1 }, { 0x06, 1, 1, 1, 4, 0 }, { 0x0c, 1, 1, 1, 8, 0 }, { 0x12, 640, 480, 1, 4, 1 },
		};
		D3DPalette palette;
		DWORD *colors = guest_alloc_guarded(1024);
		unsigned long texture_index, level, uploads = 0;
		int mip;

		for (level = 0; level < 256; level++)
			colors[level] = (DWORD)(test_random() | (test_random() << 16));
		memset(&palette, 0, sizeof(palette));
		palette.Common = 1;
		palette.Data = PLATFORM_VIRTUAL_TO_PHYSICAL(colors);
		constants_fill();
		constants_screen(640.0f, 480.0f);
		pt_SetVertexShader(vertex_shaders[4]);
		for (texture_index = 0; texture_index < sizeof(textures) / sizeof(textures[0]); texture_index++)
		{
			unsigned long width = textures[texture_index].width, height = textures[texture_index].height, size = 0, pitch = 0;
			D3DBaseTexture *texture = guest_alloc(sizeof(*texture));
			int compressed = textures[texture_index].format == 0x0c || textures[texture_index].format == 0x0e ||
				textures[texture_index].format == 0x0f;
			unsigned char *texels;

			memset(texture, 0, sizeof(*texture));
			texture->Common = 1;
			if (textures[texture_index].linear)
			{
				pitch = (width * textures[texture_index].bytes + D3DTEXTURE_PITCH_ALIGNMENT - 1) &
					~(unsigned long)(D3DTEXTURE_PITCH_ALIGNMENT - 1);
				size = pitch * height;
				texture->Format = (textures[texture_index].format << D3DFORMAT_FORMAT_SHIFT) | (2UL << D3DFORMAT_DIMENSION_SHIFT) |
					(1UL << D3DFORMAT_MIPMAP_SHIFT);
				texture->Size = ((pitch / D3DTEXTURE_PITCH_ALIGNMENT - 1) << D3DSIZE_PITCH_SHIFT) | ((height - 1) << D3DSIZE_HEIGHT_SHIFT) |
					(width - 1);
			}
			else
			{
				for (level = 0; level < textures[texture_index].levels; level++)
				{
					unsigned long level_width = width >> level ? width >> level : 1, level_height = height >> level ? height >> level : 1;

					size += compressed ? ((level_width + 3) / 4) * ((level_height + 3) / 4) * textures[texture_index].bytes :
						level_width * level_height * textures[texture_index].bytes;
				}
				texture->Format = (textures[texture_index].format << D3DFORMAT_FORMAT_SHIFT) | (2UL << D3DFORMAT_DIMENSION_SHIFT) |
					(textures[texture_index].levels << D3DFORMAT_MIPMAP_SHIFT) | (log2_of(width) << D3DFORMAT_USIZE_SHIFT) |
					(log2_of(height) << D3DFORMAT_VSIZE_SHIFT);
			}
			texels = guest_alloc_guarded(size);
			for (level = 0; level < size; level++)
				texels[level] = (unsigned char)test_random();
			texture->Data = PLATFORM_VIRTUAL_TO_PHYSICAL(texels);
			for (mip = 0; mip < 2; mip++)
			{
				char name[96];

				pt_clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, PT_CLEAR_COLOR, 1.0f);
				pt_apply(&site);
				pt_filter(0, D3DTEXF_LINEAR);
				D3D__TextureState[0][D3DTSS_MIPFILTER] = mip ? D3DTEXF_LINEAR : D3DTEXF_NONE;
				D3D__TextureState[0][D3DTSS_ADDRESSU] = textures[texture_index].linear ? D3DTADDRESS_CLAMP : D3DTADDRESS_WRAP;
				D3D__TextureState[0][D3DTSS_ADDRESSV] = textures[texture_index].linear ? D3DTADDRESS_CLAMP : D3DTADDRESS_WRAP;
				pt_SetTexture(0, texture);
				pt_SetPalette(0, textures[texture_index].format == 0x0b ? &palette : NULL);
				/* four window pixels to a texel: the first level is the one sampled, with a chain or without */
				screen_quad(8.0f, 8.0f, 8.0f + (float)(width > 150 ? 150 : width) * 4.0f, 8.0f + (float)(height > 110 ? 110 : height) * 4.0f, 8);
				{
					struct hud_vertex *vertices = (struct hud_vertex *)streams.data[0] + 8;
					float s = (width > 150 ? 150.0f / (float)width : 1.0f) * (textures[texture_index].linear ? (float)width : 1.0f);
					float t = (height > 110 ? 110.0f / (float)height : 1.0f) * (textures[texture_index].linear ? (float)height : 1.0f);

					vertices[1].coordinates[0] = vertices[2].coordinates[0] = s;
					vertices[2].coordinates[1] = vertices[3].coordinates[1] = t;
				}
				pt_counts_reset();
				pt_draw(D3DPT_QUADLIST, 8, 4);
				uploads++;
				snprintf(name, sizeof(name), "guarded texture %lu (format %02lx %lux%lu, %lu levels)%s", texture_index,
					(unsigned long)textures[texture_index].format, width, height, textures[texture_index].levels, mip ? ", mip filter" : "");
				/* (a texture above the context's size is made smaller: not compared) */
				if (width <= (unsigned long)driver.maximum_texture_size && height <= (unsigned long)driver.maximum_texture_size &&
					(allowed.edge_clamp || !textures[texture_index].linear))
				{
					pt_expect_same(name, !strcmp(texture_bits_setting, "32") ? 3 : 14, 0);
				}
				present();
			}
		}
		printf("# guard: %lu textures drawn from memory that ends with their last texel\n", uploads);
		pt_SetPalette(0, NULL);
		pt_SetTexture(0, stage_textures[0]);
	}
}
