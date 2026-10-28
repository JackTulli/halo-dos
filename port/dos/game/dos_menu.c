/*
DOS_MENU.C

The DOS and Windows 95 builds' menu, after the N64 port's
(port/n64/src/n64_menu.c in halo-64): pages of rows drawn over ui.map's
scene (the ring and its stars, the camera the map's scripts glide about),
in Halo's own fonts (ui\large_ui, ui\small_ui), read from the keyboard:

	TITLE        CAMPAIGN, MULTIPLAYER, SETTINGS, QUIT
	CAMPAIGN     the level, the difficulty, the players (co-op: split
	             screen's first two)
	MULTIPLAYER  ONLINE, SPLIT SCREEN, CUSTOM GAME, FIREFIGHT
	ONLINE       QUICK JOIN (the best game found after a few seconds),
	             BROWSE GAMES (the LAN's, ChupathingyCE's and OpenCE's online,
	             Custom Edition servers: tabs by kind, an order, a FILTERS
	             box; dos_network.c), HOST A GAME, the PLAYERS at this screen
	             (split screen's play online too), and the LOBBY of the game
	             hosted or joined, between its games too
	SPLIT SCREEN two to four players at this computer, each joining with a
	             device of their own (a controller's A, the keyboard's
	             ENTER) and choosing their armour and team; then the game:
	             online, a custom game, Firefight or the campaign in co-op.
	             Every game page's PLAYERS row opens it too, and comes back
	CUSTOM GAME  the game type, the map, the score to win, the bots (how
	             many, how good), the player's team, the weapons and the
	             vehicles
	FIREFIGHT    the map, the difficulty
	SETTINGS     PLAYER (the name others see, the armour), DISPLAY,
	             GRAPHICS, SOUND, CONTROLS, CONTROLLER: what HALO.CFG holds.
	             What Halo reads only as it starts (the renderer, the
	             picture's size, colours and shape, the texture quality,
	             sound on or off) waits, marked *, for APPLY AND RESTART
	RESULTS      the game that ended

In a game ESCAPE pauses it (RESUME, REVERT TO CHECKPOINT, RESTART, QUIT TO
MENU, QUIT HALO); a system link game goes on under its pause, which leaves
it (LEAVE GAME) rather than restarting it. The game's controller reads nothing while the menu or the
pause has the keyboard, nor until the keys that left it are let go.

	arrows, W S      a row up or down
	arrows, A D      the row's value
	ENTER, SPACE     choose
	ESCAPE, BACKSPACE  back
*/

#include "cseries.h"

#include "cache/cache_files.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "main/main.h"
#include "rasterizer/rasterizer.h"
#include "render/render.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "tag_files/tag_groups.h"
#include "text/draw_string.h"

#include "halo_system.h"
#include "dos_render.h"
#include "dos_game.h"
#include "halo_bindings.h"
#include "dos_menu_art.h"
#include "dos_split.h"
#include "dos_map_download.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the platform's (port/dos/xdk) */
const struct system_input *dos_input(void);
const char *dos_setting(const char *name, const char *default_value);
long dos_setting_integer(const char *name, long default_value);
float dos_setting_real(const char *name, float default_value);
void dos_setting_set(const char *name, const char *value);
void dos_settings_save(void);
void dos_render_reload_settings(void);
extern struct r_settings dos_render_settings;
long dos_game_engine_postgame_state(void);
void dos_network_autotest(int menu_up);
int system_display_modes(int bits, int *widths, int *heights, int maximum);
unsigned long *dos_interface_picture(long bitmap_tag_index, float crop_u, float crop_v, int width, int height);

/* ---------- constants */

/* draw_string.c's justifications */
enum
{
	JUSTIFY_LEFT,
	JUSTIFY_RIGHT,
	JUSTIFY_CENTER
};

/* the game's 640x480 screen (drawn scaled to the picture's pixels), and
what matters inside 32 of its edges */
#define SCREEN_W 640
#define SCREEN_H 480
#define SAFE_LEFT 32
#define SAFE_RIGHT (SCREEN_W - 32)
/* the pages' rows: their column, and their room between the title's rule
and the chosen row's help (a longer list scrolls) */
#define LIST_X0 96
#define LIST_X1 544
#define LIST_TOP 88
#define LIST_BOTTOM 392
#define ROW_H 24
/* the help's line, and the footer's rule over its keys */
#define HELP_Y 398
#define FOOTER_Y 432
#define LINE 2

#define TEXT_COLOR 0xFF8CC0FFUL
#define TEXT_BRIGHT 0xFFFFFFFFUL
#define TEXT_DIM 0xFF6A8CB8UL
#define CHOSEN_COLOR 0x50306CB8UL
#define GLOW_COLOR 0x48286CFFUL
#define FRAME_COLOR 0xFF8CC0FFUL
#define RULE_COLOR 0xC08CC0FFUL
#define BACKDROP_COLOR 0xA0000410UL
#define TITLE_BACKDROP_COLOR 0x50000614UL

/* key repeat: after this long held, every this long (microseconds) */
#define REPEAT_DELAY 400000ULL
#define REPEAT_EVERY 110000ULL

enum
{
	SCREEN_TITLE,
	SCREEN_CAMPAIGN,
	SCREEN_MULTIPLAYER,
	SCREEN_FIREFIGHT,
	SCREEN_SETTINGS,
	SCREEN_RESULTS,
	SCREEN_PAUSE,
	SCREEN_NETWORK,
	SCREEN_NET_HOST,
	SCREEN_NET_JOIN,
	SCREEN_LOBBY,
	/* settings' own pages */
	SCREEN_CONTROLS,
	SCREEN_CONTROLLER,
	/* split screen's: the players joining, then the game's choice */
	SCREEN_SPLIT,
	SCREEN_SPLIT_GAME,
	/* the title's MULTIPLAYER: online, split screen, a custom game,
	Firefight */
	SCREEN_MULTIPLAYER_MENU,
	/* the settings' pages: the player, the display, the graphics, the
	sound (the controls' and the controller's above) */
	SCREEN_SET_PLAYER,
	SCREEN_SET_DISPLAY,
	SCREEN_SET_GRAPHICS,
	SCREEN_SET_SOUND,
	/* a Custom Edition server being joined (its map downloaded first, when
	it is not here) */
	SCREEN_CE_JOIN,
};

/* the rows of each page */
enum
{
	ROW_LEVEL,
	ROW_DIFFICULTY,
	ROW_GAME_TYPE,
	ROW_MAP,
	ROW_SCORE,
	ROW_BOTS,
	ROW_BOT_SKILL,
	ROW_TEAM,
	ROW_WEAPONS,
	ROW_VEHICLES,
	ROW_START,
	ROW_BACK,
	/* the title's */
	ROW_TITLE_CAMPAIGN,
	ROW_TITLE_MULTIPLAYER,
	ROW_TITLE_FIREFIGHT,
	ROW_TITLE_SETTINGS,
	ROW_TITLE_QUIT,
	/* the settings' */
	ROW_SET_RESOLUTION,
	/* the screen's shape (a wide one: the views wider, the HUD at its sides,
	the menus centred) and the slow machines' doubled pixels */
	ROW_SET_ASPECT,
	ROW_SET_DOUBLE_WIDE,
	ROW_SET_DOUBLE_TALL,
	ROW_SET_COLOURS,
	ROW_SET_DISPLAY,
	ROW_SET_SCALING,
	ROW_SET_RENDERER,
	ROW_SET_DRAW_DISTANCE,
	ROW_SET_DETAIL,
	ROW_SET_FIELD_OF_VIEW,
	ROW_SET_TEXTURES,
	ROW_SET_TEXTURE_QUALITY,
	ROW_SET_LIGHT,
	/* the Xbox's look (DIRECT3D 8 alone): the level's lightmaps, the
	objects' light */
	ROW_SET_D3D_LIGHTMAPS,
	ROW_SET_D3D_OBJECT_LIGHT,
	ROW_SET_D3D_TEXTURES,
	ROW_SET_VIEW_MODEL,
	ROW_SET_PARTICLES,
	ROW_SET_SOUND,
	ROW_SET_VOLUME,
	ROW_SET_SKY,
	ROW_SET_MOUSE,
	ROW_SET_INVERT,
	ROW_SET_FPS,
	ROW_SET_FRAME_RATE,
	ROW_SET_NAME_COLOR,
	ROW_SET_CONTROLS,
	ROW_SET_CONTROLLER,
	/* the settings' own pages, the player's name, and what needs Halo
	started again done */
	ROW_SET_PLAYER,
	ROW_SET_DISPLAY_PAGE,
	ROW_SET_GRAPHICS_PAGE,
	ROW_SET_SOUND_PAGE,
	ROW_SET_NAME,
	ROW_SET_APPLY,
	ROW_SET_AIM_ASSIST,
	/* the controls page's (its actions' rows: ROW_BIND + the action) */
	ROW_BIND_RESET,
	/* the controller page's */
	ROW_PAD_DEVICE,
	ROW_PAD_ENABLED,
	ROW_PAD_LAYOUT,
	ROW_PAD_STICKS,
	ROW_PAD_SENSITIVITY,
	ROW_PAD_INVERT,
	ROW_PAD_AIRCRAFT,
	ROW_PAD_VIBRATION,
	/* the pause's */
	ROW_PAUSE_RESUME,
	ROW_PAUSE_REVERT,
	ROW_PAUSE_RESTART,
	ROW_PAUSE_MENU,
	ROW_PAUSE_QUIT,
	/* system link's */
	ROW_TITLE_NETWORK,
	ROW_NET_HOST,
	ROW_NET_JOIN,
	ROW_NET_CREATE,
	ROW_NET_START,
	ROW_NET_LEAVE,
	ROW_NET_QUICK,
	/* the join page's filters (its FILTERS box: filter_box) */
	ROW_FILTER_FULL,
	ROW_FILTER_EMPTY,
	ROW_FILTER_JOINABLE,
	/* split screen's, and every game page's players at this screen */
	ROW_TITLE_SPLIT,
	ROW_SPLIT_PLAYERS,
	ROW_PLAYERS,
	/* MULTIPLAYER's custom game (against bots, or split screen's players) */
	ROW_MP_CUSTOM,
	/* (the games found: ROW_NET_GAME + their index) */
	ROW_NET_GAME,
	/* (the controls page's actions: ROW_BIND + halo_bindings.h's action) */
	ROW_BIND = 200,
};

/* ---------- state */

static struct
{
	int active; /* the menu (over ui.map) */
	int paused; /* the pause (over a game) */
	int screen;
	int row; /* the chosen row's index in the page's rows */
	int rows[DOS_NETWORK_MAXIMUM_GAMES + 16];
	int row_count;
	/* a long list's first row shown */
	int first_row;
	/* the game's keys stay away until the menu's keys are let go */
	int holding;
	unsigned char previous[SYSTEM_KEY_COUNT];
	unsigned long long held_since[4], repeated_at[4];
	/* the results of the game that ended */
	char results[16][64];
	int result_count;
	/* the settings page's (written to HALO.CFG on leaving it) */
	int resolution, resolution_count, colours;
	int widths[32], heights[32];
	int settings_changed;
	char message[64];
	/* system link's: the games found */
	struct dos_network_game games[DOS_NETWORK_MAXIMUM_GAMES];
	int game_count;
	/* the controls page: the action waiting for its key (-1 none), and
	whether every key has been let go since (the Enter that chose it) */
	int binding, binding_armed;
	/* the page's start (it slides and fades in), and the chosen row's
	frame gliding to it: where it is, when it moved, on which page */
	unsigned long long page_started, highlight_time;
	float highlight_y;
	int highlight_screen;
	/* the page the last game was started from (the menu comes back to it) */
	int game_screen;
	/* the join page's FILTERS box: up (its chosen row), and whether a
	filter changed (saved as it closes) */
	int filter_box, filter_row, filters_changed;
	/* the split screen page's way out: the page it came from (NONE: on to
	the game's choice, SCREEN_SPLIT_GAME) */
	int split_return;
	/* the player's name being typed (the PLAYER page): the text, where the
	cursor is */
	int editing;
	char edit_text[12];
	int edit_cursor;
	/* the settings that take effect when Halo starts again, changed but not
	applied (APPLY AND RESTART writes them and starts it again) */
	struct
	{
		char name[24];
		char value[32];
	} pending[12];
	int pending_count;
	/* the mouse's pointer on the menu's 640x480 screen: where it is,
	whether it is shown (once the mouse moves; a key or a controller hides
	it), and this frame's motion, clicks and wheel turns */
	float pointer_x, pointer_y;
	int pointer_shown, pointer_moved, clicked, right_clicked, wheel;
	/* the chosen row is the pointer's (no row kept above or below it as the
	list scrolls: it would scroll on under the pointer) */
	int pointer_chose;
	unsigned char previous_buttons;
	/* the Custom Edition server being joined: its name and map, as listed */
	char join_name[32], join_map[32];
} menu = { .binding = -1, .highlight_screen = -1, .split_return = -1 };

/* ---------- what the mouse can point at

Each page records where its rows, tabs, heads and footer keys were drawn
(the frame before: the mouse's input reads them before the page is drawn
again); the pointer over one chooses it, a click does it. */

enum
{
	HIT_ROW, /* a row of the page (its index) */
	HIT_TAB, /* the game list's tab (DOS_SHOW_*) */
	HIT_SORT, /* the game list's column head (DOS_SORT_*) */
	HIT_FILTER, /* the FILTERS box's row */
	HIT_HINT, /* a footer's key (HINT_*) */
	HIT_SCROLL /* a long list's arrow (-1 up, 1 down) */
};

static struct
{
	short x0, y0, x1, y1;
	short kind, value;
} hits[160];
static int hit_count;

static void add_hit(int x0, int y0, int x1, int y1, int kind, int value)
{
	if (hit_count >= (int)NUMBEROF(hits))
		return;
	hits[hit_count].x0 = (short)x0;
	hits[hit_count].y0 = (short)y0;
	hits[hit_count].x1 = (short)x1;
	hits[hit_count].y1 = (short)y1;
	hits[hit_count].kind = (short)kind;
	hits[hit_count].value = (short)value;
	hit_count++;
}

/* what is under the pointer (the last drawn on top: a box over a page):
its index in hits, or -1 */
static int hit_under_pointer(void)
{
	int x = (int)menu.pointer_x, y = (int)menu.pointer_y;

	for (int i = hit_count - 1; i >= 0; i--)
	{
		if (x >= hits[i].x0 && x < hits[i].x1 && y >= hits[i].y0 && y < hits[i].y1)
			return i;
	}
	return -1;
}

/* the settings that take effect only when Halo starts again: changed on the
settings' pages they wait (menu.pending) for APPLY AND RESTART */
static const char *const restart_settings[] = { "renderer", "width", "height", "bits", "aspect", "double_wide",
	"double_tall", "texture_size", "sound" };

static int needs_restart(const char *name)
{
	for (unsigned i = 0; i < NUMBEROF(restart_settings); i++)
		if (!_stricmp(restart_settings[i], name))
			return TRUE;
	return FALSE;
}

static int pending_index(const char *name)
{
	for (int i = 0; i < menu.pending_count; i++)
		if (!_stricmp(menu.pending[i].name, name))
			return i;
	return -1;
}

/* the menu's view of the settings: those waiting for APPLY as they will
be, the rest as they are */
static const char *menu_string(const char *name, const char *default_value)
{
	int i = pending_index(name);

	return i >= 0 ? menu.pending[i].value : dos_setting(name, default_value);
}

static long menu_integer(const char *name, long default_value)
{
	int i = pending_index(name);

	return i >= 0 ? atol(menu.pending[i].value) : dos_setting_integer(name, default_value);
}

/* a setting changed on a page: at once, or (one Halo reads only as it
starts) waiting for APPLY, until set back as it is */
static void menu_set(const char *name, const char *value)
{
	int i = pending_index(name);

	if (!needs_restart(name))
	{
		dos_setting_set(name, value);
		menu.settings_changed = TRUE;
		return;
	}
	if (!_stricmp(dos_setting(name, ""), value))
	{
		if (i >= 0)
			menu.pending[i] = menu.pending[--menu.pending_count];
		return;
	}
	if (i < 0)
	{
		if (menu.pending_count >= (int)NUMBEROF(menu.pending))
			return;
		i = menu.pending_count++;
		snprintf(menu.pending[i].name, sizeof(menu.pending[i].name), "%s", name);
	}
	snprintf(menu.pending[i].value, sizeof(menu.pending[i].value), "%s", value);
}

/* (a game's row stays below the controls page's) */
typedef char dos_menu_game_rows_fit[ROW_NET_GAME + DOS_NETWORK_MAXIMUM_GAMES <= ROW_BIND ? 1 : -1];

/* where a listed game is, as its row's last column says it (short) or its
help line (long): retail Halo PC and Custom Edition servers are listed to
look at */
static const char *game_kind(const struct dos_network_game *game, int long_name)
{
	if (!game->compatible)
		return long_name ? "ANOTHER VERSION" : "OLD";
	switch (game->kind)
	{
	case DOS_GAME_CHUPATHINGY: return long_name ? "CHUPATHINGY CE" : "CHUPA";
	case DOS_GAME_OPENCE: return long_name ? "OPEN CE" : "OPENCE";
	/* (both announce network version 39: their listings are alike) */
	case DOS_GAME_EITHER: return long_name ? "CHUPATHINGY OR OPEN CE (THE LIST CANNOT TELL)" : "CH/OCE";
	case DOS_GAME_CUSTOM_EDITION: return long_name ? "HALO CUSTOM EDITION SERVER" : "CE";
	case DOS_GAME_RETAIL_PC: return long_name ? "HALO PC SERVER" : "PC";
	}
	return "LAN";
}

/* the rows' column on the page drawn (draw_page) */
static int list_x0 = 96, list_x1 = 544;

static const struct
{
	const char *name;
	int bits;
} colour_choices[] = { { "256 COLOURS", 8 }, { "THOUSANDS", 16 }, { "MILLIONS", 32 } };

/* Halo's armour colours (its colour indices), and how they look */
static const char *const armour_names[] = { "WHITE", "BLACK", "RED", "BLUE", "GRAY", "YELLOW", "GREEN", "PINK",
	"PURPLE", "CYAN", "COBALT", "ORANGE", "TEAL", "SAGE", "BROWN", "TAN", "MAROON", "SALMON" };
static const unsigned long armour_argb[] = { 0xFFF0F0F0UL, 0xFF303030UL, 0xFFD82020UL, 0xFF2050E0UL, 0xFF8C8C8CUL,
	0xFFF0D820UL, 0xFF30B030UL, 0xFFF080C0UL, 0xFF8040C8UL, 0xFF30D8E0UL, 0xFF4868B0UL, 0xFFF08020UL, 0xFF20A098UL,
	0xFF88A878UL, 0xFF704828UL, 0xFFC8A878UL, 0xFF801828UL, 0xFFF08878UL };
#define RED_TEAM_ARGB 0xFFD82020UL
#define BLUE_TEAM_ARGB 0xFF2050E0UL

/* ---------- input */

static int key_down(int key)
{
	return dos_input()->keys[key] != 0;
}

/* testing (setting menu_keys=down,down,enter,...): a key pressed every
half second, as if by the player (also wait, and esc); a count after a
key's name presses it that many times (down12), the setting's value being
short */
static int scripted_key;

static void scripted_keys(void)
{
	static const struct
	{
		const char *name;
		int key;
	} names[] = { { "up", SYSTEM_KEY_UP }, { "down", SYSTEM_KEY_DOWN }, { "left", SYSTEM_KEY_LEFT },
		{ "right", SYSTEM_KEY_RIGHT }, { "enter", SYSTEM_KEY_ENTER }, { "esc", SYSTEM_KEY_ESCAPE }, { "wait", 0 } };
	static const char *next;
	static unsigned long long at;
	static int repeat_key, repeats;
	unsigned long long now = system_microseconds();
	char token[16];
	int length = 0, count;

	scripted_key = 0;
	if (!next)
		next = menu_string("menu_keys", "");
	if (now < at)
		return;
	if (repeats > 0)
	{
		at = now + 500000;
		scripted_key = repeat_key;
		repeats--;
		return;
	}
	if (!*next)
		return;
	at = now + 500000;
	while (next[length] && next[length] != ',' && length < (int)sizeof(token) - 1)
	{
		token[length] = next[length];
		length++;
	}
	token[length] = 0;
	next += next[length] == ',' ? length + 1 : length;
	/* (wN: N half seconds' wait) */
	if ((token[0] == 'w' || token[0] == 'W') && token[1] >= '0' && token[1] <= '9')
	{
		at = now + 500000ULL * (unsigned long long)atoi(token + 1);
		return;
	}
	count = 1;
	for (int i = 0; token[i]; i++)
	{
		if (token[i] >= '0' && token[i] <= '9')
		{
			count = atoi(token + i);
			token[i] = 0;
			break;
		}
	}
	for (unsigned i = 0; i < NUMBEROF(names); i++)
	{
		if (!_stricmp(token, names[i].name))
			scripted_key = names[i].key;
	}
	if (scripted_key && count > 1)
	{
		repeat_key = scripted_key;
		repeats = count - 1;
	}
}

static int key_pressed(int key)
{
	if (scripted_key && key == scripted_key)
		return TRUE;
	return dos_input()->keys[key] && !menu.previous[key];
}

/* a direction held repeats: 0 up, 1 down, 2 left, 3 right */
static int direction(int which)
{
	static const int keys[4][2] = { { SYSTEM_KEY_UP, SYSTEM_KEY_W }, { SYSTEM_KEY_DOWN, SYSTEM_KEY_S },
		{ SYSTEM_KEY_LEFT, SYSTEM_KEY_A }, { SYSTEM_KEY_RIGHT, SYSTEM_KEY_D } };
	unsigned long long now = system_microseconds();

	if (key_pressed(keys[which][0]) || key_pressed(keys[which][1]))
	{
		menu.held_since[which] = now;
		menu.repeated_at[which] = now;
		return TRUE;
	}
	if ((key_down(keys[which][0]) || key_down(keys[which][1])) && now - menu.held_since[which] > REPEAT_DELAY &&
		now - menu.repeated_at[which] > REPEAT_EVERY)
	{
		menu.repeated_at[which] = now;
		return TRUE;
	}
	return FALSE;
}

static int choose_pressed(void)
{
	return key_pressed(SYSTEM_KEY_ENTER) || key_pressed(SYSTEM_KEY_KEYPAD_ENTER) || key_pressed(SYSTEM_KEY_SPACE);
}

static int back_pressed(void)
{
	return key_pressed(SYSTEM_KEY_ESCAPE) || key_pressed(SYSTEM_KEY_BACKSPACE);
}

static int any_key_down(void)
{
	const struct system_input *input = dos_input();

	for (int k = 1; k < SYSTEM_KEY_COUNT; k++)
	{
		if (input->keys[k])
			return TRUE;
	}
	return input->mouse_buttons != 0;
}

/* ---------- drawing

On the game's 640x480 screen, scaled to the picture's pixels: a page's
backdrop darkens the scene behind it; its title is on the left over a rule
(a note, the network's word, on the right); its rows are a column of a
setting's name and its value, or a button, the chosen one in a framed box,
its value between arrows and pulsing; under them a line of help for the
chosen row (or the last message), and the footer's keys. A list longer
than the room scrolls. Halo's fonts are made for 640x480: at low
resolutions the text is drawn larger (text_scale), and the help in the
large font. */

static long font_large = NONE, font_small = NONE;

static void find_fonts(void)
{
	font_large = tag_loaded('font', "ui\\large_ui");
	font_small = tag_loaded('font', "ui\\small_ui");
	if (font_small == NONE)
		font_small = font_large;
	if (font_large == NONE)
		font_large = font_small;
	/* (testing, setting debug_ui_tags=1: the map's bitmap and widget tags'
	names, once) */
	{
		static int listed;

		if (!listed && menu_integer("debug_ui_tags", 0))
		{
			static const unsigned long groups[] = { 'bitm', 'DeLa', 'font', 'ustr' };
			char line[256];

			listed = TRUE;
			for (int g = 0; g < (int)NUMBEROF(groups); g++)
			{
				struct tag_iterator iterator;
				long index;

				tag_iterator_new(&iterator, (long)groups[g]);
				while ((index = tag_iterator_next(&iterator)) != NONE)
				{
					snprintf(line, sizeof(line), "tag %c%c%c%c %s", (char)(groups[g] >> 24), (char)(groups[g] >> 16),
						(char)(groups[g] >> 8), (char)groups[g], tag_get_name(index));
					system_log(line);
				}
			}
		}
	}
}

/* a colour's alpha times f */
static unsigned long faded(unsigned long argb, float f)
{
	unsigned long alpha = (unsigned long)((float)(argb >> 24) * f + 0.5f);

	return (alpha > 255 ? 255UL : alpha) << 24 | (argb & 0x00FFFFFFUL);
}

static void fill(int x0, int y0, int x1, int y1, unsigned long argb)
{
	r_fill_rectangle((int)DOS_SCREEN_X(x0), (int)(y0 * dos_screen_scale_y), (int)DOS_SCREEN_X(x1),
		(int)(y1 * dos_screen_scale_y), argb);
}

/* the whole picture's width, from top to bottom (menus' units): a
backdrop, past the centred 640 on a wide screen */
static void fill_across(int y0, int y1, unsigned long argb)
{
	r_fill_rectangle(0, (int)(y0 * dos_screen_scale_y), (int)((float)dos_screen_width * dos_screen_scale_x + 0.5f),
		(int)(y1 * dos_screen_scale_y), argb);
}

static void frame_box(int x0, int y0, int x1, int y1, unsigned long argb)
{
	/* (a pixel at least) */
	int px = dos_screen_scale_x < 0.75f ? 2 : 1;

	fill(x0, y0, x1, y0 + px, argb);
	fill(x0, y1 - px, x1, y1, argb);
	fill(x0, y0 + px, x0 + px, y1 - px, argb);
	fill(x1 - px, y0 + px, x1, y1 - px, argb);
}

static real text_scale(void)
{
	return dos_screen_scale_y < 0.55f ? 1.15f : 1.0f;
}

/* the help's and the footer's font: at low resolutions the large one (the
small one's capitals would be four pixels tall) */
static long font_help(void)
{
	return dos_screen_scale_y < 0.55f ? font_large : font_small;
}

