/*
T_PICTURE_GL.H

A software OpenGL 1.1 (with the texture environment and blend extensions the
light renderer uses) behind t_fixed_device.c's recorder: the recorder keeps
the state, this keeps the texels and the window's colour and depth buffers
and rasterizes every draw. Written from the OpenGL 1.3 specification, with
nothing of the device in sight.

Included by t_picture.c after t_fixed_device.c.
*/

struct sw_texture
{
	int width, height;
	/* base internal format: 0 RGBA, 1 RGB, 2 LUMINANCE, 3 LUMINANCE_ALPHA, 4 INTENSITY, 5 ALPHA */
	int base;
	/* level 0 as red, green, blue, alpha bytes, as the texture environment sees them */
	unsigned char *texels;
	float border[4];
	/* a copy of the frame buffer */
	int is_copy;
};

static struct sw_texture sw_textures[REC_TEXTURES];

static struct
{
	int width, height;
	/* rows from the bottom, as OpenGL has them */
	unsigned char *color;
	double *depth;
	float clear_color[4];
	double clear_depth;
	unsigned long pixels_drawn, triangles_drawn;
	/* the depth the last fragment drawn had, for the tests of polygon offset */
	double last_depth;
} sw;

static void sw_resize(void)
{
	if (sw.width == rec.window_width && sw.height == rec.window_height && sw.color)
		return;
	free(sw.color);
	free(sw.depth);
	sw.width = rec.window_width;
	sw.height = rec.window_height;
	sw.color = calloc((size_t)sw.width * sw.height, 4);
	sw.depth = calloc((size_t)sw.width * sw.height, sizeof(double));
}

static unsigned char sw_byte(double value)
{
	return (unsigned char)(value <= 0.0 ? 0 : value >= 1.0 ? 255 : (int)(value * 255.0 + 0.5));
}

/* ----- clears */

static void APIENTRY sw_glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a)
{
	rec_glClearColor(r, g, b, a);
	sw.clear_color[0] = r; sw.clear_color[1] = g; sw.clear_color[2] = b; sw.clear_color[3] = a;
}

static void APIENTRY sw_glClearDepth(GLclampd depth)
{
	rec_glClearDepth(depth);
	sw.clear_depth = depth;
}

static void APIENTRY sw_glClear(GLbitfield mask)
{
	int x0 = 0, y0 = 0, x1, y1, x, y, channel;

	rec_glClear(mask);
	sw_resize();
	x1 = sw.width;
	y1 = sw.height;
	if (rec.state.scissor_test)
	{
		x0 = rec.state.scissor[0];
		y0 = rec.state.scissor[1];
		x1 = x0 + rec.state.scissor[2];
		y1 = y0 + rec.state.scissor[3];
		if (x0 < 0) x0 = 0;
		if (y0 < 0) y0 = 0;
		if (x1 > sw.width) x1 = sw.width;
		if (y1 > sw.height) y1 = sw.height;
	}
	for (y = y0; y < y1; y++)
	{
		for (x = x0; x < x1; x++)
		{
			if (mask & GL_COLOR_BUFFER_BIT)
			{
				for (channel = 0; channel < 4; channel++)
				{
					if (rec.state.color_mask & (1 << channel))
						sw.color[((size_t)y * sw.width + x) * 4 + channel] = sw_byte(sw.clear_color[channel]);
				}
			}
			if ((mask & GL_DEPTH_BUFFER_BIT) && rec.state.depth_mask)
				sw.depth[(size_t)y * sw.width + x] = sw.clear_depth;
		}
	}
}

/* ----- texture images */

static int sw_base_format(GLint internal)
{
	switch (internal)
	{
	case GL_RGB: case GL_RGB5: case GL_RGB8: case 3: return 1;
	case GL_LUMINANCE8: case GL_LUMINANCE: case 1: return 2;
	case GL_LUMINANCE8_ALPHA8: case GL_LUMINANCE_ALPHA: case 2: return 3;
	case GL_INTENSITY8: return 4;
	case GL_ALPHA8: case GL_ALPHA: return 5;
	default: return 0;
	}
}

/* the bits a channel keeps in an internal format (what a driver of the time
would store): red, green, blue, alpha */
static void sw_internal_bits(GLint internal, GLenum type, int bits[4])
{
	bits[0] = bits[1] = bits[2] = bits[3] = 8;
	switch (internal)
	{
	case GL_RGB5:
		bits[0] = bits[2] = 5;
		bits[1] = type == GL_UNSIGNED_SHORT_5_6_5 ? 6 : 5;
		break;
	case GL_RGB5_A1: bits[0] = bits[1] = bits[2] = 5; bits[3] = 1; break;
	case GL_RGBA4: bits[0] = bits[1] = bits[2] = bits[3] = 4; break;
	default: break;
	}
}

static unsigned char sw_quantize(unsigned value, int bits)
{
	unsigned levels = (1u << bits) - 1;
	unsigned level = (value * levels + 127) / 255;

	return (unsigned char)((level * 255 + levels / 2) / levels);
}

static struct sw_texture *sw_bound(void)
{
	GLuint name = rec.state.units[rec.state.active_unit].texture;

