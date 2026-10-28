/*
DOS_NETWORK.C

System link play in the DOS and Windows 95 builds: a game hosted on the
LAN, or one another machine hosts joined, from the builds' own menu
(dos_menu.c) instead of the Xbox shell's widgets, which these builds leave
out. The game's own networking (source/networking, ChupathingyCE's
netcode) plays it, so ChupathingyCE's PC builds and these see each other's
games and play together.

What the shell's screens did, done here as ChupathingyCE's automated test
(port/linux/game/network_test.c) does it:

- hosting: a server and its own client (player_ui_fast_setup_network_server
  without its pregame screen), the game's map and variant a moment later,
  then this machine's player; the host's START starts the game, as the
  pregame screen's;
- joining: a client searching (its broadcasts find the LAN's games: listed
  by dos_network_listed_game), the one chosen joined, then this machine's
  player on a team;
- between games the lobby is the menu's (the connection is kept: main.c
  main_menu_load, HALO_DOS), and leaving it ends the connection.

Over the internet too, as ChupathingyCE plays it (its port/linux/src/p2p*.c,
built in, polled from here each frame: dos_p2p_poll): a hosted game is
listed in the online game list (network.host_public, its default), and the
join page lists the list's games beside the LAN's. Joining one opens the
tunnel to its host (p2p_join_invite); its game then turns up among the LAN's
(the host's identifier in its XNADDR), and is joined as one of them.

The player's name and armour colour are the settings' (dos_network_profile:
player_ui_get_active_player_profile, HALO_DOS).
*/

#include "cseries.h"

#include "game/game.h"
#include "game/game_engine.h"
#include "interface/player_ui.h"
#include "main/main.h"
#include "networking/network_client_manager.h"
#include "networking/network_game_globals.h"
#include "networking/network_game_manager.h"
#include "networking/network_server_manager.h"

#include "halo_system.h"
#include "dos_game.h"
#include "dos_split.h"
#include "../../../chupathingyce/port/linux/src/p2p.h"
#include "../retail/retail_browse.h"

/* ChupathingyCE's internet play, polled (p2p.c, HALO_DOS) */
void dos_p2p_poll(void);
int config_boolean(const char *name);

#include <stdio.h>
#include <string.h>

/* the settings (port/dos/xdk/dos_settings.c) */
const char *dos_setting(const char *name, const char *default_value);
/* the Custom Edition client (dos_ce_client.c) */
int dos_ce_join(uint32_t address, uint16_t port, const char *password);
void dos_ce_frame(void);
int dos_ce_active(void);
void dos_ce_leave(void);
const char *dos_ce_status(void);
long dos_setting_integer(const char *name, long default_value);

/* network_client_manager.c's (HALO_DOS) */
boolean dos_network_listed_game(long index, wchar_t *name, char *map, int map_size, short *engine_type, short *players,
	short *maximum_players, boolean *open, boolean *teams, boolean *compatible, boolean *in_progress);
long dos_network_join_listed_game(long index);
long dos_network_client_state(short *error_code);
boolean dos_network_listed_identifier(long index, unsigned char *identifier);

/* (the online list holds 256: as many as the page shows) */
#define MAXIMUM_INTERNET_GAMES DOS_NETWORK_MAXIMUM_GAMES

/* ce_maps.c's: whether a Custom Edition map is in MAPS_CE (0) */
int ce_session_map_checksum(const char *map_name, uint32_t *checksum);

enum
{
	/* network_client_manager.c's client states */
	CLIENT_SEARCHING,
	CLIENT_JOINING,
	CLIENT_PREGAME,
	CLIENT_INGAME,
	CLIENT_POSTGAME,
};

static struct
{
	int mode;
	unsigned long long since; /* microseconds: the mode's start */
	int map_set;
	int player_added;
	int team_set;
	int listed_index[DOS_NETWORK_MAXIMUM_GAMES];
	char message[64];
	/* the online list's games, and which listed lines are theirs */
	struct p2p_listing internet[MAXIMUM_INTERNET_GAMES];
	int internet_count;
	int listed_internet[DOS_NETWORK_MAXIMUM_GAMES];
	/* which listed lines are retail Halo PC / Custom Edition servers
	(port/dos/retail), and where those are: a Custom Edition server is
	joined by dos_ce_client.c, a Halo PC one only listed */
	int listed_retail[DOS_NETWORK_MAXIMUM_GAMES];
	uint32_t retail_address[DOS_NETWORK_MAXIMUM_GAMES];
	uint16_t retail_port[DOS_NETWORK_MAXIMUM_GAMES];
	int retail_custom_edition[DOS_NETWORK_MAXIMUM_GAMES];
	/* the Custom Edition client's last status said */
	char ce_status[96];
	/* an online game being reached: its host's identifier, since when */
	unsigned char target[6];
	int targeting;
	unsigned long long target_since;
	/* this host's invite link written out (net_invite_file) */
	int invite_written;
	/* a joined client's state last said (the lobby's word) */
	long client_state;
	/* QUICK JOIN waiting for its search to fill the list, and the game it
	went for */
	int quick_join;
	char joining_name[32], joining_map[32];
} net;

/* the list sorted freely for this long after a search starts */
#define LIST_SETTLING_US 4000000ULL
/* QUICK JOIN after a search of its own: the list's time to fill */
#define QUICK_JOIN_WAIT_US 5000000ULL

/* the order held: the lines' keys as shown */
static struct
{
	char keys[DOS_NETWORK_MAXIMUM_GAMES][40];
	int count;
	/* sorted freely until then (microseconds), or at the next look */
	unsigned long long settling_until;
	int resort;
	/* the games the tab and the filters hid at the last look */
	int hidden;
} order;

