#!/usr/bin/env python3
"""dos_hooks.py: the DOS and Windows 95 builds' hooks (HALO_DOS) in the game's
sources (the chupathingyce submodule, branch dos-port). Each is applied once:
running it again changes nothing. After moving the submodule to a newer game,
run it again and fix any hook whose surroundings changed.

    python tools/dos_hooks.py
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from hook import append, at_function_start, insert_after, insert_before, read, replace, write  # noqa: E402

NO_LIGHT = """#ifdef HALO_DOS_TRIMS
	/* the DOS and Windows 95 builds have no lighting at all: no light is
	made, so none is moved through the map's clusters every tick */
	return NONE;
#endif
"""
NO_PARTICLE_SYSTEM = """#ifdef HALO_DOS_TRIMS
	/* the DOS and Windows 95 builds draw no particles: none is made, so
	none is moved or collided every tick */
	return NONE;
#endif
"""


def lights_and_particles():
    L = "source/objects/object_lights.c"
    at_function_start(L, "light_new", NO_LIGHT)
    at_function_start(L, "light_new_unattached", NO_LIGHT)
    at_function_start(L, "light_particle", """#ifdef HALO_DOS_TRIMS
	/* the DOS and Windows 95 builds work out no light while playing (and
	draw no particles): no ray to the lightmap below */
	*light = *global_real_rgb_grey;
	*diffuse = *global_real_rgb_grey;
	(void)position;
	(void)block;
	return;
#endif
""")
    at_function_start(L, "lights_illumination_at_point", """#ifdef HALO_DOS_TRIMS
	/* the DOS and Windows 95 builds work out no light while playing: no ray
	to the lightmap below a unit, nor its lights */
	*color = *global_real_rgb_black;
	(void)point;
	(void)location;
	return;
#endif
""")
    at_function_start("source/effects/particles.c", "particle_new", """#ifdef HALO_DOS
	/* the DOS and Windows 95 builds' particles (port/dos/game/dos_particles.c):
	none made with the setting particles off, nor past particle_limit alive */
	extern int dos_particles_allowed(void);

	if (!dos_particles_allowed())
		return;
#endif
""")
    at_function_start("source/effects/particle_systems.c", "particle_system_new_unattached", NO_PARTICLE_SYSTEM)
    at_function_start("source/effects/particle_systems.c", "particle_system_new_attached", NO_PARTICLE_SYSTEM)
    at_function_start("source/effects/contrails.c", "contrail_new", NO_PARTICLE_SYSTEM)


def tags_and_state():
    at_function_start("source/tag_files/tag_groups.c", "verify_tag_reference", """#ifdef HALO_DOS
	/* (the DOS and Windows 95 builds: the reference's index, as the
	cache's tags are linked; no search by name each frame) */
	return reference->index;
#endif
""")
    replace("source/saved games/game_state_xbox.c", """	xbox_game_state_globals.buffer_allocated = TRUE;
#ifdef HALO_ADDRESS_TRANSLATED
	xbox_game_state_globals.buffer = xbox_pointer(address);
#else""", """	xbox_game_state_globals.buffer_allocated = TRUE;
#if defined(HALO_DOS)
	/* (the DOS and Windows 95 builds' game state is an ordinary allocation:
	port/dos/game/dos_game.c) */
	xbox_game_state_globals.buffer = result;
#elif defined(HALO_ADDRESS_TRANSLATED)
	xbox_game_state_globals.buffer = xbox_pointer(address);
#else""")
    replace("port/linux/src/platform.h", """#ifndef __HALO_LINUX_PLATFORM_H
#define __HALO_LINUX_PLATFORM_H
""", """#if defined(HALO_DOS)
/* the DOS and Windows 95 builds' platform layer (port/dos/xdk/platform.h,
in the console repository beside the game) stands in for this header */
#include "../../../../port/dos/xdk/platform.h"
#else
#ifndef __HALO_LINUX_PLATFORM_H
#define __HALO_LINUX_PLATFORM_H
""")
    text = read("port/linux/src/platform.h")
    if not text.rstrip().endswith("#endif /* HALO_DOS */"):
        write("port/linux/src/platform.h", text.rstrip() + "\n#endif /* HALO_DOS */\n")


def cache_files():
    C = "source/cache/cache_files.c"
    insert_before(C, "static struct cache_file_globals cache_file_globals = { 0 };", """#ifdef HALO_DOS
/* The DOS and Windows 95 builds keep no tag cache of the Xbox's size (22
MB): the tag data is read into a block of its own size and each structure
BSP into another (port/dos/game/dos_game.c), wherever the heap has them,
and every address in them moves there as they are read
(port/support/xbox_map_rebase.c), so that the game's pointers are this
build's own. */
#include "halo_memory_window.h"
#include "xbox_map_rebase.h"
void *dos_tag_cache_reserve(unsigned long size);
void *dos_structure_bsp_reserve(unsigned long size);
void dos_tags_import(void *tag_header, void **tag_instances);
#define DOS_LINKED_TAG_CACHE_ADDRESS (HALO_XBOX_WINDOW_BASE + 0x003A6000UL)
#define DOS_TAG_DATA_DELTA ((long)((unsigned long)physical_memory_get_tag_cache_base_address() - \\
	DOS_LINKED_TAG_CACHE_ADDRESS))
#endif

""")
    replace(C, """		tag_cache_base_address = physical_memory_get_tag_cache_base_address();
#ifdef HALO_CUSTOM_EDITION
		/* port: a Custom Edition map's tags, in their own tag cache */""", """#ifdef HALO_DOS
		tag_cache_base_address = dos_tag_cache_reserve(cache_file_globals.header.tag_data_size);
#else
		tag_cache_base_address = physical_memory_get_tag_cache_base_address();
#endif
#ifdef HALO_CUSTOM_EDITION
		/* port: a Custom Edition map's tags, in their own tag cache */""")
    replace(C, """			csmemset(tag_cache_base_address, 0xCD, 0x01600000);
			cache_file_read(""", """#ifndef HALO_DOS
			csmemset(tag_cache_base_address, 0xCD, 0x01600000);
#endif
			cache_file_read(""")
    insert_before(C, """			if (!cache_file_tag_header_verify(
				tag_cache_base_address,""", """#ifdef HALO_DOS
			/* (its addresses moved to its block before anything reads them) */
			{
				char rebase_error[256];

				if (!xbox_map_rebase_tag_data(
					tag_cache_base_address,
					cache_file_globals.header.tag_data_size,
					DOS_LINKED_TAG_CACHE_ADDRESS,
					DOS_TAG_DATA_DELTA,
					rebase_error,
					sizeof(rebase_error)))
				{
					error(_error_silent, "the map '%s' cannot be moved to this build's memory: %s", scenario_name,
						rebase_error);
					cache_file_close();

					return NONE;
				}
			}
#endif
""")
    insert_after(C, """			global_tag_instances = xbox_pointer(cache_file_globals.tag_header->tag_instances);
""", """#ifdef HALO_DOS
			/* Firefight's Covenant: a campaign map's tags join these
			(port/dos/game/dos_import.c) */
			dos_tags_import(cache_file_globals.tag_header, (void **)&global_tag_instances);
#endif
""")
    replace(C, """	tag_cache_base_address = physical_memory_get_tag_cache_base_address();
#ifdef HALO_CUSTOM_EDITION
	/* port: (not a Custom Edition map's: its tags are in a tag cache of their
	own, and may be more than the Xbox's holds) */
	if (!cache_file_is_ce)
#endif
	csmemset(
		tag_cache_base_address + cache_file_globals.header.tag_data_size,
		0xCD,
		0x01600000 - cache_file_globals.header.tag_data_size);""", """#ifdef HALO_DOS
	/* (in a block of its own: see DOS_TAG_DATA_DELTA; a Custom Edition map's
	is seen at the address it was linked to, port/dos/game/dos_ce.c) */
	tag_cache_base_address = dos_structure_bsp_reserve(reference->file_size);
#ifdef HALO_CUSTOM_EDITION
	if (cache_file_is_ce)
		dos_ce_structure_bsp_mapped(tag_cache_base_address, (unsigned long)reference->base_address,
			reference->file_size);
#endif
#else
	tag_cache_base_address = physical_memory_get_tag_cache_base_address();
#ifdef HALO_CUSTOM_EDITION
	/* port: (not a Custom Edition map's: its tags are in a tag cache of their
	own, and may be more than the Xbox's holds) */
	if (!cache_file_is_ce)
#endif
	csmemset(
		tag_cache_base_address + cache_file_globals.header.tag_data_size,
		0xCD,
		0x01600000 - cache_file_globals.header.tag_data_size);
#endif""")
    replace(C, """			reference->file_size,
			xbox_pointer(reference->base_address),
			&read_complete,
			TRUE);""", """			reference->file_size,
#ifdef HALO_DOS
			tag_cache_base_address,
#else
			xbox_pointer(reference->base_address),
#endif
			&read_complete,
			TRUE);""")
    replace(C, """		structure_bsp_header = xbox_pointer(reference->base_address);
#ifdef HALO_BIG_ENDIAN""", """#ifdef HALO_DOS
		structure_bsp_header = (void *)tag_cache_base_address;
		/* its own addresses moved to its block, its tag references' names
		(in the tag data) as the tag data's were (not a Custom Edition map's:
		seen where it was linked) */
		if (
#ifdef HALO_CUSTOM_EDITION
			!cache_file_is_ce &&
#endif
			read_complete == TRUE && structure_bsp_header->signature == CACHE_FILE_STRUCTURE_BSP_HEADER_SIGNATURE)
		{
			char rebase_error[256];
			unsigned long linked_address = (unsigned long)reference->base_address - DOS_TAG_DATA_DELTA;

			if (!xbox_map_rebase_structure_bsp_apart(
				tag_cache_base_address,
				reference->file_size,
				linked_address,
				(long)((unsigned long)tag_cache_base_address - linked_address),
				DOS_TAG_DATA_DELTA,
				rebase_error,
				sizeof(rebase_error)))
			{
				error(_error_silent, "a structure bsp of the map '%s' cannot be moved to this build's memory: %s",
					cache_file_globals.header.name, rebase_error);

				return FALSE;
			}
		}
#else
		structure_bsp_header = xbox_pointer(reference->base_address);
#endif
#ifdef HALO_BIG_ENDIAN""")


