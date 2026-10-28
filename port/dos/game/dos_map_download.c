/*
DOS_MAP_DOWNLOAD.C

A Custom Edition map a server plays that is not in MAPS_CE, downloaded as
Chimera downloads it (its map_loading.cpp and map_downloader.cpp): from the
HAC2/HaloNet repository Chimera uses, by default

	http://maps.halonet.net/halonet/locator.php?format=inv&map={map}&type=halom

(the setting map_download_url: {map} the map's name, {game} halom), the
answer the map itself. The repository sends maps compressed by Invader:
the 2048-byte header as it is but for its engine version (0x861A0261 for
Custom Edition's 609) and the rest one zstd stream; a map sent plain is kept
as it comes. The download goes to MAPS_CE\<map>.dl, is unpacked into
<map>.tmp (its engine version 609 again), and becomes <map>.map once all of
it is there, so a map half done is never taken for one.

Polled from the game's frame (dos_ce_client.c), never waiting: the
download a few reads a frame (http_get.c), the unpacking for a few
hundredths of a second a frame, so the page showing it keeps moving.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "halo_system.h"
#include "dos_map_download.h"
#include "../retail/http_get.h"
#include "../third_party/zstd/lib/zstd.h"

const char *dos_setting(const char *name, const char *default_value);

#define MAP_HEADER_SIZE 0x800
#define MAP_HEAD 0x68656164UL /* "head" read as a little-endian long: "daeh" in the file */
#define MAP_FOOT 0x666F6F74UL
#define ENGINE_CUSTOM_EDITION 609UL
#define ENGINE_CUSTOM_EDITION_COMPRESSED 0x861A0261UL
/* the unpacking's share of a frame */
#define UNPACK_SLICE_US 30000ULL
/* the answer time's window for the speed shown */
#define SPEED_WINDOW_MS 2000UL

static struct
{
	int state;
	char map[64];
	char folder[128];
	struct http_get get;
	FILE *file;
	unsigned char header[MAP_HEADER_SIZE];
	int header_size;
	unsigned long started_ms;
	/* the speed: a sample of (time, received) every half second */
	unsigned long sample_ms[8];
	long sample_received[8];
	int sample_count;
	long bytes_per_second;
	/* the unpacking */
	ZSTD_DStream *stream;
	FILE *input, *output;
	unsigned char *in_buffer, *out_buffer;
	size_t in_size, in_used, in_filled, out_size;
	long unpacked, unpacked_total;
	char error[96];
} download;

static unsigned long now_ms(void)
{
	return (unsigned long)(system_microseconds() / 1000);
}

static unsigned long get_long(const unsigned char *at)
{
	return (unsigned long)at[0] | (unsigned long)at[1] << 8 | (unsigned long)at[2] << 16 | (unsigned long)at[3] << 24;
}

static void put_long(unsigned char *at, unsigned long value)
{
	at[0] = (unsigned char)value;
	at[1] = (unsigned char)(value >> 8);
	at[2] = (unsigned char)(value >> 16);
	at[3] = (unsigned char)(value >> 24);
}

static void path_of(char *path, int size, const char *extension)
{
	snprintf(path, (size_t)size, "%s\\%s.%s", download.folder, download.map, extension);
}

/* everything open closed, the files half done removed */
static void clean_up(int remove_files)
{
	char path[300];

	if (download.get.state != _http_get_off)
		http_get_cancel(&download.get);
	if (download.file)
		fclose(download.file);
	if (download.input)
		fclose(download.input);
	if (download.output)
		fclose(download.output);
	download.file = download.input = download.output = NULL;
	if (download.stream)
		ZSTD_freeDStream(download.stream);
	download.stream = NULL;
	(free)(download.in_buffer);
	(free)(download.out_buffer);
	download.in_buffer = download.out_buffer = NULL;
	if (remove_files)
	{
		path_of(path, sizeof(path), "dl");
		remove(path);
		path_of(path, sizeof(path), "tmp");
		remove(path);
	}
}

static void fail(const char *reason)
{
	char line[160];

	snprintf(download.error, sizeof(download.error), "%s", reason);
	snprintf(line, sizeof(line), "map download: %s: %s", download.map, reason);
	system_log(line);
	clean_up(1);
	download.state = DOS_MAP_DOWNLOAD_FAILED;
}

/* the body as it comes: its first 2048 bytes must be a map's header (as
Chimera checks), then all of it to the .dl file */
static int write_body(void *context, const unsigned char *data, int size)
{
	(void)context;
	if (download.header_size < MAP_HEADER_SIZE)
	{
		int take = size < MAP_HEADER_SIZE - download.header_size ? size : MAP_HEADER_SIZE - download.header_size;

		memcpy(download.header + download.header_size, data, (size_t)take);
		download.header_size += take;
		if (download.header_size == MAP_HEADER_SIZE &&
			(get_long(download.header) != MAP_HEAD || get_long(download.header + 0x7FC) != MAP_FOOT))
		{
			snprintf(download.get.error, sizeof(download.get.error), "the repository sent something that is not a map");
			return 1;
		}
	}
	if (fwrite(data, 1, (size_t)size, download.file) != (size_t)size)
	{
		snprintf(download.get.error, sizeof(download.get.error), "the map could not be written (is the disk full?)");
		return 1;
	}
	return 0;
}

