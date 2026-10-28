/*
DOS_MENU_ART.C

Halo's menu art (dos_menu_art.h): each bitmap of each tag below decoded
once, while ui.map is up, into a texture of the renderer's (in full colour
for a card that keeps it, r_gl.c), with the box its visible texels fill
(its alpha's), and drawn as screen quads of the menus' 640x480 units
scaled to the picture: once made, they need no map.
*/

#include "cseries.h"

#include "rasterizer/rasterizer.h"
#include "render/render.h"
#include "tag_files/tag_groups.h"

#include "halo_system.h"
#include "dos_render.h"
#include "dos_menu_art.h"

#include <stdlib.h>
#include <string.h>

int dos_bitmap_tag_count(long bitmap_tag_index);
unsigned long *dos_bitmap_tag_decode(long bitmap_tag_index, short bitmap_index, int largest, int *width, int *height);

#define ART_BITMAPS 16

/* the tags, in enum dos_art's order */
static const char *const art_tags[DOS_ART_COUNT] =
{
	"ui\\shell\\bitmaps\\sp_levels",
	"ui\\shell\\bitmaps\\mp_map_grafix",
	"ui\\shell\\bitmaps\\game_type_grafix",
	"ui\\shell\\main_menu\\difficulty_select\\difficulty_options",
	"ui\\shell\\bitmaps\\controller_graphix",
	"ui\\shell\\bitmaps\\a_butn",
	"ui\\shell\\bitmaps\\b_butn",
	"ui\\shell\\bitmaps\\x_butn",
	"ui\\shell\\bitmaps\\y_butn",
	"ui\\shell\\bitmaps\\list_field",
	"ui\\shell\\bitmaps\\alert_bkd",
	"ui\\shell\\bitmaps\\split_screen_bkd",
	"ui\\shell\\bitmaps\\option_bkds",
	"ui\\shell\\bitmaps\\arrow_sm_left",
	"ui\\shell\\bitmaps\\arrow_sm_right",
	"ui\\shell\\bitmaps\\console_grafix",
};

/* the campaign's and the maps' pictures: the left of their bitmaps */
const int dos_art_picture_crop[4] = { 0, 0, 140, 114 };

static struct
{
	int read;
	int count;
	r_texture *texture[ART_BITMAPS];
	int width[ART_BITMAPS], height[ART_BITMAPS];
	/* the box its visible texels fill */
	int box[ART_BITMAPS][4];
} art[DOS_ART_COUNT];

/* whether the loaded map's tags were looked through: a map's tags stay as
they are until the next is loaded, and the search by name through all of
them, every frame of a game whose map has none of the art (one started
without the menu: -play), cost a slow machine milliseconds a frame */
static int art_searched;

void dos_art_new_map(void)
{
	art_searched = FALSE;
}

void dos_art_load(void)
{
	if (art_searched)
		return;
	art_searched = TRUE;
	for (int a = 0; a < DOS_ART_COUNT; a++)
	{
		long tag;

		if (art[a].read)
			continue;
		tag = tag_loaded('bitm', art_tags[a]);
		if (tag == NONE)
			continue;
		art[a].read = TRUE;
		art[a].count = dos_bitmap_tag_count(tag);
		if (art[a].count > ART_BITMAPS)
			art[a].count = ART_BITMAPS;
		for (int b = 0; b < art[a].count; b++)
		{
			int width, height, x0, y0, x1 = 0, y1 = 0;
			unsigned long *argb = dos_bitmap_tag_decode(tag, (short)b, 512, &width, &height);

			if (!argb)
				continue;
			x0 = width;
			y0 = height;
			for (int y = 0; y < height; y++)
			{
				for (int x = 0; x < width; x++)
				{
					if (argb[y * width + x] >> 24)
					{
						if (x < x0) x0 = x;
						if (y < y0) y0 = y;
						if (x + 1 > x1) x1 = x + 1;
						if (y + 1 > y1) y1 = y + 1;
					}
				}
			}
			if (x1 <= x0 || y1 <= y0)
			{
				x0 = y0 = 0;
				x1 = width;
				y1 = height;
			}
			art[a].box[b][0] = x0;
			art[a].box[b][1] = y0;
			art[a].box[b][2] = x1;
			art[a].box[b][3] = y1;
			art[a].width[b] = width;
			art[a].height[b] = height;
			art[a].texture[b] = r_texture_new(argb, width, height, R_TEXTURE_ALPHA | R_TEXTURE_CLAMP |
				R_TEXTURE_LARGE);
			(free)(argb);
		}
	}
}

