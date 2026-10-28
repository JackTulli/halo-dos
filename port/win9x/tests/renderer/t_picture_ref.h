/*
T_PICTURE_REF.H

What the Xbox draws, computed without the light renderer: the vertex program
interpreted instruction by instruction (the meaning nv2a_vsh.c's GLSL gives
each), triangles rasterized in Direct3D's screen space (pixel centres on
whole numbers, rows from the top, depth 0 to 1), textures read from the
Xbox's own texel formats, the pixel state by nv2a_ff_reference (which is
nv2a_psh.c's GLSL in C), Direct3D's blend, depth and alpha tests.

The picture is made at the window's resolution, so that it can be laid over
what the software OpenGL drew: a window pixel's centre is the sample point.

Included by t_picture.c.
*/

/* ---------- what the test told the device (the wrappers in t_picture.c keep these) */

static float pt_constants[XGPU_VERTEX_CONSTANT_COUNT][4];
static D3DVIEWPORT8 pt_viewport = { 0, 0, 640, 480, 0.0f, 1.0f };
static struct
{
	const unsigned char *data;
	unsigned long stride;
} pt_streams[4];
static int pt_program = -1;
static D3DBaseTexture *pt_textures[4];
static D3DPalette *pt_palettes[4];
static float pt_attributes[16][4];

/* ---------- the vertex program */

struct ref_vertex
{
	/* oPos: the screen's x, y and depth (c[-38], c[-37]), and the clip space w */
	float position[4];
	float d0[4], d1[4];
	float fog;
	float t[4][4];
};

static unsigned long ref_field(const DWORD *instruction, int word, int low_bit, int bit_count)
{
	return (instruction[word] >> low_bit) & ((1UL << bit_count) - 1);
}

static const DWORD *ref_program_words(int program, unsigned long *count)
{
	unsigned long word = 0;
	int index;

	for (index = 0; index < program; index++)
	{
		word += 1 + (vertex_shader_code[word] >> 16) * 4;
		if (!vertex_shader_code[word])
			word++;
	}
	*count = vertex_shader_code[word] >> 16;
	return &vertex_shader_code[word + 1];
}

static void ref_operand(const DWORD *instruction, int which, int relative, int a0, float r[13][4], float o[13][4],
	const float v[16][4], float *out)
{
	unsigned long negate, swizzle[4], index, mux;
	const float *source;
	static const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	int component;

	if (which == 0)
	{
		negate = ref_field(instruction, 1, 8, 1);
		swizzle[0] = ref_field(instruction, 1, 6, 2); swizzle[1] = ref_field(instruction, 1, 4, 2);
		swizzle[2] = ref_field(instruction, 1, 2, 2); swizzle[3] = ref_field(instruction, 1, 0, 2);
		index = ref_field(instruction, 2, 28, 4);
		mux = ref_field(instruction, 2, 26, 2);
	}
	else if (which == 1)
	{
		negate = ref_field(instruction, 2, 25, 1);
		swizzle[0] = ref_field(instruction, 2, 23, 2); swizzle[1] = ref_field(instruction, 2, 21, 2);
		swizzle[2] = ref_field(instruction, 2, 19, 2); swizzle[3] = ref_field(instruction, 2, 17, 2);
		index = ref_field(instruction, 2, 13, 4);
		mux = ref_field(instruction, 2, 11, 2);
	}
	else
	{
		negate = ref_field(instruction, 2, 10, 1);
		swizzle[0] = ref_field(instruction, 2, 8, 2); swizzle[1] = ref_field(instruction, 2, 6, 2);
		swizzle[2] = ref_field(instruction, 2, 4, 2); swizzle[3] = ref_field(instruction, 2, 2, 2);
		index = (ref_field(instruction, 2, 0, 2) << 2) | ref_field(instruction, 3, 30, 2);
		mux = ref_field(instruction, 3, 28, 2);
	}
	switch (mux)
	{
	case 1: source = index == 12 ? o[0] : r[index]; break;
	case 2: source = v[ref_field(instruction, 1, 9, 4)]; break;
	case 3:
	{
		long constant = (long)ref_field(instruction, 1, 13, 8);

		if (relative)
		{
			constant += a0;
			constant = constant < 0 ? 0 : constant > XGPU_VERTEX_CONSTANT_COUNT - 1 ? XGPU_VERTEX_CONSTANT_COUNT - 1 : constant;
		}
		source = pt_constants[constant];
		break;
	}
	default: source = zero; break;
	}
	for (component = 0; component < 4; component++)
		out[component] = negate ? -source[swizzle[component]] : source[swizzle[component]];
}

static void ref_write(float *destination, const float *value, unsigned long mask)
{
	if (mask & 8) destination[0] = value[0];
	if (mask & 4) destination[1] = value[1];
	if (mask & 2) destination[2] = value[2];
	if (mask & 1) destination[3] = value[3];
}

/* the output components the program writes (four bits a register, bit 3 x) and the inputs it reads */
static void ref_program_usage(int program, unsigned long written[13], unsigned long *inputs)
{
	unsigned long count, index;
	const DWORD *words = ref_program_words(program, &count);

	memset(written, 0, 13 * sizeof(written[0]));
	*inputs = 0;
	for (index = 0; index < count; index++)
	{
		const DWORD *instruction = words + index * 4;
		unsigned long mac = ref_field(instruction, 1, 21, 4), ilu = ref_field(instruction, 1, 25, 3);
		unsigned long output_mask = ref_field(instruction, 3, 12, 4), address = ref_field(instruction, 3, 3, 8);

		if (output_mask && ref_field(instruction, 3, 11, 1) && address < 13 && (ref_field(instruction, 3, 2, 1) ? ilu : mac))
			written[address] |= output_mask;
		if ((mac && ref_field(instruction, 2, 26, 2) == 2) || (mac && mac != 1 && mac != 3 && mac != 13 && ref_field(instruction, 2, 11, 2) == 2) ||
			((ilu || mac == 3 || mac == 4) && ref_field(instruction, 3, 28, 2) == 2))
		{
			*inputs |= 1UL << ref_field(instruction, 1, 9, 4);
		}
		if (ref_field(instruction, 3, 0, 1))
			break;
	}
}

