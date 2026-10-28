/*
DOS_SPLIT.C

Split screen's players (dos_split.h): joined on the menu's SPLIT SCREEN
page, each on a device, in the order they joined (the first is player 1,
whose name and colour are the settings'). A player who leaves takes their
place with them: the others move up.
*/

#include <string.h>

#include "dos_split.h"

/* the colours a joining player starts with: red, blue, green, yellow
(Halo's colour indices) */
static const int join_colors[DOS_SPLIT_PLAYERS] = { 2, 3, 6, 5 };

static struct
{
	int count;
	struct dos_split_player players[DOS_SPLIT_PLAYERS];
} split;

int dos_split_count(void)
{
	return split.count;
}

const struct dos_split_player *dos_split_player(int index)
{
	return index >= 0 && index < split.count ? &split.players[index] : NULL;
}

int dos_split_find(int device, unsigned short pad)
{
	for (int i = 0; i < split.count; i++)
	{
		if (split.players[i].device == device && (device != DOS_DEVICE_PAD || split.players[i].pad == pad))
			return i;
	}
	return -1;
}

int dos_split_join(int device, unsigned short pad)
{
	struct dos_split_player *player;

	if (split.count >= DOS_SPLIT_PLAYERS || device == DOS_DEVICE_NONE || dos_split_find(device, pad) >= 0)
		return -1;
	player = &split.players[split.count];
	memset(player, 0, sizeof(*player));
	player->device = device;
	player->pad = pad;
	player->color = join_colors[split.count];
	player->team = split.count & 1;
	return split.count++;
}

void dos_split_leave(int index)
{
	if (index < 0 || index >= split.count)
		return;
	memmove(&split.players[index], &split.players[index + 1],
		(size_t)(split.count - index - 1) * sizeof(split.players[0]));
	split.count--;
}

void dos_split_clear(void)
{
	split.count = 0;
}

void dos_split_set(int index, int color, int team)
{
	if (index < 0 || index >= split.count)
		return;
	split.players[index].color = color;
	split.players[index].team = team;
}

int dos_split_keyboard_port(void)
{
	if (!split.count)
		return 0;
	return dos_split_find(DOS_DEVICE_KEYBOARD, 0);
}
