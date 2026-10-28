/*
DOS_CE.C

Halo Custom Edition's maps (Halo PC's, cache version 609) in the DOS and
Windows 95 builds. They are played as ChupathingyCE plays them (HALO_CUSTOM_EDITION:
its port/linux/game/ce_*.c read them and make them the Xbox's as they load:
gbxmodels made models, a structure BSP's vertices compressed, the resource
maps' tags copied in, shader types put back, the HUD's placements halved)
from a folder beside MAPS:

  MAPS_CE\<name>.map, with Custom Edition's bitmaps.map, sounds.map and loc.map,

each played as <name>@ce (the multiplayer page lists them after the Xbox's,
marked [CE]; testing: -play bloodgulch@ce). The Xbox's maps in MAPS are still
needed: the menus are ui.map's.

A Custom Edition map's tags are linked to a tag cache at 0x40440000 (23 MB)
and each of its structure BSPs to the top of it. DOS has no memory there
(DOS/32A hands out the machine's own addresses, below 64 MB on most), and
the maps' tags cannot be moved as the Xbox's are (port/support/xbox_map_rebase.c
knows the retail maps' layouts, not any map's), so on both systems they stay
where they were linked, in a window: the tags
(and what is copied and made after them) are read into one block of the
program's own and the loaded BSP into another (dos_game.c's), and every
address in the window is found through a table of 64 KB pages, each moved
by its block's displacement (xbox_address.h's HALO_DOS branch: xbox_pointer
and xbox_address; tools/dos_hooks.py custom_edition). The tags' block is no
larger than its check found the map needs (ce_map_checks.c: its tags, the
resources copied in after them, the models' parts and strips, the BSP's
vertex buffers), not the 23 MB of Halo PC's tag cache. With no Custom
Edition map loaded the window is empty and the translation is the identity.

An address outside the window is never moved, so the program's own blocks
must not lie inside it: on Windows the range is reserved at start-up
(w95_system.c); on DOS the program's memory is the machine's, below it.

The pixels of a bitmap and the samples of a sound may be in the resource
maps (bitmaps.map, sounds.map), which ce_resources.c names by tag
(ce_resources_file_for_tag): dos_ce_read reads from there.
*/

#include "cseries.h"
#include "cseries/cseries_windows.h"

#include "cache/cache_files.h"
#include "tag_files/tag_groups.h"

#include "halo_map_families.h"
#include "halo_system.h"
#include "dos_game.h"

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- constants */

#define WINDOW_BASE HALO_DOS_CE_WINDOW_BASE
#define WINDOW_SIZE HALO_DOS_CE_WINDOW_SIZE
#define PAGE_SHIFT HALO_DOS_CE_WINDOW_PAGE_SHIFT
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#define PAGE_COUNT (WINDOW_SIZE >> PAGE_SHIFT)
/* (Halo PC's tag cache: cache_files.c's CE_TAG_CACHE_SIZE) */
#define CE_TAG_CACHE_SIZE 0x01700000UL
/* what the tags' block takes beyond what the check counted (allocations are
aligned to 16 bytes, which the check counts too: a margin) */
#define TAGS_SLACK 0x20000UL
/* the room a check's image of the tags has for the resources copied in
after them (the shipped maps' take about 0.6 MB): on DOS, whose memory is
short, 4 MB; on Windows, 16 MB */
#define CHECK_RESOURCE_ROOM_DOS 0x00400000UL
#define CHECK_RESOURCE_ROOM_WINDOWS 0x01000000UL
#define CHECK_RESOURCE_ROOM (system_memory_total() ? CHECK_RESOURCE_ROOM_DOS : CHECK_RESOURCE_ROOM_WINDOWS)
/* the Custom Edition maps the multiplayer page lists, at most */
#define MAXIMUM_LISTED_MAPS 64

enum
{
	_region_tags,
	_region_structure_bsp,
	NUMBER_OF_REGIONS
};

/* ---------- structures */

/* the head of a map's file (cache_files.c's struct cache_file_header, 0x800
bytes, whose fields this reads) */
struct dos_ce_map_header
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
};

#define MAP_HEADER_SIZE 0x800

/* ---------- prototypes */

int dos_map_open_path(const char *path, void *header, unsigned long header_size);
int dos_map_read(unsigned long offset, void *buffer, unsigned long size);
void *dos_map_handle(void);
unsigned long dos_map_size(void);

