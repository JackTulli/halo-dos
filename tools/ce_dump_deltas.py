"""Static dump of Halo CE 1.10 (haloceded.exe 01.00.10.0621) message delta tables.

Reads the program file only (never runs it). Walks:
  - the message delta definition array (K_MESSAGE_DELTA_PACKETS 0x5573C0, 57 entries)
  - every field_properties_definition reachable from it (headers, bodies, array elements,
    structure members, pointer referents) and its type parameters
  - the global field type list (0x5AB380) and the message delta parameters (0x5AB35C)
Computes the engine's maximum sizes the way the type procs do (they run at start-up;
the file holds NONE there), cross-checks against OpenSauce's XML, and writes
  deltas_report.txt, ce_delta_tables.c, ce_delta_tables.h (+ deltas.json).

usage: python dump_deltas.py [haloceded.exe] [opensauce xml]
"""
import json
import os
import struct
import sys
import xml.etree.ElementTree as ET

from peimage import Image

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_EXE = os.path.join(HERE, "..", "server", "haloceded.exe")
DEFAULT_XML = os.path.join(HERE, "..", "..", "ce-refs", "OpenSauce", "Doc", "Blam", "Halo1",
                           "Halo1_CE.message_delta_definitions.xml")

# ---- addresses (1.10 dedicated server; from OpenSauce HaloCE_110_Dedi*.inl, verified below)
K_MESSAGE_DELTA_PACKETS = 0x5573C0
K_MESSAGE_DELTA_PACKETS_END = 0x5574A4      # loop bound in mdp initialize (0x4D8DC1)
K_GLOBAL_FIELD_TYPE_LIST = 0x5AB380         # field_type_definition[28], 0x18 bytes each
K_MESSAGE_DELTA_PARAMETERS = 0x5AB35C       # locality + item placement parameters
K_ENCODING_CLASS = 0x5AC3E0                 # g_message_delta_encoding_class (0 lan, 1 internet)
K_BITS_TABLE = 0x5574A8                     # byte table: bits needed for n (min 1), n < 2048
K_PROTOCOL_CHANGEOVER = 0x620A10            # bss: parameters protocol changeover enabled

FIELD_TYPES = [
    "integer", "real", "boolean", "ascii_character", "wide_character", "ascii_string",
    "wide_string", "arbitrary_data", "array", "structure", "pointer", "enumeration",
    "bounded_index", "translated_index", "point", "vector", "angle", "flags", "time",
    "grenade_counts", "fixed_width", "fixed_width_normal_vector",
    "locality_reference_position", "digital_throttle", "fixed_width_weapon_index",
    "fixed_width_grenade_index", "smart_vector", "item_placement_position",
]
INTEGER_WIDTH_BITS = [8, 16, 32, 1, 3, 5, 6]
INTEGER_WIDTH_BYTES = [1, 2, 4, 1, 1, 1, 1]

# Procs as found in the field type list and the field properties (checked in walk()).
EXPECTED_PROCS = {
    0: (0x4D54D0, 0x4D5600), 1: (0x4D56F0, 0x4D5730), 2: (0x4D5750, 0x4D5790),
    5: (0x4D5840, 0x4D5940), 6: (0x4D59D0, 0x4D5AA0), 8: (0x4D5BC0, 0x4D5DC0),
    9: (0x4D6070, 0x4D6270), 10: (0x4D64C0, 0x4D64F0), 13: (0x4D6670, 0x4D66A0),
    14: (0x4D6860, 0x4D6AF0), 15: (0x4D6CF0, 0x4D6D00), 17: (0x4D6D60, 0x4D6E60),
    18: (0x4D6EF0, 0x4D5730), 19: (0x4D70B0, 0x4D7100), 20: (0x4D6FA0, 0x4D7050),
    21: (0x4D7350, 0x4D7510), 22: (0x4D7680, 0x4D7970), 23: (0x4D7C00, 0x4D7C70),
    24: (0x4D7CC0, 0x4D7D20), 25: (0x4D7D70, 0x4D7DD0), 26: (0x4D8120, 0x4D8330),
}


SERVER_ADDRESSES = {"packets": K_MESSAGE_DELTA_PACKETS, "count": 57, "parameters": K_MESSAGE_DELTA_PARAMETERS,
                    "class": K_ENCODING_CLASS, "bits_table": K_BITS_TABLE, "type_list": K_GLOBAL_FIELD_TYPE_LIST}
# haloce.exe 1.10 (client), OpenSauce HaloCE_110_Runtime.Networking.inl
CLIENT_ADDRESSES = {"packets": 0x5FBA40, "count": 57, "parameters": 0x635984, "class": 0x636A08,
                    "bits_table": None, "type_list": 0x6359A8}


