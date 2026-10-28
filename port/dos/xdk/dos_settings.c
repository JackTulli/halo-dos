/*
DOS_SETTINGS.C

The settings of the DOS and Windows 95 builds: a file beside the program
(the DOS build's HALODOS.CFG, the Windows 95 build's HALO.CFG:
system_settings_file), lines of "name = value" ('#' or ';' starts a
comment), and the command line after it, "-name value" or "-name=value"
("-name" alone is "1"). A setting neither gives takes the default its
caller names. The file is written with every setting the first time the
game starts without one (dos_settings_write_defaults), so that the player
can see what there is; the menu's SETTINGS page changes settings and
writes them back into it (dos_setting_set, dos_settings_save).
*/

#include "platform.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the start-up code's (dos_start.c, w95_start.c) */
extern int system_argc;
extern char **system_argv;

#define MAXIMUM_SETTINGS 96

static struct setting
{
	char name[32];
	char value[96];
	/* changed by the menu since the file was written */
	int changed;
} settings[MAXIMUM_SETTINGS];
static int setting_count;
static int loaded;
/* how many times a setting was set: the answers the game's lookups keep
(dos_config.c) are good while it stays the same */
static unsigned long generation;

unsigned long dos_settings_generation(void)
{
	return generation;
}

static void trim(char *text)
{
	size_t length = strlen(text);
	size_t start = 0;

	while (length && isspace((unsigned char)text[length - 1]))
		text[--length] = 0;
	while (text[start] && isspace((unsigned char)text[start]))
		start++;
	if (start)
		memmove(text, text + start, length - start + 1);
}

static void set(const char *name, const char *value)
{
	int i;

	for (i = 0; i < setting_count; i++)
	{
		if (!_stricmp(settings[i].name, name))
			break;
	}
	if (i == setting_count)
	{
		if (setting_count == MAXIMUM_SETTINGS)
			return;
		setting_count++;
	}
	snprintf(settings[i].name, sizeof(settings[i].name), "%s", name);
	snprintf(settings[i].value, sizeof(settings[i].value), "%s", value);
	generation++;
}

static void settings_path(char *path, size_t size)
{
	snprintf(path, size, "%s\\%s", system_program_directory(), system_settings_file());
}

void dos_settings_write_defaults(void);

static void load(void)
{
	char path[300], line[256];
	FILE *file;

	loaded = 1;
	/* (the first start: the file with every setting, then read) */
	dos_settings_write_defaults();
	settings_path(path, sizeof(path));
	file = fopen(path, "r");
	if (file)
	{
		while (fgets(line, sizeof(line), file))
		{
			char *equals, *comment = strpbrk(line, "#;");

			if (comment)
				*comment = 0;
			equals = strchr(line, '=');
			if (!equals)
				continue;
			*equals = 0;
			trim(line);
			trim(equals + 1);
			if (line[0])
				set(line, equals + 1);
		}
		fclose(file);
	}
	for (int i = 1; i < system_argc; i++)
	{
		const char *argument = system_argv[i];
		char name[32];
		const char *value = "1";
		const char *equals;

		if (argument[0] != '-' && argument[0] != '/')
			continue;
		argument++;
		equals = strchr(argument, '=');
		if (equals)
		{
			snprintf(name, sizeof(name), "%.*s", (int)(equals - argument), argument);
			value = equals + 1;
		}
		else
		{
			snprintf(name, sizeof(name), "%s", argument);
			/* (the next argument its value, unless it is an option: a
			negative number is a value) */
			if (i + 1 < system_argc && system_argv[i + 1][0] != '/' && (system_argv[i + 1][0] != '-' ||
				(system_argv[i + 1][1] >= '0' && system_argv[i + 1][1] <= '9') || system_argv[i + 1][1] == '.'))
			{
				value = system_argv[++i];
			}
		}
		set(name, value);
	}
}

const char *dos_setting(const char *name, const char *default_value)
{
	if (!loaded)
		load();
	for (int i = 0; i < setting_count; i++)
	{
		if (!_stricmp(settings[i].name, name))
			return settings[i].value;
	}
	return default_value;
}

long dos_setting_integer(const char *name, long default_value)
{
	const char *value = dos_setting(name, NULL);

	return value && value[0] ? strtol(value, NULL, 0) : default_value;
}