	return name && name < REC_TEXTURES ? &sw_textures[name] : NULL;
}

static void sw_store(struct sw_texture *texture, GLint internal, GLenum type, int width, int height, const unsigned char *rgba)
{
	int bits[4], index, channel;

	sw_internal_bits(internal, type, bits);
	free(texture->texels);
	texture->width = width;
	texture->height = height;
	texture->base = sw_base_format(internal);
	texture->is_copy = 0;
	texture->texels = malloc((size_t)width * height * 4);
	for (index = 0; index < width * height; index++)
	{
		unsigned char texel[4];

		memcpy(texel, rgba + index * 4, 4);
		for (channel = 0; channel < 4; channel++)
			texel[channel] = sw_quantize(texel[channel], bits[channel]);
		/* what the texture environment reads of each base format */
		switch (texture->base)
		{
		case 1: texel[3] = 255; break;
		case 2: texel[1] = texel[2] = texel[0]; texel[3] = 255; break;
		case 3: texel[1] = texel[2] = texel[0]; break;
		case 4: texel[1] = texel[2] = texel[3] = texel[0]; break;
		case 5: texel[0] = texel[1] = texel[2] = 0; break;
		default: break;
		}
		memcpy(texture->texels + index * 4, texel, 4);
	}
}

static void APIENTRY sw_glTexImage2D(GLenum target, GLint level, GLint internal, GLsizei width, GLsizei height, GLint border,
	GLenum format, GLenum type, const void *pixels)
{
	struct sw_texture *texture = sw_bound();
	unsigned char *rgba;
	int index;

	rec_glTexImage2D(target, level, internal, width, height, border, format, type, pixels);
	if (!texture || level != 0)
		return;
	rgba = calloc((size_t)width * height, 4);
	for (index = 0; pixels && index < width * height; index++)
	{
		unsigned char *out = rgba + index * 4;

		out[3] = 255;
		if (type == GL_UNSIGNED_BYTE)
		{
			const unsigned char *in = (const unsigned char *)pixels + index *
				(format == GL_LUMINANCE ? 1 : format == GL_LUMINANCE_ALPHA ? 2 : format == GL_RGB ? 3 : 4);

			switch (format)
			{
			case GL_LUMINANCE: out[0] = out[1] = out[2] = in[0]; break;
			case GL_LUMINANCE_ALPHA: out[0] = out[1] = out[2] = in[0]; out[3] = in[1]; break;
			case GL_RGB: out[0] = in[0]; out[1] = in[1]; out[2] = in[2]; break;
			case GL_BGRA: out[0] = in[2]; out[1] = in[1]; out[2] = in[0]; out[3] = in[3]; break;
			default: memcpy(out, in, 4); break;
			}
		}
		else
		{
			unsigned word = ((const unsigned short *)pixels)[index];

			if (type == GL_UNSIGNED_SHORT_5_6_5)
			{
				out[0] = (unsigned char)(((word >> 11) & 31) * 255 / 31);
				out[1] = (unsigned char)(((word >> 5) & 63) * 255 / 63);
				out[2] = (unsigned char)((word & 31) * 255 / 31);
			}
			else if (type == GL_UNSIGNED_SHORT_4_4_4_4)
			{
				out[0] = (unsigned char)(((word >> 12) & 15) * 17);
				out[1] = (unsigned char)(((word >> 8) & 15) * 17);
				out[2] = (unsigned char)(((word >> 4) & 15) * 17);
				out[3] = (unsigned char)((word & 15) * 17);
			}
			else
			{
				out[0] = (unsigned char)(((word >> 11) & 31) * 255 / 31);
				out[1] = (unsigned char)(((word >> 6) & 31) * 255 / 31);
				out[2] = (unsigned char)(((word >> 1) & 31) * 255 / 31);
				out[3] = (unsigned char)((word & 1) * 255);
			}
		}
	}
	sw_store(texture, internal, type, width, height, rgba);
	free(rgba);
}

