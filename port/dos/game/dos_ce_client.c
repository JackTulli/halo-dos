/*
DOS_CE_CLIENT.C

Playing on a real Halo Custom Edition server (port/dos/retail: ce_session.c
joins it and decodes its message deltas). Beside the game's own netcode
(ChupathingyCE's Delta, dos_network.c), not instead of it: while a CE game
runs, the engine is a distributed client (game_connection 1, so it skips
what a host decides: AI, damage, spawning, scoring) without a Delta
connection, and this file plays the host's part from the CE server's
messages. The plan: port/dos/retail/CE_CLIENT_DESIGN.md; how the real client
handles every message: port/dos/retail/CE_CLIENT_HANDLERS.md (its section
numbers are cited below).

The server names its objects by a network index (0..511, 0 none) and its
players by one too; the tables below turn them into this game's datums.
Objects' definitions are tag datums of the CE map, which is why the very
same map file is loaded (MAPS_CE, ce_maps.c).

Nothing is acknowledged in CE's message deltas: every baseline comes
reliably, incremental updates unreliably naming the baseline they were made
against, and one against another baseline (or older than the last) is
dropped (3.1, 3.3). Our own input goes reliably, each update against the one
before it, one a frame (5.1); the server answers with our unit's position
after a numbered update (3.5), which corrects ours by the difference to
where we were then (6, without re-simulating the updates since).

Joining: the ONLINE PLAY list (dos_network.c), or the setting
ce_connect=address[:port] at start (tests); ce_password for passwords.
*/

#include "cseries.h"
#include "cseries/errors.h"

#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "main/main.h"
#include "objects/objects.h"
#include "items/weapons.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "tag_files/tag_groups.h"
#include "units/units.h"

#include "halo_system.h"
#include "dos_map_download.h"
#include "../retail/ce_session.h"
#include "../retail/ce_delta_tables.h"
#include "../retail/ce_packets.h"
#include "../retail/retail_socket.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *dos_setting(const char *name, const char *default_value);
long dos_setting_integer(const char *name, long default_value);
void hud_print_message(short local_player_index, wchar_t const *message_text);
boolean game_in_progress(void);
boolean game_map_loaded(void);
void game_dispose_from_old_map(void);
void game_unload(void);
void main_menu_unload(void);
void game_initialize_for_new_map(void);
void game_initial_pulse(void);
void game_time_start(void);
long game_time_get(void);
void local_player_set_player_index(short local_player_index, long player_index);
void update_client_add_player(long player_index);
void game_engine_player_added(long player_index);
void game_engine_end_game(void);
void network_player_attach_unit(long player_index, long unit_index);
void network_player_detach_unit(long player_index);
boolean update_client_distributed_input(short local_player_index, long *tick, struct player_action *action,
	unsigned short *control_flags);
void update_client_handle_relayed_action(short player_index, long update_number, struct player_action const *action,
	unsigned short const *control_flags, short count);
boolean network_objects_reconcile(long object_index, real_point3d const *position, real_vector3d const *forward,
	real_vector3d const *up, real_vector3d const *velocity, real_vector3d const *angular_velocity,
	real blend_distance);
void network_objects_correct(long object_index, real_point3d const *position, real_vector3d const *forward,
	real_vector3d const *up, real_vector3d const *velocity, real_vector3d const *angular_velocity);
void network_objects_set_seat(long unit_index, long vehicle_index, short seat_index);
void unit_network_add_weapon(long unit_index, long weapon_index, short slot);
void unit_network_drop_weapon(long unit_index, short slot);
struct game_variant *game_engine_get_variant_by_name(struct game_variant *variant, char const *name);
void hud_picked_up_weapon(short local_player_index, long weapon_definition_index);
void object_damage_update(long object_index);
long game_engine_write_network_state(byte *buffer, long size);
void game_engine_read_network_state(byte const *buffer, long size);
boolean main_menu_is_active(void);

/* the CE server's limits */
#define CE_NETWORK_OBJECTS 512
#define CE_NETWORK_PLAYERS 17
/* another unit glides to the server's position within this distance */
#define CE_BLEND_DISTANCE 3.0f
/* another player's unit glides to the server's position within this
distance, and is put there beyond it (the real client puts it there beyond
1.1, 3.4, but plays its actions a tick at a time first; this one drives the
unit with the newest action, so it is a little further off between the
server's positions, and gliding hides that) */
#define CE_REMOTE_SNAP 3.0f
/* our own unit's and vehicle's corrections are applied beyond these (6:
the real client replays every later update from the server's position; this
one moves its unit by how far it was off at that update, which the noise of
the positions' encoding alone would trip below these) */
#define CE_OWN_CORRECTION 0.08f
#define CE_OWN_VEHICLE_CORRECTION 0.2f
/* the room an object's or player's baseline takes (the largest: 64 bytes) */
#define CE_BASELINE_SIZE 160
/* our input updates kept until the server acknowledges them (5.1) */
#define CE_HISTORY 64
/* the client's ping of the server (old packet 1), every so often (5.4) */
#define CE_PING_MS 3000

/* (game.c's, as network_game_manager.c copies it) */
struct game_options
{
	unsigned long flags;
	short code_version;
	short difficulty;
	unsigned long random_seed;
	char map_name[256];
};

/* an object the server made (3.1, 3.2) */
struct ce_object
{
	long index; /* NONE: none */
	int has_baseline;
	unsigned char baseline_index, message_index;
	int updated;
	unsigned char baseline[CE_BASELINE_SIZE];
};

/* a player the server made (3.3, 3.4) */
struct ce_player
{
	long index; /* NONE: none */
	int list_index;
	/* actions: the baseline and its id, the last update id (-1 none yet),
	and the tick count fed to the engine's relayed actions */
	unsigned char action_baseline[48];
	int baseline_id, last_update_id;
	long ticks_fed;
	/* positions and vehicles */
	unsigned char position_baseline[12];
	unsigned char vehicle_baseline[64];
	int position_sequence;
};

static struct
{
	int active; /* from the join until the game is left */
	struct ce_session session;
	/* the server (its next game is joined again: dos_ce_frame) */
	uint32_t address;
	uint16_t port;
	char password[9];
	/* a CE map is up (from load_map until the game is left) */
	int map_up;
	/* the server's map being downloaded (dos_map_download.c), the server
	joined again when it is in MAPS_CE; one download a join */
	int downloading, downloaded;
	/* the server's game (its game_settings_update) */
	int have_settings;
	char map_name[128];
	long random_seed;
	short difficulty;
	long game_engine_index;
	struct game_variant variant;
	struct network_player settings_players[16];
	int settings_player_count;
	/* the map loaded, the server told */
	int loaded;
	struct ce_object objects[CE_NETWORK_OBJECTS];
	struct ce_player players[CE_NETWORK_PLAYERS];
	/* the network index of the player this machine plays */
	int local_network_player;
	/* our input: the last update sent (the next one's baseline), the update
	counter, the last tick sent, and the history the server acknowledges */
	unsigned char sent_body[CE_DELTA_MAX_BODY_SIZE + 4];
	int next_update_id;
	long last_sent_tick;
	struct
	{
		int id;
		/* our unit's position after the update's ticks (sent once they ran),
		and the vehicle it drove then (NONE) and that vehicle's */
		real_point3d position;
		long vehicle;
		real_point3d vehicle_position;
	} history[CE_HISTORY];
	int history_count;
	int last_ack_sequence;
	/* scores: each player's last received (the incremental ones' baseline),
	and the game type's (16-20) */
	unsigned char score_baselines[CE_NETWORK_PLAYERS][48];
	unsigned char engine_baselines[5][432];
	unsigned long ping_ms;
	char status[96];
	int connect_tried;
	/* for the log, every 5 seconds */
	unsigned long statistics_ms;
	long own_updates, corrections, inputs_sent, inputs_refused, objects_made, dropped_updates;
	real largest_correction;
} ce;

static unsigned long now_ms(void)
{
	return (unsigned long)(system_microseconds() / 1000);
}

static void ce_log(void *context, const char *line)
{
	(void)context;
	error(3, "%s", line);
}

/* a line on the HUD (chat, the server's message of the day) */
static void chat_note_wide(wchar_t const *text)
{
	char narrow[160];
	int i;

	if (local_player_get_player_index(0) != NONE)
		hud_print_message(0, text);
	for (i = 0; i < 159 && text[i]; i++)
		narrow[i] = text[i] < 128 ? (char)text[i] : '?';
	narrow[i] = 0;
	error(3, "ce: %s", narrow);
}

static void chat_note(char const *text)
{
	wchar_t line[160];
	int i;

	for (i = 0; i < 159 && text[i]; i++)
		line[i] = (wchar_t)(unsigned char)text[i];
	line[i] = 0;
	chat_note_wide(line);
}

/* ---------- fields by name */

static const ce_field_ref *field(const ce_message_header *header, int part, const char *name)
{
	return ce_delta_field(header->def, part, name);
}

/* a body field's index (its presence bit), or -1 */
static int body_index(const ce_message_header *header, const char *name)
{
	int i;

	for (i = 0; i < header->def->body_count; i++)
		if (!strcmp(header->def->body[i].name, name))
			return i;
	return -1;
}

/* a body field sent in this message (all of a stateless one are) */
static int sent(const ce_message_header *header, const unsigned char *present, const char *name)
{
	int index = body_index(header, name);

	return index >= 0 && (header->mode == 0 || !present || present[index]);
}

static long get_int(const ce_message_header *header, int part, const void *data, const char *name, long otherwise)
{
	const ce_field_ref *ref = field(header, part, name);

	return ref ? ce_get_int(ref->props, ce_field_ptr(data, ref)) : otherwise;
}

static unsigned long get_uint(const ce_message_header *header, int part, const void *data, const char *name)
{
	const ce_field_ref *ref = field(header, part, name);

	return ref ? ce_get_uint(ref->props, ce_field_ptr(data, ref)) : 0;
}

static real get_real(const ce_message_header *header, int part, const void *data, const char *name)
{
	const ce_field_ref *ref = field(header, part, name);

	return ref ? ce_get_real(ref->props, ce_field_ptr(data, ref)) : 0.0f;
}

