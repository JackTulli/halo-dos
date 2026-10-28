# Halo CE 1.10 message deltas: wire format

Worked out statically from `haloceded.exe` 01.00.10.0621 (image base 0x400000) and
checked against `capture1.bin` (a real join to that server): every one of its 28
streams decodes to its end, in Python (`ce_delta_ref.py`) and in C (`test_ce_delta`).
Addresses below are haloceded's. The client `haloce.exe` 1.10 has identical tables
(`compare_client.py`: names, types, parameters, offsets, iteration counts).

## Files

| file | what |
|---|---|
| `dump_deltas.py` | walks the exe's tables -> `deltas_report.txt`, `ce_delta_tables.c/.h`, `deltas.json`, `deltas_names.json`; XML cross-check |
| `ce_bitstream.c/.h` | LSB-first bit stream (engine's 0x4BBB70 family) |
| `ce_delta.c/.h` | codec: streams, message headers, every field type, encode/decode, accessors, formatter |
| `ce_delta_tables.c/.h` | generated: 56 field types, 57 messages, `CE_MSG_*` ids |
| `test_ce_delta.c` | host tests (round trips, iterations, capture) |
| `ce_delta_ref.py` | Python reference decoder (captures, cross-check) |
| `compare_client.py`, `client_modes.py`, `callsites.py` | client/server table comparison, send modes per call site |
| `electron_check.py` | decodes Electron's Halo PC 1.09 bit strings |
| `capture1_decoded.txt` / `capture1_c.txt` | the capture decoded (Python / C) |

Build (32-bit; hud_chat holds a 32-bit pointer):
`vcclang.bat -std=c99 -O2 -o test_ce_delta.exe test_ce_delta.c ce_delta.c ce_bitstream.c ce_delta_tables.c`
then `test_ce_delta.exe ..\capture1.bin capture1_c.txt`. The codec also compiles cleanly with
`-march=pentium -mno-mmx -mno-sse -mfpmath=387 -std=c99 -pedantic`; it needs only
memcpy/memmove/memset/memcmp/strcmp and floor/sin/cos/atan/acos (sqrt/fabs inline). No allocation.

## Bits

Stream bit p is bit (p & 7) of byte p >> 3. An n-bit value is written low bit first.
All integers on the wire are unsigned bit patterns (an int16 -1 in 16 bits is 0xFFFF).

## Stream (one decrypted GT2 application message)

Client dispatcher 0x4C59E0 / handler 0x4C5E90:

```
11 bits   stream size in bytes, these bits included (max 2047)
repeat while at least 8 bits are left:
  1 bit   kind: 1 = message delta, 0 = old network-game packet
  kind 1: a message delta (below)
  kind 0: an old packet: 16-bit header (bits 0-1 flags, 2-3 type = 3, 4-15 size in bytes
          incl. the header), then size-2 bytes, each read as 8 bits (not byte aligned)
the last (< 8) bits of the last byte are left-overs of the send buffer, not always zero
```

The lead's "flag bit 11" is the first item's kind bit; deltas and old packets can be mixed
in one stream (capture record 1: game_settings_update, then old packet 10 begin_game).

## Message delta

```
1 bit    mode: 0 stateless, 1 incremental
6 bits   type (0..56, the definition index)
[3 bits  only with the parameters-protocol changeover: 1 bit flag, 2 bits version]   (off; not in the capture)
[n bits  iterations - 1, n = bits(max_iterations - 1); only super_remote_players_action_update
         and super_ping_update (16 iterations, 4 bits)]
per iteration:
  [incremental: one presence bit per body field, in definition order]
  header fields (always encoded without baseline)
  body fields: stateless = all, without baseline; incremental = the present ones, against
               the baseline (the caller's choice: the engine passes baselines[i], may be NULL)
```

No per-message length, no terminator: the receiver must know every field's encoding.
`bits(n)` = the engine's byte table at 0x5574A8 = max(1, bit length of n).

Encoder (mdpi_encode 0x4D93E0): an iteration whose body wrote 0 bits is dropped (the next
one overwrites it) unless the caller's last argument forces it; a message with no kept
iteration returns 0 (nothing sent); the iteration count is patched in at the end.
Decoder: 0x4D9910 (header; rejects mode > 1, type >= 57, iterations outside 1..max; with
changeover, a different version unless the flag), 0x4D9B10 (presence + header fields),
0x4D9C70 (body; a present field that decodes to 0 bits fails the message).

Server object updates (types 1-5) are encoded incrementally against the object's baseline
(header `baseline_index`; 0x4D8EF0 always passes a baseline), and the baseline itself is
sent statelessly with `update_baseline` = 1. The layout depends only on whether a baseline
is used, so a receiver without the right values still parses correctly by passing any
baseline (the capture parses with zeros); values of absent fields, and delta-coded internet
positions, need the real baseline.

## Encoding class

`g_message_delta_encoding_class` (0x5AC3E0): 0 LAN, 1 internet. The server sets it from
`sv_public` (byte 0x59A426, the console's `sv_public_value`): public = internet. It is sent
to the client as the byte at offset 8 of old packet 4 `server_new_client_challenge`; the
client sets class = (byte == 1) (0x4C6460). The local test server runs sv_public 0, so
`capture1.bin` is LAN class (it does not parse as internet). It changes: normals, locality
positions, smart vectors.

## Field types

`b` = the field has a baseline (incremental message with a baseline, or a nested element
of one). "Unchanged" = encoder writes nothing (presence bit 0 at the top level).

| type | wire | unchanged when (b) |
|---|---|---|
| integer | width bits (8/16/32/1/3/5/6 by width enum 0..6), low bits of the storage (1, 2 or 4 bytes; 1 for 1/3/5/6) | storage equal |
| real, (angle) | 32-bit IEEE | abs(base - src) <= 0.0001f (NaN: unchanged) |
| time | 32 bits | equal |
| boolean | 1 bit = (byte == 1) | bytes equal |
| ascii_string | length in bits(max) bits, then 8 bits per char, no NUL | strcmp equal |
| wide_string | length in bits(max) bits, then 16 bits per unit | wcscmp equal |
| array | no b: every element, no header. b: count presence bits, then the changed elements against their base element; nothing changed = 0 bits (no header) | all elements unchanged |
| structure | same as array, one presence bit per member | |
| pointer | the referent's encoding (hud_chat's message) | referent unchanged |
| translated_index | bits(max_active) bits of the network index (object_index 9 bits, max 511; player_index 5 bits, max 17); index 0 = NONE (OpenSauce) | dword equal |
| point / vector | no b: n floats. b: n presence bits, then each changed component (abs diff > 0.0001f) as 32 bits | no component changed |
| flags | one bit per valid flag (valid_mask), in bit order; decode keeps the other bits of the dword | no valid bit differs |
| grenade_counts | 6 bits = (g0 << 3) or g1 (signed bytes); decode g0 = v >> 3, g1 = v & 7 | 6-bit codes equal |
| fixed_width | q = floor(v * range + 0.5), unsigned clamp to range (negative v -> range!), bits bits; decode v = q / range | quantized equal |
| fixed_width_normal_vector | theta = acos(z), phi = atan(y/x) (+pi if x < 0; x within 0.0001: +-pi/2 by sign of y) of the normalized vector; q0 = floor(theta * (1/pi)f * (2^t-1) + 0.5) in t bits, q1 = floor((phi + (pi/2)f) * (1/2pi)f * (2^p-1) + 0.5) in p bits; t/p = 8/8 (normal_8bit) or 16/16 (normal_16bit) internet, 20/20 LAN. Decode theta = q0/(2^t-1) * pi_f, phi = q1/(2^p-1) * 2pi_f - pi/2_f, (cos phi sin theta, sin phi sin theta, cos theta), each of sin/cos snapped to 0 within 0.0001 | base quantized (via (v-min)/(max-min), min/max 0..pi_f, -pi/2_f..3pi/2_f) equal |
| locality_reference_position | no b: full. b: 1 bit (1 = full, 0 = delta). Full: 3 x q = floor(rangef * ((v + 5000) * 0.0001f) + 0.5), 25 bits internet / 31 LAN (rangef = (2^bits-1) rounded to float); decode v = q / (2^bits-1) * 10000 - 5000. Delta (internet only, every abs(d) <= 8.0): per component 1 sign bit + 9 bits floor(abs(d)/8 * 511 + 0.5); decode base +- q/511 * 8 | distance from base <= 0.01 |
| digital_throttle | 4 bits: x > eps -> 8, x < -eps -> 4, y > eps -> 2, y < -eps -> 1 (z ignored); decode to (+-1/0, +-1/0, 0) normalized. Stored as 2 floats (the engine's decoder writes 12 bytes) | codes equal |
| fixed_width_weapon_index | 3 bits: (v == -1) << 2, v & 3; decode -1 or v & 3 (int16) | codes equal |
| fixed_width_grenade_index | 2 bits: (v == -1) << 1, v & 1 | codes equal |
| smart_vector | no b: unary prefix: k+1 one bits if the quantized vector equals special vector k (one special: (0,0,0), so 1 bit "1"; a "0" ends the prefix when there are several), else "0" and 3 components. b: 3 components, no prefix. Components q = floor((v - min)/(max - min) * range + 0.5) (unsigned clamp, below min -> max!), 16 bits internet / 31 LAN; translational_velocity -100..100, angular_velocity -8..8; decode min + q/range * (max - min) | all quantized components equal |

Unused by any message (no field properties exist): ascii/wide character, arbitrary data,
enumeration (its size proc returns 1/2/4, sic), bounded index (bits(max - min)), angle,
item placement position (20/20/30 bits); the codec refuses them (`CE_ERR_UNSUPPORTED`).

Floating point: the server's x87 runs at 53-bit precision (MSVC default), so its arithmetic
is double; the codec computes in double, in the engine's order, with the engine's float
constants (0.0001f = 0x38D1B717, pi_f, 1/pi_f, ...). The engine is not idempotent for
LAN/internet locality positions: it encodes with 0.0001f and the range rounded to float but
decodes with 10000 and the exact range, so decode -> encode moves q by ~27 (LAN, about
1.3e-4 world units); 4 of the capture's 68 stateless messages re-encode differently for
that reason only, the other 64 bit for bit.

## Messages

Mode = how the server sends it (call sites of 0x4D93E0; client binary likewise; and the capture).

| id | hex | name | iter | header fields | body fields | mode |
|---|---|---|---|---|---|---|
| 0 | 0x00 | object_deletion | 1 | - | 1 | 0 |
| 1 | 0x01 | projectile_update | 1 | object_index, baseline_index, message_index, update_baseline | 2 | 1 (0 for baseline sends) |
| 2 | 0x02 | equipment_update | 1 | object_index, baseline_index, message_index, update_baseline | 3 | 1 (0) |
| 3 | 0x03 | weapon_update | 1 | object_index, baseline_index, message_index, update_baseline | 5 | 1 (0) |
| 4 | 0x04 | biped_update | 1 | object_index, baseline_index, message_index, update_baseline, force_shield_update | 4 | 1 (0) |
| 5 | 0x05 | vehicle_update | 1 | message_index, baseline_index, object_index, timestamp, update_baseline | 6 | 1 (0) |
| 6 | 0x06 | hud_add_item | 1 | - | 3 | 0 |
| 7 | 0x07 | player_create | 1 | - | 4 | 0 |
| 8 | 0x08 | player_spawn | 1 | - | 12 | 0 |
| 9 | 0x09 | player_exit_vehicle | 1 | - | 2 | 0 |
| 10 | 0x0A | player_set_action_result | 1 | - | 6 | 0 |
| 11 | 0x0B | player_effect_start | 1 | - | 6 | 0 |
| 12 | 0x0C | unit_kill | 1 | - | 12 | 0 |
| 13 | 0x0D | client_game_update | 1 | id | 9 | ? (no constant call site found) |
| 14 | 0x0E | player_handle_powerup | 1 | - | 3 | 0 |
| 15 | 0x0F | hud_chat | 1 | - | 3 | 0 |
| 16 | 0x10 | slayer_update | 1 | - | 2 | 0/1 |
| 17 | 0x11 | ctf_update | 1 | flag_swap_timer | 2 | 0/1 |
| 18 | 0x12 | oddball_update | 1 | - | 3 | 0/1 |
| 19 | 0x13 | king_update | 1 | - | 2 | 0/1 |
| 20 | 0x14 | race_update | 1 | - | 5 | 0/1 |
| 21 | 0x15 | player_score_update | 1 | player_index | 17 | 1 (capture: also 0) |
| 22 | 0x16 | game_engine_change_mode | 1 | - | 1 | 0 |
| 23 | 0x17 | game_engine_map_reset | 1 | - | 1 | 0 |
| 24 | 0x18 | multiplayer_hud_message | 1 | - | 3 | 0 |
| 25 | 0x19 | multiplayer_sound | 1 | - | 1 | 0 |
| 26 | 0x1A | team_change | 1 | - | 2 | 0 |
| 27 | 0x1B | unit_drop_current_weapon | 1 | - | 3 | 0 |
| 28 | 0x1C | vehicle_new | 1 | - | 15 | 0 |
| 29 | 0x1D | biped_new | 1 | - | 17 | 0 |
| 30 | 0x1E | projectile_new | 1 | - | 11 | 0 |
| 31 | 0x1F | equipment_new | 1 | - | 12 | 0 |
| 32 | 0x20 | weapon_new | 1 | - | 16 | 0 |
| 33 | 0x21 | game_settings_update | 1 | - | 11 | 0 |
| 34 | 0x22 | parameters_protocol | 1 | - | 2 | 0 |
| 35 | 0x23 | local_player_update | 1 | - | 3 | 0 |
| 36 | 0x24 | local_player_vehicle_update | 1 | - | 8 | 0 |
| 37 | 0x25 | remote_player_action_update | 1 | player_index, update_id, baseline_id | 7 | 0/1 |
| 38 | 0x26 | super_remote_players_action_update | 16 | player_index, update_id, baseline_id | 7 | 1 |
| 39 | 0x27 | remote_player_position_update | 1 | player_index, update_id, sequence_number | 1 | 0 |
| 40 | 0x28 | remote_player_vehicle_update | 1 | player_index, update_id, sequence_number | 6 | 0 |
| 41 | 0x29 | remote_player_total_update_biped | 1 | action_header.player_index, .update_id, .baseline_id, sequence_number | 8 | 0/1 |
| 42 | 0x2A | remote_player_total_update_vehicle | 1 | action_header.player_index, .update_id, .baseline_id, sequence_number | 13 | 0/1 |
| 43 | 0x2B | weapon_start_reload | 1 | - | 4 | 0 |
| 44 | 0x2C | weapon_ammo_pickup_mid_reload | 1 | - | 3 | 0 |
| 45 | 0x2D | weapon_finish_reload | 1 | - | 4 | 0 |
| 46 | 0x2E | weapon_cancel_reload | 1 | - | 4 | 0 |
| 47 | 0x2F | netgame_equipment_new | 1 | - | 3 | 0 |
| 48 | 0x30 | projectile_detonate | 1 | - | 2 | 0 |
| 49 | 0x31 | item_accelerate | 1 | - | 3 | 0 |
| 50 | 0x32 | damage_dealt | 1 | - | 3 | 0 |
| 51 | 0x33 | projectile_attach | 1 | - | 3 | 0 |
| 52 | 0x34 | client_to_server_pong | 1 | - | 1 | 0 (client binary) |
| 53 | 0x35 | super_ping_update | 16 | - | 2 | 0 |
| 54 | 0x36 | sv_motd | 1 | - | 1 | 0 |
| 55 | 0x37 | rcon_request | 1 | - | 2 | 0 |
| 56 | 0x38 | rcon_response | 1 | - | 1 | 0 |

Field-by-field layouts, sizes and maximum bit counts: `deltas_report.txt`.

## Layout notes for consumers

- Buffers use the engine's offsets. Some fields have less room than their type writes:
  `digital_throttle` is a 2D vector (8 bytes; the engine's decoder writes 12, clobbering the
  next field's first dword, which in stateless messages is then decoded over); strings whose
  maximum fills their array carry no NUL (network game name 64, map name 128, variant
  description 24, player name 12 units). `ce_field_props.size` holds the room; the codec
  never writes past it.
- `ce_delta_read_iteration_body` prepares the body (copy of the baseline, else zero; pointer
  fields kept), as the engine's callers must: absent fields keep the baseline's values.
- hud_chat's `message` is a pointer to a wide string (255 units): set it before decoding.
- Member names: top level from OpenSauce's XML; network_map / network_player from
  OpenSauce headers; universal_variant from the Xbox source's struct for its first part, the
  PC-only tail (+0x2C on) named `*_guess` (capture: 2396736, 2396736, 1800, 1, 0, 0, 36000
  fit red/blue vehicle sets, vehicle respawn ticks, friendly fire, penalty, auto balance,
  time limit ticks).

## Validation

- Tables: all 57 messages / 56 field types; names, offsets and types match OpenSauce's
  XML with no mismatch; client haloce.exe identical.
- Round trips (both classes): every message stateless (40 random cases each; re-encoding the
  decoded values gives the same bits except where the engine's quantizers are lossy) and
  incremental with random baselines and changed subsets; 16-iteration super_ping_update;
  changeover header. 0 failures.
- capture1.bin: 28/28 streams, 128 items (2 old packets, 126 deltas: game_settings_update,
  sv_motd, weapon_new x24, equipment_new x20, vehicle_new x4, biped_new, player_create,
  player_spawn, slayer_update, player_score_update x2, object_deletion x2, weapon_update,
  biped_update, vehicle_update x61 (57 incremental), super_ping_update x5), each stream
  ending with < 8 left-over bits; values sensible (Blood Gulch spawn positions, unit
  normals, "local-test", "Slayer", "probe", ammo counts).
- Electron's Halo PC 1.09 bit strings (`electron_check.txt`): many decode cleanly with the
  CE tables, internet class: weapon_new for the assault rifle (180/60 rounds) and pistol
  (48/12), biped_new at a Blood Gulch spawn, hud_chat "AAAAAAAAAAAAAAAAAAAA",
  super_ping_update (48 and 41 ms, 2 iterations), reload messages, score updates. Others are
  fragments or annotated pieces and do not parse whole; one PC 1.09 game_settings_update did
  not decode (PC 1.09 may differ there; CE's decodes perfectly in the capture).

## Open questions (most important first)

1. client_game_update (13): no constant call site in either binary; its send mode, and which
   client->server messages CE 1.10 clients really send in play (local_player_update 35,
   remote_player_action_update 37 ...), should be confirmed on a capture of a real client.
2. Baseline bookkeeping (which baseline the receiver must use for each incremental message:
   object baselines via current_baseline_index / update_baseline / baseline_index, player
   action baselines via baseline_id) is game logic, not wire format: only partly observed.
3. Client-side float precision: haloce.exe under Direct3D 9 likely runs the x87 at 24 bits,
   so quantized values the client encodes may differ by one step from this double-precision
   codec in boundary cases (the server's encodings are reproduced exactly except as noted).
4. Parameters-protocol changeover (3 extra header bits, `parameters_protocol` message 34)
   is off on this server; when it is on, `ce_delta_config.changeover` must follow it.
5. universal_variant's PC-only member names are guesses.
6. The engine quirks are mirrored (negative fixed-width and below-minimum smart-vector values
   clamp to the maximum, boolean bit = (byte == 1)) except: over-long strings are refused
   (the engine would emit a corrupt length), and the decoder writes only the room a field has.
