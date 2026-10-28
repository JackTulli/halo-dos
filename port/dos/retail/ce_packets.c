/*
CE_PACKETS.C

Halo PC / Custom Edition's network game packets (ce_packets.h): the Xbox
game's data packets with the PC's types and fields, read from the
01.00.10.0621 programs (S\ce-work\dump_packets.py lists them), and the
message streams that carry them.
*/

#include "ce_packets.h"

#include <string.h>

enum
{
	_field_pad,
	_field_bytes,
	_field_shorts,
	_field_longs,
	_field_int64s,
	_field_string,
	_field_data,
	_field_array,
	_field_raw,
	_field_end
};

struct field
{
	unsigned char type;
	short count;
};

#define F(type, count) { type, count }
#define END { _field_end, 0 }
/* a player: name, two shorts, four bytes */
#define PLAYER F(_field_shorts, 12), F(_field_shorts, 2), F(_field_bytes, 4)

static const struct field f_broadcast_game_search[] = { F(_field_shorts, 2), F(_field_bytes, 8), END };
static const struct field f_one_long[] = { F(_field_longs, 1), END };
static const struct field f_two_longs[] = { F(_field_longs, 1), F(_field_longs, 1), END };
static const struct field f_one_short[] = { F(_field_shorts, 1), END };
static const struct field f_game_advertise[] = { F(_field_raw, 368), END };
static const struct field f_new_client_challenge[] = { F(_field_bytes, 8), F(_field_bytes, 1), F(_field_pad, 3),
	F(_field_longs, 1), F(_field_longs, 1), F(_field_string, 127), END };
static const struct field f_machine_accepted[] = { F(_field_longs, 1), F(_field_shorts, 1), F(_field_pad, 2), END };
static const struct field f_game_settings_update[] = { F(_field_raw, 1008), END };
static const struct field f_join_game_request[] = { F(_field_bytes, 16), F(_field_shorts, 9), F(_field_bytes, 73),
	F(_field_bytes, 1), F(_field_pad, 2), PLAYER, F(_field_pad, 2), F(_field_longs, 1), END };
static const struct field f_player[] = { PLAYER, END };
static const struct field f_remove_player_ingame[] = { PLAYER, F(_field_longs, 1), END };
static const struct field f_map_is_precached[] = { F(_field_bytes, 256), END };
static const struct field f_host_crashed[] = { F(_field_longs, 3), F(_field_shorts, 1), F(_field_pad, 2), END };
static const struct field f_reconnect[] = { F(_field_longs, 2), END };

struct definition
{
	const char *name;
	const struct field *fields; /* NULL: not supported here */
};

static const struct definition definitions[CE_PACKET_TYPE_COUNT] =
{
	{ "client_broadcast_game_search", f_broadcast_game_search },
	{ "client_ping", f_one_long },
	{ "server_game_advertise", f_game_advertise },
	{ "server_pong", f_two_longs },
	{ "server_new_client_challenge", f_new_client_challenge },
	{ "server_machine_accepted", f_machine_accepted },
	{ "server_machine_rejected", f_one_long },
	{ "server_game_is_ending_holdup", f_one_long },
	{ "server_game_settings_update", f_game_settings_update },
	{ "server_pregame_countdown", f_one_short },
	{ "server_begin_game", f_one_long },
	{ "server_graceful_game_exit_pregame", f_one_long },
	{ "server_pregame_keep_alive", f_one_short },
	{ "server_postgame_keep_alive", f_one_short },
	{ "client_join_game_request", f_join_game_request },
	{ "client_add_player_request_pregame", f_player },
	{ "client_remove_player_request_pregame", f_player },
	{ "client_settings_request", f_one_long },
	{ "client_player_settings_request", f_player },
	{ "client_game_start_request", f_one_short },
	{ "client_graceful_game_exit_pregame", f_one_long },
	{ "client_map_is_precached_pregame", f_map_is_precached },
	/* (the Xbox's per-tick updates: arrays, unused by the PC game) */
	{ "server_game_update", NULL },
	{ "server_add_player_ingame", f_player },
	{ "server_remove_player_ingame", f_remove_player_ingame },
	{ "server_game_over", f_one_long },
	{ "client_loaded", f_one_long },
	{ "client_game_update", NULL },
	{ "client_add_player_request_ingame", f_player },
	{ "client_remove_player_request_ingame", f_player },
	{ "client_graceful_game_exit_ingame", f_one_long },
	{ "client_host_crashed_cry_for_help", f_host_crashed },
	{ "client_join_new_host", f_host_crashed },
	{ "server_reconnect", f_reconnect },
	{ "server_graceful_game_exit_postgame", f_one_long },
	{ "client_remove_player_request_postgame", f_player },
	{ "client_switch_to_pregame", f_one_long },
	{ "client_graceful_game_exit_postgame", f_one_long },
};

