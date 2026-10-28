/*
DOS_GAME.C

The game-side half of the DOS and Windows 95 platform layer, compiled like
the game's own units (it sees the game's headers), after the N64 build's
port/n64/game/n64_game.c:

- the game a session plays: the menu's choice (dos_menu.c) of a campaign
  level, a multiplayer game with bots (dos_bots.c) or Firefight
  (dos_firefight.c), loaded as the Xbox's shell loads its choice; the
  settings' map straight in for testing (dos_game_setup, from main.c,
  HALO_DOS); a game's results for the menu when it ends,
- the memory: the game state and the loaded map are ordinary allocations
  (physical_memory_map.c is left out); a map's tag data takes a block of
  its own size and its structure BSP another (cache_files.c, HALO_DOS),
- the cache files: the Xbox's maps read straight from the maps folder
  beside the program, synchronously (cache_files_windows.c, with its copy
  to the hard disk, is left out),
- the game state's buffer (game_state_xbox.c files and checkpoints it),
- the hooks the PC ports added to the game (interpolation, the screen's
  width, the field of view...), answered for these builds.
*/

#include "cseries.h"

#include "ai/ai_debug.h"
#include "cache/cache_files.h"
#include "cache/physical_memory_map.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "main/main.h"
#include "objects/objects.h"
#include "saved games/game_state.h"
#include "scenario/scenario.h"
#include "tag_files/tag_groups.h"
#include "units/units.h"

#include "halo_system.h"
#include "dos_game.h"
#include "dos_split.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the settings (port/dos/xdk/dos_settings.c) */
const char *dos_setting(const char *name, const char *default_value);
long dos_setting_integer(const char *name, long default_value);
int dos_setting_boolean(const char *name, int default_value);
void dos_settings_write_defaults(void);

extern short player_spawn_count;
extern short global_difficulty_level;

/* Halo Custom Edition's maps (dos_ce.c) */
boolean dos_ce_map_open(char const *map_name, struct cache_file_header *header);
void dos_ce_map_closed(void);
void dos_ce_release(void);
boolean dos_ce_opened(void);

/* the libc's own allocator (in the game's units malloc and free are
cseries.h's debug_malloc and debug_free) */
static void *raw_malloc(size_t size)
{
	return (malloc)(size);
}

static void raw_free(void *pointer)
{
	(free)(pointer);
}

/* ---------- the tables */

const struct dos_level dos_campaign_levels[] =
{
	{ "a10", "THE PILLAR OF AUTUMN" },
	{ "a30", "HALO" },
	{ "a50", "THE TRUTH AND RECONCILIATION" },
	{ "b30", "THE SILENT CARTOGRAPHER" },
	{ "b40", "ASSAULT ON THE CONTROL ROOM" },
	{ "c10", "343 GUILTY SPARK" },
	{ "c20", "THE LIBRARY" },
	{ "c40", "TWO BETRAYALS" },
	{ "d20", "KEYES" },
	{ "d40", "THE MAW" },
};
const int dos_campaign_level_count = NUMBEROF(dos_campaign_levels);

/* the multiplayer maps (their folders under levels\test), as the N64
port's menu lists them; then the Halo Custom Edition maps in MAPS_CE
(<name>@ce: dos_ce.c), found as the session starts */
struct dos_level dos_multiplayer_maps[DOS_MAXIMUM_MULTIPLAYER_MAPS] =
{
	{ "beavercreek", "BATTLE CREEK" },
	{ "sidewinder", "SIDEWINDER" },
	{ "damnation", "DAMNATION" },
	{ "ratrace", "RAT RACE" },
	{ "prisoner", "PRISONER" },
	{ "hangemhigh", "HANG 'EM HIGH" },
	{ "chillout", "CHILL OUT" },
	{ "carousel", "DERELICT" },
	{ "boardingaction", "BOARDING ACTION" },
	{ "bloodgulch", "BLOOD GULCH" },
	{ "wizard", "WIZARD" },
	{ "putput", "CHIRON TL-34" },
	{ "longest", "LONGEST" },
};
int dos_multiplayer_map_count = DOS_XBOX_MULTIPLAYER_MAPS;

