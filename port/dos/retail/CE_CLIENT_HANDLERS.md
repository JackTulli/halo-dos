# Halo CE 1.10 client: how every network message is handled (static analysis)

Program: `haloce.exe` 01.00.10.0621 (image base 0x400000). Addresses are haloce.exe's unless marked
"ded" (haloceded.exe). Nothing was run. Method: capstone disassembly + Ghidra 11.4.3 headless
decompilation (project `ce-work/client/gproj`, scripts `ce-work/client/gscripts`, raw outputs
`ce-work/client/out/*.c`). haloce.exe also contains the host code (it can host), so the sending side
was read from the same binary; its tunables have the defaults shown in section 7.

Certainty: **[C]** confirmed in code (instructions read), **[L]** likely (decompiler output, consistent
with everything else), **[G]** guess (named by analogy, not traced). Unmarked statements are [C].

Field names/offsets are those of `ce_deltas_1.10.txt`. "net index" = the 9-bit object / 5-bit player
translated index on the wire (0 = NONE). "role" = object datum +4, the network role:
0 master (host-owned), 1 puppet (client copy of a host object), 2 locally controlled (the client's own
unit), 3 detached/dying (dead unit, detonated projectile; no longer driven by the network).

---------------------------------------------------------------------------------------------------

## 0. The short version (what a correct client must do)

1. **No baseline acknowledgements exist.** The client never acks an object baseline, a remote-player
   baseline id or a sequence number. Consistency comes from the transport instead:
   - every *baseline* the host sends (object `*_update` with `update_baseline`=1, stateless
     `remote_player_action_update`/`super_remote_players_action_update`/`remote_player_total_update_*`,
     all `*_new`, `player_score_update`, game-engine `*_update`) goes **GT2-reliable**;
   - incremental updates go **unreliable** and carry the id of the baseline they were encoded against;
     the client **drops** an incremental whose baseline id/index differs from the one it holds (and
     drops stale ones by sequence), and simply waits for the next reliable baseline.
2. **The client's own input** (`client_game_update`, 13) goes **reliable**, one message per rendered
   frame that ran >= 1 game tick, **incremental against the previous update the client sent**
   (the host decodes against the previous one it received; reliability keeps both in step; first
   baseline = all zero). Header `id` = 6-bit update counter. Body = the frame's action and
   `ticks_to_apply_update_to` = ticks the frame ran.
3. **The host acknowledges the client's updates** with `local_player_update` (35) /
   `local_player_vehicle_update` (36), unreliable, at most every 15 ticks: `last_completed_update_id`
   + the authoritative position after that update; 5-bit `sequence_number` orders them. The client
   rewinds its unit to that position and **re-simulates every later unacknowledged update**
   (history of max 64 updates; if full, the client stops sending and zeroes its movement input).
4. **Object baselines** live per object: (`baseline_index` u8, `message_index` u8, body copy). An
   incremental `*_update` is applied only if `header.baseline_index == stored` and `message_index` is
   newer (8-bit window, below); `update_baseline`=1 replaces the stored body and index.
5. **Remote players**: actions arrive as (update_id, ticks, action) and are replayed one tick at a time
   (queue); positions arrive tagged with an update id and are applied when that update's ticks have
   been replayed (teleport if more than 1.1 world units off).
6. Every iteration of every message must be decoded or discarded with the right layout; a message the
   client cannot handle (decode failure, or a type it has no handler for: 13, 52, 55) puts the client
   **out of sync** (displayed error 8, "Your game has gone out of sync...").
7. Join rejection: `machine_rejected.reason` r is shown as displayed error `43 + r` (section 8).

---------------------------------------------------------------------------------------------------

## 1. Receive path

| step | address | what |
|---|---|---|
| connection read loop | 0x4DE5D0 | per GT2 message: copy to 0x7FD180, read the 11-bit size, then while >= 8 bits left: 1 kind bit -> 0x4DEA80 |
| kind switch | 0x4DEA80 | kind 1 -> 0x4DECC0 (message delta); kind 0 -> read 16-bit header -> 0x4DEB00 (old packet) |
| one message delta | 0x4DECC0 | `mdp_decode_message_header` 0x4EFDA0 (mode, type, iteration count), then per iteration: `mdp_decode_iteration_header` 0x4EFE20 (presence bits + header fields), **dispatcher 0x4DD760**, then require that the iteration's header *and* body were consumed (decoding_information +0x1C/+0x1D both 1); else -> 0x4DC740 |
| out of sync | 0x4DC740 | once: `client->is_out_of_sync` (+0xF20) = 1 and displayed error 8 for every local player (0x49BEB0(8, ...)) |
| body decode, stateless | 0x4EFEA0 | `mdp_decode_body_fields(exists, baseline=NULL, dest=ecx)`; fails if 0 bits decoded |
| body decode, incremental | 0x4EFF10(force) | `mdp_decode_body_fields(exists, baseline=edx, dest=ecx)`; fails if 0 bits unless `force` |
| body discard | 0x4EFF80 | decodes into a 2 KB scratch (baseline = scratch when mode 1, NULL when mode 0) -- keeps the bit position right |
| field loop | 0x4F0AE0 | for each body field with exists[i]: `field->decode(props, baseline ? baseline+field.baseline_offset : NULL, dest+field.offset, stream)` |

Dispatcher 0x4DD760 (= OpenSauce `NETWORK_GAME_CLIENT_HANDLE_MESSAGE_DELTA_MESSAGE_BODY`):
- `game_connection` (word 0x6B47B0) == 1 (client): jump table 0x4DDA84[type] (57 entries).
- == 2 (this machine hosts and also reads a stream, i.e. listen server): only types 6, 11, 15, 26, 33,
  34, 53 (byte table 0x4DDB88 -> 0x4DDB68).
- while a handler runs, byte 0x6B7350 (`dont_send_object_new_msg`) = 1 (suppresses re-broadcast).

Every handler gets the decoding_information/header block in EAX (or EDX) and checks
`decoding_information.mode` itself: almost all accept **only mode 0** and discard mode 1 bodies
(column "modes" below). The **translated index** fields decode to the raw net index; handlers
translate with `params->translated_index_allocations[net]` (+0x28 of the field parameters:
object 0x622668, player 0x622A90), giving the local datum or -1; net index 0 -> NONE.
Net index tables are filled by `*_new`/`player_create` (0x4ED5C0: only if the slot is free) and
emptied by 0x4ED630 (object_deletion, unit_kill, projectile_detonate, see below).

