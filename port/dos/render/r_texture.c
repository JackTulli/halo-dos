/*
R_TEXTURE.C

Textures from 32-bit texels: the largest level shrunk to the settings'
texture size (a power of two on each side), the smaller levels by box
filter, and the palette: a median cut of the levels' colours over a
histogram of 15-bit colours (5 bits each of red, green and blue) or, for a
texture whose alpha blends (R_TEXTURE_ALPHA), of 16-bit ones (4 bits each
of alpha, red, green and blue). 256 colours, or 255 and a transparent one
(index 0) when alpha-tested. Each texel is then its histogram square's
colour. The palette's entries are also mapped to the 8-bit picture's
nearest colours (remap), so one texture serves both pictures.
*/

#include "r_internal.h"

#include <stdlib.h>
#include <string.h>

static unsigned long texture_memory;
/* r_texture_new's colour histogram, its palette index for each colour, and
its colours' list: kept from one texture to the next */
static unsigned long *histogram_store;
static unsigned char *index_store;
static struct bucket *bucket_store;
/* every texture, for a new palette (r_textures_palette_changed) */
static r_texture *textures;
static int baking_deferred;

unsigned long r_texture_memory(void)
{
	return texture_memory;
}

unsigned long r_texture_average(const r_texture *texture)
{
	return texture ? texture->average : 0xFF808080;
}

/* ---------- sizes */

static int log2_floor(int value)
{
	int shift = 0;

	while ((2 << shift) <= value)
		shift++;
	return shift;
}

/* the image resampled to width x height (nearest texel), for sides that are
no power of two */
static unsigned long *resample(const unsigned long *source, int source_width, int source_height, int width,
	int height)
{
	unsigned long *image = malloc((size_t)width * height * 4);

	if (!image)
		return NULL;
	for (int y = 0; y < height; y++)
	{
		const unsigned long *row = source + (size_t)(y * source_height / height) * source_width;

		for (int x = 0; x < width; x++)
			image[y * width + x] = row[x * source_width / width];
	}
	return image;
}

/* half the image on each side that is over 1 (box filter) */
static unsigned long *halve(const unsigned long *source, int width, int height, int *new_width, int *new_height)
{
	int step_x = width > 1 ? 2 : 1, step_y = height > 1 ? 2 : 1;
	int half_width = width / step_x, half_height = height / step_y;
	unsigned long *image = malloc((size_t)half_width * half_height * 4);

	if (!image)
		return NULL;
	if (step_x == 2 && step_y == 2)
	{
		/* (the usual case: four texels, two channels to an add, red and
		blue in one word's halves, alpha and green in another's; the same
		sums as below) */
		for (int y = 0; y < half_height; y++)
		{
			const unsigned long *row0 = source + (size_t)(y * 2) * width, *row1 = row0 + width;
			unsigned long *out = image + (size_t)y * half_width;

			for (int x = 0; x < half_width; x++, row0 += 2, row1 += 2)
			{
				unsigned long rb = (row0[0] & 0x00FF00FF) + (row0[1] & 0x00FF00FF) + (row1[0] & 0x00FF00FF) +
					(row1[1] & 0x00FF00FF);
				unsigned long ag = ((row0[0] >> 8) & 0x00FF00FF) + ((row0[1] >> 8) & 0x00FF00FF) +
					((row1[0] >> 8) & 0x00FF00FF) + ((row1[1] >> 8) & 0x00FF00FF);

				out[x] = ((rb >> 2) & 0x00FF00FF) | (((ag >> 2) & 0x00FF00FF) << 8);
			}
		}
		*new_width = half_width;
		*new_height = half_height;
		return image;
	}
	for (int y = 0; y < half_height; y++)
	{
		for (int x = 0; x < half_width; x++)
		{
			unsigned long sum[4] = { 0, 0, 0, 0 };
			int count = step_x * step_y;

			for (int dy = 0; dy < step_y; dy++)
			{
				for (int dx = 0; dx < step_x; dx++)
				{
					unsigned long c = source[(size_t)(y * step_y + dy) * width + x * step_x + dx];

					sum[0] += c >> 24;
					sum[1] += (c >> 16) & 0xFF;
					sum[2] += (c >> 8) & 0xFF;
					sum[3] += c & 0xFF;
				}
			}
			image[y * half_width + x] = ((sum[0] / count) << 24) | ((sum[1] / count) << 16) |
				((sum[2] / count) << 8) | (sum[3] / count);
		}
	}
	*new_width = half_width;
	*new_height = half_height;
	return image;
}

