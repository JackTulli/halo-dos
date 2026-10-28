#!/usr/bin/env python3
"""Ninja file for the MS-DOS and Windows 95 builds (port/dos).

    python tools/dos_build.py            # writes build/dos/build.ninja
    ninja -f build/dos/build.ninja       # build/dos/HALO.EXE and HALO95.EXE
    ninja -f build/dos/build.ninja d3d   # build/dos/HALOD3D.EXE (port/dos/DIRECT3D.md)

Run both from the repository's root, on Windows. One build of the game for
both systems: the N64 build's cut-down Halo (port/dos/port.json leaves out
the same subsystems), compiled once by clang for the Microsoft target (the
game's ABI, as on Windows) and for any Pentium (x87 arithmetic, no MMX or
SSE), with the 9x build's C runtime (nxdk's PDCLib over KERNEL32,
port/win9x). The objects are linked twice:

- MS-DOS: Open Watcom's wlink makes a linear executable bound to the
  DOS/32A extender (HALO.EXE). The C runtime's KERNEL32 calls are answered
  by port/dos/dos/dos_kernel32.c over DPMI and DOS, so the runtime and the
  platform layer's Win32 half are the same code on both systems;
- Windows 95: lld-link makes a PE executable for Windows 95 and later
  (HALO95.EXE), importing only what port/win9x/lib/*.def lists.

What each part is:

- the game: ChupathingyCE's (the submodule chupathingyce/, its
  port/linux/port.json sources) less port.json's exclusions, plus
  its extra sources and port/dos/game (the game-side glue, which sees the
  game's headers);
- port/dos/xdk: the Xbox SDK calls the game makes, over Win32;
- port/dos/render: the software renderer, plain C over its own headers;
- port/dos/dos and port/dos/win95: each system's own half (start-up,
  video, input; DOS's KERNEL32).

Needs clang, lld-link, llvm-lib and llvm-dlltool (LLVM for Windows),
Open Watcom (WATCOM, or C:\\WATCOM), nxdk's sources for PDCLib (NXDK_DIR or
~/nxdk) and a Windows SDK's headers. README.md's "Building from source"
says where to get each, and runs tools/dos_hooks.py first.
"""

import argparse
import json
import os
import re
import shutil
import sys
from pathlib import Path
from typing import Dict, List, Optional, Sequence

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
from tools.ninja_syntax import Writer  # noqa: E402

PORT_DIR = Path("port/dos")
BUILD = Path("build/dos")
WIN9X_DIR = Path("port/win9x")
# the game (a submodule): its sources, its Windows prefix and the Xbox SDK's
# declarations
GAME = Path("chupathingyce")
WINDOWS_DIR = GAME / "port" / "windows"
XBOX_DIR = Path("port/xbox")
LINUX_DIR = GAME / "port" / "linux"
XDK_INCLUDE = GAME / "port" / "include" / "xdk"
MUSL_MATH_DIR = Path("port/support/musl-math")
# what the builds take from beside the game (the map rebasing and the Xbox
# texel converter, the other ports' own)
SUPPORT_DIR = Path("port/support")

# Shared by every unit: the Microsoft target (the game's ABI: structure
# layout, 16-bit wchar_t, calling conventions, COMDAT inline functions) for
# the first Pentium: x87 arithmetic, no MMX, no SSE, no conditional moves.
# No host headers (PDCLib's and the SDK's are given explicitly; Open
# Watcom's INCLUDE would otherwise be read). MSVC's tolerances (common
# tentative definitions, aliasing, overflow, null checks), no stack cookies
# (no runtime for them), frame pointers (the crash report walks them).
ABI_FLAGS = [
    "--target=i686-pc-windows-msvc",
    "-march=pentium",
    "-mno-mmx",
    "-mno-sse",
    "-mfpmath=387",
    "-ffp-contract=off",
    "-fno-math-errno",
    "-nostdlibinc",
    "-fms-extensions",
    "-fcommon",
    "-fno-strict-aliasing",
    "-fwrapv",
    "-fno-delete-null-pointer-checks",
    "-fno-omit-frame-pointer",
    "-fno-stack-protector",
    "-Wno-builtin-macro-redefined",
    "-D__STDC__=1",
    "-U__STDC_NO_THREADS__",
    # one thread: thread-locals are plain storage (the game's assertion flag,
    # the wide runtime's wcstok)
    "-D__thread=",
]

OPTIMISATION = "-O2"

# the game's own units (the Windows build's)
GAME_FLAGS = [
    "-std=gnu89",
    "-w",
    "-Wno-error=incompatible-pointer-types",
    "-Wno-error=incompatible-function-pointer-types",
    "-Wno-error=int-conversion",
    "-Wno-error=implicit-function-declaration",
    "-Wno-error=implicit-int",
    "-Wno-error=return-type",
]

