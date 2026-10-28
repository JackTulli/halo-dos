/*
R_PALETTE.C

The 8-bit picture's colours: a palette, the nearest colour of each 15-bit
colour, and the maps the rasterizer looks its light, fog and blends up in.

At start-up the palette is a colour cube (6 reds, 7 greens and 6 blues,
levels closer together in the dark, and four greys between), which needs
nothing from a map. Each map then gets its own (r_palette_set, from a
median cut of the colours its level shows: r_palette_median_cut): a cube
spends most of its 256 on colours Halo never shows, and has few of the
earth, metal and sky colours it does.
*/

#include "r_internal.h"

#include <stdlib.h>
#include <string.h>

unsigned char r_palette_rgb[256 * 3];

unsigned long r_palette_color(unsigned char index)
{
	const unsigned char *rgb = r_palette_rgb + index * 3;

	return ((unsigned long)rgb[0] << 16) | ((unsigned long)rgb[1] << 8) | rgb[2];
}
/* (one row more than the levels, the top one again: a dithered level may
reach it, r_soft.c) */
unsigned char r_color_map[R_LIGHT_LEVELS + 1][256];
unsigned char r_fog_map[R_FOG_LEVELS][256];
unsigned char r_inverse_palette[32768];

static unsigned long fog_map_color = 0xFFFFFFFF;
static unsigned char *add_table;
static unsigned char *blend_table;

static const unsigned char red_levels[6] = { 0, 45, 90, 140, 195, 255 };
static const unsigned char green_levels[7] = { 0, 38, 76, 117, 160, 205, 255 };
static const unsigned char blue_levels[6] = { 0, 45, 90, 140, 195, 255 };
static const unsigned char greys[4] = { 23, 60, 108, 170 };

static long distance(int r0, int g0, int b0, const unsigned char *rgb)
{
	long dr = r0 - rgb[0], dg = g0 - rgb[1], db = b0 - rgb[2];

	/* (green counts most, blue least, as the eye sees them) */
	return 3 * dr * dr + 4 * dg * dg + 2 * db * db;
}

/* the nearest of the palette's 256 to a colour (a search: for the tables) */
static unsigned char search(int r, int g, int b)
{
	long best = 0x7FFFFFFF;
	unsigned char found = 0;

	for (int i = 0; i < 256; i++)
	{
		long d = distance(r, g, b, &r_palette_rgb[i * 3]);

		if (d < best)
		{
			best = d;
			found = (unsigned char)i;
		}
	}
	return found;
}

unsigned char r_palette_nearest(unsigned long rgb)
{
	return r_inverse_palette[((rgb >> 9) & 0x7C00) | ((rgb >> 6) & 0x03E0) | ((rgb >> 3) & 0x001F)];
}

