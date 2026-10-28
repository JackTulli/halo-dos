# The Custom Edition client inside the game: design

The CE client (port/dos/retail/ce_client.c, to be written) drives the engine as a NON-authoritative client, beside
ChupathingyCE's own Delta netcode, by calling the same machinery the Delta client uses. Paths below are in
chupathingyce/ (line numbers as of v0.8.0d); ND = port/linux/game/network_distributed.c, NO = network_objects.c,
NDm = network_damage.c, NGG = source/networking/network_game_globals.c, NCM = network_client_manager.c,
NGM = network_game_manager.c, PQ = source/game/player_queues_new.c.

## Client mode
- `network_game_distributed_client()` (NGG:310) = `game_connection() == _game_connection_network_client` (1);
  `game_connection_set()` main.c:901. Set it BEFORE `game_initialize_for_new_map` (network_objects_placed reads it).
- The client mode already skips: AI (network_actors_drive instead, game.c:355), damage resolution
  (network_damage_deals FALSE, NDm:637), shields, player_spawn (players.c:4591), pickups, item/vehicle spawning,
  end-game decisions, scoring (guards in game_engine_*.c), gametype object remap, deleting host objects.

## Starting a game without a Delta connection
- Own copy of `network_game_create_game_objects` (NGM:768) minus `network_game_get_random_seed` (asserts on a NULL
  game, NGG:757): game_options_new, game_precache_new_map, main_menu_unload, game_unload, game_set_game_variant
  (+options, from CE's game_settings_update), game_load, game_initialize_for_new_map (calls
  network_distributed_new_game, objects_place, network_objects_placed).
- Players: `player_new(machine, DATUM_INDEX_NEW(slot,id), local_index|NONE, &network_player)` (players.c:1231),
  `game_engine_player_added`, `update_client_add_player`, `local_player_set_player_index`; set team_index ourselves
  (game_engine.c:7479 reads network_player only with a client).
- Then `game_time_start` (update_client_start), `game_initial_pulse` (game.c:417),
  `game_time_set_distributed(server_tick)`.
- MUST patch `network_game_client_start_frame` / `_end_frame` (NGG:544-553, 621-625) to return TRUE while the CE
  client is active, or the missing client resets the connection to local every frame.
- `network_game_player_is_local` (NGG:380) says TRUE with no client: do not use network_game_spawn_player.
- `network_game_get_game()` is NULL (scoreboard/postgame UI reads it: ~40 uses in source/interface): give the UI a
  network_game built from CE's game_settings_update, or hook those reads.

## Objects (CE names them by a network index 0..511)
- Own table CE index -> datum index; create in a FREE slot (any nonzero identifier, data.c:268).
- Export small wrappers around static NO functions: distributed_client_create (NO:2357), distributed_client_delete
  (NO:2135), distributed_client_apply_inventory (NO:2805) — rather than copying their wire structs.
- Move: network_objects_reconcile (NO:835, blend) / network_objects_correct (NO:820); seats network_objects_set_seat
  (NO:3035); health/shield damage_set_network_state (damage.c:2713); weapons unit_network_add/drop/forget_weapon
  (units.c:11988-12024).
- After the first burst of *_new: network_objects_handle_synchronized() (NO:2479) deletes unregistered map-placed
  bipeds/vehicles/weapons/equipment (the client's own duplicates).

## Input and corrections
- Own input: after each tick read `update_client_distributed_input(local, &tick, &action, flags)` (PQ:1218) and send
  CE client_game_update. Remote players: `update_client_handle_relayed_action` (PQ:1201).
- Own unit corrections: wrapper around static distributed_correct_own_unit (ND:1694; 64-tick ring from
  distributed_note_own_positions ND:1662; tolerance 3.0 ND:269).
- Outgoing Delta traffic exits only via network_distributed_client_send/_send_reliably (NGG:278/295): hook them to
  drop Delta messages while the CE client is active.

## Damage, kills, scores, chat
- Effects: damage_replay_player_effect/aftermath/kill/static (damage.c:2750-2794, struct damage_data damage.h:47).
- Kills: distributed_set_death (ND:3015) then the engine's game_engine_player_killed path; unit_kill_no_statistics.
- Reloads: the _unit_control_weapon_reload_bit control flag in relayed actions; ammo via inventories.
- Score: game_engine_write_network_state -> patch -> game_engine_read_network_state (game_engine.c:9670/9697);
  statistics: player->statistics; end: game_engine_end_game (5703).
- Chat in: chat_bridge_message(NONE, 88, entry, size, TRUE) (chat.c:1060) with delta_chat_bridge_write_line;
  chat out: bridge_say (chat.c:1102) -> our hook; local notes chat_note.