static void ref_program_run(int program, const float v[16][4], struct ref_vertex *vertex)
{
	unsigned long count, index;
	const DWORD *words = ref_program_words(program, &count);
	float r[13][4], o[13][4], mac[4] = { 0 }, ilu[4] = { 0 };
	int a0 = 0, component, address;

	memset(r, 0, sizeof(r));
	memset(o, 0, sizeof(o));
	for (address = 0; address < 13; address++)
		o[address][3] = 1.0f;
	o[5][0] = o[5][1] = o[5][2] = 1.0f;
	for (index = 0; index < count; index++)
	{
		const DWORD *instruction = words + index * 4;
		unsigned long mac_op = ref_field(instruction, 1, 21, 4), ilu_op = ref_field(instruction, 1, 25, 3);
		unsigned long mac_mask = ref_field(instruction, 3, 24, 4), temporary = ref_field(instruction, 3, 20, 4);
		unsigned long ilu_mask = ref_field(instruction, 3, 16, 4), output_mask = ref_field(instruction, 3, 12, 4);
		unsigned long output_is_register = ref_field(instruction, 3, 11, 1), output_address = ref_field(instruction, 3, 3, 8);
		unsigned long output_from_ilu = ref_field(instruction, 3, 2, 1);
		int relative = (int)ref_field(instruction, 3, 1, 1);
		float A[4], B[4], C[4], dot;

		ref_operand(instruction, 0, relative, a0, r, o, v, A);
		ref_operand(instruction, 1, relative, a0, r, o, v, B);
		ref_operand(instruction, 2, relative, a0, r, o, v, C);
		switch (mac_op)
		{
		case 0: break;
		case 1: case 13: memcpy(mac, A, sizeof(mac)); break;
		case 2: for (component = 0; component < 4; component++) mac[component] = A[component] * B[component]; break;
		case 3: for (component = 0; component < 4; component++) mac[component] = A[component] + C[component]; break;
		case 4: for (component = 0; component < 4; component++) mac[component] = A[component] * B[component] + C[component]; break;
		case 5: dot = A[0] * B[0] + A[1] * B[1] + A[2] * B[2]; mac[0] = mac[1] = mac[2] = mac[3] = dot; break;
		case 6: dot = A[0] * B[0] + A[1] * B[1] + A[2] * B[2] + B[3]; mac[0] = mac[1] = mac[2] = mac[3] = dot; break;
		case 7: dot = A[0] * B[0] + A[1] * B[1] + A[2] * B[2] + A[3] * B[3]; mac[0] = mac[1] = mac[2] = mac[3] = dot; break;
		case 8: mac[0] = 1.0f; mac[1] = A[1] * B[1]; mac[2] = A[2]; mac[3] = B[3]; break;
		case 9: for (component = 0; component < 4; component++) mac[component] = A[component] < B[component] ? A[component] : B[component]; break;
		case 10: for (component = 0; component < 4; component++) mac[component] = A[component] > B[component] ? A[component] : B[component]; break;
		case 11: for (component = 0; component < 4; component++) mac[component] = A[component] < B[component] ? 1.0f : 0.0f; break;
		case 12: for (component = 0; component < 4; component++) mac[component] = A[component] >= B[component] ? 1.0f : 0.0f; break;
		default: memset(mac, 0, sizeof(mac)); break;
		}
		switch (ilu_op)
		{
		case 0: break;
		case 1: memcpy(ilu, C, sizeof(ilu)); break;
		case 2: ilu[0] = ilu[1] = ilu[2] = ilu[3] = 1.0f / C[0]; break;
		case 3:
		{
			float reciprocal = 1.0f / C[0];

			if (reciprocal > 0.0f)
				reciprocal = reciprocal < 5.42101e-20f ? 5.42101e-20f : reciprocal > 1.884467e+19f ? 1.884467e+19f : reciprocal;
			else
				reciprocal = reciprocal < -1.884467e+19f ? -1.884467e+19f : reciprocal > -5.42101e-20f ? -5.42101e-20f : reciprocal;
			ilu[0] = ilu[1] = ilu[2] = ilu[3] = reciprocal;
			break;
		}
		case 4: ilu[0] = ilu[1] = ilu[2] = ilu[3] = 1.0f / sqrtf(fabsf(C[0])); break;
		case 5: ilu[0] = powf(2.0f, floorf(C[0])); ilu[1] = C[0] - floorf(C[0]); ilu[2] = powf(2.0f, C[0]); ilu[3] = 1.0f; break;
		case 6:
		{
			float x = fabsf(C[0]);

			if (x == 0.0f)
			{
				ilu[0] = -1.0e30f; ilu[1] = 1.0f; ilu[2] = -1.0e30f; ilu[3] = 1.0f;
			}
			else
			{
				float e = floorf(log2f(x));

				ilu[0] = e; ilu[1] = x / powf(2.0f, e); ilu[2] = log2f(x); ilu[3] = 1.0f;
			}
			break;
		}
		case 7:
		{
			float power = C[3] < -127.9961f ? -127.9961f : C[3] > 127.9961f ? 127.9961f : C[3];

			ilu[0] = 1.0f;
			ilu[1] = C[0] > 0.0f ? C[0] : 0.0f;
			ilu[2] = C[0] > 0.0f ? powf(C[1] > 0.0f ? C[1] : 0.0f, power) : 0.0f;
			ilu[3] = 1.0f;
			break;
		}
		default: memset(ilu, 0, sizeof(ilu)); break;
		}
		if (mac_op == 13)
			a0 = (int)floorf(mac[0] + 0.001f);
		else if (mac_op != 0 && mac_mask)
			ref_write(temporary == 12 ? o[0] : r[temporary], mac, mac_mask);
		if (ilu_op != 0 && ilu_mask)
		{
			unsigned long ilu_temporary = mac_op != 0 ? 1 : temporary;

			ref_write(ilu_temporary == 12 ? o[0] : r[ilu_temporary], ilu, ilu_mask);
		}
		if (output_mask && (output_from_ilu ? ilu_op : mac_op) != 0 && output_is_register && output_address < 13)
			ref_write(o[output_address], output_from_ilu ? ilu : mac, output_mask);
		if (ref_field(instruction, 3, 0, 1))
			break;
	}
	memcpy(vertex->position, o[0], sizeof(vertex->position));
	for (component = 0; component < 4; component++)
	{
		vertex->d0[component] = o[3][component] < 0.0f ? 0.0f : o[3][component] > 1.0f ? 1.0f : o[3][component];
		vertex->d1[component] = o[4][component] < 0.0f ? 0.0f : o[4][component] > 1.0f ? 1.0f : o[4][component];
	}
	vertex->fog = o[5][0];
	memcpy(vertex->t, o[9], sizeof(vertex->t));
}