/* the Custom Edition maps looked for in MAPS_CE (ce_maps.c), by name */
static struct
{
	char name[32];
	int here;
} ce_maps[48];
static int ce_map_count;

static void say(const char *text)
{
	char line[96];

	snprintf(net.message, sizeof(net.message), "%s", text);
	snprintf(line, sizeof(line), "network: %s", text);
	system_log(line);
}

static double seconds_since(void)
{
	return (double)(system_microseconds() - net.since) / 1000000.0;
}

static void set_mode(int mode)
{
	/* (the retail servers looked for only while the games are) */
	if (net.mode == DOS_NETWORK_SEARCHING && mode != DOS_NETWORK_SEARCHING)
		retail_browse_stop();
	net.mode = mode;
	net.since = system_microseconds();
	net.map_set = FALSE;
	net.player_added = FALSE;
	net.team_set = FALSE;
	net.invite_written = FALSE;
	net.client_state = NONE;
}

/* the file a host's invite link goes to (INVITE.TXT beside the program,
unless the setting net_invite_file names another): its players join with
it in their JOIN.TXT (dos_p2p_platform.c), or on ChupathingyCE's command
line or Join by link */
static const char *invite_file_name(void)
{
	return dos_setting("net_invite_file", "INVITE.TXT");
}

static void write_invite(void)
{
	const char *file_name = invite_file_name();
	char link[128], path[300];
	FILE *file;

	if (net.invite_written || !*file_name || !p2p_invite_link(link, sizeof(link)) || !link[0])
		return;
	net.invite_written = TRUE;
	if (strchr(file_name, '\\') || strchr(file_name, ':'))
		snprintf(path, sizeof(path), "%s", file_name);
	else
		snprintf(path, sizeof(path), "%s\\%s", system_program_directory(), file_name);
	if ((file = fopen(path, "w")) == NULL)
		return;
	fputs(link, file);
	fputc('\n', file);
	fclose(file);
}

int dos_network_mode(void)
{
	return net.mode;
}

const char *dos_network_message(void)
{
	return net.message;
}

/* player_ui_get_active_player_profile (HALO_DOS): the player's name and
armour from the settings */
void dos_network_profile(short local_player_index, wchar_t *name, short *color)
{
	const char *text = dos_setting("player_name", "Player");
	char numbered[12];
	int c;

	/* (split screen's players past the first: "Player 2" and on, in the
	armour they chose as they joined) */
	if (local_player_index > 0)
	{
		snprintf(numbered, sizeof(numbered), "Player %d", local_player_index + 1);
		text = numbered;
	}
	for (c = 0; c < 11 && text[c]; c++)
		name[c] = (wchar_t)(unsigned char)text[c];
	name[c] = 0;
	*color = (short)PIN(dos_setting_integer("player_color", 2), 0, 17);
	if (dos_split_count() >= 2 && local_player_index >= 0 && local_player_index < dos_split_count())
		*color = (short)dos_split_player(local_player_index)->color;
}

/* the players this machine plays a network game with: split screen's (two
to four), else one */
static int local_players(void)
{
	return dos_split_count() >= 2 ? dos_split_count() : 1;
}

/* the connection over, back to a game of this machine's own */
void dos_network_leave(void)
{
	/* (a Custom Edition server's game too) */
	if (dos_ce_active())
		dos_ce_leave();
	if (net.mode != DOS_NETWORK_OFF)
		system_log("network: left");
	dispose_global_network_game_client();
	dispose_global_network_game_server();
	network_game_accept_remote_connections(FALSE);
	game_connection_set(_game_connection_local);
	p2p_lobby_browse(FALSE);
	retail_browse_stop();
	p2p_set_hosting_allowed(FALSE);
	net.targeting = FALSE;
	net.quick_join = FALSE;
	set_mode(DOS_NETWORK_OFF);
	net.message[0] = 0;
}

/* a game on the LAN, of the menu's game type and map */
int dos_network_host(void)
{
	dos_network_leave();
	dos_choice.mode = DOS_MODE_NETWORK;
	/* (this machine's one player: the game is named after them,
	network_game_generate_local_machine_name) */
	player_ui_local_player_joined_multiplayer_game(0);
	main_set_multiplayer_map_name("");
	game_engine_playlist_initialize();
	network_game_accept_remote_connections(TRUE);
	/* (joinable from the internet, and listed online unless
	network.host_public is off, as ChupathingyCE's Create Game) */
	p2p_set_hosting_allowed(TRUE);
	p2p_set_hosting_public(config_boolean("network.host_public"));
	if (create_global_network_game_server() && create_global_network_game_client())
	{
		game_engine_playlist_begin();
		game_connection_set(_game_connection_network_server);
		set_mode(DOS_NETWORK_HOSTING);
		say("hosting a game");
		return TRUE;
	}
	dos_network_leave();
	say("the network is not there");
	return FALSE;
}

/* the LAN's games, heard of by a client searching */
int dos_network_search(void)
{
	dos_network_leave();
	dos_choice.mode = DOS_MODE_NETWORK;
	if (!create_global_network_game_client())
	{
		say("the network is not there");
		return FALSE;
	}
	game_connection_set(_game_connection_network_client);
	player_ui_local_player_joined_multiplayer_game(0);
	set_mode(DOS_NETWORK_SEARCHING);
	p2p_lobby_browse(TRUE);
	/* (and the retail Custom Edition servers, Halo PC's with browser_pc) */
	retail_browse_include_pc((int)dos_setting_integer("browser_pc", 0));
	retail_browse_start((unsigned long)(system_microseconds() / 1000), (uint32_t)system_microseconds());
	/* (the list sorted as it fills, then held; the maps looked for again) */
	order.count = 0;
	order.settling_until = system_microseconds() + LIST_SETTLING_US;
	ce_map_count = 0;
	say("looking for games");
	return TRUE;
}