/* the game types (Halo's own variants; a Pro one is its plain variant with
everyone starting with a pistol and an assault rifle; BOT MATCH is slayer
with a bot at least) */
const struct dos_game_type dos_game_types[] =
{
	{ "slayer", "SLAYER", 0, "KILLS", { 5, 10, 15, 25, 50 }, 2, 0 },
	{ "team_slayer", "TEAM SLAYER", 1, "KILLS", { 10, 25, 50, 75, 100 }, 2, 0 },
	{ "slayer_pro", "SLAYER PRO", 0, "KILLS", { 5, 10, 15, 25, 50 }, 2, 0 },
	{ "team_slayer_pro", "TEAM SLAYER PRO", 1, "KILLS", { 10, 25, 50, 75, 100 }, 2, 0 },
	{ "ctf", "CAPTURE THE FLAG", 1, "FLAGS", { 1, 3, 5, 7, 10 }, 1, 0 },
	{ "king", "KING OF THE HILL", 0, "MINUTES", { 1, 2, 3, 5, 10 }, 1, 0 },
	{ "team_king", "TEAM KING", 1, "MINUTES", { 1, 2, 3, 5, 10 }, 1, 0 },
	{ "oddball", "ODDBALL", 0, "MINUTES", { 1, 2, 3, 5, 10 }, 1, 0 },
	{ "team_oddball", "TEAM ODDBALL", 1, "MINUTES", { 1, 2, 3, 5, 10 }, 1, 0 },
	{ "race", "RACE", 0, "LAPS", { 1, 3, 5, 7, 10 }, 1, 0 },
	{ "team_race", "TEAM RACE", 1, "LAPS", { 1, 3, 5, 7, 10 }, 1, 0 },
	{ "slayer", "BOT MATCH", 0, "KILLS", { 5, 10, 15, 25, 50 }, 2, 1 },
};
const int dos_game_type_count = NUMBEROF(dos_game_types);

const char *const dos_difficulty_names[4] = { "EASY", "NORMAL", "HEROIC", "LEGENDARY" };

/* ---------- the game a session plays */

/* (blood gulch, slayer, normal) */
struct dos_game_choice dos_choice = { DOS_MODE_CAMPAIGN, 0, 1, 9, 0, 0, 0, 1, 0, DOS_WEAPONS_MAP, DOS_VEHICLES_MAP };

/* a Pro variant's (or the pistol and rifle weapons option's) starting
weapons: dos_game_pro_weapons */
static int pro_weapons;

int dos_map_exists(const char *map_name);

int dos_map_present(const char *folder)
{
	/* (a Custom Edition map is listed only if it is in MAPS_CE) */
	if (dos_ce_map_name_is(folder))
	{
		for (int i = DOS_XBOX_MULTIPLAYER_MAPS; i < dos_multiplayer_map_count; i++)
		{
			if (!_stricmp(folder, dos_multiplayer_maps[i].folder))
				return TRUE;
		}
		return FALSE;
	}
	return dos_map_exists(folder);
}

/* a multiplayer map's picture in ui.map's (mp_map_grafix, the Xbox's maps'
order): a Custom Edition map's is the Xbox map's of its name, if there is
one; else NONE */
int dos_map_picture(int map)
{
	char file[48];
	const char *at;

	if (map < 0 || map >= dos_multiplayer_map_count)
		return NONE;
	if (map < DOS_XBOX_MULTIPLAYER_MAPS)
		return map;
	at = strchr(dos_multiplayer_maps[map].folder, '@');
	snprintf(file, sizeof(file), "%.*s", at ? (int)(at - dos_multiplayer_maps[map].folder) : 47,
		dos_multiplayer_maps[map].folder);
	for (int i = 0; i < DOS_XBOX_MULTIPLAYER_MAPS; i++)
	{
		if (!_stricmp(file, dos_multiplayer_maps[i].folder))
			return i;
	}
	return NONE;
}

/* whether a multiplayer map is a Custom Edition map (no Firefight there:
its Covenant are joined from a campaign map, dos_import.c) */
int dos_map_is_custom_edition(int map)
{
	return map >= DOS_XBOX_MULTIPLAYER_MAPS && map < dos_multiplayer_map_count;
}

static const char *mode_name(int mode)
{
	return mode == DOS_MODE_CAMPAIGN ? "campaign" : mode == DOS_MODE_FIREFIGHT ? "firefight" : "multiplayer";
}