/* ---------- vertex streams: Direct3D's vertex data types */

static unsigned long ref_type_bytes(unsigned long type)
{
	switch (type)
	{
	case D3DVSDT_FLOAT1: return 4;
	case D3DVSDT_FLOAT2: return 8;
	case D3DVSDT_FLOAT3: return 12;
	case D3DVSDT_FLOAT4: return 16;
	case D3DVSDT_D3DCOLOR: return 4;
	case D3DVSDT_SHORT1: case D3DVSDT_NORMSHORT1: return 2;
	case D3DVSDT_SHORT2: case D3DVSDT_NORMSHORT2: return 4;
	case D3DVSDT_SHORT3: case D3DVSDT_NORMSHORT3: return 6;
	case D3DVSDT_SHORT4: case D3DVSDT_NORMSHORT4: return 8;
	case D3DVSDT_NORMPACKED3: return 4;
	case D3DVSDT_PBYTE1: return 1;
	case D3DVSDT_PBYTE2: return 2;
	case D3DVSDT_PBYTE3: return 3;
	case D3DVSDT_PBYTE4: return 4;
	default: return 0;
	}
}

static void ref_read_element(unsigned long type, const unsigned char *data, float *value)
{
	int count, index;

	value[0] = value[1] = value[2] = 0.0f;
	value[3] = 1.0f;
	switch (type)
	{
	case D3DVSDT_FLOAT1: case D3DVSDT_FLOAT2: case D3DVSDT_FLOAT3: case D3DVSDT_FLOAT4:
		count = type == D3DVSDT_FLOAT1 ? 1 : type == D3DVSDT_FLOAT2 ? 2 : type == D3DVSDT_FLOAT3 ? 3 : 4;
		memcpy(value, data, (size_t)count * 4);
		break;
	case D3DVSDT_D3DCOLOR:
		value[0] = data[2] / 255.0f; value[1] = data[1] / 255.0f; value[2] = data[0] / 255.0f; value[3] = data[3] / 255.0f;
		break;
	case D3DVSDT_SHORT1: case D3DVSDT_SHORT2: case D3DVSDT_SHORT3: case D3DVSDT_SHORT4:
		count = type == D3DVSDT_SHORT1 ? 1 : type == D3DVSDT_SHORT2 ? 2 : type == D3DVSDT_SHORT3 ? 3 : 4;
		for (index = 0; index < count; index++)
		{
			short word;

			memcpy(&word, data + index * 2, 2);
			value[index] = (float)word;
		}
		break;
	case D3DVSDT_NORMSHORT1: case D3DVSDT_NORMSHORT2: case D3DVSDT_NORMSHORT3: case D3DVSDT_NORMSHORT4:
		count = type == D3DVSDT_NORMSHORT1 ? 1 : type == D3DVSDT_NORMSHORT2 ? 2 : type == D3DVSDT_NORMSHORT3 ? 3 : 4;
		for (index = 0; index < count; index++)
		{
			short word;

			memcpy(&word, data + index * 2, 2);
			value[index] = (float)word / 32767.0f;
		}
		break;
	case D3DVSDT_NORMPACKED3:
	{
		unsigned int packed;

		memcpy(&packed, data, 4);
		value[0] = (float)((int)(packed << 21) >> 21) / 1023.0f;
		value[1] = (float)((int)(packed << 10) >> 21) / 1023.0f;
		value[2] = (float)((int)packed >> 22) / 511.0f;
		break;
	}
	case D3DVSDT_PBYTE1: case D3DVSDT_PBYTE2: case D3DVSDT_PBYTE3: case D3DVSDT_PBYTE4:
		count = type == D3DVSDT_PBYTE1 ? 1 : type == D3DVSDT_PBYTE2 ? 2 : type == D3DVSDT_PBYTE3 ? 3 : 4;
		for (index = 0; index < count; index++)
			value[index] = data[index] / 255.0f;
		break;
	default:
		break;
	}
}

/* the registers of one vertex of the streams, by the program's declaration */
static void ref_fetch(int program, unsigned long vertex, float v[16][4])
{
	const DWORD *token = &vertex_shader_declarations[vertex_shader_declaration_offsets[program] / sizeof(DWORD)];
	unsigned long stream = 0, offsets[4] = { 0 };

	memcpy(v, pt_attributes, sizeof(pt_attributes));
	for (; *token != 0xffffffffUL; token++)
	{
		if ((*token >> 29) == 1)
		{
			stream = *token & 15;
		}
		else if ((*token >> 29) == 2)
		{
			unsigned long type = (*token >> 16) & 0xff, reg = *token & 15;

			if (pt_streams[stream].data)
				ref_read_element(type, pt_streams[stream].data + vertex * pt_streams[stream].stride + offsets[stream], v[reg]);
			offsets[stream] += ref_type_bytes(type);
		}
	}
}

/* ---------- textures: the Xbox's texels */

struct ref_texture
{
	unsigned long format, width, height, pitch;
	/* addressed in texels */
	int linear;
	const unsigned char *data;
	const DWORD *palette;
};

static int ref_texture_describe(const D3DBaseTexture *texture, const D3DPalette *palette, struct ref_texture *out)
{
	memset(out, 0, sizeof(*out));
	if (!texture || !texture->Data)
		return 0;
	out->format = (texture->Format & D3DFORMAT_FORMAT_MASK) >> D3DFORMAT_FORMAT_SHIFT;
	out->data = PLATFORM_PHYSICAL_TO_VIRTUAL(texture->Data);
	out->palette = palette && palette->Data ? PLATFORM_PHYSICAL_TO_VIRTUAL(palette->Data) : NULL;
	if (texture->Size)
	{
		out->width = (texture->Size & D3DSIZE_WIDTH_MASK) + 1;
		out->height = ((texture->Size & D3DSIZE_HEIGHT_MASK) >> D3DSIZE_HEIGHT_SHIFT) + 1;
		out->pitch = (((texture->Size & D3DSIZE_PITCH_MASK) >> D3DSIZE_PITCH_SHIFT) + 1) * D3DTEXTURE_PITCH_ALIGNMENT;
		out->linear = 1;
	}
	else
	{
		out->width = 1UL << ((texture->Format & D3DFORMAT_USIZE_MASK) >> D3DFORMAT_USIZE_SHIFT);
		out->height = 1UL << ((texture->Format & D3DFORMAT_VSIZE_MASK) >> D3DFORMAT_VSIZE_SHIFT);
	}
	return 1;
}

