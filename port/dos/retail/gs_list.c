/*
GS_LIST.C

A GameSpy master server's list of a game's servers (gs_list.h).

The request (GameSpy's server browser, protocol 1, encoding 3): its length
(two bytes, big-endian), 0 (a list request), 1, 3, the game's version (four
bytes, 0), the game whose list it is and the game asking (each ending in a
0), eight random characters the answer is encrypted with, a filter and the
fields wanted (both empty: addresses alone; the servers are asked
themselves, gs_query.c) and four bytes of options (0).

The answer, decrypted: the asker's address (4), the servers' usual port
(2; FFFF: an error), the fields (a count, then a type and a name each), the
fields' popular values (a count, then a string each), then the servers: a
flags byte, the address (4), its port when not the usual one (flag 0x10)
and more addresses (private 0x02, ICMP 0x08, private port 0x20), the
fields' values (flag 0x40), until a flags byte of 0 and FFFFFFFF.
*/

#include "gs_list.h"

#include <stdlib.h>
#include <string.h>

/* enctypex_decoder.c's stream decoder (Luigi Auriemma's, GPL-2.0-or-later) */
typedef struct
{
	unsigned char encxkey[261];
	int offset;
	int start;
} enctypex_data_t;

unsigned char *enctypex_decoder(unsigned char *key, unsigned char *validate, unsigned char *data, int *datalen,
	enctypex_data_t *enctypex_data);

typedef char gs_list_decoder_room[sizeof(enctypex_data_t) <= sizeof(((struct gs_list *)0)->decoder) ? 1 : -1];

static void fail(struct gs_list *list, const char *reason)
{
	strncpy(list->error, reason, sizeof(list->error) - 1);
	list->error[sizeof(list->error) - 1] = 0;
	retail_socket_close(list->socket);
	list->socket = RETAIL_NO_SOCKET;
	list->state = _gs_list_failed;
}

int gs_list_begin(struct gs_list *list, uint32_t master, uint16_t port, const char *for_game,
	const char *from_game, const char *game_key, uint32_t seed, struct gs_list_server *servers, int maximum,
	unsigned long now_ms)
{
	unsigned char request[256];
	int length = 2, i;

	memset(list, 0, sizeof(*list));
	list->socket = RETAIL_NO_SOCKET;
	list->started_ms = now_ms;
	list->servers = servers;
	list->maximum = maximum;
	strncpy(list->game_key, game_key, sizeof(list->game_key) - 1);
	/* (the challenge: printable characters, as the game's) */
	for (i = 0; i < 8; i++)
	{
		do
			seed = seed * 0x343FD + 0x269EC3;
		while (((seed >> 16) & 0x7F) < 0x21 || ((seed >> 16) & 0x7F) >= 0x7F);
		list->validate[i] = (unsigned char)((seed >> 16) & 0x7F);
	}
	request[length++] = 0;
	request[length++] = 1;
	request[length++] = 3;
	memset(request + length, 0, 4);
	length += 4;
	if (strlen(for_game) + strlen(from_game) + 40 > sizeof(request))
		return 0;
	strcpy((char *)request + length, for_game);
	length += (int)strlen(for_game) + 1;
	strcpy((char *)request + length, from_game);
	length += (int)strlen(from_game) + 1;
	memcpy(request + length, list->validate, 8);
	length += 8;
	request[length++] = 0; /* the filter */
	request[length++] = 0; /* the fields */
	memset(request + length, 0, 4);
	length += 4;
	request[0] = (unsigned char)(length >> 8);
	request[1] = (unsigned char)length;

	list->capacity = 64 * 1024;
	list->buffer = malloc(list->capacity);
	list->socket = retail_socket_open(1);
	if (!list->buffer || list->socket == RETAIL_NO_SOCKET || retail_socket_connect(list->socket, master, port) < 0)
	{
		fail(list, "could not connect to the master server");
		return 0;
	}
	/* (the request waits in the buffer until the connection is made) */
	memcpy(list->buffer, request, length);
	list->length = length;
	list->state = _gs_list_connecting;
	return 1;
}

/* a string at p (a 0 within end): its length with the 0, or 0 */
static int string_length(const unsigned char *p, const unsigned char *end)
{
	const unsigned char *s = p;

	while (s < end && *s)
		s++;
	return s < end ? (int)(s - p) + 1 : 0;
}

