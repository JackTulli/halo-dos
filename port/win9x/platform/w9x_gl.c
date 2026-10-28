/*
W9X_GL.C

OpenGL on Windows 98/ME through WGL, with opengl32.dll loaded at run time
(see w9x_backend.h): the game starts without it, says what is missing, and
picks up a replacement opengl32.dll put next to the executable.

The pixel format is chosen from the whole list the driver describes,
rather than by ChoosePixelFormat, whose idea of closest differs from driver
to driver. In order of weight: hardware acceleration (an installable client
driver, then a mini client driver; Microsoft's software renderer only when
display.allow_software_gl allows it), the desktop's colour depth (a format
of another depth is drawn in software, if at all), then the colour, depth,
stencil and alpha bits asked for. A 16-bit desktop on a card of the time
often has no 32-bit formats and no stencil or alpha: those are taken all
the same, and result says what was had for the renderer to adapt to.

Only legacy contexts exist here (no WGL_ARB_create_context on these
drivers). Every call that loads or runs driver code checks the processor's
floating point state afterwards (w9x_fpu_check).
*/

#include "w9x_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* (gl.h's values; this unit takes no OpenGL header, so that nothing of
opengl32.dll is ever imported) */
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_EXTENSIONS 0x1F03

typedef HGLRC (WINAPI *wgl_create_context_function)(HDC dc);
typedef BOOL (WINAPI *wgl_delete_context_function)(HGLRC context);
typedef BOOL (WINAPI *wgl_make_current_function)(HDC dc, HGLRC context);
typedef PROC (WINAPI *wgl_get_proc_address_function)(LPCSTR name);
typedef const unsigned char *(WINAPI *gl_get_string_function)(unsigned int name);
typedef const char *(WINAPI *wgl_get_extensions_string_arb_function)(HDC dc);
typedef const char *(WINAPI *wgl_get_extensions_string_ext_function)(void);
typedef BOOL (WINAPI *wgl_swap_interval_ext_function)(int interval);

static HMODULE opengl;
static HGLRC context;
static struct
{
	wgl_create_context_function create_context;
	wgl_delete_context_function delete_context;
	wgl_make_current_function make_current;
	wgl_get_proc_address_function get_proc_address;
	gl_get_string_function get_string;
	wgl_swap_interval_ext_function swap_interval;
} wgl;
static const char *gl_extensions;
static const char *wgl_extensions;

/* ---------- the pixel format */

static enum w9x_gl_acceleration format_acceleration(const PIXELFORMATDESCRIPTOR *format)
{
	if (!(format->dwFlags & PFD_GENERIC_FORMAT))
		return _w9x_gl_icd;
	if (format->dwFlags & PFD_GENERIC_ACCELERATED)
		return _w9x_gl_mcd;
	return _w9x_gl_software;
}

/* how well a format suits; negative for one that cannot be used */
static long format_score(const PIXELFORMATDESCRIPTOR *format, const struct w9x_video_request *request,
	int desktop_bits)
{
	const DWORD needed = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
	enum w9x_gl_acceleration acceleration = format_acceleration(format);
	int wanted_color = request->color_bits > 0 ? request->color_bits : 32;
	int wanted_depth = request->depth_bits > 0 ? request->depth_bits : 24;
	long score;

	if ((format->dwFlags & needed) != needed || format->iPixelType != PFD_TYPE_RGBA ||
		(format->dwFlags & PFD_NEED_PALETTE) || format->cColorBits < 15 || format->cDepthBits < 16 ||
		format->iLayerType != PFD_MAIN_PLANE)
	{
		return -1;
	}
	if (acceleration == _w9x_gl_software && !request->allow_software)
		return -1;
	score = (long)acceleration * 1000000L;
	if (format->cColorBits == desktop_bits || (desktop_bits == 32 && format->cColorBits == 24) ||
		(desktop_bits == 24 && format->cColorBits == 32))
	{
		score += 100000L;
	}
	score += format->cColorBits >= wanted_color ? 20000L : format->cColorBits * 500L;
	score += format->cDepthBits >= wanted_depth ? 8000L : format->cDepthBits * 200L;
	if (request->stencil_bits > 0 && format->cStencilBits >= request->stencil_bits)
		score += 2000L;
	if (request->alpha_bits > 0 && format->cAlphaBits >= request->alpha_bits)
		score += 1000L;
	/* the accumulation buffer is never used, and some drivers of the time
	only offered it in software */
	if (format->cAccumBits)
		score -= 100L;
	return score;
}

