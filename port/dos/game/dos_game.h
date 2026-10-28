/*
DOS_GAME.H

The game a session plays, as the menu (dos_menu.c) chooses it and dos_game.c
starts it: the campaign, a multiplayer game with bots (dos_bots.c) or
Firefight (dos_firefight.c), after the N64 port's menu choice.
*/

#ifndef __DOS_GAME_H
#define __DOS_GAME_H

enum
{
	DOS_MODE_CAMPAIGN,
	DOS_MODE_MULTIPLAYER,
	DOS_MODE_FIREFIGHT,
	/* a system link game (dos_network.c) */
	DOS_MODE_NETWORK,
	DOS_MODE_COUNT
};

enum
{
	DOS_WEAPONS_MAP,
	DOS_WEAPONS_PISTOL_RIFLE,
	DOS_WEAPONS_COUNT
};

enum
{
	DOS_VEHICLES_MAP,
	DOS_VEHICLES_NONE,
	DOS_VEHICLES_COUNT
};

#define DOS_MAXIMUM_BOTS 15

struct dos_game_choice
{
	int mode;
	/* the campaign's level (0 a10 to 9 d40) and its difficulty (0 easy to
	3 legendary; Firefight's too) */
	int level;
	int difficulty;
	/* a multiplayer game's (and Firefight's map) */
	int map;
	int game_type;
	int score; /* to win; 0: the game type's own */
	int bots;
	int bot_skill; /* 0 easy to 3 legendary */
	int team; /* the player's, in a team game: 0 red, 1 blue */
	int weapons;
	int vehicles;
};

/* the tables (dos_game.c) */
struct dos_level
{
	const char *folder;
	const char *name;
};

struct dos_game_type
{
	const char *variant;
	const char *name;
	int teams;
	const char *unit;
	int scores[5];
	int default_score;
	int bot_match;
};

extern const struct dos_level dos_campaign_levels[];
extern const int dos_campaign_level_count;
/* the Xbox's multiplayer maps (the first DOS_XBOX_MULTIPLAYER_MAPS), then
the Halo Custom Edition maps in MAPS_CE (dos_ce.c: folder <file>@ce, name
marked [CE]) */
#define DOS_XBOX_MULTIPLAYER_MAPS 13
#define DOS_MAXIMUM_MULTIPLAYER_MAPS (DOS_XBOX_MULTIPLAYER_MAPS + 64)
extern struct dos_level dos_multiplayer_maps[DOS_MAXIMUM_MULTIPLAYER_MAPS];
extern int dos_multiplayer_map_count;
/* a multiplayer map's picture (ui.map's mp_map_grafix), or NONE; whether
it is a Custom Edition map */
int dos_map_picture(int map);
int dos_map_is_custom_edition(int map);

/* Halo Custom Edition's maps (dos_ce.c) */
int dos_ce_map_name_is(const char *map_name);
int dos_ce_list_multiplayer_maps(struct dos_level *maps, int count, int maximum);
int dos_ce_read(long tag_index, unsigned long offset, void *buffer, unsigned long size);
extern const struct dos_game_type dos_game_types[];
extern const int dos_game_type_count;
extern const char *const dos_difficulty_names[4];

/* the choice the menu edits (and the game played last) */
extern struct dos_game_choice dos_choice;

/* whether a map is in the maps folder */
int dos_map_present(const char *folder);
/* the chosen game, loaded once the menu's frame is over (main.c's
main_change_map_name) */
void dos_game_begin(void);

/* the game that ended, for the menu: up to count lines of text */
int dos_game_results(char lines[][64], int count);

/* the menu (dos_menu.c) */
void dos_menu_open(int results);
void dos_menu_frame(void);
int dos_menu_active(void);
/* the in-game pause (ESCAPE) */
int dos_pause_active(void);
/* while the menu or the pause has the keyboard (and until its keys are let
go): the game's controller reads nothing */
int dos_menu_has_input(void);

/* Firefight (dos_firefight.c) and the bots (dos_bots.c) */
void dos_firefight_begin(int difficulty, int player_count);
void dos_firefight_stop(void);
int dos_firefight_active(void);
int dos_firefight_status(char *text, int size);
int dos_firefight_results(char lines[][64], int count);
void dos_bots_begin(int count, int skill, int teams);
void dos_bots_stop(void);
const char *dos_bots_display_name(long player_index);
void dos_bots_prepare_lobby(void);