static void text_mode(long font, short justification, unsigned long argb)
{
	real_argb_color color;

	color.n[0] = (real)((argb >> 24) & 0xFF) / 255.0f;
	color.n[1] = (real)((argb >> 16) & 0xFF) / 255.0f;
	color.n[2] = (real)((argb >> 8) & 0xFF) / 255.0f;
	color.n[3] = (real)(argb & 0xFF) / 255.0f;
	draw_string_set_draw_mode(font, NONE, justification, 0, &color);
}

/* a string's width drawn at scale, in the screen's units */
static int text_width(long font, const char *string, real scale)
{
	rectangle2d bounds, text_bounds, cursor_bounds;

	if (font == NONE || !string || !*string)
		return 0;
	text_mode(font, JUSTIFY_LEFT, TEXT_BRIGHT);
	bounds.x0 = 0;
	bounds.y0 = 0;
	bounds.x1 = 4000;
	bounds.y1 = 480;
	draw_string_compute_bounds(&bounds, string, &text_bounds, &cursor_bounds);
	return (int)((real)(text_bounds.x1 - text_bounds.x0) * scale + 0.5f);
}

/* a line of text in a box (y0..y1 its line), anchored on the box's left,
right or middle as justified, scale times larger about that point: laid
out on one line, however long (the box's width does not wrap it) */
static void text_at_shadowed(long font, int x0, int y0, int x1, int y1, short justification, unsigned long argb,
	const char *string, real scale, int shadowed)
{
	rectangle2d bounds;
	real anchor = justification == JUSTIFY_LEFT ? (real)x0 : justification == JUSTIFY_RIGHT ? (real)x1 :
		(real)(x0 + x1) * 0.5f;

	if (font == NONE || !string || !*string)
		return;
	text_mode(font, justification, argb);
	bounds.x0 = (short)(justification == JUSTIFY_LEFT ? x0 : justification == JUSTIFY_RIGHT ? x1 - 2000 :
		(int)anchor - 1000);
	bounds.x1 = (short)(bounds.x0 + 2000);
	bounds.y0 = (short)y0;
	bounds.y1 = (short)y1;
	if (scale != 1.0f)
		rasterizer_text_set_scale(scale, anchor, (real)(y0 + y1) * 0.5f);
	rasterizer_text_set_shadow_color(shadowed ? faded(0xC0000000UL, (float)(argb >> 24) / 255.0f) : 0);
	rasterizer_draw_string(&bounds, NULL, NULL, 0, string);
	if (scale != 1.0f)
		rasterizer_text_set_scale(1.0f, 0.0f, 0.0f);
}

static void text_at(long font, int x0, int y0, int x1, int y1, short justification, unsigned long argb,
	const char *string, real scale)
{
	text_at_shadowed(font, x0, y0, x1, y1, justification, argb, string, scale, TRUE);
}

static void text(long font, int x0, int y0, int x1, int y1, short justification, unsigned long argb, const char *string)
{
	text_at(font, x0, y0, x1, y1, justification, argb, string, text_scale());
}

/* a line made smaller where it would not fit its box */
static void text_fitted(long font, int x0, int y0, int x1, int y1, short justification, unsigned long argb,
	const char *string)
{
	real scale = text_scale();
	int width = text_width(font, string, scale);

	if (width > x1 - x0)
		scale *= (real)(x1 - x0) / (real)width;
	text_at(font, x0, y0, x1, y1, justification, argb, string, scale);
}

/* a string cut to a width, with "..." where it was cut */
static void fit_string(long font, const char *string, int width, char *out, int size)
{
	real scale = text_scale();

	snprintf(out, size, "%s", string);
	if (text_width(font, out, scale) <= width)
		return;
	for (int length = (int)strlen(string) - 1; length > 0; length--)
	{
		while (length > 0 && string[length - 1] == ' ')
			length--;
		snprintf(out, size, "%.*s...", length, string);
		if (text_width(font, out, scale) <= width)
			return;
	}
	snprintf(out, size, "...");
}

/* the chosen row's text: between its colour and white, and back */
static unsigned long pulsing(void)
{
	real k = 0.5f + 0.5f * sinf((real)(system_microseconds() % 1600000ULL) * (6.2831853f / 1600000.0f));
	unsigned long red = 0x8C + (unsigned long)((real)(0xFF - 0x8C) * k);
	unsigned long green = 0xC0 + (unsigned long)((real)(0xFF - 0xC0) * k);

	return 0xFF000000UL | (red << 16) | (green << 8) | 0xFF;
}

static void chosen_box(int x0, int y0, int x1, int y1)
{
	fill(x0, y0, x1, y1, CHOSEN_COLOR);
	frame_box(x0, y0, x1, y1, FRAME_COLOR);
}

/* an arrow (a list's more rows) up or down, centred at x */
static void more_arrow(int x, int y, int down)
{
	for (int i = 0; i < 4; i++)
	{
		int half = down ? 8 - i * 2 : 2 + i * 2;

		fill(x - half, y + i * 2, x + half, y + i * 2 + 2, TEXT_DIM);
	}
}

/* how far the page has come in: 0 to 1 over its first fifth of a second,
easing out (a page slides in from the left and fades in) */
static float page_in(void)
{
	float t = (float)(system_microseconds() - menu.page_started) / 200000.0f;

	if (t < 0.0f)
		t = 0.0f;
	if (t > 1.0f)
		t = 1.0f;
	return 1.0f - (1.0f - t) * (1.0f - t);
}

/* a line glowing: its colour over a halo of glow (four faint copies about
it), as Halo's menu items and titles are */
static void text_glow(long font, int x0, int y0, int x1, int y1, short justification, unsigned long argb,
	unsigned long glow, const char *string, real scale)
{
	static const int offsets[4][2] = { { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } };

	if ((glow >> 24) && dos_render_settings.bits != 8)
	{
		for (int i = 0; i < 4; i++)
		{
			text_at_shadowed(font, x0 + offsets[i][0], y0 + offsets[i][1], x1 + offsets[i][0], y1 + offsets[i][1],
				justification, glow, string, scale, FALSE);
		}
	}
	text_at(font, x0, y0, x1, y1, justification, argb, string, scale);
}

/* a page's backdrop (the scene darkened) and its title glowing over a rule
that fades to the right (a note, the network's word, on the right) */
static void page(const char *title, const char *note)
{
	float in = page_in();
	int x = SAFE_LEFT + 16 - (int)(24.0f * (1.0f - in));

	fill_across(0, SCREEN_H, BACKDROP_COLOR);
	text_glow(font_large, x, 28, SAFE_RIGHT, 62, JUSTIFY_LEFT, faded(TEXT_BRIGHT, in), faded(GLOW_COLOR, in), title,
		text_scale() * 1.4f);
	if (note && note[0])
		text_fitted(font_help(), SCREEN_W / 2, 38, SAFE_RIGHT - 8, 60, JUSTIFY_RIGHT, faded(TEXT_DIM, in), note);
	for (int i = 0; i < 12; i++)
	{
		int x0 = SAFE_LEFT + 16 + i * (SAFE_RIGHT - SAFE_LEFT - 32) / 12;
		int x1 = SAFE_LEFT + 16 + (i + 1) * (SAFE_RIGHT - SAFE_LEFT - 32) / 12;

		fill(x0, 68, x1, 68 + LINE, faded(RULE_COLOR, in * (1.0f - (float)i / 12.0f)));
	}
}

/* ---------- the footer: the buttons of what was used last

A controller's A, B and its stick or D-pad (Halo's own button pictures)
after the controller was used, else the keys, each in a key cap. */

enum
{
	HINT_SELECT,
	HINT_BACK,
	HINT_CHANGE,
	HINT_CHOOSE,
	HINT_RESUME,
	HINT_CONTINUE,
	HINT_CLEAR,
	/* the split screen page's */
	HINT_JOIN,
	HINT_PLAY,
	HINT_LEAVE,
	/* the game list's */
	HINT_SHOW,
	HINT_SORT,
	HINT_FILTERS,
	HINT_QUICK,
	HINT_REFRESH,
	/* the name being typed */
	HINT_KEEP,
	HINT_UNDO,
	/* a join given up */
	HINT_CANCEL
};

static int pad_used_last(void)
{
	return dos_input()->pad_count > 0 && dos_pad_used_at() > dos_keyboard_used_at();
}

/* a key's cap: its name in a small framed box; how wide it is */
static int key_cap(int x, int y, const char *name, unsigned long color, int draw, real scale)
{
	long font = font_help();
	int width = text_width(font, name, scale * 0.9f) + 12;

	if (draw)
	{
		fill(x, y, x + width, y + 20, faded(0xC0081830UL, (float)(color >> 24) / 255.0f));
		frame_box(x, y, x + width, y + 20, faded(FRAME_COLOR, (float)(color >> 24) / 255.0f));
		text_at(font, x, y + 1, x + width, y + 19, JUSTIFY_CENTER, color, name, scale * 0.9f);
	}
	return width;
}

/* the footer's hints; both: each with the controller's button and the key
(the split screen page, read from every device at once) */
static void footer_drawn(const int *hints, int count, int both)
{
	static const char *const words[] = { "SELECT", "BACK", "CHANGE", "CHOOSE", "RESUME", "CONTINUE", "CLEAR", "JOIN",
		"PLAY", "LEAVE", "SHOW", "SORT", "FILTERS", "QUICK JOIN", "REFRESH", "KEEP", "UNDO", "CANCEL" };
	static const char *const keys[] = { "ENTER", "ESC", "LEFT/RIGHT", "UP/DOWN", "ESC", "ENTER", "DELETE", "ENTER",
		"ENTER", "ESC", "LEFT/RIGHT", "TAB", "F", "Q", "R", "ENTER", "ESC", "ESC" };
	/* (a controller's: xdk_input.c presses the keys for these) */
	static const char *const pad_buttons[] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, "D-PAD",
		"Y", "X", "BACK", "RB", NULL, NULL, NULL };
	int pad = pad_used_last(), y = FOOTER_Y + 12, total = 0, x, tries = 0;
	long font = font_help();
	real scale = text_scale();
	float in = page_in();

	fill(SAFE_LEFT, FOOTER_Y, SAFE_RIGHT, FOOTER_Y + LINE, faded(RULE_COLOR, 0.6f));
	/* (measured, made smaller while it is wider than the screen's middle,
	then drawn centred) */
	for (int pass = 0; pass < 2; pass++)
	{
		if (pass && total > SAFE_RIGHT - SAFE_LEFT - 16 && tries++ < 3)
		{
			scale *= (real)(SAFE_RIGHT - SAFE_LEFT - 16) / (real)total;
			total = 0;
			pass = -1;
			continue;
		}
		x = pass ? (SCREEN_W - total) / 2 : 0;
		for (int i = 0; i < count; i++)
		{
			int hint = hints[i], width, start = x;

			if (both)
			{
				/* (A, B or START, then the key) */
				if (hint == HINT_PLAY)
					width = key_cap(x, y + 1, "START", faded(TEXT_BRIGHT, in), pass, scale);
				else
				{
					width = 18;
					if (pass)
					{
						dos_art_draw(hint == HINT_JOIN ? DOS_ART_BUTTON_A : DOS_ART_BUTTON_B, 0, NULL, (float)x, (float)y,
							(float)(x + 18), (float)(y + 22), faded(0xFFFFFFFFUL, in));
					}
				}
				x += width + 4;
				if (pass)
					text_at(font, x, y + 2, x + 12, y + 20, JUSTIFY_LEFT, faded(TEXT_DIM, in), "/", scale);
				x += text_width(font, "/", scale) + 4;
				width = key_cap(x, y + 1, keys[hint], faded(TEXT_BRIGHT, in), pass, scale);
			}
			else if (pad && (hint == HINT_SELECT || hint == HINT_CONTINUE || hint == HINT_BACK || hint == HINT_RESUME ||
				hint == HINT_JOIN || hint == HINT_KEEP || hint == HINT_UNDO || hint == HINT_CANCEL))
			{
				/* (Halo's A and B) */
				enum dos_art art = hint == HINT_BACK || hint == HINT_RESUME || hint == HINT_UNDO || hint == HINT_CANCEL ?
					DOS_ART_BUTTON_B : DOS_ART_BUTTON_A;

				width = 18;
				if (pass)
					dos_art_draw(art, 0, NULL, (float)x, (float)y, (float)(x + 18), (float)(y + 22), faded(0xFFFFFFFFUL, in));
			}
			else if (pad && (hint == HINT_CHANGE || hint == HINT_CHOOSE))
			{
				width = key_cap(x, y + 1, hint == HINT_CHANGE ? "STICK LEFT/RIGHT" : "STICK UP/DOWN", faded(TEXT_BRIGHT, in),
					pass, scale);
			}
			else
			{
				width = key_cap(x, y + 1, pad && pad_buttons[hint] ? pad_buttons[hint] : keys[hint], faded(TEXT_BRIGHT, in),
					pass, scale);
			}
			x += width + 6;
			if (pass)
			{
				int word = text_width(font, words[hint], scale);

				text_at(font, x, y + 2, x + word, y + 20, JUSTIFY_LEFT, faded(TEXT_COLOR, in), words[hint], scale);
			}
			x += text_width(font, words[hint], scale) + 24;
			/* (each key a click does too) */
			if (pass)
				add_hit(start - 4, y - 4, x - 16, y + 26, HIT_HINT, hint);
		}
		total = x - 24;
	}
}

static void footer(const int *hints, int count)
{
	footer_drawn(hints, count, FALSE);
}

/* Halo's logo (ui.map's: the left 640x252 of its 1024x256 picture)
centred at x, its top at y, width wide: made at the picture's size once
(kept while the map's tag and the screen's scale stay), then copied each
frame. FALSE where the map has none */
static int draw_logo(int center, int top, int width)
{
	static struct
	{
		long tag_index;
		float scale_x, scale_y;
		int width, height;
		unsigned long *argb;
	} logo = { NONE };
	long tag_index = tag_loaded('bitm', "ui\\shell\\main_menu\\halo_logo");
	int height = width * 252 / 640;

	if (tag_index == NONE)
		return FALSE;
	if (logo.tag_index != tag_index || logo.scale_x != dos_screen_scale_x || logo.scale_y != dos_screen_scale_y)
	{
		(free)(logo.argb);
		logo.tag_index = tag_index;
		logo.scale_x = dos_screen_scale_x;
		logo.scale_y = dos_screen_scale_y;
		logo.width = (int)((float)width * dos_screen_scale_x + 0.5f);
		logo.height = (int)((float)height * dos_screen_scale_y + 0.5f);
		logo.argb = dos_interface_picture(tag_index, 640.0f / 1024.0f, 252.0f / 256.0f, logo.width, logo.height);
	}
	if (!logo.argb)
		return FALSE;
	r_draw_picture((int)DOS_SCREEN_X((float)(center - width / 2)), (int)((float)top * dos_screen_scale_y),
		logo.argb, logo.width, logo.height);
	return TRUE;
}

/* ---------- pages */

/* the DOS build (the same objects make the Windows 95 build: told apart by
their settings' file) */
/* ASPECT's choices: the setting aspect (0 the picture's own shape, else the
screen's width over its height times 100) */
static const struct
{
	long value;
	const char *name;
} aspects[] =
{
	{ 0, "AUTO" },
	{ 133, "4:3" },
	{ 160, "16:10" },
	{ 178, "16:9" },
	{ 233, "21:9" },
};

static int on_dos(void)
{
	return !_stricmp(system_settings_file(), "HALODOS.CFG");
}

/* the renderers the setting renderer chooses, on Windows 95: the software
one, OpenGL, Direct3D 8 (DOS: the software one or a Voodoo) */
enum
{
	_renderer_software,
	_renderer_opengl,
	_renderer_direct3d,
	NUMBER_OF_WINDOWS_RENDERERS
};

static int renderer_windows(void)
{
	const char *renderer = menu_string("renderer", "software");

	if (renderer && (!_stricmp(renderer, "opengl") || !_stricmp(renderer, "1")))
		return _renderer_opengl;
	if (renderer && (!_stricmp(renderer, "direct3d") || !_stricmp(renderer, "d3d") || !_stricmp(renderer, "d3d8")))
		return _renderer_direct3d;
	return _renderer_software;
}

/* whether the setting renderer asks for the hardware this system has (DOS:
a Voodoo, Windows 95: OpenGL or Direct3D) */
static int renderer_hardware(void)
{
	const char *renderer = menu_string("renderer", "software");

	if (!renderer)
		return FALSE;
	if (on_dos())
		return !_stricmp(renderer, "voodoo") || !_stricmp(renderer, "glide") || !_stricmp(renderer, "3dfx");
	return renderer_windows() != _renderer_software;
}

/* whether a row's setting waits for APPLY AND RESTART */
static int row_pending(int row)
{
	switch (row)
	{
	case ROW_SET_RENDERER: return pending_index("renderer") >= 0;
	case ROW_SET_RESOLUTION: return pending_index("width") >= 0 || pending_index("height") >= 0;
	case ROW_SET_COLOURS: return pending_index("bits") >= 0;
	case ROW_SET_ASPECT: return pending_index("aspect") >= 0;
	case ROW_SET_DOUBLE_WIDE: return pending_index("double_wide") >= 0;
	case ROW_SET_DOUBLE_TALL: return pending_index("double_tall") >= 0;
	case ROW_SET_TEXTURE_QUALITY: return pending_index("texture_size") >= 0;
	case ROW_SET_SOUND: return pending_index("sound") >= 0;
	}
	return FALSE;
}

/* the rows of a page */
static void set_rows(int screen)
{
	int n = 0;
	int teams = dos_game_types[dos_choice.game_type].teams;

	switch (screen)
	{
	case SCREEN_TITLE:
		menu.rows[n++] = ROW_TITLE_CAMPAIGN;
		menu.rows[n++] = ROW_TITLE_MULTIPLAYER;
		menu.rows[n++] = ROW_TITLE_SETTINGS;
		menu.rows[n++] = ROW_TITLE_QUIT;
		break;
	case SCREEN_MULTIPLAYER_MENU:
		menu.rows[n++] = ROW_TITLE_NETWORK;
		menu.rows[n++] = ROW_TITLE_SPLIT;
		menu.rows[n++] = ROW_MP_CUSTOM;
		menu.rows[n++] = ROW_TITLE_FIREFIGHT;
		menu.rows[n++] = ROW_BACK;
		break;
	case SCREEN_NETWORK:
		menu.rows[n++] = ROW_NET_QUICK;
		menu.rows[n++] = ROW_NET_JOIN;
		menu.rows[n++] = ROW_NET_HOST;
		menu.rows[n++] = ROW_PLAYERS;
		menu.rows[n++] = ROW_BACK;
		break;
	case SCREEN_NET_HOST:
		menu.rows[n++] = ROW_GAME_TYPE;
		menu.rows[n++] = ROW_MAP;
		menu.rows[n++] = ROW_SCORE;
		menu.rows[n++] = ROW_VEHICLES;
		menu.rows[n++] = ROW_PLAYERS;
		menu.rows[n++] = ROW_NET_CREATE;
		menu.rows[n++] = ROW_BACK;
		break;
	case SCREEN_NET_JOIN:
		/* (the games alone: the page's keys do the rest; BACK while there
		are none) */
		for (int i = 0; i < menu.game_count; i++)
			menu.rows[n++] = ROW_NET_GAME + i;
		if (!n)
			menu.rows[n++] = ROW_BACK;
		break;
	case SCREEN_LOBBY:
		if (dos_network_mode() == DOS_NETWORK_HOSTING)
			menu.rows[n++] = ROW_NET_START;
		menu.rows[n++] = ROW_NET_LEAVE;
		break;
	case SCREEN_CAMPAIGN:
		menu.rows[n++] = ROW_LEVEL;
		menu.rows[n++] = ROW_DIFFICULTY;
		menu.rows[n++] = ROW_PLAYERS;
		menu.rows[n++] = ROW_START;
		menu.rows[n++] = ROW_BACK;
		break;
	case SCREEN_MULTIPLAYER:
		menu.rows[n++] = ROW_GAME_TYPE;
		menu.rows[n++] = ROW_MAP;
		menu.rows[n++] = ROW_SCORE;
		menu.rows[n++] = ROW_BOTS;
		menu.rows[n++] = ROW_BOT_SKILL;
		/* (split screen's players chose their teams as they joined) */
		if (teams && !dos_split_count())
			menu.rows[n++] = ROW_TEAM;
		menu.rows[n++] = ROW_WEAPONS;
		menu.rows[n++] = ROW_VEHICLES;
		menu.rows[n++] = ROW_PLAYERS;
		menu.rows[n++] = ROW_START;
		menu.rows[n++] = ROW_BACK;
		break;
	case SCREEN_FIREFIGHT:
		menu.rows[n++] = ROW_MAP;
		menu.rows[n++] = ROW_DIFFICULTY;
		menu.rows[n++] = ROW_PLAYERS;
		menu.rows[n++] = ROW_START;
		menu.rows[n++] = ROW_BACK;
		break;
	case SCREEN_SETTINGS:
		menu.rows[n++] = ROW_SET_PLAYER;
		menu.rows[n++] = ROW_SET_DISPLAY_PAGE;
		menu.rows[n++] = ROW_SET_GRAPHICS_PAGE;
		menu.rows[n++] = ROW_SET_SOUND_PAGE;
		menu.rows[n++] = ROW_SET_CONTROLS;
		/* (a controller: Windows 95's, or a joystick on DOS's game port) */
		menu.rows[n++] = ROW_SET_CONTROLLER;
		if (menu.pending_count)
			menu.rows[n++] = ROW_SET_APPLY;
		menu.rows[n++] = ROW_BACK;
		break;
	case SCREEN_SET_PLAYER:
		menu.rows[n++] = ROW_SET_NAME;
		menu.rows[n++] = ROW_SET_NAME_COLOR;
		menu.rows[n++] = ROW_BACK;
		break;
	case SCREEN_SET_DISPLAY:
		menu.rows[n++] = ROW_SET_RENDERER;
		menu.rows[n++] = ROW_SET_RESOLUTION;
		menu.rows[n++] = ROW_SET_COLOURS;
		menu.rows[n++] = ROW_SET_ASPECT;
		/* (the software renderer's: a card draws at its own resolution) */
		if (!renderer_hardware())
		{
			menu.rows[n++] = ROW_SET_DOUBLE_WIDE;
			menu.rows[n++] = ROW_SET_DOUBLE_TALL;
		}
		/* (Windows 95's window or full screen: DOS's is the screen) */
		if (!on_dos())
		{
			menu.rows[n++] = ROW_SET_DISPLAY;
			menu.rows[n++] = ROW_SET_SCALING;
		}
		menu.rows[n++] = ROW_SET_FRAME_RATE;
		menu.rows[n++] = ROW_SET_FPS;
		if (menu.pending_count)
			menu.rows[n++] = ROW_SET_APPLY;
		menu.rows[n++] = ROW_BACK;
		break;
	case SCREEN_SET_GRAPHICS:
		menu.rows[n++] = ROW_SET_DETAIL;
		menu.rows[n++] = ROW_SET_DRAW_DISTANCE;
		menu.rows[n++] = ROW_SET_FIELD_OF_VIEW;
		menu.rows[n++] = ROW_SET_TEXTURE_QUALITY;
		menu.rows[n++] = ROW_SET_TEXTURES;
		menu.rows[n++] = ROW_SET_LIGHT;
		if (!on_dos() && renderer_windows() == _renderer_direct3d)
		{
			menu.rows[n++] = ROW_SET_D3D_LIGHTMAPS;
			menu.rows[n++] = ROW_SET_D3D_OBJECT_LIGHT;
			menu.rows[n++] = ROW_SET_D3D_TEXTURES;
		}
		menu.rows[n++] = ROW_SET_VIEW_MODEL;
		menu.rows[n++] = ROW_SET_PARTICLES;
		menu.rows[n++] = ROW_SET_SKY;
		if (menu.pending_count)
			menu.rows[n++] = ROW_SET_APPLY;
		menu.rows[n++] = ROW_BACK;
		break;
	case SCREEN_SET_SOUND:
		menu.rows[n++] = ROW_SET_SOUND;
		menu.rows[n++] = ROW_SET_VOLUME;
		if (menu.pending_count)
			menu.rows[n++] = ROW_SET_APPLY;
		menu.rows[n++] = ROW_BACK;
		break;
	case SCREEN_CONTROLS:
		menu.rows[n++] = ROW_SET_MOUSE;
		menu.rows[n++] = ROW_SET_INVERT;
		menu.rows[n++] = ROW_SET_AIM_ASSIST;
		for (int action = 0; action < DOS_ACTION_COUNT; action++)
			menu.rows[n++] = ROW_BIND + action;
		menu.rows[n++] = ROW_BIND_RESET;
		menu.rows[n++] = ROW_BACK;
		break;
	case SCREEN_CONTROLLER:
		menu.rows[n++] = ROW_PAD_DEVICE;
		menu.rows[n++] = ROW_PAD_ENABLED;
		menu.rows[n++] = ROW_PAD_LAYOUT;
		menu.rows[n++] = ROW_PAD_STICKS;
		menu.rows[n++] = ROW_PAD_SENSITIVITY;
		menu.rows[n++] = ROW_PAD_INVERT;
		menu.rows[n++] = ROW_SET_AIM_ASSIST;
		menu.rows[n++] = ROW_PAD_AIRCRAFT;
		/* (the game port's joysticks have no motors) */
		if (!on_dos())
			menu.rows[n++] = ROW_PAD_VIBRATION;
		menu.rows[n++] = ROW_BACK;
		break;
	case SCREEN_SPLIT:
		/* (the page reads its devices itself: split_input) */
		menu.rows[n++] = ROW_SPLIT_PLAYERS;
		break;
	case SCREEN_SPLIT_GAME:
		menu.rows[n++] = ROW_TITLE_NETWORK;
		menu.rows[n++] = ROW_MP_CUSTOM;
		menu.rows[n++] = ROW_TITLE_FIREFIGHT;
		menu.rows[n++] = ROW_TITLE_CAMPAIGN;
		menu.rows[n++] = ROW_SPLIT_PLAYERS;
		menu.rows[n++] = ROW_BACK;
		break;
	case SCREEN_RESULTS:
	case SCREEN_CE_JOIN:
		menu.rows[n++] = ROW_BACK;
		break;
	case SCREEN_PAUSE:
		menu.rows[n++] = ROW_PAUSE_RESUME;
		if (dos_choice.mode == DOS_MODE_NETWORK)
		{
			menu.rows[n++] = ROW_TITLE_SETTINGS;
			menu.rows[n++] = ROW_NET_LEAVE;
			menu.rows[n++] = ROW_PAUSE_QUIT;
			break;
		}
		if (dos_choice.mode == DOS_MODE_CAMPAIGN)
			menu.rows[n++] = ROW_PAUSE_REVERT;
		menu.rows[n++] = ROW_PAUSE_RESTART;
		menu.rows[n++] = ROW_TITLE_SETTINGS;
		menu.rows[n++] = ROW_PAUSE_MENU;
		menu.rows[n++] = ROW_PAUSE_QUIT;
		break;
	}
	menu.row_count = n;
	if (menu.row >= n)
		menu.row = n - 1;
	if (menu.row < 0)
		menu.row = 0;
}

