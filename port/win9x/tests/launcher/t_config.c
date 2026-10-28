/*
T_CONFIG.C

port_config.c's table interface and its writer (config_setting_count,
config_setting_info, config_setting_text, config_setting_set_text,
config_save) and the launcher's hook, built as the 9x build builds the
file (HALO_WIN9X) over the UCRT, with the Windows-facing functions it
calls stood in for here. Run with a folder of its own as the argument:
its config.toml is written there.

    t_config <folder>

The output lists the table (setting lines, for launcher_check.py to
compare with the source), then the checks (w9x_test.h). The checks:

 1. the table: names in "section.key" form, each once; the defaults of
    the numbers and booleans parse; a string's default has no quotes;
 2. without a file, every value reads as its default, and the first
    read shows the launcher (stood in for) the settings this build reads,
    with their values as text; what the stand-in changes is what the game
    then reads, and the file is written with it;
 3. the writer keeps a file as the player left it: comments, blank
    lines, the order, keys of no setting, spacing round the '=', a
    comment after a value, CRLF line ends, a string with escapes; only
    the values of the settings change, and a setting the file lacks is
    added to its section at the value;
 4. -nolauncher on the command line keeps the launcher away, -launcher
    brings it whatever launcher.show says, and an invite link on the
    command line keeps it away unless -launcher is there.
*/

#include "../w9x_test.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port_config.h"
#include "launcher.h"

/* ---------- what the real build has elsewhere */

static char folder[1024];

int w9x_executable_directory(char *path, int size)
{
	snprintf(path, (size_t)size, "%s", folder);
	return 1;
}

void platform_log(const char *format, ...)
{
	(void)format;
}

/* the game's tan (port/third_party/musl-math, through the prefix header) */
double halo_tan(double x)
{
	return tan(x);
}

/* (one thread: port_config.c's lock does nothing here) */
int pthread_mutex_lock(pthread_mutex_t *mutex)
{
	(void)mutex;
	return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
	(void)mutex;
	return 0;
}

/* the test's command line (posix_command_line_argument, win32_p2p.c) */
static const char *arguments[8];
static int argument_count;

int posix_command_line_argument(int index, char *buffer, unsigned long size)
{
	if (index < 0 || index >= argument_count)
		return 0;
	snprintf(buffer, size, "%s", arguments[index]);
	return 1;
}

/* the launcher: counts its calls, remembers what it was shown, and sets
what the test asks for */
static int launcher_calls;
static int launcher_count;
static char launcher_names[256][64];
static char launcher_values[256][256];
static const char *launcher_set_name;
static const char *launcher_set_value;
static int launcher_answer = 1;

int launcher_show(struct launcher_setting *settings, int count)
{
	int index;

	launcher_calls++;
	launcher_count = count;
	for (index = 0; index < count && index < 256; index++)
	{
		snprintf(launcher_names[index], sizeof(launcher_names[index]), "%s", settings[index].name);
		snprintf(launcher_values[index], sizeof(launcher_values[index]), "%s", settings[index].value);
		if (launcher_set_name && !strcmp(settings[index].name, launcher_set_name))
			snprintf(settings[index].value, (size_t)settings[index].value_size, "%s", launcher_set_value);
	}
	return launcher_answer;
}

/* the settings are read once a process: each scene is a run of this
program (the first argument names the folder, the second the scene) */

static int launcher_saw(const char *name)
{
	int index;

	for (index = 0; index < launcher_count; index++)
	{
		if (!strcmp(launcher_names[index], name))
			return 1;
	}
	return 0;
}

static const char *launcher_value(const char *name)
{
	int index;

	for (index = 0; index < launcher_count; index++)
	{
		if (!strcmp(launcher_names[index], name))
			return launcher_values[index];
	}
	return "";
}

static char *read_file(const char *name)
{
	char path[1200];
	FILE *file;
	long length;
	char *text;

	snprintf(path, sizeof(path), "%s\\%s", folder, name);
	file = fopen(path, "rb");
	if (!file)
		return NULL;
	fseek(file, 0, SEEK_END);
	length = ftell(file);
	fseek(file, 0, SEEK_SET);
	text = malloc((size_t)length + 1);
	length = (long)fread(text, 1, (size_t)length, file);
	text[length] = 0;
	fclose(file);
	return text;
}

