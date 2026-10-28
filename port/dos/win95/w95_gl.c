/*
W95_GL.C

OpenGL on Windows 95 (the setting renderer = opengl, SETTINGS > RENDERER):
opengl32.dll, loaded when asked for (Windows 95 has it from OSR2, or
Microsoft's OpenGL 95 package; a 3D card's driver plugs into it), a
double-buffered context on the game's window, and the OpenGL 1.1
functions the renderer's GL rasterizer calls (port/dos/render/r_gl.c),
handed to it. Without the library, a pixel format or a context, the
software renderer stays.
*/

#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "halo_system.h"
#include "r_gl.h"

#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_MAX_TEXTURE_SIZE 0x0D33

/* the settings (port/dos/xdk/dos_settings.c) */
long dos_setting_integer(const char *name, long default_value);

typedef HGLRC (WINAPI *create_context_function)(HDC device);
typedef BOOL (WINAPI *make_current_function)(HDC device, HGLRC context);
typedef BOOL (WINAPI *delete_context_function)(HGLRC context);

static struct
{
	HMODULE library;
	HWND window;
	HDC device;
	HGLRC context;
	make_current_function make_current;
	delete_context_function delete_context;
	/* testing (setting debug_gl_max_texture): the largest texture side
	the driver takes, as an older card's (256 a 3dfx card's): said so, and
	larger textures refused as such a driver does (no texels: drawn
	white) */
	void (R_GL_API *get_floats)(unsigned name, float *values);
	void (R_GL_API *texture_image)(unsigned target, int level, int internal_format, int width, int height, int border,
		unsigned format, unsigned type, const void *pixels);
	long max_texture;
} gl;

static FARPROC function(const char *name)
{
	return GetProcAddress(gl.library, name);
}

static void R_GL_API get_floats_limited(unsigned name, float *values)
{
	gl.get_floats(name, values);
	if (name == GL_MAX_TEXTURE_SIZE && values[0] > (float)gl.max_texture)
		values[0] = (float)gl.max_texture;
}

static void R_GL_API texture_image_limited(unsigned target, int level, int internal_format, int width, int height,
	int border, unsigned format, unsigned type, const void *pixels)
{
	if (width > gl.max_texture || height > gl.max_texture)
		return;
	gl.texture_image(target, level, internal_format, width, height, border, format, type, pixels);
}