static int get_vector(const ce_message_header *header, int part, const void *data, const char *name, real out[3])
{
	const ce_field_ref *ref = field(header, part, name);

	out[0] = out[1] = out[2] = 0.0f;
	if (!ref)
		return FALSE;
	ce_get_vector(ref->props, ce_field_ptr(data, ref), out);
	return TRUE;
}

static void set_uint(const ce_message_header *header, int part, void *data, const char *name, unsigned long value)
{
	const ce_field_ref *ref = field(header, part, name);

	if (ref)
		ce_set_uint(ref->props, ce_field_ptr(data, ref), value);
}

static void set_real(const ce_message_header *header, int part, void *data, const char *name, real value)
{
	const ce_field_ref *ref = field(header, part, name);

	if (ref)
		ce_set_real(ref->props, ce_field_ptr(data, ref), value);
}

static void point_of(const real v[3], real_point3d *p)
{
	p->x = v[0];
	p->y = v[1];
	p->z = v[2];
}

static void vector_of(const real v[3], real_vector3d *p)
{
	p->i = v[0];
	p->j = v[1];
	p->k = v[2];
}

/* ---------- objects */

static long object_of(long network_index)
{
	if (network_index <= 0 || network_index >= CE_NETWORK_OBJECTS)
		return NONE;
	if (ce.objects[network_index].index != NONE && !object_try_and_get(ce.objects[network_index].index))
		ce.objects[network_index].index = NONE;
	return ce.objects[network_index].index;
}

/* the server's index freed (deleted, killed, detonated): the object, if it
stays, is ours alone (4: unit_kill) */
static void forget_object(long network_index)
{
	if (network_index <= 0 || network_index >= CE_NETWORK_OBJECTS)
		return;
	ce.objects[network_index].index = NONE;
	ce.objects[network_index].has_baseline = FALSE;
	ce.objects[network_index].updated = FALSE;
}

static void delete_object(long network_index)
{
	long object_index = object_of(network_index);

	forget_object(network_index);
	if (object_index != NONE)
		object_delete(object_index);
}

/* the object update message of a *_new message's type (3.2) */
static int update_type_of(int new_type)
{
	switch (new_type)
	{
	case CE_MSG_PROJECTILE_NEW: return CE_MSG_PROJECTILE_UPDATE;
	case CE_MSG_EQUIPMENT_NEW: return CE_MSG_EQUIPMENT_UPDATE;
	case CE_MSG_WEAPON_NEW: return CE_MSG_WEAPON_UPDATE;
	case CE_MSG_BIPED_NEW: return CE_MSG_BIPED_UPDATE;
	case CE_MSG_VEHICLE_NEW: return CE_MSG_VEHICLE_UPDATE;
	default: return NONE;
	}
}

/* a *_new message's values as the object's first baseline: the update's
fields taken from the same-named fields of the creation (3.2) */
static void baseline_from_new(struct ce_object *object, const ce_message_header *header, const void *body)
{
	int type = update_type_of(header->type), i;
	const ce_message_def *update;

	memset(object->baseline, 0, sizeof(object->baseline));
	object->has_baseline = FALSE;
	if (type == NONE || !(update = ce_delta_message(type)) || update->body_size > CE_BASELINE_SIZE)
		return;
	for (i = 0; i < update->body_count; i++)
	{
		const ce_field_ref *to = &update->body[i];
		const ce_field_ref *from = ce_delta_field(header->def, 1, to->name);

		if (from && from->props->size == to->props->size)
			memcpy(object->baseline + to->offset, (const char *)body + from->offset, to->props->size);
	}
	object->baseline_index = (unsigned char)get_int(header, 1, body, "current_baseline_index", 0);
	object->message_index = 0;
	object->has_baseline = TRUE;
	object->updated = FALSE;
}

/* a map's multiplayer item's first baseline, made here from the object as
the server makes its own when it places the item: the first after none
(index 1), message 0 (3.2) */
static void baseline_from_object(struct ce_object *tracked, long object_index)
{
	struct object_datum *object = object_get(object_index);
	int type = object_try_and_get_and_verify_type(object_index, FLAG(_object_type_weapon)) ? CE_MSG_WEAPON_UPDATE :
		CE_MSG_EQUIPMENT_UPDATE;
	ce_message_header header;
	real values[3];

	memset(&header, 0, sizeof(header));
	header.def = ce_delta_message(type);
	header.type = type;
	memset(tracked->baseline, 0, sizeof(tracked->baseline));
	tracked->has_baseline = FALSE;
	if (!header.def || header.def->body_size > CE_BASELINE_SIZE)
		return;
	{
		const ce_field_ref *position = field(&header, 1, "position");
		const ce_field_ref *velocity = field(&header, 1, "translational_velocity");
		const ce_field_ref *angular = field(&header, 1, "angular_velocity");

		values[0] = object->object.position.x;
		values[1] = object->object.position.y;
		values[2] = object->object.position.z;
		if (position)
			ce_set_vector(position->props, ce_field_ptr(tracked->baseline, position), values);
		values[0] = object->object.translational_velocity.i;
		values[1] = object->object.translational_velocity.j;
		values[2] = object->object.translational_velocity.k;
		if (velocity)
			ce_set_vector(velocity->props, ce_field_ptr(tracked->baseline, velocity), values);
		values[0] = object->object.angular_velocity.i;
		values[1] = object->object.angular_velocity.j;
		values[2] = object->object.angular_velocity.k;
		if (angular)
			ce_set_vector(angular->props, ce_field_ptr(tracked->baseline, angular), values);
	}
	if (type == CE_MSG_WEAPON_UPDATE)
	{
		struct weapon_datum *weapon = (struct weapon_datum *)object;

		set_uint(&header, 1, tracked->baseline, "magazine_0_rounds_total",
			(unsigned long)(long)weapon->weapon.magazines[0].rounds_total);
		set_uint(&header, 1, tracked->baseline, "magazine_1_rounds_total",
			(unsigned long)(long)weapon->weapon.magazines[1].rounds_total);
		set_real(&header, 1, tracked->baseline, "age", weapon->weapon.age);
	}
	tracked->baseline_index = 1;
	tracked->message_index = 0;
	tracked->has_baseline = TRUE;
	tracked->updated = FALSE;
}

/* a weapon's rounds as the server has them; a reloading magazine keeps its
own (3.1 weapon) */
static void set_weapon_rounds(long object_index, const ce_message_header *header, const unsigned char *present,
	const void *body, int creation)
{
	static const char *const totals[2] = { "magazine_0_rounds_total", "magazine_1_rounds_total" };
	static const char *const loaded[2] = { "magazine_0_rounds_loaded", "magazine_1_rounds_loaded" };
	struct weapon_datum *weapon;
	int i;

	if (!object_try_and_get_and_verify_type(object_index, FLAG(_object_type_weapon)))
		return;
	weapon = (struct weapon_datum *)object_get(object_index);
	for (i = 0; i < 2; i++)
	{
		if (sent(header, present, totals[i]) && (creation || weapon->weapon.magazines[i].state != 1))
			weapon->weapon.magazines[i].rounds_total = (short)get_int(header, 1, body, totals[i], 0);
		if (creation && sent(header, present, loaded[i]))
			weapon->weapon.magazines[i].rounds_loaded = (short)get_int(header, 1, body, loaded[i], 0);
	}
}

/* a unit's health and shield, grenades (biped_new, biped_update) */
static void set_unit_vitality(long object_index, const ce_message_header *header, const unsigned char *present,
	const void *body, int shield_too)
{
	struct object_datum *object;

	if (!object_try_and_get_and_verify_type(object_index, _object_mask_unit))
		return;
	object = object_get(object_index);
	if (sent(header, present, "body_vitality"))
		object->object.body_vitality = get_real(header, 1, body, "body_vitality");
	if (shield_too && sent(header, present, "shield_vitality"))
		object->object.shield_vitality = get_real(header, 1, body, "shield_vitality") * 3.0f;
	if (sent(header, present, "grenade_counts"))
	{
		struct unit_datum *unit = (struct unit_datum *)object;
		unsigned long counts = get_uint(header, 1, body, "grenade_counts");

		/* (the unit's two grenade counts, a byte each) */
		unit->unit.grenade_counts[0] = (char)(counts & 0xFF);
		unit->unit.grenade_counts[1] = (char)(counts >> 8);
	}
}