boolean cache_file_header_verify(struct cache_file_header *header, char const *scenario_name, boolean fatal);
boolean cache_file_tags_are_ce(void);
boolean ce_map_check(HANDLE file, char const *map_name, long file_length, long tag_data_offset, long tag_data_size);
char const *ce_map_refusal_message(void);
HANDLE ce_resources_file_for_tag(long tag_index);
extern long ce_map_cache_version;

/* ---------- globals */

/* the window (xbox_address.h): how far from its base it reaches (0: no
Custom Edition map), and each page's displacement */
unsigned long dos_ce_window_span;
long dos_ce_window_delta[PAGE_COUNT];

/* the room above Halo PC's tag cache for the port's converted models and
level (ce_map_checks.h, platform.h): none here, the tags' block has it */
unsigned int platform_ce_conversion_room;

/* each page's region (NONE: none) */
static signed char page_regions[PAGE_COUNT];

static struct dos_ce_region
{
	unsigned long linked;
	unsigned long size;
	byte *host;
} regions[NUMBER_OF_REGIONS];

/* the tags' block (owned here; the BSP's is dos_game.c's) */
static byte *tags_block;

/* what the last map's check found (ce_map_checks.c): the lowest of its
BSPs, the end of the resources copied in after its tags, what its models
and the largest BSP's buffers take */
static struct
{
	boolean valid;
	unsigned long end_free;
	unsigned long next_free;
	unsigned long model_bytes;
	unsigned long bsp_bytes;
} checked;

/* whether the map opened last is a Custom Edition map, and its name */
static boolean ce_map_opened;
static char ce_map_name[64];

/* the maps the multiplayer page lists: their folder (<file>@ce) and name */
static struct
{
	char folder[48];
	char name[48];
} listed[MAXIMUM_LISTED_MAPS];
static int listed_count;

/* ---------- the libc's allocator (in the game's units malloc and free are
cseries.h's debug_malloc and debug_free) */

static void *raw_malloc(size_t size)
{
	return (malloc)(size);
}

static void raw_free(void *pointer)
{
	(free)(pointer);
}

/* ---------- the window */

static void window_rebuild(
	void)
{
	unsigned long span = 0;
	unsigned long page;
	int region;

	for (page = 0; page < PAGE_COUNT; page++)
	{
		dos_ce_window_delta[page] = 0;
		page_regions[page] = NONE;
	}
	for (region = 0; region < NUMBER_OF_REGIONS; region++)
	{
		struct dos_ce_region const *r = &regions[region];
		unsigned long first, last;

		if (!r->host || !r->size)
			continue;
		first = (r->linked - WINDOW_BASE) >> PAGE_SHIFT;
		last = (r->linked + r->size - 1 - WINDOW_BASE) >> PAGE_SHIFT;
		for (page = first; page <= last && page < PAGE_COUNT; page++)
		{
			if (page_regions[page] != NONE)
			{
				char line[128];

				snprintf(line, sizeof(line), "Custom Edition: page %08lx is in two blocks (%d and %d)",
					WINDOW_BASE + (page << PAGE_SHIFT), page_regions[page], region);
				system_log(line);
			}
			dos_ce_window_delta[page] = (long)((unsigned long)r->host - r->linked);
			page_regions[page] = (signed char)region;
		}
		if ((last + 1) << PAGE_SHIFT > span)
			span = (last + 1) << PAGE_SHIFT;
	}
	dos_ce_window_span = span > WINDOW_SIZE ? WINDOW_SIZE : span;
}

/* a region of the window mapped to a block (linked to [linked, linked +
size), all in the window), or none (host NULL) */
static void region_set(
	int region,
	unsigned long linked,
	unsigned long size,
	byte *host)
{
	if (host && (linked < WINDOW_BASE || linked - WINDOW_BASE >= WINDOW_SIZE ||
		size > WINDOW_SIZE - (linked - WINDOW_BASE)))
	{
		char line[128];

		snprintf(line, sizeof(line), "Custom Edition: %lu bytes at %08lx are not in the window", size, linked);
		system_log(line);
		host = NULL;
	}
	regions[region].linked = host ? linked : 0;
	regions[region].size = host ? size : 0;
	regions[region].host = host;
	window_rebuild();
}