/* ---------- the histogram's colours */

struct format
{
	/* 3 (red, green, blue: 5 bits each) or 4 (alpha, red, green, blue: 4) */
	int channels, bits;
};

static unsigned long key_of(const struct format *format, unsigned long c)
{
	if (format->channels == 3)
		return ((c >> 9) & 0x7C00) | ((c >> 6) & 0x03E0) | ((c >> 3) & 0x001F);
	return ((c >> 16) & 0xF000) | ((c >> 12) & 0x0F00) | ((c >> 8) & 0x00F0) | ((c >> 4) & 0x000F);
}

static int channel_of(const struct format *format, unsigned long key, int which)
{
	return (int)(key >> (format->bits * (format->channels - 1 - which))) & ((1 << format->bits) - 1);
}

/* the 8-bit value of a channel at the middle of its step */
static unsigned long expand(const struct format *format, int value)
{
	return format->bits == 5 ? (unsigned long)(value * 8 + 4) : (unsigned long)(value * 17);
}

/* ---------- the palette: median cut */

struct bucket
{
	unsigned short key;
	unsigned long count;
};

struct box
{
	int first, count;
	/* its widest channel and how wide */
	int widest, range;
};

static const struct format *sort_format;
static int sort_channel;

static int compare_buckets(const void *a, const void *b)
{
	return channel_of(sort_format, ((const struct bucket *)a)->key, sort_channel) -
		channel_of(sort_format, ((const struct bucket *)b)->key, sort_channel);
}

static void measure_box(const struct format *format, const struct bucket *buckets, struct box *box)
{
	int low[4] = { 31, 31, 31, 31 }, high[4] = { 0, 0, 0, 0 };

	for (int k = box->first; k < box->first + box->count; k++)
	{
		for (int c = 0; c < format->channels; c++)
		{
			int value = channel_of(format, buckets[k].key, c);

			if (value < low[c]) low[c] = value;
			if (value > high[c]) high[c] = value;
		}
	}
	box->widest = 0;
	box->range = box->count > 1 ? high[0] - low[0] : 0;
	for (int c = 1; c < format->channels && box->count > 1; c++)
	{
		if (high[c] - low[c] > box->range)
		{
			box->range = high[c] - low[c];
			box->widest = c;
		}
	}
}