---------------------------------------------------------------------------------------------------

## 2. Dispatcher table (client, connection == 1)

`md_*` names are mine. Modes: which `mode` values the handler decodes (others are discarded).

| id | message | handler | modes | baseline used for incremental | effect (details in section 4) |
|---|---|---|---|---|---|
| 0 | object_deletion | 0x4F9460 | 0 | - | unmap net index, delete object |
| 1 | projectile_update | 0x4F7F90 -> type slot +0x70 = 0x4C4430 | 0/1 | object +0x27C (24 B) | 3.1 |
| 2 | equipment_update | 0x4F7F90 -> 0x4BF640 | 0/1 | object +0x248 (36 B) | 3.1 |
| 3 | weapon_update | 0x4F7F90 -> 0x4C9430 | 0/1 | object +0x2E4 (44 B) | 3.1 |
| 4 | biped_update | 0x4F7F90 -> 0x55EAB0 | 0/1 | object +0x52C (16 B) | 3.1 |
| 5 | vehicle_update | 0x4F7F90 -> 0x575DE0 | 0/1 | object +0x528 (64 B) | 3.1 |
| 6 | hud_add_item | 0x4B15F0 | 0 | - | pickup HUD message, screen effect, sound |
| 7 | player_create | 0x47A400 | 0 | - | create player datum, map player net index |
| 8 | player_spawn | 0x47A7B0 | 0 | - | bind unit, team, weapons, seat, powerups |
| 9 | player_exit_vehicle | 0x56FB00 | 0 | - | unit leaves seat |
| 10 | player_set_action_result | 0x47BA50 | 0 | - | player_handle_action / weapon swap |
| 11 | player_effect_start | 0x457440 | 0 | - | damage effect on the first local player |
| 12 | unit_kill | 0x56A150 | 0 | - | death ping/animation, unit_died, respawn timer, unmap |
| 13 | client_game_update | none (0x4DDA7A) | - | - | **not handled: out of sync** |
| 14 | player_handle_powerup | 0x47C680 | 0 | - | player_powerup_on / _additional |
| 15 | hud_chat | 0x4AE330 | 0 | - | chat line |
| 16-20 | slayer/ctf/oddball/king/race_update | 0x4699A0 -> game_engine vtable +0x94 (slayer 0x4726D0) | 0/1 | client's copy of the last received state (slayer 0x64C308) | 3.6 |
| 21 | player_score_update | 0x469840 | 0/1 | per-player copy, table 0x64BAC0 + slot*0x30 | 3.6 |
| 22 | game_engine_change_mode | 0x469D70 | 0 | - | postgame states 1/2/3 |
| 23 | game_engine_map_reset | 0x46AE60 | 0 | - | reset round |
| 24 | multiplayer_hud_message | 0x461530 | 0 | - | game-engine HUD message |
| 25 | multiplayer_sound | 0x46E810 | 0 | - | announcer / mp sound |
| 26 | team_change | 0x4735D0 | 0 | - | set a player's team (team games only) |
| 27 | unit_drop_current_weapon | 0x5714B0 | 0 | - | ready + drop weapon |
| 28 | vehicle_new | 0x575810 | 0 | - | create puppet, set baseline |
| 29 | biped_new | 0x55E5D0 | 0 | - | create puppet, set baseline |
| 30 | projectile_new | 0x4C4060 | 0 | - | create puppet, set baseline |
| 31 | equipment_new | 0x4BF210 | 0 | - | create puppet, set baseline |
| 32 | weapon_new | 0x4C8FD0 | 0 | - | create puppet, set baseline |
| 33 | game_settings_update | 0x4E1DA0 | 0 | - | network_game_client_game_settings_updated |
| 34 | parameters_protocol | 0x4EF910 (+0x4EFCA0) | 0 | - | delta parameter changeover |
| 35 | local_player_update | 0x4E8CA0 | 0 | - | ack + prediction replay (3.5, 6) |
| 36 | local_player_vehicle_update | 0x4E8DA0 | 0 | - | ack + vehicle replay |
| 37 | remote_player_action_update | 0x4E8F30 | 0/1 | player +0xF0 (48 B), id player +0xEC | 3.3 |
| 38 | super_remote_players_action_update | 0x4E9030 | 0/1 | same, per iteration | 3.3 |
| 39 | remote_player_position_update | 0x4E9550 | 0/1 | player +0x164 (12 B) | 3.4 |
| 40 | remote_player_vehicle_update | 0x4E9670 | 0/1 | player +0x190 (64 B) | 3.4 |
| 41 | remote_player_total_update_biped | 0x4E9180 | 0/1 | player +0xF0 (48 B) + +0x164 (12 B) | 3.3 + 3.4 |
| 42 | remote_player_total_update_vehicle | 0x4E9340 | 0/1 | player +0xF0 (48 B) + +0x190 (64 B) | 3.3 + 3.4 |
| 43 | weapon_start_reload | 0x4C68F0 | 0 | - | reload bookkeeping |
| 44 | weapon_ammo_pickup_mid_reload | 0x4C5960 | 0 | - | add rounds |
| 45 | weapon_finish_reload | 0x4C6C30 | 0 | - | set rounds, end reload |
| 46 | weapon_cancel_reload | 0x4C7E80 | 0 | - | set rounds, stop reload |
| 47 | netgame_equipment_new | 0x460450 | 0 | - | spawn a netgame-equipment item |
| 48 | projectile_detonate | 0x4C0F00 | 0 | - | detonate + delete |
| 49 | item_accelerate | 0x4F3590 | 0 | - | push an item |
| 50 | damage_dealt | 0x4F1DE0 | 0 | - | shield hit feedback |
| 51 | projectile_attach | 0x4C2580 | 0 | - | stick projectile to object |
| 52 | client_to_server_pong | none | - | - | **not handled: out of sync** |
| 53 | super_ping_update | 0x4DEEF0 | any (stateless decode) | - | store pings, answer with pong |
| 54 | sv_motd | 0x4E8A60 | 0 | - | copy to 0x6B75B0 (255 chars) |
| 55 | rcon_request | none | - | - | **not handled: out of sync** |
| 56 | rcon_response | 0x4E8730 | 0 | - | print to console |

