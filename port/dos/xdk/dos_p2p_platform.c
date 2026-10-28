/*
DOS_P2P_PLATFORM.C

What ChupathingyCE's internet play (port/linux/src/p2p*.c) asks of its
platform, answered by the DOS and Windows 95 builds:

- this user's secret (the keys its invites are signed with): HALOKEY.BIN
  beside the program, 32 random bytes made the first time;
- the command line's arguments (an invite link passed to the program);
- the settings' folder and a whole file read (the signed tables it keeps);
- none of the desktop's extras: no link handler is registered, there is no
  hardware id, no Discord and no table switching a capability off.
*/

#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../chupathingyce/port/linux/src/posix.h"

/* the command line (dos_system.c, w95_system.c) */
extern int system_argc;
extern char **system_argv;

/* ---------- the settings' folder and files */

void config_folder(char *path, size_t size)
{
	snprintf(path, size, "%s\\", system_program_directory());
}

char *config_file_read(const char *path, size_t *size)
{
	FILE *file = fopen(path, "rb");
	char *text;
	long length;

	if (size)
		*size = 0;
	if (!file)
		return NULL;
	fseek(file, 0, SEEK_END);
	length = ftell(file);
	fseek(file, 0, SEEK_SET);
	if (length < 0 || !(text = malloc((size_t)length + 1)))
	{
		fclose(file);
		return NULL;
	}
	if (fread(text, 1, (size_t)length, file) != (size_t)length)
	{
		free(text);
		fclose(file);
		return NULL;
	}
	fclose(file);
	text[length] = 0;
	if (size)
		*size = (size_t)length;
	return text;
}

/* ---------- the process */

/* the arguments, then an invite link in JOIN.TXT beside the program, as
one more (DOS's command line holds 126 characters: too few for a link
beside the game's other arguments). The file is taken once: it becomes
JOIN.OLD, since an invite works only while its host runs. */
static char join_file_invite[160];
static int join_file_read;

static void read_join_file(void)
{
	char path[300], old_path[300];
	FILE *file;

	join_file_read = 1;
	snprintf(path, sizeof(path), "%s\\JOIN.TXT", system_program_directory());
	if ((file = fopen(path, "r")) == NULL)
		return;
	if (fgets(join_file_invite, sizeof(join_file_invite), file))
	{
		size_t length = strlen(join_file_invite);

		while (length && (join_file_invite[length - 1] == '\n' || join_file_invite[length - 1] == '\r' ||
			join_file_invite[length - 1] == ' '))
		{
			join_file_invite[--length] = 0;
		}
	}
	fclose(file);
	snprintf(old_path, sizeof(old_path), "%s\\JOIN.OLD", system_program_directory());
	remove(old_path);
	rename(path, old_path);
}

int posix_command_line_argument(int index, char *buffer, posix_ulong size)
{
	if (index < 0 || !size)
		return 0;
	if (index < system_argc)
	{
		snprintf(buffer, size, "%s", system_argv[index]);
		return 1;
	}
	if (index > system_argc)
		return 0;
	if (!join_file_read)
		read_join_file();
	if (!join_file_invite[0])
		return 0;
	snprintf(buffer, size, "%s", join_file_invite);
	return 1;
}

int posix_register_url_scheme(const char *scheme, const char *description)
{
	(void)scheme;
	(void)description;
	return 0;
}

int posix_hardware_id_source(char *text, int size)
{
	if (size > 0)
		text[0] = 0;
	return 0;
}

int posix_user_secret(unsigned char *secret, int size)
{
	char path[300];
	FILE *file;

	snprintf(path, sizeof(path), "%s\\HALOKEY.BIN", system_program_directory());
	if ((file = fopen(path, "rb")) != NULL)
	{
		int read = (int)fread(secret, 1, (size_t)size, file);

		fclose(file);
		if (read == size)
			return 1;
	}
	posix_random_bytes(secret, (posix_ulong)size);
	if ((file = fopen(path, "wb")) == NULL)
		return 0;
	fwrite(secret, 1, (size_t)size, file);
	fclose(file);
	return 1;
}

/* ---------- no Discord, no kill switches */

void p2p_discord_sanitize(char *destination, int size, const char *source, int name)
{
	int length = 0;

	if (size <= 0)
		return;
	for (; source && *source && length < size - 1; source++)
	{
		char character = *source;

		if ((character >= '0' && character <= '9') ||
			(name && ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
				character == '_' || character == '.' || character == '-')))
		{
			destination[length++] = character;
		}
	}
	destination[length] = 0;
}

void p2p_discord_user(char *id, int id_size, char *name, int name_size)
{
	if (id_size > 0)
		id[0] = 0;
	if (name_size > 0)
		name[0] = 0;
}

int delta_capability_disabled(int capability)
{
	(void)capability;
	return 0;
}

void p2p_discord_update(void)
{
}

void p2p_discord_set_join(const char *code)
{
	(void)code;
}

/* ---------- no UPnP: the tunnel reaches routers by STUN and punching */

int posix_upnp_forward_udp(unsigned short port, unsigned short preferred_port, posix_ulong *external_address,
	unsigned short *external_port, char *error, int error_size)
{
	(void)port;
	(void)preferred_port;
	(void)external_address;
	(void)external_port;
	if (error_size > 0)
		snprintf(error, (size_t)error_size, "no UPnP in this build");
	return 0;
}

void posix_upnp_stop_forwarding_udp(unsigned short external_port)
{
	(void)external_port;
}
