/*
DOS_TEXTURES.C

Textures and materials for the DOS and Windows 95 renderer.

A bitmap's texels are read from the map file where the Xbox's texture
cache read them (texture_cache_bitmap_new moves its pixels' offset to the
file's), at the level nearest the renderer's texture size, and decoded by
the PC port's Xbox texel converter (port/linux/src/xbox_textures_convert.c:
swizzling, DXT, every format) to 32-bit texels for r_texture_new.

A shader becomes one texture and how it draws (dos_material). Halo's
environment shaders multiply their base map by detail maps at twice their
value: the detail maps' average is multiplied in here, once; their light is
the vertices' (dos_level.c). Its other shaders keep their first map. The
field offsets are Invader's definitions' (tools/n64 reads the same).
*/

#include "cseries.h"

#include "bitmaps/bitmap_group.h"
#include "bitmaps/bitmaps.h"
#include "cache/cache_files.h"
#include "shaders/shader_definitions.h"
#include "tag_files/tag_groups.h"

#include "halo_system.h"
#include "dos_render.h"
#include "../../support/xbox_textures_convert.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

long bitmap_get_pixel_data_size(struct bitmap_data *bitmap);
int dos_map_read(unsigned long offset, void *buffer, unsigned long size);
long dos_setting_integer(const char *name, long default_value);
int dos_import_contains(const void *pointer);
int dos_import_read(unsigned long offset, void *buffer, unsigned long size);
/* Halo Custom Edition's maps (dos_ce.c; ChupathingyCE's ce_hud.c, ce_repairs.c):
a tag's pixels in the map or in bitmaps.map; its bitmaps are laid out as Halo
PC's, a HUD meter's channels are Halo PC's, and MCC's BC7 is not decoded */
int dos_ce_read(long tag_index, unsigned long offset, void *buffer, unsigned long size);
boolean cache_file_tags_are_ce(void);
boolean ce_hud_bitmap_is_meter(void const *bitmap);
boolean ce_bitmap_is_bc7(void const *bitmap);

/* the bitmaps' formats and flags (bitmaps.c) */
enum
{
	_format_a8, _format_y8, _format_ay8, _format_a8y8, _format_unused1, _format_unused2, _format_r5g6b5,
	_format_unused3, _format_a1r5g5b5, _format_a4r4g4b4, _format_x8r8g8b8, _format_a8r8g8b8, _format_unused4,
	_format_unused5, _format_dxt1, _format_dxt3, _format_dxt5, _format_p8_bump,
};

#define BITMAP_FLAG_SWIZZLED 0x0008
#define BITMAP_FLAG_CACHED 0x0080

static void *raw_malloc(size_t size) { return (malloc)(size); }
static void raw_free(void *pointer) { (free)(pointer); }

/* ---------- the Xbox texture cache's part (cache/xbox_texture_cache.c) */

void texture_cache_open(void) { return; }
void texture_cache_close(void) { return; }

void texture_cache_bitmap_new(
	long bitmap_tag_index,
	struct bitmap_data *bitmap)
{
	struct bitmap_group *bitmap_group = bitmap_group_get(bitmap_tag_index);

	bitmap->flags |= BITMAP_FLAG_CACHED;
	bitmap->cache_block_index = NONE;
	bitmap->base_address = NULL;
	bitmap->hardware_format = NULL;
	/* (a Custom Edition map's bitmap's offset is its file's already:
ChupathingyCE's ce_resources.c) */
	if (!cache_file_tags_are_ce())
		bitmap->pixels_offset += bitmap_group->pixel_data.file_offset;
	bitmap->pixels_size = bitmap_get_pixel_data_size(bitmap);
	bitmap->tag_index = bitmap_tag_index;
}

void texture_cache_bitmap_delete(
	struct bitmap_data *bitmap)
{
	bitmap->flags &= ~BITMAP_FLAG_CACHED;
	bitmap->hardware_format = NULL;
}

/* ---------- decoding */

static int texel_kind(short format, int *bytes, int *block)
{
	*block = 0;
	switch (format)
	{
	case _format_a8: *bytes = 1; return _texture_kind_a8;
	case _format_y8: *bytes = 1; return _texture_kind_l8;
	case _format_ay8: *bytes = 1; return _texture_kind_al8;
	case _format_a8y8: *bytes = 2; return _texture_kind_a8l8;
	case _format_r5g6b5: *bytes = 2; return _texture_kind_r5g6b5;
	case _format_a1r5g5b5: *bytes = 2; return _texture_kind_a1r5g5b5;
	case _format_a4r4g4b4: *bytes = 2; return _texture_kind_a4r4g4b4;
	case _format_x8r8g8b8: *bytes = 4; return _texture_kind_x8r8g8b8;
	case _format_a8r8g8b8: *bytes = 4; return _texture_kind_a8r8g8b8;
	case _format_dxt1: *bytes = 0; *block = 8; return _texture_kind_dxt1;
	case _format_dxt3: *bytes = 0; *block = 16; return _texture_kind_dxt3;
	case _format_dxt5: *bytes = 0; *block = 16; return _texture_kind_dxt5;
	default: *bytes = 0; return _texture_kind_unknown;
	}
}

static unsigned long level_size(int width, int height, int bytes, int block)
{
	if (block)
		return (unsigned long)((width + 3) / 4) * (unsigned long)((height + 3) / 4) * (unsigned long)block;
	return (unsigned long)width * (unsigned long)height * (unsigned long)bytes;
}

/* Halo's bitmap types */
#define BITMAP_TYPE_2D 0
#define BITMAP_TYPE_CUBE_MAP 2

/* size bytes of a bitmap's pixels, offset bytes in: from the map's file, the
map's whose tags joined it (dos_import.c), or a Custom Edition map's
bitmaps.map (dos_ce.c) */
static int bitmap_read(struct bitmap_data *bitmap, unsigned long offset, void *buffer, unsigned long size)
{
	if (dos_import_contains(bitmap))
		return dos_import_read((unsigned long)bitmap->pixels_offset + offset, buffer, size);
	return dos_ce_read(bitmap->tag_index, (unsigned long)bitmap->pixels_offset + offset, buffer, size);
}

/* where a cube map's face starts: the faces one after another, each with
its levels, rounded up to D3DTEXTURE_CUBEFACE_ALIGNMENT (128 bytes) */
static unsigned long face_offset(struct bitmap_data *bitmap, int face, int bytes, int block)
{
	unsigned long size = 0;
	int width = bitmap->width, height = bitmap->height;

	for (int level = 0; level <= bitmap->mipmap_count; level++)
	{
		size += level_size(width, height, bytes, block);
		width = width > 1 ? width / 2 : 1;
		height = height > 1 ? height / 2 : 1;
	}
	return ((size + 127) & ~127UL) * (unsigned long)face;
}

/* a 2D bitmap's texels, or (face 0 to 5) a cube map's face's */
static unsigned long *decode(struct bitmap_data *bitmap, int face, int largest, int *width_out, int *height_out)
{
	int bytes, block, kind = texel_kind(bitmap->format, &bytes, &block);
	int level = 0, width = bitmap->width, height = bitmap->height;
	unsigned long offset = 0, size;
	unsigned char *source;
	unsigned long *texels;

	/* (2D bitmaps, and the faces of cube maps for the skies: volumes and
	reflections are lights and shine, which this renderer does not draw) */
	if (kind == _texture_kind_unknown || width <= 0 || height <= 0 || !(bitmap->flags & BITMAP_FLAG_CACHED) ||
		bitmap->type != (face < 0 ? BITMAP_TYPE_2D : BITMAP_TYPE_CUBE_MAP))
	{
		return NULL;
	}
	/* (MCC's BC7, in a Custom Edition map: not decoded here) */
	if (cache_file_tags_are_ce() && ce_bitmap_is_bc7(bitmap))
		return NULL;
	while ((width > largest || height > largest) && level < bitmap->mipmap_count)
	{
		offset += level_size(width, height, bytes, block);
		width = width > 1 ? width / 2 : 1;
		height = height > 1 ? height / 2 : 1;
		level++;
	}
	size = level_size(width, height, bytes, block);
	/* (a cube map's face: the Xbox's are one after another, each with its
	levels; Halo PC's levels are one after another, each with its six faces,
	the second and third swapped, as ChupathingyCE's xbox_textures.c reads
	them) */
	if (face >= 0 && cache_file_tags_are_ce())
		offset = offset * 6 + (unsigned long)(face == 1 ? 2 : face == 2 ? 1 : face) * size;
	else if (face >= 0)
		offset += face_offset(bitmap, face, bytes, block);
	if (offset + size > (unsigned long)bitmap->pixels_size)
		return NULL;
	source = raw_malloc(size);
	texels = raw_malloc((size_t)width * height * 4);
	if (!source || !texels)
	{
		raw_free(source);
		raw_free(texels);
		return NULL;
	}
	/* (a bitmap of the map whose tags joined this one's is in its file:
	dos_import.c) */
	if (!bitmap_read(bitmap, offset, source, size))
	{
		static int logged;

		if (logged++ < 8)
		{
			char line[128];

			snprintf(line, sizeof(line), "texture: pixels at %lu (%lu bytes, imported %d) cannot be read",
				(unsigned long)bitmap->pixels_offset + offset, size, dos_import_contains(bitmap));
			system_log(line);
		}
		raw_free(source);
		raw_free(texels);
		return NULL;
	}
	if (block)
	{
		/* (levels smaller than a block: the converter takes them whole) */
		texture_convert_dxt_level(kind, _texture_destination_bgra8, texels, (unsigned long)width * 4, source,
			(unsigned long)width, (unsigned long)height, 1);
	}
	else
	{
		texture_convert_level(kind, _texture_destination_bgra8, texels, (unsigned long)width * 4, source,
			(unsigned long)width, (unsigned long)height, 1,
			/* (Halo PC's bitmaps are never swizzled: ChupathingyCE reads a Custom
			Edition map's all in rows) */
			(bitmap->flags & BITMAP_FLAG_SWIZZLED) && !cache_file_tags_are_ce() ? 0 : (unsigned long)width * bytes, NULL);
	}
	raw_free(source);
	/* (a Custom Edition map's HUD meter keeps its shape in its colour and the
	order it fills in in its alpha, the Xbox's the other way round:
	ChupathingyCE's ce_hud.c, xbox_textures.c) */
	if (cache_file_tags_are_ce() && ce_hud_bitmap_is_meter(bitmap))
	{
		for (long i = 0; i < (long)width * height; i++)
		{
			unsigned long texel = texels[i], alpha = texel >> 24, red = (texel >> 16) & 0xFF;

			texels[i] = (red << 24) | (alpha << 16) | (alpha << 8) | alpha;
		}
	}
	*width_out = width;
	*height_out = height;
	return texels;
}

unsigned long *dos_bitmap_decode(struct bitmap_data *bitmap, int largest, int *width_out, int *height_out)
{
	return decode(bitmap, -1, largest, width_out, height_out);
}

/* ---------- textures */

#define TEXTURE_SLOTS 1024

static struct texture_slot
{
	struct bitmap_data *bitmap;
	int flags;
	r_texture *texture;
	/* or the card's own texture (the Xbox's texture pass: HARDWARE_SLOT) */
	unsigned long hardware;
	int failed;
} texture_slots[TEXTURE_SLOTS];
static int texture_slot_count;

/* the largest side a texture is decoded at: the setting's (texture_size),
or while an interface picture is (the HUD's, the menu's: drawn at about
their own size on the screen, they are not shrunk to the world's
textures' limit; the renderer keeps R_TEXTURE_LARGE ones up to 512)
INTERFACE_TEXTURE_SIZE at least */
#define INTERFACE_TEXTURE_SIZE 512

static int interface_texture;

static int texture_size(void)
{
	extern struct r_settings dos_render_settings;
	int size = dos_render_settings.texture_size > 0 ? dos_render_settings.texture_size : 128;

	return interface_texture && size < INTERFACE_TEXTURE_SIZE ? INTERFACE_TEXTURE_SIZE : size;
}

static struct bitmap_data *bitmap_of(long bitmap_tag_index, short bitmap_index)
{
	struct bitmap_group *group;
	struct bitmap_data *bitmap;

	if (bitmap_tag_index == NONE)
		return NULL;
	group = bitmap_group_get(bitmap_tag_index);
	if (!group || bitmap_index < 0 || bitmap_index >= group->bitmaps.count)
		return NULL;
	bitmap = TAG_BLOCK_GET_ELEMENT(&group->bitmaps, bitmap_index, struct bitmap_data);
	/* (one the game has not asked the texture cache for yet: its pixels'
	place worked out as the cache would) */
	if (!(bitmap->flags & BITMAP_FLAG_CACHED))
		texture_cache_bitmap_new(bitmap_tag_index, bitmap);
	return bitmap;
}

/* the slots last found, by bitmap and flags (the HUD asks for its bitmaps'
textures every frame, and a search goes through every slot): each checked
by the slot's own fields, so one emptied and taken again is not trusted */
#define SLOT_MEMO 256

static struct texture_slot *slot_memo[SLOT_MEMO];

static struct texture_slot *texture_slot_find(struct bitmap_data *bitmap, int flags)
{
	unsigned long key = (((unsigned long)bitmap >> 4) ^ ((unsigned long)flags * 0x9E37UL)) & (SLOT_MEMO - 1);
	struct texture_slot *memo = slot_memo[key];

	if (memo && memo < texture_slots + texture_slot_count && memo->bitmap == bitmap && memo->flags == flags)
		return memo;
	for (int i = 0; i < texture_slot_count; i++)
	{
		if (texture_slots[i].bitmap == bitmap && texture_slots[i].flags == flags)
		{
			slot_memo[key] = &texture_slots[i];
			return &texture_slots[i];
		}
	}
	return NULL;
}