def custom_edition():
    """Halo Custom Edition's maps (Halo PC's, cache version 609: maps_ce,
    <name>@ce), which ChupathingyCE converts to the Xbox's layouts as they
    load (port/linux/game/ce_*.c, HALO_CUSTOM_EDITION). Their tags are linked
    to a tag cache at 0x40440000 and their structure BSPs to its top, where
    neither DOS nor a Windows 95 process can have memory: the DOS and
    Windows 95 builds keep them in blocks of their own and see them through a
    window of 64 KB pages at the addresses they were linked to
    (port/dos/game/dos_ce.c), which xbox_pointer and xbox_address translate.
    Everything else (an Xbox map's tags, which are moved where they are read:
    cache_files) is outside the window and reads as it always did."""
    X = "source/cseries/xbox_address.h"
    replace(X, """#if defined(HALO_64BIT) || defined(HALO_WINDOW_REBASE)
#define HALO_ADDRESS_TRANSLATED 1""", """#if defined(HALO_64BIT) || defined(HALO_WINDOW_REBASE) || defined(HALO_DOS)
#define HALO_ADDRESS_TRANSLATED 1""")
    insert_before(X, """#else

#define XPTR(type) type *
#define XBOX_NULL NULL
#define xbox_pointer(address) ((void *)(address))""", r"""#elif defined(HALO_DOS)

/* The DOS and Windows 95 builds (halo-dos tools/dos_hooks.py
custom_edition): a pointer is the program's own (an Xbox map's tags are
moved where they are read), but a Custom Edition map's tags and structure
BSP keep the addresses they were linked to, from 0x40440000, and are read
into blocks of the program's own: xbox_pointer finds each linked address's
64 KB page in that window and moves it to its block
(halo-dos port/dos/game/dos_ce.c). With no Custom Edition map loaded the
window is empty (its span 0) and both are the identity; any address
outside it is left as it is. */
#define HALO_DOS_CE_WINDOW_BASE 0x40440000UL
#define HALO_DOS_CE_WINDOW_SIZE 0x01C00000UL
#define HALO_DOS_CE_WINDOW_PAGE_SHIFT 16

extern unsigned long dos_ce_window_span;
extern long dos_ce_window_delta[HALO_DOS_CE_WINDOW_SIZE >> HALO_DOS_CE_WINDOW_PAGE_SHIFT];
void *dos_ce_window_address(void const *pointer);

#define XPTR(type) type *
#define XBOX_NULL NULL

static __inline__ __attribute__((always_inline)) void *dos_xbox_pointer(unsigned long address)
{
	unsigned long offset = address - HALO_DOS_CE_WINDOW_BASE;

	return offset < dos_ce_window_span ?
		(void *)(address + dos_ce_window_delta[offset >> HALO_DOS_CE_WINDOW_PAGE_SHIFT]) : (void *)address;
}

static __inline__ __attribute__((always_inline)) void *dos_xbox_address(void const *pointer)
{
	return dos_ce_window_span ? dos_ce_window_address(pointer) : (void *)pointer;
}

#define xbox_pointer(address) dos_xbox_pointer((unsigned long)(address))
#define xbox_address(pointer) dos_xbox_address((void const *)(pointer))
#define POINTER_BITS(pointer) ((unsigned long)(pointer))

""")
    G = "source/saved games/game_state_xbox.c"
    replace(G, r"""#ifdef HALO_ADDRESS_TRANSLATED
	match_assert(
		"c:\\halo\\SOURCE\\saved games\\game_state_xbox.c",
		59,""", r"""#if defined(HALO_ADDRESS_TRANSLATED) && !defined(HALO_DOS)
	match_assert(
		"c:\\halo\\SOURCE\\saved games\\game_state_xbox.c",
		59,""")
    C = "source/cache/cache_files.c"
    insert_after(C, "boolean cache_file_is_ce;\n", """#ifdef HALO_DOS
/* (the DOS and Windows 95 builds keep a Custom Edition map's tags and
structure BSP in blocks of their own, seen at the addresses they were linked
to: port/dos/game/dos_ce.c) */
void *dos_ce_tag_cache_reserve(unsigned long tag_data_size);
unsigned long dos_ce_tag_cache_end(unsigned long end_free);
void dos_ce_structure_bsp_mapped(void *block, unsigned long linked_address, unsigned long size);
#endif
""")
    replace(C, """			tag_cache_base_address = xbox_pointer(CE_TAG_CACHE_BASE);
			csmemset(tag_cache_base_address, 0xCD, CE_TAG_CACHE_SIZE);""", """#ifdef HALO_DOS
			/* (in a block of the DOS and Windows 95 builds' own, of the size
			its check found its tags, resources and models need: dos_ce.c) */
			tag_cache_base_address = dos_ce_tag_cache_reserve(cache_file_globals.header.tag_data_size);
			if (!tag_cache_base_address)
			{
				cache_file_close();

				return NONE;
			}
#else
			tag_cache_base_address = xbox_pointer(CE_TAG_CACHE_BASE);
			csmemset(tag_cache_base_address, 0xCD, CE_TAG_CACHE_SIZE);
#endif""")
    insert_before(C, """				if (!ce_resources_tags_loaded(global_tag_instances, ce_header->tag_count,""", """#ifdef HALO_DOS
				/* (no further than its block goes) */
				end_free = dos_ce_tag_cache_end(end_free);
#endif
""")
    K = "port/linux/game/ce_map_checks.c"
    insert_before(K, """/* ---------- globals */
""", """#ifdef HALO_DOS
/* the DOS and Windows 95 builds' blocks for a Custom Edition map
(port/dos/game/dos_ce.c): the addresses of the map's tag cache they hold,
where the check images below are made smaller, and what a check found the
map needs */
void *dos_ce_window_pointer(unsigned long address, unsigned long size);
unsigned long dos_ce_check_image_size(unsigned long tag_data_size);
void dos_ce_checked(unsigned long end_free, unsigned long next_free, unsigned long model_bytes,
	unsigned long bsp_bytes);
#endif

""")
    replace(K, """	if (address < image->base || !ce_range_within(address - image->base, size, image->size))
		return NULL;
	return image->data + (address - image->base);""", """	if (address < image->base || !ce_range_within(address - image->base, size, image->size))
		return NULL;
#ifdef HALO_DOS
	/* (the map's own tag cache, in the DOS and Windows 95 builds' blocks:
	where they hold those bytes, all in one block, or nothing) */
	if (image->base == CE_IMAGE_TAG_CACHE_BASE && image->data == xbox_pointer(CE_IMAGE_TAG_CACHE_BASE))
		return dos_ce_window_pointer(address, size);
#endif
	return image->data + (address - image->base);""")
    replace(K, """	/* (the bounds first: they pass for nearly every read) */
	if (!size || (address >= CE_IMAGE_TAG_CACHE_BASE && size > 0 &&
		ce_range_within(address - CE_IMAGE_TAG_CACHE_BASE, (unsigned long)size, CE_IMAGE_TAG_CACHE_SIZE)) ||
		!cache_file_tags_are_ce())
	{
		return xbox_pointer(address);
	}""", """#ifdef HALO_DOS
	/* (the DOS and Windows 95 builds: the bytes where their blocks hold
	them, or, outside the map's tag cache or the blocks, as below) */
	if (!size || !cache_file_tags_are_ce())
		return xbox_pointer(address);
	if (address >= CE_IMAGE_TAG_CACHE_BASE && size > 0 &&
		ce_range_within(address - CE_IMAGE_TAG_CACHE_BASE, (unsigned long)size, CE_IMAGE_TAG_CACHE_SIZE))
	{
		void *pointer = dos_ce_window_pointer(address, (unsigned long)size);

		if (pointer)
			return pointer;
	}
#else
	/* (the bounds first: they pass for nearly every read) */
	if (!size || (address >= CE_IMAGE_TAG_CACHE_BASE && size > 0 &&
		ce_range_within(address - CE_IMAGE_TAG_CACHE_BASE, (unsigned long)size, CE_IMAGE_TAG_CACHE_SIZE)) ||
		!cache_file_tags_are_ce())
	{
		return xbox_pointer(address);
	}
#endif""")
    replace(K, """	image.data = malloc(CE_IMAGE_TAG_CACHE_SIZE);
	if (!image.data)
		return ce_refuse("no memory to check it");
	memset(image.data, 0xcd, CE_IMAGE_TAG_CACHE_SIZE);""", """#ifdef HALO_DOS
	/* (the DOS and Windows 95 builds: of its tags and room for what is
	copied in after them, not of the whole tag cache, 23 MB: dos_ce.c) */
	image.data = malloc(dos_ce_check_image_size(tag_data_size));
	if (!image.data)
		return ce_refuse("no memory to check it");
	memset(image.data, 0xcd, dos_ce_check_image_size(tag_data_size));
#else
	image.data = malloc(CE_IMAGE_TAG_CACHE_SIZE);
	if (!image.data)
		return ce_refuse("no memory to check it");
	memset(image.data, 0xcd, CE_IMAGE_TAG_CACHE_SIZE);
#endif""")
    replace(K, """	if (!ce_resources_check(&image, instances, tag_count, end_free, file_size, &next_free))
		goto done;""", """#ifdef HALO_DOS
	/* (no further than the image goes: its room for them is checked again,
	below, against the whole tag cache's) */
	if (!ce_resources_check(&image, instances, tag_count,
		end_free < image.base + dos_ce_check_image_size(tag_data_size) ? end_free :
			image.base + dos_ce_check_image_size(tag_data_size), file_size, &next_free))
	{
		goto done;
	}
#else
	if (!ce_resources_check(&image, instances, tag_count, end_free, file_size, &next_free))
		goto done;
#endif""")
    insert_before(K, """	valid = ce_shaders_check(&tags, instances, tag_count) && ce_hud_check(&tags, instances, tag_count) &&""",
                  """#ifdef HALO_DOS
	/* (what the DOS and Windows 95 builds' blocks for it are made of) */
	dos_ce_checked(end_free, next_free, model_bytes, bsp_bytes);
#endif
""")
    F = "port/linux/game/ce_functions.c"
    replace(F, """		return NULL;
	}
	return xbox_pointer(address);
}""", """		return NULL;
	}
#ifdef HALO_DOS
	/* (where the DOS and Windows 95 builds' blocks hold them: dos_ce.c) */
	{
		void *dos_ce_window_pointer(unsigned long address, unsigned long size);

		return dos_ce_window_pointer(address, size);
	}
#else
	return xbox_pointer(address);
#endif
}""")
    H = "port/linux/game/ce_hud.c"
    replace(H, """		if (instance->group_tag == 'hudg' && instance->base_address)
			ce_hud_button_icons(xbox_pointer(instance->base_address), &image);""", """#ifndef HALO_DOS
		/* (the DOS and Windows 95 builds name the buttons in a HUD message
		themselves: port/dos/game/dos_prompts.c) */
		if (instance->group_tag == 'hudg' && instance->base_address)
			ce_hud_button_icons(xbox_pointer(instance->base_address), &image);
#endif""")
    R = "port/linux/game/ce_resources.c"
    replace(R, """	ce_free_end = end_free;
	ce_sounds_decode(tag_instances, tag_count);
	return TRUE;""", """	ce_free_end = end_free;
#ifdef HALO_DOS
	/* (not with the sound off: decoding a map's Ogg Vorbis sounds takes a
	slow machine a while, and 6 MB of the disk for Blood Gulch: dos_ce.c) */
	{
		extern int dos_ce_sounds_wanted(void);

		if (dos_ce_sounds_wanted())
			ce_sounds_decode(tag_instances, tag_count);
	}
#else
	ce_sounds_decode(tag_instances, tag_count);
#endif
	return TRUE;""")
    replace(R, """			sprintf(ce_decoded_sounds_path, "%s", path);
			DeleteFileA(path);""", """			sprintf(ce_decoded_sounds_path, "%s", path);
#ifndef HALO_DOS
			DeleteFileA(path);
#endif""")


