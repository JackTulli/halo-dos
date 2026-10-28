/*
DOS_CHUPATHINGY.C

What ChupathingyCE's game code asks of its native ports (port/linux) that
the DOS and Windows 95 builds answer themselves, declared where the game
calls them rather than in a header (so the stand-in generator,
tools/dos_stubs.py, does not see them):

- the tags the native ports add to a map as it loads: the menus' tags
  (menu_tags.c): none here;
- the mouse's aim, which turns off the controller's magnetism;
- this build's name, for the console and the corner of the screen;
- a model's vertices read back (models.c), from the D3D header's Data;
- the network versions its games carry.
*/

#include "cseries.h"

#include <string.h>

void menu_tags_loaded(char const *map_name)
{
	(void)map_name;
}

void menu_tags_unloaded(void)
{
}

/* aim assist (the setting aim_assist, CONTROLS' AIM ASSIST; off by
default): on, the view's magnetism and the bullets' autoaim help every
player, controller or mouse; off, neither does (player_control.c's hook,
tools/dos_hooks.py aim_assist) */
int dos_aim_assist(void)
{
	long dos_setting_integer(const char *name, long default_value);

	return dos_setting_integer("aim_assist", 0) != 0;
}

/* whether a player's view goes without magnetism (player_control.c): all of
them with aim assist off, none with it on */
int halo_linux_mouse_aiming(short gamepad_index)
{
	(void)gamepad_index;
	return !dos_aim_assist();
}

char const *build_identity(void)
{
	return "Halo for DOS and Windows 95 (ChupathingyCE 0.8.0d's game)";
}

char const *build_identity_watermark(void)
{
	return "DOS";
}

#ifndef HALO_D3D
/* the Xbox's own Direct3D header: Common, Data, Lock (the Direct3D build's
device has its own: port/dos/d3d/xbox/xd3d_resources.c) */
struct dos_d3d_vertex_buffer
{
	unsigned long common;
	unsigned long data;
	unsigned long lock;
};

void __stdcall D3DVertexBuffer_Lock(struct dos_d3d_vertex_buffer *buffer, unsigned int offset, unsigned int size,
	unsigned char **data, unsigned long flags)
{
	(void)size;
	(void)flags;
	*data = buffer ? (unsigned char *)buffer->data + offset : NULL;
}

/* a Direct3D header made at run time (ChupathingyCE's ce_models.c and
ce_bsp.c, a Custom Edition map's buffers): nothing to register, the
renderer reads the header's Data */
void __stdcall D3DResource_Register(void *resource, void *base)
{
	(void)resource;
	(void)base;
}
#endif

/* the network versions this build's games are and join (delta.c's,
without its signed table from the internet): the game's own */
int delta_legacy_announce(void)
{
	return HALO_PORT_NETWORK_VERSION;
}

int delta_legacy_minimum(void)
{
	return HALO_PORT_NETWORK_VERSION_MINIMUM;
}

/* ... up to ChupathingyCE 0.8.1's: OpenCE's 33 to 39 are additive (its
delta.h's table), 35's longer settings record is taken
(network_client_message_handler.c, tools/dos_hooks.py opence_versions), and
one past the newest is presumed additive as there (DELTA_LEGACY_PRESUMED) */
#define DOS_NETWORK_VERSION_MAXIMUM 40

int delta_legacy_maximum(void)
{
	return HALO_PORT_NETWORK_VERSION_MAXIMUM > DOS_NETWORK_VERSION_MAXIMUM ? HALO_PORT_NETWORK_VERSION_MAXIMUM :
		DOS_NETWORK_VERSION_MAXIMUM;
}