static unsigned long blend_tints(unsigned long one, unsigned long zero, unsigned long alpha)
{
	unsigned long r = (((one >> 16) & 0xFF) * alpha + ((zero >> 16) & 0xFF) * (255 - alpha)) / 255;
	unsigned long g = (((one >> 8) & 0xFF) * alpha + ((zero >> 8) & 0xFF) * (255 - alpha)) / 255;
	unsigned long b = ((one & 0xFF) * alpha + (zero & 0xFF) * (255 - alpha)) / 255;

	return (r << 16) | (g << 8) | b;
}

/* a texture from a bitmap, its alpha perhaps another's, its colour times a
tint (0xRRGGBB, 0x80 as it is) */
static r_texture *texture_from(struct bitmap_data *bitmap, struct bitmap_data *alpha_bitmap, unsigned long tint,
	int flags);

/* an environment's blended base map: each texel times its detail, the
primary's tint where its alpha is 1, the secondary's where it is 0 (Halo's
blended environment shaders); then opaque */
static r_texture *texture_blended(struct bitmap_data *bitmap, unsigned long primary_tint, unsigned long secondary_tint)
{
	int width, height;
	unsigned long *texels = dos_bitmap_decode(bitmap, texture_size(), &width, &height);
	r_texture *texture;

	if (!texels)
		return NULL;
	for (long i = 0; i < (long)width * height; i++)
	{
		unsigned long c = texels[i];
		unsigned long tint = blend_tints(primary_tint, secondary_tint, c >> 24);
		unsigned long tr = (tint >> 16) & 0xFF, tg = (tint >> 8) & 0xFF, tb = tint & 0xFF;
		unsigned long r = (((c >> 16) & 0xFF) * tr) >> 7, g = (((c >> 8) & 0xFF) * tg) >> 7, b = ((c & 0xFF) * tb) >> 7;

		texels[i] = 0xFF000000 | ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8) | (b > 255 ? 255 : b);
	}
	texture = r_texture_new(texels, width, height, 0);
	raw_free(texels);
	return texture;
}

static r_texture *texture_from(struct bitmap_data *bitmap, struct bitmap_data *alpha_bitmap, unsigned long tint,
	int flags)
{
	int width, height;
	unsigned long *texels = dos_bitmap_decode(bitmap, texture_size(), &width, &height);
	r_texture *texture;

	if (!texels)
		return NULL;
	if (alpha_bitmap)
	{
		int alpha_width, alpha_height;
		unsigned long *alpha = dos_bitmap_decode(alpha_bitmap, texture_size(), &alpha_width, &alpha_height);

		if (alpha)
		{
			for (int y = 0; y < height; y++)
			{
				for (int x = 0; x < width; x++)
				{
					unsigned long a = alpha[(y * alpha_height / height) * alpha_width + x * alpha_width / width];

					texels[y * width + x] = (texels[y * width + x] & 0x00FFFFFF) | (a & 0xFF000000);
				}
			}
			raw_free(alpha);
		}
	}
	if (tint != 0x808080)
	{
		unsigned long tr = (tint >> 16) & 0xFF, tg = (tint >> 8) & 0xFF, tb = tint & 0xFF;

		for (long i = 0; i < (long)width * height; i++)
		{
			unsigned long c = texels[i];
			unsigned long r = (((c >> 16) & 0xFF) * tr) >> 7, g = (((c >> 8) & 0xFF) * tg) >> 7, b = ((c & 0xFF) * tb) >> 7;

			texels[i] = (c & 0xFF000000) | ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8) | (b > 255 ? 255 : b);
		}
	}
	texture = r_texture_new(texels, width, height, flags);
	raw_free(texels);
	return texture;
}

r_texture *dos_bitmap_texture(long bitmap_tag_index, short bitmap_index, int texture_flags)
{
	struct bitmap_data *bitmap = bitmap_of(bitmap_tag_index, bitmap_index);
	struct texture_slot *slot;

	if (!bitmap)
		return NULL;
	slot = texture_slot_find(bitmap, texture_flags);
	if (slot)
		return slot->texture;
	if (texture_slot_count == TEXTURE_SLOTS)
		return NULL;
	slot = &texture_slots[texture_slot_count++];
	slot->bitmap = bitmap;
	slot->flags = texture_flags;
	slot->texture = texture_from(bitmap, NULL, 0x808080, texture_flags);
	slot->failed = slot->texture == NULL;
	return slot->texture;
}

/* a bitmap tag's bitmaps, for the pictures the menus keep (dos_menu.c):
how many it has, and one decoded (0xAARRGGBB, at most largest texels a
side; the caller frees them) */
int dos_bitmap_tag_count(long bitmap_tag_index)
{
	struct bitmap_group *group = bitmap_tag_index != NONE ? bitmap_group_get(bitmap_tag_index) : NULL;

	return group ? (int)group->bitmaps.count : 0;
}

unsigned long *dos_bitmap_tag_decode(long bitmap_tag_index, short bitmap_index, int largest, int *width, int *height)
{
	struct bitmap_data *bitmap = bitmap_of(bitmap_tag_index, bitmap_index);

	return bitmap ? dos_bitmap_decode(bitmap, largest, width, height) : NULL;
}

/* a detail map for the detail pass (dos_level.c): each channel times 0x80
over its average, so that the pass, which doubles it, leaves what is drawn
as it was on average (the material's texture has the details' average
colours already) and adds only their grain; cached as a slot of its own */
#define DETAIL_SLOT 0x10000

static r_texture *detail_texture(long bitmap_tag_index)
{
	struct bitmap_data *bitmap = bitmap_of(bitmap_tag_index, 0);
	struct texture_slot *slot;
	unsigned long *texels, sum[3] = { 0, 0, 0 }, average[3];
	int width, height;
	long count;

	if (!bitmap)
		return NULL;
	slot = texture_slot_find(bitmap, DETAIL_SLOT);
	if (slot)
		return slot->texture;
	if (texture_slot_count == TEXTURE_SLOTS)
		return NULL;
	slot = &texture_slots[texture_slot_count++];
	slot->bitmap = bitmap;
	slot->flags = DETAIL_SLOT;
	slot->texture = NULL;
	texels = dos_bitmap_decode(bitmap, texture_size(), &width, &height);
	if (texels)
	{
		count = (long)width * height;
		for (long i = 0; i < count; i++)
		{
			sum[0] += (texels[i] >> 16) & 0xFF;
			sum[1] += (texels[i] >> 8) & 0xFF;
			sum[2] += texels[i] & 0xFF;
		}
		for (int c = 0; c < 3; c++)
			average[c] = MAX(sum[c] / (unsigned long)count, 1);
		for (long i = 0; i < count; i++)
		{
			unsigned long c = texels[i], out = 0xFF000000;

			for (int k = 0; k < 3; k++)
			{
				unsigned long value = ((c >> (16 - 8 * k)) & 0xFF) * 128 / average[k];

				out |= (value > 255 ? 255 : value) << (16 - 8 * k);
			}
			texels[i] = out;
		}
		slot->texture = r_texture_new(texels, width, height, 0);
		raw_free(texels);
	}
	slot->failed = slot->texture == NULL;
	return slot->texture;
}

int dos_detail_maps(void)
{
	extern struct r_settings dos_render_settings;

	return dos_render_settings.texture_size >= 256 && r_draws_detail() && dos_setting_integer("detail_maps", 1);
}

/* a particle's sprite sheet (dos_particles.c), its alpha kept. multiply:
for the darkening shaders, which the renderer draws over by alpha: each
texel's alpha how dark it is (white leaves what is under as it was). tint
(0x00RRGGBB, 0x808080 none): its colour times the tint over 0x80, for the
256-colour picture, whose light has no colour (the particles' tints are
rounded to two bits a channel, so a sheet has a few copies at most) */
r_texture *dos_particle_texture(long bitmap_tag_index, short bitmap_index, int multiply, unsigned long tint)
{
	struct bitmap_data *bitmap = bitmap_of(bitmap_tag_index, bitmap_index);
	/* (each channel rounded to the nearest of 0, 0x40, 0x80 and 0xC0) */
#define TINT_LEVEL(channel) ((channel) >= 0xA0 ? 3UL : ((channel) + 0x20) >> 6)
	unsigned long tint_key = tint == 0x808080 ? 0 :
		0x2000000 | (TINT_LEVEL((tint >> 16) & 0xFF) << 8) | (TINT_LEVEL((tint >> 8) & 0xFF) << 6) |
		(TINT_LEVEL(tint & 0xFF) << 4);
#undef TINT_LEVEL
	int key = R_TEXTURE_ALPHA | R_TEXTURE_CLAMP | (multiply ? 0x1000000 : 0) | (int)tint_key;
	struct texture_slot *slot;
	unsigned long *texels;
	int width, height;

	if (!bitmap)
		return NULL;
	slot = texture_slot_find(bitmap, key);
	if (slot)
		return slot->texture;
	if (texture_slot_count == TEXTURE_SLOTS)
		return NULL;
	slot = &texture_slots[texture_slot_count++];
	slot->bitmap = bitmap;
	slot->flags = key;
	slot->texture = NULL;
	texels = dos_bitmap_decode(bitmap, texture_size(), &width, &height);
	if (texels)
	{
		if (multiply)
		{
			for (long i = 0; i < (long)width * height; i++)
			{
				unsigned long c = texels[i], r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
				unsigned long light = r > g ? (r > b ? r : b) : (g > b ? g : b);
				unsigned long alpha = (255 - light) * (c >> 24) / 255;

				texels[i] = (alpha << 24) | (c & 0x00FFFFFF);
			}
		}
		if (tint_key)
		{
			/* (the rounded tint's channels over 0x80, as the vertices' colours
			are: 0, a half, the texel as it is, one and a half; they were
			taken as thirds of 0xFF, and a tint of 1, 0x80, drew two thirds
			as bright as the 32-bit picture's) */
			unsigned long tr = ((tint_key >> 8) & 3) * 0x40, tg = ((tint_key >> 6) & 3) * 0x40;
			unsigned long tb = ((tint_key >> 4) & 3) * 0x40;

			for (long i = 0; i < (long)width * height; i++)
			{
				unsigned long c = texels[i];
				unsigned long r = (((c >> 16) & 0xFF) * tr) >> 7, g = (((c >> 8) & 0xFF) * tg) >> 7, b = ((c & 0xFF) * tb) >> 7;

				texels[i] = (c & 0xFF000000) | ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8) | (b > 255 ? 255 : b);
			}
		}
		slot->texture = r_texture_new(texels, width, height, R_TEXTURE_ALPHA | R_TEXTURE_CLAMP);
		raw_free(texels);
	}
	slot->failed = slot->texture == NULL;
	return slot->texture;
}

/* a cube map's face (0 +x, 1 -x, 2 +y, 3 -y, 4 +z, 5 -z, Direct3D's
order), for a sky's skybox (cached, as a slot of its own) */
static r_texture *cube_face_texture(long bitmap_tag_index, int face)
{
	struct bitmap_data *bitmap = bitmap_of(bitmap_tag_index, 0);
	int key = 0x10000 << face;
	struct texture_slot *slot;
	unsigned long *texels;
	int width, height;

	if (!bitmap)
		return NULL;
	slot = texture_slot_find(bitmap, key);
	if (slot)
		return slot->texture;
	if (texture_slot_count == TEXTURE_SLOTS)
		return NULL;
	slot = &texture_slots[texture_slot_count++];
	slot->bitmap = bitmap;
	slot->flags = key;
	slot->texture = NULL;
	texels = decode(bitmap, face, texture_size(), &width, &height);
	if (texels)
	{
		/* (opaque: the backdrop) */
		for (long i = 0; i < (long)width * height; i++)
			texels[i] |= 0xFF000000;
		slot->texture = r_texture_new(texels, width, height, R_TEXTURE_CLAMP);
		raw_free(texels);
	}
	slot->failed = slot->texture == NULL;
	return slot->texture;
}

/* ---------- the pixels the lighting reads (the Xbox's look: Direct3D 8,
dos_object_light): the bitmap as the map has it (Halo's own reader,
bitmaps.c bitmap_2d_get_pixel, takes its swizzled and compressed texels)
at its base_address, as the Xbox's texture cache keeps it; the least used
let go past a budget, and every one with the map */

#define LIGHTING_PIXEL_SLOTS 128
#define LIGHTING_PIXEL_BUDGET (24UL << 20)

static struct
{
	struct bitmap_data *bitmap;
	void *pixels;
	unsigned long size, used;
} lighting_pixels[LIGHTING_PIXEL_SLOTS];
static int lighting_pixel_count;
static unsigned long lighting_pixel_bytes, lighting_pixel_clock;

static void lighting_pixels_forget(int slot, int clear_bitmap)
{
	if (clear_bitmap)
		lighting_pixels[slot].bitmap->base_address = NULL;
	lighting_pixel_bytes -= lighting_pixels[slot].size;
	raw_free(lighting_pixels[slot].pixels);
	lighting_pixels[slot] = lighting_pixels[--lighting_pixel_count];
}