/* the repository's address for the map: the template's {map} the map's
name, its letters made small and the rest of its characters escaped */
static void map_url(char *url, int size)
{
	const char *template_url = dos_setting("map_download_url",
		"http://maps.halonet.net/halonet/locator.php?format=inv&map={map}&type={game}");
	int at = 0;

	for (const char *c = template_url; *c && at < size - 1; c++)
	{
		if (!strncmp(c, "{map}", 5))
		{
			for (const char *m = download.map; *m && at < size - 4; m++)
			{
				int ch = *m >= 'A' && *m <= 'Z' ? *m - 'A' + 'a' : (unsigned char)*m;

				if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.')
					url[at++] = (char)ch;
				else
					at += snprintf(url + at, (size_t)(size - at), "%%%02X", ch);
			}
			c += 4;
		}
		else if (!strncmp(c, "{game}", 6))
		{
			at += snprintf(url + at, (size_t)(size - at), "halom");
			c += 5;
		}
		else
			url[at++] = *c;
	}
	url[at < size ? at : size - 1] = 0;
}

int dos_map_download_begin(const char *map)
{
	char path[300], url[600], line[700];

	dos_map_download_cancel();
	memset(&download, 0, sizeof(download));
	if (!map[0] || strchr(map, '\\') || strchr(map, '/') || strstr(map, "..") || strlen(map) >= sizeof(download.map))
	{
		snprintf(download.error, sizeof(download.error), "the server's map has no name a file can have");
		download.state = DOS_MAP_DOWNLOAD_FAILED;
		return 0;
	}
	snprintf(download.map, sizeof(download.map), "%s", map);
	snprintf(download.folder, sizeof(download.folder), "%s", dos_setting("ce_maps", "MAPS_CE"));
	path_of(path, sizeof(path), "dl");
	download.file = fopen(path, "wb");
	if (!download.file)
	{
		fail("the map could not be written in the MAPS_CE folder");
		return 0;
	}
	map_url(url, sizeof(url));
	snprintf(line, sizeof(line), "map download: %s from %s", map, url);
	system_log(line);
	download.started_ms = now_ms();
	if (http_get_begin(&download.get, url, "Chimera MapDownloader/1.0", download.started_ms) < 0)
	{
		fail(download.get.error);
		return 0;
	}
	download.state = DOS_MAP_DOWNLOAD_CONNECTING;
	return 1;
}

void dos_map_download_cancel(void)
{
	if (download.state == DOS_MAP_DOWNLOAD_CONNECTING || download.state == DOS_MAP_DOWNLOAD_DOWNLOADING ||
		download.state == DOS_MAP_DOWNLOAD_UNPACKING)
	{
		system_log("map download: given up");
		clean_up(1);
	}
	download.state = DOS_MAP_DOWNLOAD_OFF;
}

/* the speed over the last two seconds */
static void sample_speed(unsigned long now)
{
	int last = download.sample_count - 1;

	if (download.sample_count && now - download.sample_ms[last] < 500)
		return;
	if (download.sample_count == (int)(sizeof(download.sample_ms) / sizeof(download.sample_ms[0])))
	{
		memmove(download.sample_ms, download.sample_ms + 1, sizeof(download.sample_ms) - sizeof(download.sample_ms[0]));
		memmove(download.sample_received, download.sample_received + 1,
			sizeof(download.sample_received) - sizeof(download.sample_received[0]));
		download.sample_count--;
	}
	download.sample_ms[download.sample_count] = now;
	download.sample_received[download.sample_count] = download.get.received;
	download.sample_count++;
	for (int i = 0; i < download.sample_count - 1; i++)
	{
		if (now - download.sample_ms[i] <= SPEED_WINDOW_MS)
		{
			unsigned long ms = now - download.sample_ms[i];

			download.bytes_per_second = ms ? (long)((download.get.received - download.sample_received[i]) * 1000LL /
				(long long)ms) : 0;
			break;
		}
	}
}

