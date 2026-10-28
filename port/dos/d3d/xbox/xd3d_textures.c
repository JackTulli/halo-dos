/*
XD3D_TEXTURES.C

The PC's copies of the textures the game draws with (xd3d.h), and the
targets it draws into.

A texture header is known by its Data, Format and Size; its copy is made
from the texels at Data, every format decoded to 32-bit ARGB
(xbox_textures_convert.c, the same decoder halo-ce-universal's renderers
use), and made again when the header's Lock serial changes (it was
registered or locked again: xd3d_resources.c). A paletted texture's copy
also goes with its palette's serial.

A surface the game draws into (other than the back buffer) is a target
texture on the PC, known by its Data; a texture whose Data is a target's
samples the target. The back buffer itself, sampled (the screen effects
copy the screen they are drawing), is copied into a texture of its own
each time it has changed since the last copy.
*/

#include "xd3d.h"
#include "xbox_textures_convert.h"

#include <stdlib.h>
#include <string.h>

#define BUCKETS 1024
/* frames a copy is kept without being drawn with, once there are many */
#define UNUSED_FRAMES 600
#define MANY_COPIES 1536

struct texture_entry
{
	struct texture_entry *next;
	DWORD data, format, size;
	/* what the copy was made from */
	DWORD serial, palette, palette_serial;
	BOOL target;
	struct pcd3d_texture *texture;
	/* the copy's size, and what the game's coordinates are multiplied by */
	unsigned long width, height;
	float coordinate_scale[2];
	BOOL cube_map, volume;
	unsigned long used_frame;
};

static struct texture_entry *buckets[BUCKETS];
static unsigned long entry_count, frame;
/* the back buffer's copy: made again when the back buffer changed */
static struct pcd3d_texture *screen_copy;
static unsigned long screen_copy_width, screen_copy_height;
unsigned long xd3d_back_buffer_changes;
static unsigned long screen_copied_changes = ~0UL;

static unsigned long bucket_of(DWORD data)
{
	return (data >> 7) % BUCKETS;
}

static struct texture_entry *entry_find(DWORD data, DWORD format, DWORD size, BOOL target)
{
	struct texture_entry *entry;

	for (entry = buckets[bucket_of(data)]; entry; entry = entry->next)
	{
		if (entry->data == data && entry->target == target && (target || (entry->format == format && entry->size == size)))
			return entry;
	}
	return NULL;
}

static struct texture_entry *entry_new(DWORD data, DWORD format, DWORD size, BOOL target)
{
	struct texture_entry *entry = calloc(1, sizeof(*entry));

	if (!entry)
		return NULL;
	entry->data = data;
	entry->format = format;
	entry->size = size;
	entry->target = target;
	entry->next = buckets[bucket_of(data)];
	buckets[bucket_of(data)] = entry;
	entry_count++;
	return entry;
}

static unsigned long power_of_two(unsigned long value)
{
	unsigned long result = 1;

	while (result < value)
		result <<= 1;
	return result;
}

/* ---------- copies of textures */