/* the nearest colour to r, g, b (each clamped to 0-255), or kept where it is
as near: the inverse palette answers for the middle of a colour's 15-bit
square, and of two entries close together one answered for the other, so a
colour lit as it is, faded by no fog, blended over itself or added nothing
to came back as its neighbour (seams where the next triangle took another
way; an added particle's whole quad showing over what was under it) */
static unsigned char nearest_or_kept(int r, int g, int b, unsigned char kept)
{
	unsigned char found;

	if (r > 255) r = 255;
	if (g > 255) g = 255;
	if (b > 255) b = 255;
	if (r < 0) r = 0;
	if (g < 0) g = 0;
	if (b < 0) b = 0;
	found = r_inverse_palette[((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)];
	return distance(r, g, b, &r_palette_rgb[kept * 3]) <= distance(r, g, b, &r_palette_rgb[found * 3]) ? kept : found;
}

/* the nearest colours, the light maps; the fog map and the blend tables
made again when next asked for */
/* The palette's entries in the order of their green (the weightiest
channel): the nearest to a colour is searched outward from its green, and
the search stops on a side once green alone is farther than the best so
far. The same answers as search (ties to the lowest entry), in a tenth of
the time: the whole table at every map's start took seconds on a 100 MHz
Pentium. */
static unsigned char by_green[256];

static int compare_green(const void *a, const void *b)
{
	int ga = r_palette_rgb[*(const unsigned char *)a * 3 + 1], gb = r_palette_rgb[*(const unsigned char *)b * 3 + 1];

	return ga != gb ? ga - gb : (int)*(const unsigned char *)a - (int)*(const unsigned char *)b;
}

static unsigned char search_sorted(int r, int g, int b, int start)
{
	long best = 0x7FFFFFFF;
	int found = 0;
	int low = start - 1, high = start;

	while (low >= 0 || high < 256)
	{
		if (high < 256)
		{
			int i = by_green[high];
			long dg = g - r_palette_rgb[i * 3 + 1];

			if (4 * dg * dg > best)
				high = 256;
			else
			{
				long d = distance(r, g, b, &r_palette_rgb[i * 3]);

				if (d < best || (d == best && i < found))
				{
					best = d;
					found = i;
				}
				high++;
			}
		}
		if (low >= 0)
		{
			int i = by_green[low];
			long dg = g - r_palette_rgb[i * 3 + 1];

			if (4 * dg * dg > best)
				low = -1;
			else
			{
				long d = distance(r, g, b, &r_palette_rgb[i * 3]);

				if (d < best || (d == best && i < found))
				{
					best = d;
					found = i;
				}
				low--;
			}
		}
	}
	return (unsigned char)found;
}

static void palette_tables(void)
{
	int start_of[256];

	/* the nearest colour of every 15-bit colour, at the middle of its
	square */
	for (int i = 0; i < 256; i++)
		by_green[i] = (unsigned char)i;
	qsort(by_green, 256, 1, compare_green);
	/* (where each green's search starts: the first entry at least as green) */
	for (int g = 0, k = 0; g < 256; g++)
	{
		while (k < 256 && r_palette_rgb[by_green[k] * 3 + 1] < g)
			k++;
		start_of[g] = k;
	}
	for (int c = 0; c < 32768; c++)
	{
		int r = ((c >> 10) & 31) * 8 + 4, g = ((c >> 5) & 31) * 8 + 4, b = (c & 31) * 8 + 4;

		r_inverse_palette[c] = search_sorted(r, g, b, start_of[g]);
	}
	/* light: level 16 is the colour, 0 black, 31 nearly twice */
	for (int level = 0; level < R_LIGHT_LEVELS; level++)
	{
		for (int p = 0; p < 256; p++)
		{
			const unsigned char *rgb = &r_palette_rgb[p * 3];

			r_color_map[level][p] = nearest_or_kept(rgb[0] * level / 16, rgb[1] * level / 16, rgb[2] * level / 16,
				(unsigned char)p);
		}
	}
	memcpy(r_color_map[R_LIGHT_LEVELS], r_color_map[R_LIGHT_LEVELS - 1], 256);
	fog_map_color = 0xFFFFFFFF;
	free(add_table);
	free(blend_table);
	add_table = NULL;
	blend_table = NULL;
}

void r_palette_set(const unsigned char *rgb)
{
	unsigned long fog = fog_map_color;

	memcpy(r_palette_rgb, rgb, sizeof(r_palette_rgb));
	palette_tables();
	r_palette_set_fog(fog == 0xFFFFFFFF ? 0x808080 : fog);
	r_textures_palette_changed();
}

void r_palette_initialize(void)
{
	int i = 0;

	for (int r = 0; r < 6; r++)
	{
		for (int g = 0; g < 7; g++)
		{
			for (int b = 0; b < 6; b++, i++)
			{
				r_palette_rgb[i * 3 + 0] = red_levels[r];
				r_palette_rgb[i * 3 + 1] = green_levels[g];
				r_palette_rgb[i * 3 + 2] = blue_levels[b];
			}
		}
	}
	for (int k = 0; k < 4; k++, i++)
	{
		r_palette_rgb[i * 3 + 0] = r_palette_rgb[i * 3 + 1] = r_palette_rgb[i * 3 + 2] = greys[k];
	}
	palette_tables();
	r_palette_set_fog(0x808080);
}

/* ---------- a map's palette: median cut of a weighted histogram

The box split next is the one whose widest channel times its weight is the
largest (so the colours most of the picture shows get the most entries),
cut along that channel where half its weight is on each side; each box's
colour is its cells' weighted mean. */

struct cell
{
	unsigned short key;
	unsigned long weight;
};

struct cut_box
{
	int first, count;
	unsigned long weight;
	int widest, range;
};

static int cut_channel;

static int cell_channel(unsigned short key, int channel)
{
	return (key >> (10 - 5 * channel)) & 31;
}

static int compare_cells(const void *a, const void *b)
{
	return cell_channel(((const struct cell *)a)->key, cut_channel) -
		cell_channel(((const struct cell *)b)->key, cut_channel);
}

static void measure_cut_box(const struct cell *cells, struct cut_box *box)
{
	int low[3] = { 31, 31, 31 }, high[3] = { 0, 0, 0 };

	box->weight = 0;
	for (int k = box->first; k < box->first + box->count; k++)
	{
		box->weight += cells[k].weight;
		for (int c = 0; c < 3; c++)
		{
			int value = cell_channel(cells[k].key, c);

			if (value < low[c]) low[c] = value;
			if (value > high[c]) high[c] = value;
		}
	}
	box->widest = 0;
	box->range = 0;
	for (int c = 0; c < 3 && box->count > 1; c++)
	{
		if (high[c] - low[c] > box->range)
		{
			box->range = high[c] - low[c];
			box->widest = c;
		}
	}
}

int r_palette_median_cut(const unsigned long *histogram, int wanted, unsigned long *colors)
{
	struct cell *cells = malloc(32768 * sizeof(*cells));
	struct cut_box *boxes = malloc((size_t)(wanted > 0 ? wanted : 1) * sizeof(*boxes));
	int cell_count = 0, box_count = 0;

	if (!cells || !boxes || wanted <= 0)
	{
		free(cells);
		free(boxes);
		return 0;
	}
	for (int k = 0; k < 32768; k++)
	{
		if (histogram[k])
		{
			cells[cell_count].key = (unsigned short)k;
			cells[cell_count].weight = histogram[k];
			cell_count++;
		}
	}
	if (cell_count)
	{
		boxes[0].first = 0;
		boxes[0].count = cell_count;
		measure_cut_box(cells, &boxes[0]);
		box_count = 1;
	}
	while (box_count && box_count < wanted)
	{
		int best = -1;
		float best_size = 0.0f;

		for (int i = 0; i < box_count; i++)
		{
			float size = (float)boxes[i].range * (float)boxes[i].weight;

			if (boxes[i].range > 0 && size > best_size)
			{
				best_size = size;
				best = i;
			}
		}
		if (best < 0)
			break;
		{
			struct cut_box *box = &boxes[best];
			unsigned long half = 0;
			int cut;

			cut_channel = box->widest;
			qsort(cells + box->first, (size_t)box->count, sizeof(struct cell), compare_cells);
			for (cut = box->first; cut < box->first + box->count - 1; cut++)
			{
				half += cells[cut].weight;
				if (half * 2 >= box->weight)
				{
					cut++;
					break;
				}
			}
			if (cut <= box->first)
				cut = box->first + 1;
			boxes[box_count].first = cut;
			boxes[box_count].count = box->first + box->count - cut;
			box->count = cut - box->first;
			measure_cut_box(cells, box);
			measure_cut_box(cells, &boxes[box_count]);
			box_count++;
		}
	}
	for (int b = 0; b < box_count; b++)
	{
		float total[3] = { 0, 0, 0 }, weight = 0;

		for (int k = boxes[b].first; k < boxes[b].first + boxes[b].count; k++)
		{
			for (int c = 0; c < 3; c++)
				total[c] += (float)cells[k].weight * (float)(cell_channel(cells[k].key, c) * 8 + 4);
			weight += (float)cells[k].weight;
		}
		colors[b] = ((unsigned long)(total[0] / weight) << 16) | ((unsigned long)(total[1] / weight) << 8) |
			(unsigned long)(total[2] / weight);
	}
	free(cells);
	free(boxes);
	return box_count;
}

void r_palette_set_fog(unsigned long fog_color)
{
	int fr = (int)((fog_color >> 16) & 0xFF), fg = (int)((fog_color >> 8) & 0xFF), fb = (int)(fog_color & 0xFF);

	if (fog_color == fog_map_color)
		return;
	fog_map_color = fog_color;
	for (int level = 0; level < R_FOG_LEVELS; level++)
	{
		for (int p = 0; p < 256; p++)
		{
			const unsigned char *rgb = &r_palette_rgb[p * 3];

			r_fog_map[level][p] = nearest_or_kept(rgb[0] + (fr - rgb[0]) * level / 16,
				rgb[1] + (fg - rgb[1]) * level / 16, rgb[2] + (fb - rgb[2]) * level / 16, (unsigned char)p);
		}
	}
}

const unsigned char *r_palette_add_table(void)
{
	if (!add_table)
	{
		add_table = malloc(65536);
		if (!add_table)
			return NULL;
		for (int a = 0; a < 256; a++)
		{
			for (int b = 0; b < 256; b++)
			{
				const unsigned char *x = &r_palette_rgb[a * 3], *y = &r_palette_rgb[b * 3];

				add_table[a * 256 + b] = nearest_or_kept(x[0] + y[0], x[1] + y[1], x[2] + y[2], (unsigned char)b);
			}
		}
	}
	return add_table;
}

const unsigned char *r_palette_blend_table(void)
{
	if (!blend_table)
	{
		blend_table = malloc(65536);
		if (!blend_table)
			return NULL;
		for (int a = 0; a < 256; a++)
		{
			for (int b = 0; b < 256; b++)
			{
				const unsigned char *x = &r_palette_rgb[a * 3], *y = &r_palette_rgb[b * 3];

				blend_table[a * 256 + b] = nearest_or_kept((x[0] + y[0]) / 2, (x[1] + y[1]) / 2, (x[2] + y[2]) / 2,
					(unsigned char)b);
			}
		}
	}
	return blend_table;
}

const unsigned char *r_palette(void)
{
	return r_palette_rgb;
}