/* the best format the driver has for this window's device context, or 0 */
static int format_choose(HDC dc, const struct w9x_video_request *request, int desktop_bits,
	PIXELFORMATDESCRIPTOR *chosen, int *software_only)
{
	PIXELFORMATDESCRIPTOR format;
	struct w9x_fpu fpu;
	int count, index, best = 0;
	long best_score = -1;

	*software_only = FALSE;
	w9x_fpu_save(&fpu);
	/* (the first call loads the display driver's OpenGL) */
	count = DescribePixelFormat(dc, 1, sizeof(format), &format);
	w9x_fpu_check(&fpu, "DescribePixelFormat");
	for (index = 1; index <= count; index++)
	{
		long score;

		if (!DescribePixelFormat(dc, index, sizeof(format), &format))
			continue;
		score = format_score(&format, request, desktop_bits);
		if (score < 0 && format_acceleration(&format) == _w9x_gl_software && !request->allow_software)
		{
			struct w9x_video_request allowed = *request;

			allowed.allow_software = TRUE;
			if (format_score(&format, &allowed, desktop_bits) >= 0)
				*software_only = TRUE;
		}
		if (score > best_score)
		{
			best_score = score;
			best = index;
			*chosen = format;
		}
	}
	if (best)
		*software_only = FALSE;
	return best;
}

/* ---------- strings */

static void copy_string(char *destination, size_t size, const unsigned char *source)
{
	snprintf(destination, size, "%s", source ? (const char *)source : "");
}

/* "1.2.1 Mesa 7.0" or "1.1.0" */
static void parse_version(const char *version, int *major, int *minor)
{
	*major = 0;
	*minor = 0;
	if (version[0] >= '0' && version[0] <= '9')
	{
		*major = atoi(version);
		while (*version >= '0' && *version <= '9')
			version++;
		if (*version == '.')
			*minor = atoi(version + 1);
	}
}

/* whether list (names separated by spaces) has name as a whole word */
static int list_has(const char *list, const char *name)
{
	size_t length = strlen(name);

	if (!list || !length)
		return FALSE;
	while (*list)
	{
		while (*list == ' ')
			list++;
		if (!strncmp(list, name, length) && (list[length] == ' ' || list[length] == 0))
			return TRUE;
		while (*list && *list != ' ')
			list++;
	}
	return FALSE;
}

/* ---------- public code */

void *w9x_gl_proc_address(const char *name)
{
	PROC address = NULL;

	if (!opengl)
		return NULL;
	if (wgl.get_proc_address && context)
		address = wgl.get_proc_address(name);
	/* wglGetProcAddress has only what is above OpenGL 1.1, and some
	drivers answer 1, 2, 3 or -1 for what they do not have */
	if ((ULONG_PTR)address <= 3 || (ULONG_PTR)address == (ULONG_PTR)-1)
		address = GetProcAddress(opengl, name);
	return (void *)address;
}

int w9x_gl_extension(const char *name)
{
	return list_has(gl_extensions, name) || list_has(wgl_extensions, name);
}

int w9x_gl_swap_interval(int interval)
{
	struct w9x_fpu fpu;
	BOOL result;

	if (!wgl.swap_interval)
		return FALSE;
	w9x_fpu_save(&fpu);
	result = wgl.swap_interval(interval);
	w9x_fpu_check(&fpu, "wglSwapIntervalEXT");
	return result != FALSE;
}

