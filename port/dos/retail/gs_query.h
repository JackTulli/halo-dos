/*
GS_QUERY.H

Asking retail Halo PC / Custom Edition servers what they are playing, as
the games' lobbies do: GameSpy's query (FE FD 00, an id, the server's keys
wanted), answered with the keys and values -- the name, map, game type,
players, version, password -- and timed for the ping. Many servers from one
socket, a few sent at a time; driven by polling.
*/

#ifndef __GS_QUERY_H
#define __GS_QUERY_H

#include <stdint.h>

#include "retail_socket.h"

enum
{
	_gs_query_waiting, /* not asked yet */
	_gs_query_asked,
	_gs_query_answered,
	_gs_query_silent /* asked twice, no answer */
};

struct gs_query_server
{
	uint32_t address; /* network order */
	uint16_t port;
	int state;
	int kind; /* the caller's (which list the server came from) */
	unsigned long sent_ms;
	int tries, ping_ms;
	char name[64], map[32], game_type[32], variant[32], version[24];
	int players, maximum_players, password, dedicated;
};

struct gs_query
{
	retail_socket_t socket;
	struct gs_query_server *servers;
	int count, next, answered;
	uint32_t id;
};

/* starts asking servers[0..count) (address, port and kind filled in) */
int gs_query_begin(struct gs_query *query, struct gs_query_server *servers, int count, uint32_t seed);
/* sends the next few questions and takes the answers that came: 1 when
every server has answered or fallen silent */
int gs_query_poll(struct gs_query *query, unsigned long now_ms);
void gs_query_end(struct gs_query *query);

#endif