static void go(int screen)
{
	/* (the title: one player again) */
	if (screen == SCREEN_TITLE)
		dos_split_clear();
	menu.page_started = system_microseconds();
	menu.screen = screen;
	menu.row = 0;
	menu.first_row = 0;
	menu.message[0] = 0;
	menu.filter_box = FALSE;
	menu.editing = FALSE;
	set_rows(screen);
}

/* a page, its row of that id chosen (the row that led where the player
comes back from) */
static void go_to_row(int screen, int row)
{
	go(screen);
	for (int i = 0; i < menu.row_count; i++)
	{
		if (menu.rows[i] == row)
			menu.row = i;
	}
}

/* ---------- the settings page */

static int nearest_resolution(int width, int height)
{
	int best = 0;
	long best_difference = 0x7FFFFFFF;

	for (int i = 0; i < menu.resolution_count; i++)
	{
		long difference = labs((long)menu.widths[i] - width) + labs((long)menu.heights[i] - height);

		if (difference < best_difference)
		{
			best_difference = difference;
			best = i;
		}
	}
	return best;
}

static void settings_open(void)
{
	int bits = (int)menu_integer("bits", 16);

	menu.colours = bits == 8 ? 0 : bits == 32 ? 2 : 1;
	menu.resolution_count = system_display_modes(colour_choices[menu.colours].bits, menu.widths, menu.heights,
		NUMBEROF(menu.widths));
	if (menu.resolution_count <= 0)
	{
		menu.resolution_count = 1;
		menu.widths[0] = (int)menu_integer("width", 320);
		menu.heights[0] = (int)menu_integer("height", 240);
	}
	menu.resolution = nearest_resolution((int)menu_integer("width", 320), (int)menu_integer("height", 240));
	menu.settings_changed = FALSE;
}

/* the FIELD OF VIEW row's degrees (the game's display.field_of_view, by
its short name) */
static int field_of_view(void)
{
	float degrees = dos_setting_real("field_of_view", 0.0f);

	if (degrees <= 0.0f)
		degrees = dos_setting_real("display.field_of_view", 70.0f);
	return PIN((int)(degrees + 0.5f), 50, 120);
}

/* the TEXTURE QUALITY row's steps: the largest side a texture is made at
(texture_size; ORIGINAL every texture at Halo's own size, up to 1024) */
static const int texture_sizes[] = { 32, 64, 128, 256, 1024 };

static int texture_quality(void)
{
	long size = menu_integer("texture_size", 128);
	int step = 0;

	while (step < (int)NUMBEROF(texture_sizes) - 1 && size > texture_sizes[step])
		step++;
	return step;
}

/* the TEXTURE FILTER row's choice: 0 sharp, 1 dithered, 2 smooth (the
picture's default: dithered in thousands or millions of colours) */
static int textures_mode(void)
{
	if (menu_integer("texture_smooth", 0))
		return 2;
	return menu_integer("texture_dither", colour_choices[menu.colours].bits != 8) ? 1 : 0;
}

static void setting_integer(const char *name, long value)
{
	char text_value[32];

	snprintf(text_value, sizeof(text_value), "%ld", value);
	menu_set(name, text_value);
	menu.settings_changed = TRUE;
}

/* a join page filter's setting: kept, the list sorted again with it */
static void filter_integer(const char *name, long value)
{
	char text_value[32];

	snprintf(text_value, sizeof(text_value), "%ld", value);
	menu_set(name, text_value);
	menu.filters_changed = TRUE;
	dos_network_resort();
}


static void settings_close(void)
{
	if (!menu.settings_changed)
		return;
	dos_settings_save();
	dos_render_reload_settings();
	menu.settings_changed = FALSE;
}

/* ---------- a row's value */

static int score_index(void)
{
	const struct dos_game_type *type = &dos_game_types[dos_choice.game_type];
	int i;

	for (i = 0; i < 5; i++)
	{
		if (type->scores[i] == dos_choice.score)
			return i;
	}
	return type->default_score;
}

static int step(int value, int delta, int count)
{
	value += delta;
	if (value < 0)
		value = count - 1;
	if (value >= count)
		value = 0;
	return value;
}

/* the next map of the list that is in the maps folder */
static int step_map(const struct dos_level *maps, int count, int value, int delta)
{
	for (int tries = 0; tries < count; tries++)
	{
		value = step(value, delta, count);
		if (dos_map_present(maps[value].folder))
			break;
	}
	return value;
}

static void change_value(int row, int delta);

/* a row's value stepped; the page's rows again where that changes them
(APPLY AND RESTART comes and goes with what waits for it, the renderer's own
rows with the renderer) */
static void change(int row, int delta)
{
	int pending = menu.pending_count;

	change_value(row, delta);
	if (pending != menu.pending_count || row == ROW_SET_RENDERER)
		set_rows(menu.screen);
}

static void change_value(int row, int delta)
{
	const struct dos_game_type *type = &dos_game_types[dos_choice.game_type];

	switch (row)
	{
	case ROW_LEVEL:
		dos_choice.level = step_map(dos_campaign_levels, dos_campaign_level_count, dos_choice.level, delta);
		break;
	case ROW_DIFFICULTY:
		dos_choice.difficulty = step(dos_choice.difficulty, delta, 4);
		break;
	case ROW_GAME_TYPE:
		dos_choice.game_type = step(dos_choice.game_type, delta, dos_game_type_count);
		/* (a system link game has no bots) */
		if (menu.screen == SCREEN_NET_HOST && dos_game_types[dos_choice.game_type].bot_match)
			dos_choice.game_type = step(dos_choice.game_type, delta, dos_game_type_count);
		type = &dos_game_types[dos_choice.game_type];
		dos_choice.score = type->scores[type->default_score];
		if (type->bot_match && dos_choice.bots < 2)
			dos_choice.bots = 2;
		set_rows(menu.screen);
		break;
	case ROW_MAP:
		dos_choice.map = step_map(dos_multiplayer_maps, dos_multiplayer_map_count, dos_choice.map, delta);
		/* (Firefight's Covenant join an Xbox map's tags: no Custom Edition
		map, dos_ce.c) */
		for (int tries = 0; menu.screen == SCREEN_FIREFIGHT && dos_map_is_custom_edition(dos_choice.map) &&
			tries < dos_multiplayer_map_count; tries++)
		{
			dos_choice.map = step_map(dos_multiplayer_maps, dos_multiplayer_map_count, dos_choice.map, delta);
		}
		break;
	case ROW_SCORE:
		dos_choice.score = type->scores[step(score_index(), delta, 5)];
		break;
	case ROW_BOTS:
		dos_choice.bots = PIN(dos_choice.bots + delta, type->bot_match ? 1 : 0, DOS_MAXIMUM_BOTS);
		dos_bots_prepare_lobby();
		break;
	case ROW_BOT_SKILL:
		dos_choice.bot_skill = step(dos_choice.bot_skill, delta, 4);
		break;
	case ROW_TEAM:
		dos_choice.team = step(dos_choice.team, delta, 2);
		break;
	case ROW_WEAPONS:
		dos_choice.weapons = step(dos_choice.weapons, delta, DOS_WEAPONS_COUNT);
		break;
	case ROW_VEHICLES:
		dos_choice.vehicles = step(dos_choice.vehicles, delta, DOS_VEHICLES_COUNT);
		break;
	case ROW_SET_RESOLUTION:
		menu.resolution = step(menu.resolution, delta, menu.resolution_count);
		setting_integer("width", menu.widths[menu.resolution]);
		setting_integer("height", menu.heights[menu.resolution]);
		snprintf(menu.message, sizeof(menu.message), "APPLY AND RESTART TO USE IT");
		break;
	case ROW_SET_ASPECT:
	{
		long current = menu_integer("aspect", 0);
		int i = 0;

		while (i < (int)NUMBEROF(aspects) - 1 && aspects[i].value != current)
			i++;
		setting_integer("aspect", aspects[step(i, delta, NUMBEROF(aspects))].value);
		snprintf(menu.message, sizeof(menu.message), "APPLY AND RESTART TO USE IT");
		break;
	}
	case ROW_SET_DOUBLE_WIDE:
		setting_integer("double_wide", !menu_integer("double_wide", 0));
		snprintf(menu.message, sizeof(menu.message), "APPLY AND RESTART TO USE IT");
		break;
	case ROW_SET_DOUBLE_TALL:
		setting_integer("double_tall", !menu_integer("double_tall", 0));
		snprintf(menu.message, sizeof(menu.message), "APPLY AND RESTART TO USE IT");
		break;
	case ROW_SET_COLOURS:
	{
		int width = menu.widths[menu.resolution], height = menu.heights[menu.resolution];

		menu.colours = step(menu.colours, delta, NUMBEROF(colour_choices));
		setting_integer("bits", colour_choices[menu.colours].bits);
		menu.resolution_count = system_display_modes(colour_choices[menu.colours].bits, menu.widths, menu.heights,
			NUMBEROF(menu.widths));
		if (menu.resolution_count <= 0)
		{
			menu.resolution_count = 1;
			menu.widths[0] = width;
			menu.heights[0] = height;
		}
		menu.resolution = nearest_resolution(width, height);
		setting_integer("width", menu.widths[menu.resolution]);
		setting_integer("height", menu.heights[menu.resolution]);
		snprintf(menu.message, sizeof(menu.message), "APPLY AND RESTART TO USE IT");
		break;
	}
	case ROW_SET_DRAW_DISTANCE:
	{
		static const int distances[] = { 40, 60, 90, 120, 180, 250, 400, 1000 };
		long current = menu_integer("draw_distance", 90);
		int i = 0;

		while (i < (int)NUMBEROF(distances) - 1 && distances[i] < current)
			i++;
		setting_integer("draw_distance", distances[PIN(i + delta, 0, (int)NUMBEROF(distances) - 1)]);
		break;
	}
	case ROW_SET_DETAIL:
		setting_integer("detail", PIN(menu_integer("detail", 2) + delta, 0, 3));
		break;
	case ROW_SET_DISPLAY:
		/* (Windows 95: a window, the whole screen, the whole screen in a
		display mode of its own; at once) */
		setting_integer("fullscreen", step((int)PIN(menu_integer("fullscreen", 0), 0, 2), delta, 3));
		system_display_settings_changed();
		break;
	case ROW_SET_SCALING:
		setting_integer("scaling", step((int)PIN(menu_integer("scaling", 0), 0, 2), delta, 3));
		system_display_settings_changed();
		break;
	case ROW_SET_RENDERER:
	{
		/* (DOS: the software renderer or a 3dfx Voodoo; Windows 95: or
		OpenGL, or Direct3D 8, in turn) */
		static const char *const windows[NUMBER_OF_WINDOWS_RENDERERS] = { "software", "opengl", "direct3d" };

		if (on_dos())
			menu_set("renderer", renderer_hardware() ? "software" : "voodoo");
		else
			menu_set("renderer", windows[step(renderer_windows(), delta, NUMBER_OF_WINDOWS_RENDERERS)]);
		menu.settings_changed = TRUE;
		break;
	}
	case ROW_SET_FIELD_OF_VIEW:
		/* (the game's display.field_of_view: degrees across the first-person
		view, 50 to 120, in fives) */
		setting_integer("field_of_view", PIN(field_of_view() / 5 * 5 + 5 * delta, 50, 120));
		break;
	case ROW_SET_TEXTURE_QUALITY:
		setting_integer("texture_size", texture_sizes[PIN(texture_quality() + delta, 0,
			(int)NUMBEROF(texture_sizes) - 1)]);
		break;
	case ROW_SET_TEXTURES:
	{
		/* (magnified texels: squares, dithered into each other, or
		filtered: texture_dither, texture_smooth) */
		int mode = PIN(textures_mode() + delta, 0, 2);

		setting_integer("texture_dither", mode >= 1);
		setting_integer("texture_smooth", mode == 2);
		break;
	}
	case ROW_SET_LIGHT:
		setting_integer("level_light", !menu_integer("level_light", 1));
		break;
	case ROW_SET_D3D_LIGHTMAPS:
		setting_integer("d3d_lightmaps", !menu_integer("d3d_lightmaps", 1));
		break;
	case ROW_SET_D3D_OBJECT_LIGHT:
		setting_integer("d3d_object_light", !menu_integer("d3d_object_light", 1));
		break;
	case ROW_SET_D3D_TEXTURES:
		setting_integer("d3d_textures", !menu_integer("d3d_textures", 1));
		break;
	case ROW_SET_VIEW_MODEL:
	{
		/* (in the row's order: 4 very low, 0 low, 1 medium, 2 high, 3 off:
		the N64 port's choices and one lower; the numbers are the settings'
		from before VERY LOW) */
		static const int order[] = { 4, 0, 1, 2, 3 };
		int current = PIN((int)menu_integer("view_model", 2), 0, 4), i = 0;

		while (i < 4 && order[i] != current)
			i++;
		setting_integer("view_model", order[step(i, delta, 5)]);
		break;
	}
	case ROW_SET_PARTICLES:
		setting_integer("particles", !menu_integer("particles", 1));
		break;
	case ROW_PAD_ENABLED:
		setting_integer("controller", !menu_integer("controller", 1));
		break;
	case ROW_PAD_LAYOUT:
		setting_integer("controller_layout", step((int)PIN(menu_integer("controller_layout", 0), 0, 5), delta, 6));
		break;
	case ROW_PAD_STICKS:
		setting_integer("stick_layout", step((int)PIN(menu_integer("stick_layout", 0), 0, 3), delta, 4));
		break;
	case ROW_PAD_SENSITIVITY:
		setting_integer("look_sensitivity", PIN(menu_integer("look_sensitivity", 3) + delta, 1, 10));
		break;
	case ROW_PAD_INVERT:
		setting_integer("invert_look", !menu_integer("invert_look", 0));
		break;
	case ROW_PAD_AIRCRAFT:
		setting_integer("flight_stick_aircraft", !menu_integer("flight_stick_aircraft", 0));
		break;
	case ROW_PAD_VIBRATION:
		setting_integer("vibration", !menu_integer("vibration", 1));
		break;
	case ROW_SET_SOUND:
		setting_integer("sound", !menu_integer("sound", 1));
		break;
	case ROW_SET_VOLUME:
	{
		/* (tenths, 0 to 100%) */
		char value[16];
		int tenths = (int)(dos_setting_real("sound_volume", 1.0f) * 10.0f + 0.5f);

		snprintf(value, sizeof(value), "%.1f", (double)PIN(tenths + delta, 0, 10) / 10.0);
		menu_set("sound_volume", value);
		menu.settings_changed = TRUE;
		break;
	}
	case ROW_SET_SKY:
		setting_integer("sky_model", !menu_integer("sky_model", 1));
		break;
	case ROW_SET_MOUSE:
	{
		char value[16];
		float sensitivity = dos_setting_real("mouse_sensitivity", 1.0f) + 0.25f * delta;

		snprintf(value, sizeof(value), "%.2f", (double)PIN(sensitivity, 0.25f, 5.0f));
		menu_set("mouse_sensitivity", value);
		menu.settings_changed = TRUE;
		break;
	}
	case ROW_SET_INVERT:
		setting_integer("invert_mouse", !menu_integer("invert_mouse", 0));
		break;
	case ROW_SET_AIM_ASSIST:
		setting_integer("aim_assist", !menu_integer("aim_assist", 0));
		break;
	case ROW_SET_FPS:
		setting_integer("fps_counter", !menu_integer("fps_counter", 0));
		break;
	case ROW_FILTER_FULL:
		filter_integer("browser_full", !menu_integer("browser_full", 0));
		break;
	case ROW_FILTER_EMPTY:
		filter_integer("browser_empty", !menu_integer("browser_empty", 0));
		break;
	case ROW_FILTER_JOINABLE:
		filter_integer("browser_joinable", !menu_integer("browser_joinable", 0));
		break;
	case ROW_SET_FRAME_RATE:
	{
		/* (30, Halo's own; 60 and 120 drawn between its ticks; 0 uncapped) */
		static const int rates[] = { 30, 60, 120, 0 };
		int current = (int)menu_integer("frame_rate", 30), i = 0;

		while (i < 3 && rates[i] != current)
			i++;
		setting_integer("frame_rate", rates[step(i, delta, 4)]);
		break;
	}
	case ROW_SET_NAME_COLOR:
	{
		/* (Halo's colour indices: the free-for-all armour) */
		setting_integer("player_color", step((int)menu_integer("player_color", 2), delta, NUMBEROF(armour_names)));
		break;
	}
	}
}

/* what a row shows: its label and value (NULL: a button) */
static const char *row_label(int row, char *value, int size, int *stepped)
{
	const struct dos_game_type *type = &dos_game_types[dos_choice.game_type];

	*stepped = TRUE;
	value[0] = 0;
	if (row >= ROW_BIND && row < ROW_BIND + DOS_ACTION_COUNT)
	{
		int action = row - ROW_BIND;

		*stepped = FALSE;
		if (menu.binding == action)
			snprintf(value, size, "PRESS A KEY");
		else
		{
			for (int slot = 0; slot < BIND_SLOTS; slot++)
			{
				int code = dos_binding(action, slot);

				if (code == BIND_NONE)
					continue;
				if (value[0])
					strncat(value, ", ", (size_t)size - strlen(value) - 1);
				strncat(value, dos_binding_name(code), (size_t)size - strlen(value) - 1);
			}
			if (!value[0])
				snprintf(value, size, "NONE");
		}
		return dos_action_name(action);
	}
	switch (row)
	{
	case ROW_PAD_DEVICE:
		*stepped = FALSE;
		snprintf(value, size, "%s", system_pad_description());
		return "CONTROLLER";
	case ROW_PAD_ENABLED:
		snprintf(value, size, "%s", menu_integer("controller", 1) ? "ON" : "OFF");
		return "USE A CONTROLLER";
	case ROW_PAD_LAYOUT:
	{
		static const char *const names[] = { "DEFAULT", "SOUTHPAW", "JUMPY", "BOXER", "GREEN THUMB", "BUMPER JUMPER" };

		snprintf(value, size, "%s", names[PIN(menu_integer("controller_layout", 0), 0, 5)]);
		return "BUTTON LAYOUT";
	}
	case ROW_PAD_STICKS:
	{
		static const char *const names[] = { "DEFAULT", "SOUTHPAW", "LEGACY", "LEGACY SOUTHPAW" };

		snprintf(value, size, "%s", names[PIN(menu_integer("stick_layout", 0), 0, 3)]);
		return "THUMBSTICK LAYOUT";
	}
	case ROW_PAD_SENSITIVITY:
		snprintf(value, size, "%d", (int)PIN(menu_integer("look_sensitivity", 3), 1, 10));
		return "LOOK SENSITIVITY";
	case ROW_PAD_INVERT:
		snprintf(value, size, "%s", menu_integer("invert_look", 0) ? "ON" : "OFF");
		return "INVERT LOOK";
	case ROW_PAD_AIRCRAFT:
		snprintf(value, size, "%s", menu_integer("flight_stick_aircraft", 0) ? "FLIGHT STICK" : "NORMAL");
		return "AIRCRAFT CONTROLS";
	case ROW_PAD_VIBRATION:
		snprintf(value, size, "%s", menu_integer("vibration", 1) ? "ON" : "OFF");
		return "VIBRATION";
	case ROW_LEVEL:
		snprintf(value, size, "%s", dos_campaign_levels[dos_choice.level].name);
		return "LEVEL";
	case ROW_DIFFICULTY:
		snprintf(value, size, "%s", dos_difficulty_names[dos_choice.difficulty]);
		return "DIFFICULTY";
	case ROW_GAME_TYPE:
		snprintf(value, size, "%s", type->name);
		return "GAME TYPE";
	case ROW_MAP:
		snprintf(value, size, "%s", dos_multiplayer_maps[dos_choice.map].name);
		return "MAP";
	case ROW_SCORE:
		snprintf(value, size, "%d %s", type->scores[score_index()], type->unit);
		return "SCORE TO WIN";
	case ROW_BOTS:
		if (dos_choice.bots)
			snprintf(value, size, "%d%s", dos_choice.bots, dos_choice.bots >= 8 ? " (SLOW)" : "");
		else
			snprintf(value, size, "NONE");
		return "BOTS";
	case ROW_BOT_SKILL:
		snprintf(value, size, "%s", dos_difficulty_names[dos_choice.bot_skill]);
		return "BOT SKILL";
	case ROW_TEAM:
		snprintf(value, size, "%s", dos_choice.team ? "BLUE" : "RED");
		return "YOUR TEAM";
	case ROW_WEAPONS:
		snprintf(value, size, "%s", dos_choice.weapons == DOS_WEAPONS_PISTOL_RIFLE ? "PISTOL + AR" : "MAP DEFAULT");
		return "WEAPONS";
	case ROW_VEHICLES:
		snprintf(value, size, "%s", dos_choice.vehicles == DOS_VEHICLES_NONE ? "NONE" : "MAP DEFAULT");
		return "VEHICLES";
	case ROW_SET_RESOLUTION:
		snprintf(value, size, "%d X %d", menu.widths[menu.resolution], menu.heights[menu.resolution]);
		return "RESOLUTION";
	case ROW_SET_ASPECT:
	{
		long current = menu_integer("aspect", 0);
		const char *name = "AUTO";

		for (unsigned i = 0; i < NUMBEROF(aspects); i++)
		{
			if (aspects[i].value == current)
				name = aspects[i].name;
		}
		snprintf(value, size, "%s", name);
		return "ASPECT";
	}
	case ROW_SET_DOUBLE_WIDE:
		snprintf(value, size, "%s", menu_integer("double_wide", 0) ? "ON" : "OFF");
		return "DOUBLE WIDE";
	case ROW_SET_DOUBLE_TALL:
		snprintf(value, size, "%s", menu_integer("double_tall", 0) ? "ON" : "OFF");
		return "DOUBLE TALL";
	case ROW_SET_COLOURS:
		snprintf(value, size, "%s", colour_choices[menu.colours].name);
		return "COLOURS";
	case ROW_SET_DRAW_DISTANCE:
		snprintf(value, size, "%ld", menu_integer("draw_distance", 90));
		return "DRAW DISTANCE";
	case ROW_SET_DETAIL:
	{
		static const char *const names[] = { "LOWEST", "LOW", "NORMAL", "HIGH" };

		snprintf(value, size, "%s", names[PIN(menu_integer("detail", 2), 0, 3)]);
		return "DETAIL";
	}
	case ROW_SET_FIELD_OF_VIEW:
		snprintf(value, size, "%d", field_of_view());
		return "FIELD OF VIEW";
	case ROW_SET_DISPLAY:
	{
		static const char *const names[] = { "WINDOW", "FULL SCREEN", "EXCLUSIVE" };

		snprintf(value, size, "%s", names[PIN(menu_integer("fullscreen", 0), 0, 2)]);
		return "DISPLAY";
	}
	case ROW_SET_SCALING:
	{
		static const char *const names[] = { "SHARP", "FILL", "STRETCH" };

		snprintf(value, size, "%s", names[PIN(menu_integer("scaling", 0), 0, 2)]);
		return "SCALING";
	}
	case ROW_SET_RENDERER:
		snprintf(value, size, "%s", !renderer_hardware() ? "SOFTWARE" : on_dos() ? "3DFX VOODOO" :
			renderer_windows() == _renderer_direct3d ? "DIRECT3D 8" : "OPENGL");
		return "RENDERER";
	case ROW_SET_TEXTURES:
	{
		static const char *const names[] = { "SHARP", "DITHERED", "SMOOTH" };

		/* (256 colours: always sharp, the help says) */
		snprintf(value, size, "%s", names[textures_mode()]);
		return "TEXTURE FILTER";
	}
	case ROW_SET_TEXTURE_QUALITY:
	{
		static const char *const names[] = { "LOWEST", "LOW", "MEDIUM", "HIGH", "ORIGINAL" };

		snprintf(value, size, "%s", names[texture_quality()]);
		return "TEXTURE QUALITY";
	}
	case ROW_SET_LIGHT:
		snprintf(value, size, "%s", menu_integer("level_light", 1) ? "ON" : "OFF");
		return "LEVEL LIGHT";
	case ROW_SET_D3D_LIGHTMAPS:
		snprintf(value, size, "%s", menu_integer("d3d_lightmaps", 1) ? "XBOX" : "VERTEX");
		return "LIGHTMAPS";
	case ROW_SET_D3D_OBJECT_LIGHT:
		snprintf(value, size, "%s", menu_integer("d3d_object_light", 1) ? "XBOX" : "FLAT");
		return "OBJECT LIGHT";
	case ROW_SET_D3D_TEXTURES:
		snprintf(value, size, "%s", menu_integer("d3d_textures", 1) ? "XBOX" : "BAKED");
		return "SHADERS";
	case ROW_SET_VIEW_MODEL:
	{
		static const char *const names[] = { "LOW", "MEDIUM", "HIGH", "OFF", "VERY LOW" };

		snprintf(value, size, "%s", names[PIN(menu_integer("view_model", 2), 0, 4)]);
		return "VIEW MODEL";
	}
	case ROW_SET_PARTICLES:
		snprintf(value, size, "%s", menu_integer("particles", 1) ? "ON" : "OFF");
		return "PARTICLES";
	case ROW_SET_SOUND:
		snprintf(value, size, "%s", menu_integer("sound", 1) ? "ON" : "OFF");
		return "SOUND";
	case ROW_SET_VOLUME:
		snprintf(value, size, "%d%%", (int)(dos_setting_real("sound_volume", 1.0f) * 100.0f + 0.5f));
		return "VOLUME";
	case ROW_SET_SKY:
		snprintf(value, size, "%s", menu_integer("sky_model", 1) ? "MODEL" : "GRADIENT");
		return "SKY";
	case ROW_SET_MOUSE:
		snprintf(value, size, "%.2f", (double)dos_setting_real("mouse_sensitivity", 1.0f));
		return "MOUSE SPEED";
	case ROW_SET_INVERT:
		snprintf(value, size, "%s", menu_integer("invert_mouse", 0) ? "ON" : "OFF");
		return "INVERT MOUSE";
	case ROW_SET_AIM_ASSIST:
		snprintf(value, size, "%s", menu_integer("aim_assist", 0) ? "ON" : "OFF");
		return "AIM ASSIST";
	case ROW_SET_FPS:
		snprintf(value, size, "%s", menu_integer("fps_counter", 0) ? "ON" : "OFF");
		return "FPS COUNTER";
	case ROW_FILTER_FULL:
		snprintf(value, size, "%s", menu_integer("browser_full", 0) ? "HIDE" : "SHOW");
		return "FULL GAMES";
	case ROW_FILTER_EMPTY:
		snprintf(value, size, "%s", menu_integer("browser_empty", 0) ? "HIDE" : "SHOW");
		return "EMPTY GAMES";
	case ROW_FILTER_JOINABLE:
		snprintf(value, size, "%s", menu_integer("browser_joinable", 0) ? "ONLY THESE" : "ALL");
		return "JOINABLE";
	case ROW_SET_FRAME_RATE:
	{
		int rate = (int)menu_integer("frame_rate", 30);

		if (rate <= 0)
			snprintf(value, size, "UNCAPPED");
		else
			snprintf(value, size, "%d", rate < 30 ? 30 : rate);
		return "FRAME RATE";
	}
	case ROW_SET_NAME_COLOR:
		snprintf(value, size, "%s", armour_names[PIN(menu_integer("player_color", 2), 0, 17)]);
		return "ARMOUR COLOUR";
	}
	*stepped = FALSE;
	value[0] = 0;
	switch (row)
	{
	case ROW_SET_NAME:
		/* (being typed: its cursor blinking where the next letter goes) */
		if (menu.editing)
		{
			int blink = (system_microseconds() / 400000ULL) & 1;

			snprintf(value, size, "%.*s%s%s", menu.edit_cursor, menu.edit_text, blink ? "_" : " ",
				menu.edit_text + menu.edit_cursor);
		}
		else
			snprintf(value, size, "%s", menu_string("player_name", "Player"));
		return "NAME";
	case ROW_PLAYERS:
		/* (the players at this screen: one, or split screen's) */
		if (dos_split_count() >= 2)
			snprintf(value, size, "%d, SPLIT SCREEN", dos_split_count());
		else
			snprintf(value, size, "1");
		return "PLAYERS";
	case ROW_SET_APPLY:
		snprintf(value, size, "%d CHANGE%s", menu.pending_count, menu.pending_count == 1 ? "" : "S");
		return "APPLY AND RESTART";
	}
	if (row >= ROW_NET_GAME && row < ROW_NET_GAME + menu.game_count)
	{
		const struct dos_network_game *game = &menu.games[row - ROW_NET_GAME];

		*stepped = TRUE;
		snprintf(value, size, "%s  %d/%d%s  %s", game->map, game->players, game->maximum_players,
			game->in_progress ? "  PLAYING" : "", game_kind(game, FALSE));
		return game->name;
	}
	switch (row)
	{
	case ROW_TITLE_NETWORK: return "ONLINE";
	case ROW_NET_HOST: return "HOST A GAME";
	case ROW_NET_JOIN: return "BROWSE GAMES";
	case ROW_NET_CREATE: return "HOST";
	case ROW_NET_START: return "START GAME";
	case ROW_NET_LEAVE: return menu.paused ? "LEAVE GAME" : "LEAVE";
	case ROW_NET_QUICK: return "QUICK JOIN";
	case ROW_MP_CUSTOM: return "CUSTOM GAME";
	case ROW_START: return "START GAME";
	case ROW_BACK: return menu.screen == SCREEN_CE_JOIN && dos_ce_active() ? "CANCEL" : "BACK";
	case ROW_TITLE_CAMPAIGN: return menu.screen == SCREEN_SPLIT_GAME ? "CAMPAIGN CO-OP" : "CAMPAIGN";
	case ROW_TITLE_MULTIPLAYER: return "MULTIPLAYER";
	case ROW_TITLE_FIREFIGHT: return "FIREFIGHT";
	case ROW_TITLE_SPLIT: return "SPLIT SCREEN";
	case ROW_SPLIT_PLAYERS: return "CHANGE PLAYERS";
	case ROW_TITLE_SETTINGS: return "SETTINGS";
	case ROW_TITLE_QUIT: return "QUIT";
	case ROW_SET_CONTROLS: return "CONTROLS";
	case ROW_SET_CONTROLLER: return "CONTROLLER";
	case ROW_SET_PLAYER: return "PLAYER";
	case ROW_SET_DISPLAY_PAGE: return "DISPLAY";
	case ROW_SET_GRAPHICS_PAGE: return "GRAPHICS";
	case ROW_SET_SOUND_PAGE: return "SOUND";
	case ROW_BIND_RESET: return "RESET TO DEFAULTS";
	case ROW_PAUSE_RESUME: return "RESUME";
	case ROW_PAUSE_REVERT: return "REVERT TO CHECKPOINT";
	case ROW_PAUSE_RESTART: return dos_choice.mode == DOS_MODE_CAMPAIGN ? "RESTART LEVEL" : "RESTART GAME";
	case ROW_PAUSE_MENU: return "QUIT TO MENU";
	case ROW_PAUSE_QUIT: return "QUIT HALO";
	}
	return "";
}