static void *lighting_pixels_get(struct bitmap_data *bitmap)
{
	unsigned long size = (unsigned long)bitmap->pixels_size;
	void *pixels;

	lighting_pixel_clock++;
	for (int i = 0; i < lighting_pixel_count; i++)
	{
		if (lighting_pixels[i].bitmap == bitmap)
		{
			lighting_pixels[i].used = lighting_pixel_clock;
			return lighting_pixels[i].pixels;
		}
	}
	if (!(bitmap->flags & BITMAP_FLAG_CACHED) || bitmap->pixels_size <= 0)
		return NULL;
	/* (room: the least used ones go) */
	while (lighting_pixel_count && (lighting_pixel_count == LIGHTING_PIXEL_SLOTS ||
		lighting_pixel_bytes + size > LIGHTING_PIXEL_BUDGET))
	{
		int oldest = 0;

		for (int i = 1; i < lighting_pixel_count; i++)
		{
			if (lighting_pixels[i].used < lighting_pixels[oldest].used)
				oldest = i;
		}
		lighting_pixels_forget(oldest, TRUE);
	}
	pixels = raw_malloc(size);
	if (!pixels)
		return NULL;
	if (!bitmap_read(bitmap, 0, pixels, size))
	{
		raw_free(pixels);
		return NULL;
	}
	bitmap->base_address = pixels;
	lighting_pixels[lighting_pixel_count].bitmap = bitmap;
	lighting_pixels[lighting_pixel_count].pixels = pixels;
	lighting_pixels[lighting_pixel_count].size = size;
	lighting_pixels[lighting_pixel_count].used = lighting_pixel_clock;
	lighting_pixel_count++;
	lighting_pixel_bytes += size;
	return pixels;
}

/* The texture cache's answer, which the game reads two ways: the HUD's,
the menus' and the precache's calls (load, not block) want a texture to
draw, with its alpha; the lighting's (object_lights.c: block, or neither,
"are its pixels in memory?") read the pixels of the lightmap and the base
map under a point, at their full size. Those get none but on the Xbox's
look (Direct3D 8, dos_object_light: the objects lit as Halo lights them):
elsewhere the objects take a light of their own (dos_render_game.c), the
units' illumination is for the AI, left out, and the particles' light for
particles this renderer does not draw. */
void *_texture_cache_bitmap_get_hardware_format(
	struct bitmap_data *bitmap,
	boolean block,
	boolean load)
{
	struct texture_slot *slot;

	if (bitmap && block && load && dos_object_light())
		return lighting_pixels_get(bitmap);
	if (!bitmap || block || !load)
		return NULL;
	slot = texture_slot_find(bitmap, R_TEXTURE_ALPHA);
	if (slot)
		return slot->texture;
	if (texture_slot_count == TEXTURE_SLOTS)
		return NULL;
	slot = &texture_slots[texture_slot_count++];
	slot->bitmap = bitmap;
	slot->flags = R_TEXTURE_ALPHA;
	interface_texture = TRUE;
	slot->texture = texture_from(bitmap, NULL, 0x808080, R_TEXTURE_ALPHA | R_TEXTURE_LARGE);
	interface_texture = FALSE;
	slot->failed = slot->texture == NULL;
	return slot->texture;
}

/* the menu's (dos_menu.c): a bitmap tag's first picture, its left
crop_u and top crop_v of it, averaged down (or spread up) to width x height
argb texels (malloc'd; the colours weighed by their alpha): NULL if the tag
has none */
unsigned long *dos_interface_picture(long bitmap_tag_index, float crop_u, float crop_v, int width, int height)
{
	struct bitmap_data *bitmap = bitmap_of(bitmap_tag_index, 0);
	unsigned long *texels, *out;
	int source_width, source_height, crop_width, crop_height;

	if (!bitmap || width <= 0 || height <= 0)
		return NULL;
	texels = dos_bitmap_decode(bitmap, 1024, &source_width, &source_height);
	if (!texels)
		return NULL;
	out = raw_malloc((size_t)width * height * sizeof(unsigned long));
	crop_width = (int)((float)source_width * crop_u);
	crop_height = (int)((float)source_height * crop_v);
	for (int y = 0; out && y < height; y++)
	{
		int y0 = y * crop_height / height, y1 = (y + 1) * crop_height / height;

		if (y1 <= y0)
			y1 = y0 + 1;
		for (int x = 0; x < width; x++)
		{
			int x0 = x * crop_width / width, x1 = (x + 1) * crop_width / width;
			unsigned long sum[4] = { 0, 0, 0, 0 }, count = 0;

			if (x1 <= x0)
				x1 = x0 + 1;
			for (int sy = y0; sy < y1; sy++)
			{
				for (int sx = x0; sx < x1; sx++)
				{
					unsigned long t = texels[sy * source_width + sx], a = t >> 24;

					sum[0] += ((t >> 16) & 0xFF) * a;
					sum[1] += ((t >> 8) & 0xFF) * a;
					sum[2] += (t & 0xFF) * a;
					sum[3] += a;
					count++;
				}
			}
			out[y * width + x] = !sum[3] ? 0 : ((sum[3] / count) << 24) | ((sum[0] / sum[3]) << 16) |
				((sum[1] / sum[3]) << 8) | (sum[2] / sum[3]);
		}
	}
	raw_free(texels);
	return out;
}

static long reference_index(const byte *definition, long offset);
static float read_real(const byte *definition, long offset);

/* a cube map's average colour over its top and sides (what a surface
facing up mirrors), from small copies of the faces; 0 if it has none */
static unsigned long cube_average(long bitmap_tag_index)
{
	struct bitmap_data *bitmap = bitmap_of(bitmap_tag_index, 0);
	unsigned long sum[3] = { 0, 0, 0 }, count = 0;

	if (!bitmap)
		return 0;
	for (int face = 0; face < 5; face++)
	{
		int width, height;
		unsigned long *texels = decode(bitmap, face, 16, &width, &height);

		if (!texels)
			continue;
		for (long i = 0; i < (long)width * height; i++)
		{
			sum[0] += (texels[i] >> 16) & 0xFF;
			sum[1] += (texels[i] >> 8) & 0xFF;
			sum[2] += texels[i] & 0xFF;
		}
		count += (unsigned long)width * height;
		raw_free(texels);
	}
	if (!count)
		return 0;
	return ((sum[0] / count) << 16) | ((sum[1] / count) << 8) | (sum[2] / count);
}

/* water's colour, as its base map's tint (0x80 a channel: as it is):
Halo's water is its reflection map (the sky it mirrors) under the shader's
tints, the view's perpendicular one looking down and its parallel one at a
glance, how water is mostly seen; worked out once, at the material's
making (the 0x6080A0 of before is the colour a shader without a reflection
map keeps). It is added to what is below, as Halo adds the reflection
(rasterizer_xbox_water.c: its other pass, the base map times what is
below, is near white on every map and left out) */
static unsigned long water_tint(const byte *shader, const r_texture *base)
{
	unsigned long reflected = cube_average(reference_index(shader, 0x9C));
	unsigned long base_average = base ? r_texture_average(base) : 0xFFFFFF;
	unsigned long tint = 0;

	if (!reflected)
		return 0x6080A0;
	for (int k = 0; k < 3; k++)
	{
		int shift = 16 - 8 * k;
		float perpendicular = read_real(shader, 0x70 + 4 * k), parallel = read_real(shader, 0x80 + 4 * k);
		float color = (float)((reflected >> shift) & 0xFF) * (0.25f * perpendicular + 0.75f * parallel);
		unsigned long below = (base_average >> shift) & 0xFF;
		long channel = (long)(color * 128.0f / (float)(below < 32 ? 32 : below));

		tint |= (unsigned long)(channel < 0 ? 0 : channel > 255 ? 255 : channel) << shift;
	}
	return tint;
}

/* ---------- materials */

#define MATERIAL_SLOTS 768

static struct material_slot
{
	long shader_tag_index;
	struct dos_material material;
} material_slots[MATERIAL_SLOTS];
static int material_slot_count;

static long reference_index(const byte *definition, long offset)
{
	return ((const struct tag_reference *)(definition + offset))->index;
}

static short read_short(const byte *definition, long offset)
{
	return *(const short *)(definition + offset);
}

static float read_real(const byte *definition, long offset)
{
	return *(const float *)(definition + offset);
}

/* Halo's detail functions (shader_environment's detail and micro detail) */
enum
{
	_detail_double_biased_multiply,
	_detail_multiply,
	_detail_double_biased_add,
};

/* a detail map's average colour as a tint (0x80 as it is): Halo's detail
functions take 2 x detail (the average itself is the tint) or the detail
(half of it); the add's base + 2 x detail - 1 is near a multiply where the
base is middling. Its average alpha too, 0 to 255. */
static unsigned long detail_tint(long bitmap_tag_index, short function, unsigned long *alpha)
{
	struct bitmap_data *bitmap = bitmap_of(bitmap_tag_index, 0);
	int width, height;
	unsigned long *texels;
	unsigned long sum[4] = { 0, 0, 0, 0 }, count, r, g, b;

	if (alpha)
		*alpha = 255;
	if (!bitmap)
		return 0x808080;
	texels = dos_bitmap_decode(bitmap, 16, &width, &height);
	if (!texels)
		return 0x808080;
	count = (unsigned long)width * height;
	for (unsigned long i = 0; i < count; i++)
	{
		sum[0] += (texels[i] >> 16) & 0xFF;
		sum[1] += (texels[i] >> 8) & 0xFF;
		sum[2] += texels[i] & 0xFF;
		sum[3] += texels[i] >> 24;
	}
	raw_free(texels);
	r = sum[0] / count;
	g = sum[1] / count;
	b = sum[2] / count;
	if (alpha)
		*alpha = sum[3] / count;
	if (function == _detail_multiply)
	{
		r >>= 1;
		g >>= 1;
		b >>= 1;
	}
	return (r << 16) | (g << 8) | b;
}

static unsigned long multiply_tints(unsigned long a, unsigned long b)
{
	unsigned long r = (((a >> 16) & 0xFF) * ((b >> 16) & 0xFF)) >> 7;
	unsigned long g = (((a >> 8) & 0xFF) * ((b >> 8) & 0xFF)) >> 7;
	unsigned long c = ((a & 0xFF) * (b & 0xFF)) >> 7;

	return ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8) | (c > 255 ? 255 : c);
}

/* the framebuffer blend functions of Halo's transparent shaders */
enum
{
	_blend_alpha, _blend_multiply, _blend_double_multiply, _blend_add, _blend_subtract, _blend_minimum,
	_blend_maximum, _blend_alpha_multiply_add,
};

static void transparent_material(struct dos_material *material, long bitmap_tag_index, short blend, int alpha_tested)
{
	material->transparent = TRUE;
	switch (blend)
	{
	case _blend_add:
	case _blend_alpha_multiply_add:
	case _blend_maximum:
		material->flags |= R_DRAW_ADDITIVE;
		break;
	case _blend_multiply:
	case _blend_double_multiply:
	case _blend_minimum:
	case _blend_subtract:
		/* (darkening: shadows and decals this renderer leaves out) */
		material->skip = TRUE;
		return;
	default:
		if (alpha_tested)
		{
			material->transparent = FALSE;
		}
		else
		{
			/* (by the texels' alpha: a soft-edged layer's quad not drawn
			whole) */
			material->flags |= R_DRAW_BLEND | R_DRAW_ALPHA;
		}
		break;
	}
	material->texture = dos_bitmap_texture(bitmap_tag_index, 0, alpha_tested ? R_TEXTURE_ALPHA_TEST :
		(material->flags & R_DRAW_ALPHA) ? R_TEXTURE_ALPHA : 0);
	material->flags |= R_DRAW_TWO_SIDED;
}

/* ---------- chicago shaders' maps

A transparent chicago shader stacks up to four maps, each tiled at its own
scale and offset, each map's colour and alpha functions saying how what it
has made so far goes on with the next map (a sky: twinkles times stars,
plus the blue; clouds: a mask times the clouds). This renderer draws one
texture: the stack, made once as the shader loads, at the texture size,
over the least-tiled map's repeat (the material's scale). */

enum
{
	_function_current,
	_function_next_map,
	_function_multiply,
	_function_double_multiply,
	_function_add,
	_function_add_signed_current,
	_function_add_signed_next_map,
	_function_subtract_current,
	_function_subtract_next_map,
	_function_blend_current_alpha,
	_function_blend_current_alpha_inverse,
	_function_blend_next_map_alpha,
	_function_blend_next_map_alpha_inverse,
};

#define CHICAGO_MAP_SIZE 0xDC
#define MAXIMUM_CHICAGO_MAPS 4

static float combine(int function, float current, float next, float current_alpha, float next_alpha)
{
	float value;

	switch (function)
	{
	case _function_next_map: value = next; break;
	case _function_multiply: value = current * next; break;
	case _function_double_multiply: value = 2.0f * current * next; break;
	case _function_add: value = current + next; break;
	case _function_add_signed_current:
	case _function_add_signed_next_map: value = current + next - 0.5f; break;
	case _function_subtract_current: value = current - next; break;
	case _function_subtract_next_map: value = next - current; break;
	case _function_blend_current_alpha: value = current * current_alpha + next * (1.0f - current_alpha); break;
	case _function_blend_current_alpha_inverse: value = next * current_alpha + current * (1.0f - current_alpha); break;
	case _function_blend_next_map_alpha: value = next * next_alpha + current * (1.0f - next_alpha); break;
	case _function_blend_next_map_alpha_inverse: value = current * next_alpha + next * (1.0f - next_alpha); break;
	default: value = current; break;
	}
	return value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
}

/* A chicago shader's stack on the card (Direct3D 8, dos_xbox_detail): each
map a stage of its own at its own tiling and offset over the least-tiled
map's (the material's scale), each map's colour and alpha functions the
next stage's operations -- as Halo's PC renderer drew these shaders with
fixed-function stages, nothing baked (a space sky's stars stay stars, a
cloud mask stays soft). Drawn instead of the stack's texture
(r_detail_pass's replace); FALSE where the card has too few stages. */
static unsigned long hardware_texture(long bitmap_tag_index, int alpha);
static unsigned long white_texture(void);