/* writes the copy's texels from the Xbox's; FALSE when it cannot */
static BOOL upload(struct texture_entry *entry, const struct xd3d_texture_description *description,
	const void *palette, const struct pcd3d_capabilities *capabilities)
{
	const unsigned char *base = (const unsigned char *)entry->data;
	unsigned long faces = description->cube_map ? 6 : 1;
	unsigned long face_size = xd3d_texture_face_size(description);
	unsigned long drop = 0, levels, level, face;
	unsigned long width = description->width, height = description->height;
	unsigned long maximum = capabilities->maximum_texture_size ? capabilities->maximum_texture_size : 256;

	/* the levels too large for the card are left out */
	while ((width >> drop) > maximum || (height >> drop) > maximum)
		drop++;
	if (drop >= description->levels)
		return FALSE;
	levels = description->levels - drop;
	width = width >> drop ? width >> drop : 1;
	height = height >> drop ? height >> drop : 1;
	if (description->linear && capabilities->power_of_two)
	{
		entry->width = power_of_two(width);
		entry->height = power_of_two(height);
	}
	else
	{
		entry->width = width;
		entry->height = height;
	}
	if (entry->texture)
	{
		unsigned long old_width, old_height;

		pcd3d_texture_size(entry->texture, &old_width, &old_height);
		if (old_width != entry->width || old_height != entry->height || entry->cube_map != description->cube_map)
		{
			pcd3d_texture_delete(entry->texture);
			entry->texture = NULL;
		}
	}
	if (!entry->texture)
		entry->texture = pcd3d_texture_new(entry->width, entry->height, levels, description->cube_map, FALSE);
	if (!entry->texture)
		return FALSE;
	entry->cube_map = description->cube_map;
	entry->volume = description->depth > 1;
	/* linear textures are addressed in texels */
	entry->coordinate_scale[0] = description->linear ? 1.0f / (float)entry->width : 1.0f;
	entry->coordinate_scale[1] = description->linear ? 1.0f / (float)entry->height : 1.0f;

	for (face = 0; face < faces; face++)
	{
		for (level = 0; level < levels; level++)
		{
			unsigned long source_level = level + drop;
			unsigned long level_width = description->width >> source_level ? description->width >> source_level : 1;
			unsigned long level_height = description->height >> source_level ? description->height >> source_level : 1;
			const unsigned char *source = base + face * face_size + xd3d_texture_level_offset(description, source_level);
			unsigned long pitch;
			unsigned char *destination = pcd3d_texture_lock(entry->texture, (int)face, (int)level, &pitch);

			if (!destination)
				return FALSE;
			if (description->compressed)
			{
				/* (a volume's first slice: the PC copies are flat) */
				texture_convert_dxt_level(description->kind, _texture_destination_bgra8, destination, pitch, source,
					level_width, level_height, 1);
			}
			else
			{
				texture_convert_level(description->kind, _texture_destination_bgra8, destination, pitch, source,
					level_width, level_height, 1, description->linear ? description->pitch : 0, palette);
			}
			/* (a linear texture padded to powers of two: its edge repeated) */
			if (description->linear && (entry->width > level_width || entry->height > level_height))
			{
				unsigned long row;

				for (row = 0; row < level_height; row++)
					texture_convert_pad_row(destination + row * pitch, level_width, entry->width, 4);
				texture_convert_pad_rows(destination, pitch, level_height, entry->height);
			}
			pcd3d_texture_unlock(entry->texture, (int)face, (int)level);
			/* (a linear texture has one level) */
			if (description->linear)
				break;
		}
	}
	return TRUE;
}

/* the back buffer, sampled: its copy, made again when it has changed */
static BOOL screen_copy_get(struct xd3d_bound_texture *bound)
{
	unsigned long width, height;

	pcd3d_size(&width, &height);
	if (screen_copy && (screen_copy_width != width || screen_copy_height != height))
	{
		pcd3d_texture_delete(screen_copy);
		screen_copy = NULL;
	}
	if (!screen_copy)
	{
		screen_copy = pcd3d_texture_new(width, height, 1, FALSE, TRUE);
		screen_copy_width = width;
		screen_copy_height = height;
		screen_copied_changes = ~0UL;
	}
	if (!screen_copy)
		return FALSE;
	if (screen_copied_changes != xd3d_back_buffer_changes)
	{
		pcd3d_texture_copy_back_buffer(screen_copy);
		screen_copied_changes = xd3d_back_buffer_changes;
	}
	bound->texture = screen_copy;
	/* (the game addresses the 640x480 back buffer in its texels) */
	bound->coordinate_scale[0] = 1.0f / 640.0f;
	bound->coordinate_scale[1] = 1.0f / 480.0f;
	return TRUE;
}

