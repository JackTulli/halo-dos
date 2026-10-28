/*
CE_SESSION.H

Joining a Halo Custom Edition server (ce_session.c): the connection
(ce_transport.c), the join conversation (ce_packets.c) and, once in, the
message deltas both ways (ce_delta.c), decoded for the game (ce_client.c)
or a test. Non-blocking: ce_session_poll() from the game's frame.

The order: connecting -> joining (the server's challenge answered with our
join request) -> joined: the server has taken us and named its map; the
game loads that map and calls ce_session_map_loaded() -> playing: deltas
flow until either side ends it.
*/

#ifndef __CE_SESSION_H
#define __CE_SESSION_H

#include <stdint.h>

#include "ce_delta.h"
#include "ce_transport.h"

/* the game version a 01.00.10.0621 server accepts (and no other) */
#define CE_VERSION_1_10 616999

enum ce_session_state
{
	_ce_session_off,
	_ce_session_connecting,
	_ce_session_joining,
	_ce_session_joined,
	_ce_session_playing,
	_ce_session_ended, /* error[] says why */
	_ce_session_failed /* error[] says why */
};

/* the baseline an incremental message's iteration is decoded against (its
header fields are known by then): the game's (NULL: the session's default,
zeros for object updates as the server always sends them against one) */
typedef const void *ce_session_baseline(void *context, const ce_message_header *header, int iteration,
	const void *header_fields);
/* one decoded iteration of a message delta */
typedef void ce_session_message(void *context, int reliable, const ce_message_header *header, int iteration,
	const unsigned char *present, const void *header_fields, const void *body);
typedef void ce_session_logger(void *context, const char *line);

struct ce_session
{
	struct ce_link link;
	int state;
	char map_name[128];
	char challenge[9];
	/* the server's sv_public: the message deltas' encoding (internet or LAN) */
	int internet;
	/* the challenge's two longs: the machine index the server gives this
	connection, and one not worked out yet */
	long challenge_long1, challenge_long2;
	/* the machine index sent in the join request instead (-1: the server's) */
	int machine_index;
	char player_name[12];
	char password[9];
	int reject_reason;
	/* the join ended for want of the server's map (map_name), which is not
	in MAPS_CE */
	int map_missing;
	/* the server ended its game and asked its machines to join its next
	(old packet 33, its two longs as sent): the session has ended */
	int reconnect;
	long reconnect_longs[2];
	unsigned long state_ms;
	uint32_t seed;
	char error[96];
	ce_delta_config config;
	ce_session_baseline *baseline;
	ce_session_message *message;
	ce_session_logger *log;
	void *context;
	long deltas_received, delta_errors;
	/* decoding room: a hud_chat message's text lands in chat_text */
	unsigned short chat_text[256];
	unsigned char header_fields[CE_DELTA_MAX_HEADER_SIZE + 4];
	unsigned char body[CE_DELTA_MAX_BODY_SIZE + 64];
	unsigned char zero_baseline[CE_DELTA_MAX_BODY_SIZE + 64];
};

/* the checksum of the map the server names: 0 (and *checksum) when the map
is here, -1 when it is not (the game's: it knows where its CE maps are) */
int ce_session_map_checksum(const char *map_name, uint32_t *checksum);

/* starts joining (name: up to 11 characters; password: up to 8, or "") */
int ce_session_begin(struct ce_session *session, uint32_t address, uint16_t port, const char *name,
	const char *password, uint32_t seed, ce_session_baseline *baseline, ce_session_message *message,
	ce_session_logger *log, void *context, unsigned long now_ms);
void ce_session_poll(struct ce_session *session, unsigned long now_ms);
/* the map the server named is loaded: the server starts sending the game */
void ce_session_map_loaded(struct ce_session *session, unsigned long now_ms);
/* sends one message delta (one iteration; baseline NULL: none): 0 sent,
1 nothing to send (an incremental message with nothing changed), -1 */
int ce_session_send_delta(struct ce_session *session, int reliable, int mode, int type, const void *header_fields,
	const void *body, const void *baseline, unsigned long now_ms);
/* ... sent even with nothing changed (the header alone, the body's
presence bits all clear): the real client's input, an update every frame
that the server decodes with its force flag (haloce 0x4E1620 -> 0x4EFD60
pushes it), each one ticks the server's copy of the player on */
int ce_session_send_delta_kept(struct ce_session *session, int reliable, int mode, int type, const void *header_fields,
	const void *body, const void *baseline, unsigned long now_ms);
/* leaves the server */
void ce_session_end(struct ce_session *session, unsigned long now_ms);

#endif