/* the chosen game: its variant, players and map; it loads when this
frame is over (main_set_map_name) */
void dos_game_begin(
	void)
{
	static char map_name[96];
	char line[160];
	struct dos_game_choice *c = &dos_choice;

	c->difficulty = PIN(c->difficulty, 0, 3);
	pro_weapons = FALSE;
	/* (no Firefight on a Custom Edition map: its Covenant are joined from an
	Xbox campaign map's tags, which a Halo PC map's cannot take, dos_import.c) */
	if (c->mode == DOS_MODE_FIREFIGHT && dos_map_is_custom_edition(PIN(c->map, 0, dos_multiplayer_map_count - 1)))
	{
		system_log("game: no Firefight on a Custom Edition map: a multiplayer game instead");
		c->mode = DOS_MODE_MULTIPLAYER;
	}
	if (c->mode == DOS_MODE_CAMPAIGN)
	{
		/* a campaign level: no game engine, its scripts and AI (Halo's
		own), the difficulty; the next level follows when it is won
		(main.c main_won_map_private) */
		const char *folder = dos_campaign_levels[PIN(c->level, 0, dos_campaign_level_count - 1)].folder;

		game_set_game_variant(NULL);
		dos_bots_stop();
		dos_firefight_stop();
		global_difficulty_level = (short)c->difficulty;
		snprintf(map_name, sizeof(map_name), "levels\\%s\\%s", folder, folder);
	}
	else
	{
		/* a multiplayer game or Firefight (team slayer, every player on
		the red team, no score to reach; the waves are dos_firefight.c's) */
		const struct dos_game_type *type = &dos_game_types[PIN(c->game_type, 0, dos_game_type_count - 1)];
		const char *folder = dos_multiplayer_maps[PIN(c->map, 0, dos_multiplayer_map_count - 1)].folder;
		struct game_variant variant, temporary_variant;
		struct game_variant *found;
		char variant_name[32];
		const char *name = c->mode == DOS_MODE_FIREFIGHT ? "team_slayer" : type->variant;
		int length = (int)strlen(name);

		pro_weapons = c->mode == DOS_MODE_MULTIPLAYER &&
			((length > 4 && !strcmp(name + length - 4, "_pro")) || c->weapons == DOS_WEAPONS_PISTOL_RIFLE);
		snprintf(variant_name, sizeof(variant_name), "%.*s",
			length > 4 && !strcmp(name + length - 4, "_pro") ? length - 4 : length, name);
		found = game_engine_get_variant_by_name(&temporary_variant, variant_name);
		if (!found)
			found = game_engine_get_variant_by_name(&temporary_variant, "slayer");
		variant = *found;
		if (c->mode == DOS_MODE_FIREFIGHT)
			variant.universal_variant.score_to_win = 9999;
		else if (c->score > 0)
			variant.universal_variant.score_to_win = c->score;
		/* (Halo's own vehicle set 1: every map vehicle NONE) */
		if (c->mode == DOS_MODE_MULTIPLAYER && c->vehicles == DOS_VEHICLES_NONE)
			variant.universal_variant.vehicle_set = 1;
		game_set_game_variant(&variant);
		if (c->mode == DOS_MODE_FIREFIGHT)
		{
			dos_bots_stop();
			dos_firefight_begin(c->difficulty, dos_split_count() > 1 ? dos_split_count() : 1);
		}
		else
		{
			dos_firefight_stop();
			dos_bots_begin(PIN(c->bots, type->bot_match ? 1 : 0, DOS_MAXIMUM_BOTS), c->bot_skill, type->teams);
		}
		/* (a Custom Edition map by its name, <file>@ce: dos_ce.c) */
		if (dos_ce_map_name_is(folder))
			snprintf(map_name, sizeof(map_name), "%s", folder);
		else
			snprintf(map_name, sizeof(map_name), "levels\\test\\%s\\%s", folder, folder);
	}
	/* the players at this machine: split screen's (testing: the setting
	test_split=n, n players on the one screen); the campaign's co-op is for
	two, as Halo's */
	{
		long players = dos_split_count() > 1 ? dos_split_count() : dos_setting_integer("test_split", 1);

		if (c->mode == DOS_MODE_CAMPAIGN && players > 2)
			players = 2;
		player_spawn_count = (short)PIN(players, 1, DOS_SPLIT_PLAYERS);
	}
	main_set_map_name(map_name);
	main_disallow_persistent_storage();
	snprintf(line, sizeof(line), "game: %s %s, %s, %d bots", mode_name(c->mode), map_name,
		c->mode == DOS_MODE_MULTIPLAYER ? dos_game_types[c->game_type].name : dos_difficulty_names[c->difficulty],
		c->mode == DOS_MODE_MULTIPLAYER ? c->bots : 0);
	system_log(line);
}