BOOL xd3d_texture_get(const D3DBaseTexture *texture, const D3DPalette *palette, struct xd3d_bound_texture *bound)
{
	struct texture_entry *entry;
	struct xd3d_texture_description description;
	struct pcd3d_capabilities capabilities;
	DWORD palette_data = 0, palette_serial = 0;

	memset(bound, 0, sizeof(*bound));
	if (!texture || !texture->Data)
		return FALSE;
	if (texture->Data == xd3d_back_buffer()->Data)
		return screen_copy_get(bound);
	xd3d_texture_describe(texture->Format, texture->Size, &description);
	/* a target the game drew into */
	entry = entry_find(texture->Data, 0, 0, TRUE);
	if (entry && entry->texture)
	{
		entry->used_frame = frame;
		bound->texture = entry->texture;
		bound->coordinate_scale[0] = description.linear ? 1.0f / (float)description.width : 1.0f;
		bound->coordinate_scale[1] = description.linear ? 1.0f / (float)description.height : 1.0f;
		return TRUE;
	}
	if (xd3d_format_is_depth(description.format))
		return FALSE;
	if (description.kind == _texture_kind_p8)
	{
		if (!palette || !palette->Data)
			return FALSE;
		palette_data = palette->Data;
		palette_serial = palette->Lock;
	}
	entry = entry_find(texture->Data, texture->Format, texture->Size, FALSE);
	if (!entry)
		entry = entry_new(texture->Data, texture->Format, texture->Size, FALSE);
	if (!entry)
		return FALSE;
	entry->used_frame = frame;
	if (!entry->texture || entry->serial != texture->Lock || entry->palette != palette_data ||
		entry->palette_serial != palette_serial)
	{
		pcd3d_capabilities(&capabilities);
		entry->serial = texture->Lock;
		entry->palette = palette_data;
		entry->palette_serial = palette_serial;
		if (!upload(entry, &description, (const void *)palette_data, &capabilities))
		{
			if (entry->texture)
				pcd3d_texture_delete(entry->texture);
			entry->texture = NULL;
			return FALSE;
		}
	}
	bound->texture = entry->texture;
	bound->coordinate_scale[0] = entry->coordinate_scale[0];
	bound->coordinate_scale[1] = entry->coordinate_scale[1];
	bound->cube_map = entry->cube_map;
	bound->volume = entry->volume;
	return TRUE;
}

/* ---------- targets */

struct pcd3d_texture *xd3d_target_get(const D3DSurface *surface, BOOL *usable)
{
	struct texture_entry *entry;
	struct xd3d_texture_description description;
	unsigned long width, height;

	*usable = TRUE;
	if (!surface || surface->Data == xd3d_back_buffer()->Data)
		return NULL;
	xd3d_texture_describe(surface->Format, surface->Size, &description);
	width = description.width;
	height = description.height;
	entry = entry_find(surface->Data, 0, 0, TRUE);
	if (entry && entry->texture)
	{
		unsigned long old_width, old_height;

		pcd3d_texture_size(entry->texture, &old_width, &old_height);
		if (old_width != width || old_height != height)
		{
			pcd3d_texture_delete(entry->texture);
			entry->texture = NULL;
		}
	}
	if (!entry)
		entry = entry_new(surface->Data, 0, 0, TRUE);
	if (!entry)
	{
		*usable = FALSE;
		return NULL;
	}
	entry->used_frame = frame;
	if (!entry->texture)
	{
		entry->texture = pcd3d_texture_new(width, height, 1, FALSE, TRUE);
		entry->width = width;
		entry->height = height;
		if (!entry->texture)
			platform_log("Direct3D: no %lux%lu target (format %lu) for %08lx", width, height,
				(unsigned long)description.format, (unsigned long)surface->Data);
	}
	*usable = entry->texture != NULL;
	return entry->texture;
}

/* ---------- the end of a frame */

void xd3d_textures_frame_end(void)
{
	unsigned long bucket;

	frame++;
	if (entry_count < MANY_COPIES)
		return;
	/* a part of the table each frame */
	for (bucket = frame % 16; bucket < BUCKETS; bucket += 16)
	{
		struct texture_entry **link = &buckets[bucket];

		while (*link)
		{
			struct texture_entry *entry = *link;

			if (!entry->target && frame - entry->used_frame > UNUSED_FRAMES)
			{
				*link = entry->next;
				if (entry->texture)
					pcd3d_texture_delete(entry->texture);
				free(entry);
				entry_count--;
				continue;
			}
			link = &entry->next;
		}
	}
}