static const char *engine_name(short engine_type)
{
	static const char *const names[] = { "", "CTF", "SLAYER", "ODDBALL", "KING", "RACE", "TERMINATOR", "STUB" };

	return engine_type >= 0 && engine_type < (short)NUMBEROF(names) ? names[engine_type] : "";
}

/* the map's folder as the menu names it (BLOOD GULCH), else the folder */
static void map_title(const char *path, char *title, int size)
{
	const char *last = strrchr(path, '\\');
	const char *folder = last ? last + 1 : path;
	int i;

	for (i = 0; i < dos_multiplayer_map_count; i++)
	{
		if (!_stricmp(dos_multiplayer_maps[i].folder, folder))
		{
			snprintf(title, size, "%s", dos_multiplayer_maps[i].name);
			return;
		}
	}
	snprintf(title, size, "%s", folder);
	for (i = 0; title[i]; i++)
	{
		if (title[i] >= 'a' && title[i] <= 'z')
			title[i] = (char)(title[i] - 'a' + 'A');
	}
}

static void narrow(const wchar_t *wide, char *text, int size)
{
	int i;

	for (i = 0; i < size - 1 && wide[i]; i++)
		text[i] = wide[i] < 127 && wide[i] >= 32 ? (char)wide[i] : '?';
	text[i] = 0;
}

/* ---------- the join page's list

Every game found, one line each: the LAN's, the online list's (whose hosts
are ChupathingyCE's or OpenCE's: one list, the same brokers), the retail
Custom Edition servers' (and Halo PC's with the setting browser_pc). The
lines pass the page's filters, and keep their places: for the first few
seconds of a search the list is sorted as it fills, then it holds still
(new games join its end, games gone leave it) until the page's REFRESH sorts
it again, so the line being read or chosen does not move away. */

/* a line, with what joining it takes */
struct found
{
	struct dos_network_game game;
	int listed_index, listed_internet, listed_retail, custom_edition;
	uint32_t address;
	uint16_t port;
	/* its place in the held order (the list's: lower first) */
	int rank;
};


static int ce_map_here(const char *name)
{
	uint32_t checksum;
	int i;

	for (i = 0; i < ce_map_count; i++)
		if (!_stricmp(ce_maps[i].name, name))
			return ce_maps[i].here;
	i = ce_map_count < (int)NUMBEROF(ce_maps) ? ce_map_count++ : (int)NUMBEROF(ce_maps) - 1;
	snprintf(ce_maps[i].name, sizeof(ce_maps[i].name), "%s", name);
	ce_maps[i].here = ce_session_map_checksum(name, &checksum) == 0;
	return ce_maps[i].here;
}

/* whose an online game is, by what its listing says: a dedicated server's
flags (ChupathingyCE's set 128, OpenCE's 32 alone), an OpenCE game type in
its stamp (ChupathingyCE's hosts leave it 0), else its network version:
ChupathingyCE's hosts announce 32 (their own), or 23, 25 or 38 (its signed
table's), OpenCE's testing builds 33 to 37; from 39 both announce the same
number and nothing in the listing tells them apart */
static int listing_kind(const struct p2p_listing *listing)
{
	if (listing->listed_flags & 128)
		return DOS_GAME_CHUPATHINGY;
	if ((listing->listed_flags & 32) || listing->kind)
		return DOS_GAME_OPENCE;
	if (listing->version <= 32 || listing->version == 38)
		return DOS_GAME_CHUPATHINGY;
	if (listing->version <= 37)
		return DOS_GAME_OPENCE;
	return DOS_GAME_EITHER;
}

static void upper(char *text)
{
	for (; *text; text++)
		*text = (char)(*text >= 'a' && *text <= 'z' ? *text - 'a' + 'A' : *text);
}

static void hex_key(char *key, int size, char prefix, const unsigned char *bytes, int count)
{
	int at = 0, i;

	key[at++] = prefix;
	for (i = 0; i < count && at + 2 < size; i++)
		at += snprintf(key + at, (size_t)(size - at), "%02X", bytes[i]);
	key[at] = 0;
}

/* the page's filters and order, read once a look (settings_read) */
static struct
{
	int show, hide_full, hide_empty, joinable_only, sort;
} filter;

static void settings_read(void)
{
	filter.show = (int)dos_setting_integer("browser_show", DOS_SHOW_ALL);
	filter.hide_full = (int)dos_setting_integer("browser_full", 0);
	filter.hide_empty = (int)dos_setting_integer("browser_empty", 0);
	filter.joinable_only = (int)dos_setting_integer("browser_joinable", 0);
	filter.sort = (int)dos_setting_integer("browser_sort", DOS_SORT_PLAYERS);
}