/* xbox_address.h: a pointer into one of the blocks, as the address it is
linked to; any other as it is */
void *dos_ce_window_address(
	void const *pointer)
{
	unsigned long value = (unsigned long)pointer;
	int region;

	for (region = 0; region < NUMBER_OF_REGIONS; region++)
	{
		struct dos_ce_region const *r = &regions[region];

		if (r->host && value - (unsigned long)r->host < r->size)
			return (void *)(r->linked + (value - (unsigned long)r->host));
	}
	return (void *)pointer;
}

/* size bytes at a linked address, if one block holds them all; else NULL
(ce_map_checks.c, ce_functions.c: the map's own tag cache) */
void *dos_ce_window_pointer(
	unsigned long address,
	unsigned long size)
{
	unsigned long offset = address - WINDOW_BASE;
	struct dos_ce_region const *r;
	int region;

	if (offset >= dos_ce_window_span)
		return NULL;
	region = page_regions[offset >> PAGE_SHIFT];
	if (region == NONE)
		return NULL;
	r = &regions[region];
	offset = address - r->linked;
	if (offset > r->size || size > r->size - offset)
		return NULL;
	return r->host + offset;
}

/* a block of size bytes none of whose bytes lies in the window (on
Windows the window's range is reserved, so the first is; elsewhere the
memory is below it) */
static byte *block_outside_window(
	unsigned long size)
{
	byte *refused[8];
	byte *block = NULL;
	int count = 0;

	while (count < NUMBEROF(refused))
	{
		block = raw_malloc(size);
		if (!block || (unsigned long)block + size <= WINDOW_BASE || (unsigned long)block >= WINDOW_BASE + WINDOW_SIZE)
			break;
		refused[count++] = block;
		block = NULL;
	}
	while (count > 0)
		raw_free(refused[--count]);
	return block;
}

/* ---------- the map's memory (cache_files.c, HALO_DOS) */

/* the window emptied and the tags' block freed: a new map loads
(dos_game.c's dos_tag_cache_reserve, which frees the BSP's block) */
void dos_ce_release(
	void)
{
	regions[_region_tags].host = NULL;
	regions[_region_tags].size = 0;
	regions[_region_structure_bsp].host = NULL;
	regions[_region_structure_bsp].size = 0;
	window_rebuild();
	if (tags_block)
	{
		raw_free(tags_block);
		tags_block = NULL;
	}
}

/* whether the map opened last (the one loading, or loaded) is a Custom
Edition map: its tags are this file's, not dos_game.c's block */
boolean dos_ce_opened(
	void)
{
	return ce_map_opened;
}

/* ce_map_checks.c: what a map's check found it needs */
void dos_ce_checked(
	unsigned long end_free,
	unsigned long next_free,
	unsigned long model_bytes,
	unsigned long bsp_bytes)
{
	checked.valid = TRUE;
	checked.end_free = end_free;
	checked.next_free = next_free;
	checked.model_bytes = model_bytes;
	checked.bsp_bytes = bsp_bytes;
}

/* ce_map_checks.c: the size of a check's image of the tag cache: its tags
and room for the resources copied in after them, not the whole 23 MB */
unsigned long dos_ce_check_image_size(
	unsigned long tag_data_size)
{
	unsigned long size = tag_data_size + CHECK_RESOURCE_ROOM;

	return size > CE_TAG_CACHE_SIZE ? CE_TAG_CACHE_SIZE : (size + 4095) & ~4095UL;
}