class Dump:
    def __init__(self, exe, addrs=SERVER_ADDRESSES):
        self.im = Image(exe)
        self.addrs = addrs
        if addrs["bits_table"]:
            self.bits_table = list(self.im.read(addrs["bits_table"], 2048))
        else:
            self.bits_table = [max(1, n.bit_length()) for n in range(2048)]
        self.props = {}          # address -> dict
        self.order = []          # addresses in dependency order (children first)
        self.problems = []
        p = addrs["parameters"]
        im = self.im
        self.params = {
            "locality_bits_full": im.s32(p), "locality_bits_delta": im.s32(p + 4),
            "locality_delta_cutoff": im.f32(p + 8), "locality_min_move": im.f32(p + 12),
            "locality_bits_lan": im.s32(p + 16),
            "item_bits_x": im.s32(p + 20), "item_bits_y": im.s32(p + 24), "item_bits_z": im.s32(p + 28),
        }
        self.encoding_class_static = im.u32(addrs["class"])

    def bits_for(self, n):
        if n < 0 or n >= 2048:
            raise ValueError(n)
        return self.bits_table[n]

    # ---------------------------------------------------------------- fields
    def walk(self, addr):
        if addr in self.props:
            return self.props[addr]
        im = self.im
        t = im.u32(addr)
        name = im.cstr(addr + 4, 76)
        enc, dec, par = im.u32(addr + 80), im.u32(addr + 84), im.u32(addr + 88)
        d = {"addr": addr, "name": name, "type": t, "type_name": FIELD_TYPES[t],
             "encode": enc, "decode": dec, "params_addr": par,
             "params_in_bss": bool(par) and not im.is_initialised(par),
             "file_max_size": im.s32(addr + 92), "file_header_bits": im.s32(addr + 96)}
        self.props[addr] = d
        exp = EXPECTED_PROCS.get(t)
        if exp and (enc, dec) != exp:
            self.problems.append("%s: procs %X/%X differ from type %d's %X/%X" % (name, enc, dec, t, exp[0], exp[1]))
        if t == 0:
            w = im.u32(par)
            d["width"] = w
            d["bits"] = INTEGER_WIDTH_BITS[w]
            d["size"] = INTEGER_WIDTH_BYTES[w]
        elif t in (1, 16, 18):
            d["size"] = 4
        elif t == 2:
            d["size"] = 1
        elif t == 3:
            d["size"] = 1
        elif t == 4:
            d["size"] = 2
        elif t in (5, 6):
            n = im.s32(par)
            d["max_length"] = n
            d["length_bits"] = self.bits_for(n)
            d["size"] = (n + 1) * (1 if t == 5 else 2)
        elif t == 7:
            d["max_size"] = im.s32(par)
            d["size"] = d["max_size"]
        elif t == 8:
            d["count"] = im.s32(par)
            d["stride"] = im.s32(par + 4)
            el = im.u32(par + 8)
            d["element"] = el
            self.walk(el)
            d["size"] = d["count"] * d["stride"]
        elif t == 9:
            n = im.s32(par)
            members = []
            size = 0
            for i in range(n):
                q = par + 4 + 16 * i
                mp, off, boff = im.u32(q), im.u32(q + 4), im.u32(q + 8)
                m = self.walk(mp)
                members.append({"props": mp, "offset": off, "baseline_offset": boff})
                size = max(size, off + m["size"])
                if off != boff:
                    self.problems.append("%s member %d: offset %X != baseline offset %X" % (name, i, off, boff))
            d["members"] = members
            d["size"] = size
        elif t == 10:
            ref = im.u32(par)
            d["referent"] = ref
            self.walk(ref)
            d["size"] = 4
        elif t == 11:
            d["enum_width"] = im.u32(par)
            d["size"] = [1, 2, 4][d["enum_width"]]
        elif t == 12:
            d["minimum"], d["maximum"] = im.s32(par), im.s32(par + 4)
            d["size"] = 2
        elif t == 13:
            d["maximum_active_at_once"] = im.s32(par)
            d["number_of_slots"] = im.s32(par + 4)
            d["bits"] = self.bits_for(d["maximum_active_at_once"])
            d["size"] = 4
        elif t in (14, 15):
            d["components"] = im.s32(par)
            d["size"] = 4 * d["components"]
        elif t == 17:
            n = im.s32(par)
            valid = list(im.read(par + 4, n))
            mask = 0
            for i, v in enumerate(valid):
                if v == 1:
                    mask |= 1 << i
                elif v != 0:
                    self.problems.append("%s: flag %d valid byte %d" % (name, i, v))
            d["count"] = n
            d["valid_mask"] = mask
            d["size"] = 4
        elif t == 19:
            d["size"] = 2
        elif t == 20:
            d["bits"] = im.s32(par)
            d["range"] = im.u32(par + 4)
            d["size"] = 4
        elif t == 21:
            d["theta_bits_internet"] = im.s32(par)
            d["phi_bits_internet"] = im.s32(par + 4)
            d["theta_bits_lan"] = im.s32(par + 8)
            d["phi_bits_lan"] = im.s32(par + 12)
            d["size"] = 12
        elif t == 22:
            d["size"] = 12
        elif t == 23:
            d["size"] = 12
        elif t in (24, 25):
            d["size"] = 2
        elif t == 26:
            d["minimum"] = im.f32(par)
            d["maximum"] = im.f32(par + 4)
            d["bits_internet"] = im.s32(par + 8)
            d["range_internet_file"] = im.u32(par + 12)
            d["bits_lan"] = im.s32(par + 16)
            d["range_lan_file"] = im.u32(par + 20)
            # initialize (0x4D8000) fills 0 ranges with (1 << bits) - 1
            d["range_internet"] = d["range_internet_file"] or ((1 << d["bits_internet"]) - 1)
            d["range_lan"] = d["range_lan_file"] or ((1 << d["bits_lan"]) - 1)
            n = im.s32(par + 24)
            d["special_count"] = n
            d["special_vectors"] = [[im.f32(par + 28 + 12 * i + 4 * k) for k in range(3)] for i in range(n)]
            d["size"] = 12
        elif t == 27:
            d["size"] = 12
        self.order.append(addr)
        return d

    # engine maximum_size (the type's maximum size calculator), and header_bit_size
    def max_bits(self, d):
        t = d["type"]
        P = self.params
        if t == 0:
            return d["bits"], 0
        if t in (1, 16, 18):
            return 32, 0
        if t == 2:
            return 1, 0
        if t == 3:
            return 8, 0
        if t == 4:
            return 16, 0
        if t == 5:
            return d["length_bits"] + 8 * d["max_length"], d["length_bits"]
        if t == 6:
            return d["length_bits"] + 16 * d["max_length"], d["length_bits"]
        if t == 7:
            return 8 * d["max_size"], 0
        if t == 8:
            e = self.props[d["element"]]
            return d["count"] + d["count"] * self.max_bits(e)[0], d["count"]
        if t == 9:
            s = sum(self.max_bits(self.props[m["props"]])[0] for m in d["members"])
            return len(d["members"]) + s, len(d["members"])
        if t == 10:
            return self.max_bits(self.props[d["referent"]])[0], 0
        if t == 11:
            return [1, 2, 4][d["enum_width"]], 0   # 0x4D6520 returns 1/2/4 (sic)
        if t == 12:
            return self.bits_for(d["maximum"] - d["minimum"]), 0
        if t == 13:
            return d["bits"], 0
        if t in (14, 15):
            return 33 * d["components"], d["components"]
        if t == 17:
            return d["count"], 0
        if t == 19:
            return 6, 0
        if t == 20:
            return d["bits"], 0
        if t == 21:
            return max(d["theta_bits_internet"] + d["phi_bits_internet"],
                       d["theta_bits_lan"] + d["phi_bits_lan"]), 0
        if t == 22:
            return max(3 * P["locality_bits_full"] + 1, 3 * P["locality_bits_lan"] + 1), 0
        if t == 23:
            return 4, 0
        if t == 24:
            return 3, 0
        if t == 25:
            return 2, 0
        if t == 26:
            n = d["special_count"]
            return max(3 * d["bits_internet"] + n + 1, 3 * d["bits_lan"] + n + 1), 0
        if t == 27:
            return P["item_bits_x"] + P["item_bits_y"] + P["item_bits_z"], 0
        raise ValueError(t)

    # ---------------------------------------------------------------- messages
    def field_set(self, addr):
        im = self.im
        n = im.s32(addr)
        fields = []
        for i in range(n):
            q = addr + 8 + 16 * i
            p, off, boff = im.u32(q), im.u32(q + 4), im.u32(q + 8)
            self.walk(p)
            fields.append({"props": p, "offset": off, "baseline_offset": boff})
            if off != boff:
                self.problems.append("field set %X field %d: offset %X baseline offset %X" % (addr, i, off, boff))
        q = addr + 8 + 16 * n
        if im.u32(q) or im.u32(q + 4) or im.u32(q + 8):
            self.problems.append("field set %X not NULL-terminated after %d fields" % (addr, n))
        return {"addr": addr, "count": n, "file_max": im.s32(addr + 4), "fields": fields}

    def messages(self):
        im = self.im
        out = []
        count = self.addrs["count"]
        for i in range(count):
            a = im.u32(self.addrs["packets"] + 4 * i)
            d = {"index": i, "addr": a, "type": im.s32(a), "max_iterations": im.s32(a + 0x14),
                 "file_sizes": [im.s32(a + 4 * k) for k in range(1, 5)]}
            if d["type"] != i:
                self.problems.append("definition %d has type %d" % (i, d["type"]))
            d["header"] = self.field_set(im.u32(a + 0x1C))
            d["body"] = self.field_set(a + 0x20)
            out.append(d)
        self.msgs = out
        return out

    def limit_sizes(self):
        """The engine's structures sometimes give a field less room than its type
        would write: strings whose maximum length fills their array (no NUL),
        digital_throttle (a 2D vector, 8 bytes, though the decoder writes 12). Size
        each field type by the smallest room it has (next field's offset)."""
        def limit(addr, room):
            d = self.props[addr]
            if room < d["size"]:
                if d.get("room_limited", d["size"]) > room:
                    d["room_limited"] = room
                self.problems.append("%s: %d bytes of room for a %d-byte value" % (d["name"], room, d["size"]))
                d["size"] = room
                if d["type"] == 9 and d["members"]:
                    last = max(d["members"], key=lambda m: m["offset"])
                    limit(last["props"], room - last["offset"])

        def sets():
            for m in self.msgs:
                yield m["header"]["fields"]
                yield m["body"]["fields"]
            for d in list(self.props.values()):
                if "members" in d:
                    yield d["members"]

        for fields in sets():
            fs = sorted(fields, key=lambda f: f["offset"])
            for a, b in zip(fs, fs[1:]):
                limit(a["props"], b["offset"] - a["offset"])

    def message_sizes(self, m, changeover=False):
        """0x4D9230: the sizes the engine computes at start-up."""
        hb = sum(self.max_bits(self.props[f["props"]])[0] for f in m["header"]["fields"])
        bb = sum(self.max_bits(self.props[f["props"]])[0] for f in m["body"]["fields"])
        n = m["body"]["count"]
        it = m["max_iterations"]
        iteration_bits = self.bits_for(it - 1) if it > 1 else 0
        independent = (10 if changeover else 7) + iteration_bits
        dependent = n + hb
        iteration = n + hb + bb
        total = it * iteration + independent
        return {"header_max": hb, "body_max": bb, "iteration_count_bits": iteration_bits,
                "iteration_independent_header": independent, "message_dependent_header": dependent,
                "iteration_size": iteration, "total": total}

    def struct_size(self, fs):
        size = 0
        for f in fs["fields"]:
            size = max(size, f["offset"] + self.props[f["props"]]["size"])
        return size


