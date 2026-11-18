/*
CE_TRANSPORT.C

A connection to a Halo PC / Custom Edition server (ce_transport.h).

GT2 messages start with FE FE and a type. The reliable ones (application
data 0, client challenge 1, server challenge 2, client response 3, accept
4, reject 5, close 6, keep-alive 7) carry two big-endian numbers after the
type: their own serial number and the next serial number the sender
expects from the other side (which acknowledges everything before it); they
are resent each second until acknowledged. The unreliable ones carry no
numbers: ack 100 (the expected serial number), nack 101 (a missing range),
ping 102, pong 103, closed 104. Unreliable application data has no header at
all (or FE FE before it, when its own first bytes are FE FE).

Halo's part: the client's response to the server's challenge is followed by
its public key (16 bytes) and the game's version (4 bytes, little-endian);
the server's key comes back with its accept; then every application
message is its data and a CRC-32 of the data (little-endian), encrypted
together with the shared key. Reliable data keeps its 7-byte header in the
clear.
*/

#include "ce_transport.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

enum
{
	GT2_APP_RELIABLE = 0,
	GT2_CLIENT_CHALLENGE = 1,
	GT2_SERVER_CHALLENGE = 2,
	GT2_CLIENT_RESPONSE = 3,
	GT2_ACCEPT = 4,
	GT2_REJECT = 5,
	GT2_CLOSE = 6,
	GT2_KEEP_ALIVE = 7,
	GT2_ACK = 100,
	GT2_NACK = 101,
	GT2_PING = 102,
	GT2_PONG = 103,
	GT2_CLOSED = 104
};

#define HEADER_SIZE 7
/* (our reliable messages again when not acknowledged by twice their round
trip, from 350 ms (the input goes reliably every frame, and a lost one held
the server's copy of the player still for a second) to 1.5 s (a far server's
every message was sent twice), a few at a time (after a long frame they are
all due at once)) */
#define RESEND_MS 350
#define RESEND_MAXIMUM_MS 1500
#define RESENDS_PER_POLL 16
/* (a GT2 ping when nothing else went for this long: unreliable, as the
reliable keep-alive took a serial number a server that does not handle it
(ChupathingyCE's) then waited for forever) */
#define KEEP_ALIVE_MS 5000
/* (the same while a map loads and the game does not poll) */
#define HEARTBEAT_MS 2000
#define CONNECT_TIMEOUT_MS 15000
/* (servers send something every 3 s (super_ping_update); counted only while
the game polls) */
#define SILENCE_TIMEOUT_MS 25000

static void logf_line(ce_link_logger *log, void *context, const char *format, ...)
	__attribute__((format(printf, 3, 4)));

static void logf_line(ce_link_logger *log, void *context, const char *format, ...)
{
	char line[160];
	va_list arguments;

	if (!log)
		return;
	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	log(context, line);
}

static void fail(struct ce_link *link, const char *reason)
{
	strncpy(link->error, reason, sizeof(link->error) - 1);
	link->error[sizeof(link->error) - 1] = 0;
	link->state = _ce_link_failed;
}

/* a before b, serial numbers wrapping */
static int serial_before(uint16_t a, uint16_t b)
{
	return (int16_t)(uint16_t)(a - b) < 0;
}

static void put16(unsigned char *p, uint16_t value)
{
	p[0] = (unsigned char)(value >> 8);
	p[1] = (unsigned char)value;
}

static uint16_t get16(const unsigned char *p)
{
	return (uint16_t)(p[0] << 8 | p[1]);
}

static void send_raw(struct ce_link *link, const unsigned char *data, int size, unsigned long now_ms)
{
	retail_socket_send_to(link->socket, data, size, link->address, link->port);
	link->datagrams_out++;
	link->last_send_ms = now_ms;
}

/* data + its CRC, encrypted: the size written. Data and CRC under 8 bytes
go as they are (TEA leaves them: ce_tea_encrypt), as the game's own do:
padded, the message's size would no longer be its data's, which a reader
that checks one against the other refuses */
static int seal(struct ce_link *link, unsigned char *out, const unsigned char *data, int size)
{
	uint32_t crc;

	memcpy(out, data, size);
	crc = ce_crc32(out, size);
	out[size] = (unsigned char)crc;
	out[size + 1] = (unsigned char)(crc >> 8);
	out[size + 2] = (unsigned char)(crc >> 16);
	out[size + 3] = (unsigned char)(crc >> 24);
	ce_tea_encrypt(out, size + 4, link->key);
	return size + 4;
}

