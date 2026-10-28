# Halo Custom Edition 1.10 protocol: what is known (verified against a local haloceded 01.00.10.0621)

Code: halo-dos/port/dos/retail/ce_crypto.c, ce_transport.c, ce_packets.c, tools/ce_probe.c.

## Transport (GameSpy GT2 over UDP, server port 2302)
- Reliable GT2 messages: `FE FE <type> <serial BE16> <expected serial BE16> <body>`; types 0 app data, 1 client
  challenge, 2 server challenge, 3 client response, 4 accept, 5 reject, 6 close, 7 keep-alive. Every reliable
  message (control ones too) takes a serial number. Unreliable: 100 ack `<expected BE16>`, 101 nack `<first><last>`,
  102 ping, 103 pong, 104 closed. Unreliable app data: no header (or `FE FE` first when its own first bytes are FE FE).
- Handshake: client 1 with a 32-char GT2 challenge -> server 2 (response 32 + challenge 32) -> client 3 (response 32 +
  Halo public key 16 + version LE32 = **616999** for 01.00.10.0621; 616000 is refused with reject reason 4 or 5:
  haloceded 004CB1A3 `cmp ecx, 616999`) -> server 4 accept, carrying the server's 16-byte public key.
- Keys: Auriemma's halo_pck_algo (DH, base 3, modulus 0x10001). Both directions use the same shared key.
- Data: TEA over (data + CRC32 LE of data, not inverted); reliable data keeps its 7-byte GT2 header in the clear.

## Halo's stream (the decrypted data of each GT2 app message)
- Bits LSB-first. 11 bits: the stream's total size in bytes (incl. these 12 bits); 1 bit flag: **0 = old network-game
  packets, 1 = message deltas**; then messages back to back, from bit 12.
- Old packets: a 16-bit header (LE uint16: flags 2 bits, type 2 bits = 3 'packet', size 12 bits = bytes incl. the
  header), version byte (1), fields (Bungie data_packet encoding: big-endian numbers, strings to their NUL), packet
  type byte LAST. The 38 PC packet definitions: ce-work/packets.txt (dump_packets.py; group at 005AA358).

## Joining
1. server -> packet 4 `server_new_client_challenge`: challenge (8 bytes, a 7-letter string), byte (0), 2 longs (0),
   map name string.
2. client -> packet 14 `client_join_game_request` (148 bytes): token 16 bytes = **"message in a bot"** (constant,
   haloceded 004CB0B0); password 9 UTF-16 shorts (BE); key response 73 bytes (72 chars + NUL: the server's GameSpy
   CD-key check; with sv_public 0 nothing is checked and the first 32 characters are logged as the 'cdkey hash');
   connection class byte (4 = LAN); player: name 12 UTF-16 shorts, primary color -1, icon -1, machine index (the
   challenge's byte), controller 0, team -1, player list index -1; last long = the map's CRC (CE map header offset
   0x64; bloodgulch 0x7B309554) with the challenge's characters XORed over its 4 bytes cyclically, max(4, strlen)
   steps (haloceded 0048DB50). A wrong last long or token = reject (machine_rejected, reason 0 'unknown').
3. server -> a message-delta stream (flag 1) right away (344 bytes: game settings etc.).
4. client -> packet 26 `client_loaded` (long 0) -> the server sends the world: reliable delta streams (1264, 785,
   269 bytes...) and unreliable delta streams every ~125 ms (105 bytes), and keeps sending ~16 s; it closes the
   connection when the client sends nothing more (client updates expected).

## Captures
ce-work/capture1.bin: every stream the server sent in one join (records: [1 byte reliable][2 bytes size LE][the
decrypted stream bytes as above, size bytes, without the CRC]). Probe log with hex: ce-work/probe5.txt.