def scripts_and_paths():
    insert_before("source/hs/hs_runtime.c", """void render_debug_trigger_volumes(
	void)""", """#ifdef HALO_DOS
void system_log(const char *text);

/* the DOS and Windows 95 builds' log of what render_debug_scripting shows:
each waiting script, its sleep and the function it waits in (testing:
setting log_scripts) */
void dos_log_scripts(
	void)
{
	char line[192];
	long thread_index;

	for (thread_index = data_next_index(hs_thread_data, NONE);
		hs_runtime_globals.initialized && thread_index != NONE;
		thread_index = data_next_index(hs_thread_data, thread_index))
	{
		struct hs_thread_datum *thread = hs_thread_get(thread_index);

		if (thread->sleep_until >= 0)
		{
			sprintf(line, "script %s: sleep %ld, in %s", hs_thread_format(thread_index),
				thread->sleep_until ? thread->sleep_until - game_time_get() : 0,
				thread->stack != (struct hs_stack_frame *)thread->stack_data && thread->sleep_until != -2 ?
					expression_get_function_name(thread_index, thread->stack->expression_index) : "-");
			system_log(line);
		}
	}
}
#endif

""")
    P = "source/ai/path.c"
    insert_after(P, """#include "ai/path_structure_bsp.h"
""", """
#ifdef HALO_DOS
/* the distance still to go counts this much more than the way come
(weighted A*, as the N64 port's): the bots' and Covenant's searches reach
their goal opening far fewer surfaces around an obstacle, for a way at most
this much longer than the shortest before smoothing straightens it */
#define PATH_HEURISTIC_WEIGHT 1.5f
/* a search in parts (path_state_find_continue): the surfaces it may still
open this call (-1: no limit), and whether it stopped for that */
static short dos_traverse_budget = -1;
static boolean dos_traverse_paused;
#else
#define PATH_HEURISTIC_WEIGHT 1.0f
#endif
""")
    replace(P, "quantized_cost_estimate = (long)(distance_to_destination / PATH_COST_ESTIMATE_GRANULARITY);",
            "quantized_cost_estimate = (long)(PATH_HEURISTIC_WEIGHT * distance_to_destination / PATH_COST_ESTIMATE_GRANULARITY);")
    replace(P, "initial_node->total_cost_estimate = distance_to_destination;",
            "initial_node->total_cost_estimate = PATH_HEURISTIC_WEIGHT * distance_to_destination;")
    replace(P, "state->closest_cost_estimate = distance_to_destination;",
            "state->closest_cost_estimate = PATH_HEURISTIC_WEIGHT * distance_to_destination;")
    replace(P, "total_cost_estimate += distance_to_destination;",
            "total_cost_estimate += PATH_HEURISTIC_WEIGHT * distance_to_destination;")
    replace(P, """	while (TRUE)
	{
		short cheapest_node_index = path_heap_pop_cheapest_node(state);
		struct path_node *cheapest_node;
		struct collision_bsp const *bsp;
		short edge_count;
		short edge_index;
""", """	while (TRUE)
	{
		short cheapest_node_index;
		struct path_node *cheapest_node;
		struct collision_bsp const *bsp;
		short edge_count;
		short edge_index;

#ifdef HALO_DOS
		/* (a search in parts: this call's share used up) */
		if (dos_traverse_budget >= 0)
		{
			if (dos_traverse_budget == 0)
			{
				dos_traverse_paused = TRUE;
				break;
			}
			dos_traverse_budget--;
		}
		/* the node list full and the destination none of its nodes: no node
		can be added, so the search can't reach it -- it ends here, at the
		closest node so far, instead of emptying the heap */
		if (state->node_count >= PATH_NODE_LIST_SIZE && state->destination_valid &&
			path_node_from_hash_table(state, state->destination.surface_index) == NONE)
		{
			break;
		}
#endif
		cheapest_node_index = path_heap_pop_cheapest_node(state);
""")
    replace(P, """	if (state->destination_valid)
	{
		result = state->closest_distance <= state->destination.target_radius;
	}
	if (state->debug && result)
	{
		state->debug->path_traverse_result = _path_traverse_result_success;
	}

	return result;
}
""", """#ifdef HALO_DOS
	if (dos_traverse_paused)
		return FALSE;
#endif
	if (state->destination_valid)
	{
		result = state->closest_distance <= state->destination.target_radius;
	}
	if (state->debug && result)
	{
		state->debug->path_traverse_result = _path_traverse_result_success;
	}

	return result;
}

#ifdef HALO_DOS
/* A search in parts (the bots' and Firefight's Covenant, after the N64
port: one search can be several milliseconds of a single tick): begun --
FALSE if the start can't be pathfound from, the search over -- then carried
on, at most budget surfaces opened a call, until path_state_find_continue
says it is over (TRUE; *found as path_state_find's answer). The state is
the search's alone meanwhile; path_state_build_path after, as after
path_state_find. */
boolean path_state_find_begin(
	struct path_state *state)
{
	path_state_reset(state);
	return path_state_begin(state);
}

boolean path_state_find_continue(
	struct path_state *state,
	short budget,
	boolean *found)
{
	boolean result;

	dos_traverse_budget = budget;
	dos_traverse_paused = FALSE;
	result = path_state_traverse(state);
	dos_traverse_budget = -1;
	if (dos_traverse_paused)
		return FALSE;
	if (found)
		*found = result;
	return TRUE;
}
#endif
""")
    insert_after("source/ai/path.h", """boolean path_state_find(
	struct path_state *state);
""", """#ifdef HALO_DOS
boolean path_state_find_begin(
	struct path_state *state);
boolean path_state_find_continue(
	struct path_state *state,
	short budget,
	boolean *found);
#endif
""")


def bots_and_firefight():
    C = "source/physics/collisions.c"
    insert_after(C, "boolean debug_collision_skip_objects = FALSE;\n", """#ifdef HALO_DOS
/* set while a ray asks only whether anything is in the way (the bots' and
Covenant's sight: port/dos/game/dos_bots.c, dos_firefight.c) */
int dos_collision_any_hit;
#endif
""")
    insert_before(C, "		if (test_objects && bsp_result.leaf_count > 0)\n", """#ifdef HALO_DOS
		/* (a line of sight asks only whether anything is in the way: with
		the level in the way, its objects need no test --
		dos_collision_any_hit, set around the AI's sight rays) */
		if (dos_collision_any_hit && hit)
			test_objects = FALSE;
#endif
""")
    Q = "source/game/player_queues_new.c"
    text = read(Q)
    if "dos_bots_actions" not in text:
        hook = """#ifdef HALO_DOS
	/* the bots' actions, as a controller's would be (port/dos/game/dos_bots.c,
	after the N64 port's) */
	{
		extern void dos_bots_actions(struct player_action *actions);

		dos_bots_actions(actions);
	}
#endif
"""
        anchor = "	update_client_globals.next_update_number_to_dequeue += 1;\n"
        if text.count(anchor) != 2:
            raise SystemExit(f"{Q}: {text.count(anchor)} dequeues")
        write(Q, text.replace(anchor, hook + anchor))
    insert_before("source/game/game_time.c", """						HALO_PERF_BEGIN(_halo_perf_tick);
						game_tick();""", """#ifdef HALO_DOS
						/* Firefight's Covenant, before each tick's own
						(port/dos/game/dos_firefight.c) */
						{
							extern void dos_firefight_tick(void);

							dos_firefight_tick();
						}
#endif
""")
    insert_before("source/units/unit_dialogue.c", """void unit_dialogue_update(""", """#ifdef HALO_DOS
/* a vocalization Firefight's Covenant ask a unit to say (they are no actors
of Halo's AI, which does it through ai_dialogue.c; port/dos/game/
dos_firefight.c, after the N64 port's): TRUE when it speaks */
boolean unit_vocalize(
	long unit_index,
	short vocalization_type,
	short priority)
{
	struct unit_datum *unit = unit_get(unit_index);
	long sound_definition_index = NONE;
	short play_type;

	if (unit->unit.dialogue_index == NONE ||
		vocalization_type < 0 ||
		vocalization_type >= NUMBER_OF_VOCALIZATION_TYPES)
	{
		return FALSE;
	}
	play_type = unit_test_speech(
		unit_index,
		priority,
		FALSE,
		FALSE,
		NULL,
		&vocalization_type,
		&sound_definition_index);
	if (play_type > _unit_play_speech_none && sound_definition_index != NONE)
	{
		struct unit_speech_item speech_item;

		csmemset(&speech_item, 0, sizeof(speech_item));
		speech_item.priority = priority;
		speech_item.vocalization_type = vocalization_type;
		speech_item.sound_definition_index = sound_definition_index;
		speech_item.pause_time = 15;
		ai_communication_packet_new(&speech_item.ai);
		unit_speak(unit_index, play_type, &speech_item);
		return TRUE;
	}

	return FALSE;
}
#endif

""")