static void write_file(const char *name, const char *text)
{
	char path[1200];
	FILE *file;

	snprintf(path, sizeof(path), "%s\\%s", folder, name);
	file = fopen(path, "wb");
	fwrite(text, 1, strlen(text), file);
	fclose(file);
}

static void remove_file(const char *name)
{
	char path[1200];

	snprintf(path, sizeof(path), "%s\\%s", folder, name);
	remove(path);
}

static int setting_index(const char *name)
{
	struct config_setting_info info;
	int index;

	for (index = 0; config_setting_info(index, &info); index++)
	{
		if (!strcmp(info.name, name))
			return index;
	}
	return -1;
}

/* ---------- scene 1: the table */

static int scene_table(void)
{
	struct config_setting_info info;
	int count = config_setting_count();
	int index, other;

	CHECK(count > 40);
	CHECK(!config_setting_info(-1, &info));
	CHECK(!config_setting_info(count, &info));
	for (index = 0; index < count; index++)
	{
		struct config_setting_info again;
		const char *dot;
		char *end;

		if (!CHECK(config_setting_info(index, &info)))
			continue;
		printf("setting\t%s\t%d\t%s\t%d\n", info.name, info.type, info.default_text, info.read_here);
		dot = strchr(info.name, '.');
		CHECK(dot && dot > info.name && dot[1] && !strchr(dot + 1, '.'));
		CHECK(info.type >= 0 && info.type <= 3);
		CHECK(info.comment && info.comment[0]);
		for (other = 0; other < index; other++)
		{
			config_setting_info(other, &again);
			if (!strcmp(again.name, info.name))
				printf("# FAIL %s is in the table twice\n", info.name);
			CHECK(strcmp(again.name, info.name) != 0);
		}
		switch (info.type)
		{
		case 0:
			CHECK(!strcmp(info.default_text, "true") || !strcmp(info.default_text, "false"));
			break;
		case 1:
			strtol(info.default_text, &end, 10);
			CHECK(info.default_text[0] && !*end);
			break;
		case 2:
			strtod(info.default_text, &end);
			CHECK(info.default_text[0] && !*end);
			break;
		default:
			CHECK(info.default_text[0] != '"');
			break;
		}
	}
	return test_finish("table");
}

/* ---------- scene 2: no file; the launcher's first showing */