/* the index of a texel in a swizzled level: the bits of x and y alternate, x lowest, until one runs out */
static unsigned long ref_swizzle(unsigned long x, unsigned long y, unsigned long width, unsigned long height)
{
	unsigned long index = 0, bit = 0, level;

	for (level = 0; (1UL << level) < width || (1UL << level) < height; level++)
	{
		if ((1UL << level) < width)
			index |= ((x >> level) & 1) << bit++;
		if ((1UL << level) < height)
			index |= ((y >> level) & 1) << bit++;
	}
	return index;
}

static unsigned ref_expand(unsigned value, int bits)
{
	return (value * 255 + ((1u << bits) - 1) / 2) / ((1u << bits) - 1);
}

static void ref_texel(const struct ref_texture *texture, long x, long y, double *out)
{
	unsigned long bytes, format = texture->format;
	const unsigned char *texel;
	unsigned rgba[4] = { 255, 255, 255, 255 };
	int swizzled, channel;

	switch (format)
	{
	case 0x0c: case 0x0e: case 0x0f:
	{
		unsigned char block[16][4];
		unsigned long block_bytes = format == 0x0c ? 8 : 16;
		GLenum gl_format = format == 0x0c ? GL_COMPRESSED_RGBA_S3TC_DXT1_EXT : format == 0x0e ? GL_COMPRESSED_RGBA_S3TC_DXT3_EXT :
			GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;

		sw_dxt_block(gl_format, texture->data + ((size_t)(y / 4) * ((texture->width + 3) / 4) + x / 4) * block_bytes, block);
		for (channel = 0; channel < 4; channel++)
			out[channel] = block[(y & 3) * 4 + (x & 3)][channel] / 255.0;
		return;
	}
	case 0x00: case 0x01: case 0x0b: case 0x13: case 0x19: case 0x1b: case 0x1f: bytes = 1; break;
	case 0x06: case 0x07: case 0x12: case 0x1e: bytes = 4; break;
	default: bytes = 2; break;
	}
	swizzled = !(format >= 0x10 && format <= 0x13) && !(format >= 0x1b && format <= 0x20) && !texture->linear;
	if (swizzled)
		texel = texture->data + ref_swizzle((unsigned long)x, (unsigned long)y, texture->width, texture->height) * bytes;
	else
		texel = texture->data + (size_t)y * (texture->pitch ? texture->pitch : texture->width * bytes) + (size_t)x * bytes;
	switch (format)
	{
	case 0x06: case 0x12: rgba[0] = texel[2]; rgba[1] = texel[1]; rgba[2] = texel[0]; rgba[3] = texel[3]; break;
	case 0x07: case 0x1e: rgba[0] = texel[2]; rgba[1] = texel[1]; rgba[2] = texel[0]; break;
	case 0x05: case 0x11:
	{
		unsigned word = texel[0] | (texel[1] << 8);

		rgba[0] = ref_expand(word >> 11, 5); rgba[1] = ref_expand((word >> 5) & 63, 6); rgba[2] = ref_expand(word & 31, 5);
		break;
	}
	case 0x02: case 0x10: case 0x03: case 0x1c:
	{
		unsigned word = texel[0] | (texel[1] << 8);

		rgba[0] = ref_expand((word >> 10) & 31, 5); rgba[1] = ref_expand((word >> 5) & 31, 5); rgba[2] = ref_expand(word & 31, 5);
		if (format == 0x02 || format == 0x10)
			rgba[3] = (word >> 15) * 255;
		break;
	}
	case 0x04: case 0x1d:
	{
		unsigned word = texel[0] | (texel[1] << 8);

		rgba[0] = ((word >> 8) & 15) * 17; rgba[1] = ((word >> 4) & 15) * 17; rgba[2] = (word & 15) * 17; rgba[3] = (word >> 12) * 17;
		break;
	}
	case 0x00: case 0x13: rgba[0] = rgba[1] = rgba[2] = texel[0]; break;
	case 0x01: case 0x1b: rgba[0] = rgba[1] = rgba[2] = rgba[3] = texel[0]; break;
	case 0x19: case 0x1f: rgba[3] = texel[0]; break;
	case 0x1a: case 0x20: rgba[0] = rgba[1] = rgba[2] = texel[0]; rgba[3] = texel[1]; break;
	case 0x0b:
	{
		DWORD color = texture->palette ? texture->palette[texel[0]] : 0;

		rgba[0] = (color >> 16) & 0xff; rgba[1] = (color >> 8) & 0xff; rgba[2] = color & 0xff; rgba[3] = color >> 24;
		break;
	}
	default: break;
	}
	for (channel = 0; channel < 4; channel++)
		out[channel] = rgba[channel] / 255.0;
}

static long ref_address(DWORD mode, long coordinate, long size)
{
	switch (mode)
	{
	case D3DTADDRESS_WRAP:
		coordinate %= size;
		return coordinate < 0 ? coordinate + size : coordinate;
	case D3DTADDRESS_MIRROR:
		coordinate %= 2 * size;
		if (coordinate < 0)
			coordinate += 2 * size;
		return coordinate < size ? coordinate : 2 * size - 1 - coordinate;
	default:
		/* Direct3D's clamp is to the edge texel */
		return coordinate < 0 ? 0 : coordinate >= size ? size - 1 : coordinate;
	}
}

/* a stage's texture at its coordinates (s, t already divided by q) */
static void ref_sample(const struct ref_texture *texture, int stage, double s, double t, double *out)
{
	DWORD *state = D3D__TextureState[stage];
	DWORD address_u = texture->linear ? D3DTADDRESS_CLAMP : state[D3DTSS_ADDRESSU];
	DWORD address_v = texture->linear ? D3DTADDRESS_CLAMP : state[D3DTSS_ADDRESSV];
	double u = texture->linear ? s : s * texture->width, v = texture->linear ? t : t * texture->height;
	int channel;

	if (state[D3DTSS_MAGFILTER] == D3DTEXF_POINT)
	{
		ref_texel(texture, ref_address(address_u, (long)floor(u), (long)texture->width),
			ref_address(address_v, (long)floor(v), (long)texture->height), out);
	}
	else
	{
		double fu = u - 0.5, fv = v - 0.5, corners[4][4];
		long x0 = (long)floor(fu), y0 = (long)floor(fv);
		double ax = fu - x0, ay = fv - y0;
		int corner;

		for (corner = 0; corner < 4; corner++)
		{
			ref_texel(texture, ref_address(address_u, x0 + (corner & 1), (long)texture->width),
				ref_address(address_v, y0 + (corner >> 1), (long)texture->height), corners[corner]);
		}
		for (channel = 0; channel < 4; channel++)
		{
			out[channel] = (corners[0][channel] * (1.0 - ax) + corners[1][channel] * ax) * (1.0 - ay) +
				(corners[2][channel] * (1.0 - ax) + corners[3][channel] * ax) * ay;
		}
	}
}