/* the map being played (its folder's name: the scenario's last part) */
const char *dos_game_map_name(
	void)
{
	static char folder[32];
	const char *name = global_scenario_index != NONE ? tag_get_name(global_scenario_index) : NULL;
	const char *last;

	if (!name)
		return "";
	last = strrchr(name, '\\');
	snprintf(folder, sizeof(folder), "%s", last ? last + 1 : name);
	return folder;
}

/* main.c's create_local_players: the index-th player's name, colour and
team (Halo's colour indices: red 2, blue 3) */
void dos_game_local_player(
	long index,
	wchar_t *name,
	short *color,
	short *team)
{
	const char *text = dos_setting("player_name", "Player");
	const struct dos_split_player *split = dos_split_player((int)index);
	int teams = dos_choice.mode == DOS_MODE_FIREFIGHT ||
		(dos_choice.mode == DOS_MODE_MULTIPLAYER && dos_game_types[dos_choice.game_type].teams);
	char numbered[16];
	int c;

	/* (split screen's others: Player 2, 3, 4) */
	if (split && index > 0)
	{
		snprintf(numbered, sizeof(numbered), "Player %ld", index + 1);
		text = numbered;
	}
	for (c = 0; c < 11 && text[c]; c++)
		name[c] = (wchar_t)(unsigned char)text[c];
	name[c] = 0;
	if (split)
	{
		/* (their own colour and team, chosen as they joined) */
		*team = (short)(dos_choice.mode == DOS_MODE_FIREFIGHT ? 0 : teams ? (split->team & 1) : index);
		*color = (short)(teams ? (*team ? 3 : 2) : split->color);
		return;
	}
	*team = (short)(dos_choice.mode == DOS_MODE_FIREFIGHT ? 0 : teams ? (dos_choice.team & 1) : index);
	*color = (short)(teams ? (*team ? 3 : 2) : dos_setting_integer("player_color", 2));
}

/* game_engine.c's starting weapons (HALO_DOS): a Pro variant's or the
weapons option's pistol and assault rifle; FALSE for the map's own */
boolean dos_game_pro_weapons(
	long unit_index)
{
	static const char *const names[2] = { "weapons\\pistol\\pistol", "weapons\\assault rifle\\assault rifle" };
	boolean first = TRUE;
	short i;

	if (!pro_weapons || unit_index == NONE || dos_choice.mode != DOS_MODE_MULTIPLAYER)
		return FALSE;
	/* (both or neither: the map's own weapons then) */
	if (tag_loaded('weap', names[0]) == NONE || tag_loaded('weap', names[1]) == NONE)
		return FALSE;
	for (i = 0; i < 2; i++)
	{
		struct object_placement_data placement_data;
		long weapon_index;

		object_placement_data_new(&placement_data, tag_loaded('weap', names[i]), NONE);
		weapon_index = object_new(&placement_data);
		if (weapon_index != NONE)
		{
			if (unit_add_weapon_to_inventory(unit_index, weapon_index, first ? 2 : 0))
				first = FALSE;
			else
				object_delete(weapon_index);
		}
	}
	return !first;
}

/* ---------- the results of the game that ended */

static char result_lines[16][64];
static int result_count;

static void ascii(char *out, int size, const wchar_t *text)
{
	int i;

	for (i = 0; i < size - 1 && text[i]; i++)
		out[i] = text[i] < 128 ? (char)text[i] : '?';
	out[i] = 0;
}