int w9x_gl_open(HWND window, HDC dc, const struct w9x_video_request *request, struct w9x_video_result *result,
	char *error, int error_size)
{
	PIXELFORMATDESCRIPTOR format;
	struct w9x_fpu fpu;
	int index, software_only;
	BOOL set;

	(void)window;
	w9x_fpu_save(&fpu);
	opengl = LoadLibraryA("opengl32.dll");
	w9x_fpu_check(&fpu, "LoadLibraryA(opengl32.dll)");
	if (!opengl)
	{
		snprintf(error, (size_t)error_size, "OpenGL (opengl32.dll) is not installed.");
		return FALSE;
	}
	wgl.create_context = (wgl_create_context_function)GetProcAddress(opengl, "wglCreateContext");
	wgl.delete_context = (wgl_delete_context_function)GetProcAddress(opengl, "wglDeleteContext");
	wgl.make_current = (wgl_make_current_function)GetProcAddress(opengl, "wglMakeCurrent");
	wgl.get_proc_address = (wgl_get_proc_address_function)GetProcAddress(opengl, "wglGetProcAddress");
	wgl.get_string = (gl_get_string_function)GetProcAddress(opengl, "glGetString");
	if (!wgl.create_context || !wgl.delete_context || !wgl.make_current || !wgl.get_proc_address || !wgl.get_string)
	{
		snprintf(error, (size_t)error_size, "This opengl32.dll lacks the WGL functions.");
		return FALSE;
	}

	index = format_choose(dc, request, result->display_bits, &format, &software_only);
	if (!index)
	{
		if (software_only)
		{
			snprintf(error, (size_t)error_size,
				"The display driver has no OpenGL of its own: only Microsoft's software renderer, which is too "
				"slow to play on (display.allow_software_gl in config.toml allows it). Install the graphics "
				"card's driver from its maker.");
		}
		else
		{
			snprintf(error, (size_t)error_size, "The display driver offers no double-buffered OpenGL pixel format "
				"for a %d-bit display.", result->display_bits);
		}
		return FALSE;
	}
	w9x_fpu_save(&fpu);
	set = SetPixelFormat(dc, index, &format);
	w9x_fpu_check(&fpu, "SetPixelFormat");
	if (!set)
	{
		snprintf(error, (size_t)error_size, "Could not set OpenGL pixel format %d (error %lu).", index, GetLastError());
		return FALSE;
	}
	/* what the format really has */
	DescribePixelFormat(dc, index, sizeof(format), &format);
	w9x_fpu_save(&fpu);
	context = wgl.create_context(dc);
	w9x_fpu_check(&fpu, "wglCreateContext");
	if (!context)
	{
		snprintf(error, (size_t)error_size, "Could not create an OpenGL context (error %lu).", GetLastError());
		return FALSE;
	}
	w9x_fpu_save(&fpu);
	set = wgl.make_current(dc, context);
	w9x_fpu_check(&fpu, "wglMakeCurrent");
	if (!set)
	{
		snprintf(error, (size_t)error_size, "Could not make the OpenGL context current (error %lu).", GetLastError());
		wgl.delete_context(context);
		context = NULL;
		return FALSE;
	}

	copy_string(result->vendor, sizeof(result->vendor), wgl.get_string(GL_VENDOR));
	copy_string(result->renderer, sizeof(result->renderer), wgl.get_string(GL_RENDERER));
	copy_string(result->version, sizeof(result->version), wgl.get_string(GL_VERSION));
	gl_extensions = (const char *)wgl.get_string(GL_EXTENSIONS);
	parse_version(result->version, &result->version_major, &result->version_minor);
	result->acceleration = format_acceleration(&format);
	/* the flags can say a driver when the renderer is Microsoft's */
	if (!strcmp(result->renderer, "GDI Generic"))
		result->acceleration = _w9x_gl_software;
	if (result->acceleration == _w9x_gl_software && !request->allow_software)
	{
		snprintf(error, (size_t)error_size,
			"The display driver has no OpenGL of its own: only Microsoft's software renderer, which is too slow "
			"to play on (display.allow_software_gl in config.toml allows it). Install the graphics card's driver "
			"from its maker.");
		w9x_gl_close();
		return FALSE;
	}
	{
		wgl_get_extensions_string_arb_function arb =
			(wgl_get_extensions_string_arb_function)w9x_gl_proc_address("wglGetExtensionsStringARB");
		wgl_get_extensions_string_ext_function ext =
			(wgl_get_extensions_string_ext_function)w9x_gl_proc_address("wglGetExtensionsStringEXT");

		w9x_fpu_save(&fpu);
		wgl_extensions = arb ? arb(dc) : ext ? ext() : NULL;
		w9x_fpu_check(&fpu, "wglGetExtensionsString");
	}
	/* drivers of the time list WGL_EXT_swap_control among the OpenGL
	extensions rather than the WGL ones */
	if (w9x_gl_extension("WGL_EXT_swap_control"))
		wgl.swap_interval = (wgl_swap_interval_ext_function)w9x_gl_proc_address("wglSwapIntervalEXT");

	result->gl = TRUE;
	result->pixel_format = index;
	result->color_bits = format.cColorBits;
	result->red_bits = format.cRedBits;
	result->green_bits = format.cGreenBits;
	result->blue_bits = format.cBlueBits;
	result->alpha_bits = format.cAlphaBits;
	result->depth_bits = format.cDepthBits;
	result->stencil_bits = format.cStencilBits;
	result->swap_control = wgl.swap_interval != NULL;
	result->extensions = gl_extensions;
	result->wgl_extensions = wgl_extensions;
	return TRUE;
}

void w9x_gl_swap(HDC dc)
{
	struct w9x_fpu fpu;

	/* (every frame: the check is two stores and a compare, and drivers
	have been seen to change the control word on their first swap) */
	w9x_fpu_save(&fpu);
	SwapBuffers(dc);
	w9x_fpu_check(&fpu, "SwapBuffers");
}

void w9x_gl_close(void)
{
	if (!context)
		return;
	wgl.make_current(NULL, NULL);
	wgl.delete_context(context);
	context = NULL;
}