/* decrypts in place and checks the CRC: the data's size, or -1 */
static int unseal(struct ce_link *link, unsigned char *data, int size)
{
	uint32_t crc;

	if (size < 4)
		return -1;
	ce_tea_decrypt(data, size, link->key);
	size -= 4;
	crc = (uint32_t)data[size] | (uint32_t)data[size + 1] << 8 | (uint32_t)data[size + 2] << 16 |
		(uint32_t)data[size + 3] << 24;
	if (crc != ce_crc32(data, size))
	{
		link->bad_crc++;
		return -1;
	}
	return size;
}

/* a reliable GT2 message: kept for resending until acknowledged */
static int send_reliable(struct ce_link *link, int type, const unsigned char *body, int size, int sealed,
	unsigned long now_ms)
{
	struct ce_link_message *message;

	if (link->unacknowledged_count >= CE_LINK_UNACKNOWLEDGED || size + HEADER_SIZE + 8 > CE_MAXIMUM_PACKET)
		return -1;
	message = &link->unacknowledged[link->unacknowledged_count++];
	message->data[0] = 0xFE;
	message->data[1] = 0xFE;
	message->data[2] = (unsigned char)type;
	put16(message->data + 3, link->serial);
	put16(message->data + 5, link->expected);
	if (sealed)
		size = seal(link, message->data + HEADER_SIZE, body, size);
	else if (size)
		memcpy(message->data + HEADER_SIZE, body, size);
	message->serial = link->serial++;
	message->size = (uint16_t)(HEADER_SIZE + size);
	message->sent_ms = now_ms;
	message->resent = 0;
	/* (it acknowledges what we have, as an ack would) */
	link->ack_pending = 0;
	send_raw(link, message->data, message->size, now_ms);
	return 0;
}

static void acknowledged(struct ce_link *link, uint16_t expected, unsigned long now_ms)
{
	int i, kept = 0;

	for (i = 0; i < link->unacknowledged_count; i++)
		if (serial_before(link->unacknowledged[i].serial, expected))
		{
			/* (the round trip, from those sent once) */
			if (!link->unacknowledged[i].resent)
			{
				unsigned long sample = now_ms - link->unacknowledged[i].sent_ms;

				link->round_trip_ms = link->round_trip_ms ? (link->round_trip_ms * 7 + sample) / 8 : sample;
			}
			continue;
		}
		else
		{
			if (kept != i)
				link->unacknowledged[kept] = link->unacknowledged[i];
			kept++;
		}
	link->unacknowledged_count = kept;
}

static void send_unreliable_control(struct ce_link *link, int type, const unsigned char *body, int size,
	unsigned long now_ms)
{
	unsigned char packet[64];

	if (size > (int)sizeof(packet) - 3)
		size = (int)sizeof(packet) - 3;
	packet[0] = 0xFE;
	packet[1] = 0xFE;
	packet[2] = (unsigned char)type;
	if (size)
		memcpy(packet + 3, body, size);
	send_raw(link, packet, 3 + size, now_ms);
}

int ce_link_begin(struct ce_link *link, uint32_t address, uint16_t port, uint32_t version, uint32_t seed,
	unsigned long now_ms)
{
	memset(link, 0, sizeof(*link));
	link->socket = retail_socket_open(0);
	link->address = address;
	link->port = port;
	link->version = version;
	link->seed = seed | 1;
	link->started_ms = link->last_receive_ms = now_ms;
	if (link->socket == RETAIL_NO_SOCKET)
	{
		fail(link, "no socket");
		return -1;
	}
	ce_key_secret(link->secret, seed * 0x343FD + now_ms);
	ce_key_public(link->secret, link->public_key);
	ce_gs_challenge(link->challenge, &link->seed);
	link->state = _ce_link_challenging;
	return send_reliable(link, GT2_CLIENT_CHALLENGE, link->challenge, CE_CHALLENGE_SIZE, 0, now_ms);
}