def game_engine():
    G = "source/game/game_engine.c"
    replace(G, """			if (global_network_game_client_get())
			{
				player->team_index =
					PIN((signed char)player->network_player_data.team_index % 2, 0, 1);
			}
			else
			{
				player->network_player_data.team_index =
					(char)*next_team_index;
				player->team_index =
					(signed char)*next_team_index;
				*next_team_index = (*next_team_index + 1) % 2;
			}""", """#ifdef HALO_DOS
			/* a local game's teams are the menu's (main.c
			create_local_players) and dos_bots.c's for its bots: not dealt
			out in turn */
			if (global_network_game_client_get() || !global_network_game_server_get())
			{
				player->team_index =
					PIN((signed char)player->network_player_data.team_index % 2, 0, 1);
			}
			else
#else
			if (global_network_game_client_get())
			{
				player->team_index =
					PIN((signed char)player->network_player_data.team_index % 2, 0, 1);
			}
			else
#endif
			{
				player->network_player_data.team_index =
					(char)*next_team_index;
				player->team_index =
					(signed char)*next_team_index;
				*next_team_index = (*next_team_index + 1) % 2;
			}""")
    replace(G, """			if (test_any_gamepad_button(0) || test_any_gamepad_button(12))
			{
				if (global_network_game_server_get())
				network_game_server_reset_to_pregame(
					global_network_game_server_get());
			}
			else if (test_any_gamepad_button(1) || test_any_gamepad_button(13))
			{
				network_game_abort();
			}""", """#ifdef HALO_DOS
			/* a local game: back to the menu (port/dos/game/dos_menu.c),
			which shows the results: A or START once the scores are up, or
			by itself after fifteen seconds; a Custom Edition server's game
			waits for the server's next (port/dos/game/dos_ce_client.c) */
			int dos_ce_active(void);

			if (!global_network_game_server_get() && !global_network_game_client_get() && !dos_ce_active())
			{
				game_engine_globals.postgame_timer -= delta_seconds;
				if (game_engine_globals.postgame_progress >= 1.0f &&
					(test_any_gamepad_button(_gamepad_analog_button_a) ||
					test_any_gamepad_button(_gamepad_binary_button_start) ||
					game_engine_globals.postgame_timer < -15.0f))
				{
					main_goto_main_menu();
				}
			}
			else
#endif
			if (test_any_gamepad_button(0) || test_any_gamepad_button(12))
			{
				if (global_network_game_server_get())
				network_game_server_reset_to_pregame(
					global_network_game_server_get());
			}
			else if (test_any_gamepad_button(1) || test_any_gamepad_button(13))
			{
				network_game_abort();
			}""")
    at_function_start(G, "game_engine_should_end_game", """#ifdef HALO_DOS
	{
		/* (Firefight's players are one team against the Covenant, who
		aren't players: one team was all there was and the game ended as it
		began. dos_firefight.c ends it itself, when everyone is out) */
		extern int dos_firefight_active(void);

		if (dos_firefight_active())
			return FALSE;
	}
#endif
""")
    insert_before(G, """		/* port: a custom loadout's weapons (game_variant_options) */
		if (game_variant_options_get()->loadout == _loadout_custom)""", """#ifdef HALO_DOS
		/* (a Pro variant: a pistol and an assault rifle instead,
		port/dos/game/dos_game.c) */
		extern boolean dos_game_pro_weapons(long unit_index);

		if (dos_game_pro_weapons(unit_index))
			;
		else
#endif
""")
    insert_before(G, """boolean game_engine_force_single_screen(
	void)""", """#ifdef HALO_DOS
/* the menu's pause (port/dos/game/dos_menu.c): 0 while the game is played,
1 to 3 once it has ended (no pause then) */
long dos_game_engine_postgame_state(
	void)
{
	return game_engine ? game_engine_globals.postgame_state : 0;
}
#endif

""")


def main_loop():
    M = "source/main/main.c"
    replace(M, """			player = player_new(0, NONE, gamepad_index, NULL);
			local_player_set_player_index(
				gamepad_index,
				player);
		}
	}

	return;
}
""", """#ifdef HALO_DOS
			/* the player the menu set up (port/dos/game/dos_game.c): its
			name, colour and team */
			{
				extern void dos_game_local_player(long index, wchar_t *name, short *color, short *team);
				struct network_player dos_player;
				short color, team;

				csmemset(&dos_player, 0, sizeof(dos_player));
				dos_game_local_player(i, dos_player.name, &color, &team);
				dos_player.primary_color_index = color;
				dos_player.machine_index = 0;
				dos_player.controller_index = (char)gamepad_index;
				dos_player.team_index = (char)team;
				dos_player.player_list_index = (char)i;
				player = player_new(0, NONE, gamepad_index, &dos_player);
			}
#else
			player = player_new(0, NONE, gamepad_index, NULL);
#endif
			local_player_set_player_index(
				gamepad_index,
				player);
		}
#ifdef HALO_DOS
		/* the menu's bots, players with no controller (dos_bots.c) */
		{
			extern void dos_bots_create_players(void);

			dos_bots_create_players();
		}
#endif
	}

	return;
}
""")
    replace(M, """void main_menu_load(
	void)
{
	if (!main_globals.main_menu_scenario_loaded)
		main_load_ui_scenario(FALSE);
	main_screen_shell_load();""", """#ifdef HALO_DOS
/* The DOS and Windows 95 builds' menu (port/dos/game/dos_menu.c, after the
N64 port's) runs over ui.map's scene, as the Xbox's shell does (its scripts
glide the camera), instead of the shell's widgets: the game it chooses
loads as one the shell chose (main_set_map_name, then main_change_map_name).
The game that ended gives the menu its results first. */
void main_menu_load(
	void)
{
	extern void dos_game_menu(boolean game_ended);
	boolean game_ended = game_in_progress() && !main_globals.main_menu_scenario_loaded;

	main_globals.want_to_be_at_main_menu = FALSE;
	dos_game_menu(game_ended);
	/* (a system link game's lobby keeps its connection: the menu shows it) */
	if (!global_network_game_server_get() && !global_network_game_client_get())
		main_globals.connection = _game_connection_local;
	if (!main_globals.main_menu_scenario_loaded)
		main_load_ui_scenario(FALSE);
	update_server_delete();
	update_server_new();
	update_server_start();
	game_time_dispose_from_old_map();
	game_time_initialize_for_new_map();
	game_time_start();
	hs_runtime_dispose_from_old_map();
	hs_runtime_initialize_for_new_map();

	return;
}
#else
void main_menu_load(
	void)
{
	if (!main_globals.main_menu_scenario_loaded)
		main_load_ui_scenario(FALSE);
	main_screen_shell_load();""")
    replace(M, """	hs_runtime_dispose_from_old_map();
	hs_runtime_initialize_for_new_map();
	main_globals.want_to_be_at_main_menu = FALSE;

	return;
}
""", """	hs_runtime_dispose_from_old_map();
	hs_runtime_initialize_for_new_map();
	main_globals.want_to_be_at_main_menu = FALSE;

	return;
}
#endif
""")
    insert_before(M, """	MAIN_STAGE("main_setup_connection");
	main_setup_connection();""", """#ifdef HALO_DOS
	/* the game the settings choose, straight in, or the menu
	(port/dos/game/dos_game.c) */
	{
		extern void dos_game_setup(void);

		dos_game_setup();
	}
#endif
""")
    text = read(M)
    old = """			if (debug_force_frame_rate_update)
				seconds_elapsed = CEILING(seconds_elapsed, 0.03333333507180214f);
			else
				seconds_elapsed = CEILING(seconds_elapsed, 0.06666667014360428f);
"""
    new = """#ifdef HALO_DOS
			/* the Xbox kept above 15 frames a second, and a slower frame
			ran two ticks at most: a slow PC would slow the whole game down
			with it. A frame may run up to 4 ticks (the N64 build's rule):
			real time down to 7.5 frames a second */
			seconds_elapsed = CEILING(seconds_elapsed, 0.134f);
#else
			if (debug_force_frame_rate_update)
				seconds_elapsed = CEILING(seconds_elapsed, 0.03333333507180214f);
			else
				seconds_elapsed = CEILING(seconds_elapsed, 0.06666667014360428f);
#endif
"""
    if "CEILING(seconds_elapsed, 0.134f)" not in text:
        if text.count(old) != 2:
            raise SystemExit(f"{M}: {text.count(old)} frame caps")
        write(M, text.replace(old, new))