/* system link play (dos_network.c) */
enum
{
	DOS_NETWORK_OFF,
	DOS_NETWORK_HOSTING,
	DOS_NETWORK_SEARCHING,
	DOS_NETWORK_JOINING,
	DOS_NETWORK_JOINED,
};

#define DOS_NETWORK_MAXIMUM_GAMES 96
#define DOS_NETWORK_MAXIMUM_LOBBY_PLAYERS 16

/* where a listed game is and whose: on the LAN; on the online list
(ChupathingyCE's and OpenCE's share it), a ChupathingyCE host's, an OpenCE
host's, or one the list does not tell apart (both announce network version
39: DOS_GAME_EITHER); a retail Custom Edition server (joined by
dos_ce_client.c); a Halo PC one (listed only with the setting browser_pc,
to look at) */
enum
{
	DOS_GAME_LAN,
	DOS_GAME_CHUPATHINGY,
	DOS_GAME_CUSTOM_EDITION,
	DOS_GAME_RETAIL_PC,
	DOS_GAME_OPENCE,
	DOS_GAME_EITHER
};

struct dos_network_game
{
	char name[32];
	char map[32];
	char game_type[24];
	int players, maximum_players;
	int joinable, in_progress, compatible;
	/* from the online game list (not reached yet) */
	int internet;
	/* DOS_GAME_*; a retail server's answer time (ms) and whether it asks
	for a password */
	int kind, ping_ms, password;
	/* the network version it is listed under (0 for none), whether it is a
	dedicated server's, and (a Custom Edition server's) whether its map is
	in MAPS_CE */
	int version, dedicated, has_map;
	/* what it is known by from one look to the next (the list keeps its
	order by it) */
	char key[40];
};

/* the join page's filters and order (settings browser_show, browser_full,
browser_empty, browser_joinable, browser_sort; dos_menu.c's FILTERS page) */
enum
{
	DOS_SHOW_ALL,
	DOS_SHOW_ONLINE,
	DOS_SHOW_CHUPATHINGY,
	DOS_SHOW_OPENCE,
	DOS_SHOW_CUSTOM_EDITION,
	DOS_SHOW_LAN,
	NUMBER_OF_DOS_SHOWS
};
enum
{
	DOS_SORT_PLAYERS,
	DOS_SORT_PING,
	DOS_SORT_NAME,
	DOS_SORT_MAP,
	NUMBER_OF_DOS_SORTS
};

struct dos_network_lobby
{
	int hosting;
	int team_game;
	char name[16];
	char map[32];
	char game_type[24];
	int player_count;
	char players[DOS_NETWORK_MAXIMUM_LOBBY_PLAYERS][12];
	int teams[DOS_NETWORK_MAXIMUM_LOBBY_PLAYERS];
	int seconds_to_start;
	/* hosting: the file its invite link was written to (or nothing) */
	char invite_file[24];
};

int dos_network_mode(void);
const char *dos_network_message(void);
int dos_network_host(void);
int dos_network_search(void);
int dos_network_games(struct dos_network_game *games, int maximum);
int dos_network_join(int listed);
/* the list put in order again (its order holds still otherwise, once the
first few seconds of a search have filled it: new games join its end) */
void dos_network_resort(void);
/* the games the list's tab and filters hide (the last look's) */
int dos_network_hidden(void);
/* the game QUICK JOIN went for, as listed */
void dos_network_joining(char *name, int name_size, char *map, int map_size);

/* the Custom Edition client (dos_ce_client.c): a server being joined or
played on, what it is doing, whether its map is downloading */
int dos_ce_active(void);
const char *dos_ce_status(void);
int dos_ce_downloading(void);
void dos_ce_leave(void);
/* QUICK JOIN: the best game to join among those found (the most players
short of full, the shortest answer time, of the kinds shown) joined now, or
once a search has had a few seconds (start_search: a search begun first);
FALSE (and the network's word why) if there is none */
int dos_network_quick_join(int start_search);
void dos_network_start(void);
void dos_network_leave(void);
int dos_network_lobby(struct dos_network_lobby *lobby);
void dos_network_update(void);

#endif /* __DOS_GAME_H */