/* ---------- the frame */

static struct
{
	int width, height;
	/* rows from the top; red, green, blue, alpha */
	unsigned char *color;
	double *depth;
	/* a triangle's edge passes within a fiftieth of a pixel of the sample point: either renderer may take it or not */
	unsigned char *edge;
	/* where in the window the picture is, and window pixels to one of the back buffer */
	double left, top, scale;
	/* the frame as a texture: what a draw that samples the back buffer reads (a copy made when the test says) */
	unsigned char *copy;
	unsigned long fragments;
} ref;

static void ref_resize(void)
{
	double scale_x = rec.window_width / 640.0, scale_y = rec.window_height / 480.0;

	if (ref.width != rec.window_width || ref.height != rec.window_height || !ref.color)
	{
		free(ref.color); free(ref.depth); free(ref.edge); free(ref.copy);
		ref.width = rec.window_width;
		ref.height = rec.window_height;
		ref.color = calloc((size_t)ref.width * ref.height, 4);
		ref.depth = calloc((size_t)ref.width * ref.height, sizeof(double));
		ref.edge = calloc((size_t)ref.width * ref.height, 1);
		ref.copy = calloc((size_t)ref.width * ref.height, 4);
	}
	ref.scale = scale_x < scale_y ? scale_x : scale_y;
	/* (the picture is centred; an odd pixel left over goes to the right and to the top, as the device's blit
	of the other renderers has it) */
	ref.left = floor((rec.window_width - floor(640.0 * ref.scale)) / 2.0);
	ref.top = ceil((rec.window_height - floor(480.0 * ref.scale)) / 2.0);
}

/* Direct3D's Clear: rectangles of the back buffer (rows from the top), or all of it and the bars around it */
static void ref_clear(DWORD count, const D3DRECT *rectangles, DWORD flags, D3DCOLOR color, float z)
{
	unsigned char rgba[4];
	int x, y;
	DWORD index;

	ref_resize();
	rgba[0] = (unsigned char)(color >> 16); rgba[1] = (unsigned char)(color >> 8); rgba[2] = (unsigned char)color;
	rgba[3] = (unsigned char)(color >> 24);
	for (y = 0; y < ref.height; y++)
	{
		for (x = 0; x < ref.width; x++)
		{
			double bx = (x - ref.left + 0.5) / ref.scale, by = (y - ref.top + 0.5) / ref.scale;
			int inside = count == 0;

			for (index = 0; index < count && !inside; index++)
			{
				inside = bx >= rectangles[index].x1 && bx < rectangles[index].x2 && by >= rectangles[index].y1 &&
					by < rectangles[index].y2;
			}
			if (!inside)
				continue;
			if (flags & D3DCLEAR_TARGET_R) ref.color[((size_t)y * ref.width + x) * 4] = rgba[0];
			if (flags & D3DCLEAR_TARGET_G) ref.color[((size_t)y * ref.width + x) * 4 + 1] = rgba[1];
			if (flags & D3DCLEAR_TARGET_B) ref.color[((size_t)y * ref.width + x) * 4 + 2] = rgba[2];
			if (flags & D3DCLEAR_TARGET_A) ref.color[((size_t)y * ref.width + x) * 4 + 3] = rgba[3];
			if (flags & D3DCLEAR_ZBUFFER)
				ref.depth[(size_t)y * ref.width + x] = z;
		}
	}
	memset(ref.edge, 0, (size_t)ref.width * ref.height);
}

static void ref_copy_frame(void)
{
	memcpy(ref.copy, ref.color, (size_t)ref.width * ref.height * 4);
}

/* the frame's copy as the texture a back-buffer header describes: texel coordinates of the 640x480 back buffer */
static void ref_sample_frame(double s, double t, double *out)
{
	/* the copy is at the window's resolution; point sampled at the nearest window pixel */
	long x = (long)floor(ref.left + s * ref.scale), y = (long)floor(ref.top + t * ref.scale);
	int channel;

	x = x < 0 ? 0 : x >= ref.width ? ref.width - 1 : x;
	y = y < 0 ? 0 : y >= ref.height ? ref.height - 1 : y;
	for (channel = 0; channel < 4; channel++)
		out[channel] = ref.copy[((size_t)y * ref.width + x) * 4 + channel] / 255.0;
}

static void ref_blend_factor(DWORD factor, const double *source, const double *destination, const double *constant, double *out)
{
	int channel;

	for (channel = 0; channel < 4; channel++)
	{
		switch (factor)
		{
		case D3DBLEND_ZERO: out[channel] = 0.0; break;
		case D3DBLEND_ONE: out[channel] = 1.0; break;
		case D3DBLEND_SRCCOLOR: out[channel] = source[channel]; break;
		case D3DBLEND_INVSRCCOLOR: out[channel] = 1.0 - source[channel]; break;
		case D3DBLEND_SRCALPHA: out[channel] = source[3]; break;
		case D3DBLEND_INVSRCALPHA: out[channel] = 1.0 - source[3]; break;
		case D3DBLEND_DESTALPHA: out[channel] = destination[3]; break;
		case D3DBLEND_INVDESTALPHA: out[channel] = 1.0 - destination[3]; break;
		case D3DBLEND_DESTCOLOR: out[channel] = destination[channel]; break;
		case D3DBLEND_INVDESTCOLOR: out[channel] = 1.0 - destination[channel]; break;
		case D3DBLEND_SRCALPHASAT:
			out[channel] = channel == 3 ? 1.0 : (source[3] < 1.0 - destination[3] ? source[3] : 1.0 - destination[3]);
			break;
		case D3DBLEND_CONSTANTCOLOR: out[channel] = constant[channel]; break;
		case D3DBLEND_INVCONSTANTCOLOR: out[channel] = 1.0 - constant[channel]; break;
		case D3DBLEND_CONSTANTALPHA: out[channel] = constant[3]; break;
		case D3DBLEND_INVCONSTANTALPHA: out[channel] = 1.0 - constant[3]; break;
		default: out[channel] = 1.0; break;
		}
	}
}