/* ---------- choosing */

static void split_open(void);

static void quit_halo(void)
{
	settings_close();
	system_log("quit from the menu");
	system_display_close();
	exit(0);
}

static void pause_end(void)
{
	settings_close();
	menu.paused = FALSE;
	menu.holding = TRUE;
	game_time_set_paused(FALSE);
}

static void start_game(void)
{
	/* (the maps the choice needs: one that is there) */
	if (dos_choice.mode == DOS_MODE_CAMPAIGN && !dos_map_present(dos_campaign_levels[dos_choice.level].folder))
	{
		snprintf(menu.message, sizeof(menu.message), "THAT LEVEL IS NOT IN THE MAPS FOLDER");
		return;
	}
	if (dos_choice.mode != DOS_MODE_CAMPAIGN && !dos_map_present(dos_multiplayer_maps[dos_choice.map].folder))
	{
		snprintf(menu.message, sizeof(menu.message), "THAT MAP IS NOT IN THE MAPS FOLDER");
		return;
	}
	if (dos_choice.mode == DOS_MODE_FIREFIGHT && dos_map_is_custom_edition(dos_choice.map))
	{
		snprintf(menu.message, sizeof(menu.message), "FIREFIGHT NEEDS ONE OF THE XBOX'S MAPS");
		return;
	}
	menu.active = FALSE;
	menu.holding = TRUE;
	menu.game_screen = menu.screen;
	dos_game_begin();
}

/* the settings waiting for APPLY written, and Halo started again with them
(system_restart: Windows starts it again itself, DOS's PLAY.BAT does) */
static void apply_and_restart(void)
{
	for (int i = 0; i < menu.pending_count; i++)
		dos_setting_set(menu.pending[i].name, menu.pending[i].value);
	menu.pending_count = 0;
	dos_settings_save();
	system_log("settings applied: Halo starts again");
	system_display_close();
	system_restart();
}

/* the PLAYER page's name typed: its letters from the settings', the keys
typed until now dropped */
static void edit_begin(void)
{
	int key, character, down;

	snprintf(menu.edit_text, sizeof(menu.edit_text), "%s", menu_string("player_name", "Player"));
	menu.edit_cursor = (int)strlen(menu.edit_text);
	menu.editing = TRUE;
	while (system_next_keystroke(&key, &character, &down))
		;
}

static void choose(int row);

/* where BACK goes from a page: the page above it, its row that led here
chosen (split screen's pages and the settings' come back to where they were
opened from) */
static void go_back(void)
{
	int split = dos_split_count() >= 2;

	switch (menu.screen)
	{
	case SCREEN_MULTIPLAYER_MENU:
		go_to_row(SCREEN_TITLE, ROW_TITLE_MULTIPLAYER);
		break;
	case SCREEN_CAMPAIGN:
		if (split)
			go_to_row(SCREEN_SPLIT_GAME, ROW_TITLE_CAMPAIGN);
		else
			go_to_row(SCREEN_TITLE, ROW_TITLE_CAMPAIGN);
		break;
	case SCREEN_MULTIPLAYER:
		go_to_row(split ? SCREEN_SPLIT_GAME : SCREEN_MULTIPLAYER_MENU, ROW_MP_CUSTOM);
		break;
	case SCREEN_FIREFIGHT:
		go_to_row(split ? SCREEN_SPLIT_GAME : SCREEN_MULTIPLAYER_MENU, ROW_TITLE_FIREFIGHT);
		break;
	case SCREEN_NETWORK:
		go_to_row(split ? SCREEN_SPLIT_GAME : SCREEN_MULTIPLAYER_MENU, ROW_TITLE_NETWORK);
		break;
	case SCREEN_NET_HOST:
		go_to_row(SCREEN_NETWORK, ROW_NET_HOST);
		break;
	case SCREEN_NET_JOIN:
		/* (the search over) */
		dos_network_leave();
		go_to_row(SCREEN_NETWORK, ROW_NET_JOIN);
		break;
	case SCREEN_CE_JOIN:
		/* (the join and its download given up: the games again) */
		if (dos_ce_active())
			dos_ce_leave();
		choose(ROW_NET_JOIN);
		break;
	case SCREEN_SPLIT_GAME:
		go(SCREEN_SPLIT);
		break;
	case SCREEN_SET_PLAYER:
		go_to_row(SCREEN_SETTINGS, ROW_SET_PLAYER);
		break;
	case SCREEN_SET_DISPLAY:
		go_to_row(SCREEN_SETTINGS, ROW_SET_DISPLAY_PAGE);
		break;
	case SCREEN_SET_GRAPHICS:
		go_to_row(SCREEN_SETTINGS, ROW_SET_GRAPHICS_PAGE);
		break;
	case SCREEN_SET_SOUND:
		go_to_row(SCREEN_SETTINGS, ROW_SET_SOUND_PAGE);
		break;
	case SCREEN_CONTROLS:
		go_to_row(SCREEN_SETTINGS, ROW_SET_CONTROLS);
		break;
	case SCREEN_CONTROLLER:
		go_to_row(SCREEN_SETTINGS, ROW_SET_CONTROLLER);
		break;
	case SCREEN_SETTINGS:
	{
		/* (out of the settings: what changes as Halo runs kept, what waits
		for a restart dropped, said so) */
		int dropped = menu.pending_count;

		menu.pending_count = 0;
		settings_close();
		go_to_row(menu.paused ? SCREEN_PAUSE : SCREEN_TITLE, ROW_TITLE_SETTINGS);
		if (dropped)
			snprintf(menu.message, sizeof(menu.message), "NOT APPLIED: THE CHANGES THAT NEED A RESTART");
		break;
	}
	case SCREEN_RESULTS:
		go(dos_network_mode() != DOS_NETWORK_OFF ? SCREEN_LOBBY : dos_choice.mode == DOS_MODE_CAMPAIGN ?
			SCREEN_CAMPAIGN : dos_choice.mode == DOS_MODE_FIREFIGHT ? SCREEN_FIREFIGHT : SCREEN_MULTIPLAYER);
		break;
	default:
		go(SCREEN_TITLE);
		break;
	}
}

static void choose(int row)
{
	switch (row)
	{
	case ROW_TITLE_CAMPAIGN:
		dos_choice.mode = DOS_MODE_CAMPAIGN;
		go(SCREEN_CAMPAIGN);
		break;
	case ROW_TITLE_MULTIPLAYER:
		go(SCREEN_MULTIPLAYER_MENU);
		break;
	case ROW_MP_CUSTOM:
		dos_choice.mode = DOS_MODE_MULTIPLAYER;
		go(SCREEN_MULTIPLAYER);
		break;
	case ROW_TITLE_FIREFIGHT:
		dos_choice.mode = DOS_MODE_FIREFIGHT;
		go(SCREEN_FIREFIGHT);
		break;
	case ROW_TITLE_SPLIT:
		/* (on to the game's choice once the players are in) */
		menu.split_return = NONE;
		split_open();
		break;
	case ROW_PLAYERS:
		/* (a game page's players: split screen's page, back here after) */
		menu.split_return = menu.screen;
		split_open();
		break;
	case ROW_SPLIT_PLAYERS:
		go(SCREEN_SPLIT);
		break;
	case ROW_TITLE_SETTINGS:
		settings_open();
		go(SCREEN_SETTINGS);
		break;
	case ROW_SET_PLAYER:
		go(SCREEN_SET_PLAYER);
		break;
	case ROW_SET_DISPLAY_PAGE:
		go(SCREEN_SET_DISPLAY);
		break;
	case ROW_SET_GRAPHICS_PAGE:
		go(SCREEN_SET_GRAPHICS);
		break;
	case ROW_SET_SOUND_PAGE:
		go(SCREEN_SET_SOUND);
		break;
	case ROW_SET_CONTROLS:
		go(SCREEN_CONTROLS);
		break;
	case ROW_SET_CONTROLLER:
		go(SCREEN_CONTROLLER);
		break;
	case ROW_SET_NAME:
		edit_begin();
		break;
	case ROW_SET_APPLY:
		apply_and_restart();
		break;
	case ROW_BIND_RESET:
		dos_bindings_reset();
		menu.settings_changed = TRUE;
		snprintf(menu.message, sizeof(menu.message), "THE KEYS ARE BACK TO THEIR DEFAULTS");
		break;
	case ROW_TITLE_NETWORK:
		/* (its games come back to its lobby, not to a page of the menu's) */
		menu.game_screen = SCREEN_TITLE;
		go(SCREEN_NETWORK);
		break;
	case ROW_NET_HOST:
		if (dos_game_types[dos_choice.game_type].bot_match)
			dos_choice.game_type = 0;
		if (!dos_map_present(dos_multiplayer_maps[dos_choice.map].folder))
			dos_choice.map = step_map(dos_multiplayer_maps, dos_multiplayer_map_count, dos_choice.map, 1);
		go(SCREEN_NET_HOST);
		break;
	case ROW_NET_JOIN:
		menu.game_count = 0;
		if (dos_network_search())
			go(SCREEN_NET_JOIN);
		else
			snprintf(menu.message, sizeof(menu.message), "NO NETWORK");
		break;
	case ROW_NET_QUICK:
		/* (from ONLINE: a search of its own, the best game a few seconds
		later, the list shown meanwhile; from the list: the best of it now) */
		if (menu.screen == SCREEN_NETWORK)
		{
			menu.game_count = 0;
			if (dos_network_quick_join(TRUE))
				go(SCREEN_NET_JOIN);
			else
				snprintf(menu.message, sizeof(menu.message), "NO NETWORK");
		}
		else if (!dos_network_quick_join(FALSE))
			snprintf(menu.message, sizeof(menu.message), "NO GAME TO JOIN: THE FILTERS MAY HIDE SOME");
		break;
	case ROW_NET_CREATE:
		if (!dos_map_present(dos_multiplayer_maps[dos_choice.map].folder))
		{
			snprintf(menu.message, sizeof(menu.message), "THAT MAP IS NOT IN THE MAPS FOLDER");
			break;
		}
		if (dos_network_host())
			go(SCREEN_LOBBY);
		else
			snprintf(menu.message, sizeof(menu.message), "NO NETWORK");
		break;
	case ROW_NET_START:
		dos_network_start();
		snprintf(menu.message, sizeof(menu.message), "STARTING");
		break;
	case ROW_NET_LEAVE:
		if (menu.paused)
		{
			pause_end();
			dos_network_leave();
			main_goto_main_menu();
			break;
		}
		dos_network_leave();
		go_to_row(SCREEN_NETWORK, ROW_NET_JOIN);
		break;
	case ROW_TITLE_QUIT:
	case ROW_PAUSE_QUIT:
		quit_halo();
		break;
	case ROW_START:
		start_game();
		break;
	case ROW_BACK:
		/* (the pause's settings: back to the pause) */
		if (menu.paused && menu.screen != SCREEN_SETTINGS && menu.screen != SCREEN_SET_PLAYER &&
			menu.screen != SCREEN_SET_DISPLAY && menu.screen != SCREEN_SET_GRAPHICS && menu.screen != SCREEN_SET_SOUND &&
			menu.screen != SCREEN_CONTROLS && menu.screen != SCREEN_CONTROLLER)
		{
			go(SCREEN_PAUSE);
			break;
		}
		go_back();
		break;
	case ROW_PAUSE_RESUME:
		pause_end();
		break;
	case ROW_PAUSE_REVERT:
		pause_end();
		main_revert_map();
		break;
	case ROW_PAUSE_RESTART:
		pause_end();
		dos_game_begin();
		break;
	case ROW_PAUSE_MENU:
		pause_end();
		main_goto_main_menu();
		break;
	}
}

/* ---------- each frame */

static const char *screen_title(void)
{
	switch (menu.screen)
	{
	case SCREEN_TITLE: return "HALO: COMBAT EVOLVED";
	case SCREEN_CAMPAIGN: return "CAMPAIGN";
	case SCREEN_MULTIPLAYER: return "CUSTOM GAME";
	case SCREEN_FIREFIGHT: return "FIREFIGHT";
	case SCREEN_SETTINGS: return "SETTINGS";
	case SCREEN_CONTROLS: return "CONTROLS";
	case SCREEN_CONTROLLER: return "CONTROLLER";
	case SCREEN_SPLIT: return "SPLIT SCREEN";
	case SCREEN_SPLIT_GAME: return "SPLIT SCREEN";
	case SCREEN_RESULTS: return "RESULTS";
	case SCREEN_PAUSE: return "PAUSED";
	case SCREEN_NETWORK: return "ONLINE";
	case SCREEN_NET_HOST: return "HOST A GAME";
	case SCREEN_NET_JOIN: return "BROWSE GAMES";
	case SCREEN_CE_JOIN: return "JOINING";
	case SCREEN_MULTIPLAYER_MENU: return "MULTIPLAYER";
	case SCREEN_SET_PLAYER: return "PLAYER";
	case SCREEN_SET_DISPLAY: return "DISPLAY";
	case SCREEN_SET_GRAPHICS: return "GRAPHICS";
	case SCREEN_SET_SOUND: return "SOUND";
	case SCREEN_LOBBY: return "LOBBY";
	}
	return "";
}