static void collect_results(
	void)
{
	struct
	{
		char name[24];
		char score[16];
		long sort;
		short kills, deaths, team;
	} rows[16], row;
	struct data_iterator iterator;
	struct player_datum *player;
	boolean teams;
	int count = 0, i, at;

	result_count = 0;
	if (dos_choice.mode == DOS_MODE_FIREFIGHT)
	{
		result_count = dos_firefight_results(result_lines, NUMBEROF(result_lines));
		return;
	}
	if (!game_engine)
		return;
	teams = game_engine_has_teams();
	data_iterator_new(&iterator, player_data);
	while ((player = data_iterator_next(&iterator)) != NULL && count < NUMBEROF(rows))
	{
		long player_index = iterator.datum_index;
		const char *bot_name = dos_bots_display_name(player_index);
		wchar_t score_text[32];

		csmemset(&row, 0, sizeof(row));
		if (bot_name)
			snprintf(row.name, sizeof(row.name), "%s", bot_name);
		else
			ascii(row.name, sizeof(row.name), player->name);
		csmemset(score_text, 0, sizeof(score_text));
		if (game_engine->format_player_score)
			game_engine->format_player_score(player_index, score_text);
		ascii(row.score, sizeof(row.score), score_text);
		row.kills = player->statistics.kills[0];
		row.deaths = player->statistics.deaths;
		row.team = (short)(teams ? player->team_index & 1 : -1);
		row.sort = game_engine->get_player_score ?
			game_engine->get_player_score(player_index, _get_score_individual) : row.kills;
		for (at = count; at > 0 && (row.sort > rows[at - 1].sort ||
			(row.sort == rows[at - 1].sort && row.kills > rows[at - 1].kills)); at--)
		{
			rows[at] = rows[at - 1];
		}
		rows[at] = row;
		count++;
	}
	if (teams)
	{
		long red = game_engine_get_team_score(0), blue = game_engine_get_team_score(1);

		snprintf(result_lines[result_count++], 64, "%s", red > blue ? "RED TEAM WINS" : blue > red ?
			"BLUE TEAM WINS" : "TIE GAME");
	}
	else if (count)
	{
		snprintf(result_lines[result_count++], 64, "%s WINS", rows[0].name);
	}
	for (i = 0; i < count && result_count < NUMBEROF(result_lines); i++)
	{
		snprintf(result_lines[result_count++], 64, "%s%s   %s   (%d KILLS, %d DEATHS)",
			rows[i].team == 0 ? "RED  " : rows[i].team == 1 ? "BLUE  " : "", rows[i].name, rows[i].score,
			rows[i].kills, rows[i].deaths);
	}
}

int dos_game_results(char lines[][64], int count)
{
	int i;

	for (i = 0; i < result_count && i < count; i++)
		snprintf(lines[i], 64, "%s", result_lines[i]);
	return i;
}

/* ---------- the session */

/* main.c (HALO_DOS), after the game is initialized: the game the settings
choose, straight in (testing: play=<folder>, with game=, score=, bots=,
bot_skill=, difficulty=, team=, firefight=1), or the menu */
void dos_game_setup(
	void)
{
	const char *map;
	int i;

	dos_settings_write_defaults();
	if (system_memory_total())
	{
		char line[96];

		snprintf(line, sizeof(line), "memory: %lu KB of %lu KB free after the game's start", system_memory_free() / 1024,
			system_memory_total() / 1024);
		system_log(line);
	}
	/* the Halo Custom Edition maps in MAPS_CE, after the Xbox's (dos_ce.c) */
	dos_multiplayer_map_count = dos_ce_list_multiplayer_maps(dos_multiplayer_maps, DOS_XBOX_MULTIPLAYER_MAPS,
		DOS_MAXIMUM_MULTIPLAYER_MAPS);
	map = dos_setting("play", "");
	if (!*map)
		return;
	dos_choice.mode = dos_setting_boolean("firefight", FALSE) ? DOS_MODE_FIREFIGHT : DOS_MODE_MULTIPLAYER;
	dos_choice.difficulty = (int)dos_setting_integer("difficulty", 1);
	dos_choice.score = (int)dos_setting_integer("score", 0);
	dos_choice.bots = (int)dos_setting_integer("bots", 0);
	dos_choice.bot_skill = (int)dos_setting_integer("bot_skill", 1);
	dos_choice.team = (int)dos_setting_integer("team", 0);
	for (i = 0; i < dos_campaign_level_count; i++)
	{
		if (!_stricmp(map, dos_campaign_levels[i].folder))
		{
			dos_choice.mode = DOS_MODE_CAMPAIGN;
			dos_choice.level = i;
		}
	}
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
	dos_game_begin();
}