/* a reliable message taken in order (opened: decrypted and checked already) */
static void handle_reliable(struct ce_link *link, int type, unsigned char *body, int size, int opened,
	unsigned long now_ms, ce_link_receiver *receive, ce_link_logger *log, void *context)
{
	switch (type)
	{
	case GT2_SERVER_CHALLENGE:
	{
		unsigned char expected[CE_CHALLENGE_SIZE], reply[CE_CHALLENGE_SIZE + CE_KEY_SIZE + 4];

		if (link->state != _ce_link_challenging || size < 2 * CE_CHALLENGE_SIZE)
			break;
		ce_gs_response(expected, link->challenge, &link->seed);
		if (!ce_gs_response_matches(expected, body))
			logf_line(log, context, "transport: the server's answer to our challenge is not the expected one");
		ce_gs_response(reply, body + CE_CHALLENGE_SIZE, &link->seed);
		memcpy(reply + CE_CHALLENGE_SIZE, link->public_key, CE_KEY_SIZE);
		reply[CE_CHALLENGE_SIZE + CE_KEY_SIZE + 0] = (unsigned char)link->version;
		reply[CE_CHALLENGE_SIZE + CE_KEY_SIZE + 1] = (unsigned char)(link->version >> 8);
		reply[CE_CHALLENGE_SIZE + CE_KEY_SIZE + 2] = (unsigned char)(link->version >> 16);
		reply[CE_CHALLENGE_SIZE + CE_KEY_SIZE + 3] = (unsigned char)(link->version >> 24);
		link->state = _ce_link_responding;
		send_reliable(link, GT2_CLIENT_RESPONSE, reply, (int)sizeof(reply), 0, now_ms);
		logf_line(log, context, "transport: challenge answered, key and version %lu sent",
			(unsigned long)link->version);
		break;
	}
	case GT2_ACCEPT:
		if (link->state != _ce_link_responding)
			break;
		link->state = _ce_link_connected;
		if (size >= CE_KEY_SIZE)
		{
			ce_key_shared(link->secret, body, link->key);
			link->key_known = 1;
		}
		logf_line(log, context, "transport: accepted (%d bytes with it)%s", size,
			link->key_known ? ", key made" : ", no key yet");
		break;
	case GT2_REJECT:
		link->reject_reason = size > 0 ? body[0] : -1;
		fail(link, link->reject_reason == _ce_reject_full ? "the server is full" :
			link->reject_reason == _ce_reject_version_4 || link->reject_reason == _ce_reject_version_5 ?
			"the server runs another version" : "the server refused the connection");
		break;
	case GT2_CLOSE:
		send_unreliable_control(link, GT2_CLOSED, NULL, 0, now_ms);
		link->state = _ce_link_closed;
		strcpy(link->error, "the server closed the connection");
		break;
	case GT2_KEEP_ALIVE:
		break;
	case GT2_APP_RELIABLE:
		if (link->key_known && !opened)
		{
			int got = unseal(link, body, size);

			if (got < 0)
				logf_line(log, context, "transport: a reliable message's CRC is wrong (%d bytes)", size);
			else if (receive)
				receive(context, 1, body, got);
		}
		else if (receive)
			receive(context, 1, body, size);
		break;
	default:
		logf_line(log, context, "transport: reliable message of type %d (%d bytes)", type, size);
		break;
	}
}

