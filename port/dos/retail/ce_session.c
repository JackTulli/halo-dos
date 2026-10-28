/*
CE_SESSION.C

Joining a Halo Custom Edition server (ce_session.h).

The join request (packet 14) has to carry three things the server checks
(haloceded 01.00.10.0621, 004CCB80): a constant 16-byte token, "message in
a bot" (004CB0B0); the map's checksum with the characters of the challenge
the server sent run over its four bytes (0048DB50), which shows the client
has the very same map; and a key response for the server's GameSpy CD-key
check. No key is sent: the response field holds random hexadecimal digits,
worked out from no key; a server that checks keys refuses the join ("bad
CD key"), one that does not (the common case) logs its first 32 digits.
The server's reasons for refusing: 1 a bad request (the token), 2
password, 3 full (also a machine index already taken), 4 and 5 CD key, 6
banned, 7 kicked, 0 the rest (the map's checksum, most likely).

The challenge's byte after its 8 characters is the server's sv_public: 1,
an internet server, whose message deltas use the internet encoding (fewer
bits for positions and angles); 0, the LAN encoding.

A stream from the server mixes items: message deltas, which carry no size
(the only way past one is to decode it: every one is decoded here, its
baseline asked of the game), and old packets.
*/

#include "ce_session.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ce_delta_tables.h"
#include "ce_packets.h"

/* how long the server may take to answer a join */
#define JOIN_TIMEOUT_MS 15000

static void session_log(struct ce_session *session, const char *format, ...) __attribute__((format(printf, 2, 3)));

static void session_log(struct ce_session *session, const char *format, ...)
{
	char line[192];
	va_list arguments;

	if (!session->log)
		return;
	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	session->log(session->context, line);
}

static void set_state(struct ce_session *session, int state, unsigned long now_ms)
{
	session->state = state;
	session->state_ms = now_ms;
}

static void fail(struct ce_session *session, const char *reason, unsigned long now_ms)
{
	strncpy(session->error, reason, sizeof(session->error) - 1);
	session->error[sizeof(session->error) - 1] = 0;
	session_log(session, "ce: %s", session->error);
	set_state(session, _ce_session_failed, now_ms);
}

static void link_log(void *context, const char *line)
{
	struct ce_session *session = (struct ce_session *)context;

	if (session->log)
		session->log(session->context, line);
}

static int send_packet(struct ce_session *session, int type, const void *decoded, unsigned long now_ms)
{
	unsigned char message[400], stream[420];
	ce_bitstream bs;
	long total;
	int size = ce_packet_encode(type, decoded, message, sizeof(message)), i;

	if (size < 0)
		return -1;
	ce_stream_begin(&bs, stream, sizeof(stream));
	ce_stream_put_kind(&bs, CE_ITEM_PACKET);
	for (i = 0; i < size; i++)
		ce_bs_write(&bs, message[i], 8);
	total = ce_stream_finish(&bs);
	if (total <= 0 || bs.overflow)
		return -1;
	return ce_link_send(&session->link, 1, stream, (int)total, now_ms);
}

