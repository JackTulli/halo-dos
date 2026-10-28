/*
CE_MAPS.C

Where the game finds Custom Edition maps for joining CE servers: the
folder the setting ce_maps names (MAPS_CE beside the program by default),
and a map's checksum from its header (offset 0x64, as Custom Edition's
tools write it; bloodgulch's is 7B309554), which the join request has to
show (ce_session.c).
*/

#include <stdio.h>
#include <string.h>

#include "ce_session.h"

#ifdef HALO_DOS
const char *dos_setting(const char *name, const char *default_value);
#define CE_MAPS_FOLDER() dos_setting("ce_maps", "MAPS_CE")
#else
#define CE_MAPS_FOLDER() "MAPS_CE"
#endif

/* the header's offset of the checksum */
#define CE_MAP_CHECKSUM_OFFSET 0x64
/* the cache file version of Custom Edition's maps */
#define CE_MAP_VERSION 609

int ce_session_map_checksum(const char *map_name, uint32_t *checksum)
{
	char path[300];
	unsigned char header[CE_MAP_CHECKSUM_OFFSET + 4];
	FILE *file;
	int ok = 0;

	if (!map_name[0] || strchr(map_name, '/') || strchr(map_name, '\\') || strstr(map_name, ".."))
		return -1;
	snprintf(path, sizeof(path), "%s\\%s.map", CE_MAPS_FOLDER(), map_name);
	file = fopen(path, "rb");
	if (!file)
		return -1;
	if (fread(header, 1, sizeof(header), file) == sizeof(header) && memcmp(header, "daeh", 4) == 0 &&
		(header[4] | header[5] << 8) == CE_MAP_VERSION)
	{
		*checksum = (uint32_t)header[CE_MAP_CHECKSUM_OFFSET] | (uint32_t)header[CE_MAP_CHECKSUM_OFFSET + 1] << 8 |
			(uint32_t)header[CE_MAP_CHECKSUM_OFFSET + 2] << 16 | (uint32_t)header[CE_MAP_CHECKSUM_OFFSET + 3] << 24;
		ok = 1;
	}
	fclose(file);
	return ok ? 0 : -1;
}