/* main.c's main_menu_load (HALO_DOS): a game ended (its results first) or
the session starts; the menu runs over ui.map */
void dos_game_menu(
	boolean game_ended)
{
	result_count = 0;
	if (game_ended)
		collect_results();
	dos_bots_stop();
	dos_firefight_stop();
	dos_menu_open(game_ended && result_count > 0);
}

/* ---------- memory map */

static struct
{
	void *game_state;
	void *tag_data;
	unsigned long tag_data_size;
	void *structure_bsp;
	unsigned long structure_bsp_size;
	void *sound_cache;
} dos_memory;

/* the game state: the Xbox's size (its retail pools, the AI's and the
scripts' among them), an ordinary allocation whose address
game_state_xbox.c takes (physical_memory_get_game_state_base_address) */
#define DOS_GAME_STATE_SIZE HALO_PORT_GAME_STATE_SIZE

void physical_memory_allocate(
	void)
{
	char line[96];

	if (dos_memory.game_state)
		return;
	dos_memory.game_state = raw_malloc(DOS_GAME_STATE_SIZE);
	if (!dos_memory.game_state)
		system_error("There is not enough memory for the game state.");
	memset(dos_memory.game_state, 0, DOS_GAME_STATE_SIZE);
	snprintf(line, sizeof(line), "game state: %lu KB", (unsigned long)DOS_GAME_STATE_SIZE / 1024);
	system_log(line);
}

void physical_memory_verify(
	void)
{
	return;
}

void physical_memory_free(
	void)
{
	return;
}

void *physical_memory_get_game_state_base_address(
	void)
{
	return dos_memory.game_state;
}

void *physical_memory_get_tag_cache_base_address(
	void)
{
	return dos_memory.tag_data;
}

void *physical_memory_get_texture_cache_base_address(
	void)
{
#ifdef HALO_D3D
	/* the Direct3D build's (port/dos/DIRECT3D.md): Halo's own texture cache
	loads the maps' bitmaps into the Xbox's 22 MB, on 16 KB pages
	(cache/xbox_texture_cache.c); the DOS and Windows 95 builds' renderer
	keeps its own copies instead */
	static unsigned char *texture_cache;

	if (!texture_cache)
	{
		unsigned char *memory = raw_malloc(HALO_PORT_TEXTURE_CACHE_SIZE + 0x4000);

		if (!memory)
			system_error("There is not enough memory for the texture cache.");
		texture_cache = (unsigned char *)(((unsigned long)memory + 0x3FFF) & ~0x3FFFUL);
		memset(texture_cache, 0, HALO_PORT_TEXTURE_CACHE_SIZE);
	}
	return texture_cache;
#else
	return NULL;
#endif
}

/* the sound cache's 4 MB (xbox_sound_cache.c), made when the cache opens */
void *physical_memory_get_sound_cache_base_address(
	void)
{
	if (!dos_memory.sound_cache)
	{
		dos_memory.sound_cache = raw_malloc(SOUND_CACHE_SIZE);
		if (!dos_memory.sound_cache)
			system_error("There is not enough memory for the sound cache.");
	}
	return dos_memory.sound_cache;
}

/* cache_files.c (HALO_DOS): a block for a map's tag data, the old map's
freed first (its tag data is never used again once a new map loads) */
void *dos_tag_cache_reserve(
	unsigned long size)
{
	char line[96];

	if (dos_memory.structure_bsp)
	{
		raw_free(dos_memory.structure_bsp);
		dos_memory.structure_bsp = NULL;
		dos_memory.structure_bsp_size = 0;
	}
	if (dos_memory.tag_data)
		raw_free(dos_memory.tag_data);
	dos_memory.tag_data = NULL;
	dos_memory.tag_data_size = 0;
	/* (and a Custom Edition map's blocks; a Custom Edition map's own are
	dos_ce.c's, seen where its tags were linked) */
	dos_ce_release();
	if (dos_ce_opened())
		return NULL;
	/* (the read may round up to a sector) */
	dos_memory.tag_data_size = (size + 4095) & ~4095UL;
	dos_memory.tag_data = raw_malloc(dos_memory.tag_data_size);
	if (!dos_memory.tag_data)
	{
		snprintf(line, sizeof(line), "There is not enough memory for the map's %lu KB of tag data (%lu KB of %lu KB "
			"free).", size / 1024, system_memory_free() / 1024, system_memory_total() / 1024);
		system_error(line);
	}
	snprintf(line, sizeof(line), "map: tag data %lu KB at %p", size / 1024, dos_memory.tag_data);
	system_log(line);
	return dos_memory.tag_data;
}