static void send_join(struct ce_session *session, unsigned long now_ms)
{
	struct ce_packet_join_game_request request;
	uint32_t checksum;
	unsigned char mixed[4];
	int i, length = (int)strlen(session->challenge);

	if (ce_session_map_checksum(session->map_name, &checksum) < 0)
	{
		char reason[96];

		snprintf(reason, sizeof(reason), "the server plays %.40s, a map not here", session->map_name);
		fail(session, reason, now_ms);
		/* (the game may download it, then join again: dos_map_download.c) */
		session->map_missing = 1;
		return;
	}
	memset(&request, 0, sizeof(request));
	memcpy(request.token, "message in a bot", 16);
	for (i = 0; session->password[i] && i < 8; i++)
		request.password[i] = (unsigned char)session->password[i];
	for (i = 0; i < 72; i++)
	{
		session->seed = session->seed * 0x343FD + 0x269EC3;
		request.key_response[i] = "0123456789abcdef"[(session->seed >> 16) & 15];
	}
	/* (LAN on a LAN server; a cable/DSL line on an internet one) */
	request.connection_class = (uint8_t)(session->internet ? 3 : 4);
	for (i = 0; session->player_name[i] && i < 11; i++)
		request.player.name[i] = (unsigned char)session->player_name[i];
	request.player.primary_color_index = -1;
	request.player.icon_index = -1;
	/* (the machine index the server gave us in its challenge: another one
	takes a machine already in the game, which the server refuses as full) */
	request.player.machine_index = (int8_t)(session->machine_index >= 0 ? session->machine_index :
		session->challenge_long1);
	request.player.controller_index = 0;
	request.player.team_index = -1;
	request.player.player_list_index = -1;
	mixed[0] = (unsigned char)checksum;
	mixed[1] = (unsigned char)(checksum >> 8);
	mixed[2] = (unsigned char)(checksum >> 16);
	mixed[3] = (unsigned char)(checksum >> 24);
	for (i = 0; length && i < (length < 4 ? 4 : length); i++)
		mixed[i & 3] ^= (unsigned char)session->challenge[i % length];
	request.unknown_90 = (int32_t)((uint32_t)mixed[0] | (uint32_t)mixed[1] << 8 | (uint32_t)mixed[2] << 16 |
		(uint32_t)mixed[3] << 24);
	if (send_packet(session, _ce_packet_client_join_game_request, &request, now_ms) < 0)
	{
		fail(session, "could not send the join request", now_ms);
		return;
	}
	session_log(session, "ce: joining (map %s, checksum %08lX, %s server, challenge longs %ld %ld)", session->map_name,
		(unsigned long)checksum, session->internet ? "internet" : "LAN", session->challenge_long1,
		session->challenge_long2);
	set_state(session, _ce_session_joining, now_ms);
}

static void handle_packet(struct ce_session *session, int type, const void *decoded, unsigned long now_ms)
{
	switch (type)
	{
	case _ce_packet_server_new_client_challenge:
	{
		const struct ce_packet_new_client_challenge *challenge = (const struct ce_packet_new_client_challenge *)decoded;

		if (session->state != _ce_session_connecting)
			break;
		memcpy(session->challenge, challenge->challenge, 8);
		session->challenge[8] = 0;
		strncpy(session->map_name, challenge->map_name, sizeof(session->map_name) - 1);
		session->internet = challenge->unknown_8 == 1;
		session->challenge_long1 = challenge->unknown_c;
		session->challenge_long2 = challenge->unknown_10;
		session->config.encoding_class = session->internet ? CE_CLASS_INTERNET : CE_CLASS_LAN;
		send_join(session, now_ms);
		break;
	}
	case _ce_packet_server_machine_rejected:
	{
		int32_t reason = *(const int32_t *)decoded;

		session->reject_reason = (int)reason;
		session_log(session, "ce: refused, reason %ld", (long)reason);
		fail(session, reason == 1 ? "the server did not take our join request" : reason == 2 ? "wrong password" :
			reason == 3 ? "the server is full" : reason == 4 || reason == 5 ? "the server checks CD keys" :
			reason == 6 ? "banned from this server" : reason == 7 ? "kicked from this server" :
			"the server refused us (most likely a different version of its map)", now_ms);
		break;
	}
	case _ce_packet_server_graceful_game_exit_pregame:
	case _ce_packet_server_graceful_game_exit_postgame:
		strcpy(session->error, "the server ended the game");
		set_state(session, _ce_session_ended, now_ms);
		break;
	case _ce_packet_server_reconnect:
	{
		/* (the next game in the server's rotation: a client joins it again,
		as the real client does in its postgame) */
		const int32_t *longs = (const int32_t *)decoded;

		session->reconnect = 1;
		session->reconnect_longs[0] = longs[0];
		session->reconnect_longs[1] = longs[1];
		session_log(session, "ce: the server starts its next game (reconnect %ld %ld)", (long)longs[0],
			(long)longs[1]);
		strcpy(session->error, "the server starts its next game");
		set_state(session, _ce_session_ended, now_ms);
		break;
	}
	default:
		session_log(session, "ce: packet %s", ce_packet_name(type));
		break;
	}
}

