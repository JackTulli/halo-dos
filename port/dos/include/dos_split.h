/*
DOS_SPLIT.H

Split screen: the players at this machine, up to four, each on a device of
its own (the keyboard and mouse, or a controller: halo_system.h's pads), as
the menu's SPLIT SCREEN page joins them (port/dos/game/dos_menu.c). Halo's
local player n reads the game's controller port n (port/dos/xdk/
xdk_input.c), which gives it its player's device. Off (no players joined),
the one player is played with every device at once.
*/

#ifndef __DOS_SPLIT_H
#define __DOS_SPLIT_H

#define DOS_SPLIT_PLAYERS 4

enum
{
	DOS_DEVICE_NONE,
	DOS_DEVICE_KEYBOARD,
	DOS_DEVICE_PAD
};

struct dos_split_player
{
	int device;
	/* a controller's id (halo_system.h's pads) */
	unsigned short pad;
	/* Halo's colour index, and the team in team games (0 red, 1 blue) */
	int color;
	int team;
};

/* how many have joined: 0 split screen off */
int dos_split_count(void);
const struct dos_split_player *dos_split_player(int index);
/* a device joined: its player's index, or -1 (already in, or no room) */
int dos_split_join(int device, unsigned short pad);
/* the player of a device, or -1 */
int dos_split_find(int device, unsigned short pad);
void dos_split_leave(int index);
void dos_split_clear(void);
void dos_split_set(int index, int color, int team);
/* the port the keyboard and mouse play: 0 when off, -1 where no player has
them */
int dos_split_keyboard_port(void);

#endif /* __DOS_SPLIT_H */