def networking():
    """system link play from the builds' own menu (port/dos/game/dos_network.c):
    the games a search found and joining one by its slot, the player's name
    and colour from the settings, and the menu's lobby between games"""
    C = "source/networking/network_client_manager.c"
    append(C, """
#ifdef HALO_DOS
/* The DOS and Windows 95 builds' menu (port/dos/game/dos_network.c): the
games the search has heard of, by slot, and joining one, as
network_game_client_join_first_available_game joins the first. */
boolean dos_network_listed_game(
	long index,
	wchar_t *name,
	char *map,
	int map_size,
	short *engine_type,
	short *players,
	short *maximum_players,
	boolean *open,
	boolean *teams,
	boolean *compatible,
	boolean *in_progress)
{
	struct network_game_client *client = global_network_game_client_get();
	struct network_advertised_game *game;

	if (!client || index < 0 || index >= MAXIMUM_NETWORK_ADVERTISED_GAMES)
		return FALSE;
	game = &client->available_games[index];
	if (!network_game_client_advertised_game_is_valid(game) || game->platform != network_game_get_local_platform())
		return FALSE;
	ustrncpy(name, game->game_name, NETWORK_GAME_NAME_LENGTH - 1);
	name[NETWORK_GAME_NAME_LENGTH - 1] = 0;
	csstrncpy(map, game->map.name, (unsigned long)map_size - 1);
	map[map_size - 1] = 0;
	*engine_type = game->engine_type;
	*players = (short)game->player_count;
	*maximum_players = game->maximum_player_count;
	*open = game->open;
	*teams = game->has_teams;
	*compatible = network_game_client_advertised_game_compatible(client, game, FALSE);
	*in_progress = network_game_client_advertised_game_in_progress(client, game);
	return TRUE;
}

/* 1: joining; 0: not now (searching no more, or a join under way); -1: a
game this machine cannot join */
long dos_network_join_listed_game(
	long index)
{
	struct network_game_client *client = global_network_game_client_get();
	struct network_advertised_game *game;
	struct transport_address address = { { { 0 } } };
	struct network_join_parameters join_parameters;

	if (!client || index < 0 || index >= MAXIMUM_NETWORK_ADVERTISED_GAMES ||
		client->state != _network_game_client_state_searching || client->join_in_progress ||
		!client->connection || network_connection_connected(client->connection))
	{
		return 0;
	}
	game = &client->available_games[index];
	if (!network_game_client_advertised_game_is_valid(game) || game->platform != network_game_get_local_platform() ||
		!game->open || !network_game_client_advertised_game_compatible(client, game, FALSE))
	{
		return -1;
	}
	csmemset(&join_parameters, 0, sizeof(join_parameters));
	transport_client_start((XNADDR const *)&game->xnaddr, (XNKEY const *)&game->key,
		(XNKID const *)&game->key_id, NETWORK_GAME_SERVER_PORT, &address);
	if (!address.address.long_words[0] || !address.port)
		return -1;
	network_game_generate_join_game_token(join_parameters.join_token);
	return network_game_client_initiate_join_game(client, game, &join_parameters, &address) ? 1 : -1;
}

/* the client's state (_network_game_client_state_*) and its last error */
long dos_network_client_state(
	short *error_code)
{
	struct network_game_client *client = global_network_game_client_get();

	*error_code = client ? client->error : 0;
	return client ? client->state : NONE;
}
#endif
""")
    T = "source/bungie_net/common/thread_win32.c"
    at_function_start(T, "create_thread", """#ifdef HALO_DOS
	/* one thread (port/dos/xdk/xdk_kernel.c): the job runs to its end now,
	and its thread is one that has ended */
	{
		extern HANDLE dos_thread_finished(DWORD exit_code);
		struct thread_reference *reference = get_thread_from_pool();

		if (!reference)
			return FALSE;
		*thread_reference = reference;
		reference->handle = dos_thread_finished(function(function_input));
		return reference->handle != NULL;
	}
#endif
""")
    append(C, """
#ifdef HALO_DOS
/* a listed game's host's identifier (its XNADDR's abEnet: internet play's
peers are known by it, port/linux/src/p2p.h) */
boolean dos_network_listed_identifier(
	long index,
	unsigned char *identifier)
{
	struct network_game_client *client = global_network_game_client_get();

	if (!client || index < 0 || index >= MAXIMUM_NETWORK_ADVERTISED_GAMES ||
		!network_game_client_advertised_game_is_valid(&client->available_games[index]))
	{
		return FALSE;
	}
	csmemcpy(identifier, ((XNADDR const *)&client->available_games[index].xnaddr)->abEnet, 6);
	return TRUE;
}
#endif
""")
    P = "source/interface/player_ui.c"
    at_function_start(P, "player_ui_get_active_player_profile", """#ifdef HALO_DOS
	/* the player's name and colour are the builds' settings
	(port/dos/game/dos_network.c), over the profile in use */
	{
		extern void dos_network_profile(short local_player_index, wchar_t *name, short *color);
		struct player_profile *own = profile;

		csmemcpy(profile, &player_ui_globals.local_players[local_player_index].profile, 0x30);
		dos_network_profile(local_player_index, own->player_name, &own->primary_color_index);
		return;
	}
#endif
""")


def performance():
    """work the slow machines skip: node matrices of objects that have not
    changed (port/dos/game/dos_objects.c)"""
    O = "source/objects/objects.c"
    at_function_start(O, "object_compute_node_matrices", """#ifdef HALO_DOS
	/* (the matrices it has are those of its state now: dos_objects.c) */
	{
		extern boolean dos_node_matrices_current(long object_index);

		if (dos_node_matrices_current(object_index))
			return;
	}
#endif
""")


def internet_play():
    """ChupathingyCE's internet play without its thread (these builds have
    one): p2p_initialize starts none, and each of the game's frames runs one
    pass of the thread's loop, waiting for nothing (dos_p2p_poll, from
    port/dos/game/dos_network.c)"""
    P = "port/linux/src/p2p.c"
    # (no clipboard in these builds: the link goes to a file, dos_network.c)
    replace(P, """			platform_log("Internet play: hosting. Invite players with this link (it only works while this "
				"copy of the game runs; it is on the clipboard): %s",""", """#ifdef HALO_DOS
			platform_log("Internet play: hosting. Invite players with this link (it only works while this "
				"copy of the game runs; it is written to INVITE.TXT, the setting net_invite_file): %s",
#else
			platform_log("Internet play: hosting. Invite players with this link (it only works while this "
				"copy of the game runs; it is on the clipboard): %s",
#endif""")
    replace(P, """	stun_setup();
	if (pthread_create(&thread, NULL, p2p_thread, NULL) != 0)""", """	stun_setup();
#ifdef HALO_DOS
	/* (no thread: the game's loop runs its passes, dos_p2p_poll) */
	(void)thread;
	if (0)
#else
	if (pthread_create(&thread, NULL, p2p_thread, NULL) != 0)
#endif""")
    replace(P, """		pthread_mutex_unlock(&p2p_lock);
		if (posix_socket_select(read, &read_count, write, &write_count, NULL, &error_count, 0,
			wait * 1000, 0) < 0)""", """#ifdef HALO_DOS
		/* (a pass of the game's frame: nothing waited for) */
		wait = 0;
#endif
		pthread_mutex_unlock(&p2p_lock);
		if (posix_socket_select(read, &read_count, write, &write_count, NULL, &error_count, 0,
			wait * 1000, 0) < 0)""")
    replace(P, """#if defined(HALO_ANDROID) && !defined(HALO_SWITCH_CONSOLE)
		poll_invite_file();
#endif
	}
	return NULL;
}
""", """#if defined(HALO_ANDROID) && !defined(HALO_SWITCH_CONSOLE)
		poll_invite_file();
#endif
#ifdef HALO_DOS
		/* (one pass a call) */
		break;
#endif
	}
	return NULL;
}
""")
    replace(P, """} p2p = { 0, 0, -1, 0, -1, .hosting_socket = -1 };""", """#ifdef HALO_DOS
/* (zeroed: an initialiser put its 2.6 MB in the program's file; its sockets
are -1 from the start, dos_p2p_state_initialize) */
} p2p;
#else
} p2p = { 0, 0, -1, 0, -1, .hosting_socket = -1 };
#endif""")
    append(P, """
#ifdef HALO_DOS
/* before anything else (the programs' start, port/dos/dos/dos_start.c and
port/dos/win95/w95_start.c): its sockets none */
void dos_p2p_state_initialize(void)
{
	p2p.tunnel_socket = -1;
	p2p.handoff_socket = -1;
	p2p.hosting_socket = -1;
}
#endif
""")
    append(P, """
#ifdef HALO_DOS
/* The DOS and Windows 95 builds' internet play: a pass of the thread's
loop, from the game's loop each frame (port/dos/game/dos_network.c) and
wherever it waits (Sleep, the loading screen); not inside itself */
void dos_p2p_poll(void)
{
	static int polling;

	if (p2p.running && !polling)
	{
		polling = 1;
		p2p_thread(NULL);
		polling = 0;
	}
}
#endif
""")


def memory():
    """the debug records ChupathingyCE's Xbox leaves out to fit 64 MB
    (ai_debug.h): the AI debug displays' (26 MB of actor records and 3.7 MB
    of path copies at the desktop limits), none of which these builds show"""
    A = "source/ai/ai_debug.h"
    text = read(A)
    if "HALO_DOS" not in text:
        # (and the state's offset checks, which the smaller sizes move)
        old = "#ifndef HALO_XBOX_CONSOLE\n"
        if text.count(old) != 4:
            raise SystemExit(f"{A}: {text.count(old)} Xbox switches")
        text = text.replace(old, "#if !defined(HALO_XBOX_CONSOLE) && !defined(HALO_DOS)\n")
        text = text.replace("#if !defined(HALO_64BIT) && !defined(HALO_XBOX_CONSOLE)\n",
                            "#if !defined(HALO_64BIT) && !defined(HALO_XBOX_CONSOLE) && !defined(HALO_DOS)\n")
        write(A, text)


def colors():
    """a colour blend a hair outside 0 to 1 (a factor run over its end, the
    HSV round trip's last bit) stopped the game at the release build's assert
    (seen in a DOS postgame): the result clamped first"""
    B = "source/bitmaps/bitmap_utilities.c"
    insert_before(B, r"""	match_assert_valid_real_rgb_color("c:\\halo\\SOURCE\\bitmaps\\bitmap_utilities.c", 0x95D, rgb_result);""",
                  r"""#ifdef HALO_DOS
	rgb_result->red = PIN(rgb_result->red, 0.f, 1.f);
	rgb_result->green = PIN(rgb_result->green, 0.f, 1.f);
	rgb_result->blue = PIN(rgb_result->blue, 0.f, 1.f);
#endif
""")