static int scene_first_run(void)
{
	struct config_setting_info info;
	int index;
	char text[256];
	char *file;

	remove_file("config.toml");
	launcher_set_name = "display.fullscreen";
	launcher_set_value = "false";
	/* the first read of any setting: the file (written with the
	defaults), then the launcher */
	CHECK(config_integer("display.window_scale") == 2);
	CHECK(launcher_calls == 1);
	CHECK(launcher_count > 20);
	/* what it was shown: this build's settings, each at its default as
	text */
	for (index = 0; config_setting_info(index, &info); index++)
	{
		CHECK(launcher_saw(info.name) == info.read_here);
		if (info.read_here)
		{
			if (strcmp(launcher_value(info.name), info.default_text))
				printf("# %s was shown as \"%s\", default \"%s\"\n", info.name, launcher_value(info.name), info.default_text);
			CHECK(!strcmp(launcher_value(info.name), info.default_text));
		}
	}
	CHECK(launcher_saw("display.fullscreen"));
	CHECK(launcher_saw("display.mode"));
	CHECK(launcher_saw("launcher.show"));
	CHECK(!launcher_saw("display.hd_720p"));
	CHECK(!launcher_saw("audio.backend"));
	/* its change is what the game reads, and is in the file */
	CHECK(config_boolean("display.fullscreen") == 0);
	CHECK(config_setting_text(setting_index("display.fullscreen"), text, sizeof(text)) && !strcmp(text, "false"));
	file = read_file("config.toml");
	if (CHECK(file != NULL))
	{
		CHECK(strstr(file, "\nfullscreen = false\n") != NULL);
		CHECK(strstr(file, "\nwindow_scale = 2\n") != NULL);
		CHECK(strstr(file, "# Halo settings") != NULL);
		CHECK(strstr(file, "\n[launcher]\n") != NULL);
		CHECK(strstr(file, "\nshow = true\n") != NULL);
		free(file);
	}
	/* the launcher is shown once */
	config_boolean("display.vsync");
	CHECK(launcher_calls == 1);
	/* text in and out of every type */
	CHECK(config_setting_set_text(setting_index("display.field_of_view"), "90"));
	CHECK(config_real("display.field_of_view") == 90.0);
	CHECK(config_setting_text(setting_index("display.field_of_view"), text, sizeof(text)) && !strcmp(text, "90.0"));
	CHECK(config_setting_set_text(setting_index("display.field_of_view"), "72.5"));
	CHECK(config_setting_text(setting_index("display.field_of_view"), text, sizeof(text)) && !strcmp(text, "72.5"));
	CHECK(config_setting_set_text(setting_index("display.renderer"), "gl1"));
	CHECK(!strcmp(config_string("display.renderer"), "gl1"));
	CHECK(config_setting_set_text(setting_index("display.window_scale"), "3"));
	CHECK(config_integer("display.window_scale") == 3);
	CHECK(config_setting_set_text(setting_index("display.vsync"), "no"));
	CHECK(config_boolean("display.vsync") == 0);
	CHECK(!config_setting_set_text(-1, "x"));
	CHECK(!config_setting_text(config_setting_count(), text, sizeof(text)));
	return test_finish("first run");
}

/* ---------- scene 3: a file as the player left it */

static const char player_file[] =
	"# my settings\r\n"
	"\r\n"
	"[display]\r\n"
	"# keep the window\r\n"
	"fullscreen=false # until the driver is fixed\r\n"
	"window_scale   =   3\r\n"
	"renderer = 'gl2'\r\n"
	"mode = \"800x600x16\"\r\n"
	"field_of_view = 80\r\n"
	"made_up_key = 12\r\n"
	"\r\n"
	"[graphics]\r\n"
	"shadows = false\r\n"
	"\r\n"
	"[paths]\r\n"
	"data = 'C:\\Games\\Halo'\r\n"
	"\r\n"
	"[mine]\r\n"
	"thing = \"kept\"\r\n"
	"\r\n"
	"[input]\r\n"
	"gamepad_map = \"a:b0,b:b1\" # my pad\r\n"
	"\r\n"
	"[launcher]\r\n"
	"show = false\r\n";

static int scene_player_file(void)
{
	char *file;

	write_file("config.toml", player_file);
	/* launcher.show is false: no launcher */
	CHECK(config_boolean("display.fullscreen") == 0);
	CHECK(launcher_calls == 0);
	CHECK(config_integer("display.window_scale") == 3);
	CHECK(!strcmp(config_string("display.renderer"), "gl2"));
	CHECK(config_real("display.field_of_view") == 80.0);
	CHECK(!strcmp(config_string("paths.data"), "C:\\Games\\Halo"));
	/* the file was completed with the settings it lacked, and nothing
	else changed */
	file = read_file("config.toml");
	if (CHECK(file != NULL))
	{
		CHECK(strstr(file, "# my settings\r\n") == file);
		CHECK(strstr(file, "fullscreen=false # until the driver is fixed\r\n") != NULL);
		CHECK(strstr(file, "made_up_key = 12") != NULL);
		CHECK(strstr(file, "[mine]\r\nthing = \"kept\"") != NULL);
		CHECK(strstr(file, "vsync = true") != NULL);
		free(file);
	}
	/* the launcher's kind of change, then the write */
	config_setting_set_text(setting_index("display.fullscreen"), "true");
	config_setting_set_text(setting_index("display.window_scale"), "1");
	config_setting_set_text(setting_index("display.renderer"), "gl1");
	config_setting_set_text(setting_index("display.field_of_view"), "70");
	config_setting_set_text(setting_index("paths.data"), "D:\\Halo \"CE\"");
	config_setting_set_text(setting_index("input.gamepad_map"), "");
	config_setting_set_text(setting_index("graphics.decals"), "false");
	CHECK(config_save());
	file = read_file("config.toml");
	if (CHECK(file != NULL))
	{
		/* values replaced in place, the rest of each line kept */
		CHECK(strstr(file, "# my settings\r\n\r\n[display]\r\n# keep the window\r\n"
			"fullscreen=true # until the driver is fixed\r\nwindow_scale   =   1\r\nrenderer = \"gl1\"\r\n"
			"mode = \"800x600x16\"\r\nfield_of_view = 70.0\r\nmade_up_key = 12\r\n") == file);
		CHECK(strstr(file, "\r\nshadows = false\r\n") != NULL);
		CHECK(strstr(file, "\ndecals = false\n") != NULL);
		CHECK(strstr(file, "data = \"D:\\\\Halo \\\"CE\\\"\"\r\n") != NULL);
		CHECK(strstr(file, "[mine]\r\nthing = \"kept\"\r\n") != NULL);
		CHECK(strstr(file, "gamepad_map = \"\" # my pad\r\n") != NULL);
		CHECK(strstr(file, "[launcher]\r\nshow = false\r\n") != NULL);
		CHECK(strstr(file, "made_up_key") != NULL);
		free(file);
	}
	/* read back the same */
	return test_finish("player file");
}