/* the download complete: a plain map renamed into place, a compressed one
set up to be unpacked */
static void downloaded(void)
{
	char from[300], to[300];
	unsigned long engine = get_long(download.header + 4);

	fclose(download.file);
	download.file = NULL;
	if (download.header_size < MAP_HEADER_SIZE)
	{
		fail("the repository sent too little to be a map");
		return;
	}
	if (engine == ENGINE_CUSTOM_EDITION)
	{
		path_of(from, sizeof(from), "dl");
		path_of(to, sizeof(to), "map");
		remove(to);
		if (rename(from, to))
		{
			fail("the map could not be put in the MAPS_CE folder");
			return;
		}
		download.state = DOS_MAP_DOWNLOAD_DONE;
		system_log("map download: done (a plain map)");
		return;
	}
	if (engine != ENGINE_CUSTOM_EDITION_COMPRESSED)
	{
		fail("the repository's map is not a Custom Edition map");
		return;
	}
	/* (the unpacked size the header holds: the whole map's, its header's too) */
	download.unpacked_total = (long)get_long(download.header + 8) - MAP_HEADER_SIZE;
	path_of(from, sizeof(from), "dl");
	path_of(to, sizeof(to), "tmp");
	download.input = fopen(from, "rb");
	download.output = fopen(to, "wb");
	download.stream = ZSTD_createDStream();
	download.in_size = ZSTD_DStreamInSize();
	download.out_size = ZSTD_DStreamOutSize();
	/* (the C library's own: the game's malloc is its debug allocator) */
	download.in_buffer = (unsigned char *)(malloc)(download.in_size);
	download.out_buffer = (unsigned char *)(malloc)(download.out_size);
	if (!download.input || !download.output || !download.stream || !download.in_buffer || !download.out_buffer ||
		ZSTD_isError(ZSTD_initDStream(download.stream)) || fseek(download.input, MAP_HEADER_SIZE, SEEK_SET))
	{
		fail("the map could not be unpacked (no room?)");
		return;
	}
	/* (the header as Custom Edition's own: its engine version 609) */
	put_long(download.header + 4, ENGINE_CUSTOM_EDITION);
	if (fwrite(download.header, 1, MAP_HEADER_SIZE, download.output) != MAP_HEADER_SIZE)
	{
		fail("the map could not be written (is the disk full?)");
		return;
	}
	download.in_used = download.in_filled = 0;
	download.unpacked = 0;
	download.state = DOS_MAP_DOWNLOAD_UNPACKING;
	system_log("map download: downloaded, unpacking");
}

/* a frame's share of the unpacking; the map put in place at its end */
static void unpack(void)
{
	unsigned long long until = system_microseconds() + UNPACK_SLICE_US;

	do
	{
		ZSTD_inBuffer input;
		ZSTD_outBuffer output;
		size_t result;

		if (download.in_used == download.in_filled)
		{
			download.in_filled = fread(download.in_buffer, 1, download.in_size, download.input);
			download.in_used = 0;
			if (!download.in_filled)
			{
				char from[300], to[300];

				/* (all of it read: all of it unpacked?) */
				if (download.unpacked_total > 0 && download.unpacked != download.unpacked_total)
				{
					fail("the downloaded map is cut short");
					return;
				}
				fclose(download.output);
				fclose(download.input);
				download.output = download.input = NULL;
				ZSTD_freeDStream(download.stream);
				download.stream = NULL;
				path_of(from, sizeof(from), "tmp");
				path_of(to, sizeof(to), "map");
				remove(to);
				if (rename(from, to))
				{
					fail("the map could not be put in the MAPS_CE folder");
					return;
				}
				path_of(from, sizeof(from), "dl");
				remove(from);
				(free)(download.in_buffer);
				(free)(download.out_buffer);
				download.in_buffer = download.out_buffer = NULL;
				download.state = DOS_MAP_DOWNLOAD_DONE;
				system_log("map download: done");
				return;
			}
		}
		input.src = download.in_buffer + download.in_used;
		input.size = download.in_filled - download.in_used;
		input.pos = 0;
		output.dst = download.out_buffer;
		output.size = download.out_size;
		output.pos = 0;
		result = ZSTD_decompressStream(download.stream, &output, &input);
		if (ZSTD_isError(result))
		{
			char reason[96];

			snprintf(reason, sizeof(reason), "the map could not be unpacked (%s)", ZSTD_getErrorName(result));
			fail(reason);
			return;
		}
		download.in_used += input.pos;
		if (output.pos)
		{
			if (fwrite(download.out_buffer, 1, output.pos, download.output) != output.pos)
			{
				fail("the map could not be written (is the disk full?)");
				return;
			}
			download.unpacked += (long)output.pos;
		}
	} while (system_microseconds() < until);
}

void dos_map_download_poll(void)
{
	unsigned long now = now_ms();

	switch (download.state)
	{
	case DOS_MAP_DOWNLOAD_CONNECTING:
	case DOS_MAP_DOWNLOAD_DOWNLOADING:
		switch (http_get_poll(&download.get, now, write_body, NULL))
		{
		case _http_get_body:
			download.state = DOS_MAP_DOWNLOAD_DOWNLOADING;
			sample_speed(now);
			break;
		case _http_get_done:
			sample_speed(now);
			downloaded();
			break;
		case _http_get_failed:
			fail(download.get.status == 404 ? "the map repository does not have this map" : download.get.error);
			break;
		}
		break;
	case DOS_MAP_DOWNLOAD_UNPACKING:
		unpack();
		break;
	}
}

void dos_map_download_status(struct dos_map_download_status *status)
{
	memset(status, 0, sizeof(*status));
	status->state = download.state;
	snprintf(status->map, sizeof(status->map), "%s", download.map);
	status->received = download.get.received;
	status->total = download.get.content_length;
	status->bytes_per_second = download.bytes_per_second;
	status->unpacked = download.unpacked;
	status->unpacked_total = download.unpacked_total;
	snprintf(status->host, sizeof(status->host), "%s", download.get.host);
	snprintf(status->error, sizeof(status->error), "%s", download.error);
}
