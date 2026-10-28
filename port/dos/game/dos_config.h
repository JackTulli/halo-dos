/*
DOS_CONFIG.H

The game's settings (ChupathingyCE's config_boolean, config_integer,
config_real, config_string: port/linux/src/port_config.h) in the DOS and
Windows 95 builds: their own settings file (port/dos/xdk/dos_settings.c)
over the game's defaults (dos_config_defaults_generated.c).
*/

#ifndef __DOS_CONFIG_H
#define __DOS_CONFIG_H

enum
{
	DOS_CONFIG_BOOLEAN,
	DOS_CONFIG_INTEGER,
	DOS_CONFIG_REAL,
	DOS_CONFIG_STRING
};

struct dos_config_default
{
	const char *name;
	int type;
	const char *value;
};

extern const struct dos_config_default dos_config_defaults[];
extern const int dos_config_default_count;

#endif