/* cache_files.c (HALO_DOS): a block for a structure BSP (the one before it
is unloaded by then) */
void *dos_structure_bsp_reserve(
	unsigned long size)
{
	char line[96];

	size = (size + 4095) & ~4095UL;
	if (dos_memory.structure_bsp && dos_memory.structure_bsp_size >= size)
		return dos_memory.structure_bsp;
	if (dos_memory.structure_bsp)
		raw_free(dos_memory.structure_bsp);
	dos_memory.structure_bsp = raw_malloc(size);
	dos_memory.structure_bsp_size = size;
	if (!dos_memory.structure_bsp)
	{
		snprintf(line, sizeof(line), "There is not enough memory for the level's %lu KB.", size / 1024);
		system_error(line);
	}
	snprintf(line, sizeof(line), "map: structure BSP %lu KB at %p", size / 1024, dos_memory.structure_bsp);
	system_log(line);
	return dos_memory.structure_bsp;
}

/* ---------- the cache files' platform half (cache_files_windows.c): the
map file is the platform's (port/dos/xdk/xdk_kernel.c, dos_map_*) */

int dos_map_open(const char *map_name, void *header, unsigned long header_size);
int dos_map_read(unsigned long offset, void *buffer, unsigned long size);
void dos_map_close(void);

/* the cache file header's size (cache_files.c's struct cache_file_header) */
#define CACHE_FILE_HEADER_SIZE 0x800

void cache_files_initialize(void) { return; }
void cache_files_dispose(void) { return; }
void cache_files_precache_set_priority(boolean blocking) { (void)blocking; return; }
boolean cache_files_precache_in_progress(void) { return FALSE; }
boolean cache_files_precache_is_copying_map(const char *map_name) { (void)map_name; return FALSE; }
boolean cache_files_precache_map_loaded(const char *map_name) { (void)map_name; return TRUE; }
boolean cache_files_precache_map_begin(const char *map_name, boolean copy_map) { (void)map_name; (void)copy_map; return TRUE; }
void cache_files_precache_map_end(void) { return; }
void cache_files_precache_map_queue_end(void) { return; }
short cache_files_precache_map_status(real *progress) { if (progress) *progress = 1.f; return 1; }
void cache_file_promote_read(short request) { (void)request; return; }
void cache_file_block_until_not_busy(void) { return; }

void cache_file_close(
	void)
{
	dos_map_close();
}

boolean cache_file_open(
	char const *map_name,
	struct cache_file_header *header)
{
	/* (a Halo Custom Edition map, <name>@ce, from MAPS_CE: dos_ce.c) */
	if (dos_ce_map_name_is(map_name))
		return dos_ce_map_open(map_name, header);
	dos_ce_map_closed();
	if (!dos_map_open(map_name, header, CACHE_FILE_HEADER_SIZE))
	{
		char text[200];

		snprintf(text, sizeof(text), "The map %s.map is not in the MAPS folder beside the program. "
			"Copy the maps folder of your Xbox Halo disc there.", map_name);
		system_error(text);
		return FALSE;
	}
	return TRUE;
}

short cache_file_read(
	long tag_index,
	long offset,
	long size,
	void *buffer,
	boolean *completion_flag_reference,
	boolean blocking)
{
	/* (Firefight's Covenant, joined from a campaign map, have their sounds'
	samples in that map's file: dos_import.c) */
	extern int dos_import_tag(long tag_index);
	extern int dos_import_read(unsigned long offset, void *buffer, unsigned long size);
	int read_from_source = dos_import_tag(tag_index);

	(void)blocking;
	/* (a Custom Edition map's sounds' samples may be in sounds.map: dos_ce.c) */
	if (!(read_from_source ? dos_import_read((unsigned long)offset, buffer, (unsigned long)size) :
		dos_ce_read(tag_index, (unsigned long)offset, buffer, (unsigned long)size)))
	{
		char text[128];

		snprintf(text, sizeof(text), "Could not read %ld bytes of the map at %ld.", size, offset);
		system_error(text);
	}
	if (completion_flag_reference)
		*completion_flag_reference = TRUE;
	return NONE;
}