# the port's own units
PORT_FLAGS = [
    "-std=gnu11",
    "-Wall",
    "-Wno-unused-function",
    "-Wno-unknown-pragmas",
    "-Wno-microsoft-anon-tag",
    "-Wno-pragma-pack",
    "-Wno-ignored-attributes",
    "-Wno-missing-braces",
    "-Wno-ignored-pragmas",
    "-Wno-microsoft-enum-forward-reference",
    "-Wno-language-extension-token",
    "-Wno-nonportable-include-path",
]

# the C runtime: freestanding (its memcpy must not call itself), warnings
# off for nxdk's sources
RUNTIME_FLAGS = ["-std=gnu11", "-ffreestanding", "-fno-builtin", "-w"]

# the DOS build's runtime: KERNEL32 and USER32 are not imported but linked
# (port/dos/dos/dos_kernel32.c); one thread, so no thread-local storage
# (wlink places no .tls section)
DOS_RUNTIME_FLAGS = ["-D_KERNEL32_", "-D_USER32_", "-D__thread=", "-DHALO_DOS_RUNTIME=1"]

# what link-time code may call that the archives must keep (as the 9x build)
RUNTIME_LIBCALLS = [
    "_memcpy", "_memmove", "_memset", "_memcmp", "__alldiv", "__allmul", "__allrem", "__allshl", "__allshr",
    "__aulldiv", "__aullrem", "__aullshr", "__chkstk", "__fltused",
]

EXPORTED_INLINE = re.compile(
    r"^(?!\s*static\b)[^;{}()\n]*\b(?:__inline|_inline|__forceinline)\b[^;{}()]*?"
    r"\b([A-Za-z_]\w*)\s*\([^;{}]*\)\s*\{",
    re.M,
)
COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)


def posix(path) -> str:
    return str(path).replace("\\", "/")


def quote(path) -> str:
    text = posix(path)
    return f'"{text}"' if " " in text else text


def dos_path(path) -> str:
    """a path for Open Watcom's tools, which take / for an option's switch"""
    text = str(path).replace("/", "\\")
    return f'"{text}"' if " " in text else text


def object_path(obj_dir: Path, source: Path, nxdk: Optional[Path] = None) -> Path:
    """an object's place under obj_dir: nxdk's sources under nxdk/, the
    repository's at their own path, spaces made underscores (response
    files)"""
    relative = Path(source)
    if nxdk is not None:
        try:
            relative = Path("nxdk") / Path(source).resolve().relative_to(nxdk.resolve())
        except ValueError:
            pass
    return obj_dir / Path(posix(relative.with_suffix(".obj")).replace(" ", "_"))


# ---------- the tools

def find_program(names: Sequence[str], folders: Sequence[Path] = ()) -> Optional[Path]:
    for folder in folders:
        for name in names:
            for suffix in ("", ".exe"):
                candidate = folder / (name + suffix)
                if candidate.is_file():
                    return candidate
    for name in names:
        found = shutil.which(name)
        if found:
            return Path(found)
    return None


def find_windows_sdk() -> Optional[Path]:
    """a Windows 10 or 11 SDK's include directory of one version (um/,
    shared/)"""
    candidates: List[Path] = []
    sdk_dir, sdk_version = os.environ.get("WindowsSdkDir"), os.environ.get("WindowsSDKVersion")
    if sdk_dir and sdk_version:
        candidates.append(Path(sdk_dir) / "include" / sdk_version.strip("\\/"))
    for root in (Path("C:/Program Files (x86)/Windows Kits/10/include"),
                 Path("C:/Program Files/Windows Kits/10/include")):
        if root.is_dir():
            candidates.extend(sorted((p for p in root.iterdir() if p.is_dir()), key=lambda p: p.name, reverse=True))
    for candidate in candidates:
        if (candidate / "um" / "windows.h").is_file() and (candidate / "shared").is_dir():
            return candidate
    return None


class Toolchain:
    def __init__(self, args) -> None:
        llvm = [Path(args.llvm)] if args.llvm else []
        llvm.append(Path("C:/Program Files/LLVM/bin"))
        self.cc = find_program(["clang"], llvm)
        self.lld_link = find_program(["lld-link"], llvm)
        self.lib = find_program(["llvm-lib"], llvm)
        self.dlltool = find_program(["llvm-dlltool"], llvm)
        watcom = Path(args.watcom or os.environ.get("WATCOM") or "C:/WATCOM")
        self.watcom = watcom
        self.wlink = find_program(["wlink"], [watcom / "binnt", watcom / "binw"])
        self.wasm = find_program(["wasm"], [watcom / "binnt", watcom / "binw"])
        self.dos32a = watcom / "binw" / "dos32a.exe"
        nxdk = args.nxdk or os.environ.get("NXDK_DIR") or str(Path.home() / "nxdk")
        self.nxdk = Path(nxdk)
        self.sdk = Path(args.sdk) if args.sdk else find_windows_sdk()

    def missing(self) -> List[str]:
        reasons = []
        for name, value in (("clang", self.cc), ("lld-link", self.lld_link), ("llvm-lib", self.lib),
                            ("llvm-dlltool", self.dlltool), ("Open Watcom's wlink", self.wlink),
                            ("Open Watcom's wasm", self.wasm)):
            if value is None:
                reasons.append(f"{name} not found")
        if not self.dos32a.is_file():
            reasons.append(f"DOS/32A not found ({self.dos32a})")
        if not (self.nxdk / "lib/pdclib/functions/stdio/fopen.c").is_file():
            reasons.append(f"nxdk's PDCLib sources not found in {self.nxdk} (NXDK_DIR; "
                           "git submodule update --init lib/pdclib)")
        if self.sdk is None:
            reasons.append("no Windows SDK headers (--sdk DIR, the directory with um/windows.h)")
        return reasons