/* cache_files.c: the tags' block (its address the tags were linked to:
0x40440000), as large as the check found the tags, the resources copied
in after them, the models' parts and strips and the BSPs' buffers take;
NULL (said why) if there is no memory for it */
void *dos_ce_tag_cache_reserve(
	unsigned long tag_data_size)
{
	unsigned long size, limit;
	char line[192];

	dos_ce_release();
	/* (no further than the lowest BSP's page: a page is in one block) */
	limit = checked.valid ? (checked.end_free & ~(PAGE_SIZE - 1)) - WINDOW_BASE : CE_TAG_CACHE_SIZE;
	if (checked.valid && checked.next_free > WINDOW_BASE)
		size = (checked.next_free - WINDOW_BASE) + checked.model_bytes + checked.bsp_bytes + TAGS_SLACK;
	else
		size = tag_data_size + CHECK_RESOURCE_ROOM + 0x00800000UL;
	if (size > limit)
		size = limit;
	size = (size + 4095) & ~4095UL;
	if (size < tag_data_size)
	{
		snprintf(line, sizeof(line), "The Custom Edition map's tags (%lu KB) do not fit below its level.",
			tag_data_size / 1024);
		system_error(line);
		return NULL;
	}
	tags_block = block_outside_window(size);
	if (!tags_block)
	{
		snprintf(line, sizeof(line), "There is not enough memory for the Custom Edition map's %lu KB of tags (%lu KB "
			"of %lu KB free).", size / 1024, system_memory_free() / 1024, system_memory_total() / 1024);
		system_error(line);
		return NULL;
	}
	memset(tags_block + tag_data_size, 0, size - tag_data_size);
	region_set(_region_tags, WINDOW_BASE, size, tags_block);
	snprintf(line, sizeof(line), "map: Custom Edition tags %lu KB (%lu KB of tag data) at %p, seen at %08lX",
		size / 1024, tag_data_size / 1024, tags_block, WINDOW_BASE);
	system_log(line);
	return tags_block;
}

/* cache_files.c: where the resources and the models' conversion may go,
no further than the tags' block */
unsigned long dos_ce_tag_cache_end(
	unsigned long end_free)
{
	unsigned long end = regions[_region_tags].linked + regions[_region_tags].size;

	return regions[_region_tags].host && end < end_free ? end : end_free;
}

/* cache_files.c: a structure BSP read into dos_game.c's block, seen where
it was linked */
void dos_ce_structure_bsp_mapped(
	void *block,
	unsigned long linked_address,
	unsigned long size)
{
	char line[128];

	region_set(_region_structure_bsp, linked_address, size, block);
	snprintf(line, sizeof(line), "map: Custom Edition structure BSP %lu KB at %p, seen at %08lX", size / 1024, block,
		linked_address);
	system_log(line);
}

/* ---------- the map's file (dos_game.c's cache_file_open) */

/* whether a map's name is a Custom Edition map's (<file>@ce) */
int dos_ce_map_name_is(
	char const *map_name)
{
	return map_name && map_family_parse(map_name, NULL, 0) == _map_family_custom_edition;
}

/* the map opened (its tags read by cache_files.c), checked first as
ChupathingyCE checks it (ce_map_checks.c); a map that is not there, or
that the check refuses, is said and the game stops */
boolean dos_ce_map_open(
	char const *map_name,
	struct cache_file_header *cache_header)
{
	struct dos_ce_map_header *header = (struct dos_ce_map_header *)cache_header;
	char file[64], path[256], text[384];
	short family = map_family_parse(map_name, file, sizeof(file));

	ce_map_opened = FALSE;
	checked.valid = FALSE;
	if (!map_family_find(family, file, path, sizeof(path)))
	{
		snprintf(text, sizeof(text), "The Custom Edition map %s.map is not in the MAPS_CE folder beside the program "
			"(copy the maps folder of Halo Custom Edition there, with its bitmaps.map, sounds.map and loc.map).", file);
		system_error(text);
		return FALSE;
	}
	if (!dos_map_open_path(path, header, MAP_HEADER_SIZE))
	{
		snprintf(text, sizeof(text), "The Custom Edition map %s could not be read.", path);
		system_error(text);
		return FALSE;
	}
	/* (a map built by Invader may give its length as 0: its file's) */
	if (!header->file_length)
		header->file_length = (long)dos_map_size();
	if (!cache_file_header_verify(cache_header, path, FALSE) || header->version != map_family_cache_version(family))
	{
		snprintf(text, sizeof(text), "%s is not a Halo Custom Edition map this game can read.", path);
		system_error(text);
		return FALSE;
	}
	ce_map_cache_version = header->version;
	if (!ce_map_check(dos_map_handle(), map_name, header->file_length, header->tag_data_offset,
		header->tag_data_size))
	{
		snprintf(text, sizeof(text), "%s", ce_map_refusal_message());
		system_error(text[0] ? text : "The Custom Edition map can't be played: it is damaged or unsupported.");
		return FALSE;
	}
	ce_map_opened = TRUE;
	snprintf(ce_map_name, sizeof(ce_map_name), "%s", map_name);
	snprintf(text, sizeof(text), "map: Custom Edition %s (%s, build %.32s): tags %ld KB, resources to %08lX, "
		"models %lu KB, level buffers %lu KB", map_name, path, header->build, (long)header->tag_data_size / 1024,
		checked.next_free, checked.model_bytes / 1024, checked.bsp_bytes / 1024);
	system_log(text);
	return TRUE;
}