/* S3TC, as the extension's specification gives it */
static void sw_dxt_block(GLenum format, const unsigned char *block, unsigned char out[16][4])
{
	const unsigned char *color_block = format == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT || format == 0x83f0 ? block : block + 8;
	unsigned c0 = color_block[0] | (color_block[1] << 8), c1 = color_block[2] | (color_block[3] << 8);
	unsigned bits = color_block[4] | (color_block[5] << 8) | (color_block[6] << 16) | ((unsigned)color_block[7] << 24);
	int colors[4][4], index, channel;

	colors[0][0] = ((c0 >> 11) & 31) * 255 / 31; colors[0][1] = ((c0 >> 5) & 63) * 255 / 63; colors[0][2] = (c0 & 31) * 255 / 31;
	colors[1][0] = ((c1 >> 11) & 31) * 255 / 31; colors[1][1] = ((c1 >> 5) & 63) * 255 / 63; colors[1][2] = (c1 & 31) * 255 / 31;
	colors[0][3] = colors[1][3] = colors[2][3] = colors[3][3] = 255;
	for (channel = 0; channel < 3; channel++)
	{
		if (c0 > c1 || (format != GL_COMPRESSED_RGBA_S3TC_DXT1_EXT && format != 0x83f0))
		{
			colors[2][channel] = (2 * colors[0][channel] + colors[1][channel]) / 3;
			colors[3][channel] = (colors[0][channel] + 2 * colors[1][channel]) / 3;
		}
		else
		{
			colors[2][channel] = (colors[0][channel] + colors[1][channel]) / 2;
			colors[3][channel] = 0;
			colors[3][3] = 0;
		}
	}
	for (index = 0; index < 16; index++)
	{
		int code = (bits >> (2 * index)) & 3;

		for (channel = 0; channel < 4; channel++)
			out[index][channel] = (unsigned char)colors[code][channel];
	}
	if (format == GL_COMPRESSED_RGBA_S3TC_DXT3_EXT)
	{
		for (index = 0; index < 16; index++)
			out[index][3] = (unsigned char)(((block[index / 2] >> (4 * (index & 1))) & 15) * 17);
	}
	else if (format == GL_COMPRESSED_RGBA_S3TC_DXT5_EXT)
	{
		int a0 = block[0], a1 = block[1], alphas[8];
		unsigned long long codes = 0;

		for (index = 0; index < 6; index++)
			codes |= (unsigned long long)block[2 + index] << (8 * index);
		alphas[0] = a0;
		alphas[1] = a1;
		for (index = 2; index < 8; index++)
		{
			if (a0 > a1)
				alphas[index] = ((8 - index) * a0 + (index - 1) * a1) / 7;
			else
				alphas[index] = index < 6 ? ((6 - index) * a0 + (index - 1) * a1) / 5 : index == 6 ? 0 : 255;
		}
		for (index = 0; index < 16; index++)
			out[index][3] = (unsigned char)alphas[(codes >> (3 * index)) & 7];
	}
}

static void sw_dxt_decode(GLenum format, const unsigned char *data, int width, int height, unsigned char *rgba)
{
	int block_bytes = format == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT || format == 0x83f0 ? 8 : 16;
	int bx, by, x, y;

	for (by = 0; by < (height + 3) / 4; by++)
	{
		for (bx = 0; bx < (width + 3) / 4; bx++)
		{
			unsigned char texels[16][4];

			sw_dxt_block(format, data + ((size_t)by * ((width + 3) / 4) + bx) * block_bytes, texels);
			for (y = 0; y < 4 && by * 4 + y < height; y++)
				for (x = 0; x < 4 && bx * 4 + x < width; x++)
					memcpy(rgba + ((size_t)(by * 4 + y) * width + bx * 4 + x) * 4, texels[y * 4 + x], 4);
		}
	}
}

static void APIENTRY sw_glCompressedTexImage2D(GLenum target, GLint level, GLenum internal, GLsizei width, GLsizei height,
	GLint border, GLsizei size, const void *data)
{
	struct sw_texture *texture = sw_bound();
	unsigned char *rgba;

	rec_glCompressedTexImage2D(target, level, internal, width, height, border, size, data);
	if (!texture || level != 0)
		return;
	rgba = calloc((size_t)width * height, 4);
	sw_dxt_decode(internal, data, width, height, rgba);
	sw_store(texture, GL_RGBA8, GL_UNSIGNED_BYTE, width, height, rgba);
	free(rgba);
}

static void APIENTRY sw_glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y,
	GLsizei width, GLsizei height)
{
	struct sw_texture *texture = sw_bound();
	int row, column;

	rec_glCopyTexSubImage2D(target, level, xoffset, yoffset, x, y, width, height);
	if (!texture || !texture->texels || level != 0)
		return;
	sw_resize();
	for (row = 0; row < height; row++)
	{
		for (column = 0; column < width; column++)
		{
			int tx = xoffset + column, ty = yoffset + row, fx = x + column, fy = y + row;

			if (tx < 0 || ty < 0 || tx >= texture->width || ty >= texture->height || fx < 0 || fy < 0 || fx >= sw.width ||
				fy >= sw.height)
			{
				continue;
			}
			memcpy(texture->texels + ((size_t)ty * texture->width + tx) * 4, sw.color + ((size_t)fy * sw.width + fx) * 4, 4);
			if (texture->base == 1)
				texture->texels[((size_t)ty * texture->width + tx) * 4 + 3] = 255;
		}
	}
	texture->is_copy = 1;
}

static void APIENTRY sw_glTexParameterfv(GLenum target, GLenum name, const GLfloat *value)
{
	struct sw_texture *texture = sw_bound();

	rec_glTexParameterfv(target, name, value);
	if (texture && name == GL_TEXTURE_BORDER_COLOR)
		memcpy(texture->border, value, 4 * sizeof(float));
}