# ------------------------------------------------------------------ XML cross-check
def load_xml(path):
    if not os.path.exists(path):
        return None
    root = ET.parse(path).getroot()
    types = {}
    for t in root.find("fieldTypes"):
        types[t.get("name")] = t.get("type")
    msgs = []
    for m in root.find("messages"):
        hdr = m.find("header")
        hfields = []
        if hdr is not None and hdr.find("fields") is not None:
            for f in hdr.find("fields"):
                hfields.append((f.get("struct"), int(f.get("offset"), 16), f.get("field"), f.get("type")))
        bfields = []
        for f in m.find("fields"):
            bfields.append((f.get("struct"), int(f.get("offset"), 16), f.get("field"), f.get("type")))
        msgs.append({"type": int(m.get("type"), 16), "name": m.get("name"), "header": hfields, "body": bfields,
                     "declared_body_count": int(m.find("fields").get("count"))})
    return {"types": types, "messages": msgs}


XML_TYPE_TO_ENUM = {
    "Integer": 0, "Real": 1, "Boolean": 2, "AsciiCharacter": 3, "WideCharacter": 4, "AsciiString": 5,
    "WideString": 6, "ArbitraryData": 7, "Array": 8, "Structure": 9, "Pointer": 10, "Enumeration": 11,
    "BoundedIndex": 12, "TranslatedIndex": 13, "Point": 14, "Vector": 15, "Angle": 16, "Flags": 17,
    "Time": 18, "GrenadeCounts": 19, "FixedWidth": 20, "FixedWidthNormalVector": 21,
    "LocalityReferencePosition": 22, "DigitalThrottle": 23, "FixedWidthWeaponIndex": 24,
    "FixedWidthGrenadeIndex": 25, "SmartVector": 26, "ItemPlacementPosition": 27,
}