def lookups():
    """a block's element and an array's datum inline (each on every tag and
    object read, thousands a frame, and a call is dear under DOSBox: about
    45 instructions): an index in range is its element, as the functions
    answer; anything else goes to the functions, with their checks and
    messages"""
    T = "source/tag_files/tag_groups.h"
    insert_after(T, "void *tag_block_get_element_with_size(struct tag_block const *block, long index, long element_size);\n",
                 r"""#ifdef HALO_DOS
/* port: the DOS and Windows 95 builds' (tools/dos_hooks.py lookups) */
static __inline__ __attribute__((always_inline)) void *tag_block_get_element_fast(
	struct tag_block const *block, long index, long element_size)
{
	if ((unsigned long)index < (unsigned long)block->count && block->address)
		return (void *)((byte *)xbox_pointer(block->address) + index * element_size);
	return tag_block_get_element_with_size(block, index, element_size);
}
#undef TAG_BLOCK_GET_ELEMENT
#define TAG_BLOCK_GET_ELEMENT(block_address, index, type) \
	((type *)tag_block_get_element_fast((block_address), (index), sizeof(type)))
#endif
""")
    D = "source/memory/data.h"
    insert_after(D, "void *datum_get(struct data_array *data, long index);\n", r"""#if defined(HALO_DOS) && !defined(HALO_DATA_C)
/* port: the DOS and Windows 95 builds' (tools/dos_hooks.py lookups) */
static __inline__ __attribute__((always_inline)) void *datum_get_fast(struct data_array *data, long index)
{
	short identifier = (short)(index >> 16), absolute_index = (short)index;

	if (absolute_index >= 0 && absolute_index < data->count && data->valid &&
		(identifier || !data->identifier_zero_invalid))
	{
		struct datum_header *header = (struct datum_header *)((byte *)xbox_pointer(data->data) +
			data->size * absolute_index);

		if (header->identifier && (!identifier || identifier == header->identifier))
			return header;
	}
	return datum_get(data, index);
}
#define datum_get(data, index) datum_get_fast((data), (index))
#endif
""")
    insert_before("source/memory/data.c", '#include "cseries.h"\n#include "data.h"\n',
                  "/* (datum_get itself here: data.h's inline form is the others') */\n#define HALO_DATA_C\n")
    # an object of the types asked for (object_get's): its datum, inline
    O = "source/objects/objects.h"
    insert_after(O, "extern struct data_array *object_header_data;\n", r"""#if defined(HALO_DOS) && !defined(HALO_OBJECTS_C)
/* port: the DOS and Windows 95 builds' (tools/dos_hooks.py lookups) */
static __inline__ __attribute__((always_inline)) void *object_get_and_verify_type_fast(long object_index,
	unsigned long valid_type_flags)
{
	struct object_header_datum *header = (struct object_header_datum *)datum_get(object_header_data, object_index);

	if (header && header->datum && TEST_FLAG(valid_type_flags, header->datum->object.type))
		return header->datum;
	return object_get_and_verify_type(object_index, valid_type_flags);
}
#define object_get_and_verify_type(index, flags) object_get_and_verify_type_fast((index), (flags))
#endif
""")
    insert_before("source/objects/objects.c", '#include "cseries.h"\n#include "objects.h"\n',
                  "/* (object_get_and_verify_type itself here: objects.h's inline form is the others') */\n"
                  "#define HALO_OBJECTS_C\n")


def capacity():
    """the game state's pools for a 64 MB PC: the desktop builds' took 17.6
    MB of 20 (8 MB of object memory, 2.5 MB of AI props...), and a campaign
    level then left the DOS build no room for its level (no distant terrain,
    no terrain tiles, a 22-second build). What goes on the wire stays the
    PCs' (the object slots and the cluster references, the players: so a PC
    host's objects land where they do there, docs/cross-play.md); the pools
    each machine keeps for itself are the Xbox's, or a few times them: the
    AI's (no network co-op here), the effects', and the particles', the
    contrails' and the lights' (none made in these builds). A change to them
    changes the game state's layout: checkpoints saved before do not load."""
    C = "port/linux/include/halo_port_capacity.h"
    insert_before(C, "#else\n#define HALO_PORT_GAME_STATE_BASE_ADDRESS 0x81A00000 /* (0x80061000) */\n", r"""#elif defined(HALO_DOS)
/* the DOS and Windows 95 builds' (tools/dos_hooks.py capacity) */
#define HALO_PORT_GAME_STATE_BASE_ADDRESS 0x81A00000
#ifdef HALO_D3D
/* (the Direct3D build's renderer keeps its own state there too:
port/dos/DIRECT3D.md) */
#define HALO_PORT_GAME_STATE_CPU_SIZE 0x900000
#else
#define HALO_PORT_GAME_STATE_CPU_SIZE 0x600000 /* (5.9 MB used) */
#endif
#define HALO_PORT_GAME_STATE_GPU_SIZE 0x40000
#define HALO_PORT_GAME_STATE_SIZE (HALO_PORT_GAME_STATE_CPU_SIZE+HALO_PORT_GAME_STATE_GPU_SIZE)

#define HALO_PORT_MAXIMUM_ACTORS 256
#define HALO_PORT_MAXIMUM_PROPS 1024
#define HALO_PORT_MAXIMUM_SWARMS 32
#define HALO_PORT_MAXIMUM_SWARM_COMPONENTS 256

#define HALO_PORT_MAXIMUM_OBJECTS_PER_MAP 8192
#define HALO_PORT_OBJECT_MEMORY_POOL_SIZE 0x300000
#define HALO_PORT_MAXIMUM_CLUSTER_REFERENCES 8192
#define HALO_PORT_MAXIMUM_RENDERED_OBJECTS 1024
#define HALO_PORT_MAXIMUM_CACHED_OBJECT_RENDER_STATES 1024
#define HALO_PORT_MAXIMUM_AREA_OF_EFFECT_OBJECTS 256
#define HALO_PORT_MAXIMUM_OBJECT_LISTS_PER_MAP 1024
#define HALO_PORT_MAXIMUM_LISTED_OBJECTS_PER_MAP 8192
#define HALO_PORT_MAXIMUM_WIDGETS 2048
#define HALO_PORT_MAXIMUM_LIGHT_VOLUMES 2048

#define HALO_PORT_MAXIMUM_EFFECTS 512
#define HALO_PORT_MAXIMUM_EFFECT_LOCATIONS 1024
#define HALO_PORT_MAXIMUM_PARTICLES 1024
#define HALO_PORT_MAXIMUM_PARTICLE_SYSTEMS 64
#define HALO_PORT_MAXIMUM_SYSTEM_PARTICLES 512

#include "halo_console_memory.h"

#define HALO_PORT_MAXIMUM_CONTRAILS 256
#define HALO_PORT_MAXIMUM_CONTRAIL_POINTS 1024
#define HALO_PORT_MAXIMUM_LIGHTS_PER_MAP 896
#define HALO_PORT_MAXIMUM_GAME_LOOPING_SOUNDS 1024

""")


def cinematic_detail():
    """a cinematic's "movie star" objects (a30's lifeboat and the marines in
    it...) were drawn at their most detailed whatever their size: 18,600
    triangles a frame in a30's opening. Here they take the detail their size
    on the screen asks for, as everything else does"""
    R = "source/render/render_objects.c"
    replace(R, """	if (cinematic_in_progress() &&
		TEST_FLAG(object_get(object_index)->object.flags, _object_movie_star_bit))
	{
		return REAL_MAX;
	}
""", """#ifndef HALO_DOS_TRIMS
	if (cinematic_in_progress() &&
		TEST_FLAG(object_get(object_index)->object.flags, _object_movie_star_bit))
	{
		return REAL_MAX;
	}
#endif
""")


def model_detail():
    """the DETAIL setting's lower choices take coarser models: a model's size
    on the screen scaled down for its level of detail alone (whether it is
    drawn at all stays the game's): a30's 26 pine trees through the
    lifeboat's door were 13,600 triangles a frame"""
    M = "source/models/models.c"
    replace(M, """		geometry_detail_level_index = NUMBER_OF_DETAIL_LEVELS_PER_MODEL-1;
		while (geometry_detail_level_index>0 &&
			level_of_detail_pixels<model->detail_cutoff_pixels[geometry_detail_level_index])
		{
			geometry_detail_level_index--;
		}
""", """		geometry_detail_level_index = NUMBER_OF_DETAIL_LEVELS_PER_MODEL-1;
#ifdef HALO_DOS
		/* port: (port/dos/game/dos_render_game.c dos_model_detail_scale) */
		{
			extern real dos_model_detail_scale(void);
			real scaled_pixels = level_of_detail_pixels * dos_model_detail_scale();

			while (geometry_detail_level_index>0 &&
				scaled_pixels<model->detail_cutoff_pixels[geometry_detail_level_index])
			{
				geometry_detail_level_index--;
			}
		}
#else
		while (geometry_detail_level_index>0 &&
			level_of_detail_pixels<model->detail_cutoff_pixels[geometry_detail_level_index])
		{
			geometry_detail_level_index--;
		}
#endif
""")


def model_size():
    """each model's size on the screen, for the renderer's coarse copies of
    small models' parts (port/dos/game/dos_models.c)"""
    M = "source/models/models.c"
    insert_after(M, "\tprofile_enter(render_model_section);\n", r"""#ifdef HALO_DOS
	{
		extern real dos_model_pixels;

		dos_model_pixels = level_of_detail_pixels;
	}
#endif
""")


def fps_counter():
    """the frame rate on the screen when the setting fps_counter is on (the
    menu's FPS COUNTER; ChupathingyCE shows it only for the scripts'
    display_framerate), in the top left as the builds had it before"""
    M = "source/main/main.c"
    replace(M, """	main_version_watermark_render();

	if (display_framerate)
	{""", """	main_version_watermark_render();

#ifdef HALO_DOS
	if (display_framerate || halo_fps_counter_enabled())
#else
	if (display_framerate)
#endif
	{""")
    replace(M, """			bounds.x0 = (short)(bounds.x1 - 50);
			bounds.y0 = (short)(bounds.y1 - 50);
			draw_string_set_format(NONE, _text_justification_left, 0);""", """#ifdef HALO_DOS
			/* the top left corner, above the HUD's title-safe frame */
			bounds.x0 = (short)(bounds.x0 - 40);
			bounds.y0 = (short)(bounds.y0 - 32);
			bounds.x1 = (short)(bounds.x0 + 60);
			bounds.y1 = (short)(bounds.y0 + 30);
#else
			bounds.x0 = (short)(bounds.x1 - 50);
			bounds.y0 = (short)(bounds.y1 - 50);
#endif
			draw_string_set_format(NONE, _text_justification_left, 0);""")
    insert_before(M, "void main_framerate_render(\n", """#ifdef HALO_DOS
/* the setting fps_counter (port/dos/game/dos_game.c) */
int halo_fps_counter_enabled(void);
#endif

""")