static void APIENTRY sw_glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels)
{
	int row, column;

	/* (the recorder's own depth buffer, for its visibility test, when its column is set) */
	if (rec.depth_column || rec.depth_unreadable || !sw.depth)
	{
		rec_glReadPixels(x, y, width, height, format, type, pixels);
		return;
	}
	if (format == GL_DEPTH_COMPONENT && type == GL_FLOAT)
	{
		rec.depth_reads++;
		for (row = 0; row < height; row++)
			for (column = 0; column < width; column++)
				((float *)pixels)[row * width + column] = (float)sw.depth[(size_t)(y + row) * sw.width + x + column];
	}
	else
	{
		rec_glReadPixels(x, y, width, height, format, type, pixels);
	}
}

/* ----- sampling */

static int sw_wrap(GLint mode, int coordinate, int size, int *border)
{
	*border = 0;
	switch (mode)
	{
	case GL_REPEAT:
		coordinate %= size;
		return coordinate < 0 ? coordinate + size : coordinate;
	case GL_MIRRORED_REPEAT:
		coordinate %= 2 * size;
		if (coordinate < 0)
			coordinate += 2 * size;
		return coordinate < size ? coordinate : 2 * size - 1 - coordinate;
	case GL_CLAMP:
		/* (OpenGL 1.1's clamp: beyond the edge lies the border colour) */
		if (coordinate < 0 || coordinate >= size)
			*border = 1;
		return coordinate < 0 ? 0 : coordinate >= size ? size - 1 : coordinate;
	default:
		return coordinate < 0 ? 0 : coordinate >= size ? size - 1 : coordinate;
	}
}

static void sw_fetch(const struct sw_texture *texture, const struct rec_texture *parameters, int x, int y, double *texel)
{
	int border_x, border_y, channel;

	x = sw_wrap(parameters->wrap_s, x, texture->width, &border_x);
	y = sw_wrap(parameters->wrap_t, y, texture->height, &border_y);
	for (channel = 0; channel < 4; channel++)
	{
		texel[channel] = border_x || border_y ? texture->border[channel] :
			texture->texels[((size_t)y * texture->width + x) * 4 + channel] / 255.0;
	}
}

static void sw_sample(const struct sw_texture *texture, const struct rec_texture *parameters, double s, double t, double *texel)
{
	/* (level 0 alone: the tests draw textures magnified, or without mip levels) */
	int linear = parameters->mag_filter == GL_LINEAR;
	double u, v;
	int channel;

	if (parameters->wrap_s == GL_CLAMP)
		s = s < 0.0 ? 0.0 : s > 1.0 ? 1.0 : s;
	if (parameters->wrap_t == GL_CLAMP)
		t = t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t;
	u = s * texture->width;
	v = t * texture->height;
	if (!linear)
	{
		sw_fetch(texture, parameters, (int)floor(u), (int)floor(v), texel);
	}
	else
	{
		double fu = u - 0.5, fv = v - 0.5;
		int x0 = (int)floor(fu), y0 = (int)floor(fv);
		double ax = fu - x0, ay = fv - y0, corners[4][4];

		sw_fetch(texture, parameters, x0, y0, corners[0]);
		sw_fetch(texture, parameters, x0 + 1, y0, corners[1]);
		sw_fetch(texture, parameters, x0, y0 + 1, corners[2]);
		sw_fetch(texture, parameters, x0 + 1, y0 + 1, corners[3]);
		for (channel = 0; channel < 4; channel++)
		{
			texel[channel] = (corners[0][channel] * (1.0 - ax) + corners[1][channel] * ax) * (1.0 - ay) +
				(corners[2][channel] * (1.0 - ax) + corners[3][channel] * ax) * ay;
		}
	}
}

/* ----- the texture environment (OpenGL 1.3, tables 3.22 to 3.24) */

static double sw_clamp(double value)
{
	return value < 0.0 ? 0.0 : value > 1.0 ? 1.0 : value;
}