def cross_check(dump, xml):
    lines = []
    if xml is None:
        return ["OpenSauce XML not found: no cross-check"]
    if len(xml["messages"]) != len(dump.msgs):
        lines.append("message count: exe %d, xml %d" % (len(dump.msgs), len(xml["messages"])))
    for m, x in zip(dump.msgs, xml["messages"]):
        if m["index"] != x["type"]:
            lines.append("message %d: xml type %d" % (m["index"], x["type"]))
        for part in ("header", "body"):
            ef = m[part]["fields"]
            xf = x[part]
            if len(ef) != len(xf):
                lines.append("%02d %s %s: exe %d fields, xml %d" % (m["index"], x["name"], part, len(ef), len(xf)))
            for i, (e, xx) in enumerate(zip(ef, xf)):
                p = dump.props[e["props"]]
                if e["offset"] != xx[1]:
                    lines.append("%02d %s %s field %d (%s): offset exe %X xml %X" % (
                        m["index"], x["name"], part, i, xx[2], e["offset"], xx[1]))
                if p["name"] != xx[3]:
                    lines.append("%02d %s %s field %d (%s): type name exe '%s' xml '%s'" % (
                        m["index"], x["name"], part, i, xx[2], p["name"], xx[3]))
                xt = xml["types"].get(xx[3])
                if xt is not None and XML_TYPE_TO_ENUM.get(xt) != p["type"]:
                    lines.append("%02d %s %s field %d (%s): field type exe %s xml %s" % (
                        m["index"], x["name"], part, i, xx[2], p["type_name"], xt))
        if x["declared_body_count"] != len(x["body"]):
            lines.append("%02d %s: xml declares %d body fields, lists %d" % (
                m["index"], x["name"], x["declared_body_count"], len(x["body"])))
    # field type names
    exe_names = {p["name"]: p for p in dump.props.values()}
    for n, t in xml["types"].items():
        p = exe_names.get(n)
        if p is None:
            lines.append("xml field type '%s' (%s) not reachable from the exe's messages" % (n, t))
        elif XML_TYPE_TO_ENUM.get(t) != p["type"]:
            lines.append("field type '%s': exe %s, xml %s" % (n, p["type_name"], t))
    top = set()
    for m in dump.msgs:
        for part in ("header", "body"):
            for f in m[part]["fields"]:
                top.add(dump.props[f["props"]]["name"])
    for n in sorted(top):
        if n not in xml["types"]:
            lines.append("exe field type '%s' missing from xml <fieldTypes>" % n)
    if not lines:
        lines.append("no mismatches")
    return lines