static void handle_datagram(struct ce_link *link, unsigned char *data, int size, unsigned long now_ms,
	ce_link_receiver *receive, ce_link_logger *log, void *context)
{
	int type;

	link->datagrams_in++;
	link->last_receive_ms = now_ms;
	if (size >= 3 && data[0] == 0xFE && data[1] == 0xFE && !(size >= 4 && data[2] == 0xFE && data[3] == 0xFE))
	{
		type = data[2];
		if (type <= GT2_KEEP_ALIVE)
		{
			uint16_t serial, expected;

			if (size < HEADER_SIZE)
				return;
			serial = get16(data + 3);
			expected = get16(data + 5);
			acknowledged(link, expected, now_ms);
			if (serial == link->expected)
			{
				int i, opened = 0, got = size - HEADER_SIZE;

				/* (application data checked before it is taken: one whose CRC is
				wrong is neither taken nor acknowledged, and comes again, where
				acknowledged it was lost for good) */
				if (type == GT2_APP_RELIABLE && link->key_known)
				{
					got = unseal(link, data + HEADER_SIZE, size - HEADER_SIZE);
					if (got < 0)
					{
						logf_line(log, context, "transport: a reliable message's CRC is wrong (%d bytes): awaited again",
							size - HEADER_SIZE);
						return;
					}
					opened = 1;
				}
				link->expected++;
				link->ack_pending = 1;
				handle_reliable(link, type, data + HEADER_SIZE, got, opened, now_ms, receive, log, context);
				/* (then those that came ahead of it, in order) */
				for (i = 0; i < link->ahead_count && link->state == _ce_link_connected;)
				{
					if (link->ahead[i].serial != link->expected)
					{
						i++;
						continue;
					}
					link->expected++;
					handle_reliable(link, link->ahead[i].type, link->ahead[i].data, link->ahead[i].size,
						link->ahead[i].opened, now_ms, receive, log, context);
					link->ahead[i] = link->ahead[--link->ahead_count];
					i = 0;
				}
				/* (any left behind the one awaited now: had already) */
				for (i = 0; i < link->ahead_count;)
				{
					if (serial_before(link->ahead[i].serial, link->expected))
						link->ahead[i] = link->ahead[--link->ahead_count];
					else
						i++;
				}
			}
			else if (serial_before(serial, link->expected))
			{
				/* (seen before: our acknowledgement was lost) */
				link->ack_pending = 1;
			}
			else
			{
				unsigned char range[4];
				int i, kept = 0, opened = 0, got = size - HEADER_SIZE;

				/* (ahead of one lost or late: kept for when that comes, rather than
				dropped and waited for again, a second's stall each time; checked
				first, as the one in order is) */
				for (i = 0; i < link->ahead_count; i++)
					kept |= link->ahead[i].serial == serial;
				if (!kept && type == GT2_APP_RELIABLE && link->key_known && got <= CE_MAXIMUM_PACKET)
				{
					got = unseal(link, data + HEADER_SIZE, size - HEADER_SIZE);
					opened = 1;
					if (got < 0)
						kept = 1;
				}
				if (!kept && link->ahead_count < CE_LINK_AHEAD && got <= CE_MAXIMUM_PACKET)
				{
					link->ahead[link->ahead_count].serial = serial;
					link->ahead[link->ahead_count].type = type;
					link->ahead[link->ahead_count].opened = opened;
					link->ahead[link->ahead_count].size = (uint16_t)got;
					memcpy(link->ahead[link->ahead_count].data, data + HEADER_SIZE, (size_t)got);
					link->ahead_count++;
					link->reordered++;
				}
				put16(range, link->expected);
				put16(range + 2, (uint16_t)(serial - 1));
				send_unreliable_control(link, GT2_NACK, range, 4, now_ms);
			}
			return;
		}
		switch (type)
		{
		case GT2_ACK:
			if (size >= 5)
				acknowledged(link, get16(data + 3), now_ms);
			break;
		case GT2_NACK:
			if (size >= 7)
			{
				uint16_t first = get16(data + 3), last = get16(data + 5);
				int i;

				for (i = 0; i < link->unacknowledged_count; i++)
				{
					struct ce_link_message *message = &link->unacknowledged[i];

					if (!serial_before(message->serial, first) && !serial_before(last, message->serial))
					{
						send_raw(link, message->data, message->size, now_ms);
						message->sent_ms = now_ms;
						message->resent = 1;
						link->resends++;
					}
				}
			}
			break;
		case GT2_PING:
			send_unreliable_control(link, GT2_PONG, data + 3, size - 3, now_ms);
			break;
		case GT2_PONG:
			break;
		case GT2_CLOSED:
			link->state = _ce_link_closed;
			strcpy(link->error, "the server closed the connection");
			break;
		default:
			logf_line(log, context, "transport: unknown message type %d (%d bytes)", type, size);
			break;
		}
		return;
	}
	/* unreliable application data */
	if (size >= 4 && data[0] == 0xFE && data[1] == 0xFE)
	{
		data += 2;
		size -= 2;
	}
	if (link->state != _ce_link_connected)
		return;
	if (link->key_known)
	{
		int got = unseal(link, data, size);

		if (got < 0)
			logf_line(log, context, "transport: an unreliable datagram's CRC is wrong (%d bytes)", size);
		else if (receive)
			receive(context, 0, data, got);
	}
	else if (receive)
		receive(context, 0, data, size);
}

