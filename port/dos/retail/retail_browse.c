/*
RETAIL_BROWSE.C

The retail Halo PC and Custom Edition servers for ONLINE PLAY's list
(retail_browse.h): the master server's name looked up, the retail game's
list, Custom Edition's, then every server asked (gs_query.c), the answers
kept until the next round a minute later.
*/

#include "retail_browse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gs_list.h"
#include "gs_query.h"

/* the master server Bungie's 1.0.10 patch moved the games' lobbies to */
#define RETAIL_MASTER "s1.ms01.hosthpc.com"
#define RETAIL_MAXIMUM_SERVERS 384
/* a new round of lists and questions this long after the last began */
#define RETAIL_REFRESH_MS 60000
/* the master server's name looked up for this long before giving up */
#define RETAIL_RESOLVE_MS 6000
/* questions answered for this long at most */
#define RETAIL_QUERY_MS 8000

enum
{
	_browse_off,
	_browse_resolving,
	_browse_listing, /* list[list_kind] */
	_browse_asking,
	_browse_resting /* until the next round */
};

/* whether Halo PC's list is looked at too (off: Custom Edition's alone) */
static int browse_include_pc;

void retail_browse_include_pc(int include)
{
	browse_include_pc = include;
}

static struct
{
	int state;
	unsigned long round_ms, step_ms;
	uint32_t seed, master;
	int list_kind;
	struct gs_list list;
	struct gs_list_server listed[RETAIL_MAXIMUM_SERVERS];
	struct gs_query query;
	/* this round's servers, and the last round's answers shown meanwhile */
	struct gs_query_server asking[RETAIL_MAXIMUM_SERVERS];
	int asking_count;
	struct gs_query_server shown[RETAIL_MAXIMUM_SERVERS];
	int shown_count;
	char status[80];
} browse;

static uint32_t next_seed(void)
{
	browse.seed = browse.seed * 0x343FD + 0x269EC3;
	return browse.seed;
}

static void set_status(const char *status)
{
	snprintf(browse.status, sizeof(browse.status), "%s", status);
}

static void begin_list(unsigned long now_ms)
{
	static const char *const games[2] = { GS_LIST_CUSTOM_EDITION, GS_LIST_RETAIL_PC };

	if (!gs_list_begin(&browse.list, browse.master, GS_LIST_MASTER_PORT, games[browse.list_kind],
		GS_LIST_CUSTOM_EDITION, GS_LIST_HALO_KEY, next_seed(), browse.listed, RETAIL_MAXIMUM_SERVERS, now_ms))
	{
		gs_list_end(&browse.list);
		set_status("CE SERVERS: NO NETWORK");
		browse.state = _browse_resting;
	}
}

void retail_browse_start(unsigned long now_ms, uint32_t seed)
{
	retail_browse_stop();
	browse.seed = seed ^ (uint32_t)now_ms;
	browse.state = _browse_resolving;
	browse.round_ms = browse.step_ms = now_ms;
	set_status("CE SERVERS: ASKING THE MASTER SERVER");
}

void retail_browse_stop(void)
{
	if (browse.state == _browse_listing)
		gs_list_end(&browse.list);
	if (browse.state == _browse_asking)
		gs_query_end(&browse.query);
	browse.state = _browse_off;
}

void retail_browse_update(unsigned long now_ms)
{
	switch (browse.state)
	{
	case _browse_resolving:
		browse.master = retail_resolve(RETAIL_MASTER);
		if (browse.master)
		{
			browse.asking_count = 0;
			browse.list_kind = _retail_custom_edition;
			browse.state = _browse_listing;
			begin_list(now_ms);
		}
		else if (now_ms - browse.step_ms > RETAIL_RESOLVE_MS)
		{
			set_status("CE SERVERS: THE MASTER SERVER WAS NOT FOUND");
			browse.state = _browse_resting;
		}
		break;
	case _browse_listing:
	{
		int state = gs_list_poll(&browse.list, now_ms);
		int i;

		if (state != _gs_list_done && state != _gs_list_failed)
			break;
		for (i = 0; state == _gs_list_done && i < browse.list.count && browse.asking_count < RETAIL_MAXIMUM_SERVERS; i++)
		{
			struct gs_query_server *server = &browse.asking[browse.asking_count++];

			memset(server, 0, sizeof(*server));
			server->address = browse.listed[i].address;
			server->port = browse.listed[i].port;
			server->kind = browse.list_kind;
		}
		gs_list_end(&browse.list);
		/* (Halo PC's list only when asked for: retail_browse_include_pc) */
		if (browse.list_kind == _retail_custom_edition && browse_include_pc)
		{
			browse.list_kind = _retail_pc;
			begin_list(now_ms);
		}
		else if (browse.asking_count && gs_query_begin(&browse.query, browse.asking, browse.asking_count, next_seed()))
		{
			browse.state = _browse_asking;
			browse.step_ms = now_ms;
		}
		else
		{
			set_status(browse.asking_count ? "CE SERVERS: NO NETWORK" :
				"CE SERVERS: THE MASTER SERVER DID NOT ANSWER");
			browse.state = _browse_resting;
		}
		break;
	}
	case _browse_asking:
		if (gs_query_poll(&browse.query, now_ms) || now_ms - browse.step_ms > RETAIL_QUERY_MS)
		{
			int i;

			gs_query_end(&browse.query);
			browse.shown_count = 0;
			for (i = 0; i < browse.asking_count; i++)
			{
				if (browse.asking[i].state == _gs_query_answered)
					browse.shown[browse.shown_count++] = browse.asking[i];
			}
			snprintf(browse.status, sizeof(browse.status), "CE SERVERS: %d ANSWERED", browse.shown_count);
			browse.state = _browse_resting;
		}
		break;
	case _browse_resting:
		if (now_ms - browse.round_ms > RETAIL_REFRESH_MS)
		{
			browse.round_ms = browse.step_ms = now_ms;
			browse.state = _browse_resolving;
		}
		break;
	}
}

static int by_players_then_ping(const void *a, const void *b)
{
	const struct gs_query_server *x = a, *y = b;

	if ((x->players > 0) != (y->players > 0))
		return (y->players > 0) - (x->players > 0);
	if (x->players != y->players)
		return y->players - x->players;
	return x->ping_ms - y->ping_ms;
}

int retail_browse_servers(struct retail_browse_server *servers, int maximum)
{
	static struct gs_query_server sorted[RETAIL_MAXIMUM_SERVERS];
	int i, count = browse.shown_count;

	memcpy(sorted, browse.shown, count * sizeof(sorted[0]));
	qsort(sorted, count, sizeof(sorted[0]), by_players_then_ping);
	if (count > maximum)
		count = maximum;
	for (i = 0; i < count; i++)
	{
		struct retail_browse_server *server = &servers[i];

		server->kind = sorted[i].kind;
		snprintf(server->name, sizeof(server->name), "%s", sorted[i].name);
		snprintf(server->map, sizeof(server->map), "%s", sorted[i].map);
		snprintf(server->game_type, sizeof(server->game_type), "%s",
			sorted[i].variant[0] ? sorted[i].variant : sorted[i].game_type);
		snprintf(server->version, sizeof(server->version), "%s", sorted[i].version);
		server->players = sorted[i].players;
		server->maximum_players = sorted[i].maximum_players;
		server->password = sorted[i].password;
		server->ping_ms = sorted[i].ping_ms;
		server->address = sorted[i].address;
		server->port = sorted[i].port;
	}
	return count;
}

const char *retail_browse_status(void)
{
	return browse.state == _browse_off ? "" : browse.status;
}