/* what the chosen row does: the line under the rows */
static void row_help(int row, char *help, int size)
{
	const char *line = "";

	if (row >= ROW_BIND && row < ROW_BIND + DOS_ACTION_COUNT)
		line = menu.binding >= 0 ? "PRESS A KEY OR MOUSE BUTTON. ESC: CANCEL, DELETE: CLEAR" :
			"ENTER: A KEY FOR IT (UP TO THREE), DELETE: NONE";
	switch (row)
	{
	case ROW_TITLE_CAMPAIGN: line = "THE STORY, LEVEL BY LEVEL"; break;
	case ROW_SET_CONTROLS: line = "THE KEYS AND MOUSE BUTTONS OF EVERY ACTION"; break;
	case ROW_SET_CONTROLLER:
		line = on_dos() ? "A JOYSTICK ON THE GAME PORT: ITS LAYOUT AND SENSITIVITY" :
			"A GAME CONTROLLER: ITS LAYOUT AND SENSITIVITY";
		break;
	case ROW_BIND_RESET: line = "EVERY ACTION'S KEYS AS THEY WERE AT FIRST"; break;
	case ROW_PAD_DEVICE:
		line = on_dos() ? "THE GAME PORT'S JOYSTICKS (GAME_PORT=4AXIS IN HALODOS.CFG: ONE WITH FOUR AXES)" :
			"THE CONTROLLERS FOUND (XINPUT, OR JOYSTICKS WINDOWS KNOWS)";
		break;
	case ROW_PAD_ENABLED: line = "READ A CONTROLLER AT ALL: FROM THE NEXT START"; break;
	case ROW_PAD_LAYOUT:
	{
		static const char *const lines[] = { "A JUMP, B MELEE, X ACTION, Y WEAPON, TRIGGERS FIRE AND GRENADE",
			"THE TRIGGERS SWAPPED: GRENADE ON THE RIGHT, FIRE ON THE LEFT",
			"JUMP ON THE LEFT TRIGGER, GRENADES ON A", "MELEE ON THE LEFT TRIGGER, GRENADES ON B",
			"MELEE ON THE RIGHT STICK'S CLICK, ZOOM ON B", "JUMP ON THE LEFT BUMPER, MELEE ON THE RIGHT" };

		line = lines[PIN(menu_integer("controller_layout", 0), 0, 5)];
		break;
	}
	case ROW_PAD_STICKS:
	{
		static const char *const lines[] = { "MOVE WITH THE LEFT STICK, LOOK WITH THE RIGHT",
			"MOVE WITH THE RIGHT STICK, LOOK WITH THE LEFT", "HALO'S LEGACY LAYOUT",
			"HALO'S LEGACY LAYOUT, THE STICKS SWAPPED" };

		line = lines[PIN(menu_integer("stick_layout", 0), 0, 3)];
		break;
	}
	case ROW_PAD_SENSITIVITY: line = "HOW FAST THE STICK TURNS YOU: 1 TO 10, 3 IS HALO'S OWN"; break;
	case ROW_PAD_INVERT: line = "THE STICK UP LOOKS DOWN"; break;
	case ROW_PAD_AIRCRAFT: line = "FLIGHT STICK: BANSHEES AND GHOSTS PITCH AS A PLANE DOES"; break;
	case ROW_PAD_VIBRATION: line = "THE CONTROLLER SHAKES (XINPUT CONTROLLERS)"; break;
	case ROW_TITLE_MULTIPLAYER: line = "ONLINE, SPLIT SCREEN, A CUSTOM GAME WITH BOTS, FIREFIGHT"; break;
	case ROW_MP_CUSTOM: line = "A GAME ON THIS COMPUTER: YOUR RULES, BOTS IF YOU LIKE"; break;
	case ROW_TITLE_FIREFIGHT: line = "WAVES OF COVENANT, AS LONG AS YOU LAST"; break;
	case ROW_TITLE_SPLIT: line = "TWO TO FOUR PLAYERS ON THIS SCREEN, THEN ONLINE OR HERE"; break;
	case ROW_SPLIT_PLAYERS: line = "WHO PLAYS, ON WHAT, IN WHICH ARMOUR AND TEAM"; break;
	case ROW_PLAYERS: line = "ONE, OR SPLIT SCREEN: EACH PLAYER JOINS WITH A CONTROLLER OR THE KEYBOARD"; break;
	case ROW_TITLE_NETWORK: line = "QUICK JOIN, THE GAMES ON THE LAN AND THE INTERNET, OR HOST ONE"; break;
	case ROW_TITLE_SETTINGS: line = "YOUR NAME AND ARMOUR, THE DISPLAY, GRAPHICS, SOUND, CONTROLS"; break;
	case ROW_SET_PLAYER: line = "YOUR NAME AND ARMOUR COLOUR"; break;
	case ROW_SET_DISPLAY_PAGE: line = "THE RENDERER, RESOLUTION, COLOURS, SCREEN SHAPE, FRAME RATE"; break;
	case ROW_SET_GRAPHICS_PAGE: line = "DETAIL, DRAW DISTANCE, FIELD OF VIEW, TEXTURES, LIGHT, EFFECTS"; break;
	case ROW_SET_SOUND_PAGE: line = "SOUND ON OR OFF, AND HOW LOUD"; break;
	case ROW_SET_NAME: line = menu.editing ? "TYPE IT. ENTER: KEEP IT, ESC: AS IT WAS. A PAD: UP/DOWN A LETTER, LEFT/RIGHT MOVE" :
		"THE NAME OTHER PLAYERS SEE ONLINE"; break;
	case ROW_SET_APPLY: line = "THE CHANGES MARKED * NEED HALO STARTED AGAIN: SAVED, AND IT STARTS AGAIN NOW"; break;
	case ROW_TITLE_QUIT: line = "THE SETTINGS SAVED, HALO ENDS"; break;
	case ROW_NET_HOST: line = "A GAME OTHERS JOIN: LISTED ONLINE TOO"; break;
	case ROW_NET_JOIN: line = "THE LAN'S GAMES, CHUPATHINGY'S AND OPEN CE'S ONLINE, CUSTOM EDITION SERVERS"; break;
	case ROW_NET_QUICK: line = "INTO THE GAME WITH THE MOST PLAYERS THAT HAS ROOM, OF THOSE THE FILTERS SHOW"; break;
	case ROW_FILTER_FULL: line = "GAMES WITH NO ROOM LEFT"; break;
	case ROW_FILTER_EMPTY: line = "GAMES NOBODY IS PLAYING"; break;
	case ROW_FILTER_JOINABLE: line = "ONLY GAMES THIS HALO CAN JOIN NOW: NO PASSWORD, ROOM, ITS MAP HERE"; break;
	case ROW_LEVEL: line = "WHERE THE STORY STARTS"; break;
	case ROW_DIFFICULTY: line = "HOW HARD THE COVENANT FIGHT"; break;
	case ROW_SCORE: line = "THE SCORE THAT ENDS THE GAME"; break;
	case ROW_WEAPONS: line = "WHAT EVERYONE STARTS WITH"; break;
	case ROW_VEHICLES: line = "THE MAP'S VEHICLES, OR NONE"; break;
	case ROW_SET_RESOLUTION:
		line = on_dos() ? "THE PICTURE'S SIZE: FROM THE NEXT START" :
			"THE PICTURE'S SIZE, SMALLER IS FASTER: FROM THE NEXT START";
		break;
	case ROW_SET_ASPECT:
		line = on_dos() ? "THE SCREEN'S SHAPE: AUTO IS 4:3; A WIDE ONE FOR A MONITOR THAT STRETCHES DOS: FROM THE NEXT START" :
			"THE SCREEN'S SHAPE: AUTO FOLLOWS THE WINDOW; WIDE SHOWS MORE TO THE SIDES: FROM THE NEXT START";
		break;
	case ROW_SET_DOUBLE_WIDE:
		line = "EACH PIXEL TWICE AS WIDE: HALF THE PIXELS TO DRAW, FOR SLOW PCS: FROM THE NEXT START";
		break;
	case ROW_SET_DOUBLE_TALL:
		line = "EACH PIXEL TWICE AS TALL: HALF THE PIXELS TO DRAW, FOR SLOW PCS: FROM THE NEXT START";
		break;
	case ROW_SET_DISPLAY:
	{
		static const char *const lines[] = { "A WINDOW: ALT+ENTER SWITCHES TO THE WHOLE SCREEN",
			"THE WHOLE SCREEN, AT THE DESKTOP'S RESOLUTION",
			"THE SCREEN SET TO A RESOLUTION NEAR THE PICTURE'S: FASTER ON OLD PCS" };

		line = lines[PIN(menu_integer("fullscreen", 0), 0, 2)];
		break;
	}
	case ROW_SET_RENDERER:
		line = on_dos() ? "A 3DFX VOODOO GRAPHICS CARD DRAWS IT ALL: FROM THE NEXT START" :
			renderer_windows() == _renderer_direct3d ?
			"DIRECT3D 8: THE GRAPHICS CARD DRAWS (IT NEEDS DIRECTX 8): FROM THE NEXT START" :
			renderer_windows() == _renderer_opengl ?
			"OPENGL: THE GRAPHICS CARD DRAWS (IT NEEDS ITS DRIVER): FROM THE NEXT START" :
			"THE PROCESSOR DRAWS: FROM THE NEXT START";
		break;
	case ROW_SET_SCALING:
	{
		static const char *const lines[] = { "WHOLE MULTIPLES OF THE PICTURE: CRISP, WITH BORDERS",
			"AS LARGE AS FITS, ITS SHAPE KEPT", "ALL OF THE WINDOW OR SCREEN, ITS SHAPE STRETCHED" };

		line = lines[PIN(menu_integer("scaling", 0), 0, 2)];
		break;
	}
	case ROW_SET_COLOURS: line = "256 COLOURS ARE THE FASTEST: FROM THE NEXT START"; break;
	case ROW_SET_DRAW_DISTANCE: line = "HOW FAR AWAY THE WORLD IS DRAWN"; break;
	case ROW_SET_DETAIL: line = "HOW FAR OBJECTS KEEP THEIR DETAIL"; break;
	case ROW_SET_FIELD_OF_VIEW: line = "HOW WIDE YOU SEE IN FIRST PERSON: 70 IS HALO'S OWN"; break;
	case ROW_SET_TEXTURES:
		line = colour_choices[menu.colours].bits == 8 && !renderer_hardware() ?
			"IN 256 COLOURS SURFACES ARE ALWAYS SHARP" : "SURFACES UP CLOSE: SHARP, DITHERED OR SMOOTH";
		break;
	case ROW_SET_TEXTURE_QUALITY:
		/* (OpenGL draws Halo's detail maps from HIGH: dos_detail_maps) */
		if (texture_quality() >= 3 && renderer_hardware() && !on_dos())
			line = "WITH HALO'S DETAIL MAPS UP CLOSE: FROM THE NEXT START";
		else if (texture_quality() == (int)NUMBEROF(texture_sizes) - 1)
			line = "HALO'S OWN TEXTURES, THE MOST MEMORY: FROM THE NEXT START";
		else
			line = "HIGHER IS SHARPER AND TAKES MORE MEMORY: FROM THE NEXT START";
		break;
	case ROW_SET_LIGHT: line = "THE LEVEL'S LIGHTMAPS AS ITS COLOURS: FROM THE NEXT MAP"; break;
	case ROW_SET_D3D_LIGHTMAPS:
		line = menu_integer("d3d_lightmaps", 1) ? "HALO'S LIGHTMAPS ON THE SURFACES, AS THE XBOX: FROM THE NEXT MAP" :
			"THE LIGHT AT THE CORNERS, AS OPENGL: FROM THE NEXT MAP";
		break;
	case ROW_SET_D3D_OBJECT_LIGHT:
		line = menu_integer("d3d_object_light", 1) ?
			"OBJECTS LIT BY THE LIGHT AROUND THEM, AS THE XBOX: FROM THE NEXT MAP" :
			"OBJECTS IN THEIR TEXTURES' OWN COLOURS, AS OPENGL: FROM THE NEXT MAP";
		break;
	case ROW_SET_D3D_TEXTURES:
		line = menu_integer("d3d_textures", 1) ?
			"DETAIL MAPS AND SKY LAYERS MIXED ON THE CARD AS THE XBOX DID: FROM THE NEXT MAP" :
			"DETAIL MAPS AND SKY LAYERS BAKED INTO TEXTURES, AS OPENGL: FROM THE NEXT MAP";
		break;
	case ROW_SET_VIEW_MODEL: line = "THE WEAPON IN YOUR HANDS: LOWER IS FASTER"; break;
	case ROW_SET_PARTICLES: line = "SPARKS, DUST AND MUZZLE FLASHES: OFF IS FASTER"; break;
	case ROW_SET_SOUND: line = on_dos() ? "A SOUND BLASTER (THE BLASTER VARIABLE): FROM THE NEXT START" :
		"THE WINDOWS SOUND CARD: FROM THE NEXT START"; break;
	case ROW_SET_VOLUME: line = "HOW LOUD THE GAME PLAYS"; break;
	case ROW_SET_SKY: line = "THE SKY'S MODEL (STARS, PLANETS) OR A GRADIENT"; break;
	case ROW_SET_MOUSE: line = "HOW FAST THE MOUSE TURNS YOU"; break;
	case ROW_SET_INVERT: line = "MOUSE UP LOOKS DOWN"; break;
	case ROW_SET_AIM_ASSIST:
		line = "ON: THE VIEW SLOWS OVER TARGETS AND BULLETS BEND TO THEM, MOUSE OR CONTROLLER; OFF: NEITHER";
		break;
	case ROW_SET_FPS: line = "FRAMES A SECOND, AT THE TOP LEFT"; break;
	case ROW_SET_FRAME_RATE: line = "ABOVE 30, FRAMES ARE DRAWN BETWEEN THE GAME'S 30 TICKS"; break;
	case ROW_SET_NAME_COLOR: line = "YOUR ARMOUR IN FREE-FOR-ALL GAMES"; break;
	case ROW_PAUSE_REVERT: line = "BACK TO THE LAST CHECKPOINT"; break;
	case ROW_NET_LEAVE: line = menu.paused ? "OUT OF THE GAME, BACK TO THE MENU" : "OUT OF THE LOBBY"; break;
	}
	/* (split screen's choice of game) */
	if (menu.screen == SCREEN_SPLIT_GAME)
	{
		switch (row)
		{
		case ROW_TITLE_CAMPAIGN:
			line = dos_split_count() > 2 ? "THE STORY IN CO-OP FOR TWO: PLAYERS 1 AND 2" : "THE STORY IN CO-OP FOR TWO";
			break;
		case ROW_TITLE_NETWORK: line = "ALL OF YOU IN AN ONLINE GAME: QUICK JOIN, BROWSE OR HOST"; break;
		case ROW_MP_CUSTOM: line = "ALL OF YOU IN ONE GAME, AND BOTS IF YOU LIKE"; break;
		case ROW_TITLE_FIREFIGHT: line = "ALL OF YOU AGAINST WAVES OF COVENANT"; break;
		case ROW_BACK: line = "THE PLAYERS AGAIN"; break;
		}
	}
	if ((row == ROW_BOTS || row == ROW_BOT_SKILL || row == ROW_TEAM) && dos_choice.bots)
	{
		/* (the bots' names and teams, as the lobby will have them) */
		extern const char *dos_bot_lobby_name(int index, int bot_count);
		int at;

		snprintf(help, size, "BOTS: ");
		for (int i = 0; i < dos_choice.bots && i < 6; i++)
		{
			at = (int)strlen(help);
			snprintf(help + at, size - at, "%s%s", i ? ", " : "", dos_bot_lobby_name(i, dos_choice.bots));
		}
		if (dos_choice.bots > 6)
		{
			at = (int)strlen(help);
			snprintf(help + at, size - at, " ...");
		}
		return;
	}
	if (row >= ROW_NET_GAME && row < ROW_NET_GAME + menu.game_count)
	{
		const struct dos_network_game *game = &menu.games[row - ROW_NET_GAME];

		int at;

		/* (its name, game type, whose it is, then what is worth knowing
		before joining: a server, its network version, a password, its
		answer time, a Custom Edition map missing) */
		snprintf(help, size, "%s  -  %s%s%s%s%s", game->name, game->game_type, game->game_type[0] ? "  -  " : "",
			game_kind(game, TRUE), game->dedicated && game->kind != DOS_GAME_CUSTOM_EDITION &&
			game->kind != DOS_GAME_RETAIL_PC ? " SERVER" : "", game->in_progress ? ", PLAYING" : "");
		at = (int)strlen(help);
		if (game->version > 0 && at < size)
			at += snprintf(help + at, size - at, ", NETWORK %d", game->version);
		if (game->password && at < size)
			at += snprintf(help + at, size - at, ", PASSWORD");
		if (game->ping_ms >= 0 && at < size)
			at += snprintf(help + at, size - at, ", %d MS", game->ping_ms);
		if (game->kind == DOS_GAME_CUSTOM_EDITION && !game->has_map && at < size)
			snprintf(help + at, size - at, "  -  ITS MAP IS NOT IN MAPS_CE");
		else if (game->kind == DOS_GAME_RETAIL_PC && at < size)
			snprintf(help + at, size - at, "  -  TO LOOK AT, NOT JOIN");
		for (int i = 0; help[i]; i++)
		{
			if (help[i] >= 'a' && help[i] <= 'z')
				help[i] = (char)(help[i] - 'a' + 'A');
		}
		return;
	}
	snprintf(help, size, "%s", line);
}

/* the rows that fit, the chosen one among them: the first one shown */
static int first_shown(int shown)
{
	/* (a row's room kept above and below the chosen one, so that the next
	row is seen before it is reached) */
	int margin = shown >= 5 && !menu.pointer_chose ? 1 : 0;

	if (menu.row_count <= shown)
		menu.first_row = 0;
	else
	{
		if (menu.row < menu.first_row + margin)
			menu.first_row = menu.row - margin;
		if (menu.row > menu.first_row + shown - 1 - margin)
			menu.first_row = menu.row - shown + 1 + margin;
		menu.first_row = PIN(menu.first_row, 0, menu.row_count - shown);
	}
	return menu.first_row;
}

/* a row: a dark bar (Halo's option background), a setting's name on its
left and its value on its right (glowing when chosen), or a button
(centred); the chosen row's frame is drawn by the page, gliding */
static void draw_row(int index, int y)
{
	int row = menu.rows[index], chosen = index == menu.row, stepped;
	char value[80], shown[96];
	const char *label = row_label(row, value, sizeof(value), &stepped);
	float in = page_in();
	int x0 = list_x0 - (int)(24.0f * (1.0f - in)), x1 = list_x1 - (int)(24.0f * (1.0f - in));
	unsigned long color = faded(chosen ? TEXT_BRIGHT : TEXT_COLOR, in);

	dos_art_frame(DOS_ART_ROW, 0, NULL, 6, 5.0f, (float)x0, (float)(y - 2), (float)x1, (float)(y + ROW_H + 2),
		faded(0xD0FFFFFFUL, in));
	if (stepped || value[0])
	{
		int label_width = text_width(font_large, label, text_scale());
		int room = x1 - x0 - 28 - label_width - 16, value_right = x1 - 12;

		text(font_large, x0 + 12, y, x1, y + ROW_H, JUSTIFY_LEFT, color, label);
		/* (a setting waiting for APPLY AND RESTART: marked) */
		snprintf(shown, sizeof(shown), "%s%s", value, row_pending(row) ? " *" : "");
		/* (a long value cut short of the name: the panel says it whole) */
		if (text_width(font_large, shown, text_scale()) > room)
		{
			char whole[96];

			snprintf(whole, sizeof(whole), "%s", shown);
			fit_string(font_large, whole, room, shown, sizeof(shown));
		}
		if (chosen)
			text_glow(font_large, x0, y, value_right, y + ROW_H, JUSTIFY_RIGHT, faded(TEXT_BRIGHT, in),
				faded(GLOW_COLOR, in), shown, text_scale());
		else
			text(font_large, x0, y, value_right, y + ROW_H, JUSTIFY_RIGHT, faded(TEXT_COLOR, in), shown);
	}
	else if (chosen)
	{
		text_glow(font_large, x0, y, x1, y + ROW_H, JUSTIFY_CENTER, color, faded(GLOW_COLOR, in), label, text_scale());
	}
	else
	{
		text(font_large, x0, y, x1, y + ROW_H, JUSTIFY_CENTER, color, label);
	}
}

/* the chosen row's frame (Halo's framed bar), gliding to the row: drawn
over the rows' bars, under their text */
static void draw_highlight(int y, float in)
{
	unsigned long long now = system_microseconds();
	float dt = menu.highlight_time ? (float)(now - menu.highlight_time) / 1000000.0f : 1.0f;
	int x0 = list_x0 - (int)(24.0f * (1.0f - in)), x1 = list_x1 - (int)(24.0f * (1.0f - in));

	menu.highlight_time = now;
	/* (a new page, or a jump of more than a row: there at once; a row up or
	down: glided there in a few hundredths of a second, or at once when the
	frames come slower than that) */
	if (menu.highlight_screen != menu.screen || fabsf(menu.highlight_y - (float)y) > 48.0f)
		menu.highlight_y = (float)y;
	else
		menu.highlight_y += ((float)y - menu.highlight_y) * (dt > 0.04f ? 1.0f : 1.0f - expf(-dt * 40.0f));
	menu.highlight_screen = menu.screen;
	fill(x0 + 3, (int)menu.highlight_y - 1, x1 - 3, (int)menu.highlight_y + ROW_H + 1, faded(CHOSEN_COLOR, in));
	dos_art_frame(DOS_ART_ROW, 1, NULL, 6, 5.0f, (float)(x0 - 2), menu.highlight_y - 4.0f, (float)(x1 + 2),
		menu.highlight_y + (float)ROW_H + 4.0f, faded(0xFFFFFFFFUL, in));
}

/* lines of text in a box, broken at spaces to fit its width; the y after
them */
static int text_wrapped(long font, int x0, int y, int x1, unsigned long color, const char *string, int line_height)
{
	char line[128];
	real scale = text_scale();
	int length = (int)strlen(string), start = 0;

	while (start < length)
	{
		int end = start, last_space = -1;

		while (end < length && end - start < (int)sizeof(line) - 1)
		{
			snprintf(line, sizeof(line), "%.*s", end - start + 1, string + start);
			if (text_width(font, line, scale) > x1 - x0 && last_space > start)
				break;
			if (string[end] == ' ')
				last_space = end;
			end++;
		}
		if (end < length && last_space > start)
			end = last_space;
		snprintf(line, sizeof(line), "%.*s", end - start, string + start);
		text(font, x0, y, x1, y + line_height, JUSTIFY_LEFT, color, line);
		y += line_height;
		start = end;
		while (start < length && string[start] == ' ')
			start++;
	}
	return y;
}

/* a picture of Halo's (a level's, a map's) framed in the panel; the y
under it */
static int panel_picture(enum dos_art art, int index, int x0, int y, int x1, float in)
{
	int height = (x1 - x0) * 114 / 140;

	if (!dos_art_has(art, index))
		return y;
	dos_art_draw(art, index, dos_art_picture_crop, (float)x0, (float)y, (float)x1, (float)(y + height),
		faded(0xFFFFFFFFUL, in));
	frame_box(x0 - 1, y - 1, x1 + 1, y + height + 1, faded(FRAME_COLOR, in));
	return y + height + 8;
}

/* the game types' icons (game_type_grafix): a variant's */
static int game_type_icon(const char *variant)
{
	if (strstr(variant, "ctf"))
		return 0;
	if (strstr(variant, "king"))
		return 1;
	if (strstr(variant, "oddball"))
		return 3;
	if (strstr(variant, "race"))
		return 4;
	return 2;
}

/* an icon and a word beside it, in the panel; the y under them */
static int panel_icon_line(enum dos_art art, int index, int x0, int y, int x1, const char *word, float in)
{
	int size = 54;

	if (dos_art_has(art, index))
		dos_art_draw(art, index, NULL, (float)x0, (float)y, (float)(x0 + size), (float)(y + size), faded(0xFFFFFFFFUL, in));
	text_fitted(font_large, x0 + size + 10, y + (size - ROW_H) / 2, x1, y + (size + ROW_H) / 2, JUSTIFY_LEFT,
		faded(TEXT_BRIGHT, in), word);
	return y + size + 6;
}

/* (split screen's players: draw_split's neighbours, below) */
static void split_swatches(int x, int y, int xr, float in, int teams, int playing);
static void split_roster(int x, int y, int xr, float in);

/* The panel on the right: about what the page chooses (the level's or
the map's picture, the game type, the difficulty) or what its chosen row
does */
static void draw_panel(int x0, int y0, int x1, int y1)
{
	float in = page_in();
	int x = x0 + 14, xr = x1 - 14, y = y0 + 14;
	char line[96], help[160];

	x0 += (int)(24.0f * (1.0f - in));
	dos_art_frame(DOS_ART_SPLIT_PANEL, 0, NULL, 16, 10.0f, (float)x0, (float)y0, (float)x1, (float)y1,
		faded(0xF0FFFFFFUL, in));
	switch (menu.screen)
	{
	case SCREEN_CAMPAIGN:
		y = panel_picture(DOS_ART_LEVELS, dos_choice.level, x, y, xr, in);
		text_fitted(font_large, x, y, xr, y + ROW_H, JUSTIFY_CENTER, faded(TEXT_BRIGHT, in),
			dos_campaign_levels[dos_choice.level].name);
		y += ROW_H + 8;
		panel_icon_line(DOS_ART_DIFFICULTIES, dos_choice.difficulty, x, y, xr,
			dos_difficulty_names[dos_choice.difficulty], in);
		split_swatches(x, y1 - 32, xr, in, FALSE, 2);
		return;
	case SCREEN_MULTIPLAYER:
	case SCREEN_FIREFIGHT:
	{
		const struct dos_game_type *type = &dos_game_types[dos_choice.game_type];

		y = panel_picture(DOS_ART_MAPS, dos_map_picture(dos_choice.map), x, y, xr, in);
		text_fitted(font_large, x, y, xr, y + ROW_H, JUSTIFY_CENTER, faded(TEXT_BRIGHT, in),
			dos_multiplayer_maps[dos_choice.map].name);
		y += ROW_H + 8;
		if (menu.screen == SCREEN_FIREFIGHT)
		{
			panel_icon_line(DOS_ART_DIFFICULTIES, dos_choice.difficulty, x, y, xr,
				dos_difficulty_names[dos_choice.difficulty], in);
			split_swatches(x, y1 - 32, xr, in, FALSE, DOS_SPLIT_PLAYERS);
			return;
		}
		y = panel_icon_line(DOS_ART_GAME_TYPES, game_type_icon(type->variant), x, y, xr, type->name, in);
		if (dos_choice.bots > 0)
		{
			snprintf(line, sizeof(line), "%d %s, %s", dos_choice.bots, dos_choice.bots == 1 ? "BOT" : "BOTS",
				dos_difficulty_names[dos_choice.bot_skill]);
			text_fitted(font_help(), x, y, xr, y + 20, JUSTIFY_LEFT, faded(TEXT_COLOR, in), line);
		}
		split_swatches(x, y1 - 30, xr, in, type->teams, DOS_SPLIT_PLAYERS);
		return;
	}
	default:
		break;
	}
	/* (any other page: the chosen row's name, then what it does) */
	{
		int stepped;
		char value[80];
		const char *label = row_label(menu.rows[menu.row], value, sizeof(value), &stepped);

		text_fitted(font_large, x, y, xr, y + ROW_H, JUSTIFY_LEFT, faded(TEXT_BRIGHT, in), label);
		y += ROW_H + 2;
		if (value[0])
		{
			text_fitted(font_large, x, y, xr, y + ROW_H, JUSTIFY_LEFT, faded(0xFF6CD8FFUL, in), value);
			y += ROW_H + 2;
		}
		fill(x, y + 2, xr, y + 3, faded(RULE_COLOR, in * 0.6f));
		y += 10;
		row_help(menu.rows[menu.row], help, sizeof(help));
		text_wrapped(font_help(), x, y, xr, faded(TEXT_COLOR, in), help, 20);
		/* (split screen's game: who plays it) */
		if (menu.screen == SCREEN_SPLIT_GAME)
			split_roster(x, y1 - 14 - DOS_SPLIT_PLAYERS * 40, xr, in);
	}
}

/* ---------- the game list (BROWSE GAMES)

Over the list, the kinds of games it shows as tabs (LEFT and RIGHT step
them); under them the columns' heads, the one it is sorted by marked (TAB
steps it); then a game a line: its name, map, players, answer time and
whose it is. F opens the FILTERS box over it. */

#define GAME_X0 (SAFE_LEFT + 8)
#define GAME_X1 (SAFE_RIGHT - 8)
/* the tabs' line and the heads' under it: the games start below */
#define TABS_Y (LIST_TOP - 10)
#define HEADS_Y (LIST_TOP + 14)
#define GAMES_TOP (LIST_TOP + 36)

static const char *const show_tabs[NUMBER_OF_DOS_SHOWS] = { "ALL", "ONLINE", "CHUPATHINGY", "OPEN CE", "CE SERVERS",
	"LAN" };

static struct
{
	int name_x, map_x, players_x0, players_x1, ping_x0, ping_x1, where_x0, where_x1;
} columns;

static void game_columns(void)
{
	real scale = text_scale();

	columns.where_x1 = GAME_X1 - 10;
	columns.where_x0 = columns.where_x1 - text_width(font_help(), "OPENCE", scale);
	columns.ping_x1 = columns.where_x0 - 14;
	columns.ping_x0 = columns.ping_x1 - text_width(font_help(), "PING ^", scale);
	columns.players_x1 = columns.ping_x0 - 14;
	columns.players_x0 = columns.players_x1 - text_width(font_large, "16/16", scale);
	columns.name_x = GAME_X0 + 12;
	columns.map_x = columns.name_x + (columns.players_x0 - 16 - columns.name_x) * 11 / 20;
}

static void draw_game_row(int index, int y)
{
	const struct dos_network_game *game = &menu.games[menu.rows[index] - ROW_NET_GAME];
	int chosen = index == menu.row;
	unsigned long color = !game->compatible || !game->joinable ? TEXT_DIM : chosen ? TEXT_BRIGHT : TEXT_COLOR;
	char cut[64], players[16], ping[16];

	dos_art_frame(DOS_ART_ROW, 0, NULL, 6, 5.0f, (float)GAME_X0, (float)(y - 2), (float)GAME_X1, (float)(y + ROW_H + 2),
		0x90FFFFFFUL);
	fit_string(font_large, game->name, columns.map_x - columns.name_x - 12, cut, sizeof(cut));
	if (chosen)
		text_glow(font_large, columns.name_x, y, columns.map_x, y + ROW_H, JUSTIFY_LEFT, TEXT_BRIGHT, GLOW_COLOR, cut,
			text_scale());
	else
		text(font_large, columns.name_x, y, columns.map_x, y + ROW_H, JUSTIFY_LEFT, color, cut);
	fit_string(font_large, game->map, columns.players_x0 - 16 - columns.map_x, cut, sizeof(cut));
	text(font_large, columns.map_x, y, columns.players_x0, y + ROW_H, JUSTIFY_LEFT, color, cut);
	snprintf(players, sizeof(players), "%d/%d", game->players, game->maximum_players);
	text(font_large, columns.players_x0, y, columns.players_x1, y + ROW_H, JUSTIFY_RIGHT,
		game->maximum_players > 0 && game->players >= game->maximum_players ? TEXT_DIM : color, players);
	if (game->ping_ms >= 0)
		snprintf(ping, sizeof(ping), "%d", game->ping_ms > 999 ? 999 : game->ping_ms);
	else
		snprintf(ping, sizeof(ping), "-");
	text(font_help(), columns.ping_x0, y + 2, columns.ping_x1, y + ROW_H, JUSTIFY_RIGHT, TEXT_DIM, ping);
	text(font_help(), columns.where_x0, y + 2, columns.where_x1, y + ROW_H, JUSTIFY_RIGHT, TEXT_DIM,
		game_kind(game, FALSE));
}

/* the tabs and the columns' heads over the games */
static void draw_browser_heads(float in)
{
	int show = (int)PIN(menu_integer("browser_show", DOS_SHOW_ALL), 0, NUMBER_OF_DOS_SHOWS - 1);
	int sort = (int)PIN(menu_integer("browser_sort", DOS_SORT_PLAYERS), 0, NUMBER_OF_DOS_SORTS - 1);
	long font = font_help();
	real scale = text_scale();
	int total = 0, x;

	/* (the tabs, centred: the one shown in a box, glowing) */
	for (int i = 0; i < NUMBER_OF_DOS_SHOWS; i++)
		total += text_width(font, show_tabs[i], scale) + 24;
	x = (SCREEN_W - total) / 2;
	for (int i = 0; i < NUMBER_OF_DOS_SHOWS; i++)
	{
		int width = text_width(font, show_tabs[i], scale) + 24;

		add_hit(x, TABS_Y - 4, x + width, TABS_Y + 22, HIT_TAB, i);
		if (i == show)
		{
			chosen_box(x + 2, TABS_Y - 2, x + width - 2, TABS_Y + 20);
			text_glow(font, x, TABS_Y, x + width, TABS_Y + 20, JUSTIFY_CENTER, faded(TEXT_BRIGHT, in), faded(GLOW_COLOR, in),
				show_tabs[i], scale);
		}
		else
			text_at(font, x, TABS_Y, x + width, TABS_Y + 20, JUSTIFY_CENTER, faded(TEXT_DIM, in), show_tabs[i], scale);
		x += width;
	}
	/* (the heads: the order's marked with ^; a click sorts by one) */
	game_columns();
	add_hit(columns.name_x - 8, HEADS_Y - 2, columns.map_x - 4, HEADS_Y + 20, HIT_SORT, DOS_SORT_NAME);
	add_hit(columns.map_x - 4, HEADS_Y - 2, columns.players_x0 - 44, HEADS_Y + 20, HIT_SORT, DOS_SORT_MAP);
	add_hit(columns.players_x0 - 44, HEADS_Y - 2, columns.players_x1 + 4, HEADS_Y + 20, HIT_SORT, DOS_SORT_PLAYERS);
	add_hit(columns.ping_x0 - 8, HEADS_Y - 2, columns.ping_x1 + 4, HEADS_Y + 20, HIT_SORT, DOS_SORT_PING);
	text(font, columns.name_x, HEADS_Y, columns.map_x, HEADS_Y + 18, JUSTIFY_LEFT,
		sort == DOS_SORT_NAME ? TEXT_BRIGHT : TEXT_DIM, sort == DOS_SORT_NAME ? "GAME ^" : "GAME");
	text(font, columns.map_x, HEADS_Y, columns.players_x0, HEADS_Y + 18, JUSTIFY_LEFT,
		sort == DOS_SORT_MAP ? TEXT_BRIGHT : TEXT_DIM, sort == DOS_SORT_MAP ? "MAP ^" : "MAP");
	text(font, columns.players_x0 - 40, HEADS_Y, columns.players_x1, HEADS_Y + 18, JUSTIFY_RIGHT,
		sort == DOS_SORT_PLAYERS ? TEXT_BRIGHT : TEXT_DIM, sort == DOS_SORT_PLAYERS ? "PLAYERS ^" : "PLAYERS");
	text(font, columns.ping_x0, HEADS_Y, columns.ping_x1, HEADS_Y + 18, JUSTIFY_RIGHT,
		sort == DOS_SORT_PING ? TEXT_BRIGHT : TEXT_DIM, sort == DOS_SORT_PING ? "PING ^" : "PING");
	fill(GAME_X0, HEADS_Y + 19, GAME_X1, HEADS_Y + 19 + LINE, faded(RULE_COLOR, in * 0.5f));
}

