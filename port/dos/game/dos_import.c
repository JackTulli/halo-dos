/*
DOS_IMPORT.C

Firefight's Covenant on a multiplayer map (dos_firefight.c): the Xbox's
multiplayer maps hold no grunts, jackals or elites, so a campaign map's tags
join the map being played when it loads, as the N64 port's merge.py joins
them offline (tools/n64/merge.py in halo-64).

cache_files.c (HALO_DOS) calls dos_tags_import right after the map's tag
data is read and its addresses moved. With Firefight chosen, the first
campaign map in the maps folder that has the three Covenant bipeds (a30,
then the others) is opened beside it (xdk_kernel.c's second map file), its
whole tag data read into a block of its own and its addresses moved there
(port/linux/src/xbox_map_rebase.c), each of its tag references renumbered
on the way. Its tags then follow the map's own in one tag instance array:

- a tag the map has too (the same group and name: the plasma weapons, the
  globals, most effects) is the map's own; the campaign map's references
  to it point there;
- its scenario and structure BSPs are left out (references to them NONE);
- every other tag is added after the map's, numbered as the map numbers its
  tags (salt 0xE174 + the index); its tag references are renumbered by the
  address walk, and the plain tag indices of its objects' (and weapons')
  predicted resources here.

Its bitmaps' pixels stay in its own file (dos_import_read_pixels: the
textures read them there). The block lives until the next map loads.
*/

#include "cseries.h"

#include "cache/predicted_resources.h"
#include "items/weapon_definitions.h"
#include "objects/object_definitions.h"
#include "sound/sound_definitions.h"

#include "halo_system.h"
#include "dos_game.h"

#include "../../support/halo_memory_window.h"
#include "../../support/xbox_map_rebase.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the platform's (port/dos/xdk/xdk_kernel.c) */
int dos_import_map_open(const char *map_name, void *header, unsigned long header_size);
int dos_import_map_read(unsigned long offset, void *buffer, unsigned long size);
void dos_import_map_close(void);

/* cache_files.c's (HALO_DOS): where the tag data was linked to */
#define LINKED_TAG_DATA_ADDRESS (HALO_XBOX_WINDOW_BASE + 0x003A6000UL)

/* the libc's own allocator (in the game's units malloc and free are
cseries.h's debug_malloc and debug_free) */
static void *raw_malloc(size_t size) { return (malloc)(size); }
static void raw_free(void *pointer) { (free)(pointer); }

/* ---------- the cache file's structures (cache_files.c's) */

struct instance
{
	long group_tag;
	long parent_group_tags[2];
	long tag_index;
	char *name;
	void *base_address;
	unsigned long unused[2];
};

struct tag_header
{
	struct instance *tag_instances;
	long scenario_tag_index;
	unsigned long checksum;
	long tag_count;
	long vertex_buffer_count;
	void *vertex_buffers;
	long index_buffer_count;
	void *index_buffers;
	unsigned long signature;
};

struct file_header
{
	unsigned long header_signature;
	long version;
	long file_length;
	byte reservedC[4];
	long tag_data_offset;
	long tag_data_size;
	byte reserved18[8];
	char name[0x20];
	char build[0x20];
	byte reserved60[4];
	unsigned long checksum;
	byte reserved68[0x794];
	unsigned long footer_signature;
};

typedef char import_instance_size_assert[sizeof(struct instance) == 0x20 ? 1 : -1];
typedef char import_file_header_size_assert[sizeof(struct file_header) == 0x800 ? 1 : -1];

#define TAG_HEADER_SIGNATURE 0x74616773 /* 'tags' */
#define HEADER_SIGNATURE 0x68656164 /* 'head' */
#define FOOTER_SIGNATURE 0x666F6F74 /* 'foot' */

/* the campaign maps the Covenant may come from, in turn */
static const char *const sources[] = { "a30", "a50", "b30", "b40", "c10", "c40", "d20", "a10", "c20", "d40" };