/* one message delta (after its kind bit): 0, or -1 when it would not decode
(the rest of the stream cannot be read then) */
static int read_delta(struct ce_session *session, ce_bitstream *bs, int reliable)
{
	ce_message_header header;
	int iteration, result, i;

	result = ce_delta_read_header(&session->config, bs, &header);
	if (result < 0)
		return -1;
	for (iteration = 0; iteration < header.iterations; iteration++)
	{
		unsigned char present[64];
		const void *baseline = NULL;

		result = ce_delta_read_iteration_header(&session->config, bs, &header, present, session->header_fields);
		if (result < 0)
			return -1;
		if (header.mode == 1)
		{
			if (session->baseline)
				baseline = session->baseline(session->context, &header, iteration, session->header_fields);
			/* (object updates always come against a baseline: zeros give the
			layout when the game has none) */
			if (!baseline && header.type >= CE_MSG_PROJECTILE_UPDATE && header.type <= CE_MSG_VEHICLE_UPDATE)
				baseline = session->zero_baseline;
		}
		/* (a hud_chat's text: the field is a pointer to room for it) */
		for (i = 0; i < header.def->body_count; i++)
			if (header.def->body[i].props->type == CE_FT_POINTER)
			{
				void *text = session->chat_text;

				memcpy(session->body + header.def->body[i].offset, &text, sizeof(text));
			}
		result = ce_delta_read_iteration_body(&session->config, bs, &header, present, baseline, session->body);
		if (result < 0)
			return -1;
		session->deltas_received++;
		if (session->message)
			session->message(session->context, reliable, &header, iteration, present, session->header_fields,
				session->body);
	}
	return 0;
}

static void link_received(void *context, int reliable, const unsigned char *data, int size)
{
	struct ce_session *session = (struct ce_session *)context;
	unsigned long now_ms = session->link.last_receive_ms;
	ce_bitstream bs;
	int kind;

	if (ce_stream_open(&bs, data, (unsigned long)size) < 0)
	{
		session->delta_errors++;
		return;
	}
	while ((kind = ce_stream_next(&bs)) != CE_ITEM_END && session->state != _ce_session_failed)
	{
		if (kind == CE_ITEM_DELTA)
		{
			/* (the first delta answers the join: we are in) */
			if (session->state == _ce_session_joining)
			{
				session_log(session, "ce: joined");
				set_state(session, _ce_session_joined, now_ms);
			}
			if (read_delta(session, &bs, reliable) < 0)
			{
				if (session->delta_errors++ < 8)
					session_log(session, "ce: a message delta would not decode (the rest of a stream lost)");
				return;
			}
		}
		else
		{
			unsigned char message[1100];
			union
			{
				unsigned char bytes[1536];
				int32_t align;
			} decoded;
			int length = ce_stream_read_packet(&bs, message, sizeof(message)), type;

			if (length < 0)
				return;
			if (ce_packet_decode(message, length, &type, &decoded, sizeof(decoded)) == 0)
				handle_packet(session, type, &decoded, now_ms);
		}
	}
}

