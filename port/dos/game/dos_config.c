/*
DOS_CONFIG.C

The game's settings in the DOS and Windows 95 builds: config_boolean,
config_integer, config_real and config_string (ChupathingyCE's
port/linux/src/port_config.h) answered from the builds' own settings file
(HALODOS.CFG, HALO.CFG: port/dos/xdk/dos_settings.c), where a setting goes
by its full name (display.field_of_view = 80), over the game's defaults
(dos_config_defaults_generated.c, from its settings table). A few of the
game's settings have short names of the builds' own (fps_counter,
field_of_view...), which the menu writes and which come first.
*/

#include "dos_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the platform's (port/dos/xdk/dos_settings.c) */
const char *dos_setting(const char *name, const char *default_value);
void dos_setting_set(const char *name, const char *value);
void dos_settings_save(void);

/* the game's names the builds had before under names of their own */
static const struct
{
	const char *game;
	const char *own;
} aliases[] =
{
	{ "display.fps_counter", "fps_counter" },
	{ "game.player_name", "player_name" },
	{ "display.field_of_view", "field_of_view" },
};

/* a value written as a TOML string ("auto", the game's table's defaults,
or a line copied from its config.toml) without its quotes and escapes; in
one of a few buffers in turn, which last until the caller is done with it */
static const char *unquoted(const char *value)
{
	static char buffers[8][256];
	static int next;
	char *text;
	int length = 0;

	if (value[0] != '"')
		return value;
	text = buffers[next];
	next = (next + 1) % 8;
	for (value++; *value && *value != '"' && length < (int)sizeof(buffers[0]) - 1; value++)
	{
		if (*value == '\\' && value[1])
			value++;
		text[length++] = *value;
	}
	text[length] = 0;
	return text;
}

static const char *default_of(const char *name)
{
	for (int i = 0; i < dos_config_default_count; i++)
	{
		if (!strcmp(dos_config_defaults[i].name, name))
			return unquoted(dos_config_defaults[i].value);
	}
	return "";
}

static const char *value_of(const char *name)
{
	const char *value;

	/* (the short name first: the menu's) */
	for (unsigned i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++)
	{
		if (!strcmp(aliases[i].game, name) && (value = dos_setting(aliases[i].own, NULL)) != NULL)
			return value;
	}
	value = dos_setting(name, NULL);
	if (value)
		return unquoted(value);
	return default_of(name);
}

/* The game's lookups' answers, kept until a setting is set again: some
are asked every frame (internet play's pass, the frame rate's display), and
each was a walk of the settings and the defaults by name. A slot by the
name's address, checked by the name itself. */
unsigned long dos_settings_generation(void);

#define ANSWERS 64

enum
{
	_answer_boolean = 1,
	_answer_integer,
	_answer_real,
};

static struct answer
{
	char name[48];
	unsigned long generation;
	int kind;
	long integer;
	double real;
} answers[ANSWERS];

/* the slot of a name's answer: known if it holds it still; NULL if the name
is too long to keep */
static struct answer *answer_of(const char *name, int kind, int *known)
{
	struct answer *answer = &answers[((unsigned long)name >> 2) % ANSWERS];

	*known = answer->kind == kind && answer->generation == dos_settings_generation() &&
		!strcmp(answer->name, name);
	if (!*known && strlen(name) < sizeof(answer->name))
	{
		strcpy(answer->name, name);
		answer->kind = kind;
		answer->generation = dos_settings_generation();
		return answer;
	}
	return *known ? answer : NULL;
}

int config_boolean(const char *name)
{
	int known;
	struct answer *answer = answer_of(name, _answer_boolean, &known);
	const char *value;

	if (known)
		return (int)answer->integer;
	value = value_of(name);
	known = !_stricmp(value, "true") || !_stricmp(value, "1") || !_stricmp(value, "yes") || !_stricmp(value, "on");
	if (answer)
		answer->integer = known;
	return known;
}

long config_integer(const char *name)
{
	int known;
	struct answer *answer = answer_of(name, _answer_integer, &known);
	long value;

	if (known)
		return answer->integer;
	value = strtol(value_of(name), NULL, 0);
	if (answer)
		answer->integer = value;
	return value;
}

double config_real(const char *name)
{
	int known;
	struct answer *answer = answer_of(name, _answer_real, &known);
	double value;

	if (known)
		return answer->real;
	value = atof(value_of(name));
	if (answer)
		answer->real = value;
	return value;
}

const char *config_string(const char *name)
{
	return value_of(name);
}

int config_text(const char *name, char *text, size_t size)
{
	snprintf(text, size, "%s", value_of(name));
	return 1;
}

int config_default(const char *name, char *text, size_t size)
{
	snprintf(text, size, "%s", default_of(name));
	return 1;
}

int config_set(const char *name, const char *value)
{
	dos_setting_set(name, value);
	return 1;
}

int config_write(const char *name, const char *value)
{
	dos_setting_set(name, value);
	dos_settings_save();
	return 1;
}

int config_write_boolean(const char *name, int value)
{
	return config_write(name, value ? "true" : "false");
}

/* the game's own settings' caches (the field of view's, the far clip's...)
read again when a setting changes: the menu's take effect at once */
unsigned long config_changes(void)
{
	return dos_settings_generation();
}