static void create_object(const ce_message_header *header, const void *body)
{
	struct object_placement_data data;
	long network_index = get_int(header, 1, body, "server_object_index", 0);
	long definition_index = (long)get_uint(header, 1, body, "definition_index");
	long object_index;
	real position[3], forward[3], up[3], velocity[3], angular[3];

	if (network_index <= 0 || network_index >= CE_NETWORK_OBJECTS)
		return;
	if (object_of(network_index) != NONE)
		delete_object(network_index);
	/* (a map's multiplayer item: its definition, and where the map places it) */
	if (header->type == CE_MSG_NETGAME_EQUIPMENT_NEW)
		definition_index = (long)get_uint(header, 1, body, "item_definition_index");
	object_placement_data_new(&data, definition_index, NONE);
	if (header->type == CE_MSG_NETGAME_EQUIPMENT_NEW)
	{
		struct scenario *scenario = global_scenario_get();
		long placement = get_int(header, 1, body, "netgame_equipment_index", NONE);

		if (scenario && placement >= 0 && placement < scenario->netgame_equipment.count)
		{
			struct scenario_netgame_equipment *equipment = TAG_BLOCK_GET_ELEMENT(&scenario->netgame_equipment,
				placement, struct scenario_netgame_equipment);

			data.position = equipment->position;
			data.forward.i = (real)cos(equipment->facing);
			data.forward.j = (real)sin(equipment->facing);
			data.forward.k = 0.0f;
		}
	}
	if (get_vector(header, 1, body, "position", position))
		point_of(position, &data.position);
	if (get_vector(header, 1, body, "forward", forward) && get_vector(header, 1, body, "up", up) &&
		(forward[0] || forward[1] || forward[2]))
	{
		vector_of(forward, &data.forward);
		vector_of(up, &data.up);
	}
	if (get_vector(header, 1, body, "translational_velocity", velocity))
		vector_of(velocity, &data.translational_velocity);
	if (get_vector(header, 1, body, "angular_velocity", angular))
		vector_of(angular, &data.angular_velocity);
	data.owner_team_index = (short)get_int(header, 1, body, "owner_team_index", NONE);
	object_index = object_new(&data);
	if (object_index == NONE)
	{
		error(3, "ce: could not make %s %ld (definition %08lX)", header->def->name, network_index,
			(unsigned long)definition_index);
		return;
	}
	ce.objects[network_index].index = object_index;
	if (header->type == CE_MSG_NETGAME_EQUIPMENT_NEW)
		baseline_from_object(&ce.objects[network_index], object_index);
	else
		baseline_from_new(&ce.objects[network_index], header, body);
	ce.objects_made++;
	if (header->type == CE_MSG_WEAPON_NEW)
	{
		set_weapon_rounds(object_index, header, NULL, body, TRUE);
		((struct weapon_datum *)object_get(object_index))->weapon.age = get_real(header, 1, body, "age");
	}
	if (header->type == CE_MSG_BIPED_NEW)
	{
		const ce_field_ref *colors = field(header, 1, "change_colors");

		set_unit_vitality(object_index, header, NULL, body, TRUE);
		/* (its armour: the colours the server gave it, its team's in a team
		game, the player's own in free for all; four RGB colours) */
		if (colors && colors->props->size == sizeof(object_get(object_index)->object.base_change_colors))
		{
			struct object_datum *object = object_get(object_index);
			int i;

			memcpy(object->object.base_change_colors, ce_field_ptr(body, colors), colors->props->size);
			for (i = 0; i < NUMBER_OF_OBJECT_CHANGE_COLORS; i++)
			{
				object->object.outgoing_change_colors[i].red = PIN(object->object.base_change_colors[i].red, 0.0f, 1.0f);
				object->object.outgoing_change_colors[i].green = PIN(object->object.base_change_colors[i].green, 0.0f,
					1.0f);
				object->object.outgoing_change_colors[i].blue = PIN(object->object.base_change_colors[i].blue, 0.0f, 1.0f);
			}
		}
	}
	/* (a vehicle's own weapons: its turrets, 3.2) */
	if (header->type == CE_MSG_VEHICLE_NEW)
	{
		static const char *const weapons[4] = { "weapon_object_indices[0]", "weapon_object_indices[1]",
			"weapon_object_indices[2]", "weapon_object_indices[3]" };
		int i;

		for (i = 0; i < 4; i++)
		{
			long weapon = object_of(get_int(header, 1, body, weapons[i], 0));

			if (weapon != NONE)
				unit_network_add_weapon(object_index, weapon, (short)i);
		}
	}
}

/* an object update: dropped when made against another baseline or older
than the last; kept as the baseline when it says so; applied (3.1) */
static void update_object(const ce_message_header *header, const unsigned char *present, const void *header_fields,
	const void *body)
{
	long network_index = get_int(header, 0, header_fields, "object_index", 0);
	long object_index = object_of(network_index);
	struct ce_object *tracked;
	struct object_datum *object;
	unsigned char baseline_index, message_index;
	real values[3];
	real_point3d where;
	real_vector3d f, u, v, a;
	int moved = FALSE;

	if (object_index == NONE)
		return;
	tracked = &ce.objects[network_index];
	baseline_index = (unsigned char)get_int(header, 0, header_fields, "baseline_index", 0);
	message_index = (unsigned char)get_int(header, 0, header_fields, "message_index", 0);
	if (tracked->updated && header->mode == 1 &&
		(baseline_index != tracked->baseline_index ||
			(message_index <= tracked->message_index && tracked->message_index - message_index <= 225)))
	{
		ce.dropped_updates++;
		return;
	}
	tracked->message_index = message_index;
	tracked->updated = TRUE;
	if (get_int(header, 0, header_fields, "update_baseline", 0) && header->def->body_size <= CE_BASELINE_SIZE)
	{
		tracked->baseline_index = baseline_index;
		memcpy(tracked->baseline, body, header->def->body_size);
		tracked->has_baseline = TRUE;
	}
	object = object_get(object_index);
	where = object->object.position;
	f = object->object.forward;
	u = object->object.up;
	v = object->object.translational_velocity;
	a = object->object.angular_velocity;
	/* (bipeds are moved by the players' messages, 3.4; a weapon in a unit's
	hands, or anything else not in the world, by what holds it) */
	if (header->type != CE_MSG_BIPED_UPDATE && object->object.parent_object_index == NONE &&
		TEST_FLAG(object->object.flags, _object_connected_to_map_bit) && sent(header, present, "position") &&
		get_vector(header, 1, body, "position", values))
	{
		point_of(values, &where);
		moved = TRUE;
	}
	if (sent(header, present, "forward") && get_vector(header, 1, body, "forward", values) &&
		(values[0] || values[1] || values[2]))
		vector_of(values, &f);
	if (sent(header, present, "up") && get_vector(header, 1, body, "up", values) && (values[0] || values[1] || values[2]))
		vector_of(values, &u);
	if (sent(header, present, "translational_velocity") && get_vector(header, 1, body, "translational_velocity", values))
		vector_of(values, &v);
	if (sent(header, present, "angular_velocity") && get_vector(header, 1, body, "angular_velocity", values))
		vector_of(values, &a);
	if (moved)
		network_objects_reconcile(object_index, &where, &f, &u, &v, &a, CE_BLEND_DISTANCE);
	if (header->type == CE_MSG_WEAPON_UPDATE)
	{
		set_weapon_rounds(object_index, header, present, body, FALSE);
		if (header->mode == 0)
			((struct weapon_datum *)object)->weapon.age = get_real(header, 1, body, "age");
	}
	if (header->type == CE_MSG_BIPED_UPDATE)
		set_unit_vitality(object_index, header, present, body,
			(int)get_int(header, 0, header_fields, "force_shield_update", 0));
}

/* ---------- players */

static long player_of(long network_index)
{
	if (network_index <= 0 || network_index >= CE_NETWORK_PLAYERS)
		return NONE;
	return ce.players[network_index].index;
}

static long unit_of_player(long player_index)
{
	struct player_datum *player;

	if (player_index == NONE)
		return NONE;
	player = player_get(player_index);
	if (player->unit_index == NONE || !object_try_and_get(player->unit_index))
		return NONE;
	return player->unit_index;
}

static void create_player(const ce_message_header *header, const void *body)
{
	struct network_player network_player;
	long network_index = get_int(header, 1, body, "translated_player_index", 0);
	long list_index = get_int(header, 1, body, "player_list_index", 0);
	short local_player_index = NONE;
	long player_index;
	int i;

	if (network_index <= 0 || network_index >= CE_NETWORK_PLAYERS || player_of(network_index) != NONE)
		return;
	memset(&network_player, 0, sizeof(network_player));
	/* (the game settings' entry: its name, colour and machine; ours is the
	one on the machine the server gave us in its challenge) */
	if (list_index >= 0 && list_index < ce.settings_player_count && ce.settings_players[list_index].name[0])
		network_player = ce.settings_players[list_index];
	else
	{
		char name[12];

		snprintf(name, sizeof(name), "player %ld", network_index);
		for (i = 0; name[i]; i++)
			network_player.name[i] = (wchar_t)name[i];
		network_player.machine_index = NONE;
		network_player.icon_index = NONE;
	}
	network_player.team_index = (char)get_int(header, 1, body, "team_index", 0);
	network_player.player_list_index = (char)list_index;
	if (ce.local_network_player == network_index ||
		(ce.local_network_player == 0 && list_index >= 0 && list_index < ce.settings_player_count &&
			ce.settings_players[list_index].machine_index == (char)ce.session.challenge_long1))
	{
		ce.local_network_player = (int)network_index;
		local_player_index = 0;
	}
	/* (at the host's own datum index: chat and pings name players by it) */
	{
		long raw = get_int(header, 1, body, "raw_player_index", NONE);

		if (raw == NONE || !(raw >> 16))
			raw = DATUM_INDEX_NEW((short)list_index, 1);
		player_index = player_new(network_player.machine_index, raw, local_player_index, &network_player);
		if (player_index == NONE)
			player_index = player_new(network_player.machine_index, NONE, local_player_index, &network_player);
	}
	if (player_index == NONE)
	{
		error(3, "ce: could not make player %ld", network_index);
		return;
	}
	memset(&ce.players[network_index], 0, sizeof(ce.players[network_index]));
	ce.players[network_index].index = player_index;
	ce.players[network_index].list_index = (int)list_index;
	ce.players[network_index].last_update_id = -1;
	ce.players[network_index].position_sequence = -1;
	player_get(player_index)->team_index = network_player.team_index;
	game_engine_player_added(player_index);
	update_client_add_player(player_index);
	if (local_player_index == 0)
		local_player_set_player_index(0, player_index);
}

/* player_spawn: the player's unit, team, weapons and seat (4) */
static void spawn_player(const ce_message_header *header, const void *body)
{
	static const char *const weapons[4] = { "weapon_object_indices[0]", "weapon_object_indices[1]",
		"weapon_object_indices[2]", "weapon_object_indices[3]" };
	long network_index = get_int(header, 1, body, "translated_player_index", 0);
	long player_index = player_of(network_index);
	long unit_index = object_of(get_int(header, 1, body, "unit_index", 0));
	long vehicle_index = object_of(get_int(header, 1, body, "parent_vehicle_index", 0));
	struct unit_datum *unit;
	int i;

	if (player_index == NONE || unit_index == NONE)
		return;
	player_get(player_index)->team_index = get_int(header, 1, body, "team_index", 0);
	object_get(unit_index)->object.owner_team_index = (short)player_get(player_index)->team_index;
	network_player_attach_unit(player_index, unit_index);
	/* (its weapons: none in hand, the one it wants readied by the unit) */
	unit = (struct unit_datum *)object_get(unit_index);
	for (i = 0; i < 4; i++)
	{
		long weapon = object_of(get_int(header, 1, body, weapons[i], 0));

		if (weapon != NONE && object_try_and_get_and_verify_type(weapon, FLAG(_object_type_weapon)))
			unit_network_add_weapon(unit_index, weapon, (short)i);
	}
	unit->unit.current_weapon_index = NONE;
	unit->unit.desired_weapon_index = (short)get_int(header, 1, body, "desired_weapon_index", 0);
	if (vehicle_index != NONE)
		network_objects_set_seat(unit_index, vehicle_index, (short)get_int(header, 1, body, "vehicle_seat_index", 0));
	ce.players[network_index].position_sequence = -1;
}

