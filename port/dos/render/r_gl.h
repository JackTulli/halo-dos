/*
R_GL.H

The renderer's OpenGL 1.1 rasterizer (r_gl.c): the screen-space triangles,
quads, text and pictures the software rasterizer draws (r_internal.h's
seam) drawn by the graphics card instead. The renderer is one set of
objects for DOS and Windows 95, so OpenGL is reached only through the
functions the Windows 95 build hands it (port/dos/win95/w95_gl.c, from
opengl32.dll loaded at run time); on DOS it is never active.
*/

#ifndef __R_GL_H
#define __R_GL_H

#define R_GL_API __stdcall

/* the OpenGL 1.1 functions it calls */
struct r_gl_functions
{
	void (R_GL_API *Viewport)(int x, int y, int width, int height);
	void (R_GL_API *Scissor)(int x, int y, int width, int height);
	void (R_GL_API *Enable)(unsigned capability);
	void (R_GL_API *Disable)(unsigned capability);
	void (R_GL_API *ClearColor)(float red, float green, float blue, float alpha);
	void (R_GL_API *Clear)(unsigned mask);
	void (R_GL_API *MatrixMode)(unsigned mode);
	void (R_GL_API *LoadIdentity)(void);
	void (R_GL_API *LoadMatrixf)(const float *matrix);
	void (R_GL_API *DepthFunc)(unsigned function);
	void (R_GL_API *DepthMask)(unsigned char write);
	void (R_GL_API *DepthRange)(double near_value, double far_value);
	void (R_GL_API *BlendFunc)(unsigned source, unsigned destination);
	void (R_GL_API *AlphaFunc)(unsigned function, float reference);
	void (R_GL_API *ShadeModel)(unsigned mode);
	void (R_GL_API *Hint)(unsigned target, unsigned mode);
	void (R_GL_API *Fogi)(unsigned name, int value);
	void (R_GL_API *Fogf)(unsigned name, float value);
	void (R_GL_API *Fogfv)(unsigned name, const float *values);
	void (R_GL_API *TexEnvi)(unsigned target, unsigned name, int value);
	void (R_GL_API *TexEnvf)(unsigned target, unsigned name, float value);
	void (R_GL_API *TexEnvfv)(unsigned target, unsigned name, const float *values);
	void (R_GL_API *GenTextures)(int count, unsigned *names);
	void (R_GL_API *DeleteTextures)(int count, const unsigned *names);
	void (R_GL_API *BindTexture)(unsigned target, unsigned name);
	void (R_GL_API *TexImage2D)(unsigned target, int level, int internal_format, int width, int height, int border,
		unsigned format, unsigned type, const void *pixels);
	void (R_GL_API *TexSubImage2D)(unsigned target, int level, int x, int y, int width, int height, unsigned format,
		unsigned type, const void *pixels);
	void (R_GL_API *TexParameteri)(unsigned target, unsigned name, int value);
	void (R_GL_API *TexParameterf)(unsigned target, unsigned name, float value);
	void (R_GL_API *GetFloatv)(unsigned name, float *values);
	void (R_GL_API *PixelStorei)(unsigned name, int value);
	void (R_GL_API *ReadPixels)(int x, int y, int width, int height, unsigned format, unsigned type, void *pixels);
	void (R_GL_API *Begin)(unsigned mode);
	void (R_GL_API *End)(void);
	void (R_GL_API *Vertex4f)(float x, float y, float z, float w);
	void (R_GL_API *TexCoord2f)(float s, float t);
	void (R_GL_API *Color4ub)(unsigned char red, unsigned char green, unsigned char blue, unsigned char alpha);
	const unsigned char *(R_GL_API *GetString)(unsigned name);
	void (R_GL_API *Finish)(void);
	/* vertex arrays (OpenGL 1.1's own, but a driver without them still
	draws: the batches go vertex by vertex, between Begin and End, then) */
	void (R_GL_API *EnableClientState)(unsigned array);
	void (R_GL_API *DisableClientState)(unsigned array);
	void (R_GL_API *VertexPointer)(int size, unsigned type, int stride, const void *pointer);
	void (R_GL_API *ColorPointer)(int size, unsigned type, int stride, const void *pointer);
	void (R_GL_API *TexCoordPointer)(int size, unsigned type, int stride, const void *pointer);
	void (R_GL_API *DrawElements)(unsigned mode, int count, unsigned type, const void *indices);
};

/* the card draws from now (the picture's size the r_frame's, shown in the
window's client area; r_internal.h's backend): FALSE if its functions are
not all there */
int r_gl_use(const struct r_gl_functions *functions);
/* where the picture goes in the window, from the next frame: x and y from
the client area's bottom left, as OpenGL counts, and the client area's
size */
void r_gl_viewport(int x, int y, int width, int height, int window_width, int window_height);
/* the triangles gathered so far drawn (before the frame is shown: a batch
is drawn at its end, which the next frame's would be) */
void r_gl_flush(void);

#endif /* __R_GL_H */