/* the window's device a double-buffered RGB format with depth, an OpenGL
context on it, and the renderer's functions: FALSE (and nothing kept) if
any is missing */
int w95_gl_open(HWND window)
{
	PIXELFORMATDESCRIPTOR format;
	create_context_function create_context;
	struct r_gl_functions functions;
	int index;
	char line[256];

	gl.library = LoadLibraryA("opengl32.dll");
	if (!gl.library)
	{
		system_log("opengl: Windows has no opengl32.dll: the software renderer");
		return FALSE;
	}
	create_context = (create_context_function)function("wglCreateContext");
	gl.make_current = (make_current_function)function("wglMakeCurrent");
	gl.delete_context = (delete_context_function)function("wglDeleteContext");
	if (!create_context || !gl.make_current || !gl.delete_context)
	{
		FreeLibrary(gl.library);
		memset(&gl, 0, sizeof(gl));
		return FALSE;
	}
	gl.window = window;
	gl.device = GetDC(window);
	memset(&format, 0, sizeof(format));
	format.nSize = sizeof(format);
	format.nVersion = 1;
	format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
	format.iPixelType = PFD_TYPE_RGBA;
	format.cColorBits = 32;
	format.cDepthBits = 24;
	format.iLayerType = PFD_MAIN_PLANE;
	index = ChoosePixelFormat(gl.device, &format);
	if (!index || !DescribePixelFormat(gl.device, index, sizeof(format), &format))
	{
		system_log("opengl: no pixel format: the software renderer");
		goto failed;
	}
	/* (the nearest the driver has may lack what the renderer needs: a depth
	buffer, thousands of colours at least -- a 256-colour desktop's formats
	need a palette, which the software renderer's own 256 colours do better) */
	if (!(format.dwFlags & PFD_SUPPORT_OPENGL) || format.cDepthBits == 0 || format.cColorBits < 15 ||
		(format.dwFlags & PFD_NEED_PALETTE))
	{
		snprintf(line, sizeof(line), "opengl: the display's nearest pixel format has %d-bit colour, %d-bit depth%s: "
			"the software renderer", format.cColorBits, format.cDepthBits,
			(format.dwFlags & PFD_NEED_PALETTE) ? " and needs a palette" : "");
		system_log(line);
		goto failed;
	}
	if (!SetPixelFormat(gl.device, index, &format))
	{
		system_log("opengl: no pixel format: the software renderer");
		goto failed;
	}
	gl.context = create_context(gl.device);
	if (!gl.context || !gl.make_current(gl.device, gl.context))
	{
		system_log("opengl: no context: the software renderer");
		goto failed;
	}
	memset(&functions, 0, sizeof(functions));
#define GL_FUNCTION(name) *(FARPROC *)&functions.name = function("gl" #name)
	GL_FUNCTION(Viewport);
	GL_FUNCTION(Scissor);
	GL_FUNCTION(Enable);
	GL_FUNCTION(Disable);
	GL_FUNCTION(ClearColor);
	GL_FUNCTION(Clear);
	GL_FUNCTION(MatrixMode);
	GL_FUNCTION(LoadIdentity);
	GL_FUNCTION(LoadMatrixf);
	GL_FUNCTION(DepthFunc);
	GL_FUNCTION(DepthMask);
	GL_FUNCTION(DepthRange);
	GL_FUNCTION(BlendFunc);
	GL_FUNCTION(AlphaFunc);
	GL_FUNCTION(ShadeModel);
	GL_FUNCTION(Hint);
	GL_FUNCTION(Fogi);
	GL_FUNCTION(Fogf);
	GL_FUNCTION(Fogfv);
	GL_FUNCTION(TexEnvi);
	GL_FUNCTION(TexEnvf);
	GL_FUNCTION(GenTextures);
	GL_FUNCTION(DeleteTextures);
	GL_FUNCTION(BindTexture);
	GL_FUNCTION(TexImage2D);
	GL_FUNCTION(TexSubImage2D);
	GL_FUNCTION(TexParameteri);
	GL_FUNCTION(TexParameterf);
	GL_FUNCTION(TexEnvfv);
	GL_FUNCTION(GetFloatv);
	GL_FUNCTION(PixelStorei);
	GL_FUNCTION(ReadPixels);
	GL_FUNCTION(Begin);
	GL_FUNCTION(End);
	GL_FUNCTION(Vertex4f);
	GL_FUNCTION(TexCoord2f);
	GL_FUNCTION(Color4ub);
	GL_FUNCTION(GetString);
	GL_FUNCTION(Finish);
	GL_FUNCTION(EnableClientState);
	GL_FUNCTION(DisableClientState);
	GL_FUNCTION(VertexPointer);
	GL_FUNCTION(ColorPointer);
	GL_FUNCTION(TexCoordPointer);
	GL_FUNCTION(DrawElements);
#undef GL_FUNCTION
	gl.max_texture = dos_setting_integer("debug_gl_max_texture", 0);
	if (gl.max_texture > 0 && functions.GetFloatv && functions.TexImage2D)
	{
		gl.get_floats = functions.GetFloatv;
		functions.GetFloatv = get_floats_limited;
		gl.texture_image = functions.TexImage2D;
		functions.TexImage2D = texture_image_limited;
	}
	if (!r_gl_use(&functions))
	{
		system_log("opengl: an OpenGL 1.1 function is missing: the software renderer");
		goto failed;
	}
	{
		float largest = 0.0f;

		functions.GetFloatv(GL_MAX_TEXTURE_SIZE, &largest);
		snprintf(line, sizeof(line), "opengl: %s, %s, OpenGL %s (%d-bit colour, %d-bit depth, textures up to %d)",
			(const char *)functions.GetString(GL_VENDOR), (const char *)functions.GetString(GL_RENDERER),
			(const char *)functions.GetString(GL_VERSION), format.cColorBits, format.cDepthBits, (int)largest);
	}
	system_log(line);
	return TRUE;

failed:
	if (gl.context)
	{
		gl.make_current(NULL, NULL);
		gl.delete_context(gl.context);
	}
	if (gl.device)
		ReleaseDC(window, gl.device);
	FreeLibrary(gl.library);
	memset(&gl, 0, sizeof(gl));
	return FALSE;
}

int w95_gl_active(void)
{
	return gl.context != NULL;
}

/* the frame shown (the back buffer to the front), its last triangles drawn
first */
void w95_gl_present(void)
{
	if (!gl.context)
		return;
	r_gl_flush();
	SwapBuffers(gl.device);
}

void w95_gl_close(void)
{
	if (!gl.context)
		return;
	gl.make_current(NULL, NULL);
	gl.delete_context(gl.context);
	ReleaseDC(gl.window, gl.device);
	FreeLibrary(gl.library);
	memset(&gl, 0, sizeof(gl));
}
