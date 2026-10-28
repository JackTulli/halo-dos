#!/usr/bin/env python3
"""Writes the DOS and Windows 95 builds' stand-ins for the code they leave out.

    python tools/dos_stubs.py            # after a build's objects exist
    python tools/dos_stubs.py --d3d      # the Direct3D build's (HALOD3D.EXE)

The DOS and Windows 95 builds drop whole subsystems, as the N64 build does
(the actor AI, networking, scripts, Halo's sound engine, the Xbox renderer,
the menus: port/dos/port.json); what the rest of the game still calls in
them is defined here as doing nothing: functions return zero (NONE-like
answers are listed in OVERRIDES), data is zeroed. Each stand-in is written
from the declaration the game sees, so its types are the real ones (a
function returning real returns 0.0 on the x87, one returning a structure
fills the caller's buffer). The N64 build's tools/n64_stubs.py is this
script's model.

The undefined names are found in the built objects (llvm-nm): every object
of build/dos/obj and the DOS C runtime, less what they define. Names with no
declaration in source/ (the platform's: Win32, the C runtime) are listed at
the end of the file and on the console: those are the platform layer's to
implement. Symbols the platform implements by hand are skipped
(port/dos/stubs_skip.txt).

The Direct3D build (port/dos/DIRECT3D.md) has its own: the objects of its
link (build/dos/d3d_link_objects.txt, written by tools/dos_build.py) and the
Windows 95 runtime, into port/dos/d3d/game. The Xbox Direct3D API is
declared in the XDK's headers, which are not searched: what the device has
not implemented yet is listed as the platform's.
"""

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUTPUT = ROOT / "port/dos/game/dos_stubs_generated.c"
# the game's pooled COMMON globals that no unit defines, as zeroed bytes of
# the sizes port/linux/src/halo_linker_common.c gives them: in a file of
# their own, which includes no game header whose typed declaration of the
# name would clash
COMMON_OUTPUT = ROOT / "port/dos/game/dos_common_generated.c"
COMMON_SIZES = ROOT / "chupathingyce/port/linux/src/halo_linker_common.c"
# the game's sources (the submodule)
GAME_SOURCE = ROOT / "chupathingyce/source"
NM = Path("C:/Program Files/LLVM/bin/llvm-nm.exe")
COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
PREPROC = re.compile(r"^\s*#.*?(?<!\\)$", re.M | re.S)

# functions whose "nothing happened" answer is not zero
OVERRIDES = {
    "global_network_game_server_get": "return NULL;",
    "global_network_game_client_get": "return NULL;",
    # no network object index: the object takes any free one
    "network_objects_new_object_index": "return NONE;",
    # sounds, decals and dynamic geometry that were not made
    "game_looping_sound_new": "return NONE;",
    "object_impulse_sound_new": "return NONE;",
    "unattached_impulse_sound_new": "return NONE;",
    "unattached_looping_sound_start": "return NONE;",
    "unspatialized_impulse_sound_new": "return NONE;",
    "rasterizer_decal_vertices_new": "return NONE;",
    "rasterizer_dynamic_triangles_new": "return NONE;",
    "rasterizer_dynamic_vertices_new": "return NONE;",
    # the PC ports' distributed-netcode hooks: a local game deals its own
    # damage and deletes its own objects
    "network_damage_deals": "return TRUE;",
    "network_objects_may_delete": "return TRUE;",
    # sound scenery is an object like any other (it just makes no sound)
    "sound_scenery_new": "return TRUE;",
    # (an object list nothing made)
    "object_list_new": "return NONE;",
    "object_list_get_first": "return NONE;",
    "object_list_get_next": "return NONE;",
}


D3D = "--d3d" in sys.argv
if D3D:
    OUTPUT = ROOT / "port/dos/d3d/game/d3d_stubs_generated.c"
    COMMON_OUTPUT = ROOT / "port/dos/d3d/game/d3d_common_generated.c"