static int ref_compare(DWORD function, double value, double reference)
{
	switch (function)
	{
	case D3DCMP_NEVER: return 0;
	case D3DCMP_LESS: return value < reference;
	case D3DCMP_EQUAL: return value == reference;
	case D3DCMP_LESSEQUAL: return value <= reference;
	case D3DCMP_GREATER: return value > reference;
	case D3DCMP_NOTEQUAL: return value != reference;
	case D3DCMP_GREATEREQUAL: return value >= reference;
	default: return 1;
	}
}

/* the pixel state as nv2a_ff_reference takes it, from the render states */
static struct nv2a_ff_state ref_state;
static struct nv2a_ff_constants ref_constants;
static struct ref_texture ref_stage_textures[4];
static int ref_stage_sampled[4], ref_stage_frame[4];
/* a stage has a texture mode this file does not sample (cube maps, dot products, bump maps) */
static int ref_unsupported;
/* triangles the reference left out: a vertex behind the eye (it does not clip) */
static int ref_behind_eye;

static void ref_argb(DWORD argb, float *color)
{
	color[0] = ((argb >> 16) & 0xff) / 255.0f; color[1] = ((argb >> 8) & 0xff) / 255.0f; color[2] = (argb & 0xff) / 255.0f;
	color[3] = (argb >> 24) / 255.0f;
}

static float ref_float(DWORD bits)
{
	float value;

	memcpy(&value, &bits, 4);
	return value;
}

static void ref_pixel_state(unsigned long back_buffer_data)
{
	DWORD *rs = D3D__RenderState;
	int stage, index;

	memset(&ref_state, 0, sizeof(ref_state));
	memset(&ref_constants, 0, sizeof(ref_constants));
	ref_unsupported = 0;
	for (index = 0; index < NV2A_FF_COMBINER_STATE_COUNT; index++)
		ref_state.combiner_state[index] = rs[index];
	for (index = 0; index < 16; index++)
		ref_state.combiner_state[D3DRS_PSCONSTANT0_0 + index] = 0;
	ref_state.combiner_state[D3DRS_PSFINALCOMBINERCONSTANT0] = ref_state.combiner_state[D3DRS_PSFINALCOMBINERCONSTANT1] = 0;
	ref_state.texture_modes = rs[D3DRS_PSTEXTUREMODES];
	for (stage = 0; stage < 4; stage++)
	{
		unsigned long mode = (rs[D3DRS_PSTEXTUREMODES] >> (5 * stage)) & 0x1f;

		ref_stage_sampled[stage] = ref_stage_frame[stage] = 0;
		if (mode == 1 && pt_textures[stage] && pt_textures[stage]->Data)
		{
			ref_state.sampler_type[stage] = 1;
			ref_stage_sampled[stage] = 1;
			if (pt_textures[stage]->Data == back_buffer_data)
				ref_stage_frame[stage] = 1;
			else
				ref_texture_describe(pt_textures[stage], pt_palettes[stage], &ref_stage_textures[stage]);
			if ((pt_textures[stage]->Format & D3DFORMAT_CUBEMAP) ||
				((pt_textures[stage]->Format & D3DFORMAT_DIMENSION_MASK) >> D3DFORMAT_DIMENSION_SHIFT) == 3)
			{
				ref_unsupported = 1;
			}
		}
		else if (mode != 0 && !(mode == 1 && (!pt_textures[stage] || !pt_textures[stage]->Data)))
		{
			ref_unsupported = 1;
		}
		ref_state.alpha_kill[stage] = D3D__TextureState[stage][D3DTSS_ALPHAKILL] == D3DTALPHAKILL_ENABLE;
		ref_state.color_sign[stage] = (unsigned char)((D3D__TextureState[stage][D3DTSS_COLORSIGN] >> 28) & 0xf);
	}
	ref_state.alpha_test_function = rs[D3DRS_ALPHATESTENABLE] ? rs[D3DRS_ALPHAFUNC] : 0;
	ref_state.fog_enable = rs[D3DRS_FOGENABLE] != 0;
	ref_state.fog_table_mode = (unsigned char)rs[D3DRS_FOGTABLEMODE];
	for (index = 0; index < 8; index++)
	{
		ref_argb(rs[D3DRS_PSCONSTANT0_0 + index], ref_constants.c0[index]);
		ref_argb(rs[D3DRS_PSCONSTANT1_0 + index], ref_constants.c1[index]);
	}
	ref_argb(rs[D3DRS_PSFINALCOMBINERCONSTANT0], ref_constants.final_c0);
	ref_argb(rs[D3DRS_PSFINALCOMBINERCONSTANT1], ref_constants.final_c1);
	ref_argb(rs[D3DRS_FOGCOLOR], ref_constants.fog_color);
	ref_constants.fog_parameters[0] = ref_float(rs[D3DRS_FOGSTART]);
	ref_constants.fog_parameters[1] = ref_float(rs[D3DRS_FOGEND]);
	ref_constants.fog_parameters[2] = ref_float(rs[D3DRS_FOGDENSITY]);
	ref_constants.alpha_reference = (float)(rs[D3DRS_ALPHAREF] & 0xff);
	ref_argb(rs[D3DRS_BLENDCOLOR], ref_constants.blend_color);
	for (stage = 0; stage < 4; stage++)
	{
		ref_constants.bump_luminance[stage][0] = ref_float(D3D__TextureState[stage][D3DTSS_BUMPENVLSCALE]);
		ref_constants.bump_luminance[stage][1] = ref_float(D3D__TextureState[stage][D3DTSS_BUMPENVLOFFSET]);
	}
}

#define REF_VARYINGS 25