---------------------------------------------------------------------------------------------------

## 3. Baselines, ids and sequence numbers

### 3.1 Object updates (1-5)

Per object (datum offsets; the "body" is exactly the body struct of the matching `*_update`):

| type | object mask | `baseline_index` | last `message_index` | has-baseline flag | stored baseline body | "latest update" copy |
|---|---|---|---|---|---|---|
| projectile | 0x20 | +0x27A | +0x27B | +0x279 | +0x27C (24) | +0x294 flag, +0x298 |
| equipment | 0x08 | +0x245 | +0x246 | +0x244 | +0x248 (36) | +0x26C flag, +0x270 |
| weapon | 0x04 | +0x2E1 | +0x2E2 | +0x2E0 | +0x2E4 (44) | +0x310 flag, +0x314 |
| biped | 0x01 | +0x527 | +0x528 | +0x526 | +0x52C (16) | +0x53C flag, +0x540 |
| vehicle | 0x02 | +0x526 | +0x527 | +0x525 | +0x528 (64) | +0x56C (64) |

Dispatch (0x4F7F90): translate `header.object_index`; if no local object -> discard the body
(0x4F7F30 -> 0x4EFF80). Otherwise call the most derived object type's network callback +0x70
(type definitions 0x637694[object type], 16 parent slots +0x80).

Common rule of all five handlers (e.g. weapon 0x4C9430):
```
if (object.flags & _object_has_been_updated_from_network_bit /*0x8000000, bit 27*/)
    && mode == incremental
    && ( header.baseline_index != object.baseline_index
         || (header.message_index <= object.message_index
             && (object.message_index - header.message_index) <= 225) )   // 0x4BF6AA: (m - last + 255) >= 30
    -> discard body                                                     // stale or wrong baseline
dest = copy of object.baseline
ok = mode==1 ? decode_incremental(baseline = object.baseline, dest) : decode_stateless(dest)
if ok:
    object.message_index = header.message_index
    object.flags |= 0x8000000
    if header.update_baseline: object.baseline_index = header.baseline_index; object.baseline = dest
    apply(dest)        // per type, below
```
Note the stale test only applies once the object has had one network update and only to
incremental messages; stateless updates are always applied. `message_index` counts 0..254 on the
host (0xFF is skipped: 0x55E900 / 0x4C92D0 reset 255 to 0). An incremental with no field present
decodes 0 bits and is treated as a failure (nothing applied; the host never sends one).