void ce_link_poll(struct ce_link *link, unsigned long now_ms, ce_link_receiver *receive, ce_link_logger *log,
	void *context)
{
	unsigned char datagram[CE_MAXIMUM_PACKET + 64];
	unsigned long resend_ms;
	int i, got, resent = 0;

	if (link->state == _ce_link_off || link->state == _ce_link_failed || link->socket == RETAIL_NO_SOCKET)
		return;
	/* (time the game did not poll, a map loading, is not the server's
	silence: what it sent then may be lost to a full receive ring) */
	if (link->last_poll_ms && now_ms - link->last_poll_ms > 1000)
		link->last_receive_ms += now_ms - link->last_poll_ms;
	link->last_poll_ms = now_ms;
	for (i = 0; i < 64; i++)
	{
		uint32_t from;
		uint16_t from_port;

		got = retail_socket_receive_from(link->socket, datagram, (int)sizeof(datagram), &from, &from_port);
		if (got < 0)
			break;
		if (from != link->address || from_port != link->port)
			continue;
		handle_datagram(link, datagram, got, now_ms, receive, log, context);
		if (link->state == _ce_link_failed || link->state == _ce_link_closed)
			return;
	}
	/* (acknowledged each poll that took reliable data: a server keeps few of
	its messages unacknowledged (ChupathingyCE's 8), and one held back a
	frame and a tenth of a second at a slow machine's frame rate could fill
	its window, losing updates) */
	if (link->ack_pending)
	{
		unsigned char expected[2];

		put16(expected, link->expected);
		send_unreliable_control(link, GT2_ACK, expected, 2, now_ms);
		link->ack_pending = 0;
	}
	resend_ms = link->round_trip_ms * 2;
	if (resend_ms < RESEND_MS)
		resend_ms = RESEND_MS;
	if (resend_ms > RESEND_MAXIMUM_MS)
		resend_ms = RESEND_MAXIMUM_MS;
	for (i = 0; i < link->unacknowledged_count && resent < RESENDS_PER_POLL; i++)
	{
		struct ce_link_message *message = &link->unacknowledged[i];

		if (now_ms - message->sent_ms >= resend_ms)
		{
			send_raw(link, message->data, message->size, now_ms);
			message->sent_ms = now_ms;
			message->resent = 1;
			link->resends++;
			resent++;
		}
	}
	if (link->state == _ce_link_connected && now_ms - link->last_send_ms >= KEEP_ALIVE_MS)
	{
		unsigned char stamp[4];

		stamp[0] = (unsigned char)(now_ms >> 24);
		stamp[1] = (unsigned char)(now_ms >> 16);
		stamp[2] = (unsigned char)(now_ms >> 8);
		stamp[3] = (unsigned char)now_ms;
		send_unreliable_control(link, GT2_PING, stamp, 4, now_ms);
	}
	if (link->state != _ce_link_connected && now_ms - link->started_ms >= CONNECT_TIMEOUT_MS)
		fail(link, link->state == _ce_link_challenging ? "no answer from the server" :
			"the server did not accept the connection");
	else if (link->state == _ce_link_connected && now_ms - link->last_receive_ms >= SILENCE_TIMEOUT_MS)
		fail(link, "the server stopped answering");
}

int ce_link_send(struct ce_link *link, int reliable, const unsigned char *data, int size, unsigned long now_ms)
{
	unsigned char packet[CE_MAXIMUM_PACKET];
	int sealed;

	if (link->state != _ce_link_connected || !link->key_known || size + 2 + 8 > CE_MAXIMUM_PACKET - HEADER_SIZE)
		return -1;
	if (reliable)
		return send_reliable(link, GT2_APP_RELIABLE, data, size, 1, now_ms);
	sealed = seal(link, packet + 2, data, size);
	if (packet[2] == 0xFE && packet[3] == 0xFE)
	{
		packet[0] = packet[1] = 0xFE;
		send_raw(link, packet, sealed + 2, now_ms);
	}
	else
		send_raw(link, packet + 2, sealed, now_ms);
	return 0;
}

void ce_link_close(struct ce_link *link, unsigned long now_ms)
{
	if (link->socket == RETAIL_NO_SOCKET)
		return;
	if (link->state == _ce_link_connected || link->state == _ce_link_responding)
	{
		send_reliable(link, GT2_CLOSE, NULL, 0, 0, now_ms);
		/* (and closed, GT2's hard close, twice: the socket goes now, so the
		close is never resent, and a server that missed it (or, as
		ChupathingyCE's, does not read it) held our player until its
		timeout) */
		send_unreliable_control(link, GT2_CLOSED, NULL, 0, now_ms);
		send_unreliable_control(link, GT2_CLOSED, NULL, 0, now_ms);
	}
	retail_socket_close(link->socket);
	link->socket = RETAIL_NO_SOCKET;
	if (link->state != _ce_link_failed)
		link->state = _ce_link_closed;
}

void ce_link_heartbeat(struct ce_link *link, unsigned long now_ms)
{
	unsigned char stamp[4];

	if (link->state != _ce_link_connected || link->socket == RETAIL_NO_SOCKET ||
		now_ms - link->last_send_ms < HEARTBEAT_MS)
	{
		return;
	}
	stamp[0] = (unsigned char)(now_ms >> 24);
	stamp[1] = (unsigned char)(now_ms >> 16);
	stamp[2] = (unsigned char)(now_ms >> 8);
	stamp[3] = (unsigned char)now_ms;
	send_unreliable_control(link, GT2_PING, stamp, 4, now_ms);
}