# ------------------------------------------------------------------ call sites (mode per message)
def call_site_modes():
    try:
        import callsites
        import x86dis
    except Exception as e:  # capstone missing
        return None, str(e)
    enc = {}
    for c in x86dis.calls_to(callsites.ENCODE):
        r = callsites.analyse(c)
        if not r:
            continue
        mode, dtype = r[0], r[1]
        try:
            t = int(dtype, 0)
        except ValueError:
            t = dtype
        enc.setdefault(t, set()).add(mode)
    return enc, None


# Member names of the nested structures (the exe has no member names). network_map
# and network_player: OpenSauce's s_network_game_map / s_network_game_player.
# game_variant / universal_variant: the Xbox source's struct universal_variant
# (halo-ce-universal source/game/game_engine.h) for the shared part; the PC-only
# tail (+0x2C..) named after Halo PC's game type options -- a guess (see notes).
NESTED_MEMBER_NAMES = {
    "network_map": ["version", "name"],
    "network_player": ["name", "primary_color_index", "icon_index", "machine_index",
                       "controller_index", "team_index", "player_list_index"],
    "game_variant": ["human_readable_description", "game_engine_index", "universal_variant",
                     "game_engine_variant", "flags"],
    "universal_variant": ["teams", "unknown_01", "unknown_02", "unknown_03", "flags", "goal_radar",
                          "odd_man_out", "unknown_0d", "unknown_0e", "unknown_0f",
                          "respawn_time_growth", "respawn_time", "suicide_penalty", "lives", "health",
                          "score_to_win", "weapon_set", "red_vehicle_set_guess",
                          "blue_vehicle_set_guess", "vehicle_respawn_time_guess",
                          "friendly_fire_guess", "friendly_fire_penalty_guess",
                          "auto_team_balance_guess", "time_limit_guess"],
}


# ------------------------------------------------------------------ output
def c_ident(s):
    return "".join(ch if ch.isalnum() else "_" for ch in s)