# ---------- the include sets

def pdclib_includes(tools: Toolchain) -> List[str]:
    return [f"-I{WIN9X_DIR / 'pdclib' / 'include'}", f"-isystem {quote(tools.nxdk / 'lib/pdclib/include')}"]


def sdk_includes(tools: Toolchain) -> List[str]:
    return [f"-isystem {quote(tools.sdk / 'um')}", f"-isystem {quote(tools.sdk / 'shared')}"]


def crt_includes() -> List[str]:
    return [f"-I{WIN9X_DIR / 'include' / 'crt'}", f"-I{XBOX_DIR / 'include' / 'crt'}"]


# ---------- the game's MSVC inline semantics

def inline_export_wrapper(source: Path) -> Path:
    """As tools/windows_build.py: a game unit that defines an external
    __inline function is compiled through a wrapper that takes the
    functions' addresses, so that clang emits them."""
    if source.suffix != ".c" or not posix(source).startswith(posix(GAME / "source") + "/"):
        return source
    names = EXPORTED_INLINE.findall(COMMENT.sub("", source.read_text(encoding="utf-8", errors="replace")))
    if not names:
        return source
    wrapper = BUILD / "inline_exports" / source
    text = (
        "/* generated by tools/dos_build.py: see inline_export_wrapper */\n"
        f'#include "{source.resolve().as_posix()}"\n'
        "static void *const halo_dos_inline_exports[] __attribute__((used)) = {\n"
        + "".join(f"\t(void *){name},\n" for name in names)
        + "};\n"
    )
    wrapper.parent.mkdir(parents=True, exist_ok=True)
    if not wrapper.is_file() or wrapper.read_text(encoding="utf-8") != text:
        wrapper.write_text(text, encoding="utf-8")
    return wrapper


# ---------- the runtime

