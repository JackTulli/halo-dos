/*
GS_QUERY.C

Asking retail Halo PC / Custom Edition servers what they are playing
(gs_query.h). The question: FE FD 00, a 4-byte id, then FF 00 00 (every
server key, no players, no teams). The answer: 00, the id, then key and
value strings (each ending in a 0) until an empty key.
*/

#include "gs_query.h"

#include <stdlib.h>
#include <string.h>

/* questions sent a poll, and how long an answer is waited for (asked twice) */
#define QUERIES_PER_POLL 8
#define QUERY_WAIT_MS 1500
#define QUERY_TRIES 2

int gs_query_begin(struct gs_query *query, struct gs_query_server *servers, int count, uint32_t seed)
{
	int i;

	memset(query, 0, sizeof(*query));
	query->servers = servers;
	query->count = count;
	query->id = seed | 1;
	for (i = 0; i < count; i++)
	{
		servers[i].state = _gs_query_waiting;
		servers[i].tries = 0;
		servers[i].ping_ms = -1;
	}
	query->socket = retail_socket_open(0);
	return query->socket != RETAIL_NO_SOCKET;
}

static void copy_value(char *destination, int size, const char *value)
{
	int i;

	/* (printable characters only: some servers colour their names; and
	without the spaces that leaves at either end) */
	for (i = 0; *value && i < size - 1; value++)
	{
		if ((unsigned char)*value > 0x20 && (unsigned char)*value < 0x7F)
			destination[i++] = *value;
		else if (*value == ' ' && i && destination[i - 1] != ' ')
			destination[i++] = ' ';
	}
	while (i && destination[i - 1] == ' ')
		i--;
	destination[i] = 0;
}

static void take_answer(struct gs_query_server *server, const unsigned char *data, int size, unsigned long now_ms)
{
	const char *p = (const char *)data + 5, *end = (const char *)data + size;

	while (p < end && *p)
	{
		const char *key = p, *value;

		while (p < end && *p)
			p++;
		if (++p >= end)
			break;
		value = p;
		while (p < end && *p)
			p++;
		if (p >= end)
			break;
		p++;
		if (!strcmp(key, "hostname"))
			copy_value(server->name, sizeof(server->name), value);
		else if (!strcmp(key, "mapname"))
			copy_value(server->map, sizeof(server->map), value);
		else if (!strcmp(key, "gametype"))
			copy_value(server->game_type, sizeof(server->game_type), value);
		else if (!strcmp(key, "gamevariant"))
			copy_value(server->variant, sizeof(server->variant), value);
		else if (!strcmp(key, "gamever"))
			copy_value(server->version, sizeof(server->version), value);
		else if (!strcmp(key, "numplayers"))
			server->players = atoi(value);
		else if (!strcmp(key, "maxplayers"))
			server->maximum_players = atoi(value);
		else if (!strcmp(key, "password"))
			server->password = atoi(value) != 0;
		else if (!strcmp(key, "dedicated"))
			server->dedicated = atoi(value) != 0;
	}
	server->ping_ms = (int)(now_ms - server->sent_ms);
	server->state = _gs_query_answered;
}

int gs_query_poll(struct gs_query *query, unsigned long now_ms)
{
	unsigned char packet[1400];
	int i, sent = 0, pending = 0;

	for (;;)
	{
		uint32_t from_address;
		uint16_t from_port;
		int got = retail_socket_receive_from(query->socket, packet, sizeof(packet), &from_address, &from_port);

		if (got < 0)
			break;
		if (got < 5 || packet[0] != 0)
			continue;
		for (i = 0; i < query->count; i++)
		{
			struct gs_query_server *server = &query->servers[i];

			if (server->state == _gs_query_asked && server->address == from_address &&
				server->port == from_port && !memcmp(packet + 1, &query->id, 4))
			{
				take_answer(server, packet, got, now_ms);
				query->answered++;
				break;
			}
		}
	}
	for (i = 0; i < query->count; i++)
	{
		struct gs_query_server *server = &query->servers[i];

		if (server->state == _gs_query_asked && now_ms - server->sent_ms >= QUERY_WAIT_MS)
			server->state = server->tries < QUERY_TRIES ? _gs_query_waiting : _gs_query_silent;
		if (server->state == _gs_query_waiting && sent < QUERIES_PER_POLL)
		{
			unsigned char question[10] = { 0xFE, 0xFD, 0x00, 0, 0, 0, 0, 0xFF, 0x00, 0x00 };

			memcpy(question + 3, &query->id, 4);
			retail_socket_send_to(query->socket, question, sizeof(question), server->address, server->port);
			server->sent_ms = now_ms;
			server->tries++;
			server->state = _gs_query_asked;
			sent++;
		}
		if (server->state == _gs_query_waiting || server->state == _gs_query_asked)
			pending++;
	}
	return !pending;
}

void gs_query_end(struct gs_query *query)
{
	retail_socket_close(query->socket);
	query->socket = RETAIL_NO_SOCKET;
}