def undefined_names():
    # (not the stand-ins' own object: a name something else now defines must
    # leave this file)
    if D3D:
        listed = (ROOT / "build/dos/d3d_link_objects.txt").read_text(encoding="utf-8").split()
        objects = [str(ROOT / o) for o in listed if Path(o).stem not in ("d3d_stubs_generated", "d3d_common_generated")]
        objects.append(str(ROOT / "build/dos/crt-w95/w95crt.lib"))
    else:
        objects = sorted(str(p) for p in (ROOT / "build/dos/obj").rglob("*.obj")
                         if "tests" not in p.parts and p.stem not in ("dos_stubs_generated", "dos_common_generated"))
        objects.append(str(ROOT / "build/dos/crt-dos/doscrt.lib"))
    defined, undefined = set(), set()
    for i in range(0, len(objects), 60):
        out = subprocess.run([str(NM), "--no-sort", *objects[i:i + 60]], capture_output=True, text=True).stdout
        for line in out.splitlines():
            parts = line.split()
            if len(parts) == 2 and parts[0] == "U":
                undefined.add(parts[1])
            elif len(parts) == 3 and parts[1] in "TtDdBbRrCWwVvS":
                defined.add(parts[2])
    names = []
    for symbol in sorted(undefined - defined):
        # cdecl C names (_name: the game's own may begin with _ too);
        # stdcall ones, name@n, are the platform's
        if symbol.startswith("_") and "@" not in symbol:
            names.append(symbol[1:])
        else:
            names.append(symbol)
    return names


# the native ports' game headers the game's sources include (their netcode,
# co-op, interpolation: the include paths the build gives the game units)
EXTRA_HEADER_DIRS = [ROOT / "chupathingyce/port/linux/game", ROOT / "chupathingyce/port/linux/include"]


def load_texts():
    texts = {}
    for path in sorted(GAME_SOURCE.rglob("*")):
        if path.suffix in (".h", ".c") and path.is_file():
            texts[path] = COMMENT.sub(" ", path.read_text(encoding="latin-1"))
    for folder in EXTRA_HEADER_DIRS:
        for path in sorted(folder.glob("*.h")):
            texts[path] = COMMENT.sub(" ", path.read_text(encoding="latin-1"))
    return texts


def top_level(text):
    """the text with function bodies blanked (brace depth > 0)"""
    out = []
    depth = 0
    for ch in text:
        if ch == "{":
            depth += 1
            out.append(" ")
        elif ch == "}":
            depth -= 1
            out.append(" " if depth else ";")
        else:
            out.append(ch if depth == 0 else (" " if ch != "\n" else "\n"))
    return "".join(out)


def find_declaration(name, texts, tops):
    func = re.compile(r"(?:^|;|\n)\s*((?:extern\s+)?[A-Za-z_][\w\s\*]*?[\s\*])" + re.escape(name) +
                      r"\s*\(([^;{}]*?)\)\s*;", re.S)
    data = re.compile(r"(?:^|;|\n)\s*extern\s+([A-Za-z_][\w\s\*]*?[\s\*])" + re.escape(name) +
                      r"\s*((?:\[[^\]]*\])*)\s*;", re.S)
    # headers first, then sources (a .c file's own prototype)
    order = sorted(texts, key=lambda p: (p.suffix != ".h", str(p)))
    for path in order:
        top = tops[path]
        if name not in top:
            continue
        m = func.search(top)
        if m and "typedef" not in m.group(1):
            return "func", path, m.group(1), m.group(2)
        m = data.search(top)
        if m:
            return "data", path, m.group(1), m.group(2)
    return None


def clean(text):
    text = PREPROC.sub(" ", text)
    return " ".join(text.split())


