/*
GS_LIST.H

A GameSpy master server's list of a game's servers, as Halo PC's and Custom
Edition's lobbies ask for it from the master server that replaced
GameSpy's (Bungie's 1.0.10 patch: s1.ms01.hosthpc.com, TCP 28910): the
server-browser request, its answer decrypted (Luigi Auriemma's enctypeX,
enctypex_decoder.c) and read as addresses. Driven by polling: it never
blocks.
*/

#ifndef __GS_LIST_H
#define __GS_LIST_H

#include <stdint.h>

#include "retail_socket.h"

/* the lists Halo's lobbies ask for: the retail game's and Custom
Edition's, both with Custom Edition's key */
#define GS_LIST_RETAIL_PC "halor"
#define GS_LIST_CUSTOM_EDITION "halom"
#define GS_LIST_HALO_KEY "e4Rd9J"
#define GS_LIST_MASTER_PORT 28910
/* how long a list may take, connection and all */
#define GS_LIST_TIMEOUT_MS 10000

struct gs_list_server
{
	uint32_t address; /* network order */
	uint16_t port; /* host order: the server's query port, its game port too */
};

enum
{
	_gs_list_idle,
	_gs_list_connecting,
	_gs_list_receiving,
	_gs_list_done,
	_gs_list_failed
};

struct gs_list
{
	int state;
	retail_socket_t socket;
	unsigned long started_ms;
	char game_key[16];
	unsigned char validate[9];
	unsigned char *buffer;
	int length, capacity;
	/* the stream decoder's state (enctypex_data_t) and how much of the
	decrypted list has been read */
	unsigned char decoder[272];
	int parsed;
	/* the list's own header: the fields and their popular values */
	int header_read, field_count, field_types[32];
	struct gs_list_server *servers;
	int count, maximum;
	char error[80];
};

/* starts asking master (an IPv4 address, network order) for for_game's
servers, as from_game (whose key is game_key); seed: random bits for the
request's challenge. Room for maximum servers. 0 when it could not start */
int gs_list_begin(struct gs_list *list, uint32_t master, uint16_t port, const char *for_game,
	const char *from_game, const char *game_key, uint32_t seed, struct gs_list_server *servers, int maximum,
	unsigned long now_ms);
/* its next step: _gs_list_done or _gs_list_failed when finished (the list
in servers/count, the reason in error) */
int gs_list_poll(struct gs_list *list, unsigned long now_ms);
void gs_list_end(struct gs_list *list);

#endif