Host side (haloce's own host code, 0x45BFF0 per tick): for each master object due for an update
(type slot +0x74; rates in section 7), if a new baseline is due (0x4F7FC0: object flag bit 26
`force_baseline_update`, or the object just came to rest) the type's slot +0x68 refreshes it
(`baseline_index++` (u8 wrap), baseline = current state, `message_index` = 0, e.g. weapon 0x4C9240)
and the message is sent **stateless with update_baseline=1, reliable**; otherwise **incremental
against the stored baseline, update_baseline=0, unreliable** (0x4E4EF0 reliable flag = baseline).
`message_index` is incremented after each successful encode.

Apply step per type:
- **projectile** (0x4C4430): position -> net position (+0x1C, valid +0x18), velocity -> object
  velocity (+0x68) and net velocity (+0x48, valid +0x44). If the object is connected to the map
  (flag bit 11) and (mode == stateless, or distance(net pos, current pos) > 0.2 [0x631730]):
  `object_set_position_network` (0x4F8BD0). Copy to the "latest" slot.
- **equipment** (0x4BF640): position, translational velocity (-> +0x68 and net), angular velocity
  (-> +0x8C). If distance > 2.0 [0x631408] or the object is at rest (flag bit 5):
  `object_set_position_network`.
- **weapon** (0x4C9430): position, velocity as projectile; `magazine_0/1_rounds_total` ->
  magazine[0/1].rounds_unloaded (+0x2B6 / +0x2C2) **only if that magazine is not reloading**
  (magazine state +0x2B0 / +0x2BC != 1); `age` -> weapon age (+0x240) **only from stateless
  messages**; set position if connected and (distance > 0.5 [0x631B40] or at rest or update_baseline).
- **biped** (0x55EAB0): `grenade_counts` -> unit grenades (+0x31E, two bytes); `body_vitality` ->
  object body vitality (+0xE0); `shield_vitality` -> object shield vitality (+0xE4) = value * 3.0
  **only if header.force_shield_update**; `shield_stun_ticks_greater_than_zero` -> shield stun ticks
  (+0x104) = (bool == 1); unit "network data changed" (+0x475) = 1. No position (bipeds are moved by
  the player messages, 3.3/3.4).
- **vehicle** (0x575DE0): forward/up normalized; net position, velocity, forward/up set (valid flags);
  `at_rest_bit` == 0 clears object flag bit 5; object velocity, angular velocity, forward, up set
  directly (+0x68/+0x8C/+0x74/+0x80), then 0x5743C0 [L: vehicle physics state refresh]. If distance > 10.0
  or at rest or dot(new forward, old forward) < 0.707: hard snap (position too). Timestamp: if the
  client's time sync has >= 11 samples (section 5.6) net timestamp (+0x58, valid +0x54) =
  `header.timestamp - time_delta`, else no timestamp. +0x475 = 1. Latest copy at +0x56C.

### 3.2 Object creation (28-32, 47): the baseline starts here

Common (`md_*_new`, e.g. weapon 0x4C8FD0): mode must be 0 (else discard). Decode stateless,
normalize forward/up, translate `owner_player_index` (player table) and `owner_object_index`
(object table). Build `object_placement_data` (0x88 bytes, zeroed): definition_index, owner player
(+8), owner object (+0xC), forward (+0x34), up (+0x40), and per type velocities/flags; then
**`object_new_with_role(placement, 1)` (0x4F8DC0, role 1 = puppet)**; on success **map
`server_object_index` -> new datum (0x4ED5C0)** and initialize the baseline from the message:

| message | baseline body set from | baseline_index | message_index | extra |
|---|---|---|---|---|
| projectile_new | position, translational_velocity | current_baseline_index | 0 | placement flags bit 1 set; angular velocity in placement; position via set_position_network |
| equipment_new | position, translational_velocity, angular_velocity | current_baseline_index | 0 | `object_flags` ORed into object flags |
| weapon_new | position, translational_velocity, magazine totals, age | current_baseline_index | 0 | `object_flags` ORed; magazines: rounds_unloaded = totals, rounds_loaded = `magazine_*_rounds_loaded`; age |
| biped_new | grenade_counts, body_vitality, shield_vitality, stun bool | current_baseline_index | 0 | body vitality, shield = value*3.0, change colors, integrated light bit (+0x204 bit 19) and battery (+0x344), grenades |
| vehicle_new | position, translational_velocity, angular_velocity, forward, up | current_baseline_index | 0 | `weapon_object_indices[4]`: each mapped weapon added to the vehicle's inventory (0x570B00), else slot = NONE |

Then `object_set_position_network` (0x4F8BD0) and velocities copied from the baseline.
`netgame_equipment_new` (47, 0x460450) carries no baseline: the item is placed from the scenario's
netgame equipment entry `netgame_equipment_index` (entry 0x90 bytes: position +0x40, facing +0x4C ->
forward (cos, sin, 0)), created with role 1, mapped, flag bit 5 (at rest) if the entry's flag bit 0,
and its baseline is built **locally** by the type's +0x68 refresh (0x4F7E70) exactly as the host does
when it creates the same item, so both sides start from the same baseline_index.

### 3.3 Remote player actions (37, 38 and the action part of 41/42)

Per remote player (player datum +0xE8.. = `s_player_client_update.remote_player`):
`last_acked_update_id` +0xE8 (init -1), `baseline_id` +0xEC, baseline action data +0xF0 (48 bytes:
ticks, action, facing vector).

37 (0x4E8F30): translate `player_index`; if the player is missing discard. mode 1: dest = copy of
baseline, `decode_body_fields(baseline = player+0xF0, dest)`; mode 0: stateless. Then 0x4E99D0:
```
if stateless: player.baseline_id = header.baseline_id; player.baseline = dest
else:
    if header.baseline_id != player.baseline_id -> throw away   ("...base baseline, [%d] != [%d]")
    d = header.update_id - player.last_acked_update_id  (mod 64: if new <= last, d += 64)
    if d > 31 -> throw away (old)
if player.last_acked_update_id != -1:
    action.desired_facing.yaw   = atan2(facing.y, facing.x)
    action.desired_facing.pitch = atan2(facing.z, sqrt(x^2 + y^2))
    action.desired_zoom = -1 (0xFFFF)
    push (update_id, ticks_to_apply_update_to, action) on the player's action queue (+0x120, 0x47CD40)
player.last_acked_update_id = header.update_id
```
(The very first message only sets `last_acked_update_id`; nothing is queued.)

38 (0x4E9030), 16 iterations, each handled like 37; for incremental iterations the baseline is the
player's stored one **only if the player is remote** (local_player_index == -1), the action is
queued only for remote players.

Host side (0x4EB1A0 / 0x4EB950 / 0x4EBDE0 / 0x4EC000): `baseline_id` toggles 0/1 at every new
baseline (`(id + 1) & 1`); a new baseline is sent every `remote_player_action_baseline_update_rate`
(180 ticks) **stateless and reliable**; in between, incremental actions every
`remote_player_action_update_rate` (0 = every tick) **unreliable**. With
`use_super_remote_players_action_update` (default 1) they are batched per tick into one 38 for all
players (one batch for baselines (reliable), one for incrementals (unreliable)).
So a lost incremental is simply skipped; incrementals that arrive before the new baseline (or after a
lost one) are thrown away until the reliable baseline arrives.

Playback (client, every tick, 0x477130 `players_update_before_game` client branch): for each remote
player pop one tick from its action queue (0x47CB30) and drive the unit with it; when the popped tick
is the last tick of its update, apply that update's queued position/vehicle state (3.4). If the queue
is empty: "No action available" -> repeat the last action if flag +0x138, else a zero action.
After the tick (0x4798E0): while a remote player's queue holds more than 2 updates or more than 6 ticks
(0x623D7C / 0x623D80), pop and simulate extra ticks to catch up.

### 3.4 Remote player position / vehicle (39, 40 and the position part of 41/42)

Per remote player: position part `last_acked_sequence_number` +0x15C (init -1), `last_acked_update_id`
+0x160, position baseline +0x164 (12 B), position queue +0x170; vehicle part
`last_acked_update_id` +0x18C, vehicle baseline +0x190 (64 B), vehicle queue +0x1D0, counters +0x188
(position "ignored" count) / +0x1E8 (vehicle "ignored" count).

- 39 (0x4E9550): mode 0: decode, store position as baseline (+0x164); mode 1: incremental against
  +0x164 (baseline not replaced). Then 0x4E9B80(update_id, sequence_number, position).
- 40 (0x4E9670): mode 0: decode, normalize, store 64-byte vehicle data as baseline (+0x190); mode 1:
  incremental against +0x190. Then 0x4E9E20.
- 41 (0x4E9180, only for remote players): mode 0 stateless -> position baseline (+0x164) = position;
  mode 1 decodes against the concatenation of action baseline (+0xF0) and position baseline (+0x164).
  Then 0x4E99D0 (action, as 37) and 0x4E9B80 (position).
- 42 (0x4E9340): same with the vehicle baseline +0x190 (stored on stateless). Then 0x4E99D0, 0x4E9E20.

Sequence check 0x4EA330 (3-bit `remote_player_update_sequence_number`):
accept if last == -1, or new > last and new - last <= 3, or new <= last and new - last + 8 <= 3.

Position apply 0x4E9B80 (vehicle 0x4E9E20 analogous):
```
if !sequence_ok -> drop
if player.position.last_acked_update_id != -1:
    k = update_id - (update id at the head of the player's action queue)   (mod 64; -1 if queue empty)
    if k < 0 or k > 31:
        ignored_count++
        if ignored_count >= 3 (vehicle: >= 2): ignored_count = 0; "apply immediately":
            biped: if the unit is not in a vehicle and role == 1 and distance > 1.0: 0x479E20 (snap)
            vehicle: if the unit's vehicle == message vehicle: set vehicle velocity, angular velocity,
                     forward, up (0x5743C0)
    else: queue (update_id, k, position | vehicle data) on +0x170 | +0x1D0; ignored_count = 0
    player.position.last_acked_sequence_number = sequence_number
player.position.last_acked_update_id = update_id          (vehicle: +0x18C)
```
Queued positions are applied when the action replay finishes that update id (0x479E90 / 0x479FD0):
biped: if unit not in a vehicle and role == 1 -> 0x479E20: if distance > 1.1 zero the velocity and set
at-rest, then `object_set_position_network`. Vehicle: set position/velocities/orientation of the
unit's vehicle if it is the same vehicle.

Host defaults (0x4EB460, per player per tick): when the player's position is due -
`remote_player_position_update_rate` 30 ticks (incremental, unreliable) or a new baseline every 90
ticks (stateless, reliable) - the host sends **41** (on foot) or **42** (in a vehicle, 0x479D50; rates
15 / 90) carrying action + position; otherwise only the action (37/38). Sequence number +1 (mod 8)
per 41/42 send. **39 and 40 are only sent when a machine joins** (0x4EB6A0, from 0x4E2E70: stateless,
reliable, together with a stateless 37 for every existing player), so the client sees them once per
player at join.

### 3.5 Local player acknowledgement (35, 36)

Per local player: `last_acked_sequence_number` +0xE8 (init -1), `last_acked_update_id` +0xEC,
server position +0xF0.

35 (0x4E8CA0): mode 0 only; target = first player with local_player_index != -1.
Sequence check 0x4EA2C0 (5-bit): accept if last == -1, or new > last and new - last < 16,
or new <= last and new - last + 32 < 16 (so duplicates and old ones are rejected).
On accept: +0xE8 = sequence_number, +0xEC = last_completed_update_id, +0xF0 = position;
0x4EA260 -> 0x4EA900(history, unit, position, NULL): prediction replay (section 6).

36 (0x4E8DA0): same check against the same counters; translates `vehicle_index`, normalizes
forward/up; on accept stores sequence/update id/position and calls 0x4EA900 with the vehicle data.

Host side (0x4EBAF0 / 0x4EBC00): sent **unreliable, stateless**, only when the host has completed
at least one update of that player and (never acked or `local_player_update_rate`/`..._vehicle_update_rate`
(15 ticks) passed); `sequence_number` = host counter (+1 mod 32 per send); `last_completed_update_id`
= last client update the host finished simulating; `position` = the unit's (or its vehicle's)
position after it (0x479320). 36 is used when the player is in a vehicle (0x479D50) and
`use_new_vehicle_update_scheme` (default 1); its body is the vehicle's position, velocities and
orientation.