def game_state_address():
    """the game state is an ordinary allocation here (dos_game.c
    physical_memory_allocate), not at the Xbox's address: the assert that it
    is noted an 'EXCEPTION' in debug.txt at every start"""
    G = "source/saved games/game_state_xbox.c"
    replace(G, r"""#else
	match_assert(
		"c:\\halo\\SOURCE\\saved games\\game_state_xbox.c",
		59,
		(unsigned long)result==address);
#endif""", r"""#elif !defined(HALO_DOS)
	match_assert(
		"c:\\halo\\SOURCE\\saved games\\game_state_xbox.c",
		59,
		(unsigned long)result==address);
#endif""")


def game_state_log():
    """the setting debug_game_state: each of the game state's allocations
    logged, its name and size (what the pools take)"""
    G = "source/saved games/game_state.c"
    insert_before(G, "\tgame_state_allocation_record(name, type, size, FALSE);\n", r"""#ifdef HALO_DOS
	{
		extern long dos_setting_integer(const char *name, long default_value);
		extern void system_log(const char *text);
		char line[128];

		if (dos_setting_integer("debug_game_state", 0))
		{
			_snprintf(line, sizeof(line), "game state: %s (%s) %ld bytes, at %ld", name, type, size,
				game_state_globals.cpu_allocation_size);
			system_log(line);
		}
	}
#endif
""")


def loading_screens():
    """a loading screen whenever a map loads (port/dos/game/dos_render_game.c):
    game_precache_new_map comes first on every way a map is loaded (from the
    menu to a game, back, a campaign level to the next, a network game's),
    while the old map's fonts are still there to draw it with"""
    at_function_start("source/game/game.c", "game_precache_new_map", """#ifdef HALO_DOS
	/* a loading screen while the map loads (dos_render_game.c) */
	{
		extern void dos_loading_begin(const char *map_name);

		dos_loading_begin(map_name);
	}
#endif
""")


def controller_settings():
    """the CONTROLLER page's settings (port/dos/game/dos_menu.c): the original
    game's controller settings of the first local player's profile, put in
    force by Halo's own code (set_local_player_controls_from_player_profile:
    the button and stick layouts, the look sensitivity's rates, the
    inversions) when they differ from what the profile has"""
    append("source/interface/player_ui.c", """
#ifdef HALO_DOS
/* the DOS and Windows 95 builds' CONTROLLER page (port/dos/game/dos_menu.c):
the local players' controller settings (every one's: split screen's
players share them) as the settings file has them, in force when they
differ from a profile's (a profile loaded over them gets them back) */
void dos_player_ui_controls(
	short button_preset,
	short joystick_preset,
	short look_sensitivity,
	boolean invert_look,
	boolean flight_stick_aircraft_controls,
	boolean vibration_disabled)
{
	short local_player_index;

	if (button_preset < 0 || button_preset >= _button_preset_custom)
		button_preset = _button_preset_standard;
	if (joystick_preset < 0 || joystick_preset >= NUMBER_OF_JOYSTICK_PRESETS)
		joystick_preset = _joystick_preset_standard;
	for (local_player_index = 0; local_player_index < MAXIMUM_NUMBER_OF_LOCAL_PLAYERS; local_player_index++)
	{
		struct player_profile_controller_settings *controls =
			&player_ui_globals.local_players[local_player_index].profile.controller_settings;

		if (controls->button_preset == button_preset && controls->joystick_preset == joystick_preset &&
			controls->look_sensitivity == look_sensitivity && controls->invert_look == invert_look &&
			controls->flight_stick_aircraft_controls == flight_stick_aircraft_controls &&
			controls->vibration_disabled == vibration_disabled)
		{
			continue;
		}
		controls->button_preset = (byte)button_preset;
		controls->joystick_preset = (byte)joystick_preset;
		controls->look_sensitivity = (byte)look_sensitivity;
		controls->invert_look = invert_look;
		controls->flight_stick_aircraft_controls = flight_stick_aircraft_controls;
		controls->vibration_disabled = vibration_disabled;
		set_local_player_controls_from_player_profile(local_player_index);
	}
}
#endif
""")


def split_prompts():
    """split screen (port/dos/include/dos_split.h): the prompts' keys
    ("[E]", "Hold F1 for score") are the keyboard's player's, whichever
    local player that is; the others' views keep the controller's buttons"""
    I = "source/input/input_abstraction.c"
    replace(I, """	if (local_player_index != 0 || count < 4 || !halo_keyboard_prompts())
		return FALSE;""", """#ifdef HALO_DOS
	if (local_player_index != dos_split_keyboard_port() || count < 4 || !halo_keyboard_prompts())
		return FALSE;
#else
	if (local_player_index != 0 || count < 4 || !halo_keyboard_prompts())
		return FALSE;
#endif""")
    replace(I, """	if (!message || !input_abstraction_port_icon_key(0, _port_icon_back_button, FALSE, key, NUMBEROF(key)))
		return;""", """#ifdef HALO_DOS
	if (!message ||
		!input_abstraction_port_icon_key((short)dos_split_keyboard_port(), _port_icon_back_button, FALSE, key, NUMBEROF(key)))
	{
		return;
	}
#else
	if (!message || !input_abstraction_port_icon_key(0, _port_icon_back_button, FALSE, key, NUMBEROF(key)))
		return;
#endif""")
    insert_before(I, """/* port: the keyboard's key for a button icon, "[E]", while the prompts""", """#ifdef HALO_DOS
/* split screen's keyboard player (port/dos/game/dos_split.c) */
int dos_split_keyboard_port(void);
#endif

""")
    G = "source/game/game_engine.c"
    replace(G, """			/* port: the scoreboard's key for BACK while the keyboard or
			mouse was used last (Halo PC's prompts) */
			if (result)
				input_abstraction_port_back_word(message, message_character_count);""", """			/* port: the scoreboard's key for BACK while the keyboard or
			mouse was used last (Halo PC's prompts) */
#ifdef HALO_DOS
			/* (split screen: the keyboard's player's message only) */
			if (result && player->local_player_index == dos_split_keyboard_port())
#else
			if (result)
#endif
				input_abstraction_port_back_word(message, message_character_count);""")
    insert_before(G, """boolean game_engine_force_single_screen(""", """#ifdef HALO_DOS
/* split screen's keyboard player (port/dos/game/dos_split.c) */
int dos_split_keyboard_port(void);
#endif

""")


def d3d_menu():
    """the Direct3D build (port/dos/DIRECT3D.md) draws with Halo's own
    renderer, render.c among it: the port's menu or pause over the whole
    screen's window, where the DOS and Windows 95 builds' renderer draws it
    (port/dos/game/dos_render_game.c); render.c is in no other build"""
    replace("source/render/render.c", """		interface_draw_fullscreen_overlays();
		rasterizer_debug_draw();
		break;""", """		interface_draw_fullscreen_overlays();
		rasterizer_debug_draw();
#ifdef HALO_DOS
		/* the port's menu or pause over it all (port/dos/game/dos_menu.c) */
		{
			extern void dos_menu_frame(void);

			dos_menu_frame();
		}
#endif
		break;""")


def d3d_texture_cache():
    """the Direct3D build's texture cache is the Xbox's 22 MB (the desktop
    builds' 256 MB is no size for a Windows 95 computer); the DOS and
    Windows 95 builds never allocate theirs (port/dos/game/dos_game.c)"""
    replace("port/linux/include/halo_port_capacity.h", """#elif !defined(HALO_XBOX_CONSOLE) && !defined(HALO_CONSOLE_XBOX_RENDER_SIZES)
#define HALO_PORT_TEXTURE_CACHE_SIZE 0x10000000 /* (0x1600000) */""", """#elif defined(HALO_D3D)
/* port: the Direct3D build (halo-dos port/dos/DIRECT3D.md): the Xbox's */
#define HALO_PORT_TEXTURE_CACHE_SIZE 0x1600000 /* (0x1600000) */
#elif !defined(HALO_XBOX_CONSOLE) && !defined(HALO_CONSOLE_XBOX_RENDER_SIZES)
#define HALO_PORT_TEXTURE_CACHE_SIZE 0x10000000 /* (0x1600000) */""")


def ce_client():
    """a Custom Edition server's game (port/dos/game/dos_ce_client.c) runs the
    engine as a distributed client without this netcode's client: the frame's
    start and end must not take that for a lost connection and put the game
    back to local every frame"""
    N = "source/networking/network_game_globals.c"
    insert_before(N, """/* the distributed netcode's per-tick state (port/linux/game/network_distributed.c),""", """#ifdef HALO_DOS
/* halo-dos: a Custom Edition server's game is on (port/dos/game/dos_ce_client.c) */
int dos_ce_active(void);
#endif

""")
    replace(N, """		main_goto_main_menu();
		result = TRUE;
	}
	else if (!global_network_game_client)
	{""", """		main_goto_main_menu();
		result = TRUE;
	}
#ifdef HALO_DOS
	else if (!global_network_game_client && dos_ce_active())
	{
		/* halo-dos: a Custom Edition server's game: its client is not this
		netcode's (dos_ce_client.c) */
		result = TRUE;
	}
#endif
	else if (!global_network_game_client)
	{""")
    replace(N, """	result = TRUE;
	if (!global_network_game_client)
	{
		game_connection_set(0);
		main_menu_ensure_player_queues_exist();
	}""", """	result = TRUE;
#ifdef HALO_DOS
	if (!global_network_game_client && dos_ce_active())
		/* halo-dos: a Custom Edition server's game (dos_ce_client.c) */
		result = TRUE;
	else
#endif
	if (!global_network_game_client)
	{
		game_connection_set(0);
		main_menu_ensure_player_queues_exist();
	}""")