float dos_setting_real(const char *name, float default_value)
{
	const char *value = dos_setting(name, NULL);

	return value && value[0] ? (float)atof(value) : default_value;
}

int dos_setting_boolean(const char *name, int default_value)
{
	const char *value = dos_setting(name, NULL);

	if (!value || !value[0])
		return default_value;
	return !_stricmp(value, "1") || !_stricmp(value, "yes") || !_stricmp(value, "true") || !_stricmp(value, "on");
}

/* the file's first version, with every setting and what it does (the
picture's defaults are the build's: system_default_settings) */
void dos_settings_write_defaults(void)
{
	static const char defaults[] =
		"# Halo for DOS and Windows 95: settings (the menu's SETTINGS page changes\n"
		"# the most used). Each can also be given on the command line:\n"
		"# HALO -width 640 -height 480; -play bloodgulch starts a game of that map\n"
		"# straight away (with -game team_slayer, -bots 4...), skipping the menu\n"
		"\n"
		"# the picture: its size, and the colours: 8 (256 colours, the fastest),\n"
		"# 16 (thousands) or 32 (millions)\n"
		"width = %d\n"
		"height = %d\n"
		"bits = %d\n"
		"\n"
		"# the screen's shape: 0 the picture's own (a window's, DOS's 4:3), else\n"
		"# its width over its height times 100 (133 4:3, 160 16:10, 178 16:9,\n"
		"# 233 21:9) for a monitor that stretches the picture. Wider than 4:3, the\n"
		"# game is wide: the views show more to the sides, the HUD keeps to its\n"
		"# corners, the menus are centred\n"
		"aspect = 0\n"
		"\n"
		"# for slow machines (the software renderer): each pixel twice as wide\n"
		"# (double_wide = 1: 320x240 drawn as 160x240) or twice as tall\n"
		"# (double_tall = 1), or both (160x120)\n"
		"double_wide = 0\n"
		"double_tall = 0\n"
		"\n"
		"# Windows 95 only: fullscreen 0 a window (Alt+Enter switches), 1 the\n"
		"# whole screen at the desktop's resolution, 2 the whole screen at a\n"
		"# display mode near the picture's size (faster on old PCs); scaling 0\n"
		"# sharp (whole multiples of the picture), 1 fill (as large as fits, its\n"
		"# shape kept), 2 stretch (all of the screen)\n"
		"fullscreen = 0\n"
		"scaling = 0\n"
		"\n"
		"# renderer: software (the processor draws); on DOS voodoo (a 3dfx\n"
		"# Voodoo Graphics card draws and shows it all at 640x480, through its\n"
		"# pass-through cable; tried in DOSBox-X's emulated one); on Windows 95\n"
		"# opengl (the graphics card, through its OpenGL driver: opengl32.dll,\n"
		"# from Windows 95 OSR2 or Microsoft's OpenGL 95; always in thousands or\n"
		"# millions of colours, at the window's own resolution) or direct3d (the\n"
		"# graphics card through Direct3D 8: DirectX 8.0a or later, as OpenGL)\n"
		"renderer = software\n"
		"\n"
		"# with direct3d, the Xbox's look: the level lit by Halo's own lightmaps\n"
		"# (d3d_lightmaps = 1; 0 the light at its corners, as opengl) and every\n"
		"# object lit as Halo lights it, at each corner (d3d_object_light = 1; 0\n"
		"# their textures' own colours, as opengl), the detail maps and the\n"
		"# skies' layered shaders mixed on the card as the Xbox did\n"
		"# (d3d_textures = 1; 0 baked into textures, as opengl), and every map's\n"
		"# own sky (d3d_sky = 1; 0 Blood Gulch's stand-in gradient, as opengl)\n"
		"d3d_lightmaps = 1\n"
		"d3d_object_light = 1\n"
		"d3d_textures = 1\n"
		"d3d_sky = 1\n"
		"\n"
		"# the first-person view's width in degrees, 50 to 120 (70: Halo's own)\n"
		"field_of_view = 70\n"
		"\n"
		"# how much is drawn: draw_distance in world units (Halo's maps are about\n"
		"# 100 to 300 across), detail 0 (the least) to 3 (the most), and the\n"
		"# largest texture side (32, 64, 128, 256, or 1024 for Halo's own:\n"
		"# SETTINGS, TEXTURE QUALITY)\n"
		"draw_distance = 90\n"
		"detail = 2\n"
		"texture_size = 128\n"
		"\n"
		"# textures seen up close, in thousands or millions of colours: their\n"
		"# texels dithered into one another (texture_dither = 1, the default\n"
		"# there), or filtered smooth (texture_smooth = 1: for a fast machine)\n"
		"texture_smooth = 0\n"
		"\n"
		"# internet play (as ChupathingyCE plays it): a hosted game is listed\n"
		"# online unless network.host_public = false, and its invite link is\n"
		"# written to INVITE.TXT; to join a game by its link, put the link in\n"
		"# JOIN.TXT beside the program before starting it. On DOS the network\n"
		"# card's packet driver must be loaded; net_address, net_netmask,\n"
		"# net_gateway and net_dns set the address where there is no DHCP.\n"
		"# The online list needs the right time: DOS asks net_time_server\n"
		"# (pool.ntp.org; 0 none) when the network starts, and without an\n"
		"# answer takes time_zone, hours from UTC (-5 New York in winter, 1 Berlin)\n"
		"network.host_public = true\n"
		"\n"
		"# the level's light: 1 its lightmaps as its vertices' colours, read\n"
		"# when the map loads (as the N64 version: no light is worked out while\n"
		"# playing, and objects and the weapon in your hands are never darkened),\n"
		"# 0 none (every surface its texture's own colour)\n"
		"level_light = 1\n"
		"\n"
		"# the weapon in your hands (VIEW MODEL): 4 very low, 0 low, 1 medium\n"
		"# (fewer triangles: faster), 2 high (all of them), 3 off (not drawn)\n"
		"view_model = 2\n"
		"\n"
		"# particles (sparks, dust, blood, muzzle flashes): 1 on, 0 off (the\n"
		"# faster: none is made); particle_limit, the most alive at once\n"
		"particles = 1\n"
		"particle_limit = 96\n"
		"\n"
		"# sound: 1 on (DOS: a Sound Blaster, found by the BLASTER variable or\n"
		"# sound_blaster = \"A220 I5 D1 H5\"; Windows 95: the wave mapper), 0 off\n"
		"# (the faster); sound_volume 0.0 to 1.0; sound_rate 22050 (11025 is\n"
		"# lighter, 44100 for fast machines)\n"
		"sound = 1\n"
		"sound_volume = 1.0\n"
		"sound_rate = 22050\n"
		"\n"
		"# the mouse\n"
		"mouse_sensitivity = 1.0\n"
		"invert_mouse = 0\n"
		"\n"
		"# the keys (SETTINGS > CONTROLS): up to three a action, commas between,\n"
		"# NONE for none; keys by name (W, SPACE, LEFT_SHIFT, F1, KEYPAD_0, UP...),\n"
		"# MOUSE_LEFT, MOUSE_RIGHT, MOUSE_MIDDLE, WHEEL_UP, WHEEL_DOWN. Escape is\n"
		"# always the pause\n"
		"bind_forward = W\n"
		"bind_back = S\n"
		"bind_left = A\n"
		"bind_right = D\n"
		"bind_jump = SPACE,ENTER\n"
		"bind_crouch = LEFT_CTRL,C\n"
		"bind_fire = MOUSE_LEFT,LEFT_SHIFT,RIGHT_SHIFT\n"
		"bind_grenade = MOUSE_RIGHT,G\n"
		"bind_melee = F,BACKSPACE\n"
		"bind_action = E\n"
		"bind_reload = R\n"
		"bind_zoom = Z,MOUSE_MIDDLE\n"
		"bind_weapon = TAB,WHEEL_UP,WHEEL_DOWN\n"
		"bind_grenades = X\n"
		"bind_flashlight = Q\n"
		"bind_scores = F1\n"
		"bind_look_up = UP\n"
		"bind_look_down = DOWN\n"
		"bind_look_left = LEFT\n"
		"bind_look_right = RIGHT\n"
		"\n"
		"# a game controller (Windows: XInput, or a joystick Windows knows;\n"
		"# SETTINGS > CONTROLLER): controller 1 on, 0 off; the original game's\n"
		"# settings: controller_layout 0 default, 1 southpaw, 2 jumpy, 3 boxer,\n"
		"# 4 green thumb, 5 bumper jumper; stick_layout 0 default, 1 southpaw,\n"
		"# 2 legacy, 3 legacy southpaw; look_sensitivity 1 to 10; invert_look;\n"
		"# flight_stick_aircraft (1: aircraft pitch as a plane does); vibration\n"
		"controller = 1\n"
		"controller_layout = 0\n"
		"stick_layout = 0\n"
		"look_sensitivity = 3\n"
		"invert_look = 0\n"
		"flight_stick_aircraft = 0\n"
		"vibration = 1\n"
		"\n"
		"# the frame rate and the timings, at the top left\n"
		"fps_counter = %d\n"
		"\n"
		"# the most frames a second: 30 (Halo's own), 60 or 120 (frames drawn\n"
		"# between the game's 30 ticks a second), 0 no limit\n"
		"frame_rate = 30\n"
		"\n"
		"# the skies' own models (stars, planets, clouds) instead of a gradient:\n"
		"# 1 on, 0 off (the faster)\n"
		"sky_model = %d\n"
		"\n"
		"# the player: the name (up to 11 letters) and the armour's colour in\n"
		"# free-for-all games (Halo's colours: 0 white, 1 black, 2 red, 3 blue...)\n"
		"player_name = Player\n"
		"player_color = 2\n";
	char path[300];
	FILE *file;
	int width, height, bits, fps_counter, sky_model;

	settings_path(path, sizeof(path));
	file = fopen(path, "r");
	if (file)
	{
		fclose(file);
		return;
	}
	system_default_settings(&width, &height, &bits, &fps_counter, &sky_model);
	file = fopen(path, "w");
	if (file)
	{
		fprintf(file, defaults, width, height, bits, fps_counter, sky_model);
		fclose(file);
	}
}