def write_report(dump, xml, xml_lines, modes, path):
    P = dump.params
    out = []
    w = out.append
    w("Halo CE 1.10 message deltas, from haloceded.exe 01.00.10.0621 (static)")
    w("=" * 78)
    w("")
    w("Global parameters (0x5AB35C):")
    for k, v in P.items():
        w("  %-26s %s" % (k, v))
    w("  g_message_delta_encoding_class (0x5AC3E0) in the file: %d (0 = lan, 1 = internet; set at run time)"
      % dump.encoding_class_static)
    w("")
    w("Stream (one decrypted GT2 app message; client dispatcher 0x4C59E0, checked on capture1.bin):")
    w("  11 bits total size in bytes (incl. these bits); then items while >= 8 bits are left:")
    w("  1 bit kind (1 = message delta, 0 = old network-game packet: 16-bit header, size in its top")
    w("  12 bits, size-2 bytes); the last < 8 bits are left-overs (not always zero).")
    w("Message header (iteration independent, 0x4D97A0 / 0x4D9910): 1 bit mode (0 stateless,")
    w("  1 incremental), 6 bits definition type,")
    w("  [3 bits if the parameters protocol changeover is on: 1 bit flag + 2 bits version; off by default,")
    w("  absent in the capture],")
    w("  then (iterations - 1) in bits(max_iterations - 1) bits when max_iterations > 1.")
    w("Per iteration: [incremental: one presence bit per body field] [header fields, no baseline]")
    w("  [body fields: all (stateless) or the present ones, against the baseline when incremental].")
    w("No per-message size or terminator.")
    w("")
    w("Encoding class: the server's sv_public (byte 0x59A426, printed as 'sv_public_value'):")
    w("  1 = internet, 0 = LAN; sent to the client as the byte at offset 8 of old packet 4")
    w("  (server_new_client_challenge); the client sets the class = (byte == 1) (haloce code at the")
    w("  server's 0x4C6460 twin). The local test server runs sv_public 0: capture1.bin is LAN class.")
    w("")
    w("Field types (global field type list 0x5AB380):")
    im = dump.im
    for i in range(28):
        a = K_GLOBAL_FIELD_TYPE_LIST + 24 * i
        w("  %2d %-28s requires_parameters=%d max_size=%06X initialize=%06X dispose=%06X" % (
            im.u32(a), FIELD_TYPES[i], im.u8(a + 4), im.u32(a + 8), im.u32(a + 12), im.u32(a + 16)))
    w("")
    w("Field properties (every one reachable from the messages):")
    for a in dump.order:
        d = dump.props[a]
        mb, hb = dump.max_bits(d)
        extra = []
        for k in ("width", "bits", "max_length", "length_bits", "count", "stride", "components", "range",
                  "maximum_active_at_once", "number_of_slots", "theta_bits_internet", "phi_bits_internet",
                  "theta_bits_lan", "phi_bits_lan", "minimum", "maximum", "bits_internet", "range_internet",
                  "bits_lan", "range_lan", "special_count"):
            if k in d:
                extra.append("%s=%s" % (k, d[k]))
        if "valid_mask" in d:
            extra.append("valid_mask=0x%X" % d["valid_mask"])
        if "special_vectors" in d:
            extra.append("special=%s" % d["special_vectors"])
        if "element" in d:
            extra.append("element=%s" % dump.props[d["element"]]["name"])
        if "referent" in d:
            extra.append("referent=%s" % dump.props[d["referent"]]["name"])
        w("  %06X %-42s %-27s enc=%06X dec=%06X params=%06X%s size=%dB max=%d bits hdr=%d" % (
            a, d["name"], d["type_name"], d["encode"], d["decode"], d["params_addr"],
            " (bss: zero)" if d["params_in_bss"] else "", d["size"], mb, hb))
        if extra:
            w("         " + ", ".join(extra))
        if "members" in d:
            for i, m in enumerate(d["members"]):
                mp = dump.props[m["props"]]
                w("         member %2d +%04X %-40s %s" % (i, m["offset"], mp["name"], mp["type_name"]))
    w("")
    w("Messages (K_MESSAGE_DELTA_PACKETS 0x5573C0):")
    xmsgs = xml["messages"] if xml else [None] * len(dump.msgs)
    for m, x in zip(dump.msgs, xmsgs):
        s = dump.message_sizes(m)
        name = x["name"] if x else "?"
        mode = modes.get(m["index"]) if modes else None
        w("")
        w("%02d (0x%02X) %s   def at %06X, max_iterations %d" % (m["index"], m["index"], name, m["addr"], m["max_iterations"]))
        w("   sizes (bits): header fields max %d, body max %d, iteration %d, independent header %d, total %d"
          % (s["header_max"], s["body_max"], s["iteration_size"], s["iteration_independent_header"], s["total"]))
        w("   header struct %d bytes, body struct %d bytes; header field set %06X; sent as mode %s (encode call sites)"
          % (dump.struct_size(m["header"]), dump.struct_size(m["body"]), m["header"]["addr"],
             "/".join(sorted(mode)) if mode else "?"))
        for part in ("header", "body"):
            xf = x[part] if x else []
            for i, f in enumerate(m[part]["fields"]):
                p = dump.props[f["props"]]
                member = xf[i][2] if i < len(xf) else "?"
                struct_name = xf[i][0] if i < len(xf) else ""
                w("   %-6s %2d +%04X %-34s %-40s %s" % (part, i, f["offset"], member, p["name"], p["type_name"]))
    w("")
    w("OpenSauce XML cross-check:")
    for l in xml_lines:
        w("  " + l)
    w("")
    w("Other notes from the walk:")
    for l in dump.problems or ["none"]:
        w("  " + l)
    open(path, "w", newline="\n").write("\n".join(out) + "\n")