void tags_header_register_vertex_and_index_buffers(struct cache_file_tag_header *header) { (void)header; return; }
void tags_header_deregister_vertex_and_index_buffers(struct cache_file_tag_header *header) { (void)header; return; }
void structure_bsp_header_register_vertex_buffers(struct cache_file_structure_bsp_header *header) { (void)header; return; }
void structure_bsp_header_deregister_vertex_buffers(struct cache_file_structure_bsp_header *header) { (void)header; return; }

/* ---------- the PC ports' hooks into the game */

/* the FRAME RATE setting (frame_rate: 30, Halo's own; 60 or 120, or 0 for
no limit): above 30 the frames between the game's 30 ticks are drawn too
(ChupathingyCE's interpolation, render_interpolation.c), and the limit is
waited out after each frame (render_frame_present) */
static int frame_rate(void)
{
	extern unsigned long dos_settings_generation(void);
	static unsigned long generation = (unsigned long)-1;
	static int rate = 30;

	if (generation != dos_settings_generation())
	{
		generation = dos_settings_generation();
		rate = (int)dos_setting_integer("frame_rate", 30);
		if (rate != 0 && rate < 30)
			rate = 30;
	}
	return rate;
}

int halo_interpolation_enabled(void)
{
	return frame_rate() != 30;
}

/* the least time a frame takes, in microseconds (0: none) */
unsigned long dos_frame_interval(void)
{
	int rate = frame_rate();

	return rate > 30 ? 1000000UL / (unsigned long)rate : 0;
}
/* the logical screen's width (dos_render_game.c: 640 at 4:3, wider on a
wide screen), and the game's 640-wide parts centred in it while it says
(its menus, post-game screens, progress bar: render.c) */
extern long dos_screen_width;
extern float dos_screen_offset_x;
float dos_screen_centred_offset(void);
long halo_screen_width(void) { return dos_screen_width; }
float halo_pixel_aspect(void) { return 1.f; }
float halo_screen_texture_scale_x(void) { return 1.f; }
float halo_screen_texture_scale_y(void) { return 1.f; }
int halo_performance_mode(void) { return 1; }
long halo_screen_commit(void) { return dos_screen_width; }
void halo_screen_ui_offset(unsigned char centered) { dos_screen_offset_x = centered ? dos_screen_centred_offset() : 0.0f; }
const char *halo_display_notice(void) { return NULL; }
int halo_graphics_option(const char *name) { (void)name; return 0; }
int halo_debug_console_enabled(void) { return 1; }
int halo_debug_log_file_enabled(void) { return 1; }
void network_lobby_update(void) { return; }

int halo_fps_counter_enabled(void)
{
	extern int dos_menu_active(void);
	extern int dos_pause_active(void);

	/* (not over the menu's pages) */
	return dos_setting_boolean("fps_counter", 0) && !dos_menu_active() && !dos_pause_active();
}

/* (testing: the first local player's unit's vitality, for the HUD's log) */
void dos_debug_player_vitality(char *line, int size)
{
	long player_index = local_player_get_player_index(0);
	struct player_datum *player = player_index != NONE ? player_get(player_index) : NULL;
	struct unit_datum *unit = player && player->unit_index != NONE ? unit_try_and_get(player->unit_index) : NULL;

	if (!unit)
	{
		snprintf(line, size, "player: no unit");
		return;
	}
	snprintf(line, size, "player: body %.3f of %.1f, shield %.3f of %.1f, flags %08lX", (double)unit->object.body_vitality,
		(double)unit->object.maximum_body_vitality, (double)unit->object.shield_vitality,
		(double)unit->object.maximum_shield_vitality, (unsigned long)unit->object.damage_flags);
}

/* the Xbox narrowed every view by 0.85 for a television's overscan (about
62 degrees across instead of 70); a monitor shows the whole picture */
float halo_field_of_view_tangent_scale(void)
{
	return 1.0f / 0.85f;
}

/* ---------- the stack walk (cseries/stack_walk_windows.c is left out) */

void stack_walk(short levels_to_ignore) { (void)levels_to_ignore; }
void stack_walk_initialize(void) { return; }
void stack_walk_dispose(void) { return; }
void stack_walk_disregard_symbol_names(boolean disregard) { (void)disregard; return; }