/* ---------- scene 4: a file that does not parse */

static int scene_broken_file(void)
{
	char *file;

	write_file("config.toml", "[display\nfullscreen = maybe\n");
	CHECK(config_boolean("display.fullscreen") == 1);
	CHECK(launcher_calls == 1);
	config_setting_set_text(setting_index("display.fullscreen"), "false");
	CHECK(config_save());
	file = read_file("config.toml");
	if (CHECK(file != NULL))
	{
		CHECK(strstr(file, "# Halo settings") != NULL);
		CHECK(strstr(file, "\nfullscreen = false\n") != NULL);
		free(file);
	}
	return test_finish("broken file");
}

/* ---------- scene 5: the command line */

static int scene_command_line(const char *scene)
{
	remove_file("config.toml");
	if (!strcmp(scene, "nolauncher"))
	{
		arguments[argument_count++] = "-nolauncher";
		config_boolean("display.fullscreen");
		CHECK(launcher_calls == 0);
	}
	else if (!strcmp(scene, "launcher"))
	{
		write_file("config.toml", "[launcher]\nshow = false\n");
		arguments[argument_count++] = "-launcher";
		config_boolean("display.fullscreen");
		CHECK(launcher_calls == 1);
	}
	else if (!strcmp(scene, "link"))
	{
		arguments[argument_count++] = "halo://join/abc";
		config_boolean("display.fullscreen");
		CHECK(launcher_calls == 0);
	}
	else if (!strcmp(scene, "link-launcher"))
	{
		arguments[argument_count++] = "halo://join/abc";
		arguments[argument_count++] = "-launcher";
		config_boolean("display.fullscreen");
		CHECK(launcher_calls == 1);
	}
	else if (!strcmp(scene, "quit"))
	{
		/* Quit ends the program: the exit code says so */
		launcher_answer = 0;
		config_boolean("display.fullscreen");
		printf("# FAIL the launcher's Quit did not end the program\n");
		return 1;
	}
	return test_finish(scene);
}

int main(int argc, char **argv)
{
	const char *scene = argc > 2 ? argv[2] : "table";

	if (argc < 2)
	{
		fprintf(stderr, "t_config <folder> [scene]\n");
		return 2;
	}
	snprintf(folder, sizeof(folder), "%s", argv[1]);
	arguments[argument_count++] = "halo9x.exe";
	if (!strcmp(scene, "table"))
		return scene_table();
	if (!strcmp(scene, "first-run"))
		return scene_first_run();
	if (!strcmp(scene, "player-file"))
		return scene_player_file();
	if (!strcmp(scene, "broken-file"))
		return scene_broken_file();
	return scene_command_line(scene);
}