/* the FILTERS box over the list */
static void draw_filter_box(void)
{
	static const int rows[3] = { ROW_FILTER_FULL, ROW_FILTER_EMPTY, ROW_FILTER_JOINABLE };
	int x0 = SCREEN_W / 2 - 170, x1 = SCREEN_W / 2 + 170, y0 = 130, y = y0 + 40;

	dos_art_frame(DOS_ART_ALERT, 0, NULL, 16, 10.0f, (float)(x0 - 12), (float)y0, (float)(x1 + 12),
		(float)(y0 + 50 + 4 * 32 + 10), 0xF8FFFFFFUL);
	add_hit(x0 - 12, y0, x1 + 12, y0 + 50 + 4 * 32 + 10, HIT_FILTER, -1);
	text_glow(font_large, x0, y0 + 10, x1, y0 + 34, JUSTIFY_CENTER, TEXT_BRIGHT, GLOW_COLOR, "FILTERS", text_scale());
	for (int i = 0; i < 4; i++, y += 32)
	{
		int chosen = i == menu.filter_row;

		add_hit(x0, y - 3, x1, y + ROW_H + 3, HIT_FILTER, i);
		if (chosen)
			chosen_box(x0, y - 3, x1, y + ROW_H + 3);
		if (i < 3)
		{
			char value[32];
			int stepped;
			const char *label = row_label(rows[i], value, sizeof(value), &stepped);

			text(font_large, x0 + 12, y, x1, y + ROW_H, JUSTIFY_LEFT, chosen ? TEXT_BRIGHT : TEXT_COLOR, label);
			text(font_large, x0, y, x1 - 12, y + ROW_H, JUSTIFY_RIGHT, chosen ? pulsing() : TEXT_COLOR, value);
		}
		else
			text(font_large, x0, y, x1, y + ROW_H, JUSTIFY_CENTER, chosen ? pulsing() : TEXT_COLOR, "DONE");
	}
}

/* the lobby: the game, and its players by team (under the page's rows) */
static void draw_lobby(int y)
{
	struct dos_network_lobby lobby;
	char line[96];

	if (!dos_network_lobby(&lobby))
	{
		text(font_large, LIST_X0, y, LIST_X1, y + ROW_H, JUSTIFY_CENTER, TEXT_COLOR, "CONNECTING...");
		return;
	}
	snprintf(line, sizeof(line), "%s  -  %s", lobby.map, lobby.game_type);
	text_fitted(font_large, LIST_X0, y, LIST_X1, y + ROW_H, JUSTIFY_CENTER, TEXT_BRIGHT, line);
	y += 30;
	fill(LIST_X0 + 40, y - 4, LIST_X1 - 40, y - 3, RULE_COLOR);
	for (int i = 0; i < lobby.player_count; i++)
	{
		int column = i & 1;
		int x0 = column ? (LIST_X0 + LIST_X1) / 2 + 8 : LIST_X0 + 16;
		int yy = y + (i / 2) * 24;
		unsigned long color = !lobby.team_game ? TEXT_COLOR : lobby.teams[i] == 1 ? 0xFF80A8FFUL : 0xFFFF8C80UL;
		char cut[48];

		fit_string(font_large, lobby.players[i], (LIST_X1 - LIST_X0) / 2 - 32, cut, sizeof(cut));
		text(font_large, x0, yy, x0 + (LIST_X1 - LIST_X0) / 2 - 24, yy + 22, JUSTIFY_LEFT, color, cut);
	}
	y += ((lobby.player_count + 1) / 2) * 24 + 6;
	if (lobby.seconds_to_start > 0)
	{
		snprintf(line, sizeof(line), "STARTING IN %d", lobby.seconds_to_start);
		text(font_large, LIST_X0, y, LIST_X1, y + ROW_H, JUSTIFY_CENTER, pulsing(), line);
	}
	else if (lobby.invite_file[0])
	{
		/* (where the internet invite is, for players the list does not
		show the game to) */
		snprintf(line, sizeof(line), "INTERNET INVITE LINK: %s", lobby.invite_file);
		for (int i = 0; line[i]; i++)
		{
			if (line[i] >= 'a' && line[i] <= 'z')
				line[i] = (char)(line[i] - 'a' + 'A');
		}
		text_fitted(font_help(), LIST_X0, y, LIST_X1, y + 22, JUSTIFY_CENTER, TEXT_DIM, line);
	}
}

/* The title: the scene clear (a shade only under the menu), Halo's logo,
and its items glowing as Halo's do, the chosen one bright */
static void draw_title(void)
{
	static const int hints[] = { HINT_CHOOSE, HINT_SELECT };
	int dos = on_dos();
	float in = page_in();
	real scale = text_scale() * 1.2f;

	/* (the shade: deeper toward the bottom) */
	for (int i = 0; i < 10; i++)
		fill_across(200 + i * 28, 228 + i * 28, (unsigned long)(0x10 + i * 0x0E) << 24 | 0x000410UL);
	/* (the logo's letters are the middle third of its picture) */
	if (!draw_logo(SCREEN_W / 2, 4, 560))
		text_at(font_large, 0, 100, SCREEN_W, 140, JUSTIFY_CENTER, TEXT_BRIGHT, "HALO", 2.5f);
	text(font_help(), 0, 178, SCREEN_W, 200, JUSTIFY_CENTER, TEXT_DIM, dos ? "FOR MS-DOS" : "FOR WINDOWS 95");
	for (int i = 0; i < menu.row_count; i++)
	{
		int y = 224 + i * 36, chosen = i == menu.row, stepped;
		char value[8];
		const char *label = row_label(menu.rows[i], value, sizeof(value), &stepped);

		add_hit(SCREEN_W / 2 - 150, y - 4, SCREEN_W / 2 + 150, y + ROW_H + 8, HIT_ROW, i);
		if (chosen)
		{
			text_glow(font_large, 0, y, SCREEN_W, y + ROW_H + 4, JUSTIFY_CENTER, TEXT_BRIGHT, faded(GLOW_COLOR, 1.4f),
				label, scale);
		}
		else
		{
			text_at(font_large, 0, y, SCREEN_W, y + ROW_H + 4, JUSTIFY_CENTER, faded(0xFF5A8CDCUL, in), label, scale);
		}
	}
	footer(hints, NUMBEROF(hints));
}

static void draw_results(void)
{
	static const int hints[] = { HINT_CONTINUE };
	float in = page_in();
	int y = LIST_TOP + 14;

	page("RESULTS", NULL);
	dos_art_frame(DOS_ART_ALERT, 0, NULL, 16, 10.0f, (float)(LIST_X0 - 40), (float)LIST_TOP, (float)(LIST_X1 + 40),
		(float)LIST_BOTTOM, faded(0xF0FFFFFFUL, in));
	for (int i = 0; i < menu.result_count && y < LIST_BOTTOM - 70; i++, y += 26)
	{
		text_fitted(i ? font_help() : font_large, LIST_X0 - 20, y, LIST_X1 + 20, y + 24, JUSTIFY_CENTER,
			faded(i ? TEXT_COLOR : TEXT_BRIGHT, in), menu.results[i]);
	}
	menu.row = 0;
	list_x0 = SCREEN_W / 2 - 150;
	list_x1 = SCREEN_W / 2 + 150;
	y = LIST_BOTTOM - 46;
	dos_art_frame(DOS_ART_ROW, 1, NULL, 6, 5.0f, (float)list_x0, (float)(y - 4), (float)list_x1, (float)(y + ROW_H + 4),
		faded(0xFFFFFFFFUL, in));
	text_glow(font_large, list_x0, y, list_x1, y + ROW_H, JUSTIFY_CENTER, TEXT_BRIGHT, faded(GLOW_COLOR, in), "CONTINUE",
		text_scale());
	footer(hints, NUMBEROF(hints));
}

/* ---------- split screen

The SPLIT SCREEN page (dos_split.h): four places, a player each. Every
device joins by itself, a controller with A (or START), the keyboard with
ENTER, and is then its player's: its LEFT and RIGHT change the player's
armour, UP and DOWN their team, B or ESCAPE leave; with two or more in,
START or ENTER goes on to the game's choice (SCREEN_SPLIT_GAME: the
campaign in co-op, multiplayer or Firefight). ESCAPE on a keyboard that has
not joined, or B on a controller when no one has, goes back to the title.
The page reads the controllers itself (dos_menu_reads_pads: xdk_input.c
then leaves them out of the menu's keys). */

static void menu_sound(const char *name);

static struct
{
	/* each controller's buttons as last read (its stick's directions as
	the D-pad's): a press is a button that was up. A controller first seen
	on the page has its buttons held already (the A that chose the page) */
	struct
	{
		unsigned short id, buttons;
	} pads[2 * SYSTEM_PADS];
	int pad_count;
	/* the page these are of (go() starts a page) */
	unsigned long long page_started;
} split_page;

static unsigned short split_pad_pressed(const struct system_pad *pad)
{
	unsigned short buttons = pad->buttons, pressed;
	int i = 0;

	if (pad->left_y > 16000) buttons |= SYSTEM_PAD_UP;
	if (pad->left_y < -16000) buttons |= SYSTEM_PAD_DOWN;
	if (pad->left_x < -16000) buttons |= SYSTEM_PAD_LEFT;
	if (pad->left_x > 16000) buttons |= SYSTEM_PAD_RIGHT;
	while (i < split_page.pad_count && split_page.pads[i].id != pad->id)
		i++;
	if (i == split_page.pad_count)
	{
		if (i < (int)NUMBEROF(split_page.pads))
		{
			split_page.pads[i].id = pad->id;
			split_page.pads[i].buttons = buttons;
			split_page.pad_count++;
		}
		return 0;
	}
	pressed = (unsigned short)(buttons & ~split_page.pads[i].buttons);
	split_page.pads[i].buttons = buttons;
	return pressed;
}

/* what a player plays with */
static const char *split_device_name(const struct dos_split_player *player)
{
	if (player->device == DOS_DEVICE_KEYBOARD)
		return "KEYBOARD + MOUSE";
	/* (testing's controllers that are not there: split_open) */
	if (player->pad >= 0x8000)
		return "TEST CONTROLLER";
	return system_pad_name(player->pad);
}

/* an armour no other player wears: Halo's red, blue, green, yellow... */
static int split_free_color(int player)
{
	static const int order[] = { 2, 3, 6, 5, 11, 9, 7, 8, 0, 12 };

	for (int i = 0; i < (int)NUMBEROF(order); i++)
	{
		int used = FALSE;

		for (int other = 0; other < dos_split_count(); other++)
		{
			if (other != player && dos_split_player(other)->color == order[i])
				used = TRUE;
		}
		if (!used)
			return order[i];
	}
	return 2;
}

static void split_join(int device, unsigned short pad)
{
	int player = dos_split_join(device, pad);

	if (player < 0)
		return;
	/* (the first player in the settings' armour, the others in one no one
	wears; teams by turns) */
	dos_split_set(player, player == 0 ? (int)PIN(menu_integer("player_color", 2), 0, 17) :
		split_free_color(player), player & 1);
	menu.message[0] = 0;
}

/* a player's armour (LEFT, RIGHT: Halo's eighteen) or team (UP, DOWN) */
static void split_change(int player, int color_delta, int team_change)
{
	const struct dos_split_player *joined = dos_split_player(player);

	if (!joined)
		return;
	dos_split_set(player, step(joined->color, color_delta, NUMBEROF(armour_names)),
		team_change ? !joined->team : joined->team);
	menu_sound("sound\\sfx\\ui\\cursor");
}

/* on to the game's choice, two or more in (the buttons that went on held
until let go: the next page's keys) */
static void split_play(void)
{
	/* (opened from a game page's PLAYERS: back to it with them, one player
	being no split screen) */
	if (menu.split_return != NONE)
	{
		if (dos_split_count() < 2)
			dos_split_clear();
		menu_sound("sound\\sfx\\ui\\forward");
		go_to_row(menu.split_return, ROW_PLAYERS);
		menu.holding = TRUE;
		return;
	}
	if (dos_split_count() < 2)
	{
		snprintf(menu.message, sizeof(menu.message), "ONE MORE PLAYER FIRST: A OR ENTER ON ANOTHER DEVICE");
		return;
	}
	menu_sound("sound\\sfx\\ui\\forward");
	go(SCREEN_SPLIT_GAME);
	menu.holding = TRUE;
}

static void split_back(void)
{
	menu_sound("sound\\sfx\\ui\\back");
	if (menu.split_return != NONE)
	{
		if (dos_split_count() < 2)
			dos_split_clear();
		go_to_row(menu.split_return, ROW_PLAYERS);
	}
	else
	{
		dos_split_clear();
		go_to_row(SCREEN_MULTIPLAYER_MENU, ROW_TITLE_SPLIT);
	}
	menu.holding = TRUE;
}

/* the title's SPLIT SCREEN: the page, the device that chose it in as the
first player */
static void split_open(void)
{
	const struct system_input *input = dos_input();
	int pad = -1;

	go(SCREEN_SPLIT);
	if (pad_used_last())
	{
		for (int i = 0; i < input->pad_count && pad < 0; i++)
		{
			if (input->pads[i].buttons & (SYSTEM_PAD_A | SYSTEM_PAD_START))
				pad = i;
		}
	}
	if (!dos_split_count())
		split_join(pad >= 0 ? DOS_DEVICE_PAD : DOS_DEVICE_KEYBOARD, pad >= 0 ? input->pads[pad].id : 0);
	/* (testing, setting test_split_pads=n: n controllers that are not
	there joined too) */
	for (long i = 0; i < menu_integer("test_split_pads", 0) && i < DOS_SPLIT_PLAYERS; i++)
		split_join(DOS_DEVICE_PAD, (unsigned short)(0x8000 + i));
}

static void split_input(void)
{
	const struct system_input *input = dos_input();
	int keyboard = dos_split_find(DOS_DEVICE_KEYBOARD, 0);

	if (split_page.page_started != menu.page_started)
	{
		split_page.page_started = menu.page_started;
		split_page.pad_count = 0;
	}
	/* the keyboard */
	if (choose_pressed())
	{
		if (keyboard < 0)
		{
			split_join(DOS_DEVICE_KEYBOARD, 0);
			menu_sound("sound\\sfx\\ui\\forward");
		}
		else
			split_play();
	}
	else if (back_pressed())
	{
		if (keyboard < 0)
		{
			split_back();
			return;
		}
		dos_split_leave(keyboard);
		menu_sound("sound\\sfx\\ui\\back");
	}
	else if (keyboard >= 0)
	{
		int left = direction(2), right = direction(3), up = direction(0), down = direction(1);

		if (left || right || up || down)
			split_change(keyboard, right - left, up || down);
	}
	if (menu.screen != SCREEN_SPLIT)
		return;
	/* each controller */
	for (int i = 0; i < input->pad_count; i++)
	{
		const struct system_pad *pad = &input->pads[i];
		unsigned short pressed = split_pad_pressed(pad);
		int player = dos_split_find(DOS_DEVICE_PAD, pad->id);

		if (pressed & (SYSTEM_PAD_A | SYSTEM_PAD_START))
		{
			if (player < 0)
			{
				split_join(DOS_DEVICE_PAD, pad->id);
				menu_sound("sound\\sfx\\ui\\forward");
			}
			else
				split_play();
		}
		else if (pressed & SYSTEM_PAD_B)
		{
			if (player >= 0)
			{
				dos_split_leave(player);
				menu_sound("sound\\sfx\\ui\\back");
			}
			else if (!dos_split_count())
				split_back();
		}
		else if (player >= 0 && (pressed & (SYSTEM_PAD_LEFT | SYSTEM_PAD_RIGHT | SYSTEM_PAD_UP | SYSTEM_PAD_DOWN)))
		{
			split_change(player, ((pressed & SYSTEM_PAD_RIGHT) ? 1 : 0) - ((pressed & SYSTEM_PAD_LEFT) ? 1 : 0),
				(pressed & (SYSTEM_PAD_UP | SYSTEM_PAD_DOWN)) != 0);
		}
		if (menu.screen != SCREEN_SPLIT)
			return;
	}
}

/* a keyboard and its mouse, drawn (Halo has no picture of them) in a box */
static void keyboard_icon(int x0, int y0, int x1, int y1, unsigned long argb)
{
	int kx1 = x0 + (x1 - x0) * 74 / 100, mx0 = x0 + (x1 - x0) * 82 / 100;
	int key_w = (kx1 - x0 - 6) / 7, key_h = (y1 - y0 - 6) / 4;
	unsigned long keys = faded(argb, 0.75f);

	frame_box(x0, y0, kx1, y1, argb);
	for (int row = 0; row < 3; row++)
	{
		for (int key = 0; key < 7; key++)
		{
			int x = x0 + 4 + key * key_w, y = y0 + 4 + row * key_h;

			fill(x, y, x + key_w - 2, y + key_h - 2, keys);
		}
	}
	/* (the space bar) */
	fill(x0 + (kx1 - x0) / 4, y0 + 4 + 3 * key_h, kx1 - (kx1 - x0) / 4, y0 + 2 + 4 * key_h, keys);
	/* (the mouse: its buttons over its body) */
	frame_box(mx0, y0 + (y1 - y0) / 4, x1, y1, argb);
	fill(mx0, y0 + (y1 - y0) / 2, x1, y0 + (y1 - y0) / 2 + 2, argb);
	fill((mx0 + x1) / 2 - 1, y0 + (y1 - y0) / 4, (mx0 + x1) / 2 + 1, y0 + (y1 - y0) / 2, argb);
}

/* a player's colour in a game: the team's in a team game, else their
armour's */
static unsigned long split_color(const struct dos_split_player *player, int teams)
{
	if (teams)
		return player->team ? BLUE_TEAM_ARGB : RED_TEAM_ARGB;
	return armour_argb[PIN(player->color, 0, (int)NUMBEROF(armour_argb) - 1)];
}

/* split screen's players in a line of numbered squares at the panel's
foot (those past playing, the campaign's co-op being for two, faint) */
static void split_swatches(int x, int y, int xr, float in, int teams, int playing)
{
	int count = dos_split_count();
	char number[4];

	if (!count)
		return;
	for (int i = 0; i < count; i++)
	{
		const struct dos_split_player *player = dos_split_player(i);
		float shown = i < playing ? in : in * 0.35f;
		int x0 = x + i * 30;

		fill(x0, y, x0 + 24, y + 22, faded(split_color(player, teams), shown));
		frame_box(x0 - 1, y - 1, x0 + 25, y + 23, faded(FRAME_COLOR, shown));
		snprintf(number, sizeof(number), "%d", i + 1);
		text(font_large, x0, y - 1, x0 + 24, y + 21, JUSTIFY_CENTER, faded(TEXT_BRIGHT, shown), number);
	}
	text_fitted(font_help(), x + count * 30 + 4, y + 1, xr, y + 21, JUSTIFY_LEFT, faded(TEXT_DIM, in),
		count > playing ? "CO-OP: 1 AND 2" : "SPLIT SCREEN");
}

/* split screen's players in the panel: each one's armour, number and
device */
static void split_roster(int x, int y, int xr, float in)
{
	char line[16];

	text(font_help(), x, y, xr, y + 18, JUSTIFY_LEFT, faded(TEXT_DIM, in), "PLAYERS");
	y += 20;
	for (int i = 0; i < dos_split_count(); i++, y += 40)
	{
		const struct dos_split_player *player = dos_split_player(i);

		fill(x, y + 4, x + 18, y + 22, faded(split_color(player, FALSE), in));
		frame_box(x - 1, y + 3, x + 19, y + 23, faded(FRAME_COLOR, in));
		snprintf(line, sizeof(line), "PLAYER %d", i + 1);
		text(font_large, x + 26, y, xr, y + ROW_H, JUSTIFY_LEFT, faded(TEXT_BRIGHT, in), line);
		text_fitted(font_help(), x + 26, y + 20, xr, y + 38, JUSTIFY_LEFT, faded(TEXT_COLOR, in),
			split_device_name(player));
	}
}

/* the page: the four places, then what to do next, the devices found */
static void draw_split(void)
{
	static const int hints[] = { HINT_JOIN, HINT_PLAY, HINT_LEAVE };
	const struct system_input *input = dos_input();
	float in = page_in();
	int count = dos_split_count(), width = (SAFE_RIGHT - SAFE_LEFT - 8 - 3 * 10) / 4;
	int y0 = LIST_TOP - 4, y1 = 372, first_open = TRUE;
	char line[160];

	/* (the controllers found, at the title's right) */
	if (!input->pad_count)
		snprintf(line, sizeof(line), "NO CONTROLLERS FOUND");
	else
		snprintf(line, sizeof(line), "%d CONTROLLER%s FOUND", input->pad_count, input->pad_count == 1 ? "" : "S");
	page("SPLIT SCREEN", line);
	for (int i = 0; i < DOS_SPLIT_PLAYERS; i++)
	{
		const struct dos_split_player *player = dos_split_player(i);
		int x0 = SAFE_LEFT + 4 + i * (width + 10) - (int)(24.0f * (1.0f - in)), x1 = x0 + width, cx = (x0 + x1) / 2;
		int y = y0 + 12;

		dos_art_frame(DOS_ART_SPLIT_PANEL, 0, NULL, 16, 10.0f, (float)x0, (float)y0, (float)x1, (float)y1,
			faded(player ? 0xF0FFFFFFUL : 0xB0FFFFFFUL, in));
		snprintf(line, sizeof(line), "PLAYER %d", i + 1);
		text_glow(font_large, x0, y, x1, y + ROW_H, JUSTIFY_CENTER, faded(player ? TEXT_BRIGHT : TEXT_DIM, in),
			player ? faded(GLOW_COLOR, in) : 0, line, text_scale());
		y += ROW_H + 10;
		if (!player)
		{
			/* (an open place: the first says how to join) */
			dos_art_draw(DOS_ART_PADS, 0, NULL, (float)(cx - 24), (float)y, (float)(cx + 24), (float)(y + 48),
				faded(0xB0FFFFFFUL, in));
			y += 66;
			if (first_open)
			{
				unsigned long color = faded(pulsing(), in);

				text_fitted(font_help(), x0 + 8, y, x1 - 8, y + 20, JUSTIFY_CENTER, color, "PRESS A");
				text_fitted(font_help(), x0 + 8, y + 22, x1 - 8, y + 42, JUSTIFY_CENTER, color, "OR ENTER");
				text_fitted(font_help(), x0 + 8, y + 44, x1 - 8, y + 64, JUSTIFY_CENTER, color, "TO JOIN");
				first_open = FALSE;
			}
			else
			{
				text_fitted(font_help(), x0 + 8, y, x1 - 8, y + 20, JUSTIFY_CENTER, faded(TEXT_DIM, in), "OPEN");
			}
			continue;
		}
		/* its device: Halo's picture of the player's controller, or a
		keyboard */
		if (player->device == DOS_DEVICE_KEYBOARD)
			keyboard_icon(cx - 34, y + 6, cx + 34, y + 44, faded(FRAME_COLOR, in));
		else
		{
			int art = dos_art_has(DOS_ART_PADS, 3 + 3 * i) ? 3 + 3 * i : 1;

			dos_art_draw(DOS_ART_PADS, art, NULL, (float)(cx - 24), (float)y, (float)(cx + 24), (float)(y + 48),
				faded(0xFFFFFFFFUL, in));
		}
		y += 54;
		text_fitted(font_help(), x0 + 8, y, x1 - 8, y + 20, JUSTIFY_CENTER, faded(TEXT_COLOR, in),
			split_device_name(player));
		y += 30;
		/* its armour, between the arrows that change it */
		text(font_help(), x0, y, x1, y + 18, JUSTIFY_CENTER, faded(TEXT_DIM, in), "ARMOUR");
		y += 20;
		fill(x0 + 14, y + 3, x0 + 30, y + 19, faded(split_color(player, FALSE), in));
		frame_box(x0 + 13, y + 2, x0 + 31, y + 20, faded(FRAME_COLOR, in));
		dos_art_draw(DOS_ART_ARROW_LEFT, 0, NULL, (float)(x0 + 36), (float)y + 5.0f, (float)(x0 + 44), (float)(y + 19),
			faded(0xFFFFFFFFUL, in));
		text_fitted(font_large, x0 + 46, y, x1 - 20, y + ROW_H - 2, JUSTIFY_CENTER, faded(TEXT_BRIGHT, in),
			armour_names[PIN(player->color, 0, 17)]);
		dos_art_draw(DOS_ART_ARROW_RIGHT, 0, NULL, (float)(x1 - 18), (float)y + 5.0f, (float)(x1 - 10), (float)(y + 19),
			faded(0xFFFFFFFFUL, in));
		y += ROW_H + 8;
		/* its team (in team games) */
		text(font_help(), x0, y, x1, y + 18, JUSTIFY_CENTER, faded(TEXT_DIM, in), "TEAM");
		y += 20;
		text(font_large, x0, y, x1, y + ROW_H, JUSTIFY_CENTER, faded(player->team ? 0xFF80A8FFUL : 0xFFFF8C80UL, in),
			player->team ? "BLUE" : "RED");
		/* its own key to leave */
		text_fitted(font_help(), x0 + 8, y1 - 30, x1 - 8, y1 - 12, JUSTIFY_CENTER, faded(TEXT_DIM, in),
			player->device == DOS_DEVICE_KEYBOARD ? "ESC: LEAVE" : "B: LEAVE");
	}
	/* what next */
	if (menu.message[0])
		text_fitted(font_help(), SAFE_LEFT, y1 + 6, SAFE_RIGHT, y1 + 26, JUSTIFY_CENTER, pulsing(), menu.message);
	else if (count < 2)
	{
		text_fitted(font_help(), SAFE_LEFT, y1 + 6, SAFE_RIGHT, y1 + 26, JUSTIFY_CENTER, faded(TEXT_COLOR, in),
			"ANOTHER PLAYER: A ON A CONTROLLER, OR ENTER ON THE KEYBOARD");
	}
	else
	{
		text_fitted(font_help(), SAFE_LEFT, y1 + 6, SAFE_RIGHT, y1 + 26, JUSTIFY_CENTER, faded(pulsing(), in),
			menu.split_return != NONE ? "LEFT/RIGHT: ARMOUR   UP/DOWN: TEAM   START OR ENTER: DONE" :
			"LEFT/RIGHT: ARMOUR   UP/DOWN: TEAM   START OR ENTER: CHOOSE THE GAME");
	}
	/* the devices there */
	snprintf(line, sizeof(line), "KEYBOARD + MOUSE");
	for (int i = 0; i < input->pad_count; i++)
	{
		int at = (int)strlen(line);

		snprintf(line + at, sizeof(line) - at, ", %s", system_pad_name(input->pads[i].id));
	}
	text_fitted(font_help(), SAFE_LEFT, HELP_Y + 4, SAFE_RIGHT, HELP_Y + 24, JUSTIFY_CENTER, faded(TEXT_DIM, in), line);
	footer_drawn(hints, NUMBEROF(hints), TRUE);
}