static int chicago_stage_operation(short function)
{
	switch (function)
	{
	case _function_next_map: return _r_stage_texture;
	case _function_multiply: return _r_stage_modulate;
	case _function_double_multiply: return _r_stage_modulate_twice;
	case _function_add: return _r_stage_add;
	case _function_add_signed_current:
	case _function_add_signed_next_map: return _r_stage_add_signed;
	case _function_subtract_current: return _r_stage_subtract;
	case _function_subtract_next_map: return _r_stage_subtract_reverse;
	case _function_blend_current_alpha: return _r_stage_blend_current_alpha;
	case _function_blend_current_alpha_inverse: return _r_stage_blend_current_alpha_inverse;
	case _function_blend_next_map_alpha: return _r_stage_blend_texture_alpha;
	case _function_blend_next_map_alpha_inverse: return _r_stage_blend_texture_alpha_inverse;
	default: return _r_stage_current;
	}
}

/* a constant colour (0xAARRGGBB) as a 1x1 texture on the card, for a
generic shader's stage constants (the card's texture factor came out black
on a driver: textures are safe) */
#define CONSTANT_TEXTURES 64

static struct
{
	unsigned long argb;
	unsigned long hardware;
} constant_textures[CONSTANT_TEXTURES];
static int constant_texture_count;

static unsigned long constant_texture(unsigned long argb)
{
	unsigned long texel = argb, *levels[1] = { &texel };
	int one = 1;

	for (int i = 0; i < constant_texture_count; i++)
	{
		if (constant_textures[i].argb == argb)
			return constant_textures[i].hardware;
	}
	if (constant_texture_count == CONSTANT_TEXTURES)
		return 0;
	constant_textures[constant_texture_count].argb = argb;
	constant_textures[constant_texture_count].hardware = r_hardware_texture_new(levels, &one, &one, 1, TRUE);
	return constant_textures[constant_texture_count++].hardware;
}

/* A transparent generic shader (sotr) on the card (Direct3D 8,
dos_xbox_detail). Its stages are the Xbox's register combiners
(shader_transparent_generic_preprocessor.c): AB = A*B and CD = C*D into
registers, for the colour and the alpha. Those that are only products into
the first scratch register (R0 = X*Y, X and Y a map, R0 itself, a stage
constant or one; the sky's planets, rings and haze) become fixed-function
stages: R0 kept as what is so far, a fresh product starting over, R0*R0 a
square; R0's alpha starts as the first map's, as on the NV2A; the final
colour and alpha R0's. Others (vertex colours, dot products, sums, signed
mappings, fades by the view's angle) keep the first map alone. */
#define GENERIC_MAP_SIZE 0x64
#define GENERIC_STAGE_SIZE 0x70
#define GENERIC_STEPS 8

enum
{
	_generic_zero = 0,
	_generic_one = 1,
	_generic_map_0 = 5,
	_generic_scratch_0 = 11,
	_generic_constant_0 = 13,
	_generic_constant_1 = 14
};

struct generic_step
{
	/* the texture it reads, 0 none (a square, or what is so far) */
	unsigned long texture;
	unsigned char operation;
	short map;
};

/* a product's input as a step's texture: a map, a stage constant, one; 0
for R0 (the caller's), -1 if it is none of these */
static long generic_input(int input, int alpha, const unsigned long *map_textures, int map_count, const byte *stage,
	unsigned long *texture, short *map)
{
	*map = -1;
	if (input == _generic_scratch_0)
	{
		*texture = 0;
		return 0;
	}
	if (input == _generic_one)
	{
		*texture = constant_texture(0xFFFFFFFFUL);
		return 1;
	}
	if (input >= _generic_map_0 && input < _generic_map_0 + map_count)
	{
		*map = (short)(input - _generic_map_0);
		*texture = map_textures[input - _generic_map_0];
		return 1;
	}
	if (input == _generic_constant_0 || input == _generic_constant_1)
	{
		/* (a stage's constant colours: colour 0 at 0x0C, colour 1 at 0x2C,
		alpha red green blue) */
		long at = input == _generic_constant_0 ? 0x0C : 0x2C;
		unsigned long argb = 0;

		for (int k = 0; k < 4; k++)
		{
			float value = read_real(stage, at + 4 * k);

			argb = (argb << 8) | (unsigned long)(PIN(value, 0.0f, 1.0f) * 255.0f + 0.5f);
		}
		*texture = constant_texture(argb);
		return 1;
	}
	(void)alpha;
	return -1;
}

/* one channel's stages (colour or alpha: their fields from offset at),
appended to steps; FALSE where it is not a product into R0 */
static int generic_channel(const byte *stage, int alpha, const unsigned long *map_textures, int map_count,
	struct generic_step *steps, int *count)
{
	/* (colour: A, A mapping, B, B mapping, C, C mapping, D, D mapping, AB
	out, AB function, CD out, CD function, mux sum out, out mapping; alpha:
	A..D with mappings, then AB, CD and sum outs, out mapping) */
	long at = alpha ? 0x58 : 0x3C;
	short a = read_short(stage, at), a_mapping = read_short(stage, at + 2);
	short b = read_short(stage, at + 4), b_mapping = read_short(stage, at + 6);
	short out_ab = read_short(stage, at + 16);
	short out_cd = alpha ? read_short(stage, at + 18) : read_short(stage, at + 20);
	short out_sum = alpha ? read_short(stage, at + 20) : read_short(stage, at + 24);
	short out_mapping = alpha ? read_short(stage, at + 22) : read_short(stage, at + 26);
	short ab_function = alpha ? 0 : read_short(stage, at + 18);
	unsigned long texture_a, texture_b;
	short map_a, map_b;
	long kind_a, kind_b;

	/* (mux flags: bit 0 the colour's, 1 the alpha's) */
	if (read_short(stage, 0) & (alpha ? 2 : 1))
		return FALSE;
	if (out_cd != 0 || out_sum != 0)
		return FALSE;
	if (out_ab == 0)
		return TRUE;
	if (out_ab != 1 || out_mapping != 0 || ab_function != 0 || a_mapping != 0 || b_mapping != 0)
		return FALSE;
	kind_a = generic_input(a, alpha, map_textures, map_count, stage, &texture_a, &map_a);
	kind_b = generic_input(b, alpha, map_textures, map_count, stage, &texture_b, &map_b);
	if (kind_a < 0 || kind_b < 0 || (kind_a && !texture_a) || (kind_b && !texture_b))
		return FALSE;
	if (!kind_a && !kind_b)
	{
		if (*count >= GENERIC_STEPS)
			return FALSE;
		steps[(*count)++] = (struct generic_step){ 0, _r_stage_square, -1 };
		return TRUE;
	}
	if (!kind_a || !kind_b)
	{
		if (*count >= GENERIC_STEPS)
			return FALSE;
		steps[(*count)++] = (struct generic_step){ kind_a ? texture_a : texture_b, _r_stage_modulate,
			kind_a ? map_a : map_b };
		return TRUE;
	}
	/* (a fresh product: what came before is overwritten) */
	*count = 0;
	steps[(*count)++] = (struct generic_step){ texture_a, _r_stage_texture, map_a };
	steps[(*count)++] = (struct generic_step){ texture_b, _r_stage_modulate, map_b };
	return TRUE;
}

static int generic_card_pass(const byte *shader, struct r_detail_pass *pass)
{
	const struct tag_block *maps = (const struct tag_block *)(shader + 0x54);
	const struct tag_block *stages = (const struct tag_block *)(shader + 0x60);
	unsigned long map_textures[4];
	struct generic_step colour[GENERIC_STEPS], alpha[GENERIC_STEPS];
	int map_count = maps->count < 4 ? (int)maps->count : 4, colours = 0, alphas = 0, n = 0, c = 0, a = 0;
	int limit = r_draws_xbox_detail();

	memset(pass, 0, sizeof(*pass));
	/* (a 2D first map, no fade by the view's angle) */
	if (map_count < 1 || read_short(shader, 0x2A) != 0 || read_short(shader, 0x2E) != 0 || stages->count > 7)
		return FALSE;
	for (int m = 0; m < map_count; m++)
	{
		long bitmap = reference_index((const byte *)xbox_pointer(maps->address) + m * GENERIC_MAP_SIZE, 0x1C);

		map_textures[m] = bitmap != NONE ? hardware_texture(bitmap, TRUE) : 0;
		if (!map_textures[m])
			return FALSE;
	}
	/* (R0: the first map's alpha to start with, its colour too where no
	stage writes it -- the no-stage program, T0 itself) */
	colour[colours++] = (struct generic_step){ map_textures[0], _r_stage_texture, 0 };
	alpha[alphas++] = (struct generic_step){ map_textures[0], _r_stage_texture, 0 };
	for (long s = 0; s < stages->count; s++)
	{
		const byte *stage = (const byte *)xbox_pointer(stages->address) + s * GENERIC_STAGE_SIZE;

		if (!generic_channel(stage, FALSE, map_textures, map_count, colour, &colours) ||
			!generic_channel(stage, TRUE, map_textures, map_count, alpha, &alphas))
		{
			return FALSE;
		}
	}
	/* the two channels' steps into stages: one texture a stage, the other
	channel kept where they read different textures */
	while (c < colours || a < alphas)
	{
		const struct generic_step *cs = c < colours ? &colour[c] : NULL, *as = a < alphas ? &alpha[a] : NULL;
		int together = cs && as && (!cs->texture || !as->texture || cs->texture == as->texture) &&
			(cs->map == as->map || cs->map < 0 || as->map < 0);
		unsigned long texture;
		short map;

		if (n >= limit || n >= R_DETAIL_STAGES)
			return FALSE;
		if (together)
		{
			texture = cs->texture ? cs->texture : as->texture;
			map = cs->map >= 0 ? cs->map : as->map;
			pass->operations[n] = cs->operation;
			pass->alpha_operations[n] = as->operation;
			c++;
			a++;
		}
		else if (cs && (!as || c <= a))
		{
			texture = cs->texture;
			map = cs->map;
			pass->operations[n] = cs->operation;
			pass->alpha_operations[n] = n ? _r_stage_current : _r_stage_texture;
			c++;
		}
		else
		{
			texture = as->texture;
			map = as->map;
			pass->operations[n] = n ? _r_stage_current : _r_stage_texture;
			pass->alpha_operations[n] = as->operation;
			a++;
		}
		pass->textures[n] = texture ? texture : constant_texture(0xFFFFFFFFUL);
		/* (a map's own tiling; a constant's anything) */
		pass->scales[n][0] = pass->scales[n][1] = 1.0f;
		if (map >= 0)
		{
			const byte *m = (const byte *)xbox_pointer(maps->address) + map * GENERIC_MAP_SIZE;
			float su = read_real(m, 4), sv = read_real(m, 8);

			pass->scales[n][0] = su != 0.0f ? su : 1.0f;
			pass->scales[n][1] = sv != 0.0f ? sv : 1.0f;
			pass->offsets[n][0] = read_real(m, 12);
			pass->offsets[n][1] = read_real(m, 16);
		}
		if (!pass->textures[n])
			return FALSE;
		n++;
	}
	pass->stage_count = n;
	pass->replace = TRUE;
	return TRUE;
}

static int chicago_card_pass(const struct tag_block *maps, float least_u, float least_v, struct r_detail_pass *pass)
{
	int count = maps->count < MAXIMUM_CHICAGO_MAPS ? (int)maps->count : MAXIMUM_CHICAGO_MAPS;

	memset(pass, 0, sizeof(*pass));
	if (count < 2 || count > r_draws_xbox_detail() || least_u <= 0.0f || least_v <= 0.0f)
		return FALSE;
	for (int i = 0; i < count; i++)
	{
		const byte *map = (const byte *)xbox_pointer(maps->address) + i * CHICAGO_MAP_SIZE;
		long bitmap = reference_index(map, 0x6C);
		float su = read_real(map, 0x54), sv = read_real(map, 0x58);

		pass->textures[i] = bitmap != NONE ? hardware_texture(bitmap, TRUE) : white_texture();
		if (!pass->textures[i])
		{
			memset(pass, 0, sizeof(*pass));
			return FALSE;
		}
		pass->scales[i][0] = (su > 0.0f ? su : 1.0f) / least_u;
		pass->scales[i][1] = (sv > 0.0f ? sv : 1.0f) / least_v;
		pass->offsets[i][0] = read_real(map, 0x5C);
		pass->offsets[i][1] = read_real(map, 0x60);
		if (i == 0)
		{
			pass->operations[i] = _r_stage_texture;
			pass->alpha_operations[i] = _r_stage_texture;
		}
		else
		{
			/* (the map before says how what is so far goes on with this one) */
			const byte *before = (const byte *)xbox_pointer(maps->address) + (i - 1) * CHICAGO_MAP_SIZE;

			pass->operations[i] = (unsigned char)chicago_stage_operation(read_short(before, 0x2C));
			pass->alpha_operations[i] = (unsigned char)chicago_stage_operation(read_short(before, 0x2E));
		}
	}
	pass->stage_count = count;
	pass->replace = TRUE;
	return TRUE;
}

/* (an added shader's map multiplied in at four or more times the tiling of
the others, a space sky's stars: drawn as a layer of its own, the stack
black there, and its tint what it multiplied, on average) */
static r_texture *chicago_stack(const struct tag_block *maps, int texture_flags, float *u_scale, float *v_scale,
	struct dos_material *material)
{
	struct
	{
		unsigned long *texels;
		int width, height;
		float u_scale, v_scale, u_offset, v_offset;
		short color_function, alpha_function;
	} layers[MAXIMUM_CHICAGO_MAPS];
	int count = maps->count < MAXIMUM_CHICAGO_MAPS ? (int)maps->count : MAXIMUM_CHICAGO_MAPS;
	int size = texture_size();
	float least_u = 1e9f, least_v = 1e9f;
	unsigned long *out;
	r_texture *texture = NULL;
	int split = -1;
	struct bitmap_data *split_bitmap = NULL;
	float split_sum[3] = { 0.0f, 0.0f, 0.0f };