static void ref_fragment(int x, int y, double depth, const double *varying)
{
	DWORD *rs = D3D__RenderState;
	size_t pixel = (size_t)y * ref.width + x;
	unsigned char *stored = ref.color + pixel * 4;
	struct nv2a_ff_fragment fragment;
	double source[4], destination[4], constant[4], source_factor[4], destination_factor[4], result[4];
	float color[4];
	int stage, channel;

	/* (the view volume's near and far planes) */
	if (depth < (pt_viewport.MinZ < pt_viewport.MaxZ ? pt_viewport.MinZ : pt_viewport.MaxZ) - 1.0e-9 ||
		depth > (pt_viewport.MinZ < pt_viewport.MaxZ ? pt_viewport.MaxZ : pt_viewport.MinZ) + 1.0e-9)
	{
		return;
	}
	memset(&fragment, 0, sizeof(fragment));
	for (channel = 0; channel < 4; channel++)
	{
		fragment.v0[channel] = (float)varying[channel];
		fragment.v1[channel] = (float)varying[4 + channel];
	}
	fragment.fog = (float)varying[8];
	for (stage = 0; stage < 4; stage++)
	{
		const double *coordinates = varying + 9 + stage * 4;
		double texel[4] = { 0.0, 0.0, 0.0, 1.0 };

		for (channel = 0; channel < 4; channel++)
			fragment.texcoord[stage][channel] = (float)coordinates[channel];
		if (ref_stage_frame[stage])
			ref_sample_frame(coordinates[0] / coordinates[3], coordinates[1] / coordinates[3], texel);
		else if (ref_stage_sampled[stage])
			ref_sample(&ref_stage_textures[stage], stage, coordinates[0] / coordinates[3], coordinates[1] / coordinates[3], texel);
		for (channel = 0; channel < 4; channel++)
			fragment.texel[stage][channel] = (float)texel[channel];
	}
	if (!nv2a_ff_reference(&ref_state, &ref_constants, &fragment, color))
		return;
	if (rs[D3DRS_ZENABLE])
	{
		if (!ref_compare(rs[D3DRS_ZFUNC], depth, ref.depth[pixel]))
			return;
		if (rs[D3DRS_ZWRITEENABLE])
			ref.depth[pixel] = depth;
	}
	for (channel = 0; channel < 4; channel++)
	{
		source[channel] = color[channel] < 0.0f ? 0.0 : color[channel] > 1.0f ? 1.0 : color[channel];
		destination[channel] = stored[channel] / 255.0;
		constant[channel] = ref_constants.blend_color[channel];
		result[channel] = source[channel];
	}
	if (rs[D3DRS_ALPHABLENDENABLE])
	{
		ref_blend_factor(rs[D3DRS_SRCBLEND], source, destination, constant, source_factor);
		ref_blend_factor(rs[D3DRS_DESTBLEND], source, destination, constant, destination_factor);
		for (channel = 0; channel < 4; channel++)
		{
			switch (rs[D3DRS_BLENDOP])
			{
			case D3DBLENDOP_SUBTRACT:
				result[channel] = source[channel] * source_factor[channel] - destination[channel] * destination_factor[channel];
				break;
			case D3DBLENDOP_REVSUBTRACT:
				result[channel] = destination[channel] * destination_factor[channel] - source[channel] * source_factor[channel];
				break;
			case D3DBLENDOP_MIN: result[channel] = source[channel] < destination[channel] ? source[channel] : destination[channel]; break;
			case D3DBLENDOP_MAX: result[channel] = source[channel] > destination[channel] ? source[channel] : destination[channel]; break;
			default:
				result[channel] = source[channel] * source_factor[channel] + destination[channel] * destination_factor[channel];
				break;
			}
		}
	}
	if (rs[D3DRS_COLORWRITEENABLE] & D3DCOLORWRITEENABLE_RED) stored[0] = sw_byte(result[0]);
	if (rs[D3DRS_COLORWRITEENABLE] & D3DCOLORWRITEENABLE_GREEN) stored[1] = sw_byte(result[1]);
	if (rs[D3DRS_COLORWRITEENABLE] & D3DCOLORWRITEENABLE_BLUE) stored[2] = sw_byte(result[2]);
	if (rs[D3DRS_COLORWRITEENABLE] & D3DCOLORWRITEENABLE_ALPHA) stored[3] = sw_byte(result[3]);
	ref.fragments++;
}