### 3.6 Scores and game-engine state (16-21)

Reliable on the host (0x469B50, 0x4725D0..: reliable flag 1). The client keeps a copy of the last
received state and decodes incrementals against it, then replaces the copy:
- 21 `player_score_update` (0x469840): slot = 0x4699C0(player) in table 0x64BAB8 (16 x 0x30: in-use
  byte, player index, 40-byte body); slots are created by player_create (0x4697A0). If no slot ->
  discard. mode 0 stateless into the slot; mode 1 incremental with the slot as baseline. Then player
  datum: kills[4] +0x9C, assists[4] +0xA4, friendly_fire_kills +0xAC, deaths +0xAE, suicides +0xB0,
  ctf stats +0xC4.. (flag grabs/returns/scores), special +0x88 (target player), is_odd_man_out +0x8C,
  speed_multiplier +0x6C; in game type 4 (oddball? `0x68CC78 == 4`) +0xC4 = flag_grabs * 30.
- 16-20 (0x4699A0 -> current game engine's +0x94): slayer 0x4726D0: mode 0 stateless into the live
  slayer globals 0x815940, mode 1 incremental with baseline 0x64C308 (copy of the last received state).
  ctf/oddball/king/race analogous [L].

---------------------------------------------------------------------------------------------------

## 4. The other handlers

- **0 object_deletion** (0x4F9460): mode 0; translate; if the object header is not already
  "being deleted" (header byte +2 bit 3) unmap its net index (0x4ED630); if the object exists,
  delete it (0x4F92E0, recursive over children).
- **6 hud_add_item** (0x4B15F0): for the first local player: 0x4B17F0 shows the pickup message
  (`message_offset`/`quantity`); by item definition: equipment powerup type 2 (overshield): screen
  effect 0x47C250 and (client) unit damage-flag bit 4 (+0x106 |= 0x10, shield overcharging); 3 (camo):
  0x47C310; 5 (health): 0x47C3D0; then the item's pickup sound (equipment +0x31C / weapon +0x49C tag)
  via 0x547220.
- **7 player_create** (0x47A400): network player = game.players[`player_list_index`] (game +0x1E2,
  0x20 each). If no player datum at `raw_player_index`: create it (0x4E1D10 -> player_new for that
  network player), map `translated_player_index` -> it (0x4ED5C0 on the player field 0x622AC0), create
  its update-client queue datum (0x4D37D0 on 0x692ED0) and score slot (0x4697A0). Then team
  (+0x20 and network team +0x66) = `team_index`; local players: 0x477910 (bind controller); powerup
  times = 0, action result reset; 0x45CDD0 (game engine player added). Player datum indices mirror the
  host's: `player_number` fields elsewhere (hud_chat, super_ping) are the absolute datum index.
- **8 player_spawn** (0x47A7B0): requires player and mapped unit (unit mask 3). player.unit (+0x34)
  = unit; team (+0x20, +0x66); unit owner player (+0xC0 and +0x218) = player, unit team (+0xB8);
  `unit_set_actively_controlled` 0x56D2F0. Remote player: clear its position and vehicle queues.
  Local player: unit role = 2 and local control initialised from the unit's aim (0x473A40). Powerup
  times and action result reset; 0x461F20 (game-engine spawn notification). Inventory: for each of the
  4 `weapon_object_indices`: mapped -> add to unit (0x570B00) else slot NONE; current weapon (+0x2F2)
  = NONE, desired weapon (+0x2F4) = `desired_weapon_index`. If `parent_vehicle_index` mapped:
  `unit_enter_seat(unit, vehicle, vehicle_seat_index)` 0x569E30. `powerup_durations[0/1]` > 0:
  `player_powerup_on/additional` (0x47C6E0) for active camouflage / full-spectrum vision.
- **9 player_exit_vehicle** (0x56FB00): unit mapped; unless `forced_exit`, try
  `unit_try_and_exit_seat` 0x56FB70 (done if it succeeds); otherwise `unit_exit_seat_end` 0x56FD40.
- **10 player_set_action_result** (0x47BA50): player mapped, action object mapped (else ignored if
  its net index was nonzero); player action result: result (+0x28) = `action_result`, object (+0x24),
  seat (+0x2A). `action_result_type` 0 -> `player_handle_action` (0x47BEE0; Halo's enum: 0 reload,
  1 pickup powerup, 2 pickup weapon, 3 exit vehicle, 4 swap grenades, 5 swap powerup, 6 swap weapon,
  7 add weapon to inventory, 8 enter vehicle, 9 evict, 10 touch device, 11 flip vehicle);
  nonzero -> 0x47BD80: result 6: ready `weapon_to_drop_as_result_of_swap`, drop it
  (`unit_drop_current_weapon`), add the new weapon (0x570B00); result 7: add weapon (0x570B00) + HUD.
- **11 player_effect_start** (0x457440): finds the first local player and starts the damage effect
  `damage_definition_index` with owner/flags/direction/scale/total_damage (0x457660) [L].
- **12 unit_kill** (0x56A150): unit mapped (mask 3): damage flags +0x106 |= 4 (dead), body and shield
  vitality = 0. If `should_cause_ping`: `unit_ping_animation(unit, died, feign_death, died_flying,
  ignore_hard_pings, force_hard_pings, angle, damage_part, should_alignment_vector_be_null ? NULL :
  &alignment_vector)` (0x56A2A0). The unit's player's `respawn_time` (+0x2C) = `respawn_timer`.
  `unit_died(unit, feign_death)` (0x56C170). Unit role = 3. Unless already being deleted, **unmap
  the unit's net index** (the corpse becomes client-local; the host reuses the index).
- **14 player_handle_powerup** (0x47C680): `player_powerup_on/additional(player, powerup_type,
  duration_in_ticks)`.
- **15 hud_chat** (0x4AE330): message buffer 0x200 wide chars. `player_number` != 0xFF: player =
  datum at that absolute index (missing -> ignore); prefix from `ui\multiplayer_game_text`
  (unicode_string_list): string 187 for `msg_type` 0 (all), 188 for 1 and 2 (team, vehicle),
  formatted with the player's name; other types: no prefix; prefix + message -> chat log (0x4AE8A0).
  `player_number` == 0xFF: type 3 -> `"** SERVER **  " + message`; type 4 -> message is a decimal
  string-resource id loaded from strings.dll (0x581790, language fallback 0x409); other types dropped.
- **22 game_engine_change_mode** (0x469D70): `new_mode` 1: game over (postgame_state 1, timer 7.0 s,
  multiplayer sound 0, close widgets 0x49B5E0); 2: postgame_state 2, timer 5.0 s, every player unit
  damage flag +0x106 |= 0x20; 3: postgame_state 3 (carnage report; timer from the variant if set).
  (Xbox source equivalent: `game_engine_end_game` / `game_engine_switch_to_postgame`.)
- **23 game_engine_map_reset** (0x46AE60): 0x46A9A0, 0x46AB50, 0x46AAB0 (reset/garbage-collect
  map objects [L]) and the game engine's +0xAC reset.
- **24 multiplayer_hud_message** (0x461530): player translated -> 0x461590(player, message,
  message_data): game engine +0x6C formats the message (else 0x45F0B0) and shows it on that player's
  HUD (types 0-6, 8, 13, 19, 27, 28 are flagged differently [L]).
- **25 multiplayer_sound** (0x46E810): `index` -> 0x46E840: plays entry `index` of the globals'
  multiplayer sound list (index 0x2B uses the hit-sound volume 0x624A64).
- **26 team_change** (0x4735D0): only with a game engine and a team game; in network game player list
  the entry with player_list_index `player_number` gets team `new_team_choice` (0x4731B0), and the
  player datum with that list index gets team (+0x20, +0x66) (0x4731F0).
- **27 unit_drop_current_weapon** (0x5714B0): unit and weapon mapped: if the unit's current weapon
  is not `weapon_to_drop`, make it the desired weapon and ready it (`unit_ready_desired_weapon`); if it
  then is the current weapon: `unit_drop_current_weapon(unit, immediate)`.
- **33 game_settings_update** (0x4E1DA0): ignored when hosting or when the client state (+0xF1A) is
  not 2 (pregame) or 3 (in game); mode 0 into a 1008-byte buffer -> 0x4DCC40
  `network_game_client_game_settings_updated`.
- **34 parameters_protocol** (0x4EF910): only if the changeover global (0x6B8240) is set: apply the
  parameters (0x4EFB40), version byte -> 0x6B8244; then 0x4EFCA0 re-initializes all definitions.
- **43 weapon_start_reload** (0x4C68F0): weapon mapped and role 1 (puppet): per magazine
  `starting_total_rounds` -> +0x2D4 + 2m, `starting_loaded_rounds` -> +0x2D8 + 2m; weapon flags
  +0x22C |= 8 (reloading).
- **44 weapon_ammo_pickup_mid_reload** (0x4C5960): puppet weapon: magazine m rounds_unloaded +=
  `rounds_picked_up`.
- **45 weapon_finish_reload** (0x4C6C30): puppet weapon: magazine m rounds_unloaded/loaded =
  ending values, reload timer 0, state 2; flags &= ~8.
- **46 weapon_cancel_reload** (0x4C7E80): puppet weapon: rounds as 45, flags &= ~8,
  `weapon_stop_reload` 0x4C7F10.
- **48 projectile_detonate** (0x4C0F00): unmap (unless being deleted); projectile role 3,
  `object_set_position_network(position)`, detonate (0x4C3A30, 0x4C24B0), `object_delete`.
- **49 item_accelerate** (0x4F3590): object mapped: 0x4C0440(item, direction * magnitude) (adds the
  impulse to the item).
- **50 damage_dealt** (0x4F1DE0): unit mapped: if `shield_damage` > 0: shield damage timer (+0xFC)
  = 0, current shield damage (+0xE8) = 1.0 unless depleted, recent shield damage (+0xF4) +=
  shield_damage (both clamped to 1); if `shields_depleted`: `object_deplete_shield`; a hit ping
  (`unit_ping_animation` with no kill); then 0x4F1C80: hit sound (multiplayer sound 0) at most every
  2 ticks when the hit-sound volume is nonzero.
- **51 projectile_attach** (0x4C2580): projectile and parent mapped: zero velocities, flags,
  `object_attach_to_node(parent, parent_node_index)` (0x4F9D50); sticky limit 6 per parent; detonation
  timer from the projectile definition.
- **53 super_ping_update** (0x4DEEF0): per iteration: player datum at absolute index `player_number`
  -> ping (+0xDC) = `player_ping`; then **a `client_to_server_pong` is queued (unreliable)** with
  `player_number` = the first local player's `player_list_index` (network player +0x1F). This happens
  **once per iteration** (up to 16 pongs per message; the host just recomputes the same ping).
- **54 sv_motd** (0x4E8A60): strncpy to 0x6B75B0 (255 + NUL); shown by the UI.
- **56 rcon_response** (0x4E8730): printed to the console (0x499AB0, "%s"), empty ignored.

---------------------------------------------------------------------------------------------------

## 5. What the client sends

All client->host traffic uses the same stream format as host->client (11-bit size, kind bit,
message). Two ways to send (connection = client +0xADC):
- **reliable stream**: message appended (kind bit 1 + bits) to the connection's pending bit stream
  (+0x10); if it does not fit, the stream is flushed first (0x4E0FB0); 0x4E0FB0 writes the 11-bit
  size and calls `gt2Send(connection, data, bytes, reliable = 1)` (0x5B93D0).
- **unreliable queue**: 0x4E0200 stores (bits, data) in the connection's queue (+0xA7C, 32-byte
  slots) sent unreliably later.