	for (int i = 0; i < count; i++)
	{
		const byte *map = (const byte *)xbox_pointer(maps->address) + i * CHICAGO_MAP_SIZE;
		struct bitmap_data *bitmap = bitmap_of(reference_index(map, 0x6C), 0);
		float su = read_real(map, 0x54), sv = read_real(map, 0x58);

		layers[i].texels = bitmap ? dos_bitmap_decode(bitmap, size, &layers[i].width, &layers[i].height) : NULL;
		layers[i].u_scale = su > 0.0f ? su : 1.0f;
		layers[i].v_scale = sv > 0.0f ? sv : 1.0f;
		layers[i].u_offset = read_real(map, 0x5C);
		layers[i].v_offset = read_real(map, 0x60);
		layers[i].color_function = read_short(map, 0x2C);
		layers[i].alpha_function = read_short(map, 0x2E);
		if (layers[i].texels)
		{
			if (layers[i].u_scale < least_u) least_u = layers[i].u_scale;
			if (layers[i].v_scale < least_v) least_v = layers[i].v_scale;
		}
	}
	if (material && (material->flags & R_DRAW_ADDITIVE))
	{
		for (int i = 1; i < count && split < 0; i++)
		{
			if (layers[i].texels && layers[i - 1].color_function == _function_multiply &&
				(layers[i].u_scale >= 4.0f * least_u || layers[i].v_scale >= 4.0f * least_v))
			{
				split = i;
				split_bitmap = bitmap_of(reference_index((const byte *)xbox_pointer(maps->address) + i * CHICAGO_MAP_SIZE, 0x6C), 0);
			}
		}
	}
	out = layers[0].texels ? raw_malloc((size_t)size * size * sizeof(unsigned long)) : NULL;
	if (out)
	{
		for (int y = 0; y < size; y++)
		{
			for (int x = 0; x < size; x++)
			{
				float u = ((float)x + 0.5f) / (float)size, v = ((float)y + 0.5f) / (float)size;
				float acc[4], next[4];

				for (int i = 0; i < count; i++)
				{
					float *c = i ? next : acc;

					if (i == split)
					{
						/* (the layer's: black here, what it multiplies kept) */
						for (int k = 0; k < 3; k++)
							split_sum[k] += acc[k];
						c[0] = c[1] = c[2] = 0.0f;
						c[3] = 1.0f;
					}
					else if (layers[i].texels)
					{
						float tu = u * layers[i].u_scale / least_u + layers[i].u_offset;
						float tv = v * layers[i].v_scale / least_v + layers[i].v_offset;
						int sx = (int)floorf((tu - floorf(tu)) * (float)layers[i].width);
						int sy = (int)floorf((tv - floorf(tv)) * (float)layers[i].height);
						unsigned long t;

						if (sx >= layers[i].width) sx = layers[i].width - 1;
						if (sy >= layers[i].height) sy = layers[i].height - 1;
						t = layers[i].texels[sy * layers[i].width + sx];
						c[0] = (float)((t >> 16) & 0xFF) / 255.0f;
						c[1] = (float)((t >> 8) & 0xFF) / 255.0f;
						c[2] = (float)(t & 0xFF) / 255.0f;
						c[3] = (float)(t >> 24) / 255.0f;
					}
					else
					{
						c[0] = c[1] = c[2] = c[3] = 1.0f;
					}
					if (i)
					{
						short cf = layers[i - 1].color_function, af = layers[i - 1].alpha_function;
						float alpha = acc[3];

						for (int k = 0; k < 3; k++)
							acc[k] = combine(cf, acc[k], next[k], alpha, next[3]);
						acc[3] = combine(af, alpha, next[3], alpha, next[3]);
					}
				}
				out[y * size + x] = ((unsigned long)(acc[3] * 255.0f + 0.5f) << 24) |
					((unsigned long)(acc[0] * 255.0f + 0.5f) << 16) | ((unsigned long)(acc[1] * 255.0f + 0.5f) << 8) |
					(unsigned long)(acc[2] * 255.0f + 0.5f);
			}
		}
		texture = r_texture_new(out, size, size, texture_flags);
		raw_free(out);
	}
	for (int i = 0; i < count; i++)
		raw_free(layers[i].texels);
	*u_scale = least_u < 1e8f ? least_u : 1.0f;
	*v_scale = least_v < 1e8f ? least_v : 1.0f;
	if (texture && split >= 0 && split_bitmap)
	{
		struct texture_slot *slot = texture_slot_find(split_bitmap, 0);
		unsigned long tint = 0;

		if (!slot && texture_slot_count < TEXTURE_SLOTS)
		{
			slot = &texture_slots[texture_slot_count++];
			slot->bitmap = split_bitmap;
			slot->flags = 0;
			slot->texture = texture_from(split_bitmap, NULL, 0x808080, 0);
			slot->failed = slot->texture == NULL;
		}
		for (int k = 0; k < 3; k++)
		{
			float value = split_sum[k] / (float)((long)size * size) * 128.0f;

			tint |= (unsigned long)(value > 255.0f ? 255.0f : value) << (16 - 8 * k);
		}
		material->layer = slot ? slot->texture : NULL;
		material->layer_scale[0] = layers[split].u_scale / *u_scale;
		material->layer_scale[1] = layers[split].v_scale / *v_scale;
		material->layer_tint = tint;
	}
	return texture;
}

/* ---------- the Xbox's texture pass (Direct3D 8)

An environment shader's surfaces drawn as Halo's own texture pass draws
them (rasterizer_xbox_environment.c): the base map as it is (times the
lightmap), then in one pass over it the primary and the secondary detail
map mixed at each pixel -- by the base map's alpha (blended shaders: the
primary where it is 1), or by the secondary's own (normal ones: the
primary where it is 1) -- times the micro detail map, the picture times
twice that (the double biased multiply; once for the plain multiply). The
textures are the card's, with the levels the map stores (Halo's tools
faded the detail maps' smaller levels toward grey, so their grain goes in
the distance as on the Xbox); nothing is baked or put in a palette. The
setting d3d_textures (SETTINGS > DETAIL MAPS) 0 bakes them as OpenGL
does. */

#define HARDWARE_SLOT 0x20000
#define HARDWARE_LEVELS 16

int dos_xbox_detail(void)
{
	return r_draws_xbox_detail() && dos_setting_integer("d3d_textures", 1);
}

/* whether every map's sky is its own sky model (Direct3D 8: the Xbox's),
not the stand-in gradient some maps have here (dos_level.c dos_skies) */
int dos_xbox_sky(void)
{
	return r_draws_lightmaps() && dos_setting_integer("d3d_sky", 1);
}

/* a bitmap on the card (its first), each level the map stores up to the
texture size, alpha kept or not; 0 if it cannot be */
static unsigned long hardware_texture(long bitmap_tag_index, int alpha)
{
	struct bitmap_data *bitmap = bitmap_of(bitmap_tag_index, 0);
	struct texture_slot *slot;
	unsigned long *levels[HARDWARE_LEVELS];
	int widths[HARDWARE_LEVELS], heights[HARDWARE_LEVELS], count = 0, largest = texture_size(), bytes, block;

	if (!bitmap)
		return 0;
	texel_kind(bitmap->format, &bytes, &block);
	slot = texture_slot_find(bitmap, HARDWARE_SLOT | alpha);
	if (slot)
		return slot->hardware;
	if (texture_slot_count == TEXTURE_SLOTS)
		return 0;
	slot = &texture_slots[texture_slot_count++];
	slot->bitmap = bitmap;
	slot->flags = HARDWARE_SLOT | alpha;
	slot->texture = NULL;
	slot->hardware = 0;
	/* (each level the map has, halving: decode stops at its last) */
	while (count < HARDWARE_LEVELS)
	{
		int width, height;
		unsigned long *texels = decode(bitmap, -1, largest, &width, &height);

		if (!texels)
			break;
		/* (none smaller in the map; nor, compressed, a level under a block:
		the maps hold zeros there, black that the far surfaces would take --
		the card averages those down from the last block-sized one) */
		if ((count && width >= widths[count - 1] && height >= heights[count - 1]) ||
			(count && block && (width < 4 || height < 4)))
		{
			raw_free(texels);
			break;
		}
		levels[count] = texels;
		widths[count] = width;
		heights[count] = height;
		count++;
		if (width <= 1 && height <= 1)
			break;
		largest = MAX(width, height) / 2;
	}
	slot->hardware = r_hardware_texture_new(levels, widths, heights, count, alpha);
	slot->failed = slot->hardware == 0;
	/* (testing, setting debug_shaders: each level's size and average) */
	if (dos_setting_integer("debug_shaders", 0))
	{
		char line[400];
		int used = snprintf(line, sizeof(line), "card texture %s:", tag_get_name(bitmap_tag_index));

		for (int i = 0; i < count && used < (int)sizeof(line) - 40; i++)
		{
			unsigned long sum[4] = { 0, 0, 0, 0 };
			long texels = (long)widths[i] * heights[i];

			for (long k = 0; k < texels; k++)
			{
				for (int c = 0; c < 4; c++)
					sum[c] += (levels[i][k] >> (24 - 8 * c)) & 0xFF;
			}
			used += snprintf(line + used, sizeof(line) - (size_t)used, " %dx%d %02lx%02lx%02lx%02lx", widths[i], heights[i],
				sum[0] / (unsigned long)texels, sum[1] / (unsigned long)texels, sum[2] / (unsigned long)texels,
				sum[3] / (unsigned long)texels);
		}
		system_log(line);
	}
	for (int i = 0; i < count; i++)
		raw_free(levels[i]);
	return slot->hardware;
}

/* what stands for a detail map a shader lacks (Halo's default: grey, which
changes nothing; its alpha 1) */
static unsigned long hardware_grey;

static unsigned long grey_texture(void)
{
	if (!hardware_grey)
	{
		unsigned long grey = 0xFF808080UL, *levels[1] = { &grey };
		int one = 1;

		hardware_grey = r_hardware_texture_new(levels, &one, &one, 1, TRUE);
	}
	return hardware_grey;
}

/* what stands for a chicago map without a bitmap (the stack takes it as 1) */
static unsigned long hardware_white;

static unsigned long white_texture(void)
{
	if (!hardware_white)
	{
		unsigned long white = 0xFFFFFFFFUL, *levels[1] = { &white };
		int one = 1;

		hardware_white = r_hardware_texture_new(levels, &one, &one, 1, TRUE);
	}
	return hardware_white;
}

static void detail_scale(const byte *shader, long bitmap_tag_index, float scale, struct bitmap_data *base, float *out);

/* an environment shader's texture pass (shader_environment: type 0x2A,
detail function 0xB0, primary scale 0xB4 and map 0xB8, secondary 0xC8 and
0xCC, micro function 0xF4, scale 0xF8, map 0xFC), its stages in pass; FALSE
if a texture cannot be made */
static int xbox_texture_pass(const byte *shader, struct bitmap_data *base_bitmap, struct r_detail_pass *pass)
{
	short type = read_short(shader, 0x2A), function = read_short(shader, 0xB0);
	long primary = reference_index(shader, 0xB8), secondary = reference_index(shader, 0xCC);
	long micro = reference_index(shader, 0xFC);
	int blended = type == 1 || type == 2, n = 0;
	unsigned long primary_texture = primary != NONE ? hardware_texture(primary, FALSE) : grey_texture();
	unsigned long secondary_texture = secondary != NONE ? hardware_texture(secondary, !blended) : grey_texture();
	float primary_scale = read_real(shader, 0xB4), secondary_scale = read_real(shader, 0xC8);

	memset(pass, 0, sizeof(*pass));
	if (blended)
	{
		/* (the base map's alpha first, then the secondary, then the primary
		where that alpha is 1) */
		pass->textures[n] = hardware_texture(reference_index(shader, 0x88), TRUE);
		pass->scales[n][0] = pass->scales[n][1] = 1.0f;
		pass->operations[n] = _r_stage_texture;
		pass->alpha_operations[n++] = _r_stage_texture;
		pass->textures[n] = secondary_texture;
		detail_scale(shader, secondary, secondary_scale != 0.0f ? secondary_scale : 1.0f, base_bitmap, pass->scales[n]);
		pass->operations[n] = _r_stage_texture;
		pass->alpha_operations[n++] = _r_stage_current;
	}
	else
	{
		/* (the secondary and its alpha, then the primary where it is 1) */
		pass->textures[n] = secondary_texture;
		detail_scale(shader, secondary, secondary_scale != 0.0f ? secondary_scale : 1.0f, base_bitmap, pass->scales[n]);
		pass->operations[n] = _r_stage_texture;
		pass->alpha_operations[n++] = _r_stage_texture;
	}
	pass->textures[n] = primary_texture;
	detail_scale(shader, primary, primary_scale != 0.0f ? primary_scale : 1.0f, base_bitmap, pass->scales[n]);
	pass->operations[n] = _r_stage_blend_current_alpha_inverse;
	pass->alpha_operations[n++] = _r_stage_current;
	/* (the micro detail where the card has the stage for it) */
	if (micro != NONE && n < r_draws_xbox_detail())
	{
		float micro_scale = read_real(shader, 0xF8);

		pass->textures[n] = hardware_texture(micro, FALSE);
		detail_scale(shader, micro, micro_scale != 0.0f ? micro_scale : 1.0f, base_bitmap, pass->scales[n]);
		pass->operations[n] = read_short(shader, 0xF4) == 1 ? _r_stage_modulate : _r_stage_modulate_twice;
		pass->alpha_operations[n++] = _r_stage_current;
	}
	/* (Halo's plain multiply: once; its double biased multiply and add:
	twice, the add as the multiply) */
	pass->multiply = function == 1;
	for (int i = 0; i < n; i++)
	{
		if (!pass->textures[i])
		{
			memset(pass, 0, sizeof(*pass));
			return FALSE;
		}
	}
	pass->stage_count = n;
	/* (testing, setting debug_shaders: each shader's pass) */
	if (dos_setting_integer("debug_shaders", 0))
	{
		char line[400];

		snprintf(line, sizeof(line), "xbox pass: type %d, function %d, micro function %d: primary %s x %.2f %.2f, "
			"secondary %s x %.2f %.2f, micro %s, %d stages", type, function, read_short(shader, 0xF4),
			primary != NONE ? tag_get_name(primary) : "-", (double)pass->scales[blended ? 2 : 1][0],
			(double)pass->scales[blended ? 2 : 1][1], secondary != NONE ? tag_get_name(secondary) : "-",
			(double)pass->scales[blended ? 1 : 0][0], (double)pass->scales[blended ? 1 : 0][1],
			micro != NONE ? tag_get_name(micro) : "-", n);
		system_log(line);
	}
	return TRUE;
}