/* unit_kill: dead, the player's respawn timer, the index freed (4) */
static void kill_unit(const ce_message_header *header, const void *body)
{
	long network_index = get_int(header, 1, body, "unit_index", 0);
	long unit_index = object_of(network_index);
	long player;

	if (unit_index == NONE)
		return;
	for (player = 1; player < CE_NETWORK_PLAYERS; player++)
	{
		long player_index = ce.players[player].index;

		if (player_index != NONE && player_get(player_index)->unit_index == unit_index)
		{
			player_get(player_index)->respawn_timer = get_int(header, 1, body, "respawn_timer", 0);
			network_player_detach_unit(player_index);
		}
	}
	object_get(unit_index)->object.body_vitality = 0.0f;
	object_get(unit_index)->object.shield_vitality = 0.0f;
	/* (dies at once, as a Delta client's unit does: the server counts it) */
	if (!TEST_FLAG(object_get(unit_index)->object.damage_flags, _object_dead_bit))
	{
		unit_kill_no_statistics(unit_index);
		object_damage_update(unit_index);
	}
	forget_object(network_index);
}

/* a remote player's action, as the real client takes it (3.3): from the
baseline of its id or not at all, and fed to the engine's relayed actions
(the unit is driven by it every tick) */
static void remote_action(long network_index, const ce_message_header *header, const void *header_fields,
	const void *body)
{
	struct ce_player *player;
	struct player_action action;
	unsigned short flags[4];
	long update_id, ticks;
	int baseline_id, d, i;
	real facing[3], throttle[3];

	if (network_index <= 0 || network_index >= CE_NETWORK_PLAYERS || ce.players[network_index].index == NONE ||
		network_index == ce.local_network_player)
		return;
	player = &ce.players[network_index];
	update_id = get_int(header, 0, header_fields,
		header->type >= CE_MSG_REMOTE_PLAYER_TOTAL_UPDATE_BIPED ? "action_header.update_id" : "update_id", 0);
	baseline_id = (int)get_int(header, 0, header_fields,
		header->type >= CE_MSG_REMOTE_PLAYER_TOTAL_UPDATE_BIPED ? "action_header.baseline_id" : "baseline_id", 0);
	if (header->mode == 0)
	{
		player->baseline_id = baseline_id;
		memcpy(player->action_baseline, body, sizeof(player->action_baseline));
	}
	else
	{
		if (baseline_id != player->baseline_id)
			return;
		d = (int)(update_id - player->last_update_id);
		if (player->last_update_id >= 0 && update_id <= player->last_update_id)
			d += 64;
		if (player->last_update_id >= 0 && d > 31)
			return;
	}
	ticks = get_int(header, 1, body,
		header->type >= CE_MSG_REMOTE_PLAYER_TOTAL_UPDATE_BIPED ? "action_data.ticks_to_apply_update_to" :
			"ticks_to_apply_update_to", 1);
	if (player->last_update_id >= 0 && ticks > 0)
	{
		const char *prefix = header->type >= CE_MSG_REMOTE_PLAYER_TOTAL_UPDATE_BIPED ? "action_data." : "";
		char name[64];

		memset(&action, 0, sizeof(action));
		snprintf(name, sizeof(name), "%saction.control_flags", prefix);
		action.control_flags = get_uint(header, 1, body, name);
		snprintf(name, sizeof(name), "%sfacing_vector", prefix);
		get_vector(header, 1, body, name, facing);
		action.desired_facing.yaw = (real)atan2(facing[1], facing[0]);
		action.desired_facing.pitch = (real)atan2(facing[2], sqrt(facing[0] * facing[0] + facing[1] * facing[1]));
		snprintf(name, sizeof(name), "%saction.throttle", prefix);
		get_vector(header, 1, body, name, throttle);
		action.throttle.i = throttle[0];
		action.throttle.j = throttle[1];
		snprintf(name, sizeof(name), "%saction.primary_trigger", prefix);
		action.primary_trigger = get_real(header, 1, body, name);
		snprintf(name, sizeof(name), "%saction.desired_weapon_index", prefix);
		action.desired_weapon_index = (short)get_int(header, 1, body, name, NONE);
		snprintf(name, sizeof(name), "%saction.desired_grenade_index", prefix);
		action.desired_grenade_index = (short)get_int(header, 1, body, name, NONE);
		action.desired_zoom_level = NONE;
		for (i = 0; i < 4; i++)
			flags[i] = (unsigned short)action.control_flags;
		player->ticks_fed += ticks;
		update_client_handle_relayed_action((short)DATUM_INDEX_TO_ABSOLUTE_INDEX(player->index), player->ticks_fed, &action,
			flags, (short)(ticks < 4 ? ticks : 4));
	}
	player->last_update_id = (int)update_id;
}

/* a remote player's position (3.4): its sequence checked, the unit put
there (glided within CE_REMOTE_SNAP, else set) */
static void remote_position(long network_index, const ce_message_header *header, const void *header_fields,
	const real position[3])
{
	struct ce_player *player;
	long sequence, unit_index;
	int last;

	if (network_index <= 0 || network_index >= CE_NETWORK_PLAYERS || ce.players[network_index].index == NONE ||
		network_index == ce.local_network_player)
		return;
	player = &ce.players[network_index];
	sequence = get_int(header, 0, header_fields, "sequence_number", 0);
	last = player->position_sequence;
	if (last >= 0 && !((sequence > last && sequence - last <= 3) || (sequence <= last && sequence - last + 8 <= 3)))
		return;
	player->position_sequence = (int)sequence;
	unit_index = unit_of_player(player->index);
	if (unit_index != NONE && object_get(unit_index)->object.parent_object_index == NONE)
	{
		struct object_datum *unit = object_get(unit_index);
		real_point3d where;

		point_of(position, &where);
		network_objects_reconcile(unit_index, &where, &unit->object.forward, &unit->object.up, NULL, NULL,
			CE_REMOTE_SNAP);
	}
}

/* our unit corrected by the server's position after an update of ours
(3.5, 6): by the difference from where we were then, if beyond a little */
static void own_position(const ce_message_header *header, const void *body)
{
	long sequence = get_int(header, 1, body, "sequence_number", 0);
	long update_id = get_int(header, 1, body, "last_completed_update_id", 0);
	long unit_index = unit_of_player(player_of(ce.local_network_player));
	int last = ce.last_ack_sequence, i, k;
	real position[3];

	if (last >= 0 && !((sequence > last && sequence - last < 16) || (sequence <= last && sequence - last + 32 < 16)))
		return;
	ce.last_ack_sequence = (int)sequence;
	ce.own_updates++;
	/* (the history up to that update acknowledged; its position, ours
	after its ticks (sent after the frame ran them), is where the server's
	should be) */
	for (i = 0; i < ce.history_count; i++)
		if (ce.history[i].id == update_id)
			break;
	if (i == ce.history_count)
		return;
	k = i;
	if (unit_index != NONE && get_vector(header, 1, body, "position", position) &&
		object_get(unit_index)->object.parent_object_index == NONE)
	{
		struct object_datum *unit = object_get(unit_index);
		real dx = position[0] - ce.history[k].position.x, dy = position[1] - ce.history[k].position.y,
			dz = position[2] - ce.history[k].position.z, d2 = dx * dx + dy * dy + dz * dz;

		if (d2 > ce.largest_correction * ce.largest_correction)
			ce.largest_correction = (real)sqrt(d2);
		if (d2 > CE_OWN_CORRECTION * CE_OWN_CORRECTION)
		{
			real_point3d where;
			int j;

			where.x = unit->object.position.x + dx;
			where.y = unit->object.position.y + dy;
			where.z = unit->object.position.z + dz;
			network_objects_correct(unit_index, &where, &unit->object.forward, &unit->object.up, NULL, NULL);
			ce.corrections++;
			/* (the later updates' positions moved with it) */
			for (j = k; j < ce.history_count; j++)
			{
				ce.history[j].position.x += dx;
				ce.history[j].position.y += dy;
				ce.history[j].position.z += dz;
			}
		}
	}
	/* (acknowledged ones forgotten) */
	memmove(ce.history, ce.history + i + 1, (ce.history_count - i - 1) * sizeof(ce.history[0]));
	ce.history_count -= i + 1;
}

