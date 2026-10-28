#!/usr/bin/env python3
"""
Writes every bitmap of Xbox maps to dump files for t_textures (--maps).

A map is its 0x800-byte header followed by one zlib stream; the tag data
loads at 0x803A6000 with the tag instance table after its 0x24-byte header
(source/cache/cache_files.c). A 'bitm' tag's bitmaps block is at +0x60 of
the tag, each bitmap_data 0x30 bytes (source/bitmaps/bitmap_group.h); its
pixels are at a file offset of the decompressed map, in the layout the
texture cache hands to Direct3D (source/cache/xbox_texture_cache.c).

    python3 bitmap_dump.py <output folder> <map>...

A dump is, for each bitmap, a 32-byte header (the words below) and the
pixel bytes.
"""

import struct
import sys
import zlib
from collections import Counter
from pathlib import Path

TAG_BASE = 0x803A6000
FORMATS = ["a8", "y8", "ay8", "a8y8", "?4", "?5", "r5g6b5", "?7", "a1r5g5b5", "a4r4g4b4", "x8r8g8b8", "a8r8g8b8",
    "?12", "?13", "dxt1", "dxt3", "dxt5", "p8"]
TYPES = ["2d", "3d", "cube"]


def load(path):
    raw = Path(path).read_bytes()
    return raw[:0x800] + zlib.decompress(raw[0x800:])


def dump(path, folder):
    data = load(path)
    tag_offset, tag_size = struct.unpack_from("<II", data, 0x10)
    tags = data[tag_offset:tag_offset + tag_size]

    def at(address):
        return address - TAG_BASE

    instances, _, _, count = struct.unpack_from("<IIII", tags, 0)
    kinds = Counter()
    texels = 0
    written = 0
    out = (Path(folder) / (Path(path).stem + ".bitmaps")).open("wb")
    for index in range(count):
        group, _, _, _, _, base = struct.unpack_from("<IIIIII", tags, at(instances) + index * 0x20)
        if group != 0x6269746D or not base:
            continue
        bitmap_count, bitmaps = struct.unpack_from("<II", tags, at(base) + 0x60)
        for bitmap in range(bitmap_count):
            (signature, width, height, depth, kind, pixel_format, flags, _, _, mipmaps, _, pixels_offset,
                pixels_size) = struct.unpack_from("<IhhhhhHhhhhii", tags, at(bitmaps) + bitmap * 0x30)
            if pixels_offset < 0 or pixels_offset + pixels_size > len(data) or pixels_size <= 0:
                kinds["outside the file"] += 1
                continue
            name = "%s %s%s%s%s" % (TYPES[kind] if 0 <= kind < 3 else "?", FORMATS[pixel_format] if
                0 <= pixel_format < len(FORMATS) else "?", "" if flags & 1 else " npot",
                " linear" if flags & 16 else "", " v16u16" if flags & 32 else "")
            kinds[name] += 1
            texels += width * height * depth * (6 if kind == 2 else 1)
            out.write(struct.pack("<4sHHHHHHHHII4x", b"bmp1", width, height, depth, kind, pixel_format, flags,
                mipmaps, 0, pixels_offset, pixels_size))
            out.write(data[pixels_offset:pixels_offset + pixels_size])
            written += 1
    out.close()
    print("%s: %d bitmaps, %d texels in their first levels" % (Path(path).stem, written, texels))
    for name, number in sorted(kinds.items()):
        print("    %-28s %d" % (name, number))
    return kinds


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    total = Counter()
    for path in sys.argv[2:]:
        total.update(dump(path, sys.argv[1]))
    print("all maps:")
    for name, number in sorted(total.items()):
        print("    %-28s %d" % (name, number))


if __name__ == "__main__":
    main()