### 5.1 client_game_update (13) - the input
Sender 0x4E1400, called once per rendered frame from the main loop (0x4CB1E3) **after** computing
the number of ticks to run this frame (0x4736F0) and building the local action (0x474690), **before**
running those ticks (0x4737B0 -> game_tick 0x45C0F0). Conditions: client state 3 (in game),
ticks > 0, the byte 0x6B47A8 (2nd argument, written in 0x4CD390) == 0 [G: pause/menu flag], the
first local player has a unit.
- history entry first (0x4EA460, list at client +0xF88): stores the update id, tick count, action and
  a snapshot of the unit (and its vehicle) **before** the frame's ticks; at most 64 entries. If full
  ("Player update history overflow") nothing is sent and the local action is reset to zero movement
  (0x475C50), so input freezes until acks arrive.
- header `id` = history counter (0..63, +1 per update).
- body (struct of 0x34 bytes, client +0xF54 keeps the previous one):
  `ticks_to_apply_update_to` (5 bits) = ticks this frame; `control_flags` = action control flags;
  `desired_facing.yaw/pitch` = action facing (radians); `throttle` (digital) = action throttle;
  `primary_trigger` (1 bit fixed); `desired_weapon_index`, `desired_grenade_index`,
  `desired_zoom_level` from the action (0x692EA4, update_client_globals saved action).