int dos_art_has(enum dos_art which, int index)
{
	return which >= 0 && which < DOS_ART_COUNT && index >= 0 && index < art[which].count &&
		art[which].texture[index] != NULL;
}

/* a quad of the texture: texels u0 v0 to u1 v1 into the screen's x0 y0 to
x1 y1 (menus' units) */
static void quad(enum dos_art which, int index, float u0, float v0, float u1, float v1, float x0, float y0,
	float x1, float y1, unsigned long color)
{
	struct r_screen_vertex corners[4];
	float width = (float)art[which].width[index], height = (float)art[which].height[index];
	float sx = dos_screen_scale_x, sy = dos_screen_scale_y;

	if (x1 <= x0 || y1 <= y0)
		return;
	corners[0].x = x0 * sx + dos_screen_offset_x;
	corners[0].y = y0 * sy;
	corners[1].x = x1 * sx + dos_screen_offset_x;
	corners[1].y = y0 * sy;
	corners[2].x = x1 * sx + dos_screen_offset_x;
	corners[2].y = y1 * sy;
	corners[3].x = x0 * sx + dos_screen_offset_x;
	corners[3].y = y1 * sy;
	corners[0].u = corners[3].u = u0 / width;
	corners[1].u = corners[2].u = u1 / width;
	corners[0].v = corners[1].v = v0 / height;
	corners[2].v = corners[3].v = v1 / height;
	for (int i = 0; i < 4; i++)
		corners[i].color = color;
	r_draw_screen_quad(corners, art[which].texture[index], _r_screen_alpha);
}

void dos_art_draw(enum dos_art which, int index, const int *crop, float x0, float y0, float x1, float y1,
	unsigned long color)
{
	const int *box;

	if (!dos_art_has(which, index) || !(color >> 24))
		return;
	box = crop ? crop : art[which].box[index];
	quad(which, index, (float)box[0], (float)box[1], (float)box[2], (float)box[3], x0, y0, x1, y1, color);
}

void dos_art_frame(enum dos_art which, int index, const int *crop, int border_texels, float border, float x0,
	float y0, float x1, float y1, unsigned long color)
{
	const int *box;
	float u[4], v[4], x[4], y[4];

	if (!dos_art_has(which, index) || !(color >> 24))
		return;
	box = crop ? crop : art[which].box[index];
	/* (a frame smaller than its corners: they shrink) */
	if (2.0f * border > x1 - x0)
		border = (x1 - x0) * 0.5f;
	if (2.0f * border > y1 - y0)
		border = (y1 - y0) * 0.5f;
	u[0] = (float)box[0];
	u[1] = (float)(box[0] + border_texels);
	u[2] = (float)(box[2] - border_texels);
	u[3] = (float)box[2];
	v[0] = (float)box[1];
	v[1] = (float)(box[1] + border_texels);
	v[2] = (float)(box[3] - border_texels);
	v[3] = (float)box[3];
	x[0] = x0;
	x[1] = x0 + border;
	x[2] = x1 - border;
	x[3] = x1;
	y[0] = y0;
	y[1] = y0 + border;
	y[2] = y1 - border;
	y[3] = y1;
	for (int j = 0; j < 3; j++)
	{
		for (int i = 0; i < 3; i++)
			quad(which, index, u[i], v[j], u[i + 1], v[j + 1], x[i], y[j], x[i + 1], y[j + 1], color);
	}
}