/* splits the boxes until there are as many as wanted (or none can be
split); the buckets end up grouped by box */
static int median_cut(const struct format *format, struct bucket *buckets, int bucket_count, struct box *boxes,
	int wanted)
{
	int box_count = 1;

	boxes[0].first = 0;
	boxes[0].count = bucket_count;
	measure_box(format, buckets, &boxes[0]);
	while (box_count < wanted)
	{
		int best = -1, best_range = 0;

		/* the box whose widest channel is widest */
		for (int i = 0; i < box_count; i++)
		{
			if (boxes[i].range > best_range)
			{
				best_range = boxes[i].range;
				best = i;
			}
		}
		if (best < 0)
			break;
		/* sorted along it, cut where half its texels are on each side */
		{
			struct box *box = &boxes[best];
			unsigned long total = 0, half = 0;
			int cut;

			sort_format = format;
			sort_channel = box->widest;
			qsort(buckets + box->first, (size_t)box->count, sizeof(struct bucket), compare_buckets);
			for (int k = box->first; k < box->first + box->count; k++)
				total += buckets[k].count;
			for (cut = box->first; cut < box->first + box->count - 1; cut++)
			{
				half += buckets[cut].count;
				if (half * 2 >= total)
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
			measure_box(format, buckets, box);
			measure_box(format, buckets, &boxes[box_count]);
			box_count++;
		}
	}
	return box_count;
}

/* ---------- the 8-bit picture's palette

An opaque texture's texels become the picture's palette indices (one lookup
a pixel fewer in r_soft.c's spans): its palette is then the picture's, its
remap nothing. A cut-out or blended one keeps its own palette (index 0
transparent; the alpha) and its remap to the picture's. */

static int bakes(const r_texture *texture)
{
	return r_frame.bits == 8 && !(texture->flags & (R_TEXTURE_ALPHA | R_TEXTURE_ALPHA_TEST));
}

static void bake(r_texture *texture)
{
	for (int level = 0; level < texture->levels; level++)
	{
		unsigned long count = (1UL << (texture->width_shift > level ? texture->width_shift - level : 0)) *
			(1UL << (texture->height_shift > level ? texture->height_shift - level : 0));
		unsigned char *texels = texture->texels[level];

		for (unsigned long k = 0; k < count; k++)
			texels[k] = texture->remap[texels[k]];
	}
	for (int i = 0; i < 256; i++)
	{
		texture->palette[i] = 0xFF000000 | ((unsigned long)r_palette_rgb[i * 3] << 16) |
			((unsigned long)r_palette_rgb[i * 3 + 1] << 8) | r_palette_rgb[i * 3 + 2];
		texture->remap[i] = (unsigned char)i;
	}
	texture->baked = 1;
}

void r_textures_defer_baking(int defer)
{
	baking_deferred = defer;
}

void r_textures_palette_changed(void)
{
	for (r_texture *texture = textures; texture; texture = texture->next)
	{
		/* (one baked into another palette cannot be redone: none is, the
		textures go with their map) */
		if (texture->baked)
			continue;
		for (int i = 0; i < 256; i++)
			texture->remap[i] = r_palette_nearest(texture->palette[i]);
		if (bakes(texture) && !baking_deferred)
			bake(texture);
	}
}

int r_texture_colors(const r_texture *texture, unsigned long *colors, int maximum)
{
	int level, width, height, count = 0;

	if (!texture || !texture->levels)
		return 0;
	for (level = texture->levels - 1; level > 0; level--)
	{
		if (texture->width_shift - level >= 3 && texture->height_shift - level >= 3)
			break;
	}
	width = 1 << (texture->width_shift > level ? texture->width_shift - level : 0);
	height = 1 << (texture->height_shift > level ? texture->height_shift - level : 0);
	for (int k = 0; k < width * height && count < maximum; k += (width * height + maximum - 1) / maximum)
	{
		unsigned char index = texture->texels[level][k];

		if ((texture->flags & R_TEXTURE_ALPHA_TEST) && !index)
			continue;
		colors[count++] = texture->palette[index] & 0xFFFFFF;
	}
	return count;
}

/* ---------- textures */

static void free_levels(unsigned long **levels, int count)
{
	for (int i = 0; i < count; i++)
		free(levels[i]);
}

/* the texture's levels from its texels: the largest a power of two on each
side, at most limit, then halvings down to 4 texels; how many (0: no
memory) */
static int make_levels(const unsigned long *argb, int width, int height, int limit, unsigned long **levels,
	int *widths, int *heights)
{
	int target_width = 1 << log2_floor(width), target_height = 1 << log2_floor(height);
	int image_width = target_width, image_height = target_height;
	int level_count;
	unsigned long *image;

	if (target_width != width || target_height != height)
		image = resample(argb, width, height, target_width, target_height);
	else
	{
		image = malloc((size_t)width * height * 4);
		if (image)
			memcpy(image, argb, (size_t)width * height * 4);
	}
	if (!image)
		return 0;
	while (image_width > limit || image_height > limit)
	{
		int w, h;
		unsigned long *smaller = halve(image, image_width, image_height, &w, &h);

		if (!smaller)
			break;
		free(image);
		image = smaller;
		image_width = w;
		image_height = h;
	}
	levels[0] = image;
	widths[0] = image_width;
	heights[0] = image_height;
	level_count = 1;
	while (level_count < R_MAXIMUM_LEVELS && (widths[level_count - 1] > 4 || heights[level_count - 1] > 4))
	{
		int w, h;
		unsigned long *smaller = halve(levels[level_count - 1], widths[level_count - 1], heights[level_count - 1],
			&w, &h);

		if (!smaller)
			break;
		levels[level_count] = smaller;
		widths[level_count] = w;
		heights[level_count] = h;
		level_count++;
	}
	return level_count;
}

/* a hardware rasterizer that draws in full colour (r_backend's
texture_created) is given the largest level as it was before the palette:
taken from the levels before they are freed */
static void keep_colours(r_texture *texture, unsigned long **levels, const int *widths, const int *heights)
{
	if (!r_backend || !r_backend->texture_created || !levels[0])
		return;
	texture->argb = levels[0];
	texture->argb_width = widths[0];
	texture->argb_height = heights[0];
	levels[0] = NULL;
}

r_texture *r_texture_new(const unsigned long *argb, int width, int height, int flags)
{
	r_texture *texture;
	unsigned long *levels[R_MAXIMUM_LEVELS];
	int widths[R_MAXIMUM_LEVELS], heights[R_MAXIMUM_LEVELS];
	int level_count = 0;
	int limit = r_frame.settings.texture_size > 0 ? r_frame.settings.texture_size : 128;
	/* (a terrain tile's texels are already the size its ground needs) */
	if ((flags & R_TEXTURE_LARGE) && limit < 512)
		limit = 512;
	unsigned long *histogram;
	struct bucket *buckets;
	struct box boxes[256];
	int bucket_count = 0, box_count;
	int alpha_test = (flags & R_TEXTURE_ALPHA_TEST) != 0;
	struct format format;
	unsigned char *index_of;
	unsigned long key_count;
	unsigned long total_texels = 0;
	unsigned long sum[4] = { 0, 0, 0, 0 };

	if (!argb || width <= 0 || height <= 0)
		return NULL;
	if (alpha_test)
	{
		/* (an alpha test that would cut nothing out: an opaque texture, the
		rasterizer's fastest) */
		long i, count = (long)width * height;

		for (i = 0; i < count && (argb[i] >> 24) >= 0x80; i++)
			;
		if (i == count)
		{
			alpha_test = 0;
			flags &= ~R_TEXTURE_ALPHA_TEST;
		}
	}
	format.channels = (flags & R_TEXTURE_ALPHA) ? 4 : 3;
	format.bits = (flags & R_TEXTURE_ALPHA) ? 4 : 5;
	key_count = 1UL << (format.channels * format.bits);
	texture = calloc(1, sizeof(*texture));
	if (!texture)
		return NULL;
	texture->flags = flags;

	level_count = make_levels(argb, width, height, limit, levels, widths, heights);
	if (!level_count)
	{
		free(texture);
		return NULL;
	}

	/* the colours of every level (the opaque texels of an alpha-tested one),
	in a histogram kept from one texture to the next (a map makes hundreds:
	clearing a fresh one each time was most of their making), emptied again
	at the colours it took */
	if (!histogram_store)
	{
		histogram_store = calloc(65536, sizeof(unsigned long));
		index_store = malloc(65536);
		bucket_store = malloc(65536 * sizeof(struct bucket));
		if (!histogram_store || !index_store || !bucket_store)
		{
			free(histogram_store);
			free(index_store);
			free(bucket_store);
			histogram_store = NULL;
			free_levels(levels, level_count);
			free(texture);
			return NULL;
		}
	}
	histogram = histogram_store;
	index_of = index_store;
	buckets = bucket_store;
	for (int i = 0; i < level_count; i++)
	{
		unsigned long count = (unsigned long)widths[i] * heights[i];

		for (unsigned long k = 0; k < count; k++)
		{
			unsigned long c = levels[i][k];

			if (alpha_test && (c >> 24) < 0x80)
				continue;
			histogram[key_of(&format, c)]++;
			if (i == 0)
			{
				sum[0] += c >> 24;
				sum[1] += (c >> 16) & 0xFF;
				sum[2] += (c >> 8) & 0xFF;
				sum[3] += c & 0xFF;
				total_texels++;
			}
		}
	}
	for (unsigned long k = 0; k < key_count; k++)
	{
		if (histogram[k])
		{
			buckets[bucket_count].key = (unsigned short)k;
			buckets[bucket_count].count = histogram[k];
			bucket_count++;
		}
	}
	/* (the histogram empty for the next texture: only its colours were set) */
	for (int k = 0; k < bucket_count; k++)
		histogram[buckets[k].key] = 0;
	box_count = bucket_count ? median_cut(&format, buckets, bucket_count, boxes, alpha_test ? 255 : 256) : 0;
	for (int b = 0; b < box_count; b++)
	{
		unsigned long weight = 0, total[4] = { 0, 0, 0, 0 };
		int index = b + (alpha_test ? 1 : 0);

		for (int k = boxes[b].first; k < boxes[b].first + boxes[b].count; k++)
		{
			unsigned long n = buckets[k].count;

			for (int c = 0; c < format.channels; c++)
				total[c] += n * expand(&format, channel_of(&format, buckets[k].key, c));
			weight += n;
			index_of[buckets[k].key] = (unsigned char)index;
		}
		if (!weight)
			continue;
		if (format.channels == 4)
		{
			texture->palette[index] = ((total[0] / weight) << 24) | ((total[1] / weight) << 16) |
				((total[2] / weight) << 8) | (total[3] / weight);
		}
		else
		{
			texture->palette[index] = 0xFF000000 | ((total[0] / weight) << 16) | ((total[1] / weight) << 8) |
				(total[2] / weight);
		}
	}
	for (int i = 0; i < 256; i++)
		texture->remap[i] = r_palette_nearest(texture->palette[i]);

	/* the texels */
	texture->levels = level_count;
	texture->width_shift = log2_floor(widths[0]);
	texture->height_shift = log2_floor(heights[0]);
	for (int i = 0; i < level_count; i++)
	{
		unsigned long count = (unsigned long)widths[i] * heights[i];
		unsigned char *texels = malloc(count);

		if (!texels)
		{
			texture->levels = i;
			break;
		}
		for (unsigned long k = 0; k < count; k++)
		{
			unsigned long c = levels[i][k];

			texels[k] = alpha_test && (c >> 24) < 0x80 ? 0 : index_of[key_of(&format, c)];
		}
		texture->texels[i] = texels;
		texture->bytes += count;
	}
	texture->bytes += sizeof(*texture);
	texture_memory += texture->bytes;
	texture->average = total_texels ? ((sum[0] / total_texels) << 24) | ((sum[1] / total_texels) << 16) |
		((sum[2] / total_texels) << 8) | (sum[3] / total_texels) : 0xFF808080;
	keep_colours(texture, levels, widths, heights);
	free_levels(levels, level_count);
	texture->next = textures;
	textures = texture;
	if (!texture->levels)
	{
		r_texture_delete(texture);
		return NULL;
	}
	if (bakes(texture) && !baking_deferred)
		bake(texture);
	if (r_backend && r_backend->texture_created)
		r_backend->texture_created(texture);
	return texture;
}

/* ---------- shared palettes

Textures of one look (a map's terrain tiles: one shader's colour map and
details, baked square by square) can share a palette instead of each
making its own: the histogram, the sort and the median cut, most of a
texture's making, then happen once. The palette is a median cut of the
colours of all of them (added to a histogram first, a preview of each
will do); each texel is its nearest colour in it. */

struct r_shared_palette
{
	unsigned long palette[256];
	int colors;
	/* each 15-bit colour's nearest entry, where known (the previews'
	colours at once, others the first time a texture has them) */
	unsigned char index_of[32768];
	unsigned char known[32768];
};

unsigned long *r_histogram_new(void)
{
	return calloc(32768, sizeof(unsigned long));
}

void r_histogram_add(unsigned long *histogram, const unsigned long *argb, long count)
{
	static const struct format format = { 3, 5 };

	for (long i = 0; i < count; i++)
		histogram[key_of(&format, argb[i])]++;
}

/* the nearest of a palette's colours to an RGB (as r_palette.c weighs them) */
static int nearest_entry(const unsigned long *palette, int colors, int r, int g, int b)
{
	long best = 0x7FFFFFFF;
	int found = 0;

	for (int i = 0; i < colors; i++)
	{
		long dr = r - (long)((palette[i] >> 16) & 0xFF), dg = g - (long)((palette[i] >> 8) & 0xFF);
		long db = b - (long)(palette[i] & 0xFF);
		long d = 3 * dr * dr + 4 * dg * dg + 2 * db * db;

		if (d < best)
		{
			best = d;
			found = i;
		}
	}
	return found;
}

r_shared_palette *r_shared_palette_new(const unsigned long *histogram)
{
	static const struct format format = { 3, 5 };
	r_shared_palette *shared = calloc(1, sizeof(*shared));
	struct bucket *buckets = malloc(32768 * sizeof(struct bucket));
	struct box boxes[256];
	int bucket_count = 0, box_count;

	if (!shared || !buckets)
	{
		free(shared);
		free(buckets);
		return NULL;
	}
	for (unsigned long k = 0; k < 32768; k++)
	{
		if (histogram[k])
		{
			buckets[bucket_count].key = (unsigned short)k;
			buckets[bucket_count].count = histogram[k];
			bucket_count++;
		}
	}
	box_count = bucket_count ? median_cut(&format, buckets, bucket_count, boxes, 256) : 0;
	for (int b = 0; b < box_count; b++)
	{
		unsigned long weight = 0, total[3] = { 0, 0, 0 };

		for (int k = boxes[b].first; k < boxes[b].first + boxes[b].count; k++)
		{
			for (int c = 0; c < 3; c++)
				total[c] += buckets[k].count * expand(&format, channel_of(&format, buckets[k].key, c));
			weight += buckets[k].count;
			shared->index_of[buckets[k].key] = (unsigned char)b;
			shared->known[buckets[k].key] = 1;
		}
		if (weight)
			shared->palette[b] = 0xFF000000 | ((total[0] / weight) << 16) | ((total[1] / weight) << 8) | (total[2] / weight);
	}
	shared->colors = box_count ? box_count : 1;
	free(buckets);
	return shared;
}

void r_shared_palette_delete(r_shared_palette *shared)
{
	free(shared);
}

r_texture *r_texture_new_shared(const unsigned long *argb, int width, int height, int flags,
	r_shared_palette *shared)
{
	static const struct format format = { 3, 5 };
	unsigned long *levels[R_MAXIMUM_LEVELS];
	int widths[R_MAXIMUM_LEVELS], heights[R_MAXIMUM_LEVELS];
	int limit = r_frame.settings.texture_size > 0 ? r_frame.settings.texture_size : 128;
	int level_count;
	unsigned long sum[4] = { 0, 0, 0, 0 };
	r_texture *texture;

	/* (opaque ones only: no alpha in a shared palette) */
	if (!shared || (flags & (R_TEXTURE_ALPHA | R_TEXTURE_ALPHA_TEST)))
		return r_texture_new(argb, width, height, flags);
	if (!argb || width <= 0 || height <= 0 || !(texture = calloc(1, sizeof(*texture))))
		return NULL;
	if ((flags & R_TEXTURE_LARGE) && limit < 512)
		limit = 512;
	texture->flags = flags;
	level_count = make_levels(argb, width, height, limit, levels, widths, heights);
	if (!level_count)
	{
		free(texture);
		return NULL;
	}
	memcpy(texture->palette, shared->palette, sizeof(texture->palette));
	for (int i = 0; i < 256; i++)
		texture->remap[i] = r_palette_nearest(texture->palette[i]);
	texture->levels = level_count;
	texture->width_shift = log2_floor(widths[0]);
	texture->height_shift = log2_floor(heights[0]);
	for (int i = 0; i < level_count; i++)
	{
		unsigned long count = (unsigned long)widths[i] * heights[i];
		unsigned char *texels = malloc(count);

		if (!texels)
		{
			texture->levels = i;
			break;
		}
		for (unsigned long k = 0; k < count; k++)
		{
			unsigned long c = levels[i][k];

			unsigned long key = key_of(&format, c);

			/* (a colour no preview had: its nearest, once) */
			if (!shared->known[key])
			{
				shared->index_of[key] = (unsigned char)nearest_entry(shared->palette, shared->colors,
					(int)expand(&format, channel_of(&format, key, 0)), (int)expand(&format, channel_of(&format, key, 1)),
					(int)expand(&format, channel_of(&format, key, 2)));
				shared->known[key] = 1;
			}
			texels[k] = shared->index_of[key];
			if (i == 0)
			{
				sum[1] += (c >> 16) & 0xFF;
				sum[2] += (c >> 8) & 0xFF;
				sum[3] += c & 0xFF;
			}
		}
		texture->texels[i] = texels;
		texture->bytes += count;
	}
	{
		unsigned long n = (unsigned long)widths[0] * heights[0];

		texture->average = 0xFF000000 | ((sum[1] / n) << 16) | ((sum[2] / n) << 8) | (sum[3] / n);
	}
	texture->bytes += sizeof(*texture);
	texture_memory += texture->bytes;
	keep_colours(texture, levels, widths, heights);
	free_levels(levels, level_count);
	texture->next = textures;
	textures = texture;
	if (!texture->levels)
	{
		r_texture_delete(texture);
		return NULL;
	}
	if (bakes(texture) && !baking_deferred)
		bake(texture);
	if (r_backend && r_backend->texture_created)
		r_backend->texture_created(texture);
	return texture;
}

void r_texture_delete(r_texture *texture)
{
	if (!texture)
		return;
	for (r_texture **link = &textures; *link; link = &(*link)->next)
	{
		if (*link == texture)
		{
			*link = texture->next;
			break;
		}
	}
	if (r_backend && texture->hardware)
		r_backend->texture_delete(texture);
	for (int i = 0; i < R_MAXIMUM_LEVELS; i++)
		free(texture->texels[i]);
	free(texture->argb);
	texture_memory -= texture->bytes;
	free(texture);
}