/* every packet is version 1 */
#define PACKET_VERSION 1

const char *ce_packet_name(int type)
{
	return type >= 0 && type < CE_PACKET_TYPE_COUNT ? definitions[type].name : "?";
}

static int field_size(const struct field *field)
{
	switch (field->type)
	{
	case _field_pad:
	case _field_bytes:
	case _field_raw:
		return field->count;
	case _field_shorts:
		return field->count * 2;
	case _field_longs:
		return field->count * 4;
	case _field_int64s:
		return field->count * 8;
	case _field_string:
		return field->count + 1;
	default:
		return -1;
	}
}

int ce_packet_decoded_size(int type)
{
	const struct field *field;
	int size = 0;

	if (type < 0 || type >= CE_PACKET_TYPE_COUNT || !definitions[type].fields)
		return -1;
	for (field = definitions[type].fields; field->type != _field_end; field++)
	{
		int one = field_size(field);

		if (one < 0)
			return -1;
		size += one;
	}
	return size;
}

/* numbers big-endian on the wire, host order (little-endian) decoded */
static void swap_copy(unsigned char *to, const unsigned char *from, int count, int width)
{
	int i, j;

	for (i = 0; i < count; i++)
		for (j = 0; j < width; j++)
			to[i * width + j] = from[i * width + width - 1 - j];
}

int ce_packet_encode(int type, const void *decoded, unsigned char *out, int maximum)
{
	const unsigned char *in = (const unsigned char *)decoded;
	const struct field *field;
	int size = 3, header;

	if (ce_packet_decoded_size(type) < 0 || maximum < 4)
		return -1;
	out[2] = PACKET_VERSION;
	for (field = definitions[type].fields; field->type != _field_end; field++)
	{
		int one = field_size(field);

		switch (field->type)
		{
		case _field_bytes:
		case _field_raw:
			if (size + one > maximum)
				return -1;
			memcpy(out + size, in, one);
			size += one;
			break;
		case _field_shorts:
		case _field_longs:
		case _field_int64s:
			if (size + one > maximum)
				return -1;
			swap_copy(out + size, in, field->count, one / field->count);
			size += one;
			break;
		case _field_string:
		{
			int length = 0;

			while (length < field->count && in[length])
				length++;
			if (size + length + 1 > maximum)
				return -1;
			memcpy(out + size, in, length);
			out[size + length] = 0;
			size += length + 1;
			break;
		}
		default:
			break;
		}
		in += one;
	}
	if (size + 1 > maximum || size + 1 > 0xFFF)
		return -1;
	out[size++] = (unsigned char)type;
	header = 0 | _ce_message_packet << 2 | size << 4;
	out[0] = (unsigned char)header;
	out[1] = (unsigned char)(header >> 8);
	return size;
}

int ce_packet_decode(const unsigned char *message, int size, int *type, void *decoded, int maximum)
{
	unsigned char *out = (unsigned char *)decoded;
	const struct field *field;
	int message_type, offset = 3, decoded_size;

	if (ce_message_size(message, size, &message_type) != size || message_type != _ce_message_packet || size < 4)
		return -1;
	*type = message[size - 1];
	decoded_size = ce_packet_decoded_size(*type);
	if (decoded_size < 0 || decoded_size > maximum || message[2] != PACKET_VERSION)
		return -1;
	memset(out, 0, decoded_size);
	size--;
	for (field = definitions[*type].fields; field->type != _field_end; field++)
	{
		int one = field_size(field);

		switch (field->type)
		{
		case _field_bytes:
		case _field_raw:
			if (offset + one > size)
				return -1;
			memcpy(out, message + offset, one);
			offset += one;
			break;
		case _field_shorts:
		case _field_longs:
		case _field_int64s:
			if (offset + one > size)
				return -1;
			swap_copy(out, message + offset, field->count, one / field->count);
			offset += one;
			break;
		case _field_string:
		{
			int length = 0;

			while (offset + length < size && message[offset + length] && length < field->count)
				length++;
			if (offset + length >= size || message[offset + length])
				return -1;
			memcpy(out, message + offset, length);
			offset += length + 1;
			break;
		}
		default:
			break;
		}
		out += one;
	}
	return offset == size ? 0 : -1;
}

int ce_message_size(const unsigned char *messages, int size, int *message_type)
{
	int header, length;

	if (size < 2)
		return 0;
	header = messages[0] | messages[1] << 8;
	length = header >> 4;
	*message_type = (header >> 2) & 3;
	return length >= 2 && length <= size ? length : 0;
}