/* dos_game.c: an Xbox map opened */
void dos_ce_map_closed(
	void)
{
	ce_map_opened = FALSE;
	checked.valid = FALSE;
}

/* ce_resources.c: whether the map's Ogg Vorbis sounds are decoded as it
loads (into CACHE\ce_sounds.pcm): not with the sound off (setting sound) */
int dos_ce_sounds_wanted(
	void)
{
	extern long dos_setting_integer(const char *name, long default_value);

	return dos_setting_integer("sound", 1) != 0;
}

/* ce_resources.c: the map's own file (its sounds' samples are read from it as
its tags load), or NULL */
HANDLE cache_files_ce_map_file(
	void)
{
	return ce_map_opened ? (HANDLE)dos_map_handle() : NULL;
}

/* size bytes at offset of the file a tag's pixels or samples are in: a
Custom Edition map's resource map (bitmaps.map, sounds.map) where
ce_resources.c says so, else the map's own */
int dos_ce_read(
	long tag_index,
	unsigned long offset,
	void *buffer,
	unsigned long size)
{
	HANDLE file = tag_index != NONE && cache_file_tags_are_ce() ? ce_resources_file_for_tag(tag_index) : NULL;
	DWORD done = 0;

	if (!file)
		return dos_map_read(offset, buffer, size);
	return SetFilePointer(file, (LONG)offset, NULL, FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
		ReadFile(file, buffer, size, &done, NULL) && done == size;
}

/* ---------- the maps the multiplayer page lists (dos_game.c) */

static struct
{
	const char *file;
	const char *name;
} const stock_names[] =
{
	{ "beavercreek", "BATTLE CREEK" },
	{ "bloodgulch", "BLOOD GULCH" },
	{ "boardingaction", "BOARDING ACTION" },
	{ "carousel", "DERELICT" },
	{ "chillout", "CHILL OUT" },
	{ "damnation", "DAMNATION" },
	{ "dangercanyon", "DANGER CANYON" },
	{ "deathisland", "DEATH ISLAND" },
	{ "gephyrophobia", "GEPHYROPHOBIA" },
	{ "hangemhigh", "HANG 'EM HIGH" },
	{ "icefields", "ICE FIELDS" },
	{ "infinity", "INFINITY" },
	{ "longest", "LONGEST" },
	{ "prisoner", "PRISONER" },
	{ "putput", "CHIRON TL-34" },
	{ "ratrace", "RAT RACE" },
	{ "sidewinder", "SIDEWINDER" },
	{ "timberland", "TIMBERLAND" },
	{ "wizard", "WIZARD" },
};

static void map_found(
	char const *file,
	void *context)
{
	const char *name = NULL;
	char upper[40];
	size_t i;

	(void)context;
	if (listed_count == MAXIMUM_LISTED_MAPS || strlen(file) + 3 >= sizeof(listed[0].folder))
		return;
	for (i = 0; i < NUMBEROF(stock_names) && !name; i++)
	{
		if (!_stricmp(file, stock_names[i].file))
			name = stock_names[i].name;
	}
	if (!name)
	{
		for (i = 0; file[i] && i < sizeof(upper) - 1; i++)
			upper[i] = file[i] == '_' ? ' ' : (char)(file[i] >= 'a' && file[i] <= 'z' ? file[i] - 32 : file[i]);
		upper[i] = 0;
		name = upper;
	}
	snprintf(listed[listed_count].folder, sizeof(listed[0].folder), "%s@ce", file);
	snprintf(listed[listed_count].name, sizeof(listed[0].name), "%.40s [CE]", name);
	listed_count++;
}

/* the Custom Edition multiplayer maps in MAPS_CE added after count maps
(at most maximum in all), by name; the new count */
int dos_ce_list_multiplayer_maps(
	struct dos_level *maps,
	int count,
	int maximum)
{
	int first = count;
	char line[96];

	listed_count = 0;
	map_family_list(_map_family_custom_edition, map_found, NULL);
	for (int i = 0; i < listed_count && count < maximum; i++)
	{
		maps[count].folder = listed[i].folder;
		maps[count].name = listed[i].name;
		count++;
	}
	/* (by name) */
	for (int i = first + 1; i < count; i++)
	{
		struct dos_level map = maps[i];
		int j = i;

		while (j > first && strcmp(maps[j - 1].name, map.name) > 0)
		{
			maps[j] = maps[j - 1];
			j--;
		}
		maps[j] = map;
	}
	snprintf(line, sizeof(line), "maps: %d Custom Edition maps in MAPS_CE", count - first);
	system_log(line);
	return count;
}

/* ---------- the names map_families.c reads (ChupathingyCE's
port/linux/src/map_archive.c and map_file_hash.c, without the map downloads
and whole-file hashes those are for: a map is found by its own name) */

#define HASHED_NAME_DIGITS 8

static const char *const stock_maps[] =
{
	"beavercreek", "bloodgulch", "boardingaction", "carousel", "chillout", "damnation", "dangercanyon", "deathisland",
	"gephyrophobia", "hangemhigh", "icefields", "infinity", "longest", "prisoner", "putput", "ratrace", "sidewinder",
	"timberland", "wizard", "a10", "a30", "a50", "b30", "b40", "c10", "c20", "c40", "d20", "d40",
};
static const char *const stock_files[] = { "ui", "bitmaps", "sounds", "loc" };

int map_archive_hashed_name(const char *name, char *base, int base_size, unsigned int *prefix)
{
	size_t length, index;
	unsigned int value = 0;

	if (!name)
		return 0;
	length = strlen(name);
	if (length < HASHED_NAME_DIGITS + 2 || name[length - HASHED_NAME_DIGITS - 1] != '_')
		return 0;
	for (index = length - HASHED_NAME_DIGITS; index < length; index++)
	{
		int character = name[index] >= 'A' && name[index] <= 'Z' ? name[index] + 32 : name[index];

		if (character >= '0' && character <= '9')
			value = (value << 4) | (unsigned int)(character - '0');
		else if (character >= 'a' && character <= 'f')
			value = (value << 4) | (unsigned int)(character - 'a' + 10);
		else
			return 0;
	}
	if (base && base_size > 0)
		snprintf(base, (size_t)base_size, "%.*s", (int)(length - HASHED_NAME_DIGITS - 1), name);
	if (prefix)
		*prefix = value;
	return 1;
}

void map_archive_display_name(const char *name, char *display, int size)
{
	if (size <= 0)
		return;
	if (!map_archive_hashed_name(name, display, size, NULL))
	{
		snprintf(display, (size_t)size, "%s", name ? name : "");
		return;
	}
	for (int index = 0; display[index]; index++)
		display[index] = display[index] >= 'A' && display[index] <= 'Z' ? (char)(display[index] + 32) : display[index];
}

int map_archive_stock_name(const char *name)
{
	char base[64];
	size_t index;

	if (!name || !*name)
		return 0;
	map_archive_display_name(name, base, (int)sizeof(base));
	for (index = 0; base[index]; index++)
		base[index] = base[index] >= 'A' && base[index] <= 'Z' ? (char)(base[index] + 32) : base[index];
	for (index = 0; index < NUMBEROF(stock_maps); index++)
	{
		if (!strcmp(base, stock_maps[index]))
			return 1;
	}
	for (index = 0; index < NUMBEROF(stock_files); index++)
	{
		if (!strcmp(base, stock_files[index]))
			return 2;
	}
	return 0;
}

unsigned int map_archive_header_checksum(const unsigned char *header)
{
	return (unsigned int)header[0x64] | (unsigned int)header[0x65] << 8 | (unsigned int)header[0x66] << 16 |
		(unsigned int)header[0x67] << 24;
}

int map_archive_version_matches(unsigned int version, unsigned int checksum, unsigned int hash_version)
{
	return !version || version == hash_version || version == checksum;
}

/* (no whole-file hash: none matches a hashed name) */
int map_file_hash_prefix(const char *path, unsigned int *prefix)
{
	(void)path;
	*prefix = 0;
	return 0;
}

int map_file_hash_version(const char *path, unsigned int *version)
{
	(void)path;
	*version = 0;
	return 0;
}