static void ref_triangle(const struct ref_vertex *a, const struct ref_vertex *b, const struct ref_vertex *c)
{
	const struct ref_vertex *vertices[3] = { a, b, c };
	DWORD *rs = D3D__RenderState;
	/* the viewport's depth range in oPos.z's units */
	double z_scale = pt_constants[XGPU_VERTEX_CONSTANT_BIAS - 38][2], z_offset = pt_constants[XGPU_VERTEX_CONSTANT_BIAS - 37][2];
	double screen[3][3], inverse_w[3], attributes[3][REF_VARYINGS], area;
	int x0, y0, x1, y1, x, y, index, channel;

	for (index = 0; index < 3; index++)
	{
		const struct ref_vertex *vertex = vertices[index];

		if (!(vertex->position[3] > 0.0f))
		{
			ref_behind_eye++;
			return;
		}
		screen[index][0] = vertex->position[0];
		screen[index][1] = vertex->position[1];
		/* depth: 0 at the viewport's MinZ... as the depth buffer holds it, 0 to 1 */
		screen[index][2] = pt_viewport.MinZ + (z_scale != 0.0 ? (vertex->position[2] - z_offset) / z_scale : 0.0) *
			(pt_viewport.MaxZ - pt_viewport.MinZ);
		inverse_w[index] = 1.0 / vertex->position[3];
		for (channel = 0; channel < 4; channel++)
		{
			attributes[index][channel] = vertex->d0[channel];
			attributes[index][4 + channel] = vertex->d1[channel];
			attributes[index][9 + channel] = vertex->t[0][channel];
			attributes[index][13 + channel] = vertex->t[1][channel];
			attributes[index][17 + channel] = vertex->t[2][channel];
			attributes[index][21 + channel] = vertex->t[3][channel];
		}
		attributes[index][8] = vertex->fog;
	}
	area = (screen[1][0] - screen[0][0]) * (screen[2][1] - screen[0][1]) - (screen[2][0] - screen[0][0]) * (screen[1][1] - screen[0][1]);
	if (area == 0.0)
		return;
	/* on a screen whose rows count from the top, a positive area is clockwise as seen; the cull mode names the
	winding that is not drawn */
	if (rs[D3DRS_CULLMODE] != D3DCULL_NONE && (area > 0.0) == (rs[D3DRS_CULLMODE] == D3DCULL_CW))
		return;
	/* the window pixels whose sample points the triangle may hold */
	x0 = y0 = 1 << 30;
	x1 = y1 = -(1 << 30);
	for (index = 0; index < 3; index++)
	{
		double wx = ref.left + (screen[index][0] + 0.5) * ref.scale, wy = ref.top + (screen[index][1] + 0.5) * ref.scale;

		if (wx - 2.0 < x0) x0 = (int)floor(wx - 2.0);
		if (wy - 2.0 < y0) y0 = (int)floor(wy - 2.0);
		if (wx + 2.0 > x1) x1 = (int)ceil(wx + 2.0);
		if (wy + 2.0 > y1) y1 = (int)ceil(wy + 2.0);
	}
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > ref.width) x1 = ref.width;
	if (y1 > ref.height) y1 = ref.height;
	for (y = y0; y < y1; y++)
	{
		for (x = x0; x < x1; x++)
		{
			/* the sample point in the back buffer's screen coordinates */
			double px = (x - ref.left + 0.5) / ref.scale - 0.5, py = (y - ref.top + 0.5) / ref.scale - 0.5;
			double weights[3], sum = 0.0, depth = 0.0, varying[REF_VARYINGS], nearest = 1.0e30;
			int inside = 1;

			weights[0] = ((screen[1][0] - px) * (screen[2][1] - py) - (screen[2][0] - px) * (screen[1][1] - py)) / area;
			weights[1] = ((screen[2][0] - px) * (screen[0][1] - py) - (screen[0][0] - px) * (screen[2][1] - py)) / area;
			weights[2] = 1.0 - weights[0] - weights[1];
			for (index = 0; index < 3; index++)
			{
				/* the distance to the edge opposite the vertex, in window pixels */
				const double *p = screen[(index + 1) % 3], *q = screen[(index + 2) % 3];
				double length = sqrt((q[0] - p[0]) * (q[0] - p[0]) + (q[1] - p[1]) * (q[1] - p[1]));
				double distance = length > 0.0 ? fabs(weights[index] * area) / length * ref.scale : 0.0;

				if (distance < nearest)
					nearest = distance;
				if (weights[index] < 0.0)
					inside = 0;
			}
			/* (the viewport's rectangle: Direct3D clips to it) */
			if (px + 0.5 < pt_viewport.X || px + 0.5 > pt_viewport.X + pt_viewport.Width || py + 0.5 < pt_viewport.Y ||
				py + 0.5 > pt_viewport.Y + pt_viewport.Height)
			{
				continue;
			}
			if (nearest < 0.02 && (inside || (weights[0] > -0.5 && weights[1] > -0.5 && weights[2] > -0.5)))
				ref.edge[(size_t)y * ref.width + x] = 1;
			if (!inside)
				continue;
			for (index = 0; index < 3; index++)
			{
				depth += weights[index] * screen[index][2];
				weights[index] *= inverse_w[index];
				sum += weights[index];
			}
			for (channel = 0; channel < REF_VARYINGS; channel++)
			{
				varying[channel] = (weights[0] * attributes[0][channel] + weights[1] * attributes[1][channel] +
					weights[2] * attributes[2][channel]) / sum;
			}
			ref_fragment(x, y, depth, varying);
		}
	}
}

/* a draw, as the test asked the device for it: the vertices are those of the streams (or `immediate`, sixteen
registers of four floats a vertex) */
static void ref_draw(D3DPRIMITIVETYPE type, unsigned long first, unsigned long count, const unsigned short *indices,
	const float *immediate, unsigned long back_buffer_data)
{
	struct ref_vertex *vertices = malloc(count * sizeof(*vertices));
	unsigned long index;
	float c38[4], c37[4];

	ref_resize();
	/* Direct3D's own constants: the viewport's scale and offset, depth in the depth buffer's units */
	c38[0] = pt_viewport.Width * 0.5f; c38[1] = -(float)pt_viewport.Height * 0.5f;
	c38[2] = 16777215.0f * (pt_viewport.MaxZ - pt_viewport.MinZ); c38[3] = 0.0f;
	c37[0] = pt_viewport.X + pt_viewport.Width * 0.5f; c37[1] = pt_viewport.Y + pt_viewport.Height * 0.5f;
	c37[2] = 16777215.0f * pt_viewport.MinZ; c37[3] = 0.0f;
	memcpy(pt_constants[XGPU_VERTEX_CONSTANT_BIAS - 38], c38, sizeof(c38));
	memcpy(pt_constants[XGPU_VERTEX_CONSTANT_BIAS - 37], c37, sizeof(c37));
	ref_pixel_state(back_buffer_data);
	for (index = 0; index < count; index++)
	{
		float v[16][4];

		if (immediate)
			memcpy(v, immediate + index * 64, sizeof(v));
		else
			ref_fetch(pt_program, indices ? indices[index] : first + index, v);
		ref_program_run(pt_program, (const float (*)[4])v, &vertices[index]);
	}
	switch (type)
	{
	case D3DPT_TRIANGLELIST:
		for (index = 0; index + 2 < count; index += 3)
			ref_triangle(&vertices[index], &vertices[index + 1], &vertices[index + 2]);
		break;
	case D3DPT_TRIANGLESTRIP:
		for (index = 0; index + 2 < count; index++)
		{
			if (index & 1)
				ref_triangle(&vertices[index + 1], &vertices[index], &vertices[index + 2]);
			else
				ref_triangle(&vertices[index], &vertices[index + 1], &vertices[index + 2]);
		}
		break;
	case D3DPT_TRIANGLEFAN:
	case D3DPT_POLYGON:
		for (index = 1; index + 1 < count; index++)
			ref_triangle(&vertices[0], &vertices[index], &vertices[index + 1]);
		break;
	case D3DPT_QUADLIST:
		for (index = 0; index + 3 < count; index += 4)
		{
			ref_triangle(&vertices[index], &vertices[index + 1], &vertices[index + 2]);
			ref_triangle(&vertices[index], &vertices[index + 2], &vertices[index + 3]);
		}
		break;
	case D3DPT_QUADSTRIP:
		for (index = 0; index + 3 < count; index += 2)
		{
			ref_triangle(&vertices[index], &vertices[index + 1], &vertices[index + 3]);
			ref_triangle(&vertices[index], &vertices[index + 3], &vertices[index + 2]);
		}
		break;
	default:
		break;
	}
	free(vertices);
}