/* what the decrypted list holds from list->parsed on: the header once, then
whole server records; 1 at the list's end */
static int parse(struct gs_list *list, const unsigned char *data, int size)
{
	const unsigned char *p = data + list->parsed, *end = data + size;

	if (!list->header_read)
	{
		int i, count, n;

		if (end - p < 6)
			return 0;
		if (p[4] == 0xFF && p[5] == 0xFF)
		{
			fail(list, "the master server refused the request");
			return 1;
		}
		p += 6;
		if (p >= end)
			return 0;
		count = *p++;
		list->field_count = count;
		for (i = 0; i < count; i++)
		{
			if (p >= end)
				return 0;
			if (i < 32)
				list->field_types[i] = *p;
			p++;
			if (!(n = string_length(p, end)))
				return 0;
			p += n;
		}
		if (p >= end)
			return 0;
		count = *p++;
		for (i = 0; i < count; i++)
		{
			if (!(n = string_length(p, end)))
				return 0;
			p += n;
		}
		list->header_read = 1;
		list->parsed = (int)(p - data);
	}
	for (;;)
	{
		const unsigned char *record = p;
		int flags, extra, i;
		struct gs_list_server server;

		if (p >= end)
			return 0;
		flags = *p++;
		if (!flags)
		{
			if (end - p < 4)
				return 0;
			if (!memcmp(p, "\xFF\xFF\xFF\xFF", 4))
				return 1;
		}
		if (end - p < 4)
			return 0;
		memcpy(&server.address, p, 4);
		server.port = 0;
		p += 4;
		if (flags & 0x10)
		{
			if (end - p < 2)
				return 0;
			server.port = (uint16_t)(p[0] << 8 | p[1]);
			p += 2;
		}
		extra = (flags & 0x02 ? 4 : 0) + (flags & 0x08 ? 4 : 0) + (flags & 0x20 ? 2 : 0);
		if (end - p < extra)
			return 0;
		p += extra;
		if (flags & 0x40)
		{
			for (i = 0; i < list->field_count; i++)
			{
				int value;

				if (p >= end)
					return 0;
				value = *p++;
				if (i < 32 && !list->field_types[i] && value == 0xFF)
				{
					int n = string_length(p, end);

					if (!n)
						return 0;
					p += n;
				}
			}
		}
		if (!server.port)
		{
			/* (the usual port: the header's) */
			server.port = (uint16_t)(data[4] << 8 | data[5]);
		}
		if (list->count < list->maximum)
			list->servers[list->count++] = server;
		list->parsed = (int)(p - data);
		(void)record;
	}
}

int gs_list_poll(struct gs_list *list, unsigned long now_ms)
{
	enctypex_data_t *decoder = (enctypex_data_t *)list->decoder;

	if ((list->state == _gs_list_connecting || list->state == _gs_list_receiving) &&
		now_ms - list->started_ms > GS_LIST_TIMEOUT_MS)
	{
		fail(list, list->state == _gs_list_connecting ? "the master server did not answer" :
			"the master server's list did not end");
	}
	if (list->state == _gs_list_connecting)
	{
		/* (the request sent once the connection is made: until then the
		send is answered "not yet") */
		int sent = retail_socket_send(list->socket, list->buffer, list->length);

		if (sent == list->length)
		{
			list->length = 0;
			list->state = _gs_list_receiving;
		}
		else if (sent >= 0 || !retail_socket_pending())
			fail(list, "could not send the request");
	}
	if (list->state == _gs_list_receiving)
	{
		for (;;)
		{
			int got;
			unsigned char *data;
			int length;

			if (list->length == list->capacity)
			{
				fail(list, "the list is too long");
				break;
			}
			got = retail_socket_receive(list->socket, list->buffer + list->length, list->capacity - list->length);
			if (got < 0 && retail_socket_pending())
				break;
			if (got <= 0)
			{
				fail(list, got ? "the master server's connection failed" :
					"the master server closed the connection before the list's end");
				break;
			}
			list->length += got;
			length = list->length;
			data = enctypex_decoder((unsigned char *)list->game_key, list->validate, list->buffer, &length, decoder);
			if (!data)
				continue; /* (the encryption's header not whole yet) */
			if (parse(list, data, decoder->offset - decoder->start))
			{
				if (list->state != _gs_list_failed)
				{
					list->state = _gs_list_done;
					retail_socket_close(list->socket);
					list->socket = RETAIL_NO_SOCKET;
				}
				break;
			}
		}
	}
	return list->state;
}

void gs_list_end(struct gs_list *list)
{
	retail_socket_close(list->socket);
	list->socket = RETAIL_NO_SOCKET;
	if (list->buffer)
		free(list->buffer);
	list->buffer = NULL;
}