static void sw_environment(const struct rec_unit *unit, int base, const double *texel, const double *primary,
	double *color /* previous in, result out */)
{
	const double *constant_f = NULL;
	double constant[4], previous[4], result[4];
	int channel, side;

	for (channel = 0; channel < 4; channel++)
	{
		constant[channel] = unit->env_color[channel];
		previous[channel] = color[channel];
		result[channel] = color[channel];
	}
	(void)constant_f;
	switch (unit->mode)
	{
	case GL_REPLACE:
		if (base != 5)
			for (channel = 0; channel < 3; channel++) result[channel] = texel[channel];
		if (base == 0 || base == 3 || base == 4 || base == 5)
			result[3] = texel[3];
		break;
	case GL_MODULATE:
		if (base != 5)
			for (channel = 0; channel < 3; channel++) result[channel] = previous[channel] * texel[channel];
		if (base == 0 || base == 3 || base == 4 || base == 5)
			result[3] = previous[3] * texel[3];
		break;
	case GL_DECAL:
		CHECK(base == 0 || base == 1, "GL_DECAL on a texture of base format %d: undefined", base);
		for (channel = 0; channel < 3; channel++)
			result[channel] = base == 1 ? texel[channel] : previous[channel] * (1.0 - texel[3]) + texel[channel] * texel[3];
		break;
	case GL_BLEND:
		if (base != 5)
			for (channel = 0; channel < 3; channel++)
				result[channel] = previous[channel] * (1.0 - texel[channel]) + constant[channel] * texel[channel];
		if (base == 4)
			result[3] = previous[3] * (1.0 - texel[3]) + constant[3] * texel[3];
		else if (base == 0 || base == 3 || base == 5)
			result[3] = previous[3] * texel[3];
		break;
	case GL_ADD:
		if (base != 5)
			for (channel = 0; channel < 3; channel++) result[channel] = previous[channel] + texel[channel];
		if (base == 4)
			result[3] = previous[3] + texel[3];
		else if (base == 0 || base == 3 || base == 5)
			result[3] = previous[3] * texel[3];
		break;
	case GL_COMBINE:
		for (side = 0; side < 2; side++)
		{
			double arguments[3][4];
			int argument;

			for (argument = 0; argument < 3; argument++)
			{
				GLenum source = unit->source[side][argument], operand = unit->operand[side][argument];
				const double *value = source == GL_TEXTURE ? texel : source == GL_CONSTANT ? constant :
					source == GL_PRIMARY_COLOR ? primary : previous;

				for (channel = 0; channel < 4; channel++)
				{
					switch (operand)
					{
					case GL_SRC_COLOR: arguments[argument][channel] = value[channel]; break;
					case GL_ONE_MINUS_SRC_COLOR: arguments[argument][channel] = 1.0 - value[channel]; break;
					case GL_SRC_ALPHA: arguments[argument][channel] = value[3]; break;
					default: arguments[argument][channel] = 1.0 - value[3]; break;
					}
				}
			}
			for (channel = side ? 3 : 0; channel < (side ? 4 : 3); channel++)
			{
				double a0 = arguments[0][channel], a1 = arguments[1][channel], a2 = arguments[2][channel], value;

				switch (unit->combine[side])
				{
				case GL_REPLACE: value = a0; break;
				case GL_MODULATE: value = a0 * a1; break;
				case GL_ADD: value = a0 + a1; break;
				case GL_ADD_SIGNED: value = a0 + a1 - 0.5; break;
				case GL_INTERPOLATE: value = a0 * a2 + a1 * (1.0 - a2); break;
				case GL_SUBTRACT: value = a0 - a1; break;
				case GL_DOT3_RGB:
					value = 4.0 * ((arguments[0][0] - 0.5) * (arguments[1][0] - 0.5) + (arguments[0][1] - 0.5) * (arguments[1][1] - 0.5) +
						(arguments[0][2] - 0.5) * (arguments[1][2] - 0.5));
					break;
				default: value = a0; fail("combine function %04x", (unsigned)unit->combine[side]); break;
				}
				result[channel] = value * unit->scale[side];
			}
		}
		break;
	default:
		fail("texture environment mode %04x", (unsigned)unit->mode);
		break;
	}
	for (channel = 0; channel < 4; channel++)
		color[channel] = sw_clamp(result[channel]);
}

/* ----- blending and tests */

static int sw_compare(GLenum function, double value, double reference)
{
	switch (function)
	{
	case GL_NEVER: return 0;
	case GL_LESS: return value < reference;
	case GL_EQUAL: return value == reference;
	case GL_LEQUAL: return value <= reference;
	case GL_GREATER: return value > reference;
	case GL_NOTEQUAL: return value != reference;
	case GL_GEQUAL: return value >= reference;
	default: return 1;
	}
}

static void sw_blend_factor(GLenum factor, const double *source, const double *destination, const float *constant,
	int is_source, double *out)
{
	int channel;

	/* OpenGL before 1.4: the source's own colour is no source factor, the destination's no destination factor */
	if (driver.version_number < 140)
	{
		CHECK(!(is_source && (factor == GL_SRC_COLOR || factor == GL_ONE_MINUS_SRC_COLOR)),
			"blend source factor %04x needs OpenGL 1.4", (unsigned)factor);
		CHECK(!(!is_source && (factor == GL_DST_COLOR || factor == GL_ONE_MINUS_DST_COLOR)),
			"blend destination factor %04x needs OpenGL 1.4", (unsigned)factor);
	}
	CHECK(is_source || factor != GL_SRC_ALPHA_SATURATE, "GL_SRC_ALPHA_SATURATE as a destination factor");
	for (channel = 0; channel < 4; channel++)
	{
		switch (factor)
		{
		case GL_ZERO: out[channel] = 0.0; break;
		case GL_ONE: out[channel] = 1.0; break;
		case GL_SRC_COLOR: out[channel] = source[channel]; break;
		case GL_ONE_MINUS_SRC_COLOR: out[channel] = 1.0 - source[channel]; break;
		case GL_SRC_ALPHA: out[channel] = source[3]; break;
		case GL_ONE_MINUS_SRC_ALPHA: out[channel] = 1.0 - source[3]; break;
		case GL_DST_ALPHA: out[channel] = destination[3]; break;
		case GL_ONE_MINUS_DST_ALPHA: out[channel] = 1.0 - destination[3]; break;
		case GL_DST_COLOR: out[channel] = destination[channel]; break;
		case GL_ONE_MINUS_DST_COLOR: out[channel] = 1.0 - destination[channel]; break;
		case GL_SRC_ALPHA_SATURATE:
			out[channel] = channel == 3 ? 1.0 : (source[3] < 1.0 - destination[3] ? source[3] : 1.0 - destination[3]);
			break;
		case GL_CONSTANT_COLOR: out[channel] = constant[channel]; break;
		case GL_ONE_MINUS_CONSTANT_COLOR: out[channel] = 1.0 - constant[channel]; break;
		case GL_CONSTANT_ALPHA: out[channel] = constant[3]; break;
		case GL_ONE_MINUS_CONSTANT_ALPHA: out[channel] = 1.0 - constant[3]; break;
		default: out[channel] = 1.0; fail("blend factor %04x", (unsigned)factor); break;
		}
	}
}