/* ... and in a vehicle (36): the vehicle where the server has it after
that update, against where ours was then (not now: the server's is a round
trip old, and putting ours there pulled it back as it drove), moved by the
difference beyond a little, as our unit is */
static void own_vehicle(const ce_message_header *header, const void *body)
{
	long sequence = get_int(header, 1, body, "sequence_number", 0);
	long update_id = get_int(header, 1, body, "last_completed_update_id", 0);
	long vehicle_index = object_of(get_int(header, 1, body, "vehicle_index", 0));
	int last = ce.last_ack_sequence, i, j;
	real values[3];

	if (last >= 0 && !((sequence > last && sequence - last < 16) || (sequence <= last && sequence - last + 32 < 16)))
		return;
	ce.last_ack_sequence = (int)sequence;
	ce.own_updates++;
	for (i = 0; i < ce.history_count; i++)
		if (ce.history[i].id == update_id)
			break;
	if (i == ce.history_count)
		return;
	if (vehicle_index != NONE && object_try_and_get_and_verify_type(vehicle_index, _object_mask_vehicle) &&
		ce.history[i].vehicle == vehicle_index && get_vector(header, 1, body, "position", values))
	{
		struct object_datum *vehicle = object_get(vehicle_index);
		real dx = values[0] - ce.history[i].vehicle_position.x, dy = values[1] - ce.history[i].vehicle_position.y,
			dz = values[2] - ce.history[i].vehicle_position.z, d2 = dx * dx + dy * dy + dz * dz;

		if (d2 > ce.largest_correction * ce.largest_correction)
			ce.largest_correction = (real)sqrt(d2);
		if (d2 > CE_OWN_VEHICLE_CORRECTION * CE_OWN_VEHICLE_CORRECTION)
		{
			real_point3d where;

			where.x = vehicle->object.position.x + dx;
			where.y = vehicle->object.position.y + dy;
			where.z = vehicle->object.position.z + dz;
			network_objects_correct(vehicle_index, &where, &vehicle->object.forward, &vehicle->object.up,
				&vehicle->object.translational_velocity, &vehicle->object.angular_velocity);
			ce.corrections++;
			for (j = i + 1; j < ce.history_count; j++)
			{
				if (ce.history[j].vehicle != vehicle_index)
					continue;
				ce.history[j].vehicle_position.x += dx;
				ce.history[j].vehicle_position.y += dy;
				ce.history[j].vehicle_position.z += dz;
			}
		}
	}
	memmove(ce.history, ce.history + i + 1, (ce.history_count - i - 1) * sizeof(ce.history[0]));
	ce.history_count -= i + 1;
}

/* a remote player's vehicle (40, the vehicle part of 42): put where the
server has it, if the player drives it (3.4) */
static void remote_vehicle(long network_index, const ce_message_header *header, const void *header_fields,
	const void *body, const char *prefix)
{
	struct ce_player *player;
	long sequence, vehicle_index, unit_index;
	int last;
	char name[64];
	real values[3];
	real_point3d where;
	real_vector3d f, u, v, a;

	if (network_index <= 0 || network_index >= CE_NETWORK_PLAYERS || ce.players[network_index].index == NONE ||
		network_index == ce.local_network_player)
		return;
	player = &ce.players[network_index];
	sequence = get_int(header, 0, header_fields, "sequence_number", 0);
	last = player->position_sequence;
	if (last >= 0 && !((sequence > last && sequence - last <= 3) || (sequence <= last && sequence - last + 8 <= 3)))
		return;
	player->position_sequence = (int)sequence;
	snprintf(name, sizeof(name), "%svehicle_index", prefix);
	vehicle_index = object_of(get_int(header, 1, body, name, 0));
	unit_index = unit_of_player(player->index);
	if (vehicle_index == NONE || unit_index == NONE || object_get(unit_index)->object.parent_object_index != vehicle_index)
		return;
	snprintf(name, sizeof(name), "%sposition", prefix);
	get_vector(header, 1, body, name, values);
	point_of(values, &where);
	snprintf(name, sizeof(name), "%sforward", prefix);
	get_vector(header, 1, body, name, values);
	vector_of(values, &f);
	snprintf(name, sizeof(name), "%sup", prefix);
	get_vector(header, 1, body, name, values);
	vector_of(values, &u);
	snprintf(name, sizeof(name), "%stranslational_velocity", prefix);
	get_vector(header, 1, body, name, values);
	vector_of(values, &v);
	snprintf(name, sizeof(name), "%sangular_velocity", prefix);
	get_vector(header, 1, body, name, values);
	vector_of(values, &a);
	if (normalize3d(&f) < 0.5f || normalize3d(&u) < 0.5f)
	{
		f = object_get(vehicle_index)->object.forward;
		u = object_get(vehicle_index)->object.up;
	}
	network_objects_reconcile(vehicle_index, &where, &f, &u, &v, &a, CE_BLEND_DISTANCE);
}

/* ---------- the game */

/* the server's game type as this game's variant: Halo's built-in of the
same engine, with the settings the PC's universal variant shares with the
Xbox's (its first 0x2C bytes: teams to weapon set) */
static void take_settings(const ce_message_header *header, const void *body)
{
	static const char *const engines[] = { "slayer", "ctf", "slayer", "oddball", "king", "race" };
	const ce_field_ref *map = field(header, 1, "map"), *variant = field(header, 1, "variant");
	const ce_field_ref *players = field(header, 1, "players");
	const unsigned char *pc_variant;
	struct game_variant temporary, *found;
	long engine, count;
	char name[32];

	if (!map || !variant)
		return;
	/* (network_map: a long, then the map's name at 4) */
	snprintf(ce.map_name, sizeof(ce.map_name), "%.127s", (const char *)ce_field_ptr(body, map) + 4);
	ce.random_seed = get_int(header, 1, body, "network_game_random_seed", 0);
	ce.difficulty = (short)get_int(header, 1, body, "difficulty_level", 1);
	pc_variant = (const unsigned char *)ce_field_ptr(body, variant);
	memcpy(&engine, pc_variant + 0x30, sizeof(engine));
	ce.game_engine_index = engine;
	snprintf(name, sizeof(name), "%s%s", pc_variant[0x34] && engine != 1 && engine != 5 ? "team_" : "",
		engines[engine >= 0 && engine < 6 ? engine : 0]);
	found = game_engine_get_variant_by_name(&temporary, name);
	if (!found)
		found = game_engine_get_variant_by_name(&temporary, engines[engine >= 0 && engine < 6 ? engine : 0]);
	if (found)
		ce.variant = *found;
	memcpy(&ce.variant.universal_variant, pc_variant + 0x34, 0x2C);
	/* (its players: the PC's network_player is laid out as this game's) */
	count = get_int(header, 1, body, "player_count", 0);
	if (players && count >= 0 && count <= 16)
	{
		memcpy(ce.settings_players, ce_field_ptr(body, players), (size_t)count * sizeof(struct network_player));
		ce.settings_player_count = (int)count;
	}
	ce.have_settings = TRUE;
}

/* a player's scores (3.6): kills, assists, deaths... into its statistics */
static void take_scores(const ce_message_header *header, const void *header_fields, const void *body)
{
	long network_index = get_int(header, 0, header_fields, "player_index", 0);
	long player_index = player_of(network_index);
	struct game_statistics *statistics;
	int i;

	if (player_index == NONE)
		return;
	memcpy(ce.score_baselines[network_index], body, header->def->body_size < 48 ? header->def->body_size : 48);
	statistics = &player_get(player_index)->statistics;
	for (i = 0; i < 4; i++)
	{
		char name[16];

		snprintf(name, sizeof(name), "kills[%d]", i);
		statistics->kills[i] = (short)get_int(header, 1, body, name, 0);
		snprintf(name, sizeof(name), "assists[%d]", i);
		statistics->assists[i] = (short)get_int(header, 1, body, name, 0);
	}
	statistics->friendly_fire_kills = (short)get_int(header, 1, body, "friendly_fire_kills", 0);
	statistics->deaths = (short)get_int(header, 1, body, "deaths", 0);
	statistics->suicides = (short)get_int(header, 1, body, "suicides", 0);
}

/* the game type's scores (3.6): slayer's, oddball's (teams and players by
absolute index) and capture the flag's (teams) into the game type's own
state, through the state a Delta host sends its clients (game_engine.c's
read and write); king's and race's are kept as baselines only */
static void take_game_scores(const ce_message_header *header, const void *body)
{
	static unsigned char state[4096];
	const ce_field_ref *team = field(header, 1, header->type == CE_MSG_CTF_UPDATE ? "score" : "team_score");
	const ce_field_ref *individual = field(header, 1, "individual_score");
	long size, team_at, individual_at, teams, i;
	int engine = header->type == CE_MSG_SLAYER_UPDATE ? game_engine_slayer : header->type == CE_MSG_CTF_UPDATE ?
		game_engine_ctf : header->type == CE_MSG_ODDBALL_UPDATE ? game_engine_oddball : NONE;

	if (header->def->body_size <= sizeof(ce.engine_baselines[0]))
		memcpy(ce.engine_baselines[header->type - CE_MSG_SLAYER_UPDATE], body, header->def->body_size);
	/* (the game type the server plays, which the variant was made from) */
	if (engine == NONE || ce.game_engine_index != engine || !team)
		return;
	size = game_engine_write_network_state(state, sizeof(state));
	/* (after the postgame state: slayer's and oddball's team scores, then
	their players'; capture the flag's two flags' weapons, then the scores) */
	team_at = sizeof(long) + (engine == game_engine_ctf ? 2 * sizeof(long) : 0);
	individual_at = team_at + HALO_PORT_MAXIMUM_NETWORK_PLAYERS * sizeof(long);
	teams = engine == game_engine_ctf ? 2 : 16;
	if (teams > HALO_PORT_MAXIMUM_NETWORK_PLAYERS)
		teams = HALO_PORT_MAXIMUM_NETWORK_PLAYERS;
	if (size < individual_at + (long)(teams * sizeof(long)))
		return;
	for (i = 0; i < teams; i++)
	{
		int32_t value;

		memcpy(&value, (const char *)ce_field_ptr(body, team) + i * 4, 4);
		memcpy(state + team_at + i * sizeof(long), &(long){ value }, sizeof(long));
		if (individual && engine != game_engine_ctf)
		{
			memcpy(&value, (const char *)ce_field_ptr(body, individual) + i * 4, 4);
			memcpy(state + individual_at + i * sizeof(long), &(long){ value }, sizeof(long));
		}
	}
	game_engine_read_network_state(state, size);
}

/* a weapon into the unit's first free slot (or the one given), shown on its
player's HUD */
static void give_weapon(long unit_index, long weapon_index, short slot)
{
	struct unit_datum *unit = (struct unit_datum *)object_get(unit_index);

	if (slot == NONE)
	{
		for (slot = 0; slot < 4; slot++)
			if (unit->unit.weapon_object_indices[slot] == NONE)
				break;
		if (slot == 4)
			return;
	}
	unit_network_add_weapon(unit_index, weapon_index, slot);
	if (unit->unit.player_index != NONE && player_get(unit->unit.player_index)->local_player_index != NONE)
		hud_picked_up_weapon(player_get(unit->unit.player_index)->local_player_index,
			object_get(weapon_index)->definition_index);
}