def main():
    names = undefined_names()
    skip = set()
    for skip_file in [ROOT / "port/dos/stubs_skip.txt"] + ([ROOT / "port/dos/d3d/stubs_skip.txt"] if D3D else []):
        if skip_file.exists():
            skip |= {l.split("#")[0].strip() for l in skip_file.read_text().splitlines()} - {""}
    texts = load_texts()
    tops = {p: top_level(t) for p, t in texts.items()}
    # (a size may be a sum, "30924 + 8", and differ between #ifdef branches --
    # dsound_globals is larger with HALO_CUSTOM_EDITION's eight sound channel
    # types: the largest of a name's sizes is taken, never too little room)
    common_sizes = {}
    for m in re.finditer(r"HALO_COMMON\((\w+),\s*([0-9+*\- ()x]+)\)", COMMON_SIZES.read_text(encoding="latin-1")):
        size = sum(int(term, 0) for term in m.group(2).replace(" ", "").split("+"))
        common_sizes[m.group(1)] = max(common_sizes.get(m.group(1), 0), size)
    headers = set()
    funcs, datas, missing, commons = [], [], [], []
    for name in sorted(set(names)):
        if name in skip:
            continue
        if name in common_sizes:
            commons.append((name, common_sizes[name]))
            continue
        if not re.match(r"^[A-Za-z_]\w*$", name) or name in ("_tls_index", "_tls_array"):
            missing.append(name)
            continue
        found = find_declaration(name, texts, tops)
        if not found:
            missing.append(name)
            continue
        kind, path, rtype, rest = found
        rtype = clean(rtype).replace("extern ", "").strip()
        rtype = re.sub(r"\b(static|__inline|_inline|__forceinline|__cdecl|__stdcall|WINAPI)\b", "", rtype).strip()
        if path.suffix == ".h":
            headers.add(path)
        if kind == "func":
            funcs.append((name, rtype, clean(rest), path))
        else:
            datas.append((name, rtype, clean(rest), path))
    lines = ["/* generated by tools/dos_stubs.py - do not edit (see that script) */", "", '#include "cseries.h"', '#include "math/real_math.h"']
    for h in sorted(headers, key=str):
        rel = h.relative_to(GAME_SOURCE).as_posix() if GAME_SOURCE in h.parents else h.name
        lines.append(f'#include "{rel}"')
    lines += ["#include <string.h>", ""]
    for name, rtype, dims, path in datas:
        # an array the declaration leaves unsized: room enough for a flag
        dims = dims.replace("[]", "[16]")
        lines.append(f"{rtype} {name}{dims}; /* stand-in: {name} */")
    lines.append("")
    for name, rtype, params, path in funcs:
        params = params or "void"
        if name in OVERRIDES:
            body = OVERRIDES[name]
        elif rtype == "void":
            body = ""
        else:
            body = f"{rtype} result; memset(&result, 0, sizeof(result)); return result;"
        lines.append(f"{rtype} {name}({params}) {{ {body} }} /* stand-in: {name} */")
    platform = missing
    if platform:
        lines += ["", "/* no declaration in source/ (the platform layer's to implement): " + " ".join(platform) + " */"]
    text = "\n".join(lines) + "\n"
    if not OUTPUT.exists() or OUTPUT.read_text(encoding="latin-1") != text:
        OUTPUT.write_text(text, encoding="latin-1", newline="\n")
    common = ["/* generated by tools/dos_stubs.py - do not edit (see that script): the game's pooled",
              "COMMON globals no unit defines, zeroed, of halo_linker_common.c's sizes */", ""]
    common += [f"__attribute__((aligned(16))) unsigned char {name}[{size}];" for name, size in commons]
    common_text = "\n".join(common) + "\n"
    if not COMMON_OUTPUT.exists() or COMMON_OUTPUT.read_text(encoding="latin-1") != common_text:
        COMMON_OUTPUT.write_text(common_text, encoding="latin-1", newline="\n")
    print(f"{len(funcs)} functions, {len(datas)} data, {len(commons)} common, {len(headers)} headers; "
          f"the platform's to implement: "
          + (" ".join(platform) if platform else "none"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