/* whether a line passes the page's filters */
static int shown(const struct dos_network_game *game)
{
	int full = game->maximum_players > 0 && game->players >= game->maximum_players;

	switch (filter.show)
	{
	case DOS_SHOW_ONLINE:
		if (game->kind == DOS_GAME_LAN)
			return FALSE;
		break;
	case DOS_SHOW_CHUPATHINGY:
		if (game->kind != DOS_GAME_CHUPATHINGY && game->kind != DOS_GAME_EITHER)
			return FALSE;
		break;
	case DOS_SHOW_OPENCE:
		if (game->kind != DOS_GAME_OPENCE && game->kind != DOS_GAME_EITHER)
			return FALSE;
		break;
	case DOS_SHOW_CUSTOM_EDITION:
		if (game->kind != DOS_GAME_CUSTOM_EDITION)
			return FALSE;
		break;
	case DOS_SHOW_LAN:
		if (game->kind != DOS_GAME_LAN)
			return FALSE;
		break;
	}
	if (filter.hide_full && full)
		return FALSE;
	if (filter.hide_empty && game->players <= 0)
		return FALSE;
	if (filter.joinable_only && (!game->joinable || !game->compatible || game->password || full))
	{
		return FALSE;
	}
	return TRUE;
}

/* the order chosen (setting browser_sort): by players (full lobbies first,
regardless of version, then the most players, empty ones last), by answer
time, by name, or by map */
static int listed_before(const struct dos_network_game *a, const struct dos_network_game *b)
{
	int a_full = a->maximum_players > 0 && a->players >= a->maximum_players;
	int b_full = b->maximum_players > 0 && b->players >= b->maximum_players;
	int by_name = strcmp(a->name, b->name), by_map = strcmp(a->map, b->map);

	switch (filter.sort)
	{
	case DOS_SORT_PING:
		if ((a->ping_ms < 0) != (b->ping_ms < 0))
			return a->ping_ms >= 0;
		if (a->ping_ms != b->ping_ms)
			return a->ping_ms < b->ping_ms;
		break;
	case DOS_SORT_NAME:
		if (by_name)
			return by_name < 0;
		break;
	case DOS_SORT_MAP:
		if (by_map)
			return by_map < 0;
		break;
	}
	if (a_full != b_full)
		return a_full;
	if (a->players != b->players)
		return a->players > b->players;
	if ((a->ping_ms < 0) != (b->ping_ms < 0))
		return a->ping_ms >= 0;
	if (a->ping_ms != b->ping_ms)
		return a->ping_ms < b->ping_ms;
	return by_name < 0;
}

/* (stable: equal lines keep the order they came in) */
static void sort_found(struct found *found, int count, int by_rank)
{
	for (int i = 1; i < count; i++)
	{
		struct found line = found[i];
		int j = i;

		while (j > 0 && (by_rank ? line.rank < found[j - 1].rank : listed_before(&line.game, &found[j - 1].game)))
		{
			found[j] = found[j - 1];
			j--;
		}
		found[j] = line;
	}
}

/* every game found, unfiltered, in the order found */
static int find_games(struct found *found, int maximum)
{
	int count = 0;
	long index;

	for (index = 0; index < 64 && count < maximum; index++)
	{
		struct found *line = &found[count];
		wchar_t name[16];
		char map[128];
		short engine_type, players, maximum_players;
		boolean open, teams, compatible, in_progress;
		unsigned char identifier[6];

		if (!dos_network_listed_game(index, name, map, sizeof(map), &engine_type, &players, &maximum_players, &open,
			&teams, &compatible, &in_progress))
		{
			continue;
		}
		memset(line, 0, sizeof(*line));
		narrow(name, line->game.name, sizeof(line->game.name));
		map_title(map, line->game.map, sizeof(line->game.map));
		snprintf(line->game.game_type, sizeof(line->game.game_type), "%s%s", teams ? "TEAM " : "",
			engine_name(engine_type));
		line->game.players = players;
		line->game.maximum_players = maximum_players;
		line->game.joinable = open && compatible;
		line->game.in_progress = in_progress;
		line->game.compatible = compatible;
		line->game.kind = DOS_GAME_LAN;
		line->game.ping_ms = -1;
		line->game.has_map = TRUE;
		if (dos_network_listed_identifier(index, identifier))
			hex_key(line->game.key, sizeof(line->game.key), 'L', identifier, 6);
		else
			snprintf(line->game.key, sizeof(line->game.key), "N%ld %s", index, line->game.name);
		line->listed_index = (int)index;
		line->listed_internet = -1;
		count++;
	}
	/* the online list's games not on the LAN's list already (a reached
	host's game is there, by its identifier) */
	net.internet_count = p2p_lobby_games(net.internet, MAXIMUM_INTERNET_GAMES);
	for (int i = 0; i < net.internet_count && count < maximum; i++)
	{
		const struct p2p_listing *listing = &net.internet[i];
		struct found *line = &found[count];
		char reached[40];
		int known = FALSE;

		hex_key(reached, sizeof(reached), 'L', listing->identifier, 6);
		for (int k = 0; k < count && !known; k++)
			known = found[k].listed_internet < 0 && !strcmp(found[k].game.key, reached);
		if (known)
		{
			/* (its line the LAN's, under the online list's word on whose) */
			for (int k = 0; k < count; k++)
				if (!strcmp(found[k].game.key, reached))
					found[k].game.kind = listing_kind(listing);
			continue;
		}
		memset(line, 0, sizeof(*line));
		snprintf(line->game.name, sizeof(line->game.name), "%s", listing->name);
		map_title(listing->map, line->game.map, sizeof(line->game.map));
		snprintf(line->game.game_type, sizeof(line->game.game_type), "%s", listing->gametype);
		upper(line->game.name);
		upper(line->game.game_type);
		line->game.players = listing->player_count;
		line->game.maximum_players = listing->maximum_player_count;
		line->game.joinable = listing->open && !listing->failed && listing->invite[0];
		line->game.in_progress = listing->in_progress;
		line->game.compatible = TRUE;
		line->game.internet = TRUE;
		line->game.kind = listing_kind(listing);
		line->game.ping_ms = listing->ping;
		line->game.password = listing->locked;
		line->game.version = listing->version;
		line->game.dedicated = listing->dedicated;
		line->game.has_map = TRUE;
		/* (by its host's identifier, which every listing has: the key hash
		is kept only for a game with a password, open ones' are zeros) */
		hex_key(line->game.key, sizeof(line->game.key), 'I', listing->identifier, 6);
		line->listed_index = -1;
		line->listed_internet = i;
		count++;
	}
	/* then the retail Custom Edition servers (joined by dos_ce_client.c)
	and Halo PC's when asked for (only listed) */
	{
		static struct retail_browse_server retail[DOS_NETWORK_MAXIMUM_GAMES];
		int room = maximum - count < (int)NUMBEROF(retail) ? maximum - count : (int)NUMBEROF(retail);
		int retail_count = room > 0 ? retail_browse_servers(retail, room) : 0;
		int pc = (int)dos_setting_integer("browser_pc", 0);

		for (int i = 0; i < retail_count; i++)
		{
			struct found *line = &found[count];

			if (retail[i].kind == _retail_pc && !pc)
				continue;
			memset(line, 0, sizeof(*line));
			snprintf(line->game.name, sizeof(line->game.name), "%s", retail[i].name[0] ? retail[i].name : "(NO NAME)");
			map_title(retail[i].map, line->game.map, sizeof(line->game.map));
			snprintf(line->game.game_type, sizeof(line->game.game_type), "%s", retail[i].game_type);
			upper(line->game.name);
			upper(line->game.map);
			upper(line->game.game_type);
			line->game.players = retail[i].players;
			line->game.maximum_players = retail[i].maximum_players;
			line->game.in_progress = FALSE;
			line->game.compatible = TRUE;
			line->game.internet = TRUE;
			line->game.kind = retail[i].kind == _retail_pc ? DOS_GAME_RETAIL_PC : DOS_GAME_CUSTOM_EDITION;
			line->game.ping_ms = retail[i].ping_ms;
			line->game.password = retail[i].password;
			line->game.dedicated = TRUE;
			/* (a Custom Edition server: joinable with its map in MAPS_CE, or
			one the join downloads first: dos_map_download.c, the setting
			map_download) */
			line->game.has_map = retail[i].kind != _retail_pc && ce_map_here(retail[i].map);
			line->game.joinable = retail[i].kind != _retail_pc &&
				(line->game.has_map || dos_setting_integer("map_download", 1));
			snprintf(line->game.key, sizeof(line->game.key), "C%08lX:%u", (unsigned long)retail[i].address,
				(unsigned)retail[i].port);
			line->listed_index = -1;
			line->listed_internet = -1;
			line->listed_retail = TRUE;
			line->address = retail[i].address;
			line->port = retail[i].port;
			line->custom_edition = retail[i].kind != _retail_pc;
			count++;
		}
	}
	return count;
}

