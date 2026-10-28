/*
D3D_GLUE.C

The port's game-side glue for the Direct3D build (HALOD3D.EXE,
port/dos/DIRECT3D.md), in place of what the port's own renderer gives the
DOS and Windows 95 builds (port/dos/game/dos_render_game.c and the units
port.json's d3d section leaves out): the menu's drawing goes to the
device's overlay (port/dos/d3d/xbox/xd3d_device.c), the renderer's
settings and the level-of-detail asks answer as the Xbox's renderer would.
*/

#include "cseries.h"
#include "r_public.h"

/* (xd3d_device.c) */
struct pcd3d_texture;
unsigned long xd3d_frame_number(void);
void xd3d_overlay_quad(float x0, float y0, float x1, float y1, unsigned long argb, struct pcd3d_texture *texture,
	float u0, float v0, float u1, float v1);
void xd3d_overlay_picture(float x, float y, const unsigned long *pixels, int width, int height);

/* ---------- the renderer's settings: the game's own screen, 640x480 in
full colour (the menu lays itself out in those pixels) */

struct r_settings dos_render_settings = { 640, 480, 32, 256, 3, 0.0f, 0, 0, 0 };
float dos_screen_scale_x = 1.0f, dos_screen_scale_y = 1.0f;

void dos_render_reload_settings(void)
{
}

long dos_frame_number(void)
{
	return (long)xd3d_frame_number();
}

/* ---------- level of detail: as the Xbox chose it */

real dos_model_pixels;

real dos_model_detail_scale(void)
{
	return 1.0f;
}

boolean dos_object_recently_seen(long object_index)
{
	(void)object_index;
	return TRUE;
}

int dos_particles_allowed(void)
{
	return TRUE;
}

/* ---------- the menu's drawing */

void r_fill_rectangle(int x0, int y0, int x1, int y1, unsigned long argb)
{
	xd3d_overlay_quad((float)x0, (float)y0, (float)x1, (float)y1, argb, NULL, 0.0f, 0.0f, 0.0f, 0.0f);
}

void r_draw_picture(int x, int y, const unsigned long *argb, int width, int height)
{
	xd3d_overlay_picture((float)x, (float)y, argb, width, height);
}

/* (the interface's pictures from ui.map's bitmaps: not yet) */
unsigned long *dos_interface_picture(long bitmap_tag_index, float crop_u, float crop_v, int width, int height)
{
	(void)bitmap_tag_index;
	(void)crop_u;
	(void)crop_v;
	(void)width;
	(void)height;
	return NULL;
}

/* ---------- the menu's art (port/dos/game/dos_menu_art.c draws with the
port's renderer): not yet */

const int dos_art_picture_crop[4] = { 0, 0, 140, 114 };

void dos_art_load(void)
{
}

int dos_art_has(int art, int index)
{
	(void)art;
	(void)index;
	return 0;
}

void dos_art_draw(int art, int index, const int *crop, float x0, float y0, float x1, float y1, unsigned long argb)
{
	(void)art;
	(void)index;
	(void)crop;
	(void)x0;
	(void)y0;
	(void)x1;
	(void)y1;
	(void)argb;
}

void dos_art_frame(int art, int index, const int *crop, int border_texels, float border, float x0, float y0,
	float x1, float y1, unsigned long argb)
{
	(void)art;
	(void)index;
	(void)crop;
	(void)border_texels;
	(void)border;
	(void)x0;
	(void)y0;
	(void)x1;
	(void)y1;
	(void)argb;
}

/* ---------- loading screens: the game's own frames go on (not yet the
menu's loading picture) */

void dos_loading_begin(const char *map_name)
{
	(void)map_name;
}

void dos_loading_read(unsigned long done, unsigned long total)
{
	(void)done;
	(void)total;
}

void dos_loading_level(long done, long total)
{
	(void)done;
	(void)total;
}

void dos_loading_progress(const char *what, long permille)
{
	(void)what;
	(void)permille;
}

/* ---------- Windows 95's OpenGL (port/dos/win95/w95_gl.c is left out) */

int w95_gl_open(void *window)
{
	(void)window;
	return 0;
}

void w95_gl_present(void)
{
}

void w95_gl_close(void)
{
}

void r_gl_viewport(int x, int y, int width, int height, int window_width, int window_height)
{
	(void)x;
	(void)y;
	(void)width;
	(void)height;
	(void)window_width;
	(void)window_height;
}