/* ---------- the JOINING page (a Custom Edition server)

The server's name and map, what is being done ("DOWNLOADING THE MAP",
"UNPACKING THE MAP", the join's own words), and while the map comes a bar:
a dark track in a frame, Halo's blue as far as it has come (a light line on
its top edge), a light sweeping along it; under it how much, how fast and
how long. CANCEL (ESCAPE) gives the join up, the download with it. */

/* a size for people: "850 KB", "12.4 MB" */
static void size_text(long bytes, char *text, int size)
{
	if (bytes < 1024L * 1024L)
		snprintf(text, (size_t)size, "%ld KB", (bytes + 1023) / 1024);
	else
		snprintf(text, (size_t)size, "%ld.%ld MB", bytes / (1024L * 1024L), bytes % (1024L * 1024L) * 10 / (1024L * 1024L));
}

static void upper_text(char *text)
{
	for (; *text; text++)
		*text = (char)(*text >= 'a' && *text <= 'z' ? *text - 'a' + 'A' : *text);
}

/* the bar from x0 to x1: permille of it done (negative: not known, the
sweep alone) */
static void progress_bar(int x0, int y0, int x1, int y1, long permille, float in)
{
	float sweep = (float)(system_microseconds() % 1400000ULL) / 1400000.0f;
	int width = (x1 - x0) / 6, sx = x0 + (int)((float)(x1 - x0 + width) * sweep) - width;

	frame_box(x0 - 3, y0 - 3, x1 + 3, y1 + 3, faded(FRAME_COLOR, in * 0.8f));
	fill(x0, y0, x1, y1, faded(0xF0040810UL, in));
	if (permille >= 0)
	{
		int filled = x0 + (int)((long)(x1 - x0) * PIN(permille, 0, 1000) / 1000);

		fill(x0, y0, filled, y1, faded(0xFF2878E8UL, in));
		fill(x0, y0, filled, y0 + 2, faded(0xFF8CC8FFUL, in));
	}
	fill(sx < x0 ? x0 : sx, y0, sx + width > x1 ? x1 : sx + width, y1, faded(0x50A0D8FFUL, in));
}

static void draw_ce_join(void)
{
	static const int cancel_hints[] = { HINT_CANCEL };
	static const int back_hints[] = { HINT_BACK };
	struct dos_map_download_status download;
	float in = page_in();
	real scale = text_scale();
	char line[128], status[128];
	int downloading = dos_ce_downloading(), y = 112;

	page(screen_title(), NULL);
	/* (the server and its map) */
	text_glow(font_large, SAFE_LEFT, y, SAFE_RIGHT, y + 32, JUSTIFY_CENTER, faded(TEXT_BRIGHT, in),
		faded(GLOW_COLOR, in), menu.join_name[0] ? menu.join_name : "CUSTOM EDITION SERVER", scale * 1.25f);
	y += 40;
	dos_map_download_status(&download);
	snprintf(line, sizeof(line), "%s", downloading && download.map[0] ? download.map : menu.join_map);
	upper_text(line);
	if (line[0])
		text_fitted(font_help(), SAFE_LEFT, y, SAFE_RIGHT, y + 20, JUSTIFY_CENTER, faded(TEXT_COLOR, in), line);
	y += 50;
	if (downloading)
	{
		long permille = -1;
		char done[24], total[24];

		/* (what is being done, the bar, then the numbers) */
		snprintf(status, sizeof(status), "%s", download.state == DOS_MAP_DOWNLOAD_UNPACKING ? "UNPACKING THE MAP" :
			download.state == DOS_MAP_DOWNLOAD_CONNECTING ? "FINDING THE MAP" : "DOWNLOADING THE MAP");
		text_glow(font_large, SAFE_LEFT, y, SAFE_RIGHT, y + 26, JUSTIFY_CENTER, faded(TEXT_BRIGHT, in),
			faded(GLOW_COLOR, in), status, scale);
		y += 40;
		if (download.state == DOS_MAP_DOWNLOAD_UNPACKING && download.unpacked_total > 0)
			permille = (long)((long long)download.unpacked * 1000 / download.unpacked_total);
		else if (download.state == DOS_MAP_DOWNLOAD_DOWNLOADING && download.total > 0)
			permille = (long)((long long)download.received * 1000 / download.total);
		progress_bar(100, y, SCREEN_W - 100, y + 18, permille, in);
		y += 32;
		line[0] = 0;
		if (download.state == DOS_MAP_DOWNLOAD_UNPACKING)
		{
			size_text(download.unpacked, done, sizeof(done));
			size_text(download.unpacked_total, total, sizeof(total));
			snprintf(line, sizeof(line), "%s OF %s  -  %ld%%", done, total, permille >= 0 ? permille / 10 : 0);
		}
		else if (download.state == DOS_MAP_DOWNLOAD_DOWNLOADING)
		{
			char speed[24];
			int at;

			size_text(download.received, done, sizeof(done));
			if (download.total > 0)
			{
				size_text(download.total, total, sizeof(total));
				at = snprintf(line, sizeof(line), "%s OF %s", done, total);
			}
			else
				at = snprintf(line, sizeof(line), "%s", done);
			if (download.bytes_per_second > 0)
			{
				size_text(download.bytes_per_second, speed, sizeof(speed));
				at += snprintf(line + at, sizeof(line) - (size_t)at, "  -  %s/S", speed);
				if (download.total > 0)
				{
					long left = (download.total - download.received) / download.bytes_per_second;

					snprintf(line + at, sizeof(line) - (size_t)at, "  -  %ld:%02ld LEFT", left / 60, left % 60);
				}
			}
		}
		if (line[0])
			text_fitted(font_help(), SAFE_LEFT, y, SAFE_RIGHT, y + 22, JUSTIFY_CENTER, faded(TEXT_COLOR, in), line);
		y += 28;
		snprintf(line, sizeof(line), "FROM %s, CHIMERA'S MAP REPOSITORY", download.host[0] ? download.host : "THE INTERNET");
		upper_text(line);
		text_fitted(font_help(), SAFE_LEFT, y, SAFE_RIGHT, y + 20, JUSTIFY_CENTER, faded(TEXT_DIM, in), line);
	}
	else
	{
		/* (the join's own words: joining, loading, or why it ended) */
		snprintf(status, sizeof(status), "%s", dos_ce_status());
		upper_text(status);
		text_fitted(font_large, SAFE_LEFT, y, SAFE_RIGHT, y + 26, JUSTIFY_CENTER,
			dos_ce_active() ? pulsing() : faded(TEXT_COLOR, in), status[0] ? status : "JOINING");
		y += 40;
		if (dos_ce_active())
			progress_bar(100, y, SCREEN_W - 100, y + 18, -1, in);
	}
	/* (CANCEL, or BACK once the join is over) */
	list_x0 = SCREEN_W / 2 - 110;
	list_x1 = SCREEN_W / 2 + 110;
	draw_highlight(340, in);
	draw_row(0, 340);
	add_hit(list_x0, 337, list_x1, 340 + ROW_H + 3, HIT_ROW, 0);
	if (dos_ce_active())
		footer(cancel_hints, NUMBEROF(cancel_hints));
	else
		footer(back_hints, NUMBEROF(back_hints));
}

/* whether a page has the panel on its right (its rows in the left column) */
static int has_panel(void)
{
	switch (menu.screen)
	{
	case SCREEN_NET_JOIN:
	case SCREEN_LOBBY:
	case SCREEN_PAUSE:
		return FALSE;
	default:
		return TRUE;
	}
}

static void draw_page(void)
{
	static const int hints[] = { HINT_CHOOSE, HINT_CHANGE, HINT_SELECT, HINT_BACK };
	static const int pause_hints[] = { HINT_CHOOSE, HINT_SELECT, HINT_RESUME };
	static const int binding_hints[] = { HINT_CHOOSE, HINT_SELECT, HINT_CLEAR, HINT_BACK };
	static const int browser_hints[] = { HINT_JOIN, HINT_SHOW, HINT_SORT, HINT_FILTERS, HINT_QUICK, HINT_REFRESH,
		HINT_BACK };
	static const int filter_hints[] = { HINT_CHOOSE, HINT_CHANGE, HINT_BACK };
	static const int edit_hints[] = { HINT_KEEP, HINT_UNDO };
	char note[64], help[160];
	int top = LIST_TOP, bottom = LIST_BOTTOM, step, shown, first, panel;
	float in;

	if (menu.screen == SCREEN_TITLE)
	{
		draw_title();
		return;
	}
	if (menu.screen == SCREEN_RESULTS)
	{
		draw_results();
		return;
	}
	if (menu.screen == SCREEN_SPLIT)
	{
		draw_split();
		return;
	}
	if (menu.screen == SCREEN_CE_JOIN)
	{
		draw_ce_join();
		return;
	}
	in = page_in();
	panel = has_panel();
	/* (the network's word on the right of the title, in capitals) */
	note[0] = 0;
	if (menu.screen == SCREEN_NET_JOIN || menu.screen == SCREEN_LOBBY)
	{
		snprintf(note, sizeof(note), "%s", dos_network_message());
		for (int i = 0; note[i]; i++)
		{
			if (note[i] >= 'a' && note[i] <= 'z')
				note[i] = (char)(note[i] - 'a' + 'A');
		}
	}
	/* (the lobby's word is its help line: what it waits for, long) */
	page(screen_title(), menu.screen == SCREEN_LOBBY ? NULL : note);
	/* the rows' column: the left with the panel, else the middle (the
	pause's narrower) */
	if (panel)
	{
		list_x0 = SAFE_LEFT + 16;
		list_x1 = 372;
		draw_panel(388, top - 4, SAFE_RIGHT - 8, bottom + 6);
	}
	else if (menu.screen == SCREEN_PAUSE)
	{
		list_x0 = SCREEN_W / 2 - 150;
		list_x1 = SCREEN_W / 2 + 150;
		dos_art_frame(DOS_ART_ALERT, 0, NULL, 16, 10.0f, (float)(list_x0 - 24), (float)(top - 4), (float)(list_x1 + 24),
			(float)(top + menu.row_count * 32 + 16), faded(0xF0FFFFFFUL, in));
		top += 8;
	}
	else if (menu.screen == SCREEN_NET_JOIN)
	{
		list_x0 = GAME_X0;
		list_x1 = GAME_X1;
	}
	else
	{
		list_x0 = LIST_X0;
		list_x1 = LIST_X1;
	}
	if (menu.screen == SCREEN_NET_JOIN)
	{
		/* (the tabs and the columns' heads over the games; none: why) */
		draw_browser_heads(in);
		top = GAMES_TOP;
		if (!menu.game_count)
		{
			char line[64];
			int hidden = dos_network_hidden();

			if (hidden)
				snprintf(line, sizeof(line), "NONE SHOWN: %d HIDDEN BY THE TAB OR FILTERS", hidden);
			else
				snprintf(line, sizeof(line), "LOOKING FOR GAMES...");
			text(font_large, LIST_X0, top + 40, LIST_X1, top + 40 + ROW_H, JUSTIFY_CENTER, TEXT_COLOR, line);
		}
	}
	/* (the lobby's players under its rows) */
	if (menu.screen == SCREEN_LOBBY)
		bottom = top + menu.row_count * 32;
	step = menu.screen == SCREEN_NET_JOIN ? 28 : menu.row_count * 32 <= bottom - top ? 32 : 30;
	shown = (bottom - top) / step;
	first = first_shown(shown);
	if (menu.screen == SCREEN_NET_JOIN && !menu.game_count)
		top += 100;
	/* the rows' bars, the chosen one's frame gliding over them (a game's
	too), then their text */
	draw_highlight(top + (menu.row - first) * step, in);
	for (int i = first; i < menu.row_count && i < first + shown; i++)
	{
		int y = top + (i - first) * step;

		add_hit(list_x0, y - 3, list_x1, y + ROW_H + 3, HIT_ROW, i);
		if (menu.rows[i] >= ROW_NET_GAME && menu.rows[i] < ROW_NET_GAME + menu.game_count)
			draw_game_row(i, y);
		else
			draw_row(i, y);
	}
	if (first > 0)
	{
		more_arrow((list_x0 + list_x1) / 2, top - 12, FALSE);
		add_hit((list_x0 + list_x1) / 2 - 30, top - 16, (list_x0 + list_x1) / 2 + 30, top - 2, HIT_SCROLL, -1);
	}
	if (first + shown < menu.row_count)
	{
		more_arrow((list_x0 + list_x1) / 2, top + shown * step, TRUE);
		add_hit((list_x0 + list_x1) / 2 - 30, top + shown * step - 4, (list_x0 + list_x1) / 2 + 30,
			top + shown * step + 12, HIT_SCROLL, 1);
	}
	if (menu.screen == SCREEN_LOBBY)
		draw_lobby(bottom + 14);
	/* the last message (in the lobby, the network's word; without the
	panel, the chosen row's help) */
	if (menu.message[0])
		text_fitted(font_help(), SAFE_LEFT, HELP_Y + 2, SAFE_RIGHT, HELP_Y + 26, JUSTIFY_CENTER, pulsing(), menu.message);
	else if (menu.screen == SCREEN_LOBBY && note[0])
		text_fitted(font_help(), SAFE_LEFT, HELP_Y + 2, SAFE_RIGHT, HELP_Y + 26, JUSTIFY_CENTER, TEXT_COLOR, note);
	else if (!panel)
	{
		row_help(menu.rows[menu.row], help, sizeof(help));
		text_fitted(font_help(), SAFE_LEFT, HELP_Y + 2, SAFE_RIGHT, HELP_Y + 26, JUSTIFY_CENTER, TEXT_DIM, help);
	}
	/* (the game list's FILTERS box over it) */
	if (menu.screen == SCREEN_NET_JOIN && menu.filter_box)
		draw_filter_box();
	if (menu.screen == SCREEN_PAUSE)
		footer(pause_hints, NUMBEROF(pause_hints));
	else if (menu.screen == SCREEN_CONTROLS)
		footer(binding_hints, NUMBEROF(binding_hints));
	else if (menu.editing)
		footer(edit_hints, NUMBEROF(edit_hints));
	else if (menu.screen == SCREEN_NET_JOIN && menu.filter_box)
		footer(filter_hints, NUMBEROF(filter_hints));
	else if (menu.screen == SCREEN_NET_JOIN)
		footer(browser_hints, NUMBEROF(browser_hints));
	else
		footer(hints, NUMBEROF(hints));
}

/* Halo's title music (ui.map's sound\music\title1\title1, as ui_widget.c
plays it): on while the menu is up, off when a game loads (main.c's
main_change_map_name stops it). Never "active" to main.c, which would
otherwise hold a map's load for a second's fade the menu here does not do */
static int title_music;

boolean ui_main_menu_music_active(void)
{
	return FALSE;
}

void ui_stop_main_menu_music(void)
{
	extern void scripted_looping_sound_stop(long looping_sound_index);
	long tag = tag_loaded('lsnd', "sound\\music\\title1\\title1");

	if (title_music && tag != NONE)
		scripted_looping_sound_stop(tag);
	title_music = FALSE;
}

static void title_music_update(void)
{
	extern void scripted_looping_sound_start(long looping_sound_index, long object_index, real scale);
	long tag;

	if (title_music || !menu.active || menu.paused)
		return;
	tag = tag_loaded('lsnd', "sound\\music\\title1\\title1");
	if (tag != NONE)
	{
		scripted_looping_sound_start(tag, NONE, 1.0f);
		title_music = TRUE;
	}
}

/* Halo's menu sounds (ui.map's: ui_widget.c's audio feedback), where the
loaded map has them (the pause's map may not) */
static void menu_sound(const char *name)
{
	extern long unspatialized_impulse_sound_new(long definition_index, real scale);
	long tag = tag_loaded('snd!', name);

	if (tag != NONE)
		unspatialized_impulse_sound_new(tag, 1.0f);
}

/* up or down a row: a short page goes round from its last row to its
first, a long list stops at its ends (where holding the key would
otherwise run it round and round) */
static void move_row(int delta)
{
	int next = menu.row + delta;
	int round = menu.row_count <= 8;

	if (next < 0)
		next = round ? menu.row_count - 1 : 0;
	if (next >= menu.row_count)
		next = round ? 0 : menu.row_count - 1;
	if (next == menu.row)
		return;
	menu.row = next;
	menu_sound("sound\\sfx\\ui\\cursor");
}