/* the lines the page shows: filtered, in the held order (sorted while the
list settles or when asked) */
int dos_network_games(struct dos_network_game *games, int maximum)
{
	static struct found found[DOS_NETWORK_MAXIMUM_GAMES * 2];
	int count, kept = 0, i, k;

	if (net.mode != DOS_NETWORK_SEARCHING)
		return 0;
	if (maximum > DOS_NETWORK_MAXIMUM_GAMES)
		maximum = DOS_NETWORK_MAXIMUM_GAMES;
	settings_read();
	count = find_games(found, (int)NUMBEROF(found));
	for (i = 0; i < count; i++)
		if (shown(&found[i].game))
			found[kept++] = found[i];
	order.hidden = count - kept;
	count = kept;
	if (order.resort || system_microseconds() < order.settling_until)
	{
		order.resort = FALSE;
		sort_found(found, count, FALSE);
	}
	else
	{
		/* (the lines seen before in their places; new ones after them, in
		the order chosen among themselves) */
		int next = order.count;

		sort_found(found, count, FALSE);
		for (i = 0; i < count; i++)
		{
			found[i].rank = DOS_NETWORK_MAXIMUM_GAMES * 4 + i;
			for (k = 0; k < order.count; k++)
				if (!strcmp(order.keys[k], found[i].game.key))
				{
					found[i].rank = k;
					break;
				}
			if (k == order.count)
				found[i].rank = next++;
		}
		sort_found(found, count, TRUE);
	}
	if (count > maximum)
		count = maximum;
	order.count = 0;
	for (i = 0; i < count; i++)
	{
		games[i] = found[i].game;
		net.listed_index[i] = found[i].listed_index;
		net.listed_internet[i] = found[i].listed_internet;
		net.listed_retail[i] = found[i].listed_retail;
		net.retail_custom_edition[i] = found[i].custom_edition;
		net.retail_address[i] = found[i].address;
		net.retail_port[i] = found[i].port;
		snprintf(order.keys[order.count++], sizeof(order.keys[0]), "%s", found[i].game.key);
	}
	return count;
}

void dos_network_resort(void)
{
	order.resort = TRUE;
}

int dos_network_hidden(void)
{
	return net.mode == DOS_NETWORK_SEARCHING ? order.hidden : 0;
}