/* ----- rasterization */

#define SW_VARYINGS 20

struct sw_vertex
{
	double clip[4];
	/* colour, then four units' s, t, r, q */
	double varying[SW_VARYINGS];
};

static void sw_vertex_lerp(const struct sw_vertex *a, const struct sw_vertex *b, double t, struct sw_vertex *out)
{
	int index;

	for (index = 0; index < 4; index++)
		out->clip[index] = a->clip[index] + (b->clip[index] - a->clip[index]) * t;
	for (index = 0; index < SW_VARYINGS; index++)
		out->varying[index] = a->varying[index] + (b->varying[index] - a->varying[index]) * t;
}

/* a polygon against one plane: the distance of a vertex is d(v) >= 0 inside */
static int sw_clip_plane(struct sw_vertex *polygon, int count, int plane)
{
	struct sw_vertex result[12];
	int index, out = 0;

	for (index = 0; index < count; index++)
	{
		const struct sw_vertex *a = &polygon[index], *b = &polygon[(index + 1) % count];
		double da = plane == 0 ? a->clip[3] + a->clip[2] : plane == 1 ? a->clip[3] - a->clip[2] : a->clip[3] - 1.0e-6;
		double db = plane == 0 ? b->clip[3] + b->clip[2] : plane == 1 ? b->clip[3] - b->clip[2] : b->clip[3] - 1.0e-6;

		if (da >= 0.0)
			result[out++] = *a;
		if ((da >= 0.0) != (db >= 0.0))
			sw_vertex_lerp(a, b, da / (da - db), &result[out++]);
	}
	memcpy(polygon, result, (size_t)out * sizeof(result[0]));
	return out;
}

static void sw_fragment(int x, int y, double depth, const double *varying)
{
	size_t pixel = (size_t)y * sw.width + x;
	unsigned char *stored = sw.color + pixel * 4;
	double color[4], primary[4], destination[4], result[4];
	int unit, channel;

	for (channel = 0; channel < 4; channel++)
		color[channel] = primary[channel] = sw_clamp(varying[channel]);
	for (unit = 0; unit < REC_UNITS; unit++)
	{
		const struct rec_unit *state = &rec.state.units[unit];
		const struct sw_texture *texture;
		const double *coordinates = varying + 4 + unit * 4;
		double texel[4];

		if (!state->enabled || !state->texture || state->texture >= REC_TEXTURES)
			continue;
		texture = &sw_textures[state->texture];
		if (!texture->texels || !rec_texture_complete(&rec.textures[state->texture]))
			continue;
		sw_sample(texture, &rec.textures[state->texture], coordinates[0] / coordinates[3], coordinates[1] / coordinates[3], texel);
		sw_environment(state, texture->base, texel, primary, color);
	}
	if (rec.state.alpha_test && !sw_compare(rec.state.alpha_function, color[3], rec.state.alpha_reference))
		return;
	if (rec.state.depth_test && driver.depth_bits)
	{
		if (!sw_compare(rec.state.depth_function, depth, sw.depth[pixel]))
			return;
		if (rec.state.depth_mask)
			sw.depth[pixel] = depth;
	}
	sw.last_depth = depth;
	for (channel = 0; channel < 4; channel++)
		destination[channel] = stored[channel] / 255.0;
	if (!driver.alpha_bits)
		destination[3] = 1.0;
	memcpy(result, color, sizeof(result));
	if (rec.state.blend)
	{
		double source_factor[4], destination_factor[4];

		sw_blend_factor(rec.state.blend_source, color, destination, rec.state.blend_color, 1, source_factor);
		sw_blend_factor(rec.state.blend_destination, color, destination, rec.state.blend_color, 0, destination_factor);
		for (channel = 0; channel < 4; channel++)
		{
			switch (rec.state.blend_equation)
			{
			case GL_FUNC_SUBTRACT:
				result[channel] = color[channel] * source_factor[channel] - destination[channel] * destination_factor[channel];
				break;
			case GL_FUNC_REVERSE_SUBTRACT:
				result[channel] = destination[channel] * destination_factor[channel] - color[channel] * source_factor[channel];
				break;
			case GL_MIN: result[channel] = color[channel] < destination[channel] ? color[channel] : destination[channel]; break;
			case GL_MAX: result[channel] = color[channel] > destination[channel] ? color[channel] : destination[channel]; break;
			default:
				result[channel] = color[channel] * source_factor[channel] + destination[channel] * destination_factor[channel];
				break;
			}
		}
	}
	for (channel = 0; channel < 4; channel++)
	{
		if (rec.state.color_mask & (1 << channel))
			stored[channel] = sw_byte(result[channel]);
	}
	sw.pixels_drawn++;
}

