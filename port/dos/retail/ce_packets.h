/*
CE_PACKETS.H

Halo PC / Custom Edition's network game packets and the message streams
that carry them (ce_packets.c): the joining and pregame conversation before
the game's own message deltas take over.

A connection's application data is a bit stream of items (ce_delta.h):
message deltas and these packets. A packet's message starts with a 16-bit
header (2 bits of flags, 2 bits of type -- 3 for a packet -- and 12 bits of
size in bytes, counting the header) and then holds its version byte, its
fields as the Xbox game encodes them (big-endian numbers, strings to their
terminator) and its type in a last byte. The definitions are the 01.00.10.0621
programs' own (tools: S\ce-work\dump_packets.py).
*/

#ifndef __CE_PACKETS_H
#define __CE_PACKETS_H

#include <stdint.h>

enum ce_packet_type
{
	_ce_packet_client_broadcast_game_search = 0,
	_ce_packet_client_ping,
	_ce_packet_server_game_advertise,
	_ce_packet_server_pong,
	_ce_packet_server_new_client_challenge,
	_ce_packet_server_machine_accepted,
	_ce_packet_server_machine_rejected,
	_ce_packet_server_game_is_ending_holdup,
	_ce_packet_server_game_settings_update,
	_ce_packet_server_pregame_countdown,
	_ce_packet_server_begin_game,
	_ce_packet_server_graceful_game_exit_pregame,
	_ce_packet_server_pregame_keep_alive,
	_ce_packet_server_postgame_keep_alive,
	_ce_packet_client_join_game_request,
	_ce_packet_client_add_player_request_pregame,
	_ce_packet_client_remove_player_request_pregame,
	_ce_packet_client_settings_request,
	_ce_packet_client_player_settings_request,
	_ce_packet_client_game_start_request,
	_ce_packet_client_graceful_game_exit_pregame,
	_ce_packet_client_map_is_precached_pregame,
	_ce_packet_server_game_update,
	_ce_packet_server_add_player_ingame,
	_ce_packet_server_remove_player_ingame,
	_ce_packet_server_game_over,
	_ce_packet_client_loaded,
	_ce_packet_client_game_update,
	_ce_packet_client_add_player_request_ingame,
	_ce_packet_client_remove_player_request_ingame,
	_ce_packet_client_graceful_game_exit_ingame,
	_ce_packet_client_host_crashed_cry_for_help,
	_ce_packet_client_join_new_host,
	_ce_packet_server_reconnect,
	_ce_packet_server_graceful_game_exit_postgame,
	_ce_packet_client_remove_player_request_postgame,
	_ce_packet_client_switch_to_pregame,
	_ce_packet_client_graceful_game_exit_postgame,
	CE_PACKET_TYPE_COUNT
};

/* the message header's types */
enum
{
	_ce_message_none = 0,
	_ce_message_lowlevel_error,
	_ce_message_data,
	_ce_message_packet
};

/* a player as the packets carry one (32 bytes) */
struct ce_network_player
{
	uint16_t name[12]; /* UTF-16 */
	int16_t primary_color_index;
	int16_t icon_index;
	int8_t machine_index;
	int8_t controller_index;
	int8_t team_index;
	int8_t player_list_index;
};

struct ce_packet_new_client_challenge
{
	char challenge[8];
	uint8_t unknown_8;
	uint8_t pad[3];
	int32_t unknown_c;
	int32_t unknown_10;
	char map_name[128];
};

struct ce_packet_join_game_request
{
	uint8_t token[16];
	uint16_t password[9]; /* UTF-16 */
	char key_response[73];
	uint8_t connection_class;
	uint8_t pad[2];
	struct ce_network_player player;
	uint8_t pad2[2];
	int32_t unknown_90;
};

struct ce_packet_machine_accepted
{
	int32_t unknown_0;
	int16_t machine_index;
	uint8_t pad[2];
};

/* the packet types' names, for logs */
const char *ce_packet_name(int type);
/* a packet's decoded size (its structure's), or -1 */
int ce_packet_decoded_size(int type);

/* encodes a packet as a whole message (header, version, fields, type):
its size in bytes, or -1 */
int ce_packet_encode(int type, const void *decoded, unsigned char *out, int maximum);
/* decodes a message holding a packet: 0 (type and decoded filled), or -1 */
int ce_packet_decode(const unsigned char *message, int size, int *type, void *decoded, int maximum);
/* a message's size (from its header), or 0 */
int ce_message_size(const unsigned char *messages, int size, int *message_type);

/* (streams themselves -- an 11-bit size, then items each led by a kind
bit: 1 a message delta, 0 one of these packets, read 8 bits at a time --
are ce_delta.h's ce_stream_* functions) */

#endif