/* the line QUICK JOIN takes among those shown: one that can be joined (no
password, room for a player, its map here), the most players first, then
the shortest answer time; an empty game only when none has players */
static int quick_pick(const struct dos_network_game *games, int count)
{
	int best = -1;

	for (int i = 0; i < count; i++)
	{
		const struct dos_network_game *game = &games[i];
		const struct dos_network_game *chosen = best >= 0 ? &games[best] : NULL;

		/* (a Custom Edition server whose map is not here waits for a
		download: not quick) */
		if (!game->joinable || !game->compatible || game->password || game->kind == DOS_GAME_RETAIL_PC ||
			(game->kind == DOS_GAME_CUSTOM_EDITION && !game->has_map) ||
			(game->maximum_players > 0 && game->players >= game->maximum_players))
		{
			continue;
		}
		if (!chosen || game->players > chosen->players ||
			(game->players == chosen->players && game->ping_ms >= 0 &&
				(chosen->ping_ms < 0 || game->ping_ms < chosen->ping_ms)))
		{
			best = i;
		}
	}
	return best;
}

int dos_network_quick_join(int start_search)
{
	static struct dos_network_game games[DOS_NETWORK_MAXIMUM_GAMES];
	int count, best;

	if (start_search || net.mode != DOS_NETWORK_SEARCHING)
	{
		if (!dos_network_search())
			return FALSE;
		net.quick_join = TRUE;
		say("quick join: looking for a game");
		return TRUE;
	}
	net.quick_join = FALSE;
	count = dos_network_games(games, DOS_NETWORK_MAXIMUM_GAMES);
	best = quick_pick(games, count);
	if (best < 0)
	{
		say("quick join: no game to join (the filters may hide some)");
		return FALSE;
	}
	{
		char line[96];

		snprintf(line, sizeof(line), "quick join: %s", games[best].name);
		say(line);
	}
	snprintf(net.joining_name, sizeof(net.joining_name), "%s", games[best].name);
	snprintf(net.joining_map, sizeof(net.joining_map), "%s", games[best].map);
	return dos_network_join(best) || net.targeting || dos_ce_active();
}

/* the game QUICK JOIN went for, as listed (the JOINING page's words) */
void dos_network_joining(char *name, int name_size, char *map, int map_size)
{
	snprintf(name, (size_t)name_size, "%s", net.joining_name);
	snprintf(map, (size_t)map_size, "%s", net.joining_map);
}

/* the game's own error messages (ui_widget.c's, which these builds leave
out), said on the network's line */
void display_error_text_deferred(wchar_t const *text, short local_player_index)
{
	char line[96];
	int i;

	(void)local_player_index;
	for (i = 0; i < (int)sizeof(line) - 1 && text[i]; i++)
		line[i] = text[i] == '\r' ? ' ' : text[i] == '\n' ? ' ' : text[i] < 127 && text[i] >= 32 ? (char)text[i] : '?';
	line[i] = 0;
	say(line);
}

int dos_network_join(int listed)
{
	long result;

	if (net.mode != DOS_NETWORK_SEARCHING || listed < 0 || listed >= DOS_NETWORK_MAXIMUM_GAMES)
		return FALSE;
	/* a retail Custom Edition server: joined by the CE client
	(dos_ce_client.c; a password from the setting ce_password); a Halo PC
	one: listed to look at */
	if (net.listed_retail[listed])
	{
		if (!net.retail_custom_edition[listed])
		{
			say("Halo PC servers are listed to look at: only Custom Edition servers can be joined");
			return FALSE;
		}
		/* (this netcode's search and its client end first: the CE game runs
		the engine as a client with no client of this netcode's, and a
		searching one left over kept the game on its loading screen) */
		{
			uint32_t address = net.retail_address[listed];
			uint16_t port = net.retail_port[listed];

			dos_network_leave();
			if (!dos_ce_join(address, port, dos_setting("ce_password", "")))
			{
				say(dos_ce_status());
				return FALSE;
			}
		}
		say("joining the Custom Edition server");
		/* (not the lobby page, which is this netcode's: the menu closes when
		the CE server's map is up) */
		return FALSE;
	}
	/* an online game: its host reached through the tunnel first; joined
	when its game turns up among the LAN's (dos_network_update) */
	if (net.listed_internet[listed] >= 0)
	{
		const struct p2p_listing *listing = &net.internet[net.listed_internet[listed]];

		if (!listing->invite[0] || !p2p_join_invite(listing->invite))
		{
			say("that game cannot be joined");
			return FALSE;
		}
		memcpy(net.target, listing->identifier, sizeof(net.target));
		net.targeting = TRUE;
		net.target_since = system_microseconds();
		say("reaching the host over the internet");
		return FALSE;
	}
	result = dos_network_join_listed_game(net.listed_index[listed]);
	if (result > 0)
	{
		set_mode(DOS_NETWORK_JOINING);
		say("joining");
		return TRUE;
	}
	say(result < 0 ? "that game cannot be joined" : "not now: try again");
	return FALSE;
}

/* the host's START: the game begins (its countdown at 3 seconds) */
void dos_network_start(void)
{
	short error_code;

	if (net.mode == DOS_NETWORK_HOSTING && net.player_added && dos_network_client_state(&error_code) == CLIENT_PREGAME)
	{
		network_game_client_request_immediate_start();
		say("starting");
	}
}

