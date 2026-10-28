#!/usr/bin/env python3
"""Names the Windows 95 build's profile (port/dos/win95/w95_profile.c).

    python tools/dos_profile.py build/dos/HALO95.map PROFILE.BIN [--top N]

Prints the functions the samples were in (self) and the functions they were
in or under (total: a function counted once a sample however deep), from
lld-link's map file.
"""

import argparse
import bisect
import collections
import re
import struct
import subprocess
import sys

DEPTH = 12


def read_map(path):
    """lld-link's map (an address per symbol), or Open Watcom's (the code
    object's offsets, 0001:xxxxxxxx: where it was loaded comes from the
    profile's first record)"""
    symbols = []
    for line in open(path, encoding="latin-1"):
        m = re.match(r"\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{8,16})\s", line)
        if m:
            symbols.append((int(m.group(2), 16), m.group(1)))
            continue
        m = re.match(r"0001:([0-9a-fA-F]{8})[*+ ] (\S+)", line)
        if m:
            symbols.append((int(m.group(1), 16), m.group(2)))
    symbols.sort()
    return [s[0] for s in symbols], [s[1] for s in symbols]


def add_static_functions(keys, names, objects):
    """each object's functions (llvm-nm: code symbols, static ones too),
    placed by one of its public ones the map has (the object's .text kept
    whole by the linker)"""
    known = dict(zip(names, keys))
    symbols = list(zip(keys, names))
    for path in objects:
        listing = subprocess.run(["llvm-nm", "--defined-only", path], capture_output=True, text=True).stdout
        found = []
        for line in listing.splitlines():
            m = re.match(r"([0-9a-fA-F]+) ([tT]) (\S+)", line)
            if m:
                found.append((int(m.group(1), 16), m.group(3), m.group(2) == "T"))
        anchor = next(((offset, name) for offset, name, public in found if public and name in known), None)
        if not anchor:
            continue
        base = known[anchor[1]] - anchor[0]
        symbols.extend((base + offset, name) for offset, name, public in found if not public)
    symbols.sort()
    return [s[0] for s in symbols], [s[1] for s in symbols]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("map")
    parser.add_argument("profile")
    parser.add_argument("--top", type=int, default=40)
    parser.add_argument("--within", help="also the addresses the samples were at inside this function")
    parser.add_argument("--under", help="also the functions under this one (their total there, as a share of it)")
    parser.add_argument("--objects", nargs="*", default=[],
                        help="objects whose static functions to name too (Open Watcom's map lists only "
                             "public ones), found where a public one of theirs is")
    args = parser.parse_args(argv)
    keys, names = read_map(args.map)
    if args.objects:
        keys, names = add_static_functions(keys, names, args.objects)

    def name(address):
        i = bisect.bisect_right(keys, address) - 1
        return names[i] if i >= 0 and address - keys[i] < 0x100000 else f"?{address:08X}"

    data = open(args.profile, "rb").read()
    first = struct.unpack_from(f"<{DEPTH}I", data, 0)
    if first[0] == 0x464F5250:
        # the DOS build's: "PROF", where dos_profile_start was, the rate
        anchor = next(k for k, n in zip(keys, names) if n == "_dos_profile_start")
        delta = first[1] - anchor
        keys = [k + delta for k in keys]
        data = data[DEPTH * 4:]
    count = len(data) // (DEPTH * 4)
    own = collections.Counter()
    total = collections.Counter()
    within = collections.Counter()
    under = collections.Counter()
    under_count = [0]
    for i in range(count):
        words = struct.unpack_from(f"<{DEPTH}I", data, i * DEPTH * 4)
        frames = [w for w in words if w]
        if not frames:
            continue
        own[name(frames[0])] += 1
        if args.within and name(frames[0]) == args.within:
            within[frames[0]] += 1
        if args.under:
            chain = [name(w) for w in frames]
            if args.under in chain:
                under_count[0] += 1
                for function in set(chain[:chain.index(args.under)]):
                    under[function] += 1
        for function in {name(w) for w in frames}:
            total[function] += 1
    print(f"{count} samples")
    print("\nself:")
    for function, n in own.most_common(args.top):
        print(f"  {100.0 * n / count:6.2f}%  {function}")
    print("\ntotal:")
    for function, n in total.most_common(args.top):
        print(f"  {100.0 * n / count:6.2f}%  {function}")
    if args.under and under_count[0]:
        print(f"\nunder {args.under} ({100.0 * under_count[0] / count:.1f}% of the samples):")
        for function, n in under.most_common(args.top):
            print(f"  {100.0 * n / under_count[0]:6.2f}%  {function}")
    if args.within:
        start = next(k for k, n in zip(keys, names) if n == args.within)
        print(f"\nin {args.within} (offsets from its start):")
        for address, n in within.most_common(args.top):
            print(f"  {100.0 * n / count:6.2f}%  +{address - start:05X}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