static const char *const covenant[] =
{
	"characters\\grunt\\grunt",
	"characters\\jackal\\jackal",
	"characters\\elite\\elite",
};

/* ---------- state */

static struct
{
	unsigned char *block;
	unsigned long size;
	struct instance *merged;
	/* the source's tags: each one's merged datum index (NONE: left out) */
	long *remap;
	long source_count;
	/* the map's own tags: those past them are the source's */
	long own_count;
	char source[16];
} imported;

/* ---------- small things */

static long datum_index(long absolute)
{
	return (long)((unsigned long)((0xE174 + absolute) & 0xFFFF) << 16) | absolute;
}

/* the names' hash table: the map's own tags by group and name */
#define HASH_SIZE 8192

static unsigned long hash_of(long group_tag, const char *name)
{
	unsigned long hash = 2166136261UL ^ (unsigned long)group_tag;

	for (; *name; name++)
	{
		char c = *name >= 'A' && *name <= 'Z' ? (char)(*name + 32) : *name;

		hash = (hash ^ (unsigned char)c) * 16777619UL;
	}
	return hash;
}

static void remap_reference(void *context, unsigned long *index)
{
	long value = (long)*index;
	long absolute = value & 0xFFFF;

	(void)context;
	if (value == NONE)
		return;
	*index = (unsigned long)(absolute < imported.source_count ? imported.remap[absolute] : NONE);
}

int dos_import_contains(const void *pointer);

/* an added object's predicted resources (plain tag indices: no tag
references, which the walk renumbers) */
static void remap_predicted(struct tag_block *block)
{
	struct predicted_resource *resources = (struct predicted_resource *)block->address;

	if (block->count <= 0 || !dos_import_contains(resources) ||
		!dos_import_contains(resources + block->count - 1))
	{
		return;
	}
	for (long i = 0; i < block->count; i++)
		remap_reference(NULL, (unsigned long *)&resources[i].tag_index);
}

void dos_tags_import_dispose(void)
{
	raw_free(imported.block);
	raw_free(imported.merged);
	raw_free(imported.remap);
	memset(&imported, 0, sizeof(imported));
	dos_import_map_close();
}

/* the texture glue's (dos_textures.c): whether a bitmap is one of the
source map's, whose pixels are in its file */
int dos_import_contains(const void *pointer)
{
	return imported.block && (const unsigned char *)pointer >= imported.block &&
		(const unsigned char *)pointer < imported.block + imported.size;
}

/* cache_file_read's (dos_game.c): whether a tag is one of the source map's,
whose data (a sound's samples, a bitmap's pixels) is in the source's file */
int dos_import_tag(long tag_index)
{
	return imported.merged && tag_index != NONE && (tag_index & 0xFFFF) >= imported.own_count;
}

/* a bitmap's pixels, a sound's samples: from the source map's file */
int dos_import_read(unsigned long offset, void *buffer, unsigned long size)
{
	return dos_import_map_read(offset, buffer, size);
}

/* an added sound's permutations: the tag they read their samples for
(sound_permutation's cache tag index, and its runtime one, which names it in
the sound cache's messages) is the source map's numbering; renumbered to
the merged tag, which cache_file_read then reads from the source's file
(dos_import_tag) */
static void renumber_sound(struct sound_definition *sound, long tag_index)
{
	struct sound_pitch_range *ranges = (struct sound_pitch_range *)sound->pitch_ranges.address;

	if (sound->pitch_ranges.count <= 0 || !dos_import_contains(ranges) ||
		!dos_import_contains(ranges + sound->pitch_ranges.count - 1))
	{
		return;
	}
	for (long r = 0; r < sound->pitch_ranges.count; r++)
	{
		struct sound_permutation *permutations = (struct sound_permutation *)ranges[r].permutations.address;

		if (ranges[r].permutations.count <= 0 || !dos_import_contains(permutations) ||
			!dos_import_contains(permutations + ranges[r].permutations.count - 1))
		{
			continue;
		}
		for (long i = 0; i < ranges[r].permutations.count; i++)
		{
			permutations[i].unknown2 = (unsigned long)tag_index;
			permutations[i].unknown3 = (unsigned long)tag_index;
		}
	}
}