/* the lobby: the game's name, map and variant, and its players */
int dos_network_lobby(struct dos_network_lobby *lobby)
{
	struct network_game *game = network_game_get_game();
	struct network_game_client *client = global_network_game_client_get();
	int i;

	memset(lobby, 0, sizeof(*lobby));
	if (net.mode == DOS_NETWORK_OFF || net.mode == DOS_NETWORK_SEARCHING || !game)
		return FALSE;
	lobby->hosting = net.mode == DOS_NETWORK_HOSTING;
	lobby->team_game = game->variant.universal_variant.teams;
	narrow(game->name, lobby->name, sizeof(lobby->name));
	map_title(game->map.name, lobby->map, sizeof(lobby->map));
	narrow(game->variant.human_readable_game_description, lobby->game_type, sizeof(lobby->game_type));
	/* (the built-in variants have no description: their engine's name) */
	if (!lobby->game_type[0])
	{
		snprintf(lobby->game_type, sizeof(lobby->game_type), "%s%s", game->variant.universal_variant.teams ? "TEAM " :
			"", engine_name((short)game->variant.game_engine_index));
	}
	lobby->player_count = PIN(game->player_count, 0, DOS_NETWORK_MAXIMUM_LOBBY_PLAYERS);
	for (i = 0; i < lobby->player_count; i++)
	{
		narrow(game->players[i].name, lobby->players[i], sizeof(lobby->players[i]));
		lobby->teams[i] = game->players[i].team_index;
	}
	lobby->seconds_to_start = client ? network_game_client_get_seconds_to_game_start(client) : 0;
	if (lobby->hosting && net.invite_written)
		snprintf(lobby->invite_file, sizeof(lobby->invite_file), "%s", invite_file_name());
	return TRUE;
}

/* whether this machine's player of that controller is in the game's
settings yet */
static boolean local_player_in(int controller)
{
	struct network_game *game = network_game_get_game();
	struct network_game_client *client = global_network_game_client_get();
	short machine;
	int i;

	if (!game || !client)
		return FALSE;
	machine = network_game_client_get_machine_index(client);
	for (i = 0; i < game->player_count && i < (int)NUMBEROF(game->players); i++)
	{
		if (game->players[i].machine_index == machine && game->players[i].controller_index == controller)
			return TRUE;
	}
	return FALSE;
}

/* ... and all of this machine's: each asked for until it is (split
screen's too: the host takes a machine's players one by one, as the Xbox's
system link does) */
static boolean own_players_in(void)
{
	boolean all = TRUE;

	for (int controller = 0; controller < local_players(); controller++)
	{
		if (!local_player_in(controller))
		{
			network_game_client_add_player(global_network_game_client_get(), (short)controller);
			all = FALSE;
		}
	}
	return all;
}

static boolean own_player_in(void)
{
	return local_player_in(0);
}

/* the game type and map the menu chose, as the server's (dos_game.c's
variants) */
static void host_settings(void)
{
	struct network_game_server *server = global_network_game_server_get();
	const struct dos_game_type *type = &dos_game_types[PIN(dos_choice.game_type, 0, dos_game_type_count - 1)];
	const char *folder = dos_multiplayer_maps[PIN(dos_choice.map, 0, dos_multiplayer_map_count - 1)].folder;
	struct game_variant variant, temporary_variant;
	struct game_variant *found;
	char path[128], variant_name[32];
	const char *name = type->variant;
	int length = (int)strlen(name);

	/* (the Pro variants are their plain ones: starting weapons are the
	menu's own game's) */
	snprintf(variant_name, sizeof(variant_name), "%.*s",
		length > 4 && !strcmp(name + length - 4, "_pro") ? length - 4 : length, name);
	found = game_engine_get_variant_by_name(&temporary_variant, variant_name);
	if (!found)
		found = game_engine_get_variant_by_name(&temporary_variant, "slayer");
	variant = *found;
	if (dos_choice.score > 0)
		variant.universal_variant.score_to_win = dos_choice.score;
	if (dos_choice.vehicles == DOS_VEHICLES_NONE)
		variant.universal_variant.vehicle_set = 1;
	snprintf(path, sizeof(path), "levels\\test\\%s\\%s", folder, folder);
	network_game_server_change_map_name(server, path);
	player_ui_set_game_variant(&variant);
	network_game_server_change_game_variant(server, &variant);
	{
		char line[160];

		snprintf(line, sizeof(line), "network: hosting %s, %s", path, variant_name);
		system_log(line);
	}
}

