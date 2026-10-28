"""The old-style network game packets of a Halo PC / CE program (Bungie's
data packet groups): every data_packet_definition found through its name,
its fields, and the group's packet type order.

    python dump_packets.py haloceded.exe

Layouts (the decompilation's memory/data_packet_groups.h):
    data_packet_field      { short type, count, minimum_version, maximum_version, size }
    data_packet_definition { char *name; long flags; short size, version; data_packet_field *fields; bool initialized }
    data_packet_entry      { short packet_class, flags; data_packet_definition *definition }
    data_packet_group_definition { char *name; short type_count, class_count; long max decoded, max encoded; entry *packets }
"""
import re
import struct
import sys

FIELD_TYPES = ["pad", "bytes", "shorts", "longs", "int64s", "string", "data", "array", "raw", "end"]


class Image:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        d = self.data
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        count = struct.unpack_from("<H", d, pe + 6)[0]
        optional = struct.unpack_from("<H", d, pe + 20)[0]
        self.base = struct.unpack_from("<I", d, pe + 24 + 28)[0]
        self.sections = []
        for i in range(count):
            name, vsize, vaddr, rsize, raw = struct.unpack_from("<8sIIII", d, pe + 24 + optional + 40 * i)
            self.sections.append((name.rstrip(b"\0").decode(), self.base + vaddr, max(vsize, rsize), raw, rsize))

    def offset(self, va):
        for name, v, size, raw, rsize in self.sections:
            if v <= va < v + rsize:
                return raw + va - v
        return None

    def u32(self, va):
        o = self.offset(va)
        return None if o is None else struct.unpack_from("<I", self.data, o)[0]

    def string(self, va):
        o = self.offset(va)
        if o is None:
            return None
        end = self.data.index(b"\0", o)
        return self.data[o:end].decode("latin-1")

    def find_dword(self, value):
        needle = struct.pack("<I", value)
        result = []
        for name, v, size, raw, rsize in self.sections:
            chunk = self.data[raw:raw + rsize]
            start = 0
            while True:
                i = chunk.find(needle, start)
                if i < 0:
                    break
                if i % 4 == 0:
                    result.append(v + i)
                start = i + 1
        return result


def fields_of(image, va):
    fields = []
    while True:
        o = image.offset(va)
        t, count, low, high, size = struct.unpack_from("<5h", image.data, o)
        fields.append((t, count, low, high, size))
        va += 10
        if t == 9 or len(fields) > 64:
            break
    return fields


def main():
    image = Image(sys.argv[1])
    names = {}
    for name, v, size, raw, rsize in image.sections:
        chunk = image.data[raw:raw + rsize]
        for m in re.finditer(rb"message_[a-z_]+_packet\0", chunk):
            names[v + m.start()] = m.group()[:-1].decode()
    definitions = {}
    for va, name in sorted(names.items()):
        for ref in image.find_dword(va):
            # a definition: name pointer, flags, size/version, fields pointer
            o = image.offset(ref)
            if o is None:
                continue
            _, flags, size, version, fields = struct.unpack_from("<IiHhI", image.data, o)
            if image.offset(fields) is None or size > 0x4000:
                continue
            definitions[ref] = (name, flags, size, version, fields)
    # the group's entry array: entries point at definitions
    order = {}
    for ref in definitions:
        for entry in image.find_dword(ref):
            order.setdefault(ref, []).append(entry - 4)
    groups = {}
    for ref, entries in order.items():
        for e in entries:
            groups.setdefault(e, ref)
    # contiguous runs of 8-byte entries
    starts = sorted(groups)
    runs, run = [], []
    for e in starts:
        if run and e != run[-1] + 8:
            runs.append(run)
            run = []
        run.append(e)
    if run:
        runs.append(run)
    best = max(runs, key=len)
    print("packet entry array at %08X, %d entries" % (best[0], len(best)))
    for index, e in enumerate(best):
        o = image.offset(e)
        packet_class, flags = struct.unpack_from("<hh", image.data, o)
        ref = groups[e]
        name, dflags, size, version, fields = definitions[ref]
        print("%2d class %d  %-52s size %4d version %d flags %X" % (index, packet_class, name, size, version, dflags))
        for t, count, low, high, fsize in fields_of(image, fields):
            kind = FIELD_TYPES[t] if 0 <= t < len(FIELD_TYPES) else "?%d" % t
            if kind == "end":
                break
            versions = "" if (low, high) == (0, 0) else "  versions %d..%d" % (low, high)
            print("        %-7s count %4d size %4d%s" % (kind, count, fsize, versions))
    # the group itself
    for g in image.find_dword(best[0]):
        o = image.offset(g - 16)
        name_va, count, classes, decoded, encoded, packets = struct.unpack_from("<IhhiiI", image.data, o)
        print("group %r: %d types, %d classes, decoded %d, encoded %d" % (image.string(name_va), count, classes,
            decoded, encoded))


if __name__ == "__main__":
    main()