int ce_session_begin(struct ce_session *session, uint32_t address, uint16_t port, const char *name,
	const char *password, uint32_t seed, ce_session_baseline *baseline, ce_session_message *message,
	ce_session_logger *log, void *context, unsigned long now_ms)
{
	memset(session, 0, sizeof(*session));
	strncpy(session->player_name, name, sizeof(session->player_name) - 1);
	strncpy(session->password, password ? password : "", sizeof(session->password) - 1);
	ce_delta_config_default(&session->config);
	session->machine_index = -1;
	session->seed = seed | 1;
	session->baseline = baseline;
	session->message = message;
	session->log = log;
	session->context = context;
	set_state(session, _ce_session_connecting, now_ms);
	if (ce_link_begin(&session->link, address, port, CE_VERSION_1_10, seed, now_ms) < 0)
	{
		fail(session, session->link.error, now_ms);
		return -1;
	}
	return 0;
}

void ce_session_poll(struct ce_session *session, unsigned long now_ms)
{
	if (session->state == _ce_session_off || session->state == _ce_session_failed || session->state == _ce_session_ended)
		return;
	ce_link_poll(&session->link, now_ms, link_received, link_log, session);
	if (session->state == _ce_session_failed || session->state == _ce_session_ended)
		return;
	if (session->link.state == _ce_link_failed)
		fail(session, session->link.error, now_ms);
	else if (session->link.state == _ce_link_closed)
	{
		strcpy(session->error, session->link.error[0] ? session->link.error : "the server closed the connection");
		set_state(session, _ce_session_ended, now_ms);
	}
	else if (session->state == _ce_session_joining && now_ms - session->state_ms >= JOIN_TIMEOUT_MS)
		fail(session, "the server did not answer the join", now_ms);
}

void ce_session_map_loaded(struct ce_session *session, unsigned long now_ms)
{
	int32_t zero = 0;

	if (session->state != _ce_session_joined)
		return;
	if (send_packet(session, _ce_packet_client_loaded, &zero, now_ms) < 0)
	{
		fail(session, "could not tell the server the map is loaded", now_ms);
		return;
	}
	session_log(session, "ce: map loaded, playing");
	set_state(session, _ce_session_playing, now_ms);
}

static int send_delta(struct ce_session *session, int reliable, int mode, int type, const void *header_fields,
	const void *body, const void *baseline, int keep_empty, unsigned long now_ms)
{
	unsigned char stream[CE_MAXIMUM_PACKET];
	ce_bitstream bs;
	const void *headers[1], *bodies[1], *baselines[1];
	long bits, total;

	if (session->state != _ce_session_playing)
		return -1;
	ce_stream_begin(&bs, stream, sizeof(stream) - 16);
	ce_stream_put_kind(&bs, CE_ITEM_DELTA);
	headers[0] = header_fields;
	bodies[0] = body;
	baselines[0] = baseline;
	bits = ce_delta_encode(&session->config, &bs, mode, type, headers, bodies, baselines, 1, keep_empty);
	if (bits < 0)
		return -1;
	if (bits == 0)
		return 1;
	total = ce_stream_finish(&bs);
	if (total <= 0 || bs.overflow)
		return -1;
	return ce_link_send(&session->link, reliable, stream, (int)total, now_ms);
}

int ce_session_send_delta(struct ce_session *session, int reliable, int mode, int type, const void *header_fields,
	const void *body, const void *baseline, unsigned long now_ms)
{
	return send_delta(session, reliable, mode, type, header_fields, body, baseline, 0, now_ms);
}

int ce_session_send_delta_kept(struct ce_session *session, int reliable, int mode, int type, const void *header_fields,
	const void *body, const void *baseline, unsigned long now_ms)
{
	return send_delta(session, reliable, mode, type, header_fields, body, baseline, 1, now_ms);
}

void ce_session_end(struct ce_session *session, unsigned long now_ms)
{
	if (session->state == _ce_session_playing)
	{
		int32_t zero = 0;

		send_packet(session, _ce_packet_client_graceful_game_exit_ingame, &zero, now_ms);
	}
	ce_link_close(&session->link, now_ms);
	if (session->state != _ce_session_failed)
		set_state(session, _ce_session_ended, now_ms);
}
