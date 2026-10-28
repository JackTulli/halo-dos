#ifndef XBOX_MAP_REBASE_H
#define XBOX_MAP_REBASE_H

/* ---------- moving a map's addresses to another memory window

The retail maps are linked to the Xbox memory window: their tag data holds
absolute addresses in the tag cache (0x803A6000) and each structure BSP
absolute addresses where it loads (below 0x819A6000, the tag cache's end).
A build whose window is elsewhere (HALO_WINDOW_BASE in
port/include/halo_memory_window.h; Windows 9x, where 0x80000000 and up is
the system's shared arena) moves every one of those addresses by the
window's displacement right after reading, before anything uses them
(source/cache/cache_files.c). The addresses are found by walking the tag
data from its header through the field layouts of each tag group
(xbox_map_layouts.h, made by tools/xbox_map_layouts.py), never by looking
at values: colours, flags and floats look like addresses too.

Each call works on the data as read, at its new place: linked_address is
the address the data was linked to, delta the new address minus it. Only
[data, data + size) is touched. They return nonzero on success; on failure
they leave a sentence saying what was wrong in error.

The layouts hold the fields that carry an address in the maps of one build
of the game (xbox_map_rebase_build, as a map's header names it). A map of
another build may keep addresses elsewhere, so a build that moves addresses
refuses it. */

const char *xbox_map_rebase_build(void);
int xbox_map_rebase_tag_data(void *data, unsigned long size, unsigned long linked_address, long delta,
	char *error, unsigned long error_size);
/* as xbox_map_rebase_tag_data, handing each tag reference's index word (a
tag's datum index, as the map numbers its tags) to visit_reference as the
walk meets it: a map whose tags join another's renumbers them (the DOS
build's Firefight, port/dos/game/dos_import.c) */
int xbox_map_rebase_tag_data_visiting(void *data, unsigned long size, unsigned long linked_address, long delta,
	void (*visit_reference)(void *context, unsigned long *index), void *context, char *error,
	unsigned long error_size);
int xbox_map_rebase_structure_bsp(void *data, unsigned long size, unsigned long linked_address, long delta,
	char *error, unsigned long error_size);
/* as xbox_map_rebase_structure_bsp, for a BSP read somewhere of its own
(the DOS build's, which keeps no tag cache of the Xbox's size): its own
addresses move by delta, and its tag references' names, which are in the
tag data, by names_delta (the tag data's) */
int xbox_map_rebase_structure_bsp_apart(void *data, unsigned long size, unsigned long linked_address, long delta,
	long names_delta, char *error, unsigned long error_size);

#endif