/* player_set_action_result (4): what the server let a player do with the
action key: a weapon picked up or swapped, a vehicle entered */
static void action_result(const ce_message_header *header, const void *body)
{
	long player_index = player_of(get_int(header, 1, body, "player_index", 0));
	long unit_index = unit_of_player(player_index);
	long object_index = object_of(get_int(header, 1, body, "action_object_index", 0));
	long result = get_int(header, 1, body, "action_result", NONE);
	short seat = (short)get_int(header, 1, body, "action_seat_index", NONE);
	struct player_datum *player;
	struct unit_datum *unit;
	short slot;

	if (player_index == NONE || unit_index == NONE || object_index == NONE)
		return;
	player = player_get(player_index);
	player->action_result = (short)result;
	player->action_object_index = object_index;
	player->action_seat_index = seat;
	unit = (struct unit_datum *)object_get(unit_index);
	switch (result)
	{
	case _player_action_result_pickup_weapon:
	case _player_action_result_add_weapon_to_inventory:
		if (object_try_and_get_and_verify_type(object_index, FLAG(_object_type_weapon)))
			give_weapon(unit_index, object_index, NONE);
		break;
	case _player_action_result_swap_for_weapon:
	{
		/* (the weapon swapped out dropped from its slot, the new one in it) */
		long dropped = object_of(get_int(header, 1, body, "weapon_to_drop_as_result_of_swap", 0));

		if (!object_try_and_get_and_verify_type(object_index, FLAG(_object_type_weapon)))
			break;
		slot = NONE;
		for (slot = 0; slot < 4; slot++)
			if (dropped != NONE && unit->unit.weapon_object_indices[slot] == dropped)
				break;
		if (slot == 4)
			slot = unit->unit.current_weapon_index;
		if (slot < 0 || slot >= 4)
			slot = 0;
		if (unit->unit.weapon_object_indices[slot] != NONE)
			unit_network_drop_weapon(unit_index, slot);
		give_weapon(unit_index, object_index, slot);
		unit->unit.desired_weapon_index = slot;
		break;
	}
	case _player_action_result_enter_vehicle:
	case _player_action_result_evict_from_vehicle:
		if (object_try_and_get_and_verify_type(object_index, _object_mask_vehicle) && seat != NONE)
			network_objects_set_seat(unit_index, object_index, seat);
		break;
	default:
		break;
	}
}

/* the map loaded as a client of the server: the steps the game's own
network client takes (network_game_create_game_objects), with the CE
server's seed and game type */
static int load_map(void)
{
	struct game_options options;
	struct object_iterator iterator;
	int i;

	game_connection_set(_game_connection_network_client);
	game_options_new(&options);
	/* (Custom Edition's maps: ce_maps.c's folder, the map family's name) */
	snprintf(options.map_name, sizeof(options.map_name), "%s@ce", ce.map_name);
	options.difficulty = ce.difficulty;
	options.random_seed = ce.random_seed;
	game_precache_new_map(options.map_name, TRUE);
	main_menu_unload();
	if (game_in_progress() || game_map_loaded())
	{
		game_dispose_from_old_map();
		game_unload();
	}
	game_set_game_variant(&ce.variant);
	if (!game_load(&options))
		return FALSE;
	game_initialize_for_new_map();
	/* (the server sends every unit, vehicle and item: the map's own go) */
	object_iterator_new(&iterator, FLAG(_object_type_biped) | FLAG(_object_type_vehicle) | FLAG(_object_type_weapon) |
		FLAG(_object_type_equipment), 0);
	while (object_iterator_next(&iterator))
		object_delete(iterator.index);
	for (i = 0; i < CE_NETWORK_OBJECTS; i++)
		forget_object(i);
	for (i = 0; i < CE_NETWORK_PLAYERS; i++)
		ce.players[i].index = NONE;
	ce.local_network_player = 0;
	ce.history_count = 0;
	ce.last_ack_sequence = -1;
	ce.last_sent_tick = NONE;
	ce.next_update_id = 0;
	memset(ce.sent_body, 0, sizeof(ce.sent_body));
	memset(ce.score_baselines, 0, sizeof(ce.score_baselines));
	memset(ce.engine_baselines, 0, sizeof(ce.engine_baselines));
	game_time_start();
	game_initial_pulse();
	ce.map_up = TRUE;
	/* (the next game's map, if it is not here either, downloaded too) */
	ce.downloaded = FALSE;
	return TRUE;
}

/* a packet the session did not handle (a new game's begin, pongs) */
static void send_pong(void)
{
	const ce_message_def *def = ce_delta_message(CE_MSG_CLIENT_TO_SERVER_PONG);
	ce_message_header header;
	unsigned char body[16];
	long local = ce.local_network_player ? ce.players[ce.local_network_player].list_index : 0;

	if (!def)
		return;
	memset(&header, 0, sizeof(header));
	header.def = def;
	header.type = CE_MSG_CLIENT_TO_SERVER_PONG;
	memset(body, 0, sizeof(body));
	set_uint(&header, 1, body, "player_number", (unsigned long)local);
	ce_session_send_delta(&ce.session, 0, 0, CE_MSG_CLIENT_TO_SERVER_PONG, NULL, body, NULL, now_ms());
}