def runtime_sources(tools: Toolchain, win9x: Dict) -> List[tuple]:
    """the 9x build's C runtime units and their kind (pdclib: nxdk's
    generic PDCLib and maths; glue: port/win9x/pdclib; runtime:
    port/win9x/src and the shared Xbox units; helper: nxdk's assembly)"""
    sources = []
    functions = tools.nxdk / "lib" / "pdclib" / "functions"
    replaced = set(win9x.get("pdclib_replaced", []))
    for source in sorted(functions.rglob("*.c")):
        if source.relative_to(functions).as_posix() not in replaced:
            sources.append((source, "pdclib"))
    math = tools.nxdk / "lib" / "pdclib" / "platform" / "xbox" / "functions" / "math"
    math_replaced = set(win9x.get("nxdk_math_replaced", []))
    for source in sorted(math.glob("*.c")):
        if source.stem not in math_replaced:
            sources.append((source, "pdclib"))
    for source in sorted((WIN9X_DIR / "pdclib" / "functions").rglob("*.c")):
        sources.append((source, "glue"))
    for name in win9x.get("runtime_sources", []):
        # (the 9x malloc is VirtualAlloc's; the DOS one is its own)
        sources.append((Path(name), "runtime"))
    for name in win9x.get("nxdk_runtime_helpers", []):
        sources.append((tools.nxdk / "lib" / "xboxrt" / "c_runtime" / name, "helper"))
    # what these builds add (the x87's floating-point flags)
    for source in sorted((PORT_DIR / "runtime").glob("*.c")):
        sources.append((source, "runtime"))
    return sources


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--llvm", help="LLVM's bin folder (default: C:/Program Files/LLVM/bin, then the PATH)")
    parser.add_argument("--watcom", help="Open Watcom's folder (default: WATCOM, or C:/WATCOM)")
    parser.add_argument("--nxdk", help="nxdk's sources (default: NXDK_DIR, or ~/nxdk)")
    parser.add_argument("--sdk", help="a Windows SDK's include directory of one version")
    parser.add_argument("--launcher", nargs=2, metavar=("SOURCE", "OUT"),
                        help="copy a launcher batch file with DOS line ends (the build's own step)")
    args = parser.parse_args(argv)
    if args.launcher:
        lines = Path(args.launcher[0]).read_text(encoding="utf-8").splitlines()
        Path(args.launcher[1]).write_bytes("".join(line + "\r\n" for line in lines).encode("ascii"))
        return 0
    os.chdir(ROOT)
    tools = Toolchain(args)
    missing = tools.missing()
    if missing:
        for reason in missing:
            print(f"dos_build: {reason}", file=sys.stderr)
        return 1

    port = json.loads((PORT_DIR / "port.json").read_text(encoding="utf-8"))
    win9x = json.loads((WIN9X_DIR / "port.json").read_text(encoding="utf-8"))
    # the game's sources, defines and include folders (the native builds')
    game_config = json.loads((LINUX_DIR / "port.json").read_text(encoding="utf-8"))["game"]
    options = {
        "defines": game_config.get("defines", []),
        "include_dirs": [posix(GAME / d) for d in game_config.get("include_dirs", [])],
    }

    BUILD.mkdir(parents=True, exist_ok=True)
    out_path = BUILD / "build.ninja"
    with open(out_path, "w", encoding="utf-8", newline="\n") as out:
        n = Writer(out)
        n.comment("generated by tools/dos_build.py - do not edit")
        n.variable("ninja_required_version", "1.10")
        n.variable("builddir", posix(BUILD))
        n.variable("cc", quote(tools.cc))
        n.variable("lld_link", quote(tools.lld_link))
        n.variable("llvm_lib", quote(tools.lib))
        n.variable("dlltool", quote(tools.dlltool))
        n.variable("wlink", quote(tools.wlink))
        n.variable("wasm", quote(tools.wasm))
        n.variable("python", quote(Path(sys.executable)))
        n.newline()
        n.rule("cc", command="$cc -MMD -MF $out.d $cflags -c $in -o $out", description="DOS CC $out",
               depfile="$out.d", deps="gcc")
        n.rule("as", command="$cc $asflags -c $in -o $out", description="DOS AS $out")
        n.rule("wasm", command="$wasm -q $source -fo=$object", description="DOS WASM $out")
        n.rule("lib", command="$llvm_lib /out:$out @$out.rsp", description="DOS LIB $out",
               rspfile="$out.rsp", rspfile_content="$in_newline")
        n.rule("implib", command="$dlltool -m i386 -k -d $in -l $out", description="DOS IMPLIB $out")
        n.rule("tags", command=f"$python {posix(GAME / 'tools' / 'linux_msvc_semantics.py')} --output $out "
               f"--tags {posix(GAME / 'source')}",
               description="DOS MSVC TAGS $out", restat=True)
        # wlink reads a directive file: the format, the stub, the entry, the
        # stack, then each object and library
        n.rule("wlink", command="$wlink @$out.lnk", description="DOS LINK $out",
               rspfile="$out.lnk", rspfile_content="$directives $in_newline_files $libs_files")
        n.rule("lld_link", command="$lld_link $ldflags -out:$out @$out.rsp $libs", description="WIN95 LINK $out",
               rspfile="$out.rsp", rspfile_content="$in_newline")
        n.rule("regenerate", command="$python tools/dos_build.py", description="DOS CONFIGURE", generator=True)
        n.rule("launcher", command="$python tools/dos_build.py --launcher $in $out", description="DOS LAUNCHER $out")
        n.newline()
        n.build(posix(out_path), "regenerate",
                implicit=["tools/dos_build.py", posix(PORT_DIR / "port.json"), posix(WIN9X_DIR / "port.json"),
                          posix(LINUX_DIR / "port.json"), *sorted(posix(p) for p in PORT_DIR.rglob("*.c")),
                          *sorted(posix(p) for p in PORT_DIR.rglob("*.s"))])
        n.newline()

        tags = BUILD / "halo_msvc_tags.h"
        game_headers = sorted(posix(p) for p in (GAME / "source").rglob("*") if p.suffix in (".c", ".h"))
        n.build(posix(tags), "tags", implicit=[posix(GAME / "tools" / "linux_msvc_semantics.py"), *game_headers])

        prefix = PORT_DIR / "include" / "halo_dos_prefix.h"
        abi = " ".join(ABI_FLAGS + [OPTIMISATION, "-g", "-gcodeview"])
        defines = " ".join(f"-D{d}" for d in (options.get("defines") or []) + port.get("defines", []))
        includes = " ".join(f"-I{quote(d)}" for d in options.get("include_dirs") or []
                            if Path(d) != Path("xbox/include"))
        game_cflags = " ".join([
            abi, *GAME_FLAGS,
            f"-include {posix(prefix)}", f"-include {posix(tags)}",
            defines, *crt_includes(), f"-I{posix(WINDOWS_DIR / 'include')}", includes,
            f"-I{posix(XDK_INCLUDE)}", *pdclib_includes(tools),
            # (the native ports' headers the shared sources include: their
            # network co-op's, the keyboard's, the settings'), after the rest
            f"-idirafter {posix(LINUX_DIR / 'include')}", f"-idirafter {posix(LINUX_DIR / 'game')}",
            f"-I{posix(SUPPORT_DIR)}",
        ])
        # the Xbox-facing platform units (port/dos/xdk) and the game-side
        # glue's port flags: the game's headers, the Xbox SDK's
        xdk_cflags = " ".join([
            abi, *PORT_FLAGS, "-DHALO_LINUX_PLATFORM_LAYER", defines,
            f"-include {posix(prefix)}",
            # (one thread: port/dos/include/threads' pthread.h first)
            f"-I{posix(PORT_DIR / 'include' / 'threads')}",
            *crt_includes(), f"-I{posix(PORT_DIR / 'include')}", f"-I{posix(PORT_DIR / 'xdk')}",
            f"-I{posix(PORT_DIR / 'render')}", f"-I{posix(WINDOWS_DIR / 'include')}",
            f"-I{posix(GAME / 'source')} -I{posix(GAME / 'source' / 'cseries')}", f"-I{posix(SUPPORT_DIR)}",
            f"-I{posix(XDK_INCLUDE)}", *pdclib_includes(tools),
        ])
        # the renderer: plain C over the runtime's headers
        render_cflags = " ".join([abi, *PORT_FLAGS, f"-I{posix(PORT_DIR / 'render')}",
                                  f"-I{posix(PORT_DIR / 'include')}", *crt_includes(), *pdclib_includes(tools)])
        # each system's own half: Win32 (the SDK's headers over PDCLib's)
        win32_cflags = " ".join([
            abi, *PORT_FLAGS, "-DWIN32_LEAN_AND_MEAN", "-D_WIN32_WINNT=0x0400", "-DWINVER=0x0400",
            f"-include {posix(WIN9X_DIR / 'include' / 'w9x_win32.h')}",
            f"-I{posix(PORT_DIR / 'include')}", f"-I{posix(PORT_DIR / 'render')}",
            f"-I{posix(WIN9X_DIR / 'include')}", *crt_includes(), *pdclib_includes(tools), *sdk_includes(tools),
            # (Direct3D 8's: Wine's headers, the SDK has none: w95_d3d.c)
            f"-I{posix(PORT_DIR / 'win95' / 'include')}",
        ])

        game_objects: List[str] = []

        def add(objects: List[str], source: Path, cflags: str, obj_dir: Path = BUILD / "obj",
                implicit: Sequence[str] = ()) -> None:
            obj = object_path(obj_dir, source)
            objects.append(posix(obj))
            n.build(posix(obj), "cc", posix(inline_export_wrapper(source)),
                    implicit=[posix(tags), *implicit], variables={"cflags": cflags})

        exclude_dirs = [d.rstrip("/") + "/" for d in port.get("exclude_dirs", [])]
        exclude_sources = set(port.get("exclude_sources", []))
        game_excluded = set(game_config.get("exclude", []))
        for source in sorted((GAME / game_config["root"]).rglob("*.c")):
            name = source.relative_to(GAME).as_posix()
            if name in game_excluded or name in exclude_sources or any(name.startswith(d) for d in exclude_dirs):
                continue
            add(game_objects, source, game_cflags)
        for extra in port.get("extra_game_sources", []):
            add(game_objects, GAME / extra, game_cflags)
        # the native ports' game code these builds play the same (their
        # netcode, co-op, PAL maps): port.json's port_game_sources
        for name in port.get("port_game_sources", []):
            add(game_objects, LINUX_DIR / "game" / name, game_cflags)
        for source in sorted((PORT_DIR / "game").glob("*.c")):
            add(game_objects, source, game_cflags + f" -std=gnu99 -I{posix(PORT_DIR / 'include')} "
                f"-I{posix(PORT_DIR / 'render')}")
        math_start = len(game_objects)
        for source in sorted((MUSL_MATH_DIR / "src").glob("*.c")):
            add(game_objects, source, " ".join([abi, "-std=gnu11", "-w", f"-I{MUSL_MATH_DIR}/include",
                                                f"-include {MUSL_MATH_DIR}/include/libm.h",
                                                *pdclib_includes(tools)]))
        math_objects = game_objects[math_start:]
        platform_objects: List[str] = []
        for source in sorted((PORT_DIR / "xdk").glob("*.c")):
            add(platform_objects, source, xdk_cflags)
        for name in port.get("xdk_sources", []):
            add(platform_objects, Path(name), xdk_cflags)
        # internet play (ChupathingyCE's p2p, polled: port.json's p2p_sources)
        # and the libraries it uses
        third_party = GAME / "port" / "third_party"
        p2p_cflags = " ".join([xdk_cflags, f"-I{posix(third_party / 'kcp')}", f"-I{posix(third_party / 'monocypher')}",
                               f"-idirafter {posix(LINUX_DIR / 'include')}"])
        for name in port.get("p2p_sources", []):
            add(platform_objects, Path(name), p2p_cflags)
        # the retail Halo PC / Custom Edition servers ONLINE PLAY lists, to
        # look at (port/dos/retail; its tools/ is the host test): over the
        # sockets internet play uses (ChupathingyCE's posix.h), built as its
        # units are
        for source in sorted((PORT_DIR / "retail").glob("*.c")):
            add(platform_objects, source, p2p_cflags + f" -I{posix(LINUX_DIR / 'src')}")
        for name in port.get("third_party_sources", []):
            add(platform_objects, Path(name), " ".join([abi, "-std=gnu11", "-w", *crt_includes(),
                                                        *pdclib_includes(tools)]))
        render_start = len(platform_objects)
        for source in sorted((PORT_DIR / "render").glob("*.c")):
            add(platform_objects, source, render_cflags)
        # (the renderer's hand-written loops: both systems')
        for source in sorted((PORT_DIR / "render").glob("*.s")):
            obj = object_path(BUILD / "obj", source)
            platform_objects.append(posix(obj))
            n.build(posix(obj), "as", posix(source), variables={"asflags": "--target=i686-pc-windows-msvc"})
        # (the Direct3D build keeps the texture decoder: render_sources)
        render_objects = set(platform_objects[render_start:])
        for name in port.get("render_sources", []):
            add(platform_objects, Path(name), render_cflags)
        # the Win32-facing units both systems share (port/dos/sys): over
        # KERNEL32, which on DOS is port/dos/dos/dos_kernel32.c
        for source in sorted((PORT_DIR / "sys").glob("*.c")):
            add(platform_objects, source, win32_cflags + " " + " ".join(DOS_RUNTIME_FLAGS[:2]))
        n.build("dos_game", "phony", game_objects)
        n.newline()

        # ---------- the C runtime, twice
        def runtime(kind_dir: str, extra: Sequence[str]) -> str:
            crt_dir = BUILD / kind_dir
            members: List[str] = []
            base = [*ABI_FLAGS, OPTIMISATION, *RUNTIME_FLAGS, *extra]
            pdclib = " ".join([*base, *pdclib_includes(tools)])
            glue = " ".join([
                *base, f"-include {posix(WIN9X_DIR / 'include' / 'w9x_win32.h')}",
                f"-I{posix(WIN9X_DIR / 'include')}", f"-I{posix(WIN9X_DIR / 'include' / 'crt')}",
                *pdclib_includes(tools), f"-I{quote(tools.nxdk / 'lib/pdclib/functions')}", *sdk_includes(tools),
            ])
            helper = f"--target=i686-pc-windows-msvc -I{quote(tools.nxdk / 'lib/xboxrt')}"
            for source, kind in runtime_sources(tools, win9x):
                if kind_dir == "crt-dos" and posix(source) == "port/win9x/src/w9x_malloc.c":
                    source, kind = PORT_DIR / "dos" / "dos_malloc.c", "runtime"
                obj = object_path(crt_dir, source, tools.nxdk)
                members.append(posix(obj))
                if source.suffix == ".s":
                    n.build(posix(obj), "as", posix(source),
                            variables={"asflags": helper if kind == "helper" else "--target=i686-pc-windows-msvc"})
                else:
                    n.build(posix(obj), "cc", posix(source),
                            variables={"cflags": pdclib if kind == "pdclib" else glue})
            library = crt_dir / ("doscrt.lib" if kind_dir == "crt-dos" else "w95crt.lib")
            n.build(posix(library), "lib", members)
            return posix(library)

        dos_runtime = runtime("crt-dos", DOS_RUNTIME_FLAGS)
        w95_runtime = runtime("crt-w95", [])
        n.newline()

        # ---------- MS-DOS
        dos_objects: List[str] = []
        for source in sorted((PORT_DIR / "dos").glob("*.c")):
            if source.name == "dos_malloc.c":
                continue
            add(dos_objects, source, " ".join([abi, *PORT_FLAGS, *DOS_RUNTIME_FLAGS,
                                               f"-I{posix(PORT_DIR / 'include')}",
                                               f"-I{posix(PORT_DIR / 'render')}", f"-I{posix(PORT_DIR / 'dos')}",
                                               f"-include {posix(WIN9X_DIR / 'include' / 'w9x_win32.h')}",
                                               f"-I{posix(WIN9X_DIR / 'include')}", *crt_includes(),
                                               *pdclib_includes(tools), *sdk_includes(tools)]))
        for source in sorted((PORT_DIR / "dos").glob("*.s")):
            obj = object_path(BUILD / "obj", source)
            dos_objects.append(posix(obj))
            n.build(posix(obj), "as", posix(source), variables={"asflags": "--target=i686-pc-windows-msvc"})
        # the network: mTCP (port/dos/mtcp, C++ built for protected mode) and
        # the sockets over it (port/dos/dos/*.cpp)
        mtcp_dir = PORT_DIR / "mtcp"
        cxx_flags = " ".join([abi, "-std=gnu++11", "-fno-exceptions", "-fno-rtti", "-w", *DOS_RUNTIME_FLAGS,
                              f"-include {posix(mtcp_dir / 'halo_mtcp.h')}", f"-I{posix(mtcp_dir)}",
                              f"-I{posix(mtcp_dir / 'compat')}", f"-I{posix(PORT_DIR / 'dos')}",
                              f"-I{posix(PORT_DIR / 'include')}", *pdclib_includes(tools)])
        for source in [*sorted(mtcp_dir.glob("*.cpp")), *sorted((PORT_DIR / "dos").glob("*.cpp"))]:
            add(dos_objects, source, cxx_flags)
        for source in sorted(mtcp_dir.glob("*.s")):
            obj = object_path(BUILD / "obj", source)
            dos_objects.append(posix(obj))
            n.build(posix(obj), "as", posix(source), variables={"asflags": "--target=i686-pc-windows-msvc"})
        stack = BUILD / "obj" / "port" / "dos" / "dos" / "dos_stack.obj"
        n.build(posix(stack), "wasm", posix(PORT_DIR / "dos" / "dos_stack.asm"),
                variables={"source": dos_path(PORT_DIR / "dos" / "dos_stack.asm"), "object": dos_path(stack)})
        dos_exe = BUILD / "HALO.EXE"
        directives = " ".join([
            "format os2 le",
            "option quiet",
            "option osname='DOS/32 Advanced DOS Extender (LE-style)'",
            f"option stub={dos_path(tools.dos32a)}",
            "option start=_dos_start",
            "option stack=256k",
            "option caseexact",
            f"option map={dos_path(dos_exe.with_suffix('.map'))}",
            "option eliminate",
            # (COFF weak externals: the game's MSVC inline functions)
            "disable 1125",
            "name", dos_path(dos_exe),
        ])
        dos_inputs = [posix(stack), *dos_objects, *game_objects, *platform_objects]
        n.build(posix(dos_exe), "wlink", dos_inputs, implicit=[dos_runtime],
                implicit_outputs=[posix(dos_exe.with_suffix(".map"))],
                variables={
                    "directives": directives,
                    "in_newline_files": " ".join(f"file {dos_path(p)}" for p in dos_inputs),
                    "libs_files": f"library {dos_path(dos_runtime)}",
                })
        n.build("dos", "phony", [posix(dos_exe)])

        # the runtime's test programs (port/dos/tests): each a DOS program of
        # the DOS units and the runtime alone
        tests = []
        test_cflags = " ".join([abi, *PORT_FLAGS, *DOS_RUNTIME_FLAGS, f"-I{posix(PORT_DIR / 'dos')}", *crt_includes(),
                                *pdclib_includes(tools)])
        # (what the DOS units ask of the game: test_support.c)
        support_objects: List[str] = []
        add(support_objects, PORT_DIR / "tests" / "test_support.c", test_cflags)
        for source in sorted((PORT_DIR / "tests").glob("t_*.c")):
            test_objects: List[str] = list(support_objects)
            add(test_objects, source, test_cflags)
            test_exe = BUILD / "tests" / (source.stem.upper() + ".EXE")
            inputs = [posix(stack), *dos_objects, *test_objects]
            n.build(posix(test_exe), "wlink", inputs, implicit=[dos_runtime],
                    variables={
                        "directives": directives.replace(dos_path(dos_exe.with_suffix(".map")),
                                                         dos_path(test_exe.with_suffix(".map"))).replace(
                            dos_path(dos_exe), dos_path(test_exe)),
                        "in_newline_files": " ".join(f"file {dos_path(p)}" for p in inputs),
                        "libs_files": f"library {dos_path(dos_runtime)}",
                    })
            tests.append(posix(test_exe))
        n.build("dos_tests", "phony", tests)
        n.newline()

        # ---------- Windows 95
        libraries = []
        for name in win9x.get("import_libraries", []):
            library = BUILD / "lib" / f"{name}.lib"
            n.build(posix(library), "implib", posix(WIN9X_DIR / "lib" / f"{name}.def"))
            libraries.append(posix(library))
        w95_objects: List[str] = []
        for source in sorted((PORT_DIR / "win95").glob("*.c")):
            add(w95_objects, source, win32_cflags)
        w95_exe = BUILD / "HALO95.EXE"
        ldflags = [
            "-nodefaultlib", "-entry:w95_start", "-subsystem:windows,4.0", "-osversion:4.0", "-fixed",
            "-base:0x400000", "-filealign:4096", "-stack:0x800000", "-dynamicbase:no", "-nxcompat:no",
            "-tsaware:no", "-opt:ref", "-opt:noicf", "-debug", f"-pdb:{posix(w95_exe.with_suffix('.pdb'))}",
            f"-map:{posix(w95_exe.with_suffix('.map'))}", *(f"-include:{name}" for name in RUNTIME_LIBCALLS),
        ]
        n.build(posix(w95_exe), "lld_link", [*w95_objects, *game_objects, *platform_objects],
                implicit=[w95_runtime, *libraries],
                implicit_outputs=[posix(w95_exe.with_suffix(".map")), posix(w95_exe.with_suffix(".pdb"))],
                variables={"ldflags": " ".join(ldflags), "libs": " ".join([w95_runtime, *libraries])})
        n.build("win95", "phony", [posix(w95_exe)])
        n.newline()

        # ---------- Direct3D: Halo's own renderer on Direct3D 8 (port/dos/DIRECT3D.md)
        # The game compiled again with HALO_D3D (the renderer trims off), Halo's
        # renderer and texture cache put back (port.json's d3d section), the
        # port's own 3D renderer left out, the Direct3D device in its place
        d3d = port.get("d3d", {})
        d3d_dir = PORT_DIR / "d3d"
        d3d_obj = BUILD / "obj-d3d"
        d3d_game_cflags = game_cflags + " -DHALO_D3D"
        restore_dirs = [d.rstrip("/") + "/" for d in d3d.get("restore_dirs", [])]
        restore_sources = set(d3d.get("restore_sources", []))
        d3d_excluded = set(d3d.get("exclude_sources", []))
        d3d_objects: List[str] = []
        d3d_names = set()
        for source in sorted((GAME / game_config["root"]).rglob("*.c")):
            name = source.relative_to(GAME).as_posix()
            if name in game_excluded or name in d3d_excluded:
                continue
            restored = name in restore_sources or any(name.startswith(d) for d in restore_dirs)
            if not restored and (name in exclude_sources or any(name.startswith(d) for d in exclude_dirs)):
                continue
            d3d_names.add(name)
            add(d3d_objects, source, d3d_game_cflags, d3d_obj)
        for extra in port.get("extra_game_sources", []):
            if extra not in d3d_names:
                add(d3d_objects, GAME / extra, d3d_game_cflags, d3d_obj)
        for name in port.get("port_game_sources", []):
            add(d3d_objects, LINUX_DIR / "game" / name, d3d_game_cflags, d3d_obj)
        glue_cflags = d3d_game_cflags + (f" -std=gnu99 -I{posix(PORT_DIR / 'include')} -I{posix(PORT_DIR / 'render')}"
                                         f" -I{posix(d3d_dir)}")
        glue_excluded = set(d3d.get("port_game_exclude", []))
        for source in sorted((PORT_DIR / "game").glob("*.c")):
            if source.name not in glue_excluded:
                add(d3d_objects, source, glue_cflags, d3d_obj)
        # (its own game-side glue, and its stand-ins: tools/dos_stubs.py --d3d)
        for source in sorted((d3d_dir / "game").glob("*.c")):
            add(d3d_objects, source, glue_cflags, d3d_obj)
        d3d_objects.extend(math_objects)
        # the device: its Xbox half over the game's Direct3D headers, its PC
        # half over Direct3D 8's (port/dos/d3d/include/halo_d3d8.h)
        for source in sorted((d3d_dir / "xbox").glob("*.c")):
            add(d3d_objects, source, xdk_cflags + f" -DHALO_D3D -I{posix(d3d_dir)}", d3d_obj)
        for source in sorted((d3d_dir / "pc").glob("*.c")):
            add(d3d_objects, source, win32_cflags + f" -DHALO_D3D -I{posix(d3d_dir)}",
                d3d_obj)
        # Windows 95's half again, for the Direct3D device's window
        win95_excluded = set(d3d.get("win95_exclude", []))
        for source in sorted((PORT_DIR / "win95").glob("*.c")):
            if source.name not in win95_excluded:
                add(d3d_objects, source, win32_cflags + f" -DHALO_D3D -I{posix(d3d_dir)}", d3d_obj)
        d3d_objects.extend(o for o in platform_objects if o not in render_objects)
        # (the link's objects, for tools/dos_stubs.py --d3d)
        (BUILD / "d3d_link_objects.txt").write_text("\n".join(d3d_objects) + "\n", encoding="utf-8")
        d3d_exe = BUILD / "HALOD3D.EXE"
        d3d_ldflags = [flag.replace(posix(w95_exe.with_suffix("")), posix(d3d_exe.with_suffix(""))) for flag in ldflags]
        n.build(posix(d3d_exe), "lld_link", d3d_objects,
                implicit=[w95_runtime, *libraries],
                implicit_outputs=[posix(d3d_exe.with_suffix(".map")), posix(d3d_exe.with_suffix(".pdb"))],
                variables={"ldflags": " ".join(d3d_ldflags), "libs": " ".join([w95_runtime, *libraries])})
        n.build("d3d", "phony", [posix(d3d_exe)])
        n.build("d3d_objects", "phony", d3d_objects)
        n.newline()

        # the launchers for testing, beside the programs
        launchers = []
        for source in sorted((PORT_DIR / "launch").glob("*.bat")):
            launcher = BUILD / source.name
            n.build(posix(launcher), "launcher", posix(source))
            launchers.append(posix(launcher))
        n.build("launchers", "phony", launchers)
        n.newline()
        n.default(["dos", "win95", "launchers"])
    print(f"wrote {posix(out_path)}: ninja -f {posix(out_path)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
