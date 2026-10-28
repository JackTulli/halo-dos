/*
CE_PROBE.C

Joins a Halo Custom Edition server with ce_session.c and logs what happens:
the handshake, the join, then every message delta the server sends,
decoded (all fields of a few kinds), and a count of each kind at the end.
For working out the protocol against a LOCAL dedicated server only. Its
folder needs MAPS_CE (a link to Custom Edition's maps) for the map's
checksum. A native Windows console program (32-bit: the message layouts hold
32-bit pointers):

    clang --target=i686-pc-windows-msvc ce_probe.c ../ce_session.c ../ce_maps.c ../ce_transport.c
        ../ce_crypto.c ../ce_packets.c ../ce_delta.c ../ce_delta_tables.c ../ce_bitstream.c -I.. -lws2_32
    ce_probe.exe 127.0.0.1 2302 [seconds] [verbose: 1 all fields of every message]
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ce_session.h"

static unsigned long now_ms(void)
{
	return GetTickCount();
}

static struct ce_session session, second;
/* a second machine joining while the first plays: the machine index its
request carries (-1 the default; -2 no second machine) */
static int second_machine = -2;
static long counts[64];
static int verbose;

static void field(void *user, const char *path, const ce_field_props *p, const void *at)
{
	char value[160];

	(void)user;
	if (ce_format_value(p, at, value, sizeof(value)) >= 0)
		printf("      %s = %s\n", path, value);
}

static void message(void *context, int reliable, const ce_message_header *header, int iteration,
	const unsigned char *present, const void *header_fields, const void *body)
{
	int shown;

	(void)context;
	(void)present;
	if (header->type >= 0 && header->type < 64)
		counts[header->type]++;
	shown = verbose || header->type == CE_MSG_PLAYER_CREATE || header->type == CE_MSG_PLAYER_SPAWN ||
		header->type == CE_MSG_BIPED_NEW || header->type == CE_MSG_HUD_CHAT ||
		(header->type == CE_MSG_WEAPON_NEW && counts[header->type] <= 2);
	if (counts[header->type] > 3 && !shown)
		return;
	printf("[%lu] %s %s%s (%s)\n", now_ms(), reliable ? "reliable" : "unreliable", header->def->name,
		header->iterations > 1 ? " iteration" : "", header->mode ? "incremental" : "stateless");
	if (shown)
	{
		if (header->def->header_count)
			ce_delta_visit(header->def, 0, header_fields, field, NULL);
		ce_delta_visit(header->def, 1, body, field, NULL);
	}
	(void)iteration;
	fflush(stdout);
}

static void logged(void *context, const char *line)
{
	printf("[%lu] %s%s\n", now_ms(), context ? "SECOND " : "", line);
	fflush(stdout);
}

int main(int argc, char **argv)
{
	WSADATA wsa;
	unsigned long seconds, start;
	int state = -1, i;

	if (argc < 3)
	{
		printf("ce_probe <address> <port> [seconds] [verbose]\n");
		return 2;
	}
	WSAStartup(MAKEWORD(2, 2), &wsa);
	seconds = argc > 3 ? strtoul(argv[3], NULL, 0) : 20;
	verbose = argc > 4 && atoi(argv[4]);
	if (argc > 5)
		second_machine = atoi(argv[5]);
	start = now_ms();
	ce_session_begin(&session, inet_addr(argv[1]), (uint16_t)atoi(argv[2]), "probe", "",
		(uint32_t)start * 2654435761u, NULL, message, logged, NULL, start);
	while (now_ms() - start < seconds * 1000)
	{
		ce_session_poll(&session, now_ms());
		if (session.state != state)
		{
			state = session.state;
			printf("[%lu] session state %d %s\n", now_ms(), state, session.error);
			fflush(stdout);
			/* (no map to load here: say so at once) */
			if (state == _ce_session_joined)
				ce_session_map_loaded(&session, now_ms());
		}
		if (session.state == _ce_session_failed || session.state == _ce_session_ended)
			break;
		if (second_machine >= -1)
		{
			static unsigned long playing_since;
			static int second_state = -1;

			if (session.state == _ce_session_playing && !playing_since)
				playing_since = now_ms();
			if (playing_since && second.state == _ce_session_off && now_ms() - playing_since > 3000)
			{
				ce_session_begin(&second, inet_addr(argv[1]), (uint16_t)atoi(argv[2]), "second", "",
					(uint32_t)now_ms() * 2246822519u, NULL, NULL, logged, (void *)1, now_ms());
				second.machine_index = second_machine;
			}
			if (second.state != _ce_session_off)
			{
				ce_session_poll(&second, now_ms());
				if (second.state != second_state)
				{
					second_state = second.state;
					printf("[%lu] SECOND session state %d %s\n", now_ms(), second_state, second.error);
					if (second_state == _ce_session_joined)
						ce_session_map_loaded(&second, now_ms());
				}
			}
		}
		Sleep(5);
	}
	printf("datagrams in %ld out %ld, deltas %ld (errors %ld), bad CRCs %ld, resends %ld\n",
		session.link.datagrams_in, session.link.datagrams_out, session.deltas_received, session.delta_errors,
		session.link.bad_crc, session.link.resends);
	for (i = 0; i < 64; i++)
		if (counts[i])
			printf("  %-40s %ld\n", ce_delta_message(i)->name, counts[i]);
	ce_session_end(&session, now_ms());
	WSACleanup();
	return 0;
}