static void make_material(long shader_tag_index, struct dos_material *material)
{
	unsigned long group = tag_get_group_tag(shader_tag_index);
	const byte *shader = tag_get(SHADER_DEFINITION_TAG, shader_tag_index);

	memset(material, 0, sizeof(*material));
	material->tint = 0x808080;
	material->u_scale = material->v_scale = 1.0f;
	if (!shader)
	{
		material->skip = TRUE;
		return;
	}
	/* (testing, setting debug_shaders 2: every shader's bitmaps, by the
	offset of their references) */
	if (dos_setting_integer("debug_shaders", 0) > 1)
	{
		char line[256];

		snprintf(line, sizeof(line), "material %s: %c%c%c%c", tag_get_name(shader_tag_index), (char)(group >> 24),
			(char)(group >> 16), (char)(group >> 8), (char)group);
		system_log(line);
		for (long offset = 0x28; offset < 0x200; offset += 4)
		{
			const struct tag_reference *reference = (const struct tag_reference *)(shader + offset);

			if (reference->group_tag == 'bitm' && reference->index != NONE)
			{
				snprintf(line, sizeof(line), "  %03lX: %s", offset, tag_get_name(reference->index));
				system_log(line);
			}
		}
	}
	switch (group)
	{
	case 'senv':
	{
		/* environment: the base map times its details, which the base map's
		alpha blends (blended: the primary where it is 1, the secondary
		where it is 0) or the secondary's own alpha does (normal), then the
		micro detail; cut out by the bump map's alpha when alpha tested */
		long base = reference_index(shader, 0x88);
		long primary = reference_index(shader, 0xB8), secondary = reference_index(shader, 0xCC);
		long micro = reference_index(shader, 0xFC);
		short type = read_short(shader, 0x2A), function = read_short(shader, 0xB0);
		unsigned long primary_tint = 0x808080, secondary_tint = 0x808080, micro_tint = 0x808080;
		unsigned long secondary_alpha = 255;
		struct bitmap_data *base_bitmap = bitmap_of(base, 0);
		int alpha_tested = read_short(shader, 0x28) & 1;

		if (primary != NONE)
			primary_tint = detail_tint(primary, function, NULL);
		if (secondary != NONE)
			secondary_tint = detail_tint(secondary, function, &secondary_alpha);
		if (micro != NONE)
			micro_tint = detail_tint(micro, read_short(shader, 0xF4), NULL);
		primary_tint = multiply_tints(primary_tint, micro_tint);
		secondary_tint = multiply_tints(secondary_tint, micro_tint);
		if (base_bitmap && !alpha_tested && dos_xbox_detail() &&
			xbox_texture_pass(shader, base_bitmap, &material->xbox_detail))
		{
			/* (the Xbox's: the base map as it is, the details over it) */
			material->texture = texture_from(base_bitmap, NULL, 0x808080, 0);
		}
		else if (base_bitmap && (type == 1 || type == 2) && !alpha_tested)
		{
			material->texture = texture_blended(base_bitmap, primary_tint, secondary_tint);
		}
		else if (base_bitmap)
		{
			/* (normal: the secondary over the primary where its alpha is low;
			no secondary, the primary alone) */
			unsigned long tint = secondary != NONE ?
				blend_tints(primary_tint, secondary_tint, secondary_alpha) : primary_tint;

			material->texture = texture_from(base_bitmap,
				alpha_tested ? bitmap_of(reference_index(shader, 0x128), 0) : NULL, tint,
				alpha_tested ? R_TEXTURE_ALPHA_TEST : 0);
		}
		if (alpha_tested)
			material->flags |= R_DRAW_TWO_SIDED;
		/* its details for the detail pass (not where alpha tested: the pass
		would cover the holes): blended, both, which the base map's alpha
		chooses between; else the primary (or the secondary) alone */
		if (!alpha_tested && base_bitmap && dos_detail_maps() && !material->xbox_detail.stage_count)
		{
			long maps[2] = { primary, secondary };
			float scales[2] = { read_real(shader, 0xB4), read_real(shader, 0xC8) };

			material->detail_blended = type == 1 || type == 2;
			if (!material->detail_blended && primary == NONE)
			{
				maps[0] = secondary;
				scales[0] = scales[1];
			}
			for (int k = 0; k < (material->detail_blended ? 2 : 1); k++)
			{
				if (maps[k] == NONE)
					continue;
				material->detail[k] = detail_texture(maps[k]);
				detail_scale(shader, maps[k], scales[k] != 0.0f ? scales[k] : 1.0f, base_bitmap,
					material->detail_scale[k]);
			}
			material->detail_base = base_bitmap;
		}
		break;
	}
	case 'soso':
	{
		/* model: the base map, cut out by its alpha unless the shader says
		not to; the change colour's source */
		unsigned short flags = (unsigned short)read_short(shader, 0x28);
		int alpha_tested = !(flags & 0x0004);
		float u_scale = read_real(shader, 0x9C), v_scale = read_real(shader, 0xA0);

		material->texture = dos_bitmap_texture(reference_index(shader, 0xA4), 0,
			alpha_tested ? R_TEXTURE_ALPHA_TEST : 0);
		material->bitmap_tag_index = reference_index(shader, 0xA4);
		material->texture_flags = alpha_tested ? R_TEXTURE_ALPHA_TEST : 0;
		if (flags & 0x0002)
			material->flags |= R_DRAW_TWO_SIDED;
		material->change_color = read_short(shader, 0x4C) != 0;
		material->u_scale = u_scale != 0.0f ? u_scale : 1.0f;
		material->v_scale = v_scale != 0.0f ? v_scale : 1.0f;
		material->translucency = read_real(shader, 0x38);
		/* its detail map (shader_model_properties: function 0xD4, scale 0xD8,
		map 0xDC, v scale 0xEC): its average colour in a texture of the
		material's own, as the environments' (the ring's corroded hull),
		and its grain over it in the detail pass (not where alpha tested) */
		{
			long detail = reference_index(shader, 0xDC);
			struct bitmap_data *base = bitmap_of(reference_index(shader, 0xA4), 0);

			if (detail != NONE && base)
			{
				unsigned long tint = detail_tint(detail, read_short(shader, 0xD4), NULL);
				float scale = read_real(shader, 0xD8), v_scale_detail = read_real(shader, 0xEC);

				if (tint != 0x808080)
				{
					r_texture *tinted = texture_from(base, NULL, tint, material->texture_flags);

					if (tinted)
						material->texture = tinted;
				}
				if (!alpha_tested && dos_detail_maps())
				{
					scale = scale != 0.0f ? scale : 1.0f;
					material->detail[0] = detail_texture(detail);
					material->detail_scale[0][0] = scale;
					material->detail_scale[0][1] = scale * (v_scale_detail != 0.0f ? v_scale_detail : 1.0f);
				}
			}
		}
		break;
	}
	case 'schi':
	case 'scex':
	{
		/* chicago: the first map, blended as the shader blends; a cube map
		looked up by the view's direction is a sky's backdrop (a skybox of
		its faces, dos_render_game.c), the others are reflections, left out */
		const struct tag_block *maps = (const struct tag_block *)(shader + 0x54);
		short first_map_type = read_short(shader, 0x2A);

		if (dos_setting_integer("debug_shaders", 0))
		{
			char line[256];

			snprintf(line, sizeof(line), "shader %s: type %d blend %d flags %02X, %ld maps, %ld extra layers",
				tag_get_name(shader_tag_index), first_map_type, read_short(shader, 0x2C), *(shader + 0x29),
				maps->count, ((const struct tag_block *)(shader + 0x48))->count);
			system_log(line);
			for (long m = 0; m < maps->count; m++)
			{
				const byte *map = (const byte *)xbox_pointer(maps->address) + m * 0xDC;
				long bitmap = reference_index(map, 0x6C);

				snprintf(line, sizeof(line), "  map %ld: %s flags %04X color %d alpha %d scale %.2f %.2f offset %.2f %.2f",
					m, bitmap != NONE ? tag_get_name(bitmap) : "none", (unsigned)read_short(map, 0), read_short(map, 0x2C),
					read_short(map, 0x2E), (double)read_real(map, 0x54), (double)read_real(map, 0x58),
					(double)read_real(map, 0x5C), (double)read_real(map, 0x60));
				system_log(line);
				if (bitmap != NONE)
				{
					snprintf(line, sizeof(line), "    average %06lX", detail_tint(bitmap, _detail_double_biased_multiply, NULL));
					system_log(line);
				}
			}
		}

		if (maps->count > 0 && first_map_type != 0)
		{
			material->skip = TRUE;
			if (first_map_type == 3)
			{
				for (int face = 0; face < 6; face++)
					material->cube[face] = cube_face_texture(reference_index((const byte *)xbox_pointer(maps->address), 0x6C), face);
			}
		}
		else if (maps->count > 0)
		{
			const byte *map = (const byte *)xbox_pointer(maps->address);
			int alpha_tested = (*(shader + 0x29) & 1) != 0;
			float su = read_real(map, 0x54), sv = read_real(map, 0x58);

			transparent_material(material, reference_index(map, 0x6C), read_short(shader, 0x2C), alpha_tested);
			/* (the first map's own tiling; a stack of maps, made into one) */
			material->u_scale = su > 0.0f ? su : 1.0f;
			material->v_scale = sv > 0.0f ? sv : 1.0f;
			if (maps->count > 1 && !material->skip)
			{
				r_texture *stack = chicago_stack(maps, alpha_tested ? R_TEXTURE_ALPHA_TEST :
					(material->flags & R_DRAW_ALPHA) ? R_TEXTURE_ALPHA : 0, &material->u_scale,
					&material->v_scale, material);

				if (stack)
					material->texture = stack;
				/* (on the card, each map a stage of its own: the stack and its
				layer stand only for the software rasterizer) */
				if (stack && dos_xbox_detail() &&
					chicago_card_pass(maps, material->u_scale, material->v_scale, &material->xbox_detail))
				{
					material->layer = NULL;
				}
			}
		}
		else
		{
			material->skip = TRUE;
		}
		break;
	}
	case 'sotr':
	{
		const struct tag_block *maps = (const struct tag_block *)(shader + 0x54);

		/* (testing, setting debug_shaders: the generic shader's maps and
		combiner stages, shader_transparent_generic_preprocessor.c's) */
		if (dos_setting_integer("debug_shaders", 0))
		{
			const struct tag_block *stages = (const struct tag_block *)(shader + 0x60);
			char line[400];

			snprintf(line, sizeof(line), "generic %s: flags %02X type %d blend %d fade %d source %d, %ld maps, %ld stages",
				tag_get_name(shader_tag_index), *(shader + 0x29), read_short(shader, 0x2A), read_short(shader, 0x2C),
				read_short(shader, 0x2E), read_short(shader, 0x30), maps->count, stages->count);
			system_log(line);
			for (long m = 0; m < maps->count; m++)
			{
				const byte *map = (const byte *)xbox_pointer(maps->address) + m * 0x64;
				long bitmap = reference_index(map, 0x1C);

				snprintf(line, sizeof(line), "  map %ld: %s flags %04X scale %.2f %.2f offset %.2f %.2f", m,
					bitmap != NONE ? tag_get_name(bitmap) : "none", (unsigned)read_short(map, 0), (double)read_real(map, 4),
					(double)read_real(map, 8), (double)read_real(map, 12), (double)read_real(map, 16));
				system_log(line);
			}
			for (long s = 0; s < stages->count; s++)
			{
				const byte *stage = (const byte *)xbox_pointer(stages->address) + s * 0x70;
				int used = snprintf(line, sizeof(line), "  stage %ld: flags %04X c0 %.2f %.2f %.2f %.2f c1 %.2f %.2f %.2f %.2f:", s,
					(unsigned)read_short(stage, 0), (double)read_real(stage, 0x0C), (double)read_real(stage, 0x10),
					(double)read_real(stage, 0x14), (double)read_real(stage, 0x18), (double)read_real(stage, 0x2C),
					(double)read_real(stage, 0x30), (double)read_real(stage, 0x34), (double)read_real(stage, 0x38));

				for (long o = 0x3C; o < 0x70 && used < (int)sizeof(line) - 8; o += 2)
					used += snprintf(line + used, sizeof(line) - (size_t)used, " %d", read_short(stage, o));
				system_log(line);
			}
		}
		if (maps->count > 0)
		{
			const byte *map = (const byte *)xbox_pointer(maps->address);

			transparent_material(material, reference_index(map, 0x1C), read_short(shader, 0x2C),
				(*(shader + 0x29) & 1) != 0);
			/* (its combiner stages on the card, where they are products: the
			first map alone stands for them elsewhere) */
			if (!material->skip && dos_xbox_detail())
				generic_card_pass(shader, &material->xbox_detail);
		}
		else
		{
			material->skip = TRUE;
		}
		break;
	}
	case 'swat':
		if (dos_setting_integer("debug_shaders", 0))
		{
			char line[256];
			long base = reference_index(shader, 0x4C), reflection = reference_index(shader, 0x9C);

			snprintf(line, sizeof(line), "water %s: flags %04X base %s reflection %s perpendicular %.2f %.2f %.2f %.2f "
				"parallel %.2f %.2f %.2f %.2f", tag_get_name(shader_tag_index), (unsigned)read_short(shader, 0x28),
				base != NONE ? tag_get_name(base) : "none", reflection != NONE ? tag_get_name(reflection) : "none",
				(double)read_real(shader, 0x6C), (double)read_real(shader, 0x70), (double)read_real(shader, 0x74),
				(double)read_real(shader, 0x78), (double)read_real(shader, 0x7C), (double)read_real(shader, 0x80),
				(double)read_real(shader, 0x84), (double)read_real(shader, 0x88));
			system_log(line);
			snprintf(line, sizeof(line), "  base average %08lX, reflection average %08lX, tint %06lX",
				base != NONE ? detail_tint(base, _detail_double_biased_multiply, NULL) : 0,
				reflection != NONE ? cube_average(reflection) : 0, water_tint(shader,
				dos_bitmap_texture(base, 0, 0)));
			system_log(line);
		}
		/* water: its base map in the colour the shader gives it (water_tint),
		added to what is below (half over it, as before, darkened the bed:
		black where the colour is dark), faded out in fog as all added light
		is (r_main.c) */
		material->texture = dos_bitmap_texture(reference_index(shader, 0x4C), 0, 0);
		material->flags |= R_DRAW_ADDITIVE | R_DRAW_TWO_SIDED;
		material->transparent = TRUE;
		material->tint = water_tint(shader, material->texture);
		break;
	case 'sgla':
		/* glass: its diffuse map, half over */
		material->texture = dos_bitmap_texture(reference_index(shader, 0x158), 0, 0);
		material->flags |= R_DRAW_BLEND | R_DRAW_TWO_SIDED;
		material->transparent = TRUE;
		break;
	default:
		/* plasma, meters: left out */
		material->skip = TRUE;
		break;
	}
}