def opence_versions():
    """hosts of OpenCE's network versions 35 on (OpenCE itself, ChupathingyCE
    0.8.1 with its signed table) send the game's settings record 16 bytes
    longer (20 from 40): the gametype's further options, in front of the
    record's last 4 bytes (ChupathingyCE 0.8.1's network_rules.c). A client
    takes it at any of those sizes and makes it the older layout; a game of
    one of OpenCE's own game types (SWAT, Infection, Gun Game, Hardcore),
    which this game does not play, is left with the reason"""
    M = "source/networking/network_client_message_handler.c"
    replace(M, """#ifdef HALO_PORT_CONSOLE_LIMITS
#include "networking/network_game_protocol.h"
#endif
#include "networking/network_messages.h"
""", """#if defined(HALO_PORT_CONSOLE_LIMITS) || defined(HALO_DOS)
#include "networking/network_game_protocol.h"
#endif
#include "networking/network_messages.h"
""")
    insert_after(M, """static long network_game_client_settings_staging_size = 0;
""", """#ifdef HALO_DOS
/* halo-dos: OpenCE's records of network version 35 on, 16 bytes longer (20
from 40, up to 64 more by a signed table's layouts): taken here, then made
the older layout in the staging record; the further options (the first 16
of them: the mode, then Hardcore) kept to see the game type */
enum
{
	DOS_SETTINGS_SIZE = 13120,
	DOS_SETTINGS_FURTHER = 16 + 64,
	DOS_OPTION_MODE = 0,
	DOS_OPTION_HARDCORE = 1,
	DOS_MODE_SWAT = 1,
	DOS_MODE_INFECTION = 2,
	DOS_MODE_GUN_GAME = 3
};
typedef char dos_settings_size_assert[sizeof(network_game_client_settings_staging) == DOS_SETTINGS_SIZE ? 1 : -1];
static byte dos_settings_wire[DOS_SETTINGS_SIZE + DOS_SETTINGS_FURTHER];
static byte dos_settings_options[16];

/* the record taken (total_size bytes in dos_settings_wire) made the older
layout in the staging record */
static void dos_settings_fold(
	long total_size)
{
	long growth = total_size - DOS_SETTINGS_SIZE;
	byte *staging = (byte *)&network_game_client_settings_staging;

	csmemset(dos_settings_options, 0, sizeof(dos_settings_options));
	csmemcpy(staging, dos_settings_wire, DOS_SETTINGS_SIZE - 4);
	csmemcpy(staging + DOS_SETTINGS_SIZE - 4, dos_settings_wire + total_size - 4, 4);
	if (growth > 0)
	{
		csmemcpy(dos_settings_options, dos_settings_wire + DOS_SETTINGS_SIZE - 4,
			growth < (long)sizeof(dos_settings_options) ? growth : (long)sizeof(dos_settings_options));
		network_event("the host's record is OpenCE's longer one (%ld bytes): game type option %d, hardcore %d",
			total_size, (int)dos_settings_options[DOS_OPTION_MODE], (int)dos_settings_options[DOS_OPTION_HARDCORE]);
	}
}

/* the OpenCE game type the record plays, which this game does not, or NULL
(Infection with teams, Gun Game without, SWAT, on the Slayer engine;
Hardcore on any) */
static wchar_t const *dos_settings_unsupported(
	struct network_game const *game)
{
	int mode = dos_settings_options[DOS_OPTION_MODE];
	boolean teams = game->variant.universal_variant.teams != 0;

	if (game->variant.game_engine_index == game_engine_slayer)
	{
		if (mode == DOS_MODE_SWAT)
			return L"This game plays OpenCE's SWAT,\\r\\nwhich this Halo does not play.";
		if (mode == DOS_MODE_INFECTION && teams)
			return L"This game plays OpenCE's Infection,\\r\\nwhich this Halo does not play.";
		if (mode == DOS_MODE_GUN_GAME && !teams)
			return L"This game plays OpenCE's Gun Game,\\r\\nwhich this Halo does not play.";
	}
	if (dos_settings_options[DOS_OPTION_HARDCORE])
		return L"This game plays OpenCE's Hardcore,\\r\\nwhich this Halo does not play.";
	return NULL;
}
void display_error_text_deferred(wchar_t const *text, short local_player_index);
#endif
""")
    replace(M, """	if (piece->total_size != sizeof(network_game_client_settings_staging) ||
		piece->length > sizeof(piece->data) ||
		piece->offset + piece->length > sizeof(network_game_client_settings_staging))
	{""", """#ifdef HALO_DOS
	/* halo-dos: the older record, or OpenCE's longer ones (35 on) */
	if (piece->total_size < DOS_SETTINGS_SIZE || piece->total_size > DOS_SETTINGS_SIZE + DOS_SETTINGS_FURTHER ||
		piece->length > sizeof(piece->data) ||
		piece->offset + piece->length > piece->total_size)
#else
	if (piece->total_size != sizeof(network_game_client_settings_staging) ||
		piece->length > sizeof(piece->data) ||
		piece->offset + piece->length > sizeof(network_game_client_settings_staging))
#endif
	{""")
    replace(M, """			csmemcpy(
				(byte *)&network_game_client_settings_staging + piece->offset,
				piece->data,
				piece->length);
			network_game_client_settings_staging_size += piece->length;
			if (network_game_client_settings_staging_size == piece->total_size)
			{
				network_game_client_settings_staging_size = 0;
""", """#ifdef HALO_DOS
			csmemcpy(dos_settings_wire + piece->offset, piece->data, piece->length);
#else
			csmemcpy(
				(byte *)&network_game_client_settings_staging + piece->offset,
				piece->data,
				piece->length);
#endif
			network_game_client_settings_staging_size += piece->length;
			if (network_game_client_settings_staging_size == piece->total_size)
			{
				network_game_client_settings_staging_size = 0;
#ifdef HALO_DOS
				dos_settings_fold(piece->total_size);
#endif
""")
    replace(M, """				result = network_game_client_game_settings_updated(client, &network_game_client_settings_folded);
#else""", """#ifdef HALO_DOS
				if (dos_settings_unsupported(&network_game_client_settings_folded))
				{
					network_event("cross-play: leaving the host's game: it plays one of OpenCE's game types");
					display_error_text_deferred(dos_settings_unsupported(&network_game_client_settings_folded), NONE);
					network_game_client_rejected_by_game(client, source_address, _rejection_code_game_is_closed);
					return TRUE;
				}
#endif
				result = network_game_client_game_settings_updated(client, &network_game_client_settings_folded);
#else""")
    replace(M, """#else
				result = network_game_client_game_settings_updated(client, &network_game_client_settings_staging);
#endif""", """#else
#ifdef HALO_DOS
				if (dos_settings_unsupported(&network_game_client_settings_staging))
				{
					network_event("cross-play: leaving the host's game: it plays one of OpenCE's game types");
					display_error_text_deferred(dos_settings_unsupported(&network_game_client_settings_staging), NONE);
					network_game_client_rejected_by_game(client, source_address, _rejection_code_game_is_closed);
					return TRUE;
				}
#endif
				result = network_game_client_game_settings_updated(client, &network_game_client_settings_staging);
#endif""")


def listing_origin():
    """an online listing's flags and kind kept for the join page
    (port/dos/game/dos_network.c), which tells ChupathingyCE's games from
    OpenCE's by them: OpenCE's dedicated servers set bit 32 (its version
    39), ChupathingyCE's 128 too; the stamp's first byte is an OpenCE game
    type (version 37), which ChupathingyCE's hosts leave 0"""
    P = "port/linux/src/p2p.h"
    L = "port/linux/src/p2p_lobby.c"
    replace(P, """	/* the network version it was listed under (0: not known) */
	unsigned short version;
};""", """	/* the network version it was listed under (0: not known) */
	unsigned short version;
#ifdef HALO_DOS
	/* halo-dos: its flags as listed, and the kind its stamp says (an OpenCE
	game type, version 37; 0 for none) */
	unsigned char listed_flags, kind;
#endif
};""")
    replace(L, """	char gametype[P2P_LISTING_GAMETYPE_SIZE + 1];
	/* the signed part's size */
	int signed_size;
};""", """	char gametype[P2P_LISTING_GAMETYPE_SIZE + 1];
#ifdef HALO_DOS
	/* halo-dos: the stamp's first byte (an OpenCE game type, version 37) */
	unsigned char kind;
#endif
	/* the signed part's size */
	int signed_size;
};""")
    replace(L, """	offset += STAMP_SIZE;
	if (offset + P2P_SIGNATURE_SIZE != size)
		return 0;""", """#ifdef HALO_DOS
	if (offset + STAMP_SIZE > size)
		return 0;
	listing->kind = bytes[offset];
#endif
	offset += STAMP_SIZE;
	if (offset + P2P_SIGNATURE_SIZE != size)
		return 0;""")
    replace(L, """	shown->version = (unsigned short)listing->version;
""", """	shown->version = (unsigned short)listing->version;
#ifdef HALO_DOS
	/* halo-dos: OpenCE's dedicated servers' bit (32, its version 39) lists
	one as dedicated too */
	shown->dedicated = (listing->flags & (_listing_dedicated | 32)) != 0;
	shown->listed_flags = (unsigned char)listing->flags;
	shown->kind = listing->kind;
#endif
""")


def aim_assist():
    """the setting aim_assist (CONTROLS' AIM ASSIST, off by default): off,
    no player's bullets are autoaimed either (the view's magnetism is
    dos_chupathingy.c's halo_linux_mouse_aiming)"""
    C = "source/game/player_control.c"
    replace(C, """							if (halo_linux_mouse_aiming(gamepad_index))
							{
								control->magnetism_level = 0.f;
							}
						}
""", """							if (halo_linux_mouse_aiming(gamepad_index))
							{
								control->magnetism_level = 0.f;
							}
#ifdef HALO_DOS
							/* halo-dos: aim assist off (the setting aim_assist, port/dos/game/
							dos_chupathingy.c): the bullets' autoaim too */
							{
								extern int dos_aim_assist(void);

								if (!dos_aim_assist())
									control->autoaim_level = 0.f;
							}
#endif
						}
""")


def main():
    aim_assist()
    listing_origin()
    opence_versions()
    ce_client()
    d3d_menu()
    d3d_texture_cache()
    loading_screens()
    controller_settings()
    split_prompts()
    memory()
    colors()
    lookups()
    capacity()
    cinematic_detail()
    model_detail()
    model_size()
    fps_counter()
    game_state_address()
    game_state_log()
    lights_and_particles()
    tags_and_state()
    cache_files()
    custom_edition()
    scripts_and_paths()
    bots_and_firefight()
    game_engine()
    main_loop()
    networking()
    performance()
    internet_play()
    print("hooks applied")


if __name__ == "__main__":
    main()