/* every menu frame (dos_menu.c) and game frame: the steps that wait on the
network, and the connection's end */
void dos_network_update(void)
{
	short error_code = 0;
	long state;

	/* a Custom Edition server's game (dos_ce_client.c), beside this
	netcode's: its steps, and what it has to say */
	dos_ce_frame();
	if (strcmp(dos_ce_status(), net.ce_status))
	{
		snprintf(net.ce_status, sizeof(net.ce_status), "%s", dos_ce_status());
		if (net.ce_status[0])
			say(net.ce_status);
	}
	if (dos_ce_active())
		return;
	dos_p2p_poll();
	if (net.mode == DOS_NETWORK_OFF)
		return;
	if (net.mode == DOS_NETWORK_SEARCHING)
		retail_browse_update((unsigned long)(system_microseconds() / 1000));
	/* QUICK JOIN's own search: the best game once the list has filled (a
	Custom Edition server's game ends this netcode's search) */
	if (net.quick_join && net.mode == DOS_NETWORK_SEARCHING && !net.targeting &&
		system_microseconds() - net.since >= QUICK_JOIN_WAIT_US)
	{
		dos_network_quick_join(FALSE);
		if (dos_ce_active() || net.mode == DOS_NETWORK_OFF)
			return;
	}
	if (net.targeting && net.mode == DOS_NETWORK_SEARCHING)
	{
		/* (its game among the LAN's: joined) */
		for (long index = 0; index < 64; index++)
		{
			unsigned char identifier[6];

			if (dos_network_listed_identifier(index, identifier) && !memcmp(identifier, net.target, 6))
			{
				long result = dos_network_join_listed_game(index);

				net.targeting = FALSE;
				if (result > 0)
				{
					set_mode(DOS_NETWORK_JOINING);
					say("joining");
				}
				else
					say(result < 0 ? "that game cannot be joined" : "not now: try again");
				break;
			}
		}
		if (net.targeting && system_microseconds() - net.target_since > 30000000ULL)
		{
			net.targeting = FALSE;
			p2p_lobby_mark_failed(net.target);
			say("the host could not be reached");
		}
	}
	if (!global_network_game_client_get())
	{
		set_mode(DOS_NETWORK_OFF);
		game_connection_set(_game_connection_local);
		say("the connection was lost");
		return;
	}
	state = dos_network_client_state(&error_code);
	switch (net.mode)
	{
	case DOS_NETWORK_HOSTING:
		/* (its invite link, once it has one, for players to join a game
		that is not listed, or before the list shows it) */
		write_invite();
		if (!global_network_game_server_get())
		{
			dos_network_leave();
			say("the game ended");
			break;
		}
		if (!net.map_set && seconds_since() >= 0.5)
		{
			host_settings();
			net.map_set = TRUE;
		}
		/* this machine's player, asked for until the settings have them
		(as the pregame screen asks every frame: the client asks every half
		second at most) */
		if (net.map_set && seconds_since() >= 1.0 && state == CLIENT_PREGAME)
			own_players_in();
		net.player_added = own_player_in();
		break;
	case DOS_NETWORK_SEARCHING:
		break;
	case DOS_NETWORK_JOINING:
		if (state == CLIENT_SEARCHING && seconds_since() > 1.0)
		{
			/* (refused, or no answer: the search goes on) */
			set_mode(DOS_NETWORK_SEARCHING);
			say(error_code ? "the host refused" : "no answer from the host");
		}
		else if (state >= CLIENT_PREGAME)
		{
			set_mode(DOS_NETWORK_JOINED);
			say("joined");
		}
		break;
	case DOS_NETWORK_JOINED:
		if (state == CLIENT_SEARCHING)
		{
			dos_network_leave();
			say("the host went away");
			break;
		}
		/* (the lobby's word: what the joined game waits for) */
		if (state != net.client_state)
		{
			net.client_state = state;
			if (state == CLIENT_PREGAME)
				say("joined: waiting for the host to start");
			else if (state == CLIENT_INGAME)
				say("joined: in the host's game");
			else if (state == CLIENT_POSTGAME)
				say("joined: the host's game is ending");
		}
		if (state == CLIENT_PREGAME)
			own_players_in();
		net.player_added = own_player_in();
		/* (the other team from the host's: a team game needs both) */
		if (net.player_added && !net.team_set && seconds_since() >= 2.0 && state == CLIENT_PREGAME)
			net.team_set = network_game_client_set_team(NONE);
		break;
	}
}

/* ---------- testing without the menu */

/* the settings net=host or net=join (with net_map, game, net_players,
net_start): once the menu is up, this machine hosts net_map (the game
variant game) and starts the game when net_players are in (or net_start
seconds after it has a player of its own, if sooner), or joins the first
game it finds that it can, as a player choosing them in the menu would */
void dos_network_autotest(int menu_up)
{
	static int done;
	static unsigned long long menu_since;
	const char *test = dos_setting("net", "");
	int i;

	if (!*test || !menu_up)
	{
		menu_since = 0;
		return;
	}
	if (!menu_since)
		menu_since = system_microseconds();
	if (system_microseconds() - menu_since < 2000000ULL)
		return;
	if (!done)
	{
		done = TRUE;
		if (!_stricmp(test, "host"))
		{
			const char *map = dos_setting("net_map", "bloodgulch");

			for (i = 0; i < dos_multiplayer_map_count; i++)
			{
				if (!_stricmp(map, dos_multiplayer_maps[i].folder))
					dos_choice.map = i;
			}
			for (i = 0; i < dos_game_type_count; i++)
			{
				if (!_stricmp(dos_setting("game", "slayer"), dos_game_types[i].variant))
				{
					dos_choice.game_type = i;
					break;
				}
			}
			dos_choice.score = (int)dos_setting_integer("score", 0);
			dos_network_host();
		}
		else
		{
			dos_network_search();
		}
		return;
	}
	if (net.mode == DOS_NETWORK_HOSTING && net.player_added)
	{
		struct dos_network_lobby lobby;
		static int started;

		short error_code;

		/* (again once a game has been played: a test host runs game after
		game) */
		if (dos_network_client_state(&error_code) == CLIENT_INGAME)
			started = FALSE;
		if (!started && dos_network_lobby(&lobby) &&
			(lobby.player_count >= dos_setting_integer("net_players", 2) ||
			seconds_since() >= (double)dos_setting_integer("net_start", 60)))
		{
			started = TRUE;
			dos_network_start();
		}
	}
	if (net.mode == DOS_NETWORK_SEARCHING)
	{
		struct dos_network_game games[DOS_NETWORK_MAXIMUM_GAMES];
		int count = dos_network_games(games, DOS_NETWORK_MAXIMUM_GAMES);

		for (i = 0; i < count; i++)
		{
			/* (never one from the online list: other people's games; a test
			joins its own host on the LAN or by its invite) */
			if (games[i].joinable && !games[i].internet)
			{
				char line[96];

				snprintf(line, sizeof(line), "network test: joining %s (%s, %s, %d players)", games[i].name,
					games[i].map, games[i].game_type, games[i].players);
				system_log(line);
				dos_network_join(i);
				break;
			}
		}
	}
}