/* the menu's: a setting's new value, for this session and (dos_settings_
save) the file */
void dos_setting_set(const char *name, const char *value)
{
	if (!loaded)
		load();
	set(name, value);
	for (int i = 0; i < setting_count; i++)
	{
		if (!_stricmp(settings[i].name, name))
			settings[i].changed = 1;
	}
}

/* the changed settings written into the file: each one's line given its
new value, those it has no line for added at its end */
void dos_settings_save(void)
{
	char path[300], line[256], *text = NULL;
	size_t length = 0, size = 0;
	int written[MAXIMUM_SETTINGS] = { 0 };
	FILE *file;

	settings_path(path, sizeof(path));
	file = fopen(path, "r");
	if (file)
	{
		while (fgets(line, sizeof(line), file))
		{
			char name[64], *equals = strchr(line, '=');
			char *comment = strpbrk(line, "#;");
			int replaced = 0;

			if (equals && (!comment || comment > equals))
			{
				snprintf(name, sizeof(name), "%.*s", (int)(equals - line), line);
				trim(name);
				for (int i = 0; i < setting_count; i++)
				{
					if (settings[i].changed && !_stricmp(settings[i].name, name))
					{
						snprintf(line, sizeof(line), "%s = %s\n", settings[i].name, settings[i].value);
						written[i] = 1;
						replaced = 1;
					}
				}
			}
			(void)replaced;
			if (length + strlen(line) + 1 > size)
			{
				char *grown = realloc(text, size = (length + strlen(line) + 1) * 2);

				if (!grown)
					break;
				text = grown;
			}
			memcpy(text + length, line, strlen(line) + 1);
			length += strlen(line);
		}
		fclose(file);
	}
	file = fopen(path, "w");
	if (!file)
	{
		free(text);
		return;
	}
	if (text)
		fputs(text, file);
	if (length && text[length - 1] != '\n')
		fputs("\n", file);
	for (int i = 0; i < setting_count; i++)
	{
		if (settings[i].changed && !written[i])
			fprintf(file, "%s = %s\n", settings[i].name, settings[i].value);
		settings[i].changed = 0;
	}
	fclose(file);
	free(text);
}

float dos_mouse_sensitivity(void)
{
	return dos_setting_real("mouse_sensitivity", 1.0f);
}

int dos_invert_mouse(void)
{
	return dos_setting_boolean("invert_mouse", 0);
}