#define TINTED_SLOTS 96

static struct tinted_slot
{
	const struct dos_material *material;
	unsigned long change_color;
	r_texture *texture;
} tinted_slots[TINTED_SLOTS];
static int tinted_slot_count;

r_texture *dos_material_tinted(const struct dos_material *material, unsigned long change_color)
{
	struct bitmap_data *bitmap;
	struct tinted_slot *slot;
	unsigned long tint;

	for (int i = 0; i < tinted_slot_count; i++)
	{
		if (tinted_slots[i].material == material && tinted_slots[i].change_color == change_color)
			return tinted_slots[i].texture ? tinted_slots[i].texture : material->texture;
	}
	if (tinted_slot_count == TINTED_SLOTS || !(bitmap = bitmap_of(material->bitmap_tag_index, 0)))
		return material->texture;
	/* (0xFF a channel as it is: the tint's 0x80) */
	tint = ((((change_color >> 16) & 0xFF) * 128 / 255) << 16) | ((((change_color >> 8) & 0xFF) * 128 / 255) << 8) |
		((change_color & 0xFF) * 128 / 255);
	slot = &tinted_slots[tinted_slot_count++];
	slot->material = material;
	slot->change_color = change_color;
	slot->texture = texture_from(bitmap, NULL, tint, material->texture_flags);
	return slot->texture ? slot->texture : material->texture;
}

const struct dos_material *dos_shader_material(long shader_tag_index)
{
	static struct dos_material none = { NULL, 0, 0x808080, FALSE, TRUE, FALSE, 1.0f, 1.0f };

	if (shader_tag_index == NONE)
		return &none;
	for (int i = 0; i < material_slot_count; i++)
	{
		if (material_slots[i].shader_tag_index == shader_tag_index)
			return &material_slots[i].material;
	}
	if (material_slot_count == MATERIAL_SLOTS)
		return &none;
	material_slots[material_slot_count].shader_tag_index = shader_tag_index;
	make_material(shader_tag_index, &material_slots[material_slot_count].material);
	return &material_slots[material_slot_count++].material;
}

/* ---------- terrain tiles

A blended environment shader (Blood Gulch's ground, most outdoor terrain)
has a base map that covers the whole level at a few texels a metre, whose
alpha chooses between two detail maps (grass, sand...) tiled many times
over it, and a micro detail over both. Its material's texture is the base
map times the details' average colours: up close, big blocks of colour.

A terrain tile is a batch's own texture instead (dos_level.c): its square of
the base map, sampled smoothly, times the real details at their own scale,
combined as Halo's shader does, baked once when the level is built. Drawing
it costs what drawing the material's texture did: one texture a triangle.
The details are averaged to about the tile's texels (their halvings), so
they do not sparkle; the renderer's own smaller levels of the tile do the
distance. */

#define TERRAIN_MIPS 10

/* a map and its halvings, each texel the average of four of the level
above, down to 1 texel: a tile samples the level whose texels are about its
own, so the details average out rather than sparkle */
struct terrain_map
{
	unsigned long *texels[TERRAIN_MIPS];
	int width[TERRAIN_MIPS], height[TERRAIN_MIPS];
	int levels;
};

static struct
{
	long shader;
	int usable;
	struct terrain_map base, primary, secondary, micro;
	float primary_scale[2], secondary_scale[2], micro_scale[2];
	short function, micro_function;
} terrain = { NONE };

static void terrain_map_free(struct terrain_map *map)
{
	for (int i = 0; i < map->levels; i++)
		raw_free(map->texels[i]);
	memset(map, 0, sizeof(*map));
}

static void terrain_map_load(struct terrain_map *map, long bitmap_tag_index, int largest)
{
	struct bitmap_data *bitmap = bitmap_of(bitmap_tag_index, 0);

	memset(map, 0, sizeof(*map));
	if (!bitmap || !(map->texels[0] = dos_bitmap_decode(bitmap, largest, &map->width[0], &map->height[0])))
		return;
	map->levels = 1;
	while (map->levels < TERRAIN_MIPS && (map->width[map->levels - 1] > 1 || map->height[map->levels - 1] > 1))
	{
		int level = map->levels;
		int w = MAX(map->width[level - 1] / 2, 1), h = MAX(map->height[level - 1] / 2, 1);
		const unsigned long *above = map->texels[level - 1];
		int above_w = map->width[level - 1], above_h = map->height[level - 1];
		unsigned long *texels = raw_malloc((size_t)w * h * sizeof(unsigned long));

		if (!texels)
			break;
		for (int y = 0; y < h; y++)
		{
			for (int x = 0; x < w; x++)
			{
				unsigned long sum[4] = { 0, 0, 0, 0 };

				for (int k = 0; k < 4; k++)
				{
					unsigned long c = above[MIN(y * 2 + (k >> 1), above_h - 1) * above_w + MIN(x * 2 + (k & 1), above_w - 1)];

					sum[0] += c >> 24;
					sum[1] += (c >> 16) & 0xFF;
					sum[2] += (c >> 8) & 0xFF;
					sum[3] += c & 0xFF;
				}
				texels[y * w + x] = ((sum[0] / 4) << 24) | ((sum[1] / 4) << 16) | ((sum[2] / 4) << 8) | (sum[3] / 4);
			}
		}
		map->texels[level] = texels;
		map->width[level] = w;
		map->height[level] = h;
		map->levels++;
	}
}

/* the level of a map whose texels are about spacing (in its coordinates)
apart */
static int terrain_level(const struct terrain_map *map, float spacing)
{
	int level = 0;

	while (level + 1 < map->levels && spacing * (float)map->width[level] > 2.0f)
		level++;
	return level;
}

/* the detail map's scale over the base map's coordinates: the shader's,
in base map texels when it rescales its details (diffuse flag 0) */
static void detail_scale(const byte *shader, long bitmap_tag_index, float scale, struct bitmap_data *base, float *out)
{
	struct bitmap_data *detail = bitmap_of(bitmap_tag_index, 0);

	out[0] = out[1] = scale;
	if (base && detail && (read_short(shader, 0x6C) & 1) && detail->width > 0 && detail->height > 0)
	{
		out[0] = scale * (float)base->width / (float)detail->width;
		out[1] = scale * (float)base->height / (float)detail->height;
	}
}

void dos_terrain_tiles_done(void)
{
	terrain_map_free(&terrain.base);
	terrain_map_free(&terrain.primary);
	terrain_map_free(&terrain.secondary);
	terrain_map_free(&terrain.micro);
	terrain.shader = NONE;
	terrain.usable = FALSE;
}

/* whether a shader's batches can be terrain tiles: a blended environment
shader with details, not alpha tested (nothing is decoded) */
int dos_terrain_shader(long shader_tag_index)
{
	const byte *shader;
	short type;

	if (shader_tag_index == NONE || tag_get_group_tag(shader_tag_index) != 'senv')
		return FALSE;
	shader = tag_get(SHADER_DEFINITION_TAG, shader_tag_index);
	type = read_short(shader, 0x2A);
	return reference_index(shader, 0x88) != NONE && (type == 1 || type == 2) && !(read_short(shader, 0x28) & 1) &&
		(reference_index(shader, 0xB8) != NONE || reference_index(shader, 0xCC) != NONE);
}

static int terrain_prepare(long shader_tag_index)
{
	const byte *shader;
	struct bitmap_data *base;
	long primary, secondary, micro;
	short type;

	if (terrain.shader == shader_tag_index)
		return terrain.usable;
	dos_terrain_tiles_done();
	terrain.shader = shader_tag_index;
	if (shader_tag_index == NONE || tag_get_group_tag(shader_tag_index) != 'senv')
		return FALSE;
	shader = tag_get(SHADER_DEFINITION_TAG, shader_tag_index);
	type = read_short(shader, 0x2A);
	primary = reference_index(shader, 0xB8);
	secondary = reference_index(shader, 0xCC);
	micro = reference_index(shader, 0xFC);
	base = bitmap_of(reference_index(shader, 0x88), 0);
	/* (blended ones, not alpha tested: their base map is the level's colour
	map; a normal shader's base is a texture of its own, tiled) */
	if (!base || (type != 1 && type != 2) || (read_short(shader, 0x28) & 1) || (primary == NONE && secondary == NONE))
	{
		if (dos_setting_integer("debug_tiles", 0))
		{
			char line[200];

			snprintf(line, sizeof(line), "tiles: %s: type %d, flags %x, base %dx%d, details %ld %ld: not blended",
				tag_get_name(shader_tag_index), type, read_short(shader, 0x28), base ? base->width : 0,
				base ? base->height : 0, primary, secondary);
			system_log(line);
		}
		return FALSE;
	}
	terrain.function = read_short(shader, 0xB0);
	terrain.micro_function = read_short(shader, 0xF4);
	detail_scale(shader, primary, read_real(shader, 0xB4), base, terrain.primary_scale);
	detail_scale(shader, secondary, read_real(shader, 0xC8), base, terrain.secondary_scale);
	detail_scale(shader, micro, read_real(shader, 0xF8), base, terrain.micro_scale);
	terrain_map_load(&terrain.base, reference_index(shader, 0x88), 512);
	terrain_map_load(&terrain.primary, primary, 256);
	terrain_map_load(&terrain.secondary, secondary, 256);
	terrain_map_load(&terrain.micro, micro, 128);
	terrain.usable = terrain.base.levels && (terrain.primary.levels || terrain.secondary.levels);
	return terrain.usable;
}

/* A map read along a row of a tile in 16.16 fixed point texels of its
level: where the row starts (wrapped into the map) and its step a texel of
the tile. Integers throughout: a slow machine bakes a map's tiles while it
loads (floats, and the C library's floorf, took half a minute at 100 MHz). */
struct terrain_walk
{
	const unsigned long *texels;
	int width, height;
	unsigned long width_mask, height_mask; /* (0: a side that is no power of two) */
	long u, v, du, dv;
};

static void terrain_walk_start(struct terrain_walk *walk, const struct terrain_map *map, int level, float u, float v,
	float du, float dv, float u_scale, float v_scale)
{
	double x, y;

	walk->texels = map->texels[level];
	walk->width = map->width[level];
	walk->height = map->height[level];
	walk->width_mask = (walk->width & (walk->width - 1)) ? 0 : (unsigned long)walk->width - 1;
	walk->height_mask = (walk->height & (walk->height - 1)) ? 0 : (unsigned long)walk->height - 1;
	/* (texel centres: half a texel back) */
	x = (double)u * u_scale * walk->width - 0.5;
	y = (double)v * v_scale * walk->height - 0.5;
	x -= (double)walk->width * (double)(long)(x / walk->width);
	y -= (double)walk->height * (double)(long)(y / walk->height);
	if (x < 0.0)
		x += walk->width;
	if (y < 0.0)
		y += walk->height;
	walk->u = (long)(x * 65536.0);
	walk->v = (long)(y * 65536.0);
	walk->du = (long)((double)du * u_scale * walk->width * 65536.0);
	walk->dv = (long)((double)dv * v_scale * walk->height * 65536.0);
}

static inline unsigned long walk_wrap_x(const struct terrain_walk *walk, long x)
{
	if (walk->width_mask)
		return (unsigned long)x & walk->width_mask;
	x %= walk->width;
	return (unsigned long)(x < 0 ? x + walk->width : x);
}