static void message(void *context, int reliable, const ce_message_header *header, int iteration,
	const unsigned char *present, const void *header_fields, const void *body)
{
	(void)context;
	(void)reliable;
	(void)iteration;
	switch (header->type)
	{
	case CE_MSG_GAME_SETTINGS_UPDATE:
		take_settings(header, body);
		break;
	case CE_MSG_WEAPON_NEW:
	case CE_MSG_VEHICLE_NEW:
	case CE_MSG_EQUIPMENT_NEW:
	case CE_MSG_BIPED_NEW:
	case CE_MSG_PROJECTILE_NEW:
	case CE_MSG_NETGAME_EQUIPMENT_NEW:
		if (ce.loaded && header->mode == 0)
			create_object(header, body);
		break;
	case CE_MSG_PROJECTILE_UPDATE:
	case CE_MSG_EQUIPMENT_UPDATE:
	case CE_MSG_WEAPON_UPDATE:
	case CE_MSG_BIPED_UPDATE:
	case CE_MSG_VEHICLE_UPDATE:
		if (ce.loaded)
			update_object(header, present, header_fields, body);
		break;
	case CE_MSG_OBJECT_DELETION:
	case CE_MSG_PROJECTILE_DETONATE:
		if (ce.loaded)
			delete_object(get_int(header, 1, body, "object_index", 0));
		break;
	case CE_MSG_PLAYER_CREATE:
		if (ce.loaded)
			create_player(header, body);
		break;
	case CE_MSG_PLAYER_SPAWN:
		if (ce.loaded)
			spawn_player(header, body);
		break;
	case CE_MSG_UNIT_KILL:
		if (ce.loaded)
			kill_unit(header, body);
		break;
	case CE_MSG_PLAYER_EXIT_VEHICLE:
	{
		/* (out as the unit can, or at once when forced, 4) */
		long unit_index = object_of(get_int(header, 1, body, "object_index", 0));

		if (ce.loaded && unit_index != NONE && object_try_and_get_and_verify_type(unit_index, _object_mask_unit) &&
			object_get(unit_index)->object.parent_object_index != NONE &&
			(get_int(header, 1, body, "forced_exit", 0) || !unit_try_and_exit_seat(unit_index)))
		{
			unit_exit_seat_end(unit_index);
		}
		break;
	}
	case CE_MSG_PLAYER_SET_ACTION_RESULT:
		if (ce.loaded)
			action_result(header, body);
		break;
	case CE_MSG_UNIT_DROP_CURRENT_WEAPON:
	{
		/* (the weapon named readied first if it is not in hand, 4) */
		long unit_index = object_of(get_int(header, 1, body, "unit_index", 0));
		long weapon_index = object_of(get_int(header, 1, body, "weapon_to_drop", 0));
		struct unit_datum *unit;
		short slot;

		if (!ce.loaded || unit_index == NONE || weapon_index == NONE ||
			!object_try_and_get_and_verify_type(unit_index, _object_mask_unit))
			break;
		unit = (struct unit_datum *)object_get(unit_index);
		for (slot = 0; slot < 4; slot++)
			if (unit->unit.weapon_object_indices[slot] == weapon_index)
			{
				unit_network_drop_weapon(unit_index, slot);
				break;
			}
		break;
	}
	case CE_MSG_TEAM_CHANGE:
	{
		/* (a team game's player moved: by its place in the player list) */
		long list_index = get_int(header, 1, body, "player_number", NONE);
		long team = get_int(header, 1, body, "new_team_choice", 0);
		long player;

		if (!ce.loaded)
			break;
		for (player = 1; player < CE_NETWORK_PLAYERS; player++)
			if (ce.players[player].index != NONE && ce.players[player].list_index == list_index)
				player_get(ce.players[player].index)->team_index = team;
		if (list_index >= 0 && list_index < ce.settings_player_count)
			ce.settings_players[list_index].team_index = (char)team;
		break;
	}
	case CE_MSG_SLAYER_UPDATE:
	case CE_MSG_CTF_UPDATE:
	case CE_MSG_ODDBALL_UPDATE:
	case CE_MSG_KING_UPDATE:
	case CE_MSG_RACE_UPDATE:
		if (ce.loaded)
			take_game_scores(header, body);
		break;
	case CE_MSG_LOCAL_PLAYER_VEHICLE_UPDATE:
		if (ce.loaded)
			own_vehicle(header, body);
		break;
	case CE_MSG_REMOTE_PLAYER_VEHICLE_UPDATE:
		if (ce.loaded)
		{
			long network_index = get_int(header, 0, header_fields, "player_index", 0);

			if (header->mode == 0 && network_index > 0 && network_index < CE_NETWORK_PLAYERS &&
				header->def->body_size <= sizeof(ce.players[0].vehicle_baseline))
				memcpy(ce.players[network_index].vehicle_baseline, body, header->def->body_size);
			remote_vehicle(network_index, header, header_fields, body, "");
		}
		break;
	case CE_MSG_WEAPON_START_RELOAD:
	case CE_MSG_WEAPON_FINISH_RELOAD:
	case CE_MSG_WEAPON_CANCEL_RELOAD:
	case CE_MSG_WEAPON_AMMO_PICKUP_MID_RELOAD:
	{
		long weapon_index = object_of(get_int(header, 1, body, "weapon_index", 0));
		long magazine = get_int(header, 1, body, "magazine_index", 0);
		struct weapon_datum *weapon;

		if (!ce.loaded || weapon_index == NONE || magazine < 0 || magazine > 1 ||
			!object_try_and_get_and_verify_type(weapon_index, FLAG(_object_type_weapon)))
			break;
		weapon = (struct weapon_datum *)object_get(weapon_index);
		if (header->type == CE_MSG_WEAPON_AMMO_PICKUP_MID_RELOAD)
			weapon->weapon.magazines[magazine].rounds_total +=
				(short)get_int(header, 1, body, "rounds_picked_up", 0);
		else if (header->type != CE_MSG_WEAPON_START_RELOAD)
		{
			weapon->weapon.magazines[magazine].rounds_total = (short)get_int(header, 1, body, "ending_total_rounds", 0);
			weapon->weapon.magazines[magazine].rounds_loaded = (short)get_int(header, 1, body, "ending_loaded_rounds", 0);
		}
		break;
	}
	case CE_MSG_PLAYER_SCORE_UPDATE:
		if (ce.loaded)
			take_scores(header, header_fields, body);
		break;
	case CE_MSG_GAME_ENGINE_CHANGE_MODE:
		/* (1: the game over; 2, 3: the postgame and its scores) */
		if (ce.loaded && get_int(header, 1, body, "new_mode", 0) == 1)
			game_engine_end_game();
		break;
	case CE_MSG_LOCAL_PLAYER_UPDATE:
		if (ce.loaded)
			own_position(header, body);
		break;
	case CE_MSG_REMOTE_PLAYER_ACTION_UPDATE:
	case CE_MSG_SUPER_REMOTE_PLAYERS_ACTION_UPDATE:
		if (ce.loaded)
			remote_action(get_int(header, 0, header_fields, "player_index", 0), header, header_fields, body);
		break;
	case CE_MSG_REMOTE_PLAYER_POSITION_UPDATE:
	case CE_MSG_REMOTE_PLAYER_TOTAL_UPDATE_BIPED:
	{
		int total = header->type == CE_MSG_REMOTE_PLAYER_TOTAL_UPDATE_BIPED;
		long network_index = get_int(header, 0, header_fields, total ? "action_header.player_index" : "player_index", 0);
		real position[3];

		if (!ce.loaded)
			break;
		if (header->mode == 0 && network_index > 0 && network_index < CE_NETWORK_PLAYERS)
		{
			const ce_field_ref *ref = field(header, 1, total ? "biped_data.position" : "position");

			if (ref)
				memcpy(ce.players[network_index].position_baseline, ce_field_ptr(body, ref), 12);
		}
		if (total)
			remote_action(network_index, header, header_fields, body);
		if (get_vector(header, 1, body, total ? "biped_data.position" : "position", position))
			remote_position(network_index, header, header_fields, position);
		break;
	}
	case CE_MSG_REMOTE_PLAYER_TOTAL_UPDATE_VEHICLE:
		if (ce.loaded)
		{
			long network_index = get_int(header, 0, header_fields, "action_header.player_index", 0);

			if (header->mode == 0 && network_index > 0 && network_index < CE_NETWORK_PLAYERS &&
				header->def->body_size >= 48 + 64)
				memcpy(ce.players[network_index].vehicle_baseline, (const char *)body + 48, 64);
			remote_action(network_index, header, header_fields, body);
		}
		break;
	case CE_MSG_SUPER_PING_UPDATE:
		/* (each iteration answered with a pong, 4) */
		send_pong();
		break;
	case CE_MSG_HUD_CHAT:
	{
		wchar_t line[200];
		long number = get_int(header, 1, body, "player_number", 0xFF);
		int at = 0, i;

		/* (the sender's name first; the server's own lines marked) */
		if (number == 0xFF)
		{
			const char *server = "** SERVER ** ";

			for (i = 0; server[i]; i++)
				line[at++] = (wchar_t)server[i];
		}
		else
		{
			long player;

			for (player = 1; player < CE_NETWORK_PLAYERS; player++)
				if (ce.players[player].index != NONE &&
					DATUM_INDEX_TO_ABSOLUTE_INDEX(ce.players[player].index) == number)
				{
					wchar_t const *name = player_get(ce.players[player].index)->name;

					for (i = 0; name[i] && at < 20; i++)
						line[at++] = name[i];
					line[at++] = ':';
					line[at++] = ' ';
					break;
				}
		}
		for (i = 0; ce.session.chat_text[i] && at < 199; i++)
			line[at++] = (wchar_t)ce.session.chat_text[i];
		line[at] = 0;
		chat_note_wide(line);
		break;
	}
	case CE_MSG_SV_MOTD:
	{
		const ce_field_ref *ref = field(header, 1, "motd_text");

		if (ref && *(const char *)ce_field_ptr(body, ref))
			chat_note((const char *)ce_field_ptr(body, ref));
		break;
	}
	default:
		break;
	}
}

/* the score updates' baselines: each player's last received (3.6) */
static const void *baseline_for(void *context, const ce_message_header *header, int iteration,
	const void *header_fields)
{
	long network_index;

	(void)context;
	(void)iteration;
	if (header->type >= CE_MSG_PROJECTILE_UPDATE && header->type <= CE_MSG_VEHICLE_UPDATE)
	{
		network_index = get_int(header, 0, header_fields, "object_index", 0);
		if (network_index > 0 && network_index < CE_NETWORK_OBJECTS && ce.objects[network_index].has_baseline)
			return ce.objects[network_index].baseline;
		return NULL;
	}
	if (header->type == CE_MSG_PLAYER_SCORE_UPDATE)
	{
		network_index = get_int(header, 0, header_fields, "player_index", 0);
		return network_index > 0 && network_index < CE_NETWORK_PLAYERS ? ce.score_baselines[network_index] : NULL;
	}
	if (header->type >= CE_MSG_SLAYER_UPDATE && header->type <= CE_MSG_RACE_UPDATE)
		return ce.engine_baselines[header->type - CE_MSG_SLAYER_UPDATE];
	network_index = get_int(header, 0, header_fields,
		header->type == CE_MSG_REMOTE_PLAYER_TOTAL_UPDATE_BIPED || header->type == CE_MSG_REMOTE_PLAYER_TOTAL_UPDATE_VEHICLE ?
			"action_header.player_index" : "player_index", 0);
	if (network_index <= 0 || network_index >= CE_NETWORK_PLAYERS)
		return NULL;
	switch (header->type)
	{
	case CE_MSG_REMOTE_PLAYER_ACTION_UPDATE:
	case CE_MSG_SUPER_REMOTE_PLAYERS_ACTION_UPDATE:
		return ce.players[network_index].action_baseline;
	case CE_MSG_REMOTE_PLAYER_POSITION_UPDATE:
		return ce.players[network_index].position_baseline;
	case CE_MSG_REMOTE_PLAYER_TOTAL_UPDATE_BIPED:
	case CE_MSG_REMOTE_PLAYER_TOTAL_UPDATE_VEHICLE:
	{
		/* (the action baseline and the position's or vehicle's, together) */
		static unsigned char together[48 + 64];

		memcpy(together, ce.players[network_index].action_baseline, 48);
		if (header->type == CE_MSG_REMOTE_PLAYER_TOTAL_UPDATE_BIPED)
			memcpy(together + 48, ce.players[network_index].position_baseline, 12);
		else
			memcpy(together + 48, ce.players[network_index].vehicle_baseline, 64);
		return together;
	}
	default:
		return NULL;
	}
}

/* ---------- our input, a frame at a time (5.1) */

static void send_input(void)
{
	struct player_action action;
	unsigned short flags[4];
	long tick, ticks, unit_index = unit_of_player(player_of(ce.local_network_player));
	const ce_message_def *def = ce_delta_message(CE_MSG_CLIENT_GAME_UPDATE);
	ce_message_header header;
	unsigned char header_fields[CE_DELTA_MAX_HEADER_SIZE + 4], body[CE_DELTA_MAX_BODY_SIZE + 4];
	int result;

	if (!def || unit_index == NONE || !update_client_distributed_input(0, &tick, &action, flags))
		return;
	if (ce.last_sent_tick == NONE)
		ce.last_sent_tick = tick - 1;
	ticks = tick - ce.last_sent_tick;
	if (ticks <= 0)
		return;
	if (ticks > 31)
		ticks = 31;
	/* (the server acknowledges at most 64 at a time: beyond, nothing is sent
	and the unit stands, as the real client does) */
	if (ce.history_count >= CE_HISTORY)
		return;
	memset(&header, 0, sizeof(header));
	header.def = def;
	header.type = CE_MSG_CLIENT_GAME_UPDATE;
	memset(header_fields, 0, sizeof(header_fields));
	memset(body, 0, sizeof(body));
	set_uint(&header, 0, header_fields, "id", (unsigned long)ce.next_update_id);
	set_uint(&header, 1, body, "ticks_to_apply_update_to", (unsigned long)ticks);
	set_uint(&header, 1, body, "control_flags", action.control_flags);
	set_real(&header, 1, body, "desired_facing.yaw", action.desired_facing.yaw);
	set_real(&header, 1, body, "desired_facing.pitch", action.desired_facing.pitch);
	{
		const ce_field_ref *throttle = field(&header, 1, "throttle");

		if (throttle)
		{
			real values[3];

			values[0] = action.throttle.i;
			values[1] = action.throttle.j;
			values[2] = 0.0f;
			ce_set_vector(throttle->props, ce_field_ptr(body, throttle), values);
		}
	}
	set_real(&header, 1, body, "primary_trigger", action.primary_trigger);
	set_uint(&header, 1, body, "desired_weapon_index", (unsigned long)(long)action.desired_weapon_index);
	set_uint(&header, 1, body, "desired_grenade_index", (unsigned long)(long)action.desired_grenade_index);
	set_uint(&header, 1, body, "desired_zoom_level", (unsigned long)(long)action.desired_zoom_level);
	/* (reliable, against the one sent before: the server decodes it against
	the one it received before. Sent every frame that ran ticks, nothing
	changed or not, as the real client does: each one is the ticks the server
	runs our player for; one left out was ticks the server never ran, and its
	position fell behind ours, which pulled us back: the rubber band) */
	result = ce_session_send_delta_kept(&ce.session, 1, 1, CE_MSG_CLIENT_GAME_UPDATE, header_fields, body, ce.sent_body,
		now_ms());
	if (result != 0)
	{
		/* (not sent: the window full; its ticks go with the next) */
		ce.inputs_refused++;
		return;
	}
	ce.last_sent_tick = tick;
	memcpy(ce.sent_body, body, def->body_size);
	ce.history[ce.history_count].id = ce.next_update_id;
	ce.history[ce.history_count].position = object_get(unit_index)->object.position;
	ce.history[ce.history_count].vehicle = object_get(unit_index)->object.parent_object_index;
	if (ce.history[ce.history_count].vehicle != NONE && object_try_and_get(ce.history[ce.history_count].vehicle))
		ce.history[ce.history_count].vehicle_position = object_get(ce.history[ce.history_count].vehicle)->object.position;
	else
		ce.history[ce.history_count].vehicle = NONE;
	ce.history_count++;
	ce.next_update_id = (ce.next_update_id + 1) & 63;
	ce.inputs_sent++;
}

