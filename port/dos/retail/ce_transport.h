/*
CE_TRANSPORT.H

A connection to a Halo PC / Custom Edition server (ce_transport.c): the
GameSpy transport (GT2) over UDP as Halo uses it -- the handshake, serial
numbers, acknowledgements and resends -- with Halo's key exchange and its
TEA-encrypted, CRC-checked data. Non-blocking: ce_link_poll() does
everything that is due, from the game's frame or a test's loop.
*/

#ifndef __CE_TRANSPORT_H
#define __CE_TRANSPORT_H

#include <stdint.h>

#include "ce_crypto.h"
#include "retail_socket.h"

/* the largest datagram either side sends */
#define CE_MAXIMUM_PACKET 1400
/* our reliable messages kept until acknowledged (the input goes reliably
each frame: a round trip's worth at 60 frames a second, and room to spare) */
#define CE_LINK_UNACKNOWLEDGED 64
/* the server's reliable messages that came ahead of one lost or late, kept
until it comes (they are delivered in order then) */
#define CE_LINK_AHEAD 24

enum ce_link_state
{
	_ce_link_off,
	_ce_link_challenging, /* our challenge sent, the server's awaited */
	_ce_link_responding, /* our response and key sent, accept awaited */
	_ce_link_connected,
	_ce_link_closed, /* closed by either side */
	_ce_link_failed /* error[] says why */
};

/* the server's GT2 reasons for a reject (the first byte of its data) */
enum
{
	_ce_reject_version_4 = 4,
	_ce_reject_version_5 = 5,
	_ce_reject_full = 6
};

struct ce_link_message
{
	uint16_t serial;
	uint16_t size;
	unsigned long sent_ms;
	unsigned char data[CE_MAXIMUM_PACKET];
};

struct ce_link
{
	retail_socket_t socket;
	uint32_t address; /* network order */
	uint16_t port;
	int state;
	uint32_t seed, version;
	unsigned long started_ms, last_send_ms, last_receive_ms, ack_since_ms;
	unsigned char challenge[CE_CHALLENGE_SIZE];
	char secret[CE_SECRET_SIZE];
	unsigned char public_key[CE_KEY_SIZE], key[CE_KEY_SIZE];
	int key_known;
	/* the serial number of our next reliable message, and of the server's
	next one we are waiting for */
	uint16_t serial, expected;
	int ack_pending;
	struct ce_link_message unacknowledged[CE_LINK_UNACKNOWLEDGED];
	int unacknowledged_count;
	struct
	{
		uint16_t serial;
		uint16_t size;
		int type;
		unsigned char data[CE_MAXIMUM_PACKET];
	} ahead[CE_LINK_AHEAD];
	int ahead_count;
	int reject_reason;
	/* counts, for the log */
	long datagrams_in, datagrams_out, bad_crc, resends, reordered;
	char error[96];
};

/* what arrived: a reliable message's or an unreliable datagram's data,
decrypted and checked, without its CRC; or, while connecting, a GT2
message the link does not handle itself (type, the bytes after its header) */
typedef void ce_link_receiver(void *context, int reliable, const unsigned char *data, int size);
typedef void ce_link_logger(void *context, const char *line);

/* starts connecting: 0 on success (state challenging), -1 (state failed) */
int ce_link_begin(struct ce_link *link, uint32_t address, uint16_t port, uint32_t version, uint32_t seed,
	unsigned long now_ms);
/* reads what arrived, answers, resends: call often */
void ce_link_poll(struct ce_link *link, unsigned long now_ms, ce_link_receiver *receive, ce_link_logger *log,
	void *context);
/* sends application data (reliable: resent until acknowledged); 0 or -1 */
int ce_link_send(struct ce_link *link, int reliable, const unsigned char *data, int size, unsigned long now_ms);
/* says goodbye and closes the socket */
void ce_link_close(struct ce_link *link, unsigned long now_ms);

#endif