static inline unsigned long walk_wrap_y(const struct terrain_walk *walk, long y)
{
	if (walk->height_mask)
		return (unsigned long)y & walk->height_mask;
	y %= walk->height;
	return (unsigned long)(y < 0 ? y + walk->height : y);
}

/* the nearest texel at (u, v) */
static inline unsigned long walk_nearest(const struct terrain_walk *walk, long u, long v)
{
	return walk->texels[walk_wrap_y(walk, (v + 0x8000) >> 16) * (unsigned long)walk->width +
		walk_wrap_x(walk, (u + 0x8000) >> 16)];
}

/* two colours blended (weight of the second 0 to 256), the four channels
in two multiplies */
static inline unsigned long blend_argb(unsigned long a, unsigned long b, unsigned long weight)
{
	unsigned long rb = (((a & 0x00FF00FF) * (256 - weight) + (b & 0x00FF00FF) * weight) >> 8) & 0x00FF00FF;
	unsigned long ag = ((((a >> 8) & 0x00FF00FF) * (256 - weight) + ((b >> 8) & 0x00FF00FF) * weight) >> 8) & 0x00FF00FF;

	return rb | (ag << 8);
}

/* the four nearest texels at (u, v) blended */
static inline unsigned long walk_bilinear(const struct terrain_walk *walk, long u, long v)
{
	long x = u >> 16, y = v >> 16;
	unsigned long ax = ((unsigned long)u >> 8) & 0xFF, ay = ((unsigned long)v >> 8) & 0xFF;
	unsigned long x0 = walk_wrap_x(walk, x), x1 = walk_wrap_x(walk, x + 1);
	const unsigned long *row0 = walk->texels + walk_wrap_y(walk, y) * (unsigned long)walk->width;
	const unsigned long *row1 = walk->texels + walk_wrap_y(walk, y + 1) * (unsigned long)walk->width;

	return blend_argb(blend_argb(row0[x0], row0[x1], ax), blend_argb(row1[x0], row1[x1], ax), ay);
}

/* Halo's detail functions on 0 to 255: 2 x base x detail, base x detail,
base + 2 x detail - 1 */
static inline long terrain_combine(short function, long base, long detail)
{
	switch (function)
	{
	case _detail_multiply:
		return (base * detail) >> 8;
	case _detail_double_biased_add:
		return base + 2 * detail - 255;
	default:
		return (base * detail) >> 7;
	}
}

/* a tile's texels, size a side (the caller frees them); NULL if the shader
is no terrain's */
static unsigned long *bake_tile(long shader_tag_index, const float *uv_low, const float *uv_high, int size)
{
	unsigned long *texels;
	float du, dv;
	int base_level, primary_level, secondary_level, micro_level;

	if (size < 2 || !terrain_prepare(shader_tag_index))
		return NULL;
	texels = raw_malloc((size_t)size * size * sizeof(unsigned long));
	if (!texels)
		return NULL;
	/* (texel centres at the corners: the tile's coordinates map its square
	from the first texel's middle to the last's, dos_level.c) */
	du = (uv_high[0] - uv_low[0]) / (float)(size - 1);
	dv = (uv_high[1] - uv_low[1]) / (float)(size - 1);
	{
		float spacing = MAX(du, dv);

		base_level = terrain_level(&terrain.base, spacing);
		primary_level = terrain_level(&terrain.primary, spacing * MAX(terrain.primary_scale[0], terrain.primary_scale[1]));
		secondary_level = terrain_level(&terrain.secondary,
			spacing * MAX(terrain.secondary_scale[0], terrain.secondary_scale[1]));
		micro_level = terrain_level(&terrain.micro, spacing * MAX(terrain.micro_scale[0], terrain.micro_scale[1]));
	}
	for (int t = 0; t < size; t++)
	{
		float v = uv_low[1] + dv * (float)t;
		struct terrain_walk base, primary, secondary, micro;
		int has_primary = terrain.primary.levels > 0, has_secondary = terrain.secondary.levels > 0;
		int has_micro = terrain.micro.levels > 0;

		terrain_walk_start(&base, &terrain.base, base_level, uv_low[0], v, du, 0.0f, 1.0f, 1.0f);
		if (has_primary)
		{
			terrain_walk_start(&primary, &terrain.primary, primary_level, uv_low[0], v, du, 0.0f,
				terrain.primary_scale[0], terrain.primary_scale[1]);
		}
		if (has_secondary)
		{
			terrain_walk_start(&secondary, &terrain.secondary, secondary_level, uv_low[0], v, du, 0.0f,
				terrain.secondary_scale[0], terrain.secondary_scale[1]);
		}
		if (has_micro)
		{
			terrain_walk_start(&micro, &terrain.micro, micro_level, uv_low[0], v, du, 0.0f, terrain.micro_scale[0],
				terrain.micro_scale[1]);
		}
		/* the usual case, quicker: every map a power of two a side, both
		functions 2 x base x detail (and the coordinates stepped, not
		multiplied); the same texels as the loop after it */
		if (base.width_mask && base.height_mask && has_primary && primary.width_mask && primary.height_mask &&
			(!has_secondary || (secondary.width_mask && secondary.height_mask)) &&
			(!has_micro || (micro.width_mask && micro.height_mask && terrain.micro_function == _detail_double_biased_multiply)) &&
			terrain.function == _detail_double_biased_multiply)
		{
			const unsigned long *base_row0 = base.texels + ((unsigned long)(base.v >> 16) & base.height_mask) * base.width;
			const unsigned long *base_row1 = base.texels + ((unsigned long)((base.v >> 16) + 1) & base.height_mask) * base.width;
			unsigned long ay = ((unsigned long)base.v >> 8) & 0xFF;
			const unsigned long *primary_row = primary.texels +
				((unsigned long)((primary.v + 0x8000) >> 16) & primary.height_mask) * primary.width;
			const unsigned long *secondary_row = has_secondary ? secondary.texels +
				((unsigned long)((secondary.v + 0x8000) >> 16) & secondary.height_mask) * secondary.width : primary_row;
			const unsigned long *micro_row = has_micro ? micro.texels +
				((unsigned long)((micro.v + 0x8000) >> 16) & micro.height_mask) * micro.width : NULL;
			unsigned long secondary_mask = has_secondary ? secondary.width_mask : primary.width_mask;
			long bu = base.u, pu = primary.u + 0x8000, su = has_secondary ? secondary.u + 0x8000 : pu;
			long su_step = has_secondary ? secondary.du : primary.du, mu = has_micro ? micro.u + 0x8000 : 0;
			unsigned long *out = texels + (size_t)t * size;

			for (int s = 0; s < size; s++)
			{
				unsigned long x = (unsigned long)(bu >> 16), ax = ((unsigned long)bu >> 8) & 0xFF;
				unsigned long x0 = x & base.width_mask, x1 = (x + 1) & base.width_mask;
				unsigned long b = blend_argb(blend_argb(base_row0[x0], base_row0[x1], ax),
					blend_argb(base_row1[x0], base_row1[x1], ax), ay);
				unsigned long p = primary_row[(unsigned long)(pu >> 16) & primary.width_mask];
				unsigned long q = secondary_row[(unsigned long)(su >> 16) & secondary_mask];
				unsigned long alpha = b >> 24;
				unsigned long detail = blend_argb(q, p, alpha + (alpha >> 7));
				unsigned long color = 0xFF000000;

				if (micro_row)
				{
					unsigned long m = micro_row[(unsigned long)(mu >> 16) & micro.width_mask];

					for (int shift = 16; shift >= 0; shift -= 8)
					{
						unsigned long value = ((((b >> shift) & 0xFF) * ((detail >> shift) & 0xFF)) >> 7);

						value = value > 255 ? 255 : value;
						value = (value * ((m >> shift) & 0xFF)) >> 7;
						color |= (value > 255 ? 255 : value) << shift;
					}
					mu += micro.du;
				}
				else
				{
					for (int shift = 16; shift >= 0; shift -= 8)
					{
						unsigned long value = ((((b >> shift) & 0xFF) * ((detail >> shift) & 0xFF)) >> 7);

						color |= (value > 255 ? 255 : value) << shift;
					}
				}
				out[s] = color;
				bu += base.du;
				pu += primary.du;
				su += su_step;
			}
			continue;
		}
		for (int s = 0; s < size; s++)
		{
			unsigned long b = walk_bilinear(&base, base.u + base.du * s, base.v);
			unsigned long p = has_primary ? walk_nearest(&primary, primary.u + primary.du * s, primary.v) : 0;
			unsigned long q = has_secondary ? walk_nearest(&secondary, secondary.u + secondary.du * s, secondary.v) : p;
			unsigned long m = has_micro ? walk_nearest(&micro, micro.u + micro.du * s, micro.v) : 0;
			/* (the base's alpha: the primary where it is 1) */
			unsigned long alpha = b >> 24;
			unsigned long detail = blend_argb(q, has_primary ? p : q, alpha + (alpha >> 7));
			unsigned long color = 0xFF000000;

			for (int shift = 16; shift >= 0; shift -= 8)
			{
				long value = terrain_combine(terrain.function, (long)((b >> shift) & 0xFF), (long)((detail >> shift) & 0xFF));

				/* (clamped between the stages, as the Xbox's combiners are) */
				value = value < 0 ? 0 : value > 255 ? 255 : value;
				if (has_micro)
					value = terrain_combine(terrain.micro_function, value, (long)((m >> shift) & 0xFF));
				value = value < 0 ? 0 : value > 255 ? 255 : value;
				color |= (unsigned long)value << shift;
			}
			texels[t * size + s] = color;
		}
	}
	return texels;
}

void dos_terrain_preview(long shader_tag_index, const float *uv_low, const float *uv_high, int size,
	unsigned long *histogram)
{
	unsigned long *texels = bake_tile(shader_tag_index, uv_low, uv_high, size);

	if (texels)
		r_histogram_add(histogram, texels, (long)size * size);
	raw_free(texels);
}

r_texture *dos_terrain_tile(long shader_tag_index, const float *uv_low, const float *uv_high, int size,
	r_shared_palette *palette)
{
	unsigned long *texels = bake_tile(shader_tag_index, uv_low, uv_high, size);
	r_texture *texture;

	if (!texels)
		return NULL;
	/* (testing: debug_tiles 3 writes the first tiles as TILEnn.RAW, 32-bit
	texels) */
	if (dos_setting_integer("debug_tiles", 0) > 2)
	{
		static int written;

		if (written < 8)
		{
			char name[32];
			FILE *file;

			snprintf(name, sizeof(name), "TILE%02d.RAW", written++);
			if ((file = fopen(name, "wb")) != NULL)
			{
				fwrite(texels, 4, (size_t)size * size, file);
				fclose(file);
			}
		}
	}
	texture = r_texture_new_shared(texels, size, size, R_TEXTURE_LARGE, palette);
	raw_free(texels);
	return texture;
}

/* ---------- maps */

static int shader_index_count;

void dos_textures_initialize_for_new_map(void)
{
	/* (the lighting's pixels: the old map's bitmaps are gone with it) */
	while (lighting_pixel_count)
		lighting_pixels_forget(lighting_pixel_count - 1, FALSE);
	texture_slot_count = 0;
	material_slot_count = 0;
	tinted_slot_count = 0;
	shader_index_count = 0;
	for (int i = 0; i < TEXTURE_SLOTS; i++)
		texture_slots[i].hardware = 0;
	hardware_grey = 0;
	hardware_white = 0;
	constant_texture_count = 0;
}

void dos_textures_dispose_from_old_map(void)
{
	char line[96];

	snprintf(line, sizeof(line), "render: %d textures, %lu KB, %d materials", texture_slot_count,
		r_texture_memory() / 1024, material_slot_count);
	system_log(line);
	for (int i = 0; i < texture_slot_count; i++)
	{
		r_texture_delete(texture_slots[i].texture);
		r_hardware_texture_delete(texture_slots[i].hardware);
		texture_slots[i].hardware = 0;
	}
	r_hardware_texture_delete(hardware_grey);
	hardware_grey = 0;
	r_hardware_texture_delete(hardware_white);
	hardware_white = 0;
	for (int i = 0; i < constant_texture_count; i++)
		r_hardware_texture_delete(constant_textures[i].hardware);
	constant_texture_count = 0;
	/* (a material's own texture, not a slot's: senv's tinted ones) */
	for (int i = 0; i < material_slot_count; i++)
	{
		r_texture *texture = material_slots[i].material.texture;
		int shared = FALSE;

		for (int k = 0; k < texture_slot_count && !shared; k++)
			shared = texture_slots[k].texture == texture;
		if (!shared)
			r_texture_delete(texture);
	}
	texture_slot_count = 0;
	material_slot_count = 0;
}

/* ---------- a shader's tag from its definition (the models' parts name
their shader by its definition) */

#define SHADER_INDEX_SLOTS 512

static struct
{
	struct shader const *shader;
	long tag_index;
} shader_indices[SHADER_INDEX_SLOTS];

long dos_shader_tag_index(struct shader const *shader)
{
	struct tag_iterator iterator;
	long tag_index;

	for (int i = 0; i < shader_index_count; i++)
	{
		if (shader_indices[i].shader == shader)
			return shader_indices[i].tag_index;
	}
	tag_iterator_new(&iterator, SHADER_DEFINITION_TAG);
	while ((tag_index = tag_iterator_next(&iterator)) != NONE)
	{
		if (tag_get(SHADER_DEFINITION_TAG, tag_index) == (void *)shader)
			break;
	}
	if (shader_index_count < SHADER_INDEX_SLOTS)
	{
		shader_indices[shader_index_count].shader = shader;
		shader_indices[shader_index_count].tag_index = tag_index;
		shader_index_count++;
	}
	return tag_index;
}
