#!/usr/bin/env python3
"""
Packs a release of the DOS and Windows 95 builds: halo-dos-<version>.zip
(the programs, the launchers, README.TXT, the licences, empty MAPS and
MAPS_CE folders; never any map, settings file, log or key) and
halo-dos-<version>-symbols.zip (the link maps and the PDB, for reading the
addresses in a crash report's HALO.LOG; kept by whoever made the release:
the PDB holds the build machine's folder names).

    python tools/dos_package.py 0.1.0

Build first (ninja -f build/dos/build.ninja dos win95 launchers). The zips
go to build/release. Every text file is written with DOS line ends.
"""

import argparse
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build" / "dos"
GAME = ROOT / "chupathingyce"
RELEASE = ROOT / "port" / "dos" / "release"

# (name in the zip, file built or kept in the repository)
PROGRAMS = [
    ("HALO.EXE", BUILD / "HALO.EXE"),
    ("HALO95.EXE", BUILD / "HALO95.EXE"),
]
LAUNCHERS = [
    ("PLAY.BAT", ROOT / "port/dos/launch/play.bat"),
    ("RUN-DOS.BAT", ROOT / "port/dos/launch/run-dos.bat"),
    ("RUN-DOS-NET.BAT", ROOT / "port/dos/launch/run-dos-net.bat"),
    ("RUN-WIN95.BAT", ROOT / "port/dos/launch/run-win95.bat"),
]
SYMBOLS = [
    ("HALO.map", BUILD / "HALO.map"),
    ("HALO95.map", BUILD / "HALO95.map"),
    ("HALO95.pdb", BUILD / "HALO95.pdb"),
]
# THIRDPTY.TXT: (what, where it is used, its licence's text)
THIRD_PARTY = [
    ("DOS/32 Advanced DOS Extender (Supernar Systems)", "HALO.EXE's DOS extender",
     RELEASE / "DOS32A.TXT"),
    ("Zstandard (Meta Platforms)", "unpacking downloaded Custom Edition maps",
     ROOT / "port/dos/third_party/zstd/LICENSE"),
    ("musl (Rich Felker, et al.)", "the maths functions",
     ROOT / "port/support/musl-math/COPYRIGHT"),
    ("KCP (Lin Wei)", "internet play's reliable streams",
     GAME / "port/third_party/kcp/LICENSE"),
    ("Monocypher (Loup Vaillant, et al.)", "internet play's encryption and signatures",
     GAME / "port/third_party/monocypher/LICENCE.md"),
    ("Irrlicht_Water_Shader (Mahmoud Galal; after ATI's RenderMonkey ocean water)",
     "Direct3D 8's water program for pixel shaders 1.4",
     RELEASE / "WATER_SHADER.TXT"),
]
NOTES = """\
THE PARTS OF THESE PROGRAMS MADE BY OTHER PEOPLE
================================================

The game code is ChupathingyCE's (https://github.com/ChupathingyCE/chupathingyce),
licensed under the GNU General Public License version 3 (LICENSE.TXT), as
is this port. Its source, and this port's, is at
https://github.com/JackTulli/halo-dos.

mTCP (Michael B. Brutman, https://www.brutman.com/mTCP/), the DOS build's
TCP/IP, is licensed under the GNU General Public License version 3
(LICENSE.TXT).

The C library is PDCLib (https://github.com/DevSolar/pdclib, through nxdk),
dedicated to the public domain under CC0 1.0.

The others' licences follow.
"""


def dos_text(path: Path, replacements=None) -> bytes:
    text = path.read_text(encoding="utf-8")
    for old, new in (replacements or {}).items():
        text = text.replace(old, new)
    lines = text.replace("\r\n", "\n").split("\n")
    return "\r\n".join(lines).encode("cp437", errors="replace")


def third_party_text() -> bytes:
    parts = [NOTES]
    for what, use, licence in THIRD_PARTY:
        rule = "-" * 78
        parts.append(f"\n{rule}\n{what}\n({use})\n{rule}\n\n")
        parts.append(licence.read_text(encoding="utf-8").replace("\r\n", "\n").strip() + "\n")
    text = "".join(parts)
    # (plain ASCII for DOS editors)
    for old, new in {"©": "(C)", "–": "-", "—": "-", "’": "'", "“": '"', "”": '"'}.items():
        text = text.replace(old, new)
    return text.replace("\n", "\r\n").encode("ascii", errors="replace")


def add(archive: zipfile.ZipFile, name: str, data: bytes, compress=True) -> None:
    info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_DEFLATED if compress else zipfile.ZIP_STORED
    info.external_attr = 0o644 << 16
    archive.writestr(info, data)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("version", help="the release's version, as 0.1.0")
    parser.add_argument("--out", default=str(ROOT / "build" / "release"), help="where the zips go")
    args = parser.parse_args()

    missing = [str(path) for _, path in PROGRAMS + SYMBOLS if not path.is_file()]
    if missing:
        print("not built yet: " + ", ".join(missing), file=sys.stderr)
        return 1
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    top = "HALO/"

    game_zip = out / f"halo-dos-{args.version}.zip"
    with zipfile.ZipFile(game_zip, "w") as archive:
        for name, path in PROGRAMS:
            add(archive, top + name, path.read_bytes())
        for name, path in LAUNCHERS:
            add(archive, top + name, dos_text(path))
        add(archive, top + "README.TXT", dos_text(RELEASE / "README.TXT", {"%VERSION%": args.version}))
        add(archive, top + "LICENSE.TXT", dos_text(ROOT / "LICENSE"))
        add(archive, top + "THIRDPTY.TXT", third_party_text())
        add(archive, top + "MAPS/PUT_MAPS_HERE.TXT", dos_text(RELEASE / "MAPS.TXT"))
        add(archive, top + "MAPS_CE/README.TXT", dos_text(RELEASE / "MAPS_CE.TXT"))

    symbols_zip = out / f"halo-dos-{args.version}-symbols.zip"
    with zipfile.ZipFile(symbols_zip, "w") as archive:
        for name, path in SYMBOLS:
            add(archive, name, path.read_bytes())

    for path in (game_zip, symbols_zip):
        print(f"{path} ({path.stat().st_size // 1024} KB)")
        with zipfile.ZipFile(path) as archive:
            for info in archive.infolist():
                print(f"  {info.filename} ({info.file_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
