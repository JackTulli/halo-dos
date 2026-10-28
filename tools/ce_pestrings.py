"""Printable strings of a PE file with their virtual addresses, filtered by a
regular expression: python pestrings.py <exe> <regex> [min length]"""
import re
import struct
import sys


def sections(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    count = struct.unpack_from("<H", data, pe + 6)[0]
    optional = struct.unpack_from("<H", data, pe + 20)[0]
    base = struct.unpack_from("<I", data, pe + 24 + 28)[0]
    table = pe + 24 + optional
    result = []
    for i in range(count):
        name, vsize, vaddr, rsize, raw = struct.unpack_from("<8sIIII", data, table + 40 * i)
        result.append((name.rstrip(b"\0").decode(), base + vaddr, raw, rsize))
    return base, result


def main():
    data = open(sys.argv[1], "rb").read()
    pattern = re.compile(sys.argv[2], re.I)
    minimum = int(sys.argv[3]) if len(sys.argv) > 3 else 5
    base, table = sections(data)
    for name, vaddr, raw, rsize in table:
        chunk = data[raw:raw + rsize]
        for m in re.finditer(rb"[\x20-\x7e]{%d,}" % minimum, chunk):
            text = m.group().decode("ascii")
            if pattern.search(text):
                print("%08X %-6s %s" % (vaddr + m.start(), name, text))
        # UTF-16 strings
        for m in re.finditer(rb"(?:[\x20-\x7e]\x00){%d,}" % minimum, chunk):
            text = m.group().decode("utf-16-le")
            if pattern.search(text):
                print("%08X %-6s u'%s'" % (vaddr + m.start(), name, text))


if __name__ == "__main__":
    main()