/* the player's name being typed (the PLAYER page's NAME): letters, digits
and signs from the keyboard where the cursor is, BACKSPACE and DELETE;
LEFT and RIGHT move the cursor, UP and DOWN step the letter under it (a
controller's way to spell it); ENTER keeps it, ESCAPE leaves it as it was */
static void edit_input(void)
{
	static const char letters[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 -_.";
	int key, character, down, length = (int)strlen(menu.edit_text);

	if (key_pressed(SYSTEM_KEY_ENTER) || key_pressed(SYSTEM_KEY_KEYPAD_ENTER))
	{
		char name[12];
		int start = 0, end = length;

		/* (no spaces at its ends; none at all: Halo's own "Player") */
		while (start < end && menu.edit_text[start] == ' ')
			start++;
		while (end > start && menu.edit_text[end - 1] == ' ')
			end--;
		snprintf(name, sizeof(name), "%.*s", end - start, menu.edit_text + start);
		menu_set("player_name", name[0] ? name : "Player");
		dos_settings_save();
		menu.editing = FALSE;
		menu.holding = TRUE;
		menu_sound("sound\\sfx\\ui\\forward");
		return;
	}
	if (key_pressed(SYSTEM_KEY_ESCAPE))
	{
		menu.editing = FALSE;
		menu.holding = TRUE;
		menu_sound("sound\\sfx\\ui\\back");
		return;
	}
	if (key_pressed(SYSTEM_KEY_LEFT) && menu.edit_cursor > 0)
		menu.edit_cursor--;
	if (key_pressed(SYSTEM_KEY_RIGHT) && menu.edit_cursor < length)
		menu.edit_cursor++;
	if ((key_pressed(SYSTEM_KEY_UP) || key_pressed(SYSTEM_KEY_DOWN)) && (menu.edit_cursor < length || length < 11))
	{
		const char *at = menu.edit_cursor < length ? strchr(letters, menu.edit_text[menu.edit_cursor]) : NULL;
		int index = at ? (int)(at - letters) : -1;
		int count = (int)sizeof(letters) - 1;

		index = key_pressed(SYSTEM_KEY_UP) ? (index + count - 1) % count : (index + 1) % count;
		if (menu.edit_cursor == length)
			menu.edit_text[length + 1] = 0;
		menu.edit_text[menu.edit_cursor] = letters[index];
	}
	if (key_pressed(SYSTEM_KEY_BACKSPACE) && menu.edit_cursor > 0)
	{
		memmove(menu.edit_text + menu.edit_cursor - 1, menu.edit_text + menu.edit_cursor,
			(size_t)(length - menu.edit_cursor + 1));
		menu.edit_cursor--;
	}
	if (key_pressed(SYSTEM_KEY_DELETE) && menu.edit_cursor < length)
		memmove(menu.edit_text + menu.edit_cursor, menu.edit_text + menu.edit_cursor + 1, (size_t)(length - menu.edit_cursor));
	/* (the keys typed: their characters, those a name can have) */
	while (system_next_keystroke(&key, &character, &down))
	{
		length = (int)strlen(menu.edit_text);
		if (!down || character < 32 || character > 126 || length >= 11)
			continue;
		memmove(menu.edit_text + menu.edit_cursor + 1, menu.edit_text + menu.edit_cursor,
			(size_t)(length - menu.edit_cursor + 1));
		menu.edit_text[menu.edit_cursor++] = (char)character;
	}
}

/* the join page's FILTERS box: full games, empty games, only the joinable
ones, and DONE; UP and DOWN choose, LEFT, RIGHT and ENTER change, ESCAPE
or F closes it (the filters saved) */
enum
{
	FILTER_BOX_ROWS = 4
};

static void filter_box_close(void)
{
	menu.filter_box = FALSE;
	if (menu.filters_changed)
		dos_settings_save();
	menu.filters_changed = FALSE;
	menu_sound("sound\\sfx\\ui\\back");
}

static void filter_box_input(void)
{
	static const int rows[FILTER_BOX_ROWS - 1] = { ROW_FILTER_FULL, ROW_FILTER_EMPTY, ROW_FILTER_JOINABLE };

	if (back_pressed() || key_pressed(SYSTEM_KEY_F))
	{
		filter_box_close();
		return;
	}
	if (direction(0) && menu.filter_row > 0)
	{
		menu.filter_row--;
		menu_sound("sound\\sfx\\ui\\cursor");
	}
	if (direction(1) && menu.filter_row < FILTER_BOX_ROWS - 1)
	{
		menu.filter_row++;
		menu_sound("sound\\sfx\\ui\\cursor");
	}
	if (menu.filter_row == FILTER_BOX_ROWS - 1)
	{
		if (choose_pressed())
			filter_box_close();
		return;
	}
	if (direction(2) || direction(3) || choose_pressed())
	{
		change(rows[menu.filter_row], 1);
		menu_sound("sound\\sfx\\ui\\cursor");
	}
}

static void browser_show(int show);
static void browser_sort(int sort);
static void browser_filters_open(void);
static void browser_refresh(void);
static void hint_clicked(int hint);

/* the join page's keys besides UP, DOWN, ENTER and ESCAPE: LEFT and RIGHT
the kind of games shown (the tabs over the list), TAB the order, F the
FILTERS box, Q QUICK JOIN, R the list sorted again (a controller: its
D-pad, Y, X, BACK and the right bumper press these); TRUE when one was */
static int browser_input(void)
{
	int delta = (direction(3) ? 1 : 0) - (direction(2) ? 1 : 0);

	if (menu.filter_box)
	{
		filter_box_input();
		return TRUE;
	}
	if (delta)
	{
		browser_show(step((int)PIN(menu_integer("browser_show", DOS_SHOW_ALL), 0, NUMBER_OF_DOS_SHOWS - 1), delta,
			NUMBER_OF_DOS_SHOWS));
		return TRUE;
	}
	if (key_pressed(SYSTEM_KEY_TAB))
	{
		browser_sort(step((int)PIN(menu_integer("browser_sort", DOS_SORT_PLAYERS), 0, NUMBER_OF_DOS_SORTS - 1), 1,
			NUMBER_OF_DOS_SORTS));
		return TRUE;
	}
	if (key_pressed(SYSTEM_KEY_F))
	{
		browser_filters_open();
		return TRUE;
	}
	if (key_pressed(SYSTEM_KEY_Q))
	{
		hint_clicked(HINT_QUICK);
		return TRUE;
	}
	if (key_pressed(SYSTEM_KEY_R))
	{
		browser_refresh();
		return TRUE;
	}
	return FALSE;
}

/* the chosen row done, as ENTER does it: a game joined, a setting stepped,
a button chosen */
static void activate_row(void)
{
	int row = menu.rows[menu.row], stepped;
	char value[80];

	row_label(row, value, sizeof(value), &stepped);
	menu_sound("sound\\sfx\\ui\\forward");
	if (row >= ROW_NET_GAME && row < ROW_NET_GAME + menu.game_count)
	{
		const struct dos_network_game *game = &menu.games[row - ROW_NET_GAME];

		snprintf(menu.join_name, sizeof(menu.join_name), "%s", game->name);
		snprintf(menu.join_map, sizeof(menu.join_map), "%s", game->map);
		if (dos_network_join(row - ROW_NET_GAME))
			go(SCREEN_LOBBY);
		else if (dos_ce_active())
			go(SCREEN_CE_JOIN);
	}
	else if (stepped)
		change(row, 1);
	else
		choose(row);
}

/* a page's way back, as ESCAPE: the pause ended, the lobby left, else the
page above */
static void do_back(void)
{
	if (menu.screen != SCREEN_TITLE || menu.paused)
		menu_sound("sound\\sfx\\ui\\back");
	if (menu.screen == SCREEN_PAUSE)
		pause_end();
	else if (menu.screen == SCREEN_LOBBY)
		choose(ROW_NET_LEAVE);
	else if (menu.screen != SCREEN_TITLE || menu.paused)
		choose(ROW_BACK);
}

/* the game list's own actions (its keys, its footer's keys clicked, its
tabs and heads clicked) */
static void browser_show(int show)
{
	filter_integer("browser_show", PIN(show, 0, NUMBER_OF_DOS_SHOWS - 1));
	dos_settings_save();
	menu.filters_changed = FALSE;
	menu.row = 0;
	menu.first_row = 0;
	menu_sound("sound\\sfx\\ui\\cursor");
}

static void browser_sort(int sort)
{
	filter_integer("browser_sort", PIN(sort, 0, NUMBER_OF_DOS_SORTS - 1));
	dos_settings_save();
	menu.filters_changed = FALSE;
	menu_sound("sound\\sfx\\ui\\cursor");
}

static void browser_filters_open(void)
{
	menu.filter_box = TRUE;
	menu.filter_row = 0;
	menu.filters_changed = FALSE;
	menu_sound("sound\\sfx\\ui\\forward");
}

static void browser_refresh(void)
{
	dos_network_resort();
	snprintf(menu.message, sizeof(menu.message), "THE LIST SORTED AGAIN");
	menu_sound("sound\\sfx\\ui\\cursor");
}

/* rows up or down by the mouse's wheel or a list's arrow: no going round
(the list scrolls with the chosen row) */
static void wheel_rows(int delta)
{
	int next = PIN(menu.row + delta, 0, menu.row_count - 1);

	if (next == menu.row)
		return;
	menu.row = next;
	menu.pointer_chose = FALSE;
	menu_sound("sound\\sfx\\ui\\cursor");
}

/* the mouse's pointer, each frame: moved by the mouse (shown from then
on), its clicks and wheel turns; a key or a controller's button pressed
hides it until the mouse moves again */
static void pointer_update(const struct system_input *input)
{
	float sx, sy, speed = dos_setting_real("menu_mouse_speed", 1.0f);
	unsigned char pressed = (unsigned char)(input->mouse_buttons & ~menu.previous_buttons);

	system_mouse_menu_scale(&sx, &sy);
	menu.pointer_moved = FALSE;
	/* (in a game the mouse is the player's: the pointer waits, hidden, in
	the middle at first) */
	if (!menu.active && !menu.paused)
	{
		menu.previous_buttons = input->mouse_buttons;
		menu.pointer_shown = FALSE;
		menu.clicked = menu.right_clicked = menu.wheel = 0;
		return;
	}
	if (!menu.pointer_x && !menu.pointer_y)
	{
		menu.pointer_x = SCREEN_W / 2;
		menu.pointer_y = SCREEN_H / 2;
	}
	if (input->mouse_dx || input->mouse_dy)
	{
		menu.pointer_x = PIN(menu.pointer_x + (float)input->mouse_dx * sx * speed, 0.0f, (float)(SCREEN_W - 1));
		menu.pointer_y = PIN(menu.pointer_y + (float)input->mouse_dy * sy * speed, 0.0f, (float)(SCREEN_H - 1));
		menu.pointer_moved = TRUE;
		menu.pointer_shown = TRUE;
	}
	menu.clicked = (pressed & SYSTEM_MOUSE_LEFT) != 0;
	menu.right_clicked = (pressed & SYSTEM_MOUSE_RIGHT) != 0;
	menu.wheel = input->mouse_wheel;
	menu.previous_buttons = input->mouse_buttons;
	if (menu.clicked || menu.right_clicked || menu.wheel)
		menu.pointer_shown = TRUE;
	else if (!menu.pointer_moved)
	{
		for (int key = 1; key < SYSTEM_KEY_COUNT; key++)
		{
			if (input->keys[key] && !menu.previous[key])
			{
				menu.pointer_shown = FALSE;
				menu.pointer_chose = FALSE;
				break;
			}
		}
	}
}

/* a footer's key clicked: what pressing it does */
static void hint_clicked(int hint)
{
	switch (hint)
	{
	case HINT_SELECT:
	case HINT_JOIN:
	case HINT_CONTINUE:
		if (menu.screen == SCREEN_RESULTS)
			choose(ROW_BACK);
		else
			activate_row();
		break;
	case HINT_CHANGE:
		change(menu.rows[menu.row], 1);
		menu_sound("sound\\sfx\\ui\\cursor");
		break;
	case HINT_CHOOSE:
		wheel_rows(1);
		break;
	case HINT_BACK:
	case HINT_LEAVE:
	case HINT_CANCEL:
		if (menu.filter_box)
			filter_box_close();
		else
			do_back();
		break;
	case HINT_RESUME:
		pause_end();
		break;
	case HINT_CLEAR:
		if (menu.rows[menu.row] >= ROW_BIND && menu.rows[menu.row] < ROW_BIND + DOS_ACTION_COUNT)
		{
			dos_binding_clear(menu.rows[menu.row] - ROW_BIND);
			menu.settings_changed = TRUE;
		}
		break;
	case HINT_SHOW:
		browser_show(step((int)PIN(menu_integer("browser_show", DOS_SHOW_ALL), 0, NUMBER_OF_DOS_SHOWS - 1), 1,
			NUMBER_OF_DOS_SHOWS));
		break;
	case HINT_SORT:
		browser_sort(step((int)PIN(menu_integer("browser_sort", DOS_SORT_PLAYERS), 0, NUMBER_OF_DOS_SORTS - 1), 1,
			NUMBER_OF_DOS_SORTS));
		break;
	case HINT_FILTERS:
		browser_filters_open();
		break;
	case HINT_QUICK:
		menu_sound("sound\\sfx\\ui\\forward");
		choose(ROW_NET_QUICK);
		break;
	case HINT_REFRESH:
		browser_refresh();
		break;
	}
}

/* the mouse on a page: the wheel scrolls, the right button goes back, the
pointer over a row chooses it and a click does it (or a tab, a head, the
FILTERS box's row, a footer's key); TRUE when it did something the keys
must not do again this frame */
static int mouse_input(void)
{
	int index, kind, value;

	if (!menu.pointer_shown)
		return FALSE;
	if (menu.wheel)
	{
		if (!menu.filter_box)
			wheel_rows(-menu.wheel);
		return TRUE;
	}
	if (menu.right_clicked)
	{
		if (menu.filter_box)
			filter_box_close();
		else
			do_back();
		return TRUE;
	}
	index = hit_under_pointer();
	kind = index >= 0 ? hits[index].kind : NONE;
	value = index >= 0 ? hits[index].value : NONE;
	/* (the FILTERS box up: only it and the footer; a click elsewhere closes
	it) */
	if (menu.filter_box && kind != HIT_FILTER && kind != HIT_HINT)
	{
		if (menu.clicked)
		{
			filter_box_close();
			return TRUE;
		}
		return FALSE;
	}
	switch (kind)
	{
	case HIT_ROW:
		if (value < 0 || value >= menu.row_count)
			return FALSE;
		if ((menu.pointer_moved || menu.clicked) && value != menu.row)
		{
			menu.row = value;
			menu.pointer_chose = TRUE;
			if (!menu.clicked)
				menu_sound("sound\\sfx\\ui\\cursor");
		}
		if (menu.clicked)
		{
			activate_row();
			return TRUE;
		}
		return FALSE;
	case HIT_TAB:
		if (!menu.clicked)
			return FALSE;
		browser_show(value);
		return TRUE;
	case HIT_SORT:
		if (!menu.clicked)
			return FALSE;
		browser_sort(value);
		return TRUE;
	case HIT_FILTER:
		if (value < 0)
			return menu.clicked;
		if (menu.pointer_moved && value != menu.filter_row)
		{
			menu.filter_row = value;
			menu_sound("sound\\sfx\\ui\\cursor");
		}
		if (!menu.clicked)
			return FALSE;
		menu.filter_row = value;
		if (value == FILTER_BOX_ROWS - 1)
			filter_box_close();
		else
		{
			static const int rows[FILTER_BOX_ROWS - 1] = { ROW_FILTER_FULL, ROW_FILTER_EMPTY, ROW_FILTER_JOINABLE };

			change(rows[value], 1);
			menu_sound("sound\\sfx\\ui\\cursor");
		}
		return TRUE;
	case HIT_HINT:
		if (!menu.clicked)
			return FALSE;
		hint_clicked(value);
		return TRUE;
	case HIT_SCROLL:
		if (!menu.clicked)
			return FALSE;
		wheel_rows(value);
		return TRUE;
	}
	return FALSE;
}

/* the pointer: the usual arrow (X its outline, o inside), its tip at the
top left */
static const char *const pointer_art[] =
{
	"X",
	"XX",
	"XoX",
	"XooX",
	"XoooX",
	"XooooX",
	"XoooooX",
	"XooooooX",
	"XoooooooX",
	"XooooooooX",
	"XoooooooooX",
	"XooooooXXXXX",
	"XoooXooX",
	"XooXXooX",
	"XoX  XooX",
	"XX   XooX",
	"X     XooX",
	"      XooX",
	"       XX",
};

#define POINTER_ART_WIDTH 12
#define POINTER_ART_HEIGHT ((int)NUMBEROF(pointer_art))

/* the arrow drawn as a picture at the screen's own pixels: each of its
pixels a whole number of the picture's (one at the smaller resolutions,
two from about 960 lines...), so that it stays sharp; white going to
Halo's pale blue down it, a dark outline, a soft shadow below right. Made
once a scale (the card renderers keep a picture by its address) */
static void draw_pointer(void)
{
	static unsigned long *argb;
	static int made_scale;
	int scale = (int)(dos_screen_scale_y + 0.5f);
	int width, height;

	if (scale < 1)
		scale = 1;
	width = (POINTER_ART_WIDTH + 1) * scale;
	height = (POINTER_ART_HEIGHT + 1) * scale;
	if (!argb || made_scale != scale)
	{
		(free)(argb);
		argb = (unsigned long *)(malloc)((size_t)width * (size_t)height * sizeof(*argb));
		if (!argb)
			return;
		made_scale = scale;
		memset(argb, 0, (size_t)width * (size_t)height * sizeof(*argb));
		for (int pass = 0; pass < 2; pass++)
		{
			for (int row = 0; row < POINTER_ART_HEIGHT; row++)
			{
				for (int column = 0; pointer_art[row][column]; column++)
				{
					char c = pointer_art[row][column];
					unsigned long color;
					int ax = column + (pass ? 0 : 1), ay = row + (pass ? 0 : 1);

					if (c == ' ')
						continue;
					if (!pass)
						color = 0x50000000UL;
					else if (c == 'X')
						color = 0xF0081420UL;
					else
					{
						/* (white at the tip to a pale blue at the tail) */
						int k = row * 255 / (POINTER_ART_HEIGHT - 1);
						unsigned long red = 0xFF - (unsigned long)(k * (0xFF - 0xB8) / 255);
						unsigned long green = 0xFF - (unsigned long)(k * (0xFF - 0xD8) / 255);

						color = 0xFF0000FFUL | (red << 16) | (green << 8);
					}
					for (int y = 0; y < scale; y++)
					{
						for (int x = 0; x < scale; x++)
						{
							unsigned long *texel = &argb[(ay * scale + y) * width + ax * scale + x];

							/* (the shadow only where the arrow is not) */
							if (pass || !*texel)
								*texel = color;
						}
					}
				}
			}
		}
	}
	r_draw_picture((int)DOS_SCREEN_X(menu.pointer_x), (int)(menu.pointer_y * dos_screen_scale_y), argb, width, height);
}

static void handle_input(void)
{
	int row = menu.rows[menu.row];

	/* the controls page waiting for a key: Escape gives up, Delete
	clears, any other key, mouse button or wheel turn is the action's (once
	every key held when it began has been let go) */
	if (menu.binding >= 0)
	{
		int code = dos_binding_pressed();

		if (!menu.binding_armed)
		{
			menu.binding_armed = !any_key_down();
			return;
		}
		/* (Escape, or a controller's B or Start, which press it) */
		if (code == SYSTEM_KEY_ESCAPE || key_pressed(SYSTEM_KEY_ESCAPE))
			menu.binding = -1;
		else if (code == SYSTEM_KEY_DELETE)
		{
			dos_binding_clear(menu.binding);
			menu.settings_changed = TRUE;
			menu.binding = -1;
		}
		else if (code != BIND_NONE)
		{
			dos_binding_assign(menu.binding, code);
			menu.settings_changed = TRUE;
			menu.binding = -1;
		}
		/* (the key let go before the page reads keys again) */
		if (menu.binding < 0)
			menu.holding = TRUE;
		return;
	}
	if (row >= ROW_BIND && row < ROW_BIND + DOS_ACTION_COUNT)
	{
		if (choose_pressed() && !key_pressed(SYSTEM_KEY_SPACE))
		{
			menu.binding = row - ROW_BIND;
			menu.binding_armed = FALSE;
			return;
		}
		if (key_pressed(SYSTEM_KEY_DELETE))
		{
			dos_binding_clear(row - ROW_BIND);
			menu.settings_changed = TRUE;
			return;
		}
	}

	if (menu.screen == SCREEN_RESULTS)
	{
		if (choose_pressed() || back_pressed() || menu.clicked || menu.right_clicked)
			choose(ROW_BACK);
		return;
	}
	if (menu.screen == SCREEN_SPLIT)
	{
		split_input();
		return;
	}
	/* the player's name being typed */
	if (menu.editing)
	{
		edit_input();
		return;
	}
	/* the mouse */
	if (mouse_input())
		return;
	/* the game list's own keys, and its FILTERS box */
	if (menu.screen == SCREEN_NET_JOIN && browser_input())
		return;
	if (direction(0))
	{
		menu.pointer_chose = FALSE;
		move_row(-1);
	}
	if (direction(1))
	{
		menu.pointer_chose = FALSE;
		move_row(1);
	}
	if (direction(2))
	{
		change(menu.rows[menu.row], -1);
		menu_sound("sound\\sfx\\ui\\cursor");
	}
	if (direction(3))
	{
		change(menu.rows[menu.row], 1);
		menu_sound("sound\\sfx\\ui\\cursor");
	}
	if (choose_pressed())
		activate_row();
	else if (back_pressed())
		do_back();
}

/* main_menu_load's (dos_game.c's dos_game_menu): the menu over ui.map */
void dos_menu_open(int results)
{
	/* (the ui map just loaded: its music not started yet) */
	title_music = FALSE;
	memset(menu.previous, 1, sizeof(menu.previous));
	menu.active = TRUE;
	menu.paused = FALSE;
	menu.holding = TRUE;
	menu.result_count = results ? dos_game_results(menu.results, NUMBEROF(menu.results)) : 0;
	/* (the results, or the page the game was chosen on, or the title) */
	if (menu.result_count)
		go(SCREEN_RESULTS);
	else if (dos_network_mode() != DOS_NETWORK_OFF && dos_network_mode() != DOS_NETWORK_SEARCHING)
		go(SCREEN_LOBBY);
	else if (menu.game_screen == SCREEN_CAMPAIGN || menu.game_screen == SCREEN_MULTIPLAYER ||
		menu.game_screen == SCREEN_FIREFIGHT)
	{
		go(menu.game_screen);
	}
	else
		go(SCREEN_TITLE);
}

int dos_menu_active(void)
{
	return menu.active && game_in_progress();
}

int dos_pause_active(void)
{
	return menu.paused;
}

int dos_menu_has_input(void)
{
	return menu.active || menu.paused || menu.holding;
}

/* the game's question (ui_widget.c's, which this build has not got):
whether the main menu's scene is up, ui.map under this menu or its lobby.
players.c spawns no player there: as a stand-in answering FALSE, the menu's
player was spawned again every tick, and each try reset the camera's
observer, so the title's camera jumped from point to point instead of
gliding along the ring */
boolean main_menu_is_active(void)
{
	struct scenario *scenario = global_scenario_get();

	return scenario && scenario->type == _scenario_type_main_menu;
}

/* xdk_input.c's: the split screen page reads each controller itself (which
one pressed A is who joins): none of them is the menu's keys there */
int dos_menu_reads_pads(void)
{
	return menu.active && menu.screen == SCREEN_SPLIT;
}

/* the CONTROLLER page's settings in the local players' profiles, Halo's own
controller settings (player_ui.c's dos_player_ui_controls: a hook; it does
nothing while they are as the profile has them) */
static void controller_settings_apply(void)
{
	extern void dos_player_ui_controls(short button_preset, short joystick_preset, short look_sensitivity,
		boolean invert_look, boolean flight_stick_aircraft_controls, boolean vibration_disabled);
	extern unsigned long dos_settings_generation(void);
	static unsigned long generation = 0xFFFFFFFFUL;
	static short layout, sticks, sensitivity;
	static boolean invert, aircraft, no_vibration;

	if (generation != dos_settings_generation())
	{
		generation = dos_settings_generation();
		layout = (short)PIN(menu_integer("controller_layout", 0), 0, 5);
		sticks = (short)PIN(menu_integer("stick_layout", 0), 0, 3);
		sensitivity = (short)PIN(menu_integer("look_sensitivity", 3), 1, 10);
		invert = menu_integer("invert_look", 0) != 0;
		aircraft = menu_integer("flight_stick_aircraft", 0) != 0;
		no_vibration = menu_integer("vibration", 1) == 0;
	}
	dos_player_ui_controls(layout, sticks, sensitivity, invert, aircraft, no_vibration);
}

/* render_frame's (dos_render_game.c), with the whole screen's window: the
menu or the pause over the frame, and its keys */
/* (the menus' 640 columns centred on a wide screen, their backdrops across
it: dos_screen_offset_x) */
static void menu_frame(void);

void dos_menu_frame(void)
{
	float saved = dos_screen_offset_x;

	dos_screen_offset_x = dos_screen_centred_offset();
	menu_frame();
	dos_screen_offset_x = saved;
}

static void menu_frame(void)
{
	const struct system_input *input = dos_input();
	int in_game = game_in_progress() && !menu.active;

	find_fonts();
	/* (Halo's menu art, kept from the first time ui.map is up) */
	dos_art_load();
	title_music_update();
	scripted_keys();
	/* (the mouse's pointer moved, its clicks and wheel read) */
	pointer_update(input);
	controller_settings_apply();
	/* system link's steps, and the games a search found (the join page's
	rows follow them) */
	dos_network_update();
	dos_network_autotest(menu.active && game_in_progress());
	if (menu.active && menu.screen == SCREEN_NET_JOIN)
	{
		int chosen = menu.row, row_id = menu.row >= 0 && menu.row < menu.row_count ? menu.rows[menu.row] : NONE;
		int had_game = row_id >= ROW_NET_GAME && row_id < ROW_NET_GAME + menu.game_count;
		struct dos_network_game was;

		/* (the list holds still once it has filled, but games come and
		go: the chosen game stays chosen wherever it is, by what it is known
		by, and the page's own rows by what they are) */
		if (had_game)
			was = menu.games[row_id - ROW_NET_GAME];
		menu.game_count = dos_network_games(menu.games, DOS_NETWORK_MAXIMUM_GAMES);
		set_rows(SCREEN_NET_JOIN);
		if (had_game)
		{
			/* (the line it was on when that is still the game: two alike
			never pull the choice back to the first of them) */
			int at = row_id - ROW_NET_GAME;

			if (at < 0 || at >= menu.game_count || strcmp(menu.games[at].key, was.key))
			{
				for (at = 0; at < menu.game_count && strcmp(menu.games[at].key, was.key); at++)
					;
			}
			for (int r = 0; at < menu.game_count && r < menu.row_count; r++)
				if (menu.rows[r] == ROW_NET_GAME + at)
					chosen = r;
		}
		else if (row_id != NONE)
		{
			for (int r = 0; r < menu.row_count; r++)
				if (menu.rows[r] == row_id)
					chosen = r;
		}
		menu.row = PIN(chosen, 0, menu.row_count - 1);
		/* (a game being joined, over the internet or by QUICK JOIN: its
		lobby; a Custom Edition server's: the JOINING page) */
		if (dos_network_mode() == DOS_NETWORK_JOINING || dos_network_mode() == DOS_NETWORK_JOINED)
			go(SCREEN_LOBBY);
		else if (dos_ce_active())
		{
			/* (QUICK JOIN's choice, which the page did not make) */
			dos_network_joining(menu.join_name, sizeof(menu.join_name), menu.join_map, sizeof(menu.join_map));
			go(SCREEN_CE_JOIN);
		}
	}
	/* (a Custom Edition server being joined from elsewhere, the setting
	ce_connect: its JOINING page over the menu's own pages) */
	if (menu.active && dos_ce_active() && (menu.screen == SCREEN_TITLE || menu.screen == SCREEN_MULTIPLAYER_MENU ||
		menu.screen == SCREEN_NETWORK))
	{
		go(SCREEN_CE_JOIN);
	}
	/* (the lobby's rows follow the connection: a host's START) */
	if (menu.active && menu.screen == SCREEN_LOBBY)
	{
		if (dos_network_mode() == DOS_NETWORK_OFF || dos_network_mode() == DOS_NETWORK_SEARCHING)
			go(dos_network_mode() == DOS_NETWORK_SEARCHING ? SCREEN_NET_JOIN : SCREEN_NETWORK);
		else
			set_rows(SCREEN_LOBBY);
	}
	if (menu.holding && !any_key_down())
		menu.holding = FALSE;
	if (menu.active && !game_in_progress())
		menu.active = FALSE;
	/* a game's map up, whatever brought it (the host's START, a joined
	host's game, one joined in progress, over the LAN or the internet, even
	with the network's own state lost on the way): the menu (its lobby)
	gone, the game's keys the player's. The menu is only ever opened with
	the main menu's map (main_menu_load loads it in the same call), and
	comes back with it, with the scores after a game (dos_menu_open) */
	if (menu.active && global_scenario_get() && global_scenario_get()->type != _scenario_type_main_menu)
	{
		char line[96];

		snprintf(line, sizeof(line), "menu: closed for the game (%s page, network %d)", screen_title(),
			dos_network_mode());
		system_log(line);
		menu.active = FALSE;
		menu.holding = TRUE;
		menu.message[0] = 0;
	}
	/* ESCAPE in a game: the pause (not while it ends: its scores are up) */
	if (in_game && !menu.paused && !menu.holding && key_pressed(SYSTEM_KEY_ESCAPE) &&
		dos_game_engine_postgame_state() == 0)
	{
		menu.paused = TRUE;
		menu.holding = FALSE;
		/* (a system link game goes on) */
		if (dos_choice.mode != DOS_MODE_NETWORK)
			game_time_set_paused(TRUE);
		go(SCREEN_PAUSE);
	}
	else if ((menu.active || menu.paused) && !menu.holding)
	{
		handle_input();
	}
	if (menu.active || menu.paused)
	{
		/* (where things are drawn, for the mouse's next frame; the pointer
		over all of it, but on the split screen page, which only the players'
		devices drive) */
		hit_count = 0;
		draw_page();
		if (menu.pointer_shown && menu.screen != SCREEN_SPLIT)
			draw_pointer();
	}
	if (in_game && !menu.paused && dos_firefight_active())
	{
		char status[64];

		if (dos_firefight_status(status, sizeof(status)))
			text(font_small, 0, 26, SCREEN_W, 50, JUSTIFY_CENTER, TEXT_BRIGHT, status);
	}
	memcpy(menu.previous, input->keys, sizeof(menu.previous));
}

/* ---------- the loading screen's picture

dos_render_game.c's loading screen takes this as a map's load begins
(drawn with the old map's fonts, which are still loaded, over black), then
runs its bar under the LOADING: the level's or the map's picture, its name,
what is played (the difficulty or the game type, with its icon); Halo's
logo on the way back to the menu. */
static void draw_loading(const char *map_name, int width, int height);

void dos_menu_draw_loading(const char *map_name, int width, int height)
{
	float saved = dos_screen_offset_x;

	dos_screen_offset_x = dos_screen_centred_offset();
	draw_loading(map_name, width, height);
	dos_screen_offset_x = saved;
}

static void draw_loading(const char *map_name, int width, int height)
{
	const char *folder = map_name ? map_name : "";
	int level = -1, map = -1, ui;
	float in = 1.0f;

	(void)width;
	(void)height;
	for (const char *at = folder; *at; at++)
	{
		if (*at == '\\' || *at == '/')
			folder = at + 1;
	}
	ui = !_stricmp(folder, "ui");
	for (int i = 0; i < dos_campaign_level_count && level < 0; i++)
	{
		if (!_stricmp(folder, dos_campaign_levels[i].folder))
			level = i;
	}
	for (int i = 0; i < dos_multiplayer_map_count && level < 0 && map < 0; i++)
	{
		if (!_stricmp(folder, dos_multiplayer_maps[i].folder))
			map = i;
	}
	find_fonts();
	menu.page_started = 0;
	if (ui)
	{
		draw_logo(SCREEN_W / 2, 120, 520);
	}
	else if (level >= 0 || map >= 0)
	{
		enum dos_art art = level >= 0 ? DOS_ART_LEVELS : DOS_ART_MAPS;
		int index = level >= 0 ? level : dos_map_picture(map), y = 56;
		const char *name = level >= 0 ? dos_campaign_levels[level].name : dos_multiplayer_maps[map].name;

		if (dos_art_has(art, index))
		{
			dos_art_draw(art, index, dos_art_picture_crop, 176.0f, (float)y, 464.0f, (float)(y + 234), 0xFFFFFFFFUL);
			frame_box(175, y - 1, 465, y + 235, FRAME_COLOR);
			y += 248;
		}
		else
		{
			y += 160;
		}
		text_glow(font_large, 0, y, SCREEN_W, y + 32, JUSTIFY_CENTER, TEXT_BRIGHT, GLOW_COLOR, name, text_scale() * 1.4f);
		y += 42;
		/* (what is played: the campaign's difficulty, Firefight, or the game
		type) */
		{
			enum dos_art icon_art = DOS_ART_DIFFICULTIES;
			int icon = dos_choice.difficulty;
			const char *what = dos_difficulty_names[dos_choice.difficulty];
			char line[64];
			int text_w, x;

			if (level < 0 && dos_choice.mode == DOS_MODE_FIREFIGHT)
			{
				snprintf(line, sizeof(line), "FIREFIGHT, %s", dos_difficulty_names[dos_choice.difficulty]);
				what = line;
			}
			else if (level < 0)
			{
				icon_art = DOS_ART_GAME_TYPES;
				icon = game_type_icon(dos_game_types[dos_choice.game_type].variant);
				what = dos_game_types[dos_choice.game_type].name;
			}
			text_w = text_width(font_large, what, text_scale());
			x = (SCREEN_W - text_w - 44) / 2;
			if (dos_art_has(icon_art, icon))
				dos_art_draw(icon_art, icon, NULL, (float)x, (float)y - 4.0f, (float)(x + 34), (float)(y + 30), 0xFFFFFFFFUL);
			text(font_large, x + 44, y, x + 44 + text_w + 8, y + ROW_H, JUSTIFY_LEFT, TEXT_COLOR, what);
		}
	}
	else
	{
		char name[64];

		snprintf(name, sizeof(name), "%s", folder);
		for (int i = 0; name[i]; i++)
		{
			if (name[i] >= 'a' && name[i] <= 'z')
				name[i] = (char)(name[i] - 'a' + 'A');
		}
		text_glow(font_large, 0, 200, SCREEN_W, 232, JUSTIFY_CENTER, TEXT_BRIGHT, GLOW_COLOR, name, text_scale() * 1.4f);
	}
	(void)in;
	text(font_help(), 0, 408, SCREEN_W, 428, JUSTIFY_CENTER, TEXT_DIM, "LOADING");
}