static void sw_triangle(const struct sw_vertex *a, const struct sw_vertex *b, const struct sw_vertex *c)
{
	const struct sw_vertex *vertices[3] = { a, b, c };
	double window[3][3], inverse_w[3], area, offset = 0.0;
	int x0, y0, x1, y1, x, y, index, front;

	for (index = 0; index < 3; index++)
	{
		const double *clip = vertices[index]->clip;

		inverse_w[index] = 1.0 / clip[3];
		window[index][0] = rec.state.viewport[0] + (clip[0] * inverse_w[index] + 1.0) * 0.5 * rec.state.viewport[2];
		window[index][1] = rec.state.viewport[1] + (clip[1] * inverse_w[index] + 1.0) * 0.5 * rec.state.viewport[3];
		window[index][2] = rec.state.depth_range[0] + (clip[2] * inverse_w[index] + 1.0) * 0.5 *
			(rec.state.depth_range[1] - rec.state.depth_range[0]);
	}
	area = (window[1][0] - window[0][0]) * (window[2][1] - window[0][1]) -
		(window[2][0] - window[0][0]) * (window[1][1] - window[0][1]);
	if (area == 0.0)
		return;
	/* counter-clockwise in window coordinates (y up) has a positive area */
	front = (area > 0.0) == (rec.state.front_face == GL_CCW);
	if (rec.state.cull && (rec.state.cull_mode == GL_FRONT_AND_BACK || (rec.state.cull_mode == GL_BACK) != front))
		return;
	if (rec.state.offset_fill)
	{
		/* the depth's slope: the plane through the three vertices */
		double ex1 = window[1][0] - window[0][0], ey1 = window[1][1] - window[0][1], ez1 = window[1][2] - window[0][2];
		double ex2 = window[2][0] - window[0][0], ey2 = window[2][1] - window[0][1], ez2 = window[2][2] - window[0][2];
		double dzdx = (ez1 * ey2 - ez2 * ey1) / area, dzdy = (ez2 * ex1 - ez1 * ex2) / area;
		double slope = fabs(dzdx) > fabs(dzdy) ? fabs(dzdx) : fabs(dzdy);
		double resolvable = 1.0 / (double)(1UL << (driver.depth_bits >= 24 ? 24 : driver.depth_bits ? driver.depth_bits : 16));

		offset = slope * rec.state.polygon_offset[0] + resolvable * rec.state.polygon_offset[1];
	}
	sw.triangles_drawn++;
	x0 = y0 = 1 << 30;
	x1 = y1 = -(1 << 30);
	for (index = 0; index < 3; index++)
	{
		if (window[index][0] < x0) x0 = (int)floor(window[index][0]);
		if (window[index][1] < y0) y0 = (int)floor(window[index][1]);
		if (window[index][0] > x1) x1 = (int)ceil(window[index][0]);
		if (window[index][1] > y1) y1 = (int)ceil(window[index][1]);
	}
	/* (the view volume's sides are the viewport's) */
	if (x0 < rec.state.viewport[0]) x0 = rec.state.viewport[0];
	if (y0 < rec.state.viewport[1]) y0 = rec.state.viewport[1];
	if (x1 > rec.state.viewport[0] + rec.state.viewport[2]) x1 = rec.state.viewport[0] + rec.state.viewport[2];
	if (y1 > rec.state.viewport[1] + rec.state.viewport[3]) y1 = rec.state.viewport[1] + rec.state.viewport[3];
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > sw.width) x1 = sw.width;
	if (y1 > sw.height) y1 = sw.height;
	for (y = y0; y < y1; y++)
	{
		for (x = x0; x < x1; x++)
		{
			double px = x + 0.5, py = y + 0.5, weights[3], sum = 0.0, varying[SW_VARYINGS], depth = 0.0;
			int varying_index;

			weights[0] = ((window[1][0] - px) * (window[2][1] - py) - (window[2][0] - px) * (window[1][1] - py)) / area;
			weights[1] = ((window[2][0] - px) * (window[0][1] - py) - (window[0][0] - px) * (window[2][1] - py)) / area;
			weights[2] = 1.0 - weights[0] - weights[1];
			if (weights[0] < 0.0 || weights[1] < 0.0 || weights[2] < 0.0)
				continue;
			for (index = 0; index < 3; index++)
			{
				depth += weights[index] * window[index][2];
				weights[index] *= inverse_w[index];
				sum += weights[index];
			}
			for (varying_index = 0; varying_index < SW_VARYINGS; varying_index++)
			{
				varying[varying_index] = (weights[0] * a->varying[varying_index] + weights[1] * b->varying[varying_index] +
					weights[2] * c->varying[varying_index]) / sum;
			}
			sw_fragment(x, y, depth + offset, varying);
		}
	}
}