- encoded **incremental (mode 1) against the previous sent body** (0x4EFD60 -> mdpi_encode), then
  written to the reliable stream and flushed at once (`reliable`). The previous body is then replaced.
  Initial previous body: zero [L] (client struct zeroed at creation).
- host (0x4E36F0): keeps the last received body per machine (+0x1C) as baseline, decodes, stores it,
  and queues it if ticks > 0.

### 5.2 Other messages the client sends
| message | when | encoding | channel | address |
|---|---|---|---|---|
| hud_chat (15) | chat entry | stateless: `msg_type` = chat mode (0 all, 1 team, 2 vehicle; 0x64E78C), `player_number` = local player's absolute index, message (<= 254 chars) | reliable stream | 0x4ADD40 (from 0x4ADBF0) |
| team_change (26) | team switch in a team game | stateless: `player_number` = player_list_index, `new_team_choice` | reliable stream | 0x473090 |
| rcon_request (55) | console `rcon` | stateless: password (<= 8 chars), command (<= 64) | reliable stream | 0x4E83B0 |
| client_to_server_pong (52) | each super_ping_update iteration | stateless: `player_number` = player_list_index | unreliable queue | 0x4DEEF0 |
| old packet 1 client_ping | every > 3000 ms while the ping target is set (joined) | long = client time in ms (QueryPerformanceCounter) | unreliable queue | 0x4DC800 |
| old packet 26 client_loaded | after begin_game (state 2), see CE_PROTOCOL | long 0 | reliable | 0x4DCDA0 |
| old packet 30 graceful_game_exit_ingame | leaving the game | long | reliable | 0x4DC620 |
| old packet 18 player_settings_request | profile/team change in pregame | player | reliable | 0x4DD570 |
| old packets 15/16/28/29 add/remove player request | split-screen players joining/leaving | player | reliable | 0x4DC450, 0x4DD690 |

The host's handler of client deltas (0x4E53B0) accepts exactly 13, 15, 26, 52, 55. Host relays chat
itself (0x4ADE10): it overwrites `player_number` with the sender's player and sends type 0 to all,
1 to the sender's team, 2 to players in the same vehicle.

### 5.3 Acks
There is no message-delta ack in either direction. The only acknowledgements are GT2's reliable
acks (transport) and the host's `local_player_update`/`_vehicle_update` (which acknowledge client
input). Pongs (52) only feed the host's ping display.

### 5.4 Time sync (needed for vehicle timestamps)
The client pings with old packet 1 (every 3 s); the host answers old packet 3 `server_pong`
(echoed client time, host time); 0x4DC7B0 adds a sample to the client time struct (client +0xAD8,
`s_network_client_time`, 30 samples) and updates the ping. `time_delta` is used for
`vehicle_update.timestamp` once 11 samples exist.

---------------------------------------------------------------------------------------------------

## 6. Local player prediction and correction