/* the client's ping (old packet 1, the client's time in ms), every 3 s (5.4) */
static void send_ping(void)
{
	int32_t time = (int32_t)now_ms();
	unsigned char message[16], stream[24];
	ce_bitstream bs;
	long total;
	int size = ce_packet_encode(_ce_packet_client_ping, &time, message, sizeof(message)), i;

	if (size < 0)
		return;
	ce_stream_begin(&bs, stream, sizeof(stream));
	ce_stream_put_kind(&bs, CE_ITEM_PACKET);
	for (i = 0; i < size; i++)
		ce_bs_write(&bs, message[i], 8);
	total = ce_stream_finish(&bs);
	if (total > 0 && !bs.overflow)
		ce_link_send(&ce.session.link, 0, stream, (int)total, now_ms());
}

/* ---------- public code */

int dos_ce_active(void)
{
	return ce.active;
}

/* the server's map being downloaded (the joining page's progress) */
int dos_ce_downloading(void)
{
	return ce.active && ce.downloading;
}

const char *dos_ce_status(void)
{
	return ce.status;
}

/* a join of the server: the first, or its next game's (the map that is up
stays up until the next is loaded) */
static int join(void)
{
	const char *name = dos_setting("player_name", "Player");

	ce.have_settings = FALSE;
	ce.settings_player_count = 0;
	ce.loaded = FALSE;
	if (ce_session_begin(&ce.session, ce.address, ce.port, name, ce.password, (uint32_t)system_microseconds(),
		baseline_for, message, ce_log, NULL, now_ms()) < 0)
	{
		snprintf(ce.status, sizeof(ce.status), "could not join: %s", ce.session.error);
		return FALSE;
	}
	ce.active = TRUE;
	snprintf(ce.status, sizeof(ce.status), "joining the CE server");
	return TRUE;
}

/* Custom Edition's shared maps (its bitmaps, sounds and text, which every
CE map's resources are in) in MAPS_CE: NULL, or the first one missing */
static const char *missing_shared_map(void)
{
	static const char *const shared[] = { "bitmaps", "sounds", "loc" };
	char path[300];

	for (int i = 0; i < 3; i++)
	{
		FILE *file;

		snprintf(path, sizeof(path), "%s\\%s.map", dos_setting("ce_maps", "MAPS_CE"), shared[i]);
		file = fopen(path, "rb");
		if (!file)
			return shared[i];
		fclose(file);
	}
	return NULL;
}

int dos_ce_join(uint32_t address, uint16_t port, const char *password)
{
	int connect_tried = ce.connect_tried, i;
	const char *missing = missing_shared_map();

	if (ce.active)
		return FALSE;
	/* (none of its maps would load without them: said now, not as the
	map's load stops Halo) */
	if (missing)
	{
		snprintf(ce.status, sizeof(ce.status), "MAPS_CE needs Custom Edition's %s.map (copy CE's maps folder there)",
			missing);
		ce_log(NULL, ce.status);
		return FALSE;
	}
	memset(&ce, 0, sizeof(ce));
	ce.connect_tried = connect_tried;
	for (i = 0; i < CE_NETWORK_OBJECTS; i++)
		ce.objects[i].index = NONE;
	for (i = 0; i < CE_NETWORK_PLAYERS; i++)
		ce.players[i].index = NONE;
	ce.last_sent_tick = NONE;
	ce.last_ack_sequence = -1;
	ce.address = address;
	ce.port = port;
	snprintf(ce.password, sizeof(ce.password), "%s", password ? password : "");
	return join();
}

void dos_ce_leave(void)
{
	if (!ce.active)
		return;
	if (ce.downloading)
		dos_map_download_cancel();
	ce.downloading = FALSE;
	ce_session_end(&ce.session, now_ms());
	ce.active = FALSE;
	ce.loaded = FALSE;
	game_connection_set(_game_connection_local);
	/* (back to the menus only from the server's map: a join given up while
	connecting is in them already) */
	if (ce.map_up)
		main_goto_main_menu();
	ce.map_up = FALSE;
	snprintf(ce.status, sizeof(ce.status), "left the Custom Edition server");
}

/* every frame */
void dos_ce_frame(void)
{
	/* (testing: ce_connect=address[:port] joins at once) */
	if (!ce.active && !ce.connect_tried)
	{
		const char *target = dos_setting("ce_connect", "");

		ce.connect_tried = TRUE;
		if (target[0])
		{
			char host[64];
			const char *colon = strchr(target, ':');
			long port = colon ? atol(colon + 1) : 2302;

			snprintf(host, sizeof(host), "%.*s", colon ? (int)(colon - target) : (int)strlen(target), target);
			dos_ce_join(retail_resolve(host), (uint16_t)port, dos_setting("ce_password", ""));
		}
	}
	if (!ce.active)
		return;
	/* (the server's map downloading: the server joined again once it is in) */
	if (ce.downloading)
	{
		struct dos_map_download_status download;

		dos_map_download_poll();
		dos_map_download_status(&download);
		if (download.state == DOS_MAP_DOWNLOAD_DONE)
		{
			ce.downloading = FALSE;
			ce.downloaded = TRUE;
			snprintf(ce.status, sizeof(ce.status), "%s downloaded: joining", download.map);
			if (!join())
				ce.active = FALSE;
		}
		else if (download.state == DOS_MAP_DOWNLOAD_FAILED)
		{
			ce.downloading = FALSE;
			snprintf(ce.status, sizeof(ce.status), "could not download %s: %s", download.map, download.error);
			ce.active = FALSE;
		}
		return;
	}
	ce_session_poll(&ce.session, now_ms());
	switch (ce.session.state)
	{
	case _ce_session_joined:
		if (!ce.loaded && ce.have_settings)
		{
			snprintf(ce.status, sizeof(ce.status), "loading %s", ce.map_name);
			if (!load_map())
			{
				snprintf(ce.status, sizeof(ce.status), "could not load %s", ce.map_name);
				dos_ce_leave();
				return;
			}
			ce.loaded = TRUE;
			ce_session_map_loaded(&ce.session, now_ms());
		}
		break;
	case _ce_session_playing:
		/* (the menus' QUIT TO MENU took the game away: the server is left) */
		if (ce.loaded && main_menu_is_active())
		{
			ce_session_end(&ce.session, now_ms());
			ce.active = FALSE;
			ce.loaded = FALSE;
			ce.map_up = FALSE;
			game_connection_set(_game_connection_local);
			snprintf(ce.status, sizeof(ce.status), "left the Custom Edition server");
			return;
		}
		snprintf(ce.status, sizeof(ce.status), "playing on the CE server");
		send_input();
		if (now_ms() - ce.ping_ms >= CE_PING_MS)
		{
			ce.ping_ms = now_ms();
			send_ping();
		}
		if (now_ms() - ce.statistics_ms >= 5000)
		{
			ce.statistics_ms = now_ms();
			error(3, "ce: %ld deltas (%ld errors), %ld objects made, %ld inputs sent (%d unacknowledged, %ld refused), own"
				" unit %ld updates, %ld corrections (largest %.2f), %ld object updates dropped; link %ld resends, %ld"
				" reordered, %ld unacknowledged", ce.session.deltas_received, ce.session.delta_errors, ce.objects_made,
				ce.inputs_sent, ce.history_count, ce.inputs_refused, ce.own_updates, ce.corrections, ce.largest_correction,
				ce.dropped_updates, ce.session.link.resends, ce.session.link.reordered, (long)ce.session.link.unacknowledged_count);
			ce.largest_correction = 0.0f;
		}
		break;
	case _ce_session_failed:
	case _ce_session_ended:
		snprintf(ce.status, sizeof(ce.status), "%s", ce.session.error);
		error(3, "ce: %s", ce.session.error);
		/* (the server's next game: joined again, its map loaded when its
		settings come, this one up until then) */
		if (ce.session.state == _ce_session_ended && ce.session.reconnect)
		{
			ce_session_end(&ce.session, now_ms());
			if (join())
				break;
		}
		/* (the server's map not here: downloaded as Chimera downloads it, the
		setting map_download off leaves it; once a join, so a map the
		repository has in another version does not loop) */
		if (ce.session.map_missing && !ce.downloaded && dos_setting_integer("map_download", 1) &&
			dos_map_download_begin(ce.session.map_name))
		{
			ce_session_end(&ce.session, now_ms());
			ce.downloading = TRUE;
			snprintf(ce.status, sizeof(ce.status), "downloading %s", ce.session.map_name);
			break;
		}
		if (ce.map_up)
			dos_ce_leave();
		ce.active = FALSE;
		break;
	default:
		break;
	}
}