static void sw_polygon(const struct sw_vertex *a, const struct sw_vertex *b, const struct sw_vertex *c)
{
	struct sw_vertex polygon[12];
	int count = 3, plane, index;

	polygon[0] = *a;
	polygon[1] = *b;
	polygon[2] = *c;
	for (plane = 0; plane < 3 && count >= 3; plane++)
		count = sw_clip_plane(polygon, count, plane);
	for (index = 1; index + 1 < count; index++)
		sw_triangle(&polygon[0], &polygon[index], &polygon[index + 1]);
}

/* the vertex array through GL_PROJECTION (the recorder reads the arrays
by their types), the colour, and each unit's coordinates through its
GL_TEXTURE matrix; a unit that is off gets 0, 0, 0, 1 through its matrix */
static void sw_vertex_get(unsigned long vertex, struct sw_vertex *out)
{
	int unit, channel;

	rec_vertex_clip(&rec.state, vertex, out->clip);
	for (channel = 0; channel < 4; channel++)
	{
		out->varying[channel] = rec.state.color_array ? rec.state.color_pointer[vertex * 4 + channel] / 255.0 :
			rec.state.color[channel];
	}
	for (unit = 0; unit < REC_UNITS; unit++)
	{
		const struct rec_unit *state = &rec.state.units[unit];
		double in[4] = { 0.0, 0.0, 0.0, 1.0 }, *result = out->varying + 4 + unit * 4;

		if (state->enabled)
			rec_vertex_coordinates(&rec.state, unit, vertex, result);
		else
			rec_transform(state->matrix, in, result);
	}
}

static void sw_draw(GLenum mode, GLsizei count, const unsigned short *indices, GLint first)
{
	struct sw_vertex a, b, c, d;
	GLsizei index;

#define SW_VERTEX(n, out) sw_vertex_get(indices ? indices[n] : (unsigned long)first + (unsigned long)(n), out)
	if (!rec.state.vertex_array || !rec.state.vertex_pointer || count <= 0)
		return;
	sw_resize();
	switch (mode)
	{
	case GL_TRIANGLES:
		for (index = 0; index + 2 < count; index += 3)
		{
			SW_VERTEX(index, &a); SW_VERTEX(index + 1, &b); SW_VERTEX(index + 2, &c);
			sw_polygon(&a, &b, &c);
		}
		break;
	case GL_TRIANGLE_STRIP:
		for (index = 0; index + 2 < count; index++)
		{
			SW_VERTEX(index, &a); SW_VERTEX(index + 1, &b); SW_VERTEX(index + 2, &c);
			if (index & 1)
				sw_polygon(&b, &a, &c);
			else
				sw_polygon(&a, &b, &c);
		}
		break;
	case GL_TRIANGLE_FAN:
	case GL_POLYGON:
		for (index = 1; index + 1 < count; index++)
		{
			SW_VERTEX(0, &a); SW_VERTEX(index, &b); SW_VERTEX(index + 1, &c);
			sw_polygon(&a, &b, &c);
		}
		break;
	case GL_QUADS:
		for (index = 0; index + 3 < count; index += 4)
		{
			SW_VERTEX(index, &a); SW_VERTEX(index + 1, &b); SW_VERTEX(index + 2, &c); SW_VERTEX(index + 3, &d);
			sw_polygon(&a, &b, &c);
			sw_polygon(&a, &c, &d);
		}
		break;
	case GL_QUAD_STRIP:
		for (index = 0; index + 3 < count; index += 2)
		{
			SW_VERTEX(index, &a); SW_VERTEX(index + 1, &b); SW_VERTEX(index + 3, &c); SW_VERTEX(index + 2, &d);
			sw_polygon(&a, &b, &c);
			sw_polygon(&a, &c, &d);
		}
		break;
	default:
		/* points and lines: not rasterized */
		break;
	}
#undef SW_VERTEX
}

static void APIENTRY sw_glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
	rec_glDrawArrays(mode, first, count);
	sw_draw(mode, count, NULL, first);
}

static void APIENTRY sw_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices)
{
	rec_glDrawElements(mode, count, type, indices);
	sw_draw(mode, count, indices, 0);
}

/* the entry points this file stands in for; the others are the recorder's */
static const struct
{
	const char *name;
	function_pointer function;
} sw_entry_points[] =
{
	{ "glClearColor", (function_pointer)sw_glClearColor },
	{ "glClearDepth", (function_pointer)sw_glClearDepth },
	{ "glClear", (function_pointer)sw_glClear },
	{ "glTexImage2D", (function_pointer)sw_glTexImage2D },
	{ "glCompressedTexImage2D", (function_pointer)sw_glCompressedTexImage2D },
	{ "glCompressedTexImage2DARB", (function_pointer)sw_glCompressedTexImage2D },
	{ "glCopyTexSubImage2D", (function_pointer)sw_glCopyTexSubImage2D },
	{ "glTexParameterfv", (function_pointer)sw_glTexParameterfv },
	{ "glReadPixels", (function_pointer)sw_glReadPixels },
	{ "glDrawArrays", (function_pointer)sw_glDrawArrays },
	{ "glDrawElements", (function_pointer)sw_glDrawElements },
};