The client simulates its own unit immediately with its own input (role 2). On every accepted
`local_player_update` (0x4EA900):
1. find the history entry with id == `last_completed_update_id` and free it and all older ones
   (0x4EA870); `starting_update` = the next entry. If none ("starting_update == NULL") nothing is
   corrected.
2. skip if the unit is in animation state 0x1A/0x1B (unit +0x2A3) [L: seat enter/exit]; if in a
   vehicle: only for 36 (vehicle data given), only if the entry was taken in that vehicle, and a
   one-shot skip flag (vehicle +0x524).
3. restore the unit state saved in `starting_update` (velocity, forward/up, aim, control state,
   animation state ...) and set **position = the host's position** (vehicle: vehicle state from the
   message: position, velocities, forward, up).
4. for each entry from `starting_update` to the newest: re-apply its action (0x476900) and run its
   tick count of `unit_update` + biped update (0x565A70 + 0x55C560) (vehicle: `object_update`).
No tolerance: the rewind/replay happens on every accepted ack (every 15 ticks by default); the
result is only logged (ClientPlayerUpdateHistory.log stats).

Remote units are not predicted: they replay the host-relayed actions (3.3) and are pulled to the
host's positions at the matching update (3.4).

---------------------------------------------------------------------------------------------------

## 7. Host tunables (defaults in haloce.exe's host code; console variables)

| variable | default (ticks) | effect |
|---|---|---|
| weapon/equipment/projectile_incremental_rate | 15 | object incremental update period |
| biped_incremental_rate | 7 | |
| vehicle_incremental_rate | 4 | |
| local_player_update_rate | 15 | 35 period |
| local_player_vehicle_update_rate | 15 | 36 period |
| remote_player_action_update_rate | 0 | every tick |
| remote_player_action_baseline_update_rate | 180 | new action baseline (toggles baseline_id) |
| remote_player_position_update_rate / _baseline_ | 30 / 90 | |
| remote_player_vehicle_update_rate / _baseline_ | 15 / 90 | |
| use_super_remote_players_action_update | 1 | 38 instead of 37 |
| use_new_vehicle_update_scheme | 1 | 36/40/42 when driving |
| oddball_baseline_rate | 210 | |
(haloce: table 0x624BC8.., values 0x624A68.. / 0x6249A8..; **haloceded has the same defaults**:
table 0x59A568.., e.g. local_player_update_rate at ded 0x59A408 = 15, vehicle_incremental_rate ded
0x59A348 = 4.)

Cross-check with the real dedicated server capture (`ce-work/capture1.bin`, decoded in
`deltas/capture1_decoded.txt`): every `*_update` with `update_baseline`=1 is in a reliable record
(records 10, 13, 21, 23), every incremental `vehicle_update` in an unreliable one; `*_new` carry
`current_baseline_index` 1 and the first refresh arrives as baseline_index 2 with message_index 0.

---------------------------------------------------------------------------------------------------

## 8. Join rejection and other client errors

Old packet 6 `machine_rejected` (0x4DF190, client state not 0/4) -> 0x4DCC20: if no error is
pending, error = **43 + reason**, then the client disconnects (0x6B736E = 1). The error is shown by
0x49BEB0 with `ui\shell\strings\displayed_error_messages` (ui.map, 60 strings):

| reason | error | text (ui.map) | host call sites (lead) |
|---|---|---|---|
| 0 | 43 | Unable to join the game server! | map checksum / other |
| 1 | 44 | The server reported an invalid connection request. | token |
| 2 | 45 | Your password was rejected by the server. | password |
| 3 | 46 | The server is full. | could not add player |
| 4 | 47 | Your CD Key is invalid. | (GameSpy CD key) |
| 5 | 48 | Your CD Key is in use by another player. | (GameSpy CD key) |
| 6 | 49 | The server operator has banned you from playing on this server. | GameSpy CD-key callback + 2 (ban list) |
| 7 | 50 | The server operator has kicked you from this server. | sv_kick |

Other displayed errors a client can hit: 8 out of sync (any undecodable delta, 0x4DC740), 6
"Network connection lost.", 42 "The server you selected requires a password!", 55-57 network
protocol / version mismatch (transport-level rejects). Client states (+0xF1A): 0 none, 1 joining,
2 pregame, 3 in game, 4 postgame (old packet 25 game_over -> 4; 7 game_is_ending_holdup -> 4;
33 reconnect in state 4 -> 0x4DD310 connects to the next game).

Old packets the client accepts (0x4DEB00): 2, 3, 4 (state 1; sets the encoding class), 5 (state 1),
6, 7, 8, 9, 10 (state 2), 11, 12, 13, 22, 23 (add player ingame, state 3: 0x4DD270 ->
`network_game_add_player`), 24 (remove player, states 2-4), 25 (state 3 -> 4), 33 (state 4), 34.

---------------------------------------------------------------------------------------------------

## 9. Uncertain points (most important first)

1. [L] The first `client_game_update` baseline is all zero: the client struct is zeroed when created
   and +0xF54 is only written after a send; not traced through the join code.
2. [L] `player_effect_start` targets the first local player only (iteration code read in Ghidra
   only; the effect call 0x457660 not traced).
3. [L] ctf/oddball/king/race handlers mirror slayer's (stateless into live state, incremental against
   a stored copy); only slayer was read.
4. [G] Names of 0x46A9A0/0x46AB50/0x46AAB0 (map reset), 0x461590's message flag list, 0x5743C0.
5. [L] `game_engine_change_mode` 2 sets damage flag 0x20 on all player units (meaning not checked).
6. The host-side sending logic (who gets what, reliability) was read in haloce.exe's host code;
   haloceded's tunables are identical and the capture agrees on reliability, but its send functions
   were not traced instruction by instruction.
7. The pong-per-iteration behaviour (53) is read from the handler; whether the host minds is
   irrelevant (it only recomputes the ping).

## 10. Files

`ce-work/client/`: `cdis.py` (capstone helper for both exes), `jtable.py` (dispatcher table),
`rcstrings.py` (RT_STRING dump), `ustr_dump.py` (unicode_string_list dump from a cache map),
`gscripts/Decomp.java`, `gscripts/Rename.java`, `decomp.ps1`, `rename.ps1`, `names_mine.txt`,
`os_funcs.txt`, `out/*.c` (decompiled handlers, senders, host code), `gproj/` (Ghidra project).