/* ---------- the import */

static int source_has_covenant(const struct tag_header *header)
{
	int found = 0;

	for (long i = 0; i < header->tag_count; i++)
	{
		const struct instance *instance = &header->tag_instances[i];

		if (instance->group_tag != 'bipd' || !instance->name)
			continue;
		for (unsigned k = 0; k < NUMBEROF(covenant); k++)
		{
			if (!_stricmp(instance->name, covenant[k]))
				found++;
		}
	}
	return found >= (int)NUMBEROF(covenant);
}

/* cache_files.c's scenario_tags_load (HALO_DOS): the map's tags are in;
with Firefight, the Covenant's join them */
void dos_tags_import(void *tag_header_address, void **tag_instances)
{
	struct tag_header *header = (struct tag_header *)tag_header_address;
	struct tag_header *source;
	struct file_header file;
	short *hash = NULL;
	long own_count = header->tag_count, added = 0;
	char line[200], error[256];
	unsigned s;

	dos_tags_import_dispose();
	if (!dos_firefight_active())
		return;
	for (s = 0; s < NUMBEROF(sources); s++)
	{
		if (!dos_map_present(sources[s]) || !dos_import_map_open(sources[s], &file, sizeof(file)))
			continue;
		if (file.header_signature != HEADER_SIGNATURE || file.footer_signature != FOOTER_SIGNATURE ||
			file.tag_data_size <= 0 || file.tag_data_offset <= 0)
		{
			dos_import_map_close();
			continue;
		}
		imported.size = (unsigned long)file.tag_data_size;
		imported.block = raw_malloc(imported.size + 4);
		if (!imported.block)
		{
			system_log("firefight: no memory for the Covenant's map");
			dos_tags_import_dispose();
			return;
		}
		/* (aligned: the walk reads words) */
		if (((unsigned long)imported.block & 3) ||
			!dos_import_map_read((unsigned long)file.tag_data_offset, imported.block, imported.size))
		{
			dos_tags_import_dispose();
			continue;
		}
		source = (struct tag_header *)imported.block;
		if (source->signature != TAG_HEADER_SIGNATURE)
		{
			dos_tags_import_dispose();
			continue;
		}
		/* (its instances, from the address they were linked at: the walk
		below moves them) */
		{
			struct instance *instances = (struct instance *)(imported.block +
				((unsigned long)source->tag_instances - LINKED_TAG_DATA_ADDRESS));
			long count = source->tag_count;

			if ((unsigned long)source->tag_instances < LINKED_TAG_DATA_ADDRESS || count <= 0 ||
				(unsigned long)((unsigned char *)(instances + count) - imported.block) > imported.size)
			{
				dos_tags_import_dispose();
				continue;
			}
			imported.source_count = count;
			imported.remap = raw_malloc((size_t)count * sizeof(long));
			if (!imported.remap)
			{
				dos_tags_import_dispose();
				return;
			}
			snprintf(imported.source, sizeof(imported.source), "%s", sources[s]);

			/* the map's own tags by group and name */
			hash = raw_malloc(HASH_SIZE * sizeof(short));
			if (!hash)
			{
				dos_tags_import_dispose();
				return;
			}
			memset(hash, 0xFF, HASH_SIZE * sizeof(short));
			for (long i = 0; i < own_count; i++)
			{
				const struct instance *own = &header->tag_instances[i];
				unsigned long slot = hash_of(own->group_tag, own->name ? own->name : "") & (HASH_SIZE - 1);

				while (hash[slot] != -1)
					slot = (slot + 1) & (HASH_SIZE - 1);
				hash[slot] = (short)i;
			}
			/* each source tag: the map's own, left out, or added */
			for (long i = 0; i < count; i++)
			{
				const struct instance *instance = &instances[i];
				const char *name = (const char *)(imported.block + ((unsigned long)instance->name - LINKED_TAG_DATA_ADDRESS));
				unsigned long slot;
				long own = NONE;

				if ((unsigned long)instance->name < LINKED_TAG_DATA_ADDRESS ||
					(unsigned long)instance->name - LINKED_TAG_DATA_ADDRESS >= imported.size ||
					instance->group_tag == 'scnr' || instance->group_tag == 'sbsp')
				{
					imported.remap[i] = NONE;
					continue;
				}
				slot = hash_of(instance->group_tag, name) & (HASH_SIZE - 1);
				while (hash[slot] != -1)
				{
					const struct instance *candidate = &header->tag_instances[hash[slot]];

					if (candidate->group_tag == instance->group_tag && candidate->name &&
						!_stricmp(candidate->name, name))
					{
						own = candidate->tag_index;
						break;
					}
					slot = (slot + 1) & (HASH_SIZE - 1);
				}
				imported.remap[i] = own != NONE ? own : datum_index(own_count + added++);
			}
			raw_free(hash);
			hash = NULL;
			if (own_count + added > 32000)
			{
				system_log("firefight: too many tags to join");
				dos_tags_import_dispose();
				return;
			}

			/* its addresses moved to the block, its references renumbered */
			if (!xbox_map_rebase_tag_data_visiting(imported.block, imported.size, LINKED_TAG_DATA_ADDRESS,
				(long)((unsigned long)imported.block - LINKED_TAG_DATA_ADDRESS), remap_reference, NULL, error,
				sizeof(error)))
			{
				snprintf(line, sizeof(line), "firefight: %s's tags cannot be moved: %s", sources[s], error);
				system_log(line);
				dos_tags_import_dispose();
				continue;
			}

			/* the instances: the map's, then the added */
			instances = source->tag_instances;
			imported.merged = raw_malloc((size_t)(own_count + added) * sizeof(struct instance));
			if (!imported.merged)
			{
				dos_tags_import_dispose();
				return;
			}
			memcpy(imported.merged, header->tag_instances, (size_t)own_count * sizeof(struct instance));
			for (long i = 0; i < count; i++)
			{
				long merged = imported.remap[i];

				if (merged == NONE || (merged & 0xFFFF) < own_count)
					continue;
				imported.merged[merged & 0xFFFF] = instances[i];
				imported.merged[merged & 0xFFFF].tag_index = merged;
				if (instances[i].base_address && dos_import_contains(instances[i].base_address))
				{
					struct instance *instance = &instances[i];

					if (instance->group_tag == 'obje' || instance->parent_group_tags[0] == 'obje' ||
						instance->parent_group_tags[1] == 'obje')
					{
						remap_predicted(&((struct object_definition *)instance->base_address)->object.predicted_resources);
					}
					if (instance->group_tag == 'weap')
						remap_predicted(&((struct weapon_definition *)instance->base_address)->weapon.predicted_resources);
					if (instance->group_tag == 'snd!')
						renumber_sound((struct sound_definition *)instance->base_address, merged);
				}
			}
			header->tag_instances = imported.merged;
			header->tag_count = own_count + added;
			imported.own_count = own_count;
			*tag_instances = imported.merged;
		}
		snprintf(line, sizeof(line), "firefight: %ld of %s's %ld tags joined the map's %ld (%lu KB)", added,
			sources[s], imported.source_count, own_count, imported.size / 1024);
		system_log(line);
		if (!source_has_covenant(header))
			system_log("firefight: but no Covenant among them");
		return;
	}
	system_log("firefight: no campaign map with the Covenant in the maps folder (a30.map...)");
}