def write_c(dump, xml, hpath, cpath):
    xmsgs = xml["messages"] if xml else None
    names = {}
    for a in dump.order:
        names[a] = "ce_fp_" + c_ident(dump.props[a]["name"])
    h = []
    h.append("/* Generated by dump_deltas.py from haloceded.exe 01.00.10.0621 -- do not edit. */")
    h.append("#ifndef CE_DELTA_TABLES_H")
    h.append("#define CE_DELTA_TABLES_H")
    h.append("")
    h.append('#include "ce_delta.h"')
    h.append("")
    h.append("#define CE_MESSAGE_COUNT %d" % len(dump.msgs))
    h.append("#define CE_DELTA_MAX_HEADER_SIZE %d" % max(max(dump.struct_size(m["header"]) for m in dump.msgs), 4))
    h.append("#define CE_DELTA_MAX_BODY_SIZE %d" % max(dump.struct_size(m["body"]) for m in dump.msgs))
    h.append("#define CE_DELTA_MAX_BODY_FIELDS %d" % max(m["body"]["count"] for m in dump.msgs))
    h.append("")
    h.append("enum ce_message_type {")
    for i, m in enumerate(dump.msgs):
        nm = xmsgs[i]["name"] if xmsgs else "message_%02d" % i
        h.append("    CE_MSG_%s = %d," % (c_ident(nm).upper(), i))
    h.append("};")
    h.append("")
    h.append("extern const ce_message_def ce_message_defs[CE_MESSAGE_COUNT];")
    h.append("")
    for a in dump.order:
        h.append("extern const ce_field_props %s;" % names[a])
    h.append("")
    h.append("#endif")
    open(hpath, "w", newline="\n").write("\n".join(h) + "\n")

    c = []
    c.append("/* Generated by dump_deltas.py from haloceded.exe 01.00.10.0621 -- do not edit.")
    c.append("   Field layouts (offsets) are the engine's; see ce_delta_notes.md. */")
    c.append('#include "ce_delta_tables.h"')
    c.append("")
    c.append("#define F(x) ((float)(x))")
    c.append("")
    # forward declarations for members/elements (already in order: children first)
    for a in dump.order:
        d = dump.props[a]
        nm = names[a]
        t = d["type"]
        mb, hb = dump.max_bits(d)
        if "members" in d:
            mnames = NESTED_MEMBER_NAMES.get(d["name"], [])
            c.append("static const ce_field_ref %s_members[%d] = {" % (nm, len(d["members"])))
            for i, m in enumerate(d["members"]):
                mn = mnames[i] if i < len(mnames) else "%s_%02x" % (dump.props[m["props"]]["name"], m["offset"])
                c.append("    { &%s, 0x%X, 0x%X, \"%s\" }," % (names[m["props"]], m["offset"], m["baseline_offset"], mn))
            c.append("};")
        if "special_vectors" in d and d["special_count"]:
            c.append("static const float %s_special[%d][3] = {" % (nm, d["special_count"]))
            for v in d["special_vectors"]:
                c.append("    { F(%r), F(%r), F(%r) }," % tuple(v))
            c.append("};")
        init = {
            "name": '"%s"' % d["name"], "type": "CE_FT_%s" % d["type_name"].upper(), "size": d["size"],
            "max_bits": mb, "header_bits": hb,
        }
        if t == 0:
            init["bits"] = d["bits"]
        elif t in (5, 6):
            init["count"] = d["max_length"]
            init["bits"] = d["length_bits"]
        elif t == 7:
            init["count"] = d["max_size"]
        elif t == 8:
            init["count"] = d["count"]
            init["stride"] = d["stride"]
            init["element"] = "&" + names[d["element"]]
        elif t == 9:
            init["count"] = len(d["members"])
            init["members"] = nm + "_members"
        elif t == 10:
            init["element"] = "&" + names[d["referent"]]
        elif t == 11:
            init["bits"] = [1, 2, 4][d["enum_width"]]
        elif t == 12:
            init["bits"] = dump.bits_for(d["maximum"] - d["minimum"])
            init["range"] = "0x%XUL" % (d["minimum"] & 0xFFFFFFFF)
        elif t == 13:
            init["bits"] = d["bits"]
            init["count"] = d["maximum_active_at_once"]
        elif t in (14, 15):
            init["count"] = d["components"]
        elif t == 17:
            init["count"] = d["count"]
            init["valid_mask"] = "0x%XUL" % d["valid_mask"]
        elif t == 20:
            init["bits"] = d["bits"]
            init["range"] = "0x%XUL" % d["range"]
        elif t == 21:
            init["bits_internet"] = "{ %d, %d }" % (d["theta_bits_internet"], d["phi_bits_internet"])
            init["bits_lan"] = "{ %d, %d }" % (d["theta_bits_lan"], d["phi_bits_lan"])
        elif t == 26:
            init["bits_internet"] = "{ %d, %d }" % (d["bits_internet"], d["bits_internet"])
            init["bits_lan"] = "{ %d, %d }" % (d["bits_lan"], d["bits_lan"])
            init["range"] = "0x%XUL" % d["range_internet"]
            init["range_lan"] = "0x%XUL" % d["range_lan"]
            init["minimum"] = "F(%r)" % d["minimum"]
            init["maximum"] = "F(%r)" % d["maximum"]
            init["count"] = d["special_count"]
            if d["special_count"]:
                init["special"] = nm + "_special"
        c.append("const ce_field_props %s = {" % nm)
        for k, v in init.items():
            c.append("    .%s = %s," % (k, v))
        c.append("};")
        c.append("")
    # field sets
    for m in dump.msgs:
        for part in ("header", "body"):
            fs = m[part]
            if not fs["count"]:
                continue
            c.append("static const ce_field_ref ce_msg%02d_%s[%d] = {" % (m["index"], part, fs["count"]))
            xf = xmsgs[m["index"]][part] if xmsgs else []
            for i, f in enumerate(fs["fields"]):
                mn = xf[i][2] if i < len(xf) else "%s_%02x" % (dump.props[f["props"]]["name"], f["offset"])
                c.append("    { &%s, 0x%X, 0x%X, \"%s\" }," % (names[f["props"]], f["offset"], f["baseline_offset"], mn))
            c.append("};")
    c.append("")
    c.append("const ce_message_def ce_message_defs[CE_MESSAGE_COUNT] = {")
    for i, m in enumerate(dump.msgs):
        nm = xmsgs[i]["name"] if xmsgs else "message_%02d" % i
        s = dump.message_sizes(m)
        hfs = "ce_msg%02d_header" % i if m["header"]["count"] else "0"
        bfs = "ce_msg%02d_body" % i if m["body"]["count"] else "0"
        c.append("    { %d, \"%s\", %d, %d, %s, %d, %s, %d, %d, %d, %d }," % (
            i, nm, m["max_iterations"], m["header"]["count"], hfs, m["body"]["count"], bfs,
            dump.struct_size(m["header"]), dump.struct_size(m["body"]),
            s["iteration_size"], s["total"]))
    c.append("};")
    open(cpath, "w", newline="\n").write("\n".join(c) + "\n")


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_EXE
    xmlp = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_XML
    dump = Dump(exe)
    dump.messages()
    dump.limit_sizes()
    xml = load_xml(xmlp)
    lines = cross_check(dump, xml)
    modes, err = call_site_modes()
    if err:
        print("call-site analysis skipped:", err)
    write_report(dump, xml, lines, modes, os.path.join(HERE, "deltas_report.txt"))
    write_c(dump, xml, os.path.join(HERE, "ce_delta_tables.h"), os.path.join(HERE, "ce_delta_tables.c"))
    js = {"params": dump.params, "props": {"%X" % a: dump.props[a] for a in dump.order},
          "messages": dump.msgs}
    json.dump(js, open(os.path.join(HERE, "deltas.json"), "w"), indent=1, default=str)
    names = {}
    if xml:
        for x in xml["messages"]:
            names["msg.%d" % x["type"]] = x["name"]
            for part in ("header", "body"):
                for f in x[part]:
                    names["%d.%s.%X" % (x["type"], part, f[1])] = f[2]
    json.dump(names, open(os.path.join(HERE, "deltas_names.json"), "w"), indent=1)
    print("messages %d, field properties %d, xml check: %d lines" % (len(dump.msgs), len(dump.props), len(lines)))
    for l in lines[:40]:
        print("  ", l)
    for l in dump.problems:
        print("  note:", l)


if __name__ == "__main__":
    main()
